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

#include "ctrlr.h"
#include "nvmf_internal.h"
#include "request.h"
#include "subsystem.h"
#include "transport.h"

#include "spdk/trace.h"
#include "spdk/nvme_spec.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

#define MIN_KEEP_ALIVE_TIMEOUT 10000

#define MODEL_NUMBER "SPDK bdev Controller"
#define FW_VERSION "FFFFFFFF"

static uint16_t spdk_nvmf_ctrlr_gen_cntlid(void);

static struct spdk_nvmf_ctrlr *
spdk_nvmf_ctrlr_create(struct spdk_nvmf_subsystem *subsystem,
		       struct spdk_nvmf_qpair *admin_qpair,
		       struct spdk_nvmf_fabric_connect_cmd *connect_cmd,
		       struct spdk_nvmf_fabric_connect_data *connect_data)
{
	struct spdk_nvmf_ctrlr *ctrlr;

	ctrlr = calloc(1, sizeof(*ctrlr));
	if (ctrlr == NULL) {
		SPDK_ERRLOG("Memory allocation failed\n");
		return NULL;
	}

	ctrlr->group = spdk_nvmf_transport_poll_group_create(admin_qpair->transport);
	if (ctrlr->group == NULL) {
		SPDK_ERRLOG("spdk_nvmf_transport_poll_group_create() failed\n");
		free(ctrlr);
		return NULL;
	}

	ctrlr->cntlid = spdk_nvmf_ctrlr_gen_cntlid();
	if (ctrlr->cntlid == 0) {
		/* Unable to get a cntlid */
		SPDK_ERRLOG("Reached max simultaneous ctrlrs\n");
		spdk_nvmf_transport_poll_group_destroy(ctrlr->group);
		free(ctrlr);
		return NULL;
	}

	TAILQ_INIT(&ctrlr->qpairs);
	ctrlr->kato = connect_cmd->kato;
	ctrlr->async_event_config.raw = 0;
	ctrlr->num_qpairs = 0;
	ctrlr->subsys = subsystem;
	ctrlr->max_qpairs_allowed = g_nvmf_tgt.max_qpairs_per_ctrlr;

	memcpy(ctrlr->hostid, connect_data->hostid, sizeof(ctrlr->hostid));

	if (spdk_nvmf_transport_poll_group_add(ctrlr->group, admin_qpair)) {
		spdk_nvmf_transport_poll_group_destroy(ctrlr->group);
		free(ctrlr);
		return NULL;
	}

	ctrlr->vcprop.cap.raw = 0;
	ctrlr->vcprop.cap.bits.cqr = 1; /* NVMe-oF specification required */
	ctrlr->vcprop.cap.bits.mqes = g_nvmf_tgt.max_queue_depth - 1; /* max queue depth */
	ctrlr->vcprop.cap.bits.ams = 0; /* optional arb mechanisms */
	ctrlr->vcprop.cap.bits.to = 1; /* ready timeout - 500 msec units */
	ctrlr->vcprop.cap.bits.dstrd = 0; /* fixed to 0 for NVMe-oF */
	ctrlr->vcprop.cap.bits.css_nvm = 1; /* NVM command set */
	ctrlr->vcprop.cap.bits.mpsmin = 0; /* 2 ^ (12 + mpsmin) == 4k */
	ctrlr->vcprop.cap.bits.mpsmax = 0; /* 2 ^ (12 + mpsmax) == 4k */

	/* Version Supported: 1.2.1 */
	ctrlr->vcprop.vs.bits.mjr = 1;
	ctrlr->vcprop.vs.bits.mnr = 2;
	ctrlr->vcprop.vs.bits.ter = 1;

	ctrlr->vcprop.cc.raw = 0;
	ctrlr->vcprop.cc.bits.en = 0; /* Init controller disabled */

	ctrlr->vcprop.csts.raw = 0;
	ctrlr->vcprop.csts.bits.rdy = 0; /* Init controller as not ready */

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "cap 0x%" PRIx64 "\n", ctrlr->vcprop.cap.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "vs 0x%x\n", ctrlr->vcprop.vs.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "cc 0x%x\n", ctrlr->vcprop.cc.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "csts 0x%x\n", ctrlr->vcprop.csts.raw);

	TAILQ_INSERT_TAIL(&subsystem->ctrlrs, ctrlr, link);
	return ctrlr;
}

static void ctrlr_destruct(struct spdk_nvmf_ctrlr *ctrlr)
{
	TAILQ_REMOVE(&ctrlr->subsys->ctrlrs, ctrlr, link);
	spdk_nvmf_transport_poll_group_destroy(ctrlr->group);
	free(ctrlr);
}

void
spdk_nvmf_ctrlr_destruct(struct spdk_nvmf_ctrlr *ctrlr)
{
	while (!TAILQ_EMPTY(&ctrlr->qpairs)) {
		struct spdk_nvmf_qpair *qpair = TAILQ_FIRST(&ctrlr->qpairs);

		TAILQ_REMOVE(&ctrlr->qpairs, qpair, link);
		ctrlr->num_qpairs--;
		spdk_nvmf_transport_qpair_fini(qpair);
	}

	ctrlr_destruct(ctrlr);
}

static void
invalid_connect_response(struct spdk_nvmf_fabric_connect_rsp *rsp, uint8_t iattr, uint16_t ipo)
{
	rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
	rsp->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
	rsp->status_code_specific.invalid.iattr = iattr;
	rsp->status_code_specific.invalid.ipo = ipo;
}

static uint16_t
spdk_nvmf_ctrlr_gen_cntlid(void)
{
	static uint16_t cntlid = 0; /* cntlid is static, so its value is preserved */
	struct spdk_nvmf_subsystem *subsystem;
	uint16_t count;

	count = UINT16_MAX - 1;
	do {
		/* cntlid is an unsigned 16-bit integer, so let it overflow
		 * back to 0 if necessary.
		 */
		cntlid++;
		if (cntlid == 0) {
			/* 0 is not a valid cntlid because it is the reserved value in the RDMA
			 * private data for cntlid. This is the value sent by pre-NVMe-oF 1.1
			 * initiators.
			 */
			cntlid++;
		}

		/* Check if a subsystem with this cntlid currently exists. This could
		 * happen for a very long-lived ctrlr on a target with many short-lived
		 * ctrlrs, where cntlid wraps around.
		 */
		subsystem = spdk_nvmf_find_subsystem_with_cntlid(cntlid);

		count--;

	} while (subsystem != NULL && count > 0);

	if (count == 0) {
		return 0;
	}

	return cntlid;
}

void
spdk_nvmf_ctrlr_connect(struct spdk_nvmf_qpair *qpair,
			struct spdk_nvmf_fabric_connect_cmd *cmd,
			struct spdk_nvmf_fabric_connect_data *data,
			struct spdk_nvmf_fabric_connect_rsp *rsp)
{
	struct spdk_nvmf_ctrlr *ctrlr;
	struct spdk_nvmf_subsystem *subsystem;

#define INVALID_CONNECT_CMD(field) invalid_connect_response(rsp, 0, offsetof(struct spdk_nvmf_fabric_connect_cmd, field))
#define INVALID_CONNECT_DATA(field) invalid_connect_response(rsp, 1, offsetof(struct spdk_nvmf_fabric_connect_data, field))

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "recfmt 0x%x qid %u sqsize %u\n",
		      cmd->recfmt, cmd->qid, cmd->sqsize);

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Connect data:\n");
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "  cntlid:  0x%04x\n", data->cntlid);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "  hostid: %08x-%04x-%04x-%02x%02x-%04x%08x ***\n",
		      ntohl(*(uint32_t *)&data->hostid[0]),
		      ntohs(*(uint16_t *)&data->hostid[4]),
		      ntohs(*(uint16_t *)&data->hostid[6]),
		      data->hostid[8],
		      data->hostid[9],
		      ntohs(*(uint16_t *)&data->hostid[10]),
		      ntohl(*(uint32_t *)&data->hostid[12]));
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "  subnqn: \"%s\"\n", data->subnqn);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "  hostnqn: \"%s\"\n", data->hostnqn);

	subsystem = spdk_nvmf_find_subsystem(data->subnqn);
	if (subsystem == NULL) {
		SPDK_ERRLOG("Could not find subsystem '%s'\n", data->subnqn);
		INVALID_CONNECT_DATA(subnqn);
		return;
	}

	/*
	 * SQSIZE is a 0-based value, so it must be at least 1 (minimum queue depth is 2) and
	 *  strictly less than max_queue_depth.
	 */
	if (cmd->sqsize == 0 || cmd->sqsize >= g_nvmf_tgt.max_queue_depth) {
		SPDK_ERRLOG("Invalid SQSIZE %u (min 1, max %u)\n",
			    cmd->sqsize, g_nvmf_tgt.max_queue_depth - 1);
		INVALID_CONNECT_CMD(sqsize);
		return;
	}
	qpair->sq_head_max = cmd->sqsize;
	qpair->qid = cmd->qid;

	if (cmd->qid == 0) {
		qpair->type = QPAIR_TYPE_AQ;

		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Connect Admin Queue for controller ID 0x%x\n", data->cntlid);

		if (data->cntlid != 0xFFFF) {
			/* This NVMf target only supports dynamic mode. */
			SPDK_ERRLOG("The NVMf target only supports dynamic mode (CNTLID = 0x%x).\n", data->cntlid);
			INVALID_CONNECT_DATA(cntlid);
			return;
		}

		/* Establish a new ctrlr */
		ctrlr = spdk_nvmf_ctrlr_create(subsystem, qpair, cmd, data);
		if (!ctrlr) {
			SPDK_ERRLOG("spdk_nvmf_ctrlr_create() failed\n");
			rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return;
		}
	} else {
		struct spdk_nvmf_ctrlr *tmp;

		qpair->type = QPAIR_TYPE_IOQ;
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Connect I/O Queue for controller id 0x%x\n", data->cntlid);

		ctrlr = NULL;
		TAILQ_FOREACH(tmp, &subsystem->ctrlrs, link) {
			if (tmp->cntlid == data->cntlid) {
				ctrlr = tmp;
				break;
			}
		}
		if (ctrlr == NULL) {
			SPDK_ERRLOG("Unknown controller ID 0x%x\n", data->cntlid);
			INVALID_CONNECT_DATA(cntlid);
			return;
		}

		if (!ctrlr->vcprop.cc.bits.en) {
			SPDK_ERRLOG("Got I/O connect before ctrlr was enabled\n");
			INVALID_CONNECT_CMD(qid);
			return;
		}

		if (1u << ctrlr->vcprop.cc.bits.iosqes != sizeof(struct spdk_nvme_cmd)) {
			SPDK_ERRLOG("Got I/O connect with invalid IOSQES %u\n",
				    ctrlr->vcprop.cc.bits.iosqes);
			INVALID_CONNECT_CMD(qid);
			return;
		}

		if (1u << ctrlr->vcprop.cc.bits.iocqes != sizeof(struct spdk_nvme_cpl)) {
			SPDK_ERRLOG("Got I/O connect with invalid IOCQES %u\n",
				    ctrlr->vcprop.cc.bits.iocqes);
			INVALID_CONNECT_CMD(qid);
			return;
		}

		/* check if we would exceed ctrlr connection limit */
		if (ctrlr->num_qpairs >= ctrlr->max_qpairs_allowed) {
			SPDK_ERRLOG("qpair limit %d\n", ctrlr->num_qpairs);
			rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
			rsp->status.sc = SPDK_NVMF_FABRIC_SC_CONTROLLER_BUSY;
			return;
		}

		if (spdk_nvmf_transport_poll_group_add(ctrlr->group, qpair)) {
			INVALID_CONNECT_CMD(qid);
			return;
		}
	}

	ctrlr->num_qpairs++;
	TAILQ_INSERT_HEAD(&ctrlr->qpairs, qpair, link);
	qpair->ctrlr = ctrlr;

	rsp->status.sc = SPDK_NVME_SC_SUCCESS;
	rsp->status_code_specific.success.cntlid = ctrlr->cntlid;
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "connect capsule response: cntlid = 0x%04x\n",
		      rsp->status_code_specific.success.cntlid);
}

void
spdk_nvmf_ctrlr_disconnect(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;

	assert(ctrlr != NULL);
	ctrlr->num_qpairs--;
	TAILQ_REMOVE(&ctrlr->qpairs, qpair, link);

	spdk_nvmf_transport_poll_group_remove(ctrlr->group, qpair);
	spdk_nvmf_transport_qpair_fini(qpair);

	if (ctrlr->num_qpairs == 0) {
		ctrlr_destruct(ctrlr);
	}
}

struct spdk_nvmf_qpair *
spdk_nvmf_ctrlr_get_qpair(struct spdk_nvmf_ctrlr *ctrlr, uint16_t qid)
{
	struct spdk_nvmf_qpair *qpair;

	TAILQ_FOREACH(qpair, &ctrlr->qpairs, link) {
		if (qpair->qid == qid) {
			return qpair;
		}
	}
	return NULL;
}

struct spdk_nvmf_request *
spdk_nvmf_qpair_get_request(struct spdk_nvmf_qpair *qpair, uint16_t cid)
{
	/* TODO: track list of outstanding requests in qpair? */
	return NULL;
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

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "cur CC: 0x%08x\n", ctrlr->vcprop.cc.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "new CC: 0x%08x\n", cc.raw);

	/*
	 * Calculate which bits changed between the current and new CC.
	 * Mark each bit as 0 once it is handled to determine if any unhandled bits were changed.
	 */
	diff.raw = cc.raw ^ ctrlr->vcprop.cc.raw;

	if (diff.bits.en) {
		if (cc.bits.en) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Property Set CC Enable!\n");
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
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Property Set CC Shutdown %u%ub!\n",
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
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Prop Set IOSQES = %u (%u bytes)\n",
			      cc.bits.iosqes, 1u << cc.bits.iosqes);
		ctrlr->vcprop.cc.bits.iosqes = cc.bits.iosqes;
		diff.bits.iosqes = 0;
	}

	if (diff.bits.iocqes) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Prop Set IOCQES = %u (%u bytes)\n",
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

void
spdk_nvmf_property_get(struct spdk_nvmf_ctrlr *ctrlr,
		       struct spdk_nvmf_fabric_prop_get_cmd *cmd,
		       struct spdk_nvmf_fabric_prop_get_rsp *response)
{
	const struct nvmf_prop *prop;

	response->status.sc = 0;
	response->value.u64 = 0;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "size %d, offset 0x%x\n",
		      cmd->attrib.size, cmd->ofst);

	if (cmd->attrib.size != SPDK_NVMF_PROP_SIZE_4 &&
	    cmd->attrib.size != SPDK_NVMF_PROP_SIZE_8) {
		SPDK_ERRLOG("Invalid size value %d\n", cmd->attrib.size);
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return;
	}

	prop = find_prop(cmd->ofst);
	if (prop == NULL || prop->get_cb == NULL) {
		/* Reserved properties return 0 when read */
		return;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "name: %s\n", prop->name);
	if (cmd->attrib.size != prop->size) {
		SPDK_ERRLOG("offset 0x%x size mismatch: cmd %u, prop %u\n",
			    cmd->ofst, cmd->attrib.size, prop->size);
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return;
	}

	response->value.u64 = prop->get_cb(ctrlr);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "response value: 0x%" PRIx64 "\n", response->value.u64);
}

void
spdk_nvmf_property_set(struct spdk_nvmf_ctrlr *ctrlr,
		       struct spdk_nvmf_fabric_prop_set_cmd *cmd,
		       struct spdk_nvme_cpl *response)
{
	const struct nvmf_prop *prop;
	uint64_t value;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "size %d, offset 0x%x, value 0x%" PRIx64 "\n",
		      cmd->attrib.size, cmd->ofst, cmd->value.u64);

	prop = find_prop(cmd->ofst);
	if (prop == NULL || prop->set_cb == NULL) {
		SPDK_ERRLOG("Invalid offset 0x%x\n", cmd->ofst);
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "name: %s\n", prop->name);
	if (cmd->attrib.size != prop->size) {
		SPDK_ERRLOG("offset 0x%x size mismatch: cmd %u, prop %u\n",
			    cmd->ofst, cmd->attrib.size, prop->size);
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return;
	}

	value = cmd->value.u64;
	if (prop->size == SPDK_NVMF_PROP_SIZE_4) {
		value = (uint32_t)value;
	}

	if (!prop->set_cb(ctrlr, value)) {
		SPDK_ERRLOG("prop set_cb failed\n");
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return;
	}
}

int
spdk_nvmf_ctrlr_poll(struct spdk_nvmf_ctrlr *ctrlr)
{
	struct spdk_nvmf_qpair		*qpair, *tmp;
	struct spdk_nvmf_subsystem 	*subsys = ctrlr->subsys;

	if (subsys->is_removed) {
		if (ctrlr->aer_req) {
			struct spdk_nvmf_request *aer = ctrlr->aer_req;

			aer->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			aer->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
			aer->rsp->nvme_cpl.status.dnr = 0;
			spdk_nvmf_request_complete(aer);
			ctrlr->aer_req = NULL;
		}
	}

	TAILQ_FOREACH_SAFE(qpair, &ctrlr->qpairs, link, tmp) {
		if (spdk_nvmf_transport_qpair_poll(qpair) < 0) {
			SPDK_ERRLOG("Transport poll failed for qpair %p; closing connection\n", qpair);
			spdk_nvmf_ctrlr_disconnect(qpair);
		}
	}

	return 0;
}

int
spdk_nvmf_ctrlr_set_features_host_identifier(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	SPDK_ERRLOG("Set Features - Host Identifier not allowed\n");
	response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_ctrlr_get_features_host_identifier(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Get Features - Host Identifier\n");
	if (!(cmd->cdw11 & 1)) {
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

int
spdk_nvmf_ctrlr_set_features_keep_alive_timer(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Set Features - Keep Alive Timer (%u ms)\n", cmd->cdw11);

	if (cmd->cdw11 == 0) {
		rsp->status.sc = SPDK_NVME_SC_KEEP_ALIVE_INVALID;
	} else if (cmd->cdw11 < MIN_KEEP_ALIVE_TIMEOUT) {
		ctrlr->kato = MIN_KEEP_ALIVE_TIMEOUT;
	} else {
		ctrlr->kato = cmd->cdw11;
	}

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Set Features - Keep Alive Timer set to %u ms\n", ctrlr->kato);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_ctrlr_get_features_keep_alive_timer(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Get Features - Keep Alive Timer\n");
	rsp->cdw0 = ctrlr->kato;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_ctrlr_set_features_number_of_queues(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint32_t nr_io_queues;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Set Features - Number of Queues, cdw11 0x%x\n",
		      req->cmd->nvme_cmd.cdw11);

	/* Extra 1 connection for Admin queue */
	nr_io_queues = ctrlr->max_qpairs_allowed - 1;

	/* verify that the contoller is ready to process commands */
	if (ctrlr->num_qpairs > 1) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Queue pairs already active!\n");
		rsp->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
	} else {
		/* Number of IO queues has a zero based value */
		rsp->cdw0 = ((nr_io_queues - 1) << 16) |
			    (nr_io_queues - 1);
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_ctrlr_get_features_number_of_queues(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint32_t nr_io_queues;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Get Features - Number of Queues\n");

	nr_io_queues = ctrlr->max_qpairs_allowed - 1;

	/* Number of IO queues has a zero based value */
	rsp->cdw0 = ((nr_io_queues - 1) << 16) |
		    (nr_io_queues - 1);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_ctrlr_set_features_async_event_configuration(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Set Features - Async Event Configuration, cdw11 0x%08x\n",
		      cmd->cdw11);
	ctrlr->async_event_config.raw = cmd->cdw11;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_ctrlr_get_features_async_event_configuration(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Get Features - Async Event Configuration\n");
	rsp->cdw0 = ctrlr->async_event_config.raw;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_ctrlr_async_event_request(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Async Event Request\n");

	/* Only one asynchronous event is supported for now */
	if (ctrlr->aer_req != NULL) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "AERL exceeded\n");
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ctrlr->aer_req = req;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
spdk_nvmf_ctrlr_get_log_page(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_subsystem *subsystem = req->qpair->ctrlr->subsys;
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

	memset(req->data, 0, req->length);

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
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Get log page: LID=0x%02X offset=0x%" PRIx64 " len=0x%" PRIx64 "\n",
		      lid, offset, len);

	if (subsystem->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY) {
		switch (lid) {
		case SPDK_NVME_LOG_DISCOVERY:
			spdk_nvmf_get_discovery_log_page(req->data, offset, len);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		default:
			goto invalid_log_page;
		}
	} else {
		switch (lid) {
		case SPDK_NVME_LOG_ERROR:
		case SPDK_NVME_LOG_HEALTH_INFORMATION:
		case SPDK_NVME_LOG_FIRMWARE_SLOT:
			/* TODO: actually fill out log page data */
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
spdk_nvmf_ctrlr_identify_ns(struct spdk_nvmf_subsystem *subsystem,
			    struct spdk_nvme_cmd *cmd,
			    struct spdk_nvme_cpl *rsp,
			    struct spdk_nvme_ns_data *nsdata)
{
	struct spdk_bdev *bdev;

	if (cmd->nsid > subsystem->dev.max_nsid || cmd->nsid == 0) {
		SPDK_ERRLOG("Identify Namespace for invalid NSID %u\n", cmd->nsid);
		rsp->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	bdev = subsystem->dev.ns_list[cmd->nsid - 1];

	if (bdev == NULL) {
		memset(nsdata, 0, sizeof(*nsdata));
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return spdk_nvmf_bdev_ctrlr_identify_ns(bdev, nsdata);
}

static int
spdk_nvmf_ctrlr_identify_ctrlr(struct spdk_nvmf_ctrlr *ctrlr, struct spdk_nvme_ctrlr_data *cdata)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;

	/*
	 * Common fields for discovery and NVM subsystems
	 */
	spdk_strcpy_pad(cdata->fr, FW_VERSION, sizeof(cdata->fr), ' ');
	assert((g_nvmf_tgt.max_io_size % 4096) == 0);
	cdata->mdts = spdk_u32log2(g_nvmf_tgt.max_io_size / 4096);
	cdata->cntlid = ctrlr->cntlid;
	cdata->ver = ctrlr->vcprop.vs;
	cdata->lpa.edlp = 1;
	cdata->elpe = 127;
	cdata->maxcmd = g_nvmf_tgt.max_queue_depth;
	cdata->sgls.supported = 1;
	cdata->sgls.keyed_sgl = 1;
	cdata->sgls.sgl_offset = 1;
	spdk_strcpy_pad(cdata->subnqn, subsystem->subnqn, sizeof(cdata->subnqn), '\0');

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "ctrlr data: maxcmd 0x%x\n", cdata->maxcmd);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "sgls data: 0x%x\n", *(uint32_t *)&cdata->sgls);

	/*
	 * NVM subsystem fields (reserved for discovery subsystems)
	 */
	if (subsystem->subtype == SPDK_NVMF_SUBTYPE_NVME) {
		spdk_strcpy_pad(cdata->mn, MODEL_NUMBER, sizeof(cdata->mn), ' ');
		spdk_strcpy_pad(cdata->sn, spdk_nvmf_subsystem_get_sn(subsystem), sizeof(cdata->sn), ' ');
		cdata->aerl = 0;
		cdata->kas = 10;

		cdata->rab = 6;
		cdata->ctratt.host_id_exhid_supported = 1;
		cdata->aerl = 0;
		cdata->frmw.slot1_ro = 1;
		cdata->frmw.num_slots = 1;

		cdata->sqes.min = 6;
		cdata->sqes.max = 6;
		cdata->cqes.min = 4;
		cdata->cqes.max = 4;
		cdata->nn = subsystem->dev.max_nsid;
		cdata->vwc.present = 1;

		cdata->nvmf_specific.ioccsz = sizeof(struct spdk_nvme_cmd) / 16;
		cdata->nvmf_specific.iorcsz = sizeof(struct spdk_nvme_cpl) / 16;
		cdata->nvmf_specific.icdoff = 0; /* offset starts directly after SQE */
		cdata->nvmf_specific.ctrattr.ctrlr_model = SPDK_NVMF_CTRLR_MODEL_DYNAMIC;
		cdata->nvmf_specific.msdbd = 1; /* target supports single SGL in capsule */

		/* TODO: this should be set by the transport */
		cdata->nvmf_specific.ioccsz += g_nvmf_tgt.in_capsule_data_size / 16;

		cdata->oncs.dsm = spdk_nvmf_ctrlr_dsm_supported(ctrlr);

		SPDK_TRACELOG(SPDK_TRACE_NVMF, "ext ctrlr data: ioccsz 0x%x\n",
			      cdata->nvmf_specific.ioccsz);
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "ext ctrlr data: iorcsz 0x%x\n",
			      cdata->nvmf_specific.iorcsz);
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "ext ctrlr data: icdoff 0x%x\n",
			      cdata->nvmf_specific.icdoff);
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "ext ctrlr data: ctrattr 0x%x\n",
			      *(uint8_t *)&cdata->nvmf_specific.ctrattr);
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "ext ctrlr data: msdbd 0x%x\n",
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
	uint32_t i, num_ns, count = 0;

	if (cmd->nsid >= 0xfffffffeUL) {
		SPDK_ERRLOG("Identify Active Namespace List with invalid NSID %u\n", cmd->nsid);
		rsp->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	num_ns = subsystem->dev.max_nsid;

	for (i = 1; i <= num_ns; i++) {
		if (i <= cmd->nsid) {
			continue;
		}
		if (subsystem->dev.ns_list[i - 1] == NULL) {
			continue;
		}
		ns_list->ns_list[count++] = i;
		if (count == SPDK_COUNTOF(ns_list->ns_list)) {
			break;
		}
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
spdk_nvmf_ctrlr_identify(struct spdk_nvmf_request *req)
{
	uint8_t cns;
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;

	memset(req->data, 0, req->length);

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
		return spdk_nvmf_ctrlr_identify_ns(subsystem, cmd, rsp, req->data);
	case SPDK_NVME_IDENTIFY_CTRLR:
		return spdk_nvmf_ctrlr_identify_ctrlr(ctrlr, req->data);
	case SPDK_NVME_IDENTIFY_ACTIVE_NS_LIST:
		return spdk_nvmf_ctrlr_identify_active_ns_list(subsystem, cmd, rsp, req->data);
	default:
		goto invalid_cns;
	}

invalid_cns:
	SPDK_ERRLOG("Identify command with unsupported CNS 0x%02x\n", cns);
	rsp->status.sct = SPDK_NVME_SCT_GENERIC;
	rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}
