/*
 *   BSD LICENSE
 *
 *   Copyright (c) 2018 Broadcom.  All Rights Reserved.
 *   The term "Broadcom" refers to Broadcom Limited and/or its subsidiaries.
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
#include "spdk_internal/log.h"
#include "nvmf_internal.h"
#include "transport.h"

#include "nvmf_fc.h"

/* set to 1 to send ls disconnect in response to ls disconnect from host (per standard) */
#define NVMF_FC_LS_SEND_LS_DISCONNECT 1

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

/* Poller API structures (arguments and callback data */

struct spdk_nvmf_fc_ls_add_conn_api_data {
	struct spdk_nvmf_fc_poller_api_add_connection_args args;
	struct spdk_nvmf_fc_ls_rqst *ls_rqst;
	struct spdk_nvmf_fc_association *assoc;
	bool assoc_conn; /* true if adding connection for new association */
};

/* Disconnect (connection) request functions */
struct spdk_nvmf_fc_ls_del_conn_api_data {
	struct spdk_nvmf_fc_poller_api_del_connection_args args;
	struct spdk_nvmf_fc_association *assoc;
	struct spdk_nvmf_fc_ls_rqst *ls_rqst;
	bool assoc_conn; /* true if deleting AQ connection */
};

/* used by LS disconnect association cmd handling */
struct spdk_nvmf_fc_ls_disconn_assoc_api_data {
	struct spdk_nvmf_fc_nport *tgtport;
	struct spdk_nvmf_fc_ls_rqst *ls_rqst;
};

/* used by delete association call */
struct spdk_nvmf_fc_delete_assoc_api_data {
	struct spdk_nvmf_fc_poller_api_del_connection_args args;
	struct spdk_nvmf_fc_association *assoc;
	bool from_ls_rqst;   /* true = request came for LS */
	spdk_nvmf_fc_del_assoc_cb del_assoc_cb;
	void *del_assoc_cb_data;
};

struct nvmf_fc_ls_op_ctx {
	union {
		struct spdk_nvmf_fc_ls_add_conn_api_data add_conn;
		struct spdk_nvmf_fc_ls_del_conn_api_data del_conn;
		struct spdk_nvmf_fc_ls_disconn_assoc_api_data disconn_assoc;
		struct spdk_nvmf_fc_delete_assoc_api_data del_assoc;
	} u;
	struct  nvmf_fc_ls_op_ctx *next_op_ctx;
};

#define be32_to_cpu(i) from_be32((i))
#define be16_to_cpu(i) from_be16((i))
#define be64_to_cpu(i) from_be64((i))

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

static inline int
nvmf_fc_xmt_ls_rsp(struct spdk_nvmf_fc_nport *tgtport,
		   struct spdk_nvmf_fc_ls_rqst *ls_rqst)
{
	return spdk_nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
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

/* ************************************************** */
/* Allocators/Deallocators (assocations, connections, */
/* poller API data)                                   */

static inline void
nvmf_fc_ls_free_association(struct spdk_nvmf_fc_association *assoc)
{
	struct spdk_nvmf_fc_conn *fc_conn;

	/* return slots for aq conn */
	spdk_nvmf_fc_lld_ops.release_conn(assoc->aq_conn->hwqp, assoc->aq_conn->conn_id,
					  assoc->aq_conn->max_queue_depth);

	/* return the q slots of the io conns for the association */
	TAILQ_FOREACH(fc_conn, &assoc->avail_fc_conns, assoc_avail_link) {
		spdk_nvmf_fc_lld_ops.release_conn(fc_conn->hwqp, fc_conn->conn_id,
						  fc_conn->max_queue_depth);

	}

	/* free assocation's send disconnect buffer */
	if (assoc->snd_disconn_bufs) {
		spdk_nvmf_fc_lld_ops.free_srsr_bufs(assoc->snd_disconn_bufs);
	}

	/* free assocation's connections */
	free((void *)assoc->conns_buf);

	/* free asosciation's host */
	free((void *) assoc->host->nqn);
	free((void *) assoc->host);

	/* free the association */
	free((void *)assoc);
}

static int
nvmf_fc_ls_alloc_assign_connections(struct spdk_nvmf_fc_association *assoc,
				    struct spdk_nvmf_host *host,
				    struct spdk_nvmf_transport *nvmf_transport)
{
	uint32_t i;
	struct spdk_nvmf_fc_conn *fc_conn;
	struct spdk_nvmf_fc_port *fc_port = assoc->tgtport->fc_port;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS, "Pre-alloc %d qpairs for host %s\n",
		      nvmf_transport->opts.max_qpairs_per_ctrlr, host->nqn);

	/* allocate memory for all connections at once */
	assoc->conns_buf = calloc(1, (nvmf_transport->opts.max_qpairs_per_ctrlr) *
				  sizeof(struct spdk_nvmf_fc_conn));

	if (assoc->conns_buf == NULL) {
		SPDK_ERRLOG("Out of memory for connections for new association\n");
		return -1;
	}

	/* admin queue connection gets first connection */
	assoc->aq_conn = assoc->conns_buf;
	/* initialize admin queue connection */
	assoc->aq_conn->qpair.transport = nvmf_transport;
	assoc->aq_conn->max_queue_depth = nvmf_transport->opts.max_aq_depth;
	assoc->aq_conn->fc_assoc = assoc;
	/* assign admin queue to hwqp */
	assoc->aq_conn->hwqp = spdk_nvmf_fc_lld_ops.assign_conn_to_hwqp(
				       fc_port->io_queues,
				       fc_port->num_io_queues,
				       &assoc->aq_conn->conn_id,
				       nvmf_transport->opts.max_aq_depth,
				       true);

	for (i = 1; i < nvmf_transport->opts.max_qpairs_per_ctrlr; i++) {
		fc_conn = assoc->conns_buf + (i * sizeof(struct spdk_nvmf_fc_conn));
		fc_conn->qpair.state = SPDK_NVMF_QPAIR_UNINITIALIZED;
		fc_conn->qpair.transport = nvmf_transport;
		TAILQ_INIT(&fc_conn->qpair.outstanding);
		fc_conn->max_queue_depth = nvmf_transport->opts.max_queue_depth;
		fc_conn->fc_assoc = assoc;
		fc_conn->hwqp = spdk_nvmf_fc_lld_ops.assign_conn_to_hwqp(
					fc_port->io_queues,
					fc_port->num_io_queues,
					&fc_conn->conn_id,
					nvmf_transport->opts.max_queue_depth,
					false);
		if (fc_conn->hwqp == NULL) {
			/* failed to assign connection a hwqp - cleanup and get out */
			nvmf_fc_ls_free_association(assoc);
			SPDK_ERRLOG("Could not allocate hwqp's for new assocation\n");
			return -1;
		}

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
			   struct spdk_nvmf_host *host, uint16_t rpi,
			   struct spdk_nvmf_transport *nvmf_transport)
{
	struct spdk_nvmf_fc_association *assoc;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS,
		      "New Association request for port %d nport %d rpi 0x%x\n",
		      tgtport->fc_port->port_hdl, tgtport->nport_hdl, rpi);

	assert(rport);
	if (!rport) {
		SPDK_ERRLOG("rport is null.\n");
		return NULL;
	}

	assoc = (struct spdk_nvmf_fc_association *)
		calloc(1, sizeof(struct spdk_nvmf_fc_association));

	if (assoc) {
		/* initialize association */
#if (NVMF_FC_LS_SEND_LS_DISCONNECT == 1)
		/* allocate buffers to send LS disconnect command to host */
		assoc->snd_disconn_bufs = spdk_nvmf_fc_lld_ops.alloc_srsr_bufs(
						  sizeof(struct spdk_nvmf_fc_ls_disconnect_rqst),
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
		assoc->assoc_state = SPDK_NVMF_FC_OBJECT_CREATED;
		memcpy(assoc->host_id, a_cmd->hostid, FCNVME_ASSOC_HOSTID_LEN);
		memcpy(assoc->host_nqn, a_cmd->hostnqn, FCNVME_ASSOC_HOSTNQN_LEN);
		memcpy(assoc->sub_nqn, a_cmd->subnqn, FCNVME_ASSOC_HOSTNQN_LEN);

		TAILQ_INIT(&assoc->fc_conns);
		TAILQ_INIT(&assoc->avail_fc_conns);
		assoc->ls_del_op_ctx = NULL;

		/* Back pointer to host specific controller configuration. */
		assoc->host = host;

		/* allocate and assign connections for association */
		if (nvmf_fc_ls_alloc_assign_connections(assoc, host, nvmf_transport) == 0) {
			/* add association to target port's association list */
			TAILQ_INSERT_TAIL(&tgtport->fc_associations, assoc, link);
			tgtport->assoc_count++;
			rport->assoc_count++;
		} else {  /* failed to pre-allocate/assign connections */
			return NULL;
		}
	} else {
		SPDK_ERRLOG("out of memory for new association\n");
	}

	return assoc;
}

static inline void
nvmf_fc_ls_append_del_cb_ctx(struct spdk_nvmf_fc_association *assoc,
			     struct nvmf_fc_ls_op_ctx *opd)
{
	/* append to delete assoc callback list */
	if (!assoc->ls_del_op_ctx) {
		assoc->ls_del_op_ctx = (void *)opd;
	} else {
		struct nvmf_fc_ls_op_ctx *nxt =
			(struct nvmf_fc_ls_op_ctx *) assoc->ls_del_op_ctx;
		while (nxt->next_op_ctx) {
			nxt = nxt->next_op_ctx;
		}
		nxt->next_op_ctx = opd;
	}
}

static struct spdk_nvmf_fc_conn *
nvmf_fc_ls_new_connection(struct spdk_nvmf_fc_association *assoc,
			  struct spdk_nvmf_host *host, uint16_t qid,
			  uint16_t esrp_ratio, uint16_t rpi, uint16_t sq_size,
			  struct spdk_nvmf_fc_nport *tgtport)
{
	struct spdk_nvmf_fc_conn *fc_conn;

	fc_conn = qid == 0 ? assoc->aq_conn : TAILQ_FIRST(&assoc->avail_fc_conns);
	if (fc_conn) {
		if (qid != 0) {
			TAILQ_REMOVE(&assoc->avail_fc_conns, fc_conn, assoc_avail_link);
		}
		fc_conn->qpair.qid = qid;
		fc_conn->qpair.sq_head_max = sq_size;
		fc_conn->esrp_ratio = esrp_ratio;
		fc_conn->fc_assoc = assoc;
		fc_conn->rpi = rpi;
		fc_conn->max_queue_depth = sq_size + 1;

		/* save target port trid in connection (for subsystem
		 * listener validation in fabric connect command) */
		spdk_nvmf_fc_create_trid(&fc_conn->trid, tgtport->fc_nodename.u.wwn,
					 tgtport->fc_portname.u.wwn);

		TAILQ_INIT(&fc_conn->pending_queue);
	} else {
		SPDK_ERRLOG("out of connections for association %p\n", assoc);
	}

	return fc_conn;
}

static inline void
nvmf_fc_ls_free_connection(struct spdk_nvmf_fc_conn *fc_conn)
{
	if (fc_conn->qpair.qid != 0) {
		TAILQ_INSERT_TAIL(&fc_conn->fc_assoc->avail_fc_conns, fc_conn, assoc_avail_link);
	}
}

static inline struct nvmf_fc_ls_op_ctx *
nvmf_fc_ls_new_op_ctx(void)
{
	return (struct nvmf_fc_ls_op_ctx *)
	       calloc(1, sizeof(struct nvmf_fc_ls_op_ctx));
}

static inline void
nvmf_fc_ls_free_op_ctx(struct nvmf_fc_ls_op_ctx *ctx_ptr)
{
	free((void *)ctx_ptr);
}

/* End - Allocators/Deallocators (assocations, connections, */
/*       poller API data)                                   */
/* ******************************************************** */

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
nvmf_fc_del_assoc_from_tgt_port(struct spdk_nvmf_fc_association *assoc)
{
	struct spdk_nvmf_fc_nport *tgtport = assoc->tgtport;
	TAILQ_REMOVE(&tgtport->fc_associations, assoc, link);
	tgtport->assoc_count--;
	assoc->rport->assoc_count--;
}

static void
nvmf_fc_ls_rsp_fail_del_conn_cb(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret)
{
	struct nvmf_fc_ls_op_ctx *opd =
		(struct nvmf_fc_ls_op_ctx *)cb_data;
	struct spdk_nvmf_fc_ls_del_conn_api_data *dp = &opd->u.del_conn;
	struct spdk_nvmf_fc_association *assoc = dp->assoc;
	struct spdk_nvmf_fc_conn *fc_conn = dp->args.fc_conn;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS, "Delete Connection callback "
		      "for assoc_id 0x%lx conn_id 0x%lx\n", assoc->assoc_id,
		      fc_conn->conn_id);

	if (dp->assoc_conn) {
		/* delete association */
		nvmf_fc_del_assoc_from_tgt_port(assoc);
		nvmf_fc_ls_free_association(assoc);
	} else {
		/* remove connection from association's connection list */
		TAILQ_REMOVE(&assoc->fc_conns, fc_conn, assoc_link);
		nvmf_fc_ls_free_connection(fc_conn);
	}

	nvmf_fc_ls_free_op_ctx(opd);
}

static void
nvmf_fc_handle_xmt_ls_rsp_failure(struct spdk_nvmf_fc_association *assoc,
				  struct spdk_nvmf_fc_conn *fc_conn,
				  bool assoc_conn)
{
	struct spdk_nvmf_fc_ls_del_conn_api_data *api_data;
	struct nvmf_fc_ls_op_ctx *opd = NULL;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS, "Transmit LS response failure "
		      "for assoc_id 0x%lx conn_id 0x%lx\n", assoc->assoc_id,
		      fc_conn->conn_id);


	/* create context for delete connection API */
	opd = nvmf_fc_ls_new_op_ctx();
	if (!opd) { /* hopefully this doesn't happen - if so, we leak the connection */
		SPDK_ERRLOG("Mem alloc failed for del conn op data");
		return;
	}

	api_data = &opd->u.del_conn;
	api_data->assoc = assoc;
	api_data->ls_rqst = NULL;
	api_data->assoc_conn = assoc_conn;
	api_data->args.fc_conn = fc_conn;
	api_data->args.send_abts = false;
	api_data->args.hwqp = fc_conn->hwqp;
	api_data->args.cb_info.cb_thread = spdk_get_thread();
	api_data->args.cb_info.cb_func = nvmf_fc_ls_rsp_fail_del_conn_cb;
	api_data->args.cb_info.cb_data = opd;

	spdk_nvmf_fc_poller_api_func(api_data->args.hwqp,
				     SPDK_NVMF_FC_POLLER_API_DEL_CONNECTION,
				     &api_data->args);
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

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS,
		      "add_conn_cb: assoc_id = 0x%lx, conn_id = 0x%lx\n",
		      assoc->assoc_id, fc_conn->conn_id);

	if (assoc->assoc_state == SPDK_NVMF_FC_OBJECT_TO_BE_DELETED) {
		/* association is already being deleted - don't continue */
		nvmf_fc_ls_free_op_ctx(opd);
		return;
	}

	if (dp->assoc_conn) {
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
			    dp->assoc_conn ? "association" : "connection");
		nvmf_fc_handle_xmt_ls_rsp_failure(assoc, fc_conn,
						  dp->assoc_conn);
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS,
			      "LS response (conn_id 0x%lx) sent\n", fc_conn->conn_id);
	}

	nvmf_fc_ls_free_op_ctx(opd);
}

static void
nvmf_fc_ls_add_conn_to_poller(
	struct spdk_nvmf_fc_association *assoc,
	struct spdk_nvmf_fc_ls_rqst *ls_rqst,
	struct spdk_nvmf_fc_conn *fc_conn,
	bool assoc_conn)
{
	struct nvmf_fc_ls_op_ctx *opd = nvmf_fc_ls_new_op_ctx();
	struct spdk_nvmf_fc_nport *tgtport = assoc->tgtport;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS, "Add Connection to poller for "
		      "assoc_id 0x%lx conn_id 0x%lx\n", assoc->assoc_id,
		      fc_conn->conn_id);

	if (opd) {
		/* insert conn in association's connection list */
		struct spdk_nvmf_fc_ls_add_conn_api_data *api_data = &opd->u.add_conn;
		TAILQ_INSERT_TAIL(&assoc->fc_conns, fc_conn, assoc_link);
		assoc->conn_count++;

		if (assoc_conn) {
			/* assign association ID to aq's connection id */
			assoc->assoc_id = fc_conn->conn_id;
		}

		api_data->args.fc_conn = fc_conn;
		api_data->args.cb_info.cb_thread = spdk_get_thread();
		api_data->args.cb_info.cb_func = nvmf_fc_ls_add_conn_cb;
		api_data->args.cb_info.cb_data = (void *)opd;
		api_data->assoc = assoc;
		api_data->ls_rqst = ls_rqst;
		api_data->assoc_conn = assoc_conn;
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS,
			      "Add connection API called for conn_id = 0x%lx\n",
			      fc_conn->conn_id);
		spdk_nvmf_fc_poller_api_func(api_data->args.fc_conn->hwqp,
					     SPDK_NVMF_FC_POLLER_API_ADD_CONNECTION,
					     &api_data->args);
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS, "Add connection API returned "
			      "for conn_id = 0x%lx\n", fc_conn->conn_id);
		return;
	}

	/* send failure response */
	struct spdk_nvmf_fc_ls_cr_assoc_rqst *rqst =
		(struct spdk_nvmf_fc_ls_cr_assoc_rqst *)ls_rqst->rqstbuf.virt;
	struct spdk_nvmf_fc_ls_cr_assoc_acc *acc =
		(struct spdk_nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
	SPDK_ERRLOG(opd ?
		    "failed to find hwqp that could fit requested sq size\n" :
		    "allocate api data for add conn op failed\n");
	ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
			   FCNVME_MAX_LS_BUFFER_SIZE, rqst->w0.ls_cmd,
			   FCNVME_RJT_RC_INSUFF_RES,
			   FCNVME_RJT_EXP_NONE, 0);
	nvmf_fc_ls_free_connection(fc_conn);
	if (assoc_conn) {
		nvmf_fc_del_assoc_from_tgt_port(assoc);
		nvmf_fc_ls_free_association(assoc);
	}
	(void)nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	if (opd) {
		nvmf_fc_ls_free_op_ctx(opd);
	}
}

/* Delete association functions */

static void
nvmf_fc_do_del_assoc_cbs(struct nvmf_fc_ls_op_ctx *opd, int ret)
{
	while (opd) {
		struct nvmf_fc_ls_op_ctx *nxt = opd->next_op_ctx;
		struct spdk_nvmf_fc_delete_assoc_api_data *dp = &opd->u.del_assoc;

		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS, "performing delete assoc. callback\n");
		dp->del_assoc_cb(dp->del_assoc_cb_data, ret);
		nvmf_fc_ls_free_op_ctx(opd);
		opd = nxt;
	}
}

static void
nvmf_fs_send_ls_disconnect_cb(void *hwqp, int32_t status, void *args)
{
	if (args) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS, "free disconnect buffers\n");
		spdk_nvmf_fc_lld_ops.free_srsr_bufs((struct spdk_nvmf_fc_srsr_bufs *)args);
	}
}

static void
nvmf_fc_del_all_conns_cb(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret)
{
	struct nvmf_fc_ls_op_ctx *opd = (struct nvmf_fc_ls_op_ctx *)cb_data;
	struct spdk_nvmf_fc_delete_assoc_api_data *dp = &opd->u.del_assoc;
	struct spdk_nvmf_fc_association *assoc = dp->assoc;
	struct spdk_nvmf_fc_conn *fc_conn = dp->args.fc_conn;

	/* Assumption here is that there will be no error (i.e. ret=success).
	 * Since connections are deleted in parallel, nothing can be
	 * done anyway if there is an error because we need to complete
	 * all connection deletes and callback to caller */

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS,
		      "Delete all connections for assoc_id 0x%lx, conn_id = %lx\n",
		      assoc->assoc_id, fc_conn->conn_id);

	/* remove connection from association's connection list */
	TAILQ_REMOVE(&assoc->fc_conns, fc_conn, assoc_link);
	nvmf_fc_ls_free_connection(fc_conn);

	if (--assoc->conn_count == 0) {
		/* last connection - remove association from target port's association list */
		struct nvmf_fc_ls_op_ctx *cb_opd = (struct nvmf_fc_ls_op_ctx *)assoc->ls_del_op_ctx;

		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS,
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

			SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS, "Send LS disconnect\n");
			if (spdk_nvmf_fc_xmt_srsr_req(&assoc->tgtport->fc_port->ls_queue,
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

	nvmf_fc_ls_free_op_ctx(opd);
}

static void
nvmf_fc_kill_io_del_all_conns_cb(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret)
{
	struct nvmf_fc_ls_op_ctx *opd =
		(struct nvmf_fc_ls_op_ctx *)cb_data;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS, "Callback after killing outstanding ABTS.");
	/*
	 * NOTE: We should not access any connection or association related data
	 * structures here.
	 */
	nvmf_fc_ls_free_op_ctx(opd);
}


/* Disconnect/delete (association) request functions */

static
int
nvmf_fc_delete_association(struct spdk_nvmf_fc_nport *tgtport,
			   uint64_t assoc_id, bool send_abts,
			   spdk_nvmf_fc_del_assoc_cb del_assoc_cb,
			   void *cb_data, bool from_ls_rqst)
{

	struct nvmf_fc_ls_op_ctx *opd, *opd_tail, *opd_head = NULL;
	struct spdk_nvmf_fc_delete_assoc_api_data *api_data;
	struct spdk_nvmf_fc_conn *fc_conn;
	struct spdk_nvmf_fc_association *assoc =
		nvmf_fc_ls_find_assoc(tgtport, assoc_id);
	struct spdk_nvmf_fc_port *fc_port = tgtport->fc_port;
	enum spdk_nvmf_fc_object_state assoc_state;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS, "Delete association, "
		      "assoc_id 0x%lx\n", assoc_id);

	if (!assoc) {
		SPDK_ERRLOG("Delete association failed: %s\n",
			    validation_errors[VERR_NO_ASSOC]);
		return VERR_NO_ASSOC;
	}

	/* create cb context to put in association's list of
	 * callbacks to call when delete association is done */
	opd = nvmf_fc_ls_new_op_ctx();
	if (!opd) {
		SPDK_ERRLOG("Mem alloc failed for del assoc cb data");
		return ENOMEM;
	}

	api_data = &opd->u.del_assoc;
	api_data->assoc = assoc;
	api_data->from_ls_rqst = from_ls_rqst;
	api_data->del_assoc_cb = del_assoc_cb;
	api_data->del_assoc_cb_data = cb_data;
	api_data->args.cb_info.cb_data = opd;
	nvmf_fc_ls_append_del_cb_ctx(assoc, opd);

	assoc_state = assoc->assoc_state;
	if ((assoc_state == SPDK_NVMF_FC_OBJECT_TO_BE_DELETED) &&
	    (fc_port->hw_port_status != SPDK_FC_PORT_QUIESCED)) {
		/* association already being deleted */
		return 0;
	}

	/* mark assoc. to be deleted */
	assoc->assoc_state = SPDK_NVMF_FC_OBJECT_TO_BE_DELETED;

	/* create a list of all connection to delete */
	TAILQ_FOREACH(fc_conn, &assoc->fc_conns, assoc_link) {
		opd = nvmf_fc_ls_new_op_ctx();
		if (!opd) { /* hopefully this doesn't happen */
			SPDK_ERRLOG("Mem alloc failed for del conn op data");
			while (opd_head) { /* free any contexts already allocated */
				opd = opd_head;
				opd_head = opd->next_op_ctx;
				nvmf_fc_ls_free_op_ctx(opd);
			}
			return ENOMEM;
		}


		api_data = &opd->u.del_assoc;
		api_data->args.fc_conn = fc_conn;
		api_data->assoc = assoc;
		api_data->args.send_abts = send_abts;
		api_data->args.hwqp = spdk_nvmf_fc_lld_ops.get_hwqp_from_conn_id(
					      assoc->tgtport->fc_port->io_queues,
					      assoc->tgtport->fc_port->num_io_queues,
					      fc_conn->conn_id);
		api_data->args.cb_info.cb_thread = spdk_get_thread();
		if ((fc_port->hw_port_status == SPDK_FC_PORT_QUIESCED) &&
		    (assoc_state == SPDK_NVMF_FC_OBJECT_TO_BE_DELETED)) {
			/*
			 * If there are any connections deletes or IO abts that are
			 * stuck because of firmware reset, a second invocation of
			 * SPDK_NVMF_FC_POLLER_API_DEL_CONNECTION will result in
			 * outstanding connections & requests being killed and
			 * their corresponding callbacks being executed.
			 */
			api_data->args.cb_info.cb_func = nvmf_fc_kill_io_del_all_conns_cb;
		} else {
			api_data->args.cb_info.cb_func = nvmf_fc_del_all_conns_cb;
		}
		api_data->args.cb_info.cb_data = opd;
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS,
			      "conn_id = %lx\n", fc_conn->conn_id);

		if (!opd_head) {
			opd_head = opd;
		} else {
			opd_tail->next_op_ctx = opd;
		}
		opd_tail = opd;
	}

	/* make poller api calls to delete connetions */
	while (opd_head) {
		opd = opd_head;
		opd_head = opd->next_op_ctx;
		api_data = &opd->u.del_assoc;
		spdk_nvmf_fc_poller_api_func(api_data->args.hwqp,
					     SPDK_NVMF_FC_POLLER_API_DEL_CONNECTION,
					     &api_data->args);
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

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS, "Disconnect association callback begin "
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

	(void)nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);

	nvmf_fc_ls_free_op_ctx(opd);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS, "Disconnect association callback complete "
		      "nport %d err %d\n", tgtport->nport_hdl, err);
}

static void
nvmf_fc_ls_disconnect_assoc(struct spdk_nvmf_fc_nport *tgtport,
			    struct spdk_nvmf_fc_ls_rqst *ls_rqst, uint64_t assoc_id)
{
	struct nvmf_fc_ls_op_ctx *opd = nvmf_fc_ls_new_op_ctx();

	if (opd) {
		struct spdk_nvmf_fc_ls_disconn_assoc_api_data *api_data =
				&opd->u.disconn_assoc;
		int ret;

		api_data->tgtport = tgtport;
		api_data->ls_rqst = ls_rqst;
		ret = nvmf_fc_delete_association(tgtport, assoc_id,
						 false,
						 nvmf_fc_ls_disconnect_assoc_cb,
						 api_data, true);

		if (ret != 0) {
			/* delete association failed */
			struct spdk_nvmf_fc_ls_cr_assoc_rqst *rqst =
				(struct spdk_nvmf_fc_ls_cr_assoc_rqst *)
				ls_rqst->rqstbuf.virt;
			struct spdk_nvmf_fc_ls_cr_assoc_acc *acc =
				(struct spdk_nvmf_fc_ls_cr_assoc_acc *)
				ls_rqst->rspbuf.virt;
			ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
					   FCNVME_MAX_LS_BUFFER_SIZE,
					   rqst->w0.ls_cmd,
					   ret == VERR_NO_ASSOC ?
					   FCNVME_RJT_RC_INV_ASSOC :
					   ret == ENOMEM ?
					   FCNVME_RJT_RC_INSUFF_RES :
					   FCNVME_RJT_RC_LOGIC,
					   FCNVME_RJT_EXP_NONE, 0);
			(void)nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
		}
	} else {
		/* send failure response */
		struct spdk_nvmf_fc_ls_cr_assoc_rqst *rqst =
			(struct spdk_nvmf_fc_ls_cr_assoc_rqst *)ls_rqst->rqstbuf.virt;
		struct spdk_nvmf_fc_ls_cr_assoc_acc *acc =
			(struct spdk_nvmf_fc_ls_cr_assoc_acc *)ls_rqst->rspbuf.virt;
		SPDK_ERRLOG("Allocate disconn assoc op data failed\n");
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
				   FCNVME_MAX_LS_BUFFER_SIZE,
				   rqst->w0.ls_cmd,
				   FCNVME_RJT_RC_INSUFF_RES,
				   FCNVME_RJT_EXP_NONE, 0);
		(void)nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	}
}

static struct spdk_nvmf_host *
nvmf_fc_ls_dup_host(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn,
		    struct spdk_nvmf_transport *transport, int *rslt)
{
	struct spdk_nvmf_host *host;

	*rslt = 0;

	if (!spdk_nvmf_subsystem_host_allowed(subsystem, hostnqn)) {
		*rslt =  -1;
		return NULL;
	}

	host = calloc(1, sizeof(struct spdk_nvmf_host));
	if (host == NULL) {
		*rslt = -ENOMEM;
		return NULL;
	}
	host->nqn = strdup(hostnqn);
	if (!host->nqn) {
		free(host);
		*rslt = -ENOMEM;
		return NULL;
	}

	return host;
}

/* **************************** */
/* LS Reqeust Handler Functions */

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
	struct spdk_nvmf_host *host = NULL;
	int errmsg_ind = 0;
	uint8_t rc = FCNVME_RJT_RC_NONE;
	uint8_t ec = FCNVME_RJT_EXP_NONE;
	struct spdk_nvmf_transport *transport = spdk_nvmf_tgt_get_transport(ls_rqst->nvmf_tgt,
						(enum spdk_nvme_transport_type) SPDK_NVMF_TRTYPE_FC);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS,
		      "LS_CASS: ls_rqst_len=%d, desc_list_len=%d, cmd_len=%d, sq_size=%d, "
		      "Subnqn: %s, Hostnqn: %s, Tgtport nn:%lx, pn:%lx\n",
		      ls_rqst->rqst_len, be32_to_cpu(&rqst->desc_list_len),
		      be32_to_cpu(&rqst->assoc_cmd.desc_len),
		      be16_to_cpu(&rqst->assoc_cmd.sqsize),
		      rqst->assoc_cmd.subnqn, rqst->assoc_cmd.hostnqn,
		      tgtport->fc_nodename.u.wwn, tgtport->fc_portname.u.wwn);

	if (ls_rqst->rqst_len < FCNVME_LS_CA_CMD_MIN_LEN) {
		SPDK_ERRLOG("assoc_cmd req len = %d, should be at least %d\n",
			    ls_rqst->rqst_len, FCNVME_LS_CA_CMD_MIN_LEN);
		errmsg_ind = VERR_CR_ASSOC_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (be32_to_cpu(&rqst->desc_list_len) <
		   FCNVME_LS_CA_DESC_LIST_MIN_LEN) {
		SPDK_ERRLOG("assoc_cmd desc list len = %d, should be at least %d\n",
			    be32_to_cpu(&rqst->desc_list_len),
			    FCNVME_LS_CA_DESC_LIST_MIN_LEN);
		errmsg_ind = VERR_CR_ASSOC_RQST_LEN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_LEN;
	} else if (rqst->assoc_cmd.desc_tag !=
		   cpu_to_be32(FCNVME_LSDESC_CREATE_ASSOC_CMD)) {
		errmsg_ind = VERR_CR_ASSOC_CMD;
		rc = FCNVME_RJT_RC_INV_PARAM;
	} else if (be32_to_cpu(&rqst->assoc_cmd.desc_len) <
		   FCNVME_LS_CA_DESC_MIN_LEN) {
		SPDK_ERRLOG("assoc_cmd desc len = %d, should be at least %d\n",
			    be32_to_cpu(&rqst->assoc_cmd.desc_len),
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
	} else if ((subsystem = spdk_nvmf_tgt_find_subsystem(ls_rqst->nvmf_tgt, rqst->assoc_cmd.subnqn))
		   == NULL) {
		errmsg_ind = VERR_SUBNQN;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_INV_SUBNQN;
	} else if ((host = nvmf_fc_ls_dup_host(subsystem, (const char *) rqst->assoc_cmd.hostnqn,
					       transport, &errmsg_ind)) == NULL) {
		if (errmsg_ind == -ENOMEM) {
			SPDK_ERRLOG("Failed to alloc host entry for association\n");
			errmsg_ind = VERR_ASSOC_ALLOC_FAIL;
			rc = FCNVME_RJT_RC_INSUFF_RES;
		} else {
			errmsg_ind = VERR_HOSTNQN;
			rc = FCNVME_RJT_RC_INV_HOST;
			ec = FCNVME_RJT_EXP_INV_HOSTNQN;
		}
	} else if (from_be16(&rqst->assoc_cmd.sqsize) == 0 ||
		   from_be16(&rqst->assoc_cmd.sqsize) >= transport->opts.max_aq_depth) {
		errmsg_ind = VERR_SQSIZE;
		rc = FCNVME_RJT_RC_INV_PARAM;
		ec = FCNVME_RJT_EXP_SQ_SIZE;
	} else {
		/* get new association */
		assoc = nvmf_fc_ls_new_association(s_id, tgtport, ls_rqst->rport,
						   &rqst->assoc_cmd,
						   subsystem, host,
						   ls_rqst->rpi,
						   transport);
		if (!assoc) {
			errmsg_ind = VERR_ASSOC_ALLOC_FAIL;
			rc = FCNVME_RJT_RC_INSUFF_RES;
			ec = FCNVME_RJT_EXP_NONE;
		} else { /* alloc admin q (i.e. connection) */
			fc_conn = nvmf_fc_ls_new_connection(assoc, host, 0,
							    from_be16(&rqst->assoc_cmd.ersp_ratio),
							    ls_rqst->rpi,
							    from_be16(&rqst->assoc_cmd.sqsize),
							    tgtport);
			if (!fc_conn) {
				nvmf_fc_ls_free_association(assoc);
				errmsg_ind = VERR_CONN_ALLOC_FAIL;
				rc = FCNVME_RJT_RC_INSUFF_RES;
				ec = FCNVME_RJT_EXP_NONE;
			}
		}
	}

	if (rc != FCNVME_RJT_RC_NONE) {
		SPDK_ERRLOG("Create Association LS failed: %s\n",
			    validation_errors[errmsg_ind]);
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
				   FCNVME_MAX_LS_BUFFER_SIZE,
				   rqst->w0.ls_cmd, rc,
				   ec, 0);
		(void)nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	}

	else {
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
	}
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
						(enum spdk_nvme_transport_type) SPDK_NVMF_TRTYPE_FC);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS,
		      "LS_CIOC: ls_rqst_len=%d, desc_list_len=%d, cmd_len=%d, "
		      "assoc_id=0x%lx, sq_size=%d, esrp=%d, Tgtport nn:%lx, pn:%lx\n",
		      ls_rqst->rqst_len, be32_to_cpu(&rqst->desc_list_len),
		      be32_to_cpu(&rqst->connect_cmd.desc_len),
		      be64_to_cpu(&rqst->assoc_id.association_id),
		      be16_to_cpu(&rqst->connect_cmd.sqsize),
		      be16_to_cpu(&rqst->connect_cmd.ersp_ratio),
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
	} else {
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
		} else if (from_be16(&rqst->connect_cmd.sqsize) == 0 ||
			   from_be16(&rqst->connect_cmd.sqsize) >= transport->opts.max_queue_depth) {
			errmsg_ind = VERR_SQSIZE;
			rc = FCNVME_RJT_RC_INV_PARAM;
			ec = FCNVME_RJT_EXP_SQ_SIZE;
		} else {
			fc_conn = nvmf_fc_ls_new_connection(assoc, assoc->host,
							    from_be16(&rqst->connect_cmd.qid),
							    from_be16(&rqst->connect_cmd.ersp_ratio),
							    ls_rqst->rpi,
							    from_be16(&rqst->connect_cmd.sqsize),
							    tgtport);
			if (!fc_conn) {
				errmsg_ind = VERR_CONN_ALLOC_FAIL;
				rc = FCNVME_RJT_RC_INSUFF_RES;
				ec = FCNVME_RJT_EXP_NONE;
			}
		}
	}

	if (rc != FCNVME_RJT_RC_NONE) {
		SPDK_ERRLOG("Create Connection LS failed: %s\n",
			    validation_errors[errmsg_ind]);
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
				   FCNVME_MAX_LS_BUFFER_SIZE,
				   rqst->w0.ls_cmd,
				   rc, ec, 0);
		(void)nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	} else {
		/* format accept response */
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS, "Formatting LS accept response for "
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
			nvmf_fc_lsdesc_len(
				sizeof(struct spdk_nvmf_fc_lsdesc_conn_id));

		/* assign connection to HWQP poller - also sends response */
		nvmf_fc_ls_add_conn_to_poller(assoc, ls_rqst, fc_conn, false);
	}
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

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS,
		      "LS_DISC: ls_rqst_len=%d, desc_list_len=%d, cmd_len=%d,"
		      "assoc_id=0x%lx\n",
		      ls_rqst->rqst_len, be32_to_cpu(&rqst->desc_list_len),
		      be32_to_cpu(&rqst->disconn_cmd.desc_len),
		      be64_to_cpu(&rqst->assoc_id.association_id));

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
	} else {
		/* match an active association */
		assoc = nvmf_fc_ls_find_assoc(tgtport,
					      from_be64(&rqst->assoc_id.association_id));
		if (!assoc) {
			errmsg_ind = VERR_NO_ASSOC;
			rc = FCNVME_RJT_RC_INV_ASSOC;
		}
	}

	if (rc != FCNVME_RJT_RC_NONE) {
		SPDK_ERRLOG("Disconnect LS failed: %s\n",
			    validation_errors[errmsg_ind]);
		ls_rqst->rsp_len = nvmf_fc_ls_format_rjt(acc,
				   FCNVME_MAX_LS_BUFFER_SIZE,
				   rqst->w0.ls_cmd,
				   rc, ec, 0);
		(void)nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	}

	else {
		/* format response */
		bzero(acc, sizeof(*acc));
		ls_rqst->rsp_len = sizeof(*acc);

		nvmf_fc_ls_format_rsp_hdr(acc, FCNVME_LS_ACC,
					  nvmf_fc_lsdesc_len(
						  sizeof(struct spdk_nvmf_fc_ls_disconnect_acc)),
					  FCNVME_LS_DISCONNECT);

		nvmf_fc_ls_disconnect_assoc(tgtport, ls_rqst, assoc->assoc_id);
	}
}

/* ************************ */
/* external functions       */

void
spdk_nvmf_fc_ls_init(struct spdk_nvmf_fc_port *fc_port)
{
}

void
spdk_nvmf_fc_ls_fini(struct spdk_nvmf_fc_port *fc_port)
{
}

void
spdk_nvmf_fc_handle_ls_rqst(struct spdk_nvmf_fc_ls_rqst *ls_rqst)
{
	struct spdk_nvmf_fc_ls_rqst_w0 *w0 =
		(struct spdk_nvmf_fc_ls_rqst_w0 *)ls_rqst->rqstbuf.virt;
	uint32_t s_id = ls_rqst->s_id;
	struct spdk_nvmf_fc_nport *tgtport = ls_rqst->nport;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_LS, "LS cmd=%d\n", w0->ls_cmd);

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
		(void)nvmf_fc_xmt_ls_rsp(tgtport, ls_rqst);
	}
}

int
spdk_nvmf_fc_delete_association(struct spdk_nvmf_fc_nport *tgtport,
				uint64_t assoc_id, bool send_abts,
				spdk_nvmf_fc_del_assoc_cb del_assoc_cb,
				void *cb_data)
{
	return nvmf_fc_delete_association(tgtport, assoc_id, send_abts,
					  del_assoc_cb, cb_data, false);
}

SPDK_LOG_REGISTER_COMPONENT("nvmf_fc_ls", SPDK_LOG_NVMF_FC_LS)
