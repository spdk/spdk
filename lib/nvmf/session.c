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
#include "subsystem_grp.h"
#include "spdk/log.h"
#include "spdk/trace.h"
#include "spdk/nvme_spec.h"

static struct nvmf_session *
nvmf_create_session(const char *subnqn)
{
	struct nvmf_session	*session;
	struct spdk_nvmf_subsystem	*subsystem;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "nvmf_create_session:\n");

	/* locate the previously provisioned subsystem */
	subsystem = nvmf_find_subsystem(subnqn);
	if (subsystem == NULL)
		return NULL;

	session = calloc(1, sizeof(struct nvmf_session));
	if (session == NULL)
		goto exit;

	subsystem->num_sessions++;
	/* define cntlid that is unique across all subsystems */
	session->cntlid = (subsystem->num << NVMF_CNTLID_SUBS_SHIFT) + subsystem->num_sessions;
	TAILQ_INSERT_HEAD(&subsystem->sessions, session, entries);

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "nvmf_create_session: allocated session cntlid %d\n",
		      session->cntlid);
	TAILQ_INIT(&session->connections);
	session->num_connections = 0;
	session->is_valid = 1;
	session->subsys = subsystem;

exit:
	return session;
}

static void
nvmf_delete_session(struct nvmf_session	*session)
{
	session->subsys->num_sessions--;
	TAILQ_REMOVE(&session->subsys->sessions, session, entries);

	free(session);
}

static void
nvmf_init_discovery_session_properties(struct nvmf_session *session)
{
	struct spdk_nvmf_extended_identify_ctrlr_data *nvmfdata;

	session->vcdata.maxcmd = SPDK_NVMF_DEFAULT_MAX_QUEUE_DEPTH;
	/* extended data for get log page supportted */
	session->vcdata.lpa.edlp = 1;
	/* reset cntlid in vcdata to match the logical cntlid known to NVMf */
	session->vcdata.cntlid = session->cntlid;
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
nvmf_init_nvme_session_properties(struct nvmf_session *session, int aq_depth)
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
	memcpy((char *)&session->vcdata, (char *)cdata, sizeof(struct spdk_nvme_ctrlr_data));

	/* indicate support for only a single AER */
	session->vcdata.aerl = 0;

	/* reset cntlid in vcdata to match the logical cntlid known to NVMf */
	session->vcdata.cntlid = session->cntlid;

	/* initialize the nvmf new and extension details in controller data */
	session->vcdata.kas = 10;
	session->vcdata.maxcmd = SPDK_NVMF_DEFAULT_MAX_QUEUE_DEPTH;
	nvmfdata = (struct spdk_nvmf_extended_identify_ctrlr_data *)session->vcdata.nvmf_specific;
	nvmfdata->ioccsz = (NVMF_H2C_MAX_MSG / 16);
	nvmfdata->iorcsz = (NVMF_C2H_MAX_MSG / 16);
	nvmfdata->icdoff = 0; /* offset starts directly after SQE */
	nvmfdata->ctrattr = 0; /* dynamic controller model */
	nvmfdata->msdbd = 1; /* target supports single SGL in capsule */
	session->vcdata.sgls.keyed_sgl = 1;
	session->vcdata.sgls.sgl_offset = 1;

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

	/* feature: Number Of Queues. */
	session->max_io_queues = MAX_SESSION_IO_QUEUES;

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

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: max io queues %x\n",
		      session->max_io_queues);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: cap %" PRIx64 "\n",
		      session->vcprop.cap.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: vs %x\n", session->vcprop.vs.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: cc %x\n", session->vcprop.cc.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: csts %x\n",
		      session->vcprop.csts.raw);
}

void
nvmf_init_session_properties(struct nvmf_session *session, int aq_depth)
{
	if (session->subsys->subtype == SPDK_NVMF_SUB_NVME) {
		nvmf_init_nvme_session_properties(session, aq_depth);
	} else {
		nvmf_init_discovery_session_properties(session);
	}
}

static struct nvmf_session *
nvmf_find_session_by_id(const char *subnqn, uint16_t cntl_id)
{
	struct spdk_nvmf_subsystem *subsystem;
	struct nvmf_session *sess, *tsess;

	subsystem = nvmf_find_subsystem(subnqn);
	if (subsystem == NULL)
		return NULL;

	TAILQ_FOREACH_SAFE(sess, &subsystem->sessions, entries, tsess) {
		if (sess->cntlid == cntl_id) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Session Match cntlid %d, sess %p\n", cntl_id, sess);
			return sess;
		}
	}

	return NULL;
}

struct nvmf_session *
nvmf_connect(void *fabric_conn,
	     struct spdk_nvmf_fabric_connect_cmd *connect,
	     struct spdk_nvmf_fabric_connect_data *connect_data,
	     struct spdk_nvmf_fabric_connect_rsp *response)
{
	struct nvmf_session *session;
	struct nvmf_connection_entry *connection = NULL;

	connection = calloc(1, sizeof(struct nvmf_connection_entry));
	if (connection == NULL)
		goto connect_fail;

	/* Figure out if this is the first connect and we
	 * need to allocate an nvmf_session or if this is
	 * a subsequent connect for an I/O queue and we need
	 * to return an existing session
	 */
	if (connect->qid == 0) {
		/* first connect for AQ connection */
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "AQ connect capsule\n");
		if (connect_data->cntlid == 0xffff) {
			/* no nvmf session/controller association, allocate one */
			session = nvmf_create_session(connect_data->subnqn);
			if (session == NULL) {
				SPDK_ERRLOG("create session failed\n");
				response->status.sc = SPDK_NVMF_FABRIC_SC_CONTROLLER_BUSY;
				goto connect_fail;
			}
		} else {
			SPDK_ERRLOG("nvmf AQ connection attempt to cntlid %d\n", connect_data->cntlid);
			response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
			goto connect_fail;
		}
		connection->is_aq_conn = 1;
	} else {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "IOQ connect capsule\n");
		/* locate the existing session */
		session = nvmf_find_session_by_id(connect_data->subnqn, connect_data->cntlid);
		if (session == NULL) {
			SPDK_ERRLOG("invalid nvmf cntlid %d\n", connect_data->cntlid);
			response->status.sc = SPDK_NVMF_FABRIC_SC_RESTART_DISCOVERY;
			goto connect_fail;
		}
		/* check if we would exceed session connection limit */
		if (session->num_connections >= session->max_connections_allowed) {
			SPDK_ERRLOG("connection limit %d\n", session->num_connections);
			response->status.sc = SPDK_NVMF_FABRIC_SC_CONTROLLER_BUSY;
			goto connect_fail;
		}

		if (session->is_valid == 0) {
			SPDK_ERRLOG("session invalid or at IO connection limit %d\n", session->num_connections);
			response->status.sc = SPDK_NVMF_FABRIC_SC_RESTART_DISCOVERY;
			goto connect_fail;
		}
		connection->is_aq_conn = 0;
	}

	connection->fabric_conn = fabric_conn;

	session->num_connections++;
	TAILQ_INSERT_HEAD(&session->connections, connection, entries);

	response->status_code_specific.success.cntlid = session->cntlid;
	response->status.sc = 0;

	return session;

connect_fail:
	if (connection)
		free(connection);
	return NULL;
}

void
nvmf_disconnect(void *fabric_conn,
		struct nvmf_session *session)
{
	struct nvmf_connection_entry *conn, *tconn, *rconn = NULL;

	/* Indication from the fabric transport that a
	 * specific connection has gone way.  If the
	 * connection is the AQ connection then expect
	 * that the complete session will go away
	 */
	if (session == NULL) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "nvmf_disconnect: session not active!\n");
		return;
	}

	TAILQ_FOREACH_SAFE(conn, &session->connections, entries, tconn) {
		if (conn->fabric_conn == fabric_conn) {
			rconn = conn;
			break;
		}
	}
	if (rconn == NULL) {
		SPDK_ERRLOG("Session connection did not exist!\n");
		return;
	}
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "Disconnect NVMf conn %p, sess %p\n", rconn, session);

	session->num_connections--;
	TAILQ_REMOVE(&session->connections, rconn, entries);
	free(rconn);

	if (session->num_connections == 0) {
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Session connection count 0, deleting session %p!\n",
			      session);
		nvmf_delete_session(session);
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
