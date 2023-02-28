/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   Copyright (c) 2018-2019 Broadcom.  All Rights Reserved.
 *   The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * NVMe_FC transport functions.
 */

#include "spdk/env.h"
#include "spdk/assert.h"
#include "spdk/nvmf_transport.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/util.h"
#include "spdk/likely.h"
#include "spdk/endian.h"
#include "spdk/log.h"
#include "spdk/thread.h"

#include "nvmf_fc.h"
#include "fc_lld.h"

#include "spdk_internal/trace_defs.h"

#ifndef DEV_VERIFY
#define DEV_VERIFY assert
#endif

#ifndef ASSERT_SPDK_FC_MAIN_THREAD
#define ASSERT_SPDK_FC_MAIN_THREAD() \
        DEV_VERIFY(spdk_get_thread() == nvmf_fc_get_main_thread());
#endif

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
	"SPDK_NVMF_FC_REQ_PENDING",
	"SPDK_NVMF_FC_REQ_FUSED_WAITING"
};

#define HWQP_CONN_TABLE_SIZE			8192
#define HWQP_RPI_TABLE_SIZE			4096

SPDK_TRACE_REGISTER_FN(nvmf_fc_trace, "nvmf_fc", TRACE_GROUP_NVMF_FC)
{
	spdk_trace_register_object(OBJECT_NVMF_FC_IO, 'r');
	spdk_trace_register_description("FC_NEW",
					TRACE_FC_REQ_INIT,
					OWNER_NONE, OBJECT_NVMF_FC_IO, 1,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_READ_SBMT_TO_BDEV",
					TRACE_FC_REQ_READ_BDEV,
					OWNER_NONE, OBJECT_NVMF_FC_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_READ_XFER_DATA",
					TRACE_FC_REQ_READ_XFER,
					OWNER_NONE, OBJECT_NVMF_FC_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_READ_RSP",
					TRACE_FC_REQ_READ_RSP,
					OWNER_NONE, OBJECT_NVMF_FC_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_WRITE_NEED_BUFFER",
					TRACE_FC_REQ_WRITE_BUFFS,
					OWNER_NONE, OBJECT_NVMF_FC_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_WRITE_XFER_DATA",
					TRACE_FC_REQ_WRITE_XFER,
					OWNER_NONE, OBJECT_NVMF_FC_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_WRITE_SBMT_TO_BDEV",
					TRACE_FC_REQ_WRITE_BDEV,
					OWNER_NONE, OBJECT_NVMF_FC_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_WRITE_RSP",
					TRACE_FC_REQ_WRITE_RSP,
					OWNER_NONE, OBJECT_NVMF_FC_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_NONE_SBMT_TO_BDEV",
					TRACE_FC_REQ_NONE_BDEV,
					OWNER_NONE, OBJECT_NVMF_FC_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_NONE_RSP",
					TRACE_FC_REQ_NONE_RSP,
					OWNER_NONE, OBJECT_NVMF_FC_IO, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_SUCCESS",
					TRACE_FC_REQ_SUCCESS,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_FAILED",
					TRACE_FC_REQ_FAILED,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_ABRT",
					TRACE_FC_REQ_ABORTED,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_ABRT_SBMT_TO_BDEV",
					TRACE_FC_REQ_BDEV_ABORTED,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_PENDING",
					TRACE_FC_REQ_PENDING,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
	spdk_trace_register_description("FC_FUSED_WAITING",
					TRACE_FC_REQ_FUSED_WAITING,
					OWNER_NONE, OBJECT_NONE, 0,
					SPDK_TRACE_ARG_TYPE_INT, "");
}

/**
 * The structure used by all fc adm functions
 */
struct spdk_nvmf_fc_adm_api_data {
	void *api_args;
	spdk_nvmf_fc_callback cb_func;
};

/**
 * The callback structure for nport-delete
 */
struct spdk_nvmf_fc_adm_nport_del_cb_data {
	struct spdk_nvmf_fc_nport *nport;
	uint8_t port_handle;
	spdk_nvmf_fc_callback fc_cb_func;
	void *fc_cb_ctx;
};

/**
 * The callback structure for it-delete
 */
struct spdk_nvmf_fc_adm_i_t_del_cb_data {
	struct spdk_nvmf_fc_nport *nport;
	struct spdk_nvmf_fc_remote_port_info *rport;
	uint8_t port_handle;
	spdk_nvmf_fc_callback fc_cb_func;
	void *fc_cb_ctx;
};


typedef void (*spdk_nvmf_fc_adm_i_t_delete_assoc_cb_fn)(void *arg, uint32_t err);

/**
 * The callback structure for the it-delete-assoc callback
 */
struct spdk_nvmf_fc_adm_i_t_del_assoc_cb_data {
	struct spdk_nvmf_fc_nport *nport;
	struct spdk_nvmf_fc_remote_port_info *rport;
	uint8_t port_handle;
	spdk_nvmf_fc_adm_i_t_delete_assoc_cb_fn cb_func;
	void *cb_ctx;
};

/*
 * Call back function pointer for HW port quiesce.
 */
typedef void (*spdk_nvmf_fc_adm_hw_port_quiesce_cb_fn)(void *ctx, int err);

/**
 * Context structure for quiescing a hardware port
 */
struct spdk_nvmf_fc_adm_hw_port_quiesce_ctx {
	int quiesce_count;
	void *ctx;
	spdk_nvmf_fc_adm_hw_port_quiesce_cb_fn cb_func;
};

/**
 * Context structure used to reset a hardware port
 */
struct spdk_nvmf_fc_adm_hw_port_reset_ctx {
	void *reset_args;
	spdk_nvmf_fc_callback reset_cb_func;
};

struct spdk_nvmf_fc_transport {
	struct spdk_nvmf_transport transport;
	struct spdk_poller *accept_poller;
	pthread_mutex_t lock;
};

static struct spdk_nvmf_fc_transport *g_nvmf_ftransport;

static spdk_nvmf_transport_destroy_done_cb g_transport_destroy_done_cb = NULL;

static TAILQ_HEAD(, spdk_nvmf_fc_port) g_spdk_nvmf_fc_port_list =
	TAILQ_HEAD_INITIALIZER(g_spdk_nvmf_fc_port_list);

static struct spdk_thread *g_nvmf_fc_main_thread = NULL;

static uint32_t g_nvmf_fgroup_count = 0;
static TAILQ_HEAD(, spdk_nvmf_fc_poll_group) g_nvmf_fgroups =
	TAILQ_HEAD_INITIALIZER(g_nvmf_fgroups);

struct spdk_thread *
nvmf_fc_get_main_thread(void)
{
	return g_nvmf_fc_main_thread;
}

static inline void
nvmf_fc_record_req_trace_point(struct spdk_nvmf_fc_request *fc_req,
			       enum spdk_nvmf_fc_request_state state)
{
	uint16_t tpoint_id = SPDK_TRACE_MAX_TPOINT_ID;

	switch (state) {
	case SPDK_NVMF_FC_REQ_INIT:
		/* Start IO tracing */
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
	case SPDK_NVMF_FC_REQ_FUSED_WAITING:
		tpoint_id = TRACE_FC_REQ_FUSED_WAITING;
		break;
	default:
		assert(0);
		break;
	}
	if (tpoint_id != SPDK_TRACE_MAX_TPOINT_ID) {
		spdk_trace_record(tpoint_id, fc_req->poller_lcore, 0,
				  (uint64_t)(&fc_req->req));
	}
}

static struct rte_hash *
nvmf_fc_create_hash_table(const char *name, size_t num_entries, size_t key_len)
{
	struct rte_hash_parameters hash_params = { 0 };

	hash_params.entries = num_entries;
	hash_params.key_len = key_len;
	hash_params.name = name;

	return rte_hash_create(&hash_params);
}

void
nvmf_fc_free_conn_reqpool(struct spdk_nvmf_fc_conn *fc_conn)
{
	free(fc_conn->pool_memory);
	fc_conn->pool_memory = NULL;
}

int
nvmf_fc_create_conn_reqpool(struct spdk_nvmf_fc_conn *fc_conn)
{
	uint32_t i, qd;
	struct spdk_nvmf_fc_pooled_request *req;

	/*
	 * Create number of fc-requests to be more than the actual SQ size.
	 * This is to handle race conditions where the target driver may send
	 * back a RSP and before the target driver gets to process the CQE
	 * for the RSP, the initiator may have sent a new command.
	 * Depending on the load on the HWQP, there is a slim possibility
	 * that the target reaps the RQE corresponding to the new
	 * command before processing the CQE corresponding to the RSP.
	 */
	qd = fc_conn->max_queue_depth * 2;

	STAILQ_INIT(&fc_conn->pool_queue);
	fc_conn->pool_memory = calloc((fc_conn->max_queue_depth * 2),
				      sizeof(struct spdk_nvmf_fc_request));
	if (!fc_conn->pool_memory) {
		SPDK_ERRLOG("create fc req ring objects failed\n");
		goto error;
	}
	fc_conn->pool_size = qd;
	fc_conn->pool_free_elems = qd;

	/* Initialise value in ring objects and link the objects */
	for (i = 0; i < qd; i++) {
		req = (struct spdk_nvmf_fc_pooled_request *)((char *)fc_conn->pool_memory +
				i * sizeof(struct spdk_nvmf_fc_request));

		STAILQ_INSERT_TAIL(&fc_conn->pool_queue, req, pool_link);
	}
	return 0;
error:
	nvmf_fc_free_conn_reqpool(fc_conn);
	return -1;
}

static inline struct spdk_nvmf_fc_request *
nvmf_fc_conn_alloc_fc_request(struct spdk_nvmf_fc_conn *fc_conn)
{
	struct spdk_nvmf_fc_request *fc_req;
	struct spdk_nvmf_fc_pooled_request *pooled_req;
	struct spdk_nvmf_fc_hwqp *hwqp = fc_conn->hwqp;

	pooled_req = STAILQ_FIRST(&fc_conn->pool_queue);
	if (!pooled_req) {
		SPDK_ERRLOG("Alloc request buffer failed\n");
		return NULL;
	}
	STAILQ_REMOVE_HEAD(&fc_conn->pool_queue, pool_link);
	fc_conn->pool_free_elems -= 1;

	fc_req = (struct spdk_nvmf_fc_request *)pooled_req;
	memset(fc_req, 0, sizeof(struct spdk_nvmf_fc_request));
	nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_INIT);

	TAILQ_INSERT_TAIL(&hwqp->in_use_reqs, fc_req, link);
	TAILQ_INSERT_TAIL(&fc_conn->in_use_reqs, fc_req, conn_link);
	TAILQ_INIT(&fc_req->abort_cbs);
	return fc_req;
}

static inline void
nvmf_fc_conn_free_fc_request(struct spdk_nvmf_fc_conn *fc_conn, struct spdk_nvmf_fc_request *fc_req)
{
	if (fc_req->state != SPDK_NVMF_FC_REQ_SUCCESS) {
		/* Log an error for debug purpose. */
		nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_FAILED);
	}

	/* set the magic to mark req as no longer valid. */
	fc_req->magic = 0xDEADBEEF;

	TAILQ_REMOVE(&fc_conn->hwqp->in_use_reqs, fc_req, link);
	TAILQ_REMOVE(&fc_conn->in_use_reqs, fc_req, conn_link);

	STAILQ_INSERT_HEAD(&fc_conn->pool_queue, (struct spdk_nvmf_fc_pooled_request *)fc_req, pool_link);
	fc_conn->pool_free_elems += 1;
}

static inline void
nvmf_fc_request_remove_from_pending(struct spdk_nvmf_fc_request *fc_req)
{
	STAILQ_REMOVE(&fc_req->hwqp->fgroup->group.pending_buf_queue, &fc_req->req,
		      spdk_nvmf_request, buf_link);
}

int
nvmf_fc_init_hwqp(struct spdk_nvmf_fc_port *fc_port, struct spdk_nvmf_fc_hwqp *hwqp)
{
	char name[64];

	hwqp->fc_port = fc_port;

	/* clear counters */
	memset(&hwqp->counters, 0, sizeof(struct spdk_nvmf_fc_errors));

	TAILQ_INIT(&hwqp->in_use_reqs);
	TAILQ_INIT(&hwqp->sync_cbs);
	TAILQ_INIT(&hwqp->ls_pending_queue);

	snprintf(name, sizeof(name), "nvmf_fc_conn_hash:%d-%d", fc_port->port_hdl, hwqp->hwqp_id);
	hwqp->connection_list_hash = nvmf_fc_create_hash_table(name, HWQP_CONN_TABLE_SIZE,
				     sizeof(uint64_t));
	if (!hwqp->connection_list_hash) {
		SPDK_ERRLOG("Failed to create connection hash table.\n");
		return -ENOMEM;
	}

	snprintf(name, sizeof(name), "nvmf_fc_rpi_hash:%d-%d", fc_port->port_hdl, hwqp->hwqp_id);
	hwqp->rport_list_hash = nvmf_fc_create_hash_table(name, HWQP_RPI_TABLE_SIZE, sizeof(uint16_t));
	if (!hwqp->rport_list_hash) {
		SPDK_ERRLOG("Failed to create rpi hash table.\n");
		rte_hash_free(hwqp->connection_list_hash);
		return -ENOMEM;
	}

	/* Init low level driver queues */
	nvmf_fc_init_q(hwqp);
	return 0;
}

static struct spdk_nvmf_fc_poll_group *
nvmf_fc_assign_idlest_poll_group(struct spdk_nvmf_fc_hwqp *hwqp)
{
	uint32_t max_count = UINT32_MAX;
	struct spdk_nvmf_fc_poll_group *fgroup;
	struct spdk_nvmf_fc_poll_group *ret_fgroup = NULL;

	pthread_mutex_lock(&g_nvmf_ftransport->lock);
	/* find poll group with least number of hwqp's assigned to it */
	TAILQ_FOREACH(fgroup, &g_nvmf_fgroups, link) {
		if (fgroup->hwqp_count < max_count) {
			ret_fgroup = fgroup;
			max_count = fgroup->hwqp_count;
		}
	}

	if (ret_fgroup) {
		ret_fgroup->hwqp_count++;
		hwqp->thread = ret_fgroup->group.group->thread;
		hwqp->fgroup = ret_fgroup;
	}

	pthread_mutex_unlock(&g_nvmf_ftransport->lock);

	return ret_fgroup;
}

bool
nvmf_fc_poll_group_valid(struct spdk_nvmf_fc_poll_group *fgroup)
{
	struct spdk_nvmf_fc_poll_group *tmp;
	bool rc = false;

	pthread_mutex_lock(&g_nvmf_ftransport->lock);
	TAILQ_FOREACH(tmp, &g_nvmf_fgroups, link) {
		if (tmp == fgroup) {
			rc = true;
			break;
		}
	}
	pthread_mutex_unlock(&g_nvmf_ftransport->lock);
	return rc;
}

void
nvmf_fc_poll_group_add_hwqp(struct spdk_nvmf_fc_hwqp *hwqp)
{
	assert(hwqp);
	if (hwqp == NULL) {
		SPDK_ERRLOG("Error: hwqp is NULL\n");
		return;
	}

	assert(g_nvmf_fgroup_count);

	if (!nvmf_fc_assign_idlest_poll_group(hwqp)) {
		SPDK_ERRLOG("Could not assign poll group for hwqp (%d)\n", hwqp->hwqp_id);
		return;
	}

	nvmf_fc_poller_api_func(hwqp, SPDK_NVMF_FC_POLLER_API_ADD_HWQP, NULL);
}

static void
nvmf_fc_poll_group_remove_hwqp_cb(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret)
{
	struct spdk_nvmf_fc_poller_api_remove_hwqp_args *args = cb_data;

	if (ret == SPDK_NVMF_FC_POLLER_API_SUCCESS) {
		SPDK_DEBUGLOG(nvmf_fc_adm_api,
			      "Remove hwqp%d from fgroup success\n", args->hwqp->hwqp_id);
	} else {
		SPDK_ERRLOG("Remove hwqp%d from fgroup failed.\n", args->hwqp->hwqp_id);
	}

	if (args->cb_fn) {
		args->cb_fn(args->cb_ctx, 0);
	}

	free(args);
}

void
nvmf_fc_poll_group_remove_hwqp(struct spdk_nvmf_fc_hwqp *hwqp,
			       spdk_nvmf_fc_remove_hwqp_cb cb_fn, void *cb_ctx)
{
	struct spdk_nvmf_fc_poller_api_remove_hwqp_args *args;
	struct spdk_nvmf_fc_poll_group *tmp;
	int rc = 0;

	assert(hwqp);

	SPDK_DEBUGLOG(nvmf_fc,
		      "Remove hwqp from poller: for port: %d, hwqp: %d\n",
		      hwqp->fc_port->port_hdl, hwqp->hwqp_id);

	if (!hwqp->fgroup) {
		SPDK_ERRLOG("HWQP (%d) not assigned to poll group\n", hwqp->hwqp_id);
	} else {
		pthread_mutex_lock(&g_nvmf_ftransport->lock);
		TAILQ_FOREACH(tmp, &g_nvmf_fgroups, link) {
			if (tmp == hwqp->fgroup) {
				hwqp->fgroup->hwqp_count--;
				break;
			}
		}
		pthread_mutex_unlock(&g_nvmf_ftransport->lock);

		if (tmp != hwqp->fgroup) {
			/* Pollgroup was already removed. Dont bother. */
			goto done;
		}

		args = calloc(1, sizeof(struct spdk_nvmf_fc_poller_api_remove_hwqp_args));
		if (args == NULL) {
			rc = -ENOMEM;
			SPDK_ERRLOG("Failed to allocate memory for poller remove hwqp:%d\n", hwqp->hwqp_id);
			goto done;
		}

		args->hwqp   = hwqp;
		args->cb_fn  = cb_fn;
		args->cb_ctx = cb_ctx;
		args->cb_info.cb_func = nvmf_fc_poll_group_remove_hwqp_cb;
		args->cb_info.cb_data = args;
		args->cb_info.cb_thread = spdk_get_thread();

		rc = nvmf_fc_poller_api_func(hwqp, SPDK_NVMF_FC_POLLER_API_REMOVE_HWQP, args);
		if (rc) {
			rc = -EINVAL;
			SPDK_ERRLOG("Remove hwqp%d from fgroup failed.\n", hwqp->hwqp_id);
			free(args);
			goto done;
		}
		return;
	}
done:
	if (cb_fn) {
		cb_fn(cb_ctx, rc);
	}
}

/*
 * Note: This needs to be used only on main poller.
 */
static uint64_t
nvmf_fc_get_abts_unique_id(void)
{
	static uint32_t u_id = 0;

	return (uint64_t)(++u_id);
}

static void
nvmf_fc_queue_synced_cb(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret)
{
	struct spdk_nvmf_fc_abts_ctx *ctx = cb_data;
	struct spdk_nvmf_fc_poller_api_abts_recvd_args *args, *poller_arg;

	ctx->hwqps_responded++;

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

	SPDK_DEBUGLOG(nvmf_fc,
		      "QueueSync(0x%lx) completed for nport: %d, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
		      ctx->u_id, ctx->nport->nport_hdl, ctx->rpi, ctx->oxid, ctx->rxid);

	/* Resend ABTS to pollers */
	args = ctx->abts_poller_args;
	for (int i = 0; i < ctx->num_hwqps; i++) {
		poller_arg = args + i;
		nvmf_fc_poller_api_func(poller_arg->hwqp,
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
	if (!nvmf_fc_q_sync_available()) {
		return -EPERM;
	}

	assert(ctx);
	if (!ctx) {
		SPDK_ERRLOG("NULL ctx pointer");
		return -EINVAL;
	}

	/* Reset the ctx values */
	ctx->hwqps_responded = 0;

	args = calloc(ctx->num_hwqps,
		      sizeof(struct spdk_nvmf_fc_poller_api_queue_sync_args));
	if (!args) {
		SPDK_ERRLOG("QueueSync(0x%lx) failed for nport: %d, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
			    ctx->u_id, ctx->nport->nport_hdl, ctx->rpi, ctx->oxid, ctx->rxid);
		return -ENOMEM;
	}
	ctx->sync_poller_args = args;

	abts_args = ctx->abts_poller_args;
	for (int i = 0; i < ctx->num_hwqps; i++) {
		abts_poller_arg = abts_args + i;
		poller_arg = args + i;
		poller_arg->u_id = ctx->u_id;
		poller_arg->hwqp = abts_poller_arg->hwqp;
		poller_arg->cb_info.cb_func = nvmf_fc_queue_synced_cb;
		poller_arg->cb_info.cb_data = ctx;
		poller_arg->cb_info.cb_thread = spdk_get_thread();

		/* Send a Queue sync message to interested pollers */
		nvmf_fc_poller_api_func(poller_arg->hwqp,
					SPDK_NVMF_FC_POLLER_API_QUEUE_SYNC,
					poller_arg);
	}

	SPDK_DEBUGLOG(nvmf_fc,
		      "QueueSync(0x%lx) Sent for nport: %d, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
		      ctx->u_id, ctx->nport->nport_hdl, ctx->rpi, ctx->oxid, ctx->rxid);

	/* Post Marker to queue to track aborted request */
	nvmf_fc_issue_q_sync(ctx->ls_hwqp, ctx->u_id, ctx->fcp_rq_id);

	return 0;
}

static void
nvmf_fc_abts_handled_cb(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret)
{
	struct spdk_nvmf_fc_abts_ctx *ctx = cb_data;
	struct spdk_nvmf_fc_nport *nport  = NULL;

	if (ret != SPDK_NVMF_FC_POLLER_API_OXID_NOT_FOUND) {
		ctx->handled = true;
	}

	ctx->hwqps_responded++;

	if (ctx->hwqps_responded < ctx->num_hwqps) {
		/* Wait for all pollers to complete. */
		return;
	}

	nport = nvmf_fc_nport_find(ctx->port_hdl, ctx->nport_hdl);

	if (ctx->nport != nport) {
		/* Nport can be deleted while this abort is being
		 * processed by the pollers.
		 */
		SPDK_NOTICELOG("nport_%d deleted while processing ABTS frame, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
			       ctx->nport_hdl, ctx->rpi, ctx->oxid, ctx->rxid);
	} else {
		if (!ctx->handled) {
			/* Try syncing the queues and try one more time */
			if (!ctx->queue_synced && (nvmf_fc_handle_abts_notfound(ctx) == 0)) {
				SPDK_DEBUGLOG(nvmf_fc,
					      "QueueSync(0x%lx) for nport: %d, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
					      ctx->u_id, ctx->nport->nport_hdl, ctx->rpi, ctx->oxid, ctx->rxid);
				return;
			} else {
				/* Send Reject */
				nvmf_fc_xmt_bls_rsp(&ctx->nport->fc_port->ls_queue,
						    ctx->oxid, ctx->rxid, ctx->rpi, true,
						    FCNVME_BLS_REJECT_EXP_INVALID_OXID, NULL, NULL);
			}
		} else {
			/* Send Accept */
			nvmf_fc_xmt_bls_rsp(&ctx->nport->fc_port->ls_queue,
					    ctx->oxid, ctx->rxid, ctx->rpi, false,
					    0, NULL, NULL);
		}
	}
	SPDK_NOTICELOG("BLS_%s sent for ABTS frame nport: %d, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
		       (ctx->handled) ? "ACC" : "REJ", ctx->nport->nport_hdl, ctx->rpi, ctx->oxid, ctx->rxid);

	free(ctx->abts_poller_args);
	free(ctx);
}

void
nvmf_fc_handle_abts_frame(struct spdk_nvmf_fc_nport *nport, uint16_t rpi,
			  uint16_t oxid, uint16_t rxid)
{
	struct spdk_nvmf_fc_abts_ctx *ctx = NULL;
	struct spdk_nvmf_fc_poller_api_abts_recvd_args *args = NULL, *poller_arg;
	struct spdk_nvmf_fc_association *assoc = NULL;
	struct spdk_nvmf_fc_conn *conn = NULL;
	uint32_t hwqp_cnt = 0;
	bool skip_hwqp_cnt;
	struct spdk_nvmf_fc_hwqp **hwqps = NULL;
	uint32_t i;

	SPDK_NOTICELOG("Handle ABTS frame for nport: %d, rpi: 0x%x, oxid: 0x%x, rxid: 0x%x\n",
		       nport->nport_hdl, rpi, oxid, rxid);

	/* Allocate memory to track hwqp's with at least 1 active connection. */
	hwqps = calloc(nport->fc_port->num_io_queues, sizeof(struct spdk_nvmf_fc_hwqp *));
	if (hwqps == NULL) {
		SPDK_ERRLOG("Unable to allocate temp. hwqp array for abts processing!\n");
		goto bls_rej;
	}

	TAILQ_FOREACH(assoc, &nport->fc_associations, link) {
		TAILQ_FOREACH(conn, &assoc->fc_conns, assoc_link) {
			if ((conn->rpi != rpi) || !conn->hwqp) {
				continue;
			}

			skip_hwqp_cnt = false;
			for (i = 0; i < hwqp_cnt; i++) {
				if (hwqps[i] == conn->hwqp) {
					/* Skip. This is already present */
					skip_hwqp_cnt = true;
					break;
				}
			}
			if (!skip_hwqp_cnt) {
				assert(hwqp_cnt < nport->fc_port->num_io_queues);
				hwqps[hwqp_cnt] = conn->hwqp;
				hwqp_cnt++;
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

	for (i = 0; i < hwqp_cnt; i++) {
		poller_arg = args + i;
		poller_arg->hwqp = hwqps[i];
		poller_arg->cb_info.cb_func = nvmf_fc_abts_handled_cb;
		poller_arg->cb_info.cb_data = ctx;
		poller_arg->cb_info.cb_thread = spdk_get_thread();
		poller_arg->ctx = ctx;

		nvmf_fc_poller_api_func(poller_arg->hwqp,
					SPDK_NVMF_FC_POLLER_API_ABTS_RECEIVED,
					poller_arg);
	}

	free(hwqps);

	return;
bls_rej:
	free(args);
	free(hwqps);

	/* Send Reject */
	nvmf_fc_xmt_bls_rsp(&nport->fc_port->ls_queue, oxid, rxid, rpi,
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
nvmf_fc_port_is_offline(struct spdk_nvmf_fc_port *fc_port)
{
	if (fc_port && (fc_port->hw_port_status == SPDK_FC_PORT_OFFLINE)) {
		return true;
	}

	return false;
}

/*
 * Returns true if the port is in online state.
 */
bool
nvmf_fc_port_is_online(struct spdk_nvmf_fc_port *fc_port)
{
	if (fc_port && (fc_port->hw_port_status == SPDK_FC_PORT_ONLINE)) {
		return true;
	}

	return false;
}

int
nvmf_fc_port_set_online(struct spdk_nvmf_fc_port *fc_port)
{
	if (fc_port && (fc_port->hw_port_status != SPDK_FC_PORT_ONLINE)) {
		fc_port->hw_port_status = SPDK_FC_PORT_ONLINE;
		return 0;
	}

	return -EPERM;
}

int
nvmf_fc_port_set_offline(struct spdk_nvmf_fc_port *fc_port)
{
	if (fc_port && (fc_port->hw_port_status != SPDK_FC_PORT_OFFLINE)) {
		fc_port->hw_port_status = SPDK_FC_PORT_OFFLINE;
		return 0;
	}

	return -EPERM;
}

int
nvmf_fc_hwqp_set_online(struct spdk_nvmf_fc_hwqp *hwqp)
{
	if (hwqp && (hwqp->state != SPDK_FC_HWQP_ONLINE)) {
		hwqp->state = SPDK_FC_HWQP_ONLINE;
		/* reset some queue counters */
		hwqp->num_conns = 0;
		return nvmf_fc_set_q_online_state(hwqp, true);
	}

	return -EPERM;
}

int
nvmf_fc_hwqp_set_offline(struct spdk_nvmf_fc_hwqp *hwqp)
{
	if (hwqp && (hwqp->state != SPDK_FC_HWQP_OFFLINE)) {
		hwqp->state = SPDK_FC_HWQP_OFFLINE;
		return nvmf_fc_set_q_online_state(hwqp, false);
	}

	return -EPERM;
}

void
nvmf_fc_port_add(struct spdk_nvmf_fc_port *fc_port)
{
	TAILQ_INSERT_TAIL(&g_spdk_nvmf_fc_port_list, fc_port, link);

	/*
	 * Let LLD add the port to its list.
	 */
	nvmf_fc_lld_port_add(fc_port);
}

static void
nvmf_fc_port_remove(struct spdk_nvmf_fc_port *fc_port)
{
	TAILQ_REMOVE(&g_spdk_nvmf_fc_port_list, fc_port, link);

	/*
	 * Let LLD remove the port from its list.
	 */
	nvmf_fc_lld_port_remove(fc_port);
}

struct spdk_nvmf_fc_port *
nvmf_fc_port_lookup(uint8_t port_hdl)
{
	struct spdk_nvmf_fc_port *fc_port = NULL;

	TAILQ_FOREACH(fc_port, &g_spdk_nvmf_fc_port_list, link) {
		if (fc_port->port_hdl == port_hdl) {
			return fc_port;
		}
	}
	return NULL;
}

uint32_t
nvmf_fc_get_prli_service_params(void)
{
	return (SPDK_NVMF_FC_DISCOVERY_SERVICE | SPDK_NVMF_FC_TARGET_FUNCTION);
}

int
nvmf_fc_port_add_nport(struct spdk_nvmf_fc_port *fc_port,
		       struct spdk_nvmf_fc_nport *nport)
{
	if (fc_port) {
		TAILQ_INSERT_TAIL(&fc_port->nport_list, nport, link);
		fc_port->num_nports++;
		return 0;
	}

	return -EINVAL;
}

int
nvmf_fc_port_remove_nport(struct spdk_nvmf_fc_port *fc_port,
			  struct spdk_nvmf_fc_nport *nport)
{
	if (fc_port && nport) {
		TAILQ_REMOVE(&fc_port->nport_list, nport, link);
		fc_port->num_nports--;
		return 0;
	}

	return -EINVAL;
}

static struct spdk_nvmf_fc_nport *
nvmf_fc_nport_hdl_lookup(struct spdk_nvmf_fc_port *fc_port, uint16_t nport_hdl)
{
	struct spdk_nvmf_fc_nport *fc_nport = NULL;

	TAILQ_FOREACH(fc_nport, &fc_port->nport_list, link) {
		if (fc_nport->nport_hdl == nport_hdl) {
			return fc_nport;
		}
	}

	return NULL;
}

struct spdk_nvmf_fc_nport *
nvmf_fc_nport_find(uint8_t port_hdl, uint16_t nport_hdl)
{
	struct spdk_nvmf_fc_port *fc_port = NULL;

	fc_port = nvmf_fc_port_lookup(port_hdl);
	if (fc_port) {
		return nvmf_fc_nport_hdl_lookup(fc_port, nport_hdl);
	}

	return NULL;
}

static inline int
nvmf_fc_hwqp_find_nport_and_rport(struct spdk_nvmf_fc_hwqp *hwqp,
				  uint32_t d_id, struct spdk_nvmf_fc_nport **nport,
				  uint32_t s_id, struct spdk_nvmf_fc_remote_port_info **rport)
{
	struct spdk_nvmf_fc_nport *n_port;
	struct spdk_nvmf_fc_remote_port_info *r_port;

	assert(hwqp);
	if (hwqp == NULL) {
		SPDK_ERRLOG("Error: hwqp is NULL\n");
		return -EINVAL;
	}
	assert(nport);
	if (nport == NULL) {
		SPDK_ERRLOG("Error: nport is NULL\n");
		return -EINVAL;
	}
	assert(rport);
	if (rport == NULL) {
		SPDK_ERRLOG("Error: rport is NULL\n");
		return -EINVAL;
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

	return -ENOENT;
}

/* Returns true if the Nport is empty of all rem_ports */
bool
nvmf_fc_nport_has_no_rport(struct spdk_nvmf_fc_nport *nport)
{
	if (nport && TAILQ_EMPTY(&nport->rem_port_list)) {
		assert(nport->rport_count == 0);
		return true;
	} else {
		return false;
	}
}

int
nvmf_fc_nport_set_state(struct spdk_nvmf_fc_nport *nport,
			enum spdk_nvmf_fc_object_state state)
{
	if (nport) {
		nport->nport_state = state;
		return 0;
	} else {
		return -EINVAL;
	}
}

bool
nvmf_fc_nport_add_rem_port(struct spdk_nvmf_fc_nport *nport,
			   struct spdk_nvmf_fc_remote_port_info *rem_port)
{
	if (nport && rem_port) {
		TAILQ_INSERT_TAIL(&nport->rem_port_list, rem_port, link);
		nport->rport_count++;
		return 0;
	} else {
		return -EINVAL;
	}
}

bool
nvmf_fc_nport_remove_rem_port(struct spdk_nvmf_fc_nport *nport,
			      struct spdk_nvmf_fc_remote_port_info *rem_port)
{
	if (nport && rem_port) {
		TAILQ_REMOVE(&nport->rem_port_list, rem_port, link);
		nport->rport_count--;
		return 0;
	} else {
		return -EINVAL;
	}
}

int
nvmf_fc_rport_set_state(struct spdk_nvmf_fc_remote_port_info *rport,
			enum spdk_nvmf_fc_object_state state)
{
	if (rport) {
		rport->rport_state = state;
		return 0;
	} else {
		return -EINVAL;
	}
}
int
nvmf_fc_assoc_set_state(struct spdk_nvmf_fc_association *assoc,
			enum spdk_nvmf_fc_object_state state)
{
	if (assoc) {
		assoc->assoc_state = state;
		return 0;
	} else {
		return -EINVAL;
	}
}

static struct spdk_nvmf_fc_association *
nvmf_ctrlr_get_fc_assoc(struct spdk_nvmf_ctrlr *ctrlr)
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

bool
nvmf_ctrlr_is_on_nport(uint8_t port_hdl, uint16_t nport_hdl,
		       struct spdk_nvmf_ctrlr *ctrlr)
{
	struct spdk_nvmf_fc_nport *fc_nport = NULL;
	struct spdk_nvmf_fc_association *assoc = NULL;

	if (!ctrlr) {
		return false;
	}

	fc_nport = nvmf_fc_nport_find(port_hdl, nport_hdl);
	if (!fc_nport) {
		return false;
	}

	assoc = nvmf_ctrlr_get_fc_assoc(ctrlr);
	if (assoc && assoc->tgtport == fc_nport) {
		SPDK_DEBUGLOG(nvmf_fc,
			      "Controller: %d corresponding to association: %p(%lu:%d) is on port: %d nport: %d\n",
			      ctrlr->cntlid, assoc, assoc->assoc_id, assoc->assoc_state, port_hdl,
			      nport_hdl);
		return true;
	}
	return false;
}

static void
nvmf_fc_release_ls_rqst(struct spdk_nvmf_fc_hwqp *hwqp,
			struct spdk_nvmf_fc_ls_rqst *ls_rqst)
{
	assert(ls_rqst);

	TAILQ_REMOVE(&hwqp->ls_pending_queue, ls_rqst, ls_pending_link);

	/* Return buffer to chip */
	nvmf_fc_rqpair_buffer_release(hwqp, ls_rqst->rqstbuf.buf_index);
}

static int
nvmf_fc_delete_ls_pending(struct spdk_nvmf_fc_hwqp *hwqp,
			  struct spdk_nvmf_fc_nport *nport,
			  struct spdk_nvmf_fc_remote_port_info *rport)
{
	struct spdk_nvmf_fc_ls_rqst *ls_rqst = NULL, *tmp;
	int num_deleted = 0;

	assert(hwqp);
	assert(nport);
	assert(rport);

	TAILQ_FOREACH_SAFE(ls_rqst, &hwqp->ls_pending_queue, ls_pending_link, tmp) {
		if ((ls_rqst->d_id == nport->d_id) && (ls_rqst->s_id == rport->s_id)) {
			num_deleted++;
			nvmf_fc_release_ls_rqst(hwqp, ls_rqst);
		}
	}
	return num_deleted;
}

static void
nvmf_fc_req_bdev_abort(void *arg1)
{
	struct spdk_nvmf_fc_request *fc_req = arg1;
	struct spdk_nvmf_ctrlr *ctrlr = fc_req->req.qpair->ctrlr;
	int i;

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
	/* The Fabric commands supported are
	 * Property Set
	 * Property Get
	 * Connect -> Special case (async. handling). Not sure how to
	 * handle at this point. Let it run to completion.
	 */
	for (i = 0; i < SPDK_NVMF_MAX_ASYNC_EVENTS; i++) {
		if (ctrlr->aer_req[i] == &fc_req->req) {
			SPDK_NOTICELOG("Abort AER request\n");
			nvmf_qpair_free_aer(fc_req->req.qpair);
		}
	}
}

void
nvmf_fc_request_abort_complete(void *arg1)
{
	struct spdk_nvmf_fc_request *fc_req =
		(struct spdk_nvmf_fc_request *)arg1;
	struct spdk_nvmf_fc_hwqp *hwqp = fc_req->hwqp;
	struct spdk_nvmf_fc_caller_ctx *ctx = NULL, *tmp = NULL;
	TAILQ_HEAD(, spdk_nvmf_fc_caller_ctx) abort_cbs;

	/* Make a copy of the cb list from fc_req */
	TAILQ_INIT(&abort_cbs);
	TAILQ_SWAP(&abort_cbs, &fc_req->abort_cbs, spdk_nvmf_fc_caller_ctx, link);

	SPDK_NOTICELOG("FC Request(%p) in state :%s aborted\n", fc_req,
		       fc_req_state_strs[fc_req->state]);

	_nvmf_fc_request_free(fc_req);

	/* Request abort completed. Notify all the callbacks */
	TAILQ_FOREACH_SAFE(ctx, &abort_cbs, link, tmp) {
		/* Notify */
		ctx->cb(hwqp, 0, ctx->cb_args);
		/* Remove */
		TAILQ_REMOVE(&abort_cbs, ctx, link);
		/* free */
		free(ctx);
	}
}

void
nvmf_fc_request_abort(struct spdk_nvmf_fc_request *fc_req, bool send_abts,
		      spdk_nvmf_fc_caller_cb cb, void *cb_args)
{
	struct spdk_nvmf_fc_caller_ctx *ctx = NULL;
	bool kill_req = false;

	/* Add the cb to list */
	if (cb) {
		ctx = calloc(1, sizeof(struct spdk_nvmf_fc_caller_ctx));
		if (!ctx) {
			SPDK_ERRLOG("ctx alloc failed.\n");
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
	kill_req = nvmf_fc_is_port_dead(fc_req->hwqp);
	if (kill_req && nvmf_fc_req_in_xfer(fc_req)) {
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

	switch (fc_req->state) {
	case SPDK_NVMF_FC_REQ_BDEV_ABORTED:
		/* Aborted by backend */
		goto complete;

	case SPDK_NVMF_FC_REQ_READ_BDEV:
	case SPDK_NVMF_FC_REQ_WRITE_BDEV:
	case SPDK_NVMF_FC_REQ_NONE_BDEV:
		/* Notify bdev */
		spdk_thread_send_msg(fc_req->hwqp->thread,
				     nvmf_fc_req_bdev_abort, (void *)fc_req);
		break;

	case SPDK_NVMF_FC_REQ_READ_XFER:
	case SPDK_NVMF_FC_REQ_READ_RSP:
	case SPDK_NVMF_FC_REQ_WRITE_XFER:
	case SPDK_NVMF_FC_REQ_WRITE_RSP:
	case SPDK_NVMF_FC_REQ_NONE_RSP:
		/* Notify HBA to abort this exchange  */
		nvmf_fc_issue_abort(fc_req->hwqp, fc_req->xchg, NULL, NULL);
		break;

	case SPDK_NVMF_FC_REQ_PENDING:
		/* Remove from pending */
		nvmf_fc_request_remove_from_pending(fc_req);
		goto complete;
	case SPDK_NVMF_FC_REQ_FUSED_WAITING:
		TAILQ_REMOVE(&fc_req->fc_conn->fused_waiting_queue, fc_req, fused_link);
		goto complete;
	default:
		SPDK_ERRLOG("Request in invalid state.\n");
		goto complete;
	}

	return;
complete:
	nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_ABORTED);
	nvmf_fc_poller_api_func(fc_req->hwqp, SPDK_NVMF_FC_POLLER_API_REQ_ABORT_COMPLETE,
				(void *)fc_req);
}

static int
nvmf_fc_request_alloc_buffers(struct spdk_nvmf_fc_request *fc_req)
{
	uint32_t length = fc_req->req.length;
	struct spdk_nvmf_fc_poll_group *fgroup = fc_req->hwqp->fgroup;
	struct spdk_nvmf_transport_poll_group *group = &fgroup->group;
	struct spdk_nvmf_transport *transport = group->transport;

	if (spdk_nvmf_request_get_buffers(&fc_req->req, group, transport, length)) {
		return -ENOMEM;
	}

	return 0;
}

static int
nvmf_fc_request_execute(struct spdk_nvmf_fc_request *fc_req)
{
	/* Allocate an XCHG if we dont use send frame for this command. */
	if (!nvmf_fc_use_send_frame(fc_req)) {
		fc_req->xchg = nvmf_fc_get_xri(fc_req->hwqp);
		if (!fc_req->xchg) {
			fc_req->hwqp->counters.no_xchg++;
			return -EAGAIN;
		}
	}

	if (fc_req->req.length) {
		if (nvmf_fc_request_alloc_buffers(fc_req) < 0) {
			fc_req->hwqp->counters.buf_alloc_err++;
			if (fc_req->xchg) {
				nvmf_fc_put_xchg(fc_req->hwqp, fc_req->xchg);
				fc_req->xchg = NULL;
			}
			return -EAGAIN;
		}
		fc_req->req.data = fc_req->req.iov[0].iov_base;
	}

	if (fc_req->req.xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		SPDK_DEBUGLOG(nvmf_fc, "WRITE CMD.\n");

		nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_WRITE_XFER);

		if (nvmf_fc_recv_data(fc_req)) {
			/* Dropped return success to caller */
			fc_req->hwqp->counters.unexpected_err++;
			_nvmf_fc_request_free(fc_req);
		}
	} else {
		SPDK_DEBUGLOG(nvmf_fc, "READ/NONE CMD\n");

		if (fc_req->req.xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
			nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_READ_BDEV);
		} else {
			nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_NONE_BDEV);
		}
		spdk_nvmf_request_exec(&fc_req->req);
	}

	return 0;
}

static void
nvmf_fc_set_vmid_priority(struct spdk_nvmf_fc_request *fc_req,
			  struct spdk_nvmf_fc_frame_hdr *fchdr)
{
	uint8_t df_ctl = fchdr->df_ctl;
	uint32_t f_ctl = fchdr->f_ctl;

	/* VMID */
	if (df_ctl & FCNVME_D_FCTL_DEVICE_HDR_16_MASK) {
		struct spdk_nvmf_fc_vm_header *vhdr;
		uint32_t vmhdr_offset = 0;

		if (df_ctl & FCNVME_D_FCTL_ESP_HDR_MASK) {
			vmhdr_offset += FCNVME_D_FCTL_ESP_HDR_SIZE;
		}

		if (df_ctl & FCNVME_D_FCTL_NETWORK_HDR_MASK) {
			vmhdr_offset += FCNVME_D_FCTL_NETWORK_HDR_SIZE;
		}

		vhdr = (struct spdk_nvmf_fc_vm_header *)((char *)fchdr +
				sizeof(struct spdk_nvmf_fc_frame_hdr) + vmhdr_offset);
		fc_req->app_id = from_be32(&vhdr->src_vmid);
	}

	/* Priority */
	if ((from_be32(&f_ctl) >> 8) & FCNVME_F_CTL_PRIORITY_ENABLE) {
		fc_req->csctl = fchdr->cs_ctl;
	}
}

static int
nvmf_fc_hwqp_handle_request(struct spdk_nvmf_fc_hwqp *hwqp, struct spdk_nvmf_fc_frame_hdr *frame,
			    struct spdk_nvmf_fc_buffer_desc *buffer, uint32_t plen)
{
	uint16_t cmnd_len;
	uint64_t rqst_conn_id;
	struct spdk_nvmf_fc_request *fc_req = NULL;
	struct spdk_nvmf_fc_cmnd_iu *cmd_iu = NULL;
	struct spdk_nvmf_fc_conn *fc_conn = NULL;
	enum spdk_nvme_data_transfer xfer;
	uint32_t s_id, d_id;

	s_id = (uint32_t)frame->s_id;
	d_id = (uint32_t)frame->d_id;
	s_id = from_be32(&s_id) >> 8;
	d_id = from_be32(&d_id) >> 8;

	cmd_iu = buffer->virt;
	cmnd_len = cmd_iu->cmnd_iu_len;
	cmnd_len = from_be16(&cmnd_len);

	/* check for a valid cmnd_iu format */
	if ((cmd_iu->fc_id != FCNVME_CMND_IU_FC_ID) ||
	    (cmd_iu->scsi_id != FCNVME_CMND_IU_SCSI_ID) ||
	    (cmnd_len != sizeof(struct spdk_nvmf_fc_cmnd_iu) / 4)) {
		SPDK_ERRLOG("IU CMD error\n");
		hwqp->counters.nvme_cmd_iu_err++;
		return -ENXIO;
	}

	xfer = spdk_nvme_opc_get_data_transfer(cmd_iu->flags);
	if (xfer == SPDK_NVME_DATA_BIDIRECTIONAL) {
		SPDK_ERRLOG("IU CMD xfer error\n");
		hwqp->counters.nvme_cmd_xfer_err++;
		return -EPERM;
	}

	rqst_conn_id = from_be64(&cmd_iu->conn_id);

	if (rte_hash_lookup_data(hwqp->connection_list_hash,
				 (void *)&rqst_conn_id, (void **)&fc_conn) < 0) {
		SPDK_ERRLOG("IU CMD conn(%ld) invalid\n", rqst_conn_id);
		hwqp->counters.invalid_conn_err++;
		return -ENODEV;
	}

	/* Validate s_id and d_id */
	if (s_id != fc_conn->s_id) {
		hwqp->counters.rport_invalid++;
		SPDK_ERRLOG("Frame s_id invalid for connection %ld\n", rqst_conn_id);
		return -ENODEV;
	}

	if (d_id != fc_conn->d_id) {
		hwqp->counters.nport_invalid++;
		SPDK_ERRLOG("Frame d_id invalid for connection %ld\n", rqst_conn_id);
		return -ENODEV;
	}

	/* If association/connection is being deleted - return */
	if (fc_conn->fc_assoc->assoc_state != SPDK_NVMF_FC_OBJECT_CREATED) {
		SPDK_ERRLOG("Association %ld state = %d not valid\n",
			    fc_conn->fc_assoc->assoc_id, fc_conn->fc_assoc->assoc_state);
		return -EACCES;
	}

	if (fc_conn->conn_state != SPDK_NVMF_FC_OBJECT_CREATED) {
		SPDK_ERRLOG("Connection %ld state = %d not valid\n",
			    rqst_conn_id, fc_conn->conn_state);
		return -EACCES;
	}

	if (fc_conn->qpair.state != SPDK_NVMF_QPAIR_ACTIVE) {
		SPDK_ERRLOG("Connection %ld qpair state = %d not valid\n",
			    rqst_conn_id, fc_conn->qpair.state);
		return -EACCES;
	}

	/* Make sure xfer len is according to mdts */
	if (from_be32(&cmd_iu->data_len) >
	    hwqp->fgroup->group.transport->opts.max_io_size) {
		SPDK_ERRLOG("IO length requested is greater than MDTS\n");
		return -EINVAL;
	}

	/* allocate a request buffer */
	fc_req = nvmf_fc_conn_alloc_fc_request(fc_conn);
	if (fc_req == NULL) {
		return -ENOMEM;
	}

	fc_req->req.length = from_be32(&cmd_iu->data_len);
	fc_req->req.qpair = &fc_conn->qpair;
	memcpy(&fc_req->cmd, &cmd_iu->cmd, sizeof(union nvmf_h2c_msg));
	fc_req->req.cmd = (union nvmf_h2c_msg *)&fc_req->cmd;
	fc_req->req.rsp = (union nvmf_c2h_msg *)&fc_req->ersp.rsp;
	fc_req->oxid = frame->ox_id;
	fc_req->oxid = from_be16(&fc_req->oxid);
	fc_req->rpi = fc_conn->rpi;
	fc_req->poller_lcore = hwqp->lcore_id;
	fc_req->poller_thread = hwqp->thread;
	fc_req->hwqp = hwqp;
	fc_req->fc_conn = fc_conn;
	fc_req->req.xfer = xfer;
	fc_req->s_id = s_id;
	fc_req->d_id = d_id;
	fc_req->csn  = from_be32(&cmd_iu->cmnd_seq_num);
	nvmf_fc_set_vmid_priority(fc_req, frame);

	nvmf_fc_record_req_trace_point(fc_req, SPDK_NVMF_FC_REQ_INIT);

	if (!STAILQ_EMPTY(&hwqp->fgroup->group.pending_buf_queue) || nvmf_fc_request_execute(fc_req)) {
		STAILQ_INSERT_TAIL(&hwqp->fgroup->group.pending_buf_queue, &fc_req->req, buf_link);
		nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_PENDING);
	}

	return 0;
}

/*
 * These functions are called from the FC LLD
 */

void
_nvmf_fc_request_free(struct spdk_nvmf_fc_request *fc_req)
{
	struct spdk_nvmf_fc_hwqp *hwqp;
	struct spdk_nvmf_transport_poll_group *group;

	if (!fc_req) {
		return;
	}
	hwqp = fc_req->hwqp;

	if (fc_req->xchg) {
		nvmf_fc_put_xchg(hwqp, fc_req->xchg);
		fc_req->xchg = NULL;
	}

	/* Release IO buffers */
	if (fc_req->req.data_from_pool) {
		group = &hwqp->fgroup->group;
		spdk_nvmf_request_free_buffers(&fc_req->req, group,
					       group->transport);
	}
	fc_req->req.data = NULL;
	fc_req->req.iovcnt  = 0;

	/* Free Fc request */
	nvmf_fc_conn_free_fc_request(fc_req->fc_conn, fc_req);
}

void
nvmf_fc_request_set_state(struct spdk_nvmf_fc_request *fc_req,
			  enum spdk_nvmf_fc_request_state state)
{
	assert(fc_req->magic != 0xDEADBEEF);

	SPDK_DEBUGLOG(nvmf_fc,
		      "FC Request(%p):\n\tState Old:%s New:%s\n", fc_req,
		      nvmf_fc_request_get_state_str(fc_req->state),
		      nvmf_fc_request_get_state_str(state));
	nvmf_fc_record_req_trace_point(fc_req, state);
	fc_req->state = state;
}

char *
nvmf_fc_request_get_state_str(int state)
{
	static char *unk_str = "unknown";

	return (state >= 0 && state < (int)(sizeof(fc_req_state_strs) / sizeof(char *)) ?
		fc_req_state_strs[state] : unk_str);
}

int
nvmf_fc_hwqp_process_frame(struct spdk_nvmf_fc_hwqp *hwqp,
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

	SPDK_DEBUGLOG(nvmf_fc,
		      "Process NVME frame s_id:0x%x d_id:0x%x oxid:0x%x rxid:0x%x.\n",
		      s_id, d_id,
		      ((frame->ox_id << 8) & 0xff00) | ((frame->ox_id >> 8) & 0xff),
		      ((frame->rx_id << 8) & 0xff00) | ((frame->rx_id >> 8) & 0xff));

	if ((frame->r_ctl == FCNVME_R_CTL_LS_REQUEST) &&
	    (frame->type == FCNVME_TYPE_NVMF_DATA)) {
		struct spdk_nvmf_fc_rq_buf_ls_request *req_buf = buffer->virt;
		struct spdk_nvmf_fc_ls_rqst *ls_rqst;

		SPDK_DEBUGLOG(nvmf_fc, "Process LS NVME frame\n");

		rc = nvmf_fc_hwqp_find_nport_and_rport(hwqp, d_id, &nport, s_id, &rport);
		if (rc) {
			if (nport == NULL) {
				SPDK_ERRLOG("Nport not found. Dropping\n");
				/* increment invalid nport counter */
				hwqp->counters.nport_invalid++;
			} else if (rport == NULL) {
				SPDK_ERRLOG("Rport not found. Dropping\n");
				/* increment invalid rport counter */
				hwqp->counters.rport_invalid++;
			}
			return rc;
		}

		if (nport->nport_state != SPDK_NVMF_FC_OBJECT_CREATED ||
		    rport->rport_state != SPDK_NVMF_FC_OBJECT_CREATED) {
			SPDK_ERRLOG("%s state not created. Dropping\n",
				    nport->nport_state != SPDK_NVMF_FC_OBJECT_CREATED ?
				    "Nport" : "Rport");
			return -EACCES;
		}

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
		ls_rqst->nvmf_tgt = g_nvmf_ftransport->transport.tgt;

		if (TAILQ_EMPTY(&hwqp->ls_pending_queue)) {
			ls_rqst->xchg = nvmf_fc_get_xri(hwqp);
		} else {
			ls_rqst->xchg = NULL;
		}

		if (ls_rqst->xchg) {
			/* Handover the request to LS module */
			nvmf_fc_handle_ls_rqst(ls_rqst);
		} else {
			/* No XCHG available. Add to pending list. */
			hwqp->counters.no_xchg++;
			TAILQ_INSERT_TAIL(&hwqp->ls_pending_queue, ls_rqst, ls_pending_link);
		}
	} else if ((frame->r_ctl == FCNVME_R_CTL_CMD_REQ) &&
		   (frame->type == FCNVME_TYPE_FC_EXCHANGE)) {

		SPDK_DEBUGLOG(nvmf_fc, "Process IO NVME frame\n");
		rc = nvmf_fc_hwqp_handle_request(hwqp, frame, buffer, plen);
		if (!rc) {
			nvmf_fc_rqpair_buffer_release(hwqp, buff_idx);
		}
	} else {

		SPDK_ERRLOG("Unknown frame received. Dropping\n");
		hwqp->counters.unknown_frame++;
		rc = -EINVAL;
	}

	return rc;
}

void
nvmf_fc_hwqp_process_pending_reqs(struct spdk_nvmf_fc_hwqp *hwqp)
{
	struct spdk_nvmf_request *req = NULL, *tmp;
	struct spdk_nvmf_fc_request *fc_req;
	int budget = 64;

	if (!hwqp->fgroup) {
		/* LS queue is tied to acceptor_poll group and LS pending requests
		 * are stagged and processed using hwqp->ls_pending_queue.
		 */
		return;
	}

	STAILQ_FOREACH_SAFE(req, &hwqp->fgroup->group.pending_buf_queue, buf_link, tmp) {
		fc_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_fc_request, req);
		if (!nvmf_fc_request_execute(fc_req)) {
			/* Successfully posted, Delete from pending. */
			nvmf_fc_request_remove_from_pending(fc_req);
		}

		if (budget) {
			budget--;
		} else {
			return;
		}
	}
}

void
nvmf_fc_hwqp_process_pending_ls_rqsts(struct spdk_nvmf_fc_hwqp *hwqp)
{
	struct spdk_nvmf_fc_ls_rqst *ls_rqst = NULL, *tmp;
	struct spdk_nvmf_fc_nport *nport = NULL;
	struct spdk_nvmf_fc_remote_port_info *rport = NULL;

	TAILQ_FOREACH_SAFE(ls_rqst, &hwqp->ls_pending_queue, ls_pending_link, tmp) {
		/* lookup nport and rport again - make sure they are still valid */
		int rc = nvmf_fc_hwqp_find_nport_and_rport(hwqp, ls_rqst->d_id, &nport, ls_rqst->s_id, &rport);
		if (rc) {
			if (nport == NULL) {
				SPDK_ERRLOG("Nport not found. Dropping\n");
				/* increment invalid nport counter */
				hwqp->counters.nport_invalid++;
			} else if (rport == NULL) {
				SPDK_ERRLOG("Rport not found. Dropping\n");
				/* increment invalid rport counter */
				hwqp->counters.rport_invalid++;
			}
			nvmf_fc_release_ls_rqst(hwqp, ls_rqst);
			continue;
		}
		if (nport->nport_state != SPDK_NVMF_FC_OBJECT_CREATED ||
		    rport->rport_state != SPDK_NVMF_FC_OBJECT_CREATED) {
			SPDK_ERRLOG("%s state not created. Dropping\n",
				    nport->nport_state != SPDK_NVMF_FC_OBJECT_CREATED ?
				    "Nport" : "Rport");
			nvmf_fc_release_ls_rqst(hwqp, ls_rqst);
			continue;
		}

		ls_rqst->xchg = nvmf_fc_get_xri(hwqp);
		if (ls_rqst->xchg) {
			/* Got an XCHG */
			TAILQ_REMOVE(&hwqp->ls_pending_queue, ls_rqst, ls_pending_link);
			/* Handover the request to LS module */
			nvmf_fc_handle_ls_rqst(ls_rqst);
		} else {
			/* No more XCHGs. Stop processing. */
			hwqp->counters.no_xchg++;
			return;
		}
	}
}

int
nvmf_fc_handle_rsp(struct spdk_nvmf_fc_request *fc_req)
{
	int rc = 0;
	struct spdk_nvmf_request *req = &fc_req->req;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_fc_conn *fc_conn = nvmf_fc_get_conn(qpair);
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint16_t ersp_len = 0;

	/* set sq head value in resp */
	rsp->sqhd = nvmf_fc_advance_conn_sqhead(qpair);

	/* Increment connection responses */
	fc_conn->rsp_count++;

	if (nvmf_fc_send_ersp_required(fc_req, fc_conn->rsp_count,
				       fc_req->transferred_len)) {
		/* Fill ERSP Len */
		to_be16(&ersp_len, (sizeof(struct spdk_nvmf_fc_ersp_iu) /
				    sizeof(uint32_t)));
		fc_req->ersp.ersp_len = ersp_len;

		/* Fill RSN */
		to_be32(&fc_req->ersp.response_seq_no, fc_conn->rsn);
		fc_conn->rsn++;

		/* Fill transfer length */
		to_be32(&fc_req->ersp.transferred_data_len, fc_req->transferred_len);

		SPDK_DEBUGLOG(nvmf_fc, "Posting ERSP.\n");
		rc = nvmf_fc_xmt_rsp(fc_req, (uint8_t *)&fc_req->ersp,
				     sizeof(struct spdk_nvmf_fc_ersp_iu));
	} else {
		SPDK_DEBUGLOG(nvmf_fc, "Posting RSP.\n");
		rc = nvmf_fc_xmt_rsp(fc_req, NULL, 0);
	}

	return rc;
}

bool
nvmf_fc_send_ersp_required(struct spdk_nvmf_fc_request *fc_req,
			   uint32_t rsp_cnt, uint32_t xfer_len)
{
	struct spdk_nvmf_request *req = &fc_req->req;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_fc_conn *fc_conn = nvmf_fc_get_conn(qpair);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint16_t status = *((uint16_t *)&rsp->status);

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
	    (status & 0xFFFE) || rsp->cdw0 || rsp->cdw1 ||
	    (req->length != xfer_len)) {
		return true;
	}
	return false;
}

static int
nvmf_fc_request_complete(struct spdk_nvmf_request *req)
{
	int rc = 0;
	struct spdk_nvmf_fc_request *fc_req = nvmf_fc_get_fc_req(req);
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	if (fc_req->is_aborted) {
		/* Defer this to make sure we dont call io cleanup in same context. */
		nvmf_fc_poller_api_func(fc_req->hwqp, SPDK_NVMF_FC_POLLER_API_REQ_ABORT_COMPLETE,
					(void *)fc_req);
	} else if (rsp->status.sc == SPDK_NVME_SC_SUCCESS &&
		   req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {

		nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_READ_XFER);

		rc = nvmf_fc_send_data(fc_req);
	} else {
		if (req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
			nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_WRITE_RSP);
		} else if (req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
			nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_READ_RSP);
		} else {
			nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_NONE_RSP);
		}

		rc = nvmf_fc_handle_rsp(fc_req);
	}

	if (rc) {
		SPDK_ERRLOG("Error in request complete.\n");
		_nvmf_fc_request_free(fc_req);
	}
	return 0;
}

struct spdk_nvmf_tgt *
nvmf_fc_get_tgt(void)
{
	if (g_nvmf_ftransport) {
		return g_nvmf_ftransport->transport.tgt;
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
#define SPDK_NVMF_FC_DEFAULT_NUM_SHARED_BUFFERS 8192
#define SPDK_NVMF_FC_DEFAULT_MAX_SGE (SPDK_NVMF_FC_DEFAULT_MAX_IO_SIZE /	\
				      SPDK_NVMF_FC_DEFAULT_IO_UNIT_SIZE)

static void
nvmf_fc_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth =      SPDK_NVMF_FC_DEFAULT_MAX_QUEUE_DEPTH;
	opts->max_qpairs_per_ctrlr = SPDK_NVMF_FC_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	opts->in_capsule_data_size = SPDK_NVMF_FC_DEFAULT_IN_CAPSULE_DATA_SIZE;
	opts->max_io_size =          SPDK_NVMF_FC_DEFAULT_MAX_IO_SIZE;
	opts->io_unit_size =         SPDK_NVMF_FC_DEFAULT_IO_UNIT_SIZE;
	opts->max_aq_depth =         SPDK_NVMF_FC_DEFAULT_AQ_DEPTH;
	opts->num_shared_buffers =   SPDK_NVMF_FC_DEFAULT_NUM_SHARED_BUFFERS;
}

static int nvmf_fc_accept(void *ctx);

static struct spdk_nvmf_transport *
nvmf_fc_create(struct spdk_nvmf_transport_opts *opts)
{
	uint32_t sge_count;

	SPDK_INFOLOG(nvmf_fc, "*** FC Transport Init ***\n"
		     "  Transport opts:  max_ioq_depth=%d, max_io_size=%d,\n"
		     "  max_io_qpairs_per_ctrlr=%d, io_unit_size=%d,\n"
		     "  max_aq_depth=%d\n",
		     opts->max_queue_depth,
		     opts->max_io_size,
		     opts->max_qpairs_per_ctrlr - 1,
		     opts->io_unit_size,
		     opts->max_aq_depth);

	if (g_nvmf_ftransport) {
		SPDK_ERRLOG("Duplicate NVMF-FC transport create request!\n");
		return NULL;
	}

	if (spdk_env_get_last_core() < 1) {
		SPDK_ERRLOG("Not enough cores/threads (%d) to run NVMF-FC transport!\n",
			    spdk_env_get_last_core() + 1);
		return NULL;
	}

	sge_count = opts->max_io_size / opts->io_unit_size;
	if (sge_count > SPDK_NVMF_FC_DEFAULT_MAX_SGE) {
		SPDK_ERRLOG("Unsupported IO Unit size specified, %d bytes\n", opts->io_unit_size);
		return NULL;
	}

	g_nvmf_fc_main_thread = spdk_get_thread();
	g_nvmf_fgroup_count = 0;
	g_nvmf_ftransport = calloc(1, sizeof(*g_nvmf_ftransport));

	if (!g_nvmf_ftransport) {
		SPDK_ERRLOG("Failed to allocate NVMF-FC transport\n");
		return NULL;
	}

	if (pthread_mutex_init(&g_nvmf_ftransport->lock, NULL)) {
		SPDK_ERRLOG("pthread_mutex_init() failed\n");
		free(g_nvmf_ftransport);
		g_nvmf_ftransport = NULL;
		return NULL;
	}

	g_nvmf_ftransport->accept_poller = SPDK_POLLER_REGISTER(nvmf_fc_accept,
					   &g_nvmf_ftransport->transport, opts->acceptor_poll_rate);
	if (!g_nvmf_ftransport->accept_poller) {
		free(g_nvmf_ftransport);
		g_nvmf_ftransport = NULL;
		return NULL;
	}

	/* initialize the low level FC driver */
	nvmf_fc_lld_init();

	return &g_nvmf_ftransport->transport;
}

static void
nvmf_fc_destroy_done_cb(void *cb_arg)
{
	free(g_nvmf_ftransport);
	if (g_transport_destroy_done_cb) {
		g_transport_destroy_done_cb(cb_arg);
		g_transport_destroy_done_cb = NULL;
	}
}

static int
nvmf_fc_destroy(struct spdk_nvmf_transport *transport,
		spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg)
{
	if (transport) {
		struct spdk_nvmf_fc_poll_group *fgroup, *pg_tmp;

		/* clean up any FC poll groups still around */
		TAILQ_FOREACH_SAFE(fgroup, &g_nvmf_fgroups, link, pg_tmp) {
			TAILQ_REMOVE(&g_nvmf_fgroups, fgroup, link);
			free(fgroup);
		}

		spdk_poller_unregister(&g_nvmf_ftransport->accept_poller);
		g_nvmf_fgroup_count = 0;
		g_transport_destroy_done_cb = cb_fn;

		/* low level FC driver clean up */
		nvmf_fc_lld_fini(nvmf_fc_destroy_done_cb, cb_arg);
	}

	return 0;
}

static int
nvmf_fc_listen(struct spdk_nvmf_transport *transport, const struct spdk_nvme_transport_id *trid,
	       struct spdk_nvmf_listen_opts *listen_opts)
{
	return 0;
}

static void
nvmf_fc_stop_listen(struct spdk_nvmf_transport *transport,
		    const struct spdk_nvme_transport_id *_trid)
{
}

static int
nvmf_fc_accept(void *ctx)
{
	struct spdk_nvmf_fc_port *fc_port = NULL;
	uint32_t count = 0;
	static bool start_lld = false;

	if (spdk_unlikely(!start_lld)) {
		start_lld  = true;
		nvmf_fc_lld_start();
	}

	/* poll the LS queue on each port */
	TAILQ_FOREACH(fc_port, &g_spdk_nvmf_fc_port_list, link) {
		if (fc_port->hw_port_status == SPDK_FC_PORT_ONLINE) {
			count += nvmf_fc_process_queue(&fc_port->ls_queue);
		}
	}

	return count > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static void
nvmf_fc_discover(struct spdk_nvmf_transport *transport,
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
nvmf_fc_poll_group_create(struct spdk_nvmf_transport *transport,
			  struct spdk_nvmf_poll_group *group)
{
	struct spdk_nvmf_fc_poll_group *fgroup;
	struct spdk_nvmf_fc_transport *ftransport =
		SPDK_CONTAINEROF(transport, struct spdk_nvmf_fc_transport, transport);

	fgroup = calloc(1, sizeof(struct spdk_nvmf_fc_poll_group));
	if (!fgroup) {
		SPDK_ERRLOG("Unable to alloc FC poll group\n");
		return NULL;
	}

	TAILQ_INIT(&fgroup->hwqp_list);

	pthread_mutex_lock(&ftransport->lock);
	TAILQ_INSERT_TAIL(&g_nvmf_fgroups, fgroup, link);
	g_nvmf_fgroup_count++;
	pthread_mutex_unlock(&ftransport->lock);

	return &fgroup->group;
}

static void
nvmf_fc_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_fc_poll_group *fgroup;
	struct spdk_nvmf_fc_transport *ftransport =
		SPDK_CONTAINEROF(group->transport, struct spdk_nvmf_fc_transport, transport);

	fgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_fc_poll_group, group);
	pthread_mutex_lock(&ftransport->lock);
	TAILQ_REMOVE(&g_nvmf_fgroups, fgroup, link);
	g_nvmf_fgroup_count--;
	pthread_mutex_unlock(&ftransport->lock);

	free(fgroup);
}

static int
nvmf_fc_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
		       struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_fc_poll_group *fgroup;
	struct spdk_nvmf_fc_conn *fc_conn;
	struct spdk_nvmf_fc_hwqp *hwqp = NULL;
	struct spdk_nvmf_fc_ls_add_conn_api_data *api_data = NULL;
	bool hwqp_found = false;

	fgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_fc_poll_group, group);
	fc_conn  = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_fc_conn, qpair);

	TAILQ_FOREACH(hwqp, &fgroup->hwqp_list, link) {
		if (fc_conn->fc_assoc->tgtport->fc_port == hwqp->fc_port) {
			hwqp_found = true;
			break;
		}
	}

	if (!hwqp_found) {
		SPDK_ERRLOG("No valid hwqp found for new QP.\n");
		goto err;
	}

	if (!nvmf_fc_assign_conn_to_hwqp(hwqp,
					 &fc_conn->conn_id,
					 fc_conn->max_queue_depth)) {
		SPDK_ERRLOG("Failed to get a connection id for new QP.\n");
		goto err;
	}

	fc_conn->hwqp = hwqp;

	/* If this is for ADMIN connection, then update assoc ID. */
	if (fc_conn->qpair.qid == 0) {
		fc_conn->fc_assoc->assoc_id = fc_conn->conn_id;
	}

	api_data = &fc_conn->create_opd->u.add_conn;
	nvmf_fc_poller_api_func(hwqp, SPDK_NVMF_FC_POLLER_API_ADD_CONNECTION, &api_data->args);
	return 0;
err:
	return -1;
}

static int
nvmf_fc_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	uint32_t count = 0;
	struct spdk_nvmf_fc_poll_group *fgroup;
	struct spdk_nvmf_fc_hwqp *hwqp;

	fgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_fc_poll_group, group);

	TAILQ_FOREACH(hwqp, &fgroup->hwqp_list, link) {
		if (hwqp->state == SPDK_FC_HWQP_ONLINE) {
			count += nvmf_fc_process_queue(hwqp);
		}
	}

	return (int) count;
}

static int
nvmf_fc_request_free(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fc_request *fc_req = nvmf_fc_get_fc_req(req);

	if (!fc_req->is_aborted) {
		nvmf_fc_request_set_state(fc_req, SPDK_NVMF_FC_REQ_BDEV_ABORTED);
		nvmf_fc_request_abort(fc_req, true, NULL, NULL);
	} else {
		nvmf_fc_request_abort_complete(fc_req);
	}

	return 0;
}

static void
nvmf_fc_connection_delete_done_cb(void *arg)
{
	struct spdk_nvmf_fc_qpair_remove_ctx *fc_ctx = arg;

	if (fc_ctx->cb_fn) {
		spdk_thread_send_msg(fc_ctx->qpair_thread, fc_ctx->cb_fn, fc_ctx->cb_ctx);
	}
	free(fc_ctx);
}

static void
_nvmf_fc_close_qpair(void *arg)
{
	struct spdk_nvmf_fc_qpair_remove_ctx *fc_ctx = arg;
	struct spdk_nvmf_qpair *qpair = fc_ctx->qpair;
	struct spdk_nvmf_fc_conn *fc_conn;
	int rc;

	fc_conn = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_fc_conn, qpair);
	if (fc_conn->conn_id == NVMF_FC_INVALID_CONN_ID) {
		struct spdk_nvmf_fc_ls_add_conn_api_data *api_data = NULL;

		if (fc_conn->create_opd) {
			api_data = &fc_conn->create_opd->u.add_conn;

			nvmf_fc_ls_add_conn_failure(api_data->assoc, api_data->ls_rqst,
						    api_data->args.fc_conn, api_data->aq_conn);
		}
	} else if (fc_conn->conn_state == SPDK_NVMF_FC_OBJECT_CREATED) {
		rc = nvmf_fc_delete_connection(fc_conn, false, true,
					       nvmf_fc_connection_delete_done_cb, fc_ctx);
		if (!rc) {
			/* Wait for transport to complete its work. */
			return;
		}

		SPDK_ERRLOG("%s: Delete FC connection failed.\n", __func__);
	}

	nvmf_fc_connection_delete_done_cb(fc_ctx);
}

static void
nvmf_fc_close_qpair(struct spdk_nvmf_qpair *qpair,
		    spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_fc_qpair_remove_ctx *fc_ctx;

	fc_ctx = calloc(1, sizeof(struct spdk_nvmf_fc_qpair_remove_ctx));
	if (!fc_ctx) {
		SPDK_ERRLOG("Unable to allocate close_qpair ctx.");
		if (cb_fn) {
			cb_fn(cb_arg);
		}
		return;
	}
	fc_ctx->qpair = qpair;
	fc_ctx->cb_fn = cb_fn;
	fc_ctx->cb_ctx = cb_arg;
	fc_ctx->qpair_thread = spdk_get_thread();

	spdk_thread_send_msg(nvmf_fc_get_main_thread(), _nvmf_fc_close_qpair, fc_ctx);
}

static int
nvmf_fc_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
			    struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_fc_conn *fc_conn;

	fc_conn = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_fc_conn, qpair);
	memcpy(trid, &fc_conn->trid, sizeof(struct spdk_nvme_transport_id));
	return 0;
}

static int
nvmf_fc_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
			     struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_fc_conn *fc_conn;

	fc_conn = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_fc_conn, qpair);
	memcpy(trid, &fc_conn->trid, sizeof(struct spdk_nvme_transport_id));
	return 0;
}

static int
nvmf_fc_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
			      struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_fc_conn *fc_conn;

	fc_conn = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_fc_conn, qpair);
	memcpy(trid, &fc_conn->trid, sizeof(struct spdk_nvme_transport_id));
	return 0;
}

static void
nvmf_fc_qpair_abort_request(struct spdk_nvmf_qpair *qpair,
			    struct spdk_nvmf_request *req)
{
	spdk_nvmf_request_complete(req);
}

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_fc = {
	.name = "FC",
	.type = (enum spdk_nvme_transport_type) SPDK_NVMF_TRTYPE_FC,
	.opts_init = nvmf_fc_opts_init,
	.create = nvmf_fc_create,
	.destroy = nvmf_fc_destroy,

	.listen = nvmf_fc_listen,
	.stop_listen = nvmf_fc_stop_listen,

	.listener_discover = nvmf_fc_discover,

	.poll_group_create = nvmf_fc_poll_group_create,
	.poll_group_destroy = nvmf_fc_poll_group_destroy,
	.poll_group_add = nvmf_fc_poll_group_add,
	.poll_group_poll = nvmf_fc_poll_group_poll,

	.req_complete = nvmf_fc_request_complete,
	.req_free = nvmf_fc_request_free,
	.qpair_fini = nvmf_fc_close_qpair,
	.qpair_get_peer_trid = nvmf_fc_qpair_get_peer_trid,
	.qpair_get_local_trid = nvmf_fc_qpair_get_local_trid,
	.qpair_get_listen_trid = nvmf_fc_qpair_get_listen_trid,
	.qpair_abort_request = nvmf_fc_qpair_abort_request,
};

/* Initializes the data for the creation of a FC-Port object in the SPDK
 * library. The spdk_nvmf_fc_port is a well defined structure that is part of
 * the API to the library. The contents added to this well defined structure
 * is private to each vendors implementation.
 */
static int
nvmf_fc_adm_hw_port_data_init(struct spdk_nvmf_fc_port *fc_port,
			      struct spdk_nvmf_fc_hw_port_init_args *args)
{
	int rc = 0;
	/* Used a high number for the LS HWQP so that it does not clash with the
	 * IO HWQP's and immediately shows a LS queue during tracing.
	 */
	uint32_t i;

	fc_port->port_hdl       = args->port_handle;
	fc_port->lld_fc_port	= args->lld_fc_port;
	fc_port->hw_port_status = SPDK_FC_PORT_OFFLINE;
	fc_port->fcp_rq_id      = args->fcp_rq_id;
	fc_port->num_io_queues  = args->io_queue_cnt;

	/*
	 * Set port context from init args. Used for FCP port stats.
	 */
	fc_port->port_ctx = args->port_ctx;

	/*
	 * Initialize the LS queue wherever needed.
	 */
	fc_port->ls_queue.queues = args->ls_queue;
	fc_port->ls_queue.thread = nvmf_fc_get_main_thread();
	fc_port->ls_queue.hwqp_id = SPDK_MAX_NUM_OF_FC_PORTS * fc_port->num_io_queues;
	fc_port->ls_queue.is_ls_queue = true;

	/*
	 * Initialize the LS queue.
	 */
	rc = nvmf_fc_init_hwqp(fc_port, &fc_port->ls_queue);
	if (rc) {
		return rc;
	}

	/*
	 * Initialize the IO queues.
	 */
	for (i = 0; i < args->io_queue_cnt; i++) {
		struct spdk_nvmf_fc_hwqp *hwqp = &fc_port->io_queues[i];
		hwqp->hwqp_id = i;
		hwqp->queues = args->io_queues[i];
		hwqp->is_ls_queue = false;
		rc = nvmf_fc_init_hwqp(fc_port, hwqp);
		if (rc) {
			for (; i > 0; --i) {
				rte_hash_free(fc_port->io_queues[i - 1].connection_list_hash);
				rte_hash_free(fc_port->io_queues[i - 1].rport_list_hash);
			}
			rte_hash_free(fc_port->ls_queue.connection_list_hash);
			rte_hash_free(fc_port->ls_queue.rport_list_hash);
			return rc;
		}
	}

	/*
	 * Initialize the LS processing for port
	 */
	nvmf_fc_ls_init(fc_port);

	/*
	 * Initialize the list of nport on this HW port.
	 */
	TAILQ_INIT(&fc_port->nport_list);
	fc_port->num_nports = 0;

	return 0;
}

/*
 * FC port must have all its nports deleted before transitioning to offline state.
 */
static void
nvmf_fc_adm_hw_port_offline_nport_delete(struct spdk_nvmf_fc_port *fc_port)
{
	struct spdk_nvmf_fc_nport *nport = NULL;
	/* All nports must have been deleted at this point for this fc port */
	DEV_VERIFY(fc_port && TAILQ_EMPTY(&fc_port->nport_list));
	DEV_VERIFY(fc_port->num_nports == 0);
	/* Mark the nport states to be zombie, if they exist */
	if (fc_port && !TAILQ_EMPTY(&fc_port->nport_list)) {
		TAILQ_FOREACH(nport, &fc_port->nport_list, link) {
			(void)nvmf_fc_nport_set_state(nport, SPDK_NVMF_FC_OBJECT_ZOMBIE);
		}
	}
}

static void
nvmf_fc_adm_i_t_delete_cb(void *args, uint32_t err)
{
	ASSERT_SPDK_FC_MAIN_THREAD();
	struct spdk_nvmf_fc_adm_i_t_del_cb_data *cb_data = args;
	struct spdk_nvmf_fc_nport *nport = cb_data->nport;
	struct spdk_nvmf_fc_remote_port_info *rport = cb_data->rport;
	spdk_nvmf_fc_callback cb_func = cb_data->fc_cb_func;
	int spdk_err = 0;
	uint8_t port_handle = cb_data->port_handle;
	uint32_t s_id = rport->s_id;
	uint32_t rpi = rport->rpi;
	uint32_t assoc_count = rport->assoc_count;
	uint32_t nport_hdl = nport->nport_hdl;
	uint32_t d_id = nport->d_id;
	char log_str[256];

	/*
	 * Assert on any delete failure.
	 */
	if (0 != err) {
		DEV_VERIFY(!"Error in IT Delete callback.");
		goto out;
	}

	if (cb_func != NULL) {
		(void)cb_func(port_handle, SPDK_FC_IT_DELETE, cb_data->fc_cb_ctx, spdk_err);
	}

out:
	free(cb_data);

	snprintf(log_str, sizeof(log_str),
		 "IT delete assoc_cb on nport %d done, port_handle:%d s_id:%d d_id:%d rpi:%d rport_assoc_count:%d rc = %d.\n",
		 nport_hdl, port_handle, s_id, d_id, rpi, assoc_count, err);

	if (err != 0) {
		SPDK_ERRLOG("%s", log_str);
	} else {
		SPDK_DEBUGLOG(nvmf_fc_adm_api, "%s", log_str);
	}
}

static void
nvmf_fc_adm_i_t_delete_assoc_cb(void *args, uint32_t err)
{
	ASSERT_SPDK_FC_MAIN_THREAD();
	struct spdk_nvmf_fc_adm_i_t_del_assoc_cb_data *cb_data = args;
	struct spdk_nvmf_fc_nport *nport = cb_data->nport;
	struct spdk_nvmf_fc_remote_port_info *rport = cb_data->rport;
	spdk_nvmf_fc_adm_i_t_delete_assoc_cb_fn cb_func = cb_data->cb_func;
	uint32_t s_id = rport->s_id;
	uint32_t rpi = rport->rpi;
	uint32_t assoc_count = rport->assoc_count;
	uint32_t nport_hdl = nport->nport_hdl;
	uint32_t d_id = nport->d_id;
	char log_str[256];

	/*
	 * Assert on any association delete failure. We continue to delete other
	 * associations in promoted builds.
	 */
	if (0 != err) {
		DEV_VERIFY(!"Nport's association delete callback returned error");
		if (nport->assoc_count > 0) {
			nport->assoc_count--;
		}
		if (rport->assoc_count > 0) {
			rport->assoc_count--;
		}
	}

	/*
	 * If this is the last association being deleted for the ITN,
	 * execute the callback(s).
	 */
	if (0 == rport->assoc_count) {
		/* Remove the rport from the remote port list. */
		if (nvmf_fc_nport_remove_rem_port(nport, rport) != 0) {
			SPDK_ERRLOG("Error while removing rport from list.\n");
			DEV_VERIFY(!"Error while removing rport from list.");
		}

		if (cb_func != NULL) {
			/*
			 * Callback function is provided by the caller
			 * of nvmf_fc_adm_i_t_delete_assoc().
			 */
			(void)cb_func(cb_data->cb_ctx, 0);
		}
		free(rport);
		free(args);
	}

	snprintf(log_str, sizeof(log_str),
		 "IT delete assoc_cb on nport %d done, s_id:%d d_id:%d rpi:%d rport_assoc_count:%d err = %d.\n",
		 nport_hdl, s_id, d_id, rpi, assoc_count, err);

	if (err != 0) {
		SPDK_ERRLOG("%s", log_str);
	} else {
		SPDK_DEBUGLOG(nvmf_fc_adm_api, "%s", log_str);
	}
}

/**
 * Process a IT delete.
 */
static void
nvmf_fc_adm_i_t_delete_assoc(struct spdk_nvmf_fc_nport *nport,
			     struct spdk_nvmf_fc_remote_port_info *rport,
			     spdk_nvmf_fc_adm_i_t_delete_assoc_cb_fn cb_func,
			     void *cb_ctx)
{
	int err = 0;
	struct spdk_nvmf_fc_association *assoc = NULL;
	int assoc_err = 0;
	uint32_t num_assoc = 0;
	uint32_t num_assoc_del_scheduled = 0;
	struct spdk_nvmf_fc_adm_i_t_del_assoc_cb_data *cb_data = NULL;
	uint8_t port_hdl = nport->port_hdl;
	uint32_t s_id = rport->s_id;
	uint32_t rpi = rport->rpi;
	uint32_t assoc_count = rport->assoc_count;
	char log_str[256];

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "IT delete associations on nport:%d begin.\n",
		      nport->nport_hdl);

	/*
	 * Allocate memory for callback data.
	 * This memory will be freed by the callback function.
	 */
	cb_data = calloc(1, sizeof(struct spdk_nvmf_fc_adm_i_t_del_assoc_cb_data));
	if (NULL == cb_data) {
		SPDK_ERRLOG("Failed to allocate memory for cb_data on nport:%d.\n", nport->nport_hdl);
		err = -ENOMEM;
		goto out;
	}
	cb_data->nport       = nport;
	cb_data->rport       = rport;
	cb_data->port_handle = port_hdl;
	cb_data->cb_func     = cb_func;
	cb_data->cb_ctx      = cb_ctx;

	/*
	 * Delete all associations, if any, related with this ITN/remote_port.
	 */
	TAILQ_FOREACH(assoc, &nport->fc_associations, link) {
		num_assoc++;
		if (assoc->s_id == s_id) {
			assoc_err = nvmf_fc_delete_association(nport,
							       assoc->assoc_id,
							       false /* send abts */, false,
							       nvmf_fc_adm_i_t_delete_assoc_cb, cb_data);
			if (0 != assoc_err) {
				/*
				 * Mark this association as zombie.
				 */
				err = -EINVAL;
				DEV_VERIFY(!"Error while deleting association");
				(void)nvmf_fc_assoc_set_state(assoc, SPDK_NVMF_FC_OBJECT_ZOMBIE);
			} else {
				num_assoc_del_scheduled++;
			}
		}
	}

out:
	if ((cb_data) && (num_assoc_del_scheduled == 0)) {
		/*
		 * Since there are no association_delete calls
		 * successfully scheduled, the association_delete
		 * callback function will never be called.
		 * In this case, call the callback function now.
		 */
		nvmf_fc_adm_i_t_delete_assoc_cb(cb_data, 0);
	}

	snprintf(log_str, sizeof(log_str),
		 "IT delete associations on nport:%d end. "
		 "s_id:%d rpi:%d assoc_count:%d assoc:%d assoc_del_scheduled:%d rc:%d.\n",
		 nport->nport_hdl, s_id, rpi, assoc_count, num_assoc, num_assoc_del_scheduled, err);

	if (err == 0) {
		SPDK_DEBUGLOG(nvmf_fc_adm_api, "%s", log_str);
	} else {
		SPDK_ERRLOG("%s", log_str);
	}
}

static void
nvmf_fc_adm_queue_quiesce_cb(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret)
{
	ASSERT_SPDK_FC_MAIN_THREAD();
	struct spdk_nvmf_fc_poller_api_quiesce_queue_args *quiesce_api_data = NULL;
	struct spdk_nvmf_fc_adm_hw_port_quiesce_ctx *port_quiesce_ctx = NULL;
	struct spdk_nvmf_fc_hwqp *hwqp = NULL;
	struct spdk_nvmf_fc_port *fc_port = NULL;
	int err = 0;

	quiesce_api_data = (struct spdk_nvmf_fc_poller_api_quiesce_queue_args *)cb_data;
	hwqp = quiesce_api_data->hwqp;
	fc_port = hwqp->fc_port;
	port_quiesce_ctx = (struct spdk_nvmf_fc_adm_hw_port_quiesce_ctx *)quiesce_api_data->ctx;
	spdk_nvmf_fc_adm_hw_port_quiesce_cb_fn cb_func = port_quiesce_ctx->cb_func;

	/*
	 * Decrement the callback/quiesced queue count.
	 */
	port_quiesce_ctx->quiesce_count--;
	SPDK_DEBUGLOG(nvmf_fc_adm_api, "Queue%d Quiesced\n", quiesce_api_data->hwqp->hwqp_id);

	free(quiesce_api_data);
	/*
	 * Wait for call backs i.e. max_ioq_queues + LS QUEUE.
	 */
	if (port_quiesce_ctx->quiesce_count > 0) {
		return;
	}

	if (fc_port->hw_port_status == SPDK_FC_PORT_QUIESCED) {
		SPDK_ERRLOG("Port %d already in quiesced state.\n", fc_port->port_hdl);
	} else {
		SPDK_DEBUGLOG(nvmf_fc_adm_api, "HW port %d quiesced.\n", fc_port->port_hdl);
		fc_port->hw_port_status = SPDK_FC_PORT_QUIESCED;
	}

	if (cb_func) {
		/*
		 * Callback function for the called of quiesce.
		 */
		cb_func(port_quiesce_ctx->ctx, err);
	}

	/*
	 * Free the context structure.
	 */
	free(port_quiesce_ctx);

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "HW port %d quiesce done, rc = %d.\n", fc_port->port_hdl,
		      err);
}

static int
nvmf_fc_adm_hw_queue_quiesce(struct spdk_nvmf_fc_hwqp *fc_hwqp, void *ctx,
			     spdk_nvmf_fc_poller_api_cb cb_func)
{
	struct spdk_nvmf_fc_poller_api_quiesce_queue_args *args;
	enum spdk_nvmf_fc_poller_api_ret rc = SPDK_NVMF_FC_POLLER_API_SUCCESS;
	int err = 0;

	args = calloc(1, sizeof(struct spdk_nvmf_fc_poller_api_quiesce_queue_args));

	if (args == NULL) {
		err = -ENOMEM;
		SPDK_ERRLOG("Failed to allocate memory for poller quiesce args, hwqp:%d\n", fc_hwqp->hwqp_id);
		goto done;
	}
	args->hwqp = fc_hwqp;
	args->ctx = ctx;
	args->cb_info.cb_func = cb_func;
	args->cb_info.cb_data = args;
	args->cb_info.cb_thread = spdk_get_thread();

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "Quiesce queue %d\n", fc_hwqp->hwqp_id);
	rc = nvmf_fc_poller_api_func(fc_hwqp, SPDK_NVMF_FC_POLLER_API_QUIESCE_QUEUE, args);
	if (rc) {
		free(args);
		err = -EINVAL;
	}

done:
	return err;
}

/*
 * Hw port Quiesce
 */
static int
nvmf_fc_adm_hw_port_quiesce(struct spdk_nvmf_fc_port *fc_port, void *ctx,
			    spdk_nvmf_fc_adm_hw_port_quiesce_cb_fn cb_func)
{
	struct spdk_nvmf_fc_adm_hw_port_quiesce_ctx *port_quiesce_ctx = NULL;
	uint32_t i = 0;
	int err = 0;

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "HW port:%d is being quiesced.\n", fc_port->port_hdl);

	/*
	 * If the port is in an OFFLINE state, set the state to QUIESCED
	 * and execute the callback.
	 */
	if (fc_port->hw_port_status == SPDK_FC_PORT_OFFLINE) {
		fc_port->hw_port_status = SPDK_FC_PORT_QUIESCED;
	}

	if (fc_port->hw_port_status == SPDK_FC_PORT_QUIESCED) {
		SPDK_DEBUGLOG(nvmf_fc_adm_api, "Port %d already in quiesced state.\n",
			      fc_port->port_hdl);
		/*
		 * Execute the callback function directly.
		 */
		cb_func(ctx, err);
		goto out;
	}

	port_quiesce_ctx = calloc(1, sizeof(struct spdk_nvmf_fc_adm_hw_port_quiesce_ctx));

	if (port_quiesce_ctx == NULL) {
		err = -ENOMEM;
		SPDK_ERRLOG("Failed to allocate memory for LS queue quiesce ctx, port:%d\n",
			    fc_port->port_hdl);
		goto out;
	}

	port_quiesce_ctx->quiesce_count = 0;
	port_quiesce_ctx->ctx = ctx;
	port_quiesce_ctx->cb_func = cb_func;

	/*
	 * Quiesce the LS queue.
	 */
	err = nvmf_fc_adm_hw_queue_quiesce(&fc_port->ls_queue, port_quiesce_ctx,
					   nvmf_fc_adm_queue_quiesce_cb);
	if (err != 0) {
		SPDK_ERRLOG("Failed to quiesce the LS queue.\n");
		goto out;
	}
	port_quiesce_ctx->quiesce_count++;

	/*
	 * Quiesce the IO queues.
	 */
	for (i = 0; i < fc_port->num_io_queues; i++) {
		err = nvmf_fc_adm_hw_queue_quiesce(&fc_port->io_queues[i],
						   port_quiesce_ctx,
						   nvmf_fc_adm_queue_quiesce_cb);
		if (err != 0) {
			DEV_VERIFY(0);
			SPDK_ERRLOG("Failed to quiesce the IO queue:%d.\n", fc_port->io_queues[i].hwqp_id);
		}
		port_quiesce_ctx->quiesce_count++;
	}

out:
	if (port_quiesce_ctx && err != 0) {
		free(port_quiesce_ctx);
	}
	return err;
}

/*
 * Initialize and add a HW port entry to the global
 * HW port list.
 */
static void
nvmf_fc_adm_evnt_hw_port_init(void *arg)
{
	ASSERT_SPDK_FC_MAIN_THREAD();
	struct spdk_nvmf_fc_port *fc_port = NULL;
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;
	struct spdk_nvmf_fc_hw_port_init_args *args = (struct spdk_nvmf_fc_hw_port_init_args *)
			api_data->api_args;
	int err = 0;

	if (args->io_queue_cnt > spdk_env_get_core_count()) {
		SPDK_ERRLOG("IO queues count greater than cores for %d.\n", args->port_handle);
		err = EINVAL;
		goto abort_port_init;
	}

	/*
	 * 1. Check for duplicate initialization.
	 */
	fc_port = nvmf_fc_port_lookup(args->port_handle);
	if (fc_port != NULL) {
		SPDK_ERRLOG("Duplicate port found %d.\n", args->port_handle);
		goto abort_port_init;
	}

	/*
	 * 2. Get the memory to instantiate a fc port.
	 */
	fc_port = calloc(1, sizeof(struct spdk_nvmf_fc_port) +
			 (args->io_queue_cnt * sizeof(struct spdk_nvmf_fc_hwqp)));
	if (fc_port == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for fc_port %d.\n", args->port_handle);
		err = -ENOMEM;
		goto abort_port_init;
	}

	/* assign the io_queues array */
	fc_port->io_queues = (struct spdk_nvmf_fc_hwqp *)((uint8_t *)fc_port + sizeof(
				     struct spdk_nvmf_fc_port));

	/*
	 * 3. Initialize the contents for the FC-port
	 */
	err = nvmf_fc_adm_hw_port_data_init(fc_port, args);

	if (err != 0) {
		SPDK_ERRLOG("Data initialization failed for fc_port %d.\n", args->port_handle);
		DEV_VERIFY(!"Data initialization failed for fc_port");
		goto abort_port_init;
	}

	/*
	 * 4. Add this port to the global fc port list in the library.
	 */
	nvmf_fc_port_add(fc_port);

abort_port_init:
	if (err && fc_port) {
		free(fc_port);
	}
	if (api_data->cb_func != NULL) {
		(void)api_data->cb_func(args->port_handle, SPDK_FC_HW_PORT_INIT, args->cb_ctx, err);
	}

	free(arg);

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "HW port %d initialize done, rc = %d.\n",
		      args->port_handle, err);
}

static void
nvmf_fc_adm_hwqp_clean_sync_cb(struct spdk_nvmf_fc_hwqp *hwqp)
{
	struct spdk_nvmf_fc_abts_ctx *ctx;
	struct spdk_nvmf_fc_poller_api_queue_sync_args *args = NULL, *tmp = NULL;

	TAILQ_FOREACH_SAFE(args, &hwqp->sync_cbs, link, tmp) {
		TAILQ_REMOVE(&hwqp->sync_cbs, args, link);
		ctx = args->cb_info.cb_data;
		if (ctx) {
			if (++ctx->hwqps_responded == ctx->num_hwqps) {
				free(ctx->sync_poller_args);
				free(ctx->abts_poller_args);
				free(ctx);
			}
		}
	}
}

static void
nvmf_fc_adm_evnt_hw_port_free(void *arg)
{
	ASSERT_SPDK_FC_MAIN_THREAD();
	int err = 0, i;
	struct spdk_nvmf_fc_port *fc_port = NULL;
	struct spdk_nvmf_fc_hwqp *hwqp = NULL;
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;
	struct spdk_nvmf_fc_hw_port_free_args *args = (struct spdk_nvmf_fc_hw_port_free_args *)
			api_data->api_args;

	fc_port = nvmf_fc_port_lookup(args->port_handle);
	if (!fc_port) {
		SPDK_ERRLOG("Unable to find the SPDK FC port %d\n", args->port_handle);
		err = -EINVAL;
		goto out;
	}

	if (!TAILQ_EMPTY(&fc_port->nport_list)) {
		SPDK_ERRLOG("Hw port %d: nports not cleared up yet.\n", args->port_handle);
		err = -EIO;
		goto out;
	}

	/* Clean up and free fc_port */
	hwqp = &fc_port->ls_queue;
	nvmf_fc_adm_hwqp_clean_sync_cb(hwqp);
	rte_hash_free(hwqp->connection_list_hash);
	rte_hash_free(hwqp->rport_list_hash);

	for (i = 0; i < (int)fc_port->num_io_queues; i++) {
		hwqp = &fc_port->io_queues[i];

		nvmf_fc_adm_hwqp_clean_sync_cb(&fc_port->io_queues[i]);
		rte_hash_free(hwqp->connection_list_hash);
		rte_hash_free(hwqp->rport_list_hash);
	}

	nvmf_fc_port_remove(fc_port);
	free(fc_port);
out:
	SPDK_DEBUGLOG(nvmf_fc_adm_api, "HW port %d free done, rc = %d.\n",
		      args->port_handle, err);
	if (api_data->cb_func != NULL) {
		(void)api_data->cb_func(args->port_handle, SPDK_FC_HW_PORT_FREE, args->cb_ctx, err);
	}

	free(arg);
}

/*
 * Online a HW port.
 */
static void
nvmf_fc_adm_evnt_hw_port_online(void *arg)
{
	ASSERT_SPDK_FC_MAIN_THREAD();
	struct spdk_nvmf_fc_port *fc_port = NULL;
	struct spdk_nvmf_fc_hwqp *hwqp = NULL;
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;
	struct spdk_nvmf_fc_hw_port_online_args *args = (struct spdk_nvmf_fc_hw_port_online_args *)
			api_data->api_args;
	int i = 0;
	int err = 0;

	fc_port = nvmf_fc_port_lookup(args->port_handle);
	if (fc_port) {
		/* Set the port state to online */
		err = nvmf_fc_port_set_online(fc_port);
		if (err != 0) {
			SPDK_ERRLOG("Hw port %d online failed. err = %d\n", fc_port->port_hdl, err);
			DEV_VERIFY(!"Hw port online failed");
			goto out;
		}

		hwqp = &fc_port->ls_queue;
		hwqp->context = NULL;
		(void)nvmf_fc_hwqp_set_online(hwqp);

		/* Cycle through all the io queues and setup a hwqp poller for each. */
		for (i = 0; i < (int)fc_port->num_io_queues; i++) {
			hwqp = &fc_port->io_queues[i];
			hwqp->context = NULL;
			(void)nvmf_fc_hwqp_set_online(hwqp);
			nvmf_fc_poll_group_add_hwqp(hwqp);
		}
	} else {
		SPDK_ERRLOG("Unable to find the SPDK FC port %d\n", args->port_handle);
		err = -EINVAL;
	}

out:
	if (api_data->cb_func != NULL) {
		(void)api_data->cb_func(args->port_handle, SPDK_FC_HW_PORT_ONLINE, args->cb_ctx, err);
	}

	free(arg);

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "HW port %d online done, rc = %d.\n", args->port_handle,
		      err);
}

static void
nvmf_fc_adm_hw_port_offline_cb(void *ctx, int status)
{
	int err = 0;
	struct spdk_nvmf_fc_port *fc_port = NULL;
	struct spdk_nvmf_fc_remove_hwqp_cb_args *remove_hwqp_args = ctx;
	struct spdk_nvmf_fc_hw_port_offline_args *args = remove_hwqp_args->cb_args;

	if (--remove_hwqp_args->pending_remove_hwqp) {
		return;
	}

	fc_port = nvmf_fc_port_lookup(args->port_handle);
	if (!fc_port) {
		err = -EINVAL;
		SPDK_ERRLOG("fc_port not found.\n");
		goto out;
	}

	/*
	 * Delete all the nports. Ideally, the nports should have been purged
	 * before the offline event, in which case, only a validation is required.
	 */
	nvmf_fc_adm_hw_port_offline_nport_delete(fc_port);
out:
	if (remove_hwqp_args->cb_fn) {
		remove_hwqp_args->cb_fn(args->port_handle, SPDK_FC_HW_PORT_OFFLINE, args->cb_ctx, err);
	}

	free(remove_hwqp_args);
}

/*
 * Offline a HW port.
 */
static void
nvmf_fc_adm_evnt_hw_port_offline(void *arg)
{
	ASSERT_SPDK_FC_MAIN_THREAD();
	struct spdk_nvmf_fc_port *fc_port = NULL;
	struct spdk_nvmf_fc_hwqp *hwqp = NULL;
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;
	struct spdk_nvmf_fc_hw_port_offline_args *args = (struct spdk_nvmf_fc_hw_port_offline_args *)
			api_data->api_args;
	struct spdk_nvmf_fc_remove_hwqp_cb_args *remove_hwqp_args;
	int i = 0;
	int err = 0;

	fc_port = nvmf_fc_port_lookup(args->port_handle);
	if (fc_port) {
		/* Set the port state to offline, if it is not already. */
		err = nvmf_fc_port_set_offline(fc_port);
		if (err != 0) {
			SPDK_ERRLOG("Hw port %d already offline. err = %d\n", fc_port->port_hdl, err);
			err = 0;
			goto out;
		}

		remove_hwqp_args = calloc(1, sizeof(struct spdk_nvmf_fc_remove_hwqp_cb_args));
		if (!remove_hwqp_args) {
			SPDK_ERRLOG("Failed to alloc memory for remove_hwqp_args\n");
			err = -ENOMEM;
			goto out;
		}
		remove_hwqp_args->cb_fn = api_data->cb_func;
		remove_hwqp_args->cb_args = api_data->api_args;
		remove_hwqp_args->pending_remove_hwqp = fc_port->num_io_queues;

		hwqp = &fc_port->ls_queue;
		(void)nvmf_fc_hwqp_set_offline(hwqp);

		/* Remove poller for all the io queues. */
		for (i = 0; i < (int)fc_port->num_io_queues; i++) {
			hwqp = &fc_port->io_queues[i];
			(void)nvmf_fc_hwqp_set_offline(hwqp);
			nvmf_fc_poll_group_remove_hwqp(hwqp, nvmf_fc_adm_hw_port_offline_cb,
						       remove_hwqp_args);
		}

		free(arg);

		/* Wait until all the hwqps are removed from poll groups. */
		return;
	} else {
		SPDK_ERRLOG("Unable to find the SPDK FC port %d\n", args->port_handle);
		err = -EINVAL;
	}
out:
	if (api_data->cb_func != NULL) {
		(void)api_data->cb_func(args->port_handle, SPDK_FC_HW_PORT_OFFLINE, args->cb_ctx, err);
	}

	free(arg);

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "HW port %d offline done, rc = %d.\n", args->port_handle,
		      err);
}

struct nvmf_fc_add_rem_listener_ctx {
	struct spdk_nvmf_subsystem *subsystem;
	bool add_listener;
	struct spdk_nvme_transport_id trid;
};

static void
nvmf_fc_adm_subsystem_resume_cb(struct spdk_nvmf_subsystem *subsystem, void *cb_arg, int status)
{
	ASSERT_SPDK_FC_MAIN_THREAD();
	struct nvmf_fc_add_rem_listener_ctx *ctx = (struct nvmf_fc_add_rem_listener_ctx *)cb_arg;
	free(ctx);
}

static void
nvmf_fc_adm_listen_done(void *cb_arg, int status)
{
	ASSERT_SPDK_FC_MAIN_THREAD();
	struct nvmf_fc_add_rem_listener_ctx *ctx = cb_arg;

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, nvmf_fc_adm_subsystem_resume_cb, ctx)) {
		SPDK_ERRLOG("Failed to resume subsystem: %s\n", ctx->subsystem->subnqn);
		free(ctx);
	}
}

static void
nvmf_fc_adm_subsystem_paused_cb(struct spdk_nvmf_subsystem *subsystem, void *cb_arg, int status)
{
	ASSERT_SPDK_FC_MAIN_THREAD();
	struct nvmf_fc_add_rem_listener_ctx *ctx = (struct nvmf_fc_add_rem_listener_ctx *)cb_arg;

	if (ctx->add_listener) {
		spdk_nvmf_subsystem_add_listener(subsystem, &ctx->trid, nvmf_fc_adm_listen_done, ctx);
	} else {
		spdk_nvmf_subsystem_remove_listener(subsystem, &ctx->trid);
		nvmf_fc_adm_listen_done(ctx, 0);
	}
}

static int
nvmf_fc_adm_add_rem_nport_listener(struct spdk_nvmf_fc_nport *nport, bool add)
{
	struct spdk_nvmf_tgt *tgt = nvmf_fc_get_tgt();
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_listen_opts opts;

	if (!tgt) {
		SPDK_ERRLOG("No nvmf target defined\n");
		return -EINVAL;
	}

	spdk_nvmf_listen_opts_init(&opts, sizeof(opts));

	subsystem = spdk_nvmf_subsystem_get_first(tgt);
	while (subsystem) {
		struct nvmf_fc_add_rem_listener_ctx *ctx;

		if (spdk_nvmf_subsytem_any_listener_allowed(subsystem) == true) {
			ctx = calloc(1, sizeof(struct nvmf_fc_add_rem_listener_ctx));
			if (ctx) {
				ctx->add_listener = add;
				ctx->subsystem = subsystem;
				nvmf_fc_create_trid(&ctx->trid,
						    nport->fc_nodename.u.wwn,
						    nport->fc_portname.u.wwn);

				if (spdk_nvmf_tgt_listen_ext(subsystem->tgt, &ctx->trid, &opts)) {
					SPDK_ERRLOG("Failed to add transport address %s to tgt listeners\n",
						    ctx->trid.traddr);
					free(ctx);
				} else if (spdk_nvmf_subsystem_pause(subsystem,
								     0,
								     nvmf_fc_adm_subsystem_paused_cb,
								     ctx)) {
					SPDK_ERRLOG("Failed to pause subsystem: %s\n",
						    subsystem->subnqn);
					free(ctx);
				}
			}
		}

		subsystem = spdk_nvmf_subsystem_get_next(subsystem);
	}

	return 0;
}

/*
 * Create a Nport.
 */
static void
nvmf_fc_adm_evnt_nport_create(void *arg)
{
	ASSERT_SPDK_FC_MAIN_THREAD();
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;
	struct spdk_nvmf_fc_nport_create_args *args = (struct spdk_nvmf_fc_nport_create_args *)
			api_data->api_args;
	struct spdk_nvmf_fc_nport *nport = NULL;
	struct spdk_nvmf_fc_port *fc_port = NULL;
	int err = 0;

	/*
	 * Get the physical port.
	 */
	fc_port = nvmf_fc_port_lookup(args->port_handle);
	if (fc_port == NULL) {
		err = -EINVAL;
		goto out;
	}

	/*
	 * Check for duplicate initialization.
	 */
	nport = nvmf_fc_nport_find(args->port_handle, args->nport_handle);
	if (nport != NULL) {
		SPDK_ERRLOG("Duplicate SPDK FC nport %d exists for FC port:%d.\n", args->nport_handle,
			    args->port_handle);
		err = -EINVAL;
		goto out;
	}

	/*
	 * Get the memory to instantiate a fc nport.
	 */
	nport = calloc(1, sizeof(struct spdk_nvmf_fc_nport));
	if (nport == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for nport %d.\n",
			    args->nport_handle);
		err = -ENOMEM;
		goto out;
	}

	/*
	 * Initialize the contents for the nport
	 */
	nport->nport_hdl    = args->nport_handle;
	nport->port_hdl     = args->port_handle;
	nport->nport_state  = SPDK_NVMF_FC_OBJECT_CREATED;
	nport->fc_nodename  = args->fc_nodename;
	nport->fc_portname  = args->fc_portname;
	nport->d_id         = args->d_id;
	nport->fc_port      = nvmf_fc_port_lookup(args->port_handle);

	(void)nvmf_fc_nport_set_state(nport, SPDK_NVMF_FC_OBJECT_CREATED);
	TAILQ_INIT(&nport->rem_port_list);
	nport->rport_count = 0;
	TAILQ_INIT(&nport->fc_associations);
	nport->assoc_count = 0;

	/*
	 * Populate the nport address (as listening address) to the nvmf subsystems.
	 */
	err = nvmf_fc_adm_add_rem_nport_listener(nport, true);

	(void)nvmf_fc_port_add_nport(fc_port, nport);
out:
	if (err && nport) {
		free(nport);
	}

	if (api_data->cb_func != NULL) {
		(void)api_data->cb_func(args->port_handle, SPDK_FC_NPORT_CREATE, args->cb_ctx, err);
	}

	free(arg);
}

static void
nvmf_fc_adm_delete_nport_cb(uint8_t port_handle, enum spdk_fc_event event_type,
			    void *cb_args, int spdk_err)
{
	ASSERT_SPDK_FC_MAIN_THREAD();
	struct spdk_nvmf_fc_adm_nport_del_cb_data *cb_data = cb_args;
	struct spdk_nvmf_fc_nport *nport = cb_data->nport;
	spdk_nvmf_fc_callback cb_func = cb_data->fc_cb_func;
	int err = 0;
	uint16_t nport_hdl = 0;
	char log_str[256];

	/*
	 * Assert on any delete failure.
	 */
	if (nport == NULL) {
		SPDK_ERRLOG("Nport delete callback returned null nport");
		DEV_VERIFY(!"nport is null.");
		goto out;
	}

	nport_hdl = nport->nport_hdl;
	if (0 != spdk_err) {
		SPDK_ERRLOG("Nport delete callback returned error. FC Port: "
			    "%d, Nport: %d\n",
			    nport->port_hdl, nport->nport_hdl);
		DEV_VERIFY(!"nport delete callback error.");
	}

	/*
	 * Free the nport if this is the last rport being deleted and
	 * execute the callback(s).
	 */
	if (nvmf_fc_nport_has_no_rport(nport)) {
		if (0 != nport->assoc_count) {
			SPDK_ERRLOG("association count != 0\n");
			DEV_VERIFY(!"association count != 0");
		}

		err = nvmf_fc_port_remove_nport(nport->fc_port, nport);
		if (0 != err) {
			SPDK_ERRLOG("Nport delete callback: Failed to remove "
				    "nport from nport list. FC Port:%d Nport:%d\n",
				    nport->port_hdl, nport->nport_hdl);
		}
		/* Free the nport */
		free(nport);

		if (cb_func != NULL) {
			(void)cb_func(cb_data->port_handle, SPDK_FC_NPORT_DELETE, cb_data->fc_cb_ctx, spdk_err);
		}
		free(cb_data);
	}
out:
	snprintf(log_str, sizeof(log_str),
		 "port:%d nport:%d delete cb exit, evt_type:%d rc:%d.\n",
		 port_handle, nport_hdl, event_type, spdk_err);

	if (err != 0) {
		SPDK_ERRLOG("%s", log_str);
	} else {
		SPDK_DEBUGLOG(nvmf_fc_adm_api, "%s", log_str);
	}
}

/*
 * Delete Nport.
 */
static void
nvmf_fc_adm_evnt_nport_delete(void *arg)
{
	ASSERT_SPDK_FC_MAIN_THREAD();
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;
	struct spdk_nvmf_fc_nport_delete_args *args = (struct spdk_nvmf_fc_nport_delete_args *)
			api_data->api_args;
	struct spdk_nvmf_fc_nport *nport = NULL;
	struct spdk_nvmf_fc_adm_nport_del_cb_data *cb_data = NULL;
	struct spdk_nvmf_fc_remote_port_info *rport_iter = NULL;
	int err = 0;
	uint32_t rport_cnt = 0;
	int rc = 0;

	/*
	 * Make sure that the nport exists.
	 */
	nport = nvmf_fc_nport_find(args->port_handle, args->nport_handle);
	if (nport == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC nport %d for FC Port: %d.\n", args->nport_handle,
			    args->port_handle);
		err = -EINVAL;
		goto out;
	}

	/*
	 * Allocate memory for callback data.
	 */
	cb_data = calloc(1, sizeof(struct spdk_nvmf_fc_adm_nport_del_cb_data));
	if (NULL == cb_data) {
		SPDK_ERRLOG("Failed to allocate memory for cb_data %d.\n", args->nport_handle);
		err = -ENOMEM;
		goto out;
	}

	cb_data->nport = nport;
	cb_data->port_handle = args->port_handle;
	cb_data->fc_cb_func = api_data->cb_func;
	cb_data->fc_cb_ctx = args->cb_ctx;

	/*
	 * Begin nport tear down
	 */
	if (nport->nport_state == SPDK_NVMF_FC_OBJECT_CREATED) {
		(void)nvmf_fc_nport_set_state(nport, SPDK_NVMF_FC_OBJECT_TO_BE_DELETED);
	} else if (nport->nport_state == SPDK_NVMF_FC_OBJECT_TO_BE_DELETED) {
		/*
		 * Deletion of this nport already in progress. Register callback
		 * and return.
		 */
		/* TODO: Register callback in callback vector. For now, set the error and return. */
		err = -ENODEV;
		goto out;
	} else {
		/* nport partially created/deleted */
		DEV_VERIFY(nport->nport_state == SPDK_NVMF_FC_OBJECT_ZOMBIE);
		DEV_VERIFY(0 != "Nport in zombie state");
		err = -ENODEV;
		goto out;
	}

	/*
	 * Remove this nport from listening addresses across subsystems
	 */
	rc = nvmf_fc_adm_add_rem_nport_listener(nport, false);

	if (0 != rc) {
		err = nvmf_fc_nport_set_state(nport, SPDK_NVMF_FC_OBJECT_ZOMBIE);
		SPDK_ERRLOG("Unable to remove the listen addr in the subsystems for nport %d.\n",
			    nport->nport_hdl);
		goto out;
	}

	/*
	 * Delete all the remote ports (if any) for the nport
	 */
	/* TODO - Need to do this with a "first" and a "next" accessor function
	 * for completeness. Look at app-subsystem as examples.
	 */
	if (nvmf_fc_nport_has_no_rport(nport)) {
		/* No rports to delete. Complete the nport deletion. */
		nvmf_fc_adm_delete_nport_cb(nport->port_hdl, SPDK_FC_NPORT_DELETE, cb_data, 0);
		goto out;
	}

	TAILQ_FOREACH(rport_iter, &nport->rem_port_list, link) {
		struct spdk_nvmf_fc_hw_i_t_delete_args *it_del_args = calloc(
					1, sizeof(struct spdk_nvmf_fc_hw_i_t_delete_args));

		if (it_del_args == NULL) {
			err = -ENOMEM;
			SPDK_ERRLOG("SPDK_FC_IT_DELETE no mem to delete rport with rpi:%d s_id:%d.\n",
				    rport_iter->rpi, rport_iter->s_id);
			DEV_VERIFY(!"SPDK_FC_IT_DELETE failed, cannot allocate memory");
			goto out;
		}

		rport_cnt++;
		it_del_args->port_handle = nport->port_hdl;
		it_del_args->nport_handle = nport->nport_hdl;
		it_del_args->cb_ctx = (void *)cb_data;
		it_del_args->rpi = rport_iter->rpi;
		it_del_args->s_id = rport_iter->s_id;

		nvmf_fc_main_enqueue_event(SPDK_FC_IT_DELETE, (void *)it_del_args,
					   nvmf_fc_adm_delete_nport_cb);
	}

out:
	/* On failure, execute the callback function now */
	if ((err != 0) || (rc != 0)) {
		SPDK_ERRLOG("NPort %d delete failed, error:%d, fc port:%d, "
			    "rport_cnt:%d rc:%d.\n",
			    args->nport_handle, err, args->port_handle,
			    rport_cnt, rc);
		if (cb_data) {
			free(cb_data);
		}
		if (api_data->cb_func != NULL) {
			(void)api_data->cb_func(args->port_handle, SPDK_FC_NPORT_DELETE, args->cb_ctx, err);
		}

	} else {
		SPDK_DEBUGLOG(nvmf_fc_adm_api,
			      "NPort %d delete done successfully, fc port:%d. "
			      "rport_cnt:%d\n",
			      args->nport_handle, args->port_handle, rport_cnt);
	}

	free(arg);
}

/*
 * Process an PRLI/IT add.
 */
static void
nvmf_fc_adm_evnt_i_t_add(void *arg)
{
	ASSERT_SPDK_FC_MAIN_THREAD();
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;
	struct spdk_nvmf_fc_hw_i_t_add_args *args = (struct spdk_nvmf_fc_hw_i_t_add_args *)
			api_data->api_args;
	struct spdk_nvmf_fc_nport *nport = NULL;
	struct spdk_nvmf_fc_remote_port_info *rport_iter = NULL;
	struct spdk_nvmf_fc_remote_port_info *rport = NULL;
	int err = 0;

	/*
	 * Make sure the nport port exists.
	 */
	nport = nvmf_fc_nport_find(args->port_handle, args->nport_handle);
	if (nport == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC nport %d\n", args->nport_handle);
		err = -EINVAL;
		goto out;
	}

	/*
	 * Check for duplicate i_t_add.
	 */
	TAILQ_FOREACH(rport_iter, &nport->rem_port_list, link) {
		if ((rport_iter->s_id == args->s_id) && (rport_iter->rpi == args->rpi)) {
			SPDK_ERRLOG("Duplicate rport found for FC nport %d: sid:%d rpi:%d\n",
				    args->nport_handle, rport_iter->s_id, rport_iter->rpi);
			err = -EEXIST;
			goto out;
		}
	}

	/*
	 * Get the memory to instantiate the remote port
	 */
	rport = calloc(1, sizeof(struct spdk_nvmf_fc_remote_port_info));
	if (rport == NULL) {
		SPDK_ERRLOG("Memory allocation for rem port failed.\n");
		err = -ENOMEM;
		goto out;
	}

	/*
	 * Initialize the contents for the rport
	 */
	(void)nvmf_fc_rport_set_state(rport, SPDK_NVMF_FC_OBJECT_CREATED);
	rport->s_id = args->s_id;
	rport->rpi = args->rpi;
	rport->fc_nodename = args->fc_nodename;
	rport->fc_portname = args->fc_portname;

	/*
	 * Add remote port to nport
	 */
	if (nvmf_fc_nport_add_rem_port(nport, rport) != 0) {
		DEV_VERIFY(!"Error while adding rport to list");
	};

	/*
	 * TODO: Do we validate the initiators service parameters?
	 */

	/*
	 * Get the targets service parameters from the library
	 * to return back to the driver.
	 */
	args->target_prli_info = nvmf_fc_get_prli_service_params();

out:
	if (api_data->cb_func != NULL) {
		/*
		 * Passing pointer to the args struct as the first argument.
		 * The cb_func should handle this appropriately.
		 */
		(void)api_data->cb_func(args->port_handle, SPDK_FC_IT_ADD, args->cb_ctx, err);
	}

	free(arg);

	SPDK_DEBUGLOG(nvmf_fc_adm_api,
		      "IT add on nport %d done, rc = %d.\n",
		      args->nport_handle, err);
}

/**
 * Process a IT delete.
 */
static void
nvmf_fc_adm_evnt_i_t_delete(void *arg)
{
	ASSERT_SPDK_FC_MAIN_THREAD();
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;
	struct spdk_nvmf_fc_hw_i_t_delete_args *args = (struct spdk_nvmf_fc_hw_i_t_delete_args *)
			api_data->api_args;
	int rc = 0;
	struct spdk_nvmf_fc_nport *nport = NULL;
	struct spdk_nvmf_fc_adm_i_t_del_cb_data *cb_data = NULL;
	struct spdk_nvmf_fc_remote_port_info *rport_iter = NULL;
	struct spdk_nvmf_fc_remote_port_info *rport = NULL;
	uint32_t num_rport = 0;
	char log_str[256];

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "IT delete on nport:%d begin.\n", args->nport_handle);

	/*
	 * Make sure the nport port exists. If it does not, error out.
	 */
	nport = nvmf_fc_nport_find(args->port_handle, args->nport_handle);
	if (nport == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC nport:%d\n", args->nport_handle);
		rc = -EINVAL;
		goto out;
	}

	/*
	 * Find this ITN / rport (remote port).
	 */
	TAILQ_FOREACH(rport_iter, &nport->rem_port_list, link) {
		num_rport++;
		if ((rport_iter->s_id == args->s_id) &&
		    (rport_iter->rpi == args->rpi) &&
		    (rport_iter->rport_state == SPDK_NVMF_FC_OBJECT_CREATED)) {
			rport = rport_iter;
			break;
		}
	}

	/*
	 * We should find either zero or exactly one rport.
	 *
	 * If we find zero rports, that means that a previous request has
	 * removed the rport by the time we reached here. In this case,
	 * simply return out.
	 */
	if (rport == NULL) {
		rc = -ENODEV;
		goto out;
	}

	/*
	 * We have the rport slated for deletion. At this point clean up
	 * any LS requests that are sitting in the pending list. Do this
	 * first, then, set the states of the rport so that new LS requests
	 * are not accepted. Then start the cleanup.
	 */
	nvmf_fc_delete_ls_pending(&(nport->fc_port->ls_queue), nport, rport);

	/*
	 * We have found exactly one rport. Allocate memory for callback data.
	 */
	cb_data = calloc(1, sizeof(struct spdk_nvmf_fc_adm_i_t_del_cb_data));
	if (NULL == cb_data) {
		SPDK_ERRLOG("Failed to allocate memory for cb_data for nport:%d.\n", args->nport_handle);
		rc = -ENOMEM;
		goto out;
	}

	cb_data->nport = nport;
	cb_data->rport = rport;
	cb_data->port_handle = args->port_handle;
	cb_data->fc_cb_func = api_data->cb_func;
	cb_data->fc_cb_ctx = args->cb_ctx;

	/*
	 * Validate rport object state.
	 */
	if (rport->rport_state == SPDK_NVMF_FC_OBJECT_CREATED) {
		(void)nvmf_fc_rport_set_state(rport, SPDK_NVMF_FC_OBJECT_TO_BE_DELETED);
	} else if (rport->rport_state == SPDK_NVMF_FC_OBJECT_TO_BE_DELETED) {
		/*
		 * Deletion of this rport already in progress. Register callback
		 * and return.
		 */
		/* TODO: Register callback in callback vector. For now, set the error and return. */
		rc = -ENODEV;
		goto out;
	} else {
		/* rport partially created/deleted */
		DEV_VERIFY(rport->rport_state == SPDK_NVMF_FC_OBJECT_ZOMBIE);
		DEV_VERIFY(!"Invalid rport_state");
		rc = -ENODEV;
		goto out;
	}

	/*
	 * We have successfully found a rport to delete. Call
	 * nvmf_fc_i_t_delete_assoc(), which will perform further
	 * IT-delete processing as well as free the cb_data.
	 */
	nvmf_fc_adm_i_t_delete_assoc(nport, rport, nvmf_fc_adm_i_t_delete_cb,
				     (void *)cb_data);

out:
	if (rc != 0) {
		/*
		 * We have entered here because either we encountered an
		 * error, or we did not find a rport to delete.
		 * As a result, we will not call the function
		 * nvmf_fc_i_t_delete_assoc() for further IT-delete
		 * processing. Therefore, execute the callback function now.
		 */
		if (cb_data) {
			free(cb_data);
		}
		if (api_data->cb_func != NULL) {
			(void)api_data->cb_func(args->port_handle, SPDK_FC_IT_DELETE, args->cb_ctx, rc);
		}
	}

	snprintf(log_str, sizeof(log_str),
		 "IT delete on nport:%d end. num_rport:%d rc = %d.\n",
		 args->nport_handle, num_rport, rc);

	if (rc != 0) {
		SPDK_ERRLOG("%s", log_str);
	} else {
		SPDK_DEBUGLOG(nvmf_fc_adm_api, "%s", log_str);
	}

	free(arg);
}

/*
 * Process ABTS received
 */
static void
nvmf_fc_adm_evnt_abts_recv(void *arg)
{
	ASSERT_SPDK_FC_MAIN_THREAD();
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;
	struct spdk_nvmf_fc_abts_args *args = (struct spdk_nvmf_fc_abts_args *)api_data->api_args;
	struct spdk_nvmf_fc_nport *nport = NULL;
	int err = 0;

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "FC ABTS received. RPI:%d, oxid:%d, rxid:%d\n", args->rpi,
		      args->oxid, args->rxid);

	/*
	 * 1. Make sure the nport port exists.
	 */
	nport = nvmf_fc_nport_find(args->port_handle, args->nport_handle);
	if (nport == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC nport %d\n", args->nport_handle);
		err = -EINVAL;
		goto out;
	}

	/*
	 * 2. If the nport is in the process of being deleted, drop the ABTS.
	 */
	if (nport->nport_state == SPDK_NVMF_FC_OBJECT_TO_BE_DELETED) {
		SPDK_DEBUGLOG(nvmf_fc_adm_api,
			      "FC ABTS dropped because the nport is being deleted; RPI:%d, oxid:%d, rxid:%d\n",
			      args->rpi, args->oxid, args->rxid);
		err = 0;
		goto out;

	}

	/*
	 * 3. Pass the received ABTS-LS to the library for handling.
	 */
	nvmf_fc_handle_abts_frame(nport, args->rpi, args->oxid, args->rxid);

out:
	if (api_data->cb_func != NULL) {
		/*
		 * Passing pointer to the args struct as the first argument.
		 * The cb_func should handle this appropriately.
		 */
		(void)api_data->cb_func(args->port_handle, SPDK_FC_ABTS_RECV, args, err);
	} else {
		/* No callback set, free the args */
		free(args);
	}

	free(arg);
}

/*
 * Callback function for hw port quiesce.
 */
static void
nvmf_fc_adm_hw_port_quiesce_reset_cb(void *ctx, int err)
{
	ASSERT_SPDK_FC_MAIN_THREAD();
	struct spdk_nvmf_fc_adm_hw_port_reset_ctx *reset_ctx =
		(struct spdk_nvmf_fc_adm_hw_port_reset_ctx *)ctx;
	struct spdk_nvmf_fc_hw_port_reset_args *args = reset_ctx->reset_args;
	spdk_nvmf_fc_callback cb_func = reset_ctx->reset_cb_func;
	struct spdk_nvmf_fc_queue_dump_info dump_info;
	struct spdk_nvmf_fc_port *fc_port = NULL;
	char *dump_buf = NULL;
	uint32_t dump_buf_size = SPDK_FC_HW_DUMP_BUF_SIZE;

	/*
	 * Free the callback context struct.
	 */
	free(ctx);

	if (err != 0) {
		SPDK_ERRLOG("Port %d  quiesce operation failed.\n", args->port_handle);
		goto out;
	}

	if (args->dump_queues == false) {
		/*
		 * Queues need not be dumped.
		 */
		goto out;
	}

	SPDK_ERRLOG("Dumping queues for HW port %d\n", args->port_handle);

	/*
	 * Get the fc port.
	 */
	fc_port = nvmf_fc_port_lookup(args->port_handle);
	if (fc_port == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC port %d\n", args->port_handle);
		err = -EINVAL;
		goto out;
	}

	/*
	 * Allocate memory for the dump buffer.
	 * This memory will be freed by FCT.
	 */
	dump_buf = (char *)calloc(1, dump_buf_size);
	if (dump_buf == NULL) {
		err = -ENOMEM;
		SPDK_ERRLOG("Memory allocation for dump buffer failed, SPDK FC port %d\n", args->port_handle);
		goto out;
	}
	*args->dump_buf  = (uint32_t *)dump_buf;
	dump_info.buffer = dump_buf;
	dump_info.offset = 0;

	/*
	 * Add the dump reason to the top of the buffer.
	 */
	nvmf_fc_dump_buf_print(&dump_info, "%s\n", args->reason);

	/*
	 * Dump the hwqp.
	 */
	nvmf_fc_dump_all_queues(&fc_port->ls_queue, fc_port->io_queues,
				fc_port->num_io_queues, &dump_info);

out:
	SPDK_DEBUGLOG(nvmf_fc_adm_api, "HW port %d reset done, queues_dumped = %d, rc = %d.\n",
		      args->port_handle, args->dump_queues, err);

	if (cb_func != NULL) {
		(void)cb_func(args->port_handle, SPDK_FC_HW_PORT_RESET, args->cb_ctx, err);
	}
}

/*
 * HW port reset

 */
static void
nvmf_fc_adm_evnt_hw_port_reset(void *arg)
{
	ASSERT_SPDK_FC_MAIN_THREAD();
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;
	struct spdk_nvmf_fc_hw_port_reset_args *args = (struct spdk_nvmf_fc_hw_port_reset_args *)
			api_data->api_args;
	struct spdk_nvmf_fc_port *fc_port = NULL;
	struct spdk_nvmf_fc_adm_hw_port_reset_ctx *ctx = NULL;
	int err = 0;

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "HW port %d dump\n", args->port_handle);

	/*
	 * Make sure the physical port exists.
	 */
	fc_port = nvmf_fc_port_lookup(args->port_handle);
	if (fc_port == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC port %d\n", args->port_handle);
		err = -EINVAL;
		goto out;
	}

	/*
	 * Save the reset event args and the callback in a context struct.
	 */
	ctx = calloc(1, sizeof(struct spdk_nvmf_fc_adm_hw_port_reset_ctx));

	if (ctx == NULL) {
		err = -ENOMEM;
		SPDK_ERRLOG("Memory allocation for reset ctx failed, SPDK FC port %d\n", args->port_handle);
		goto fail;
	}

	ctx->reset_args = args;
	ctx->reset_cb_func = api_data->cb_func;

	/*
	 * Quiesce the hw port.
	 */
	err = nvmf_fc_adm_hw_port_quiesce(fc_port, ctx, nvmf_fc_adm_hw_port_quiesce_reset_cb);
	if (err != 0) {
		goto fail;
	}

	/*
	 * Once the ports are successfully quiesced the reset processing
	 * will continue in the callback function: spdk_fc_port_quiesce_reset_cb
	 */
	return;
fail:
	free(ctx);

out:
	SPDK_DEBUGLOG(nvmf_fc_adm_api, "HW port %d dump done, rc = %d.\n", args->port_handle,
		      err);

	if (api_data->cb_func != NULL) {
		(void)api_data->cb_func(args->port_handle, SPDK_FC_HW_PORT_RESET, args->cb_ctx, err);
	}

	free(arg);
}

static inline void
nvmf_fc_adm_run_on_main_thread(spdk_msg_fn fn, void *args)
{
	if (nvmf_fc_get_main_thread()) {
		spdk_thread_send_msg(nvmf_fc_get_main_thread(), fn, args);
	}
}

/*
 * Queue up an event in the SPDK main threads event queue.
 * Used by the FC driver to notify the SPDK main thread of FC related events.
 */
int
nvmf_fc_main_enqueue_event(enum spdk_fc_event event_type, void *args,
			   spdk_nvmf_fc_callback cb_func)
{
	int err = 0;
	struct spdk_nvmf_fc_adm_api_data *api_data = NULL;
	spdk_msg_fn event_fn = NULL;

	SPDK_DEBUGLOG(nvmf_fc_adm_api, "Enqueue event %d.\n", event_type);

	if (event_type >= SPDK_FC_EVENT_MAX) {
		SPDK_ERRLOG("Invalid spdk_fc_event_t %d.\n", event_type);
		err = -EINVAL;
		goto done;
	}

	if (args == NULL) {
		SPDK_ERRLOG("Null args for event %d.\n", event_type);
		err = -EINVAL;
		goto done;
	}

	api_data = calloc(1, sizeof(*api_data));

	if (api_data == NULL) {
		SPDK_ERRLOG("Failed to alloc api data for event %d.\n", event_type);
		err = -ENOMEM;
		goto done;
	}

	api_data->api_args = args;
	api_data->cb_func = cb_func;

	switch (event_type) {
	case SPDK_FC_HW_PORT_INIT:
		event_fn = nvmf_fc_adm_evnt_hw_port_init;
		break;

	case SPDK_FC_HW_PORT_FREE:
		event_fn = nvmf_fc_adm_evnt_hw_port_free;
		break;

	case SPDK_FC_HW_PORT_ONLINE:
		event_fn = nvmf_fc_adm_evnt_hw_port_online;
		break;

	case SPDK_FC_HW_PORT_OFFLINE:
		event_fn = nvmf_fc_adm_evnt_hw_port_offline;
		break;

	case SPDK_FC_NPORT_CREATE:
		event_fn = nvmf_fc_adm_evnt_nport_create;
		break;

	case SPDK_FC_NPORT_DELETE:
		event_fn = nvmf_fc_adm_evnt_nport_delete;
		break;

	case SPDK_FC_IT_ADD:
		event_fn = nvmf_fc_adm_evnt_i_t_add;
		break;

	case SPDK_FC_IT_DELETE:
		event_fn = nvmf_fc_adm_evnt_i_t_delete;
		break;

	case SPDK_FC_ABTS_RECV:
		event_fn = nvmf_fc_adm_evnt_abts_recv;
		break;

	case SPDK_FC_HW_PORT_RESET:
		event_fn = nvmf_fc_adm_evnt_hw_port_reset;
		break;

	case SPDK_FC_UNRECOVERABLE_ERR:
	default:
		SPDK_ERRLOG("Invalid spdk_fc_event_t: %d\n", event_type);
		err = -EINVAL;
		break;
	}

done:

	if (err == 0) {
		assert(event_fn != NULL);
		nvmf_fc_adm_run_on_main_thread(event_fn, (void *)api_data);
		SPDK_DEBUGLOG(nvmf_fc_adm_api, "Enqueue event %d done successfully\n", event_type);
	} else {
		SPDK_ERRLOG("Enqueue event %d failed, err = %d\n", event_type, err);
		if (api_data) {
			free(api_data);
		}
	}

	return err;
}

SPDK_NVMF_TRANSPORT_REGISTER(fc, &spdk_nvmf_transport_fc);
SPDK_LOG_REGISTER_COMPONENT(nvmf_fc_adm_api)
SPDK_LOG_REGISTER_COMPONENT(nvmf_fc)
