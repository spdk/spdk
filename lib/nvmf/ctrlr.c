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
#include "spdk/util.h"

#include "spdk_internal/log.h"

#define MIN_KEEP_ALIVE_TIMEOUT 10000

static void
nvmf_init_discovery_ctrlr_properties(struct spdk_nvmf_ctrlr *ctrlr)
{
	ctrlr->vcdata.maxcmd = g_nvmf_tgt.max_queue_depth;
	/* extended data for get log page supportted */
	ctrlr->vcdata.lpa.edlp = 1;
	ctrlr->vcdata.cntlid = ctrlr->cntlid;
	ctrlr->vcdata.nvmf_specific.ioccsz = sizeof(struct spdk_nvme_cmd) / 16;
	ctrlr->vcdata.nvmf_specific.iorcsz = sizeof(struct spdk_nvme_cpl) / 16;
	ctrlr->vcdata.nvmf_specific.icdoff = 0; /* offset starts directly after SQE */
	ctrlr->vcdata.nvmf_specific.ctrattr.ctrlr_model = SPDK_NVMF_CTRLR_MODEL_DYNAMIC;
	ctrlr->vcdata.nvmf_specific.msdbd = 1; /* target supports single SGL in capsule */
	ctrlr->vcdata.sgls.keyed_sgl = 1;
	ctrlr->vcdata.sgls.sgl_offset = 1;

	strncpy((char *)ctrlr->vcdata.subnqn, SPDK_NVMF_DISCOVERY_NQN, sizeof(ctrlr->vcdata.subnqn));

	/* Properties */
	ctrlr->vcprop.cap.raw = 0;
	ctrlr->vcprop.cap.bits.cqr = 1;	/* NVMF specification required */
	ctrlr->vcprop.cap.bits.mqes = (ctrlr->vcdata.maxcmd - 1);	/* max queue depth */
	ctrlr->vcprop.cap.bits.ams = 0;	/* optional arb mechanisms */
	ctrlr->vcprop.cap.bits.dstrd = 0;	/* fixed to 0 for NVMf */
	ctrlr->vcprop.cap.bits.css_nvm = 1; /* NVM command set */
	ctrlr->vcprop.cap.bits.mpsmin = 0; /* 2 ^ 12 + mpsmin == 4k */
	ctrlr->vcprop.cap.bits.mpsmax = 0; /* 2 ^ 12 + mpsmax == 4k */

	/* Version Supported: 1.2.1 */
	ctrlr->vcprop.vs.bits.mjr = 1;
	ctrlr->vcprop.vs.bits.mnr = 2;
	ctrlr->vcprop.vs.bits.ter = 1;
	ctrlr->vcdata.ver = ctrlr->vcprop.vs;

	ctrlr->vcprop.cc.raw = 0;

	ctrlr->vcprop.csts.raw = 0;
	ctrlr->vcprop.csts.bits.rdy = 0; /* Init controller as not ready */
}

static void
nvmf_init_nvme_ctrlr_properties(struct spdk_nvmf_ctrlr *ctrlr)
{
	assert((g_nvmf_tgt.max_io_size % 4096) == 0);

	/* Init the controller details */
	ctrlr->subsys->ops->ctrlr_get_data(ctrlr);

	ctrlr->vcdata.aerl = 0;
	ctrlr->vcdata.cntlid = ctrlr->cntlid;
	ctrlr->vcdata.kas = 10;
	ctrlr->vcdata.maxcmd = g_nvmf_tgt.max_queue_depth;
	ctrlr->vcdata.mdts = spdk_u32log2(g_nvmf_tgt.max_io_size / 4096);
	ctrlr->vcdata.sgls.keyed_sgl = 1;
	ctrlr->vcdata.sgls.sgl_offset = 1;

	ctrlr->vcdata.nvmf_specific.ioccsz = sizeof(struct spdk_nvme_cmd) / 16;
	ctrlr->vcdata.nvmf_specific.iorcsz = sizeof(struct spdk_nvme_cpl) / 16;
	ctrlr->vcdata.nvmf_specific.icdoff = 0; /* offset starts directly after SQE */
	ctrlr->vcdata.nvmf_specific.ctrattr.ctrlr_model = SPDK_NVMF_CTRLR_MODEL_DYNAMIC;
	ctrlr->vcdata.nvmf_specific.msdbd = 1; /* target supports single SGL in capsule */

	/* TODO: this should be set by the transport */
	ctrlr->vcdata.nvmf_specific.ioccsz += g_nvmf_tgt.in_capsule_data_size / 16;

	strncpy((char *)ctrlr->vcdata.subnqn, ctrlr->subsys->subnqn, sizeof(ctrlr->vcdata.subnqn));

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	ctrlr data: maxcmd %x\n",
		      ctrlr->vcdata.maxcmd);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	ext ctrlr data: ioccsz %x\n",
		      ctrlr->vcdata.nvmf_specific.ioccsz);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	ext ctrlr data: iorcsz %x\n",
		      ctrlr->vcdata.nvmf_specific.iorcsz);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	ext ctrlr data: icdoff %x\n",
		      ctrlr->vcdata.nvmf_specific.icdoff);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	ext ctrlr data: ctrattr %x\n",
		      *(uint8_t *)&ctrlr->vcdata.nvmf_specific.ctrattr);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	ext ctrlr data: msdbd %x\n",
		      ctrlr->vcdata.nvmf_specific.msdbd);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	sgls data: 0x%x\n",
		      *(uint32_t *)&ctrlr->vcdata.sgls);

	ctrlr->vcprop.cap.raw = 0;
	ctrlr->vcprop.cap.bits.cqr = 1;
	ctrlr->vcprop.cap.bits.mqes = (ctrlr->vcdata.maxcmd - 1);	/* max queue depth */
	ctrlr->vcprop.cap.bits.ams = 0;	/* optional arb mechanisms */
	ctrlr->vcprop.cap.bits.to = 1;	/* ready timeout - 500 msec units */
	ctrlr->vcprop.cap.bits.dstrd = 0;	/* fixed to 0 for NVMf */
	ctrlr->vcprop.cap.bits.css_nvm = 1; /* NVM command set */
	ctrlr->vcprop.cap.bits.mpsmin = 0; /* 2 ^ 12 + mpsmin == 4k */
	ctrlr->vcprop.cap.bits.mpsmax = 0; /* 2 ^ 12 + mpsmax == 4k */

	/* Report at least version 1.2.1 */
	if (ctrlr->vcprop.vs.raw < SPDK_NVME_VERSION(1, 2, 1)) {
		ctrlr->vcprop.vs.bits.mjr = 1;
		ctrlr->vcprop.vs.bits.mnr = 2;
		ctrlr->vcprop.vs.bits.ter = 1;
		ctrlr->vcdata.ver = ctrlr->vcprop.vs;
	}

	ctrlr->vcprop.cc.raw = 0;
	ctrlr->vcprop.cc.bits.en = 0; /* Init controller disabled */

	ctrlr->vcprop.csts.raw = 0;
	ctrlr->vcprop.csts.bits.rdy = 0; /* Init controller as not ready */

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	cap %" PRIx64 "\n",
		      ctrlr->vcprop.cap.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	vs %x\n", ctrlr->vcprop.vs.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	cc %x\n", ctrlr->vcprop.cc.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	csts %x\n",
		      ctrlr->vcprop.csts.raw);
}

static void ctrlr_destruct(struct spdk_nvmf_ctrlr *ctrlr)
{
	TAILQ_REMOVE(&ctrlr->subsys->ctrlrs, ctrlr, link);
	ctrlr->transport->ctrlr_fini(ctrlr);
}

void
spdk_nvmf_ctrlr_destruct(struct spdk_nvmf_ctrlr *ctrlr)
{
	while (!TAILQ_EMPTY(&ctrlr->qpairs)) {
		struct spdk_nvmf_qpair *qpair = TAILQ_FIRST(&ctrlr->qpairs);

		TAILQ_REMOVE(&ctrlr->qpairs, qpair, link);
		ctrlr->num_qpairs--;
		qpair->transport->qpair_fini(qpair);
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
		ctrlr = qpair->transport->ctrlr_init();
		if (ctrlr == NULL) {
			SPDK_ERRLOG("Memory allocation failure\n");
			rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return;
		}

		TAILQ_INIT(&ctrlr->qpairs);

		ctrlr->cntlid = spdk_nvmf_ctrlr_gen_cntlid();
		if (ctrlr->cntlid == 0) {
			/* Unable to get a cntlid */
			SPDK_ERRLOG("Reached max simultaneous ctrlrs\n");
			rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return;
		}

		ctrlr->kato = cmd->kato;
		ctrlr->async_event_config.raw = 0;
		ctrlr->num_qpairs = 0;
		ctrlr->subsys = subsystem;
		ctrlr->max_qpairs_allowed = g_nvmf_tgt.max_qpairs_per_ctrlr;

		memcpy(ctrlr->hostid, data->hostid, sizeof(ctrlr->hostid));

		if (qpair->transport->ctrlr_add_qpair(ctrlr, qpair)) {
			rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			qpair->transport->ctrlr_fini(ctrlr);
			free(ctrlr);
			return;
		}

		if (subsystem->subtype == SPDK_NVMF_SUBTYPE_NVME) {
			nvmf_init_nvme_ctrlr_properties(ctrlr);
		} else {
			nvmf_init_discovery_ctrlr_properties(ctrlr);
		}

		TAILQ_INSERT_TAIL(&subsystem->ctrlrs, ctrlr, link);
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

		if (qpair->transport->ctrlr_add_qpair(ctrlr, qpair)) {
			INVALID_CONNECT_CMD(qid);
			return;
		}
	}

	ctrlr->num_qpairs++;
	TAILQ_INSERT_HEAD(&ctrlr->qpairs, qpair, link);
	qpair->ctrlr = ctrlr;

	rsp->status.sc = SPDK_NVME_SC_SUCCESS;
	rsp->status_code_specific.success.cntlid = ctrlr->vcdata.cntlid;
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

	qpair->transport->ctrlr_remove_qpair(ctrlr, qpair);
	qpair->transport->qpair_fini(qpair);

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
		if (qpair->transport->qpair_poll(qpair) < 0) {
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

	assert(ctrlr->vcdata.aerl + 1 == 1);
	if (ctrlr->aer_req != NULL) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "AERL exceeded\n");
		rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		rsp->status.sc = SPDK_NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ctrlr->aer_req = req;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}
