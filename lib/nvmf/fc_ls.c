/*
 *   BSD LICENSE
 *
 *   Copyright (c) 2018-2019 Broadcom.  All Rights Reserved.
 *   The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
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

#include "spdk/env.h"
#include "spdk/assert.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/util.h"
#include "spdk/endian.h"
#include "spdk/log.h"
#include "nvmf_internal.h"
#include "transport.h"
#include "spdk/nvmf_transport.h"

#include "nvmf_fc.h"
#include "fc_lld.h"

/* set to 1 to send ls disconnect in response to ls disconnect from host (per standard) */
#define NVMF_FC_LS_SEND_LS_DISCONNECT 0

/* Validation Error indexes into the string table below */
enum {
	VERR_NO_ERROR = 0,
	VERR_CR_ASSOC_LEN = 1,
	VERR_CR_ASSOC_RQST_LEN = 2,
	VERR_CR_ASSOC_CMD = 3,
	VERR_CR_ASSOC_CMD_LEN = 4,
	VERR_ERSP_RATIO = 5,
	VERR_ASSOC_ALLOC_FAIL = 6,
	VERR_CONN_ALLOC_FAIL = 7,
	VERR_CR_CONN_LEN = 8,
	VERR_CR_CONN_RQST_LEN = 9,
	VERR_ASSOC_ID = 10,
	VERR_ASSOC_ID_LEN = 11,
	VERR_NO_ASSOC = 12,
	VERR_CONN_ID = 13,
	VERR_CONN_ID_LEN = 14,
	VERR_NO_CONN = 15,
	VERR_CR_CONN_CMD = 16,
	VERR_CR_CONN_CMD_LEN = 17,
	VERR_DISCONN_LEN = 18,
	VERR_DISCONN_RQST_LEN = 19,
	VERR_DISCONN_CMD = 20,
	VERR_DISCONN_CMD_LEN = 21,
	VERR_DISCONN_SCOPE = 22,
	VERR_RS_LEN = 23,
	VERR_RS_RQST_LEN = 24,
	VERR_RS_CMD = 25,
	VERR_RS_CMD_LEN = 26,
	VERR_RS_RCTL = 27,
	VERR_RS_RO = 28,
	VERR_CONN_TOO_MANY = 29,
	VERR_SUBNQN = 30,
	VERR_HOSTNQN = 31,
	VERR_SQSIZE = 32,
	VERR_NO_RPORT = 33,
	VERR_SUBLISTENER = 34,
};

static char *validation_errors[] = {
	"OK",
	"Bad CR_ASSOC Length",
	"Bad CR_ASSOC Rqst Length",
	"Not CR_ASSOC Cmd",
	"Bad CR_ASSOC Cmd Length",
	"Bad Ersp Ratio",
	"Association Allocation Failed",
	"Queue Allocation Failed",
	"Bad CR_CONN Length",
	"Bad CR_CONN Rqst Length",
	"Not Association ID",
	"Bad Association ID Length",
	"No Association",
	"Not Connection ID",
	"Bad Connection ID Length",
	"No Connection",
	"Not CR_CONN Cmd",
	"Bad CR_CONN Cmd Length",
	"Bad DISCONN Length",
	"Bad DISCONN Rqst Length",
	"Not DISCONN Cmd",
	"Bad DISCONN Cmd Length",
	"Bad Disconnect Scope",
	"Bad RS Length",
	"Bad RS Rqst Length",
	"Not RS Cmd",
	"Bad RS Cmd Length",
	"Bad RS R_CTL",
	"Bad RS Relative Offset",
	"Too many connections for association",
	"Invalid subnqn or subsystem not found",
	"Invalid hostnqn or subsystem doesn't allow host",
	"SQ size = 0 or too big",
	"No Remote Port",
	"Bad Subsystem Port",
};

static inline void
nvmf_fc_add_assoc_to_tgt_port(struct spdk_nvmf_fc_nport *tgtport,
			      struct spdk_nvmf_fc_association *assoc,
			      struct spdk_nvmf_fc_remote_port_info *rport);

static void
nvmf_fc_del_connection(struct spdk_nvmf_fc_association *assoc,
		       struct spdk_nvmf_fc_conn *fc_conn);

static inline FCNVME_BE32 cpu_to_be32(uint32_t in)
{
	uint32_t t;

	to_be32(&t, in);
	return (FCNVME_BE32)t;
}

static inline FCNVME_BE32 nvmf_fc_lsdesc_len(size_t sz)
{
	uint32_t t;

	to_be32(&t, sz - (2 * sizeof(uint32_t)));
	return (FCNVME_BE32)t;
}

static void
nvmf_fc_ls_format_rsp_hdr(void *buf, uint8_t ls_cmd, uint32_t desc_len,
			  uint8_t rqst_ls_cmd)
{
	struct spdk_nvmf_fc_ls_acc_hdr *acc_hdr = buf;

	acc_hdr->w0.ls_cmd = ls_cmd;
	acc_hdr->desc_list_len = desc_len;
	to_be32(&acc_hdr->rqst.desc_tag, FCNVME_LSDESC_RQST);
	acc_hdr->rqst.desc_len =
		nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_lsdesc_rqst));
	acc_hdr->rqst.w0.ls_cmd = rqst_ls_cmd;
}

static int
nvmf_fc_ls_format_rjt(void *buf, uint16_t buflen, uint8_t ls_cmd,
		      uint8_t reason, uint8_t explanation, uint8_t vendor)
{
	struct spdk_nvmf_fc_ls_rjt *rjt = buf;

	bzero(buf, sizeof(struct spdk_nvmf_fc_ls_rjt));
	nvmf_fc_ls_format_rsp_hdr(buf, FCNVME_LSDESC_RQST,
				  nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_ls_rjt)),
				  ls_cmd);
	to_be32(&rjt->rjt.desc_tag, FCNVME_LSDESC_RJT);
	rjt->rjt.desc_len = nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_lsdesc_rjt));
	rjt->rjt.reason_code = reason;
	rjt->rjt.reason_explanation = explanation;
	rjt->rjt.vendor = vendor;

	return sizeof(struct spdk_nvmf_fc_ls_rjt);
}

/* *************************************************** */
/* Allocators/Deallocators (associations, connections, */
/* poller API data)                                    */

static inline void
nvmf_fc_ls_free_association(struct spdk_nvmf_fc_association *assoc)
{
	/* free association's send disconnect buffer */
	if (assoc->snd_disconn_bufs) {
		nvmf_fc_free_srsr_bufs(assoc->snd_disconn_bufs);
	}

	/* free association's connections */
	free(assoc->conns_buf);

	/* free the association */
	free(assoc);
}

static int
nvmf_fc_ls_alloc_connections(struct spdk_nvmf_fc_association *assoc,
			     struct spdk_nvmf_transport *nvmf_transport)
{
	uint32_t i;
	struct spdk_nvmf_fc_conn *fc_conn;

	SPDK_DEBUGLOG(nvmf_fc_ls, "Pre-alloc %d qpairs for host NQN %s\n",
		      nvmf_transport->opts.max_qpairs_per_ctrlr, assoc->host_nqn);

	/* allocate memory for all connections at once */
	assoc->conns_buf = calloc(nvmf_transport->opts.max_qpairs_per_ctrlr + 1,
				  sizeof(struct spdk_nvmf_fc_conn));
	if (assoc->conns_buf == NULL) {
		SPDK_ERRLOG("Out of memory for connections for new association\n");
		return -ENOMEM;
	}

	for (i = 0; i < nvmf_transport->opts.max_qpairs_per_ctrlr; i++) {
		fc_conn = assoc->conns_buf + (i * sizeof(struct spdk_nvmf_fc_conn));
		TAILQ_INSERT_TAIL(&assoc->avail_fc_conns, fc_conn, assoc_avail_link);
	}

	return 0;
}

static struct spdk_nvmf_fc_association *
nvmf_fc_ls_new_association(uint32_t s_id,
			   struct spdk_nvmf_fc_nport *tgtport,
			   struct spdk_nvmf_fc_remote_port_info *rport,
			   struct spdk_nvmf_fc_lsdesc_cr_assoc_cmd *a_cmd,
			   struct spdk_nvmf_subsystem *subsys,
			   uint16_t rpi,
			   struct spdk_nvmf_transport *nvmf_transport)
{
	struct spdk_nvmf_fc_association *assoc;
	int rc;

	SPDK_DEBUGLOG(nvmf_fc_ls,
		      "New Association request for port %d nport %d rpi 0x%x\n",
		      tgtport->fc_port->port_hdl, tgtport->nport_hdl, rpi);

	assert(rport);
	if (!rport) {
		SPDK_ERRLOG("rport is null.\n");
		return NULL;
	}

	assoc = calloc(1, sizeof(struct spdk_nvmf_fc_association));
	if (!assoc) {
		SPDK_ERRLOG("unable to allocate memory for new association\n");
		return NULL;
	}

	/* initialize association */
#if (NVMF_FC_LS_SEND_LS_DISCONNECT == 1)
	/* allocate buffers to send LS disconnect command to host */
	assoc->snd_disconn_bufs =
		nvmf_fc_alloc_srsr_bufs(sizeof(struct spdk_nvmf_fc_ls_disconnect_rqst),
					sizeof(struct spdk_nvmf_fc_ls_rjt));
	if (!assoc->snd_disconn_bufs) {
		SPDK_ERRLOG("no dma memory for association's ls disconnect bufs\n");
		free(assoc);
		return NULL;
	}

	assoc->snd_disconn_bufs->rpi = rpi;
#endif
	assoc->s_id = s_id;
	assoc->tgtport = tgtport;
	assoc->rport = rport;
	assoc->subsystem = subsys;
	assoc->nvmf_transport = nvmf_transport;
	assoc->assoc_state = SPDK_NVMF_FC_OBJECT_CREATED;
	memcpy(assoc->host_id, a_cmd->hostid, FCNVME_ASSOC_HOSTID_LEN);
	memcpy(assoc->host_nqn, a_cmd->hostnqn, SPDK_NVME_NQN_FIELD_SIZE);
	memcpy(assoc->sub_nqn, a_cmd->subnqn, SPDK_NVME_NQN_FIELD_SIZE);
	TAILQ_INIT(&assoc->fc_conns);
	TAILQ_INIT(&assoc->avail_fc_conns);
	assoc->ls_del_op_ctx = NULL;

	/* allocate and assign connections for association */
	rc =  nvmf_fc_ls_alloc_connections(assoc, nvmf_transport);
	if (rc != 0) {
		nvmf_fc_ls_free_association(assoc);
		return NULL;
	}

	/* add association to target port's association list */
	nvmf_fc_add_assoc_to_tgt_port(tgtport, assoc, rport);
	return assoc;
}

static inline void
nvmf_fc_ls_append_del_cb_ctx(struct nvmf_fc_ls_op_ctx **opd_list,
			     struct nvmf_fc_ls_op_ctx *opd)
{
	struct nvmf_fc_ls_op_ctx *nxt;

	if (*opd_list) {
		nxt = *opd_list;
		while (nxt->next_op_ctx) {
			nxt = nxt->next_op_ctx;
		}
		nxt->next_op_ctx = opd;
	} else {
		*opd_list = opd;
	}
}

static struct spdk_nvmf_fc_conn *
nvmf_fc_ls_new_connection(struct spdk_nvmf_fc_association *assoc, uint16_t qid,
			  uint16_t esrp_ratio, uint16_t rpi, uint16_t sq_size,
			  struct spdk_nvmf_fc_nport *tgtport)
{
	struct spdk_nvmf_fc_conn *fc_conn;

	fc_conn = TAILQ_FIRST(&assoc->avail_fc_conns);
	if (!fc_conn) {
		SPDK_ERRLOG("out of connections for association %p\n", assoc);
		return NULL;
	}

	/* Remove from avail list and add to in use. */
	TAILQ_REMOVE(&assoc->avail_fc_conns, fc_conn, assoc_avail_link);
	memset(fc_conn, 0, sizeof(struct spdk_nvmf_fc_conn));

	/* Add conn to association's connection list */
	TAILQ_INSERT_TAIL(&assoc->fc_conns, fc_conn, assoc_link);
	assoc->conn_count++;

	if (qid == 0) {
		/* AdminQ connection. */
		assoc->aq_conn = fc_conn;
	}

	fc_conn->qpair.qid = qid;
	fc_conn->qpair.sq_head_max = sq_size;
	fc_conn->qpair.state = SPDK_NVMF_QPAIR_UNINITIALIZED;
	fc_conn->qpair.transport = assoc->nvmf_transport;
	TAILQ_INIT(&fc_conn->qpair.outstanding);

	fc_conn->conn_id = NVMF_FC_INVALID_CONN_ID;
	fc_conn->esrp_ratio = esrp_ratio;
	fc_conn->fc_assoc = assoc;
	fc_conn->s_id = assoc->s_id;
	fc_conn->d_id = assoc->tgtport->d_id;
	fc_conn->rpi = rpi;
	fc_conn->max_queue_depth = sq_size + 1;
	fc_conn->conn_state = SPDK_NVMF_FC_OBJECT_CREATED;
	TAILQ_INIT(&fc_conn->in_use_reqs);
	TAILQ_INIT(&fc_conn->fused_waiting_queue);

	/* save target port trid in connection (for subsystem
	 * listener validation in fabric connect command)
	 */
	nvmf_fc_create_trid(&fc_conn->trid, tgtport->fc_nodename.u.wwn,
			    tgtport->fc_portname.u.wwn);

	return fc_conn;
}

/* End - Allocators/Deallocators (associations, connections, */
/*       poller API data)                                    */
/* ********************************************************* */

static inline struct spdk_nvmf_fc_association *
nvmf_fc_ls_find_assoc(struct spdk_nvmf_fc_nport *tgtport, uint64_t assoc_id)
{
	struct spdk_nvmf_fc_association *assoc = NULL;

	TAILQ_FOREACH(assoc, &tgtport->fc_associations, link) {
		if (assoc->assoc_id == assoc_id) {
			if (assoc->assoc_state == SPDK_NVMF_FC_OBJECT_ZOMBIE) {
				assoc = NULL;
			}
			break;
		}
	}
	return assoc;
}

static inline void
nvmf_fc_add_assoc_to_tgt_port(struct spdk_nvmf_fc_nport *tgtport,
			      struct spdk_nvmf_fc_association *assoc,
			      struct spdk_nvmf_fc_remote_port_info *rport)
{
	TAILQ_INSERT_TAIL(&tgtport->fc_associations, assoc, link);
	tgtport->assoc_count++;
	rport->assoc_count++;
}

static inline void
nvmf_fc_del_assoc_from_tgt_port(struct spdk_nvmf_fc_association *assoc)
{
	struct spdk_nvmf_fc_nport *tgtport = assoc->tgtport;

	TAILQ_REMOVE(&tgtport->fc_associations, assoc, link);
	tgtport->assoc_count--;
	assoc->rport->assoc_count--;
}

static void
nvmf_fc_do_del_conn_cbs(struct nvmf_fc_ls_op_ctx *opd,
			int ret)
{
	SPDK_DEBUGLOG(nvmf_fc_ls,
		      "performing delete conn. callbacks\n");
	while (opd) {
		struct nvmf_fc_ls_op_ctx *nxt = opd->next_op_ctx;
		struct spdk_nvmf_fc_ls_del_conn_api_data *dp = &opd->u.del_conn;

		if (dp->ls_rqst) {
			if (nvmf_fc_xmt_ls_rsp(dp->ls_rqst->nport, dp->ls_rqst) != 0) {
				SPDK_ERRLOG("Send LS response for delete connection failed\n");
			}
		}
		if (dp->del_conn_cb) {
			dp->del_conn_cb(dp->del_conn_cb_data);
		}
		free(opd);
		opd = nxt;
	}
}

static void
nvmf_fc_ls_poller_delete_conn_cb(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret)
{
	struct nvmf_fc_ls_op_ctx *opd =
		(struct nvmf_fc_ls_op_ctx *)cb_data;
	struct spdk_nvmf_fc_ls_del_conn_api_data *dp = &opd->u.del_conn;
	struct spdk_nvmf_fc_conn *fc_conn = dp->args.fc_conn;
	struct spdk_nvmf_fc_association *assoc = fc_conn->fc_assoc;
	struct nvmf_fc_ls_op_ctx *opd_list = (struct nvmf_fc_ls_op_ctx *)fc_conn->ls_del_op_ctx;

	SPDK_DEBUGLOG(nvmf_fc_ls, "Poller Delete connection callback "
		      "for assoc_id 0x%lx conn_id 0x%lx\n", assoc->assoc_id,
		      fc_conn->conn_id);

	nvmf_fc_del_connection(assoc, fc_conn);
	nvmf_fc_do_del_conn_cbs(opd_list, 0);
}

static int
nvmf_fc_ls_poller_delete_conn(struct spdk_nvmf_fc_conn *fc_conn, bool send_abts,
			      struct spdk_nvmf_fc_ls_rqst *ls_rqst, bool backend_initiated,
			      spdk_nvmf_fc_del_conn_cb cb_fn, void *cb_data)
{
	struct spdk_nvmf_fc_association *assoc = fc_conn->fc_assoc;
	struct spdk_nvmf_fc_ls_del_conn_api_data *api_data;
	struct nvmf_fc_ls_op_ctx *opd = NULL;

	SPDK_DEBUGLOG(nvmf_fc_ls, "Poller Delete connection "
		      "for assoc_id 0x%lx conn_id 0x%lx\n", assoc->assoc_id,
		      fc_conn->conn_id);

	/* create context for delete connection API */
	opd = calloc(1, sizeof(struct nvmf_fc_ls_op_ctx));
	if (!opd) {
		SPDK_ERRLOG("Mem alloc failed for del conn op data");
		return -ENOMEM;
	}

	api_data = &opd->u.del_conn;
	api_data->assoc = assoc;
	api_data->ls_rqst = ls_rqst;
	api_data->del_conn_cb = cb_fn;
	api_data->del_conn_cb_data = cb_data;
	api_data->aq_conn = (assoc->aq_conn == fc_conn ? true : false);
	api_data->args.fc_conn = fc_conn;
	api_data->args.send_abts = send_abts;
	api_data->args.backend_initiated = backend_initiated;
	api_data->args.hwqp = fc_conn->hwqp;
	api_data->args.cb_info.cb_thread = spdk_get_thread();
	api_data->args.cb_info.cb_func = nvmf_fc_ls_poller_delete_conn_cb;
	api_data->args.cb_info.cb_data = opd;

	nvmf_fc_ls_append_del_cb_ctx((struct nvmf_fc_ls_op_ctx **) &fc_conn->ls_del_op_ctx, opd);

	assert(fc_conn->conn_state != SPDK_NVMF_FC_OBJECT_ZOMBIE);
	if (fc_conn->conn_state == SPDK_NVMF_FC_OBJECT_CREATED) {
		fc_conn->conn_state = SPDK_NVMF_FC_OBJECT_TO_BE_DELETED;
		nvmf_fc_poller_api_func(api_data->args.hwqp,
					SPDK_NVMF_FC_POLLER_API_DEL_CONNECTION,
					&api_data->args);
	}

	return 0;
}

/* callback from poller's ADD_Connection event */
static void
nvmf_fc_ls_add_conn_cb(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret)
{
	struct nvmf_fc_ls_op_ctx *opd =
		(struct nvmf_fc_ls_op_ctx *)cb_data;
	struct spdk_nvmf_fc_ls_add_conn_api_data *dp = &opd->u.add_conn;
	struct spdk_nvmf_fc_association *assoc = dp->assoc;
	struct spdk_nvmf_fc_nport *tgtport = assoc->tgtport;
	struct spdk_nvmf_fc_conn *fc_conn = dp->args.fc_conn;
	struct spdk_nvmf_fc_ls_rqst *ls_rqst = dp->ls_rqst;

	SPDK_DEBUGLOG(nvmf_fc_ls,
		      "add_conn_cb: assoc_id = 0x%lx, conn_id = 0x%lx\n",
		      assoc->assoc_id, fc_conn->conn_id);

	fc_conn->create_opd = NULL;

	if (assoc->assoc_state == SPDK_NVMF_FC_OBJECT_TO_BE_DELETED) {
		/* association is already being deleted - don't continue */
		free(opd);
		return;
	}

	if (dp->aq_conn) {
		struct spdk_nvmf_fc_ls_cr_assoc_acc *assoc_acc =
			(struct spdk_nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
		/* put connection and association ID in response */
		to_be64(&assoc_acc->conn_id.connection_id, fc_conn->conn_id);
		assoc_acc->assoc_id.association_id = assoc_acc->conn_id.connection_id;
	} else {
		struct spdk_nvmf_fc_ls_cr_conn_acc *conn_acc =
			(struct spdk_nvmf_fc_ls_cr_conn_acc *)ls_rqst->rspbuf.virt;
		/* put connection ID in response */
		to_be64(&conn_acc->conn_id.connection_id, fc_conn->conn_id);
	}

	/* send LS response */
	if (nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst) != 0) {
		SPDK_ERRLOG("Send LS response for %s failed - cleaning up\n",
			    dp->aq_conn ? "association" : "connection");
		nvmf_fc_ls_poller_delete_conn(fc_conn, false, NULL, false, NULL, NULL);
	} else {
		SPDK_DEBUGLOG(nvmf_fc_ls,
			      "LS response (conn_id 0x%lx) sent\n", fc_conn->conn_id);
	}

	free(opd);
}

void
nvmf_fc_ls_add_conn_failure(
	struct spdk_nvmf_fc_association *assoc,
	struct spdk_nvmf_fc_ls_rqst *ls_rqst,
	struct spdk_nvmf_fc_conn *fc_conn,
	bool aq_conn)
{
	struct spdk_nvmf_fc_ls_cr_assoc_rqst *rqst;
	struct spdk_nvmf_fc_ls_cr_assoc_acc *acc;
	struct spdk_nvmf_fc_nport *tgtport = assoc->tgtport;

	if (fc_conn->create_opd) {
		free(fc_conn->create_opd);
		fc_conn->create_opd = NULL;
	}

	rqst	 = (struct spdk_nvmf_fc_ls_cr_assoc_rqst *)ls_rqst->rqstbuf.virt;
	acc	 = (struct spdk_nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;

	/* send failure response */
	ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
			   FCNVME_MAX_LS_BUFFER_SIZE, rqst->w0.ls_cmd,
			   FCNVME_RJT_RC_INSUFF_RES,
			   FCNVME_RJT_EXP_NONE, 0);

	nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	nvmf_fc_del_connection(assoc, fc_conn);
}


static void
nvmf_fc_ls_add_conn_to_poller(
	struct spdk_nvmf_fc_association *assoc,
	struct spdk_nvmf_fc_ls_rqst *ls_rqst,
	struct spdk_nvmf_fc_conn *fc_conn,
	bool aq_conn)
{
	struct nvmf_fc_ls_op_ctx *opd;
	struct spdk_nvmf_fc_ls_add_conn_api_data *api_data;

	SPDK_DEBUGLOG(nvmf_fc_ls, "Add Connection to poller for "
		      "assoc_id 0x%lx conn_id 0x%lx\n", assoc->assoc_id,
		      fc_conn->conn_id);

	/* Create fc_req pool for this connection */
	if (nvmf_fc_create_conn_reqpool(fc_conn)) {
		SPDK_ERRLOG("allocate fc_req pool failed\n");
		goto error;
	}

	opd = calloc(1, sizeof(struct nvmf_fc_ls_op_ctx));
	if (!opd) {
		SPDK_ERRLOG("allocate api data for add conn op failed\n");
		goto error;
	}

	api_data = &opd->u.add_conn;

	api_data->args.fc_conn = fc_conn;
	api_data->args.cb_info.cb_thread = spdk_get_thread();
	api_data->args.cb_info.cb_func = nvmf_fc_ls_add_conn_cb;
	api_data->args.cb_info.cb_data = (void *)opd;
	api_data->assoc = assoc;
	api_data->ls_rqst = ls_rqst;
	api_data->aq_conn = aq_conn;

	SPDK_DEBUGLOG(nvmf_fc_ls,
		      "New QP callback called.\n");

	/* Let the nvmf_tgt decide which pollgroup to use. */
	fc_conn->create_opd = opd;
	spdk_nvmf_tgt_new_qpair(ls_rqst->nvmf_tgt, &fc_conn->qpair);
	return;
error:
	nvmf_fc_ls_add_conn_failure(assoc, ls_rqst, fc_conn, aq_conn);
}

/* Delete association functions */

static void
nvmf_fc_do_del_assoc_cbs(struct nvmf_fc_ls_op_ctx *opd, int ret)
{
	struct nvmf_fc_ls_op_ctx *nxt;
	struct spdk_nvmf_fc_delete_assoc_api_data *dp;

	while (opd) {
		dp = &opd->u.del_assoc;

		SPDK_DEBUGLOG(nvmf_fc_ls, "performing delete assoc. callback\n");
		dp->del_assoc_cb(dp->del_assoc_cb_data, ret);

		nxt = opd->next_op_ctx;
		free(opd);
		opd = nxt;
	}
}

static void
nvmf_fs_send_ls_disconnect_cb(void *hwqp, int32_t status, void *args)
{
	if (args) {
		SPDK_DEBUGLOG(nvmf_fc_ls, "free disconnect buffers\n");
		nvmf_fc_free_srsr_bufs((struct spdk_nvmf_fc_srsr_bufs *)args);
	}
}

static void
nvmf_fc_del_connection(struct spdk_nvmf_fc_association *assoc,
		       struct spdk_nvmf_fc_conn *fc_conn)
{
	/* Free connection specific fc_req pool */
	nvmf_fc_free_conn_reqpool(fc_conn);

	/* remove connection from association's connection list */
	TAILQ_REMOVE(&assoc->fc_conns, fc_conn, assoc_link);

	/* Give back connection to association's free pool */
	TAILQ_INSERT_TAIL(&assoc->avail_fc_conns, fc_conn, assoc_avail_link);

	fc_conn->conn_state = SPDK_NVMF_FC_OBJECT_ZOMBIE;
	fc_conn->ls_del_op_ctx = NULL;

	if (--assoc->conn_count == 0) {
		/* last connection - remove association from target port's association list */
		struct nvmf_fc_ls_op_ctx *cb_opd = (struct nvmf_fc_ls_op_ctx *)assoc->ls_del_op_ctx;

		SPDK_DEBUGLOG(nvmf_fc_ls,
			      "remove assoc. %lx\n", assoc->assoc_id);
		nvmf_fc_del_assoc_from_tgt_port(assoc);

		if (assoc->snd_disconn_bufs &&
		    assoc->tgtport->fc_port->hw_port_status == SPDK_FC_PORT_ONLINE) {

			struct spdk_nvmf_fc_ls_disconnect_rqst *dc_rqst;
			struct spdk_nvmf_fc_srsr_bufs *srsr_bufs;

			dc_rqst = (struct spdk_nvmf_fc_ls_disconnect_rqst *)
				  assoc->snd_disconn_bufs->rqst;

			bzero(dc_rqst, sizeof(struct spdk_nvmf_fc_ls_disconnect_rqst));

			/* fill in request descriptor */
			dc_rqst->w0.ls_cmd = FCNVME_LS_DISCONNECT;
			to_be32(&dc_rqst->desc_list_len,
				sizeof(struct spdk_nvmf_fc_ls_disconnect_rqst) -
				(2 * sizeof(uint32_t)));

			/* fill in disconnect command descriptor */
			to_be32(&dc_rqst->disconn_cmd.desc_tag, FCNVME_LSDESC_DISCONN_CMD);
			to_be32(&dc_rqst->disconn_cmd.desc_len,
				sizeof(struct spdk_nvmf_fc_lsdesc_disconn_cmd) -
				(2 * sizeof(uint32_t)));

			/* fill in association id descriptor */
			to_be32(&dc_rqst->assoc_id.desc_tag, FCNVME_LSDESC_ASSOC_ID),
				to_be32(&dc_rqst->assoc_id.desc_len,
					sizeof(struct spdk_nvmf_fc_lsdesc_assoc_id) -
					(2 * sizeof(uint32_t)));
			to_be64(&dc_rqst->assoc_id.association_id, assoc->assoc_id);

			srsr_bufs = assoc->snd_disconn_bufs;
			assoc->snd_disconn_bufs = NULL;

			SPDK_DEBUGLOG(nvmf_fc_ls, "Send LS disconnect\n");
			if (nvmf_fc_xmt_srsr_req(&assoc->tgtport->fc_port->ls_queue,
						 srsr_bufs, nvmf_fs_send_ls_disconnect_cb,
						 (void *)srsr_bufs)) {
				SPDK_ERRLOG("Error sending LS disconnect\n");
				assoc->snd_disconn_bufs = srsr_bufs;
			}
		}

		nvmf_fc_ls_free_association(assoc);

		/* perform callbacks to all callers to delete association */
		nvmf_fc_do_del_assoc_cbs(cb_opd, 0);
	}
}

/* Disconnect/delete (association) request functions */

static int
_nvmf_fc_delete_association(struct spdk_nvmf_fc_nport *tgtport,
			    uint64_t assoc_id, bool send_abts, bool backend_initiated,
			    spdk_nvmf_fc_del_assoc_cb del_assoc_cb,
			    void *cb_data, bool from_ls_rqst)
{
	int rc;
	struct nvmf_fc_ls_op_ctx *opd;
	struct spdk_nvmf_fc_delete_assoc_api_data *api_data;
	struct spdk_nvmf_fc_conn *fc_conn;
	struct spdk_nvmf_fc_association *assoc =
		nvmf_fc_ls_find_assoc(tgtport, assoc_id);
	enum spdk_nvmf_fc_object_state assoc_state;

	SPDK_DEBUGLOG(nvmf_fc_ls, "Delete association, "
		      "assoc_id 0x%lx\n", assoc_id);

	if (!assoc) {
		SPDK_ERRLOG("Delete association failed: %s\n",
			    validation_errors[VERR_NO_ASSOC]);
		return VERR_NO_ASSOC;
	}

	/* create cb context to put in association's list of
	 * callbacks to call when delete association is done */
	opd = calloc(1, sizeof(struct nvmf_fc_ls_op_ctx));
	if (!opd) {
		SPDK_ERRLOG("Mem alloc failed for del assoc cb data");
		return -ENOMEM;
	}

	api_data = &opd->u.del_assoc;
	api_data->assoc = assoc;
	api_data->from_ls_rqst = from_ls_rqst;
	api_data->del_assoc_cb = del_assoc_cb;
	api_data->del_assoc_cb_data = cb_data;
	api_data->args.cb_info.cb_data = opd;
	nvmf_fc_ls_append_del_cb_ctx((struct nvmf_fc_ls_op_ctx **) &assoc->ls_del_op_ctx, opd);

	assoc_state = assoc->assoc_state;
	if (assoc_state == SPDK_NVMF_FC_OBJECT_TO_BE_DELETED) {
		/* association already being deleted */
		return 0;
	}

	/* mark assoc. to be deleted */
	assoc->assoc_state = SPDK_NVMF_FC_OBJECT_TO_BE_DELETED;

	/* delete all of the association's connections */
	TAILQ_FOREACH(fc_conn, &assoc->fc_conns, assoc_link) {
		rc = nvmf_fc_ls_poller_delete_conn(fc_conn, send_abts, NULL, backend_initiated, NULL, NULL);
		if (rc) {
			SPDK_ERRLOG("Delete connection failed for assoc_id 0x%lx conn_id 0x%lx\n",
				    assoc->assoc_id, fc_conn->conn_id);
			return rc;
		}
	}

	return 0;
}

static void
nvmf_fc_ls_disconnect_assoc_cb(void *cb_data, uint32_t err)
{
	struct nvmf_fc_ls_op_ctx *opd = (struct nvmf_fc_ls_op_ctx *)cb_data;
	struct spdk_nvmf_fc_ls_disconn_assoc_api_data *dp = &opd->u.disconn_assoc;
	struct spdk_nvmf_fc_nport *tgtport = dp->tgtport;
	struct spdk_nvmf_fc_ls_rqst *ls_rqst = dp->ls_rqst;

	SPDK_DEBUGLOG(nvmf_fc_ls, "Disconnect association callback begin "
		      "nport %d\n", tgtport->nport_hdl);
	if (err != 0) {
		/* send failure response */
		struct spdk_nvmf_fc_ls_cr_assoc_rqst *rqst =
			(struct spdk_nvmf_fc_ls_cr_assoc_rqst *)ls_rqst->rqstbuf.virt;
		struct spdk_nvmf_fc_ls_cr_assoc_acc *acc =
			(struct spdk_nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
				   FCNVME_MAX_LS_BUFFER_SIZE,
				   rqst->w0.ls_cmd,
				   FCNVME_RJT_RC_UNAB,
				   FCNVME_RJT_EXP_NONE,
				   0);
	}

	nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);

	free(opd);
	SPDK_DEBUGLOG(nvmf_fc_ls, "Disconnect association callback complete "
		      "nport %d err %d\n", tgtport->nport_hdl, err);
}

static void
nvmf_fc_ls_disconnect_assoc(struct spdk_nvmf_fc_nport *tgtport,
			    struct spdk_nvmf_fc_ls_rqst *ls_rqst, uint64_t assoc_id)
{
	struct nvmf_fc_ls_op_ctx *opd;
	struct spdk_nvmf_fc_ls_cr_assoc_rqst *rqst =
		(struct spdk_nvmf_fc_ls_cr_assoc_rqst *)ls_rqst->rqstbuf.virt;
	struct spdk_nvmf_fc_ls_cr_assoc_acc *acc =
		(struct spdk_nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
	struct spdk_nvmf_fc_ls_disconn_assoc_api_data *api_data;
	int ret;
	uint8_t reason = 0;

	opd = calloc(1, sizeof(struct nvmf_fc_ls_op_ctx));
	if (!opd) {
		/* send failure response */
		SPDK_ERRLOG("Allocate disconn assoc op data failed\n");
		reason = FCNVME_RJT_RC_INSUFF_RES;
		goto send_rjt;
	}

	api_data = &opd->u.disconn_assoc;
	api_data->tgtport = tgtport;
	api_data->ls_rqst = ls_rqst;
	ret = _nvmf_fc_delete_association(tgtport, assoc_id,
					  false, false,
					  nvmf_fc_ls_disconnect_assoc_cb,
					  api_data, true);
	if (!ret) {
		return;
	}

	/* delete association failed */
	switch (ret) {
	case VERR_NO_ASSOC:
		reason = FCNVME_RJT_RC_INV_ASSOC;
		break;
	case -ENOMEM:
		reason = FCNVME_RJT_RC_INSUFF_RES;
		break;
	default:
		reason = FCNVME_RJT_RC_LOGIC;
	}

	free(opd);

send_rjt:
	ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
			   FCNVME_MAX_LS_BUFFER_SIZE,
			   rqst->w0.ls_cmd, reason,
			   FCNVME_RJT_EXP_NONE, 0);
	nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
}

static int
nvmf_fc_ls_validate_host(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn)
{

	if (!spdk_nvmf_subsystem_host_allowed(subsystem, hostnqn)) {
		return -EPERM;
	}

	return 0;
}

/* **************************** */
/* LS Request Handler Functions */

static void
nvmf_fc_ls_process_cass(uint32_t s_id,
			struct spdk_nvmf_fc_nport *tgtport,
			struct spdk_nvmf_fc_ls_rqst *ls_rqst)
{
	struct spdk_nvmf_fc_ls_cr_assoc_rqst *rqst =
		(struct spdk_nvmf_fc_ls_cr_assoc_rqst *)ls_rqst->rqstbuf.virt;
	struct spdk_nvmf_fc_ls_cr_assoc_acc *acc =
		(struct spdk_nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
	struct spdk_nvmf_fc_association *assoc;
	struct spdk_nvmf_fc_conn *fc_conn;
	struct spdk_nvmf_subsystem *subsystem = NULL;
	const char *hostnqn = (const char *)rqst->assoc_cmd.hostnqn;
	int errmsg_ind = 0;
	uint8_t rc = FCNVME_RJT_RC_NONE;
	uint8_t ec = FCNVME_RJT_EXP_NONE;
	struct spdk_nvmf_transport *transport = spdk_nvmf_tgt_get_transport(ls_rqst->nvmf_tgt,
						SPDK_NVME_TRANSPORT_NAME_FC);

	SPDK_DEBUGLOG(nvmf_fc_ls,
		      "LS_CASS: ls_rqst_len=%d, desc_list_len=%d, cmd_len=%d, sq_size=%d, "
		      "Subnqn: %s, Hostnqn: %s, Tgtport nn:%lx, pn:%lx\n",
		      ls_rqst->rqst_len, from_be32(&rqst->desc_list_len),
		      from_be32(&rqst->assoc_cmd.desc_len),
		      from_be32(&rqst->assoc_cmd.sqsize),
		      rqst->assoc_cmd.subnqn, hostnqn,
		      tgtport->fc_nodename.u.wwn, tgtport->fc_portname.u.wwn);

	if (ls_rqst->rqst_len < FCNVME_LS_CA_CMD_MIN_LEN) {
		SPDK_ERRLOG("assoc_cmd req len = %d, should be at least %d\n",
			    ls_rqst->rqst_len, FCNVME_LS_CA_CMD_MIN_LEN);
		errmsg_ind = VERR_CR_ASSOC_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (from_be32(&rqst->desc_list_len) <
		   FCNVME_LS_CA_DESC_LIST_MIN_LEN) {
		SPDK_ERRLOG("assoc_cmd desc list len = %d, should be at least %d\n",
			    from_be32(&rqst->desc_list_len),
			    FCNVME_LS_CA_DESC_LIST_MIN_LEN);
		errmsg_ind = VERR_CR_ASSOC_RQST_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->assoc_cmd.desc_tag !=
		   cpu_to_be32(FCNVME_LSDESC_CREATE_ASSOC_CMD)) {
		errmsg_ind = VERR_CR_ASSOC_CMD;
		rc = FCNVME_RJT_RC_INV_PARAM;
	} else if (from_be32(&rqst->assoc_cmd.desc_len) <
		   FCNVME_LS_CA_DESC_MIN_LEN) {
		SPDK_ERRLOG("assoc_cmd desc len = %d, should be at least %d\n",
			    from_be32(&rqst->assoc_cmd.desc_len),
			    FCNVME_LS_CA_DESC_MIN_LEN);
		errmsg_ind = VERR_CR_ASSOC_CMD_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (!rqst->assoc_cmd.ersp_ratio ||
		   (from_be16(&rqst->assoc_cmd.ersp_ratio) >=
		    from_be16(&rqst->assoc_cmd.sqsize))) {
		errmsg_ind = VERR_ERSP_RATIO;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_ESRP;
	} else if (from_be16(&rqst->assoc_cmd.sqsize) == 0 ||
		   from_be16(&rqst->assoc_cmd.sqsize) > transport->opts.max_aq_depth) {
		errmsg_ind = VERR_SQSIZE;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_SQ_SIZE;
	}

	if (rc != FCNVME_RJT_RC_NONE) {
		goto rjt_cass;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(ls_rqst->nvmf_tgt, rqst->assoc_cmd.subnqn);
	if (subsystem == NULL) {
		errmsg_ind = VERR_SUBNQN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_SUBNQN;
		goto rjt_cass;
	}

	if (nvmf_fc_ls_validate_host(subsystem, hostnqn)) {
		errmsg_ind = VERR_HOSTNQN;
		rc = FCNVME_RJT_RC_INV_HOST;
		ec = FCNVME_RJT_EXP_INV_HOSTNQN;
		goto rjt_cass;
	}

	/* get new association */
	assoc = nvmf_fc_ls_new_association(s_id, tgtport, ls_rqst->rport,
					   &rqst->assoc_cmd, subsystem,
					   ls_rqst->rpi, transport);
	if (!assoc) {
		errmsg_ind = VERR_ASSOC_ALLOC_FAIL;
		rc = FCNVME_RJT_RC_INSUFF_RES;
		ec = FCNVME_RJT_EXP_NONE;
		goto rjt_cass;
	}

	/* alloc admin q (i.e. connection) */
	fc_conn = nvmf_fc_ls_new_connection(assoc, 0,
					    from_be16(&rqst->assoc_cmd.ersp_ratio),
					    ls_rqst->rpi,
					    from_be16(&rqst->assoc_cmd.sqsize),
					    tgtport);
	if (!fc_conn) {
		nvmf_fc_ls_free_association(assoc);
		errmsg_ind = VERR_CONN_ALLOC_FAIL;
		rc = FCNVME_RJT_RC_INSUFF_RES;
		ec = FCNVME_RJT_EXP_NONE;
		goto rjt_cass;
	}

	/* format accept response */
	bzero(acc, sizeof(*acc));
	ls_rqst->rsp_len = sizeof(*acc);

	nvmf_fc_ls_format_rsp_hdr(acc, FCNVME_LS_ACC,
				  nvmf_fc_lsdesc_len(
					  sizeof(struct spdk_nvmf_fc_ls_cr_assoc_acc)),
				  FCNVME_LS_CREATE_ASSOCIATION);
	to_be32(&acc->assoc_id.desc_tag, FCNVME_LSDESC_ASSOC_ID);
	acc->assoc_id.desc_len =
		nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_lsdesc_assoc_id));
	to_be32(&acc->conn_id.desc_tag, FCNVME_LSDESC_CONN_ID);
	acc->conn_id.desc_len =
		nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_lsdesc_conn_id));

	/* assign connection to HWQP poller - also sends response */
	nvmf_fc_ls_add_conn_to_poller(assoc, ls_rqst, fc_conn, true);

	return;

rjt_cass:
	SPDK_ERRLOG("Create Association LS failed: %s\n", validation_errors[errmsg_ind]);
	ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc, FCNVME_MAX_LS_BUFFER_SIZE,
			   rqst->w0.ls_cmd, rc, ec, 0);
	nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
}

static void
nvmf_fc_ls_process_cioc(struct spdk_nvmf_fc_nport *tgtport,
			struct spdk_nvmf_fc_ls_rqst *ls_rqst)
{
	struct spdk_nvmf_fc_ls_cr_conn_rqst *rqst =
		(struct spdk_nvmf_fc_ls_cr_conn_rqst *)ls_rqst->rqstbuf.virt;
	struct spdk_nvmf_fc_ls_cr_conn_acc *acc =
		(struct spdk_nvmf_fc_ls_cr_conn_acc *)ls_rqst->rspbuf.virt;
	struct spdk_nvmf_fc_association *assoc;
	struct spdk_nvmf_fc_conn *fc_conn = NULL;
	int errmsg_ind = 0;
	uint8_t rc = FCNVME_RJT_RC_NONE;
	uint8_t ec = FCNVME_RJT_EXP_NONE;
	struct spdk_nvmf_transport *transport = spdk_nvmf_tgt_get_transport(ls_rqst->nvmf_tgt,
						SPDK_NVME_TRANSPORT_NAME_FC);

	SPDK_DEBUGLOG(nvmf_fc_ls,
		      "LS_CIOC: ls_rqst_len=%d, desc_list_len=%d, cmd_len=%d, "
		      "assoc_id=0x%lx, sq_size=%d, esrp=%d, Tgtport nn:%lx, pn:%lx\n",
		      ls_rqst->rqst_len, from_be32(&rqst->desc_list_len),
		      from_be32(&rqst->connect_cmd.desc_len),
		      from_be64(&rqst->assoc_id.association_id),
		      from_be32(&rqst->connect_cmd.sqsize),
		      from_be32(&rqst->connect_cmd.ersp_ratio),
		      tgtport->fc_nodename.u.wwn, tgtport->fc_portname.u.wwn);

	if (ls_rqst->rqst_len < sizeof(struct spdk_nvmf_fc_ls_cr_conn_rqst)) {
		errmsg_ind = VERR_CR_CONN_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->desc_list_len !=
		   nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_ls_cr_conn_rqst))) {
		errmsg_ind = VERR_CR_CONN_RQST_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->assoc_id.desc_tag !=
		   cpu_to_be32(FCNVME_LSDESC_ASSOC_ID)) {
		errmsg_ind = VERR_ASSOC_ID;
		rc = FCNVME_RJT_RC_INV_PARAM;
	} else if (rqst->assoc_id.desc_len !=
		   nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_lsdesc_assoc_id))) {
		errmsg_ind = VERR_ASSOC_ID_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->connect_cmd.desc_tag !=
		   cpu_to_be32(FCNVME_LSDESC_CREATE_CONN_CMD)) {
		errmsg_ind = VERR_CR_CONN_CMD;
		rc = FCNVME_RJT_RC_INV_PARAM;
	} else if (rqst->connect_cmd.desc_len !=
		   nvmf_fc_lsdesc_len(
			   sizeof(struct spdk_nvmf_fc_lsdesc_cr_conn_cmd))) {
		errmsg_ind = VERR_CR_CONN_CMD_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (!rqst->connect_cmd.ersp_ratio ||
		   (from_be16(&rqst->connect_cmd.ersp_ratio) >=
		    from_be16(&rqst->connect_cmd.sqsize))) {
		errmsg_ind = VERR_ERSP_RATIO;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_ESRP;
	} else if (from_be16(&rqst->connect_cmd.sqsize) == 0 ||
		   from_be16(&rqst->connect_cmd.sqsize) > transport->opts.max_queue_depth) {
		errmsg_ind = VERR_SQSIZE;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_SQ_SIZE;
	}

	if (rc != FCNVME_RJT_RC_NONE) {
		goto rjt_cioc;
	}

	/* find association */
	assoc = nvmf_fc_ls_find_assoc(tgtport,
				      from_be64(&rqst->assoc_id.association_id));
	if (!assoc) {
		errmsg_ind = VERR_NO_ASSOC;
		rc = FCNVME_RJT_RC_INV_ASSOC;
	} else if (assoc->assoc_state == SPDK_NVMF_FC_OBJECT_TO_BE_DELETED) {
		/* association is being deleted - don't allow more connections */
		errmsg_ind = VERR_NO_ASSOC;
		rc = FCNVME_RJT_RC_INV_ASSOC;
	} else  if (assoc->conn_count >= transport->opts.max_qpairs_per_ctrlr) {
		errmsg_ind = VERR_CONN_TOO_MANY;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec =  FCNVME_RJT_EXP_INV_Q_ID;
	}

	if (rc != FCNVME_RJT_RC_NONE) {
		goto rjt_cioc;
	}

	fc_conn = nvmf_fc_ls_new_connection(assoc, from_be16(&rqst->connect_cmd.qid),
					    from_be16(&rqst->connect_cmd.ersp_ratio),
					    ls_rqst->rpi,
					    from_be16(&rqst->connect_cmd.sqsize),
					    tgtport);
	if (!fc_conn) {
		errmsg_ind = VERR_CONN_ALLOC_FAIL;
		rc = FCNVME_RJT_RC_INSUFF_RES;
		ec = FCNVME_RJT_EXP_NONE;
		goto rjt_cioc;
	}

	/* format accept response */
	SPDK_DEBUGLOG(nvmf_fc_ls, "Formatting LS accept response for "
		      "assoc_id 0x%lx conn_id 0x%lx\n", assoc->assoc_id,
		      fc_conn->conn_id);
	bzero(acc, sizeof(*acc));
	ls_rqst->rsp_len = sizeof(*acc);
	nvmf_fc_ls_format_rsp_hdr(acc, FCNVME_LS_ACC,
				  nvmf_fc_lsdesc_len(
					  sizeof(struct spdk_nvmf_fc_ls_cr_conn_acc)),
				  FCNVME_LS_CREATE_CONNECTION);
	to_be32(&acc->conn_id.desc_tag, FCNVME_LSDESC_CONN_ID);
	acc->conn_id.desc_len =
		nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_lsdesc_conn_id));

	/* assign connection to HWQP poller - also sends response */
	nvmf_fc_ls_add_conn_to_poller(assoc, ls_rqst, fc_conn, false);

	return;

rjt_cioc:
	SPDK_ERRLOG("Create Connection LS failed: %s\n", validation_errors[errmsg_ind]);

	ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc, FCNVME_MAX_LS_BUFFER_SIZE,
			   rqst->w0.ls_cmd, rc, ec, 0);
	nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
}

static void
nvmf_fc_ls_process_disc(struct spdk_nvmf_fc_nport *tgtport,
			struct spdk_nvmf_fc_ls_rqst *ls_rqst)
{
	struct spdk_nvmf_fc_ls_disconnect_rqst *rqst =
		(struct spdk_nvmf_fc_ls_disconnect_rqst *)ls_rqst->rqstbuf.virt;
	struct spdk_nvmf_fc_ls_disconnect_acc *acc =
		(struct spdk_nvmf_fc_ls_disconnect_acc *)ls_rqst->rspbuf.virt;
	struct spdk_nvmf_fc_association *assoc;
	int errmsg_ind = 0;
	uint8_t rc = FCNVME_RJT_RC_NONE;
	uint8_t ec = FCNVME_RJT_EXP_NONE;

	SPDK_DEBUGLOG(nvmf_fc_ls,
		      "LS_DISC: ls_rqst_len=%d, desc_list_len=%d, cmd_len=%d,"
		      "assoc_id=0x%lx\n",
		      ls_rqst->rqst_len, from_be32(&rqst->desc_list_len),
		      from_be32(&rqst->disconn_cmd.desc_len),
		      from_be64(&rqst->assoc_id.association_id));

	if (ls_rqst->rqst_len < sizeof(struct spdk_nvmf_fc_ls_disconnect_rqst)) {
		errmsg_ind = VERR_DISCONN_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->desc_list_len !=
		   nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_ls_disconnect_rqst))) {
		errmsg_ind = VERR_DISCONN_RQST_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->assoc_id.desc_tag !=
		   cpu_to_be32(FCNVME_LSDESC_ASSOC_ID)) {
		errmsg_ind = VERR_ASSOC_ID;
		rc = FCNVME_RJT_RC_INV_PARAM;
	} else if (rqst->assoc_id.desc_len !=
		   nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_lsdesc_assoc_id))) {
		errmsg_ind = VERR_ASSOC_ID_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->disconn_cmd.desc_tag !=
		   cpu_to_be32(FCNVME_LSDESC_DISCONN_CMD)) {
		rc = FCNVME_RJT_RC_INV_PARAM;
		errmsg_ind = VERR_DISCONN_CMD;
	} else if (rqst->disconn_cmd.desc_len !=
		   nvmf_fc_lsdesc_len(sizeof(struct spdk_nvmf_fc_lsdesc_disconn_cmd))) {
		errmsg_ind = VERR_DISCONN_CMD_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	}

	if (rc != FCNVME_RJT_RC_NONE) {
		goto rjt_disc;
	}

	/* match an active association */
	assoc = nvmf_fc_ls_find_assoc(tgtport,
				      from_be64(&rqst->assoc_id.association_id));
	if (!assoc) {
		errmsg_ind = VERR_NO_ASSOC;
		rc = FCNVME_RJT_RC_INV_ASSOC;
		goto rjt_disc;
	}

	/* format response */
	bzero(acc, sizeof(*acc));
	ls_rqst->rsp_len = sizeof(*acc);

	nvmf_fc_ls_format_rsp_hdr(acc, FCNVME_LS_ACC,
				  nvmf_fc_lsdesc_len(
					  sizeof(struct spdk_nvmf_fc_ls_disconnect_acc)),
				  FCNVME_LS_DISCONNECT);

	nvmf_fc_ls_disconnect_assoc(tgtport, ls_rqst, assoc->assoc_id);
	return;

rjt_disc:
	SPDK_ERRLOG("Disconnect LS failed: %s\n", validation_errors[errmsg_ind]);
	ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc, FCNVME_MAX_LS_BUFFER_SIZE,
			   rqst->w0.ls_cmd, rc, ec, 0);
	nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
}

/* ************************ */
/* external functions       */

void
nvmf_fc_ls_init(struct spdk_nvmf_fc_port *fc_port)
{
}

void
nvmf_fc_ls_fini(struct spdk_nvmf_fc_port *fc_port)
{
}

void
nvmf_fc_handle_ls_rqst(struct spdk_nvmf_fc_ls_rqst *ls_rqst)
{
	struct spdk_nvmf_fc_ls_rqst_w0 *w0 =
		(struct spdk_nvmf_fc_ls_rqst_w0 *)ls_rqst->rqstbuf.virt;
	uint32_t s_id = ls_rqst->s_id;
	struct spdk_nvmf_fc_nport *tgtport = ls_rqst->nport;

	SPDK_DEBUGLOG(nvmf_fc_ls, "LS cmd=%d\n", w0->ls_cmd);

	switch (w0->ls_cmd) {
	case FCNVME_LS_CREATE_ASSOCIATION:
		nvmf_fc_ls_process_cass(s_id, tgtport, ls_rqst);
		break;
	case FCNVME_LS_CREATE_CONNECTION:
		nvmf_fc_ls_process_cioc(tgtport, ls_rqst);
		break;
	case FCNVME_LS_DISCONNECT:
		nvmf_fc_ls_process_disc(tgtport, ls_rqst);
		break;
	default:
		SPDK_ERRLOG("Invalid LS cmd=%d\n", w0->ls_cmd);
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(ls_rqst->rspbuf.virt,
				   FCNVME_MAX_LS_BUFFER_SIZE, w0->ls_cmd,
				   FCNVME_RJT_RC_INVAL, FCNVME_RJT_EXP_NONE, 0);
		nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	}
}

int
nvmf_fc_delete_association(struct spdk_nvmf_fc_nport *tgtport,
			   uint64_t assoc_id, bool send_abts, bool backend_initiated,
			   spdk_nvmf_fc_del_assoc_cb del_assoc_cb,
			   void *cb_data)
{
	return _nvmf_fc_delete_association(tgtport, assoc_id, send_abts, backend_initiated,
					   del_assoc_cb, cb_data, false);
}

int
nvmf_fc_delete_connection(struct spdk_nvmf_fc_conn *fc_conn, bool send_abts,
			  bool backend_initiated, spdk_nvmf_fc_del_conn_cb cb_fn,
			  void *cb_data)
{
	return nvmf_fc_ls_poller_delete_conn(fc_conn, send_abts, NULL,
					     backend_initiated, cb_fn, cb_data);
}


static void
nvmf_fc_poller_api_cb_event(void *arg)
{
	struct spdk_nvmf_fc_poller_api_cb_info *cb_info =
		(struct spdk_nvmf_fc_poller_api_cb_info *) arg;

	assert(cb_info != NULL);
	cb_info->cb_func(cb_info->cb_data, cb_info->ret);
}

static void
nvmf_fc_poller_api_perform_cb(struct spdk_nvmf_fc_poller_api_cb_info *cb_info,
			      enum spdk_nvmf_fc_poller_api_ret ret)
{
	if (cb_info->cb_func && cb_info->cb_thread) {
		cb_info->ret = ret;
		/* callback to main thread */
		spdk_thread_send_msg(cb_info->cb_thread, nvmf_fc_poller_api_cb_event,
				     (void *) cb_info);
	}
}

static int
nvmf_fc_poller_add_conn_lookup_data(struct spdk_nvmf_fc_hwqp *hwqp,
				    struct spdk_nvmf_fc_conn *fc_conn)
{
	int rc = -1;
	struct spdk_nvmf_fc_hwqp_rport *rport = NULL;

	/* Add connection based lookup entry. */
	rc = rte_hash_add_key_data(hwqp->connection_list_hash,
				   (void *)&fc_conn->conn_id, (void *)fc_conn);

	if (rc < 0) {
		SPDK_ERRLOG("Failed to add connection hash entry\n");
		return rc;
	}

	/* RPI based lookup */
	if (rte_hash_lookup_data(hwqp->rport_list_hash, (void *)&fc_conn->rpi, (void **)&rport) < 0) {
		rport = calloc(1, sizeof(struct spdk_nvmf_fc_hwqp_rport));
		if (!rport) {
			SPDK_ERRLOG("Failed to allocate rport entry\n");
			rc = -ENOMEM;
			goto del_conn_hash;
		}

		/* Add rport table entry */
		rc = rte_hash_add_key_data(hwqp->rport_list_hash,
					   (void *)&fc_conn->rpi, (void *)rport);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to add rport hash entry\n");
			goto del_rport;
		}
		TAILQ_INIT(&rport->conn_list);
	}

	/* Add to rport conn list */
	TAILQ_INSERT_TAIL(&rport->conn_list, fc_conn, rport_link);
	return 0;

del_rport:
	free(rport);
del_conn_hash:
	rte_hash_del_key(hwqp->connection_list_hash, (void *)&fc_conn->conn_id);
	return rc;
}

static void
nvmf_fc_poller_del_conn_lookup_data(struct spdk_nvmf_fc_hwqp *hwqp,
				    struct spdk_nvmf_fc_conn *fc_conn)
{
	struct spdk_nvmf_fc_hwqp_rport *rport = NULL;

	if (rte_hash_del_key(hwqp->connection_list_hash, (void *)&fc_conn->conn_id) < 0) {
		SPDK_ERRLOG("Failed to del connection(%lx) hash entry\n",
			    fc_conn->conn_id);
	}

	if (rte_hash_lookup_data(hwqp->rport_list_hash, (void *)&fc_conn->rpi, (void **)&rport) >= 0) {
		TAILQ_REMOVE(&rport->conn_list, fc_conn, rport_link);

		/* If last conn del rpi hash */
		if (TAILQ_EMPTY(&rport->conn_list)) {
			if (rte_hash_del_key(hwqp->rport_list_hash, (void *)&fc_conn->rpi) < 0) {
				SPDK_ERRLOG("Failed to del rpi(%lx) hash entry\n",
					    fc_conn->conn_id);
			}
			free(rport);
		}
	} else {
		SPDK_ERRLOG("RPI(%d) hash entry not found\n", fc_conn->rpi);
	}
}

static struct spdk_nvmf_fc_request *
nvmf_fc_poller_rpi_find_req(struct spdk_nvmf_fc_hwqp *hwqp, uint16_t rpi, uint16_t oxid)
{
	struct spdk_nvmf_fc_request *fc_req = NULL;
	struct spdk_nvmf_fc_conn *fc_conn;
	struct spdk_nvmf_fc_hwqp_rport *rport = NULL;

	if (rte_hash_lookup_data(hwqp->rport_list_hash, (void *)&rpi, (void **)&rport) >= 0) {
		TAILQ_FOREACH(fc_conn, &rport->conn_list, rport_link) {
			TAILQ_FOREACH(fc_req, &fc_conn->in_use_reqs, conn_link) {
				if (fc_req->oxid == oxid) {
					return fc_req;
				}
			}
		}
	}
	return NULL;
}

static void
nvmf_fc_poller_api_add_connection(void *arg)
{
	enum spdk_nvmf_fc_poller_api_ret ret = SPDK_NVMF_FC_POLLER_API_SUCCESS;
	struct spdk_nvmf_fc_poller_api_add_connection_args *conn_args =
		(struct spdk_nvmf_fc_poller_api_add_connection_args *)arg;
	struct spdk_nvmf_fc_conn *fc_conn = conn_args->fc_conn, *tmp;

	SPDK_DEBUGLOG(nvmf_fc_poller_api, "Poller add connection, conn_id 0x%lx\n",
		      fc_conn->conn_id);

	/* make sure connection is not already in poller's list */
	if (rte_hash_lookup_data(fc_conn->hwqp->connection_list_hash,
				 (void *)&fc_conn->conn_id, (void **)&tmp) >= 0) {
		SPDK_ERRLOG("duplicate connection found");
		ret = SPDK_NVMF_FC_POLLER_API_DUP_CONN_ID;
	} else {
		if (nvmf_fc_poller_add_conn_lookup_data(fc_conn->hwqp, fc_conn)) {
			SPDK_ERRLOG("Failed to add connection 0x%lx\n", fc_conn->conn_id);
			ret = SPDK_NVMF_FC_POLLER_API_ERROR;
		} else {
			SPDK_DEBUGLOG(nvmf_fc_poller_api, "conn_id=%lx", fc_conn->conn_id);
			fc_conn->hwqp->num_conns++;
		}
	}

	/* perform callback */
	nvmf_fc_poller_api_perform_cb(&conn_args->cb_info, ret);
}

static void
nvmf_fc_poller_api_quiesce_queue(void *arg)
{
	struct spdk_nvmf_fc_poller_api_quiesce_queue_args *q_args =
		(struct spdk_nvmf_fc_poller_api_quiesce_queue_args *) arg;
	struct spdk_nvmf_fc_request *fc_req = NULL, *tmp;

	/* should be already, but make sure queue is quiesced */
	q_args->hwqp->state = SPDK_FC_HWQP_OFFLINE;

	/*
	 * Kill all the outstanding commands that are in the transfer state and
	 * in the process of being aborted.
	 * We can run into this situation if an adapter reset happens when an I_T Nexus delete
	 * is in progress.
	 */
	TAILQ_FOREACH_SAFE(fc_req, &q_args->hwqp->in_use_reqs, link, tmp) {
		if (nvmf_fc_req_in_xfer(fc_req) && fc_req->is_aborted == true) {
			nvmf_fc_poller_api_func(q_args->hwqp, SPDK_NVMF_FC_POLLER_API_REQ_ABORT_COMPLETE,
						(void *)fc_req);
		}
	}

	/* perform callback */
	nvmf_fc_poller_api_perform_cb(&q_args->cb_info, SPDK_NVMF_FC_POLLER_API_SUCCESS);
}

static void
nvmf_fc_poller_api_activate_queue(void *arg)
{
	struct spdk_nvmf_fc_poller_api_quiesce_queue_args *q_args =
		(struct spdk_nvmf_fc_poller_api_quiesce_queue_args *) arg;

	q_args->hwqp->state = SPDK_FC_HWQP_ONLINE;

	/* perform callback */
	nvmf_fc_poller_api_perform_cb(&q_args->cb_info, 0);
}

static void
nvmf_fc_disconnect_qpair_cb(void *ctx)
{
	struct spdk_nvmf_fc_poller_api_cb_info *cb_info = ctx;
	/* perform callback */
	nvmf_fc_poller_api_perform_cb(cb_info, SPDK_NVMF_FC_POLLER_API_SUCCESS);
}

static void
nvmf_fc_poller_conn_abort_done(void *hwqp, int32_t status, void *cb_args)
{
	struct spdk_nvmf_fc_poller_api_del_connection_args *conn_args = cb_args;

	if (conn_args->fc_request_cnt) {
		conn_args->fc_request_cnt -= 1;
	}

	if (!conn_args->fc_request_cnt) {
		struct spdk_nvmf_fc_conn *fc_conn = conn_args->fc_conn, *tmp;

		if (rte_hash_lookup_data(conn_args->hwqp->connection_list_hash,
					 (void *)&fc_conn->conn_id, (void *)&tmp) >= 0) {
			/* All the requests for this connection are aborted. */
			nvmf_fc_poller_del_conn_lookup_data(conn_args->hwqp, fc_conn);
			fc_conn->hwqp->num_conns--;

			SPDK_DEBUGLOG(nvmf_fc_poller_api, "Connection deleted, conn_id 0x%lx\n", fc_conn->conn_id);

			if (!conn_args->backend_initiated && (fc_conn->qpair.state != SPDK_NVMF_QPAIR_DEACTIVATING)) {
				/* disconnect qpair from nvmf controller */
				spdk_nvmf_qpair_disconnect(&fc_conn->qpair,
							   nvmf_fc_disconnect_qpair_cb, &conn_args->cb_info);
			} else {
				nvmf_fc_poller_api_perform_cb(&conn_args->cb_info, SPDK_NVMF_FC_POLLER_API_SUCCESS);
			}
		} else {
			/*
			 * Duplicate connection delete can happen if one is
			 * coming in via an association disconnect and the other
			 * is initiated by a port reset.
			 */
			SPDK_DEBUGLOG(nvmf_fc_poller_api, "Duplicate conn delete.");
			/* perform callback */
			nvmf_fc_poller_api_perform_cb(&conn_args->cb_info, SPDK_NVMF_FC_POLLER_API_SUCCESS);
		}
	}
}

static void
nvmf_fc_poller_api_del_connection(void *arg)
{
	struct spdk_nvmf_fc_poller_api_del_connection_args *conn_args =
		(struct spdk_nvmf_fc_poller_api_del_connection_args *)arg;
	struct spdk_nvmf_fc_conn *fc_conn = NULL;
	struct spdk_nvmf_fc_request *fc_req = NULL, *tmp;
	struct spdk_nvmf_fc_hwqp *hwqp = conn_args->hwqp;

	SPDK_DEBUGLOG(nvmf_fc_poller_api, "Poller delete connection, conn_id 0x%lx\n",
		      fc_conn->conn_id);

	/* Make sure connection is valid */
	if (rte_hash_lookup_data(hwqp->connection_list_hash,
				 (void *)&conn_args->fc_conn->conn_id, (void **)&fc_conn) < 0) {
		/* perform callback */
		nvmf_fc_poller_api_perform_cb(&conn_args->cb_info, SPDK_NVMF_FC_POLLER_API_NO_CONN_ID);
		return;
	}

	conn_args->fc_request_cnt = 0;

	TAILQ_FOREACH_SAFE(fc_req, &fc_conn->in_use_reqs, conn_link, tmp) {
		if (nvmf_qpair_is_admin_queue(&fc_conn->qpair) &&
		    (fc_req->req.cmd->nvme_cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST)) {
			/* AER will be cleaned by spdk_nvmf_qpair_disconnect. */
			continue;
		}

		conn_args->fc_request_cnt += 1;
		nvmf_fc_request_abort(fc_req, conn_args->send_abts,
				      nvmf_fc_poller_conn_abort_done,
				      conn_args);
	}

	if (!conn_args->fc_request_cnt) {
		SPDK_DEBUGLOG(nvmf_fc_poller_api, "Connection deleted.\n");
		nvmf_fc_poller_del_conn_lookup_data(conn_args->hwqp, conn_args->fc_conn);
		hwqp->num_conns--;

		if (!conn_args->backend_initiated && (fc_conn->qpair.state != SPDK_NVMF_QPAIR_DEACTIVATING)) {
			/* disconnect qpair from nvmf controller */
			spdk_nvmf_qpair_disconnect(&fc_conn->qpair, nvmf_fc_disconnect_qpair_cb,
						   &conn_args->cb_info);
		} else {
			nvmf_fc_poller_api_perform_cb(&conn_args->cb_info, SPDK_NVMF_FC_POLLER_API_SUCCESS);
		}
	}
}

static void
nvmf_fc_poller_abts_done(void *hwqp, int32_t status, void *cb_args)
{
	struct spdk_nvmf_fc_poller_api_abts_recvd_args *args = cb_args;

	SPDK_DEBUGLOG(nvmf_fc_poller_api,
		      "ABTS poller done, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
		      args->ctx->rpi, args->ctx->oxid, args->ctx->rxid);

	nvmf_fc_poller_api_perform_cb(&args->cb_info,
				      SPDK_NVMF_FC_POLLER_API_SUCCESS);
}

static void
nvmf_fc_poller_api_abts_received(void *arg)
{
	struct spdk_nvmf_fc_poller_api_abts_recvd_args *args = arg;
	struct spdk_nvmf_fc_request *fc_req;

	fc_req = nvmf_fc_poller_rpi_find_req(args->hwqp, args->ctx->rpi, args->ctx->oxid);
	if (fc_req) {
		nvmf_fc_request_abort(fc_req, false, nvmf_fc_poller_abts_done, args);
		return;
	}

	nvmf_fc_poller_api_perform_cb(&args->cb_info,
				      SPDK_NVMF_FC_POLLER_API_OXID_NOT_FOUND);
}

static void
nvmf_fc_poller_api_queue_sync(void *arg)
{
	struct spdk_nvmf_fc_poller_api_queue_sync_args *args = arg;

	SPDK_DEBUGLOG(nvmf_fc_poller_api,
		      "HWQP sync requested for u_id = 0x%lx\n", args->u_id);

	/* Add this args to hwqp sync_cb list */
	TAILQ_INSERT_TAIL(&args->hwqp->sync_cbs, args, link);
}

static void
nvmf_fc_poller_api_queue_sync_done(void *arg)
{
	struct spdk_nvmf_fc_poller_api_queue_sync_done_args *args = arg;
	struct spdk_nvmf_fc_hwqp *hwqp = args->hwqp;
	uint64_t tag = args->tag;
	struct spdk_nvmf_fc_poller_api_queue_sync_args *sync_args = NULL, *tmp = NULL;

	assert(args != NULL);

	TAILQ_FOREACH_SAFE(sync_args, &hwqp->sync_cbs, link, tmp) {
		if (sync_args->u_id == tag) {
			/* Queue successfully synced. Remove from cb list */
			TAILQ_REMOVE(&hwqp->sync_cbs, sync_args, link);

			SPDK_DEBUGLOG(nvmf_fc_poller_api,
				      "HWQP sync done for u_id = 0x%lx\n", sync_args->u_id);

			/* Return the status to poller */
			nvmf_fc_poller_api_perform_cb(&sync_args->cb_info,
						      SPDK_NVMF_FC_POLLER_API_SUCCESS);
			return;
		}
	}

	free(arg);
	/* note: no callback from this api */
}

static void
nvmf_fc_poller_api_add_hwqp(void *arg)
{
	struct spdk_nvmf_fc_hwqp *hwqp = (struct spdk_nvmf_fc_hwqp *)arg;
	struct spdk_nvmf_fc_poll_group *fgroup = hwqp->fgroup;

	assert(fgroup);

	if (nvmf_fc_poll_group_valid(fgroup)) {
		TAILQ_INSERT_TAIL(&fgroup->hwqp_list, hwqp, link);
		hwqp->lcore_id	= spdk_env_get_current_core();
	}
	/* note: no callback from this api */
}

static void
nvmf_fc_poller_api_remove_hwqp(void *arg)
{
	struct spdk_nvmf_fc_poller_api_remove_hwqp_args *args = arg;
	struct spdk_nvmf_fc_hwqp *hwqp = args->hwqp;
	struct spdk_nvmf_fc_poll_group *fgroup = hwqp->fgroup;

	if (nvmf_fc_poll_group_valid(fgroup)) {
		TAILQ_REMOVE(&fgroup->hwqp_list, hwqp, link);
	}
	hwqp->fgroup = NULL;
	hwqp->thread = NULL;

	nvmf_fc_poller_api_perform_cb(&args->cb_info, SPDK_NVMF_FC_POLLER_API_SUCCESS);
}

enum spdk_nvmf_fc_poller_api_ret
nvmf_fc_poller_api_func(struct spdk_nvmf_fc_hwqp *hwqp, enum spdk_nvmf_fc_poller_api api,
			void *api_args) {
	switch (api)
	{
	case SPDK_NVMF_FC_POLLER_API_ADD_CONNECTION:
				spdk_thread_send_msg(hwqp->thread,
						     nvmf_fc_poller_api_add_connection, api_args);
		break;

	case SPDK_NVMF_FC_POLLER_API_DEL_CONNECTION:
		spdk_thread_send_msg(hwqp->thread,
				     nvmf_fc_poller_api_del_connection, api_args);
		break;

	case SPDK_NVMF_FC_POLLER_API_QUIESCE_QUEUE:
		/* quiesce q polling now, don't wait for poller to do it */
		hwqp->state = SPDK_FC_HWQP_OFFLINE;
		spdk_thread_send_msg(hwqp->thread,
				     nvmf_fc_poller_api_quiesce_queue, api_args);
		break;

	case SPDK_NVMF_FC_POLLER_API_ACTIVATE_QUEUE:
		spdk_thread_send_msg(hwqp->thread,
				     nvmf_fc_poller_api_activate_queue, api_args);
		break;

	case SPDK_NVMF_FC_POLLER_API_ABTS_RECEIVED:
		spdk_thread_send_msg(hwqp->thread,
				     nvmf_fc_poller_api_abts_received, api_args);
		break;

	case SPDK_NVMF_FC_POLLER_API_REQ_ABORT_COMPLETE:
		spdk_thread_send_msg(hwqp->thread,
				     nvmf_fc_request_abort_complete, api_args);
		break;

	case SPDK_NVMF_FC_POLLER_API_QUEUE_SYNC:
		spdk_thread_send_msg(hwqp->thread,
				     nvmf_fc_poller_api_queue_sync, api_args);
		break;

	case SPDK_NVMF_FC_POLLER_API_QUEUE_SYNC_DONE:
		spdk_thread_send_msg(hwqp->thread,
				     nvmf_fc_poller_api_queue_sync_done, api_args);
		break;

	case SPDK_NVMF_FC_POLLER_API_ADD_HWQP:
		spdk_thread_send_msg(hwqp->thread, nvmf_fc_poller_api_add_hwqp, (void *) hwqp);
		break;

	case SPDK_NVMF_FC_POLLER_API_REMOVE_HWQP:
		spdk_thread_send_msg(hwqp->thread, nvmf_fc_poller_api_remove_hwqp, api_args);
		break;

	case SPDK_NVMF_FC_POLLER_API_ADAPTER_EVENT:
	case SPDK_NVMF_FC_POLLER_API_AEN:
	default:
		SPDK_ERRLOG("BAD ARG!");
		return SPDK_NVMF_FC_POLLER_API_INVALID_ARG;
	}

	return SPDK_NVMF_FC_POLLER_API_SUCCESS;
}

SPDK_LOG_REGISTER_COMPONENT(nvmf_fc_poller_api)
SPDK_LOG_REGISTER_COMPONENT(nvmf_fc_ls)
