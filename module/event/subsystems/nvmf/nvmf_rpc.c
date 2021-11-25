/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2019 Mellanox Technologies LTD. All rights reserved.
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

#include "event_nvmf.h"

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/cpuset.h"

static const struct spdk_json_object_decoder nvmf_rpc_subsystem_tgt_opts_decoder[] = {
	{"max_subsystems", 0, spdk_json_decode_uint32, true}
};

static void
rpc_nvmf_set_max_subsystems(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
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

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("nvmf_set_max_subsystems", rpc_nvmf_set_max_subsystems,
		  SPDK_RPC_STARTUP)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(nvmf_set_max_subsystems, set_nvmf_target_max_subsystems)

static int decode_conn_sched(const struct spdk_json_val *val, void *out)
{
	*(uint32_t *)out = 0;

	SPDK_NOTICELOG("conn_sched is no longer a supported parameter. Ignoring.");

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

static int
decode_discovery_filter(const struct spdk_json_val *val, void *out)
{
	enum spdk_nvmf_tgt_discovery_filter *_filter = (enum spdk_nvmf_tgt_discovery_filter *)out;
	enum spdk_nvmf_tgt_discovery_filter filter = SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY;
	char *tokens = spdk_json_strdup(val);
	char *tok;
	int rc = -EINVAL;
	bool all_specified = false;

	if (!tokens) {
		return -ENOMEM;
	}

	tok = strtok(tokens, ",");
	while (tok) {
		if (strncmp(tok, "match_any", 9) == 0) {
			if (filter != SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY) {
				goto out;
			}
			filter = SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY;
			all_specified = true;
		} else {
			if (all_specified) {
				goto out;
			}
			if (strncmp(tok, "transport", 9) == 0) {
				filter |= SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_TYPE;
			} else if (strncmp(tok, "address", 7) == 0) {
				filter |= SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_ADDRESS;
			} else if (strncmp(tok, "svcid", 5) == 0) {
				filter |= SPDK_NVMF_TGT_DISCOVERY_MATCH_TRANSPORT_SVCID;
			} else {
				SPDK_ERRLOG("Invalid value %s\n", tok);
				goto out;
			}
		}

		tok = strtok(NULL, ",");
	}

	rc = 0;
	*_filter = filter;

out:
	free(tokens);

	return rc;
}

static int
nvmf_is_subset_of_env_core_mask(const struct spdk_cpuset *set)
{
	uint32_t i, tmp_counter = 0;

	SPDK_ENV_FOREACH_CORE(i) {
		if (spdk_cpuset_get_cpu(set, i)) {
			++tmp_counter;
		}
	}
	return spdk_cpuset_count(set) - tmp_counter;
}

static int
nvmf_decode_poll_groups_mask(const struct spdk_json_val *val, void *out)
{
	char *mask = spdk_json_strdup(val);
	int ret = -1;

	if (mask == NULL) {
		return -1;
	}

	if (!(g_poll_groups_mask = spdk_cpuset_alloc())) {
		SPDK_ERRLOG("Unable to allocate a poll groups mask object in nvmf_decode_poll_groups_mask.\n");
		free(mask);
		return -1;
	}

	ret = spdk_cpuset_parse(g_poll_groups_mask, mask);
	free(mask);
	if (ret == 0) {
		if (nvmf_is_subset_of_env_core_mask(g_poll_groups_mask) == 0) {
			return 0;
		} else {
			SPDK_ERRLOG("Poll groups cpumask 0x%s is out of range\n", spdk_cpuset_fmt(g_poll_groups_mask));
		}
	} else {
		SPDK_ERRLOG("Invalid cpumask\n");
	}

	spdk_cpuset_free(g_poll_groups_mask);
	g_poll_groups_mask = NULL;
	return -1;
}

static const struct spdk_json_object_decoder nvmf_rpc_subsystem_tgt_conf_decoder[] = {
	{"conn_sched", offsetof(struct spdk_nvmf_tgt_conf, conn_sched), decode_conn_sched, true},
	{"admin_cmd_passthru", offsetof(struct spdk_nvmf_tgt_conf, admin_passthru), decode_admin_passthru, true},
	{"poll_groups_mask", 0, nvmf_decode_poll_groups_mask, true},
	{"discovery_filter", offsetof(struct spdk_nvmf_tgt_conf, discovery_filter), decode_discovery_filter, true}
};

static void
rpc_nvmf_set_config(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct spdk_nvmf_tgt_conf conf;

	memcpy(&conf, &g_spdk_nvmf_tgt_conf, sizeof(conf));

	if (params != NULL) {
		if (spdk_json_decode_object(params, nvmf_rpc_subsystem_tgt_conf_decoder,
					    SPDK_COUNTOF(nvmf_rpc_subsystem_tgt_conf_decoder), &conf)) {
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			return;
		}
	}

	memcpy(&g_spdk_nvmf_tgt_conf, &conf, sizeof(conf));

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("nvmf_set_config", rpc_nvmf_set_config, SPDK_RPC_STARTUP)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(nvmf_set_config, set_nvmf_target_config)

struct nvmf_rpc_set_crdt {
	uint16_t crdt1;
	uint16_t crdt2;
	uint16_t crdt3;
};

static const struct spdk_json_object_decoder rpc_set_crdt_opts_decoders[] = {
	{"crdt1", offsetof(struct nvmf_rpc_set_crdt, crdt1), spdk_json_decode_uint16, true},
	{"crdt2", offsetof(struct nvmf_rpc_set_crdt, crdt2), spdk_json_decode_uint16, true},
	{"crdt3", offsetof(struct nvmf_rpc_set_crdt, crdt3), spdk_json_decode_uint16, true},
};

static void
rpc_nvmf_set_crdt(struct spdk_jsonrpc_request *request,
		  const struct spdk_json_val *params)
{
	struct nvmf_rpc_set_crdt rpc_set_crdt;

	rpc_set_crdt.crdt1 = 0;
	rpc_set_crdt.crdt2 = 0;
	rpc_set_crdt.crdt3 = 0;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_set_crdt_opts_decoders,
					    SPDK_COUNTOF(rpc_set_crdt_opts_decoders), &rpc_set_crdt)) {
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			return;
		}
	}

	g_spdk_nvmf_tgt_crdt[0] = rpc_set_crdt.crdt1;
	g_spdk_nvmf_tgt_crdt[1] = rpc_set_crdt.crdt2;
	g_spdk_nvmf_tgt_crdt[2] = rpc_set_crdt.crdt3;

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("nvmf_set_crdt", rpc_nvmf_set_crdt, SPDK_RPC_STARTUP)
