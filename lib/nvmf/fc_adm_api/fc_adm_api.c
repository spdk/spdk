/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2018 NetApp, Inc.
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

#include "spdk/trace.h"
#include "spdk_internal/log.h"
#include "spdk/nvmf_spec.h"
#include "spdk/log.h"
#include "spdk/endian.h"
#include "spdk/string.h"
#include "spdk/fc_adm_api.h"
#include "../nvmf_fc.h"


#ifndef DEV_VERIFY
#define DEV_VERIFY assert
#endif

#ifndef ASSERT_SPDK_FC_MASTER_THREAD
#define ASSERT_SPDK_FC_MASTER_THREAD() \
        DEV_VERIFY(spdk_get_thread() == spdk_nvmf_fc_get_master_thread());
#endif

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

/**
 * The callback structure for HW port link break event
 */
struct spdk_nvmf_fc_adm_port_link_break_cb_data {
	struct spdk_nvmf_hw_port_link_break_args *args;
	struct spdk_nvmf_fc_nport_delete_args nport_del_args;
	spdk_nvmf_fc_callback cb_func;
};

/*
 * Re-initialize the FC-Port after an offline event.
 * Only the queue information needs to be populated. XCHG, lcore and other hwqp information remains
 * unchanged after the first initialization.
 *
 */
static int
nvmf_fc_adm_hw_port_reinit_validate(struct spdk_nvmf_fc_port *fc_port,
				    struct spdk_nvmf_fc_hw_port_init_args *args)
{
	int err = 0;
	uint32_t i;

	/* Verify that the port was previously in offline or quiesced state */
	if (spdk_nvmf_fc_port_is_online(fc_port)) {
		SPDK_ERRLOG("SPDK FC port %d already initialized and online.\n", args->port_handle);
		err = EINVAL;
		goto err;
	}

	/* Reinit information in new LS queue from previous queue */
	spdk_nvmf_fc_reinit_poller_queues(&fc_port->ls_queue, args->ls_queue);

	fc_port->fcp_rq_id = args->fcp_rq_id;

	/* Initialize the LS queue */
	fc_port->ls_queue.queues = args->ls_queue;
	spdk_nvmf_fc_init_poller_queues(fc_port->ls_queue.queues);

	for (i = 0; i < fc_port->num_io_queues; i++) {
		/* Reinit information in new IO queue from previous queue */
		spdk_nvmf_fc_reinit_poller_queues(&fc_port->io_queues[i],
						  args->io_queues[i]);
		fc_port->io_queues[i].queues = args->io_queues[i];
		/* Initialize the IO queues */
		spdk_nvmf_fc_init_poller_queues(fc_port->io_queues[i].queues);
	}


	fc_port->hw_port_status = SPDK_FC_PORT_OFFLINE;

	/* Validate the port information */
	DEV_VERIFY(TAILQ_EMPTY(&fc_port->nport_list));
	DEV_VERIFY(fc_port->num_nports == 0);
	if (!TAILQ_EMPTY(&fc_port->nport_list) || (fc_port->num_nports != 0)) {
		err = EINVAL;
	}

err:
	return err;
}

/* Initializes the data for the creation of a FC-Port object in the SPDK
 * library. The spdk_nvmf_fc_port is a well defined structure that is part of
 * the API to the library. The contents added to this well defined structure
 * is private to each vendors implementation.
 */
static int
nvmf_fc_adm_hw_port_data_init(struct spdk_nvmf_fc_port *fc_port,
			      struct spdk_nvmf_fc_hw_port_init_args *args)
{
	/* Used a high number for the LS HWQP so that it does not clash with the
	 * IO HWQP's and immediately shows a LS queue during tracing.
	 */
	uint32_t i;

	fc_port->port_hdl       = args->port_handle;
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
	fc_port->ls_queue.thread = spdk_nvmf_fc_get_master_thread();
	fc_port->ls_queue.hwqp_id = SPDK_MAX_NUM_OF_FC_PORTS * fc_port->num_io_queues;

	/*
	 * Initialize the LS poller.
	 */
	spdk_nvmf_fc_init_hwqp(fc_port, &fc_port->ls_queue);

	/*
	 * Initialize the IO queues.
	 */
	for (i = 0; i < args->io_queue_cnt; i++) {
		struct spdk_nvmf_fc_hwqp *hwqp = &fc_port->io_queues[i];
		hwqp->hwqp_id = i;
		hwqp->queues = args->io_queues[i];
		hwqp->rq_size = args->io_queue_size;
		hwqp->nvme_aq = args->nvme_aq_index == i ? true : false;
		spdk_nvmf_fc_init_hwqp(fc_port, hwqp);
	}

	/*
	 * Initialize the LS processing for port
	 */
	spdk_nvmf_fc_ls_init(fc_port);

	/*
	 * Initialize the list of nport on this HW port.
	 */
	TAILQ_INIT(&fc_port->nport_list);
	fc_port->num_nports = 0;

	return 0;
}

static void
nvmf_fc_adm_port_hwqp_offline_del_poller(struct spdk_nvmf_fc_port *fc_port)
{
	struct spdk_nvmf_fc_hwqp *hwqp    = NULL;
	int i = 0;

	hwqp = &fc_port->ls_queue;
	(void)spdk_nvmf_fc_hwqp_set_offline(hwqp);

	/*  Remove poller for all the io queues. */
	for (i = 0; i < (int)fc_port->num_io_queues; i++) {
		hwqp = &fc_port->io_queues[i];
		(void)spdk_nvmf_fc_hwqp_set_offline(hwqp);
		spdk_nvmf_fc_remove_hwqp_from_poller(hwqp);
	}
}

/*
 * Callback function for HW port link break operation.
 *
 * Notice that this callback is being triggered when spdk_fc_nport_delete()
 * completes, if that spdk_fc_nport_delete() called is issued by
 * nvmf_fc_adm_evnt_hw_port_link_break().
 *
 * Since nvmf_fc_adm_evnt_hw_port_link_break() can invoke spdk_fc_nport_delete() multiple
 * times (one per nport in the HW port's nport_list), a single call to
 * nvmf_fc_adm_evnt_hw_port_link_break() can result in multiple calls to this callback function.
 *
 * As a result, this function only invokes a callback to the caller of
 * nvmf_fc_adm_evnt_hw_port_link_break() only when the HW port's nport_list is empty.
 */
static void
nvmf_fc_adm_hw_port_link_break_cb(uint8_t port_handle,
				  enum spdk_fc_event event_type, void *cb_args, int spdk_err)
{
	ASSERT_SPDK_FC_MASTER_THREAD();
	struct spdk_nvmf_fc_adm_port_link_break_cb_data *offline_cb_args = cb_args;
	struct spdk_nvmf_hw_port_link_break_args *offline_args = NULL;
	spdk_nvmf_fc_callback cb_func = NULL;
	int err = 0;
	struct spdk_nvmf_fc_port *fc_port = NULL;
	int num_nports = 0;
	char log_str[256];

	if (0 != spdk_err) {
		DEV_VERIFY(!"port link break cb: spdk_err not success.");
		SPDK_ERRLOG("port link break cb: spdk_err:%d.\n", spdk_err);
		goto out;
	}

	if (!offline_cb_args) {
		DEV_VERIFY(!"port link break cb: port_offline_args is NULL.");
		err = EINVAL;
		goto out;
	}

	offline_args = offline_cb_args->args;
	if (!offline_args) {
		DEV_VERIFY(!"port link break cb: offline_args is NULL.");
		err = EINVAL;
		goto out;
	}

	if (port_handle != offline_args->port_handle) {
		DEV_VERIFY(!"port link break cb: port_handle mismatch.");
		err = EINVAL;
		goto out;
	}

	cb_func = offline_cb_args->cb_func;
	if (!cb_func) {
		DEV_VERIFY(!"port link break cb: cb_func is NULL.");
		err = EINVAL;
		goto out;
	}

	fc_port = spdk_nvmf_fc_port_list_get(port_handle);
	if (!fc_port) {
		DEV_VERIFY(!"port link break cb: fc_port is NULL.");
		SPDK_ERRLOG("port link break cb: Unable to find port:%d\n",
			    offline_args->port_handle);
		err =  EINVAL;
		goto out;
	}

	num_nports = fc_port->num_nports;
	if (!TAILQ_EMPTY(&fc_port->nport_list)) {
		/*
		 * Don't call the callback unless all nports have been deleted.
		 */
		goto out;
	}

	if (num_nports != 0) {
		DEV_VERIFY(!"port link break cb: num_nports in non-zero.");
		SPDK_ERRLOG("port link break cb: # of ports should be 0. Instead, num_nports:%d\n",
			    num_nports);
		err =  EINVAL;
	}

	/*
	 * Mark the hwqps as offline and unregister the pollers.
	 */
	(void)nvmf_fc_adm_port_hwqp_offline_del_poller(fc_port);

	/*
	 * Since there are no more nports, execute the callback(s).
	 */
	(void)cb_func(port_handle, SPDK_FC_LINK_BREAK,
		      (void *)offline_args->cb_ctx, spdk_err);

out:
	free(offline_cb_args);

	snprintf(log_str, sizeof(log_str),
		 "port link break cb: port:%d evt_type:%d num_nports:%d err:%d spdk_err:%d.\n",
		 port_handle, event_type, num_nports, err, spdk_err);

	if (err != 0) {
		SPDK_ERRLOG("%s", log_str);
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "%s", log_str);
	}
	return;
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
			(void)spdk_nvmf_fc_nport_set_state(nport, SPDK_NVMF_FC_OBJECT_ZOMBIE);
		}
	}
}

static void
nvmf_fc_adm_i_t_delete_cb(void *args, uint32_t err)
{
	ASSERT_SPDK_FC_MASTER_THREAD();
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
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "%s", log_str);
	}
}

static void
nvmf_fc_adm_i_t_delete_assoc_cb(void *args, uint32_t err)
{
	ASSERT_SPDK_FC_MASTER_THREAD();
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
		if (spdk_nvmf_fc_nport_remove_rem_port(nport, rport) != 0) {
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
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "%s", log_str);
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

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "IT delete associations on nport:%d begin.\n",
		      nport->nport_hdl);

	/*
	 * Allocate memory for callback data.
	 * This memory will be freed by the callback function.
	 */
	cb_data = calloc(1, sizeof(struct spdk_nvmf_fc_adm_i_t_del_assoc_cb_data));
	if (NULL == cb_data) {
		SPDK_ERRLOG("Failed to allocate memory for cb_data on nport:%d.\n", nport->nport_hdl);
		err = ENOMEM;
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
			assoc_err = spdk_nvmf_fc_delete_association(nport,
					assoc->assoc_id,
					false /* send abts */,
					nvmf_fc_adm_i_t_delete_assoc_cb, cb_data);
			if (0 != assoc_err) {
				/*
				 * Mark this association as zombie.
				 */
				err = EINVAL;
				DEV_VERIFY(!"Error while deleting association");
				(void)spdk_nvmf_fc_assoc_set_state(assoc, SPDK_NVMF_FC_OBJECT_ZOMBIE);
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
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "%s", log_str);
	} else {
		SPDK_ERRLOG("%s", log_str);
	}
}

static void
nvmf_fc_adm_queue_quiesce_cb(void *cb_data, enum spdk_nvmf_fc_poller_api_ret ret)
{
	ASSERT_SPDK_FC_MASTER_THREAD();
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
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "Queue%d Quiesced\n", quiesce_api_data->hwqp->hwqp_id);

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
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "HW port %d quiesced.\n", fc_port->port_hdl);
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

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "HW port %d quiesce done, rc = %d.\n", fc_port->port_hdl,
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
		err = ENOMEM;
		SPDK_ERRLOG("Failed to allocate memory for poller quiesce args, hwqp:%d\n", fc_hwqp->hwqp_id);
		goto done;
	}
	args->hwqp = fc_hwqp;
	args->ctx = ctx;
	args->cb_info.cb_func = cb_func;
	args->cb_info.cb_data = args;
	args->cb_info.cb_thread = spdk_get_thread();

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "Quiesce queue %d\n", fc_hwqp->hwqp_id);
	rc = spdk_nvmf_fc_poller_api_func(fc_hwqp, SPDK_NVMF_FC_POLLER_API_QUIESCE_QUEUE, args);
	if (rc) {
		free(args);
		err = EINVAL;
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

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "HW port:%d is being quiesced.\n", fc_port->port_hdl);

	/*
	 * If the port is in an OFFLINE state, set the state to QUIESCED
	 * and execute the callback.
	 */
	if (fc_port->hw_port_status == SPDK_FC_PORT_OFFLINE) {
		fc_port->hw_port_status = SPDK_FC_PORT_QUIESCED;
	}

	if (fc_port->hw_port_status == SPDK_FC_PORT_QUIESCED) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "Port %d already in quiesced state.\n",
			      fc_port->port_hdl);
		/*
		 * Execute the callback function directly.
		 */
		cb_func(ctx, err);
		goto fail;
	}

	port_quiesce_ctx = calloc(1, sizeof(struct spdk_nvmf_fc_adm_hw_port_quiesce_ctx));

	if (port_quiesce_ctx == NULL) {
		err = ENOMEM;
		SPDK_ERRLOG("Failed to allocate memory for LS queue quiesce ctx, port:%d\n",
			    fc_port->port_hdl);
		goto fail;
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
		goto fail;
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

fail:
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
	ASSERT_SPDK_FC_MASTER_THREAD();
	struct spdk_nvmf_fc_port *fc_port = NULL;
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;
	struct spdk_nvmf_fc_hw_port_init_args *args = (struct spdk_nvmf_fc_hw_port_init_args *)
			api_data->api_args;
	int err = 0;

	/*
	 * 1. Check for duplicate initialization.
	 */
	fc_port = spdk_nvmf_fc_port_list_get(args->port_handle);
	if (fc_port != NULL) {
		/* Port already exists, check if it has to be re-initialized */
		err = nvmf_fc_adm_hw_port_reinit_validate(fc_port, args);
		if (err) {
			/*
			 * In case of an error we do not want to free the fc_port
			 * so we set that pointer to NULL.
			 */
			fc_port = NULL;
		}
		goto err;
	}

	/*
	 * 2. Get the memory to instantiate a fc port.
	 */
	fc_port = calloc(1, sizeof(struct spdk_nvmf_fc_port) +
			 (args->io_queue_cnt * sizeof(struct spdk_nvmf_fc_hwqp)));
	if (fc_port == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for fc_port %d.\n", args->port_handle);
		err = ENOMEM;
		goto err;
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
		goto err;
	}

	/*
	 * 4. Add this port to the global fc port list in the library.
	 */
	spdk_nvmf_fc_port_list_add(fc_port);

err:
	if (err && fc_port) {
		free(fc_port);
	}
	if (api_data->cb_func != NULL) {
		(void)api_data->cb_func(args->port_handle, SPDK_FC_HW_PORT_INIT, args->cb_ctx, err);
	}

	free(arg);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "HW port %d initialize done, rc = %d.\n",
		      args->port_handle, err);
}

/*
 * Online a HW port.
 */
static void
nvmf_fc_adm_evnt_hw_port_online(void *arg)
{
	ASSERT_SPDK_FC_MASTER_THREAD();
	struct spdk_nvmf_fc_port *fc_port = NULL;
	struct spdk_nvmf_fc_hwqp *hwqp = NULL;
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;
	struct spdk_nvmf_fc_hw_port_online_args *args = (struct spdk_nvmf_fc_hw_port_online_args *)
			api_data->api_args;
	int i = 0;
	int err = 0;

	fc_port = spdk_nvmf_fc_port_list_get(args->port_handle);
	if (fc_port) {
		/* Set the port state to online */
		err = spdk_nvmf_fc_port_set_online(fc_port);
		if (err != 0) {
			SPDK_ERRLOG("Hw port %d online failed. err = %d\n", fc_port->port_hdl, err);
			DEV_VERIFY(!"Hw port online failed");
			goto out;
		}

		hwqp = &fc_port->ls_queue;
		hwqp->context = NULL;
		(void)spdk_nvmf_fc_hwqp_set_online(hwqp);

		/* Cycle through all the io queues and setup a hwqp poller for each. */
		for (i = 0; i < (int)fc_port->num_io_queues; i++) {
			hwqp = &fc_port->io_queues[i];
			hwqp->context = NULL;
			(void)spdk_nvmf_fc_hwqp_set_online(hwqp);
			spdk_nvmf_fc_add_hwqp_to_poller(hwqp);
		}
	} else {
		SPDK_ERRLOG("Unable to find the SPDK FC port %d\n", args->port_handle);
		err = EINVAL;
	}

out:
	if (api_data->cb_func != NULL) {
		(void)api_data->cb_func(args->port_handle, SPDK_FC_HW_PORT_ONLINE, args->cb_ctx, err);
	}

	free(arg);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "HW port %d online done, rc = %d.\n", args->port_handle,
		      err);
}

/*
 * Offline a HW port.
 */
static void
nvmf_fc_adm_evnt_hw_port_offline(void *arg)
{
	ASSERT_SPDK_FC_MASTER_THREAD();
	struct spdk_nvmf_fc_port *fc_port = NULL;
	struct spdk_nvmf_fc_hwqp *hwqp = NULL;
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;
	struct spdk_nvmf_fc_hw_port_offline_args *args = (struct spdk_nvmf_fc_hw_port_offline_args *)
			api_data->api_args;
	int i = 0;
	int err = 0;

	fc_port = spdk_nvmf_fc_port_list_get(args->port_handle);
	if (fc_port) {
		/* Set the port state to offline, if it is not already. */
		err = spdk_nvmf_fc_port_set_offline(fc_port);
		if (err != 0) {
			SPDK_ERRLOG("Hw port %d already offline. err = %d\n", fc_port->port_hdl, err);
			err = 0;
			goto out;
		}

		hwqp = &fc_port->ls_queue;
		(void)spdk_nvmf_fc_hwqp_set_offline(hwqp);

		/* Remove poller for all the io queues. */
		for (i = 0; i < (int)fc_port->num_io_queues; i++) {
			hwqp = &fc_port->io_queues[i];
			(void)spdk_nvmf_fc_hwqp_set_offline(hwqp);
			spdk_nvmf_fc_remove_hwqp_from_poller(hwqp);
		}

		/*
		 * Delete all the nports. Ideally, the nports should have been purged
		 * before the offline event, in which case, only a validation is required.
		 */
		nvmf_fc_adm_hw_port_offline_nport_delete(fc_port);
	} else {
		SPDK_ERRLOG("Unable to find the SPDK FC port %d\n", args->port_handle);
		err = EINVAL;
	}
out:
	if (api_data->cb_func != NULL) {
		(void)api_data->cb_func(args->port_handle, SPDK_FC_HW_PORT_OFFLINE, args->cb_ctx, err);
	}

	free(arg);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "HW port %d offline done, rc = %d.\n", args->port_handle,
		      err);
}

struct nvmf_fc_add_rem_listener_ctx {
	bool add_listener;
	struct spdk_nvme_transport_id trid;
};

static void
nvmf_fc_adm_subsystem_resume_cb(struct spdk_nvmf_subsystem *subsystem, void *cb_arg, int status)
{
	ASSERT_SPDK_FC_MASTER_THREAD();
	struct nvmf_fc_add_rem_listener_ctx *ctx = (struct nvmf_fc_add_rem_listener_ctx *)cb_arg;
	free(ctx);
}

static void
nvmf_fc_adm_subsystem_paused_cb(struct spdk_nvmf_subsystem *subsystem, void *cb_arg, int status)
{
	ASSERT_SPDK_FC_MASTER_THREAD();
	struct nvmf_fc_add_rem_listener_ctx *ctx = (struct nvmf_fc_add_rem_listener_ctx *)cb_arg;

	if (ctx->add_listener) {
		spdk_nvmf_subsystem_add_listener(subsystem, &ctx->trid);
	} else {
		spdk_nvmf_subsystem_remove_listener(subsystem, &ctx->trid);
	}
	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_fc_adm_subsystem_resume_cb, ctx)) {
		SPDK_ERRLOG("Failed to resume subsystem: %s\n", subsystem->subnqn);
		free(ctx);
	}
}

static int
nvmf_fc_adm_add_rem_nport_listener(struct spdk_nvmf_fc_nport *nport, bool add)
{
	struct spdk_nvmf_tgt *tgt = spdk_nvmf_fc_get_tgt();
	struct spdk_nvmf_subsystem *subsystem;

	if (!tgt) {
		SPDK_ERRLOG("No nvmf target defined\n");
		return EINVAL;
	}

	subsystem = spdk_nvmf_subsystem_get_first(tgt);
	while (subsystem) {
		struct nvmf_fc_add_rem_listener_ctx *ctx;

		ctx = calloc(1, sizeof(struct nvmf_fc_add_rem_listener_ctx));
		if (ctx) {
			ctx->add_listener = add;
			spdk_nvmf_fc_create_trid(&ctx->trid, nport->fc_nodename.u.wwn,
						 nport->fc_portname.u.wwn);
			if (spdk_nvmf_subsystem_pause(subsystem, nvmf_fc_adm_subsystem_paused_cb, ctx)) {
				SPDK_ERRLOG("Failed to pause subsystem: %s\n", subsystem->subnqn);
				free(ctx);
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
	ASSERT_SPDK_FC_MASTER_THREAD();
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;
	struct spdk_nvmf_fc_nport_create_args *args = (struct spdk_nvmf_fc_nport_create_args *)
			api_data->api_args;
	struct spdk_nvmf_fc_nport *nport = NULL;
	struct spdk_nvmf_fc_port *fc_port = NULL;
	int err = 0;

	/*
	 * Get the physical port.
	 */
	fc_port = spdk_nvmf_fc_port_list_get(args->port_handle);
	if (fc_port == NULL) {
		err = EINVAL;
		goto out;
	}

	/*
	 * Check for duplicate initialization.
	 */
	nport = spdk_nvmf_fc_nport_get(args->port_handle, args->nport_handle);
	if (nport != NULL) {
		SPDK_ERRLOG("Duplicate SPDK FC nport %d exists for FC port:%d.\n", args->nport_handle,
			    args->port_handle);
		err = EINVAL;
		goto out;
	}

	/*
	 * Get the memory to instantiate a fc nport.
	 */
	nport = calloc(1, sizeof(struct spdk_nvmf_fc_nport));
	if (nport == NULL) {
		SPDK_ERRLOG("Failed to allocate memory for nport %d.\n",
			    args->nport_handle);
		err = ENOMEM;
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
	nport->fc_port      = spdk_nvmf_fc_port_list_get(args->port_handle);

	(void)spdk_nvmf_fc_nport_set_state(nport, SPDK_NVMF_FC_OBJECT_CREATED);
	TAILQ_INIT(&nport->rem_port_list);
	nport->rport_count = 0;
	TAILQ_INIT(&nport->fc_associations);
	nport->assoc_count = 0;

	/*
	 * Populate the nport address (as listening address) to the nvmf subsystems.
	 */
	err = nvmf_fc_adm_add_rem_nport_listener(nport, true);

	(void)spdk_nvmf_fc_port_add_nport(fc_port, nport);
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
	ASSERT_SPDK_FC_MASTER_THREAD();
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
	if (spdk_nvmf_fc_nport_is_rport_empty(nport)) {
		if (0 != nport->assoc_count) {
			SPDK_ERRLOG("association count != 0\n");
			DEV_VERIFY(!"association count != 0");
		}

		err = spdk_nvmf_fc_port_remove_nport(nport->fc_port, nport);
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
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "%s", log_str);
	}
}

/*
 * Delete Nport.
 */
static void
nvmf_fc_adm_evnt_nport_delete(void *arg)
{
	ASSERT_SPDK_FC_MASTER_THREAD();
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
	nport = spdk_nvmf_fc_nport_get(args->port_handle, args->nport_handle);
	if (nport == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC nport %d for FC Port: %d.\n", args->nport_handle,
			    args->port_handle);
		err = EINVAL;
		goto out;
	}

	/*
	 * Allocate memory for callback data.
	 */
	cb_data = calloc(1, sizeof(struct spdk_nvmf_fc_adm_nport_del_cb_data));
	if (NULL == cb_data) {
		SPDK_ERRLOG("Failed to allocate memory for cb_data %d.\n", args->nport_handle);
		err = ENOMEM;
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
		(void)spdk_nvmf_fc_nport_set_state(nport, SPDK_NVMF_FC_OBJECT_TO_BE_DELETED);
	} else if (nport->nport_state == SPDK_NVMF_FC_OBJECT_TO_BE_DELETED) {
		/*
		 * Deletion of this nport already in progress. Register callback
		 * and return.
		 */
		/* TODO: Register callback in callback vector. For now, set the error and return. */
		err = ENODEV;
		goto out;
	} else {
		/* nport partially created/deleted */
		DEV_VERIFY(nport->nport_state == SPDK_NVMF_FC_OBJECT_ZOMBIE);
		DEV_VERIFY(0 != "Nport in zombie state");
		err = ENODEV;
		goto out;
	}

	/*
	 * Remove this nport from listening addresses across subsystems
	 */
	rc = nvmf_fc_adm_add_rem_nport_listener(nport, false);

	if (0 != rc) {
		err = spdk_nvmf_fc_nport_set_state(nport, SPDK_NVMF_FC_OBJECT_ZOMBIE);
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
	if (spdk_nvmf_fc_nport_is_rport_empty(nport)) {
		/* No rports to delete. Complete the nport deletion. */
		nvmf_fc_adm_delete_nport_cb(nport->port_hdl, SPDK_FC_NPORT_DELETE, cb_data, 0);
		goto out;
	}

	TAILQ_FOREACH(rport_iter, &nport->rem_port_list, link) {
		struct spdk_nvmf_fc_hw_i_t_delete_args *it_del_args = calloc(
					1, sizeof(struct spdk_nvmf_fc_hw_i_t_delete_args));

		if (it_del_args == NULL) {
			err = ENOMEM;
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

		spdk_nvmf_fc_master_enqueue_event(SPDK_FC_IT_DELETE, (void *)it_del_args,
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
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API,
			      "NPort %d delete done succesfully, fc port:%d. "
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
	ASSERT_SPDK_FC_MASTER_THREAD();
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
	nport = spdk_nvmf_fc_nport_get(args->port_handle, args->nport_handle);
	if (nport == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC nport %d\n", args->nport_handle);
		err = EINVAL;
		goto out;
	}

	/*
	 * Check for duplicate i_t_add.
	 */
	TAILQ_FOREACH(rport_iter, &nport->rem_port_list, link) {
		if ((rport_iter->s_id == args->s_id) && (rport_iter->rpi == args->rpi)) {
			SPDK_ERRLOG("Duplicate rport found for FC nport %d: sid:%d rpi:%d\n",
				    args->nport_handle, rport_iter->s_id, rport_iter->rpi);
			err = EEXIST;
			goto out;
		}
	}

	/*
	 * Get the memory to instantiate the remote port
	 */
	rport = calloc(1, sizeof(struct spdk_nvmf_fc_remote_port_info));
	if (rport == NULL) {
		SPDK_ERRLOG("Memory allocation for rem port failed.\n");
		err = ENOMEM;
		goto out;
	}

	/*
	 * Initialize the contents for the rport
	 */
	(void)spdk_nvmf_fc_rport_set_state(rport, SPDK_NVMF_FC_OBJECT_CREATED);
	rport->s_id = args->s_id;
	rport->rpi = args->rpi;
	rport->fc_nodename = args->fc_nodename;
	rport->fc_portname = args->fc_portname;

	/*
	 * Add remote port to nport
	 */
	if (spdk_nvmf_fc_nport_add_rem_port(nport, rport) != 0) {
		DEV_VERIFY(!"Error while adding rport to list");
	};

	/*
	 * TODO: Do we validate the initiators service parameters?
	 */

	/*
	 * Get the targets service parameters from the library
	 * to return back to the driver.
	 */
	args->target_prli_info = spdk_nvmf_fc_get_prli_service_params();

out:
	if (api_data->cb_func != NULL) {
		/*
		 * Passing pointer to the args struct as the first argument.
		 * The cb_func should handle this appropriately.
		 */
		(void)api_data->cb_func(args->port_handle, SPDK_FC_IT_ADD, args->cb_ctx, err);
	}

	free(arg);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API,
		      "IT add on nport %d done, rc = %d.\n",
		      args->nport_handle, err);
}

/**
 * Process a IT delete.
 */
static void
nvmf_fc_adm_evnt_i_t_delete(void *arg)
{
	ASSERT_SPDK_FC_MASTER_THREAD();
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

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "IT delete on nport:%d begin.\n", args->nport_handle);

	/*
	 * Make sure the nport port exists. If it does not, error out.
	 */
	nport = spdk_nvmf_fc_nport_get(args->port_handle, args->nport_handle);
	if (nport == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC nport:%d\n", args->nport_handle);
		rc = EINVAL;
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
		rc = ENODEV;
		goto out;
	}

	/*
	 * We have found exactly one rport. Allocate memory for callback data.
	 */
	cb_data = calloc(1, sizeof(struct spdk_nvmf_fc_adm_i_t_del_cb_data));
	if (NULL == cb_data) {
		SPDK_ERRLOG("Failed to allocate memory for cb_data for nport:%d.\n", args->nport_handle);
		rc = ENOMEM;
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
		(void)spdk_nvmf_fc_rport_set_state(rport, SPDK_NVMF_FC_OBJECT_TO_BE_DELETED);
	} else if (rport->rport_state == SPDK_NVMF_FC_OBJECT_TO_BE_DELETED) {
		/*
		 * Deletion of this rport already in progress. Register callback
		 * and return.
		 */
		/* TODO: Register callback in callback vector. For now, set the error and return. */
		rc = ENODEV;
		goto out;
	} else {
		/* rport partially created/deleted */
		DEV_VERIFY(rport->rport_state == SPDK_NVMF_FC_OBJECT_ZOMBIE);
		DEV_VERIFY(!"Invalid rport_state");
		rc = ENODEV;
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
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "%s", log_str);
	}

	free(arg);
}

/*
 * Process ABTS received
 */
static void
nvmf_fc_adm_evnt_abts_recv(void *arg)
{
	ASSERT_SPDK_FC_MASTER_THREAD();
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;
	struct spdk_nvmf_fc_abts_args *args = (struct spdk_nvmf_fc_abts_args *)api_data->api_args;
	struct spdk_nvmf_fc_nport *nport = NULL;
	int err = 0;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "FC ABTS received. RPI:%d, oxid:%d, rxid:%d\n", args->rpi,
		      args->oxid, args->rxid);

	/*
	 * 1. Make sure the nport port exists.
	 */
	nport = spdk_nvmf_fc_nport_get(args->port_handle, args->nport_handle);
	if (nport == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC nport %d\n", args->nport_handle);
		err = EINVAL;
		goto out;
	}

	/*
	 * 2. If the nport is in the process of being deleted, drop the ABTS.
	 */
	if (nport->nport_state == SPDK_NVMF_FC_OBJECT_TO_BE_DELETED) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API,
			      "FC ABTS dropped because the nport is being deleted; RPI:%d, oxid:%d, rxid:%d\n",
			      args->rpi, args->oxid, args->rxid);
		err = 0;
		goto out;

	}

	/*
	 * 3. Pass the received ABTS-LS to the library for handling.
	 */
	spdk_nvmf_fc_handle_abts_frame(nport, args->rpi, args->oxid, args->rxid);

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
	ASSERT_SPDK_FC_MASTER_THREAD();
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
	fc_port = spdk_nvmf_fc_port_list_get(args->port_handle);
	if (fc_port == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC port %d\n", args->port_handle);
		err = EINVAL;
		goto out;
	}

	/*
	 * Allocate memory for the dump buffer.
	 * This memory will be freed by FCT.
	 */
	dump_buf = (char *)calloc(1, dump_buf_size);
	if (dump_buf == NULL) {
		err = ENOMEM;
		SPDK_ERRLOG("Memory allocation for dump buffer failed, SPDK FC port %d\n", args->port_handle);
		goto out;
	}
	*args->dump_buf  = (uint32_t *)dump_buf;
	dump_info.buffer = dump_buf;
	dump_info.offset = 0;

	/*
	 * Add the dump reason to the top of the buffer.
	 */
	spdk_nvmf_fc_dump_buf_print(&dump_info, "%s\n", args->reason);

	/*
	 * Dump the hwqp.
	 */
	spdk_nvmf_fc_dump_all_queues(fc_port, &dump_info);

out:
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "HW port %d reset done, queues_dumped = %d, rc = %d.\n",
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
	ASSERT_SPDK_FC_MASTER_THREAD();
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;
	struct spdk_nvmf_fc_hw_port_reset_args *args = (struct spdk_nvmf_fc_hw_port_reset_args *)
			api_data->api_args;
	struct spdk_nvmf_fc_port *fc_port = NULL;
	struct spdk_nvmf_fc_adm_hw_port_reset_ctx *ctx = NULL;
	int err = 0;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "HW port %d dump\n", args->port_handle);

	/*
	 * Make sure the physical port exists.
	 */
	fc_port = spdk_nvmf_fc_port_list_get(args->port_handle);
	if (fc_port == NULL) {
		SPDK_ERRLOG("Unable to find the SPDK FC port %d\n", args->port_handle);
		err = EINVAL;
		goto out;
	}

	/*
	 * Save the reset event args and the callback in a context struct.
	 */
	ctx = calloc(1, sizeof(struct spdk_nvmf_fc_adm_hw_port_reset_ctx));

	if (ctx == NULL) {
		err = ENOMEM;
		SPDK_ERRLOG("Memory allocation for reset ctx failed, SPDK FC port %d\n", args->port_handle);
		goto fail;
	}

	ctx->reset_args = arg;
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
	if (ctx) {
		free(ctx);
	}

out:
	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "HW port %d dump done, rc = %d.\n", args->port_handle,
		      err);

	if (api_data->cb_func != NULL) {
		(void)api_data->cb_func(args->port_handle, SPDK_FC_HW_PORT_RESET, args->cb_ctx, err);
	}

	free(arg);
}

/*
 * Process a link break event on a HW port.
 */
static void
nvmf_fc_adm_evnt_hw_port_link_break(void *arg)
{
	ASSERT_SPDK_FC_MASTER_THREAD();
	struct spdk_nvmf_fc_adm_api_data *api_data = (struct spdk_nvmf_fc_adm_api_data *)arg;
	struct spdk_nvmf_hw_port_link_break_args *args = (struct spdk_nvmf_hw_port_link_break_args *)
			api_data->api_args;
	struct spdk_nvmf_fc_port *fc_port = NULL;
	int err = 0;
	struct spdk_nvmf_fc_adm_port_link_break_cb_data *cb_data = NULL;
	struct spdk_nvmf_fc_nport *nport = NULL;
	uint32_t nport_deletes_sent = 0;
	uint32_t nport_deletes_skipped = 0;
	struct spdk_nvmf_fc_nport_delete_args *nport_del_args = NULL;
	char log_str[256];

	/*
	 * Get the fc port using the port handle.
	 */
	fc_port = spdk_nvmf_fc_port_list_get(args->port_handle);
	if (!fc_port) {
		SPDK_ERRLOG("port link break: Unable to find the SPDK FC port %d\n",
			    args->port_handle);
		err = EINVAL;
		goto out;
	}

	/*
	 * Set the port state to offline, if it is not already.
	 */
	err = spdk_nvmf_fc_port_set_offline(fc_port);
	if (err != 0) {
		SPDK_ERRLOG("port link break: HW port %d already offline. rc = %d\n",
			    fc_port->port_hdl, err);
		err = 0;
		goto out;
	}

	/*
	 * Delete all the nports, if any.
	 */
	if (!TAILQ_EMPTY(&fc_port->nport_list)) {
		TAILQ_FOREACH(nport, &fc_port->nport_list, link) {
			/* Skipped the nports that are not in CREATED state */
			if (nport->nport_state != SPDK_NVMF_FC_OBJECT_CREATED) {
				nport_deletes_skipped++;
				continue;
			}

			/* Allocate memory for callback data. */
			cb_data = calloc(1, sizeof(struct spdk_nvmf_fc_adm_port_link_break_cb_data));
			if (NULL == cb_data) {
				SPDK_ERRLOG("port link break: Failed to allocate memory for cb_data %d.\n",
					    args->port_handle);
				err = ENOMEM;
				goto out;
			}
			cb_data->args = args;
			cb_data->cb_func = api_data->cb_func;
			nport_del_args = &cb_data->nport_del_args;
			nport_del_args->port_handle = args->port_handle;
			nport_del_args->nport_handle = nport->nport_hdl;
			nport_del_args->cb_ctx = cb_data;

			spdk_nvmf_fc_master_enqueue_event(SPDK_FC_NPORT_DELETE,
							  (void *)nport_del_args,
							  nvmf_fc_adm_hw_port_link_break_cb);

			nport_deletes_sent++;
		}
	}

	if (nport_deletes_sent == 0 && err == 0) {
		/*
		 * Mark the hwqps as offline and unregister the pollers.
		 */
		(void)nvmf_fc_adm_port_hwqp_offline_del_poller(fc_port);
	}

out:
	snprintf(log_str, sizeof(log_str),
		 "port link break done: port:%d nport_deletes_sent:%d nport_deletes_skipped:%d rc:%d.\n",
		 args->port_handle, nport_deletes_sent, nport_deletes_skipped, err);

	if (err != 0) {
		SPDK_ERRLOG("%s", log_str);
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "%s", log_str);
	}

	if ((api_data->cb_func != NULL) && (nport_deletes_sent == 0)) {
		/*
		 * No nport_deletes are sent, which would have eventually
		 * called the port_link_break callback. Therefore, call the
		 * port_link_break callback here.
		 */
		(void)api_data->cb_func(args->port_handle, SPDK_FC_LINK_BREAK, args->cb_ctx, err);
	}

	free(arg);
}

static inline void
nvmf_fc_adm_run_on_master_thread(spdk_msg_fn fn, void *args)
{
	if (spdk_nvmf_fc_get_master_thread()) {
		spdk_thread_send_msg(spdk_nvmf_fc_get_master_thread(), fn, args);
	}
}

/*
 * Queue up an event in the SPDK masters event queue.
 * Used by the FC driver to notify the SPDK master of FC related events.
 */
int
spdk_nvmf_fc_master_enqueue_event(enum spdk_fc_event event_type, void *args,
				  spdk_nvmf_fc_callback cb_func)
{
	int err = 0;
	struct spdk_nvmf_fc_adm_api_data *api_data = NULL;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "Enqueue event %d.\n", event_type);

	if (event_type >= SPDK_FC_EVENT_MAX) {
		SPDK_ERRLOG("Invalid spdk_fc_event_t %d.\n", event_type);
		err = EINVAL;
		goto done;
	}

	if (args == NULL) {
		SPDK_ERRLOG("Null args for event %d.\n", event_type);
		err = EINVAL;
		goto done;
	}

	api_data = calloc(1, sizeof(*api_data));

	if (api_data == NULL) {
		SPDK_ERRLOG("Failed to alloc api data for event %d.\n", event_type);
		err = ENOMEM;
		goto done;
	}

	api_data->api_args = args;
	api_data->cb_func = cb_func;

	switch (event_type) {
	case SPDK_FC_HW_PORT_INIT:
		nvmf_fc_adm_run_on_master_thread(nvmf_fc_adm_evnt_hw_port_init,
						 (void *)api_data);
		break;

	case SPDK_FC_HW_PORT_ONLINE:
		nvmf_fc_adm_run_on_master_thread(nvmf_fc_adm_evnt_hw_port_online,
						 (void *)api_data);
		break;

	case SPDK_FC_HW_PORT_OFFLINE:
		nvmf_fc_adm_run_on_master_thread(nvmf_fc_adm_evnt_hw_port_offline,
						 (void *)api_data);
		break;

	case SPDK_FC_NPORT_CREATE:
		nvmf_fc_adm_run_on_master_thread(nvmf_fc_adm_evnt_nport_create,
						 (void *)api_data);
		break;

	case SPDK_FC_NPORT_DELETE:
		nvmf_fc_adm_run_on_master_thread(nvmf_fc_adm_evnt_nport_delete,
						 (void *)api_data);
		break;

	case SPDK_FC_IT_ADD:
		nvmf_fc_adm_run_on_master_thread(nvmf_fc_adm_evnt_i_t_add,
						 (void *)api_data);
		break;

	case SPDK_FC_IT_DELETE:
		nvmf_fc_adm_run_on_master_thread(nvmf_fc_adm_evnt_i_t_delete,
						 (void *)api_data);
		break;

	case SPDK_FC_ABTS_RECV:
		nvmf_fc_adm_run_on_master_thread(nvmf_fc_adm_evnt_abts_recv,
						 (void *)api_data);
		break;

	case SPDK_FC_LINK_BREAK:
		nvmf_fc_adm_run_on_master_thread(nvmf_fc_adm_evnt_hw_port_link_break,
						 (void *)api_data);
		break;

	case SPDK_FC_HW_PORT_RESET:
		nvmf_fc_adm_run_on_master_thread(nvmf_fc_adm_evnt_hw_port_reset,
						 (void *)api_data);
		break;

	case SPDK_FC_UNRECOVERABLE_ERR:
	default:
		SPDK_ERRLOG("Invalid spdk_fc_event_t: %d\n", event_type);
		err = EINVAL;
		break;
	}

done:

	if (err == 0) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF_FC_ADM_API, "Enqueue event %d done successfully\n", event_type);
	} else {
		SPDK_ERRLOG("Enqueue event %d failed, err = %d\n", event_type, err);
		if (api_data) {
			free(api_data);
		}
	}

	return err;
}

SPDK_LOG_REGISTER_COMPONENT("nvmf_fc_adm_api", SPDK_LOG_NVMF_FC_ADM_API);
