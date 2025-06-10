/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * NVMe/TCP transport
 */

#include "nvme_internal.h"

#include "spdk/endian.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/stdinc.h"
#include "spdk/crc32.h"
#include "spdk/assert.h"
#include "spdk/trace.h"
#include "spdk/util.h"
#include "spdk/nvmf.h"
#include "spdk/dma.h"

#include "spdk_internal/nvme_tcp.h"
#include "spdk_internal/trace_defs.h"

#define NVME_TCP_RW_BUFFER_SIZE 131072

/* For async connect workloads, allow more time since we are more likely
 * to be processing lots ICREQs at once.
 */
#define ICREQ_TIMEOUT_SYNC 2 /* in seconds */
#define ICREQ_TIMEOUT_ASYNC 10 /* in seconds */

#define NVME_TCP_HPDA_DEFAULT			0
#define NVME_TCP_MAX_R2T_DEFAULT		1
#define NVME_TCP_PDU_H2C_MIN_DATA_SIZE		4096

/*
 * Maximum value of transport_ack_timeout used by TCP controller
 */
#define NVME_TCP_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT	31

enum nvme_tcp_qpair_state {
	NVME_TCP_QPAIR_STATE_INVALID = 0,
	NVME_TCP_QPAIR_STATE_INITIALIZING = 1,
	NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_SEND = 2,
	NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL = 3,
	NVME_TCP_QPAIR_STATE_AUTHENTICATING = 4,
	NVME_TCP_QPAIR_STATE_RUNNING = 5,
	NVME_TCP_QPAIR_STATE_EXITING = 6,
	NVME_TCP_QPAIR_STATE_EXITED = 7,
};

/* NVMe TCP transport extensions for spdk_nvme_ctrlr */
struct nvme_tcp_ctrlr {
	struct spdk_nvme_ctrlr			ctrlr;
	char					psk_identity[NVMF_PSK_IDENTITY_LEN];
	uint8_t					psk[SPDK_TLS_PSK_MAX_LEN];
	int					psk_size;
	char					*tls_cipher_suite;
};

struct nvme_tcp_poll_group {
	struct spdk_nvme_transport_poll_group group;
	struct spdk_sock_group *sock_group;
	uint32_t completions_per_qpair;
	int64_t num_completions;

	TAILQ_HEAD(, nvme_tcp_qpair) needs_poll;
	TAILQ_HEAD(, nvme_tcp_qpair) timeout_enabled;
	struct spdk_nvme_tcp_stat stats;
};

/* NVMe TCP qpair extensions for spdk_nvme_qpair */
struct nvme_tcp_qpair {
	struct spdk_nvme_qpair			qpair;
	struct spdk_sock			*sock;

	TAILQ_HEAD(, nvme_tcp_req)		free_reqs;
	TAILQ_HEAD(, nvme_tcp_req)		outstanding_reqs;

	TAILQ_HEAD(, nvme_tcp_pdu)		send_queue;
	struct nvme_tcp_pdu			*recv_pdu;
	struct nvme_tcp_pdu			*send_pdu; /* only for error pdu and init pdu */
	struct nvme_tcp_pdu			*send_pdus; /* Used by tcp_reqs */
	enum nvme_tcp_pdu_recv_state		recv_state;
	struct nvme_tcp_req			*tcp_reqs;
	struct spdk_nvme_tcp_stat		*stats;

	uint16_t				num_entries;
	uint16_t				async_complete;

	struct {
		uint16_t host_hdgst_enable: 1;
		uint16_t host_ddgst_enable: 1;
		uint16_t icreq_send_ack: 1;
		uint16_t in_connect_poll: 1;
		uint16_t reserved: 12;
	} flags;

	/** Specifies the maximum number of PDU-Data bytes per H2C Data Transfer PDU */
	uint32_t				maxh2cdata;

	uint32_t				maxr2t;

	/* 0 based value, which is used to guide the padding */
	uint8_t					cpda;

	enum nvme_tcp_qpair_state		state;

	TAILQ_ENTRY(nvme_tcp_qpair)		link_poll;

	TAILQ_ENTRY(nvme_tcp_qpair)		link_timeout;

	uint64_t				icreq_timeout_tsc;

	bool					shared_stats;
};

enum nvme_tcp_req_state {
	NVME_TCP_REQ_FREE,
	NVME_TCP_REQ_ACTIVE,
	NVME_TCP_REQ_ACTIVE_R2T,
};

struct nvme_tcp_req {
	struct nvme_request			*req;
	enum nvme_tcp_req_state			state;
	uint16_t				cid;
	uint16_t				ttag;
	uint32_t				datao;
	uint32_t				expected_datao;
	uint32_t				r2tl_remain;
	uint32_t				active_r2ts;
	/* Used to hold a value received from subsequent R2T while we are still
	 * waiting for H2C complete */
	uint16_t				ttag_r2t_next;
	bool					in_capsule_data;
	/* It is used to track whether the req can be safely freed */
	union {
		uint8_t raw;
		struct {
			/* The last send operation completed - kernel released send buffer */
			uint8_t				send_ack : 1;
			/* Data transfer completed - target send resp or last data bit */
			uint8_t				data_recv : 1;
			/* tcp_req is waiting for completion of the previous send operation (buffer reclaim notification
			 * from kernel) to send H2C */
			uint8_t				h2c_send_waiting_ack : 1;
			/* tcp_req received subsequent r2t while it is still waiting for send_ack.
			 * Rare case, actual when dealing with target that can send several R2T requests.
			 * SPDK TCP target sends 1 R2T for the whole data buffer */
			uint8_t				r2t_waiting_h2c_complete : 1;
			/* Accel operation is in progress */
			uint8_t				in_progress_accel : 1;
			uint8_t				domain_in_use: 1;
			uint8_t				reserved : 2;
		} bits;
	} ordering;
	struct nvme_tcp_pdu			*pdu;
	struct iovec				iov[NVME_TCP_MAX_SGL_DESCRIPTORS];
	uint32_t				iovcnt;
	/* Used to hold a value received from subsequent R2T while we are still
	 * waiting for H2C ack */
	uint32_t				r2tl_remain_next;
	struct nvme_tcp_qpair			*tqpair;
	TAILQ_ENTRY(nvme_tcp_req)		link;
	struct spdk_nvme_cpl			rsp;
	uint8_t					rsvd1[32];
};
SPDK_STATIC_ASSERT(sizeof(struct nvme_tcp_req) % SPDK_CACHE_LINE_SIZE == 0, "unaligned size");

static struct spdk_nvme_tcp_stat g_dummy_stats = {};

static void nvme_tcp_send_h2c_data(struct nvme_tcp_req *tcp_req);
static int64_t nvme_tcp_poll_group_process_completions(struct spdk_nvme_transport_poll_group
		*tgroup, uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb);
static void nvme_tcp_icresp_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu);
static void nvme_tcp_req_complete(struct nvme_tcp_req *tcp_req, struct nvme_tcp_qpair *tqpair,
				  struct spdk_nvme_cpl *rsp, bool print_on_error);

static inline struct nvme_tcp_qpair *
nvme_tcp_qpair(struct spdk_nvme_qpair *qpair)
{
	assert(qpair->trtype == SPDK_NVME_TRANSPORT_TCP);
	return SPDK_CONTAINEROF(qpair, struct nvme_tcp_qpair, qpair);
}

static inline struct nvme_tcp_poll_group *
nvme_tcp_poll_group(struct spdk_nvme_transport_poll_group *group)
{
	return SPDK_CONTAINEROF(group, struct nvme_tcp_poll_group, group);
}

static inline struct nvme_tcp_ctrlr *
nvme_tcp_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_TCP);
	return SPDK_CONTAINEROF(ctrlr, struct nvme_tcp_ctrlr, ctrlr);
}

static struct nvme_tcp_req *
nvme_tcp_req_get(struct nvme_tcp_qpair *tqpair)
{
	struct nvme_tcp_req *tcp_req;

	tcp_req = TAILQ_FIRST(&tqpair->free_reqs);
	if (!tcp_req) {
		return NULL;
	}

	assert(tcp_req->state == NVME_TCP_REQ_FREE);
	tcp_req->state = NVME_TCP_REQ_ACTIVE;
	TAILQ_REMOVE(&tqpair->free_reqs, tcp_req, link);
	tcp_req->datao = 0;
	tcp_req->expected_datao = 0;
	tcp_req->req = NULL;
	tcp_req->in_capsule_data = false;
	tcp_req->r2tl_remain = 0;
	tcp_req->r2tl_remain_next = 0;
	tcp_req->active_r2ts = 0;
	tcp_req->iovcnt = 0;
	tcp_req->ordering.raw = 0;
	memset(tcp_req->pdu, 0, sizeof(struct nvme_tcp_pdu));
	memset(&tcp_req->rsp, 0, sizeof(struct spdk_nvme_cpl));

	return tcp_req;
}

static void
nvme_tcp_req_put(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	assert(tcp_req->state != NVME_TCP_REQ_FREE);
	tcp_req->state = NVME_TCP_REQ_FREE;
	TAILQ_INSERT_HEAD(&tqpair->free_reqs, tcp_req, link);
}

static inline void
nvme_tcp_accel_finish_sequence(struct nvme_tcp_poll_group *tgroup, struct nvme_tcp_req *treq,
			       void *seq, spdk_nvme_accel_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_nvme_poll_group *pg = tgroup->group.group;

	treq->ordering.bits.in_progress_accel = 1;
	pg->accel_fn_table.finish_sequence(seq, cb_fn, cb_arg);
}

static inline void
nvme_tcp_accel_reverse_sequence(struct nvme_tcp_poll_group *tgroup, void *seq)
{
	struct spdk_nvme_poll_group *pg = tgroup->group.group;

	pg->accel_fn_table.reverse_sequence(seq);
}

static inline int
nvme_tcp_accel_append_crc32c(struct nvme_tcp_poll_group *tgroup, void **seq, uint32_t *dst,
			     struct iovec *iovs, uint32_t iovcnt, uint32_t seed,
			     spdk_nvme_accel_step_cb cb_fn, void *cb_arg)
{
	struct spdk_nvme_poll_group *pg = tgroup->group.group;

	return pg->accel_fn_table.append_crc32c(pg->ctx, seq, dst, iovs, iovcnt, NULL, NULL,
						seed, cb_fn, cb_arg);
}

static void
nvme_tcp_free_reqs(struct nvme_tcp_qpair *tqpair)
{
	free(tqpair->tcp_reqs);
	tqpair->tcp_reqs = NULL;

	spdk_free(tqpair->send_pdus);
	tqpair->send_pdus = NULL;
}

static int
nvme_tcp_alloc_reqs(struct nvme_tcp_qpair *tqpair)
{
	uint16_t i;
	struct nvme_tcp_req *tcp_req;

	tqpair->tcp_reqs = aligned_alloc(SPDK_CACHE_LINE_SIZE,
					 tqpair->num_entries * sizeof(*tcp_req));
	if (tqpair->tcp_reqs == NULL) {
		SPDK_ERRLOG("Failed to allocate tcp_reqs on tqpair=%p\n", tqpair);
		goto fail;
	}

	/* Add additional 2 member for the send_pdu, recv_pdu owned by the tqpair */
	tqpair->send_pdus = spdk_zmalloc((tqpair->num_entries + 2) * sizeof(struct nvme_tcp_pdu),
					 0x1000, NULL,
					 SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);

	if (tqpair->send_pdus == NULL) {
		SPDK_ERRLOG("Failed to allocate send_pdus on tqpair=%p\n", tqpair);
		goto fail;
	}

	memset(tqpair->tcp_reqs, 0, tqpair->num_entries * sizeof(*tcp_req));
	TAILQ_INIT(&tqpair->send_queue);
	TAILQ_INIT(&tqpair->free_reqs);
	TAILQ_INIT(&tqpair->outstanding_reqs);
	tqpair->qpair.queue_depth = 0;
	for (i = 0; i < tqpair->num_entries; i++) {
		tcp_req = &tqpair->tcp_reqs[i];
		tcp_req->cid = i;
		tcp_req->tqpair = tqpair;
		tcp_req->pdu = &tqpair->send_pdus[i];
		TAILQ_INSERT_TAIL(&tqpair->free_reqs, tcp_req, link);
	}

	tqpair->send_pdu = &tqpair->send_pdus[i];
	tqpair->recv_pdu = &tqpair->send_pdus[i + 1];

	return 0;
fail:
	nvme_tcp_free_reqs(tqpair);
	return -ENOMEM;
}

static inline void
nvme_tcp_qpair_set_recv_state(struct nvme_tcp_qpair *tqpair,
			      enum nvme_tcp_pdu_recv_state state)
{
	if (tqpair->recv_state == state) {
		SPDK_ERRLOG("The recv state of tqpair=%p is same with the state(%d) to be set\n",
			    tqpair, state);
		return;
	}

	if (state == NVME_TCP_PDU_RECV_STATE_ERROR) {
		assert(TAILQ_EMPTY(&tqpair->outstanding_reqs));
	}

	tqpair->recv_state = state;
}

static void nvme_tcp_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr);

static void
nvme_tcp_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	struct nvme_tcp_pdu *pdu;
	int rc;
	struct nvme_tcp_poll_group *group;

	if (TAILQ_ENTRY_ENQUEUED(tqpair, link_poll)) {
		group = nvme_tcp_poll_group(qpair->poll_group);
		TAILQ_REMOVE_CLEAR(&group->needs_poll, tqpair, link_poll);
	}

	rc = spdk_sock_close(&tqpair->sock);

	if (tqpair->sock != NULL) {
		SPDK_ERRLOG("tqpair=%p, errno=%d, rc=%d\n", tqpair, errno, rc);
		/* Set it to NULL manually */
		tqpair->sock = NULL;
	}

	/* clear the send_queue */
	while (!TAILQ_EMPTY(&tqpair->send_queue)) {
		pdu = TAILQ_FIRST(&tqpair->send_queue);
		/* Remove the pdu from the send_queue to prevent the wrong sending out
		 * in the next round connection
		 */
		TAILQ_REMOVE(&tqpair->send_queue, pdu, tailq);
	}

	nvme_tcp_qpair_abort_reqs(qpair, qpair->abort_dnr);

	/* If the qpair is marked as asynchronous, let it go through the process_completions() to
	 * let any outstanding requests (e.g. those with outstanding accel operations) complete.
	 * Otherwise, there's no way of waiting for them, so tqpair->outstanding_reqs has to be
	 * empty.
	 */
	if (qpair->async) {
		nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
	} else {
		assert(TAILQ_EMPTY(&tqpair->outstanding_reqs));
		nvme_transport_ctrlr_disconnect_qpair_done(qpair);
	}
}

static int
nvme_tcp_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	assert(qpair != NULL);
	nvme_tcp_qpair_abort_reqs(qpair, qpair->abort_dnr);
	assert(TAILQ_EMPTY(&tqpair->outstanding_reqs));

	nvme_qpair_deinit(qpair);
	nvme_tcp_free_reqs(tqpair);
	if (!tqpair->shared_stats) {
		free(tqpair->stats);
	}
	free(tqpair);

	return 0;
}

static int
nvme_tcp_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

static int
nvme_tcp_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_tcp_ctrlr *tctrlr = nvme_tcp_ctrlr(ctrlr);

	if (ctrlr->adminq) {
		nvme_tcp_ctrlr_delete_io_qpair(ctrlr, ctrlr->adminq);
	}

	nvme_ctrlr_destruct_finish(ctrlr);

	free(tctrlr);

	return 0;
}

/* If there are queued requests, we assume they are queued because they are waiting
 * for resources to be released. Those resources are almost certainly released in
 * response to a PDU completing. However, to attempt to make forward progress
 * the qpair needs to be polled and we can't rely on another network event to make
 * that happen. Add it to a list of qpairs to poll regardless of network activity.
 *
 * Besides, when tqpair state is NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL or
 * NVME_TCP_QPAIR_STATE_INITIALIZING, need to add it to needs_poll list too to make
 * forward progress in case that the resources are released after icreq's or CONNECT's
 * resp is processed. */
static void
nvme_tcp_cond_schedule_qpair_polling(struct nvme_tcp_qpair *tqpair)
{
	struct nvme_tcp_poll_group *pgroup;

	if (TAILQ_ENTRY_ENQUEUED(tqpair, link_poll) || !tqpair->qpair.poll_group) {
		return;
	}

	if (STAILQ_EMPTY(&tqpair->qpair.queued_req) &&
	    spdk_likely(tqpair->state != NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL &&
			tqpair->state != NVME_TCP_QPAIR_STATE_INITIALIZING)) {
		return;
	}

	pgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
	TAILQ_INSERT_TAIL(&pgroup->needs_poll, tqpair, link_poll);
}

static void
pdu_write_done(void *cb_arg, int err)
{
	struct nvme_tcp_pdu *pdu = cb_arg;
	struct nvme_tcp_qpair *tqpair = pdu->qpair;

	nvme_tcp_cond_schedule_qpair_polling(tqpair);
	TAILQ_REMOVE(&tqpair->send_queue, pdu, tailq);

	if (err != 0) {
		nvme_transport_ctrlr_disconnect_qpair(tqpair->qpair.ctrlr, &tqpair->qpair);
		return;
	}

	assert(pdu->cb_fn != NULL);
	pdu->cb_fn(pdu->cb_arg);
}

static void
pdu_write_fail(struct nvme_tcp_pdu *pdu, int status)
{
	struct nvme_tcp_qpair *tqpair = pdu->qpair;

	/* This function is similar to pdu_write_done(), but it should be called before a PDU is
	 * sent over the socket */
	TAILQ_INSERT_TAIL(&tqpair->send_queue, pdu, tailq);
	pdu_write_done(pdu, status);
}

static void
pdu_seq_fail(struct nvme_tcp_pdu *pdu, int status)
{
	struct nvme_tcp_req *treq = pdu->req;

	SPDK_ERRLOG("Failed to execute accel sequence: %d\n", status);
	nvme_tcp_cond_schedule_qpair_polling(pdu->qpair);
	treq->rsp.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	nvme_tcp_req_complete(treq, treq->tqpair, &treq->rsp, true);
}

static void
_tcp_write_pdu(struct nvme_tcp_pdu *pdu)
{
	uint32_t mapped_length = 0;
	struct nvme_tcp_qpair *tqpair = pdu->qpair;

	pdu->sock_req.iovcnt = nvme_tcp_build_iovs(pdu->iov, SPDK_COUNTOF(pdu->iov), pdu,
			       (bool)tqpair->flags.host_hdgst_enable, (bool)tqpair->flags.host_ddgst_enable,
			       &mapped_length);
	TAILQ_INSERT_TAIL(&tqpair->send_queue, pdu, tailq);
	if (spdk_unlikely(mapped_length < pdu->data_len)) {
		SPDK_ERRLOG("could not map the whole %u bytes (mapped only %u bytes)\n", pdu->data_len,
			    mapped_length);
		pdu_write_done(pdu, -EINVAL);
		return;
	}
	pdu->sock_req.cb_fn = pdu_write_done;
	pdu->sock_req.cb_arg = pdu;
	tqpair->stats->submitted_requests++;
	spdk_sock_writev_async(tqpair->sock, &pdu->sock_req);
}

static void
tcp_write_pdu_seq_cb(void *ctx, int status)
{
	struct nvme_tcp_pdu *pdu = ctx;
	struct nvme_tcp_req *treq = pdu->req;
	struct nvme_request *req = treq->req;

	assert(treq->ordering.bits.in_progress_accel);
	treq->ordering.bits.in_progress_accel = 0;

	req->accel_sequence = NULL;
	if (spdk_unlikely(status != 0)) {
		pdu_seq_fail(pdu, status);
		return;
	}

	_tcp_write_pdu(pdu);
}

static void
tcp_write_pdu(struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *treq = pdu->req;
	struct nvme_tcp_qpair *tqpair = pdu->qpair;
	struct nvme_tcp_poll_group *tgroup;
	struct nvme_request *req;

	if (spdk_likely(treq != NULL)) {
		req = treq->req;
		if (req->accel_sequence != NULL &&
		    spdk_nvme_opc_get_data_transfer(req->cmd.opc) == SPDK_NVME_DATA_HOST_TO_CONTROLLER &&
		    pdu->data_len > 0) {
			assert(tqpair->qpair.poll_group != NULL);
			tgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
			nvme_tcp_accel_finish_sequence(tgroup, treq, req->accel_sequence,
						       tcp_write_pdu_seq_cb, pdu);
			return;
		}
	}

	_tcp_write_pdu(pdu);
}

static void
pdu_accel_seq_compute_crc32_done(void *cb_arg)
{
	struct nvme_tcp_pdu *pdu = cb_arg;

	pdu->data_digest_crc32 ^= SPDK_CRC32C_XOR;
	MAKE_DIGEST_WORD(pdu->data_digest, pdu->data_digest_crc32);
}

static bool
pdu_accel_compute_crc32(struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_qpair *tqpair = pdu->qpair;
	struct nvme_tcp_poll_group *tgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
	struct nvme_request *req = ((struct nvme_tcp_req *)pdu->req)->req;
	int rc;

	/* Only support this limited case for the first step */
	if (spdk_unlikely(nvme_qpair_get_state(&tqpair->qpair) < NVME_QPAIR_CONNECTED ||
			  pdu->dif_ctx != NULL ||
			  pdu->data_len % SPDK_NVME_TCP_DIGEST_ALIGNMENT != 0)) {
		return false;
	}

	if (tqpair->qpair.poll_group == NULL ||
	    tgroup->group.group->accel_fn_table.append_crc32c == NULL) {
		return false;
	}

	rc = nvme_tcp_accel_append_crc32c(tgroup, &req->accel_sequence,
					  &pdu->data_digest_crc32,
					  pdu->data_iov, pdu->data_iovcnt, 0,
					  pdu_accel_seq_compute_crc32_done, pdu);
	if (spdk_unlikely(rc != 0)) {
		/* If accel is out of resources, fall back to non-accelerated crc32 */
		if (rc == -ENOMEM) {
			return false;
		}

		SPDK_ERRLOG("Failed to append crc32c operation: %d\n", rc);
		pdu_write_fail(pdu, rc);
		return true;
	}

	tcp_write_pdu(pdu);

	return true;
}

static void
pdu_compute_crc32_seq_cb(void *cb_arg, int status)
{
	struct nvme_tcp_pdu *pdu = cb_arg;
	struct nvme_tcp_req *treq = pdu->req;
	struct nvme_request *req = treq->req;
	uint32_t crc32c;

	assert(treq->ordering.bits.in_progress_accel);
	treq->ordering.bits.in_progress_accel = 0;

	req->accel_sequence = NULL;
	if (spdk_unlikely(status != 0)) {
		pdu_seq_fail(pdu, status);
		return;
	}

	crc32c = nvme_tcp_pdu_calc_data_digest(pdu);
	crc32c = crc32c ^ SPDK_CRC32C_XOR;
	MAKE_DIGEST_WORD(pdu->data_digest, crc32c);

	_tcp_write_pdu(pdu);
}

static void
pdu_compute_crc32(struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_qpair *tqpair = pdu->qpair;
	struct nvme_tcp_poll_group *tgroup;
	struct nvme_request *req;
	uint32_t crc32c;

	/* Data Digest */
	if (pdu->data_len > 0 && g_nvme_tcp_ddgst[pdu->hdr.common.pdu_type] &&
	    tqpair->flags.host_ddgst_enable) {
		if (pdu_accel_compute_crc32(pdu)) {
			return;
		}

		req = ((struct nvme_tcp_req *)pdu->req)->req;
		if (req->accel_sequence != NULL) {
			tgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
			nvme_tcp_accel_finish_sequence(tgroup, pdu->req, req->accel_sequence,
						       pdu_compute_crc32_seq_cb, pdu);
			return;
		}

		crc32c = nvme_tcp_pdu_calc_data_digest(pdu);
		crc32c = crc32c ^ SPDK_CRC32C_XOR;
		MAKE_DIGEST_WORD(pdu->data_digest, crc32c);
	}

	tcp_write_pdu(pdu);
}

static int
nvme_tcp_qpair_write_pdu(struct nvme_tcp_qpair *tqpair,
			 struct nvme_tcp_pdu *pdu,
			 nvme_tcp_qpair_xfer_complete_cb cb_fn,
			 void *cb_arg)
{
	int hlen;
	uint32_t crc32c;

	hlen = pdu->hdr.common.hlen;
	pdu->cb_fn = cb_fn;
	pdu->cb_arg = cb_arg;
	pdu->qpair = tqpair;

	/* Header Digest */
	if (g_nvme_tcp_hdgst[pdu->hdr.common.pdu_type] && tqpair->flags.host_hdgst_enable) {
		crc32c = nvme_tcp_pdu_calc_header_digest(pdu);
		MAKE_DIGEST_WORD((uint8_t *)&pdu->hdr.raw[hlen], crc32c);
	}

	pdu_compute_crc32(pdu);

	return 0;
}

static int
nvme_tcp_try_memory_translation(struct nvme_tcp_req *tcp_req, void **addr, uint32_t length)
{
	struct nvme_request *req = tcp_req->req;
	struct spdk_memory_domain_translation_result translation = {
		.iov_count = 0,
		.size = sizeof(translation)
	};
	int rc;

	if (!tcp_req->ordering.bits.domain_in_use) {
		return 0;
	}

	rc = spdk_memory_domain_translate_data(req->payload.opts->memory_domain,
					       req->payload.opts->memory_domain_ctx, spdk_memory_domain_get_system_domain(), NULL, *addr, length,
					       &translation);
	if (spdk_unlikely(rc || translation.iov_count != 1)) {
		SPDK_ERRLOG("DMA memory translation failed, rc %d, iov_count %u\n", rc, translation.iov_count);
		return -EFAULT;
	}

	assert(length == translation.iov.iov_len);
	*addr = translation.iov.iov_base;
	return 0;
}

/*
 * Build SGL describing contiguous payload buffer.
 */
static int
nvme_tcp_build_contig_request(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	struct nvme_request *req = tcp_req->req;

	/* ubsan complains about applying zero offset to null pointer if contig_or_cb_arg is NULL,
	 * so just double cast it to make it go away */
	void *addr = (void *)((uintptr_t)req->payload.contig_or_cb_arg + req->payload_offset);
	size_t length = req->payload_size;
	int rc;

	SPDK_DEBUGLOG(nvme, "enter\n");

	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG);
	rc = nvme_tcp_try_memory_translation(tcp_req, &addr, length);
	if (spdk_unlikely(rc)) {
		return rc;
	}

	tcp_req->iov[0].iov_base = addr;
	tcp_req->iov[0].iov_len = length;
	tcp_req->iovcnt = 1;
	return 0;
}

/*
 * Build SGL describing scattered payload buffer.
 */
static int
nvme_tcp_build_sgl_request(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	int rc;
	uint32_t length, remaining_size, iovcnt = 0, max_num_sgl;
	struct nvme_request *req = tcp_req->req;

	SPDK_DEBUGLOG(nvme, "enter\n");

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	assert(req->payload.next_sge_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);

	max_num_sgl = spdk_min(req->qpair->ctrlr->max_sges, NVME_TCP_MAX_SGL_DESCRIPTORS);
	remaining_size = req->payload_size;

	do {
		void *addr;

		rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &addr, &length);
		if (rc) {
			return -1;
		}

		rc = nvme_tcp_try_memory_translation(tcp_req, &addr, length);
		if (spdk_unlikely(rc)) {
			return rc;
		}

		length = spdk_min(length, remaining_size);
		tcp_req->iov[iovcnt].iov_base = addr;
		tcp_req->iov[iovcnt].iov_len = length;
		remaining_size -= length;
		iovcnt++;
	} while (remaining_size > 0 && iovcnt < max_num_sgl);


	/* Should be impossible if we did our sgl checks properly up the stack, but do a sanity check here. */
	if (remaining_size > 0) {
		SPDK_ERRLOG("Failed to construct tcp_req=%p, and the iovcnt=%u, remaining_size=%u\n",
			    tcp_req, iovcnt, remaining_size);
		return -1;
	}

	tcp_req->iovcnt = iovcnt;

	return 0;
}

static int
nvme_tcp_req_init(struct nvme_tcp_qpair *tqpair, struct nvme_request *req,
		  struct nvme_tcp_req *tcp_req)
{
	struct spdk_nvme_ctrlr *ctrlr = tqpair->qpair.ctrlr;
	int rc = 0;
	enum spdk_nvme_data_transfer xfer;
	uint32_t max_in_capsule_data_size;

	tcp_req->req = req;
	tcp_req->ordering.bits.domain_in_use = (req->payload.opts && req->payload.opts->memory_domain);

	req->cmd.cid = tcp_req->cid;
	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK;
	req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_TRANSPORT;
	req->cmd.dptr.sgl1.unkeyed.length = req->payload_size;

	if (spdk_unlikely(req->cmd.opc == SPDK_NVME_OPC_FABRIC)) {
		struct spdk_nvmf_capsule_cmd *nvmf_cmd = (struct spdk_nvmf_capsule_cmd *)&req->cmd;

		xfer = spdk_nvme_opc_get_data_transfer(nvmf_cmd->fctype);
	} else {
		xfer = spdk_nvme_opc_get_data_transfer(req->cmd.opc);
	}

	/* For c2h delay filling in the iov until the data arrives.
	 * For h2c some delay is also possible if data doesn't fit into cmd capsule (not implemented). */
	if (nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG) {
		if (xfer != SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
			rc = nvme_tcp_build_contig_request(tqpair, tcp_req);
		}
	} else if (nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL) {
		if (xfer != SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
			rc = nvme_tcp_build_sgl_request(tqpair, tcp_req);
		}
	} else {
		rc = -1;
	}

	if (rc) {
		return rc;
	}

	if (xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		max_in_capsule_data_size = ctrlr->ioccsz_bytes;
		if (spdk_unlikely((req->cmd.opc == SPDK_NVME_OPC_FABRIC) ||
				  nvme_qpair_is_admin_queue(&tqpair->qpair))) {
			max_in_capsule_data_size = SPDK_NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE;
		}

		if (req->payload_size <= max_in_capsule_data_size) {
			req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
			req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
			req->cmd.dptr.sgl1.address = 0;
			tcp_req->in_capsule_data = true;
		}
	}

	return 0;
}

static inline bool
nvme_tcp_req_complete_safe(struct nvme_tcp_req *tcp_req)
{
	if (!(tcp_req->ordering.bits.send_ack && tcp_req->ordering.bits.data_recv &&
	      !tcp_req->ordering.bits.in_progress_accel)) {
		return false;
	}

	assert(tcp_req->state == NVME_TCP_REQ_ACTIVE);
	assert(tcp_req->tqpair != NULL);
	assert(tcp_req->req != NULL);

	nvme_tcp_req_complete(tcp_req, tcp_req->tqpair, &tcp_req->rsp, true);
	return true;
}

static void
nvme_tcp_qpair_cmd_send_complete(void *cb_arg)
{
	struct nvme_tcp_req *tcp_req = cb_arg;

	SPDK_DEBUGLOG(nvme, "tcp req %p, cid %u, qid %u\n", tcp_req, tcp_req->cid,
		      tcp_req->tqpair->qpair.id);
	tcp_req->ordering.bits.send_ack = 1;
	/* Handle the r2t case */
	if (spdk_unlikely(tcp_req->ordering.bits.h2c_send_waiting_ack)) {
		SPDK_DEBUGLOG(nvme, "tcp req %p, send H2C data\n", tcp_req);
		nvme_tcp_send_h2c_data(tcp_req);
	} else {
		if (tcp_req->in_capsule_data && tcp_req->ordering.bits.domain_in_use) {
			spdk_memory_domain_invalidate_data(tcp_req->req->payload.opts->memory_domain,
							   tcp_req->req->payload.opts->memory_domain_ctx, tcp_req->iov, tcp_req->iovcnt);
		}

		nvme_tcp_req_complete_safe(tcp_req);
	}
}

static int
nvme_tcp_qpair_capsule_cmd_send(struct nvme_tcp_qpair *tqpair,
				struct nvme_tcp_req *tcp_req)
{
	struct nvme_tcp_pdu *pdu;
	struct spdk_nvme_tcp_cmd *capsule_cmd;
	uint32_t plen = 0, alignment;
	uint8_t pdo;

	SPDK_DEBUGLOG(nvme, "enter\n");
	pdu = tcp_req->pdu;
	pdu->req = tcp_req;

	capsule_cmd = &pdu->hdr.capsule_cmd;
	capsule_cmd->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD;
	plen = capsule_cmd->common.hlen = sizeof(*capsule_cmd);
	capsule_cmd->ccsqe = tcp_req->req->cmd;

	SPDK_DEBUGLOG(nvme, "capsule_cmd cid=%u on tqpair(%p)\n", tcp_req->req->cmd.cid, tqpair);

	if (tqpair->flags.host_hdgst_enable) {
		SPDK_DEBUGLOG(nvme, "Header digest is enabled for capsule command on tcp_req=%p\n",
			      tcp_req);
		capsule_cmd->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
		plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	if ((tcp_req->req->payload_size == 0) || !tcp_req->in_capsule_data) {
		goto end;
	}

	pdo = plen;
	pdu->padding_len = 0;
	if (tqpair->cpda) {
		alignment = (tqpair->cpda + 1) << 2;
		if (alignment > plen) {
			pdu->padding_len = alignment - plen;
			pdo = alignment;
			plen = alignment;
		}
	}

	capsule_cmd->common.pdo = pdo;
	plen += tcp_req->req->payload_size;
	if (tqpair->flags.host_ddgst_enable) {
		capsule_cmd->common.flags |= SPDK_NVME_TCP_CH_FLAGS_DDGSTF;
		plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	tcp_req->datao = 0;
	nvme_tcp_pdu_set_data_buf(pdu, tcp_req->iov, tcp_req->iovcnt,
				  0, tcp_req->req->payload_size);
end:
	capsule_cmd->common.plen = plen;
	return nvme_tcp_qpair_write_pdu(tqpair, pdu, nvme_tcp_qpair_cmd_send_complete, tcp_req);

}

static int
nvme_tcp_qpair_submit_request(struct spdk_nvme_qpair *qpair,
			      struct nvme_request *req)
{
	struct nvme_tcp_qpair *tqpair;
	struct nvme_tcp_req *tcp_req;

	tqpair = nvme_tcp_qpair(qpair);
	assert(tqpair != NULL);
	assert(req != NULL);

	tcp_req = nvme_tcp_req_get(tqpair);
	if (!tcp_req) {
		tqpair->stats->queued_requests++;
		/* Inform the upper layer to try again later. */
		return -EAGAIN;
	}

	if (spdk_unlikely(nvme_tcp_req_init(tqpair, req, tcp_req))) {
		SPDK_ERRLOG("nvme_tcp_req_init() failed\n");
		nvme_tcp_req_put(tqpair, tcp_req);
		return -1;
	}

	tqpair->qpair.queue_depth++;
	spdk_trace_record(TRACE_NVME_TCP_SUBMIT, qpair->id, 0, (uintptr_t)tcp_req->pdu, req->cb_arg,
			  (uint32_t)req->cmd.cid, (uint32_t)req->cmd.opc,
			  req->cmd.cdw10, req->cmd.cdw11, req->cmd.cdw12, tqpair->qpair.queue_depth);
	TAILQ_INSERT_TAIL(&tqpair->outstanding_reqs, tcp_req, link);

	if (TAILQ_ENTRY_NOT_ENQUEUED(tqpair, link_timeout) && qpair->poll_group != NULL &&
	    qpair->ctrlr->timeout_enabled) {
		struct nvme_tcp_poll_group *tgroup;

		tgroup = nvme_tcp_poll_group(qpair->poll_group);
		TAILQ_INSERT_TAIL(&tgroup->timeout_enabled, tqpair, link_timeout);
	}

	return nvme_tcp_qpair_capsule_cmd_send(tqpair, tcp_req);
}

static int
nvme_tcp_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

static void
nvme_tcp_req_complete(struct nvme_tcp_req *tcp_req,
		      struct nvme_tcp_qpair *tqpair,
		      struct spdk_nvme_cpl *rsp,
		      bool print_on_error)
{
	struct spdk_nvme_cpl	cpl;
	struct spdk_nvme_qpair	*qpair;
	struct nvme_request	*req;
	bool			print_error;

	assert(tcp_req->req != NULL);
	req = tcp_req->req;
	qpair = req->qpair;

	SPDK_DEBUGLOG(nvme, "complete tcp_req(%p) on tqpair=%p\n", tcp_req, tqpair);

	if (!qpair->in_completion_context) {
		tqpair->async_complete++;
	}

	/* Cache arguments to be passed to nvme_complete_request since tcp_req can be zeroed when released */
	memcpy(&cpl, rsp, sizeof(cpl));

	if (spdk_unlikely(spdk_nvme_cpl_is_error(rsp))) {
		print_error = print_on_error && !qpair->ctrlr->opts.disable_error_logging;

		if (print_error) {
			spdk_nvme_qpair_print_command(qpair, &req->cmd);
		}

		if (print_error || SPDK_DEBUGLOG_FLAG_ENABLED("nvme")) {
			spdk_nvme_qpair_print_completion(qpair, rsp);
		}
	}

	qpair->queue_depth--;
	spdk_trace_record(TRACE_NVME_TCP_COMPLETE, qpair->id, 0, (uintptr_t)tcp_req->pdu, req->cb_arg,
			  (uint32_t)req->cmd.cid, (uint32_t)cpl.status_raw, qpair->queue_depth);
	TAILQ_REMOVE(&tqpair->outstanding_reqs, tcp_req, link);

	if (TAILQ_EMPTY(&tqpair->outstanding_reqs) && qpair->poll_group != NULL &&
	    TAILQ_ENTRY_ENQUEUED(tqpair, link_timeout)) {
		struct nvme_tcp_poll_group *tgroup;

		assert(qpair->ctrlr->timeout_enabled);

		tgroup = nvme_tcp_poll_group(qpair->poll_group);
		TAILQ_REMOVE_CLEAR(&tgroup->timeout_enabled, tqpair, link_timeout);
	}

	nvme_tcp_req_put(tqpair, tcp_req);
	nvme_complete_request(req->cb_fn, req->cb_arg, req->qpair, req, &cpl);
}

static void
nvme_tcp_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	struct nvme_tcp_req *tcp_req, *tmp;
	struct spdk_nvme_cpl cpl = {};
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	cpl.sqid = qpair->id;
	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.dnr = dnr;

	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->outstanding_reqs, link, tmp) {
		/* We cannot abort requests with accel operations in progress */
		if (tcp_req->ordering.bits.in_progress_accel) {
			continue;
		}

		nvme_tcp_req_complete(tcp_req, tqpair, &cpl, true);
	}
}

static void
nvme_tcp_qpair_send_h2c_term_req_complete(void *cb_arg)
{
	struct nvme_tcp_qpair *tqpair = cb_arg;

	tqpair->state = NVME_TCP_QPAIR_STATE_EXITING;
}

static void
nvme_tcp_qpair_send_h2c_term_req(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu,
				 enum spdk_nvme_tcp_term_req_fes fes, uint32_t error_offset)
{
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_term_req_hdr *h2c_term_req;
	uint32_t h2c_term_req_hdr_len = sizeof(*h2c_term_req);
	uint8_t copy_len;

	rsp_pdu = tqpair->send_pdu;
	memset(rsp_pdu, 0, sizeof(*rsp_pdu));
	h2c_term_req = &rsp_pdu->hdr.term_req;
	h2c_term_req->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ;
	h2c_term_req->common.hlen = h2c_term_req_hdr_len;

	if ((fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER)) {
		DSET32(&h2c_term_req->fei, error_offset);
	}

	copy_len = pdu->hdr.common.hlen;
	if (copy_len > SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE) {
		copy_len = SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE;
	}

	/* Copy the error info into the buffer */
	memcpy((uint8_t *)rsp_pdu->hdr.raw + h2c_term_req_hdr_len, pdu->hdr.raw, copy_len);
	nvme_tcp_pdu_set_data(rsp_pdu, (uint8_t *)rsp_pdu->hdr.raw + h2c_term_req_hdr_len, copy_len);

	/* Contain the header len of the wrong received pdu */
	h2c_term_req->common.plen = h2c_term_req->common.hlen + copy_len;
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
	nvme_tcp_qpair_write_pdu(tqpair, rsp_pdu, nvme_tcp_qpair_send_h2c_term_req_complete, tqpair);
}

static bool
nvme_tcp_qpair_recv_state_valid(struct nvme_tcp_qpair *tqpair)
{
	switch (tqpair->state) {
	case NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_SEND:
	case NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL:
	case NVME_TCP_QPAIR_STATE_AUTHENTICATING:
	case NVME_TCP_QPAIR_STATE_RUNNING:
		return true;
	default:
		return false;
	}
}

static void
nvme_tcp_pdu_ch_handle(struct nvme_tcp_qpair *tqpair)
{
	struct nvme_tcp_pdu *pdu;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	uint32_t expected_hlen, hd_len = 0;
	bool plen_error = false;

	pdu = tqpair->recv_pdu;

	SPDK_DEBUGLOG(nvme, "pdu type = %d\n", pdu->hdr.common.pdu_type);
	if (pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_RESP) {
		if (tqpair->state != NVME_TCP_QPAIR_STATE_INVALID) {
			SPDK_ERRLOG("Already received IC_RESP PDU, and we should reject this pdu=%p\n", pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
			goto err;
		}
		expected_hlen = sizeof(struct spdk_nvme_tcp_ic_resp);
		if (pdu->hdr.common.plen != expected_hlen) {
			plen_error = true;
		}
	} else {
		if (spdk_unlikely(!nvme_tcp_qpair_recv_state_valid(tqpair))) {
			SPDK_ERRLOG("The TCP/IP tqpair connection is not negotiated\n");
			fes = SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
			goto err;
		}

		switch (pdu->hdr.common.pdu_type) {
		case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP:
			expected_hlen = sizeof(struct spdk_nvme_tcp_rsp);
			if (pdu->hdr.common.flags & SPDK_NVME_TCP_CH_FLAGS_HDGSTF) {
				hd_len = SPDK_NVME_TCP_DIGEST_LEN;
			}

			if (pdu->hdr.common.plen != (expected_hlen + hd_len)) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_C2H_DATA:
			expected_hlen = sizeof(struct spdk_nvme_tcp_c2h_data_hdr);
			if (pdu->hdr.common.plen < pdu->hdr.common.pdo) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ:
			expected_hlen = sizeof(struct spdk_nvme_tcp_term_req_hdr);
			if ((pdu->hdr.common.plen <= expected_hlen) ||
			    (pdu->hdr.common.plen > SPDK_NVME_TCP_TERM_REQ_PDU_MAX_SIZE)) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_R2T:
			expected_hlen = sizeof(struct spdk_nvme_tcp_r2t_hdr);
			if (pdu->hdr.common.flags & SPDK_NVME_TCP_CH_FLAGS_HDGSTF) {
				hd_len = SPDK_NVME_TCP_DIGEST_LEN;
			}

			if (pdu->hdr.common.plen != (expected_hlen + hd_len)) {
				plen_error = true;
			}
			break;

		default:
			SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", tqpair->recv_pdu->hdr.common.pdu_type);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
			error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, pdu_type);
			goto err;
		}
	}

	if (pdu->hdr.common.hlen != expected_hlen) {
		SPDK_ERRLOG("Expected PDU header length %u, got %u\n",
			    expected_hlen, pdu->hdr.common.hlen);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, hlen);
		goto err;

	} else if (plen_error) {
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, plen);
		goto err;
	} else {
		nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
		nvme_tcp_pdu_calc_psh_len(tqpair->recv_pdu, tqpair->flags.host_hdgst_enable);
		return;
	}
err:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
}

static struct nvme_tcp_req *
get_nvme_active_req_by_cid(struct nvme_tcp_qpair *tqpair, uint32_t cid)
{
	assert(tqpair != NULL);
	if ((cid >= tqpair->num_entries) || (tqpair->tcp_reqs[cid].state == NVME_TCP_REQ_FREE)) {
		return NULL;
	}

	return &tqpair->tcp_reqs[cid];
}

static void
nvme_tcp_recv_payload_seq_cb(void *cb_arg, int status)
{
	struct nvme_tcp_req *treq = cb_arg;
	struct nvme_request *req = treq->req;
	struct nvme_tcp_qpair *tqpair = treq->tqpair;

	assert(treq->ordering.bits.in_progress_accel);
	treq->ordering.bits.in_progress_accel = 0;

	nvme_tcp_cond_schedule_qpair_polling(tqpair);

	req->accel_sequence = NULL;
	if (spdk_unlikely(status != 0)) {
		pdu_seq_fail(treq->pdu, status);
		return;
	}

	nvme_tcp_req_complete_safe(treq);
}

static void
nvme_tcp_c2h_data_payload_handle(struct nvme_tcp_qpair *tqpair,
				 struct nvme_tcp_pdu *pdu, uint32_t *reaped)
{
	struct nvme_tcp_req *tcp_req;
	struct nvme_tcp_poll_group *tgroup;
	struct spdk_nvme_tcp_c2h_data_hdr *c2h_data;
	uint8_t flags;

	tcp_req = pdu->req;
	assert(tcp_req != NULL);

	SPDK_DEBUGLOG(nvme, "enter\n");
	c2h_data = &pdu->hdr.c2h_data;
	tcp_req->datao += pdu->data_len;
	flags = c2h_data->common.flags;

	if (flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU) {
		if (tcp_req->datao == tcp_req->req->payload_size) {
			tcp_req->rsp.status.p = 0;
		} else {
			tcp_req->rsp.status.p = 1;
		}

		tcp_req->rsp.cid = tcp_req->cid;
		tcp_req->rsp.sqid = tqpair->qpair.id;
		if (flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS) {
			tcp_req->ordering.bits.data_recv = 1;
			if (tcp_req->req->accel_sequence != NULL) {
				tgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
				nvme_tcp_accel_reverse_sequence(tgroup, tcp_req->req->accel_sequence);
				nvme_tcp_accel_finish_sequence(tgroup, tcp_req,
							       tcp_req->req->accel_sequence,
							       nvme_tcp_recv_payload_seq_cb,
							       tcp_req);
				return;
			}

			if (nvme_tcp_req_complete_safe(tcp_req)) {
				(*reaped)++;
			}
		}
	}
}

static const char *spdk_nvme_tcp_term_req_fes_str[] = {
	"Invalid PDU Header Field",
	"PDU Sequence Error",
	"Header Digest Error",
	"Data Transfer Out of Range",
	"Data Transfer Limit Exceeded",
	"Unsupported parameter",
};

static void
nvme_tcp_c2h_term_req_dump(struct spdk_nvme_tcp_term_req_hdr *c2h_term_req)
{
	SPDK_ERRLOG("Error info of pdu(%p): %s\n", c2h_term_req,
		    spdk_nvme_tcp_term_req_fes_str[c2h_term_req->fes]);
	if ((c2h_term_req->fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (c2h_term_req->fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER)) {
		SPDK_DEBUGLOG(nvme, "The offset from the start of the PDU header is %u\n",
			      DGET32(c2h_term_req->fei));
	}
	/* we may also need to dump some other info here */
}

static void
nvme_tcp_c2h_term_req_payload_handle(struct nvme_tcp_qpair *tqpair,
				     struct nvme_tcp_pdu *pdu)
{
	nvme_tcp_c2h_term_req_dump(&pdu->hdr.term_req);
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
}

static void
_nvme_tcp_pdu_payload_handle(struct nvme_tcp_qpair *tqpair, uint32_t *reaped)
{
	struct nvme_tcp_pdu *pdu;

	assert(tqpair != NULL);
	pdu = tqpair->recv_pdu;

	switch (pdu->hdr.common.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_C2H_DATA:
		nvme_tcp_c2h_data_payload_handle(tqpair, pdu, reaped);
		nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
		break;

	case SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ:
		nvme_tcp_c2h_term_req_payload_handle(tqpair, pdu);
		break;

	default:
		/* The code should not go to here */
		SPDK_ERRLOG("The code should not go to here\n");
		break;
	}
}

static void
nvme_tcp_req_copy_pdu(struct nvme_tcp_req *treq, struct nvme_tcp_pdu *pdu)
{
	treq->pdu->hdr = pdu->hdr;
	treq->pdu->req = treq;
	memcpy(treq->pdu->data_digest, pdu->data_digest, sizeof(pdu->data_digest));
	memcpy(treq->pdu->data_iov, pdu->data_iov, sizeof(pdu->data_iov[0]) * pdu->data_iovcnt);
	treq->pdu->data_iovcnt = pdu->data_iovcnt;
	treq->pdu->data_len = pdu->data_len;
}

static void
nvme_tcp_accel_seq_recv_compute_crc32_done(void *cb_arg)
{
	struct nvme_tcp_req *treq = cb_arg;
	struct nvme_tcp_qpair *tqpair = treq->tqpair;
	struct nvme_tcp_pdu *pdu = treq->pdu;
	bool result;

	pdu->data_digest_crc32 ^= SPDK_CRC32C_XOR;
	result = MATCH_DIGEST_WORD(pdu->data_digest, pdu->data_digest_crc32);
	if (spdk_unlikely(!result)) {
		SPDK_ERRLOG("data digest error on tqpair=(%p)\n", tqpair);
		treq->rsp.status.sc = SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR;
	}
}

static bool
nvme_tcp_accel_recv_compute_crc32(struct nvme_tcp_req *treq, struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_qpair *tqpair = treq->tqpair;
	struct nvme_tcp_poll_group *tgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
	struct nvme_request *req = treq->req;
	int rc, dummy = 0;

	/* Only support this limited case that the request has only one c2h pdu */
	if (spdk_unlikely(nvme_qpair_get_state(&tqpair->qpair) < NVME_QPAIR_CONNECTED ||
			  tqpair->qpair.poll_group == NULL || pdu->dif_ctx != NULL ||
			  pdu->data_len % SPDK_NVME_TCP_DIGEST_ALIGNMENT != 0 ||
			  pdu->data_len != req->payload_size)) {
		return false;
	}

	if (tgroup->group.group->accel_fn_table.append_crc32c == NULL) {
		return false;
	}

	nvme_tcp_req_copy_pdu(treq, pdu);
	rc = nvme_tcp_accel_append_crc32c(tgroup, &req->accel_sequence,
					  &treq->pdu->data_digest_crc32,
					  treq->pdu->data_iov, treq->pdu->data_iovcnt, 0,
					  nvme_tcp_accel_seq_recv_compute_crc32_done, treq);
	if (spdk_unlikely(rc != 0)) {
		/* If accel is out of resources, fall back to non-accelerated crc32 */
		if (rc == -ENOMEM) {
			return false;
		}

		SPDK_ERRLOG("Failed to append crc32c operation: %d\n", rc);
		treq->rsp.status.sc = SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR;
	}

	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	nvme_tcp_c2h_data_payload_handle(tqpair, treq->pdu, &dummy);

	return true;
}

static void
nvme_tcp_pdu_payload_handle(struct nvme_tcp_qpair *tqpair,
			    uint32_t *reaped)
{
	int rc = 0;
	struct nvme_tcp_pdu *pdu = tqpair->recv_pdu;
	uint32_t crc32c;
	struct nvme_tcp_req *tcp_req = pdu->req;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	SPDK_DEBUGLOG(nvme, "enter\n");

	/* The request can be NULL, e.g. in case of C2HTermReq */
	if (spdk_likely(tcp_req != NULL)) {
		tcp_req->expected_datao += pdu->data_len;
	}

	/* check data digest if need */
	if (pdu->ddgst_enable) {
		/* But if the data digest is enabled, tcp_req cannot be NULL */
		assert(tcp_req != NULL);
		if (nvme_tcp_accel_recv_compute_crc32(tcp_req, pdu)) {
			return;
		}

		crc32c = nvme_tcp_pdu_calc_data_digest(pdu);
		crc32c = crc32c ^ SPDK_CRC32C_XOR;
		rc = MATCH_DIGEST_WORD(pdu->data_digest, crc32c);
		if (rc == 0) {
			SPDK_ERRLOG("data digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
			tcp_req = pdu->req;
			assert(tcp_req != NULL);
			tcp_req->rsp.status.sc = SPDK_NVME_SC_COMMAND_TRANSIENT_TRANSPORT_ERROR;
		}
	}

	_nvme_tcp_pdu_payload_handle(tqpair, reaped);
}

static void
nvme_tcp_send_icreq_complete(void *cb_arg)
{
	struct nvme_tcp_qpair *tqpair = cb_arg;

	SPDK_DEBUGLOG(nvme, "Complete the icreq send for tqpair=%p %u\n", tqpair, tqpair->qpair.id);

	tqpair->flags.icreq_send_ack = true;

	if (tqpair->state == NVME_TCP_QPAIR_STATE_INITIALIZING) {
		SPDK_DEBUGLOG(nvme, "tqpair %p %u, finalize icresp\n", tqpair, tqpair->qpair.id);
		tqpair->state = NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_SEND;
	}
}

static void
nvme_tcp_icresp_handle(struct nvme_tcp_qpair *tqpair,
		       struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_ic_resp *ic_resp = &pdu->hdr.ic_resp;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	int recv_buf_size;

	/* Only PFV 0 is defined currently */
	if (ic_resp->pfv != 0) {
		SPDK_ERRLOG("Expected ICResp PFV %u, got %u\n", 0u, ic_resp->pfv);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_ic_resp, pfv);
		goto end;
	}

	if (ic_resp->maxh2cdata < NVME_TCP_PDU_H2C_MIN_DATA_SIZE) {
		SPDK_ERRLOG("Expected ICResp maxh2cdata >=%u, got %u\n", NVME_TCP_PDU_H2C_MIN_DATA_SIZE,
			    ic_resp->maxh2cdata);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_ic_resp, maxh2cdata);
		goto end;
	}
	tqpair->maxh2cdata = ic_resp->maxh2cdata;

	if (ic_resp->cpda > SPDK_NVME_TCP_CPDA_MAX) {
		SPDK_ERRLOG("Expected ICResp cpda <=%u, got %u\n", SPDK_NVME_TCP_CPDA_MAX, ic_resp->cpda);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_ic_resp, cpda);
		goto end;
	}
	tqpair->cpda = ic_resp->cpda;

	tqpair->flags.host_hdgst_enable = ic_resp->dgst.bits.hdgst_enable ? true : false;
	tqpair->flags.host_ddgst_enable = ic_resp->dgst.bits.ddgst_enable ? true : false;
	SPDK_DEBUGLOG(nvme, "host_hdgst_enable: %u\n", tqpair->flags.host_hdgst_enable);
	SPDK_DEBUGLOG(nvme, "host_ddgst_enable: %u\n", tqpair->flags.host_ddgst_enable);

	/* Now that we know whether digests are enabled, properly size the receive buffer to
	 * handle several incoming 4K read commands according to SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR
	 * parameter. */
	recv_buf_size = 0x1000 + sizeof(struct spdk_nvme_tcp_c2h_data_hdr);

	if (tqpair->flags.host_hdgst_enable) {
		recv_buf_size += SPDK_NVME_TCP_DIGEST_LEN;
	}

	if (tqpair->flags.host_ddgst_enable) {
		recv_buf_size += SPDK_NVME_TCP_DIGEST_LEN;
	}

	if (spdk_sock_set_recvbuf(tqpair->sock, recv_buf_size * SPDK_NVMF_TCP_RECV_BUF_SIZE_FACTOR) < 0) {
		SPDK_WARNLOG("Unable to allocate enough memory for receive buffer on tqpair=%p with size=%d\n",
			     tqpair,
			     recv_buf_size);
		/* Not fatal. */
	}

	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);

	if (!tqpair->flags.icreq_send_ack) {
		tqpair->state = NVME_TCP_QPAIR_STATE_INITIALIZING;
		SPDK_DEBUGLOG(nvme, "tqpair %p %u, waiting icreq ack\n", tqpair, tqpair->qpair.id);
		return;
	}

	tqpair->state = NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_SEND;
	return;
end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
}

static void
nvme_tcp_capsule_resp_hdr_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu,
				 uint32_t *reaped)
{
	struct nvme_tcp_req *tcp_req;
	struct nvme_tcp_poll_group *tgroup;
	struct spdk_nvme_tcp_rsp *capsule_resp = &pdu->hdr.capsule_resp;
	uint32_t cid, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	SPDK_DEBUGLOG(nvme, "enter\n");
	cid = capsule_resp->rccqe.cid;
	tcp_req = get_nvme_active_req_by_cid(tqpair, cid);

	if (!tcp_req) {
		SPDK_ERRLOG("no tcp_req is found with cid=%u for tqpair=%p\n", cid, tqpair);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_rsp, rccqe);
		goto end;
	}

	assert(tcp_req->req != NULL);

	tcp_req->rsp = capsule_resp->rccqe;
	tcp_req->ordering.bits.data_recv = 1;

	/* Recv the pdu again */
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);

	if (tcp_req->req->accel_sequence != NULL) {
		tgroup = nvme_tcp_poll_group(tqpair->qpair.poll_group);
		nvme_tcp_accel_reverse_sequence(tgroup, tcp_req->req->accel_sequence);
		nvme_tcp_accel_finish_sequence(tgroup, tcp_req, tcp_req->req->accel_sequence,
					       nvme_tcp_recv_payload_seq_cb, tcp_req);
		return;
	}

	if (nvme_tcp_req_complete_safe(tcp_req)) {
		(*reaped)++;
	}

	return;

end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
}

static void
nvme_tcp_c2h_term_req_hdr_handle(struct nvme_tcp_qpair *tqpair,
				 struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_term_req_hdr *c2h_term_req = &pdu->hdr.term_req;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	if (c2h_term_req->fes > SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER) {
		SPDK_ERRLOG("Fatal Error Status(FES) is unknown for c2h_term_req pdu=%p\n", pdu);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_term_req_hdr, fes);
		goto end;
	}

	/* set the data buffer */
	nvme_tcp_pdu_set_data(pdu, (uint8_t *)pdu->hdr.raw + c2h_term_req->common.hlen,
			      c2h_term_req->common.plen - c2h_term_req->common.hlen);
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	return;
end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
}

static void
nvme_tcp_c2h_data_hdr_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req;
	struct spdk_nvme_tcp_c2h_data_hdr *c2h_data = &pdu->hdr.c2h_data;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	int flags = c2h_data->common.flags;
	int rc;

	SPDK_DEBUGLOG(nvme, "enter\n");
	SPDK_DEBUGLOG(nvme, "c2h_data info on tqpair(%p): datao=%u, datal=%u, cccid=%d\n",
		      tqpair, c2h_data->datao, c2h_data->datal, c2h_data->cccid);
	tcp_req = get_nvme_active_req_by_cid(tqpair, c2h_data->cccid);
	if (!tcp_req) {
		SPDK_ERRLOG("no tcp_req found for c2hdata cid=%d\n", c2h_data->cccid);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_c2h_data_hdr, cccid);
		goto end;

	}

	SPDK_DEBUGLOG(nvme, "tcp_req(%p) on tqpair(%p): expected_datao=%u, payload_size=%u\n",
		      tcp_req, tqpair, tcp_req->expected_datao, tcp_req->req->payload_size);

	if (spdk_unlikely((flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS) &&
			  !(flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_LAST_PDU))) {
		SPDK_ERRLOG("Invalid flag flags=%d in c2h_data=%p\n", flags, c2h_data);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_c2h_data_hdr, common);
		goto end;
	}

	if (c2h_data->datal > tcp_req->req->payload_size) {
		SPDK_ERRLOG("Invalid datal for tcp_req(%p), datal(%u) exceeds payload_size(%u)\n",
			    tcp_req, c2h_data->datal, tcp_req->req->payload_size);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE;
		goto end;
	}

	if (tcp_req->expected_datao != c2h_data->datao) {
		SPDK_ERRLOG("Invalid datao for tcp_req(%p), received datal(%u) != expected datao(%u) in tcp_req\n",
			    tcp_req, c2h_data->datao, tcp_req->expected_datao);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_c2h_data_hdr, datao);
		goto end;
	}

	if ((c2h_data->datao + c2h_data->datal) > tcp_req->req->payload_size) {
		SPDK_ERRLOG("Invalid data range for tcp_req(%p), received (datao(%u) + datal(%u)) > datao(%u) in tcp_req\n",
			    tcp_req, c2h_data->datao, c2h_data->datal, tcp_req->req->payload_size);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE;
		error_offset = offsetof(struct spdk_nvme_tcp_c2h_data_hdr, datal);
		goto end;

	}

	if (nvme_payload_type(&tcp_req->req->payload) == NVME_PAYLOAD_TYPE_CONTIG) {
		rc = nvme_tcp_build_contig_request(tqpair, tcp_req);
	} else {
		assert(nvme_payload_type(&tcp_req->req->payload) == NVME_PAYLOAD_TYPE_SGL);
		rc = nvme_tcp_build_sgl_request(tqpair, tcp_req);
	}

	if (rc) {
		/* Not the right error message but at least it handles the failure. */
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_LIMIT_EXCEEDED;
		goto end;
	}

	nvme_tcp_pdu_set_data_buf(pdu, tcp_req->iov, tcp_req->iovcnt,
				  c2h_data->datao, c2h_data->datal);
	pdu->req = tcp_req;

	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	return;

end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
}

static void
nvme_tcp_qpair_h2c_data_send_complete(void *cb_arg)
{
	struct nvme_tcp_req *tcp_req = cb_arg;

	assert(tcp_req != NULL);

	tcp_req->ordering.bits.send_ack = 1;
	if (tcp_req->r2tl_remain) {
		nvme_tcp_send_h2c_data(tcp_req);
	} else {
		assert(tcp_req->active_r2ts > 0);
		tcp_req->active_r2ts--;
		tcp_req->state = NVME_TCP_REQ_ACTIVE;

		if (tcp_req->ordering.bits.r2t_waiting_h2c_complete) {
			tcp_req->ordering.bits.r2t_waiting_h2c_complete = 0;
			SPDK_DEBUGLOG(nvme, "tcp_req %p: continue r2t\n", tcp_req);
			assert(tcp_req->active_r2ts > 0);
			tcp_req->ttag = tcp_req->ttag_r2t_next;
			tcp_req->r2tl_remain = tcp_req->r2tl_remain_next;
			tcp_req->state = NVME_TCP_REQ_ACTIVE_R2T;
			nvme_tcp_send_h2c_data(tcp_req);
			return;
		}

		if (tcp_req->ordering.bits.domain_in_use) {
			spdk_memory_domain_invalidate_data(tcp_req->req->payload.opts->memory_domain,
							   tcp_req->req->payload.opts->memory_domain_ctx, tcp_req->iov, tcp_req->iovcnt);
		}

		/* Need also call this function to free the resource */
		nvme_tcp_req_complete_safe(tcp_req);
	}
}

static void
nvme_tcp_send_h2c_data(struct nvme_tcp_req *tcp_req)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(tcp_req->req->qpair);
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_h2c_data_hdr *h2c_data;
	uint32_t plen, pdo, alignment;

	/* Reinit the send_ack and h2c_send_waiting_ack bits */
	tcp_req->ordering.bits.send_ack = 0;
	tcp_req->ordering.bits.h2c_send_waiting_ack = 0;
	rsp_pdu = tcp_req->pdu;
	memset(rsp_pdu, 0, sizeof(*rsp_pdu));
	rsp_pdu->req = tcp_req;
	h2c_data = &rsp_pdu->hdr.h2c_data;

	h2c_data->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_H2C_DATA;
	plen = h2c_data->common.hlen = sizeof(*h2c_data);
	h2c_data->cccid = tcp_req->cid;
	h2c_data->ttag = tcp_req->ttag;
	h2c_data->datao = tcp_req->datao;

	h2c_data->datal = spdk_min(tcp_req->r2tl_remain, tqpair->maxh2cdata);
	nvme_tcp_pdu_set_data_buf(rsp_pdu, tcp_req->iov, tcp_req->iovcnt,
				  h2c_data->datao, h2c_data->datal);
	tcp_req->r2tl_remain -= h2c_data->datal;

	if (tqpair->flags.host_hdgst_enable) {
		h2c_data->common.flags |= SPDK_NVME_TCP_CH_FLAGS_HDGSTF;
		plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	rsp_pdu->padding_len = 0;
	pdo = plen;
	if (tqpair->cpda) {
		alignment = (tqpair->cpda + 1) << 2;
		if (alignment > plen) {
			rsp_pdu->padding_len = alignment - plen;
			pdo = plen = alignment;
		}
	}

	h2c_data->common.pdo = pdo;
	plen += h2c_data->datal;
	if (tqpair->flags.host_ddgst_enable) {
		h2c_data->common.flags |= SPDK_NVME_TCP_CH_FLAGS_DDGSTF;
		plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	h2c_data->common.plen = plen;
	tcp_req->datao += h2c_data->datal;
	if (!tcp_req->r2tl_remain) {
		h2c_data->common.flags |= SPDK_NVME_TCP_H2C_DATA_FLAGS_LAST_PDU;
	}

	SPDK_DEBUGLOG(nvme, "h2c_data info: datao=%u, datal=%u, pdu_len=%u for tqpair=%p\n",
		      h2c_data->datao, h2c_data->datal, h2c_data->common.plen, tqpair);

	nvme_tcp_qpair_write_pdu(tqpair, rsp_pdu, nvme_tcp_qpair_h2c_data_send_complete, tcp_req);
}

static void
nvme_tcp_r2t_hdr_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req;
	struct spdk_nvme_tcp_r2t_hdr *r2t = &pdu->hdr.r2t;
	uint32_t cid, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	SPDK_DEBUGLOG(nvme, "enter\n");
	cid = r2t->cccid;
	tcp_req = get_nvme_active_req_by_cid(tqpair, cid);
	if (!tcp_req) {
		SPDK_ERRLOG("Cannot find tcp_req for tqpair=%p\n", tqpair);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_r2t_hdr, cccid);
		goto end;
	}

	SPDK_DEBUGLOG(nvme, "r2t info: r2to=%u, r2tl=%u for tqpair=%p\n", r2t->r2to, r2t->r2tl,
		      tqpair);

	if (tcp_req->state == NVME_TCP_REQ_ACTIVE) {
		assert(tcp_req->active_r2ts == 0);
		tcp_req->state = NVME_TCP_REQ_ACTIVE_R2T;
	}

	if (tcp_req->datao != r2t->r2to) {
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_r2t_hdr, r2to);
		goto end;

	}

	if ((r2t->r2tl + r2t->r2to) > tcp_req->req->payload_size) {
		SPDK_ERRLOG("Invalid R2T info for tcp_req=%p: (r2to(%u) + r2tl(%u)) exceeds payload_size(%u)\n",
			    tcp_req, r2t->r2to, r2t->r2tl, tqpair->maxh2cdata);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE;
		error_offset = offsetof(struct spdk_nvme_tcp_r2t_hdr, r2tl);
		goto end;
	}

	tcp_req->active_r2ts++;
	if (spdk_unlikely(tcp_req->active_r2ts > tqpair->maxr2t)) {
		if (tcp_req->state == NVME_TCP_REQ_ACTIVE_R2T && !tcp_req->ordering.bits.send_ack) {
			/* We receive a subsequent R2T while we are waiting for H2C transfer to complete */
			SPDK_DEBUGLOG(nvme, "received a subsequent R2T\n");
			assert(tcp_req->active_r2ts == tqpair->maxr2t + 1);
			tcp_req->ttag_r2t_next = r2t->ttag;
			tcp_req->r2tl_remain_next = r2t->r2tl;
			tcp_req->ordering.bits.r2t_waiting_h2c_complete = 1;
			nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
			return;
		} else {
			fes = SPDK_NVME_TCP_TERM_REQ_FES_R2T_LIMIT_EXCEEDED;
			SPDK_ERRLOG("Invalid R2T: Maximum number of R2T exceeded! Max: %u for tqpair=%p\n", tqpair->maxr2t,
				    tqpair);
			goto end;
		}
	}

	tcp_req->ttag = r2t->ttag;
	tcp_req->r2tl_remain = r2t->r2tl;
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);

	if (spdk_likely(tcp_req->ordering.bits.send_ack)) {
		nvme_tcp_send_h2c_data(tcp_req);
	} else {
		tcp_req->ordering.bits.h2c_send_waiting_ack = 1;
	}

	return;

end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);

}

static void
nvme_tcp_pdu_psh_handle(struct nvme_tcp_qpair *tqpair, uint32_t *reaped)
{
	struct nvme_tcp_pdu *pdu;
	int rc;
	uint32_t crc32c, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
	pdu = tqpair->recv_pdu;

	SPDK_DEBUGLOG(nvme, "enter: pdu type =%u\n", pdu->hdr.common.pdu_type);
	/* check header digest if needed */
	if (pdu->has_hdgst) {
		crc32c = nvme_tcp_pdu_calc_header_digest(pdu);
		rc = MATCH_DIGEST_WORD((uint8_t *)pdu->hdr.raw + pdu->hdr.common.hlen, crc32c);
		if (rc == 0) {
			SPDK_ERRLOG("header digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_HDGST_ERROR;
			nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
			return;

		}
	}

	switch (pdu->hdr.common.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_IC_RESP:
		nvme_tcp_icresp_handle(tqpair, pdu);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP:
		nvme_tcp_capsule_resp_hdr_handle(tqpair, pdu, reaped);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_C2H_DATA:
		nvme_tcp_c2h_data_hdr_handle(tqpair, pdu);
		break;

	case SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ:
		nvme_tcp_c2h_term_req_hdr_handle(tqpair, pdu);
		break;
	case SPDK_NVME_TCP_PDU_TYPE_R2T:
		nvme_tcp_r2t_hdr_handle(tqpair, pdu);
		break;

	default:
		SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", tqpair->recv_pdu->hdr.common.pdu_type);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = 1;
		nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
		break;
	}

}

static int
nvme_tcp_read_pdu(struct nvme_tcp_qpair *tqpair, uint32_t *reaped, uint32_t max_completions)
{
	int rc = 0;
	struct nvme_tcp_pdu *pdu;
	uint32_t data_len;
	enum nvme_tcp_pdu_recv_state prev_state;

	*reaped = tqpair->async_complete;
	tqpair->async_complete = 0;

	/* The loop here is to allow for several back-to-back state changes. */
	do {
		if (*reaped >= max_completions) {
			break;
		}

		prev_state = tqpair->recv_state;
		pdu = tqpair->recv_pdu;
		switch (tqpair->recv_state) {
		/* If in a new state */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY:
			memset(pdu, 0, sizeof(struct nvme_tcp_pdu));
			nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH);
			break;
		/* Wait for the pdu common header */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH:
			assert(pdu->ch_valid_bytes < sizeof(struct spdk_nvme_tcp_common_pdu_hdr));
			rc = nvme_tcp_read_data(tqpair->sock,
						sizeof(struct spdk_nvme_tcp_common_pdu_hdr) - pdu->ch_valid_bytes,
						(uint8_t *)&pdu->hdr.common + pdu->ch_valid_bytes);
			if (rc < 0) {
				nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
				break;
			}
			pdu->ch_valid_bytes += rc;
			if (pdu->ch_valid_bytes < sizeof(struct spdk_nvme_tcp_common_pdu_hdr)) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			/* The command header of this PDU has now been read from the socket. */
			nvme_tcp_pdu_ch_handle(tqpair);
			break;
		/* Wait for the pdu specific header  */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH:
			assert(pdu->psh_valid_bytes < pdu->psh_len);
			rc = nvme_tcp_read_data(tqpair->sock,
						pdu->psh_len - pdu->psh_valid_bytes,
						(uint8_t *)&pdu->hdr.raw + sizeof(struct spdk_nvme_tcp_common_pdu_hdr) + pdu->psh_valid_bytes);
			if (rc < 0) {
				nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
				break;
			}

			pdu->psh_valid_bytes += rc;
			if (pdu->psh_valid_bytes < pdu->psh_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			/* All header(ch, psh, head digits) of this PDU has now been read from the socket. */
			nvme_tcp_pdu_psh_handle(tqpair, reaped);
			break;
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD:
			/* check whether the data is valid, if not we just return */
			if (!pdu->data_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			data_len = pdu->data_len;
			/* data digest */
			if (spdk_unlikely((pdu->hdr.common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_DATA) &&
					  tqpair->flags.host_ddgst_enable)) {
				data_len += SPDK_NVME_TCP_DIGEST_LEN;
				pdu->ddgst_enable = true;
			}

			rc = nvme_tcp_read_payload_data(tqpair->sock, pdu);
			if (rc < 0) {
				nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_QUIESCING);
				break;
			}

			pdu->rw_offset += rc;
			if (pdu->rw_offset < data_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			assert(pdu->rw_offset == data_len);
			/* All of this PDU has now been read from the socket. */
			nvme_tcp_pdu_payload_handle(tqpair, reaped);
			break;
		case NVME_TCP_PDU_RECV_STATE_QUIESCING:
			if (TAILQ_EMPTY(&tqpair->outstanding_reqs)) {
				if (nvme_qpair_get_state(&tqpair->qpair) == NVME_QPAIR_DISCONNECTING) {
					nvme_transport_ctrlr_disconnect_qpair_done(&tqpair->qpair);
				}

				nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
			}
			break;
		case NVME_TCP_PDU_RECV_STATE_ERROR:
			memset(pdu, 0, sizeof(struct nvme_tcp_pdu));
			return NVME_TCP_PDU_FATAL;
		default:
			assert(0);
			break;
		}
	} while (prev_state != tqpair->recv_state);

	return rc > 0 ? 0 : rc;
}

static void
nvme_tcp_qpair_check_timeout(struct spdk_nvme_qpair *qpair)
{
	uint64_t t02;
	struct nvme_tcp_req *tcp_req, *tmp;
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvme_ctrlr_process *active_proc;

	/* Don't check timeouts during controller initialization. */
	if (spdk_unlikely(ctrlr->state != NVME_CTRLR_STATE_READY)) {
		return;
	}

	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
		active_proc = nvme_ctrlr_get_current_process(ctrlr);
	} else {
		active_proc = qpair->active_proc;
	}

	/* Only check timeouts if the current process has a timeout callback. */
	if (spdk_unlikely(active_proc == NULL || active_proc->timeout_cb_fn == NULL)) {
		return;
	}

	t02 = spdk_get_ticks();
	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->outstanding_reqs, link, tmp) {
		if (spdk_unlikely(ctrlr->is_failed)) {
			/* The controller state may be changed to failed in one of the nvme_request_check_timeout callbacks. */
			return;
		}
		assert(tcp_req->req != NULL);

		if (spdk_likely(nvme_request_check_timeout(tcp_req->req, tcp_req->cid, active_proc, t02))) {
			/*
			 * The requests are in order, so as soon as one has not timed out,
			 * stop iterating.
			 */
			break;
		}
	}
}

static int nvme_tcp_ctrlr_connect_qpair_poll(struct spdk_nvme_ctrlr *ctrlr,
		struct spdk_nvme_qpair *qpair);

static int
nvme_tcp_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	uint32_t reaped;
	int rc;

	if (qpair->poll_group == NULL) {
		if (qpair->ctrlr->timeout_enabled) {
			nvme_tcp_qpair_check_timeout(qpair);
		}

		rc = spdk_sock_flush(tqpair->sock);
		if (rc < 0 && errno != EAGAIN) {
			SPDK_ERRLOG("Failed to flush tqpair=%p (%d): %s\n", tqpair,
				    errno, spdk_strerror(errno));
			if (nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTING) {
				if (TAILQ_EMPTY(&tqpair->outstanding_reqs)) {
					nvme_transport_ctrlr_disconnect_qpair_done(qpair);
				}

				/* Don't return errors until the qpair gets disconnected */
				return 0;
			}

			goto fail;
		}
	}

	if (max_completions == 0) {
		max_completions = spdk_max(tqpair->num_entries, 1);
	} else {
		max_completions = spdk_min(max_completions, tqpair->num_entries);
	}

	reaped = 0;
	rc = nvme_tcp_read_pdu(tqpair, &reaped, max_completions);
	if (rc < 0) {
		SPDK_DEBUGLOG(nvme, "Error polling CQ! (%d): %s\n",
			      errno, spdk_strerror(errno));
		goto fail;
	}

	if (spdk_unlikely(nvme_qpair_get_state(qpair) == NVME_QPAIR_CONNECTING)) {
		rc = nvme_tcp_ctrlr_connect_qpair_poll(qpair->ctrlr, qpair);
		if (rc != 0 && rc != -EAGAIN) {
			SPDK_ERRLOG("Failed to connect tqpair=%p\n", tqpair);
			goto fail;
		} else if (rc == 0) {
			/* Once the connection is completed, we can submit queued requests */
			nvme_qpair_resubmit_requests(qpair, tqpair->num_entries);
		}
	}

	return reaped;
fail:

	/*
	 * Since admin queues take the ctrlr_lock before entering this function,
	 * we can call nvme_transport_ctrlr_disconnect_qpair. For other qpairs we need
	 * to call the generic function which will take the lock for us.
	 */
	qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_UNKNOWN;

	if (nvme_qpair_is_admin_queue(qpair)) {
		enum nvme_qpair_state state_prev = nvme_qpair_get_state(qpair);

		nvme_transport_ctrlr_disconnect_qpair(qpair->ctrlr, qpair);

		if (state_prev == NVME_QPAIR_CONNECTING && qpair->poll_status != NULL) {
			/* Needed to free the poll_status */
			nvme_tcp_ctrlr_connect_qpair_poll(qpair->ctrlr, qpair);
		}
	} else {
		nvme_ctrlr_disconnect_qpair(qpair);
	}
	return -ENXIO;
}

static void
nvme_tcp_qpair_sock_cb(void *ctx, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_nvme_qpair *qpair = ctx;
	struct nvme_tcp_poll_group *pgroup = nvme_tcp_poll_group(qpair->poll_group);
	int32_t num_completions;
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	if (TAILQ_ENTRY_ENQUEUED(tqpair, link_poll)) {
		TAILQ_REMOVE_CLEAR(&pgroup->needs_poll, tqpair, link_poll);
	}

	num_completions = spdk_nvme_qpair_process_completions(qpair, pgroup->completions_per_qpair);

	if (pgroup->num_completions >= 0 && num_completions >= 0) {
		pgroup->num_completions += num_completions;
		pgroup->stats.nvme_completions += num_completions;
	} else {
		pgroup->num_completions = -ENXIO;
	}
}

static int
nvme_tcp_qpair_icreq_send(struct nvme_tcp_qpair *tqpair)
{
	struct spdk_nvme_tcp_ic_req *ic_req;
	struct nvme_tcp_pdu *pdu;
	uint32_t timeout_in_sec;

	pdu = tqpair->send_pdu;
	memset(tqpair->send_pdu, 0, sizeof(*tqpair->send_pdu));
	ic_req = &pdu->hdr.ic_req;

	ic_req->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_REQ;
	ic_req->common.hlen = ic_req->common.plen = sizeof(*ic_req);
	ic_req->pfv = 0;
	ic_req->maxr2t = NVME_TCP_MAX_R2T_DEFAULT - 1;
	ic_req->hpda = NVME_TCP_HPDA_DEFAULT;

	ic_req->dgst.bits.hdgst_enable = tqpair->qpair.ctrlr->opts.header_digest;
	ic_req->dgst.bits.ddgst_enable = tqpair->qpair.ctrlr->opts.data_digest;

	nvme_tcp_qpair_write_pdu(tqpair, pdu, nvme_tcp_send_icreq_complete, tqpair);

	timeout_in_sec = tqpair->qpair.async ? ICREQ_TIMEOUT_ASYNC : ICREQ_TIMEOUT_SYNC;
	tqpair->icreq_timeout_tsc = spdk_get_ticks() + (timeout_in_sec * spdk_get_ticks_hz());
	return 0;
}

static void
nvme_tcp_sock_connect_cb_fn(void *cb_arg, int status)
{
	struct nvme_tcp_qpair *tqpair = cb_arg;

	if (status < 0) {
		SPDK_ERRLOG("sock connection error of tqpair=%p with %d (%s)\n", tqpair, status,
			    spdk_strerror(abs(status)));
	}
}

static int
nvme_tcp_qpair_connect_sock(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct sockaddr_storage dst_addr;
	struct sockaddr_storage src_addr;
	int rc;
	struct nvme_tcp_qpair *tqpair;
	int family;
	long int port, src_port = 0;
	char *sock_impl_name;
	struct spdk_sock_impl_opts impl_opts = {};
	size_t impl_opts_size = sizeof(impl_opts);
	struct spdk_sock_opts opts;
	struct nvme_tcp_ctrlr *tcp_ctrlr;

	tqpair = nvme_tcp_qpair(qpair);

	switch (ctrlr->trid.adrfam) {
	case SPDK_NVMF_ADRFAM_IPV4:
		family = AF_INET;
		break;
	case SPDK_NVMF_ADRFAM_IPV6:
		family = AF_INET6;
		break;
	default:
		SPDK_ERRLOG("Unhandled ADRFAM %d\n", ctrlr->trid.adrfam);
		rc = -1;
		return rc;
	}

	SPDK_DEBUGLOG(nvme, "adrfam %d ai_family %d\n", ctrlr->trid.adrfam, family);

	memset(&dst_addr, 0, sizeof(dst_addr));

	SPDK_DEBUGLOG(nvme, "trsvcid is %s\n", ctrlr->trid.trsvcid);
	rc = nvme_parse_addr(&dst_addr, family, ctrlr->trid.traddr, ctrlr->trid.trsvcid, &port);
	if (rc != 0) {
		SPDK_ERRLOG("dst_addr nvme_parse_addr() failed\n");
		return rc;
	}

	if (ctrlr->opts.src_addr[0] || ctrlr->opts.src_svcid[0]) {
		memset(&src_addr, 0, sizeof(src_addr));
		rc = nvme_parse_addr(&src_addr, family,
				     ctrlr->opts.src_addr[0] ? ctrlr->opts.src_addr : NULL,
				     ctrlr->opts.src_svcid[0] ? ctrlr->opts.src_svcid : NULL,
				     &src_port);
		if (rc != 0) {
			SPDK_ERRLOG("src_addr nvme_parse_addr() failed\n");
			return rc;
		}
	}

	tcp_ctrlr = SPDK_CONTAINEROF(ctrlr, struct nvme_tcp_ctrlr, ctrlr);
	sock_impl_name = tcp_ctrlr->psk[0] ? "ssl" : NULL;
	SPDK_DEBUGLOG(nvme, "sock_impl_name is %s\n", sock_impl_name);

	if (sock_impl_name) {
		spdk_sock_impl_get_opts(sock_impl_name, &impl_opts, &impl_opts_size);
		impl_opts.tls_version = SPDK_TLS_VERSION_1_3;
		impl_opts.psk_identity = tcp_ctrlr->psk_identity;
		impl_opts.psk_key = tcp_ctrlr->psk;
		impl_opts.psk_key_size = tcp_ctrlr->psk_size;
		impl_opts.tls_cipher_suites = tcp_ctrlr->tls_cipher_suite;
	}
	opts.opts_size = sizeof(opts);
	spdk_sock_get_default_opts(&opts);
	opts.priority = ctrlr->trid.priority;
	opts.zcopy = !nvme_qpair_is_admin_queue(qpair);
	opts.src_addr = ctrlr->opts.src_addr[0] ? ctrlr->opts.src_addr : NULL;
	opts.src_port = src_port;
	if (ctrlr->opts.transport_ack_timeout) {
		opts.ack_timeout = 1ULL << ctrlr->opts.transport_ack_timeout;
	}

	opts.connect_timeout = g_spdk_nvme_transport_opts.tcp_connect_timeout_ms;

	if (sock_impl_name) {
		opts.impl_opts = &impl_opts;
		opts.impl_opts_size = sizeof(impl_opts);
	}

	tqpair->sock = spdk_sock_connect_async(ctrlr->trid.traddr, port, sock_impl_name, &opts,
					       nvme_tcp_sock_connect_cb_fn, tqpair);
	if (!tqpair->sock) {
		SPDK_ERRLOG("sock connection error of tqpair=%p with addr=%s, port=%ld\n",
			    tqpair, ctrlr->trid.traddr, port);
		rc = -1;
		return rc;
	}

	return 0;
}

static int
nvme_tcp_ctrlr_connect_qpair_poll(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair;
	int rc;

	tqpair = nvme_tcp_qpair(qpair);

	/* Prevent this function from being called recursively, as it could lead to issues with
	 * nvme_fabric_qpair_connect_poll() if the connect response is received in the recursive
	 * call.
	 */
	if (tqpair->flags.in_connect_poll) {
		return -EAGAIN;
	}

	tqpair->flags.in_connect_poll = 1;

	switch (tqpair->state) {
	case NVME_TCP_QPAIR_STATE_INVALID:
	case NVME_TCP_QPAIR_STATE_INITIALIZING:
		if (spdk_get_ticks() > tqpair->icreq_timeout_tsc) {
			SPDK_ERRLOG("Failed to construct the tqpair=%p via correct icresp\n", tqpair);
			rc = -ETIMEDOUT;
			break;
		}
		rc = -EAGAIN;
		break;
	case NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_SEND:
		rc = nvme_fabric_qpair_connect_async(&tqpair->qpair, tqpair->num_entries + 1);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to send an NVMe-oF Fabric CONNECT command\n");
			break;
		}
		tqpair->state = NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL;
		rc = -EAGAIN;
		break;
	case NVME_TCP_QPAIR_STATE_FABRIC_CONNECT_POLL:
		rc = nvme_fabric_qpair_connect_poll(&tqpair->qpair);
		if (rc == 0) {
			if (nvme_fabric_qpair_auth_required(qpair)) {
				rc = nvme_fabric_qpair_authenticate_async(qpair);
				if (rc == 0) {
					tqpair->state = NVME_TCP_QPAIR_STATE_AUTHENTICATING;
					rc = -EAGAIN;
				}
			} else {
				tqpair->state = NVME_TCP_QPAIR_STATE_RUNNING;
				nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
			}
		} else if (rc != -EAGAIN) {
			SPDK_ERRLOG("Failed to poll NVMe-oF Fabric CONNECT command\n");
		}
		break;
	case NVME_TCP_QPAIR_STATE_AUTHENTICATING:
		rc = nvme_fabric_qpair_authenticate_poll(qpair);
		if (rc == 0) {
			tqpair->state = NVME_TCP_QPAIR_STATE_RUNNING;
			nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
		}
		break;
	case NVME_TCP_QPAIR_STATE_RUNNING:
		rc = 0;
		break;
	default:
		assert(false);
		rc = -EINVAL;
		break;
	}

	tqpair->flags.in_connect_poll = 0;
	return rc;
}

static int
nvme_tcp_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	int rc = 0;
	struct nvme_tcp_qpair *tqpair;
	struct nvme_tcp_poll_group *tgroup;

	tqpair = nvme_tcp_qpair(qpair);

	if (!tqpair->sock) {
		rc = nvme_tcp_qpair_connect_sock(ctrlr, qpair);
		if (rc < 0) {
			return rc;
		}
	}

	if (qpair->poll_group) {
		rc = nvme_poll_group_connect_qpair(qpair);
		if (rc) {
			SPDK_ERRLOG("Unable to activate the tcp qpair.\n");
			return rc;
		}
		tgroup = nvme_tcp_poll_group(qpair->poll_group);
		tqpair->stats = &tgroup->stats;
		tqpair->shared_stats = true;
	} else {
		/* When resetting a controller, we disconnect adminq and then reconnect. The stats
		 * is not freed when disconnecting. So when reconnecting, don't allocate memory
		 * again.
		 */
		if (tqpair->stats == NULL) {
			tqpair->stats = calloc(1, sizeof(*tqpair->stats));
			if (!tqpair->stats) {
				SPDK_ERRLOG("tcp stats memory allocation failed\n");
				return -ENOMEM;
			}
		}
	}

	tqpair->maxr2t = NVME_TCP_MAX_R2T_DEFAULT;
	/* Explicitly set the state and recv_state of tqpair */
	tqpair->state = NVME_TCP_QPAIR_STATE_INVALID;
	if (tqpair->recv_state != NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY) {
		nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	}
	rc = nvme_tcp_qpair_icreq_send(tqpair);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to connect the tqpair\n");
		return rc;
	}

	return rc;
}

static struct spdk_nvme_qpair *
nvme_tcp_ctrlr_create_qpair(struct spdk_nvme_ctrlr *ctrlr,
			    uint16_t qid, uint32_t qsize,
			    enum spdk_nvme_qprio qprio,
			    uint32_t num_requests, bool async)
{
	struct nvme_tcp_qpair *tqpair;
	struct spdk_nvme_qpair *qpair;
	int rc;

	if (qsize < SPDK_NVME_QUEUE_MIN_ENTRIES) {
		SPDK_ERRLOG("Failed to create qpair with size %u. Minimum queue size is %d.\n",
			    qsize, SPDK_NVME_QUEUE_MIN_ENTRIES);
		return NULL;
	}

	tqpair = calloc(1, sizeof(struct nvme_tcp_qpair));
	if (!tqpair) {
		SPDK_ERRLOG("failed to get create tqpair\n");
		return NULL;
	}

	/* Set num_entries one less than queue size. According to NVMe
	 * and NVMe-oF specs we can not submit queue size requests,
	 * one slot shall always remain empty.
	 */
	tqpair->num_entries = qsize - 1;
	qpair = &tqpair->qpair;
	rc = nvme_qpair_init(qpair, qid, ctrlr, qprio, num_requests, async);
	if (rc != 0) {
		free(tqpair);
		return NULL;
	}

	rc = nvme_tcp_alloc_reqs(tqpair);
	if (rc) {
		nvme_tcp_ctrlr_delete_io_qpair(ctrlr, qpair);
		return NULL;
	}

	/* spdk_nvme_qpair_get_optimal_poll_group needs socket information.
	 * So create the socket first when creating a qpair. */
	rc = nvme_tcp_qpair_connect_sock(ctrlr, qpair);
	if (rc) {
		nvme_tcp_ctrlr_delete_io_qpair(ctrlr, qpair);
		return NULL;
	}

	return qpair;
}

static struct spdk_nvme_qpair *
nvme_tcp_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
			       const struct spdk_nvme_io_qpair_opts *opts)
{
	return nvme_tcp_ctrlr_create_qpair(ctrlr, qid, opts->io_queue_size, opts->qprio,
					   opts->io_queue_requests, opts->async_mode);
}

static int
nvme_tcp_generate_tls_credentials(struct nvme_tcp_ctrlr *tctrlr)
{
	struct spdk_nvme_ctrlr *ctrlr = &tctrlr->ctrlr;
	int rc;
	uint8_t psk_retained[SPDK_TLS_PSK_MAX_LEN] = {};
	uint8_t psk_configured[SPDK_TLS_PSK_MAX_LEN] = {};
	uint8_t pskbuf[SPDK_TLS_PSK_MAX_LEN + 1] = {};
	uint8_t tls_cipher_suite;
	uint8_t psk_retained_hash;
	uint64_t psk_configured_size;

	rc = spdk_key_get_key(ctrlr->opts.tls_psk, pskbuf, SPDK_TLS_PSK_MAX_LEN);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to obtain key '%s': %s\n",
			    spdk_key_get_name(ctrlr->opts.tls_psk), spdk_strerror(-rc));
		goto finish;
	}

	rc = nvme_tcp_parse_interchange_psk(pskbuf, psk_configured, sizeof(psk_configured),
					    &psk_configured_size, &psk_retained_hash);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to parse PSK interchange!\n");
		goto finish;
	}

	/* The Base64 string encodes the configured PSK (32 or 48 bytes binary).
	 * This check also ensures that psk_configured_size is smaller than
	 * psk_retained buffer size. */
	if (psk_configured_size == SHA256_DIGEST_LENGTH) {
		tls_cipher_suite = NVME_TCP_CIPHER_AES_128_GCM_SHA256;
		tctrlr->tls_cipher_suite = "TLS_AES_128_GCM_SHA256";
	} else if (psk_configured_size == SHA384_DIGEST_LENGTH) {
		tls_cipher_suite = NVME_TCP_CIPHER_AES_256_GCM_SHA384;
		tctrlr->tls_cipher_suite = "TLS_AES_256_GCM_SHA384";
	} else {
		SPDK_ERRLOG("Unrecognized cipher suite!\n");
		rc = -ENOTSUP;
		goto finish;
	}

	rc = nvme_tcp_generate_psk_identity(tctrlr->psk_identity, sizeof(tctrlr->psk_identity),
					    ctrlr->opts.hostnqn, ctrlr->trid.subnqn,
					    tls_cipher_suite);
	if (rc) {
		SPDK_ERRLOG("could not generate PSK identity\n");
		goto finish;
	}

	/* No hash indicates that Configured PSK must be used as Retained PSK. */
	if (psk_retained_hash == NVME_TCP_HASH_ALGORITHM_NONE) {
		assert(psk_configured_size < sizeof(psk_retained));
		memcpy(psk_retained, psk_configured, psk_configured_size);
		rc = psk_configured_size;
	} else {
		/* Derive retained PSK. */
		rc = nvme_tcp_derive_retained_psk(psk_configured, psk_configured_size, ctrlr->opts.hostnqn,
						  psk_retained, sizeof(psk_retained), psk_retained_hash);
		if (rc < 0) {
			SPDK_ERRLOG("Unable to derive retained PSK!\n");
			goto finish;
		}
	}

	rc = nvme_tcp_derive_tls_psk(psk_retained, rc, tctrlr->psk_identity, tctrlr->psk,
				     sizeof(tctrlr->psk), tls_cipher_suite);
	if (rc < 0) {
		SPDK_ERRLOG("Could not generate TLS PSK!\n");
		goto finish;
	}

	tctrlr->psk_size = rc;
	rc = 0;
finish:
	spdk_memset_s(psk_configured, sizeof(psk_configured), 0, sizeof(psk_configured));
	spdk_memset_s(pskbuf, sizeof(pskbuf), 0, sizeof(pskbuf));

	return rc;
}

/* We have to use the typedef in the function declaration to appease astyle. */
typedef struct spdk_nvme_ctrlr spdk_nvme_ctrlr_t;

static spdk_nvme_ctrlr_t *
nvme_tcp_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
			 const struct spdk_nvme_ctrlr_opts *opts,
			 void *devhandle)
{
	struct nvme_tcp_ctrlr *tctrlr;
	struct nvme_tcp_qpair *tqpair;
	int rc;

	tctrlr = calloc(1, sizeof(*tctrlr));
	if (tctrlr == NULL) {
		SPDK_ERRLOG("could not allocate ctrlr\n");
		return NULL;
	}

	tctrlr->ctrlr.opts = *opts;
	tctrlr->ctrlr.trid = *trid;

	if (opts->tls_psk != NULL) {
		rc = nvme_tcp_generate_tls_credentials(tctrlr);
		if (rc != 0) {
			free(tctrlr);
			return NULL;
		}
	}

	if (opts->transport_ack_timeout > NVME_TCP_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT) {
		SPDK_NOTICELOG("transport_ack_timeout exceeds max value %d, use max value\n",
			       NVME_TCP_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT);
		tctrlr->ctrlr.opts.transport_ack_timeout = NVME_TCP_CTRLR_MAX_TRANSPORT_ACK_TIMEOUT;
	}

	rc = nvme_ctrlr_construct(&tctrlr->ctrlr);
	if (rc != 0) {
		free(tctrlr);
		return NULL;
	}

	/* Sequence might be used not only for data digest offload purposes but
	 * to handle a potential COPY operation appended as the result of translation. */
	tctrlr->ctrlr.flags |= SPDK_NVME_CTRLR_ACCEL_SEQUENCE_SUPPORTED;
	tctrlr->ctrlr.adminq = nvme_tcp_ctrlr_create_qpair(&tctrlr->ctrlr, 0,
			       tctrlr->ctrlr.opts.admin_queue_size, 0,
			       tctrlr->ctrlr.opts.admin_queue_size, true);
	if (!tctrlr->ctrlr.adminq) {
		SPDK_ERRLOG("failed to create admin qpair\n");
		nvme_tcp_ctrlr_destruct(&tctrlr->ctrlr);
		return NULL;
	}

	tqpair = nvme_tcp_qpair(tctrlr->ctrlr.adminq);
	tctrlr->ctrlr.numa.id_valid = 1;
	tctrlr->ctrlr.numa.id = spdk_sock_get_numa_id(tqpair->sock);

	if (nvme_ctrlr_add_process(&tctrlr->ctrlr, 0) != 0) {
		SPDK_ERRLOG("nvme_ctrlr_add_process() failed\n");
		nvme_ctrlr_destruct(&tctrlr->ctrlr);
		return NULL;
	}

	return &tctrlr->ctrlr;
}

static uint32_t
nvme_tcp_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	/* TCP transport doesn't limit maximum IO transfer size. */
	return UINT32_MAX;
}

static uint16_t
nvme_tcp_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	return NVME_TCP_MAX_SGL_DESCRIPTORS;
}

static int
nvme_tcp_qpair_iterate_requests(struct spdk_nvme_qpair *qpair,
				int (*iter_fn)(struct nvme_request *req, void *arg),
				void *arg)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	struct nvme_tcp_req *tcp_req, *tmp;
	int rc;

	assert(iter_fn != NULL);

	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->outstanding_reqs, link, tmp) {
		assert(tcp_req->req != NULL);

		rc = iter_fn(tcp_req->req, arg);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

static int
nvme_tcp_qpair_authenticate(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	int rc;

	/* If the qpair is still connecting, it'll be forced to authenticate later on */
	if (tqpair->state < NVME_TCP_QPAIR_STATE_RUNNING) {
		return 0;
	} else if (tqpair->state != NVME_TCP_QPAIR_STATE_RUNNING) {
		return -ENOTCONN;
	}

	rc = nvme_fabric_qpair_authenticate_async(qpair);
	if (rc == 0) {
		nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTING);
		tqpair->state = NVME_TCP_QPAIR_STATE_AUTHENTICATING;
	}

	return rc;
}

static void
nvme_tcp_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_req *tcp_req, *tmp;
	struct spdk_nvme_cpl cpl = {};
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;

	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->outstanding_reqs, link, tmp) {
		assert(tcp_req->req != NULL);
		if (tcp_req->req->cmd.opc != SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
			continue;
		}

		nvme_tcp_req_complete(tcp_req, tqpair, &cpl, false);
	}
}

static struct spdk_nvme_transport_poll_group *
nvme_tcp_poll_group_create(void)
{
	struct nvme_tcp_poll_group *group = calloc(1, sizeof(*group));

	if (group == NULL) {
		SPDK_ERRLOG("Unable to allocate poll group.\n");
		return NULL;
	}

	TAILQ_INIT(&group->needs_poll);
	TAILQ_INIT(&group->timeout_enabled);

	group->sock_group = spdk_sock_group_create(group);
	if (group->sock_group == NULL) {
		free(group);
		SPDK_ERRLOG("Unable to allocate sock group.\n");
		return NULL;
	}

	return &group->group;
}

static struct spdk_nvme_transport_poll_group *
nvme_tcp_qpair_get_optimal_poll_group(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	struct spdk_sock_group *group = NULL;
	int rc;

	rc = spdk_sock_get_optimal_sock_group(tqpair->sock, &group, NULL);
	if (!rc && group != NULL) {
		return spdk_sock_group_get_ctx(group);
	}

	return NULL;
}

static int
nvme_tcp_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(qpair->poll_group);
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	if (spdk_sock_group_add_sock(group->sock_group, tqpair->sock, nvme_tcp_qpair_sock_cb, qpair)) {
		return -EPROTO;
	}
	return 0;
}

static int
nvme_tcp_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(qpair->poll_group);
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	if (TAILQ_ENTRY_ENQUEUED(tqpair, link_poll)) {
		TAILQ_REMOVE_CLEAR(&group->needs_poll, tqpair, link_poll);
	}

	if (tqpair->sock && group->sock_group) {
		if (spdk_sock_group_remove_sock(group->sock_group, tqpair->sock)) {
			return -EPROTO;
		}
	}
	return 0;
}

static int
nvme_tcp_poll_group_add(struct spdk_nvme_transport_poll_group *tgroup,
			struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(tgroup);

	/* disconnected qpairs won't have a sock to add. */
	if (nvme_qpair_get_state(qpair) >= NVME_QPAIR_CONNECTED) {
		if (spdk_sock_group_add_sock(group->sock_group, tqpair->sock, nvme_tcp_qpair_sock_cb, qpair)) {
			return -EPROTO;
		}
	}

	return 0;
}

static int
nvme_tcp_poll_group_remove(struct spdk_nvme_transport_poll_group *tgroup,
			   struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair;
	struct nvme_tcp_poll_group *group;

	assert(qpair->poll_group_tailq_head == &tgroup->disconnected_qpairs);

	tqpair = nvme_tcp_qpair(qpair);
	group = nvme_tcp_poll_group(tgroup);

	assert(tqpair->shared_stats == true);
	tqpair->stats = &g_dummy_stats;

	if (TAILQ_ENTRY_ENQUEUED(tqpair, link_poll)) {
		TAILQ_REMOVE_CLEAR(&group->needs_poll, tqpair, link_poll);
	}
	if (TAILQ_ENTRY_ENQUEUED(tqpair, link_timeout)) {
		TAILQ_REMOVE_CLEAR(&group->timeout_enabled, tqpair, link_timeout);
	}

	return 0;
}

static int64_t
nvme_tcp_poll_group_process_completions(struct spdk_nvme_transport_poll_group *tgroup,
					uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
	struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(tgroup);
	struct spdk_nvme_qpair *qpair, *tmp_qpair;
	struct nvme_tcp_qpair *tqpair, *tmp_tqpair;
	int num_events;

	group->completions_per_qpair = completions_per_qpair;
	group->num_completions = 0;
	group->stats.polls++;

	num_events = spdk_sock_group_poll(group->sock_group);

	STAILQ_FOREACH_SAFE(qpair, &tgroup->disconnected_qpairs, poll_group_stailq, tmp_qpair) {
		tqpair = nvme_tcp_qpair(qpair);
		if (nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTING) {
			if (TAILQ_EMPTY(&tqpair->outstanding_reqs)) {
				nvme_transport_ctrlr_disconnect_qpair_done(qpair);
			}
		}
		/* Wait until the qpair transitions to the DISCONNECTED state, otherwise user might
		 * want to free it from disconnect_qpair_cb, while it's not fully disconnected (and
		 * might still have outstanding requests) */
		if (nvme_qpair_get_state(qpair) == NVME_QPAIR_DISCONNECTED) {
			disconnected_qpair_cb(qpair, tgroup->group->ctx);
		}
	}

	/* If any qpairs were marked as needing to be polled due to an asynchronous write completion
	 * and they weren't polled as a consequence of calling spdk_sock_group_poll above, poll them now. */
	TAILQ_FOREACH_SAFE(tqpair, &group->needs_poll, link_poll, tmp_tqpair) {
		nvme_tcp_qpair_sock_cb(&tqpair->qpair, group->sock_group, tqpair->sock);
	}

	TAILQ_FOREACH_SAFE(tqpair, &group->timeout_enabled, link_timeout, tmp_tqpair) {
		qpair = &tqpair->qpair;
		assert(qpair->ctrlr->timeout_enabled);
		nvme_tcp_qpair_check_timeout(qpair);
	}

	if (spdk_unlikely(num_events < 0)) {
		return num_events;
	}

	group->stats.idle_polls += !num_events;
	group->stats.socket_completions += num_events;

	return group->num_completions;
}

/*
 * Handle disconnected qpairs when interrupt support gets added.
 */
static void
nvme_tcp_poll_group_check_disconnected_qpairs(struct spdk_nvme_transport_poll_group *tgroup,
		spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
}

static int
nvme_tcp_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup)
{
	int rc;
	struct nvme_tcp_poll_group *group = nvme_tcp_poll_group(tgroup);

	if (!STAILQ_EMPTY(&tgroup->connected_qpairs) || !STAILQ_EMPTY(&tgroup->disconnected_qpairs)) {
		return -EBUSY;
	}

	rc = spdk_sock_group_close(&group->sock_group);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to close the sock group for a tcp poll group.\n");
		assert(false);
	}

	free(tgroup);

	return 0;
}

static int
nvme_tcp_poll_group_get_stats(struct spdk_nvme_transport_poll_group *tgroup,
			      struct spdk_nvme_transport_poll_group_stat **_stats)
{
	struct nvme_tcp_poll_group *group;
	struct spdk_nvme_transport_poll_group_stat *stats;

	if (tgroup == NULL || _stats == NULL) {
		SPDK_ERRLOG("Invalid stats or group pointer\n");
		return -EINVAL;
	}

	group = nvme_tcp_poll_group(tgroup);

	stats = calloc(1, sizeof(*stats));
	if (!stats) {
		SPDK_ERRLOG("Can't allocate memory for TCP stats\n");
		return -ENOMEM;
	}
	stats->trtype = SPDK_NVME_TRANSPORT_TCP;
	memcpy(&stats->tcp, &group->stats, sizeof(group->stats));

	*_stats = stats;

	return 0;
}

static void
nvme_tcp_poll_group_free_stats(struct spdk_nvme_transport_poll_group *tgroup,
			       struct spdk_nvme_transport_poll_group_stat *stats)
{
	free(stats);
}

static int
nvme_tcp_ctrlr_get_memory_domains(const struct spdk_nvme_ctrlr *ctrlr,
				  struct spdk_memory_domain **domains, int array_size)
{
	if (domains && array_size > 0) {
		domains[0] = spdk_memory_domain_get_system_domain();
	}

	return 1;
}

const struct spdk_nvme_transport_ops tcp_ops = {
	.name = "TCP",
	.type = SPDK_NVME_TRANSPORT_TCP,
	.ctrlr_construct = nvme_tcp_ctrlr_construct,
	.ctrlr_scan = nvme_fabric_ctrlr_scan,
	.ctrlr_destruct = nvme_tcp_ctrlr_destruct,
	.ctrlr_enable = nvme_tcp_ctrlr_enable,

	.ctrlr_set_reg_4 = nvme_fabric_ctrlr_set_reg_4,
	.ctrlr_set_reg_8 = nvme_fabric_ctrlr_set_reg_8,
	.ctrlr_get_reg_4 = nvme_fabric_ctrlr_get_reg_4,
	.ctrlr_get_reg_8 = nvme_fabric_ctrlr_get_reg_8,
	.ctrlr_set_reg_4_async = nvme_fabric_ctrlr_set_reg_4_async,
	.ctrlr_set_reg_8_async = nvme_fabric_ctrlr_set_reg_8_async,
	.ctrlr_get_reg_4_async = nvme_fabric_ctrlr_get_reg_4_async,
	.ctrlr_get_reg_8_async = nvme_fabric_ctrlr_get_reg_8_async,

	.ctrlr_get_max_xfer_size = nvme_tcp_ctrlr_get_max_xfer_size,
	.ctrlr_get_max_sges = nvme_tcp_ctrlr_get_max_sges,

	.ctrlr_create_io_qpair = nvme_tcp_ctrlr_create_io_qpair,
	.ctrlr_delete_io_qpair = nvme_tcp_ctrlr_delete_io_qpair,
	.ctrlr_connect_qpair = nvme_tcp_ctrlr_connect_qpair,
	.ctrlr_disconnect_qpair = nvme_tcp_ctrlr_disconnect_qpair,

	.ctrlr_get_memory_domains = nvme_tcp_ctrlr_get_memory_domains,

	.qpair_abort_reqs = nvme_tcp_qpair_abort_reqs,
	.qpair_reset = nvme_tcp_qpair_reset,
	.qpair_submit_request = nvme_tcp_qpair_submit_request,
	.qpair_process_completions = nvme_tcp_qpair_process_completions,
	.qpair_iterate_requests = nvme_tcp_qpair_iterate_requests,
	.qpair_authenticate = nvme_tcp_qpair_authenticate,
	.admin_qpair_abort_aers = nvme_tcp_admin_qpair_abort_aers,

	.poll_group_create = nvme_tcp_poll_group_create,
	.qpair_get_optimal_poll_group = nvme_tcp_qpair_get_optimal_poll_group,
	.poll_group_connect_qpair = nvme_tcp_poll_group_connect_qpair,
	.poll_group_disconnect_qpair = nvme_tcp_poll_group_disconnect_qpair,
	.poll_group_add = nvme_tcp_poll_group_add,
	.poll_group_remove = nvme_tcp_poll_group_remove,
	.poll_group_process_completions = nvme_tcp_poll_group_process_completions,
	.poll_group_check_disconnected_qpairs = nvme_tcp_poll_group_check_disconnected_qpairs,
	.poll_group_destroy = nvme_tcp_poll_group_destroy,
	.poll_group_get_stats = nvme_tcp_poll_group_get_stats,
	.poll_group_free_stats = nvme_tcp_poll_group_free_stats,
};

SPDK_NVME_TRANSPORT_REGISTER(tcp, &tcp_ops);

static void
nvme_tcp_trace(void)
{
	struct spdk_trace_tpoint_opts opts[] = {
		{
			"NVME_TCP_SUBMIT", TRACE_NVME_TCP_SUBMIT,
			OWNER_TYPE_NVME_TCP_QP, OBJECT_NVME_TCP_REQ, 1,
			{	{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 },
				{ "cid", SPDK_TRACE_ARG_TYPE_INT, 4 },
				{ "opc", SPDK_TRACE_ARG_TYPE_INT, 4 },
				{ "dw10", SPDK_TRACE_ARG_TYPE_PTR, 4 },
				{ "dw11", SPDK_TRACE_ARG_TYPE_PTR, 4 },
				{ "dw12", SPDK_TRACE_ARG_TYPE_PTR, 4 },
				{ "qd", SPDK_TRACE_ARG_TYPE_INT, 4 }
			}
		},
		{
			"NVME_TCP_COMPLETE", TRACE_NVME_TCP_COMPLETE,
			OWNER_TYPE_NVME_TCP_QP, OBJECT_NVME_TCP_REQ, 0,
			{	{ "ctx", SPDK_TRACE_ARG_TYPE_PTR, 8 },
				{ "cid", SPDK_TRACE_ARG_TYPE_INT, 4 },
				{ "cpl", SPDK_TRACE_ARG_TYPE_PTR, 4 },
				{ "qd", SPDK_TRACE_ARG_TYPE_INT, 4 }
			}
		},
	};

	spdk_trace_register_object(OBJECT_NVME_TCP_REQ, 'p');
	spdk_trace_register_owner_type(OWNER_TYPE_NVME_TCP_QP, 'q');
	spdk_trace_register_description_ext(opts, SPDK_COUNTOF(opts));

	spdk_trace_tpoint_register_relation(TRACE_SOCK_REQ_QUEUE, OBJECT_NVME_TCP_REQ, 0);
	spdk_trace_tpoint_register_relation(TRACE_SOCK_REQ_PEND, OBJECT_NVME_TCP_REQ, 0);
	spdk_trace_tpoint_register_relation(TRACE_SOCK_REQ_COMPLETE, OBJECT_NVME_TCP_REQ, 0);
}
SPDK_TRACE_REGISTER_FN(nvme_tcp_trace, "nvme_tcp", TRACE_GROUP_NVME_TCP)
