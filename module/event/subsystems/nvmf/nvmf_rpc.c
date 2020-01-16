/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2019 Mellanox Technologies LTD. All rights reserved.
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

#include "event_nvmf.h"

#include "spdk/rpc.h"
#include "spdk/util.h"

static const struct spdk_json_object_decoder nvmf_rpc_subsystem_tgt_opts_decoder[] = {
	{"max_subsystems", 0, spdk_json_decode_uint32, true}
};

static void
spdk_rpc_nvmf_set_max_subsystems(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	uint32_t max_subsystems = 0;

	if (g_spdk_nvmf_tgt_max_subsystems != 0) {
		SPDK_ERRLOG("this RPC must not be called more than once.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Must not call more than once");
		return;
	}

	if (params != NULL) {
		if (spdk_json_decode_object(params, nvmf_rpc_subsystem_tgt_opts_decoder,
					    SPDK_COUNTOF(nvmf_rpc_subsystem_tgt_opts_decoder), &max_subsystems)) {
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			return;
		}
	}

	g_spdk_nvmf_tgt_max_subsystems = max_subsystems;

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("nvmf_set_max_subsystems", spdk_rpc_nvmf_set_max_subsystems,
		  SPDK_RPC_STARTUP)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(nvmf_set_max_subsystems, set_nvmf_target_max_subsystems)

static int decode_conn_sched(const struct spdk_json_val *val, void *out)
{
	enum spdk_nvmf_connect_sched *sched = out;

	if (spdk_json_strequal(val, "roundrobin") == true) {
		*sched = CONNECT_SCHED_ROUND_ROBIN;
	} else if (spdk_json_strequal(val, "hostip") == true) {
		*sched = CONNECT_SCHED_HOST_IP;
	} else if (spdk_json_strequal(val, "transport") == true) {
		*sched = CONNECT_SCHED_TRANSPORT_OPTIMAL_GROUP;
	} else {
		SPDK_ERRLOG("Invalid connection scheduling parameter\n");
		return -EINVAL;
	}

	return 0;
}

static const struct spdk_json_object_decoder admin_passthru_decoder[] = {
	{"identify_ctrlr", offsetof(struct spdk_nvmf_admin_passthru_conf, identify_ctrlr), spdk_json_decode_bool}
};

static int decode_admin_passthru(const struct spdk_json_val *val, void *out)
{
	struct spdk_nvmf_admin_passthru_conf *req = (struct spdk_nvmf_admin_passthru_conf *)out;

	if (spdk_json_decode_object(val, admin_passthru_decoder,
				    SPDK_COUNTOF(admin_passthru_decoder),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		return -1;
	}

	return 0;
}

static const struct spdk_json_object_decoder nvmf_rpc_subsystem_tgt_conf_decoder[] = {
	{"acceptor_poll_rate", offsetof(struct spdk_nvmf_tgt_conf, acceptor_poll_rate), spdk_json_decode_uint32, true},
	{"conn_sched", offsetof(struct spdk_nvmf_tgt_conf, conn_sched), decode_conn_sched, true},
	{"admin_cmd_passthru", offsetof(struct spdk_nvmf_tgt_conf, admin_passthru), decode_admin_passthru, true}
};

static void
spdk_rpc_nvmf_set_config(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct spdk_nvmf_tgt_conf *conf;
	struct spdk_json_write_ctx *w;

	if (g_spdk_nvmf_tgt_conf != NULL) {
		SPDK_ERRLOG("this RPC must not be called more than once.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Must not call more than once");
		return;
	}

	conf = calloc(1, sizeof(*conf));
	if (conf == NULL) {
		SPDK_ERRLOG("calloc() failed for target config\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Out of memory");
		return;
	}

	conf->acceptor_poll_rate = ACCEPT_TIMEOUT_US;
	conf->conn_sched = DEFAULT_CONN_SCHED;
	conf->admin_passthru.identify_ctrlr = false;

	if (params != NULL) {
		if (spdk_json_decode_object(params, nvmf_rpc_subsystem_tgt_conf_decoder,
					    SPDK_COUNTOF(nvmf_rpc_subsystem_tgt_conf_decoder), conf)) {
			free(conf);
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			return;
		}
	}

	g_spdk_nvmf_tgt_conf = conf;

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("nvmf_set_config", spdk_rpc_nvmf_set_config, SPDK_RPC_STARTUP)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(nvmf_set_config, set_nvmf_target_config)
