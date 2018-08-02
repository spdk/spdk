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

/*
 * Maximum number of SGL elements.
 * This is chosen to match the current nvme_pcie.c limit.
 */
#define NVME_TCP_MAX_SGL_DESCRIPTORS	(253)

struct nvme_tcp_request;

/* NVMe TCP transport extensions for spdk_nvme_ctrlr */
struct nvme_tcp_ctrlr {
	struct spdk_nvme_ctrlr			ctrlr;
	uint16_t				cntlid;
};

/* NVMe TCP qpair extensions for spdk_nvme_qpair */
struct nvme_tcp_qpair {
	struct spdk_nvme_qpair			qpair;
	struct spdk_sock			*sock;
	STAILQ_HEAD(, nvme_tcp_request)		free_reqs;
	STAILQ_HEAD(, nvme_tcp_request)		send_queue;
	uint32_t				in_capsule_data_size;
};

struct nvme_tcp_request {
	struct nvme_request			*req;
	uint16_t				cid;
	bool					in_capsule_data;
	uint32_t				datao;
	STAILQ_ENTRY(nvme_tcp_request)		link;
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

#if 0
static struct spdk_nvme_rdma_req *
nvme_rdma_req_get(struct nvme_rdma_qpair *rqpair)
{
	struct spdk_nvme_rdma_req *rdma_req;

	rdma_req = STAILQ_FIRST(&rqpair->free_reqs);
	if (rdma_req) {
		STAILQ_REMOVE_HEAD(&rqpair->free_reqs, link);
	}

	return rdma_req;
}

static void
nvme_rdma_req_put(struct nvme_rdma_qpair *rqpair, struct spdk_nvme_rdma_req *rdma_req)
{
	STAILQ_INSERT_HEAD(&rqpair->free_reqs, rdma_req, link);
}
#endif

#if 0
static int
nvme_tcp_qpair_init(struct nvme_tcp_qpair *tqpair)
{
	// TODO
	return -1;
}
#endif


#if 0
static int
nvme_rdma_parse_addr(struct sockaddr_storage *sa, int family, const char *addr, const char *service)
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
nvme_rdma_qpair_fabric_connect(struct nvme_rdma_qpair *rqpair)
{
	struct nvme_completion_poll_status status;
	struct spdk_nvmf_fabric_connect_rsp *rsp;
	struct spdk_nvmf_fabric_connect_cmd cmd;
	struct spdk_nvmf_fabric_connect_data *nvmf_data;
	struct spdk_nvme_ctrlr *ctrlr;
	struct nvme_rdma_ctrlr *rctrlr;
	int rc = 0;

	ctrlr = rqpair->qpair.ctrlr;
	if (!ctrlr) {
		return -1;
	}

	rctrlr = nvme_rdma_ctrlr(ctrlr);

	nvmf_data = spdk_dma_zmalloc(sizeof(*nvmf_data), 0, NULL);
	if (!nvmf_data) {
		SPDK_ERRLOG("nvmf_data allocation error\n");
		rc = -1;
		return rc;
	}

	memset(&cmd, 0, sizeof(cmd));
	memset(&status, 0, sizeof(struct nvme_completion_poll_status));

	cmd.opcode = SPDK_NVME_OPC_FABRIC;
	cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_CONNECT;
	cmd.qid = rqpair->qpair.id;
	cmd.sqsize = rqpair->num_entries - 1;
	cmd.kato = ctrlr->opts.keep_alive_timeout_ms;

	if (nvme_qpair_is_admin_queue(&rqpair->qpair)) {
		nvmf_data->cntlid = 0xFFFF;
	} else {
		nvmf_data->cntlid = rctrlr->cntlid;
	}

	SPDK_STATIC_ASSERT(sizeof(nvmf_data->hostid) == sizeof(ctrlr->opts.extended_host_id),
			   "host ID size mismatch");
	memcpy(nvmf_data->hostid, ctrlr->opts.extended_host_id, sizeof(nvmf_data->hostid));
	strncpy((char *)nvmf_data->hostnqn, ctrlr->opts.hostnqn, sizeof(nvmf_data->hostnqn));
	strncpy((char *)nvmf_data->subnqn, ctrlr->trid.subnqn, sizeof(nvmf_data->subnqn));

	rc = spdk_nvme_ctrlr_cmd_io_raw(ctrlr, &rqpair->qpair,
					(struct spdk_nvme_cmd *)&cmd,
					nvmf_data, sizeof(*nvmf_data),
					nvme_completion_poll_cb, &status);
	if (rc < 0) {
		SPDK_ERRLOG("spdk_nvme_rdma_req_fabric_connect failed\n");
		rc = -1;
		goto ret;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(&rqpair->qpair, 0);
	}

	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("Connect command failed\n");
		return -1;
	}

	if (nvme_qpair_is_admin_queue(&rqpair->qpair)) {
		rsp = (struct spdk_nvmf_fabric_connect_rsp *)&status.cpl;
		rctrlr->cntlid = rsp->status_code_specific.success.cntlid;
	}
ret:
	spdk_dma_free(nvmf_data);
	return rc;
}

static int
nvme_rdma_qpair_connect(struct nvme_rdma_qpair *rqpair)
{
	struct sockaddr_storage dst_addr;
	struct sockaddr_storage src_addr;
	bool src_addr_specified;
	int rc;
	struct spdk_nvme_ctrlr *ctrlr;
	int family;

	rqpair->cm_channel = rdma_create_event_channel();
	if (rqpair->cm_channel == NULL) {
		SPDK_ERRLOG("rdma_create_event_channel() failed\n");
		return -1;
	}

	ctrlr = rqpair->qpair.ctrlr;

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
	rc = nvme_rdma_parse_addr(&dst_addr, family, ctrlr->trid.traddr, ctrlr->trid.trsvcid);
	if (rc != 0) {
		SPDK_ERRLOG("dst_addr nvme_rdma_parse_addr() failed\n");
		return -1;
	}

	if (ctrlr->opts.src_addr[0] || ctrlr->opts.src_svcid[0]) {
		memset(&src_addr, 0, sizeof(src_addr));
		rc = nvme_rdma_parse_addr(&src_addr, family, ctrlr->opts.src_addr, ctrlr->opts.src_svcid);
		if (rc != 0) {
			SPDK_ERRLOG("src_addr nvme_rdma_parse_addr() failed\n");
			return -1;
		}
		src_addr_specified = true;
	} else {
		src_addr_specified = false;
	}

	rc = rdma_create_id(rqpair->cm_channel, &rqpair->cm_id, rqpair, RDMA_PS_TCP);
	if (rc < 0) {
		SPDK_ERRLOG("rdma_create_id() failed\n");
		return -1;
	}

	rc = nvme_rdma_resolve_addr(rqpair,
				    src_addr_specified ? (struct sockaddr *)&src_addr : NULL,
				    (struct sockaddr *)&dst_addr, rqpair->cm_channel);
	if (rc < 0) {
		SPDK_ERRLOG("nvme_rdma_resolve_addr() failed\n");
		return -1;
	}

	rc = nvme_rdma_qpair_init(rqpair);
	if (rc < 0) {
		SPDK_ERRLOG("nvme_rdma_qpair_init() failed\n");
		return -1;
	}

	rc = nvme_rdma_connect(rqpair);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to connect the rqpair\n");
		return -1;
	}

	rc = nvme_rdma_alloc_reqs(rqpair);
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "rc =%d\n", rc);
	if (rc) {
		SPDK_ERRLOG("Unable to allocate rqpair  RDMA requests\n");
		return -1;
	}
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "RDMA requests allocated\n");

	rc = nvme_rdma_alloc_rsps(rqpair);
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "rc =%d\n", rc);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to allocate rqpair RDMA responses\n");
		return -1;
	}
	SPDK_DEBUGLOG(SPDK_LOG_NVME, "RDMA responses allocated\n");

	rc = nvme_rdma_register_mem(rqpair);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to register memory for RDMA\n");
		return -1;
	}

	rc = nvme_rdma_qpair_fabric_connect(rqpair);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to send an NVMe-oF Fabric CONNECT command\n");
		return -1;
	}

	return 0;
}
#endif

static struct spdk_nvme_qpair *
nvme_tcp_ctrlr_create_qpair(struct spdk_nvme_ctrlr *ctrlr,
			    uint16_t qid, uint32_t qsize,
			    enum spdk_nvme_qprio qprio,
			    uint32_t num_requests)
{
	struct nvme_tcp_qpair *tqpair;
	int rc;

	tqpair = calloc(1, sizeof(*tqpair));
	if (!tqpair) {
		SPDK_ERRLOG("failed to allocate tqpair\n");
		return NULL;
	}

//	tqpair->num_entries = qsize;

	if (nvme_qpair_init(&tqpair->qpair, qid, ctrlr, qprio, num_requests) != 0) {
		return NULL;
	}

#if 0 // TODO
	if (nvme_tcp_qpair_connect(rqpair) < 0) {
		nvme_tcp_qpair_destroy(&tqpair->qpair);
		return NULL;
	}
#endif

	rc = nvme_fabric_qpair_connect(&tqpair->qpair, qsize);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to send an NVMe-oF Fabric CONNECT command\n");
		//nvme_tcp_qpair_destroy(&tqpair->qpair); // TODO
		return NULL;
	}

	return &tqpair->qpair;
}

static int
nvme_tcp_qpair_destroy(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);

	// TODO

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

int
nvme_tcp_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_tcp_qpair *tqpair = nvme_tcp_qpair(qpair);
	int rc;

	rc = nvme_tcp_qpair_process_send_queue(tqpair);
	if (rc) {
		// TODO: error handling
	}

	// TODO: recv here

#if 0
	struct nvme_tcp_qpair	*tqpair = nvme_tcp_qpair(qpair);
	int			i, rc, batch_size;
	uint32_t		reaped;

	if (max_completions == 0) {
		max_completions = rqpair->num_entries;
	} else {
		max_completions = spdk_min(max_completions, rqpair->num_entries);
	}

	reaped = 0;
	do {
		batch_size = spdk_min((max_completions - reaped),
				      MAX_COMPLETIONS_PER_POLL);
		rc = ibv_poll_cq(cq, batch_size, wc);
		if (rc < 0) {
			SPDK_ERRLOG("Error polling CQ! (%d): %s\n",
				    errno, spdk_strerror(errno));
			return -1;
		} else if (rc == 0) {
			/* Ran out of completions */
			break;
		}

		for (i = 0; i < rc; i++) {
			if (wc[i].status) {
				SPDK_ERRLOG("CQ error on Queue Pair %p, Response Index %lu (%d): %s\n",
					    qpair, wc[i].wr_id, wc[i].status, ibv_wc_status_str(wc[i].status));
				return -1;
			}

			switch (wc[i].opcode) {
			case IBV_WC_RECV:
				SPDK_DEBUGLOG(SPDK_LOG_NVME, "CQ recv completion\n");

				reaped++;

				if (wc[i].byte_len < sizeof(struct spdk_nvme_cpl)) {
					SPDK_ERRLOG("recv length %u less than expected response size\n", wc[i].byte_len);
					return -1;
				}

				if (nvme_rdma_recv(rqpair, wc[i].wr_id)) {
					SPDK_ERRLOG("nvme_rdma_recv processing failure\n");
					return -1;
				}
				break;

			case IBV_WC_SEND:
				break;

			default:
				SPDK_ERRLOG("Received an unexpected opcode on the CQ: %d\n", wc[i].opcode);
				return -1;
			}
		}
	} while (reaped < max_completions);

	return reaped;
#endif
	return -1; // TODO
}

uint32_t
nvme_tcp_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	// TODO: this should probably be mdts from controller (or is that applied elsewhere?)
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
