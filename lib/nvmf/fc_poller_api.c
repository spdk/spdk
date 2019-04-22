/*
 *
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
#include "spdk/event.h"
#include "spdk_internal/log.h"
#include "spdk/io_channel.h"

#include "nvmf_fc.h"

static void
nvmf_fc_poller_api_cb_event(void *arg)
{
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_POLLER_API, "\n");
	if (arg) {
		struct spdk_nvmf_fc_poller_api_cb_info *cb_info =
			(struct spdk_nvmf_fc_poller_api_cb_info *) arg;

		cb_info->cb_func(cb_info->cb_data, cb_info->ret);
	}
}

static void
nvmf_fc_poller_api_perform_cb(struct spdk_nvmf_fc_poller_api_cb_info *cb_info,
			      enum spdk_nvmf_fc_poller_api_ret ret)
{
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_POLLER_API, "\n");

	if (cb_info->cb_func && cb_info->cb_thread) {
		cb_info->ret = ret;
		/* callback to master thread */
		spdk_thread_send_msg(cb_info->cb_thread, nvmf_fc_poller_api_cb_event,
				     (void *) cb_info);
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_POLLER_API, "\n");
	}
}

static void
nvmf_fc_poller_api_add_connection(void *arg)
{
	enum spdk_nvmf_fc_poller_api_ret ret = SPDK_NVMF_FC_POLLER_API_SUCCESS;
	struct spdk_nvmf_fc_poller_api_add_connection_args *conn_args =
		(struct spdk_nvmf_fc_poller_api_add_connection_args *)arg;
	struct spdk_nvmf_fc_conn *fc_conn;
	bool bfound = false;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_POLLER_API, "Poller add connection, conn_id 0x%lx\n",
		      conn_args->fc_conn->conn_id);

	/* make sure connection is not already in poller's list */
	TAILQ_FOREACH(fc_conn, &conn_args->fc_conn->hwqp->connection_list,
		      link) {
		if (fc_conn->conn_id == conn_args->fc_conn->conn_id) {
			bfound = true;
			break;
		}
	}
	if (bfound) {
		SPDK_ERRLOG("duplicate connection found");
		ret = SPDK_NVMF_FC_POLLER_API_DUP_CONN_ID;
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_POLLER_API,
			      "conn_id=%lx", conn_args->fc_conn->conn_id);
		TAILQ_INSERT_TAIL(&conn_args->fc_conn->hwqp->connection_list,
				  conn_args->fc_conn, link);
		/* add qpair to nvmf poll grou */
		spdk_nvmf_poll_group_add(conn_args->fc_conn->hwqp->fc_poll_group->poll_group,
					 &conn_args->fc_conn->qpair);
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
	 * We can run into this situation if an adapter reset happens when an IT delete
	 * is in progress.
	 */
	TAILQ_FOREACH_SAFE(fc_req, &q_args->hwqp->in_use_reqs, link, tmp) {
		if (spdk_nvmf_fc_req_in_xfer(fc_req) && fc_req->is_aborted == true) {
			spdk_nvmf_fc_poller_api_func(q_args->hwqp, SPDK_NVMF_FC_POLLER_API_REQ_ABORT_COMPLETE,
						     (void *)fc_req);
		}
	}

	/* perform callback */
	nvmf_fc_poller_api_perform_cb(&q_args->cb_info, 0);
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
	return;
}

static void
nvmf_fc_poller_conn_abort_done(void *hwqp, int32_t status, void *cb_args)
{
	struct spdk_nvmf_fc_poller_api_del_connection_args *conn_args = cb_args;
	enum spdk_nvmf_fc_poller_api_ret ret = SPDK_NVMF_FC_POLLER_API_SUCCESS;

	if (conn_args->fc_request_cnt) {
		conn_args->fc_request_cnt -= 1;
	}

	if (!conn_args->fc_request_cnt) {
		if (!TAILQ_EMPTY(&conn_args->hwqp->connection_list)) {
			/* All the requests for this connection are aborted. */
			TAILQ_REMOVE(&conn_args->hwqp->connection_list,	conn_args->fc_conn, link);

			SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_POLLER_API, "Connection deleted, conn_id 0x%lx\n",
				      conn_args->fc_conn->conn_id);

			/* disconnect qpair from nvmf controller */
			spdk_nvmf_qpair_disconnect(&conn_args->fc_conn->qpair,
						   nvmf_fc_disconnect_qpair_cb, &conn_args->cb_info);
		} else {
			/*
			 * Duplicate connection delete can happen if one is
			 * coming in via an association disconnect and the other
			 * is initiated by a port reset.
			 */
			SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_POLLER_API, "Duplicate conn delete.");
			/* perform callback */
			nvmf_fc_poller_api_perform_cb(&conn_args->cb_info, ret);
		}
	}
}

static void
nvmf_fc_poller_api_del_connection(void *arg)
{
	enum spdk_nvmf_fc_poller_api_ret ret = SPDK_NVMF_FC_POLLER_API_SUCCESS;
	struct spdk_nvmf_fc_poller_api_del_connection_args *conn_args =
		(struct spdk_nvmf_fc_poller_api_del_connection_args *)arg;
	struct spdk_nvmf_fc_conn *fc_conn;
	bool bfound = false;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_POLLER_API, "Poller delete connection, conn_id 0x%lx\n",
		      conn_args->fc_conn->conn_id);

	/* find the connection in poller's list */
	TAILQ_FOREACH(fc_conn, &conn_args->hwqp->connection_list, link) {
		if (fc_conn->conn_id == conn_args->fc_conn->conn_id) {
			bfound = true;
			break;
		}
	}
	if (bfound) {
		struct spdk_nvmf_fc_request *fc_req = NULL, *tmp;
		struct spdk_nvmf_fc_hwqp *hwqp = conn_args->hwqp;

		conn_args->fc_request_cnt = 0;

		TAILQ_FOREACH_SAFE(fc_req, &hwqp->in_use_reqs, link, tmp) {
			if (fc_req->fc_conn->conn_id == fc_conn->conn_id) {
				if (spdk_nvmf_qpair_is_admin_queue(&fc_conn->qpair) &&
				    (fc_req->req.cmd->nvme_cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST)) {
					/* AER will be cleaned by spdk_nvmf_qpair_disconnect. */
					continue;
				}

				conn_args->fc_request_cnt += 1;
				spdk_nvmf_fc_req_abort(fc_req, conn_args->send_abts,
						       nvmf_fc_poller_conn_abort_done,
						       conn_args);
			}
		}

		if (!conn_args->fc_request_cnt) {
			SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_POLLER_API, "Connection deleted.\n");
			TAILQ_REMOVE(&conn_args->hwqp->connection_list,	fc_conn, link);
			/* disconnect qpair from nvmf controller */
			spdk_nvmf_qpair_disconnect(&fc_conn->qpair, nvmf_fc_disconnect_qpair_cb,
						   &conn_args->cb_info);
			return;
		} else {
			/* Will be handled in req abort callback */
			return;
		}

	} else {
		ret = SPDK_NVMF_FC_POLLER_API_NO_CONN_ID;
		/* perform callback */
		nvmf_fc_poller_api_perform_cb(&conn_args->cb_info, ret);
	}

}

static void
nvmf_fc_poller_abts_done(void *hwqp, int32_t status, void *cb_args)
{
	struct spdk_nvmf_fc_poller_api_abts_recvd_args *args = cb_args;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_POLLER_API,
		      "ABTS poller done, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
		      args->ctx->rpi, args->ctx->oxid, args->ctx->rxid);

	nvmf_fc_poller_api_perform_cb(&args->cb_info,
				      SPDK_NVMF_FC_POLLER_API_SUCCESS);
}

static void
nvmf_fc_poller_api_abts_received(void *arg)
{
	enum spdk_nvmf_fc_poller_api_ret ret = SPDK_NVMF_FC_POLLER_API_OXID_NOT_FOUND;
	struct spdk_nvmf_fc_poller_api_abts_recvd_args *args = arg;
	struct spdk_nvmf_fc_request *fc_req = NULL;
	struct spdk_nvmf_fc_hwqp *hwqp = args->hwqp;

	TAILQ_FOREACH(fc_req, &hwqp->in_use_reqs, link) {
		if ((fc_req->rpi == args->ctx->rpi) &&
		    (fc_req->oxid == args->ctx->oxid)) {
			spdk_nvmf_fc_req_abort(fc_req, false,
					       nvmf_fc_poller_abts_done, args);
			return;
		}
	}

	nvmf_fc_poller_api_perform_cb(&args->cb_info, ret);
}

static void
nvmf_fc_poller_api_queue_sync(void *arg)
{
	struct spdk_nvmf_fc_poller_api_queue_sync_args *args = arg;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_POLLER_API,
		      "HWQP sync requested for u_id = 0x%lx\n", args->u_id);

	/* Add this args to hwqp sync_cb list */
	TAILQ_INSERT_TAIL(&args->hwqp->sync_cbs, args, link);
}

static void
nvmf_fc_poller_api_queue_sync_done(void *arg)
{
	if (arg) {
		struct spdk_nvmf_fc_poller_api_queue_sync_done_args *args = arg;
		struct spdk_nvmf_fc_hwqp *hwqp = args->hwqp;
		uint64_t tag = args->tag;
		struct spdk_nvmf_fc_poller_api_queue_sync_args *sync_args = NULL, *tmp = NULL;

		TAILQ_FOREACH_SAFE(sync_args, &hwqp->sync_cbs, link, tmp) {
			if (sync_args->u_id == tag) {
				/* Queue successfully synced. Remove from cb list */
				TAILQ_REMOVE(&hwqp->sync_cbs, sync_args, link);

				SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_POLLER_API,
					      "HWQP sync done for u_id = 0x%lx\n", sync_args->u_id);

				/* Return the status to poller */
				nvmf_fc_poller_api_perform_cb(&sync_args->cb_info,
							      SPDK_NVMF_FC_POLLER_API_SUCCESS);
				return;
			}
		}

		free(arg);
	}

	/* note: no callback from this api */
}

static void
nvmf_fc_poller_api_add_hwqp(void *arg)
{
	struct spdk_nvmf_fc_hwqp *hwqp = (struct spdk_nvmf_fc_hwqp *)arg;

	hwqp->lcore_id = spdk_env_get_current_core(); /* for tracing purposes only */
	TAILQ_INSERT_TAIL(&hwqp->fc_poll_group->hwqp_list, hwqp, link);
	/* note: no callback from this api */
}

static void
nvmf_fc_poller_api_remove_hwqp(void *arg)
{
	struct spdk_nvmf_fc_hwqp *hwqp = (struct spdk_nvmf_fc_hwqp *)arg;
	struct spdk_nvmf_fc_poll_group *fc_poll_group = hwqp->fc_poll_group;

	TAILQ_REMOVE(&fc_poll_group->hwqp_list, hwqp, link);
	hwqp->fc_poll_group = NULL;
	/* note: no callback from this api */
}

enum spdk_nvmf_fc_poller_api_ret
spdk_nvmf_fc_poller_api_func(struct spdk_nvmf_fc_hwqp *hwqp, enum spdk_nvmf_fc_poller_api api,
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
				     spdk_nvmf_fc_req_abort_complete, api_args);
		break;

	case SPDK_NVMF_FC_POLLER_API_QUEUE_SYNC:
		spdk_thread_send_msg(hwqp->thread,
				     nvmf_fc_poller_api_queue_sync, api_args);
		break;

	case SPDK_NVMF_FC_POLLER_API_QUEUE_SYNC_DONE:
		spdk_thread_send_msg(hwqp->thread,
				     nvmf_fc_poller_api_queue_sync_done, api_args);
		break;

	case SPDK_NVMF_FC_ADD_HWQP_TO_POLLER:
		spdk_thread_send_msg(hwqp->thread, nvmf_fc_poller_api_add_hwqp, (void *) hwqp);
		break;

	case SPDK_NVMF_FC_REMOVE_HWQP_FROM_POLLER:
		spdk_thread_send_msg(hwqp->thread, nvmf_fc_poller_api_remove_hwqp, (void *) hwqp);
		break;

	case SPDK_NVMF_FC_POLLER_API_ADAPTER_EVENT:
	case SPDK_NVMF_FC_POLLER_API_AEN:
	default:
		SPDK_ERRLOG("BAD ARG!");
		return SPDK_NVMF_FC_POLLER_API_INVALID_ARG;
	}

	return SPDK_NVMF_FC_POLLER_API_SUCCESS;
}

SPDK_LOG_REGISTER_COMPONENT("nvmf_fc_poller_api", SPDK_LOG_NVMF_FC_POLLER_API)
