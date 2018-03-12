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

#include "iscsi/iscsi.h"
#include "iscsi/conn.h"

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/event.h"

#include "spdk_internal/log.h"

static const struct spdk_json_object_decoder rpc_initialize_iscsi_subsystem_decoders[] = {
	{"auth_file", offsetof(struct spdk_iscsi_opts, authfile), spdk_json_decode_string, true},
	{"node_base", offsetof(struct spdk_iscsi_opts, nodebase), spdk_json_decode_string, true},
	{"timeout", offsetof(struct spdk_iscsi_opts, timeout), spdk_json_decode_int32, true},
	{"nop_in_interval", offsetof(struct spdk_iscsi_opts, nopininterval), spdk_json_decode_int32, true},
	{"no_discovery_auth", offsetof(struct spdk_iscsi_opts, no_discovery_auth), spdk_json_decode_bool, true},
	{"req_discovery_auth", offsetof(struct spdk_iscsi_opts, req_discovery_auth), spdk_json_decode_bool, true},
	{"req_discovery_auth_mutual", offsetof(struct spdk_iscsi_opts, req_discovery_auth_mutual), spdk_json_decode_bool, true},
	{"discovery_auth_group", offsetof(struct spdk_iscsi_opts, discovery_auth_group), spdk_json_decode_bool, true},
	{"max_sessions", offsetof(struct spdk_iscsi_opts, MaxSessions), spdk_json_decode_uint32, true},
	{"max_connections_per_session", offsetof(struct spdk_iscsi_opts, MaxConnectionsPerSession), spdk_json_decode_uint32, true},
	{"default_time2wait", offsetof(struct spdk_iscsi_opts, DefaultTime2Wait), spdk_json_decode_uint32, true},
	{"default_time2retain", offsetof(struct spdk_iscsi_opts, DefaultTime2Retain), spdk_json_decode_uint32, true},
	{"immediate_data", offsetof(struct spdk_iscsi_opts, ImmediateData), spdk_json_decode_bool, true},
	{"error_recovery_level", offsetof(struct spdk_iscsi_opts, ErrorRecoveryLevel), spdk_json_decode_bool, true},
	{"allow_duplicated_isid", offsetof(struct spdk_iscsi_opts, AllowDuplicateIsid), spdk_json_decode_bool, true},
	{"min_connections_per_session", offsetof(struct spdk_iscsi_opts, min_connections_per_core), spdk_json_decode_uint32, true},
};

static void
spdk_iscsi_read_rpc_config_params(struct spdk_iscsi_opts *req, struct spdk_iscsi_opts *opts)
{
	if (req->authfile != NULL) {
		free(opts->authfile);
		opts->authfile = strdup(req->authfile);
	}

	if (req->nodebase != NULL) {
		free(opts->nodebase);
		opts->nodebase = strdup(req->nodebase);
	}

	if (req->MaxSessions == 0) {
		SPDK_ERRLOG("MaxSessions == 0 invalid, ignoring\n");
	} else if (req->MaxSessions > 65535) {
		SPDK_ERRLOG("MaxSessions == %d invalid, ignoring\n", req->MaxSessions);
	} else {
		opts->MaxSessions = req->MaxSessions;
	}

	if (req->MaxConnectionsPerSession == 0) {
		SPDK_ERRLOG("MaxConnectionsPerSession == 0 invalid, ignoring\n");
	} else if (req->MaxConnectionsPerSession > 65535) {
		SPDK_ERRLOG("MaxConnectionsPerSession == %d invalid, ignoring\n",
			    req->MaxConnectionsPerSession);
	} else {
		opts->MaxConnectionsPerSession = req->MaxConnectionsPerSession;
	}

	if (req->MaxQueueDepth == 0) {
		SPDK_ERRLOG("MaxQueueDepth == 0 invalid, ignoring\n");
	} else if (req->MaxQueueDepth > 256) {
		SPDK_ERRLOG("MaxQueueDepth == %d invalid, ignoring\n", req->MaxQueueDepth);
	} else {
		opts->MaxQueueDepth = req->MaxQueueDepth;
	}

	if (req->DefaultTime2Wait > 3600) {
		SPDK_ERRLOG("DefaultTime2Wait == %d invalid, ignoring\n", req->DefaultTime2Wait);
	} else {
		opts->DefaultTime2Wait = req->DefaultTime2Wait;
	}

	if (req->DefaultTime2Retain > 3600) {
		SPDK_ERRLOG("DefaultTime2Retain == %d invalid, ignoring\n", req->DefaultTime2Retain);
	} else {
		opts->DefaultTime2Retain = req->DefaultTime2Retain;
	}

	opts->ImmediateData = req->ImmediateData;

	/* This option is only for test.
	 * If AllowDuplicateIsid is enabled, it allows different connections carrying
	 * TSIH=0 login the target within the same session.
	 */
	opts->AllowDuplicateIsid = req->AllowDuplicateIsid;

	if (req->ErrorRecoveryLevel > 2) {
		SPDK_ERRLOG("ErrorRecoveryLevel %d not supported, keeping existing %d\n",
			    req->ErrorRecoveryLevel, opts->ErrorRecoveryLevel);
	} else {
		opts->ErrorRecoveryLevel = req->ErrorRecoveryLevel;
	}

	if (req->timeout >= 0) {
		opts->timeout = req->timeout;
	}

	if (req->nopininterval >= 0) {
		if (req->nopininterval > MAX_NOPININTERVAL) {
			SPDK_ERRLOG("NopInInterval == %d invalid, ignoring\n", req->nopininterval);
		} else {
			opts->nopininterval = req->nopininterval;
		}
	}

	opts->no_discovery_auth = req->no_discovery_auth;
	opts->req_discovery_auth = req->req_discovery_auth;
	opts->req_discovery_auth_mutual = req->req_discovery_auth_mutual;
	opts->discovery_auth_group = req->discovery_auth_group;

	opts->min_connections_per_core = req->min_connections_per_core;
}

static void
spdk_rpc_initialize_iscsi_subsystem_cb(void *ctx)
{
	struct spdk_jsonrpc_request *request = ctx;
	struct spdk_json_write_ctx *w;

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;
}

static void
spdk_rpc_initialize_iscsi_subsystem(struct spdk_jsonrpc_request *request,
				    const struct spdk_json_val *params)
{
	struct spdk_iscsi_opts req = {}, opts = {};
	int rc;

	spdk_iscsi_opts_val_init(&req);

	if (spdk_json_decode_object(params, rpc_initialize_iscsi_subsystem_decoders,
				    SPDK_COUNTOF(rpc_initialize_iscsi_subsystem_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object() failed\n");
		goto invalid;
	}

	spdk_iscsi_opts_init(&opts);

	spdk_iscsi_read_rpc_config_params(&req, &opts);
	spdk_iscsi_opts_free(&req);

	rc = spdk_iscsi_initialize_iscsi_globals(&opts);
	spdk_iscsi_opts_free(&opts);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_iscsi_initialize_iscsi_globals() failed\n");
		goto invalid;
	}

	spdk_initialize_iscsi_conns();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_initialize_iscsi_conns() failed\n");
		goto invalid;
	}

	spdk_initialize_iscsi_poll_group(spdk_rpc_initialize_iscsi_subsystem_cb, request);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 "Invalid parameters");
}
SPDK_RPC_REGISTER("initialize_iscsi_subsystem", spdk_rpc_initialize_iscsi_subsystem)
