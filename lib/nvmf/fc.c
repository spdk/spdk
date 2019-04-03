/*
 *   BSD LICENSE
 *
 *   Copyright (c) 2018 Broadcom.  All Rights Reserved.
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

/*
 * NVMe_FC transport functions.
 */

#include "spdk/env.h"
#include "spdk/assert.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/util.h"
#include "spdk/event.h"
#include "spdk/likely.h"
#include "spdk/endian.h"
#include "spdk/log.h"
#include "spdk/io_channel.h"
#include "spdk_internal/log.h"
#include "nvmf_internal.h"
#include "transport.h"
#include "nvmf_fc.h"

#include <rte_config.h>
#include <rte_mempool.h>

/*
 * PRLI service parameters
 */
enum spdk_nvmf_fc_service_parameters {
	SPDK_NVMF_FC_FIRST_BURST_SUPPORTED = 0x0001,
	SPDK_NVMF_FC_DISCOVERY_SERVICE = 0x0008,
	SPDK_NVMF_FC_TARGET_FUNCTION = 0x0010,
	SPDK_NVMF_FC_INITIATOR_FUNCTION = 0x0020,
	SPDK_NVMF_FC_CONFIRMED_COMPLETION_SUPPORTED = 0x0080,
};

static char *fc_req_state_strs[] = {
	"SPDK_NVMF_FC_REQ_INIT",
	"SPDK_NVMF_FC_REQ_READ_BDEV",
	"SPDK_NVMF_FC_REQ_READ_XFER",
	"SPDK_NVMF_FC_REQ_READ_RSP",
	"SPDK_NVMF_FC_REQ_WRITE_BUFFS",
	"SPDK_NVMF_FC_REQ_WRITE_XFER",
	"SPDK_NVMF_FC_REQ_WRITE_BDEV",
	"SPDK_NVMF_FC_REQ_WRITE_RSP",
	"SPDK_NVMF_FC_REQ_NONE_BDEV",
	"SPDK_NVMF_FC_REQ_NONE_RSP",
	"SPDK_NVMF_FC_REQ_SUCCESS",
	"SPDK_NVMF_FC_REQ_FAILED",
	"SPDK_NVMF_FC_REQ_ABORTED",
	"SPDK_NVMF_FC_REQ_BDEV_ABORTED",
	"SPDK_NVMF_FC_REQ_PENDING"
};


struct spdk_nvmf_fc_transport {
	struct spdk_nvmf_transport transport;
	struct spdk_mempool *data_buff_pool;
};

static	struct spdk_nvmf_fc_transport *g_nvmf_fc_transport;

static TAILQ_HEAD(, spdk_nvmf_fc_port) g_spdk_nvmf_fc_port_list =
	TAILQ_HEAD_INITIALIZER(g_spdk_nvmf_fc_port_list);

static struct spdk_thread *g_nvmf_fc_master_thread = NULL;

static uint32_t g_nvmf_fc_poll_group_count = 0;
static TAILQ_HEAD(, spdk_nvmf_fc_poll_group) g_nvmf_fc_poll_groups =
	TAILQ_HEAD_INITIALIZER(g_nvmf_fc_poll_groups);

struct spdk_thread *
spdk_nvmf_fc_get_master_thread(void)
{
	return g_nvmf_fc_master_thread;
}

static inline void
nvmf_fc_record_req_trace_point(struct spdk_nvmf_fc_request *fc_req,
			       enum spdk_nvmf_fc_request_state state)
{
	uint16_t tpoint_id = SPDK_TRACE_MAX_TPOINT_ID;

	switch (state) {
	case SPDK_NVMF_FC_REQ_INIT:
		/* Start IO tracing */
		spdk_trace_record(TRACE_NVMF_IO_START, fc_req->poller_lcore,
				  0, (uint64_t)(&fc_req->req), 0);
		tpoint_id = TRACE_FC_REQ_INIT;
		break;
	case SPDK_NVMF_FC_REQ_READ_BDEV:
		tpoint_id = TRACE_FC_REQ_READ_BDEV;
		break;
	case SPDK_NVMF_FC_REQ_READ_XFER:
		tpoint_id = TRACE_FC_REQ_READ_XFER;
		break;
	case SPDK_NVMF_FC_REQ_READ_RSP:
		tpoint_id = TRACE_FC_REQ_READ_RSP;
		break;
	case SPDK_NVMF_FC_REQ_WRITE_BUFFS:
		tpoint_id = TRACE_FC_REQ_WRITE_BUFFS;
		break;
	case SPDK_NVMF_FC_REQ_WRITE_XFER:
		tpoint_id = TRACE_FC_REQ_WRITE_XFER;
		break;
	case SPDK_NVMF_FC_REQ_WRITE_BDEV:
		tpoint_id = TRACE_FC_REQ_WRITE_BDEV;
		break;
	case SPDK_NVMF_FC_REQ_WRITE_RSP:
		tpoint_id = TRACE_FC_REQ_WRITE_RSP;
		break;
	case SPDK_NVMF_FC_REQ_NONE_BDEV:
		tpoint_id = TRACE_FC_REQ_NONE_BDEV;
		break;
	case SPDK_NVMF_FC_REQ_NONE_RSP:
		tpoint_id = TRACE_FC_REQ_NONE_RSP;
		break;
	case SPDK_NVMF_FC_REQ_SUCCESS:
		tpoint_id = TRACE_FC_REQ_SUCCESS;
		break;
	case SPDK_NVMF_FC_REQ_FAILED:
		tpoint_id = TRACE_FC_REQ_FAILED;
		break;
	case SPDK_NVMF_FC_REQ_ABORTED:
		tpoint_id = TRACE_FC_REQ_ABORTED;
		break;
	case SPDK_NVMF_FC_REQ_BDEV_ABORTED:
		tpoint_id = TRACE_FC_REQ_ABORTED;
		break;
	case SPDK_NVMF_FC_REQ_PENDING:
		tpoint_id = TRACE_FC_REQ_PENDING;
		break;
	default:
		assert(0);
		break;
	}
	if (tpoint_id != SPDK_TRACE_MAX_TPOINT_ID) {
		fc_req->req.req_state_trace[state] = spdk_get_ticks();
		spdk_trace_record(tpoint_id, fc_req->poller_lcore, 0,
				  (uint64_t)(&fc_req->req), 0);
	}
}

static int
nvmf_fc_create_req_mempool(struct spdk_nvmf_fc_hwqp *hwqp)
{
	if (hwqp->fc_request_pool == NULL) {
		char name[48];
		static int unique_number = 0;

		unique_number++;

		/* Name should be unique, otherwise API fails. */
		snprintf(name, sizeof(name), "NVMF_FC_REQ_POOL:%d", unique_number);
		hwqp->fc_request_pool = spdk_mempool_create(name,
					hwqp->rq_size,
					sizeof(struct spdk_nvmf_fc_request),
					0, SPDK_ENV_SOCKET_ID_ANY);

		if (hwqp->fc_request_pool == NULL) {
			SPDK_ERRLOG("create fc request pool failed\n");
			return -1;
		}
		TAILQ_INIT(&hwqp->in_use_reqs);
	}
	return 0;
}

static inline struct spdk_nvmf_fc_request *
nvmf_fc_alloc_req_buf(struct spdk_nvmf_fc_hwqp *hwqp)
{
	struct spdk_nvmf_fc_request *fc_req;

	fc_req = (struct spdk_nvmf_fc_request *)spdk_mempool_get(hwqp->fc_request_pool);
	if (!fc_req) {
		SPDK_ERRLOG("Alloc request buffer failed\n");
		return NULL;
	}

	memset(fc_req, 0, sizeof(struct spdk_nvmf_fc_request));
	TAILQ_INSERT_TAIL(&hwqp->in_use_reqs, fc_req, link);
	TAILQ_INIT(&fc_req->abort_cbs);
	return fc_req;
}

static inline void
nvmf_fc_free_req_buf(struct spdk_nvmf_fc_hwqp *hwqp, struct spdk_nvmf_fc_request *fc_req)
{
	if (fc_req->state != SPDK_NVMF_FC_REQ_SUCCESS) {
		/* Log an error for debug purpose. */
		spdk_nvmf_fc_req_set_state(fc_req, SPDK_NVMF_FC_REQ_FAILED);
	}

	/* set the magic to mark req as no longer valid. */
	fc_req->magic = 0xDEADBEEF;

	TAILQ_REMOVE(&hwqp->in_use_reqs, fc_req, link);
	spdk_mempool_put(hwqp->fc_request_pool, (void *)fc_req);
}

static inline bool
nvmf_fc_req_in_get_buff(struct spdk_nvmf_fc_request *fc_req)
{
	switch (fc_req->state) {
	case SPDK_NVMF_FC_REQ_WRITE_BUFFS:
		return true;
	default:
		return false;
	}
}

static void
nvmf_fc_release_io_buff(struct spdk_nvmf_fc_request *fc_req)
{

	if (fc_req->data_from_pool) {
		for (uint32_t i = 0; i < fc_req->req.iovcnt; i++) {
			spdk_mempool_put(fc_req->hwqp->fc_poll_group->fc_transport->data_buff_pool,
					 fc_req->buffers[i]);
			fc_req->req.iov[i].iov_base = NULL;
			fc_req->buffers[i] = NULL;
		}
		fc_req->data_from_pool = false;
	}
	fc_req->req.data = NULL;
	fc_req->req.iovcnt  = 0;
}

void
spdk_nvmf_fc_init_poller_queues(struct spdk_nvmf_fc_hwqp *hwqp)
{
	spdk_nvmf_fc_lld_ops.init_q_buffers(hwqp);
}

void
spdk_nvmf_fc_reinit_poller_queues(struct spdk_nvmf_fc_hwqp *hwqp, void *queues_curr)
{
	struct spdk_nvmf_fc_abts_ctx *ctx;
	struct spdk_nvmf_fc_poller_api_queue_sync_args *args = NULL, *tmp = NULL;

	/* Clean up any pending sync callbacks */
	TAILQ_FOREACH_SAFE(args, &hwqp->sync_cbs, link, tmp) {
		TAILQ_REMOVE(&hwqp->sync_cbs, args, link);
		ctx = args->cb_info.cb_data;
		if (ctx) {
			if (++ctx->hwqps_responded == ctx->num_hwqps) {
				if (ctx->sync_poller_args) {
					free(ctx->sync_poller_args);
				}
				if (ctx->abts_poller_args) {
					free(ctx->abts_poller_args);
				}
				free(ctx);
			}
		}
	}

	spdk_nvmf_fc_lld_ops.reinit_q(hwqp->queues, queues_curr);
}

void
spdk_nvmf_fc_init_hwqp(struct spdk_nvmf_fc_port *fc_port, struct spdk_nvmf_fc_hwqp *hwqp)
{
	hwqp->fc_port = fc_port;

	/* clear counters */
	memset(&hwqp->counters, 0, sizeof(struct spdk_nvmf_fc_errors));

	spdk_nvmf_fc_init_poller_queues(hwqp);
	if (&fc_port->ls_queue != hwqp) {
		(void)nvmf_fc_create_req_mempool(hwqp);
	}
	(void)spdk_nvmf_fc_lld_ops.init_q(hwqp);
	TAILQ_INIT(&hwqp->connection_list);
	TAILQ_INIT(&hwqp->sync_cbs);
	TAILQ_INIT(&hwqp->ls_pending_queue);
}

static struct spdk_nvmf_fc_poll_group *
nvmf_fc_assign_hwqp_to_poll_group(struct spdk_nvmf_fc_hwqp *hwqp)
{
	uint32_t max_count = UINT32_MAX;
	struct spdk_nvmf_fc_poll_group *fc_poll_group, *ret_fc_poll_group = NULL;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC,
		      "Add hwqp to poller for port: %d, hwqp: %d\n",
		      hwqp->fc_port->port_hdl, hwqp->hwqp_id);

	assert(g_nvmf_fc_poll_group_count);

	if (hwqp->nvme_aq) {
		/* hwqp for admin queues only are assigned to master thread poll group */
		TAILQ_FOREACH(fc_poll_group, &g_nvmf_fc_poll_groups, link) {
			if (fc_poll_group->poll_group->thread == g_nvmf_fc_master_thread) {
				ret_fc_poll_group = fc_poll_group;
				break;
			}
		}
		if (!ret_fc_poll_group) {
			SPDK_ERRLOG("Unable to find master thread for admin hwqp.\n");
		}
	} else {
		/* find poll group with least number of hwqp's assigned to it */
		TAILQ_FOREACH(fc_poll_group, &g_nvmf_fc_poll_groups, link) {
			/*
			 * Skip master thread poll group and
			 * if applicable, lld driver reserved thread poll group.
			 */
			if (fc_poll_group->poll_group->thread == g_nvmf_fc_master_thread ||
			    fc_poll_group->poll_group->thread ==
			    spdk_nvmf_fc_lld_ops.get_rsvd_thread()) {
				continue;
			}
			if (fc_poll_group->hwqp_count < max_count) {
				ret_fc_poll_group = fc_poll_group;
				max_count = fc_poll_group->hwqp_count;
			}
		}
	}

	return ret_fc_poll_group;
}

void
spdk_nvmf_fc_add_hwqp_to_poller(struct spdk_nvmf_fc_hwqp *hwqp)
{
	struct spdk_nvmf_fc_poll_group *fc_poll_group;

	assert(hwqp);
	if (hwqp == NULL) {
		SPDK_ERRLOG("Error: hwqp is NULL\n");
		return;
	}

	fc_poll_group = nvmf_fc_assign_hwqp_to_poll_group(hwqp);
	if (!fc_poll_group && !hwqp->nvme_aq) {
		SPDK_WARNLOG("Assigning hwqp to admin poll group\n");
		hwqp->nvme_aq = true;
		fc_poll_group = nvmf_fc_assign_hwqp_to_poll_group(hwqp);
		hwqp->nvme_aq = false;
	}
	if (fc_poll_group) {
		hwqp->thread = fc_poll_group->poll_group->thread;
		hwqp->fc_poll_group = fc_poll_group;
		fc_poll_group->hwqp_count++;
		spdk_nvmf_fc_poller_api_func(hwqp, SPDK_NVMF_FC_ADD_HWQP_TO_POLLER, NULL);
	} else {
		SPDK_ERRLOG("Could not assign poll group for hwqp (%d)\n", hwqp->hwqp_id);
	}
}

void
spdk_nvmf_fc_remove_hwqp_from_poller(struct spdk_nvmf_fc_hwqp *hwqp)
{
	assert(hwqp);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC,
		      "Remove hwqp from poller: for port: %d, hwqp: %d\n",
		      hwqp->fc_port->port_hdl, hwqp->hwqp_id);

	if (!hwqp->fc_poll_group) {
		SPDK_ERRLOG("HWQP (%d) not assigned to poll group\n", hwqp->hwqp_id);
	} else {
		hwqp->fc_poll_group->hwqp_count--;
		spdk_nvmf_fc_poller_api_func(hwqp, SPDK_NVMF_FC_REMOVE_HWQP_FROM_POLLER, NULL);
	}
}

/*
 * Note: This needs to be used only on master poller.
 */
static uint64_t
nvmf_fc_get_abts_unique_id(void)
{
	static uint32_t u_id = 0;

	return (uint64_t)(++ u_id);
}

static void
nvmf_fc_queue_synced_cb(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret)
{
	struct spdk_nvmf_fc_abts_ctx *ctx = cb_data;
	struct spdk_nvmf_fc_poller_api_abts_recvd_args *args, *poller_arg;

	ctx->hwqps_responded ++;

	if (ctx->hwqps_responded < ctx->num_hwqps) {
		/* Wait for all pollers to complete. */
		return;
	}

	/* Free the queue sync poller args. */
	free(ctx->sync_poller_args);

	/* Mark as queue synced */
	ctx->queue_synced = true;

	/* Reset the ctx values */
	ctx->hwqps_responded = 0;
	ctx->handled = false;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC,
		      "QueueSync(0x%lx) completed for nport: %d, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
		      ctx->u_id, ctx->nport->nport_hdl, ctx->rpi, ctx->oxid, ctx->rxid);

	/* Resend ABTS to pollers */
	args = ctx->abts_poller_args;
	for (int i = 0; i < ctx->num_hwqps; i ++) {
		poller_arg = args + i;
		spdk_nvmf_fc_poller_api_func(poller_arg->hwqp,
					     SPDK_NVMF_FC_POLLER_API_ABTS_RECEIVED,
					     poller_arg);
	}
}

static int
nvmf_fc_handle_abts_notfound(struct spdk_nvmf_fc_abts_ctx *ctx)
{
	struct spdk_nvmf_fc_poller_api_queue_sync_args *args, *poller_arg;
	struct spdk_nvmf_fc_poller_api_abts_recvd_args *abts_args, *abts_poller_arg;

	/* check if FC driver supports queue sync */
	if (!spdk_nvmf_fc_lld_ops.q_sync_available()) {
		return -1;
	}

	assert(ctx);
	if (!ctx) {
		SPDK_ERRLOG("NULL ctx pointer");
		return -1;
	}

	/* Reset the ctx values */
	ctx->hwqps_responded = 0;

	args = calloc(ctx->num_hwqps,
		      sizeof(struct spdk_nvmf_fc_poller_api_queue_sync_args));
	if (!args) {
		goto fail;
	}
	ctx->sync_poller_args = args;

	abts_args = ctx->abts_poller_args;
	for (int i = 0; i < ctx->num_hwqps; i ++) {
		abts_poller_arg = abts_args + i;
		poller_arg = args + i;
		poller_arg->u_id = ctx->u_id;
		poller_arg->hwqp = abts_poller_arg->hwqp;
		poller_arg->cb_info.cb_func = nvmf_fc_queue_synced_cb;
		poller_arg->cb_info.cb_data = ctx;
		poller_arg->cb_info.cb_thread = spdk_get_thread();

		/* Send a Queue sync message to interested pollers */
		spdk_nvmf_fc_poller_api_func(poller_arg->hwqp,
					     SPDK_NVMF_FC_POLLER_API_QUEUE_SYNC,
					     poller_arg);
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC,
		      "QueueSync(0x%lx) Sent for nport: %d, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
		      ctx->u_id, ctx->nport->nport_hdl, ctx->rpi, ctx->oxid, ctx->rxid);

	/* Post Marker to queue to track aborted request */
	spdk_nvmf_fc_lld_ops.issue_q_sync(ctx->ls_hwqp, ctx->u_id, ctx->fcp_rq_id);

	return 0;
fail:
	SPDK_ERRLOG("QueueSync(0x%lx) failed for nport: %d, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
		    ctx->u_id, ctx->nport->nport_hdl, ctx->rpi, ctx->oxid, ctx->rxid);
	return -1;
}

static void
nvmf_fc_abts_handled_cb(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret)
{
	struct spdk_nvmf_fc_abts_ctx *ctx = cb_data;
	struct spdk_nvmf_fc_nport *nport  = NULL;

	if (ret != SPDK_NVMF_FC_POLLER_API_OXID_NOT_FOUND) {
		ctx->handled = true;
	}

	ctx->hwqps_responded ++;

	if (ctx->hwqps_responded < ctx->num_hwqps) {
		/* Wait for all pollers to complete. */
		return;
	}

	nport = spdk_nvmf_fc_nport_get(ctx->port_hdl, ctx->nport_hdl);

	if (!(nport && ctx->nport == nport)) {
		/* Nport can be deleted while this abort is being
		 * processed by the pollers.
		 */
		SPDK_NOTICELOG("nport_%d deleted while processing ABTS frame, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
			       ctx->nport_hdl, ctx->rpi, ctx->oxid, ctx->rxid);
		goto out;
	} else if (!ctx->handled) {
		/* Try syncing the queues and try one more time */
		if (!ctx->queue_synced && (nvmf_fc_handle_abts_notfound(ctx) == 0)) {

			SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC,
				      "QueueSync(0x%lx) for nport: %d, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
				      ctx->u_id, ctx->nport->nport_hdl, ctx->rpi, ctx->oxid, ctx->rxid);
			return;
		} else {
			/* Send Reject */
			spdk_nvmf_fc_lld_ops.xmt_bls_rsp(&ctx->nport->fc_port->ls_queue,
							 ctx->oxid, ctx->rxid, ctx->rpi, true,
							 FCNVME_BLS_REJECT_EXP_INVALID_OXID, NULL, NULL);
		}
	} else {
		/* Send Accept */
		spdk_nvmf_fc_lld_ops.xmt_bls_rsp(&ctx->nport->fc_port->ls_queue,
						 ctx->oxid, ctx->rxid, ctx->rpi, false,
						 0, NULL, NULL);
	}
	SPDK_NOTICELOG("BLS_%s sent for ABTS frame nport: %d, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
		       (ctx->handled) ? "ACC" : "REJ", ctx->nport->nport_hdl, ctx->rpi, ctx->oxid, ctx->rxid);
out:

	free(ctx->abts_poller_args);
	free(ctx);
}

void
spdk_nvmf_fc_handle_abts_frame(struct spdk_nvmf_fc_nport *nport, uint16_t rpi,
			       uint16_t oxid, uint16_t rxid)
{
	struct spdk_nvmf_fc_abts_ctx *ctx = NULL;
	struct spdk_nvmf_fc_poller_api_abts_recvd_args *args = NULL, *poller_arg;
	struct spdk_nvmf_fc_association *assoc = NULL;
	struct spdk_nvmf_fc_conn *conn = NULL;
	uint32_t hwqp_cnt = 0;
	bool skip_hwqp_cnt = false;
	struct spdk_nvmf_fc_hwqp **hwqps = NULL;

	SPDK_NOTICELOG("Handle ABTS frame for nport: %d, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
		       nport->nport_hdl, rpi, oxid, rxid);

	hwqps = calloc(nport->fc_port->num_io_queues, sizeof(struct spdk_nvmf_fc_hwqp *));
	if (hwqps == NULL) {
		SPDK_ERRLOG("Unable to allocate temp. hwqp array for abts processing!\n");
		goto bls_rej;
	}

	TAILQ_FOREACH(assoc, &nport->fc_associations, link) {
		TAILQ_FOREACH(conn, &assoc->fc_conns, assoc_link) {
			if (conn->rpi != rpi) {
				continue;
			}

			for (uint32_t k = 0; k < hwqp_cnt; k ++) {
				if (hwqps[k] == conn->hwqp) {
					/* Skip. This is already present */
					skip_hwqp_cnt = true;
					break;
				}
			}
			if (!skip_hwqp_cnt) {
				assert(hwqp_cnt < nport->fc_port->num_io_queues);
				hwqps[hwqp_cnt] = conn->hwqp;
				hwqp_cnt ++;
			} else {
				skip_hwqp_cnt = false;
			}
		}
	}

	if (!hwqp_cnt) {
		goto bls_rej;
	}

	args = calloc(hwqp_cnt,
		      sizeof(struct spdk_nvmf_fc_poller_api_abts_recvd_args));
	if (!args) {
		goto bls_rej;
	}

	ctx = calloc(1, sizeof(struct spdk_nvmf_fc_abts_ctx));
	if (!ctx) {
		goto bls_rej;
	}
	ctx->rpi = rpi;
	ctx->oxid = oxid;
	ctx->rxid = rxid;
	ctx->nport = nport;
	ctx->nport_hdl = nport->nport_hdl;
	ctx->port_hdl = nport->fc_port->port_hdl;
	ctx->num_hwqps = hwqp_cnt;
	ctx->ls_hwqp = &nport->fc_port->ls_queue;
	ctx->fcp_rq_id = nport->fc_port->fcp_rq_id;
	ctx->abts_poller_args = args;

	/* Get a unique context for this ABTS */
	ctx->u_id = nvmf_fc_get_abts_unique_id();

	for (uint32_t i = 0; i < hwqp_cnt; i ++) {
		poller_arg = args + i;
		poller_arg->hwqp = hwqps[i];
		poller_arg->cb_info.cb_func = nvmf_fc_abts_handled_cb;
		poller_arg->cb_info.cb_data = ctx;
		poller_arg->cb_info.cb_thread = spdk_get_thread();
		poller_arg->ctx = ctx;

		spdk_nvmf_fc_poller_api_func(poller_arg->hwqp,
					     SPDK_NVMF_FC_POLLER_API_ABTS_RECEIVED,
					     poller_arg);
	}

	return;
bls_rej:
	if (args) {
		free(args);
	}

	if (hwqps) {
		free(hwqps);
	}

	/* Send Reject */
	spdk_nvmf_fc_lld_ops.xmt_bls_rsp(&nport->fc_port->ls_queue, oxid, rxid, rpi,
					 true, FCNVME_BLS_REJECT_EXP_NOINFO, NULL, NULL);
	SPDK_NOTICELOG("BLS_RJT for ABTS frame for nport: %d, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
		       nport->nport_hdl, rpi, oxid, rxid);
	return;
}

/*** Accessor functions for the FC structures - BEGIN */
/*
 * Returns true if the port is in offline state.
 */
bool
spdk_nvmf_fc_port_is_offline(struct spdk_nvmf_fc_port *fc_port)
{
	if (fc_port && (fc_port->hw_port_status == SPDK_FC_PORT_OFFLINE)) {
		return true;
	} else {
		return false;
	}
}

/*
 * Returns true if the port is in online state.
 */
bool
spdk_nvmf_fc_port_is_online(struct spdk_nvmf_fc_port *fc_port)
{
	if (fc_port && (fc_port->hw_port_status == SPDK_FC_PORT_ONLINE)) {
		return true;
	}

	return false;
}

int
spdk_nvmf_fc_port_set_online(struct spdk_nvmf_fc_port *fc_port)
{
	if (fc_port && (fc_port->hw_port_status != SPDK_FC_PORT_ONLINE)) {
		fc_port->hw_port_status = SPDK_FC_PORT_ONLINE;
		return 0;
	} else {
		return EALREADY;
	}
}

int
spdk_nvmf_fc_port_set_offline(struct spdk_nvmf_fc_port *fc_port)
{
	if (fc_port && (fc_port->hw_port_status != SPDK_FC_PORT_OFFLINE)) {
		fc_port->hw_port_status = SPDK_FC_PORT_OFFLINE;
		return 0;
	} else {
		return EALREADY;
	}
}

int
spdk_nvmf_fc_hwqp_set_online(struct spdk_nvmf_fc_hwqp *hwqp)
{
	if (hwqp && (hwqp->state != SPDK_FC_HWQP_ONLINE)) {
		hwqp->state = SPDK_FC_HWQP_ONLINE;
		/* reset some queue counters */
		hwqp->num_conns = 0;
		return spdk_nvmf_fc_lld_ops.set_q_online_state(hwqp, true);
	} else {
		return EALREADY;
	}
}

int
spdk_nvmf_fc_hwqp_set_offline(struct spdk_nvmf_fc_hwqp *hwqp)
{
	if (hwqp && (hwqp->state != SPDK_FC_HWQP_OFFLINE)) {
		hwqp->state = SPDK_FC_HWQP_OFFLINE;
		return spdk_nvmf_fc_lld_ops.set_q_online_state(hwqp, false);
	} else {
		return EALREADY;
	}
}

void
spdk_nvmf_fc_port_list_add(struct spdk_nvmf_fc_port *fc_port)
{
	TAILQ_INSERT_TAIL(&g_spdk_nvmf_fc_port_list, fc_port, link);
}

struct spdk_nvmf_fc_port *
spdk_nvmf_fc_port_list_get(uint8_t port_hdl)
{
	struct spdk_nvmf_fc_port *fc_port = NULL;

	TAILQ_FOREACH(fc_port, &g_spdk_nvmf_fc_port_list, link) {
		if (fc_port->port_hdl == port_hdl) {
			return fc_port;
		}
	}
	return NULL;
}

static void
nvmf_fc_port_cleanup(void)
{
	struct spdk_nvmf_fc_port *fc_port, *tmp;
	uint32_t i;

	TAILQ_FOREACH_SAFE(fc_port, &g_spdk_nvmf_fc_port_list, link, tmp) {
		TAILQ_REMOVE(&g_spdk_nvmf_fc_port_list,  fc_port, link);
		for (i = 0; i < fc_port->num_io_queues; i++) {
			if (fc_port->io_queues[i].fc_request_pool) {
				spdk_mempool_free(fc_port->io_queues[i].fc_request_pool);
			}
		}
		free(fc_port);
	}
}

uint32_t
spdk_nvmf_fc_get_prli_service_params(void)
{
	return (SPDK_NVMF_FC_DISCOVERY_SERVICE | SPDK_NVMF_FC_TARGET_FUNCTION);
}

int
spdk_nvmf_fc_port_add_nport(struct spdk_nvmf_fc_port *fc_port,
			    struct spdk_nvmf_fc_nport *nport)
{
	if (fc_port) {
		TAILQ_INSERT_TAIL(&fc_port->nport_list, nport, link);
		fc_port->num_nports++;
		return 0;
	} else {
		return EINVAL;
	}
}

int
spdk_nvmf_fc_port_remove_nport(struct spdk_nvmf_fc_port *fc_port,
			       struct spdk_nvmf_fc_nport *nport)
{
	if (fc_port && nport) {
		TAILQ_REMOVE(&fc_port->nport_list, nport, link);
		fc_port->num_nports--;
		return 0;
	} else {
		return EINVAL;
	}
}

struct spdk_nvmf_fc_nport *
spdk_nvmf_fc_nport_get(uint8_t port_hdl, uint16_t nport_hdl)
{
	struct spdk_nvmf_fc_port *fc_port = NULL;
	struct spdk_nvmf_fc_nport *fc_nport = NULL;

	fc_port = spdk_nvmf_fc_port_list_get(port_hdl);
	if (fc_port) {
		TAILQ_FOREACH(fc_nport, &fc_port->nport_list, link) {
			if (fc_nport->nport_hdl == nport_hdl) {
				return fc_nport;
			}
		}
	}
	return NULL;
}

static inline int
nvmf_fc_find_nport_and_rport(struct spdk_nvmf_fc_hwqp *hwqp,
			     uint32_t d_id, struct spdk_nvmf_fc_nport **nport,
			     uint32_t s_id, struct spdk_nvmf_fc_remote_port_info **rport)
{
	struct spdk_nvmf_fc_nport *n_port;
	struct spdk_nvmf_fc_remote_port_info *r_port;

	assert(hwqp);
	if (hwqp == NULL) {
		SPDK_ERRLOG("Error: hwqp is NULL\n");
		return -1;
	}
	assert(nport);
	if (nport == NULL) {
		SPDK_ERRLOG("Error: nport is NULL\n");
		return -1;
	}
	assert(rport);
	if (rport == NULL) {
		SPDK_ERRLOG("Error: rport is NULL\n");
		return -1;
	}

	TAILQ_FOREACH(n_port, &hwqp->fc_port->nport_list, link) {
		if (n_port->d_id == d_id) {
			TAILQ_FOREACH(r_port, &n_port->rem_port_list, link) {
				if (r_port->s_id == s_id) {
					*nport = n_port;
					*rport = r_port;
					return 0;
				}
			}
			break;
		}
	}
	return -1;
}

/* Returns true if the Nport is empty of all rem_ports */
bool
spdk_nvmf_fc_nport_is_rport_empty(struct spdk_nvmf_fc_nport *nport)
{
	if (nport && TAILQ_EMPTY(&nport->rem_port_list)) {
		assert(nport->rport_count == 0);
		return true;
	} else {
		return false;
	}
}

int
spdk_nvmf_fc_nport_set_state(struct spdk_nvmf_fc_nport *nport,
			     enum spdk_nvmf_fc_object_state state)
{
	if (nport) {
		nport->nport_state = state;
		return 0;
	} else {
		return EINVAL;
	}
}

bool
spdk_nvmf_fc_nport_add_rem_port(struct spdk_nvmf_fc_nport *nport,
				struct spdk_nvmf_fc_remote_port_info *rem_port)
{
	if (nport && rem_port) {
		TAILQ_INSERT_TAIL(&nport->rem_port_list, rem_port, link);
		nport->rport_count++;
		return 0;
	} else {
		return EINVAL;
	}
}

bool
spdk_nvmf_fc_nport_remove_rem_port(struct spdk_nvmf_fc_nport *nport,
				   struct spdk_nvmf_fc_remote_port_info *rem_port)
{
	if (nport && rem_port) {
		TAILQ_REMOVE(&nport->rem_port_list, rem_port, link);
		nport->rport_count--;
		return 0;
	} else {
		return EINVAL;
	}
}

int
spdk_nvmf_fc_rport_set_state(struct spdk_nvmf_fc_remote_port_info *rport,
			     enum spdk_nvmf_fc_object_state state)
{
	if (rport) {
		rport->rport_state = state;
		return 0;
	} else {
		return EINVAL;
	}
}
int
spdk_nvmf_fc_assoc_set_state(struct spdk_nvmf_fc_association *assoc,
			     enum spdk_nvmf_fc_object_state state)
{
	if (assoc) {
		assoc->assoc_state = state;
		return 0;
	} else {
		return EINVAL;
	}
}

struct spdk_nvmf_fc_association *
spdk_nvmf_fc_get_ctrlr_assoc(struct spdk_nvmf_ctrlr *ctrlr)
{
	struct spdk_nvmf_qpair *qpair = ctrlr->admin_qpair;
	struct spdk_nvmf_fc_conn *fc_conn;

	if (!qpair) {
		SPDK_ERRLOG("Controller %d has no associations\n", ctrlr->cntlid);
		return NULL;
	}

	fc_conn = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_fc_conn, qpair);

	return fc_conn->fc_assoc;
}

static inline struct spdk_nvmf_fc_conn *
nvmf_fc_get_fc_conn(struct spdk_nvmf_qpair *qpair)
{
	return (struct spdk_nvmf_fc_conn *)
	       ((uintptr_t)qpair - offsetof(struct spdk_nvmf_fc_conn, qpair));
}

bool
spdk_nvmf_fc_is_spdk_ctrlr_on_nport(uint8_t port_hdl, uint16_t nport_hdl,
				    struct spdk_nvmf_ctrlr *ctrlr)
{
	struct spdk_nvmf_fc_nport *fc_nport = NULL;
	struct spdk_nvmf_fc_association *assoc = NULL;

	if (!ctrlr) {
		return false;
	}

	fc_nport = spdk_nvmf_fc_nport_get(port_hdl, nport_hdl);
	if (!fc_nport) {
		return false;
	}

	assoc = spdk_nvmf_fc_get_ctrlr_assoc(ctrlr);
	if (assoc && assoc->tgtport == fc_nport) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC,
			      "Controller: %d corresponding to association: %p(%lu:%d) is on port: %d nport: %d\n",
			      ctrlr->cntlid, assoc, assoc->assoc_id, assoc->assoc_state, port_hdl,
			      nport_hdl);
		return true;
	}
	return false;
}

static inline bool
nvmf_fc_req_in_bdev(struct spdk_nvmf_fc_request *fc_req)
{
	switch (fc_req->state) {
	case SPDK_NVMF_FC_REQ_READ_BDEV:
	case SPDK_NVMF_FC_REQ_WRITE_BDEV:
	case SPDK_NVMF_FC_REQ_NONE_BDEV:
		return true;
	default:
		return false;
	}
}

static inline bool
nvmf_fc_req_in_pending(struct spdk_nvmf_fc_request *fc_req)
{
	struct spdk_nvmf_fc_request *tmp = NULL;

	TAILQ_FOREACH(tmp, &fc_req->fc_conn->pending_queue, pending_link) {
		if (tmp == fc_req) {
			return true;
		}
	}
	return false;
}

static void
nvmf_fc_req_bdev_abort(void *arg1, void *arg2)
{
	/*
	struct spdk_nvmf_fc_request *fc_req = arg1;
	 */
	/* Initial release - we don't have to abort Admin Queue or
	 * Fabric commands. The AQ commands supported at this time are
	 * Get-Log-Page,
	 * Identify
	 * Set Features
	 * Get Features
	 * AER -> Special case and handled differently.
	 * Every one of the above Admin commands (except AER) run
	 * to completion and so an Abort of such commands doesn't
	 * make sense.
	 */
	/* Note that fabric commands are also not aborted via this
	 * mechanism. That check is present in the spdk_nvmf_request_abort
	 * function. The Fabric commands supported are
	 * Property Set
	 * Property Get
	 * Connect -> Special case (async. handling). Not sure how to
	 * handle at this point. Let it run to completion.
	 */
	/* TODO
	spdk_nvmf_request_abort(&fc_req->req);
	 */
}

void
spdk_nvmf_fc_req_abort_complete(void *arg1)
{
	struct spdk_nvmf_fc_request *fc_req =
		(struct spdk_nvmf_fc_request *)arg1;
	struct spdk_nvmf_fc_caller_ctx *ctx = NULL, *tmp = NULL;

	/* Request abort completed. Notify all the callbacks */
	TAILQ_FOREACH_SAFE(ctx, &fc_req->abort_cbs, link, tmp) {
		/* Notify */
		ctx->cb(fc_req->hwqp, 0, ctx->cb_args);
		/* Remove */
		TAILQ_REMOVE(&fc_req->abort_cbs, ctx, link);
		/* free */
		free(ctx);
	}

	SPDK_NOTICELOG("FC Request(%p) in state :%s aborted\n", fc_req,
		       fc_req_state_strs[fc_req->state]);

	spdk_nvmf_fc_free_req(fc_req);
}

void
spdk_nvmf_fc_req_abort(struct spdk_nvmf_fc_request *fc_req,
		       bool send_abts, spdk_nvmf_fc_caller_cb cb,
		       void *cb_args)
{
	struct spdk_nvmf_fc_caller_ctx *ctx = NULL;
	bool kill_req = false;

	/* Add the cb to list */
	if (cb) {
		ctx = calloc(1, sizeof(struct spdk_nvmf_fc_caller_ctx));
		if (!ctx) {
			SPDK_ERRLOG("%s: ctx alloc failed.\n", __func__);
			return;
		}
		ctx->cb = cb;
		ctx->cb_args = cb_args;

		TAILQ_INSERT_TAIL(&fc_req->abort_cbs, ctx, link);
	}

	if (!fc_req->is_aborted) {
		/* Increment aborted command counter */
		fc_req->hwqp->counters.num_aborted++;
	}

	/* If port is dead, skip abort wqe */
	kill_req = spdk_nvmf_fc_is_port_dead(fc_req->hwqp);
	if (kill_req && spdk_nvmf_fc_req_in_xfer(fc_req)) {
		fc_req->is_aborted = true;
		goto complete;
	}

	/* Check if the request is already marked for deletion */
	if (fc_req->is_aborted) {
		return;
	}

	/* Mark request as aborted */
	fc_req->is_aborted = true;

	/* If xchg is allocated, then save if we need to send abts or not. */
	if (fc_req->xchg) {
		fc_req->xchg->send_abts = send_abts;
		fc_req->xchg->aborted	= true;
	}

	if (fc_req->state == SPDK_NVMF_FC_REQ_BDEV_ABORTED) {
		/* Aborted by backend */
		goto complete;
	} else if (nvmf_fc_req_in_bdev(fc_req)) {
		/* Notify bdev */
		nvmf_fc_req_bdev_abort(fc_req, NULL);
	} else if (spdk_nvmf_fc_req_in_xfer(fc_req)) {
		/* Notify HBA to abort this exchange  */
		spdk_nvmf_fc_lld_ops.issue_abort(fc_req->hwqp, fc_req->xchg, NULL, NULL);
	} else if (nvmf_fc_req_in_get_buff(fc_req)) {
		/* Will be completed by request_complete callback. */
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC, "Abort req when getting buffers.\n");
	} else if (nvmf_fc_req_in_pending(fc_req)) {
		/* Remove from pending */
		TAILQ_REMOVE(&fc_req->fc_conn->pending_queue, fc_req, pending_link);
		goto complete;
	} else {
		/* Should never happen */
		SPDK_ERRLOG("%s: Request in invalid state\n", __func__);
		goto complete;
	}

	return;
complete:
	spdk_nvmf_fc_req_set_state(fc_req, SPDK_NVMF_FC_REQ_ABORTED);
	spdk_nvmf_fc_poller_api_func(fc_req->hwqp, SPDK_NVMF_FC_POLLER_API_REQ_ABORT_COMPLETE,
				     (void *)fc_req);
}

static int
nvmf_fc_req_get_buffers(struct spdk_nvmf_fc_request *fc_req)
{
	void	 *buf = NULL;
	uint32_t length = fc_req->req.length;
	uint32_t i = 0;
	struct spdk_nvmf_fc_transport *fc_transport = fc_req->hwqp->fc_poll_group->fc_transport;

	fc_req->req.iovcnt = 0;
	while (length) {
		buf = spdk_mempool_get(fc_transport->data_buff_pool);
		if (!buf) {
			goto nomem;
		}

		fc_req->req.iov[i].iov_base = (void *)((unsigned long)((char *)buf + 512) & ~511UL);
		fc_req->req.iov[i].iov_len  = spdk_min(length,
						       fc_transport->transport.opts.io_unit_size);
		fc_req->req.iovcnt++;
		fc_req->buffers[i] = buf;
		length -= fc_req->req.iov[i++].iov_len;
	}

	fc_req->data_from_pool = true;
	return 0;

nomem:
	while (i) {
		i--;
		spdk_mempool_put(fc_req->hwqp->fc_poll_group->fc_transport->data_buff_pool,
				 fc_req->buffers[i]);
		fc_req->req.iov[i].iov_base = NULL;
		fc_req->req.iov[i].iov_len = 0;
		fc_req->buffers[i] = NULL;
	}

	fc_req->req.iovcnt = 0;
	return -ENOMEM;
}


static int
nvmf_fc_execute_nvme_rqst(struct spdk_nvmf_fc_request *fc_req)
{
	/* Allocate an XCHG if we dont use send frame for this command. */
	if (!spdk_nvmf_fc_use_send_frame(&fc_req->req)) {
		fc_req->xchg = spdk_nvmf_fc_lld_ops.get_xchg(fc_req->hwqp);
		if (!fc_req->xchg) {
			fc_req->hwqp->counters.no_xchg++;
			printf("NO XCHGs!\n");
			goto pending;
		}
	}

	if (fc_req->req.length) {
		if (nvmf_fc_req_get_buffers(fc_req)) {
			fc_req->hwqp->counters.buf_alloc_err++;
			goto pending;
		}

		if (fc_req->req.iovcnt == 1) {
			fc_req->req.data = fc_req->req.iov[0].iov_base;
		} else {
			fc_req->req.data = NULL;
		}
	}

	if (fc_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC, "WRITE CMD.\n");

		spdk_nvmf_fc_req_set_state(fc_req, SPDK_NVMF_FC_REQ_WRITE_XFER);

		if (spdk_nvmf_fc_lld_ops.recv_data(fc_req)) {
			goto error;
		}
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC, "READ/NONE CMD\n");

		if (fc_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
			spdk_nvmf_fc_req_set_state(fc_req, SPDK_NVMF_FC_REQ_READ_BDEV);

			if (!fc_req->req.data && !fc_req->req.iovcnt) {
				fc_req->req.iovcnt = 1;
			}

		} else {
			spdk_nvmf_fc_req_set_state(fc_req, SPDK_NVMF_FC_REQ_NONE_BDEV);
		}
		spdk_nvmf_request_exec(&fc_req->req);
	}

	return 0;

pending:
	if (fc_req->xchg) {
		spdk_nvmf_fc_lld_ops.put_xchg(fc_req->hwqp, fc_req->xchg);
		fc_req->xchg = NULL;
	}

	spdk_nvmf_fc_req_set_state(fc_req, SPDK_NVMF_FC_REQ_PENDING);

	return 1;
error:
	/* Dropped return success to caller */
	fc_req->hwqp->counters.unexpected_err++;
	spdk_nvmf_fc_free_req(fc_req);
	return 0;
}

static int
nvmf_fc_handle_nvme_rqst(struct spdk_nvmf_fc_hwqp *hwqp, struct spdk_nvmf_fc_frame_hdr *frame,
			 uint32_t buf_idx, struct spdk_nvmf_fc_buffer_desc *buffer, uint32_t plen)
{
	uint16_t cmnd_len;
	uint64_t rqst_conn_id;
	struct spdk_nvmf_fc_request *fc_req = NULL;
	struct spdk_nvmf_fc_cmnd_iu *cmd_iu = NULL;
	struct spdk_nvmf_fc_conn *fc_conn = NULL;
	enum spdk_nvme_data_transfer xfer;
	bool found = false;

	cmd_iu = buffer->virt;
	cmnd_len = cmd_iu->cmnd_iu_len;
	cmnd_len = from_be16(&cmnd_len);

	/* check for a valid cmnd_iu format */
	if ((cmd_iu->fc_id != FCNVME_CMND_IU_FC_ID) ||
	    (cmd_iu->scsi_id != FCNVME_CMND_IU_SCSI_ID) ||
	    (cmnd_len != sizeof(struct spdk_nvmf_fc_cmnd_iu) / 4)) {
		SPDK_ERRLOG("IU CMD error\n");
		hwqp->counters.nvme_cmd_iu_err++;
		goto abort;
	}

	xfer = spdk_nvme_opc_get_data_transfer(cmd_iu->flags);
	if (xfer == SPDK_NVME_DATA_BIDIRECTIONAL) {
		SPDK_ERRLOG("IU CMD xfer error\n");
		hwqp->counters.nvme_cmd_xfer_err++;
		goto abort;
	}

	rqst_conn_id = from_be64(&cmd_iu->conn_id);

	/* Check if conn id is valid */
	TAILQ_FOREACH(fc_conn, &hwqp->connection_list, link) {
		if (fc_conn->conn_id == rqst_conn_id) {
			found = true;
			break;
		}
	}

	if (!found) {
		SPDK_ERRLOG("IU CMD conn(%ld) invalid\n", rqst_conn_id);
		hwqp->counters.invalid_conn_err++;
		goto abort;
	}

	/* If association/connection is being deleted - return */
	if (fc_conn->fc_assoc->assoc_state !=  SPDK_NVMF_FC_OBJECT_CREATED) {
		SPDK_ERRLOG("Association state not valid\n");
		goto abort;
	}

	/* Make sure xfer len is according to mdts */
	if (from_be32(&cmd_iu->data_len) >
	    hwqp->fc_poll_group->fc_transport->transport.opts.max_io_size) {
		SPDK_ERRLOG("IO length requested is greater than MDTS\n");
		goto abort;
	}

	/* allocate a request buffer */
	fc_req = nvmf_fc_alloc_req_buf(hwqp);
	if (fc_req == NULL) {
		/* Should not happen. Since fc_reqs == RQ buffers */
		goto abort;
	}

	fc_req->req.length = from_be32(&cmd_iu->data_len);
	fc_req->req.qpair = &fc_conn->qpair;
	fc_req->req.cmd = (union nvmf_h2c_msg *)&cmd_iu->cmd;
	fc_req->req.rsp = (union nvmf_c2h_msg *)&fc_req->ersp.rsp;
	fc_req->req.io_rsrc_pool = hwqp->fc_port->io_rsrc_pool;
	fc_req->oxid = frame->ox_id;
	fc_req->oxid = from_be16(&fc_req->oxid);
	fc_req->rpi = fc_conn->rpi;
	fc_req->buf_index = buf_idx;
	fc_req->poller_lcore = hwqp->lcore_id;
	fc_req->poller_thread = hwqp->thread;
	fc_req->hwqp = hwqp;
	fc_req->fc_conn = fc_conn;
	fc_req->req.xfer = xfer;
	fc_req->s_id = (uint32_t)frame->s_id;
	fc_req->d_id = (uint32_t)frame->d_id;
	fc_req->s_id = from_be32(&fc_req->s_id) >> 8;
	fc_req->d_id = from_be32(&fc_req->d_id) >> 8;

	nvmf_fc_record_req_trace_point(fc_req, SPDK_NVMF_FC_REQ_INIT);
	if (nvmf_fc_execute_nvme_rqst(fc_req)) {
		TAILQ_INSERT_TAIL(&fc_conn->pending_queue, fc_req, pending_link);
	}

	return 0;

abort:
	/* Issue abort for oxid */
	SPDK_ERRLOG("Aborted CMD\n");
	return -1;
}

/*
 * These functions are called from the FC LLD
 */

void
spdk_nvmf_fc_free_req(struct spdk_nvmf_fc_request *fc_req)
{
	if (!fc_req) {
		return;
	}

	if (fc_req->xchg) {
		spdk_nvmf_fc_lld_ops.put_xchg(fc_req->hwqp, fc_req->xchg);
		fc_req->xchg = NULL;
	}

	/* Release IO buffers */
	nvmf_fc_release_io_buff(fc_req);

	/* Release Q buffer */
	spdk_nvmf_fc_lld_ops.q_buffer_release(fc_req->hwqp, fc_req->buf_index);

	/* Free Fc request */
	nvmf_fc_free_req_buf(fc_req->hwqp, fc_req);
}


void
spdk_nvmf_fc_req_set_state(struct spdk_nvmf_fc_request *fc_req,
			   enum spdk_nvmf_fc_request_state state)
{
	assert(fc_req->magic != 0xDEADBEEF);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC,
		      "FC Request(%p):\n\tState Old:%s New:%s\n", fc_req,
		      spdk_nvmf_fc_req_get_state_str(fc_req->state),
		      spdk_nvmf_fc_req_get_state_str(state));
	nvmf_fc_record_req_trace_point(fc_req, state);
	fc_req->state = state;
}

char *
spdk_nvmf_fc_req_get_state_str(int state)
{
	static char *unk_str = "unknown";

	return (state >= 0 && state < (int)(sizeof(fc_req_state_strs) / sizeof(char *)) ?
		fc_req_state_strs[state] : unk_str);
}

int
spdk_nvmf_fc_process_frame(struct spdk_nvmf_fc_hwqp *hwqp,
			   uint32_t buff_idx,
			   struct spdk_nvmf_fc_frame_hdr *frame,
			   struct spdk_nvmf_fc_buffer_desc *buffer,
			   uint32_t plen)
{
	int rc = 0;
	uint32_t s_id, d_id;
	struct spdk_nvmf_fc_nport *nport = NULL;
	struct spdk_nvmf_fc_remote_port_info *rport = NULL;

	s_id = (uint32_t)frame->s_id;
	d_id = (uint32_t)frame->d_id;
	s_id = from_be32(&s_id) >> 8;
	d_id = from_be32(&d_id) >> 8;

	/* Note: In tracelog below, we directly do endian conversion on rx_id and.
	 * ox_id Since these are fields, we can't pass address to from_be16().
	 * Since ox_id and rx_id are only needed for tracelog, assigning to local
	 * vars. and doing conversion is a waste of time in non-debug builds. */
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC,
		      "Process NVME frame s_id:0x%x d_id:0x%x oxid:0x%x rxid:0x%x.\n",
		      s_id, d_id,
		      ((frame->ox_id << 8) & 0xff00) | ((frame->ox_id >> 8) & 0xff),
		      ((frame->rx_id << 8) & 0xff00) | ((frame->rx_id >> 8) & 0xff));

	rc = nvmf_fc_find_nport_and_rport(hwqp, d_id, &nport, s_id, &rport);
	if (rc) {
		if (nport == NULL) {
			SPDK_ERRLOG("%s: Nport not found. Dropping\n", __func__);
			/* increment invalid nport counter */
			hwqp->counters.nport_invalid++;
		} else if (rport == NULL) {
			SPDK_ERRLOG("%s: Rport not found. Dropping\n", __func__);
			/* increment invalid rport counter */
			hwqp->counters.rport_invalid++;
		}
		return rc;
	}

	if (nport->nport_state != SPDK_NVMF_FC_OBJECT_CREATED ||
	    rport->rport_state != SPDK_NVMF_FC_OBJECT_CREATED) {
		SPDK_ERRLOG("%s: %s state not created. Dropping\n", __func__,
			    nport->nport_state != SPDK_NVMF_FC_OBJECT_CREATED ?
			    "Nport" : "Rport");
		return -1;
	}

	if ((frame->r_ctl == FCNVME_R_CTL_LS_REQUEST) &&
	    (frame->type == FCNVME_TYPE_NVMF_DATA)) {
		struct spdk_nvmf_fc_rq_buf_ls_request *req_buf = buffer->virt;
		struct spdk_nvmf_fc_ls_rqst *ls_rqst;

		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC, "Process LS NVME frame\n");

		/* Use the RQ buffer for holding LS request. */
		ls_rqst = (struct spdk_nvmf_fc_ls_rqst *)&req_buf->ls_rqst;

		/* Fill in the LS request structure */
		ls_rqst->rqstbuf.virt = (void *)&req_buf->rqst;
		ls_rqst->rqstbuf.phys = buffer->phys +
					offsetof(struct spdk_nvmf_fc_rq_buf_ls_request, rqst);
		ls_rqst->rqstbuf.buf_index = buff_idx;
		ls_rqst->rqst_len = plen;

		ls_rqst->rspbuf.virt = (void *)&req_buf->resp;
		ls_rqst->rspbuf.phys = buffer->phys +
				       offsetof(struct spdk_nvmf_fc_rq_buf_ls_request, resp);
		ls_rqst->rsp_len = FCNVME_MAX_LS_RSP_SIZE;

		ls_rqst->private_data = (void *)hwqp;
		ls_rqst->rpi = rport->rpi;
		ls_rqst->oxid = (uint16_t)frame->ox_id;
		ls_rqst->oxid = from_be16(&ls_rqst->oxid);
		ls_rqst->s_id = s_id;
		ls_rqst->d_id = d_id;
		ls_rqst->nport = nport;
		ls_rqst->rport = rport;
		ls_rqst->nvmf_tgt = g_nvmf_fc_transport->transport.tgt;

		ls_rqst->xchg = spdk_nvmf_fc_lld_ops.get_xchg(hwqp);
		if (!ls_rqst->xchg) {
			/* No XCHG available. Add to pending list. */
			hwqp->counters.no_xchg++;
			TAILQ_INSERT_TAIL(&hwqp->ls_pending_queue, ls_rqst, ls_pending_link);
		} else {
			/* Handover the request to LS module */
			spdk_nvmf_fc_handle_ls_rqst(ls_rqst);
		}

	} else if ((frame->r_ctl == FCNVME_R_CTL_CMD_REQ) &&
		   (frame->type == FCNVME_TYPE_FC_EXCHANGE)) {

		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC, "Process IO NVME frame\n");
		rc = nvmf_fc_handle_nvme_rqst(hwqp, frame, buff_idx, buffer, plen);
	} else {

		SPDK_ERRLOG("%s Unknown frame received. Dropping\n", __func__);
		hwqp->counters.unknown_frame++;
		rc = -1;
	}

	return rc;
}

void
spdk_nvmf_fc_process_pending_req(struct spdk_nvmf_fc_hwqp *hwqp)
{
	struct spdk_nvmf_fc_conn *fc_conn = NULL;
	struct spdk_nvmf_fc_request *fc_req = NULL, *tmp;
	int budget = 64;

	TAILQ_FOREACH(fc_conn, &hwqp->connection_list, link) {
		TAILQ_FOREACH_SAFE(fc_req, &fc_conn->pending_queue, pending_link, tmp) {
			if (!nvmf_fc_execute_nvme_rqst(fc_req)) {
				/* Succesfuly posted, Delete from pending. */
				TAILQ_REMOVE(&fc_conn->pending_queue, fc_req, pending_link);
			}

			if (budget) {
				budget --;
			} else {
				return;
			}
		}
	}
}

void
spdk_nvmf_fc_process_pending_ls_rqst(struct spdk_nvmf_fc_hwqp *hwqp)
{
	struct spdk_nvmf_fc_ls_rqst *ls_rqst = NULL, *tmp;
	struct spdk_nvmf_fc_nport *nport = NULL;
	struct spdk_nvmf_fc_remote_port_info *rport = NULL;

	TAILQ_FOREACH_SAFE(ls_rqst, &hwqp->ls_pending_queue, ls_pending_link, tmp) {
		/* lookup nport and rport again - make sure they are still valid */
		int rc = nvmf_fc_find_nport_and_rport(hwqp, ls_rqst->d_id, &nport, ls_rqst->s_id, &rport);
		if (rc) {
			if (nport == NULL) {
				SPDK_ERRLOG("%s: Nport not found. Dropping\n", __func__);
				/* increment invalid nport counter */
				hwqp->counters.nport_invalid++;
			} else if (rport == NULL) {
				SPDK_ERRLOG("%s: Rport not found. Dropping\n", __func__);
				/* increment invalid rport counter */
				hwqp->counters.rport_invalid++;
			}
			TAILQ_REMOVE(&hwqp->ls_pending_queue, ls_rqst, ls_pending_link);
			/* Return buffer to chip */
			spdk_nvmf_fc_lld_ops.q_buffer_release(hwqp, ls_rqst->rqstbuf.buf_index);
			continue;
		}
		if (nport->nport_state != SPDK_NVMF_FC_OBJECT_CREATED ||
		    rport->rport_state != SPDK_NVMF_FC_OBJECT_CREATED) {
			SPDK_ERRLOG("%s: %s state not created. Dropping\n", __func__,
				    nport->nport_state != SPDK_NVMF_FC_OBJECT_CREATED ?
				    "Nport" : "Rport");
			TAILQ_REMOVE(&hwqp->ls_pending_queue, ls_rqst, ls_pending_link);
			/* Return buffer to chip */
			spdk_nvmf_fc_lld_ops.q_buffer_release(hwqp, ls_rqst->rqstbuf.buf_index);
			continue;
		}

		ls_rqst->xchg = spdk_nvmf_fc_lld_ops.get_xchg(hwqp);
		if (ls_rqst->xchg) {
			/* Got an XCHG */
			TAILQ_REMOVE(&hwqp->ls_pending_queue, ls_rqst, ls_pending_link);
			/* Handover the request to LS module */
			spdk_nvmf_fc_handle_ls_rqst(ls_rqst);
		} else {
			/* No more XCHGs. Stop processing. */
			hwqp->counters.no_xchg++;
			return;
		}
	}
}

int
spdk_nvmf_fc_handle_rsp(struct spdk_nvmf_fc_request *fc_req)
{
	int rc = 0;
	struct spdk_nvmf_request *req = &fc_req->req;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_fc_conn *fc_conn = spdk_nvmf_fc_get_conn(qpair);
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint16_t ersp_len = 0;

	/* set sq head value in resp */
	rsp->sqhd = spdk_nvmf_fc_advance_conn_sqhead(qpair);

	/* Increment connection responses */
	fc_conn->rsp_count++;

	if (spdk_nvmf_fc_send_ersp_required(fc_req, fc_conn->rsp_count,
					    fc_req->transfered_len)) {
		/* Fill ERSP Len */
		to_be16(&ersp_len, (sizeof(struct spdk_nvmf_fc_ersp_iu) /
				    sizeof(uint32_t)));
		fc_req->ersp.ersp_len = ersp_len;

		/* Fill RSN */
		to_be32(&fc_req->ersp.response_seq_no, fc_conn->rsn);
		fc_conn->rsn++;

		/* Fill transfer length */
		to_be32(&fc_req->ersp.transferred_data_len, fc_req->transfered_len);

		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC, "Posting ERSP.\n");
		rc = spdk_nvmf_fc_lld_ops.xmt_rsp(fc_req, (uint8_t *)&fc_req->ersp,
						  sizeof(struct spdk_nvmf_fc_ersp_iu));
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC, "Posting RSP.\n");
		rc = spdk_nvmf_fc_lld_ops.xmt_rsp(fc_req, NULL, 0);
	}

	return rc;
}

int
spdk_nvmf_fc_xmt_ls_rsp(struct spdk_nvmf_fc_nport *tgtport,
			struct spdk_nvmf_fc_ls_rqst *ls_rqst)
{
	return spdk_nvmf_fc_lld_ops.xmt_ls_rsp(tgtport, ls_rqst);
}

int
spdk_nvmf_fc_xmt_srsr_req(struct spdk_nvmf_fc_hwqp *hwqp,
			  struct spdk_nvmf_fc_srsr_bufs *srsr_bufs,
			  spdk_nvmf_fc_caller_cb cb, void *cb_args)
{
	return spdk_nvmf_fc_lld_ops.xmt_srsr_req(hwqp, srsr_bufs, cb, cb_args);
}

bool
spdk_nvmf_fc_send_ersp_required(struct spdk_nvmf_fc_request *fc_req,
				uint32_t rsp_cnt, uint32_t xfer_len)
{
	struct spdk_nvmf_request *req = &fc_req->req;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_fc_conn *fc_conn = spdk_nvmf_fc_get_conn(qpair);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint16_t status = *((uint16_t *)&rsp->status);
	bool rc = false;

	/*
	 * Check if we need to send ERSP
	 * 1) For every N responses where N == ersp_ratio
	 * 2) Fabric commands.
	 * 3) Completion status failed or Completion dw0 or dw1 valid.
	 * 4) SQ == 90% full.
	 * 5) Transfer length not equal to CMD IU length
	 */

	if (!(rsp_cnt % fc_conn->esrp_ratio) ||
	    (cmd->opc == SPDK_NVME_OPC_FABRIC) ||
	    (status & 0xFFFE) || rsp->cdw0 || rsp->rsvd1 ||
	    (req->length != xfer_len)) {
		rc = true;
	}
	return rc;
}

void
spdk_nvmf_fc_dump_all_queues(struct spdk_nvmf_fc_port *fc_port,
			     struct spdk_nvmf_fc_queue_dump_info *dump_info)
{
	spdk_nvmf_fc_lld_ops.dump_all_queues(&fc_port->ls_queue, fc_port->io_queues,
					     fc_port->num_io_queues, dump_info);
}

static void
nvmf_fc_request_complete_process(void *arg1)
{
	int rc = 0;
	struct spdk_nvmf_request *req = (struct spdk_nvmf_request *)arg1;
	struct spdk_nvmf_fc_request *fc_req = spdk_nvmf_fc_get_fc_req(req);
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	if (fc_req->is_aborted) {
		/* Defer this to make sure we dont call io cleanup in same context. */
		spdk_nvmf_fc_poller_api_func(fc_req->hwqp, SPDK_NVMF_FC_POLLER_API_REQ_ABORT_COMPLETE,
					     (void *)fc_req);
	} else if (rsp->status.sc == SPDK_NVME_SC_SUCCESS &&
		   req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {

		spdk_nvmf_fc_req_set_state(fc_req, SPDK_NVMF_FC_REQ_READ_XFER);

		rc = spdk_nvmf_fc_lld_ops.send_data(fc_req);
	} else {
		if (req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
			spdk_nvmf_fc_req_set_state(fc_req, SPDK_NVMF_FC_REQ_WRITE_RSP);
		} else if (req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
			spdk_nvmf_fc_req_set_state(fc_req, SPDK_NVMF_FC_REQ_READ_RSP);
		} else {
			spdk_nvmf_fc_req_set_state(fc_req, SPDK_NVMF_FC_REQ_NONE_RSP);
		}

		rc = spdk_nvmf_fc_handle_rsp(fc_req);
	}

	if (rc) {
		SPDK_ERRLOG("Error in request complete.\n");
		spdk_nvmf_fc_free_req(fc_req);
	}
}

struct spdk_nvmf_tgt *
spdk_nvmf_fc_get_tgt(void)
{
	if (g_nvmf_fc_transport) {
		return g_nvmf_fc_transport->transport.tgt;
	}
	return NULL;
}

/*
 * FC Transport Public API begins here
 */

#define SPDK_NVMF_FC_DEFAULT_MAX_QUEUE_DEPTH 128
#define SPDK_NVMF_FC_DEFAULT_AQ_DEPTH 32
#define SPDK_NVMF_FC_DEFAULT_MAX_QPAIRS_PER_CTRLR 5
#define SPDK_NVMF_FC_DEFAULT_IN_CAPSULE_DATA_SIZE 0
#define SPDK_NVMF_FC_DEFAULT_MAX_IO_SIZE 65536
#define SPDK_NVMF_FC_DEFAULT_IO_UNIT_SIZE 4096
#define SPDK_NVMF_FC_DATA_BUFF_POOL_SIZE 8192
#define SPDK_NVMF_FC_DEFAULT_NUM_SHARED_BUFFERS 4096

static void
spdk_nvmf_fc_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth =      SPDK_NVMF_FC_DEFAULT_MAX_QUEUE_DEPTH;
	opts->max_qpairs_per_ctrlr = SPDK_NVMF_FC_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	opts->in_capsule_data_size = SPDK_NVMF_FC_DEFAULT_IN_CAPSULE_DATA_SIZE;
	opts->max_io_size =          SPDK_NVMF_FC_DEFAULT_MAX_IO_SIZE;
	opts->io_unit_size =         SPDK_NVMF_FC_DEFAULT_IO_UNIT_SIZE;
	opts->max_aq_depth =         SPDK_NVMF_FC_DEFAULT_AQ_DEPTH;
	opts->num_shared_buffers =   SPDK_NVMF_FC_DEFAULT_NUM_SHARED_BUFFERS;
}

static struct spdk_nvmf_transport *
spdk_nvmf_fc_create(struct spdk_nvmf_transport_opts *opts)
{
	size_t cache_size;

	SPDK_INFOLOG(SPDK_LOG_NVMF_FC, "*** FC Transport Init ***\n"
		     "  Transport opts:  max_ioq_depth=%d, max_io_size=%d,\n"
		     "  max_qpairs_per_ctrlr=%d, io_unit_size=%d,\n"
		     "  max_aq_depth=%d\n",
		     opts->max_queue_depth,
		     opts->max_io_size,
		     opts->max_qpairs_per_ctrlr,
		     opts->io_unit_size,
		     opts->max_aq_depth);

	if (spdk_env_get_last_core() < 1) {
		SPDK_ERRLOG("Not enough cores/threads (%d) to run NVMF-FC transport!\n",
			    spdk_env_get_last_core() + 1);
		return NULL;
	}

	g_nvmf_fc_master_thread = spdk_get_thread();
	g_nvmf_fc_poll_group_count = 0;
	g_nvmf_fc_transport = calloc(1, sizeof(*g_nvmf_fc_transport));

	if (!g_nvmf_fc_transport) {
		SPDK_ERRLOG("Failed to allocate NVMF-FC transport\n");
		return NULL;
	}

	/* Create a databuff pool */
	cache_size = (SPDK_NVMF_FC_DATA_BUFF_POOL_SIZE / 2) / spdk_env_get_core_count();
	cache_size = spdk_min(cache_size, RTE_MEMPOOL_CACHE_MAX_SIZE);

	g_nvmf_fc_transport->data_buff_pool = (struct spdk_mempool *)
					      rte_mempool_create("spdk_nvmf_fc_data_buff",
							      SPDK_NVMF_FC_DATA_BUFF_POOL_SIZE,
							      opts->io_unit_size + 512, cache_size,
							      0, NULL, NULL, NULL, NULL, SOCKET_ID_ANY, 0);

	if (!g_nvmf_fc_transport->data_buff_pool) {
		free(g_nvmf_fc_transport);
		g_nvmf_fc_transport = NULL;
		return NULL;
	}

	/* initialize the low level FC driver */
	spdk_nvmf_fc_lld_ops.lld_init();

	return &g_nvmf_fc_transport->transport;
}

static int
spdk_nvmf_fc_destroy(struct spdk_nvmf_transport *transport)
{
	if (transport) {
		struct spdk_nvmf_fc_transport *fc_transport;
		struct spdk_nvmf_fc_poll_group *fc_poll_group, *pg_tmp;

		fc_transport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_fc_transport, transport);
		spdk_mempool_free(fc_transport->data_buff_pool);

		free(fc_transport);

		/* clean up any FC poll groups still around */
		TAILQ_FOREACH_SAFE(fc_poll_group, &g_nvmf_fc_poll_groups, link, pg_tmp) {
			TAILQ_REMOVE(&g_nvmf_fc_poll_groups, fc_poll_group, link);
			free(fc_poll_group);
		}
		g_nvmf_fc_poll_group_count = 0;

		/* low level FC driver clean up */
		spdk_nvmf_fc_lld_ops.lld_fini();

		nvmf_fc_port_cleanup();
	}

	return 0;
}

static int
spdk_nvmf_fc_listen(struct spdk_nvmf_transport *transport,
		    const struct spdk_nvme_transport_id *trid)
{
	return 0;
}

static int
spdk_nvmf_fc_stop_listen(struct spdk_nvmf_transport *transport,
			 const struct spdk_nvme_transport_id *_trid)
{
	return 0;
}

static void
spdk_nvmf_fc_accept(struct spdk_nvmf_transport *transport, new_qpair_fn cb_fn)
{
	struct spdk_nvmf_fc_port *fc_port = NULL;
	static bool start_lld = false;

	if (!start_lld) {
		start_lld  = true;
		spdk_nvmf_fc_lld_ops.lld_start();
	}

	/* poll the LS queue on each port */
	TAILQ_FOREACH(fc_port, &g_spdk_nvmf_fc_port_list, link) {
		if (fc_port->hw_port_status == SPDK_FC_PORT_ONLINE) {
			spdk_nvmf_fc_lld_ops.poll_queue(&fc_port->ls_queue);
		}
	}
}

static void
spdk_nvmf_fc_discover(struct spdk_nvmf_transport *transport,
		      struct spdk_nvme_transport_id *trid,
		      struct spdk_nvmf_discovery_log_page_entry *entry)
{
	entry->trtype = (enum spdk_nvme_transport_type) SPDK_NVMF_TRTYPE_FC;
	entry->adrfam = trid->adrfam;
	entry->treq.secure_channel = SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_SPECIFIED;

	spdk_strcpy_pad(entry->trsvcid, trid->trsvcid, sizeof(entry->trsvcid), ' ');
	spdk_strcpy_pad(entry->traddr, trid->traddr, sizeof(entry->traddr), ' ');
}

static struct spdk_nvmf_transport_poll_group *
spdk_nvmf_fc_poll_group_create(struct spdk_nvmf_transport *transport)
{
	struct spdk_nvmf_fc_poll_group *fc_poll_group;
	struct spdk_io_channel *ch;

	fc_poll_group = calloc(1, sizeof(struct spdk_nvmf_fc_poll_group));

	if (!fc_poll_group) {
		SPDK_ERRLOG("Unable to alloc FC poll group\n");
		return NULL;
	}

	TAILQ_INIT(&fc_poll_group->hwqp_list);
	fc_poll_group->fc_transport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_fc_transport, transport);

	TAILQ_INSERT_TAIL(&g_nvmf_fc_poll_groups, fc_poll_group, link);
	g_nvmf_fc_poll_group_count++;

	ch = spdk_get_io_channel(g_nvmf_fc_transport->transport.tgt);
	if (ch) {
		fc_poll_group->poll_group = spdk_io_channel_get_ctx(ch);
		spdk_put_io_channel(ch);
	}

	return &fc_poll_group->tp_poll_group;
}

static void
spdk_nvmf_fc_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_fc_poll_group *fc_poll_group;

	fc_poll_group = SPDK_CONTAINEROF(group, struct spdk_nvmf_fc_poll_group, tp_poll_group);
	TAILQ_REMOVE(&g_nvmf_fc_poll_groups, fc_poll_group, link);
	g_nvmf_fc_poll_group_count--;
	free(fc_poll_group);
}

static int
spdk_nvmf_fc_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			    struct spdk_nvmf_qpair *qpair)
{
	return 0;
}

static int
spdk_nvmf_fc_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	uint32_t count = 0;
	struct spdk_nvmf_fc_poll_group *fc_poll_group;
	struct spdk_nvmf_fc_hwqp *hwqp;

	fc_poll_group = SPDK_CONTAINEROF(group, struct spdk_nvmf_fc_poll_group, tp_poll_group);

	TAILQ_FOREACH(hwqp, &fc_poll_group->hwqp_list, link) {
		if (hwqp->state == SPDK_FC_HWQP_ONLINE) {
			count += spdk_nvmf_fc_lld_ops.poll_queue(hwqp);
		}
	}

	return (int) count;
}

static int
spdk_nvmf_fc_request_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fc_request *fc_req = spdk_nvmf_fc_get_fc_req(req);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvmf_fc_conn *fc_conn = fc_req->fc_conn;

	/* Switch back to correct thread for IOQ fabric commands */
	if ((cmd->opc == SPDK_NVME_OPC_FABRIC) &&
	    !spdk_nvmf_qpair_is_admin_queue(&fc_conn->qpair)) {
		spdk_thread_send_msg(fc_req->hwqp->thread,
				     nvmf_fc_request_complete_process, (void *)req);
	} else {
		nvmf_fc_request_complete_process(req);
	}
	return 0;
}

static int
spdk_nvmf_fc_request_free(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fc_request *fc_req = spdk_nvmf_fc_get_fc_req(req);

	spdk_nvmf_fc_req_set_state(fc_req, SPDK_NVMF_FC_REQ_BDEV_ABORTED);

	spdk_nvmf_fc_req_abort(fc_req, true, NULL, NULL);
	return 0;
}

static void
spdk_nvmf_fc_close_qpair(struct spdk_nvmf_qpair *qpair)
{
	/* do nothing - handled in LS processor */
}

static int
spdk_nvmf_fc_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
				 struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_fc_conn *fc_conn;

	fc_conn = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_fc_conn, qpair);
	memcpy(trid, &fc_conn->trid, sizeof(struct spdk_nvme_transport_id));
	return 0;
}

static int
spdk_nvmf_fc_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
				  struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_fc_conn *fc_conn;

	fc_conn = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_fc_conn, qpair);
	memcpy(trid, &fc_conn->trid, sizeof(struct spdk_nvme_transport_id));
	return 0;
}

static int
spdk_nvmf_fc_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
				   struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_fc_conn *fc_conn;

	fc_conn = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_fc_conn, qpair);
	memcpy(trid, &fc_conn->trid, sizeof(struct spdk_nvme_transport_id));
	return 0;
}

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_fc = {
	.type = (enum spdk_nvme_transport_type) SPDK_NVMF_TRTYPE_FC,
	.opts_init = spdk_nvmf_fc_opts_init,
	.create = spdk_nvmf_fc_create,
	.destroy = spdk_nvmf_fc_destroy,

	.listen = spdk_nvmf_fc_listen,
	.stop_listen = spdk_nvmf_fc_stop_listen,
	.accept = spdk_nvmf_fc_accept,

	.listener_discover = spdk_nvmf_fc_discover,

	.poll_group_create = spdk_nvmf_fc_poll_group_create,
	.poll_group_destroy = spdk_nvmf_fc_poll_group_destroy,
	.poll_group_add = spdk_nvmf_fc_poll_group_add,
	.poll_group_poll = spdk_nvmf_fc_poll_group_poll,

	.req_complete = spdk_nvmf_fc_request_complete,
	.req_free = spdk_nvmf_fc_request_free,
	.qpair_fini = spdk_nvmf_fc_close_qpair,
	.qpair_get_peer_trid = spdk_nvmf_fc_qpair_get_peer_trid,
	.qpair_get_local_trid = spdk_nvmf_fc_qpair_get_local_trid,
	.qpair_get_listen_trid = spdk_nvmf_fc_qpair_get_listen_trid,
};

SPDK_LOG_REGISTER_COMPONENT("nvmf_fc", SPDK_LOG_NVMF_FC)
