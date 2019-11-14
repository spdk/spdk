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
#include "spdk/string.h"
#include "spdk/stdinc.h"
#include "spdk/crc32.h"
#include "spdk/endian.h"
#include "spdk/assert.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/trace.h"
#include "spdk/util.h"

#include "spdk_internal/nvme_tcp.h"

#define NVME_TCP_RW_BUFFER_SIZE 131072

#define NVME_TCP_HPDA_DEFAULT			0
#define NVME_TCP_MAX_R2T_DEFAULT		1
#define NVME_TCP_PDU_H2C_MIN_DATA_SIZE		4096
#define NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE	8192

/* NVMe TCP transport extensions for spdk_nvme_ctrlr */
struct nvme_tcp_ctrlr {
	struct spdk_nvme_ctrlr			ctrlr;
};

/* NVMe TCP qpair extensions for spdk_nvme_qpair */
struct nvme_tcp_qpair {
	struct spdk_nvme_qpair			qpair;
	struct spdk_sock			*sock;

	TAILQ_HEAD(, nvme_tcp_req)		free_reqs;
	TAILQ_HEAD(, nvme_tcp_req)		outstanding_reqs;

	TAILQ_HEAD(, nvme_tcp_pdu)		send_queue;
	struct nvme_tcp_pdu			recv_pdu;
	struct nvme_tcp_pdu			send_pdu; /* only for error pdu and init pdu */
	enum nvme_tcp_pdu_recv_state		recv_state;

	struct nvme_tcp_req			*tcp_reqs;

	uint16_t				num_entries;

	bool					host_hdgst_enable;
	bool					host_ddgst_enable;

	/** Specifies the maximum number of PDU-Data bytes per H2C Data Transfer PDU */
	uint32_t				maxh2cdata;

	uint32_t				maxr2t;

	/* 0 based value, which is used to guide the padding */
	uint8_t					cpda;

	enum nvme_tcp_qpair_state		state;
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
	uint32_t				r2tl_remain;
	uint32_t				active_r2ts;
	bool					in_capsule_data;
	struct nvme_tcp_pdu			send_pdu;
	struct iovec				iov[NVME_TCP_MAX_SGL_DESCRIPTORS];
	uint32_t				iovcnt;
	TAILQ_ENTRY(nvme_tcp_req)		link;
};

static void spdk_nvme_tcp_send_h2c_data(struct nvme_tcp_req *tcp_req);

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
	tcp_req->req = NULL;
	tcp_req->in_capsule_data = false;
	tcp_req->r2tl_remain = 0;
	tcp_req->active_r2ts = 0;
	tcp_req->iovcnt = 0;
	memset(&tcp_req->send_pdu, 0, sizeof(tcp_req->send_pdu));
	tcp_req->send_pdu.hdr = &tcp_req->send_pdu.hdr_mem;
	TAILQ_INSERT_TAIL(&tqpair->outstanding_reqs, tcp_req, link);

	return tcp_req;
}

static void
nvme_tcp_req_put(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	assert(tcp_req->state != NVME_TCP_REQ_FREE);
	tcp_req->state = NVME_TCP_REQ_FREE;
	TAILQ_REMOVE(&tqpair->outstanding_reqs, tcp_req, link);
	TAILQ_INSERT_TAIL(&tqpair->free_reqs, tcp_req, link);
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

static void
nvme_tcp_free_reqs(struct nvme_tcp_qpair *tqpair)
{
	free(tqpair->tcp_reqs);
	tqpair->tcp_reqs = NULL;
}

static int
nvme_tcp_alloc_reqs(struct nvme_tcp_qpair *tqpair)
{
	int i;
	struct nvme_tcp_req	*tcp_req;

	tqpair->tcp_reqs = calloc(tqpair->num_entries, sizeof(struct nvme_tcp_req));
	if (tqpair->tcp_reqs == NULL) {
		SPDK_ERRLOG("Failed to allocate tcp_reqs\n");
		goto fail;
	}

	TAILQ_INIT(&tqpair->send_queue);
	TAILQ_INIT(&tqpair->free_reqs);
	TAILQ_INIT(&tqpair->outstanding_reqs);
	for (i = 0; i < tqpair->num_entries; i++) {
		tcp_req = &tqpair->tcp_reqs[i];
		tcp_req->cid = i;
		TAILQ_INSERT_TAIL(&tqpair->free_reqs, tcp_req, link);
	}

	return 0;
fail:
	nvme_tcp_free_reqs(tqpair);
	return -ENOMEM;
}

static void
nvme_tcp_qpair_disconnect(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	struct nvme_tcp_pdu *pdu;

	qpair->transport_qp_is_failed = true;
	spdk_sock_close(&tqpair->sock);

	/* clear the send_queue */
	while (!TAILQ_EMPTY(&tqpair->send_queue)) {
		pdu = TAILQ_FIRST(&tqpair->send_queue);
		/* Remove the pdu from the send_queue to prevent the wrong sending out
		 * in the next round connection
		 */
		TAILQ_REMOVE(&tqpair->send_queue, pdu, tailq);
	}
}

static int
nvme_tcp_qpair_destroy(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair;

	if (!qpair) {
		return -1;
	}

	nvme_tcp_qpair_disconnect(qpair);
	nvme_tcp_qpair_abort_reqs(qpair, 1);
	nvme_qpair_deinit(qpair);
	tqpair = nvme_tcp_qpair(qpair);
	nvme_tcp_free_reqs(tqpair);
	free(tqpair);

	return 0;
}

int
nvme_tcp_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	return 0;
}

/* This function must only be called while holding g_spdk_nvme_driver->lock */
int
nvme_tcp_ctrlr_scan(struct spdk_nvme_probe_ctx *probe_ctx,
		    bool direct_connect)
{
	return nvme_fabric_ctrlr_scan(probe_ctx, direct_connect);
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
nvme_tcp_qpair_process_send_queue(struct nvme_tcp_qpair *tqpair)
{
	const int array_size = 32;
	struct iovec iovs[array_size];
	int iovcnt = 0;
	int bytes = 0;
	uint32_t mapped_length;
	struct nvme_tcp_pdu *pdu;
	int pdu_length;
	TAILQ_HEAD(, nvme_tcp_pdu) completed_pdus_list;

	pdu = TAILQ_FIRST(&tqpair->send_queue);

	if (pdu == NULL) {
		return 0;
	}

	/*
	 * Build up a list of iovecs for the first few PDUs in the
	 *  tqpair 's send_queue.
	 */
	while (pdu != NULL && ((array_size - iovcnt) >= (2 + (int)pdu->data_iovcnt))) {
		iovcnt += nvme_tcp_build_iovs(&iovs[iovcnt], array_size - iovcnt,
					      pdu, tqpair->host_hdgst_enable,
					      tqpair->host_ddgst_enable, &mapped_length);
		pdu = TAILQ_NEXT(pdu, tailq);
	}

	bytes = spdk_sock_writev(tqpair->sock, iovs, iovcnt);
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "bytes=%d are out\n", bytes);
	if (bytes == -1) {
		if (errno == EWOULDBLOCK || errno == EAGAIN) {
			return 1;
		} else {
			SPDK_ERRLOG("spdk_sock_writev() failed, errno %d: %s\n",
				    errno, spdk_strerror(errno));
			return -errno;
		}
	}

	pdu = TAILQ_FIRST(&tqpair->send_queue);

	/*
	 * Free any PDUs that were fully written.  If a PDU was only
	 *  partially written, update its writev_offset so that next
	 *  time only the unwritten portion will be sent to writev().
	 */
	TAILQ_INIT(&completed_pdus_list);
	while (bytes > 0) {
		pdu_length = pdu->hdr->common.plen - pdu->writev_offset;
		assert(pdu_length > 0);
		if (bytes >= pdu_length) {
			bytes -= pdu_length;
			TAILQ_REMOVE(&tqpair->send_queue, pdu, tailq);
			TAILQ_INSERT_TAIL(&completed_pdus_list, pdu, tailq);
			pdu = TAILQ_FIRST(&tqpair->send_queue);

		} else {
			pdu->writev_offset += bytes;
			bytes = 0;
		}
	}

	while (!TAILQ_EMPTY(&completed_pdus_list)) {
		pdu = TAILQ_FIRST(&completed_pdus_list);
		TAILQ_REMOVE(&completed_pdus_list, pdu, tailq);
		assert(pdu->cb_fn != NULL);
		pdu->cb_fn(pdu->cb_arg);
	}

	return TAILQ_EMPTY(&tqpair->send_queue) ? 0 : 1;

}

static int
nvme_tcp_qpair_write_pdu(struct nvme_tcp_qpair *tqpair,
			 struct nvme_tcp_pdu *pdu,
			 nvme_tcp_qpair_xfer_complete_cb cb_fn,
			 void *cb_arg)
{
	int enable_digest;
	int hlen;
	uint32_t crc32c;

	hlen = pdu->hdr->common.hlen;
	enable_digest = 1;
	if (pdu->hdr->common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_REQ ||
	    pdu->hdr->common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ) {
		/* this PDU should be sent without digest */
		enable_digest = 0;
	}

	/* Header Digest */
	if (enable_digest && tqpair->host_hdgst_enable) {
		crc32c = nvme_tcp_pdu_calc_header_digest(pdu);
		MAKE_DIGEST_WORD((uint8_t *)pdu->hdr->raw + hlen, crc32c);
	}

	/* Data Digest */
	if (pdu->data_len > 0 && enable_digest && tqpair->host_ddgst_enable) {
		crc32c = nvme_tcp_pdu_calc_data_digest(pdu);
		MAKE_DIGEST_WORD(pdu->data_digest, crc32c);
	}

	pdu->cb_fn = cb_fn;
	pdu->cb_arg = cb_arg;
	TAILQ_INSERT_TAIL(&tqpair->send_queue, pdu, tailq);
	return 0;
}

/*
 * Build SGL describing contiguous payload buffer.
 */
static int
nvme_tcp_build_contig_request(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	struct nvme_request *req = tcp_req->req;

	tcp_req->iov[0].iov_base = req->payload.contig_or_cb_arg + req->payload_offset;
	tcp_req->iov[0].iov_len = req->payload_size;
	tcp_req->iovcnt = 1;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "enter\n");

	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG);

	return 0;
}

/*
 * Build SGL describing scattered payload buffer.
 */
static int
nvme_tcp_build_sgl_request(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_req *tcp_req)
{
	int rc, iovcnt;
	uint32_t length;
	uint64_t remaining_size;
	struct nvme_request *req = tcp_req->req;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "enter\n");

	assert(req->payload_size != 0);
	assert(nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.reset_sgl_fn != NULL);
	assert(req->payload.next_sge_fn != NULL);
	req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);

	remaining_size = req->payload_size;
	iovcnt = 0;

	do {
		rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &tcp_req->iov[iovcnt].iov_base,
					      &length);
		if (rc) {
			return -1;
		}

		length = spdk_min(length, remaining_size);
		tcp_req->iov[iovcnt].iov_len = length;
		remaining_size -= length;
		iovcnt++;
	} while (remaining_size > 0 && iovcnt < NVME_TCP_MAX_SGL_DESCRIPTORS);


	/* Should be impossible if we did our sgl checks properly up the stack, but do a sanity check here. */
	if (remaining_size > 0) {
		return -1;
	}

	tcp_req->iovcnt = iovcnt;

	return 0;
}

static inline uint32_t
nvme_tcp_icdsz_bytes(struct spdk_nvme_ctrlr *ctrlr)
{
	return (ctrlr->cdata.nvmf_specific.ioccsz * 16 - sizeof(struct spdk_nvme_cmd));
}

static int
nvme_tcp_req_init(struct nvme_tcp_qpair *tqpair, struct nvme_request *req,
		  struct nvme_tcp_req *tcp_req)
{
	struct spdk_nvme_ctrlr *ctrlr = tqpair->qpair.ctrlr;
	int rc = 0;
	enum spdk_nvme_data_transfer xfer;
	uint32_t max_incapsule_data_size;

	tcp_req->req = req;
	req->cmd.cid = tcp_req->cid;
	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
	req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK;
	req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_TRANSPORT;
	req->cmd.dptr.sgl1.unkeyed.length = req->payload_size;

	if (nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_CONTIG) {
		rc = nvme_tcp_build_contig_request(tqpair, tcp_req);
	} else if (nvme_payload_type(&req->payload) == NVME_PAYLOAD_TYPE_SGL) {
		rc = nvme_tcp_build_sgl_request(tqpair, tcp_req);
	} else {
		rc = -1;
	}

	if (rc) {
		return rc;
	}

	if (req->cmd.opc == SPDK_NVME_OPC_FABRIC) {
		struct spdk_nvmf_capsule_cmd *nvmf_cmd = (struct spdk_nvmf_capsule_cmd *)&req->cmd;

		xfer = spdk_nvme_opc_get_data_transfer(nvmf_cmd->fctype);
	} else {
		xfer = spdk_nvme_opc_get_data_transfer(req->cmd.opc);
	}
	if (xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		max_incapsule_data_size = nvme_tcp_icdsz_bytes(ctrlr);
		if ((req->cmd.opc == SPDK_NVME_OPC_FABRIC) || nvme_qpair_is_admin_queue(&tqpair->qpair)) {
			max_incapsule_data_size = spdk_min(max_incapsule_data_size, NVME_TCP_IN_CAPSULE_DATA_MAX_SIZE);
		}

		if (req->payload_size <= max_incapsule_data_size) {
			req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
			req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
			req->cmd.dptr.sgl1.address = 0;
			tcp_req->in_capsule_data = true;
		}
	}

	return 0;
}

static void
nvme_tcp_qpair_cmd_send_complete(void *cb_arg)
{
}

static int
nvme_tcp_qpair_capsule_cmd_send(struct nvme_tcp_qpair *tqpair,
				struct nvme_tcp_req *tcp_req)
{
	struct nvme_tcp_pdu *pdu;
	struct spdk_nvme_tcp_cmd *capsule_cmd;
	uint32_t plen = 0, alignment;
	uint8_t pdo;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "enter\n");
	pdu = &tcp_req->send_pdu;

	capsule_cmd = &pdu->hdr->capsule_cmd;
	capsule_cmd->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_CAPSULE_CMD;
	plen = capsule_cmd->common.hlen = sizeof(*capsule_cmd);
	capsule_cmd->ccsqe = tcp_req->req->cmd;


	SPDK_DEBUGLOG(SPDK_LOG_NVME, "capsule_cmd cid=%u on tqpair(%p)\n", tcp_req->req->cmd.cid, tqpair);

	if (tqpair->host_hdgst_enable) {
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "Header digest is enabled for capsule command on tcp_req=%p\n",
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
	if (tqpair->host_ddgst_enable) {
		capsule_cmd->common.flags |= SPDK_NVME_TCP_CH_FLAGS_DDGSTF;
		plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	tcp_req->datao = 0;
	nvme_tcp_pdu_set_data_buf(pdu, tcp_req->iov, tcp_req->iovcnt,
				  0, tcp_req->req->payload_size);
end:
	capsule_cmd->common.plen = plen;
	return nvme_tcp_qpair_write_pdu(tqpair, pdu, nvme_tcp_qpair_cmd_send_complete, NULL);

}

int
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
		/* Inform the upper layer to try again later. */
		return -EAGAIN;
	}

	if (nvme_tcp_req_init(tqpair, req, tcp_req)) {
		SPDK_ERRLOG("nvme_tcp_req_init() failed\n");
		nvme_tcp_req_put(tqpair, tcp_req);
		return -1;
	}

	return nvme_tcp_qpair_capsule_cmd_send(tqpair, tcp_req);
}

int
nvme_tcp_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	return nvme_tcp_qpair_destroy(qpair);
}

int
nvme_tcp_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	return 0;
}

static void
nvme_tcp_req_complete(struct nvme_request *req,
		      struct spdk_nvme_cpl *rsp)
{
	nvme_complete_request(req->cb_fn, req->cb_arg, req->qpair, req, rsp);
	nvme_free_request(req);
}

void
nvme_tcp_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	struct nvme_tcp_req *tcp_req, *tmp;
	struct nvme_request *req;
	struct spdk_nvme_cpl cpl;
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	cpl.status.dnr = dnr;

	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->outstanding_reqs, link, tmp) {
		assert(tcp_req->req != NULL);
		req = tcp_req->req;

		nvme_tcp_req_complete(req, &cpl);
		nvme_tcp_req_put(tqpair, tcp_req);
	}
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
	case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY:
	case NVME_TCP_PDU_RECV_STATE_ERROR:
		memset(&tqpair->recv_pdu, 0, sizeof(struct nvme_tcp_pdu));
		tqpair->recv_pdu.hdr = &tqpair->recv_pdu.hdr_mem;
		break;
	case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH:
	case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH:
	case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD:
	default:
		break;
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

	rsp_pdu = &tqpair->send_pdu;
	memset(rsp_pdu, 0, sizeof(*rsp_pdu));
	rsp_pdu->hdr = &rsp_pdu->hdr_mem;
	h2c_term_req = &rsp_pdu->hdr->term_req;
	h2c_term_req->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_H2C_TERM_REQ;
	h2c_term_req->common.hlen = h2c_term_req_hdr_len;

	if ((fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD) ||
	    (fes == SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER)) {
		DSET32(&h2c_term_req->fei, error_offset);
	}

	copy_len = pdu->hdr->common.hlen;
	if (copy_len > SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE) {
		copy_len = SPDK_NVME_TCP_TERM_REQ_ERROR_DATA_MAX_SIZE;
	}

	/* Copy the error info into the buffer */
	memcpy((uint8_t *)rsp_pdu->hdr->raw + h2c_term_req_hdr_len, pdu->hdr->raw, copy_len);
	nvme_tcp_pdu_set_data(rsp_pdu, (uint8_t *)rsp_pdu->hdr->raw + h2c_term_req_hdr_len, copy_len);

	/* Contain the header len of the wrong received pdu */
	h2c_term_req->common.plen = h2c_term_req->common.hlen + copy_len;
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
	nvme_tcp_qpair_write_pdu(tqpair, rsp_pdu, nvme_tcp_qpair_send_h2c_term_req_complete, NULL);

}

static void
nvme_tcp_pdu_ch_handle(struct nvme_tcp_qpair *tqpair)
{
	struct nvme_tcp_pdu *pdu;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	uint32_t expected_hlen, hd_len = 0;
	bool plen_error = false;

	pdu = &tqpair->recv_pdu;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "pdu type = %d\n", pdu->hdr->common.pdu_type);
	if (pdu->hdr->common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_IC_RESP) {
		if (tqpair->state != NVME_TCP_QPAIR_STATE_INVALID) {
			SPDK_ERRLOG("Already received IC_RESP PDU, and we should reject this pdu=%p\n", pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
			goto err;
		}
		expected_hlen = sizeof(struct spdk_nvme_tcp_ic_resp);
		if (pdu->hdr->common.plen != expected_hlen) {
			plen_error = true;
		}
	} else {
		if (tqpair->state != NVME_TCP_QPAIR_STATE_RUNNING) {
			SPDK_ERRLOG("The TCP/IP tqpair connection is not negotitated\n");
			fes = SPDK_NVME_TCP_TERM_REQ_FES_PDU_SEQUENCE_ERROR;
			goto err;
		}

		switch (pdu->hdr->common.pdu_type) {
		case SPDK_NVME_TCP_PDU_TYPE_CAPSULE_RESP:
			expected_hlen = sizeof(struct spdk_nvme_tcp_rsp);
			if (pdu->hdr->common.flags & SPDK_NVME_TCP_CH_FLAGS_HDGSTF) {
				hd_len = SPDK_NVME_TCP_DIGEST_LEN;
			}

			if (pdu->hdr->common.plen != (expected_hlen + hd_len)) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_C2H_DATA:
			expected_hlen = sizeof(struct spdk_nvme_tcp_c2h_data_hdr);
			if (pdu->hdr->common.plen < pdu->hdr->common.pdo) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_C2H_TERM_REQ:
			expected_hlen = sizeof(struct spdk_nvme_tcp_term_req_hdr);
			if ((pdu->hdr->common.plen <= expected_hlen) ||
			    (pdu->hdr->common.plen > SPDK_NVME_TCP_TERM_REQ_PDU_MAX_SIZE)) {
				plen_error = true;
			}
			break;
		case SPDK_NVME_TCP_PDU_TYPE_R2T:
			expected_hlen = sizeof(struct spdk_nvme_tcp_r2t_hdr);
			if (pdu->hdr->common.flags & SPDK_NVME_TCP_CH_FLAGS_HDGSTF) {
				hd_len = SPDK_NVME_TCP_DIGEST_LEN;
			}

			if (pdu->hdr->common.plen != (expected_hlen + hd_len)) {
				plen_error = true;
			}
			break;

		default:
			SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", tqpair->recv_pdu.hdr->common.pdu_type);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
			error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, pdu_type);
			goto err;
		}
	}

	if (pdu->hdr->common.hlen != expected_hlen) {
		SPDK_ERRLOG("Expected PDU header length %u, got %u\n",
			    expected_hlen, pdu->hdr->common.hlen);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, hlen);
		goto err;

	} else if (plen_error) {
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_common_pdu_hdr, plen);
		goto err;
	} else {
		nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
		nvme_tcp_pdu_calc_psh_len(&tqpair->recv_pdu, tqpair->host_hdgst_enable);
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
nvme_tcp_c2h_data_payload_handle(struct nvme_tcp_qpair *tqpair,
				 struct nvme_tcp_pdu *pdu, uint32_t *reaped)
{
	struct nvme_tcp_req *tcp_req;
	struct spdk_nvme_tcp_c2h_data_hdr *c2h_data;
	struct spdk_nvme_cpl cpl = {};
	uint8_t flags;

	tcp_req = pdu->req;
	assert(tcp_req != NULL);

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "enter\n");
	c2h_data = &pdu->hdr->c2h_data;
	tcp_req->datao += pdu->data_len;
	flags = c2h_data->common.flags;

	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	if (flags & SPDK_NVME_TCP_C2H_DATA_FLAGS_SUCCESS) {
		if (tcp_req->datao == tcp_req->req->payload_size) {
			cpl.status.p = 0;
		} else {
			cpl.status.p = 1;
		}

		cpl.cid = tcp_req->cid;
		cpl.sqid = tqpair->qpair.id;
		nvme_tcp_req_complete(tcp_req->req, &cpl);
		nvme_tcp_req_put(tqpair, tcp_req);
		(*reaped)++;
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
		SPDK_DEBUGLOG(SPDK_LOG_NVME, "The offset from the start of the PDU header is %u\n",
			      DGET32(c2h_term_req->fei));
	}
	/* we may also need to dump some other info here */
}

static void
nvme_tcp_c2h_term_req_payload_handle(struct nvme_tcp_qpair *tqpair,
				     struct nvme_tcp_pdu *pdu)
{
	nvme_tcp_c2h_term_req_dump(&pdu->hdr->term_req);
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
}

static void
nvme_tcp_pdu_payload_handle(struct nvme_tcp_qpair *tqpair,
			    uint32_t *reaped)
{
	int rc = 0;
	struct nvme_tcp_pdu *pdu;
	uint32_t crc32c, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	pdu = &tqpair->recv_pdu;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "enter\n");

	/* check data digest if need */
	if (pdu->ddgst_enable) {
		crc32c = nvme_tcp_pdu_calc_data_digest(pdu);
		rc = MATCH_DIGEST_WORD(pdu->data_digest, crc32c);
		if (rc == 0) {
			SPDK_ERRLOG("data digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_HDGST_ERROR;
			nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
			return;
		}
	}

	switch (pdu->hdr->common.pdu_type) {
	case SPDK_NVME_TCP_PDU_TYPE_C2H_DATA:
		nvme_tcp_c2h_data_payload_handle(tqpair, pdu, reaped);
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
nvme_tcp_send_icreq_complete(void *cb_arg)
{
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "Complete the icreq send for tqpair=%p\n",
		      (struct nvme_tcp_qpair *)cb_arg);
}

static void
nvme_tcp_icresp_handle(struct nvme_tcp_qpair *tqpair,
		       struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_ic_resp *ic_resp = &pdu->hdr->ic_resp;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

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

	tqpair->host_hdgst_enable = ic_resp->dgst.bits.hdgst_enable ? true : false;
	tqpair->host_ddgst_enable = ic_resp->dgst.bits.ddgst_enable ? true : false;
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "host_hdgst_enable: %u\n", tqpair->host_hdgst_enable);
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "host_ddgst_enable: %u\n", tqpair->host_ddgst_enable);

	tqpair->state = NVME_TCP_QPAIR_STATE_RUNNING;
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);
	return;
end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
	return;
}

static void
nvme_tcp_capsule_resp_hdr_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu,
				 uint32_t *reaped)
{
	struct nvme_tcp_req *tcp_req;
	struct spdk_nvme_tcp_rsp *capsule_resp = &pdu->hdr->capsule_resp;
	uint32_t cid, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;
	struct spdk_nvme_cpl cpl;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "enter\n");
	cpl = capsule_resp->rccqe;
	cid = cpl.cid;

	/* Recv the pdu again */
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);

	tcp_req = get_nvme_active_req_by_cid(tqpair, cid);
	if (!tcp_req) {
		SPDK_ERRLOG("no tcp_req is found with cid=%u for tqpair=%p\n", cid, tqpair);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_rsp, rccqe);
		goto end;

	}

	assert(tcp_req->req != NULL);
	assert(tcp_req->state == NVME_TCP_REQ_ACTIVE);
	nvme_tcp_req_complete(tcp_req->req, &cpl);
	nvme_tcp_req_put(tqpair, tcp_req);
	(*reaped)++;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "complete tcp_req(%p) on tqpair=%p\n", tcp_req, tqpair);

	return;

end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
	return;
}

static void
nvme_tcp_c2h_term_req_hdr_handle(struct nvme_tcp_qpair *tqpair,
				 struct nvme_tcp_pdu *pdu)
{
	struct spdk_nvme_tcp_term_req_hdr *c2h_term_req = &pdu->hdr->term_req;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	if (c2h_term_req->fes > SPDK_NVME_TCP_TERM_REQ_FES_INVALID_DATA_UNSUPPORTED_PARAMETER) {
		SPDK_ERRLOG("Fatal Error Stauts(FES) is unknown for c2h_term_req pdu=%p\n", pdu);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_term_req_hdr, fes);
		goto end;
	}

	/* set the data buffer */
	nvme_tcp_pdu_set_data(pdu, (uint8_t *)pdu->hdr->raw + c2h_term_req->common.hlen,
			      c2h_term_req->common.plen - c2h_term_req->common.hlen);
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	return;
end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
	return;
}

static void
nvme_tcp_c2h_data_hdr_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req;
	struct spdk_nvme_tcp_c2h_data_hdr *c2h_data = &pdu->hdr->c2h_data;
	uint32_t error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "enter\n");
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "c2h_data info on tqpair(%p): datao=%u, datal=%u, cccid=%d\n",
		      tqpair, c2h_data->datao, c2h_data->datal, c2h_data->cccid);
	tcp_req = get_nvme_active_req_by_cid(tqpair, c2h_data->cccid);
	if (!tcp_req) {
		SPDK_ERRLOG("no tcp_req found for c2hdata cid=%d\n", c2h_data->cccid);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_c2h_data_hdr, cccid);
		goto end;

	}

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "tcp_req(%p) on tqpair(%p): datao=%u, payload_size=%u\n",
		      tcp_req, tqpair, tcp_req->datao, tcp_req->req->payload_size);

	if (c2h_data->datal > tcp_req->req->payload_size) {
		SPDK_ERRLOG("Invalid datal for tcp_req(%p), datal(%u) exceeds payload_size(%u)\n",
			    tcp_req, c2h_data->datal, tcp_req->req->payload_size);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_DATA_TRANSFER_OUT_OF_RANGE;
		goto end;
	}

	if (tcp_req->datao != c2h_data->datao) {
		SPDK_ERRLOG("Invalid datao for tcp_req(%p), received datal(%u) != datao(%u) in tcp_req\n",
			    tcp_req, c2h_data->datao, tcp_req->datao);
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

	nvme_tcp_pdu_set_data_buf(pdu, tcp_req->iov, tcp_req->iovcnt,
				  c2h_data->datao, c2h_data->datal);
	pdu->req = tcp_req;

	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD);
	return;

end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
	return;
}

static void
nvme_tcp_qpair_h2c_data_send_complete(void *cb_arg)
{
	struct nvme_tcp_req *tcp_req = cb_arg;

	assert(tcp_req != NULL);

	if (tcp_req->r2tl_remain) {
		spdk_nvme_tcp_send_h2c_data(tcp_req);
	} else {
		assert(tcp_req->active_r2ts > 0);
		tcp_req->active_r2ts--;
		tcp_req->state = NVME_TCP_REQ_ACTIVE;
	}
}

static void
spdk_nvme_tcp_send_h2c_data(struct nvme_tcp_req *tcp_req)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(tcp_req->req->qpair);
	struct nvme_tcp_pdu *rsp_pdu;
	struct spdk_nvme_tcp_h2c_data_hdr *h2c_data;
	uint32_t plen, pdo, alignment;

	rsp_pdu = &tcp_req->send_pdu;
	memset(rsp_pdu, 0, sizeof(*rsp_pdu));
	rsp_pdu->hdr = &rsp_pdu->hdr_mem;
	h2c_data = &rsp_pdu->hdr->h2c_data;

	h2c_data->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_H2C_DATA;
	plen = h2c_data->common.hlen = sizeof(*h2c_data);
	h2c_data->cccid = tcp_req->cid;
	h2c_data->ttag = tcp_req->ttag;
	h2c_data->datao = tcp_req->datao;

	h2c_data->datal = spdk_min(tcp_req->r2tl_remain, tqpair->maxh2cdata);
	nvme_tcp_pdu_set_data_buf(rsp_pdu, tcp_req->iov, tcp_req->iovcnt,
				  h2c_data->datao, h2c_data->datal);
	tcp_req->r2tl_remain -= h2c_data->datal;

	if (tqpair->host_hdgst_enable) {
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
	if (tqpair->host_ddgst_enable) {
		h2c_data->common.flags |= SPDK_NVME_TCP_CH_FLAGS_DDGSTF;
		plen += SPDK_NVME_TCP_DIGEST_LEN;
	}

	h2c_data->common.plen = plen;
	tcp_req->datao += h2c_data->datal;
	if (!tcp_req->r2tl_remain) {
		h2c_data->common.flags |= SPDK_NVME_TCP_H2C_DATA_FLAGS_LAST_PDU;
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "h2c_data info: datao=%u, datal=%u, pdu_len=%u for tqpair=%p\n",
		      h2c_data->datao, h2c_data->datal, h2c_data->common.plen, tqpair);

	nvme_tcp_qpair_write_pdu(tqpair, rsp_pdu, nvme_tcp_qpair_h2c_data_send_complete, tcp_req);
}

static void
nvme_tcp_r2t_hdr_handle(struct nvme_tcp_qpair *tqpair, struct nvme_tcp_pdu *pdu)
{
	struct nvme_tcp_req *tcp_req;
	struct spdk_nvme_tcp_r2t_hdr *r2t = &pdu->hdr->r2t;
	uint32_t cid, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "enter\n");
	cid = r2t->cccid;
	tcp_req = get_nvme_active_req_by_cid(tqpair, cid);
	if (!tcp_req) {
		SPDK_ERRLOG("Cannot find tcp_req for tqpair=%p\n", tqpair);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = offsetof(struct spdk_nvme_tcp_r2t_hdr, cccid);
		goto end;
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "r2t info: r2to=%u, r2tl=%u for tqpair=%p\n", r2t->r2to, r2t->r2tl,
		      tqpair);

	if (tcp_req->state == NVME_TCP_REQ_ACTIVE) {
		assert(tcp_req->active_r2ts == 0);
		tcp_req->state = NVME_TCP_REQ_ACTIVE_R2T;
	}

	tcp_req->active_r2ts++;
	if (tcp_req->active_r2ts > tqpair->maxr2t) {
		fes = SPDK_NVME_TCP_TERM_REQ_FES_R2T_LIMIT_EXCEEDED;
		SPDK_ERRLOG("Invalid R2T: it exceeds the R2T maixmal=%u for tqpair=%p\n", tqpair->maxr2t, tqpair);
		goto end;
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

	tcp_req->ttag = r2t->ttag;
	tcp_req->r2tl_remain = r2t->r2tl;
	nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY);

	spdk_nvme_tcp_send_h2c_data(tcp_req);
	return;

end:
	nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
	return;

}

static void
nvme_tcp_pdu_psh_handle(struct nvme_tcp_qpair *tqpair, uint32_t *reaped)
{
	struct nvme_tcp_pdu *pdu;
	int rc;
	uint32_t crc32c, error_offset = 0;
	enum spdk_nvme_tcp_term_req_fes fes;

	assert(tqpair->recv_state == NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH);
	pdu = &tqpair->recv_pdu;

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "enter: pdu type =%u\n", pdu->hdr->common.pdu_type);
	/* check header digest if needed */
	if (pdu->has_hdgst) {
		crc32c = nvme_tcp_pdu_calc_header_digest(pdu);
		rc = MATCH_DIGEST_WORD((uint8_t *)pdu->hdr->raw + pdu->hdr->common.hlen, crc32c);
		if (rc == 0) {
			SPDK_ERRLOG("header digest error on tqpair=(%p) with pdu=%p\n", tqpair, pdu);
			fes = SPDK_NVME_TCP_TERM_REQ_FES_HDGST_ERROR;
			nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
			return;

		}
	}

	switch (pdu->hdr->common.pdu_type) {
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
		SPDK_ERRLOG("Unexpected PDU type 0x%02x\n", tqpair->recv_pdu.hdr->common.pdu_type);
		fes = SPDK_NVME_TCP_TERM_REQ_FES_INVALID_HEADER_FIELD;
		error_offset = 1;
		nvme_tcp_qpair_send_h2c_term_req(tqpair, pdu, fes, error_offset);
		break;
	}

}

static int
nvme_tcp_read_pdu(struct nvme_tcp_qpair *tqpair, uint32_t *reaped)
{
	int rc = 0;
	struct nvme_tcp_pdu *pdu;
	uint32_t data_len;
	enum nvme_tcp_pdu_recv_state prev_state;

	/* The loop here is to allow for several back-to-back state changes. */
	do {
		prev_state = tqpair->recv_state;
		switch (tqpair->recv_state) {
		/* If in a new state */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_READY:
			nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH);
			break;
		/* common header */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_CH:
			pdu = &tqpair->recv_pdu;
			if (pdu->ch_valid_bytes < sizeof(struct spdk_nvme_tcp_common_pdu_hdr)) {
				rc = nvme_tcp_read_data(tqpair->sock,
							sizeof(struct spdk_nvme_tcp_common_pdu_hdr) - pdu->ch_valid_bytes,
							(uint8_t *)&pdu->hdr->common + pdu->ch_valid_bytes);
				if (rc < 0) {
					nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
					break;
				}
				pdu->ch_valid_bytes += rc;
				if (pdu->ch_valid_bytes < sizeof(struct spdk_nvme_tcp_common_pdu_hdr)) {
					return NVME_TCP_PDU_IN_PROGRESS;
				}
			}

			/* The command header of this PDU has now been read from the socket. */
			nvme_tcp_pdu_ch_handle(tqpair);
			break;
		/* Wait for the pdu specific header  */
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PSH:
			pdu = &tqpair->recv_pdu;
			rc = nvme_tcp_read_data(tqpair->sock,
						pdu->psh_len - pdu->psh_valid_bytes,
						(uint8_t *)&pdu->hdr->raw + sizeof(struct spdk_nvme_tcp_common_pdu_hdr) + pdu->psh_valid_bytes);
			if (rc < 0) {
				nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
				break;
			}

			pdu->psh_valid_bytes += rc;
			if (pdu->psh_valid_bytes < pdu->psh_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			/* All header(ch, psh, head digist) of this PDU has now been read from the socket. */
			nvme_tcp_pdu_psh_handle(tqpair, reaped);
			break;
		case NVME_TCP_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD:
			pdu = &tqpair->recv_pdu;
			/* check whether the data is valid, if not we just return */
			if (!pdu->data_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			data_len = pdu->data_len;
			/* data digest */
			if (spdk_unlikely((pdu->hdr->common.pdu_type == SPDK_NVME_TCP_PDU_TYPE_C2H_DATA) &&
					  tqpair->host_ddgst_enable)) {
				data_len += SPDK_NVME_TCP_DIGEST_LEN;
				pdu->ddgst_enable = true;

			}

			rc = nvme_tcp_read_payload_data(tqpair->sock, pdu);
			if (rc < 0) {
				nvme_tcp_qpair_set_recv_state(tqpair, NVME_TCP_PDU_RECV_STATE_ERROR);
				break;
			}

			pdu->readv_offset += rc;
			if (pdu->readv_offset < data_len) {
				return NVME_TCP_PDU_IN_PROGRESS;
			}

			assert(pdu->readv_offset == data_len);
			/* All of this PDU has now been read from the socket. */
			nvme_tcp_pdu_payload_handle(tqpair, reaped);
			break;
		case NVME_TCP_PDU_RECV_STATE_ERROR:
			rc = NVME_TCP_PDU_FATAL;
			break;
		default:
			assert(0);
			break;
		}
	} while (prev_state != tqpair->recv_state);

	return rc;
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
	if (ctrlr->state != NVME_CTRLR_STATE_READY) {
		return;
	}

	if (nvme_qpair_is_admin_queue(qpair)) {
		active_proc = spdk_nvme_ctrlr_get_current_process(ctrlr);
	} else {
		active_proc = qpair->active_proc;
	}

	/* Only check timeouts if the current process has a timeout callback. */
	if (active_proc == NULL || active_proc->timeout_cb_fn == NULL) {
		return;
	}

	t02 = spdk_get_ticks();
	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->outstanding_reqs, link, tmp) {
		assert(tcp_req->req != NULL);

		if (nvme_request_check_timeout(tcp_req->req, tcp_req->cid, active_proc, t02)) {
			/*
			 * The requests are in order, so as soon as one has not timed out,
			 * stop iterating.
			 */
			break;
		}
	}
}

int
nvme_tcp_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	uint32_t reaped;
	int rc;

	rc = nvme_tcp_qpair_process_send_queue(tqpair);
	if (rc < 0) {
		return rc;
	}

	if (max_completions == 0) {
		max_completions = tqpair->num_entries;
	} else {
		max_completions = spdk_min(max_completions, tqpair->num_entries);
	}

	reaped = 0;
	do {
		rc = nvme_tcp_read_pdu(tqpair, &reaped);
		if (rc < 0) {
			SPDK_DEBUGLOG(SPDK_LOG_NVME, "Error polling CQ! (%d): %s\n",
				      errno, spdk_strerror(errno));
			goto fail;
		} else if (rc == 0) {
			/* Partial PDU is read */
			break;
		}

	} while (reaped < max_completions);

	if (spdk_unlikely(tqpair->qpair.ctrlr->timeout_enabled)) {
		nvme_tcp_qpair_check_timeout(qpair);
	}

	return reaped;
fail:

	/*
	 * Since admin queues take the ctrlr_lock before entering this function,
	 * we can call nvme_tcp_qpair_disconnect. For other qpairs we need
	 * to call the generic function which will take the lock for us.
	 */
	if (nvme_qpair_is_admin_queue(qpair)) {
		nvme_tcp_qpair_disconnect(qpair);
	} else {
		nvme_ctrlr_disconnect_qpair(qpair);
	}
	return -ENXIO;
}

static int
nvme_tcp_qpair_icreq_send(struct nvme_tcp_qpair *tqpair)
{
	struct spdk_nvme_tcp_ic_req *ic_req;
	struct nvme_tcp_pdu *pdu;

	pdu = &tqpair->send_pdu;
	memset(&tqpair->send_pdu, 0, sizeof(tqpair->send_pdu));
	pdu->hdr = &pdu->hdr_mem;
	ic_req = &pdu->hdr->ic_req;

	ic_req->common.pdu_type = SPDK_NVME_TCP_PDU_TYPE_IC_REQ;
	ic_req->common.hlen = ic_req->common.plen = sizeof(*ic_req);
	ic_req->pfv = 0;
	ic_req->maxr2t = NVME_TCP_MAX_R2T_DEFAULT - 1;
	ic_req->hpda = NVME_TCP_HPDA_DEFAULT;

	ic_req->dgst.bits.hdgst_enable = tqpair->qpair.ctrlr->opts.header_digest;
	ic_req->dgst.bits.ddgst_enable = tqpair->qpair.ctrlr->opts.data_digest;

	nvme_tcp_qpair_write_pdu(tqpair, pdu, nvme_tcp_send_icreq_complete, tqpair);

	while (tqpair->state == NVME_TCP_QPAIR_STATE_INVALID) {
		nvme_tcp_qpair_process_completions(&tqpair->qpair, 0);
	}

	if (tqpair->state != NVME_TCP_QPAIR_STATE_RUNNING) {
		SPDK_ERRLOG("Failed to construct the tqpair=%p via correct icresp\n", tqpair);
		return -1;
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVME, "Succesfully construct the tqpair=%p via correct icresp\n", tqpair);

	return 0;
}

static int
nvme_tcp_qpair_connect(struct nvme_tcp_qpair *tqpair)
{
	struct sockaddr_storage dst_addr;
	struct sockaddr_storage src_addr;
	int rc;
	struct spdk_nvme_ctrlr *ctrlr;
	int family;
	long int port;

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

	port = spdk_strtol(ctrlr->trid.trsvcid, 10);
	if (port <= 0 || port >= INT_MAX) {
		SPDK_ERRLOG("Invalid port: %s\n", ctrlr->trid.trsvcid);
		return -1;
	}

	tqpair->sock = spdk_sock_connect(ctrlr->trid.traddr, port);
	if (!tqpair->sock) {
		SPDK_ERRLOG("sock connection error of tqpair=%p with addr=%s, port=%ld\n",
			    tqpair, ctrlr->trid.traddr, port);
		return -1;
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
		return -1;
	}

	tqpair->qpair.transport_qp_is_failed = false;
	rc = nvme_fabric_qpair_connect(&tqpair->qpair, tqpair->num_entries);
	if (rc < 0) {
		tqpair->qpair.transport_qp_is_failed = true;
		SPDK_ERRLOG("Failed to send an NVMe-oF Fabric CONNECT command\n");
		return -1;
	}

	return 0;
}

int
nvme_tcp_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	return nvme_tcp_qpair_connect(nvme_tcp_qpair(qpair));
}

void
nvme_tcp_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	return nvme_tcp_qpair_disconnect(qpair);
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
	tqpair->recv_pdu.hdr = &tqpair->recv_pdu.hdr_mem;

	rc = nvme_qpair_init(qpair, qid, ctrlr, qprio, num_requests);
	if (rc != 0) {
		free(tqpair);
		return NULL;
	}

	rc = nvme_tcp_alloc_reqs(tqpair);
	if (rc) {
		nvme_tcp_qpair_destroy(qpair);
		return NULL;
	}

	rc = nvme_transport_ctrlr_connect_qpair(ctrlr, qpair);
	if (rc < 0) {
		nvme_tcp_qpair_destroy(qpair);
		return NULL;
	}

	return qpair;
}

struct spdk_nvme_qpair *
nvme_tcp_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
			       const struct spdk_nvme_io_qpair_opts *opts)
{
	return nvme_tcp_ctrlr_create_qpair(ctrlr, qid, opts->io_queue_size, opts->qprio,
					   opts->io_queue_requests);
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
		free(tctrlr);
		return NULL;
	}

	tctrlr->ctrlr.adminq = nvme_tcp_ctrlr_create_qpair(&tctrlr->ctrlr, 0,
			       SPDK_NVMF_MIN_ADMIN_QUEUE_ENTRIES, 0, SPDK_NVMF_MIN_ADMIN_QUEUE_ENTRIES);
	if (!tctrlr->ctrlr.adminq) {
		SPDK_ERRLOG("failed to create admin qpair\n");
		nvme_tcp_ctrlr_destruct(&tctrlr->ctrlr);
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

	if (nvme_ctrlr_add_process(&tctrlr->ctrlr, 0) != 0) {
		SPDK_ERRLOG("nvme_ctrlr_add_process() failed\n");
		nvme_ctrlr_destruct(&tctrlr->ctrlr);
		return NULL;
	}

	nvme_ctrlr_init_cap(&tctrlr->ctrlr, &cap, &vs);

	return &tctrlr->ctrlr;
}

uint32_t
nvme_tcp_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	/* TCP transport doens't limit maximum IO transfer size. */
	return UINT32_MAX;
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

volatile struct spdk_nvme_registers *
nvme_tcp_ctrlr_get_registers(struct spdk_nvme_ctrlr *ctrlr)
{
	return NULL;
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

void
nvme_tcp_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_req *tcp_req, *tmp;
	struct nvme_request *req;
	struct spdk_nvme_cpl cpl;
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
	cpl.status.sct = SPDK_NVME_SCT_GENERIC;

	TAILQ_FOREACH_SAFE(tcp_req, &tqpair->outstanding_reqs, link, tmp) {
		assert(tcp_req->req != NULL);
		req = tcp_req->req;
		if (req->cmd.opc != SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
			continue;
		}

		nvme_tcp_req_complete(req, &cpl);
		nvme_tcp_req_put(tqpair, tcp_req);
	}
}
