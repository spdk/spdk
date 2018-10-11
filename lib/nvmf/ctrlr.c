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

#include "spdk/stdinc.h"

#include "nvmf_internal.h"
#include "transport.h"

#include "spdk/bit_array.h"
#include "spdk/endian.h"
#include "spdk/thread.h"
#include "spdk/trace.h"
#include "spdk/nvme_spec.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/version.h"

#include "spdk_internal/log.h"

#define MIN_KEEP_ALIVE_TIMEOUT 10000

#define MODEL_NUMBER "SPDK bdev Controller"

/*
 * Report the SPDK version as the firmware revision.
 * SPDK_VERSION_STRING won't fit into FR (only 8 bytes), so try to fit the most important parts.
 */
#define FW_VERSION SPDK_VERSION_MAJOR_STRING SPDK_VERSION_MINOR_STRING SPDK_VERSION_PATCH_STRING

static inline void
spdk_nvmf_invalid_connect_response(struct spdk_nvmf_fabric_connect_rsp *rsp,
				   uint8_t iattr, uint16_t ipo)
{
	rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
	rsp->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
	rsp->status_code_specific.invalid.iattr = iattr;
	rsp->status_code_specific.invalid.ipo = ipo;
}

#define SPDK_NVMF_INVALID_CONNECT_CMD(rsp, field)	\
	spdk_nvmf_invalid_connect_response(rsp, 0, offsetof(struct spdk_nvmf_fabric_connect_cmd, field))
#define SPDK_NVMF_INVALID_CONNECT_DATA(rsp, field)	\
	spdk_nvmf_invalid_connect_response(rsp, 1, offsetof(struct spdk_nvmf_fabric_connect_data, field))

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
	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "connect capsule response: cntlid = 0x%04x\n",
		      rsp->status_code_specific.success.cntlid);
}

static void
_spdk_nvmf_request_complete(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;

	spdk_nvmf_request_complete(req);
}

static void
_spdk_nvmf_ctrlr_add_admin_qpair(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;

	ctrlr->admin_qpair = qpair;
	ctrlr_add_qpair_and_update_rsp(qpair, ctrlr, rsp);
	spdk_nvmf_request_complete(req);
}

static void
_spdk_nvmf_subsystem_add_ctrlr(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;

	if (spdk_nvmf_subsystem_add_ctrlr(ctrlr->subsys, ctrlr)) {
		SPDK_ERRLOG("Unable to add controller to subsystem\n");
		free(ctrlr);
		qpair->ctrlr = NULL;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		spdk_thread_send_msg(qpair->group->thread, _spdk_nvmf_request_complete, req);
		return;
	}

	spdk_thread_send_msg(ctrlr->thread, _spdk_nvmf_ctrlr_add_admin_qpair, req);
}

static struct spdk_nvmf_ctrlr *
spdk_nvmf_ctrlr_create(struct spdk_nvmf_subsystem *subsystem,
		       struct spdk_nvmf_request *req,
		       struct spdk_nvmf_fabric_connect_cmd *connect_cmd,
		       struct spdk_nvmf_fabric_connect_data *connect_data)
{
	struct spdk_nvmf_ctrlr	*ctrlr;
	struct spdk_nvmf_transport *transport;

	ctrlr = calloc(1, sizeof(*ctrlr));
	if (ctrlr == NULL) {
		SPDK_ERRLOG("Memory allocation failed\n");
		return NULL;
	}

	req->qpair->ctrlr = ctrlr;
	ctrlr->subsys = subsystem;
	ctrlr->thread = req->qpair->group->thread;

	transport = req->qpair->transport;
	ctrlr->qpair_mask = spdk_bit_array_create(transport->opts.max_qpairs_per_ctrlr);
	if (!ctrlr->qpair_mask) {
		SPDK_ERRLOG("Failed to allocate controller qpair mask\n");
		free(ctrlr);
		return NULL;
	}

	ctrlr->feat.keep_alive_timer.bits.kato = connect_cmd->kato;
	ctrlr->feat.async_event_configuration.bits.ns_attr_notice = 1;
	ctrlr->feat.volatile_write_cache.bits.wce = 1;

	/* Subtract 1 for admin queue, 1 for 0's based */
	ctrlr->feat.number_of_queues.bits.ncqr = transport->opts.max_qpairs_per_ctrlr - 1 -
			1;
	ctrlr->feat.number_of_queues.bits.nsqr = transport->opts.max_qpairs_per_ctrlr - 1 -
			1;

	memcpy(ctrlr->hostid, connect_data->hostid, sizeof(ctrlr->hostid));

	ctrlr->vcprop.cap.raw = 0;
	ctrlr->vcprop.cap.bits.cqr = 1; /* NVMe-oF specification required */
	ctrlr->vcprop.cap.bits.mqes = transport->opts.max_queue_depth -
				      1; /* max queue depth */
	ctrlr->vcprop.cap.bits.ams = 0; /* optional arb mechanisms */
	ctrlr->vcprop.cap.bits.to = 1; /* ready timeout - 500 msec units */
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

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "cap 0x%" PRIx64 "\n", ctrlr->vcprop.cap.raw);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "vs 0x%x\n", ctrlr->vcprop.vs.raw);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "cc 0x%x\n", ctrlr->vcprop.cc.raw);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "csts 0x%x\n", ctrlr->vcprop.csts.raw);

	spdk_thread_send_msg(subsystem->thread, _spdk_nvmf_subsystem_add_ctrlr, req);

	return ctrlr;
}

void
spdk_nvmf_ctrlr_destruct(struct spdk_nvmf_ctrlr *ctrlr)
{
	spdk_nvmf_subsystem_remove_ctrlr(ctrlr->subsys, ctrlr);

	free(ctrlr);
}

static void
spdk_nvmf_ctrlr_add_io_qpair(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;

	/* Unit test will check qpair->ctrlr after calling spdk_nvmf_ctrlr_connect.
	  * For error case, the value should be NULL. So set it to NULL at first.
	  */
	qpair->ctrlr = NULL;

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

	ctrlr_add_qpair_and_update_rsp(qpair, ctrlr, rsp);

end:
	spdk_thread_send_msg(qpair->group->thread, _spdk_nvmf_request_complete, req);
}

static void
_spdk_nvmf_ctrlr_add_io_qpair(void *ctx)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	struct spdk_nvmf_fabric_connect_data *data = req->data;
	struct spdk_nvmf_ctrlr *ctrlr;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_qpair *admin_qpair;
	struct spdk_nvmf_tgt *tgt = qpair->transport->tgt;
	struct spdk_nvmf_subsystem *subsystem;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Connect I/O Queue for controller id 0x%x\n", data->cntlid);

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, data->subnqn);
	/* We already checked this in spdk_nvmf_ctrlr_connect */
	assert(subsystem != NULL);

	ctrlr = spdk_nvmf_subsystem_get_ctrlr(subsystem, data->cntlid);
	if (ctrlr == NULL) {
		SPDK_ERRLOG("Unknown controller ID 0x%x\n", data->cntlid);
		SPDK_NVMF_INVALID_CONNECT_DATA(rsp, cntlid);
		spdk_thread_send_msg(qpair->group->thread, _spdk_nvmf_request_complete, req);
		return;
	}

	admin_qpair = ctrlr->admin_qpair;
	qpair->ctrlr = ctrlr;
	spdk_thread_send_msg(admin_qpair->group->thread, spdk_nvmf_ctrlr_add_io_qpair, req);
}

static int
spdk_nvmf_ctrlr_connect(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_connect_data *data = req->data;
	struct spdk_nvmf_fabric_connect_cmd *cmd = &req->cmd->connect_cmd;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_tgt *tgt = qpair->transport->tgt;
	struct spdk_nvmf_ctrlr *ctrlr;
	struct spdk_nvmf_subsystem *subsystem;
	const char *subnqn, *hostnqn;
	struct spdk_nvme_transport_id listen_trid = {};
	void *end;

	if (req->length < sizeof(struct spdk_nvmf_fabric_connect_data)) {
		SPDK_ERRLOG("Connect command data length 0x%x too small\n", req->length);
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "recfmt 0x%x qid %u sqsize %u\n",
		      cmd->recfmt, cmd->qid, cmd->sqsize);

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Connect data:\n");
	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "  cntlid:  0x%04x\n", data->cntlid);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "  hostid: %08x-%04x-%04x-%02x%02x-%04x%08x ***\n",
		      ntohl(*(uint32_t *)&data->hostid[0]),
		      ntohs(*(uint16_t *)&data->hostid[4]),
		      ntohs(*(uint16_t *)&data->hostid[6]),
		      data->hostid[8],
		      data->hostid[9],
		      ntohs(*(uint16_t *)&data->hostid[10]),
		      ntohl(*(uint32_t *)&data->hostid[12]));

	if (cmd->recfmt != 0) {
		SPDK_ERRLOG("Connect command unsupported RECFMT %u\n", cmd->recfmt);
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVMF_FABRIC_SC_INCOMPATIBLE_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* Ensure that subnqn is null terminated */
	end = memchr(data->subnqn, '\0', SPDK_NVMF_NQN_MAX_LEN + 1);
	if (!end) {
		SPDK_ERRLOG("Connect SUBNQN is not null terminated\n");
		SPDK_NVMF_INVALID_CONNECT_DATA(rsp, subnqn);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	subnqn = data->subnqn;
	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "  subnqn: \"%s\"\n", subnqn);

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, subnqn);
	if (subsystem == NULL) {
		SPDK_ERRLOG("Could not find subsystem '%s'\n", subnqn);
		SPDK_NVMF_INVALID_CONNECT_DATA(rsp, subnqn);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* Ensure that hostnqn is null terminated */
	end = memchr(data->hostnqn, '\0', SPDK_NVMF_NQN_MAX_LEN + 1);
	if (!end) {
		SPDK_ERRLOG("Connect HOSTNQN is not null terminated\n");
		SPDK_NVMF_INVALID_CONNECT_DATA(rsp, hostnqn);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	hostnqn = data->hostnqn;
	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "  hostnqn: \"%s\"\n", hostnqn);

	if (!spdk_nvmf_subsystem_host_allowed(subsystem, hostnqn)) {
		SPDK_ERRLOG("Subsystem '%s' does not allow host '%s'\n", subnqn, hostnqn);
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_HOST;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_nvmf_qpair_get_listen_trid(qpair, &listen_trid)) {
		SPDK_ERRLOG("Subsystem '%s' is unable to enforce access control due to an internal error.\n",
			    subnqn);
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_HOST;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (!spdk_nvmf_subsystem_listener_allowed(subsystem, &listen_trid)) {
		SPDK_ERRLOG("Subsystem '%s' does not allow host '%s' to connect at this address.\n", subnqn,
			    hostnqn);
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_HOST;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/*
	 * SQSIZE is a 0-based value, so it must be at least 1 (minimum queue depth is 2) and
	 *  strictly less than max_queue_depth.
	 */
	if (cmd->sqsize == 0 || cmd->sqsize >= qpair->transport->opts.max_queue_depth) {
		SPDK_ERRLOG("Invalid SQSIZE %u (min 1, max %u)\n",
			    cmd->sqsize, qpair->transport->opts.max_queue_depth - 1);
		SPDK_NVMF_INVALID_CONNECT_CMD(rsp, sqsize);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	qpair->sq_head_max = cmd->sqsize;
	qpair->qid = cmd->qid;

	if (cmd->qid == 0) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Connect Admin Queue for controller ID 0x%x\n", data->cntlid);

		if (data->cntlid != 0xFFFF) {
			/* This NVMf target only supports dynamic mode. */
			SPDK_ERRLOG("The NVMf target only supports dynamic mode (CNTLID = 0x%x).\n", data->cntlid);
			SPDK_NVMF_INVALID_CONNECT_DATA(rsp, cntlid);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		/* Establish a new ctrlr */
		ctrlr = spdk_nvmf_ctrlr_create(subsystem, req, cmd, data);
		if (!ctrlr) {
			SPDK_ERRLOG("spdk_nvmf_ctrlr_create() failed\n");
			rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		} else {
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
	} else {
		spdk_thread_send_msg(subsystem->thread, _spdk_nvmf_ctrlr_add_io_qpair, req);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	}
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
nvmf_prop_set_cc(struct spdk_nvmf_ctrlr *ctrlr, uint64_t value)
{
	union spdk_nvme_cc_register cc, diff;

	cc.raw = (uint32_t)value;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "cur CC: 0x%08x\n", ctrlr->vcprop.cc.raw);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "new CC: 0x%08x\n", cc.raw);

	/*
	 * Calculate which bits changed between the current and new CC.
	 * Mark each bit as 0 once it is handled to determine if any unhandled bits were changed.
	 */
	diff.raw = cc.raw ^ ctrlr->vcprop.cc.raw;

	if (diff.bits.en) {
		if (cc.bits.en) {
			SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Property Set CC Enable!\n");
			ctrlr->vcprop.cc.bits.en = 1;
			ctrlr->vcprop.csts.bits.rdy = 1;
		} else {
			SPDK_ERRLOG("CC.EN transition from 1 to 0 (reset) not implemented!\n");

		}
		diff.bits.en = 0;
	}

	if (diff.bits.shn) {
		if (cc.bits.shn == SPDK_NVME_SHN_NORMAL ||
		    cc.bits.shn == SPDK_NVME_SHN_ABRUPT) {
			SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Property Set CC Shutdown %u%ub!\n",
				      cc.bits.shn >> 1, cc.bits.shn & 1);
			ctrlr->vcprop.cc.bits.shn = cc.bits.shn;
			ctrlr->vcprop.cc.bits.en = 0;
			ctrlr->vcprop.csts.bits.rdy = 0;
			ctrlr->vcprop.csts.bits.shst = SPDK_NVME_SHST_COMPLETE;
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
		SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Prop Set IOSQES = %u (%u bytes)\n",
			      cc.bits.iosqes, 1u << cc.bits.iosqes);
		ctrlr->vcprop.cc.bits.iosqes = cc.bits.iosqes;
		diff.bits.iosqes = 0;
	}

	if (diff.bits.iocqes) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Prop Set IOCQES = %u (%u bytes)\n",
			      cc.bits.iocqes, 1u << cc.bits.iocqes);
		ctrlr->vcprop.cc.bits.iocqes = cc.bits.iocqes;
		diff.bits.iocqes = 0;
	}

	if (diff.raw != 0) {
		SPDK_ERRLOG("Prop Set CC toggled reserved bits 0x%x!\n", diff.raw);
		return false;
	}

	return true;
}

static uint64_t
nvmf_prop_get_csts(struct spdk_nvmf_ctrlr *ctrlr)
{
	return ctrlr->vcprop.csts.raw;
}

struct nvmf_prop {
	uint32_t ofst;
	uint8_t size;
	char name[11];
	uint64_t (*get_cb)(struct spdk_nvmf_ctrlr *ctrlr);
	bool (*set_cb)(struct spdk_nvmf_ctrlr *ctrlr, uint64_t value);
};

#define PROP(field, size, get_cb, set_cb) \
	{ \
		offsetof(struct spdk_nvme_registers, field), \
		SPDK_NVMF_PROP_SIZE_##size, \
		#field, \
		get_cb, set_cb \
	}

static const struct nvmf_prop nvmf_props[] = {
	PROP(cap,  8, nvmf_prop_get_cap,  NULL),
	PROP(vs,   4, nvmf_prop_get_vs,   NULL),
	PROP(cc,   4, nvmf_prop_get_cc,   nvmf_prop_set_cc),
	PROP(csts, 4, nvmf_prop_get_csts, NULL),
};

static const struct nvmf_prop *
find_prop(uint32_t ofst)
{
	size_t i;

	for (i = 0; i < SPDK_COUNTOF(nvmf_props); i++) {
		const struct nvmf_prop *prop = &nvmf_props[i];

		if (prop->ofst == ofst) {
			return prop;
		}
	}

	return NULL;
}

static int
spdk_nvmf_property_get(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvmf_fabric_prop_get_cmd *cmd = &req->cmd->prop_get_cmd;
	struct spdk_nvmf_fabric_prop_get_rsp *response = &req->rsp->prop_get_rsp;
	const struct nvmf_prop *prop;

	response->status.sc = 0;
	response->value.u64 = 0;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "size %d, offset 0x%x\n",
		      cmd->attrib.size, cmd->ofst);

	if (cmd->attrib.size != SPDK_NVMF_PROP_SIZE_4 &&
	    cmd->attrib.size != SPDK_NVMF_PROP_SIZE_8) {
		SPDK_ERRLOG("Invalid size value %d\n", cmd->attrib.size);
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	prop = find_prop(cmd->ofst);
	if (prop == NULL || prop->get_cb == NULL) {
		/* Reserved properties return 0 when read */
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "name: %s\n", prop->name);
	if (cmd->attrib.size != prop->size) {
		SPDK_ERRLOG("offset 0x%x size mismatch: cmd %u, prop %u\n",
			    cmd->ofst, cmd->attrib.size, prop->size);
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	response->value.u64 = prop->get_cb(ctrlr);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "response value: 0x%" PRIx64 "\n", response->value.u64);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
spdk_nvmf_property_set(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvmf_fabric_prop_set_cmd *cmd = &req->cmd->prop_set_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	const struct nvmf_prop *prop;
	uint64_t value;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "size %d, offset 0x%x, value 0x%" PRIx64 "\n",
		      cmd->attrib.size, cmd->ofst, cmd->value.u64);

	prop = find_prop(cmd->ofst);
	if (prop == NULL || prop->set_cb == NULL) {
		SPDK_ERRLOG("Invalid offset 0x%x\n", cmd->ofst);
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "name: %s\n", prop->name);
	if (cmd->attrib.size != prop->size) {
		SPDK_ERRLOG("offset 0x%x size mismatch: cmd %u, prop %u\n",
			    cmd->ofst, cmd->attrib.size, prop->size);
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	value = cmd->value.u64;
	if (prop->size == SPDK_NVMF_PROP_SIZE_4) {
		value = (uint32_t)value;
	}

	if (!prop->set_cb(ctrlr, value)) {
		SPDK_ERRLOG("prop set_cb failed\n");
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
spdk_nvmf_ctrlr_set_features_arbitration(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Set Features - Arbitration (cdw11 = 0x%0x)\n", cmd->cdw11);

	ctrlr->feat.arbitration.raw = cmd->cdw11;
	ctrlr->feat.arbitration.bits.reserved = 0;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
spdk_nvmf_ctrlr_set_features_power_management(struct spdk_nvmf_request *req)
{
	union spdk_nvme_feat_power_management opts;
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Set Features - Power Management (cdw11 = 0x%0x)\n", cmd->cdw11);
	opts.raw = cmd->cdw11;

	/* Only PS = 0 is allowed, since we report NPSS = 0 */
	if (opts.bits.ps != 0) {
		SPDK_ERRLOG("Invalid power state %u\n", opts.bits.ps);
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
spdk_nvmf_ctrlr_set_features_temperature_threshold(struct spdk_nvmf_request *req)
{
	union spdk_nvme_feat_temperature_threshold opts;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Set Features - Temperature Threshold (cdw11 = 0x%0x)\n", cmd->cdw11);
	opts.raw = cmd->cdw11;

	if (!temp_threshold_opts_valid(&opts)) {
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* TODO: no sensors implemented - ignore new values */
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
spdk_nvmf_ctrlr_get_features_temperature_threshold(struct spdk_nvmf_request *req)
{
	union spdk_nvme_feat_temperature_threshold opts;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Get Features - Temperature Threshold (cdw11 = 0x%0x)\n", cmd->cdw11);
	opts.raw = cmd->cdw11;

	if (!temp_threshold_opts_valid(&opts)) {
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/* TODO: no sensors implemented - return 0 for all thresholds */
	rsp->cdw0 = 0;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
spdk_nvmf_ctrlr_set_features_error_recovery(struct spdk_nvmf_request *req)
{
	union spdk_nvme_feat_error_recovery opts;
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Set Features - Error Recovery (cdw11 = 0x%0x)\n", cmd->cdw11);
	opts.raw = cmd->cdw11;

	if (opts.bits.dulbe) {
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
spdk_nvmf_ctrlr_set_features_volatile_write_cache(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Set Features - Volatile Write Cache (cdw11 = 0x%0x)\n", cmd->cdw11);

	ctrlr->feat.volatile_write_cache.raw = cmd->cdw11;
	ctrlr->feat.volatile_write_cache.bits.reserved = 0;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Set Features - Volatile Write Cache %s\n",
		      ctrlr->feat.volatile_write_cache.bits.wce ? "Enabled" : "Disabled");
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
spdk_nvmf_ctrlr_set_features_write_atomicity(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Set Features - Write Atomicity (cdw11 = 0x%0x)\n", cmd->cdw11);

	ctrlr->feat.write_atomicity.raw = cmd->cdw11;
	ctrlr->feat.write_atomicity.bits.reserved = 0;

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
spdk_nvmf_ctrlr_set_features_host_identifier(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	SPDK_ERRLOG("Set Features - Host Identifier not allowed\n");
	response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
spdk_nvmf_ctrlr_get_features_host_identifier(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	union spdk_nvme_feat_host_identifier opts;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Get Features - Host Identifier\n");

	opts.raw = cmd->cdw11;
	if (!opts.bits.exhid) {
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

	memcpy(req->data, ctrlr->hostid, sizeof(ctrlr->hostid));
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
spdk_nvmf_ctrlr_set_features_keep_alive_timer(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Set Features - Keep Alive Timer (%u ms)\n", cmd->cdw11);

	if (cmd->cdw11 == 0) {
		rsp->status.sc = SPDK_NVME_SC_KEEP_ALIVE_INVALID;
	} else if (cmd->cdw11 < MIN_KEEP_ALIVE_TIMEOUT) {
		ctrlr->feat.keep_alive_timer.bits.kato = MIN_KEEP_ALIVE_TIMEOUT;
	} else {
		ctrlr->feat.keep_alive_timer.bits.kato = cmd->cdw11;
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Set Features - Keep Alive Timer set to %u ms\n",
		      ctrlr->feat.keep_alive_timer.bits.kato);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
spdk_nvmf_ctrlr_set_features_number_of_queues(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint32_t count;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Set Features - Number of Queues, cdw11 0x%x\n",
		      req->cmd->nvme_cmd.cdw11);

	count = spdk_bit_array_count_set(ctrlr->qpair_mask);
	/* verify that the controller is ready to process commands */
	if (count > 1) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Queue pairs already active!\n");
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

static int
spdk_nvmf_ctrlr_set_features_async_event_configuration(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Set Features - Async Event Configuration, cdw11 0x%08x\n",
		      cmd->cdw11);
	ctrlr->feat.async_event_configuration.raw = cmd->cdw11;
	ctrlr->feat.async_event_configuration.bits.reserved = 0;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
spdk_nvmf_ctrlr_async_event_request(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Async Event Request\n");

	/* Only one asynchronous event is supported for now */
	if (ctrlr->aer_req != NULL) {
		SPDK_DEBUGLOG(SPDK_LOG_NVMF, "AERL exceeded\n");
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (ctrlr->notice_event.bits.async_event_type ==
	    SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE) {
		rsp->cdw0 = ctrlr->notice_event.raw;
		ctrlr->notice_event.raw = 0;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ctrlr->aer_req = req;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static void
spdk_nvmf_get_firmware_slot_log_page(void *buffer, uint64_t offset, uint32_t length)
{
	struct spdk_nvme_firmware_page fw_page;
	size_t copy_len;

	memset(&fw_page, 0, sizeof(fw_page));
	fw_page.afi.active_slot = 1;
	fw_page.afi.next_reset_slot = 0;
	spdk_strcpy_pad(fw_page.revision[0], FW_VERSION, sizeof(fw_page.revision[0]), ' ');

	if (offset < sizeof(fw_page)) {
		copy_len = spdk_min(sizeof(fw_page) - offset, length);
		if (copy_len > 0) {
			memcpy(buffer, (const char *)&fw_page + offset, copy_len);
		}
	}
}

void
spdk_nvmf_ctrlr_ns_changed(struct spdk_nvmf_ctrlr *ctrlr, uint32_t nsid)
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

	spdk_nvmf_ctrlr_async_event_ns_notice(ctrlr);
}

static void
spdk_nvmf_get_changed_ns_list_log_page(struct spdk_nvmf_ctrlr *ctrlr,
				       void *buffer, uint64_t offset, uint32_t length)
{
	size_t copy_length;

	if (offset < sizeof(ctrlr->changed_ns_list)) {
		copy_length = spdk_min(length, sizeof(ctrlr->changed_ns_list) - offset);
		if (copy_length) {
			memcpy(buffer, (char *)&ctrlr->changed_ns_list + offset, copy_length);
		}
	}

	/* Clear log page each time it is read */
	ctrlr->changed_ns_list_count = 0;
	memset(&ctrlr->changed_ns_list, 0, sizeof(ctrlr->changed_ns_list));
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
	},
};

static void
spdk_nvmf_get_cmds_and_effects_log_page(void *buffer,
					uint64_t offset, uint32_t length)
{
	uint32_t page_size = sizeof(struct spdk_nvme_cmds_and_effect_log_page);
	size_t copy_len = 0;
	size_t zero_len = length;

	if (offset < page_size) {
		copy_len = spdk_min(page_size - offset, length);
		zero_len -= copy_len;
		memcpy(buffer, (char *)(&g_cmds_and_effect_log_page) + offset, copy_len);
	}

	if (zero_len) {
		memset((char *)buffer + copy_len, 0, zero_len);
	}
}

static int
spdk_nvmf_ctrlr_get_log_page(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	uint64_t offset, len;
	uint32_t numdl, numdu;
	uint8_t lid;

	if (req->data == NULL) {
		SPDK_ERRLOG("get log command with no buffer\n");
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

	numdl = (cmd->cdw10 >> 16) & 0xFFFFu;
	numdu = (cmd->cdw11) & 0xFFFFu;
	len = ((numdu << 16) + numdl + (uint64_t)1) * 4;
	if (len > req->length) {
		SPDK_ERRLOG("Get log page: len (%" PRIu64 ") > buf size (%u)\n",
			    len, req->length);
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	lid = cmd->cdw10 & 0xFF;
	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Get log page: LID=0x%02X offset=0x%" PRIx64 " len=0x%" PRIx64 "\n",
		      lid, offset, len);

	if (subsystem->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY) {
		switch (lid) {
		case SPDK_NVME_LOG_DISCOVERY:
			spdk_nvmf_get_discovery_log_page(subsystem->tgt, req->data, offset, len);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		default:
			goto invalid_log_page;
		}
	} else {
		switch (lid) {
		case SPDK_NVME_LOG_ERROR:
		case SPDK_NVME_LOG_HEALTH_INFORMATION:
			/* TODO: actually fill out log page data */
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		case SPDK_NVME_LOG_FIRMWARE_SLOT:
			spdk_nvmf_get_firmware_slot_log_page(req->data, offset, len);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		case SPDK_NVME_LOG_COMMAND_EFFECTS_LOG:
			spdk_nvmf_get_cmds_and_effects_log_page(req->data, offset, len);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		case SPDK_NVME_LOG_CHANGED_NS_LIST:
			spdk_nvmf_get_changed_ns_list_log_page(ctrlr, req->data, offset, len);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		default:
			goto invalid_log_page;
		}
	}

invalid_log_page:
	SPDK_ERRLOG("Unsupported Get Log Page 0x%02X\n", lid);
	response->status.sct = SPDK_NVME_SCT_GENERIC;
	response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
spdk_nvmf_ctrlr_identify_ns(struct spdk_nvmf_ctrlr *ctrlr,
			    struct spdk_nvme_cmd *cmd,
			    struct spdk_nvme_cpl *rsp,
			    struct spdk_nvme_ns_data *nsdata)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvmf_ns *ns;
	uint32_t max_num_blocks;

	if (cmd->nsid == 0 || cmd->nsid > subsystem->max_nsid) {
		SPDK_ERRLOG("Identify Namespace for invalid NSID %u\n", cmd->nsid);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ns = _spdk_nvmf_subsystem_get_ns(subsystem, cmd->nsid);
	if (ns == NULL || ns->bdev == NULL) {
		/*
		 * Inactive namespaces should return a zero filled data structure.
		 * The data buffer is already zeroed by spdk_nvmf_ctrlr_process_admin_cmd(),
		 * so we can just return early here.
		 */
		SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Identify Namespace for inactive NSID %u\n", cmd->nsid);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_SUCCESS;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	spdk_nvmf_bdev_ctrlr_identify_ns(ns, nsdata);

	/* Due to bug in the Linux kernel NVMe driver we have to set noiob no larger than mdts */
	max_num_blocks = ctrlr->admin_qpair->transport->opts.max_io_size /
			 (1U << nsdata->lbaf[nsdata->flbas.format].lbads);
	if (nsdata->noiob > max_num_blocks) {
		nsdata->noiob = max_num_blocks;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
spdk_nvmf_ctrlr_identify_ctrlr(struct spdk_nvmf_ctrlr *ctrlr, struct spdk_nvme_ctrlr_data *cdata)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvmf_transport *transport = ctrlr->admin_qpair->transport;

	/*
	 * Common fields for discovery and NVM subsystems
	 */
	spdk_strcpy_pad(cdata->fr, FW_VERSION, sizeof(cdata->fr), ' ');
	assert((transport->opts.max_io_size % 4096) == 0);
	cdata->mdts = spdk_u32log2(transport->opts.max_io_size / 4096);
	cdata->cntlid = ctrlr->cntlid;
	cdata->ver = ctrlr->vcprop.vs;
	cdata->lpa.edlp = 1;
	cdata->elpe = 127;
	cdata->maxcmd = transport->opts.max_queue_depth;
	cdata->sgls.supported = 1;
	cdata->sgls.keyed_sgl = 1;
	cdata->sgls.sgl_offset = 1;
	spdk_strcpy_pad(cdata->subnqn, subsystem->subnqn, sizeof(cdata->subnqn), '\0');

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "ctrlr data: maxcmd 0x%x\n", cdata->maxcmd);
	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "sgls data: 0x%x\n", from_le32(&cdata->sgls));

	/*
	 * NVM subsystem fields (reserved for discovery subsystems)
	 */
	if (subsystem->subtype == SPDK_NVMF_SUBTYPE_NVME) {
		spdk_strcpy_pad(cdata->mn, MODEL_NUMBER, sizeof(cdata->mn), ' ');
		spdk_strcpy_pad(cdata->sn, spdk_nvmf_subsystem_get_sn(subsystem), sizeof(cdata->sn), ' ');
		cdata->kas = 10;

		cdata->rab = 6;
		cdata->cmic.multi_port = 1;
		cdata->cmic.multi_host = 1;
		cdata->oaes.ns_attribute_notices = 1;
		cdata->ctratt.host_id_exhid_supported = 1;
		cdata->aerl = 0;
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

		cdata->nvmf_specific.ioccsz = sizeof(struct spdk_nvme_cmd) / 16;
		cdata->nvmf_specific.iorcsz = sizeof(struct spdk_nvme_cpl) / 16;
		cdata->nvmf_specific.icdoff = 0; /* offset starts directly after SQE */
		cdata->nvmf_specific.ctrattr.ctrlr_model = SPDK_NVMF_CTRLR_MODEL_DYNAMIC;
		cdata->nvmf_specific.msdbd = 1; /* target supports single SGL in capsule */

		/* TODO: this should be set by the transport */
		cdata->nvmf_specific.ioccsz += transport->opts.in_capsule_data_size / 16;

		cdata->oncs.dsm = spdk_nvmf_ctrlr_dsm_supported(ctrlr);
		cdata->oncs.write_zeroes = spdk_nvmf_ctrlr_write_zeroes_supported(ctrlr);

		SPDK_DEBUGLOG(SPDK_LOG_NVMF, "ext ctrlr data: ioccsz 0x%x\n",
			      cdata->nvmf_specific.ioccsz);
		SPDK_DEBUGLOG(SPDK_LOG_NVMF, "ext ctrlr data: iorcsz 0x%x\n",
			      cdata->nvmf_specific.iorcsz);
		SPDK_DEBUGLOG(SPDK_LOG_NVMF, "ext ctrlr data: icdoff 0x%x\n",
			      cdata->nvmf_specific.icdoff);
		SPDK_DEBUGLOG(SPDK_LOG_NVMF, "ext ctrlr data: ctrattr 0x%x\n",
			      *(uint8_t *)&cdata->nvmf_specific.ctrattr);
		SPDK_DEBUGLOG(SPDK_LOG_NVMF, "ext ctrlr data: msdbd 0x%x\n",
			      cdata->nvmf_specific.msdbd);
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
spdk_nvmf_ctrlr_identify_active_ns_list(struct spdk_nvmf_subsystem *subsystem,
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
spdk_nvmf_ctrlr_identify_ns_id_descriptor_list(
	struct spdk_nvmf_subsystem *subsystem,
	struct spdk_nvme_cmd *cmd,
	struct spdk_nvme_cpl *rsp,
	void *id_desc_list, size_t id_desc_list_size)
{
	struct spdk_nvmf_ns *ns;
	size_t buf_remain = id_desc_list_size;
	void *buf_ptr = id_desc_list;

	ns = _spdk_nvmf_subsystem_get_ns(subsystem, cmd->nsid);
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
	 * admin commands always get zeroed in spdk_nvmf_ctrlr_process_admin_cmd().
	 */

#undef ADD_ID_DESC

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
spdk_nvmf_ctrlr_identify(struct spdk_nvmf_request *req)
{
	uint8_t cns;
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;

	if (req->data == NULL || req->length < 4096) {
		SPDK_ERRLOG("identify command with invalid buffer\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	cns = cmd->cdw10 & 0xFF;

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
		return spdk_nvmf_ctrlr_identify_active_ns_list(subsystem, cmd, rsp, req->data);
	case SPDK_NVME_IDENTIFY_NS_ID_DESCRIPTOR_LIST:
		return spdk_nvmf_ctrlr_identify_ns_id_descriptor_list(subsystem, cmd, rsp, req->data, req->length);
	default:
		goto invalid_cns;
	}

invalid_cns:
	SPDK_ERRLOG("Identify command with unsupported CNS 0x%02x\n", cns);
	rsp->status.sct = SPDK_NVME_SCT_GENERIC;
	rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}


static struct spdk_nvmf_request *
spdk_nvmf_qpair_abort(struct spdk_nvmf_qpair *qpair, uint16_t cid)
{
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvmf_request *req;

	if (spdk_nvmf_qpair_is_admin_queue(qpair)) {
		if (ctrlr->aer_req && ctrlr->aer_req->cmd->nvme_cmd.cid == cid) {
			SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Aborting AER request\n");
			req = ctrlr->aer_req;
			ctrlr->aer_req = NULL;
			return req;
		}
	}

	/* TODO: track list of outstanding requests in qpair? */
	return NULL;
}

static void
spdk_nvmf_ctrlr_abort_done(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvmf_request *req = spdk_io_channel_iter_get_ctx(i);

	spdk_nvmf_request_complete(req);
}

static void
spdk_nvmf_ctrlr_abort_on_pg(struct spdk_io_channel_iter *i)
{
	struct spdk_nvmf_request *req = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct spdk_nvmf_poll_group *group = spdk_io_channel_get_ctx(ch);
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	uint16_t sqid = cmd->cdw10 & 0xFFFFu;
	struct spdk_nvmf_qpair *qpair;

	TAILQ_FOREACH(qpair, &group->qpairs, link) {
		if (qpair->ctrlr == req->qpair->ctrlr && qpair->qid == sqid) {
			struct spdk_nvmf_request *req_to_abort;
			uint16_t cid = cmd->cdw10 >> 16;

			/* Found the qpair */

			req_to_abort = spdk_nvmf_qpair_abort(qpair, cid);
			if (req_to_abort == NULL) {
				SPDK_DEBUGLOG(SPDK_LOG_NVMF, "cid %u not found\n", cid);
				rsp->status.sct = SPDK_NVME_SCT_GENERIC;
				rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
				spdk_for_each_channel_continue(i, -EINVAL);
				return;
			}

			/* Complete the request with aborted status */
			req_to_abort->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req_to_abort->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_ABORTED_BY_REQUEST;
			spdk_nvmf_request_complete(req_to_abort);

			SPDK_DEBUGLOG(SPDK_LOG_NVMF, "abort ctrlr=%p req=%p sqid=%u cid=%u successful\n",
				      qpair->ctrlr, req_to_abort, sqid, cid);
			rsp->cdw0 = 0; /* Command successfully aborted */
			rsp->status.sct = SPDK_NVME_SCT_GENERIC;
			rsp->status.sc = SPDK_NVME_SC_SUCCESS;
			/* Return -1 for the status so the iteration across threads stops. */
			spdk_for_each_channel_continue(i, -1);

		}
	}

	spdk_for_each_channel_continue(i, 0);
}

static int
spdk_nvmf_ctrlr_abort(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	rsp->cdw0 = 1; /* Command not aborted */
	rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
	rsp->status.sc = SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER;

	/* Send a message to each poll group, searching for this ctrlr, sqid, and command. */
	spdk_for_each_channel(req->qpair->ctrlr->subsys->tgt,
			      spdk_nvmf_ctrlr_abort_on_pg,
			      req,
			      spdk_nvmf_ctrlr_abort_done
			     );

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static int
get_features_generic(struct spdk_nvmf_request *req, uint32_t cdw0)
{
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	rsp->cdw0 = cdw0;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
spdk_nvmf_ctrlr_get_features(struct spdk_nvmf_request *req)
{
	uint8_t feature;
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	feature = cmd->cdw10 & 0xff; /* mask out the FID value */
	switch (feature) {
	case SPDK_NVME_FEAT_ARBITRATION:
		return get_features_generic(req, ctrlr->feat.arbitration.raw);
	case SPDK_NVME_FEAT_POWER_MANAGEMENT:
		return get_features_generic(req, ctrlr->feat.power_management.raw);
	case SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD:
		return spdk_nvmf_ctrlr_get_features_temperature_threshold(req);
	case SPDK_NVME_FEAT_ERROR_RECOVERY:
		return get_features_generic(req, ctrlr->feat.error_recovery.raw);
	case SPDK_NVME_FEAT_VOLATILE_WRITE_CACHE:
		return get_features_generic(req, ctrlr->feat.volatile_write_cache.raw);
	case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
		return get_features_generic(req, ctrlr->feat.number_of_queues.raw);
	case SPDK_NVME_FEAT_WRITE_ATOMICITY:
		return get_features_generic(req, ctrlr->feat.write_atomicity.raw);
	case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
		return get_features_generic(req, ctrlr->feat.async_event_configuration.raw);
	case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
		return get_features_generic(req, ctrlr->feat.keep_alive_timer.raw);
	case SPDK_NVME_FEAT_HOST_IDENTIFIER:
		return spdk_nvmf_ctrlr_get_features_host_identifier(req);
	default:
		SPDK_ERRLOG("Get Features command with unsupported feature ID 0x%02x\n", feature);
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

static int
spdk_nvmf_ctrlr_set_features(struct spdk_nvmf_request *req)
{
	uint8_t feature;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	feature = cmd->cdw10 & 0xff; /* mask out the FID value */
	switch (feature) {
	case SPDK_NVME_FEAT_ARBITRATION:
		return spdk_nvmf_ctrlr_set_features_arbitration(req);
	case SPDK_NVME_FEAT_POWER_MANAGEMENT:
		return spdk_nvmf_ctrlr_set_features_power_management(req);
	case SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD:
		return spdk_nvmf_ctrlr_set_features_temperature_threshold(req);
	case SPDK_NVME_FEAT_ERROR_RECOVERY:
		return spdk_nvmf_ctrlr_set_features_error_recovery(req);
	case SPDK_NVME_FEAT_VOLATILE_WRITE_CACHE:
		return spdk_nvmf_ctrlr_set_features_volatile_write_cache(req);
	case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
		return spdk_nvmf_ctrlr_set_features_number_of_queues(req);
	case SPDK_NVME_FEAT_WRITE_ATOMICITY:
		return spdk_nvmf_ctrlr_set_features_write_atomicity(req);
	case SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION:
		return spdk_nvmf_ctrlr_set_features_async_event_configuration(req);
	case SPDK_NVME_FEAT_KEEP_ALIVE_TIMER:
		return spdk_nvmf_ctrlr_set_features_keep_alive_timer(req);
	case SPDK_NVME_FEAT_HOST_IDENTIFIER:
		return spdk_nvmf_ctrlr_set_features_host_identifier(req);
	default:
		SPDK_ERRLOG("Set Features command with unsupported feature ID 0x%02x\n", feature);
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

static int
spdk_nvmf_ctrlr_keep_alive(struct spdk_nvmf_request *req)
{
	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Keep Alive\n");
	/*
	 * To handle keep alive just clear or reset the
	 * ctrlr based keep alive duration counter.
	 * When added, a separate timer based process
	 * will monitor if the time since last recorded
	 * keep alive has exceeded the max duration and
	 * take appropriate action.
	 */
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_ctrlr_process_admin_cmd(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	if (ctrlr == NULL) {
		SPDK_ERRLOG("Admin command sent before CONNECT\n");
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (ctrlr->vcprop.cc.bits.en != 1) {
		SPDK_ERRLOG("Admin command sent to disabled controller\n");
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (req->data && spdk_nvme_opc_get_data_transfer(cmd->opc) == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		memset(req->data, 0, req->length);
	}

	if (ctrlr->subsys->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY) {
		/* Discovery controllers only support Get Log Page and Identify */
		switch (cmd->opc) {
		case SPDK_NVME_OPC_IDENTIFY:
		case SPDK_NVME_OPC_GET_LOG_PAGE:
			break;
		default:
			goto invalid_opcode;
		}
	}

	switch (cmd->opc) {
	case SPDK_NVME_OPC_GET_LOG_PAGE:
		return spdk_nvmf_ctrlr_get_log_page(req);
	case SPDK_NVME_OPC_IDENTIFY:
		return spdk_nvmf_ctrlr_identify(req);
	case SPDK_NVME_OPC_ABORT:
		return spdk_nvmf_ctrlr_abort(req);
	case SPDK_NVME_OPC_GET_FEATURES:
		return spdk_nvmf_ctrlr_get_features(req);
	case SPDK_NVME_OPC_SET_FEATURES:
		return spdk_nvmf_ctrlr_set_features(req);
	case SPDK_NVME_OPC_ASYNC_EVENT_REQUEST:
		return spdk_nvmf_ctrlr_async_event_request(req);
	case SPDK_NVME_OPC_KEEP_ALIVE:
		return spdk_nvmf_ctrlr_keep_alive(req);

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
	SPDK_ERRLOG("Unsupported admin opcode 0x%x\n", cmd->opc);
	response->status.sct = SPDK_NVME_SCT_GENERIC;
	response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_ctrlr_process_fabrics_cmd(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_capsule_cmd *cap_hdr;

	cap_hdr = &req->cmd->nvmf_cmd;

	if (qpair->ctrlr == NULL) {
		/* No ctrlr established yet; the only valid command is Connect */
		if (cap_hdr->fctype == SPDK_NVMF_FABRIC_COMMAND_CONNECT) {
			return spdk_nvmf_ctrlr_connect(req);
		} else {
			SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Got fctype 0x%x, expected Connect\n",
				      cap_hdr->fctype);
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	} else if (spdk_nvmf_qpair_is_admin_queue(qpair)) {
		/*
		 * Controller session is established, and this is an admin queue.
		 * Disallow Connect and allow other fabrics commands.
		 */
		switch (cap_hdr->fctype) {
		case SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET:
			return spdk_nvmf_property_set(req);
		case SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET:
			return spdk_nvmf_property_get(req);
		default:
			SPDK_DEBUGLOG(SPDK_LOG_NVMF, "unknown fctype 0x%02x\n",
				      cap_hdr->fctype);
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	} else {
		/* Controller session is established, and this is an I/O queue */
		/* For now, no I/O-specific Fabrics commands are implemented (other than Connect) */
		SPDK_DEBUGLOG(SPDK_LOG_NVMF, "Unexpected I/O fctype 0x%x\n", cap_hdr->fctype);
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

int
spdk_nvmf_ctrlr_async_event_ns_notice(struct spdk_nvmf_ctrlr *ctrlr)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;
	union spdk_nvme_async_event_completion event = {0};

	/* Users may disable the event notification */
	if (!ctrlr->feat.async_event_configuration.bits.ns_attr_notice) {
		return 0;
	}

	event.bits.async_event_type = SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE;
	event.bits.async_event_info = SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGED;
	event.bits.log_page_identifier = SPDK_NVME_LOG_CHANGED_NS_LIST;

	/* If there is no outstanding AER request, queue the event.  Then
	 * if an AER is later submitted, this event can be sent as a
	 * response.
	 */
	if (!ctrlr->aer_req) {
		if (ctrlr->notice_event.bits.async_event_type ==
		    SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE) {
			return 0;
		}

		ctrlr->notice_event.raw = event.raw;
		return 0;
	}

	req = ctrlr->aer_req;
	rsp = &req->rsp->nvme_cpl;

	rsp->cdw0 = event.raw;

	spdk_nvmf_request_complete(req);
	ctrlr->aer_req = NULL;

	return 0;
}

void
spdk_nvmf_qpair_free_aer(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;

	if (!spdk_nvmf_qpair_is_admin_queue(qpair)) {
		return;
	}

	if (ctrlr->aer_req != NULL) {
		spdk_nvmf_request_free(ctrlr->aer_req);
		ctrlr->aer_req = NULL;
	}
}

void
spdk_nvmf_ctrlr_abort_aer(struct spdk_nvmf_ctrlr *ctrlr)
{
	if (!ctrlr->aer_req) {
		return;
	}

	spdk_nvmf_request_complete(ctrlr->aer_req);
	ctrlr->aer_req = NULL;
}
