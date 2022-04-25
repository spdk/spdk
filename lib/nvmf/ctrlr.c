/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019, 2020 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#include "spdk/stdinc.h"

#include "nvmf_internal.h"
#include "transport.h"

#include "spdk/bit_array.h"
#include "spdk/endian.h"
#include "spdk/thread.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvmf_cmd.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/version.h"
#include "spdk/log.h"
#include "spdk_internal/usdt.h"

#define MIN_KEEP_ALIVE_TIMEOUT_IN_MS 10000
#define NVMF_DISC_KATO_IN_MS 120000
#define KAS_TIME_UNIT_IN_MS 100
#define KAS_DEFAULT_VALUE (MIN_KEEP_ALIVE_TIMEOUT_IN_MS / KAS_TIME_UNIT_IN_MS)

#define NVMF_CC_RESET_SHN_TIMEOUT_IN_MS	10000

#define NVMF_CTRLR_RESET_SHN_TIMEOUT_IN_MS	(NVMF_CC_RESET_SHN_TIMEOUT_IN_MS + 5000)

/*
 * Report the SPDK version as the firmware revision.
 * SPDK_VERSION_STRING won't fit into FR (only 8 bytes), so try to fit the most important parts.
 */
#define FW_VERSION SPDK_VERSION_MAJOR_STRING SPDK_VERSION_MINOR_STRING SPDK_VERSION_PATCH_STRING

#define ANA_TRANSITION_TIME_IN_SEC 10

/*
 * Support for custom admin command handlers
 */
struct spdk_nvmf_custom_admin_cmd {
	spdk_nvmf_custom_cmd_hdlr hdlr;
	uint32_t nsid; /* nsid to forward */
};

static struct spdk_nvmf_custom_admin_cmd g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_MAX_OPC + 1];

static void _nvmf_request_complete(void *ctx);

static inline void
nvmf_invalid_connect_response(struct spdk_nvmf_fabric_connect_rsp *rsp,
			      uint8_t iattr, uint16_t ipo)
{
	rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
	rsp->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
	rsp->status_code_specific.invalid.iattr = iattr;
	rsp->status_code_specific.invalid.ipo = ipo;
}

#define SPDK_NVMF_INVALID_CONNECT_CMD(rsp, field)	\
	nvmf_invalid_connect_response(rsp, 0, offsetof(struct spdk_nvmf_fabric_connect_cmd, field))
#define SPDK_NVMF_INVALID_CONNECT_DATA(rsp, field)	\
	nvmf_invalid_connect_response(rsp, 1, offsetof(struct spdk_nvmf_fabric_connect_data, field))


static void
nvmf_ctrlr_stop_keep_alive_timer(struct spdk_nvmf_ctrlr *ctrlr)
{
	if (!ctrlr) {
		SPDK_ERRLOG("Controller is NULL\n");
		return;
	}

	if (ctrlr->keep_alive_poller == NULL) {
		return;
	}

	SPDK_DEBUGLOG(nvmf, "Stop keep alive poller\n");
	spdk_poller_unregister(&ctrlr->keep_alive_poller);
}

static void
nvmf_ctrlr_stop_association_timer(struct spdk_nvmf_ctrlr *ctrlr)
{
	if (!ctrlr) {
		SPDK_ERRLOG("Controller is NULL\n");
		assert(false);
		return;
	}

	if (ctrlr->association_timer == NULL) {
		return;
	}

	SPDK_DEBUGLOG(nvmf, "Stop association timer\n");
	spdk_poller_unregister(&ctrlr->association_timer);
}

static void
nvmf_ctrlr_disconnect_qpairs_done(struct spdk_io_channel_iter *i, int status)
{
	if (status == 0) {
		SPDK_DEBUGLOG(nvmf, "ctrlr disconnect qpairs complete successfully\n");
	} else {
		SPDK_ERRLOG("Fail to disconnect ctrlr qpairs\n");
	}
}

static int
_nvmf_ctrlr_disconnect_qpairs_on_pg(struct spdk_io_channel_iter *i, bool include_admin)
{
	int rc = 0;
	struct spdk_nvmf_ctrlr *ctrlr;
	struct spdk_nvmf_qpair *qpair, *temp_qpair;
	struct spdk_io_channel *ch;
	struct spdk_nvmf_poll_group *group;

	ctrlr = spdk_io_channel_iter_get_ctx(i);
	ch = spdk_io_channel_iter_get_channel(i);
	group = spdk_io_channel_get_ctx(ch);

	TAILQ_FOREACH_SAFE(qpair, &group->qpairs, link, temp_qpair) {
		if (qpair->ctrlr == ctrlr && (include_admin || !nvmf_qpair_is_admin_queue(qpair))) {
			rc = spdk_nvmf_qpair_disconnect(qpair, NULL, NULL);
			if (rc) {
				SPDK_ERRLOG("Qpair disconnect failed\n");
				return rc;
			}
		}
	}

	return rc;
}

static void
nvmf_ctrlr_disconnect_qpairs_on_pg(struct spdk_io_channel_iter *i)
{
	spdk_for_each_channel_continue(i, _nvmf_ctrlr_disconnect_qpairs_on_pg(i, true));
}

static void
nvmf_ctrlr_disconnect_io_qpairs_on_pg(struct spdk_io_channel_iter *i)
{
	spdk_for_each_channel_continue(i, _nvmf_ctrlr_disconnect_qpairs_on_pg(i, false));
}

static int
nvmf_ctrlr_keep_alive_poll(void *ctx)
{
	uint64_t keep_alive_timeout_tick;
	uint64_t now = spdk_get_ticks();
	struct spdk_nvmf_ctrlr *ctrlr = ctx;

	if (ctrlr->in_destruct) {
		nvmf_ctrlr_stop_keep_alive_timer(ctrlr);
		return SPDK_POLLER_IDLE;
	}

	SPDK_DEBUGLOG(nvmf, "Polling ctrlr keep alive timeout\n");

	/* If the Keep alive feature is in use and the timer expires */
	keep_alive_timeout_tick = ctrlr->last_keep_alive_tick +
				  ctrlr->feat.keep_alive_timer.bits.kato * spdk_get_ticks_hz() / UINT64_C(1000);
	if (now > keep_alive_timeout_tick) {
		SPDK_NOTICELOG("Disconnecting host %s from subsystem %s due to keep alive timeout.\n",
			       ctrlr->hostnqn, ctrlr->subsys->subnqn);
		/* set the Controller Fatal Status bit to '1' */
		if (ctrlr->vcprop.csts.bits.cfs == 0) {
			ctrlr->vcprop.csts.bits.cfs = 1;

			/*
			 * disconnect qpairs, terminate Transport connection
			 * destroy ctrlr, break the host to controller association
			 * disconnect qpairs with qpair->ctrlr == ctrlr
			 */
			spdk_for_each_channel(ctrlr->subsys->tgt,
					      nvmf_ctrlr_disconnect_qpairs_on_pg,
					      ctrlr,
					      nvmf_ctrlr_disconnect_qpairs_done);
			return SPDK_POLLER_BUSY;
		}
	}

	return SPDK_POLLER_IDLE;
}

static void
nvmf_ctrlr_start_keep_alive_timer(struct spdk_nvmf_ctrlr *ctrlr)
{
	if (!ctrlr) {
		SPDK_ERRLOG("Controller is NULL\n");
		return;
	}

	/* if cleared to 0 then the Keep Alive Timer is disabled */
	if (ctrlr->feat.keep_alive_timer.bits.kato != 0) {

		ctrlr->last_keep_alive_tick = spdk_get_ticks();

		SPDK_DEBUGLOG(nvmf, "Ctrlr add keep alive poller\n");
		ctrlr->keep_alive_poller = SPDK_POLLER_REGISTER(nvmf_ctrlr_keep_alive_poll, ctrlr,
					   ctrlr->feat.keep_alive_timer.bits.kato * 1000);
	}
}

static void
ctrlr_add_qpair_and_update_rsp(struct spdk_nvmf_qpair *qpair,
			       struct spdk_nvmf_ctrlr *ctrlr,
			       struct spdk_nvmf_fabric_connect_rsp *rsp)
{
	assert(ctrlr->admin_qpair->group->thread == spdk_get_thread());

	/* check if we would exceed ctrlr connection limit */
	if (qpair->qid >= spdk_bit_array_capacity(ctrlr->qpair_mask)) {
		SPDK_ERRLOG("Requested QID %u but Max QID is %u\n",
			    qpair->qid, spdk_bit_array_capacity(ctrlr->qpair_mask) - 1);
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
		return;
	}

	if (spdk_bit_array_get(ctrlr->qpair_mask, qpair->qid)) {
		SPDK_ERRLOG("Got I/O connect with duplicate QID %u\n", qpair->qid);
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;
		return;
	}

	qpair->ctrlr = ctrlr;
	spdk_bit_array_set(ctrlr->qpair_mask, qpair->qid);

	rsp->status.sc = SPDK_NVME_SC_SUCCESS;
	rsp->status_code_specific.success.cntlid = ctrlr->cntlid;
	SPDK_DEBUGLOG(nvmf, "connect capsule response: cntlid = 0x%04x\n",
		      rsp->status_code_specific.success.cntlid);

	SPDK_DTRACE_PROBE4(nvmf_ctrlr_add_qpair, qpair, qpair->qid, ctrlr->subsys->subnqn,
			   ctrlr->hostnqn);
}

static void
_nvmf_ctrlr_add_admin_qpair(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;

	ctrlr->admin_qpair = qpair;
	ctrlr->association_timeout = qpair->transport->opts.association_timeout;
	nvmf_ctrlr_start_keep_alive_timer(ctrlr);
	ctrlr_add_qpair_and_update_rsp(qpair, ctrlr, rsp);
	_nvmf_request_complete(req);
}

static void
_nvmf_subsystem_add_ctrlr(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;

	if (nvmf_subsystem_add_ctrlr(ctrlr->subsys, ctrlr)) {
		SPDK_ERRLOG("Unable to add controller to subsystem\n");
		spdk_bit_array_free(&ctrlr->qpair_mask);
		free(ctrlr);
		qpair->ctrlr = NULL;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		spdk_nvmf_request_complete(req);
		return;
	}

	spdk_thread_send_msg(ctrlr->thread, _nvmf_ctrlr_add_admin_qpair, req);
}

static void
nvmf_ctrlr_cdata_init(struct spdk_nvmf_transport *transport, struct spdk_nvmf_subsystem *subsystem,
		      struct spdk_nvmf_ctrlr_data *cdata)
{
	cdata->aerl = NVMF_MAX_ASYNC_EVENTS - 1;
	cdata->kas = KAS_DEFAULT_VALUE;
	cdata->vid = SPDK_PCI_VID_INTEL;
	cdata->ssvid = SPDK_PCI_VID_INTEL;
	/* INTEL OUI */
	cdata->ieee[0] = 0xe4;
	cdata->ieee[1] = 0xd2;
	cdata->ieee[2] = 0x5c;
	cdata->oncs.reservations = 1;
	cdata->sgls.supported = 1;
	cdata->sgls.keyed_sgl = 1;
	cdata->sgls.sgl_offset = 1;
	cdata->nvmf_specific.ioccsz = sizeof(struct spdk_nvme_cmd) / 16;
	cdata->nvmf_specific.ioccsz += transport->opts.in_capsule_data_size / 16;
	cdata->nvmf_specific.iorcsz = sizeof(struct spdk_nvme_cpl) / 16;
	cdata->nvmf_specific.icdoff = 0; /* offset starts directly after SQE */
	cdata->nvmf_specific.ctrattr.ctrlr_model = SPDK_NVMF_CTRLR_MODEL_DYNAMIC;
	cdata->nvmf_specific.msdbd = 1;

	if (transport->ops->cdata_init) {
		transport->ops->cdata_init(transport, subsystem, cdata);
	}
}

static struct spdk_nvmf_ctrlr *
nvmf_ctrlr_create(struct spdk_nvmf_subsystem *subsystem,
		  struct spdk_nvmf_request *req,
		  struct spdk_nvmf_fabric_connect_cmd *connect_cmd,
		  struct spdk_nvmf_fabric_connect_data *connect_data)
{
	struct spdk_nvmf_ctrlr *ctrlr;
	struct spdk_nvmf_transport *transport = req->qpair->transport;
	struct spdk_nvme_transport_id listen_trid = {};

	ctrlr = calloc(1, sizeof(*ctrlr));
	if (ctrlr == NULL) {
		SPDK_ERRLOG("Memory allocation failed\n");
		return NULL;
	}

	if (spdk_nvme_trtype_is_fabrics(transport->ops->type)) {
		ctrlr->dynamic_ctrlr = true;
	} else {
		ctrlr->cntlid = connect_data->cntlid;
	}

	SPDK_DTRACE_PROBE3(nvmf_ctrlr_create, ctrlr, subsystem->subnqn,
			   spdk_thread_get_id(req->qpair->group->thread));

	STAILQ_INIT(&ctrlr->async_events);
	TAILQ_INIT(&ctrlr->log_head);
	ctrlr->subsys = subsystem;
	ctrlr->thread = req->qpair->group->thread;
	ctrlr->disconnect_in_progress = false;

	ctrlr->qpair_mask = spdk_bit_array_create(transport->opts.max_qpairs_per_ctrlr);
	if (!ctrlr->qpair_mask) {
		SPDK_ERRLOG("Failed to allocate controller qpair mask\n");
		goto err_qpair_mask;
	}

	nvmf_ctrlr_cdata_init(transport, subsystem, &ctrlr->cdata);

	/*
	 * KAS: This field indicates the granularity of the Keep Alive Timer in 100ms units.
	 * If this field is cleared to 0h, then Keep Alive is not supported.
	 */
	if (ctrlr->cdata.kas) {
		ctrlr->feat.keep_alive_timer.bits.kato = spdk_divide_round_up(connect_cmd->kato,
				KAS_DEFAULT_VALUE * KAS_TIME_UNIT_IN_MS) *
				KAS_DEFAULT_VALUE * KAS_TIME_UNIT_IN_MS;
	}

	ctrlr->feat.async_event_configuration.bits.ns_attr_notice = 1;
	if (ctrlr->subsys->flags.ana_reporting) {
		ctrlr->feat.async_event_configuration.bits.ana_change_notice = 1;
	}
	ctrlr->feat.volatile_write_cache.bits.wce = 1;
	/* Coalescing Disable */
	ctrlr->feat.interrupt_vector_configuration.bits.cd = 1;

	if (ctrlr->subsys->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY) {
		/*
		 * If keep-alive timeout is not set, discovery controllers use some
		 * arbitrary high value in order to cleanup stale discovery sessions
		 *
		 * From the 1.0a nvme-of spec:
		 * "The Keep Alive command is reserved for
		 * Discovery controllers. A transport may specify a
		 * fixed Discovery controller activity timeout value
		 * (e.g., 2 minutes). If no commands are received
		 * by a Discovery controller within that time
		 * period, the controller may perform the
		 * actions for Keep Alive Timer expiration".
		 *
		 * From the 1.1 nvme-of spec:
		 * "A host requests an explicit persistent connection
		 * to a Discovery controller and Asynchronous Event Notifications from
		 * the Discovery controller on that persistent connection by specifying
		 * a non-zero Keep Alive Timer value in the Connect command."
		 *
		 * In case non-zero KATO is used, we enable discovery_log_change_notice
		 * otherwise we disable it and use default discovery controller KATO.
		 * KATO is in millisecond.
		 */
		if (ctrlr->feat.keep_alive_timer.bits.kato == 0) {
			ctrlr->feat.keep_alive_timer.bits.kato = NVMF_DISC_KATO_IN_MS;
			ctrlr->feat.async_event_configuration.bits.discovery_log_change_notice = 0;
		} else {
			ctrlr->feat.async_event_configuration.bits.discovery_log_change_notice = 1;
		}
	}

	/* Subtract 1 for admin queue, 1 for 0's based */
	ctrlr->feat.number_of_queues.bits.ncqr = transport->opts.max_qpairs_per_ctrlr - 1 -
			1;
	ctrlr->feat.number_of_queues.bits.nsqr = transport->opts.max_qpairs_per_ctrlr - 1 -
			1;

	spdk_uuid_copy(&ctrlr->hostid, (struct spdk_uuid *)connect_data->hostid);
	memcpy(ctrlr->hostnqn, connect_data->hostnqn, sizeof(ctrlr->hostnqn));

	ctrlr->vcprop.cap.raw = 0;
	ctrlr->vcprop.cap.bits.cqr = 1; /* NVMe-oF specification required */
	ctrlr->vcprop.cap.bits.mqes = transport->opts.max_queue_depth -
				      1; /* max queue depth */
	ctrlr->vcprop.cap.bits.ams = 0; /* optional arb mechanisms */
	/* ready timeout - 500 msec units */
	ctrlr->vcprop.cap.bits.to = NVMF_CTRLR_RESET_SHN_TIMEOUT_IN_MS / 500;
	ctrlr->vcprop.cap.bits.dstrd = 0; /* fixed to 0 for NVMe-oF */
	ctrlr->vcprop.cap.bits.css = SPDK_NVME_CAP_CSS_NVM; /* NVM command set */
	ctrlr->vcprop.cap.bits.mpsmin = 0; /* 2 ^ (12 + mpsmin) == 4k */
	ctrlr->vcprop.cap.bits.mpsmax = 0; /* 2 ^ (12 + mpsmax) == 4k */

	/* Version Supported: 1.3 */
	ctrlr->vcprop.vs.bits.mjr = 1;
	ctrlr->vcprop.vs.bits.mnr = 3;
	ctrlr->vcprop.vs.bits.ter = 0;

	ctrlr->vcprop.cc.raw = 0;
	ctrlr->vcprop.cc.bits.en = 0; /* Init controller disabled */

	ctrlr->vcprop.csts.raw = 0;
	ctrlr->vcprop.csts.bits.rdy = 0; /* Init controller as not ready */

	SPDK_DEBUGLOG(nvmf, "cap 0x%" PRIx64 "\n", ctrlr->vcprop.cap.raw);
	SPDK_DEBUGLOG(nvmf, "vs 0x%x\n", ctrlr->vcprop.vs.raw);
	SPDK_DEBUGLOG(nvmf, "cc 0x%x\n", ctrlr->vcprop.cc.raw);
	SPDK_DEBUGLOG(nvmf, "csts 0x%x\n", ctrlr->vcprop.csts.raw);

	ctrlr->dif_insert_or_strip = transport->opts.dif_insert_or_strip;

	if (ctrlr->subsys->subtype == SPDK_NVMF_SUBTYPE_NVME) {
		if (spdk_nvmf_qpair_get_listen_trid(req->qpair, &listen_trid) != 0) {
			SPDK_ERRLOG("Could not get listener transport ID\n");
			goto err_listener;
		}

		ctrlr->listener = nvmf_subsystem_find_listener(ctrlr->subsys, &listen_trid);
		if (!ctrlr->listener) {
			SPDK_ERRLOG("Listener was not found\n");
			goto err_listener;
		}
	}

	req->qpair->ctrlr = ctrlr;
	spdk_thread_send_msg(subsystem->thread, _nvmf_subsystem_add_ctrlr, req);

	return ctrlr;
err_listener:
	spdk_bit_array_free(&ctrlr->qpair_mask);
err_qpair_mask:
	free(ctrlr);
	return NULL;
}

static void
_nvmf_ctrlr_destruct(void *ctx)
{
	struct spdk_nvmf_ctrlr *ctrlr = ctx;
	struct spdk_nvmf_reservation_log *log, *log_tmp;
	struct spdk_nvmf_async_event_completion *event, *event_tmp;

	SPDK_DTRACE_PROBE3(nvmf_ctrlr_destruct, ctrlr, ctrlr->subsys->subnqn,
			   spdk_thread_get_id(ctrlr->thread));

	assert(spdk_get_thread() == ctrlr->thread);
	assert(ctrlr->in_destruct);

	SPDK_DEBUGLOG(nvmf, "Destroy ctrlr 0x%hx\n", ctrlr->cntlid);
	if (ctrlr->disconnect_in_progress) {
		SPDK_ERRLOG("freeing ctrlr with disconnect in progress\n");
		spdk_thread_send_msg(ctrlr->thread, _nvmf_ctrlr_destruct, ctrlr);
		return;
	}

	nvmf_ctrlr_stop_keep_alive_timer(ctrlr);
	nvmf_ctrlr_stop_association_timer(ctrlr);
	spdk_bit_array_free(&ctrlr->qpair_mask);

	TAILQ_FOREACH_SAFE(log, &ctrlr->log_head, link, log_tmp) {
		TAILQ_REMOVE(&ctrlr->log_head, log, link);
		free(log);
	}
	STAILQ_FOREACH_SAFE(event, &ctrlr->async_events, link, event_tmp) {
		STAILQ_REMOVE(&ctrlr->async_events, event, spdk_nvmf_async_event_completion, link);
		free(event);
	}
	free(ctrlr);
}

void
nvmf_ctrlr_destruct(struct spdk_nvmf_ctrlr *ctrlr)
{
	nvmf_subsystem_remove_ctrlr(ctrlr->subsys, ctrlr);

	spdk_thread_send_msg(ctrlr->thread, _nvmf_ctrlr_destruct, ctrlr);
}

static void
nvmf_ctrlr_add_io_qpair(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvmf_qpair *admin_qpair = ctrlr->admin_qpair;

	SPDK_DTRACE_PROBE4(nvmf_ctrlr_add_io_qpair, ctrlr, req->qpair, req->qpair->qid,
			   spdk_thread_get_id(ctrlr->thread));

	/* Unit test will check qpair->ctrlr after calling spdk_nvmf_ctrlr_connect.
	  * For error case, the value should be NULL. So set it to NULL at first.
	  */
	qpair->ctrlr = NULL;

	/* Make sure the controller is not being destroyed. */
	if (ctrlr->in_destruct) {
		SPDK_ERRLOG("Got I/O connect while ctrlr was being destroyed.\n");
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		goto end;
	}

	if (ctrlr->subsys->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY) {
		SPDK_ERRLOG("I/O connect not allowed on discovery controller\n");
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		goto end;
	}

	if (!ctrlr->vcprop.cc.bits.en) {
		SPDK_ERRLOG("Got I/O connect before ctrlr was enabled\n");
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		goto end;
	}

	if (1u << ctrlr->vcprop.cc.bits.iosqes != sizeof(struct spdk_nvme_cmd)) {
		SPDK_ERRLOG("Got I/O connect with invalid IOSQES %u\n",
			    ctrlr->vcprop.cc.bits.iosqes);
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		goto end;
	}

	if (1u << ctrlr->vcprop.cc.bits.iocqes != sizeof(struct spdk_nvme_cpl)) {
		SPDK_ERRLOG("Got I/O connect with invalid IOCQES %u\n",
			    ctrlr->vcprop.cc.bits.iocqes);
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		goto end;
	}

	if (admin_qpair->state != SPDK_NVMF_QPAIR_ACTIVE || admin_qpair->group == NULL) {
		/* There is a chance that admin qpair is being destroyed at this moment due to e.g.
		 * expired keep alive timer. Part of the qpair destruction process is change of qpair's
		 * state to DEACTIVATING and removing it from poll group */
		SPDK_ERRLOG("Inactive admin qpair (state %d, group %p)\n", admin_qpair->state, admin_qpair->group);
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		goto end;
	}

	ctrlr_add_qpair_and_update_rsp(qpair, ctrlr, rsp);
end:
	spdk_nvmf_request_complete(req);
}

static void
_nvmf_ctrlr_add_io_qpair(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	struct spdk_nvmf_fabric_connect_data *data = req->data;
	struct spdk_nvmf_ctrlr *ctrlr;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_qpair *admin_qpair;
	struct spdk_nvmf_tgt *tgt = qpair->transport->tgt;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvme_transport_id listen_trid = {};
	const struct spdk_nvmf_subsystem_listener *listener;

	SPDK_DEBUGLOG(nvmf, "Connect I/O Queue for controller id 0x%x\n", data->cntlid);

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, data->subnqn);
	/* We already checked this in spdk_nvmf_ctrlr_connect */
	assert(subsystem != NULL);

	ctrlr = nvmf_subsystem_get_ctrlr(subsystem, data->cntlid);
	if (ctrlr == NULL) {
		SPDK_ERRLOG("Unknown controller ID 0x%x\n", data->cntlid);
		SPDK_NVMF_INVALID_CONNECT_DATA(rsp, cntlid);
		spdk_nvmf_request_complete(req);
		return;
	}

	/* fail before passing a message to the controller thread. */
	if (ctrlr->in_destruct) {
		SPDK_ERRLOG("Got I/O connect while ctrlr was being destroyed.\n");
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		spdk_nvmf_request_complete(req);
		return;
	}

	/* If ANA reporting is enabled, check if I/O connect is on the same listener. */
	if (subsystem->flags.ana_reporting) {
		if (spdk_nvmf_qpair_get_listen_trid(req->qpair, &listen_trid) != 0) {
			SPDK_ERRLOG("Could not get listener transport ID\n");
			SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
			spdk_nvmf_request_complete(req);
			return;
		}

		listener = nvmf_subsystem_find_listener(subsystem, &listen_trid);
		if (listener != ctrlr->listener) {
			SPDK_ERRLOG("I/O connect is on a listener different from admin connect\n");
			SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
			spdk_nvmf_request_complete(req);
			return;
		}
	}

	admin_qpair = ctrlr->admin_qpair;
	if (admin_qpair->state != SPDK_NVMF_QPAIR_ACTIVE || admin_qpair->group == NULL) {
		/* There is a chance that admin qpair is being destroyed at this moment due to e.g.
		 * expired keep alive timer. Part of the qpair destruction process is change of qpair's
		 * state to DEACTIVATING and removing it from poll group */
		SPDK_ERRLOG("Inactive admin qpair (state %d, group %p)\n", admin_qpair->state, admin_qpair->group);
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, qid);
		spdk_nvmf_request_complete(req);
		return;
	}
	qpair->ctrlr = ctrlr;
	spdk_thread_send_msg(admin_qpair->group->thread, nvmf_ctrlr_add_io_qpair, req);
}

static bool
nvmf_qpair_access_allowed(struct spdk_nvmf_qpair *qpair, struct spdk_nvmf_subsystem *subsystem,
			  const char *hostnqn)
{
	struct spdk_nvme_transport_id listen_trid = {};

	if (!spdk_nvmf_subsystem_host_allowed(subsystem, hostnqn)) {
		SPDK_ERRLOG("Subsystem '%s' does not allow host '%s'\n", subsystem->subnqn, hostnqn);
		return false;
	}

	if (spdk_nvmf_qpair_get_listen_trid(qpair, &listen_trid)) {
		SPDK_ERRLOG("Subsystem '%s' is unable to enforce access control due to an internal error.\n",
			    subsystem->subnqn);
		return false;
	}

	if (!spdk_nvmf_subsystem_listener_allowed(subsystem, &listen_trid)) {
		SPDK_ERRLOG("Subsystem '%s' does not allow host '%s' to connect at this address.\n",
			    subsystem->subnqn, hostnqn);
		return false;
	}

	return true;
}

static int
_nvmf_ctrlr_connect(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_connect_data *data = req->data;
	struct spdk_nvmf_fabric_connect_cmd *cmd = &req->cmd->connect_cmd;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_transport *transport = qpair->transport;
	struct spdk_nvmf_ctrlr *ctrlr;
	struct spdk_nvmf_subsystem *subsystem;

	SPDK_DEBUGLOG(nvmf, "recfmt 0x%x qid %u sqsize %u\n",
		      cmd->recfmt, cmd->qid, cmd->sqsize);

	SPDK_DEBUGLOG(nvmf, "Connect data:\n");
	SPDK_DEBUGLOG(nvmf, "  cntlid:  0x%04x\n", data->cntlid);
	SPDK_DEBUGLOG(nvmf, "  hostid: %08x-%04x-%04x-%02x%02x-%04x%08x ***\n",
		      ntohl(*(uint32_t *)&data->hostid[0]),
		      ntohs(*(uint16_t *)&data->hostid[4]),
		      ntohs(*(uint16_t *)&data->hostid[6]),
		      data->hostid[8],
		      data->hostid[9],
		      ntohs(*(uint16_t *)&data->hostid[10]),
		      ntohl(*(uint32_t *)&data->hostid[12]));
	SPDK_DEBUGLOG(nvmf, "  subnqn: \"%s\"\n", data->subnqn);
	SPDK_DEBUGLOG(nvmf, "  hostnqn: \"%s\"\n", data->hostnqn);

	subsystem = spdk_nvmf_tgt_find_subsystem(transport->tgt, data->subnqn);
	if (!subsystem) {
		SPDK_NVMF_INVALID_CONNECT_DATA(rsp, subnqn);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (cmd->recfmt != 0) {
		SPDK_ERRLOG("Connect command unsupported RECFMT %u\n", cmd->recfmt);
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVMF_FABRIC_SC_INCOMPATIBLE_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/*
	 * SQSIZE is a 0-based value, so it must be at least 1 (minimum queue depth is 2) and
	 * strictly less than max_aq_depth (admin queues) or max_queue_depth (io queues).
	 */
	if (cmd->sqsize == 0) {
		SPDK_ERRLOG("Invalid SQSIZE = 0\n");
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, sqsize);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (cmd->qid == 0) {
		if (cmd->sqsize >= transport->opts.max_aq_depth) {
			SPDK_ERRLOG("Invalid SQSIZE for admin queue %u (min 1, max %u)\n",
				    cmd->sqsize, transport->opts.max_aq_depth - 1);
			SPDK_NVMF_INVALID_CONNECT_CMD(rsp, sqsize);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	} else if (cmd->sqsize >= transport->opts.max_queue_depth) {
		SPDK_ERRLOG("Invalid SQSIZE %u (min 1, max %u)\n",
			    cmd->sqsize, transport->opts.max_queue_depth - 1);
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, sqsize);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	qpair->sq_head_max = cmd->sqsize;
	qpair->qid = cmd->qid;

	if (0 == qpair->qid) {
		qpair->group->stat.admin_qpairs++;
		qpair->group->stat.current_admin_qpairs++;
	} else {
		qpair->group->stat.io_qpairs++;
		qpair->group->stat.current_io_qpairs++;
	}

	if (cmd->qid == 0) {
		SPDK_DEBUGLOG(nvmf, "Connect Admin Queue for controller ID 0x%x\n", data->cntlid);

		if (spdk_nvme_trtype_is_fabrics(transport->ops->type) && data->cntlid != 0xFFFF) {
			/* This NVMf target only supports dynamic mode. */
			SPDK_ERRLOG("The NVMf target only supports dynamic mode (CNTLID = 0x%x).\n", data->cntlid);
			SPDK_NVMF_INVALID_CONNECT_DATA(rsp, cntlid);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		/* Establish a new ctrlr */
		ctrlr = nvmf_ctrlr_create(subsystem, req, cmd, data);
		if (!ctrlr) {
			SPDK_ERRLOG("nvmf_ctrlr_create() failed\n");
			rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		} else {
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
	} else {
		spdk_thread_send_msg(subsystem->thread, _nvmf_ctrlr_add_io_qpair, req);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	}
}

static inline bool
nvmf_request_is_fabric_connect(struct spdk_nvmf_request *req)
{
	return req->cmd->nvmf_cmd.opcode == SPDK_NVME_OPC_FABRIC &&
	       req->cmd->nvmf_cmd.fctype == SPDK_NVMF_FABRIC_COMMAND_CONNECT;
}

static struct spdk_nvmf_subsystem_poll_group *
nvmf_subsystem_pg_from_connect_cmd(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_connect_data *data;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;

	assert(nvmf_request_is_fabric_connect(req));
	assert(req->qpair->ctrlr == NULL);

	data = req->data;
	tgt = req->qpair->transport->tgt;

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, data->subnqn);
	if (subsystem == NULL) {
		return NULL;
	}

	return &req->qpair->group->sgroups[subsystem->id];
}

int
spdk_nvmf_ctrlr_connect(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	enum spdk_nvmf_request_exec_status status;

	sgroup = nvmf_subsystem_pg_from_connect_cmd(req);
	if (!sgroup) {
		SPDK_NVMF_INVALID_CONNECT_DATA(rsp, subnqn);
		status = SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		goto out;
	}

	sgroup->mgmt_io_outstanding++;
	TAILQ_INSERT_TAIL(&qpair->outstanding, req, link);

	status = _nvmf_ctrlr_connect(req);

out:
	if (status == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		_nvmf_request_complete(req);
	}

	return status;
}

static int nvmf_ctrlr_cmd_connect(struct spdk_nvmf_request *req);

static int
retry_connect(void *arg)
{
	struct spdk_nvmf_request *req = arg;
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	int rc;

	sgroup = nvmf_subsystem_pg_from_connect_cmd(req);
	assert(sgroup != NULL);
	sgroup->mgmt_io_outstanding++;
	spdk_poller_unregister(&req->poller);
	rc = nvmf_ctrlr_cmd_connect(req);
	if (rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		_nvmf_request_complete(req);
	}
	return SPDK_POLLER_BUSY;
}

static int
nvmf_ctrlr_cmd_connect(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_connect_data *data = req->data;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	struct spdk_nvmf_transport *transport = req->qpair->transport;
	struct spdk_nvmf_subsystem *subsystem;

	if (req->length < sizeof(struct spdk_nvmf_fabric_connect_data)) {
		SPDK_ERRLOG("Connect command data length 0x%x too small\n", req->length);
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(transport->tgt, data->subnqn);
	if (!subsystem) {
		SPDK_NVMF_INVALID_CONNECT_DATA(rsp, subnqn);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if ((subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE) ||
	    (subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSING) ||
	    (subsystem->state == SPDK_NVMF_SUBSYSTEM_PAUSED) ||
	    (subsystem->state == SPDK_NVMF_SUBSYSTEM_DEACTIVATING)) {
		struct spdk_nvmf_subsystem_poll_group *sgroup;

		if (req->timeout_tsc == 0) {
			/* We will only retry the request up to 1 second. */
			req->timeout_tsc = spdk_get_ticks() + spdk_get_ticks_hz();
		} else if (spdk_get_ticks() > req->timeout_tsc) {
			SPDK_ERRLOG("Subsystem '%s' was not ready for 1 second\n", subsystem->subnqn);
			rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
			rsp->status.sc = SPDK_NVMF_FABRIC_SC_CONTROLLER_BUSY;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		/* Subsystem is not ready to handle a connect. Use a poller to retry it
		 * again later. Decrement the mgmt_io_outstanding to avoid the
		 * subsystem waiting for this command to complete before unpausing.
		 */
		sgroup = nvmf_subsystem_pg_from_connect_cmd(req);
		assert(sgroup != NULL);
		sgroup->mgmt_io_outstanding--;
		SPDK_DEBUGLOG(nvmf, "Subsystem '%s' is not ready for connect, retrying...\n", subsystem->subnqn);
		req->poller = SPDK_POLLER_REGISTER(retry_connect, req, 100);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	}

	/* Ensure that hostnqn is null terminated */
	if (!memchr(data->hostnqn, '\0', SPDK_NVMF_NQN_MAX_LEN + 1)) {
		SPDK_ERRLOG("Connect HOSTNQN is not null terminated\n");
		SPDK_NVMF_INVALID_CONNECT_DATA(rsp, hostnqn);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (!nvmf_qpair_access_allowed(req->qpair, subsystem, data->hostnqn)) {
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_HOST;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return _nvmf_ctrlr_connect(req);
}

static int
nvmf_ctrlr_association_remove(void *ctx)
{
	struct spdk_nvmf_ctrlr *ctrlr = ctx;
	int rc;

	nvmf_ctrlr_stop_association_timer(ctrlr);

	if (ctrlr->in_destruct) {
		return SPDK_POLLER_IDLE;
	}
	SPDK_DEBUGLOG(nvmf, "Disconnecting host from subsystem %s due to association timeout.\n",
		      ctrlr->subsys->subnqn);

	if (ctrlr->admin_qpair) {
		rc = spdk_nvmf_qpair_disconnect(ctrlr->admin_qpair, NULL, NULL);
		if (rc < 0) {
			SPDK_ERRLOG("Fail to disconnect admin ctrlr qpair\n");
			assert(false);
		}
	}

	return SPDK_POLLER_BUSY;
}

static int
_nvmf_ctrlr_cc_reset_shn_done(void *ctx)
{
	struct spdk_nvmf_ctrlr *ctrlr = ctx;
	uint64_t now = spdk_get_ticks();
	uint32_t count;

	if (ctrlr->cc_timer) {
		spdk_poller_unregister(&ctrlr->cc_timer);
	}

	count = spdk_bit_array_count_set(ctrlr->qpair_mask);
	SPDK_DEBUGLOG(nvmf, "ctrlr %p active queue count %u\n", ctrlr, count);

	if (count > 1) {
		if (now < ctrlr->cc_timeout_tsc) {
			/* restart cc timer */
			ctrlr->cc_timer = SPDK_POLLER_REGISTER(_nvmf_ctrlr_cc_reset_shn_done, ctrlr, 100 * 1000);
			return SPDK_POLLER_IDLE;
		} else {
			/* controller fatal status */
			SPDK_WARNLOG("IO timeout, ctrlr %p is in fatal status\n", ctrlr);
			ctrlr->vcprop.csts.bits.cfs = 1;
		}
	}

	spdk_poller_unregister(&ctrlr->cc_timeout_timer);

	if (ctrlr->disconnect_is_shn) {
		ctrlr->vcprop.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
		ctrlr->disconnect_is_shn = false;
	} else {
		/* Only a subset of the registers are cleared out on a reset */
		ctrlr->vcprop.cc.raw = 0;
		ctrlr->vcprop.csts.raw = 0;
	}

	/* After CC.EN transitions to 0 (due to shutdown or reset), the association
	 * between the host and controller shall be preserved for at least 2 minutes */
	if (ctrlr->association_timer) {
		SPDK_DEBUGLOG(nvmf, "Association timer already set\n");
		nvmf_ctrlr_stop_association_timer(ctrlr);
	}
	if (ctrlr->association_timeout) {
		ctrlr->association_timer = SPDK_POLLER_REGISTER(nvmf_ctrlr_association_remove, ctrlr,
					   ctrlr->association_timeout * 1000);
	}
	ctrlr->disconnect_in_progress = false;
	return SPDK_POLLER_BUSY;
}

static void
nvmf_ctrlr_cc_reset_shn_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvmf_ctrlr *ctrlr = spdk_io_channel_iter_get_ctx(i);

	if (status < 0) {
		SPDK_ERRLOG("Fail to disconnect io ctrlr qpairs\n");
		assert(false);
	}

	_nvmf_ctrlr_cc_reset_shn_done((void *)ctrlr);
}

static void
nvmf_bdev_complete_reset(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	SPDK_NOTICELOG("Resetting bdev done with %s\n", success ? "success" : "failure");

	spdk_bdev_free_io(bdev_io);
}


static int
nvmf_ctrlr_cc_timeout(void *ctx)
{
	struct spdk_nvmf_ctrlr *ctrlr = ctx;
	struct spdk_nvmf_poll_group *group = ctrlr->admin_qpair->group;
	struct spdk_nvmf_ns *ns;
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info;

	assert(group != NULL && group->sgroups != NULL);
	spdk_poller_unregister(&ctrlr->cc_timeout_timer);
	SPDK_DEBUGLOG(nvmf, "Ctrlr %p reset or shutdown timeout\n", ctrlr);

	for (ns = spdk_nvmf_subsystem_get_first_ns(ctrlr->subsys); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(ctrlr->subsys, ns)) {
		if (ns->bdev == NULL) {
			continue;
		}
		ns_info = &group->sgroups[ctrlr->subsys->id].ns_info[ns->opts.nsid - 1];
		SPDK_NOTICELOG("Ctrlr %p resetting NSID %u\n", ctrlr, ns->opts.nsid);
		spdk_bdev_reset(ns->desc, ns_info->channel, nvmf_bdev_complete_reset, NULL);
	}

	return SPDK_POLLER_BUSY;
}

const struct spdk_nvmf_registers *
spdk_nvmf_ctrlr_get_regs(struct spdk_nvmf_ctrlr *ctrlr)
{
	return &ctrlr->vcprop;
}

static uint64_t
nvmf_prop_get_cap(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.cap.raw;
}

static uint64_t
nvmf_prop_get_vs(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.vs.raw;
}

static uint64_t
nvmf_prop_get_cc(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.cc.raw;
}

static bool
nvmf_prop_set_cc(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	union spdk_nvme_cc_register cc, diff;
	uint32_t cc_timeout_ms;

	cc.raw = value;

	SPDK_DEBUGLOG(nvmf, "cur CC: 0x%08x\n", ctrlr->vcprop.cc.raw);
	SPDK_DEBUGLOG(nvmf, "new CC: 0x%08x\n", cc.raw);

	/*
	 * Calculate which bits changed between the current and new CC.
	 * Mark each bit as 0 once it is handled to determine if any unhandled bits were changed.
	 */
	diff.raw = cc.raw ^ ctrlr->vcprop.cc.raw;

	if (diff.bits.en) {
		if (cc.bits.en) {
			SPDK_DEBUGLOG(nvmf, "Property Set CC Enable!\n");
			nvmf_ctrlr_stop_association_timer(ctrlr);

			ctrlr->vcprop.cc.bits.en = 1;
			ctrlr->vcprop.csts.bits.rdy = 1;
		} else {
			SPDK_DEBUGLOG(nvmf, "Property Set CC Disable!\n");
			if (ctrlr->disconnect_in_progress) {
				SPDK_DEBUGLOG(nvmf, "Disconnect in progress\n");
				return true;
			}

			ctrlr->cc_timeout_timer = SPDK_POLLER_REGISTER(nvmf_ctrlr_cc_timeout, ctrlr,
						  NVMF_CC_RESET_SHN_TIMEOUT_IN_MS * 1000);
			/* Make sure cc_timeout_ms is between cc_timeout_timer and Host reset/shutdown timeout */
			cc_timeout_ms = (NVMF_CC_RESET_SHN_TIMEOUT_IN_MS + NVMF_CTRLR_RESET_SHN_TIMEOUT_IN_MS) / 2;
			ctrlr->cc_timeout_tsc = spdk_get_ticks() + cc_timeout_ms * spdk_get_ticks_hz() / (uint64_t)1000;

			ctrlr->vcprop.cc.bits.en = 0;
			ctrlr->disconnect_in_progress = true;
			ctrlr->disconnect_is_shn = false;
			spdk_for_each_channel(ctrlr->subsys->tgt,
					      nvmf_ctrlr_disconnect_io_qpairs_on_pg,
					      ctrlr,
					      nvmf_ctrlr_cc_reset_shn_done);
		}
		diff.bits.en = 0;
	}

	if (diff.bits.shn) {
		if (cc.bits.shn == SPDK_NVME_SHN_NORMAL ||
		    cc.bits.shn == SPDK_NVME_SHN_ABRUPT) {
			SPDK_DEBUGLOG(nvmf, "Property Set CC Shutdown %u%ub!\n",
				      cc.bits.shn >> 1, cc.bits.shn & 1);
			if (ctrlr->disconnect_in_progress) {
				SPDK_DEBUGLOG(nvmf, "Disconnect in progress\n");
				return true;
			}

			ctrlr->cc_timeout_timer = SPDK_POLLER_REGISTER(nvmf_ctrlr_cc_timeout, ctrlr,
						  NVMF_CC_RESET_SHN_TIMEOUT_IN_MS * 1000);
			/* Make sure cc_timeout_ms is between cc_timeout_timer and Host reset/shutdown timeout */
			cc_timeout_ms = (NVMF_CC_RESET_SHN_TIMEOUT_IN_MS + NVMF_CTRLR_RESET_SHN_TIMEOUT_IN_MS) / 2;
			ctrlr->cc_timeout_tsc = spdk_get_ticks() + cc_timeout_ms * spdk_get_ticks_hz() / (uint64_t)1000;

			ctrlr->vcprop.cc.bits.shn = cc.bits.shn;
			ctrlr->disconnect_in_progress = true;
			ctrlr->disconnect_is_shn = true;
			spdk_for_each_channel(ctrlr->subsys->tgt,
					      nvmf_ctrlr_disconnect_io_qpairs_on_pg,
					      ctrlr,
					      nvmf_ctrlr_cc_reset_shn_done);

			/* From the time a shutdown is initiated the controller shall disable
			 * Keep Alive timer */
			nvmf_ctrlr_stop_keep_alive_timer(ctrlr);
		} else if (cc.bits.shn == 0) {
			ctrlr->vcprop.cc.bits.shn = 0;
		} else {
			SPDK_ERRLOG("Prop Set CC: Invalid SHN value %u%ub\n",
				    cc.bits.shn >> 1, cc.bits.shn & 1);
			return false;
		}
		diff.bits.shn = 0;
	}

	if (diff.bits.iosqes) {
		SPDK_DEBUGLOG(nvmf, "Prop Set IOSQES = %u (%u bytes)\n",
			      cc.bits.iosqes, 1u << cc.bits.iosqes);
		ctrlr->vcprop.cc.bits.iosqes = cc.bits.iosqes;
		diff.bits.iosqes = 0;
	}

	if (diff.bits.iocqes) {
		SPDK_DEBUGLOG(nvmf, "Prop Set IOCQES = %u (%u bytes)\n",
			      cc.bits.iocqes, 1u << cc.bits.iocqes);
		ctrlr->vcprop.cc.bits.iocqes = cc.bits.iocqes;
		diff.bits.iocqes = 0;
	}

	if (diff.bits.ams) {
		SPDK_ERRLOG("Arbitration Mechanism Selected (AMS) 0x%x not supported!\n", cc.bits.ams);
		return false;
	}

	if (diff.bits.mps) {
		SPDK_ERRLOG("Memory Page Size (MPS) %u KiB not supported!\n", (1 << (2 + cc.bits.mps)));
		return false;
	}

	if (diff.bits.css) {
		SPDK_ERRLOG("I/O Command Set Selected (CSS) 0x%x not supported!\n", cc.bits.css);
		return false;
	}

	if (diff.raw != 0) {
		/* Print an error message, but don't fail the command in this case.
		 * If we did want to fail in this case, we'd need to ensure we acted
		 * on no other bits or the initiator gets confused. */
		SPDK_ERRLOG("Prop Set CC toggled reserved bits 0x%x!\n", diff.raw);
	}

	return true;
}

static uint64_t
nvmf_prop_get_csts(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.csts.raw;
}

static uint64_t
nvmf_prop_get_aqa(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.aqa.raw;
}

static bool
nvmf_prop_set_aqa(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	union spdk_nvme_aqa_register aqa;

	aqa.raw = value;

	if (aqa.bits.asqs < SPDK_NVME_ADMIN_QUEUE_MIN_ENTRIES - 1 ||
	    aqa.bits.acqs < SPDK_NVME_ADMIN_QUEUE_MIN_ENTRIES - 1 ||
	    aqa.bits.reserved1 != 0 || aqa.bits.reserved2 != 0) {
		return false;
	}

	ctrlr->vcprop.aqa.raw = value;

	return true;
}

static uint64_t
nvmf_prop_get_asq(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.asq;
}

static bool
nvmf_prop_set_asq_lower(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	ctrlr->vcprop.asq = (ctrlr->vcprop.asq & (0xFFFFFFFFULL << 32ULL)) | value;

	return true;
}

static bool
nvmf_prop_set_asq_upper(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	ctrlr->vcprop.asq = (ctrlr->vcprop.asq & 0xFFFFFFFFULL) | ((uint64_t)value << 32ULL);

	return true;
}

static uint64_t
nvmf_prop_get_acq(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.acq;
}

static bool
nvmf_prop_set_acq_lower(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	ctrlr->vcprop.acq = (ctrlr->vcprop.acq & (0xFFFFFFFFULL << 32ULL)) | value;

	return true;
}

static bool
nvmf_prop_set_acq_upper(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value)
{
	ctrlr->vcprop.acq = (ctrlr->vcprop.acq & 0xFFFFFFFFULL) | ((uint64_t)value << 32ULL);

	return true;
}

struct nvmf_prop {
	uint32_t ofst;
	uint8_t size;
	char name[11];
	uint64_t (*get_cb)(struct spdk_nvmf_ctrlr *ctrlr);
	bool (*set_cb)(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value);
	bool (*set_upper_cb)(struct spdk_nvmf_ctrlr *ctrlr, uint32_t value);
};

#define PROP(field, size, get_cb, set_cb, set_upper_cb) \
	{ \
		offsetof(struct spdk_nvme_registers, field), \
		size, \
		#field, \
		get_cb, set_cb, set_upper_cb \
	}

static const struct nvmf_prop nvmf_props[] = {
	PROP(cap,  8, nvmf_prop_get_cap,  NULL,                    NULL),
	PROP(vs,   4, nvmf_prop_get_vs,   NULL,                    NULL),
	PROP(cc,   4, nvmf_prop_get_cc,   nvmf_prop_set_cc,        NULL),
	PROP(csts, 4, nvmf_prop_get_csts, NULL,                    NULL),
	PROP(aqa,  4, nvmf_prop_get_aqa,  nvmf_prop_set_aqa,       NULL),
	PROP(asq,  8, nvmf_prop_get_asq,  nvmf_prop_set_asq_lower, nvmf_prop_set_asq_upper),
	PROP(acq,  8, nvmf_prop_get_acq,  nvmf_prop_set_acq_lower, nvmf_prop_set_acq_upper),
};

static const struct nvmf_prop *
find_prop(uint32_t ofst, uint8_t size)
{
	size_t i;

	for (i = 0; i < SPDK_COUNTOF(nvmf_props); i++) {
		const struct nvmf_prop *prop = &nvmf_props[i];

		if ((ofst >= prop->ofst) && (ofst + size <= prop->ofst + prop->size)) {
			return prop;
		}
	}

	return NULL;
}

static int
nvmf_property_get(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvmf_fabric_prop_get_cmd *cmd = &req->cmd->prop_get_cmd;
	struct spdk_nvmf_fabric_prop_get_rsp *response = &req->rsp->prop_get_rsp;
	const struct nvmf_prop *prop;
	uint8_t size;

	response->status.sc = 0;
	response->value.u64 = 0;

	SPDK_DEBUGLOG(nvmf, "size %d, offset 0x%x\n",
		      cmd->attrib.size, cmd->ofst);

	switch (cmd->attrib.size) {
	case SPDK_NVMF_PROP_SIZE_4:
		size = 4;
		break;
	case SPDK_NVMF_PROP_SIZE_8:
		size = 8;
		break;
	default:
		SPDK_DEBUGLOG(nvmf, "Invalid size value %d\n", cmd->attrib.size);
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	prop = find_prop(cmd->ofst, size);
	if (prop == NULL || prop->get_cb == NULL) {
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	SPDK_DEBUGLOG(nvmf, "name: %s\n", prop->name);

	response->value.u64 = prop->get_cb(ctrlr);

	if (size != prop->size) {
		/* The size must be 4 and the prop->size is 8. Figure out which part of the property to read. */
		assert(size == 4);
		assert(prop->size == 8);

		if (cmd->ofst == prop->ofst) {
			/* Keep bottom 4 bytes only */
			response->value.u64 &= 0xFFFFFFFF;
		} else {
			/* Keep top 4 bytes only */
			response->value.u64 >>= 32;
		}
	}

	SPDK_DEBUGLOG(nvmf, "response value: 0x%" PRIx64 "\n", response->value.u64);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_property_set(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvmf_fabric_prop_set_cmd *cmd = &req->cmd->prop_set_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	const struct nvmf_prop *prop;
	uint64_t value;
	uint8_t size;
	bool ret;

	SPDK_DEBUGLOG(nvmf, "size %d, offset 0x%x, value 0x%" PRIx64 "\n",
		      cmd->attrib.size, cmd->ofst, cmd->value.u64);

	switch (cmd->attrib.size) {
	case SPDK_NVMF_PROP_SIZE_4:
		size = 4;
		break;
	case SPDK_NVMF_PROP_SIZE_8:
		size = 8;
		break;
	default:
		SPDK_DEBUGLOG(nvmf, "Invalid size value %d\n", cmd->attrib.size);
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	prop = find_prop(cmd->ofst, size);
	if (prop == NULL || prop->set_cb == NULL) {
		SPDK_INFOLOG(nvmf, "Invalid offset 0x%x\n", cmd->ofst);
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	SPDK_DEBUGLOG(nvmf, "name: %s\n", prop->name);

	value = cmd->value.u64;

	if (prop->size == 4) {
		ret = prop->set_cb(ctrlr, (uint32_t)value);
	} else if (size != prop->size) {
		/* The size must be 4 and the prop->size is 8. Figure out which part of the property to write. */
		assert(size == 4);
		assert(prop->size == 8);

		if (cmd->ofst == prop->ofst) {
			ret = prop->set_cb(ctrlr, (uint32_t)value);
		} else {
			ret = prop->set_upper_cb(ctrlr, (uint32_t)value);
		}
	} else {
		ret = prop->set_cb(ctrlr, (uint32_t)value);
		if (ret) {
			ret = prop->set_upper_cb(ctrlr, (uint32_t)(value >> 32));
		}
	}

	if (!ret) {
		SPDK_ERRLOG("prop set_cb failed\n");
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_arbitration(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	SPDK_DEBUGLOG(nvmf, "Set Features - Arbitration (cdw11 = 0x%0x)\n", cmd->cdw11);

	ctrlr->feat.arbitration.raw = cmd->cdw11;
	ctrlr->feat.arbitration.bits.reserved = 0;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_power_management(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_DEBUGLOG(nvmf, "Set Features - Power Management (cdw11 = 0x%0x)\n", cmd->cdw11);

	/* Only PS = 0 is allowed, since we report NPSS = 0 */
	if (cmd->cdw11_bits.feat_power_management.bits.ps != 0) {
		SPDK_ERRLOG("Invalid power state %u\n", cmd->cdw11_bits.feat_power_management.bits.ps);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ctrlr->feat.power_management.raw = cmd->cdw11;
	ctrlr->feat.power_management.bits.reserved = 0;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static bool
temp_threshold_opts_valid(const union spdk_nvme_feat_temperature_threshold *opts)
{
	/*
	 * Valid TMPSEL values:
	 *  0000b - 1000b: temperature sensors
	 *  1111b: set all implemented temperature sensors
	 */
	if (opts->bits.tmpsel >= 9 && opts->bits.tmpsel != 15) {
		/* 1001b - 1110b: reserved */
		SPDK_ERRLOG("Invalid TMPSEL %u\n", opts->bits.tmpsel);
		return false;
	}

	/*
	 * Valid THSEL values:
	 *  00b: over temperature threshold
	 *  01b: under temperature threshold
	 */
	if (opts->bits.thsel > 1) {
		/* 10b - 11b: reserved */
		SPDK_ERRLOG("Invalid THSEL %u\n", opts->bits.thsel);
		return false;
	}

	return true;
}

static int
nvmf_ctrlr_set_features_temperature_threshold(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_DEBUGLOG(nvmf, "Set Features - Temperature Threshold (cdw11 = 0x%0x)\n", cmd->cdw11);

	if (!temp_threshold_opts_valid(&cmd->cdw11_bits.feat_temp_threshold)) {
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* TODO: no sensors implemented - ignore new values */
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_get_features_temperature_threshold(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_DEBUGLOG(nvmf, "Get Features - Temperature Threshold (cdw11 = 0x%0x)\n", cmd->cdw11);

	if (!temp_threshold_opts_valid(&cmd->cdw11_bits.feat_temp_threshold)) {
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* TODO: no sensors implemented - return 0 for all thresholds */
	rsp->cdw0 = 0;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_get_features_interrupt_vector_configuration(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	union spdk_nvme_feat_interrupt_vector_configuration iv_conf = {};

	SPDK_DEBUGLOG(nvmf, "Get Features - Interrupt Vector Configuration (cdw11 = 0x%0x)\n", cmd->cdw11);

	iv_conf.bits.iv = cmd->cdw11_bits.feat_interrupt_vector_configuration.bits.iv;
	iv_conf.bits.cd = ctrlr->feat.interrupt_vector_configuration.bits.cd;
	rsp->cdw0 = iv_conf.raw;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_error_recovery(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_DEBUGLOG(nvmf, "Set Features - Error Recovery (cdw11 = 0x%0x)\n", cmd->cdw11);

	if (cmd->cdw11_bits.feat_error_recovery.bits.dulbe) {
		/*
		 * Host is not allowed to set this bit, since we don't advertise it in
		 * Identify Namespace.
		 */
		SPDK_ERRLOG("Host set unsupported DULBE bit\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ctrlr->feat.error_recovery.raw = cmd->cdw11;
	ctrlr->feat.error_recovery.bits.reserved = 0;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_volatile_write_cache(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	SPDK_DEBUGLOG(nvmf, "Set Features - Volatile Write Cache (cdw11 = 0x%0x)\n", cmd->cdw11);

	ctrlr->feat.volatile_write_cache.raw = cmd->cdw11;
	ctrlr->feat.volatile_write_cache.bits.reserved = 0;

	SPDK_DEBUGLOG(nvmf, "Set Features - Volatile Write Cache %s\n",
		      ctrlr->feat.volatile_write_cache.bits.wce ? "Enabled" : "Disabled");
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_write_atomicity(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	SPDK_DEBUGLOG(nvmf, "Set Features - Write Atomicity (cdw11 = 0x%0x)\n", cmd->cdw11);

	ctrlr->feat.write_atomicity.raw = cmd->cdw11;
	ctrlr->feat.write_atomicity.bits.reserved = 0;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_host_identifier(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	SPDK_ERRLOG("Set Features - Host Identifier not allowed\n");
	response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_get_features_host_identifier(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	SPDK_DEBUGLOG(nvmf, "Get Features - Host Identifier\n");

	if (!cmd->cdw11_bits.feat_host_identifier.bits.exhid) {
		/* NVMe over Fabrics requires EXHID=1 (128-bit/16-byte host ID) */
		SPDK_ERRLOG("Get Features - Host Identifier with EXHID=0 not allowed\n");
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (req->data == NULL || req->length < sizeof(ctrlr->hostid)) {
		SPDK_ERRLOG("Invalid data buffer for Get Features - Host Identifier\n");
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	spdk_uuid_copy((struct spdk_uuid *)req->data, &ctrlr->hostid);
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_get_features_reservation_notification_mask(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_ns *ns;

	SPDK_DEBUGLOG(nvmf, "get Features - Reservation Notification Mask\n");

	if (cmd->nsid == SPDK_NVME_GLOBAL_NS_TAG) {
		SPDK_ERRLOG("get Features - Invalid Namespace ID\n");
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ns = _nvmf_subsystem_get_ns(ctrlr->subsys, cmd->nsid);
	if (ns == NULL) {
		SPDK_ERRLOG("Set Features - Invalid Namespace ID\n");
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	rsp->cdw0 = ns->mask;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_reservation_notification_mask(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_ns *ns;

	SPDK_DEBUGLOG(nvmf, "Set Features - Reservation Notification Mask\n");

	if (cmd->nsid == SPDK_NVME_GLOBAL_NS_TAG) {
		for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
		     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
			ns->mask = cmd->cdw11;
		}
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ns = _nvmf_subsystem_get_ns(ctrlr->subsys, cmd->nsid);
	if (ns == NULL) {
		SPDK_ERRLOG("Set Features - Invalid Namespace ID\n");
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	ns->mask = cmd->cdw11;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_get_features_reservation_persistence(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_nvmf_ns *ns;

	SPDK_DEBUGLOG(nvmf, "Get Features - Reservation Persistence\n");

	ns = _nvmf_subsystem_get_ns(ctrlr->subsys, cmd->nsid);
	/* NSID with SPDK_NVME_GLOBAL_NS_TAG (=0xffffffff) also included */
	if (ns == NULL) {
		SPDK_ERRLOG("Get Features - Invalid Namespace ID\n");
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	response->cdw0 = ns->ptpl_activated;

	response->status.sct = SPDK_NVME_SCT_GENERIC;
	response->status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_reservation_persistence(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_nvmf_ns *ns;
	bool ptpl;

	SPDK_DEBUGLOG(nvmf, "Set Features - Reservation Persistence\n");

	ns = _nvmf_subsystem_get_ns(ctrlr->subsys, cmd->nsid);
	ptpl = cmd->cdw11_bits.feat_rsv_persistence.bits.ptpl;

	if (cmd->nsid != SPDK_NVME_GLOBAL_NS_TAG && ns && ns->ptpl_file) {
		ns->ptpl_activated = ptpl;
	} else if (cmd->nsid == SPDK_NVME_GLOBAL_NS_TAG) {
		for (ns = spdk_nvmf_subsystem_get_first_ns(ctrlr->subsys); ns && ns->ptpl_file;
		     ns = spdk_nvmf_subsystem_get_next_ns(ctrlr->subsys, ns)) {
			ns->ptpl_activated = ptpl;
		}
	} else {
		SPDK_ERRLOG("Set Features - Invalid Namespace ID or Reservation Configuration\n");
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* TODO: Feature not changeable for now */
	response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
	response->status.sc = SPDK_NVME_SC_FEATURE_ID_NOT_SAVEABLE;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_get_features_host_behavior_support(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_nvme_host_behavior host_behavior = {};

	SPDK_DEBUGLOG(nvmf, "Get Features - Host Behavior Support\n");

	if (req->data == NULL || req->length < sizeof(struct spdk_nvme_host_behavior)) {
		SPDK_ERRLOG("invalid data buffer for Host Behavior Support\n");
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	host_behavior.acre = ctrlr->acre_enabled;
	memcpy(req->data, &host_behavior, sizeof(host_behavior));

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_host_behavior_support(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_nvme_host_behavior *host_behavior;

	SPDK_DEBUGLOG(nvmf, "Set Features - Host Behavior Support\n");
	if (req->iovcnt != 1) {
		SPDK_ERRLOG("Host Behavior Support invalid iovcnt: %d\n", req->iovcnt);
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	if (req->iov[0].iov_len != sizeof(struct spdk_nvme_host_behavior)) {
		SPDK_ERRLOG("Host Behavior Support invalid iov_len: %zd\n", req->iov[0].iov_len);
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	host_behavior = (struct spdk_nvme_host_behavior *)req->iov[0].iov_base;
	if (host_behavior->acre == 0) {
		ctrlr->acre_enabled = false;
	} else if (host_behavior->acre == 1) {
		ctrlr->acre_enabled = true;
	} else {
		SPDK_ERRLOG("Host Behavior Support invalid acre: 0x%02x\n", host_behavior->acre);
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_keep_alive_timer(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_DEBUGLOG(nvmf, "Set Features - Keep Alive Timer (%u ms)\n", cmd->cdw11);

	/*
	 * if attempts to disable keep alive by setting kato to 0h
	 * a status value of keep alive invalid shall be returned
	 */
	if (cmd->cdw11_bits.feat_keep_alive_timer.bits.kato == 0) {
		rsp->status.sc = SPDK_NVME_SC_KEEP_ALIVE_INVALID;
	} else if (cmd->cdw11_bits.feat_keep_alive_timer.bits.kato < MIN_KEEP_ALIVE_TIMEOUT_IN_MS) {
		ctrlr->feat.keep_alive_timer.bits.kato = MIN_KEEP_ALIVE_TIMEOUT_IN_MS;
	} else {
		/* round up to milliseconds */
		ctrlr->feat.keep_alive_timer.bits.kato = spdk_divide_round_up(
					cmd->cdw11_bits.feat_keep_alive_timer.bits.kato,
					KAS_DEFAULT_VALUE * KAS_TIME_UNIT_IN_MS) *
				KAS_DEFAULT_VALUE * KAS_TIME_UNIT_IN_MS;
	}

	/*
	 * if change the keep alive timeout value successfully
	 * update the keep alive poller.
	 */
	if (cmd->cdw11_bits.feat_keep_alive_timer.bits.kato != 0) {
		if (ctrlr->keep_alive_poller != NULL) {
			spdk_poller_unregister(&ctrlr->keep_alive_poller);
		}
		ctrlr->keep_alive_poller = SPDK_POLLER_REGISTER(nvmf_ctrlr_keep_alive_poll, ctrlr,
					   ctrlr->feat.keep_alive_timer.bits.kato * 1000);
	}

	SPDK_DEBUGLOG(nvmf, "Set Features - Keep Alive Timer set to %u ms\n",
		      ctrlr->feat.keep_alive_timer.bits.kato);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_set_features_number_of_queues(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint32_t count;

	SPDK_DEBUGLOG(nvmf, "Set Features - Number of Queues, cdw11 0x%x\n",
		      req->cmd->nvme_cmd.cdw11);

	if (cmd->cdw11_bits.feat_num_of_queues.bits.ncqr == UINT16_MAX ||
	    cmd->cdw11_bits.feat_num_of_queues.bits.nsqr == UINT16_MAX) {
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	count = spdk_bit_array_count_set(ctrlr->qpair_mask);
	/* verify that the controller is ready to process commands */
	if (count > 1) {
		SPDK_DEBUGLOG(nvmf, "Queue pairs already active!\n");
		rsp->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
	} else {
		/*
		 * Ignore the value requested by the host -
		 * always return the pre-configured value based on max_qpairs_allowed.
		 */
		rsp->cdw0 = ctrlr->feat.number_of_queues.raw;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
nvmf_ctrlr_save_aers(struct spdk_nvmf_ctrlr *ctrlr, uint16_t *aer_cids,
		     uint16_t max_aers)
{
	struct spdk_nvmf_request *req;
	uint16_t i;

	if (!aer_cids || max_aers < ctrlr->nr_aer_reqs) {
		return -EINVAL;
	}

	for (i = 0; i < ctrlr->nr_aer_reqs; i++) {
		req = ctrlr->aer_req[i];
		aer_cids[i] = req->cmd->nvme_cmd.cid;
	}

	return ctrlr->nr_aer_reqs;
}

int
nvmf_ctrlr_save_migr_data(struct spdk_nvmf_ctrlr *ctrlr, struct nvmf_ctrlr_migr_data *data)
{
	uint32_t num_async_events = 0;
	struct spdk_nvmf_async_event_completion *event, *event_tmp;

	memcpy(&data->feat, &ctrlr->feat, sizeof(struct spdk_nvmf_ctrlr_feat));
	data->cntlid = ctrlr->cntlid;
	data->acre_enabled = ctrlr->acre_enabled;
	data->notice_aen_mask = ctrlr->notice_aen_mask;

	STAILQ_FOREACH_SAFE(event, &ctrlr->async_events, link, event_tmp) {
		data->async_events[num_async_events++].raw = event->event.raw;
		if (num_async_events == NVMF_MIGR_MAX_PENDING_AERS) {
			SPDK_ERRLOG("%p has too many pending AERs\n", ctrlr);
			break;
		}
	}
	data->num_async_events = num_async_events;

	return 0;
}

int
nvmf_ctrlr_restore_migr_data(struct spdk_nvmf_ctrlr *ctrlr, struct nvmf_ctrlr_migr_data *data)
{
	struct spdk_nvmf_async_event_completion *event;
	uint32_t i;

	memcpy(&ctrlr->feat, &data->feat, sizeof(struct spdk_nvmf_ctrlr_feat));
	ctrlr->acre_enabled = data->acre_enabled;
	ctrlr->notice_aen_mask = data->notice_aen_mask;

	for (i = 0; i < data->num_async_events; i++) {
		event = calloc(1, sizeof(struct spdk_nvmf_async_event_completion));
		if (!event) {
			return -ENOMEM;
		}
		event->event.raw = data->async_events[i].raw;
		STAILQ_INSERT_TAIL(&ctrlr->async_events, event, link);
	}

	return 0;
}

static int
nvmf_ctrlr_set_features_async_event_configuration(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	SPDK_DEBUGLOG(nvmf, "Set Features - Async Event Configuration, cdw11 0x%08x\n",
		      cmd->cdw11);
	ctrlr->feat.async_event_configuration.raw = cmd->cdw11;
	ctrlr->feat.async_event_configuration.bits.reserved1 = 0;
	ctrlr->feat.async_event_configuration.bits.reserved2 = 0;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_async_event_request(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_subsystem_poll_group *sgroup;
	struct spdk_nvmf_async_event_completion *pending_event;

	SPDK_DEBUGLOG(nvmf, "Async Event Request\n");

	/* AER cmd is an exception */
	sgroup = &req->qpair->group->sgroups[ctrlr->subsys->id];
	assert(sgroup != NULL);
	sgroup->mgmt_io_outstanding--;

	/* Four asynchronous events are supported for now */
	if (ctrlr->nr_aer_reqs >= NVMF_MAX_ASYNC_EVENTS) {
		SPDK_DEBUGLOG(nvmf, "AERL exceeded\n");
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (!STAILQ_EMPTY(&ctrlr->async_events)) {
		pending_event = STAILQ_FIRST(&ctrlr->async_events);
		rsp->cdw0 = pending_event->event.raw;
		STAILQ_REMOVE(&ctrlr->async_events, pending_event, spdk_nvmf_async_event_completion, link);
		free(pending_event);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ctrlr->aer_req[ctrlr->nr_aer_reqs++] = req;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

struct copy_iovs_ctx {
	struct iovec *iovs;
	int iovcnt;
	int cur_iov_idx;
	size_t cur_iov_offset;
};

static void
_clear_iovs(struct iovec *iovs, int iovcnt)
{
	int iov_idx = 0;
	struct iovec *iov;

	while (iov_idx < iovcnt) {
		iov = &iovs[iov_idx];
		memset(iov->iov_base, 0, iov->iov_len);
		iov_idx++;
	}
}

static void
_init_copy_iovs_ctx(struct copy_iovs_ctx *copy_ctx, struct iovec *iovs, int iovcnt)
{
	copy_ctx->iovs = iovs;
	copy_ctx->iovcnt = iovcnt;
	copy_ctx->cur_iov_idx = 0;
	copy_ctx->cur_iov_offset = 0;
}

static size_t
_copy_buf_to_iovs(struct copy_iovs_ctx *copy_ctx, const void *buf, size_t buf_len)
{
	size_t len, iov_remain_len, copied_len = 0;
	struct iovec *iov;

	if (buf_len == 0) {
		return 0;
	}

	while (copy_ctx->cur_iov_idx < copy_ctx->iovcnt) {
		iov = &copy_ctx->iovs[copy_ctx->cur_iov_idx];
		iov_remain_len = iov->iov_len - copy_ctx->cur_iov_offset;
		if (iov_remain_len == 0) {
			copy_ctx->cur_iov_idx++;
			copy_ctx->cur_iov_offset = 0;
			continue;
		}

		len = spdk_min(iov_remain_len, buf_len - copied_len);
		memcpy((char *)iov->iov_base + copy_ctx->cur_iov_offset,
		       (const char *)buf + copied_len,
		       len);
		copied_len += len;
		copy_ctx->cur_iov_offset += len;

		if (buf_len == copied_len) {
			return copied_len;
		}
	}

	return copied_len;
}

static void
nvmf_get_firmware_slot_log_page(struct iovec *iovs, int iovcnt, uint64_t offset, uint32_t length)
{
	struct spdk_nvme_firmware_page fw_page;
	size_t copy_len;
	struct copy_iovs_ctx copy_ctx;

	_init_copy_iovs_ctx(&copy_ctx, iovs, iovcnt);

	memset(&fw_page, 0, sizeof(fw_page));
	fw_page.afi.active_slot = 1;
	fw_page.afi.next_reset_slot = 0;
	spdk_strcpy_pad(fw_page.revision[0], FW_VERSION, sizeof(fw_page.revision[0]), ' ');

	if (offset < sizeof(fw_page)) {
		copy_len = spdk_min(sizeof(fw_page) - offset, length);
		if (copy_len > 0) {
			_copy_buf_to_iovs(&copy_ctx, (const char *)&fw_page + offset, copy_len);
		}
	}
}

/*
 * Asynchronous Event Mask Bit
 */
enum spdk_nvme_async_event_mask_bit {
	/* Mask Namespace Change Notification */
	SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGE_MASK_BIT		= 0,
	/* Mask Asymmetric Namespace Access Change Notification */
	SPDK_NVME_ASYNC_EVENT_ANA_CHANGE_MASK_BIT		= 1,
	/* Mask Discovery Log Change Notification */
	SPDK_NVME_ASYNC_EVENT_DISCOVERY_LOG_CHANGE_MASK_BIT	= 2,
	/* Mask Reservation Log Page Available Notification */
	SPDK_NVME_ASYNC_EVENT_RESERVATION_LOG_AVAIL_MASK_BIT	= 3,
	/* Mask Error Event */
	SPDK_NVME_ASYNC_EVENT_ERROR_MASK_BIT			= 4,
	/* 4 - 63 Reserved */
};

static inline void
nvmf_ctrlr_unmask_aen(struct spdk_nvmf_ctrlr *ctrlr,
		      enum spdk_nvme_async_event_mask_bit mask)
{
	ctrlr->notice_aen_mask &= ~(1 << mask);
}

static inline bool
nvmf_ctrlr_mask_aen(struct spdk_nvmf_ctrlr *ctrlr,
		    enum spdk_nvme_async_event_mask_bit mask)
{
	if (ctrlr->notice_aen_mask & (1 << mask)) {
		return false;
	} else {
		ctrlr->notice_aen_mask |= (1 << mask);
		return true;
	}
}

/* we have to use the typedef in the function declaration to appease astyle. */
typedef enum spdk_nvme_ana_state spdk_nvme_ana_state_t;

static inline spdk_nvme_ana_state_t
nvmf_ctrlr_get_ana_state(struct spdk_nvmf_ctrlr *ctrlr, uint32_t anagrpid)
{
	if (!ctrlr->subsys->flags.ana_reporting) {
		return SPDK_NVME_ANA_OPTIMIZED_STATE;
	}

	if (spdk_unlikely(ctrlr->listener == NULL)) {
		return SPDK_NVME_ANA_INACCESSIBLE_STATE;
	}

	assert(anagrpid - 1 < ctrlr->subsys->max_nsid);
	return ctrlr->listener->ana_state[anagrpid - 1];
}

static spdk_nvme_ana_state_t
nvmf_ctrlr_get_ana_state_from_nsid(struct spdk_nvmf_ctrlr *ctrlr, uint32_t nsid)
{
	struct spdk_nvmf_ns *ns;

	/* We do not have NVM subsystem specific ANA state. Hence if NSID is either
	 * SPDK_NVMF_GLOBAL_NS_TAG, invalid, or for inactive namespace, return
	 * the optimized state.
	 */
	ns = _nvmf_subsystem_get_ns(ctrlr->subsys, nsid);
	if (ns == NULL) {
		return SPDK_NVME_ANA_OPTIMIZED_STATE;
	}

	return nvmf_ctrlr_get_ana_state(ctrlr, ns->anagrpid);
}

static void
nvmf_get_error_log_page(struct spdk_nvmf_ctrlr *ctrlr, struct iovec *iovs, int iovcnt,
			uint64_t offset, uint32_t length, uint32_t rae)
{
	if (!rae) {
		nvmf_ctrlr_unmask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_ERROR_MASK_BIT);
	}

	/* TODO: actually fill out log page data */
}

static void
nvmf_get_ana_log_page(struct spdk_nvmf_ctrlr *ctrlr, struct iovec *iovs, int iovcnt,
		      uint64_t offset, uint32_t length, uint32_t rae)
{
	struct spdk_nvme_ana_page ana_hdr;
	struct spdk_nvme_ana_group_descriptor ana_desc;
	size_t copy_len, copied_len;
	uint32_t num_anagrp = 0, anagrpid;
	struct spdk_nvmf_ns *ns;
	struct copy_iovs_ctx copy_ctx;

	_init_copy_iovs_ctx(&copy_ctx, iovs, iovcnt);

	if (length == 0) {
		goto done;
	}

	if (offset >= sizeof(ana_hdr)) {
		offset -= sizeof(ana_hdr);
	} else {
		for (anagrpid = 1; anagrpid <= ctrlr->subsys->max_nsid; anagrpid++) {
			if (ctrlr->subsys->ana_group[anagrpid - 1] > 0) {
				num_anagrp++;
			}
		}

		memset(&ana_hdr, 0, sizeof(ana_hdr));

		ana_hdr.num_ana_group_desc = num_anagrp;
		/* TODO: Support Change Count. */
		ana_hdr.change_count = 0;

		copy_len = spdk_min(sizeof(ana_hdr) - offset, length);
		copied_len = _copy_buf_to_iovs(&copy_ctx, (const char *)&ana_hdr + offset, copy_len);
		assert(copied_len == copy_len);
		length -= copied_len;
		offset = 0;
	}

	if (length == 0) {
		goto done;
	}

	for (anagrpid = 1; anagrpid <= ctrlr->subsys->max_nsid; anagrpid++) {
		if (ctrlr->subsys->ana_group[anagrpid - 1] == 0) {
			continue;
		}

		if (offset >= sizeof(ana_desc)) {
			offset -= sizeof(ana_desc);
		} else {
			memset(&ana_desc, 0, sizeof(ana_desc));

			ana_desc.ana_group_id = anagrpid;
			ana_desc.num_of_nsid = ctrlr->subsys->ana_group[anagrpid - 1];
			ana_desc.ana_state = nvmf_ctrlr_get_ana_state(ctrlr, anagrpid);

			copy_len = spdk_min(sizeof(ana_desc) - offset, length);
			copied_len = _copy_buf_to_iovs(&copy_ctx, (const char *)&ana_desc + offset,
						       copy_len);
			assert(copied_len == copy_len);
			length -= copied_len;
			offset = 0;

			if (length == 0) {
				goto done;
			}
		}

		/* TODO: Revisit here about O(n^2) cost if we have subsystem with
		 * many namespaces in the future.
		 */
		for (ns = spdk_nvmf_subsystem_get_first_ns(ctrlr->subsys); ns != NULL;
		     ns = spdk_nvmf_subsystem_get_next_ns(ctrlr->subsys, ns)) {
			if (ns->anagrpid != anagrpid) {
				continue;
			}

			if (offset >= sizeof(uint32_t)) {
				offset -= sizeof(uint32_t);
				continue;
			}

			copy_len = spdk_min(sizeof(uint32_t) - offset, length);
			copied_len = _copy_buf_to_iovs(&copy_ctx, (const char *)&ns->nsid + offset,
						       copy_len);
			assert(copied_len == copy_len);
			length -= copied_len;
			offset = 0;

			if (length == 0) {
				goto done;
			}
		}
	}

done:
	if (!rae) {
		nvmf_ctrlr_unmask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_ANA_CHANGE_MASK_BIT);
	}
}

void
nvmf_ctrlr_ns_changed(struct spdk_nvmf_ctrlr *ctrlr, uint32_t nsid)
{
	uint16_t max_changes = SPDK_COUNTOF(ctrlr->changed_ns_list.ns_list);
	uint16_t i;
	bool found = false;

	for (i = 0; i < ctrlr->changed_ns_list_count; i++) {
		if (ctrlr->changed_ns_list.ns_list[i] == nsid) {
			/* nsid is already in the list */
			found = true;
			break;
		}
	}

	if (!found) {
		if (ctrlr->changed_ns_list_count == max_changes) {
			/* Out of space - set first entry to FFFFFFFFh and zero-fill the rest. */
			ctrlr->changed_ns_list.ns_list[0] = 0xFFFFFFFFu;
			for (i = 1; i < max_changes; i++) {
				ctrlr->changed_ns_list.ns_list[i] = 0;
			}
		} else {
			ctrlr->changed_ns_list.ns_list[ctrlr->changed_ns_list_count++] = nsid;
		}
	}
}

static void
nvmf_get_changed_ns_list_log_page(struct spdk_nvmf_ctrlr *ctrlr,
				  struct iovec *iovs, int iovcnt, uint64_t offset, uint32_t length, uint32_t rae)
{
	size_t copy_length;
	struct copy_iovs_ctx copy_ctx;

	_init_copy_iovs_ctx(&copy_ctx, iovs, iovcnt);

	if (offset < sizeof(ctrlr->changed_ns_list)) {
		copy_length = spdk_min(length, sizeof(ctrlr->changed_ns_list) - offset);
		if (copy_length) {
			_copy_buf_to_iovs(&copy_ctx, (char *)&ctrlr->changed_ns_list + offset, copy_length);
		}
	}

	/* Clear log page each time it is read */
	ctrlr->changed_ns_list_count = 0;
	memset(&ctrlr->changed_ns_list, 0, sizeof(ctrlr->changed_ns_list));

	if (!rae) {
		nvmf_ctrlr_unmask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGE_MASK_BIT);
	}
}

/* The structure can be modified if we provide support for other commands in future */
static const struct spdk_nvme_cmds_and_effect_log_page g_cmds_and_effect_log_page = {
	.admin_cmds_supported = {
		/* CSUPP, LBCC, NCC, NIC, CCC, CSE */
		/* Get Log Page */
		[SPDK_NVME_OPC_GET_LOG_PAGE]		= {1, 0, 0, 0, 0, 0, 0, 0},
		/* Identify */
		[SPDK_NVME_OPC_IDENTIFY]		= {1, 0, 0, 0, 0, 0, 0, 0},
		/* Abort */
		[SPDK_NVME_OPC_ABORT]			= {1, 0, 0, 0, 0, 0, 0, 0},
		/* Set Features */
		[SPDK_NVME_OPC_SET_FEATURES]		= {1, 0, 0, 0, 0, 0, 0, 0},
		/* Get Features */
		[SPDK_NVME_OPC_GET_FEATURES]		= {1, 0, 0, 0, 0, 0, 0, 0},
		/* Async Event Request */
		[SPDK_NVME_OPC_ASYNC_EVENT_REQUEST]	= {1, 0, 0, 0, 0, 0, 0, 0},
		/* Keep Alive */
		[SPDK_NVME_OPC_KEEP_ALIVE]		= {1, 0, 0, 0, 0, 0, 0, 0},
	},
	.io_cmds_supported = {
		/* FLUSH */
		[SPDK_NVME_OPC_FLUSH]			= {1, 1, 0, 0, 0, 0, 0, 0},
		/* WRITE */
		[SPDK_NVME_OPC_WRITE]			= {1, 1, 0, 0, 0, 0, 0, 0},
		/* READ */
		[SPDK_NVME_OPC_READ]			= {1, 0, 0, 0, 0, 0, 0, 0},
		/* WRITE ZEROES */
		[SPDK_NVME_OPC_WRITE_ZEROES]		= {1, 1, 0, 0, 0, 0, 0, 0},
		/* DATASET MANAGEMENT */
		[SPDK_NVME_OPC_DATASET_MANAGEMENT]	= {1, 1, 0, 0, 0, 0, 0, 0},
		/* COMPARE */
		[SPDK_NVME_OPC_COMPARE]			= {1, 0, 0, 0, 0, 0, 0, 0},
	},
};

static void
nvmf_get_cmds_and_effects_log_page(struct iovec *iovs, int iovcnt,
				   uint64_t offset, uint32_t length)
{
	uint32_t page_size = sizeof(struct spdk_nvme_cmds_and_effect_log_page);
	size_t copy_len = 0;
	struct copy_iovs_ctx copy_ctx;

	_init_copy_iovs_ctx(&copy_ctx, iovs, iovcnt);

	if (offset < page_size) {
		copy_len = spdk_min(page_size - offset, length);
		_copy_buf_to_iovs(&copy_ctx, (char *)(&g_cmds_and_effect_log_page) + offset, copy_len);
	}
}

static void
nvmf_get_reservation_notification_log_page(struct spdk_nvmf_ctrlr *ctrlr,
		struct iovec *iovs, int iovcnt, uint64_t offset, uint32_t length, uint32_t rae)
{
	uint32_t unit_log_len, avail_log_len, next_pos, copy_len;
	struct spdk_nvmf_reservation_log *log, *log_tmp;
	struct copy_iovs_ctx copy_ctx;

	_init_copy_iovs_ctx(&copy_ctx, iovs, iovcnt);

	unit_log_len = sizeof(struct spdk_nvme_reservation_notification_log);
	/* No available log, return zeroed log pages */
	if (!ctrlr->num_avail_log_pages) {
		return;
	}

	avail_log_len = ctrlr->num_avail_log_pages * unit_log_len;
	if (offset >= avail_log_len) {
		return;
	}

	next_pos = 0;
	TAILQ_FOREACH_SAFE(log, &ctrlr->log_head, link, log_tmp) {
		TAILQ_REMOVE(&ctrlr->log_head, log, link);
		ctrlr->num_avail_log_pages--;

		next_pos += unit_log_len;
		if (next_pos > offset) {
			copy_len = spdk_min(next_pos - offset, length);
			_copy_buf_to_iovs(&copy_ctx, &log->log, copy_len);
			length -= copy_len;
			offset += copy_len;
		}
		free(log);

		if (length == 0) {
			break;
		}
	}

	if (!rae) {
		nvmf_ctrlr_unmask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_RESERVATION_LOG_AVAIL_MASK_BIT);
	}
	return;
}

static int
nvmf_ctrlr_get_log_page(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_nvme_transport_id cmd_source_trid;
	uint64_t offset, len;
	uint32_t rae, numdl, numdu;
	uint8_t lid;

	if (req->data == NULL) {
		SPDK_DEBUGLOG(nvmf, "get log command with no buffer\n");
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	offset = (uint64_t)cmd->cdw12 | ((uint64_t)cmd->cdw13 << 32);
	if (offset & 3) {
		SPDK_ERRLOG("Invalid log page offset 0x%" PRIx64 "\n", offset);
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rae = cmd->cdw10_bits.get_log_page.rae;
	numdl = cmd->cdw10_bits.get_log_page.numdl;
	numdu = cmd->cdw11_bits.get_log_page.numdu;
	len = ((numdu << 16) + numdl + (uint64_t)1) * 4;
	if (len > req->length) {
		SPDK_ERRLOG("Get log page: len (%" PRIu64 ") > buf size (%u)\n",
			    len, req->length);
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	lid = cmd->cdw10_bits.get_log_page.lid;
	SPDK_DEBUGLOG(nvmf, "Get log page: LID=0x%02X offset=0x%" PRIx64 " len=0x%" PRIx64 " rae=%u\n",
		      lid, offset, len, rae);

	if (subsystem->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY) {
		switch (lid) {
		case SPDK_NVME_LOG_DISCOVERY:
			if (spdk_nvmf_qpair_get_listen_trid(req->qpair, &cmd_source_trid)) {
				SPDK_ERRLOG("Failed to get LOG_DISCOVERY source trid\n");
				response->status.sct = SPDK_NVME_SCT_GENERIC;
				response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
			}
			nvmf_get_discovery_log_page(subsystem->tgt, ctrlr->hostnqn, req->iov, req->iovcnt,
						    offset, len, &cmd_source_trid);
			if (!rae) {
				nvmf_ctrlr_unmask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_DISCOVERY_LOG_CHANGE_MASK_BIT);
			}
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		default:
			goto invalid_log_page;
		}
	} else {
		if (offset > len) {
			SPDK_ERRLOG("Get log page: offset (%" PRIu64 ") > len (%" PRIu64 ")\n",
				    offset, len);
			response->status.sct = SPDK_NVME_SCT_GENERIC;
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		switch (lid) {
		case SPDK_NVME_LOG_ERROR:
			nvmf_get_error_log_page(ctrlr, req->iov, req->iovcnt, offset, len, rae);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		case SPDK_NVME_LOG_HEALTH_INFORMATION:
			/* TODO: actually fill out log page data */
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		case SPDK_NVME_LOG_FIRMWARE_SLOT:
			nvmf_get_firmware_slot_log_page(req->iov, req->iovcnt, offset, len);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		case SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS:
			if (subsystem->flags.ana_reporting) {
				nvmf_get_ana_log_page(ctrlr, req->iov, req->iovcnt, offset, len, rae);
				return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
			} else {
				goto invalid_log_page;
			}
		case SPDK_NVME_LOG_COMMAND_EFFECTS_LOG:
			nvmf_get_cmds_and_effects_log_page(req->iov, req->iovcnt, offset, len);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		case SPDK_NVME_LOG_CHANGED_NS_LIST:
			nvmf_get_changed_ns_list_log_page(ctrlr, req->iov, req->iovcnt, offset, len, rae);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		case SPDK_NVME_LOG_RESERVATION_NOTIFICATION:
			nvmf_get_reservation_notification_log_page(ctrlr, req->iov, req->iovcnt, offset, len, rae);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		default:
			goto invalid_log_page;
		}
	}

invalid_log_page:
	SPDK_INFOLOG(nvmf, "Unsupported Get Log Page 0x%02X\n", lid);
	response->status.sct = SPDK_NVME_SCT_GENERIC;
	response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_ctrlr_identify_ns(struct spdk_nvmf_ctrlr *ctrlr,
			    struct spdk_nvme_cmd *cmd,
			    struct spdk_nvme_cpl *rsp,
			    struct spdk_nvme_ns_data *nsdata)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvmf_ns *ns;
	uint32_t max_num_blocks;
	enum spdk_nvme_ana_state ana_state;

	if (cmd->nsid == 0 || cmd->nsid > subsystem->max_nsid) {
		SPDK_ERRLOG("Identify Namespace for invalid NSID %u\n", cmd->nsid);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ns = _nvmf_subsystem_get_ns(subsystem, cmd->nsid);
	if (ns == NULL || ns->bdev == NULL) {
		/*
		 * Inactive namespaces should return a zero filled data structure.
		 * The data buffer is already zeroed by nvmf_ctrlr_process_admin_cmd(),
		 * so we can just return early here.
		 */
		SPDK_DEBUGLOG(nvmf, "Identify Namespace for inactive NSID %u\n", cmd->nsid);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_SUCCESS;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	nvmf_bdev_ctrlr_identify_ns(ns, nsdata, ctrlr->dif_insert_or_strip);

	assert(ctrlr->admin_qpair);
	/* Due to bug in the Linux kernel NVMe driver we have to set noiob no larger than mdts */
	max_num_blocks = ctrlr->admin_qpair->transport->opts.max_io_size /
			 (1U << nsdata->lbaf[nsdata->flbas.format].lbads);
	if (nsdata->noiob > max_num_blocks) {
		nsdata->noiob = max_num_blocks;
	}

	/* Set NOWS equal to Controller MDTS */
	if (nsdata->nsfeat.optperf) {
		nsdata->nows = max_num_blocks - 1;
	}

	if (subsystem->flags.ana_reporting) {
		assert(ns->anagrpid - 1 < subsystem->max_nsid);
		nsdata->anagrpid = ns->anagrpid;

		ana_state = nvmf_ctrlr_get_ana_state(ctrlr, ns->anagrpid);
		if (ana_state == SPDK_NVME_ANA_INACCESSIBLE_STATE ||
		    ana_state == SPDK_NVME_ANA_PERSISTENT_LOSS_STATE) {
			nsdata->nuse = 0;
		}
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static void
nvmf_ctrlr_populate_oacs(struct spdk_nvmf_ctrlr *ctrlr,
			 struct spdk_nvme_ctrlr_data *cdata)
{
	cdata->oacs.virtualization_management =
		g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_VIRTUALIZATION_MANAGEMENT].hdlr != NULL;
	cdata->oacs.nvme_mi = g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_NVME_MI_SEND].hdlr != NULL
			      && g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_NVME_MI_RECEIVE].hdlr != NULL;
	cdata->oacs.directives = g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_DIRECTIVE_SEND].hdlr != NULL
				 && g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_DIRECTIVE_RECEIVE].hdlr != NULL;
	cdata->oacs.device_self_test =
		g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_DEVICE_SELF_TEST].hdlr != NULL;
	cdata->oacs.ns_manage = g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_NS_MANAGEMENT].hdlr != NULL
				&& g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_NS_ATTACHMENT].hdlr != NULL;
	cdata->oacs.firmware = g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD].hdlr !=
			       NULL
			       && g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_FIRMWARE_COMMIT].hdlr != NULL;
	cdata->oacs.format =
		g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_FORMAT_NVM].hdlr != NULL;
	cdata->oacs.security = g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_SECURITY_SEND].hdlr != NULL
			       && g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_SECURITY_RECEIVE].hdlr != NULL;
	cdata->oacs.get_lba_status = g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_GET_LBA_STATUS].hdlr !=
				     NULL;
}

int
spdk_nvmf_ctrlr_identify_ctrlr(struct spdk_nvmf_ctrlr *ctrlr, struct spdk_nvme_ctrlr_data *cdata)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvmf_transport *transport;

	/*
	 * Common fields for discovery and NVM subsystems
	 */
	assert(ctrlr->admin_qpair);
	transport = ctrlr->admin_qpair->transport;
	spdk_strcpy_pad(cdata->fr, FW_VERSION, sizeof(cdata->fr), ' ');
	assert((transport->opts.max_io_size % 4096) == 0);
	cdata->mdts = spdk_u32log2(transport->opts.max_io_size / 4096);
	cdata->cntlid = ctrlr->cntlid;
	cdata->ver = ctrlr->vcprop.vs;
	cdata->aerl = ctrlr->cdata.aerl;
	cdata->lpa.edlp = 1;
	cdata->elpe = 127;
	cdata->maxcmd = transport->opts.max_queue_depth;
	cdata->sgls = ctrlr->cdata.sgls;
	cdata->fuses.compare_and_write = 1;
	cdata->acwu = 0; /* ACWU is 0-based. */
	if (subsystem->flags.ana_reporting) {
		cdata->mnan = subsystem->max_nsid;
	}
	spdk_strcpy_pad(cdata->subnqn, subsystem->subnqn, sizeof(cdata->subnqn), '\0');

	SPDK_DEBUGLOG(nvmf, "ctrlr data: maxcmd 0x%x\n", cdata->maxcmd);
	SPDK_DEBUGLOG(nvmf, "sgls data: 0x%x\n", from_le32(&cdata->sgls));


	if (subsystem->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY) {
		/*
		 * NVM Discovery subsystem fields
		 */
		cdata->oaes.discovery_log_change_notices = 1;
	} else {
		cdata->vid = ctrlr->cdata.vid;
		cdata->ssvid = ctrlr->cdata.ssvid;
		cdata->ieee[0] = ctrlr->cdata.ieee[0];
		cdata->ieee[1] = ctrlr->cdata.ieee[1];
		cdata->ieee[2] = ctrlr->cdata.ieee[2];

		/*
		 * NVM subsystem fields (reserved for discovery subsystems)
		 */
		spdk_strcpy_pad(cdata->mn, spdk_nvmf_subsystem_get_mn(subsystem), sizeof(cdata->mn), ' ');
		spdk_strcpy_pad(cdata->sn, spdk_nvmf_subsystem_get_sn(subsystem), sizeof(cdata->sn), ' ');
		cdata->kas = ctrlr->cdata.kas;

		cdata->rab = 6;
		cdata->cmic.multi_port = 1;
		cdata->cmic.multi_ctrlr = 1;
		if (subsystem->flags.ana_reporting) {
			/* Asymmetric Namespace Access Reporting is supported. */
			cdata->cmic.ana_reporting = 1;
		}
		cdata->oaes.ns_attribute_notices = 1;
		if (subsystem->flags.ana_reporting) {
			cdata->oaes.ana_change_notices = 1;
		}
		cdata->ctratt.host_id_exhid_supported = 1;
		/* TODO: Concurrent execution of multiple abort commands. */
		cdata->acl = 0;
		cdata->frmw.slot1_ro = 1;
		cdata->frmw.num_slots = 1;

		cdata->lpa.celp = 1; /* Command Effects log page supported */

		cdata->sqes.min = 6;
		cdata->sqes.max = 6;
		cdata->cqes.min = 4;
		cdata->cqes.max = 4;
		cdata->nn = subsystem->max_nsid;
		cdata->vwc.present = 1;
		cdata->vwc.flush_broadcast = SPDK_NVME_FLUSH_BROADCAST_NOT_SUPPORTED;

		cdata->nvmf_specific = ctrlr->cdata.nvmf_specific;

		cdata->oncs.dsm = nvmf_ctrlr_dsm_supported(ctrlr);
		cdata->oncs.write_zeroes = nvmf_ctrlr_write_zeroes_supported(ctrlr);
		cdata->oncs.reservations = ctrlr->cdata.oncs.reservations;
		if (subsystem->flags.ana_reporting) {
			cdata->anatt = ANA_TRANSITION_TIME_IN_SEC;
			/* ANA Change state is not used, and ANA Persistent Loss state
			 * is not supported for now.
			 */
			cdata->anacap.ana_optimized_state = 1;
			cdata->anacap.ana_non_optimized_state = 1;
			cdata->anacap.ana_inaccessible_state = 1;
			/* ANAGRPID does not change while namespace is attached to controller */
			cdata->anacap.no_change_anagrpid = 1;
			cdata->anagrpmax = subsystem->max_nsid;
			cdata->nanagrpid = subsystem->max_nsid;
		}

		nvmf_ctrlr_populate_oacs(ctrlr, cdata);

		assert(subsystem->tgt != NULL);
		cdata->crdt[0] = subsystem->tgt->crdt[0];
		cdata->crdt[1] = subsystem->tgt->crdt[1];
		cdata->crdt[2] = subsystem->tgt->crdt[2];

		SPDK_DEBUGLOG(nvmf, "ext ctrlr data: ioccsz 0x%x\n",
			      cdata->nvmf_specific.ioccsz);
		SPDK_DEBUGLOG(nvmf, "ext ctrlr data: iorcsz 0x%x\n",
			      cdata->nvmf_specific.iorcsz);
		SPDK_DEBUGLOG(nvmf, "ext ctrlr data: icdoff 0x%x\n",
			      cdata->nvmf_specific.icdoff);
		SPDK_DEBUGLOG(nvmf, "ext ctrlr data: ctrattr 0x%x\n",
			      *(uint8_t *)&cdata->nvmf_specific.ctrattr);
		SPDK_DEBUGLOG(nvmf, "ext ctrlr data: msdbd 0x%x\n",
			      cdata->nvmf_specific.msdbd);
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_identify_active_ns_list(struct spdk_nvmf_subsystem *subsystem,
				   struct spdk_nvme_cmd *cmd,
				   struct spdk_nvme_cpl *rsp,
				   struct spdk_nvme_ns_list *ns_list)
{
	struct spdk_nvmf_ns *ns;
	uint32_t count = 0;

	if (cmd->nsid >= 0xfffffffeUL) {
		SPDK_ERRLOG("Identify Active Namespace List with invalid NSID %u\n", cmd->nsid);
		rsp->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	memset(ns_list, 0, sizeof(*ns_list));

	for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
		if (ns->opts.nsid <= cmd->nsid) {
			continue;
		}

		ns_list->ns_list[count++] = ns->opts.nsid;
		if (count == SPDK_COUNTOF(ns_list->ns_list)) {
			break;
		}
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static void
_add_ns_id_desc(void **buf_ptr, size_t *buf_remain,
		enum spdk_nvme_nidt type,
		const void *data, size_t data_size)
{
	struct spdk_nvme_ns_id_desc *desc;
	size_t desc_size = sizeof(*desc) + data_size;

	/*
	 * These should never fail in practice, since all valid NS ID descriptors
	 * should be defined so that they fit in the available 4096-byte buffer.
	 */
	assert(data_size > 0);
	assert(data_size <= UINT8_MAX);
	assert(desc_size < *buf_remain);
	if (data_size == 0 || data_size > UINT8_MAX || desc_size > *buf_remain) {
		return;
	}

	desc = *buf_ptr;
	desc->nidt = type;
	desc->nidl = data_size;
	memcpy(desc->nid, data, data_size);

	*buf_ptr += desc_size;
	*buf_remain -= desc_size;
}

static int
nvmf_ctrlr_identify_ns_id_descriptor_list(
	struct spdk_nvmf_subsystem *subsystem,
	struct spdk_nvme_cmd *cmd,
	struct spdk_nvme_cpl *rsp,
	void *id_desc_list, size_t id_desc_list_size)
{
	struct spdk_nvmf_ns *ns;
	size_t buf_remain = id_desc_list_size;
	void *buf_ptr = id_desc_list;

	ns = _nvmf_subsystem_get_ns(subsystem, cmd->nsid);
	if (ns == NULL || ns->bdev == NULL) {
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

#define ADD_ID_DESC(type, data, size) \
	do { \
		if (!spdk_mem_all_zero(data, size)) { \
			_add_ns_id_desc(&buf_ptr, &buf_remain, type, data, size); \
		} \
	} while (0)

	ADD_ID_DESC(SPDK_NVME_NIDT_EUI64, ns->opts.eui64, sizeof(ns->opts.eui64));
	ADD_ID_DESC(SPDK_NVME_NIDT_NGUID, ns->opts.nguid, sizeof(ns->opts.nguid));
	ADD_ID_DESC(SPDK_NVME_NIDT_UUID, &ns->opts.uuid, sizeof(ns->opts.uuid));

	/*
	 * The list is automatically 0-terminated because controller to host buffers in
	 * admin commands always get zeroed in nvmf_ctrlr_process_admin_cmd().
	 */

#undef ADD_ID_DESC

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_identify(struct spdk_nvmf_request *req)
{
	uint8_t cns;
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;

	if (req->data == NULL || req->length < 4096) {
		SPDK_DEBUGLOG(nvmf, "identify command with invalid buffer\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	cns = cmd->cdw10_bits.identify.cns;

	if (subsystem->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY &&
	    cns != SPDK_NVME_IDENTIFY_CTRLR) {
		/* Discovery controllers only support Identify Controller */
		goto invalid_cns;
	}

	switch (cns) {
	case SPDK_NVME_IDENTIFY_NS:
		return spdk_nvmf_ctrlr_identify_ns(ctrlr, cmd, rsp, req->data);
	case SPDK_NVME_IDENTIFY_CTRLR:
		return spdk_nvmf_ctrlr_identify_ctrlr(ctrlr, req->data);
	case SPDK_NVME_IDENTIFY_ACTIVE_NS_LIST:
		return nvmf_ctrlr_identify_active_ns_list(subsystem, cmd, rsp, req->data);
	case SPDK_NVME_IDENTIFY_NS_ID_DESCRIPTOR_LIST:
		return nvmf_ctrlr_identify_ns_id_descriptor_list(subsystem, cmd, rsp, req->data, req->length);
	default:
		goto invalid_cns;
	}

invalid_cns:
	SPDK_INFOLOG(nvmf, "Identify command with unsupported CNS 0x%02x\n", cns);
	rsp->status.sct = SPDK_NVME_SCT_GENERIC;
	rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static bool
nvmf_qpair_abort_aer(struct spdk_nvmf_qpair *qpair, uint16_t cid)
{
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvmf_request *req;
	int i;

	if (!nvmf_qpair_is_admin_queue(qpair)) {
		return false;
	}

	for (i = 0; i < ctrlr->nr_aer_reqs; i++) {
		if (ctrlr->aer_req[i]->cmd->nvme_cmd.cid == cid) {
			SPDK_DEBUGLOG(nvmf, "Aborting AER request\n");
			req = ctrlr->aer_req[i];
			ctrlr->aer_req[i] = NULL;
			ctrlr->nr_aer_reqs--;

			/* Move the last req to the aborting position for making aer_reqs
			 * in continuous
			 */
			if (i < ctrlr->nr_aer_reqs) {
				ctrlr->aer_req[i] = ctrlr->aer_req[ctrlr->nr_aer_reqs];
				ctrlr->aer_req[ctrlr->nr_aer_reqs] = NULL;
			}

			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
			_nvmf_request_complete(req);
			return true;
		}
	}

	return false;
}

void
nvmf_qpair_abort_pending_zcopy_reqs(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_request *req, *tmp;

	TAILQ_FOREACH_SAFE(req, &qpair->outstanding, link, tmp) {
		if (req->zcopy_phase == NVMF_ZCOPY_PHASE_EXECUTE) {
			/* Zero-copy requests are kept on the outstanding queue from the moment
			 * zcopy_start is sent until a zcopy_end callback is received.  Therefore,
			 * we can't remove them from the outstanding queue here, but need to rely on
			 * the transport to do a zcopy_end to release their buffers and, in turn,
			 * remove them from the queue.
			 */
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
			nvmf_transport_req_free(req);
		}
	}
}

static void
nvmf_qpair_abort_request(struct spdk_nvmf_qpair *qpair, struct spdk_nvmf_request *req)
{
	uint16_t cid = req->cmd->nvme_cmd.cdw10_bits.abort.cid;

	if (nvmf_qpair_abort_aer(qpair, cid)) {
		SPDK_DEBUGLOG(nvmf, "abort ctrlr=%p sqid=%u cid=%u successful\n",
			      qpair->ctrlr, qpair->qid, cid);
		req->rsp->nvme_cpl.cdw0 &= ~1U; /* Command successfully aborted */

		spdk_nvmf_request_complete(req);
		return;
	}

	nvmf_transport_qpair_abort_request(qpair, req);
}

static void
nvmf_ctrlr_abort_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvmf_request *req = spdk_io_channel_iter_get_ctx(i);

	if (status == 0) {
		/* There was no qpair whose ID matches SQID of the abort command.
		 * Hence call _nvmf_request_complete() here.
		 */
		_nvmf_request_complete(req);
	}
}

static void
nvmf_ctrlr_abort_on_pg(struct spdk_io_channel_iter *i)
{
	struct spdk_nvmf_request *req = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_nvmf_poll_group *group = spdk_io_channel_get_ctx(ch);
	uint16_t sqid = req->cmd->nvme_cmd.cdw10_bits.abort.sqid;
	struct spdk_nvmf_qpair *qpair;

	TAILQ_FOREACH(qpair, &group->qpairs, link) {
		if (qpair->ctrlr == req->qpair->ctrlr && qpair->qid == sqid) {
			/* Found the qpair */

			nvmf_qpair_abort_request(qpair, req);

			/* Return -1 for the status so the iteration across threads stops. */
			spdk_for_each_channel_continue(i, -1);
			return;
		}
	}

	spdk_for_each_channel_continue(i, 0);
}

static int
nvmf_ctrlr_abort(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	rsp->cdw0 = 1U; /* Command not aborted */
	rsp->status.sct = SPDK_NVME_SCT_GENERIC;
	rsp->status.sc = SPDK_NVME_SC_SUCCESS;

	/* Send a message to each poll group, searching for this ctrlr, sqid, and command. */
	spdk_for_each_channel(req->qpair->ctrlr->subsys->tgt,
			      nvmf_ctrlr_abort_on_pg,
			      req,
			      nvmf_ctrlr_abort_done
			     );

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_ctrlr_abort_request(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_request *req_to_abort = req->req_to_abort;
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	int rc;

	assert(req_to_abort != NULL);

	if (g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_ABORT].hdlr &&
	    nvmf_qpair_is_admin_queue(req_to_abort->qpair)) {
		return g_nvmf_custom_admin_cmd_hdlrs[SPDK_NVME_OPC_ABORT].hdlr(req);
	}

	rc = spdk_nvmf_request_get_bdev(req_to_abort->cmd->nvme_cmd.nsid, req_to_abort,
					&bdev, &desc, &ch);
	if (rc != 0) {
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return spdk_nvmf_bdev_ctrlr_abort_cmd(bdev, desc, ch, req, req_to_abort);
}

static int
get_features_generic(struct spdk_nvmf_request *req, uint32_t cdw0)
{
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	rsp->cdw0 = cdw0;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

/* we have to use the typedef in the function declaration to appease astyle. */
typedef enum spdk_nvme_path_status_code spdk_nvme_path_status_code_t;

static spdk_nvme_path_status_code_t
_nvme_ana_state_to_path_status(enum spdk_nvme_ana_state ana_state)
{
	switch (ana_state) {
	case SPDK_NVME_ANA_INACCESSIBLE_STATE:
		return SPDK_NVME_SC_ASYMMETRIC_ACCESS_INACCESSIBLE;
	case SPDK_NVME_ANA_PERSISTENT_LOSS_STATE:
		return SPDK_NVME_SC_ASYMMETRIC_ACCESS_PERSISTENT_LOSS;
	case SPDK_NVME_ANA_CHANGE_STATE:
		return SPDK_NVME_SC_ASYMMETRIC_ACCESS_TRANSITION;
	default:
		return SPDK_NVME_SC_INTERNAL_PATH_ERROR;
	}
}

static int
nvmf_ctrlr_get_features(struct spdk_nvmf_request *req)
{
	uint8_t feature;
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	enum spdk_nvme_ana_state ana_state;

	feature = cmd->cdw10_bits.get_features.fid;

	if (ctrlr->subsys->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY) {
		/*
		 * Features supported by Discovery controller
		 */
		switch (feature) {
		case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
			return get_features_generic(req, ctrlr->feat.keep_alive_timer.raw);
		case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
			return get_features_generic(req, ctrlr->feat.async_event_configuration.raw);
		default:
			SPDK_INFOLOG(nvmf, "Get Features command with unsupported feature ID 0x%02x\n", feature);
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	}
	/*
	 * Process Get Features command for non-discovery controller
	 */
	ana_state = nvmf_ctrlr_get_ana_state_from_nsid(ctrlr, cmd->nsid);
	switch (ana_state) {
	case SPDK_NVME_ANA_INACCESSIBLE_STATE:
	case SPDK_NVME_ANA_PERSISTENT_LOSS_STATE:
	case SPDK_NVME_ANA_CHANGE_STATE:
		switch (feature) {
		case SPDK_NVME_FEAT_ERROR_RECOVERY:
		case SPDK_NVME_FEAT_WRITE_ATOMICITY:
		case SPDK_NVME_FEAT_HOST_RESERVE_MASK:
		case SPDK_NVME_FEAT_HOST_RESERVE_PERSIST:
			response->status.sct = SPDK_NVME_SCT_PATH;
			response->status.sc = _nvme_ana_state_to_path_status(ana_state);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		default:
			break;
		}
		break;
	default:
		break;
	}

	switch (feature) {
	case SPDK_NVME_FEAT_ARBITRATION:
		return get_features_generic(req, ctrlr->feat.arbitration.raw);
	case SPDK_NVME_FEAT_POWER_MANAGEMENT:
		return get_features_generic(req, ctrlr->feat.power_management.raw);
	case SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD:
		return nvmf_ctrlr_get_features_temperature_threshold(req);
	case SPDK_NVME_FEAT_ERROR_RECOVERY:
		return get_features_generic(req, ctrlr->feat.error_recovery.raw);
	case SPDK_NVME_FEAT_VOLATILE_WRITE_CACHE:
		return get_features_generic(req, ctrlr->feat.volatile_write_cache.raw);
	case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
		return get_features_generic(req, ctrlr->feat.number_of_queues.raw);
	case SPDK_NVME_FEAT_INTERRUPT_COALESCING:
		return get_features_generic(req, ctrlr->feat.interrupt_coalescing.raw);
	case SPDK_NVME_FEAT_INTERRUPT_VECTOR_CONFIGURATION:
		return nvmf_ctrlr_get_features_interrupt_vector_configuration(req);
	case SPDK_NVME_FEAT_WRITE_ATOMICITY:
		return get_features_generic(req, ctrlr->feat.write_atomicity.raw);
	case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
		return get_features_generic(req, ctrlr->feat.async_event_configuration.raw);
	case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
		return get_features_generic(req, ctrlr->feat.keep_alive_timer.raw);
	case SPDK_NVME_FEAT_HOST_IDENTIFIER:
		return nvmf_ctrlr_get_features_host_identifier(req);
	case SPDK_NVME_FEAT_HOST_RESERVE_MASK:
		return nvmf_ctrlr_get_features_reservation_notification_mask(req);
	case SPDK_NVME_FEAT_HOST_RESERVE_PERSIST:
		return nvmf_ctrlr_get_features_reservation_persistence(req);
	case SPDK_NVME_FEAT_HOST_BEHAVIOR_SUPPORT:
		return nvmf_ctrlr_get_features_host_behavior_support(req);
	default:
		SPDK_INFOLOG(nvmf, "Get Features command with unsupported feature ID 0x%02x\n", feature);
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

static int
nvmf_ctrlr_set_features(struct spdk_nvmf_request *req)
{
	uint8_t feature, save;
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	enum spdk_nvme_ana_state ana_state;
	/*
	 * Features are not saveable by the controller as indicated by
	 * ONCS field of the Identify Controller data.
	 * */
	save = cmd->cdw10_bits.set_features.sv;
	if (save) {
		response->status.sc = SPDK_NVME_SC_FEATURE_ID_NOT_SAVEABLE;
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	feature = cmd->cdw10_bits.set_features.fid;

	if (ctrlr->subsys->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY) {
		/*
		 * Features supported by Discovery controller
		 */
		switch (feature) {
		case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
			return nvmf_ctrlr_set_features_keep_alive_timer(req);
		case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
			return nvmf_ctrlr_set_features_async_event_configuration(req);
		default:
			SPDK_INFOLOG(nvmf, "Set Features command with unsupported feature ID 0x%02x\n", feature);
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	}
	/*
	 * Process Set Features command for non-discovery controller
	 */
	ana_state = nvmf_ctrlr_get_ana_state_from_nsid(ctrlr, cmd->nsid);
	switch (ana_state) {
	case SPDK_NVME_ANA_INACCESSIBLE_STATE:
	case SPDK_NVME_ANA_CHANGE_STATE:
		if (cmd->nsid == SPDK_NVME_GLOBAL_NS_TAG) {
			response->status.sct = SPDK_NVME_SCT_PATH;
			response->status.sc = _nvme_ana_state_to_path_status(ana_state);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		} else {
			switch (feature) {
			case SPDK_NVME_FEAT_ERROR_RECOVERY:
			case SPDK_NVME_FEAT_WRITE_ATOMICITY:
			case SPDK_NVME_FEAT_HOST_RESERVE_MASK:
			case SPDK_NVME_FEAT_HOST_RESERVE_PERSIST:
				response->status.sct = SPDK_NVME_SCT_PATH;
				response->status.sc = _nvme_ana_state_to_path_status(ana_state);
				return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
			default:
				break;
			}
		}
		break;
	case SPDK_NVME_ANA_PERSISTENT_LOSS_STATE:
		response->status.sct = SPDK_NVME_SCT_PATH;
		response->status.sc = SPDK_NVME_SC_ASYMMETRIC_ACCESS_PERSISTENT_LOSS;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	default:
		break;
	}

	switch (feature) {
	case SPDK_NVME_FEAT_ARBITRATION:
		return nvmf_ctrlr_set_features_arbitration(req);
	case SPDK_NVME_FEAT_POWER_MANAGEMENT:
		return nvmf_ctrlr_set_features_power_management(req);
	case SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD:
		return nvmf_ctrlr_set_features_temperature_threshold(req);
	case SPDK_NVME_FEAT_ERROR_RECOVERY:
		return nvmf_ctrlr_set_features_error_recovery(req);
	case SPDK_NVME_FEAT_VOLATILE_WRITE_CACHE:
		return nvmf_ctrlr_set_features_volatile_write_cache(req);
	case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
		return nvmf_ctrlr_set_features_number_of_queues(req);
	case SPDK_NVME_FEAT_INTERRUPT_COALESCING:
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVME_SC_FEATURE_NOT_CHANGEABLE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	case SPDK_NVME_FEAT_WRITE_ATOMICITY:
		return nvmf_ctrlr_set_features_write_atomicity(req);
	case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
		return nvmf_ctrlr_set_features_async_event_configuration(req);
	case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
		return nvmf_ctrlr_set_features_keep_alive_timer(req);
	case SPDK_NVME_FEAT_HOST_IDENTIFIER:
		return nvmf_ctrlr_set_features_host_identifier(req);
	case SPDK_NVME_FEAT_HOST_RESERVE_MASK:
		return nvmf_ctrlr_set_features_reservation_notification_mask(req);
	case SPDK_NVME_FEAT_HOST_RESERVE_PERSIST:
		return nvmf_ctrlr_set_features_reservation_persistence(req);
	case SPDK_NVME_FEAT_HOST_BEHAVIOR_SUPPORT:
		return nvmf_ctrlr_set_features_host_behavior_support(req);
	default:
		SPDK_INFOLOG(nvmf, "Set Features command with unsupported feature ID 0x%02x\n", feature);
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

static int
nvmf_ctrlr_keep_alive(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;

	SPDK_DEBUGLOG(nvmf, "Keep Alive\n");
	/*
	 * To handle keep alive just clear or reset the
	 * ctrlr based keep alive duration counter.
	 * When added, a separate timer based process
	 * will monitor if the time since last recorded
	 * keep alive has exceeded the max duration and
	 * take appropriate action.
	 */
	ctrlr->last_keep_alive_tick = spdk_get_ticks();

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
nvmf_ctrlr_process_admin_cmd(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	int rc;

	if (ctrlr == NULL) {
		SPDK_ERRLOG("Admin command sent before CONNECT\n");
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (cmd->fuse != 0) {
		/* Fused admin commands are not supported. */
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (ctrlr->vcprop.cc.bits.en != 1) {
		SPDK_ERRLOG("Admin command sent to disabled controller\n");
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (req->data && spdk_nvme_opc_get_data_transfer(cmd->opc) == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		_clear_iovs(req->iov, req->iovcnt);
	}

	if (ctrlr->subsys->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY) {
		/* Discovery controllers only support these admin OPS. */
		switch (cmd->opc) {
		case SPDK_NVME_OPC_IDENTIFY:
		case SPDK_NVME_OPC_GET_LOG_PAGE:
		case SPDK_NVME_OPC_KEEP_ALIVE:
		case SPDK_NVME_OPC_SET_FEATURES:
		case SPDK_NVME_OPC_GET_FEATURES:
		case SPDK_NVME_OPC_ASYNC_EVENT_REQUEST:
			break;
		default:
			goto invalid_opcode;
		}
	}

	/* Call a custom adm cmd handler if set. Aborts are handled in a different path (see nvmf_passthru_admin_cmd) */
	if (g_nvmf_custom_admin_cmd_hdlrs[cmd->opc].hdlr && cmd->opc != SPDK_NVME_OPC_ABORT) {
		rc = g_nvmf_custom_admin_cmd_hdlrs[cmd->opc].hdlr(req);
		if (rc >= SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
			/* The handler took care of this command */
			return rc;
		}
	}

	switch (cmd->opc) {
	case SPDK_NVME_OPC_GET_LOG_PAGE:
		return nvmf_ctrlr_get_log_page(req);
	case SPDK_NVME_OPC_IDENTIFY:
		return nvmf_ctrlr_identify(req);
	case SPDK_NVME_OPC_ABORT:
		return nvmf_ctrlr_abort(req);
	case SPDK_NVME_OPC_GET_FEATURES:
		return nvmf_ctrlr_get_features(req);
	case SPDK_NVME_OPC_SET_FEATURES:
		return nvmf_ctrlr_set_features(req);
	case SPDK_NVME_OPC_ASYNC_EVENT_REQUEST:
		return nvmf_ctrlr_async_event_request(req);
	case SPDK_NVME_OPC_KEEP_ALIVE:
		return nvmf_ctrlr_keep_alive(req);

	case SPDK_NVME_OPC_CREATE_IO_SQ:
	case SPDK_NVME_OPC_CREATE_IO_CQ:
	case SPDK_NVME_OPC_DELETE_IO_SQ:
	case SPDK_NVME_OPC_DELETE_IO_CQ:
		/* Create and Delete I/O CQ/SQ not allowed in NVMe-oF */
		goto invalid_opcode;

	default:
		goto invalid_opcode;
	}

invalid_opcode:
	SPDK_INFOLOG(nvmf, "Unsupported admin opcode 0x%x\n", cmd->opc);
	response->status.sct = SPDK_NVME_SCT_GENERIC;
	response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ctrlr_process_fabrics_cmd(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_capsule_cmd *cap_hdr;

	cap_hdr = &req->cmd->nvmf_cmd;

	if (qpair->ctrlr == NULL) {
		/* No ctrlr established yet; the only valid command is Connect */
		if (cap_hdr->fctype == SPDK_NVMF_FABRIC_COMMAND_CONNECT) {
			return nvmf_ctrlr_cmd_connect(req);
		} else {
			SPDK_DEBUGLOG(nvmf, "Got fctype 0x%x, expected Connect\n",
				      cap_hdr->fctype);
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	} else if (nvmf_qpair_is_admin_queue(qpair)) {
		/*
		 * Controller session is established, and this is an admin queue.
		 * Disallow Connect and allow other fabrics commands.
		 */
		switch (cap_hdr->fctype) {
		case SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET:
			return nvmf_property_set(req);
		case SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET:
			return nvmf_property_get(req);
		default:
			SPDK_DEBUGLOG(nvmf, "unknown fctype 0x%02x\n",
				      cap_hdr->fctype);
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	} else {
		/* Controller session is established, and this is an I/O queue */
		/* For now, no I/O-specific Fabrics commands are implemented (other than Connect) */
		SPDK_DEBUGLOG(nvmf, "Unexpected I/O fctype 0x%x\n", cap_hdr->fctype);
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

static inline void
nvmf_ctrlr_queue_pending_async_event(struct spdk_nvmf_ctrlr *ctrlr,
				     union spdk_nvme_async_event_completion *event)
{
	struct spdk_nvmf_async_event_completion *nvmf_event;

	nvmf_event = calloc(1, sizeof(*nvmf_event));
	if (!nvmf_event) {
		SPDK_ERRLOG("Alloc nvmf event failed, ignore the event\n");
		return;
	}
	nvmf_event->event.raw = event->raw;
	STAILQ_INSERT_TAIL(&ctrlr->async_events, nvmf_event, link);
}

static inline int
nvmf_ctrlr_async_event_notification(struct spdk_nvmf_ctrlr *ctrlr,
				    union spdk_nvme_async_event_completion *event)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;

	/* If there is no outstanding AER request, queue the event.  Then
	 * if an AER is later submitted, this event can be sent as a
	 * response.
	 */
	if (ctrlr->nr_aer_reqs == 0) {
		nvmf_ctrlr_queue_pending_async_event(ctrlr, event);
		return 0;
	}

	req = ctrlr->aer_req[--ctrlr->nr_aer_reqs];
	rsp = &req->rsp->nvme_cpl;

	rsp->cdw0 = event->raw;

	_nvmf_request_complete(req);
	ctrlr->aer_req[ctrlr->nr_aer_reqs] = NULL;

	return 0;
}

int
nvmf_ctrlr_async_event_ns_notice(struct spdk_nvmf_ctrlr *ctrlr)
{
	union spdk_nvme_async_event_completion event = {0};

	/* Users may disable the event notification */
	if (!ctrlr->feat.async_event_configuration.bits.ns_attr_notice) {
		return 0;
	}

	if (!nvmf_ctrlr_mask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGE_MASK_BIT)) {
		return 0;
	}

	event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE;
	event.bits.async_event_info = SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGED;
	event.bits.log_page_identifier = SPDK_NVME_LOG_CHANGED_NS_LIST;

	return nvmf_ctrlr_async_event_notification(ctrlr, &event);
}

int
nvmf_ctrlr_async_event_ana_change_notice(struct spdk_nvmf_ctrlr *ctrlr)
{
	union spdk_nvme_async_event_completion event = {0};

	/* Users may disable the event notification */
	if (!ctrlr->feat.async_event_configuration.bits.ana_change_notice) {
		return 0;
	}

	if (!nvmf_ctrlr_mask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_ANA_CHANGE_MASK_BIT)) {
		return 0;
	}

	event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE;
	event.bits.async_event_info = SPDK_NVME_ASYNC_EVENT_ANA_CHANGE;
	event.bits.log_page_identifier = SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS;

	return nvmf_ctrlr_async_event_notification(ctrlr, &event);
}

void
nvmf_ctrlr_async_event_reservation_notification(struct spdk_nvmf_ctrlr *ctrlr)
{
	union spdk_nvme_async_event_completion event = {0};

	if (!ctrlr->num_avail_log_pages) {
		return;
	}

	if (!nvmf_ctrlr_mask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_RESERVATION_LOG_AVAIL_MASK_BIT)) {
		return;
	}

	event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_IO;
	event.bits.async_event_info = SPDK_NVME_ASYNC_EVENT_RESERVATION_LOG_AVAIL;
	event.bits.log_page_identifier = SPDK_NVME_LOG_RESERVATION_NOTIFICATION;

	nvmf_ctrlr_async_event_notification(ctrlr, &event);
}

int
nvmf_ctrlr_async_event_discovery_log_change_notice(struct spdk_nvmf_ctrlr *ctrlr)
{
	union spdk_nvme_async_event_completion event = {0};

	/* Users may disable the event notification manually or
	 * it may not be enabled due to keep alive timeout
	 * not being set in connect command to discovery controller.
	 */
	if (!ctrlr->feat.async_event_configuration.bits.discovery_log_change_notice) {
		return 0;
	}

	if (!nvmf_ctrlr_mask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_DISCOVERY_LOG_CHANGE_MASK_BIT)) {
		return 0;
	}

	event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE;
	event.bits.async_event_info = SPDK_NVME_ASYNC_EVENT_DISCOVERY_LOG_CHANGE;
	event.bits.log_page_identifier = SPDK_NVME_LOG_DISCOVERY;

	return nvmf_ctrlr_async_event_notification(ctrlr, &event);
}

int
nvmf_ctrlr_async_event_error_event(struct spdk_nvmf_ctrlr *ctrlr,
				   union spdk_nvme_async_event_completion event)
{
	if (!nvmf_ctrlr_mask_aen(ctrlr, SPDK_NVME_ASYNC_EVENT_ERROR_MASK_BIT)) {
		return 0;
	}

	if (event.bits.async_event_type != SPDK_NVME_ASYNC_EVENT_TYPE_ERROR ||
	    event.bits.async_event_info > SPDK_NVME_ASYNC_EVENT_FW_IMAGE_LOAD) {
		return 0;
	}

	return nvmf_ctrlr_async_event_notification(ctrlr, &event);
}

void
nvmf_qpair_free_aer(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;
	int i;

	if (!nvmf_qpair_is_admin_queue(qpair)) {
		return;
	}

	for (i = 0; i < ctrlr->nr_aer_reqs; i++) {
		spdk_nvmf_request_free(ctrlr->aer_req[i]);
		ctrlr->aer_req[i] = NULL;
	}

	ctrlr->nr_aer_reqs = 0;
}

void
nvmf_ctrlr_abort_aer(struct spdk_nvmf_ctrlr *ctrlr)
{
	struct spdk_nvmf_request *req;
	int i;

	if (!ctrlr->nr_aer_reqs) {
		return;
	}

	for (i = 0; i < ctrlr->nr_aer_reqs; i++) {
		req = ctrlr->aer_req[i];

		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
		_nvmf_request_complete(req);

		ctrlr->aer_req[i] = NULL;
	}

	ctrlr->nr_aer_reqs = 0;
}

static void
_nvmf_ctrlr_add_reservation_log(void *ctx)
{
	struct spdk_nvmf_reservation_log *log = (struct spdk_nvmf_reservation_log *)ctx;
	struct spdk_nvmf_ctrlr *ctrlr = log->ctrlr;

	ctrlr->log_page_count++;

	/* Maximum number of queued log pages is 255 */
	if (ctrlr->num_avail_log_pages == 0xff) {
		struct spdk_nvmf_reservation_log *entry;
		entry = TAILQ_LAST(&ctrlr->log_head, log_page_head);
		entry->log.log_page_count = ctrlr->log_page_count;
		free(log);
		return;
	}

	log->log.log_page_count = ctrlr->log_page_count;
	log->log.num_avail_log_pages = ctrlr->num_avail_log_pages++;
	TAILQ_INSERT_TAIL(&ctrlr->log_head, log, link);

	nvmf_ctrlr_async_event_reservation_notification(ctrlr);
}

void
nvmf_ctrlr_reservation_notice_log(struct spdk_nvmf_ctrlr *ctrlr,
				  struct spdk_nvmf_ns *ns,
				  enum spdk_nvme_reservation_notification_log_page_type type)
{
	struct spdk_nvmf_reservation_log *log;

	switch (type) {
	case SPDK_NVME_RESERVATION_LOG_PAGE_EMPTY:
		return;
	case SPDK_NVME_REGISTRATION_PREEMPTED:
		if (ns->mask & SPDK_NVME_REGISTRATION_PREEMPTED_MASK) {
			return;
		}
		break;
	case SPDK_NVME_RESERVATION_RELEASED:
		if (ns->mask & SPDK_NVME_RESERVATION_RELEASED_MASK) {
			return;
		}
		break;
	case SPDK_NVME_RESERVATION_PREEMPTED:
		if (ns->mask & SPDK_NVME_RESERVATION_PREEMPTED_MASK) {
			return;
		}
		break;
	default:
		return;
	}

	log = calloc(1, sizeof(*log));
	if (!log) {
		SPDK_ERRLOG("Alloc log page failed, ignore the log\n");
		return;
	}
	log->ctrlr = ctrlr;
	log->log.type = type;
	log->log.nsid = ns->nsid;

	spdk_thread_send_msg(ctrlr->thread, _nvmf_ctrlr_add_reservation_log, log);
}

/* Check from subsystem poll group's namespace information data structure */
static bool
nvmf_ns_info_ctrlr_is_registrant(struct spdk_nvmf_subsystem_pg_ns_info *ns_info,
				 struct spdk_nvmf_ctrlr *ctrlr)
{
	uint32_t i;

	for (i = 0; i < SPDK_NVMF_MAX_NUM_REGISTRANTS; i++) {
		if (!spdk_uuid_compare(&ns_info->reg_hostid[i], &ctrlr->hostid)) {
			return true;
		}
	}

	return false;
}

/*
 * Check the NVMe command is permitted or not for current controller(Host).
 */
static int
nvmf_ns_reservation_request_check(struct spdk_nvmf_subsystem_pg_ns_info *ns_info,
				  struct spdk_nvmf_ctrlr *ctrlr,
				  struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	enum spdk_nvme_reservation_type rtype = ns_info->rtype;
	uint8_t status = SPDK_NVME_SC_SUCCESS;
	uint8_t racqa;
	bool is_registrant;

	/* No valid reservation */
	if (!rtype) {
		return 0;
	}

	is_registrant = nvmf_ns_info_ctrlr_is_registrant(ns_info, ctrlr);
	/* All registrants type and current ctrlr is a valid registrant */
	if ((rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS ||
	     rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_ALL_REGS) && is_registrant) {
		return 0;
	} else if (!spdk_uuid_compare(&ns_info->holder_id, &ctrlr->hostid)) {
		return 0;
	}

	/* Non-holder for current controller */
	switch (cmd->opc) {
	case SPDK_NVME_OPC_READ:
	case SPDK_NVME_OPC_COMPARE:
		if (rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS) {
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			goto exit;
		}
		if ((rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_REG_ONLY ||
		     rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_ALL_REGS) && !is_registrant) {
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
		}
		break;
	case SPDK_NVME_OPC_FLUSH:
	case SPDK_NVME_OPC_WRITE:
	case SPDK_NVME_OPC_WRITE_UNCORRECTABLE:
	case SPDK_NVME_OPC_WRITE_ZEROES:
	case SPDK_NVME_OPC_DATASET_MANAGEMENT:
		if (rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE ||
		    rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS) {
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			goto exit;
		}
		if (!is_registrant) {
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
		}
		break;
	case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
		racqa = cmd->cdw10_bits.resv_acquire.racqa;
		if (racqa == SPDK_NVME_RESERVE_ACQUIRE) {
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			goto exit;
		}
		if (!is_registrant) {
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
		}
		break;
	case SPDK_NVME_OPC_RESERVATION_RELEASE:
		if (!is_registrant) {
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
		}
		break;
	default:
		break;
	}

exit:
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	req->rsp->nvme_cpl.status.sc = status;
	if (status == SPDK_NVME_SC_RESERVATION_CONFLICT) {
		return -EPERM;
	}

	return 0;
}

static int
nvmf_ctrlr_process_io_fused_cmd(struct spdk_nvmf_request *req, struct spdk_bdev *bdev,
				struct spdk_bdev_desc *desc, struct spdk_io_channel *ch)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_request *first_fused_req = req->qpair->first_fused_req;
	int rc;

	if (cmd->fuse == SPDK_NVME_CMD_FUSE_FIRST) {
		/* first fused operation (should be compare) */
		if (first_fused_req != NULL) {
			struct spdk_nvme_cpl *fused_response = &first_fused_req->rsp->nvme_cpl;

			SPDK_ERRLOG("Wrong sequence of fused operations\n");

			/* abort req->qpair->first_fused_request and continue with new fused command */
			fused_response->status.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
			fused_response->status.sct = SPDK_NVME_SCT_GENERIC;
			_nvmf_request_complete(first_fused_req);
		} else if (cmd->opc != SPDK_NVME_OPC_COMPARE) {
			SPDK_ERRLOG("Wrong op code of fused operations\n");
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		req->qpair->first_fused_req = req;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	} else if (cmd->fuse == SPDK_NVME_CMD_FUSE_SECOND) {
		/* second fused operation (should be write) */
		if (first_fused_req == NULL) {
			SPDK_ERRLOG("Wrong sequence of fused operations\n");
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		} else if (cmd->opc != SPDK_NVME_OPC_WRITE) {
			struct spdk_nvme_cpl *fused_response = &first_fused_req->rsp->nvme_cpl;

			SPDK_ERRLOG("Wrong op code of fused operations\n");

			/* abort req->qpair->first_fused_request and fail current command */
			fused_response->status.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
			fused_response->status.sct = SPDK_NVME_SCT_GENERIC;
			_nvmf_request_complete(first_fused_req);

			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
			req->qpair->first_fused_req = NULL;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		/* save request of first command to generate response later */
		req->first_fused_req = first_fused_req;
		req->qpair->first_fused_req = NULL;
	} else {
		SPDK_ERRLOG("Invalid fused command fuse field.\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = nvmf_bdev_ctrlr_compare_and_write_cmd(bdev, desc, ch, req->first_fused_req, req);

	if (rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		if (spdk_nvme_cpl_is_error(rsp)) {
			struct spdk_nvme_cpl *fused_response = &first_fused_req->rsp->nvme_cpl;

			fused_response->status = rsp->status;
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_ABORTED_FAILED_FUSED;
			/* Complete first of fused commands. Second will be completed by upper layer */
			_nvmf_request_complete(first_fused_req);
			req->first_fused_req = NULL;
		}
	}

	return rc;
}

bool
nvmf_ctrlr_use_zcopy(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_transport *transport = req->qpair->transport;
	struct spdk_nvmf_ns *ns;

	assert(req->zcopy_phase == NVMF_ZCOPY_PHASE_NONE);

	if (!transport->opts.zcopy) {
		return false;
	}

	if (nvmf_qpair_is_admin_queue(req->qpair)) {
		/* Admin queue */
		return false;
	}

	if ((req->cmd->nvme_cmd.opc != SPDK_NVME_OPC_WRITE) &&
	    (req->cmd->nvme_cmd.opc != SPDK_NVME_OPC_READ)) {
		/* Not a READ or WRITE command */
		return false;
	}

	if (req->cmd->nvme_cmd.fuse != SPDK_NVME_CMD_FUSE_NONE) {
		/* Fused commands dont use zcopy buffers */
		return false;
	}

	ns = _nvmf_subsystem_get_ns(req->qpair->ctrlr->subsys, req->cmd->nvme_cmd.nsid);
	if (ns == NULL || ns->bdev == NULL || !ns->zcopy) {
		return false;
	}

	req->zcopy_phase = NVMF_ZCOPY_PHASE_INIT;
	return true;
}

void
spdk_nvmf_request_zcopy_start(struct spdk_nvmf_request *req)
{
	assert(req->zcopy_phase == NVMF_ZCOPY_PHASE_INIT);

	/* Set iovcnt to be the maximum number of iovs that the ZCOPY can use */
	req->iovcnt = NVMF_REQ_MAX_BUFFERS;

	spdk_nvmf_request_exec(req);
}

void
spdk_nvmf_request_zcopy_end(struct spdk_nvmf_request *req, bool commit)
{
	assert(req->zcopy_phase == NVMF_ZCOPY_PHASE_EXECUTE);
	req->zcopy_phase = NVMF_ZCOPY_PHASE_END_PENDING;

	nvmf_bdev_ctrlr_zcopy_end(req, commit);
}

int
nvmf_ctrlr_process_io_cmd(struct spdk_nvmf_request *req)
{
	uint32_t nsid;
	struct spdk_nvmf_ns *ns;
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	struct spdk_nvmf_poll_group *group = req->qpair->group;
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info;
	enum spdk_nvme_ana_state ana_state;

	/* pre-set response details for this command */
	response->status.sc = SPDK_NVME_SC_SUCCESS;
	nsid = cmd->nsid;

	if (spdk_unlikely(ctrlr == NULL)) {
		SPDK_ERRLOG("I/O command sent before CONNECT\n");
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(ctrlr->vcprop.cc.bits.en != 1)) {
		SPDK_ERRLOG("I/O command sent to disabled controller\n");
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ns = _nvmf_subsystem_get_ns(ctrlr->subsys, nsid);
	if (ns == NULL || ns->bdev == NULL) {
		SPDK_DEBUGLOG(nvmf, "Unsuccessful query for nsid %u\n", cmd->nsid);
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		response->status.dnr = 1;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ana_state = nvmf_ctrlr_get_ana_state(ctrlr, ns->anagrpid);
	if (spdk_unlikely(ana_state != SPDK_NVME_ANA_OPTIMIZED_STATE &&
			  ana_state != SPDK_NVME_ANA_NON_OPTIMIZED_STATE)) {
		SPDK_DEBUGLOG(nvmf, "Fail I/O command due to ANA state %d\n",
			      ana_state);
		response->status.sct = SPDK_NVME_SCT_PATH;
		response->status.sc = _nvme_ana_state_to_path_status(ana_state);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_likely(ctrlr->listener != NULL)) {
		SPDK_DTRACE_PROBE3(nvmf_request_io_exec_path, req,
				   ctrlr->listener->trid->traddr,
				   ctrlr->listener->trid->trsvcid);
	}

	/* scan-build falsely reporting dereference of null pointer */
	assert(group != NULL && group->sgroups != NULL);
	ns_info = &group->sgroups[ctrlr->subsys->id].ns_info[nsid - 1];
	if (nvmf_ns_reservation_request_check(ns_info, ctrlr, req)) {
		SPDK_DEBUGLOG(nvmf, "Reservation Conflict for nsid %u, opcode %u\n",
			      cmd->nsid, cmd->opc);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	bdev = ns->bdev;
	desc = ns->desc;
	ch = ns_info->channel;

	if (spdk_unlikely(cmd->fuse & SPDK_NVME_CMD_FUSE_MASK)) {
		return nvmf_ctrlr_process_io_fused_cmd(req, bdev, desc, ch);
	} else if (spdk_unlikely(req->qpair->first_fused_req != NULL)) {
		struct spdk_nvme_cpl *fused_response = &req->qpair->first_fused_req->rsp->nvme_cpl;

		SPDK_ERRLOG("Expected second of fused commands - failing first of fused commands\n");

		/* abort req->qpair->first_fused_request and continue with new command */
		fused_response->status.sc = SPDK_NVME_SC_ABORTED_MISSING_FUSED;
		fused_response->status.sct = SPDK_NVME_SCT_GENERIC;
		_nvmf_request_complete(req->qpair->first_fused_req);
		req->qpair->first_fused_req = NULL;
	}

	if (spdk_nvmf_request_using_zcopy(req)) {
		assert(req->zcopy_phase == NVMF_ZCOPY_PHASE_INIT);
		return nvmf_bdev_ctrlr_zcopy_start(bdev, desc, ch, req);
	} else {
		switch (cmd->opc) {
		case SPDK_NVME_OPC_READ:
			return nvmf_bdev_ctrlr_read_cmd(bdev, desc, ch, req);
		case SPDK_NVME_OPC_WRITE:
			return nvmf_bdev_ctrlr_write_cmd(bdev, desc, ch, req);
		case SPDK_NVME_OPC_COMPARE:
			return nvmf_bdev_ctrlr_compare_cmd(bdev, desc, ch, req);
		case SPDK_NVME_OPC_WRITE_ZEROES:
			return nvmf_bdev_ctrlr_write_zeroes_cmd(bdev, desc, ch, req);
		case SPDK_NVME_OPC_FLUSH:
			return nvmf_bdev_ctrlr_flush_cmd(bdev, desc, ch, req);
		case SPDK_NVME_OPC_DATASET_MANAGEMENT:
			return nvmf_bdev_ctrlr_dsm_cmd(bdev, desc, ch, req);
		case SPDK_NVME_OPC_RESERVATION_REGISTER:
		case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
		case SPDK_NVME_OPC_RESERVATION_RELEASE:
		case SPDK_NVME_OPC_RESERVATION_REPORT:
			spdk_thread_send_msg(ctrlr->subsys->thread, nvmf_ns_reservation_request, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		default:
			return nvmf_bdev_ctrlr_nvme_passthru_io(bdev, desc, ch, req);
		}
	}
}

static void
nvmf_qpair_request_cleanup(struct spdk_nvmf_qpair *qpair)
{
	if (qpair->state == SPDK_NVMF_QPAIR_DEACTIVATING) {
		assert(qpair->state_cb != NULL);

		if (TAILQ_EMPTY(&qpair->outstanding)) {
			qpair->state_cb(qpair->state_cb_arg, 0);
		}
	}
}

int
spdk_nvmf_request_free(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;

	TAILQ_REMOVE(&qpair->outstanding, req, link);
	if (nvmf_transport_req_free(req)) {
		SPDK_ERRLOG("Unable to free transport level request resources.\n");
	}

	nvmf_qpair_request_cleanup(qpair);

	return 0;
}

static void
_nvmf_request_complete(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_qpair *qpair;
	struct spdk_nvmf_subsystem_poll_group *sgroup = NULL;
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info;
	bool is_aer = false;
	uint32_t nsid;
	bool paused;
	uint8_t opcode;

	rsp->sqid = 0;
	rsp->status.p = 0;
	rsp->cid = req->cmd->nvme_cmd.cid;
	nsid = req->cmd->nvme_cmd.nsid;
	opcode = req->cmd->nvmf_cmd.opcode;

	qpair = req->qpair;
	if (qpair->ctrlr) {
		sgroup = &qpair->group->sgroups[qpair->ctrlr->subsys->id];
		assert(sgroup != NULL);
		is_aer = req->cmd->nvme_cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST;

		/*
		 * Set the crd value.
		 * If the the IO has any error, and dnr (DoNotRetry) is not 1,
		 * and ACRE is enabled, we will set the crd to 1 to select the first CRDT.
		 */
		if (spdk_nvme_cpl_is_error(rsp) &&
		    rsp->status.dnr == 0 &&
		    qpair->ctrlr->acre_enabled) {
			rsp->status.crd = 1;
		}
	} else if (spdk_unlikely(nvmf_request_is_fabric_connect(req))) {
		sgroup = nvmf_subsystem_pg_from_connect_cmd(req);
	}

	if (SPDK_DEBUGLOG_FLAG_ENABLED("nvmf")) {
		spdk_nvme_print_completion(qpair->qid, rsp);
	}

	switch (req->zcopy_phase) {
	case NVMF_ZCOPY_PHASE_NONE:
		TAILQ_REMOVE(&qpair->outstanding, req, link);
		break;
	case NVMF_ZCOPY_PHASE_INIT:
		if (spdk_unlikely(spdk_nvme_cpl_is_error(rsp))) {
			req->zcopy_phase = NVMF_ZCOPY_PHASE_INIT_FAILED;
			TAILQ_REMOVE(&qpair->outstanding, req, link);
		} else {
			req->zcopy_phase = NVMF_ZCOPY_PHASE_EXECUTE;
		}
		break;
	case NVMF_ZCOPY_PHASE_EXECUTE:
		break;
	case NVMF_ZCOPY_PHASE_END_PENDING:
		TAILQ_REMOVE(&qpair->outstanding, req, link);
		req->zcopy_phase = NVMF_ZCOPY_PHASE_COMPLETE;
		break;
	default:
		SPDK_ERRLOG("Invalid ZCOPY phase %u\n", req->zcopy_phase);
		break;
	}

	if (nvmf_transport_req_complete(req)) {
		SPDK_ERRLOG("Transport request completion error!\n");
	}

	/* AER cmd is an exception */
	if (sgroup && !is_aer) {
		if (spdk_unlikely(opcode == SPDK_NVME_OPC_FABRIC ||
				  nvmf_qpair_is_admin_queue(qpair))) {
			assert(sgroup->mgmt_io_outstanding > 0);
			sgroup->mgmt_io_outstanding--;
		} else {
			if (req->zcopy_phase == NVMF_ZCOPY_PHASE_NONE ||
			    req->zcopy_phase == NVMF_ZCOPY_PHASE_COMPLETE ||
			    req->zcopy_phase == NVMF_ZCOPY_PHASE_INIT_FAILED) {
				/* End of request */

				/* NOTE: This implicitly also checks for 0, since 0 - 1 wraps around to UINT32_MAX. */
				if (spdk_likely(nsid - 1 < sgroup->num_ns)) {
					sgroup->ns_info[nsid - 1].io_outstanding--;
				}
			}
		}

		if (spdk_unlikely(sgroup->state == SPDK_NVMF_SUBSYSTEM_PAUSING &&
				  sgroup->mgmt_io_outstanding == 0)) {
			paused = true;
			for (nsid = 0; nsid < sgroup->num_ns; nsid++) {
				ns_info = &sgroup->ns_info[nsid];

				if (ns_info->state == SPDK_NVMF_SUBSYSTEM_PAUSING &&
				    ns_info->io_outstanding > 0) {
					paused = false;
					break;
				}
			}

			if (paused) {
				sgroup->state = SPDK_NVMF_SUBSYSTEM_PAUSED;
				sgroup->cb_fn(sgroup->cb_arg, 0);
				sgroup->cb_fn = NULL;
				sgroup->cb_arg = NULL;
			}
		}

	}

	nvmf_qpair_request_cleanup(qpair);
}

int
spdk_nvmf_request_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;

	if (spdk_likely(qpair->group->thread == spdk_get_thread())) {
		_nvmf_request_complete(req);
	} else {
		spdk_thread_send_msg(qpair->group->thread,
				     _nvmf_request_complete, req);
	}

	return 0;
}

void
spdk_nvmf_request_exec_fabrics(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_subsystem_poll_group *sgroup = NULL;
	enum spdk_nvmf_request_exec_status status;

	if (qpair->ctrlr) {
		sgroup = &qpair->group->sgroups[qpair->ctrlr->subsys->id];
	} else if (spdk_unlikely(nvmf_request_is_fabric_connect(req))) {
		sgroup = nvmf_subsystem_pg_from_connect_cmd(req);
	}

	assert(sgroup != NULL);
	sgroup->mgmt_io_outstanding++;

	/* Place the request on the outstanding list so we can keep track of it */
	TAILQ_INSERT_TAIL(&qpair->outstanding, req, link);

	assert(req->cmd->nvmf_cmd.opcode == SPDK_NVME_OPC_FABRIC);
	status = nvmf_ctrlr_process_fabrics_cmd(req);

	if (status == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		_nvmf_request_complete(req);
	}
}

static bool nvmf_check_subsystem_active(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_subsystem_poll_group *sgroup = NULL;
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info;
	uint32_t nsid;

	if (qpair->ctrlr) {
		sgroup = &qpair->group->sgroups[qpair->ctrlr->subsys->id];
		assert(sgroup != NULL);
	} else if (spdk_unlikely(nvmf_request_is_fabric_connect(req))) {
		sgroup = nvmf_subsystem_pg_from_connect_cmd(req);
	}

	/* Check if the subsystem is paused (if there is a subsystem) */
	if (sgroup != NULL) {
		if (spdk_unlikely(req->cmd->nvmf_cmd.opcode == SPDK_NVME_OPC_FABRIC ||
				  nvmf_qpair_is_admin_queue(qpair))) {
			if (sgroup->state != SPDK_NVMF_SUBSYSTEM_ACTIVE) {
				/* The subsystem is not currently active. Queue this request. */
				TAILQ_INSERT_TAIL(&sgroup->queued, req, link);
				return false;
			}
			sgroup->mgmt_io_outstanding++;
		} else {
			nsid = req->cmd->nvme_cmd.nsid;

			/* NOTE: This implicitly also checks for 0, since 0 - 1 wraps around to UINT32_MAX. */
			if (spdk_unlikely(nsid - 1 >= sgroup->num_ns)) {
				req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
				req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
				req->rsp->nvme_cpl.status.dnr = 1;
				TAILQ_INSERT_TAIL(&qpair->outstanding, req, link);
				_nvmf_request_complete(req);
				return false;
			}

			ns_info = &sgroup->ns_info[nsid - 1];
			if (ns_info->channel == NULL) {
				/* This can can happen if host sends I/O to a namespace that is
				 * in the process of being added, but before the full addition
				 * process is complete.  Report invalid namespace in that case.
				 */
				req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
				req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
				req->rsp->nvme_cpl.status.dnr = 1;
				TAILQ_INSERT_TAIL(&qpair->outstanding, req, link);
				ns_info->io_outstanding++;
				_nvmf_request_complete(req);
				return false;
			}

			if (ns_info->state != SPDK_NVMF_SUBSYSTEM_ACTIVE) {
				/* The namespace is not currently active. Queue this request. */
				TAILQ_INSERT_TAIL(&sgroup->queued, req, link);
				return false;
			}

			ns_info->io_outstanding++;
		}

		if (qpair->state != SPDK_NVMF_QPAIR_ACTIVE) {
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
			TAILQ_INSERT_TAIL(&qpair->outstanding, req, link);
			_nvmf_request_complete(req);
			return false;
		}
	}

	return true;
}

void
spdk_nvmf_request_exec(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_transport *transport = qpair->transport;
	enum spdk_nvmf_request_exec_status status;

	if (!nvmf_check_subsystem_active(req)) {
		return;
	}

	if (SPDK_DEBUGLOG_FLAG_ENABLED("nvmf")) {
		spdk_nvme_print_command(qpair->qid, &req->cmd->nvme_cmd);
	}

	/* Place the request on the outstanding list so we can keep track of it */
	TAILQ_INSERT_TAIL(&qpair->outstanding, req, link);

	if (spdk_unlikely((req->cmd->nvmf_cmd.opcode == SPDK_NVME_OPC_FABRIC) &&
			  spdk_nvme_trtype_is_fabrics(transport->ops->type))) {
		status = nvmf_ctrlr_process_fabrics_cmd(req);
	} else if (spdk_unlikely(nvmf_qpair_is_admin_queue(qpair))) {
		status = nvmf_ctrlr_process_admin_cmd(req);
	} else {
		status = nvmf_ctrlr_process_io_cmd(req);
	}

	if (status == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		_nvmf_request_complete(req);
	}
}

static bool
nvmf_ctrlr_get_dif_ctx(struct spdk_nvmf_ctrlr *ctrlr, struct spdk_nvme_cmd *cmd,
		       struct spdk_dif_ctx *dif_ctx)
{
	struct spdk_nvmf_ns *ns;
	struct spdk_bdev *bdev;

	if (ctrlr == NULL || cmd == NULL) {
		return false;
	}

	ns = _nvmf_subsystem_get_ns(ctrlr->subsys, cmd->nsid);
	if (ns == NULL || ns->bdev == NULL) {
		return false;
	}

	bdev = ns->bdev;

	switch (cmd->opc) {
	case SPDK_NVME_OPC_READ:
	case SPDK_NVME_OPC_WRITE:
	case SPDK_NVME_OPC_COMPARE:
		return nvmf_bdev_ctrlr_get_dif_ctx(bdev, cmd, dif_ctx);
	default:
		break;
	}

	return false;
}

bool
spdk_nvmf_request_get_dif_ctx(struct spdk_nvmf_request *req, struct spdk_dif_ctx *dif_ctx)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;

	if (spdk_likely(ctrlr == NULL || !ctrlr->dif_insert_or_strip)) {
		return false;
	}

	if (spdk_unlikely(qpair->state != SPDK_NVMF_QPAIR_ACTIVE)) {
		return false;
	}

	if (spdk_unlikely(req->cmd->nvmf_cmd.opcode == SPDK_NVME_OPC_FABRIC)) {
		return false;
	}

	if (spdk_unlikely(nvmf_qpair_is_admin_queue(qpair))) {
		return false;
	}

	return nvmf_ctrlr_get_dif_ctx(ctrlr, &req->cmd->nvme_cmd, dif_ctx);
}

void
spdk_nvmf_set_custom_admin_cmd_hdlr(uint8_t opc, spdk_nvmf_custom_cmd_hdlr hdlr)
{
	g_nvmf_custom_admin_cmd_hdlrs[opc].hdlr = hdlr;
}

static int
nvmf_passthru_admin_cmd(struct spdk_nvmf_request *req)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	struct spdk_nvme_cmd *cmd = spdk_nvmf_request_get_cmd(req);
	struct spdk_nvme_cpl *response = spdk_nvmf_request_get_response(req);
	uint32_t bdev_nsid;
	int rc;

	if (g_nvmf_custom_admin_cmd_hdlrs[cmd->opc].nsid == 0) {
		bdev_nsid = cmd->nsid;
	} else {
		bdev_nsid = g_nvmf_custom_admin_cmd_hdlrs[cmd->opc].nsid;
	}

	rc = spdk_nvmf_request_get_bdev(bdev_nsid, req, &bdev, &desc, &ch);
	if (rc) {
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	return spdk_nvmf_bdev_ctrlr_nvme_passthru_admin(bdev, desc, ch, req, NULL);
}

void
spdk_nvmf_set_passthru_admin_cmd(uint8_t opc, uint32_t forward_nsid)
{
	g_nvmf_custom_admin_cmd_hdlrs[opc].hdlr = nvmf_passthru_admin_cmd;
	g_nvmf_custom_admin_cmd_hdlrs[opc].nsid = forward_nsid;
}

int
spdk_nvmf_request_get_bdev(uint32_t nsid, struct spdk_nvmf_request *req,
			   struct spdk_bdev **bdev, struct spdk_bdev_desc **desc, struct spdk_io_channel **ch)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvmf_ns *ns;
	struct spdk_nvmf_poll_group *group = req->qpair->group;
	struct spdk_nvmf_subsystem_pg_ns_info *ns_info;

	*bdev = NULL;
	*desc = NULL;
	*ch = NULL;

	ns = _nvmf_subsystem_get_ns(ctrlr->subsys, nsid);
	if (ns == NULL || ns->bdev == NULL) {
		return -EINVAL;
	}

	assert(group != NULL && group->sgroups != NULL);
	ns_info = &group->sgroups[ctrlr->subsys->id].ns_info[nsid - 1];
	*bdev = ns->bdev;
	*desc = ns->desc;
	*ch = ns_info->channel;

	return 0;
}

struct spdk_nvmf_ctrlr *spdk_nvmf_request_get_ctrlr(struct spdk_nvmf_request *req)
{
	return req->qpair->ctrlr;
}

struct spdk_nvme_cmd *spdk_nvmf_request_get_cmd(struct spdk_nvmf_request *req)
{
	return &req->cmd->nvme_cmd;
}

struct spdk_nvme_cpl *spdk_nvmf_request_get_response(struct spdk_nvmf_request *req)
{
	return &req->rsp->nvme_cpl;
}

struct spdk_nvmf_subsystem *spdk_nvmf_request_get_subsystem(struct spdk_nvmf_request *req)
{
	return req->qpair->ctrlr->subsys;
}

void spdk_nvmf_request_get_data(struct spdk_nvmf_request *req, void **data, uint32_t *length)
{
	*data = req->data;
	*length = req->length;
}

struct spdk_nvmf_subsystem *spdk_nvmf_ctrlr_get_subsystem(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->subsys;
}

uint16_t spdk_nvmf_ctrlr_get_id(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->cntlid;
}

struct spdk_nvmf_request *spdk_nvmf_request_get_req_to_abort(struct spdk_nvmf_request *req)
{
	return req->req_to_abort;
}
