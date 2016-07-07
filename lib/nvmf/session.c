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

#include <string.h>

#include "session.h"
#include "nvmf_internal.h"
#include "subsystem.h"
#include "spdk/log.h"
#include "spdk/trace.h"
#include "spdk/nvme_spec.h"

static void
nvmf_init_discovery_session_properties(struct nvmf_session *session)
{
	struct spdk_nvmf_extended_identify_ctrlr_data *nvmfdata;

	session->vcdata.maxcmd = SPDK_NVMF_DEFAULT_MAX_QUEUE_DEPTH;
	/* extended data for get log page supportted */
	session->vcdata.lpa.edlp = 1;
	session->vcdata.cntlid = 0; /* There is one controller per subsystem, so its id is 0 */
	nvmfdata = (struct spdk_nvmf_extended_identify_ctrlr_data *)session->vcdata.nvmf_specific;
	nvmfdata->ioccsz = (NVMF_H2C_MAX_MSG / 16);
	nvmfdata->iorcsz = (NVMF_C2H_MAX_MSG / 16);
	nvmfdata->icdoff = 0; /* offset starts directly after SQE */
	nvmfdata->ctrattr = 0; /* dynamic controller model */
	nvmfdata->msdbd = 1; /* target supports single SGL in capsule */
	session->vcdata.sgls.keyed_sgl = 1;
	session->vcdata.sgls.sgl_offset = 1;

	/* Properties */
	session->vcprop.cap.raw = 0;
	session->vcprop.cap.bits.cqr = 1;	/* NVMF specification required */
	session->vcprop.cap.bits.mqes = (session->vcdata.maxcmd - 1);	/* max queue depth */
	session->vcprop.cap.bits.ams = 0;	/* optional arb mechanisms */
	session->vcprop.cap.bits.dstrd = 0;	/* fixed to 0 for NVMf */
	session->vcprop.cap.bits.css_nvm = 1; /* NVM command set */
	session->vcprop.cap.bits.mpsmin = 0; /* 2 ^ 12 + mpsmin == 4k */
	session->vcprop.cap.bits.mpsmax = 0; /* 2 ^ 12 + mpsmax == 4k */

	/* Version Supported: 1.0 */
	session->vcprop.vs.bits.mjr = 1;
	session->vcprop.vs.bits.mnr = 0;
	session->vcprop.vs.bits.ter = 0;

	session->vcprop.cc.raw = 0;

	session->vcprop.csts.raw = 0;
	session->vcprop.csts.bits.rdy = 0; /* Init controller as not ready */
}

static void
nvmf_init_nvme_session_properties(struct nvmf_session *session)
{
	const struct spdk_nvme_ctrlr_data	*cdata;
	struct spdk_nvmf_extended_identify_ctrlr_data *nvmfdata;

	/*
	  Here we are going to initialize the features, properties, and
	  identify controller details for the virtual controller associated
	  with a specific subsystem session.
	*/

	/* Init the virtual controller details using actual HW details */
	cdata = spdk_nvme_ctrlr_get_data(session->subsys->ctrlr);
	memcpy(&session->vcdata, cdata, sizeof(struct spdk_nvme_ctrlr_data));

	session->vcdata.aerl = 0;
	session->vcdata.cntlid = 0;
	session->vcdata.kas = 10;
	session->vcdata.maxcmd = SPDK_NVMF_DEFAULT_MAX_QUEUE_DEPTH;
	session->vcdata.mdts = SPDK_NVMF_MAX_RECV_DATA_TRANSFER_SIZE / 4096;
	session->vcdata.sgls.keyed_sgl = 1;
	session->vcdata.sgls.sgl_offset = 1;

	nvmfdata = (struct spdk_nvmf_extended_identify_ctrlr_data *)session->vcdata.nvmf_specific;
	nvmfdata->ioccsz = (NVMF_H2C_MAX_MSG / 16);
	nvmfdata->iorcsz = (NVMF_C2H_MAX_MSG / 16);
	nvmfdata->icdoff = 0; /* offset starts directly after SQE */
	nvmfdata->ctrattr = 0; /* dynamic controller model */
	nvmfdata->msdbd = 1; /* target supports single SGL in capsule */

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: ctrlr data: maxcmd %x\n",
		      session->vcdata.maxcmd);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: ext ctrlr data: ioccsz %x\n",
		      nvmfdata->ioccsz);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: ext ctrlr data: iorcsz %x\n",
		      nvmfdata->iorcsz);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: ext ctrlr data: icdoff %x\n",
		      nvmfdata->icdoff);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: ext ctrlr data: ctrattr %x\n",
		      nvmfdata->ctrattr);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: ext ctrlr data: msdbd %x\n",
		      nvmfdata->msdbd);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: sgls data: 0x%x\n",
		      *(uint32_t *)&session->vcdata.sgls);

	session->vcprop.cap.raw = 0;
	session->vcprop.cap.bits.cqr = 0;	/* queues not contiguous */
	session->vcprop.cap.bits.mqes = (session->vcdata.maxcmd - 1);	/* max queue depth */
	session->vcprop.cap.bits.ams = 0;	/* optional arb mechanisms */
	session->vcprop.cap.bits.to = 1;	/* ready timeout - 500 msec units */
	session->vcprop.cap.bits.dstrd = 0;	/* fixed to 0 for NVMf */
	session->vcprop.cap.bits.css_nvm = 1; /* NVM command set */
	session->vcprop.cap.bits.mpsmin = 0; /* 2 ^ 12 + mpsmin == 4k */
	session->vcprop.cap.bits.mpsmax = 0; /* 2 ^ 12 + mpsmax == 4k */

	/* Version Supported: 1.0 */
	session->vcprop.vs.bits.mjr = 1;
	session->vcprop.vs.bits.mnr = 0;
	session->vcprop.vs.bits.ter = 0;

	session->vcprop.cc.raw = 0;
	session->vcprop.cc.bits.en = 0; /* Init controller disabled */

	session->vcprop.csts.raw = 0;
	session->vcprop.csts.bits.rdy = 0; /* Init controller as not ready */

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: cap %" PRIx64 "\n",
		      session->vcprop.cap.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: vs %x\n", session->vcprop.vs.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: cc %x\n", session->vcprop.cc.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: csts %x\n",
		      session->vcprop.csts.raw);
}

static void
nvmf_init_session_properties(struct nvmf_session *session)
{
	if (session->subsys->subtype == SPDK_NVMF_SUB_NVME) {
		nvmf_init_nvme_session_properties(session);
	} else {
		nvmf_init_discovery_session_properties(session);
	}
}

static struct nvmf_session *
nvmf_create_session(const char *subnqn)
{
	struct nvmf_session	*session;
	struct spdk_nvmf_subsystem	*subsystem;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "nvmf_create_session:\n");

	/* locate the previously provisioned subsystem */
	subsystem = nvmf_find_subsystem(subnqn);
	if (subsystem == NULL) {
		return NULL;
	}

	session = calloc(1, sizeof(struct nvmf_session));
	if (session == NULL) {
		return NULL;
	}

	TAILQ_INIT(&session->connections);
	session->num_connections = 0;
	session->subsys = subsystem;
	session->max_connections_allowed = g_nvmf_tgt.MaxConnectionsPerSession;

	nvmf_init_session_properties(session);

	subsystem->session = session;

	return session;
}

static void
nvmf_delete_session(struct nvmf_session	*session)
{
	session->subsys->session = NULL;
	free(session);
}

static struct nvmf_session *
nvmf_find_session(const char *subnqn)
{
	struct spdk_nvmf_subsystem *subsystem;

	subsystem = nvmf_find_subsystem(subnqn);
	if (subsystem == NULL) {
		return NULL;
	}

	return subsystem->session;
}

struct nvmf_session *
nvmf_connect(struct spdk_nvmf_conn *conn,
	     struct spdk_nvmf_fabric_connect_cmd *connect,
	     struct spdk_nvmf_fabric_connect_data *connect_data,
	     struct spdk_nvmf_fabric_connect_rsp *response)
{
	struct nvmf_session *session;

	if (conn->type == CONN_TYPE_AQ) {
		/* For admin connections, establish a new session */
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "CONNECT Admin Queue for controller id %d\n", connect_data->cntlid);
		if (connect_data->cntlid != 0xFFFF) {
			/* This NVMf target only supports dynamic mode. */
			SPDK_ERRLOG("The NVMf target only supports dynamic mode.\n");
			response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
			return NULL;
		}

		session = nvmf_create_session(connect_data->subnqn);
		if (session == NULL) {
			response->status.sc = SPDK_NVMF_FABRIC_SC_CONTROLLER_BUSY;
			return NULL;
		}
	} else {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "CONNECT I/O Queue for controller id %d\n", connect_data->cntlid);
		session = nvmf_find_session(connect_data->subnqn);
		if (session == NULL) {
			SPDK_ERRLOG("Unknown controller id %d\n", connect_data->cntlid);
			response->status.sc = SPDK_NVMF_FABRIC_SC_RESTART_DISCOVERY;
			return NULL;
		}

		/* check if we would exceed session connection limit */
		if (session->num_connections >= session->max_connections_allowed) {
			SPDK_ERRLOG("connection limit %d\n", session->num_connections);
			response->status.sc = SPDK_NVMF_FABRIC_SC_CONTROLLER_BUSY;
			return NULL;
		}
	}

	session->num_connections++;
	TAILQ_INSERT_HEAD(&session->connections, conn, link);

	response->status_code_specific.success.cntlid = 0;
	response->status.sc = 0;

	return session;
}

void
nvmf_disconnect(struct nvmf_session *session,
		struct spdk_nvmf_conn *conn)
{
	if (session) {
		if (session->num_connections > 0) {
			session->num_connections--;
			TAILQ_REMOVE(&session->connections, conn, link);
		}

		if (session->num_connections == 0) {
			nvmf_delete_session(session);
		}
	}
}

void
nvmf_complete_cmd(void *ctx, const struct spdk_nvme_cpl *cmp)
{
	struct spdk_nvmf_request *req  = ctx;
	struct spdk_nvme_cpl *response;

	spdk_trace_record(TRACE_NVMF_LIB_COMPLETE, 0, 0, (uint64_t)req, 0);

	response = &req->rsp->nvme_cpl;
	memcpy(response, cmp, sizeof(*cmp));

	spdk_nvmf_request_complete(req);
}

static uint64_t
nvmf_prop_get_cap(struct nvmf_session *session)
{
	return session->vcprop.cap.raw;
}

static uint64_t
nvmf_prop_get_vs(struct nvmf_session *session)
{
	return session->vcprop.vs.raw;
}

static uint64_t
nvmf_prop_get_cc(struct nvmf_session *session)
{
	return session->vcprop.cc.raw;
}

static bool
nvmf_prop_set_cc(struct nvmf_session *session, uint64_t value)
{
	union spdk_nvme_cc_register cc;

	cc.raw = (uint32_t)value;

	if (cc.bits.en && !session->vcprop.cc.bits.en) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Property Set CC Enable!\n");
		session->vcprop.csts.bits.rdy = 1;
	}

	if (cc.bits.shn && !session->vcprop.cc.bits.shn) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Property Set CC Shutdown!\n");
		session->vcprop.cc.bits.en = 0;
	}

	session->vcprop.cc.raw = cc.raw;
	return true;
}

static uint64_t
nvmf_prop_get_csts(struct nvmf_session *session)
{
	return session->vcprop.csts.raw;
}

struct nvmf_prop {
	uint32_t ofst;
	uint8_t size;
	char name[11];
	uint64_t (*get_cb)(struct nvmf_session *session);
	bool (*set_cb)(struct nvmf_session *session, uint64_t value);
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

	for (i = 0; i < sizeof(nvmf_props) / sizeof(*nvmf_props); i++) {
		const struct nvmf_prop *prop = &nvmf_props[i];

		if (prop->ofst == ofst) {
			return prop;
		}
	}

	return NULL;
}

void
nvmf_property_get(struct nvmf_session *session,
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

	response->value.u64 = prop->get_cb(session);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "response value: 0x%" PRIx64 "\n", response->value.u64);
}

void
nvmf_property_set(struct nvmf_session *session,
		  struct spdk_nvmf_fabric_prop_set_cmd *cmd,
		  struct spdk_nvmf_fabric_prop_set_rsp *response,
		  bool *shutdown)
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

	if (!prop->set_cb(session, value)) {
		SPDK_ERRLOG("prop set_cb failed\n");
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return;
	}
}

void
nvmf_check_admin_completions(struct nvmf_session *session)
{
	/* Discovery subsystem won't have a real NVMe controller, so check ctrlr first */
	if (session->subsys->ctrlr) {
		spdk_nvme_ctrlr_process_admin_completions(session->subsys->ctrlr);
	}
}

void
nvmf_check_io_completions(struct nvmf_session *session)
{
	spdk_nvme_qpair_process_completions(session->subsys->io_qpair, 0);
}
