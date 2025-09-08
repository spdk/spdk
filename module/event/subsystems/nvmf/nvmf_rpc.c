/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

	if (g_spdk_nvmf_tgt_conf.opts.max_subsystems != 0) {
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

	g_spdk_nvmf_tgt_conf.opts.max_subsystems = max_subsystems;

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("nvmf_set_max_subsystems", rpc_nvmf_set_max_subsystems,
		  SPDK_RPC_STARTUP)

static const struct spdk_json_object_decoder admin_passthru_decoder[] = {
	{"identify_ctrlr", offsetof(struct spdk_nvmf_admin_passthru_conf, identify_ctrlr), spdk_json_decode_bool, true},
	{"identify_uuid_list", offsetof(struct spdk_nvmf_admin_passthru_conf, identify_uuid_list), spdk_json_decode_bool, true},
	{"get_log_page", offsetof(struct spdk_nvmf_admin_passthru_conf, get_log_page), spdk_json_decode_bool, true},
	{"get_set_features", offsetof(struct spdk_nvmf_admin_passthru_conf, get_set_features), spdk_json_decode_bool, true},
	{"sanitize", offsetof(struct spdk_nvmf_admin_passthru_conf, sanitize), spdk_json_decode_bool, true},
	{"security_send_recv", offsetof(struct spdk_nvmf_admin_passthru_conf, security_send_recv), spdk_json_decode_bool, true},
	{"fw_update", offsetof(struct spdk_nvmf_admin_passthru_conf, fw_update), spdk_json_decode_bool, true},
	{"nvme_mi", offsetof(struct spdk_nvmf_admin_passthru_conf, nvme_mi), spdk_json_decode_bool, true},
	{"vendor_specific", offsetof(struct spdk_nvmf_admin_passthru_conf, vendor_specific), spdk_json_decode_bool, true},
};

static int
decode_admin_passthru(const struct spdk_json_val *val, void *out)
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
	uint32_t *_filter = out;
	uint32_t filter = SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY;
	char *tokens = spdk_json_strdup(val);
	char *tok;
	char *sp = NULL;
	int rc = -EINVAL;
	bool all_specified = false;

	if (!tokens) {
		return -ENOMEM;
	}

	tok = strtok_r(tokens, ",", &sp);
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

		tok = strtok_r(NULL, ",", &sp);
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

static int
decode_digest(const struct spdk_json_val *val, void *out)
{
	uint32_t *flags = out;
	char *digest = NULL;
	int rc;

	rc = spdk_json_decode_string(val, &digest);
	if (rc != 0) {
		return rc;
	}

	rc = spdk_nvme_dhchap_get_digest_id(digest);
	if (rc >= 0) {
		*flags |= SPDK_BIT(rc);
		rc = 0;
	}
	free(digest);

	return rc;
}

static int
decode_digest_array(const struct spdk_json_val *val, void *out)
{
	uint32_t *flags = out;
	size_t count;

	*flags = 0;

	return spdk_json_decode_array(val, decode_digest, out, 32, &count, 0);
}

static int
decode_dhgroup(const struct spdk_json_val *val, void *out)
{
	uint32_t *flags = out;
	char *dhgroup = NULL;
	int rc;

	rc = spdk_json_decode_string(val, &dhgroup);
	if (rc != 0) {
		return rc;
	}

	rc = spdk_nvme_dhchap_get_dhgroup_id(dhgroup);
	if (rc >= 0) {
		*flags |= SPDK_BIT(rc);
		rc = 0;
	}
	free(dhgroup);

	return rc;
}

static int
decode_dhgroup_array(const struct spdk_json_val *val, void *out)
{
	uint32_t *flags = out;
	size_t count;

	*flags = 0;

	return spdk_json_decode_array(val, decode_dhgroup, out, 32, &count, 0);
}

static const struct spdk_json_object_decoder nvmf_rpc_subsystem_tgt_conf_decoder[] = {
	{"admin_cmd_passthru", offsetof(struct spdk_nvmf_tgt_conf, admin_passthru), decode_admin_passthru, true},
	{"poll_groups_mask", 0, nvmf_decode_poll_groups_mask, true},
	{"discovery_filter", offsetof(struct spdk_nvmf_tgt_conf, opts.discovery_filter), decode_discovery_filter, true},
	{"dhchap_digests", offsetof(struct spdk_nvmf_tgt_conf, opts.dhchap_digests), decode_digest_array, true},
	{"dhchap_dhgroups", offsetof(struct spdk_nvmf_tgt_conf, opts.dhchap_dhgroups), decode_dhgroup_array, true},
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

	g_spdk_nvmf_tgt_conf.opts.crdt[0] = rpc_set_crdt.crdt1;
	g_spdk_nvmf_tgt_conf.opts.crdt[1] = rpc_set_crdt.crdt2;
	g_spdk_nvmf_tgt_conf.opts.crdt[2] = rpc_set_crdt.crdt3;

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("nvmf_set_crdt", rpc_nvmf_set_crdt, SPDK_RPC_STARTUP)
