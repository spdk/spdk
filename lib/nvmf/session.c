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
	session->vcprop.cap_lo.raw = 0;
	session->vcprop.cap_lo.bits.cqr = 1;	/* NVMF specification required */
	session->vcprop.cap_lo.bits.mqes = (session->vcdata.maxcmd - 1);	/* max queue depth */
	session->vcprop.cap_lo.bits.ams = 0;	/* optional arb mechanisms */

	session->vcprop.cap_hi.raw = 0;
	session->vcprop.cap_hi.bits.dstrd = 0;	/* fixed to 0 for NVMf */
	session->vcprop.cap_hi.bits.css_nvm = 1; /* NVM command set */
	session->vcprop.cap_hi.bits.mpsmin = 0; /* 2 ^ 12 + mpsmin == 4k */
	session->vcprop.cap_hi.bits.mpsmax = 0; /* 2 ^ 12 + mpsmax == 4k */

	session->vcprop.vs = 0x10000;	/* Version Supported: Major 1, Minor 0 */

	session->vcprop.cc.raw = 0;

	session->vcprop.csts.raw = 0;
	session->vcprop.csts.bits.rdy = 0; /* Init controller as not ready */
}

static void
nvmf_init_nvme_session_properties(struct nvmf_session *session, int aq_depth)
{
	/* for now base virtual controller properties on first namespace controller */
	struct spdk_nvme_ctrlr *ctrlr = session->subsys->ns_list_map[0].ctrlr;
	const struct spdk_nvme_ctrlr_data	*cdata;
	struct spdk_nvmf_extended_identify_ctrlr_data *nvmfdata;

	/*
	  Here we are going to initialize the features, properties, and
	  identify controller details for the virtual controller associated
	  with a specific subsystem session.
	*/

	/* Init the virtual controller details using actual HW details */
	cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	memcpy((char *)&session->vcdata, (char *)cdata, sizeof(struct spdk_nvme_ctrlr_data));

	/* update virtual controller data to represent merge of
	   controllers for all namespaces
	*/
	session->vcdata.nn = session->subsys->ns_count;

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

	session->vcprop.cap_lo.raw = 0;
	session->vcprop.cap_lo.bits.cqr = 0;	/* queues not contiguous */
	session->vcprop.cap_lo.bits.mqes = (session->vcdata.maxcmd - 1);	/* max queue depth */
	session->vcprop.cap_lo.bits.ams = 0;	/* optional arb mechanisms */
	session->vcprop.cap_lo.bits.to = 1;	/* ready timeout - 500 msec units */

	session->vcprop.cap_hi.raw = 0;
	session->vcprop.cap_hi.bits.dstrd = 0;	/* fixed to 0 for NVMf */
	session->vcprop.cap_hi.bits.css_nvm = 1; /* NVM command set */
	session->vcprop.cap_hi.bits.mpsmin = 0; /* 2 ^ 12 + mpsmin == 4k */
	session->vcprop.cap_hi.bits.mpsmax = 0; /* 2 ^ 12 + mpsmax == 4k */

	session->vcprop.vs = 0x10000;	/* Version Supported: Major 1, Minor 0 */

	session->vcprop.cc.raw = 0;
	session->vcprop.cc.bits.en = 0; /* Init controller disabled */

	session->vcprop.csts.raw = 0;
	session->vcprop.csts.bits.rdy = 0; /* Init controller as not ready */

	/* nssr not defined for v1.0 */

	/* Set AQA details to reflect the virtual connection SQ/CQ depth */
	session->vcprop.aqa.bits.asqs = (aq_depth & 0xFFF);
	session->vcprop.aqa.bits.acqs = (aq_depth & 0xFFF);

	session->vcprop.propsz.bits.size = sizeof(struct spdk_nvmf_ctrlr_properties) / 64;
	session->vcprop.capattr_hi.raw = 0;
	session->vcprop.capattr_lo.bits.rspsz = sizeof(union nvmf_c2h_msg) / 16;
	session->vcprop.capattr_lo.bits.cmdsz = sizeof(union nvmf_h2c_msg) / 16;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: max io queues %x\n",
		      session->max_io_queues);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: cap_lo %x\n",
		      session->vcprop.cap_lo.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: cap_hi %x\n",
		      session->vcprop.cap_hi.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: vs %x\n", session->vcprop.vs);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: cc %x\n", session->vcprop.cc.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: csts %x\n",
		      session->vcprop.csts.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: nssr %x\n", session->vcprop.nssr);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: aqa %x\n", session->vcprop.aqa.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: propsz %x\n",
		      session->vcprop.propsz.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: capattr_lo %x\n",
		      session->vcprop.capattr_lo.raw);
	SPDK_TRACELOG(SPDK_TRACE_NVMF, "	nvmf_init_session_properties: capattr_hi %x\n",
		      session->vcprop.capattr_hi.raw);
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

void
nvmf_property_get(struct nvmf_session *session,
		  struct spdk_nvmf_fabric_prop_get_cmd *cmd,
		  struct spdk_nvmf_fabric_prop_get_rsp *response)
{
	response->status.sc = 0;
	response->value.u64 = 0;

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "nvmf_property_get: attrib %d, offset %x\n",
		      cmd->attrib, cmd->ofst);

	if (cmd->ofst > offsetof(struct spdk_nvmf_ctrlr_properties, capattr_hi)) {
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return;
	}

	switch (cmd->ofst) {
	case (offsetof(struct spdk_nvmf_ctrlr_properties, cap_lo)):
		response->value.u32.low = session->vcprop.cap_lo.raw;
		if (cmd->attrib == 1)
			response->value.u32.high = session->vcprop.cap_hi.raw;
		break;
	case (offsetof(struct spdk_nvmf_ctrlr_properties, cap_hi)):
		if (cmd->attrib == 1)
			response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		else
			response->value.u32.low = session->vcprop.cap_hi.raw;
		break;
	case (offsetof(struct spdk_nvmf_ctrlr_properties, vs)):
		if (cmd->attrib == 1)
			response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		else
			response->value.u32.low = session->vcprop.vs;
		break;
	case (offsetof(struct spdk_nvmf_ctrlr_properties, intms)):
	case (offsetof(struct spdk_nvmf_ctrlr_properties, intmc)):
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		break;
	case (offsetof(struct spdk_nvmf_ctrlr_properties, cc)):
		if (cmd->attrib == 1)
			response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		else
			response->value.u32.low = session->vcprop.cc.raw;
		break;
	case (offsetof(struct spdk_nvmf_ctrlr_properties, csts)):
		if (cmd->attrib == 1)
			response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		else
			response->value.u32.low = session->vcprop.csts.raw;
		break;
	case (offsetof(struct spdk_nvmf_ctrlr_properties, nssr)):
		if (cmd->attrib == 1)
			response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		else
			response->value.u32.low = session->vcprop.nssr;
		break;
	case (offsetof(struct spdk_nvmf_ctrlr_properties, aqa)):
		if (cmd->attrib == 1)
			response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		else
			response->value.u32.low = session->vcprop.aqa.raw;
		break;
	case (offsetof(struct spdk_nvmf_ctrlr_properties, asq)):
	case (offsetof(struct spdk_nvmf_ctrlr_properties, acq)):
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		break;
	case (offsetof(struct spdk_nvmf_ctrlr_properties, propsz)):
		if (cmd->attrib == 1)
			response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		else
			response->value.u32.low = session->vcprop.propsz.raw;
		break;
	case (offsetof(struct spdk_nvmf_ctrlr_properties, capattr_lo)):
		response->value.u32.low = session->vcprop.capattr_lo.raw;
		if (cmd->attrib == 1)
			response->value.u32.high = session->vcprop.capattr_hi.raw;
		break;
	case (offsetof(struct spdk_nvmf_ctrlr_properties, capattr_hi)):
		if (cmd->attrib == 1)
			response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		else
			response->value.u32.low = session->vcprop.capattr_hi.raw;
		break;
	default:
		break;
	}
}

void
nvmf_property_set(struct nvmf_session *session,
		  struct spdk_nvmf_fabric_prop_set_cmd *cmd,
		  struct spdk_nvmf_fabric_prop_set_rsp *response,
		  bool *shutdown)
{
	response->status.sc = 0;

	SPDK_TRACELOG(SPDK_TRACE_NVMF,
		      "nvmf_property_set: attrib %d, offset %x, value %lx, value low %x, value high %x\n",
		      cmd->attrib, cmd->ofst, cmd->value.u64, cmd->value.u32.low, cmd->value.u32.high);

	if (cmd->ofst > offsetof(struct spdk_nvmf_ctrlr_properties, capattr_hi)) {
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		return;
	}

	/* TBD: determine which values we allow to be changed, deal with spec version
		difference.  Fields within 32bit value, ex. for reset in csts */

	switch (cmd->ofst) {
	case (offsetof(struct spdk_nvmf_ctrlr_properties, cc)): {
		union spdk_nvme_cc_register cc;

		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Property Set CC\n");
		if (cmd->attrib == 1)
			response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		else {
			cc.raw = cmd->value.u32.low;

			if (cc.bits.en == 1 && session->vcprop.cc.bits.en == 0) {
				SPDK_TRACELOG(SPDK_TRACE_NVMF, "Property Set CC Enable!\n");
				session->vcprop.csts.bits.rdy = 1;
			}

			if (cc.bits.shn && session->vcprop.cc.bits.shn == 0) {
				SPDK_TRACELOG(SPDK_TRACE_NVMF, "Property Set CC Shutdown!\n");
				session->vcprop.cc.bits.en = 0;
				*shutdown = true;
			}

			session->vcprop.cc.raw = cc.raw;
		}
	}
	break;
	case (offsetof(struct spdk_nvmf_ctrlr_properties, csts)):
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Property Set CSTS\n");
		if (cmd->attrib == 1)
			response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		else
			session->vcprop.csts.raw = cmd->value.u32.low;
		break;
	case (offsetof(struct spdk_nvmf_ctrlr_properties, nssr)):
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Property Set NSSR\n");
		if (cmd->attrib == 1)
			response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		else
			session->vcprop.nssr = cmd->value.u32.low;
		break;
	case (offsetof(struct spdk_nvmf_ctrlr_properties, aqa)):
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Property Set AQA\n");
		if (cmd->attrib == 1)
			response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		else
			session->vcprop.aqa.raw = cmd->value.u32.low;
		break;
	default:
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Property Set Invalid Offset %x\n", cmd->ofst);
		response->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
		break;
	}
}

void
nvmf_check_admin_completions(struct nvmf_session *session)
{
	struct spdk_nvmf_subsystem *subsystem = session->subsys;
	struct spdk_nvme_ctrlr *ctrlr, *prev_ctrlr = NULL;
	int i;

	for (i = 0; i < MAX_PER_SUBSYSTEM_NAMESPACES; i++) {
		ctrlr = subsystem->ns_list_map[i].ctrlr;
		if (ctrlr == NULL)
			continue;
		if (ctrlr != NULL && ctrlr != prev_ctrlr) {
			spdk_nvme_ctrlr_process_admin_completions(ctrlr);
			prev_ctrlr = ctrlr;
		}
	}
}

void
nvmf_check_io_completions(struct nvmf_session *session)
{
	struct spdk_nvmf_subsystem *subsystem = session->subsys;
	struct spdk_nvme_qpair *qpair, *prev_qpair = NULL;
	int i;

	for (i = 0; i < MAX_PER_SUBSYSTEM_NAMESPACES; i++) {
		qpair = subsystem->ns_list_map[i].qpair;
		if (qpair == NULL)
			continue;
		if (qpair != NULL && qpair != prev_qpair) {
			spdk_nvme_qpair_process_completions(qpair, 0);
			prev_qpair = qpair;
		}
	}
}
