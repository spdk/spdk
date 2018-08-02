/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * NVMe/TCP transport
 */

#include "nvme_internal.h"

#include "spdk/endian.h"
#include "spdk/likely.h"
#include "spdk/sock.h"
#include "spdk/string.h"
#include "spdk/nvmf_spec.h"

#define NVME_TCP_TIME_OUT_IN_MS 30000
#define NVME_TCP_RW_BUFFER_SIZE 131072

/*
 * Maximum number of SGL elements.
 * This is chosen to match the current nvme_pcie.c limit.
 */
#define NVME_TCP_MAX_SGL_DESCRIPTORS	(253)

struct nvme_tcp_request;


#define NVMF_TCP_CH_LEN sizeof(struct spdk_nvme_tcp_common_pdu_hdr)
#define NVMF_TCP_PSH_LEN_MAX 120
#define NVMF_TCP_DIGEST_LEN 4
#define NVMF_TCP_CPDA_MAX 31
#define NVMF_TCP_ALIGNMENT 4

#define NVMF_TCP_TERM_REQ_PDU_MAX_SIZE  152
#define NVMF_TCP_PDU_PDO_MAX_OFFSET	((NVMF_TCP_CPDA_MAX + 1) << 2)
#define NVMF_TCP_PDU_MAX_DATA_SIZE	4096
#define NVMF_TCP_QPAIR_EXIT_TIMEOUT	30

/* The following security functions  shoudl put in general */
#define SPDK_CRC32C_INITIAL    0xffffffffUL
#define SPDK_CRC32C_XOR        0xffffffffUL

#define MAKE_DIGEST_WORD(BUF, CRC32C) \
	(   ((*((uint8_t *)(BUF)+0)) = (uint8_t)((uint32_t)(CRC32C) >> 0)), \
	    ((*((uint8_t *)(BUF)+1)) = (uint8_t)((uint32_t)(CRC32C) >> 8)), \
	    ((*((uint8_t *)(BUF)+2)) = (uint8_t)((uint32_t)(CRC32C) >> 16)), \
	    ((*((uint8_t *)(BUF)+3)) = (uint8_t)((uint32_t)(CRC32C) >> 24)))

#define MATCH_DIGEST_WORD(BUF, CRC32C) \
	(    ((((uint32_t) *((uint8_t *)(BUF)+0)) << 0)		\
	    | (((uint32_t) *((uint8_t *)(BUF)+1)) << 8)		\
	    | (((uint32_t) *((uint8_t *)(BUF)+2)) << 16)	\
	    | (((uint32_t) *((uint8_t *)(BUF)+3)) << 24))	\
	    == (CRC32C))

#define DGET32(B)								\
	(((  (uint32_t) *((uint8_t *)(B)+0)) << 0)				\
	 | (((uint32_t) *((uint8_t *)(B)+1)) << 8)				\
	 | (((uint32_t) *((uint8_t *)(B)+2)) << 16)				\
	 | (((uint32_t) *((uint8_t *)(B)+3)) << 24))

#define DSET32(B,D)								\
	(((*((uint8_t *)(B)+0)) = (uint8_t)((uint32_t)(D) >> 0)),		\
	 ((*((uint8_t *)(B)+1)) = (uint8_t)((uint32_t)(D) >> 8)),		\
	 ((*((uint8_t *)(B)+1)) = (uint8_t)((uint32_t)(D) >> 16)),		\
	 ((*((uint8_t *)(B)+2)) = (uint8_t)((uint32_t)(D) >> 24)))


enum spdk_nvmf_tcp_error_codes {
	SPDK_NVMF_TCP_PDU_IN_PROGRESS	= 0,
	SPDK_NVMF_TCP_CONNECTION_FATAL	= -1,
	SPDK_NVMF_TCP_PDU_FATAL		= -2,
};

typedef void (*nvme_tcp_qpair_xfer_complete_cb)(void *cb_arg);

struct spdk_nvme_tcp_pdu {
	union {
		/* to hold error pdu data */
		uint8_t					raw[NVMF_TCP_TERM_REQ_PDU_MAX_SIZE];
		struct spdk_nvme_tcp_common_pdu_hdr	hdr;
		struct spdk_nvme_tcp_ic_req		ic_req;
		struct spdk_nvme_tcp_term_req_hdr	term_req;
		struct spdk_nvme_tcp_cmd		capsule_cmd;
		struct spdk_nvme_tcp_h2c_data_hdr	h2c_data;
		struct spdk_nvme_tcp_ic_resp		ic_resp;
		struct spdk_nvme_tcp_rsp		capsule_resp;
		struct spdk_nvme_tcp_c2h_data		c2h_data;
		struct spdk_nvme_tcp_r2t_hdr		r2t;

	} u;

	uint8_t						header_digest[NVMF_TCP_DIGEST_LEN];
	uint8_t						data_digest[NVMF_TCP_DIGEST_LEN];
	uint32_t					padding_valid_bytes;
	uint32_t					hdigest_valid_bytes;
	uint32_t					ddigest_valid_bytes;

	uint32_t					ch_valid_bytes;
	uint32_t					psh_valid_bytes;
	uint32_t					data_valid_bytes;

	nvme_tcp_qpair_xfer_complete_cb			cb_fn;
	void						*cb_arg;
	int						ref;
	uint8_t						*data;
	uint32_t					data_len;
	bool						data_from_req;
	struct spdk_nvmf_tcp_qpair			*tqpair;

	struct spdk_nvmf_tcp_request			*tcp_req; /* data tied to a tcp request */
	uint32_t					writev_offset;
	TAILQ_ENTRY(spdk_nvmf_tcp_pdu)			tailq;
	uint32_t					remaining;
	uint32_t					padding_len;
};

/* NVMe TCP transport extensions for spdk_nvme_ctrlr */
struct nvme_tcp_ctrlr {
	struct spdk_nvme_ctrlr			ctrlr;
};

enum nvme_tcp_pdu_recv_state {
	/* Ready to wait to wait PDU */
	TCP_PDU_RECV_STATE_AWAIT_PDU_READY,

	/* Active tqpair waiting for any PDU ch header */
	TCP_PDU_RECV_STATE_AWAIT_PDU_CH_HDR,

	/* Active tqpair waiting for any PDU header */
	TCP_PDU_RECV_STATE_AWAIT_PDU_HDR,

	/* Active tqpair waiting for payload */
	TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD,

	/* Active tqpair does not wait for payload */
	TCP_PDU_RECV_STATE_ERROR,
};

/* NVMe TCP qpair extensions for spdk_nvme_qpair */
struct nvme_tcp_qpair {
	struct spdk_nvme_qpair			qpair;
	struct spdk_sock			*sock;
	TAILQ_HEAD(, spdk_nvme_tcp_request)	free_reqs;
	TAILQ_HEAD(, spdk_nvme_tcp_request)	send_queue;
	uint32_t				in_capsule_data_size;
	struct spdk_nvme_tcp_pdu		recv_pdu;
	enum nvme_tcp_pdu_recv_state		recv_state;

	struct spdk_nvme_tcp_request		*tcp_reqs;

	uint16_t				num_entries;
};

struct spdk_nvme_tcp_request {
	struct nvme_request			*req;
	struct spdk_nvme_cmd			cmd;
	struct spdk_nvme_cpl			cpl;
	uint16_t				cid;
	bool					in_capsule_data;
	uint32_t				datao;
	TAILQ_ENTRY(spdk_nvme_tcp_request)	link;
};

static inline struct nvme_tcp_qpair *
nvme_tcp_qpair(struct spdk_nvme_qpair *qpair)
{
	assert(qpair->trtype == SPDK_NVME_TRANSPORT_TCP);
	return SPDK_CONTAINEROF(qpair, struct nvme_tcp_qpair, qpair);
}

static inline struct nvme_tcp_ctrlr *
nvme_tcp_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_TCP);
	return SPDK_CONTAINEROF(ctrlr, struct nvme_tcp_ctrlr, ctrlr);
}

static struct spdk_nvme_tcp_req *
nvme_tcp_req_get(struct nvme_tcp_qpair *tqpair)
{
	struct spdk_nvme_tcp_req *tcp_req;

	tcp_req = STAILQ_FIRST(&tqpair->free_reqs);
	if (tcp_req) {
		TAILQ_REMOVE_HEAD(&tqpair->free_reqs, link);
	}

	return tcp_req;
}

static void
nvme_tcp_req_put(struct nvme_tcp_qpair *tqpair, struct spdk_nvme_tcp_req *tcp_req)
{
	TAILQ_INSERT_HEAD(&tqpair->free_reqs, tcp_req, link);
}

static int
nvme_tcp_parse_addr(struct sockaddr_storage *sa, int family, const char *addr, const char *service)
{
	struct addrinfo *res;
	struct addrinfo hints;
	int ret;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;

	ret = getaddrinfo(addr, service, &hints, &res);
	if (ret) {
		SPDK_ERRLOG("getaddrinfo failed: %s (%d)\n", gai_strerror(ret), ret);
		return ret;
	}

	if (res->ai_addrlen > sizeof(*sa)) {
		SPDK_ERRLOG("getaddrinfo() ai_addrlen %zu too large\n", (size_t)res->ai_addrlen);
		ret = EINVAL;
	} else {
		memcpy(sa, res->ai_addr, res->ai_addrlen);
	}

	freeaddrinfo(res);
	return ret;
}

static int
nvme_tcp_qpair_socket_init(struct nvme_tcp_qpair *tqpair)
{
	int buf_size, rc;

	/* set recv buffer size */
	buf_size = 2 * 1024 * 1024;
	rc = spdk_sock_set_recvbuf(tqpair->sock, buf_size);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_sock_set_recvbuf failed\n");
		return -1;
	}

	/* set send buffer size */
	rc = spdk_sock_set_sendbuf(tqpair->sock, buf_size);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_sock_set_sendbuf failed\n");
		return -1;
	}

	/* set low water mark */
	rc = spdk_sock_set_recvlowat(tqpair->sock, 1);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_sock_set_recvlowat() failed\n");
		return -1;
	}

	return 0;
}

static void
nvme_tcp_free_reqs(struct nvme_tcp_qpair *tqpair)
{
	if (!tqpair->tcp_reqs) {
		return;
	}

	free(tqpair->tcp_reqs);
	tqpair->tcp_reqs = NULL;
}

static int
nvme_tcp_alloc_reqs(struct nvme_tcp_qpair *tqpair)
{
	int i;

	tqpair->tcp_reqs = calloc(tqpair->num_entries, sizeof(struct spdk_nvme_tcp_request));
	if (tqpair->tcp_reqs == NULL) {
		SPDK_ERRLOG("Failed to allocate tcp_reqs\n");
		goto fail;
	}

	TAILQ_INIT(&tqpair->free_reqs);
	TAILQ_INIT(&tqpair->outstanding_reqs);
	for (i = 0; i < tqpair->num_entries; i++) {
		struct spdk_nvme_tcp_request	*tcp_req;
		tcp_req = &tqpair->tcp_reqs[i];
		tcp_req->cid = i;
		TAILQ_INSERT_TAIL(&tqpair->free_reqs, tcp_req, link);
	}

	return 0;

fail:
	nvme_tcp_free_reqs(tqpair);
	return -ENOMEM;
}

static int
nvme_tcp_qpair_connect(struct nvme_tcp_qpair *tqpair)
{
	struct sockaddr_storage dst_addr;
	struct sockaddr_storage src_addr;
	int rc;
	struct spdk_nvme_ctrlr *ctrlr;
	int family;

	ctrlr = tqpair->qpair.ctrlr;

	switch (ctrlr->trid.adrfam) {
	case SPDK_NVMF_ADRFAM_IPV4:
		family = AF_INET;
		break;
	case SPDK_NVMF_ADRFAM_IPV6:
		family = AF_INET6;
		break;
	default:
		SPDK_ERRLOG("Unhandled ADRFAM %d\n", ctrlr->trid.adrfam);
		return -1;
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "adrfam %d ai_family %d\n", ctrlr->trid.adrfam, family);

	memset(&dst_addr, 0, sizeof(dst_addr));

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "trsvcid is %s\n", ctrlr->trid.trsvcid);
	rc = nvme_tcp_parse_addr(&dst_addr, family, ctrlr->trid.traddr, ctrlr->trid.trsvcid);
	if (rc != 0) {
		SPDK_ERRLOG("dst_addr nvme_tcp_parse_addr() failed\n");
		return -1;
	}

	if (ctrlr->opts.src_addr[0] || ctrlr->opts.src_svcid[0]) {
		memset(&src_addr, 0, sizeof(src_addr));
		rc = nvme_tcp_parse_addr(&src_addr, family, ctrlr->opts.src_addr, ctrlr->opts.src_svcid);
		if (rc != 0) {
			SPDK_ERRLOG("src_addr nvme_tcp_parse_addr() failed\n");
			return -1;
		}
	}

	tqpair->sock = spdk_sock_connect(ctrlr->trid.traddr, atoi(ctrlr->trid.trsvcid));
	if (!tqpair->sock) {
		SPDK_ERRLOG("sock connection error of tqpair=%p with addr=%s, port=%d\n",
			    tqpair, ctrlr->trid.traddr, atoi(ctrlr->trid.trsvcid));
		return -1;
	}

	rc = nvme_tcp_qpair_socket_init(tqpair);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to init the socket of tqpair=%p\n", tqpair);
		return -1;
	}

	rc = nvme_tcp_alloc_reqs(tqpair);
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "rc =%d\n", rc);
	if (rc) {
		SPDK_ERRLOG("Unable to allocate tqpair tcp requests\n");
		return -1;
	}
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "TCP requests allocated\n");

	rc = nvme_fabric_qpair_connect(&tqpair->qpair, tqpair->num_entries);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to send an NVMe-oF Fabric CONNECT command\n");
		return -1;
	}

	return 0;
}

static struct spdk_nvme_qpair *
nvme_tcp_ctrlr_create_qpair(struct spdk_nvme_ctrlr *ctrlr,
			    uint16_t qid, uint32_t qsize,
			    enum spdk_nvme_qprio qprio,
			    uint32_t num_requests)
{
	struct nvme_tcp_qpair *tqpair;
	struct spdk_nvme_qpair *qpair;
	int rc;

	tqpair = calloc(1, sizeof(struct nvme_tcp_qpair));
	if (!tqpair) {
		SPDK_ERRLOG("failed to get create tqpair\n");
		return NULL;
	}

	tqpair->num_entries = qsize;

	qpair = &tqpair->qpair;

	rc = nvme_qpair_init(qpair, qid, ctrlr, qprio, num_requests);
	if (rc != 0) {
		return NULL;
	}

	rc = nvme_tcp_qpair_connect(tqpair);
	if (rc < 0) {
		nvme_tcp_qpair_destroy(qpair);
		return NULL;
	}

	return qpair;
}

static int
nvme_tcp_qpair_destroy(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);


	spdk_sock_close(&tqpair->sock);
	free(tqpair);

	return 0;
}

struct spdk_nvme_qpair *
nvme_tcp_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
			       const struct spdk_nvme_io_qpair_opts *opts)
{
	return nvme_tcp_ctrlr_create_qpair(ctrlr, qid, opts->io_queue_size, opts->qprio,
					   opts->io_queue_requests);
}

int
nvme_tcp_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

/* This function must only be called while holding g_spdk_nvme_driver->lock */
int
nvme_tcp_ctrlr_scan(const struct spdk_nvme_transport_id *trid,
		    void *cb_ctx,
		    spdk_nvme_probe_cb probe_cb,
		    spdk_nvme_remove_cb remove_cb,
		    bool direct_connect)
{
	struct spdk_nvme_ctrlr_opts discovery_opts;
	struct spdk_nvme_ctrlr *discovery_ctrlr;
	union spdk_nvme_cc_register cc;
	int rc;
	struct nvme_completion_poll_status status;

	if (strcmp(trid->subnqn, SPDK_NVMF_DISCOVERY_NQN) != 0) {
		/* Not a discovery controller - connect directly. */
		rc = nvme_ctrlr_probe(trid, NULL, probe_cb, cb_ctx);
		return rc;
	}

	spdk_nvme_ctrlr_get_default_ctrlr_opts(&discovery_opts, sizeof(discovery_opts));
	/* For discovery_ctrlr set the timeout to 0 */
	discovery_opts.keep_alive_timeout_ms = 0;

	discovery_ctrlr = nvme_tcp_ctrlr_construct(trid, &discovery_opts, NULL);
	if (discovery_ctrlr == NULL) {
		return -1;
	}

	/* TODO: this should be using the normal NVMe controller initialization process */
	cc.raw = 0;
	cc.bits.en = 1;
	cc.bits.iosqes = 6; /* SQ entry size == 64 == 2^6 */
	cc.bits.iocqes = 4; /* CQ entry size == 16 == 2^4 */
	rc = nvme_transport_ctrlr_set_reg_4(discovery_ctrlr, offsetof(struct spdk_nvme_registers, cc.raw),
					    cc.raw);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to set cc\n");
		nvme_ctrlr_destruct(discovery_ctrlr);
		return -1;
	}

	/* get the cdata info */
	status.done = false;
	rc = nvme_ctrlr_cmd_identify(discovery_ctrlr, SPDK_NVME_IDENTIFY_CTRLR, 0, 0,
				     &discovery_ctrlr->cdata, sizeof(discovery_ctrlr->cdata),
				     nvme_completion_poll_cb, &status);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to identify cdata\n");
		return rc;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(discovery_ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("nvme_identify_controller failed!\n");
		return -ENXIO;
	}

	/* Direct attach through spdk_nvme_connect() API */
	if (direct_connect == true) {
		/* Set the ready state to skip the normal init process */
		discovery_ctrlr->state = NVME_CTRLR_STATE_READY;
		nvme_ctrlr_connected(discovery_ctrlr);
		nvme_ctrlr_add_process(discovery_ctrlr, 0);
		return 0;
	}

	rc = nvme_fabric_ctrlr_discover(discovery_ctrlr, cb_ctx, probe_cb);
	nvme_ctrlr_destruct(discovery_ctrlr);
	return rc;
}

struct spdk_nvme_ctrlr *nvme_tcp_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		void *devhandle)
{
	struct nvme_tcp_ctrlr *tctrlr;
	union spdk_nvme_cap_register cap;
	union spdk_nvme_vs_register vs;
	int rc;

	tctrlr = calloc(1, sizeof(*tctrlr));
	if (tctrlr == NULL) {
		SPDK_ERRLOG("could not allocate ctrlr\n");
		return NULL;
	}

	tctrlr->ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_TCP;
	tctrlr->ctrlr.opts = *opts;
	tctrlr->ctrlr.trid = *trid;

	rc = nvme_ctrlr_construct(&tctrlr->ctrlr);
	if (rc != 0) {
		nvme_ctrlr_destruct(&tctrlr->ctrlr);
		return NULL;
	}

	tctrlr->ctrlr.adminq = nvme_tcp_ctrlr_create_qpair(&tctrlr->ctrlr, 0,
			       SPDK_NVMF_MIN_ADMIN_QUEUE_ENTRIES, 0, SPDK_NVMF_MIN_ADMIN_QUEUE_ENTRIES);
	if (!tctrlr->ctrlr.adminq) {
		SPDK_ERRLOG("failed to create admin qpair\n");
		return NULL;
	}

	if (nvme_ctrlr_get_cap(&tctrlr->ctrlr, &cap)) {
		SPDK_ERRLOG("get_cap() failed\n");
		nvme_ctrlr_destruct(&tctrlr->ctrlr);
		return NULL;
	}

	if (nvme_ctrlr_get_vs(&tctrlr->ctrlr, &vs)) {
		SPDK_ERRLOG("get_vs() failed\n");
		nvme_ctrlr_destruct(&tctrlr->ctrlr);
		return NULL;
	}

	nvme_ctrlr_init_cap(&tctrlr->ctrlr, &cap, &vs);

	return &tctrlr->ctrlr;
}

int
nvme_tcp_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_tcp_ctrlr *tctrlr = nvme_tcp_ctrlr(ctrlr);

	if (ctrlr->adminq) {
		nvme_tcp_qpair_destroy(ctrlr->adminq);
	}

	nvme_ctrlr_destruct_finish(ctrlr);

	free(tctrlr);

	return 0;
}

int
nvme_tcp_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	return nvme_fabric_ctrlr_set_reg_4(ctrlr, offset, value);
}

int
nvme_tcp_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	return nvme_fabric_ctrlr_set_reg_8(ctrlr, offset, value);
}

int
nvme_tcp_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value)
{
	return nvme_fabric_ctrlr_get_reg_4(ctrlr, offset, value);
}

int
nvme_tcp_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value)
{
	return nvme_fabric_ctrlr_get_reg_8(ctrlr, offset, value);
}

static int
nvme_tcp_request_append_iov(struct nvme_tcp_request *treq, struct iovec *iov, int iov_avail)
{
	// TODO: send PDU header
	// TODO: send in-capsule data if required
	// TODO: send SGL entries if xfer == host to controller

	// R2T is going to throw a wrench in this idea.  We won't necessarily be able to send the
	// requests in the order we want to - we have to be able to send data in response to any
	// R2T.  This probably needs an array of requests indexed by CID (or is it TTAG?) and
	// maybe a bitmask or queue of which requests have an outstanding R2T. (We also need to remember
	// the R2T Data Length, so maybe pending R2Ts need their own data structure on the host side too.)

	// TODO
	return -1;
}

static ssize_t
nvme_tcp_request_bytes_sent(struct nvme_tcp_request *treq, ssize_t bytes_sent)
{
	// TODO - adjust treq for the data that was sent, and drop it from send_queue if all done
	return 0;
}

static int
nvme_tcp_qpair_process_send_queue(struct nvme_tcp_qpair *tqpair)
{
	struct nvme_tcp_request *treq, *next_treq;
	struct iovec iovs[10]; // TODO
	ssize_t bytes_sent;
	int iovcnt, iovcnt_max;
	int rc;

	treq = STAILQ_FIRST(&tqpair->send_queue);
	if (treq == NULL) {
		return 0;
	}

	iovcnt = 0;
	iovcnt_max = SPDK_COUNTOF(iovs);
	do {
		rc = nvme_tcp_request_append_iov(treq, &iovs[iovcnt], iovcnt_max - iovcnt);
		if (rc < 0) {
			// TODO: handle error
			return -EIO;
		}

		assert(rc >= 0);
		iovcnt += rc;
		treq = STAILQ_NEXT(treq, link);
	} while (iovcnt < iovcnt_max);

	if (iovcnt == 0) {
		/* Nothing to send right now. */
		return 0;
	}

	bytes_sent = spdk_sock_writev(tqpair->sock, iovs, iovcnt);
	if (bytes_sent < 0) {
		// TODO: error handling, EAGAIN, etc.
		return -1;
	}

	treq = STAILQ_FIRST(&tqpair->send_queue);
	while (bytes_sent > 0 && treq) {
		next_treq = STAILQ_NEXT(treq, link);
		bytes_sent -= nvme_tcp_request_bytes_sent(treq, bytes_sent);
		treq = next_treq;
	}
	assert(bytes_sent == 0);

	return 0;
}

int
nvme_tcp_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	enum spdk_nvme_data_transfer xfer;
	struct nvme_tcp_request *treq;
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	treq = STAILQ_FIRST(&tqpair->free_reqs);
	if (spdk_unlikely(treq == NULL)) {
		/* No free nvme_tcp_requests available.  Queue the request to be processed later. */
		STAILQ_INSERT_TAIL(&qpair->queued_req, req, stailq);
		return 0;
	}

	STAILQ_REMOVE_HEAD(&tqpair->free_reqs, link);

	treq->req = req;
	req->cmd.cid = treq->cid;
	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.length = req->payload_size;
	req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK;

	if (req->cmd.opc == SPDK_NVME_OPC_FABRIC) {
		struct spdk_nvmf_capsule_cmd *nvmf_cmd = (struct spdk_nvmf_capsule_cmd *)&req->cmd;

		xfer = spdk_nvme_opc_get_data_transfer(nvmf_cmd->fctype);
	} else {
		xfer = spdk_nvme_opc_get_data_transfer(req->cmd.opc);
	}

	if (xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER &&
	    tqpair->in_capsule_data_size != 0 &&
	    req->payload_size <= tqpair->in_capsule_data_size) {
		req->cmd.dptr.sgl1.unkeyed.subtype = 0x1; /* in-capsule data */
		treq->in_capsule_data = true;
	} else {
		req->cmd.dptr.sgl1.unkeyed.subtype = 0xA; /* command data block */
		treq->in_capsule_data = false;
	}

	if (nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL) {
		/* Reset the SGL iterator.  The entries of the SGL will be retrieved later. */
		req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);
	}

	STAILQ_INSERT_TAIL(&tqpair->send_queue, treq, link);
	return nvme_tcp_qpair_process_send_queue(tqpair);
}

int
nvme_tcp_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	return nvme_tcp_qpair_destroy(qpair);
}

int
nvme_tcp_ctrlr_reinit_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	// TODO
	return -1;
	//return nvme_tcp_qpair_connect(nvme_tcp_qpair(qpair));
}

int
nvme_tcp_qpair_enable(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

int
nvme_tcp_qpair_disable(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

int
nvme_tcp_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

int
nvme_tcp_qpair_fail(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

static void
nvme_tcp_qpair_set_recv_state(struct nvme_tcp_qpair *tqpair,
				   enum nvme_tcp_pdu_recv_state state)
{
	if (tqpair->recv_state == state) {
		SPDK_ERRLOG("The recv state of tqpair=%p is same with the state(%d) to be set\n",
			    tqpair, state);
		return;
	}

	tqpair->recv_state = state;
	switch (state) {
	case TCP_PDU_RECV_STATE_AWAIT_PDU_READY:
	case TCP_PDU_RECV_STATE_ERROR:
		memset(&tqpair->recv_pdu, 0, sizeof(struct spdk_nvme_tcp_pdu));
		break;
	case TCP_PDU_RECV_STATE_AWAIT_PDU_CH_HDR:
	case TCP_PDU_RECV_STATE_AWAIT_PDU_HDR:
	case TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD:
	default:
		break;
	}
}

static int
nvme_tcp_qpair_read_data(struct spdk_nvmf_tcp_qpair *tqpair, int bytes,
			      void *buf)
{
	int ret;

	if (bytes == 0) {
		return 0;
	}

	ret = spdk_sock_recv(tqpair->sock, buf, bytes);

	if (ret > 0) {
		return ret;
	}

	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return 0;
		}

		/* For connect reset issue, do not output error log */
		if (errno == ECONNRESET) {
			SPDK_DEBUGLOG(SPDK_LOG_NVMF_TCP, "spdk_sock_recv() failed, errno %d: %s\n",
				      errno, spdk_strerror(errno));
		} else {
			SPDK_ERRLOG("spdk_sock_recv() failed, errno %d: %s\n",
				    errno, spdk_strerror(errno));
		}
	}

	return SPDK_NVMF_TCP_CONNECTION_FATAL;
}

static void
nvme_tcp_pdu_ch_handle(struct spdk_nvmf_tcp_qpair *tqpair)
{
	struct spdk_nvmf_tcp_pdu *pdu;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	uint32_t expected_hlen;
	bool plen_error = false;

	pdu = &tqpair->recv_pdu;

	if (pdu->u.hdr.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_REQ) {
		if (tqpair->state != NVMF_TCP_QPAIR_STATE_INVALID) {
			SPDK_ERRLOG("Already received ICreq PDU, and reject this pdu=%p\n", pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_PDU_SEQUENCE;
			goto err;
		}
		expected_hlen = sizeof(struct spdk_nvme_tcp_ic_req);
		if (pdu->u.hdr.plen != expected_hlen) {
			plen_error = true;
		}
	} else {
		if (tqpair->state != NVMF_TCP_QPAIR_STATE_RUNNING) {
			SPDK_ERRLOG("The TCP/IP connection is not negotitated\n");
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_PDU_SEQUENCE;
			goto err;
		}

		switch (pdu->u.hdr.pdu_type) {
		case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD:
			expected_hlen = sizeof(struct spdk_nvme_tcp_cmd);
			if (pdu->u.hdr.plen < expected_hlen) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_H2C_DATA:
			expected_hlen = sizeof(struct spdk_nvme_tcp_h2c_data_hdr);
			if (pdu->u.hdr.plen < expected_hlen) {
				plen_error = true;
			}
			break;

		case SPDK_NVME_TCP_PDU_TYPE_TERM_REQ:
			expected_hlen = sizeof(struct spdk_nvme_tcp_term_req_hdr);
			if ((pdu->u.hdr.plen < expected_hlen) ||
			    (pdu->u.hdr.plen > NVMF_TCP_TERM_REQ_PDU_MAX_SIZE)) {
				plen_error = true;
			}
			break;

		default:
			SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", tqpair->pdu_in_progress->u.hdr.pdu_type);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
			error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, pdu_type);
			goto err;
		}
	}

	if (pdu->u.hdr.hlen != expected_hlen) {
		SPDK_ERRLOG("Expected ICReq header length %u, got %u\n",
			    expected_hlen, pdu->u.hdr.hlen);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, hlen);
		goto err;

	} else if (plen_error) {
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, plen);
		goto err;
	} else {
		spdk_nvmf_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_AWAIT_PDU_HDR);
		return;
	}
err:
	spdk_nvmf_tcp_send_c2h_term_req(tqpair, pdu, fes, error_offset);
}

static int
nvme_tcp_read_pdu(struct nvme_tcp_qpair *tqpair)
{
	int rc = 0;
	struct spdk_nvmf_tcp_pdu *pdu;
	uint32_t psh_len, data_len, hlen, padding_len, pdo;

	/* If in a new state */
	if (tqpair->recv_state == TCP_PDU_RECV_STATE_AWAIT_PDU_READY) {
		nvme_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_AWAIT_PDU_HDR);
	}

	/* Wait for the common header  */
	if (tqpair->recv_state == TCP_PDU_RECV_STATE_AWAIT_PDU_CH_HDR) {
		pdu = &tqpair->recv_pdu;
		/* common header */
		if (pdu->ch_valid_bytes < NVMF_TCP_CH_LEN) {
			rc = spdk_nvmf_tcp_qpair_read_data(tqpair,
							   NVMF_TCP_CH_LEN - pdu->ch_valid_bytes,
							   (uint8_t *)&pdu->u.hdr + pdu->ch_valid_bytes);
			if (rc < 0) {
				goto end;
			}
			pdu->ch_valid_bytes += rc;
			if (pdu->ch_valid_bytes < NVMF_TCP_CH_LEN) {
				return SPDK_NVMF_TCP_PDU_IN_PROGRESS;
			}
		}

		/* The command header of this PDU has now been read from the socket. */
		spdk_nvmf_tcp_pdu_ch_handle(tqpair);
		rc = 0;
	}

	/* Wait for the pdu specific header  */
	if (tqpair->recv_state == TCP_PDU_RECV_STATE_AWAIT_PDU_HDR) {
		hlen = pdu->u.hdr.hlen;
		/* We already check the psh_len, so no worry about the negative value, i.e., overflow  */
		psh_len = hlen - NVMF_TCP_CH_LEN;
		if (pdu->psh_valid_bytes < psh_len) {
			rc = spdk_nvmf_tcp_qpair_read_data(tqpair,
							   psh_len - pdu->psh_valid_bytes,
							   (uint8_t *)&pdu->u.raw + NVMF_TCP_CH_LEN + pdu->psh_valid_bytes);
			if (rc < 0) {
				goto end;
			}

			pdu->psh_valid_bytes += rc;
			if (pdu->psh_valid_bytes < psh_len) {
				return SPDK_NVMF_TCP_PDU_IN_PROGRESS;
			}
		}

		/* Only capsule_cmd and h2c_data has digest */
		if (((pdu->u.hdr.pdu_type == SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD) ||
		     (pdu->u.hdr.pdu_type == SPDK_NVME_TCP_PDU_TYPE_H2C_DATA)) &&
		    tqpair->host_ddgst_enable && pdu->hdigest_valid_bytes < NVMF_TCP_DIGEST_LEN) {

			rc = spdk_nvmf_tcp_qpair_read_data(tqpair,
							   NVMF_TCP_DIGEST_LEN - pdu->hdigest_valid_bytes,
							   pdu->header_digest + pdu->hdigest_valid_bytes);
			if (rc < 0) {
				goto end;
			}

			pdu->hdigest_valid_bytes += rc;
			if (pdu->hdigest_valid_bytes < NVMF_TCP_DIGEST_LEN) {
				return SPDK_NVMF_TCP_PDU_IN_PROGRESS;
			}
		}

		/* All header(ch, psh, head digist) of this PDU has now been read from the socket. */
		spdk_nvmf_tcp_pdu_psh_handle(tqpair);
		rc = 0;
	}

	if (tqpair->recv_state == TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD) {
		pdu = tqpair->pdu_in_progress;
		assert(pdu != NULL);

		/* check whether the data is valid, if not we just return */
		if (!pdu->data) {
			return SPDK_NVMF_TCP_PDU_IN_PROGRESS;
		}

		data_len = pdu->data_len;
		if (pdu->u.hdr.pdu_type == SPDK_NVME_TCP_PDU_TYPE_TERM_REQ) {
			if (pdu->data_valid_bytes < data_len) {
				rc = spdk_nvmf_tcp_qpair_read_data(tqpair,
								   data_len - pdu->data_valid_bytes,
								   (uint8_t *)pdu->data + pdu->data_valid_bytes);
				if (rc < 0) {
					goto end;
				}

				pdu->data_valid_bytes += rc;
				if (pdu->data_valid_bytes < data_len) {
					return SPDK_NVMF_TCP_PDU_IN_PROGRESS;
				}
			}

		} else  {
			pdo = pdu->u.hdr.pdo;
			padding_len = pdo - hlen;
			if (tqpair->host_ddgst_enable) {
				padding_len -= NVMF_TCP_DIGEST_LEN;
			}

			if (padding_len > 0 && pdu->padding_valid_bytes < padding_len) {
				/* reuse the space in raw */
				rc = spdk_nvmf_tcp_qpair_read_data(tqpair,
								   padding_len - pdu->padding_valid_bytes,
								   (uint8_t *)pdu->u.raw + hlen + pdu->padding_valid_bytes);
				if (rc < 0) {
					goto end;
				}

				pdu->padding_valid_bytes += rc;
				if (pdu->padding_valid_bytes < padding_len) {
					return SPDK_NVMF_TCP_PDU_IN_PROGRESS;
				}
			}

			/* data len */
			if (pdu->data_valid_bytes < data_len) {
				rc = spdk_nvmf_tcp_qpair_read_data(tqpair,
								   data_len - pdu->data_valid_bytes,
								   (uint8_t *)pdu->tcp_req->buf + pdu->data_valid_bytes);
				if (rc < 0) {
					goto end;
				}

				pdu->data_valid_bytes += rc;
				if (pdu->data_valid_bytes < data_len) {
					return SPDK_NVMF_TCP_PDU_IN_PROGRESS;
				}
			}

			/* data digest */
			if (tqpair->host_ddgst_enable && pdu->ddigest_valid_bytes < NVMF_TCP_DIGEST_LEN) {
				rc = spdk_nvmf_tcp_qpair_read_data(tqpair,
								   NVMF_TCP_DIGEST_LEN - pdu->ddigest_valid_bytes,
								   pdu->data_digest + pdu->ddigest_valid_bytes);
				if (rc < 0) {
					goto end;
				}

				pdu->ddigest_valid_bytes += rc;
				if (pdu->ddigest_valid_bytes < NVMF_TCP_DIGEST_LEN) {
					return SPDK_NVMF_TCP_PDU_IN_PROGRESS;
				}
			}
		}

		/* All of this PDU has now been read from the socket. */
		spdk_nvmf_tcp_pdu_payload_handle(tqpair);
		rc = 1;
	}

	/* If in a error state */
	if (tqpair->recv_state == TCP_PDU_RECV_STATE_ERROR) {
		return SPDK_NVMF_TCP_PDU_FATAL;
	} else {
		return rc;
	}

end:
	nvme_tcp_qpair_set_recv_state(tqpair, TCP_PDU_RECV_STATE_ERROR);
	return rc;

}

int
nvme_tcp_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_tcp_qpair	*tqpair = nvme_tcp_qpair(qpair);
	int rc;
	uint32_t		reaped;

	
	rc = nvme_tcp_qpair_process_send_queue(tqpair);
	if (rc) {
		return 0;
	}

	if (max_completions == 0) {
		max_completions = tqpair->num_entries;
	} else {
		max_completions = spdk_min(max_completions, tqpair->num_entries);
	}

	reaped = 0;
	do {
		rc = nvme_tcp_read_pdu(tqpair);
		if (rc < 0) {
			SPDK_ERRLOG("Error polling CQ! (%d): %s\n",
				    errno, spdk_strerror(errno));
			return -1;
		} else if (rc == 0) {
			/* Ran out of completions */
			break;
		}

		reaped++;
	} while (reaped < max_completions);

	return reaped;

}

uint32_t
nvme_tcp_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	// /* Todo, which should get from the NVMF target */
	return NVME_TCP_RW_BUFFER_SIZE;
}

uint16_t
nvme_tcp_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	/*
	 * We do not support >1 SGE in the initiator currently,
	 *  so we can only return 1 here.  Once that support is
	 *  added, this should return ctrlr->cdata.nvmf_specific.msdbd
	 *  instead.
	 */
	return 1;
}

void *
nvme_tcp_ctrlr_alloc_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, size_t size)
{
	return NULL;
}

int
nvme_tcp_ctrlr_free_cmb_io_buffer(struct spdk_nvme_ctrlr *ctrlr, void *buf, size_t size)
{
	return 0;
}
