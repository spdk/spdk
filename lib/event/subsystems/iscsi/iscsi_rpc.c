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

static const struct spdk_json_object_decoder rpc_set_iscsi_opts_decoders[] = {
	{"auth_file", offsetof(struct spdk_iscsi_opts, authfile), spdk_json_decode_string, true},
	{"node_base", offsetof(struct spdk_iscsi_opts, nodebase), spdk_json_decode_string, true},
	{"nop_timeout", offsetof(struct spdk_iscsi_opts, timeout), spdk_json_decode_int32, true},
	{"nop_in_interval", offsetof(struct spdk_iscsi_opts, nopininterval), spdk_json_decode_int32, true},
	{"no_discovery_auth", offsetof(struct spdk_iscsi_opts, disable_chap), spdk_json_decode_bool, true},
	{"req_discovery_auth", offsetof(struct spdk_iscsi_opts, require_chap), spdk_json_decode_bool, true},
	{"req_discovery_auth_mutual", offsetof(struct spdk_iscsi_opts, mutual_chap), spdk_json_decode_bool, true},
	{"discovery_auth_group", offsetof(struct spdk_iscsi_opts, chap_group), spdk_json_decode_int32, true},
	{"disable_chap", offsetof(struct spdk_iscsi_opts, disable_chap), spdk_json_decode_bool, true},
	{"require_chap", offsetof(struct spdk_iscsi_opts, require_chap), spdk_json_decode_bool, true},
	{"mutual_chap", offsetof(struct spdk_iscsi_opts, mutual_chap), spdk_json_decode_bool, true},
	{"chap_group", offsetof(struct spdk_iscsi_opts, chap_group), spdk_json_decode_int32, true},
	{"max_sessions", offsetof(struct spdk_iscsi_opts, MaxSessions), spdk_json_decode_uint32, true},
	{"max_queue_depth", offsetof(struct spdk_iscsi_opts, MaxQueueDepth), spdk_json_decode_uint32, true},
	{"max_connections_per_session", offsetof(struct spdk_iscsi_opts, MaxConnectionsPerSession), spdk_json_decode_uint32, true},
	{"default_time2wait", offsetof(struct spdk_iscsi_opts, DefaultTime2Wait), spdk_json_decode_uint32, true},
	{"default_time2retain", offsetof(struct spdk_iscsi_opts, DefaultTime2Retain), spdk_json_decode_uint32, true},
	{"first_burst_length", offsetof(struct spdk_iscsi_opts, FirstBurstLength), spdk_json_decode_uint32, true},
	{"immediate_data", offsetof(struct spdk_iscsi_opts, ImmediateData), spdk_json_decode_bool, true},
	{"error_recovery_level", offsetof(struct spdk_iscsi_opts, ErrorRecoveryLevel), spdk_json_decode_uint32, true},
	{"allow_duplicated_isid", offsetof(struct spdk_iscsi_opts, AllowDuplicateIsid), spdk_json_decode_bool, true},
	{"min_connections_per_core", offsetof(struct spdk_iscsi_opts, min_connections_per_core), spdk_json_decode_uint32, true},
};

static void
spdk_rpc_iscsi_set_opts(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct spdk_iscsi_opts *opts;
	struct spdk_json_write_ctx *w;

	if (g_spdk_iscsi_opts != NULL) {
		SPDK_ERRLOG("this RPC must not be called more than once.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Must not call more than once");
		return;
	}

	opts = spdk_iscsi_opts_alloc();
	if (opts == NULL) {
		SPDK_ERRLOG("spdk_iscsi_opts_alloc() failed.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Out of memory");
		return;
	}

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_set_iscsi_opts_decoders,
					    SPDK_COUNTOF(rpc_set_iscsi_opts_decoders), opts)) {
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			spdk_iscsi_opts_free(opts);
			return;
		}
	}

	g_spdk_iscsi_opts = spdk_iscsi_opts_copy(opts);
	spdk_iscsi_opts_free(opts);

	if (g_spdk_iscsi_opts == NULL) {
		SPDK_ERRLOG("spdk_iscsi_opts_copy() failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Out of memory");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("set_iscsi_options", spdk_rpc_iscsi_set_opts, SPDK_RPC_STARTUP)
