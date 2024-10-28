/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/bdev.h"
#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/nvmf.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/bit_array.h"
#include "spdk/config.h"

#include "spdk_internal/assert.h"

#include "nvmf_internal.h"

static int rpc_ana_state_parse(const char *str, enum spdk_nvme_ana_state *ana_state);

static int
json_write_hex_str(struct spdk_json_write_ctx *w, const void *data, size_t size)
{
	static const char hex_char[16] = "0123456789ABCDEF";
	const uint8_t *buf = data;
	char *str, *out;
	int rc;

	str = malloc(size * 2 + 1);
	if (str == NULL) {
		return -1;
	}

	out = str;
	while (size--) {
		unsigned byte = *buf++;

		out[0] = hex_char[(byte >> 4) & 0xF];
		out[1] = hex_char[byte & 0xF];

		out += 2;
	}
	*out = '\0';

	rc = spdk_json_write_string(w, str);
	free(str);

	return rc;
}

static int
hex_nybble_to_num(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}

	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 0xA;
	}

	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 0xA;
	}

	return -1;
}

static int
hex_byte_to_num(const char *str)
{
	int hi, lo;

	hi = hex_nybble_to_num(str[0]);
	if (hi < 0) {
		return hi;
	}

	lo = hex_nybble_to_num(str[1]);
	if (lo < 0) {
		return lo;
	}

	return hi * 16 + lo;
}

static int
decode_hex_string_be(const char *str, uint8_t *out, size_t size)
{
	size_t i;

	/* Decode a string in "ABCDEF012345" format to its binary representation */
	for (i = 0; i < size; i++) {
		int num = hex_byte_to_num(str);

		if (num < 0) {
			/* Invalid hex byte or end of string */
			return -1;
		}

		out[i] = (uint8_t)num;
		str += 2;
	}

	if (i != size || *str != '\0') {
		/* Length mismatch */
		return -1;
	}

	return 0;
}

static int
decode_ns_nguid(const struct spdk_json_val *val, void *out)
{
	char *str = NULL;
	int rc;

	rc = spdk_json_decode_string(val, &str);
	if (rc == 0) {
		/* 16-byte NGUID */
		rc = decode_hex_string_be(str, out, 16);
	}

	free(str);
	return rc;
}

static int
decode_ns_eui64(const struct spdk_json_val *val, void *out)
{
	char *str = NULL;
	int rc;

	rc = spdk_json_decode_string(val, &str);
	if (rc == 0) {
		/* 8-byte EUI-64 */
		rc = decode_hex_string_be(str, out, 8);
	}

	free(str);
	return rc;
}

struct rpc_get_subsystem {
	char *nqn;
	char *tgt_name;
};

static const struct spdk_json_object_decoder rpc_get_subsystem_decoders[] = {
	{"nqn", offsetof(struct rpc_get_subsystem, nqn), spdk_json_decode_string, true},
	{"tgt_name", offsetof(struct rpc_get_subsystem, tgt_name), spdk_json_decode_string, true},
};

static void
dump_nvmf_subsystem(struct spdk_json_write_ctx *w, struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_host			*host;
	struct spdk_nvmf_subsystem_listener	*listener;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "nqn", spdk_nvmf_subsystem_get_nqn(subsystem));
	spdk_json_write_name(w, "subtype");
	if (spdk_nvmf_subsystem_get_type(subsystem) == SPDK_NVMF_SUBTYPE_NVME) {
		spdk_json_write_string(w, "NVMe");
	} else {
		spdk_json_write_string(w, "Discovery");
	}

	spdk_json_write_named_array_begin(w, "listen_addresses");

	for (listener = spdk_nvmf_subsystem_get_first_listener(subsystem); listener != NULL;
	     listener = spdk_nvmf_subsystem_get_next_listener(subsystem, listener)) {
		const struct spdk_nvme_transport_id *trid;

		trid = spdk_nvmf_subsystem_listener_get_trid(listener);

		spdk_json_write_object_begin(w);
		nvmf_transport_listen_dump_trid(trid, w);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	spdk_json_write_named_bool(w, "allow_any_host",
				   spdk_nvmf_subsystem_get_allow_any_host(subsystem));

	spdk_json_write_named_array_begin(w, "hosts");

	for (host = spdk_nvmf_subsystem_get_first_host(subsystem); host != NULL;
	     host = spdk_nvmf_subsystem_get_next_host(subsystem, host)) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "nqn", spdk_nvmf_host_get_nqn(host));
		if (host->dhchap_key != NULL) {
			spdk_json_write_named_string(w, "dhchap_key",
						     spdk_key_get_name(host->dhchap_key));
		}
		if (host->dhchap_ctrlr_key != NULL) {
			spdk_json_write_named_string(w, "dhchap_ctrlr_key",
						     spdk_key_get_name(host->dhchap_ctrlr_key));
		}
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	if (spdk_nvmf_subsystem_get_type(subsystem) == SPDK_NVMF_SUBTYPE_NVME) {
		struct spdk_nvmf_ns *ns;
		struct spdk_nvmf_ns_opts ns_opts;
		uint32_t max_namespaces;

		spdk_json_write_named_string(w, "serial_number", spdk_nvmf_subsystem_get_sn(subsystem));

		spdk_json_write_named_string(w, "model_number", spdk_nvmf_subsystem_get_mn(subsystem));

		max_namespaces = spdk_nvmf_subsystem_get_max_namespaces(subsystem);
		if (max_namespaces != 0) {
			spdk_json_write_named_uint32(w, "max_namespaces", max_namespaces);
		}

		spdk_json_write_named_uint32(w, "min_cntlid", spdk_nvmf_subsystem_get_min_cntlid(subsystem));
		spdk_json_write_named_uint32(w, "max_cntlid", spdk_nvmf_subsystem_get_max_cntlid(subsystem));

		spdk_json_write_named_array_begin(w, "namespaces");
		for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
		     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
			spdk_nvmf_ns_get_opts(ns, &ns_opts, sizeof(ns_opts));
			spdk_json_write_object_begin(w);
			spdk_json_write_named_int32(w, "nsid", spdk_nvmf_ns_get_id(ns));
			spdk_json_write_named_string(w, "bdev_name",
						     spdk_bdev_get_name(spdk_nvmf_ns_get_bdev(ns)));
			/* NOTE: "name" is kept for compatibility only - new code should use bdev_name. */
			spdk_json_write_named_string(w, "name",
						     spdk_bdev_get_name(spdk_nvmf_ns_get_bdev(ns)));

			if (!spdk_mem_all_zero(ns_opts.nguid, sizeof(ns_opts.nguid))) {
				spdk_json_write_name(w, "nguid");
				json_write_hex_str(w, ns_opts.nguid, sizeof(ns_opts.nguid));
			}

			if (!spdk_mem_all_zero(ns_opts.eui64, sizeof(ns_opts.eui64))) {
				spdk_json_write_name(w, "eui64");
				json_write_hex_str(w, ns_opts.eui64, sizeof(ns_opts.eui64));
			}

			if (!spdk_uuid_is_null(&ns_opts.uuid)) {
				spdk_json_write_named_uuid(w, "uuid", &ns_opts.uuid);
			}

			if (spdk_nvmf_subsystem_get_ana_reporting(subsystem)) {
				spdk_json_write_named_uint32(w, "anagrpid", ns_opts.anagrpid);
			}

			spdk_json_write_object_end(w);
		}
		spdk_json_write_array_end(w);
	}
	spdk_json_write_object_end(w);
}

static void
rpc_nvmf_get_subsystems(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_get_subsystem req = { 0 };
	struct spdk_json_write_ctx *w;
	struct spdk_nvmf_subsystem *subsystem = NULL;
	struct spdk_nvmf_tgt *tgt;

	if (params) {
		if (spdk_json_decode_object(params, rpc_get_subsystem_decoders,
					    SPDK_COUNTOF(rpc_get_subsystem_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			return;
		}
	}

	tgt = spdk_nvmf_get_tgt(req.tgt_name);
	if (!tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free(req.tgt_name);
		free(req.nqn);
		return;
	}

	if (req.nqn) {
		subsystem = spdk_nvmf_tgt_find_subsystem(tgt, req.nqn);
		if (!subsystem) {
			SPDK_ERRLOG("subsystem '%s' does not exist\n", req.nqn);
			spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
			free(req.tgt_name);
			free(req.nqn);
			return;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (subsystem) {
		dump_nvmf_subsystem(w, subsystem);
	} else {
		for (subsystem = spdk_nvmf_subsystem_get_first(tgt); subsystem != NULL;
		     subsystem = spdk_nvmf_subsystem_get_next(subsystem)) {
			dump_nvmf_subsystem(w, subsystem);
		}
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	free(req.tgt_name);
	free(req.nqn);
}
SPDK_RPC_REGISTER("nvmf_get_subsystems", rpc_nvmf_get_subsystems, SPDK_RPC_RUNTIME)

struct rpc_subsystem_create {
	char *nqn;
	char *serial_number;
	char *model_number;
	char *tgt_name;
	uint32_t max_namespaces;
	bool allow_any_host;
	bool ana_reporting;
	uint16_t min_cntlid;
	uint16_t max_cntlid;
	uint64_t max_discard_size_kib;
	uint64_t max_write_zeroes_size_kib;
	bool passthrough;
};

static const struct spdk_json_object_decoder rpc_subsystem_create_decoders[] = {
	{"nqn", offsetof(struct rpc_subsystem_create, nqn), spdk_json_decode_string},
	{"serial_number", offsetof(struct rpc_subsystem_create, serial_number), spdk_json_decode_string, true},
	{"model_number", offsetof(struct rpc_subsystem_create, model_number), spdk_json_decode_string, true},
	{"tgt_name", offsetof(struct rpc_subsystem_create, tgt_name), spdk_json_decode_string, true},
	{"max_namespaces", offsetof(struct rpc_subsystem_create, max_namespaces), spdk_json_decode_uint32, true},
	{"allow_any_host", offsetof(struct rpc_subsystem_create, allow_any_host), spdk_json_decode_bool, true},
	{"ana_reporting", offsetof(struct rpc_subsystem_create, ana_reporting), spdk_json_decode_bool, true},
	{"min_cntlid", offsetof(struct rpc_subsystem_create, min_cntlid), spdk_json_decode_uint16, true},
	{"max_cntlid", offsetof(struct rpc_subsystem_create, max_cntlid), spdk_json_decode_uint16, true},
	{"max_discard_size_kib", offsetof(struct rpc_subsystem_create, max_discard_size_kib), spdk_json_decode_uint64, true},
	{"max_write_zeroes_size_kib", offsetof(struct rpc_subsystem_create, max_write_zeroes_size_kib), spdk_json_decode_uint64, true},
	{"passthrough", offsetof(struct rpc_subsystem_create, passthrough), spdk_json_decode_bool, true},
};

static void
rpc_nvmf_subsystem_started(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (!status) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Subsystem %s start failed",
						     subsystem->subnqn);
		spdk_nvmf_subsystem_destroy(subsystem, NULL, NULL);
	}
}

static void
rpc_nvmf_create_subsystem(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_subsystem_create *req;
	struct spdk_nvmf_subsystem *subsystem = NULL;
	struct spdk_nvmf_tgt *tgt;
	int rc = -1;

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Memory allocation failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation failed");
		return;
	}
	req->min_cntlid = NVMF_MIN_CNTLID;
	req->max_cntlid = NVMF_MAX_CNTLID;

	if (spdk_json_decode_object(params, rpc_subsystem_create_decoders,
				    SPDK_COUNTOF(rpc_subsystem_create_decoders),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto cleanup;
	}

	tgt = spdk_nvmf_get_tgt(req->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find target %s\n", req->tgt_name);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Unable to find target %s", req->tgt_name);
		goto cleanup;
	}

	subsystem = spdk_nvmf_subsystem_create(tgt, req->nqn, SPDK_NVMF_SUBTYPE_NVME,
					       req->max_namespaces);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to create subsystem %s\n", req->nqn);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Unable to create subsystem %s", req->nqn);
		goto cleanup;
	}

	if (req->serial_number) {
		if (spdk_nvmf_subsystem_set_sn(subsystem, req->serial_number)) {
			SPDK_ERRLOG("Subsystem %s: invalid serial number '%s'\n", req->nqn, req->serial_number);
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							     "Invalid SN %s", req->serial_number);
			goto cleanup;
		}
	}

	if (req->model_number) {
		if (spdk_nvmf_subsystem_set_mn(subsystem, req->model_number)) {
			SPDK_ERRLOG("Subsystem %s: invalid model number '%s'\n", req->nqn, req->model_number);
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							     "Invalid MN %s", req->model_number);
			goto cleanup;
		}
	}

	spdk_nvmf_subsystem_set_allow_any_host(subsystem, req->allow_any_host);

	spdk_nvmf_subsystem_set_ana_reporting(subsystem, req->ana_reporting);

	if (spdk_nvmf_subsystem_set_cntlid_range(subsystem, req->min_cntlid, req->max_cntlid)) {
		SPDK_ERRLOG("Subsystem %s: invalid cntlid range [%u-%u]\n", req->nqn, req->min_cntlid,
			    req->max_cntlid);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid cntlid range [%u-%u]", req->min_cntlid, req->max_cntlid);
		goto cleanup;
	}

	subsystem->max_discard_size_kib = req->max_discard_size_kib;

	/* max_write_zeroes_size_kib must be aligned to 4 and power of 2 */
	if (req->max_write_zeroes_size_kib == 0 || (req->max_write_zeroes_size_kib > 2 &&
			spdk_u64_is_pow2(req->max_write_zeroes_size_kib))) {
		subsystem->max_write_zeroes_size_kib = req->max_write_zeroes_size_kib;
	} else {
		SPDK_ERRLOG("Subsystem %s: invalid max_write_zeroes_size_kib %"PRIu64"\n", req->nqn,
			    req->max_write_zeroes_size_kib);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid max_write_zeroes_size_kib %"PRIu64, req->max_write_zeroes_size_kib);
		goto cleanup;
	}

	subsystem->passthrough = req->passthrough;

	rc = spdk_nvmf_subsystem_start(subsystem,
				       rpc_nvmf_subsystem_started,
				       request);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to start subsystem");
	}

cleanup:
	free(req->nqn);
	free(req->tgt_name);
	free(req->serial_number);
	free(req->model_number);
	free(req);

	if (rc && subsystem) {
		spdk_nvmf_subsystem_destroy(subsystem, NULL, NULL);
	}
}
SPDK_RPC_REGISTER("nvmf_create_subsystem", rpc_nvmf_create_subsystem, SPDK_RPC_RUNTIME)

struct rpc_delete_subsystem {
	char *nqn;
	char *tgt_name;
};

static void
free_rpc_delete_subsystem(struct rpc_delete_subsystem *r)
{
	free(r->nqn);
	free(r->tgt_name);
}

static void
rpc_nvmf_subsystem_destroy_complete_cb(void *cb_arg)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_nvmf_subsystem_stopped(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	int rc;

	nvmf_subsystem_remove_all_listeners(subsystem, true);
	rc = spdk_nvmf_subsystem_destroy(subsystem, rpc_nvmf_subsystem_destroy_complete_cb, request);
	if (rc) {
		if (rc == -EINPROGRESS) {
			/* response will be sent in completion callback */
			return;
		} else {
			SPDK_ERRLOG("Subsystem destruction failed, rc %d\n", rc);
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							     "Subsystem destruction failed, rc %d", rc);
			return;
		}
	}
	spdk_jsonrpc_send_bool_response(request, true);
}

static const struct spdk_json_object_decoder rpc_delete_subsystem_decoders[] = {
	{"nqn", offsetof(struct rpc_delete_subsystem, nqn), spdk_json_decode_string},
	{"tgt_name", offsetof(struct rpc_delete_subsystem, tgt_name), spdk_json_decode_string, true},
};

static void
rpc_nvmf_delete_subsystem(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_delete_subsystem req = { 0 };
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	if (spdk_json_decode_object(params, rpc_delete_subsystem_decoders,
				    SPDK_COUNTOF(rpc_delete_subsystem_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.nqn == NULL) {
		SPDK_ERRLOG("missing name param\n");
		goto invalid;
	}

	tgt = spdk_nvmf_get_tgt(req.tgt_name);
	if (!tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		goto invalid_custom_response;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, req.nqn);
	if (!subsystem) {
		goto invalid;
	}

	free_rpc_delete_subsystem(&req);

	rc = spdk_nvmf_subsystem_stop(subsystem,
				      rpc_nvmf_subsystem_stopped,
				      request);
	if (rc == -EBUSY) {
		SPDK_ERRLOG("Subsystem currently in another state change try again later.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Subsystem currently in another state change try again later.");
	} else if (rc != 0) {
		SPDK_ERRLOG("Unable to change state on subsystem. rc=%d\n", rc);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Unable to change state on subsystem. rc=%d", rc);
	}

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
invalid_custom_response:
	free_rpc_delete_subsystem(&req);
}
SPDK_RPC_REGISTER("nvmf_delete_subsystem", rpc_nvmf_delete_subsystem, SPDK_RPC_RUNTIME)

struct rpc_listen_address {
	char *trtype;
	char *adrfam;
	char *traddr;
	char *trsvcid;
};

static const struct spdk_json_object_decoder rpc_listen_address_decoders[] = {
	{"trtype", offsetof(struct rpc_listen_address, trtype), spdk_json_decode_string, true},
	{"adrfam", offsetof(struct rpc_listen_address, adrfam), spdk_json_decode_string, true},
	{"traddr", offsetof(struct rpc_listen_address, traddr), spdk_json_decode_string},
	{"trsvcid", offsetof(struct rpc_listen_address, trsvcid), spdk_json_decode_string, true},
};

static int
decode_rpc_listen_address(const struct spdk_json_val *val, void *out)
{
	struct rpc_listen_address *req = (struct rpc_listen_address *)out;

	return spdk_json_decode_object(val, rpc_listen_address_decoders,
				       SPDK_COUNTOF(rpc_listen_address_decoders), req);
}

static void
free_rpc_listen_address(struct rpc_listen_address *r)
{
	free(r->trtype);
	free(r->adrfam);
	free(r->traddr);
	free(r->trsvcid);
}

enum nvmf_rpc_listen_op {
	NVMF_RPC_LISTEN_ADD,
	NVMF_RPC_LISTEN_REMOVE,
	NVMF_RPC_LISTEN_SET_ANA_STATE,
};

struct nvmf_rpc_listener_ctx {
	char				*nqn;
	char				*tgt_name;
	struct spdk_nvmf_tgt		*tgt;
	struct spdk_nvmf_transport	*transport;
	struct spdk_nvmf_subsystem	*subsystem;
	struct rpc_listen_address	address;
	char				*ana_state_str;
	enum spdk_nvme_ana_state	ana_state;
	uint32_t			anagrpid;

	struct spdk_jsonrpc_request	*request;
	struct spdk_nvme_transport_id	trid;
	enum nvmf_rpc_listen_op		op;
	bool				response_sent;
	struct spdk_nvmf_listen_opts	opts;

	/* Hole at bytes 705-711 */
	uint8_t reserved1[7];

	/* Additional options for listener creation.
	 * Must be 8-byte aligned. */
	struct spdk_nvmf_listener_opts	listener_opts;
};

static const struct spdk_json_object_decoder nvmf_rpc_listener_decoder[] = {
	{"nqn", offsetof(struct nvmf_rpc_listener_ctx, nqn), spdk_json_decode_string},
	{"listen_address", offsetof(struct nvmf_rpc_listener_ctx, address), decode_rpc_listen_address},
	{"tgt_name", offsetof(struct nvmf_rpc_listener_ctx, tgt_name), spdk_json_decode_string, true},
	{"secure_channel", offsetof(struct nvmf_rpc_listener_ctx, listener_opts.secure_channel), spdk_json_decode_bool, true},
	{"ana_state", offsetof(struct nvmf_rpc_listener_ctx, ana_state_str), spdk_json_decode_string, true},
	{"sock_impl", offsetof(struct nvmf_rpc_listener_ctx, listener_opts.sock_impl), spdk_json_decode_string, true},
};

static void
nvmf_rpc_listener_ctx_free(struct nvmf_rpc_listener_ctx *ctx)
{
	free(ctx->nqn);
	free(ctx->tgt_name);
	free_rpc_listen_address(&ctx->address);
	free(ctx->ana_state_str);
	free(ctx);
}

static void
nvmf_rpc_listen_resumed(struct spdk_nvmf_subsystem *subsystem,
			void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request;

	request = ctx->request;
	if (ctx->response_sent) {
		/* If an error occurred, the response has already been sent. */
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	nvmf_rpc_listener_ctx_free(ctx);

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
nvmf_rpc_subsystem_listen(void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;

	if (status) {
		/* Destroy the listener that we just created. Ignore the error code because
		 * the RPC is failing already anyway. */
		spdk_nvmf_tgt_stop_listen(ctx->tgt, &ctx->trid);

		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ctx->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, nvmf_rpc_listen_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}
		nvmf_rpc_listener_ctx_free(ctx);
		/* Can't really do anything to recover here - subsystem will remain paused. */
	}
}
static void
nvmf_rpc_stop_listen_async_done(void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;

	if (status) {
		SPDK_ERRLOG("Unable to stop listener.\n");
		spdk_jsonrpc_send_error_response_fmt(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "error stopping listener: %d", status);
		ctx->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, nvmf_rpc_listen_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}
		nvmf_rpc_listener_ctx_free(ctx);
		/* Can't really do anything to recover here - subsystem will remain paused. */
	}
}

static void
nvmf_rpc_set_ana_state_done(void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;

	if (status) {
		SPDK_ERRLOG("Unable to set ANA state.\n");
		spdk_jsonrpc_send_error_response_fmt(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "error setting ANA state: %d", status);
		ctx->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, nvmf_rpc_listen_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}
		nvmf_rpc_listener_ctx_free(ctx);
		/* Can't really do anything to recover here - subsystem will remain paused. */
	}
}

static void
nvmf_rpc_listen_paused(struct spdk_nvmf_subsystem *subsystem,
		       void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;
	int rc;

	switch (ctx->op) {
	case NVMF_RPC_LISTEN_ADD:
		if (nvmf_subsystem_find_listener(subsystem, &ctx->trid)) {
			SPDK_ERRLOG("Listener already exists\n");
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			ctx->response_sent = true;
			break;
		}

		rc = spdk_nvmf_tgt_listen_ext(ctx->tgt, &ctx->trid, &ctx->opts);
		if (rc) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			ctx->response_sent = true;
			break;
		}

		spdk_nvmf_subsystem_add_listener_ext(ctx->subsystem, &ctx->trid, nvmf_rpc_subsystem_listen, ctx,
						     &ctx->listener_opts);
		return;
	case NVMF_RPC_LISTEN_REMOVE:
		rc = spdk_nvmf_subsystem_remove_listener(subsystem, &ctx->trid);
		if (rc) {
			SPDK_ERRLOG("Unable to remove listener, rc %d\n", rc);
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			ctx->response_sent = true;
			break;
		}

		spdk_nvmf_transport_stop_listen_async(ctx->transport, &ctx->trid, subsystem,
						      nvmf_rpc_stop_listen_async_done, ctx);
		return;
	case NVMF_RPC_LISTEN_SET_ANA_STATE:
		spdk_nvmf_subsystem_set_ana_state(subsystem, &ctx->trid, ctx->ana_state, ctx->anagrpid,
						  nvmf_rpc_set_ana_state_done, ctx);
		return;
	default:
		SPDK_UNREACHABLE();
	}

	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_listen_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}

		nvmf_rpc_listener_ctx_free(ctx);
		/* Can't really do anything to recover here - subsystem will remain paused. */
	}
}

static int
rpc_listen_address_to_trid(const struct rpc_listen_address *address,
			   struct spdk_nvme_transport_id *trid)
{
	size_t len;

	memset(trid, 0, sizeof(*trid));

	if (spdk_nvme_transport_id_populate_trstring(trid, address->trtype)) {
		SPDK_ERRLOG("Invalid trtype string: %s\n", address->trtype);
		return -EINVAL;
	}

	if (spdk_nvme_transport_id_parse_trtype(&trid->trtype, address->trtype)) {
		SPDK_ERRLOG("Invalid trtype type: %s\n", address->trtype);
		return -EINVAL;
	}

	if (address->adrfam) {
		if (spdk_nvme_transport_id_parse_adrfam(&trid->adrfam, address->adrfam)) {
			SPDK_ERRLOG("Invalid adrfam: %s\n", address->adrfam);
			return -EINVAL;
		}
	} else {
		trid->adrfam = SPDK_NVMF_ADRFAM_IPV4;
	}

	len = strlen(address->traddr);
	if (len > sizeof(trid->traddr) - 1) {
		SPDK_ERRLOG("Transport address longer than %zu characters: %s\n",
			    sizeof(trid->traddr) - 1, address->traddr);
		return -EINVAL;
	}
	memcpy(trid->traddr, address->traddr, len + 1);

	trid->trsvcid[0] = '\0';
	if (address->trsvcid) {
		len = strlen(address->trsvcid);
		if (len > sizeof(trid->trsvcid) - 1) {
			SPDK_ERRLOG("Transport service id longer than %zu characters: %s\n",
				    sizeof(trid->trsvcid) - 1, address->trsvcid);
			return -EINVAL;
		}
		memcpy(trid->trsvcid, address->trsvcid, len + 1);
	}

	return 0;
}

static void
rpc_nvmf_subsystem_add_listener(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct nvmf_rpc_listener_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	ctx->request = request;

	spdk_nvmf_subsystem_listener_opts_init(&ctx->listener_opts, sizeof(ctx->listener_opts));

	if (spdk_json_decode_object_relaxed(params, nvmf_rpc_listener_decoder,
					    SPDK_COUNTOF(nvmf_rpc_listener_decoder),
					    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}
	ctx->tgt = tgt;

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->subsystem = subsystem;

	if (rpc_listen_address_to_trid(&ctx->address, &ctx->trid)) {
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->op = NVMF_RPC_LISTEN_ADD;
	spdk_nvmf_listen_opts_init(&ctx->opts, sizeof(ctx->opts));
	ctx->opts.transport_specific = params;
	if (spdk_nvmf_subsystem_get_allow_any_host(subsystem) && ctx->listener_opts.secure_channel) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Cannot establish secure channel, when 'allow_any_host' is set");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}
	ctx->opts.secure_channel = ctx->listener_opts.secure_channel;

	if (ctx->ana_state_str) {
		if (rpc_ana_state_parse(ctx->ana_state_str, &ctx->ana_state)) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			nvmf_rpc_listener_ctx_free(ctx);
			return;
		}
		ctx->listener_opts.ana_state = ctx->ana_state;
	}

	ctx->opts.sock_impl = ctx->listener_opts.sock_impl;

	rc = spdk_nvmf_subsystem_pause(subsystem, 0, nvmf_rpc_listen_paused, ctx);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_listener_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_add_listener", rpc_nvmf_subsystem_add_listener,
		  SPDK_RPC_RUNTIME);

static void
rpc_nvmf_subsystem_remove_listener(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct nvmf_rpc_listener_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	ctx->request = request;

	if (spdk_json_decode_object(params, nvmf_rpc_listener_decoder,
				    SPDK_COUNTOF(nvmf_rpc_listener_decoder),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}
	ctx->tgt = tgt;

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->subsystem = subsystem;

	if (rpc_listen_address_to_trid(&ctx->address, &ctx->trid)) {
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->transport = spdk_nvmf_tgt_get_transport(tgt, ctx->trid.trstring);
	if (!ctx->transport) {
		SPDK_ERRLOG("Unable to find %s transport. The transport must be created first also make sure it is properly registered.\n",
			    ctx->trid.trstring);
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->op = NVMF_RPC_LISTEN_REMOVE;

	rc = spdk_nvmf_subsystem_pause(subsystem, 0, nvmf_rpc_listen_paused, ctx);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_listener_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_remove_listener", rpc_nvmf_subsystem_remove_listener,
		  SPDK_RPC_RUNTIME);

struct nvmf_rpc_referral_ctx {
	char				*tgt_name;
	struct rpc_listen_address	address;
	bool				secure_channel;
	char				*subnqn;
};

static const struct spdk_json_object_decoder nvmf_rpc_referral_decoder[] = {
	{"address", offsetof(struct nvmf_rpc_referral_ctx, address), decode_rpc_listen_address},
	{"tgt_name", offsetof(struct nvmf_rpc_referral_ctx, tgt_name), spdk_json_decode_string, true},
	{"secure_channel", offsetof(struct nvmf_rpc_referral_ctx, secure_channel), spdk_json_decode_bool, true},
	{"subnqn", offsetof(struct nvmf_rpc_referral_ctx, subnqn), spdk_json_decode_string, true},
};

static void
nvmf_rpc_referral_ctx_free(struct nvmf_rpc_referral_ctx *ctx)
{
	free(ctx->tgt_name);
	free(ctx->subnqn);
	free_rpc_listen_address(&ctx->address);
}

static void
rpc_nvmf_add_referral(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	struct nvmf_rpc_referral_ctx ctx = {};
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvmf_tgt *tgt;
	struct spdk_nvmf_referral_opts opts = {};
	int rc;

	if (spdk_json_decode_object_relaxed(params, nvmf_rpc_referral_decoder,
					    SPDK_COUNTOF(nvmf_rpc_referral_decoder),
					    &ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx.tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	if (rpc_listen_address_to_trid(&ctx.address, &trid)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	if (ctx.subnqn != NULL) {
		rc = snprintf(trid.subnqn, sizeof(trid.subnqn), "%s", ctx.subnqn);
		if (rc < 0 || (size_t)rc >= sizeof(trid.subnqn)) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid subsystem NQN");
			nvmf_rpc_referral_ctx_free(&ctx);
			return;
		}
	}

	if ((trid.trtype == SPDK_NVME_TRANSPORT_TCP ||
	     trid.trtype == SPDK_NVME_TRANSPORT_RDMA) &&
	    !strlen(trid.trsvcid)) {
		SPDK_ERRLOG("Service ID is required.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Service ID is required.");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	opts.size = SPDK_SIZEOF(&opts, secure_channel);
	opts.trid = trid;
	opts.secure_channel = ctx.secure_channel;

	rc = spdk_nvmf_tgt_add_referral(tgt, &opts);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Internal error");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	nvmf_rpc_referral_ctx_free(&ctx);

	spdk_jsonrpc_send_bool_response(request, true);
}

SPDK_RPC_REGISTER("nvmf_discovery_add_referral", rpc_nvmf_add_referral,
		  SPDK_RPC_RUNTIME);

static void
rpc_nvmf_remove_referral(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct nvmf_rpc_referral_ctx ctx = {};
	struct spdk_nvme_transport_id trid = {};
	struct spdk_nvmf_referral_opts opts = {};
	struct spdk_nvmf_tgt *tgt;
	int rc;

	if (spdk_json_decode_object(params, nvmf_rpc_referral_decoder,
				    SPDK_COUNTOF(nvmf_rpc_referral_decoder),
				    &ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx.tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	if (rpc_listen_address_to_trid(&ctx.address, &trid)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	if (ctx.subnqn != NULL) {
		rc = snprintf(trid.subnqn, sizeof(trid.subnqn), "%s", ctx.subnqn);
		if (rc < 0 || (size_t)rc >= sizeof(trid.subnqn)) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid subsystem NQN");
			nvmf_rpc_referral_ctx_free(&ctx);
			return;
		}
	}

	opts.size = SPDK_SIZEOF(&opts, secure_channel);
	opts.trid = trid;

	if (spdk_nvmf_tgt_remove_referral(tgt, &opts)) {
		SPDK_ERRLOG("Failed to remove referral.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to remove a referral.");
		nvmf_rpc_referral_ctx_free(&ctx);
		return;
	}

	nvmf_rpc_referral_ctx_free(&ctx);

	spdk_jsonrpc_send_bool_response(request, true);
}

SPDK_RPC_REGISTER("nvmf_discovery_remove_referral", rpc_nvmf_remove_referral,
		  SPDK_RPC_RUNTIME);

static void
dump_nvmf_referral(struct spdk_json_write_ctx *w,
		   struct spdk_nvmf_referral *referral)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_named_object_begin(w, "address");
	nvmf_transport_listen_dump_trid(&referral->trid, w);
	spdk_json_write_object_end(w);
	spdk_json_write_named_bool(w, "secure_channel",
				   referral->entry.treq.secure_channel == SPDK_NVMF_TREQ_SECURE_CHANNEL_REQUIRED);
	spdk_json_write_named_string(w, "subnqn", referral->trid.subnqn);

	spdk_json_write_object_end(w);
}

struct rpc_get_referrals_ctx {
	char *tgt_name;
};

static const struct spdk_json_object_decoder rpc_get_referrals_decoders[] = {
	{"tgt_name", offsetof(struct rpc_get_referrals_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
free_rpc_get_referrals_ctx(struct rpc_get_referrals_ctx *ctx)
{
	free(ctx->tgt_name);
	free(ctx);
}

static void
rpc_nvmf_get_referrals(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_get_referrals_ctx *ctx;
	struct spdk_nvmf_tgt *tgt;
	struct spdk_json_write_ctx *w;
	struct spdk_nvmf_referral *referral;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Out of memory");
		return;
	}

	if (params) {
		if (spdk_json_decode_object(params, rpc_get_referrals_decoders,
					    SPDK_COUNTOF(rpc_get_referrals_decoders),
					    ctx)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			free_rpc_get_referrals_ctx(ctx);
			return;
		}
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target");
		free_rpc_get_referrals_ctx(ctx);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_array_begin(w);

	TAILQ_FOREACH(referral, &tgt->referrals, link) {
		dump_nvmf_referral(w, referral);
	}

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);

	free_rpc_get_referrals_ctx(ctx);
}
SPDK_RPC_REGISTER("nvmf_discovery_get_referrals", rpc_nvmf_get_referrals,
		  SPDK_RPC_RUNTIME);

static const struct spdk_json_object_decoder nvmf_rpc_set_ana_state_decoder[] = {
	{"nqn", offsetof(struct nvmf_rpc_listener_ctx, nqn), spdk_json_decode_string},
	{"listen_address", offsetof(struct nvmf_rpc_listener_ctx, address), decode_rpc_listen_address},
	{"ana_state", offsetof(struct nvmf_rpc_listener_ctx, ana_state_str), spdk_json_decode_string},
	{"tgt_name", offsetof(struct nvmf_rpc_listener_ctx, tgt_name), spdk_json_decode_string, true},
	{"anagrpid", offsetof(struct nvmf_rpc_listener_ctx, anagrpid), spdk_json_decode_uint32, true},
};

static int
rpc_ana_state_parse(const char *str, enum spdk_nvme_ana_state *ana_state)
{
	if (ana_state == NULL || str == NULL) {
		return -EINVAL;
	}

	if (strcasecmp(str, "optimized") == 0) {
		*ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	} else if (strcasecmp(str, "non_optimized") == 0) {
		*ana_state = SPDK_NVME_ANA_NON_OPTIMIZED_STATE;
	} else if (strcasecmp(str, "inaccessible") == 0) {
		*ana_state = SPDK_NVME_ANA_INACCESSIBLE_STATE;
	} else {
		return -ENOENT;
	}

	return 0;
}

static void
rpc_nvmf_subsystem_listener_set_ana_state(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct nvmf_rpc_listener_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Out of memory");
		return;
	}

	ctx->request = request;

	if (spdk_json_decode_object(params, nvmf_rpc_set_ana_state_decoder,
				    SPDK_COUNTOF(nvmf_rpc_set_ana_state_decoder),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->tgt = tgt;

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Unable to find subsystem with NQN %s",
						     ctx->nqn);
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->subsystem = subsystem;

	if (rpc_listen_address_to_trid(&ctx->address, &ctx->trid)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	if (rpc_ana_state_parse(ctx->ana_state_str, &ctx->ana_state)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->op = NVMF_RPC_LISTEN_SET_ANA_STATE;

	if (spdk_nvmf_subsystem_pause(subsystem, 0, nvmf_rpc_listen_paused, ctx)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Internal error");
		nvmf_rpc_listener_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_listener_set_ana_state",
		  rpc_nvmf_subsystem_listener_set_ana_state, SPDK_RPC_RUNTIME);

struct nvmf_rpc_ns_params {
	char *bdev_name;
	char *ptpl_file;
	uint32_t nsid;
	char nguid[16];
	char eui64[8];
	struct spdk_uuid uuid;
	uint32_t anagrpid;
	bool no_auto_visible;
};

static const struct spdk_json_object_decoder rpc_ns_params_decoders[] = {
	{"nsid", offsetof(struct nvmf_rpc_ns_params, nsid), spdk_json_decode_uint32, true},
	{"bdev_name", offsetof(struct nvmf_rpc_ns_params, bdev_name), spdk_json_decode_string},
	{"ptpl_file", offsetof(struct nvmf_rpc_ns_params, ptpl_file), spdk_json_decode_string, true},
	{"nguid", offsetof(struct nvmf_rpc_ns_params, nguid), decode_ns_nguid, true},
	{"eui64", offsetof(struct nvmf_rpc_ns_params, eui64), decode_ns_eui64, true},
	{"uuid", offsetof(struct nvmf_rpc_ns_params, uuid), spdk_json_decode_uuid, true},
	{"anagrpid", offsetof(struct nvmf_rpc_ns_params, anagrpid), spdk_json_decode_uint32, true},
	{"no_auto_visible", offsetof(struct nvmf_rpc_ns_params, no_auto_visible), spdk_json_decode_bool, true},
};

static int
decode_rpc_ns_params(const struct spdk_json_val *val, void *out)
{
	struct nvmf_rpc_ns_params *ns_params = out;

	return spdk_json_decode_object(val, rpc_ns_params_decoders,
				       SPDK_COUNTOF(rpc_ns_params_decoders),
				       ns_params);
}

struct nvmf_rpc_ns_ctx {
	char *nqn;
	char *tgt_name;
	struct nvmf_rpc_ns_params ns_params;

	struct spdk_jsonrpc_request *request;
	const struct spdk_json_val *params;
	bool response_sent;
};

static const struct spdk_json_object_decoder nvmf_rpc_subsystem_ns_decoder[] = {
	{"nqn", offsetof(struct nvmf_rpc_ns_ctx, nqn), spdk_json_decode_string},
	{"namespace", offsetof(struct nvmf_rpc_ns_ctx, ns_params), decode_rpc_ns_params},
	{"tgt_name", offsetof(struct nvmf_rpc_ns_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
nvmf_rpc_ns_ctx_free(struct nvmf_rpc_ns_ctx *ctx)
{
	free(ctx->nqn);
	free(ctx->tgt_name);
	free(ctx->ns_params.bdev_name);
	free(ctx->ns_params.ptpl_file);
	free(ctx);
}

static void
nvmf_rpc_ns_failback_resumed(struct spdk_nvmf_subsystem *subsystem,
			     void *cb_arg, int status)
{
	struct nvmf_rpc_ns_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request = ctx->request;

	if (status) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to add ns, subsystem in invalid state");
	} else {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to add ns, subsystem in active state");
	}

	nvmf_rpc_ns_ctx_free(ctx);
}

static void
nvmf_rpc_ns_resumed(struct spdk_nvmf_subsystem *subsystem,
		    void *cb_arg, int status)
{
	struct nvmf_rpc_ns_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request = ctx->request;
	uint32_t nsid = ctx->ns_params.nsid;
	bool response_sent = ctx->response_sent;
	struct spdk_json_write_ctx *w;
	int rc;

	/* The case where the call to add the namespace was successful, but the subsystem couldn't be resumed. */
	if (status && !ctx->response_sent) {
		rc = spdk_nvmf_subsystem_remove_ns(subsystem, nsid);
		if (rc != 0) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Unable to add ns, subsystem in invalid state");
			nvmf_rpc_ns_ctx_free(ctx);
			return;
		}

		rc = spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_ns_failback_resumed, ctx);
		if (rc != 0) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
			nvmf_rpc_ns_ctx_free(ctx);
			return;
		}

		return;
	}

	nvmf_rpc_ns_ctx_free(ctx);

	if (response_sent) {
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_uint32(w, nsid);
	spdk_jsonrpc_end_result(request, w);
}

static void
nvmf_rpc_ns_paused(struct spdk_nvmf_subsystem *subsystem,
		   void *cb_arg, int status)
{
	struct nvmf_rpc_ns_ctx *ctx = cb_arg;
	struct spdk_nvmf_ns_opts ns_opts;

	spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
	ns_opts.nsid = ctx->ns_params.nsid;
	ns_opts.transport_specific = ctx->params;

	SPDK_STATIC_ASSERT(sizeof(ns_opts.nguid) == sizeof(ctx->ns_params.nguid), "size mismatch");
	memcpy(ns_opts.nguid, ctx->ns_params.nguid, sizeof(ns_opts.nguid));

	SPDK_STATIC_ASSERT(sizeof(ns_opts.eui64) == sizeof(ctx->ns_params.eui64), "size mismatch");
	memcpy(ns_opts.eui64, ctx->ns_params.eui64, sizeof(ns_opts.eui64));

	if (!spdk_uuid_is_null(&ctx->ns_params.uuid)) {
		ns_opts.uuid = ctx->ns_params.uuid;
	}

	ns_opts.anagrpid = ctx->ns_params.anagrpid;
	ns_opts.no_auto_visible = ctx->ns_params.no_auto_visible;

	ctx->ns_params.nsid = spdk_nvmf_subsystem_add_ns_ext(subsystem, ctx->ns_params.bdev_name,
			      &ns_opts, sizeof(ns_opts),
			      ctx->ns_params.ptpl_file);
	if (ctx->ns_params.nsid == 0) {
		SPDK_ERRLOG("Unable to add namespace\n");
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ctx->response_sent = true;
		goto resume;
	}

resume:
	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_ns_resumed, ctx)) {
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_ns_ctx_free(ctx);
	}
}

static void
rpc_nvmf_subsystem_add_ns(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct nvmf_rpc_ns_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object_relaxed(params, nvmf_rpc_subsystem_ns_decoder,
					    SPDK_COUNTOF(nvmf_rpc_subsystem_ns_decoder), ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_ns_ctx_free(ctx);
		return;
	}

	ctx->request = request;
	ctx->params = params;
	ctx->response_sent = false;

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_ns_ctx_free(ctx);
		return;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_ns_ctx_free(ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_pause(subsystem, ctx->ns_params.nsid, nvmf_rpc_ns_paused, ctx);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_ns_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_add_ns", rpc_nvmf_subsystem_add_ns, SPDK_RPC_RUNTIME)

struct nvmf_rpc_ana_group_ctx {
	char *nqn;
	char *tgt_name;
	uint32_t nsid;
	uint32_t anagrpid;

	struct spdk_jsonrpc_request *request;
	bool response_sent;
};

static const struct spdk_json_object_decoder nvmf_rpc_subsystem_ana_group_decoder[] = {
	{"nqn", offsetof(struct nvmf_rpc_ana_group_ctx, nqn), spdk_json_decode_string},
	{"nsid", offsetof(struct nvmf_rpc_ana_group_ctx, nsid), spdk_json_decode_uint32},
	{"anagrpid", offsetof(struct nvmf_rpc_ana_group_ctx, anagrpid), spdk_json_decode_uint32},
	{"tgt_name", offsetof(struct nvmf_rpc_ana_group_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
nvmf_rpc_ana_group_ctx_free(struct nvmf_rpc_ana_group_ctx *ctx)
{
	free(ctx->nqn);
	free(ctx->tgt_name);
	free(ctx);
}

static void
nvmf_rpc_anagrpid_resumed(struct spdk_nvmf_subsystem *subsystem,
			  void *cb_arg, int status)
{
	struct nvmf_rpc_ana_group_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request = ctx->request;
	bool response_sent = ctx->response_sent;

	nvmf_rpc_ana_group_ctx_free(ctx);

	if (response_sent) {
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
nvmf_rpc_ana_group(struct spdk_nvmf_subsystem *subsystem,
		   void *cb_arg, int status)
{
	struct nvmf_rpc_ana_group_ctx *ctx = cb_arg;
	int rc;

	rc = spdk_nvmf_subsystem_set_ns_ana_group(subsystem, ctx->nsid, ctx->anagrpid);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to change ANA group ID\n");
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ctx->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_anagrpid_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Internal error");
		}
		nvmf_rpc_ana_group_ctx_free(ctx);
	}
}

static void
rpc_nvmf_subsystem_set_ns_ana_group(struct spdk_jsonrpc_request *request,
				    const struct spdk_json_val *params)
{
	struct nvmf_rpc_ana_group_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object(params, nvmf_rpc_subsystem_ana_group_decoder,
				    SPDK_COUNTOF(nvmf_rpc_subsystem_ana_group_decoder), ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_ana_group_ctx_free(ctx);
		return;
	}

	ctx->request = request;
	ctx->response_sent = false;

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_ana_group_ctx_free(ctx);
		return;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_ana_group_ctx_free(ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_pause(subsystem, ctx->nsid, nvmf_rpc_ana_group, ctx);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_ana_group_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_set_ns_ana_group", rpc_nvmf_subsystem_set_ns_ana_group,
		  SPDK_RPC_RUNTIME)

struct nvmf_rpc_remove_ns_ctx {
	char *nqn;
	char *tgt_name;
	uint32_t nsid;

	struct spdk_jsonrpc_request *request;
	bool response_sent;
};

static const struct spdk_json_object_decoder nvmf_rpc_subsystem_remove_ns_decoder[] = {
	{"nqn", offsetof(struct nvmf_rpc_remove_ns_ctx, nqn), spdk_json_decode_string},
	{"nsid", offsetof(struct nvmf_rpc_remove_ns_ctx, nsid), spdk_json_decode_uint32},
	{"tgt_name", offsetof(struct nvmf_rpc_remove_ns_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
nvmf_rpc_remove_ns_ctx_free(struct nvmf_rpc_remove_ns_ctx *ctx)
{
	free(ctx->nqn);
	free(ctx->tgt_name);
	free(ctx);
}

static void
nvmf_rpc_remove_ns_resumed(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	struct nvmf_rpc_remove_ns_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request = ctx->request;
	bool response_sent = ctx->response_sent;

	nvmf_rpc_remove_ns_ctx_free(ctx);

	if (response_sent) {
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
nvmf_rpc_remove_ns_paused(struct spdk_nvmf_subsystem *subsystem,
			  void *cb_arg, int status)
{
	struct nvmf_rpc_remove_ns_ctx *ctx = cb_arg;
	int ret;

	ret = spdk_nvmf_subsystem_remove_ns(subsystem, ctx->nsid);
	if (ret < 0) {
		SPDK_ERRLOG("Unable to remove namespace ID %u\n", ctx->nsid);
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ctx->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_remove_ns_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		}
		nvmf_rpc_remove_ns_ctx_free(ctx);
	}
}

static void
rpc_nvmf_subsystem_remove_ns(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct nvmf_rpc_remove_ns_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object(params, nvmf_rpc_subsystem_remove_ns_decoder,
				    SPDK_COUNTOF(nvmf_rpc_subsystem_remove_ns_decoder),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_remove_ns_ctx_free(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_remove_ns_ctx_free(ctx);
		return;
	}

	ctx->request = request;
	ctx->response_sent = false;

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_remove_ns_ctx_free(ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_pause(subsystem, ctx->nsid, nvmf_rpc_remove_ns_paused, ctx);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_remove_ns_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_remove_ns", rpc_nvmf_subsystem_remove_ns, SPDK_RPC_RUNTIME)

struct nvmf_rpc_ns_visible_ctx {
	struct spdk_jsonrpc_request *request;
	char *nqn;
	uint32_t nsid;
	char *host;
	char *tgt_name;
	bool visible;
	bool response_sent;
};

static const struct spdk_json_object_decoder nvmf_rpc_ns_visible_decoder[] = {
	{"nqn", offsetof(struct nvmf_rpc_ns_visible_ctx, nqn), spdk_json_decode_string},
	{"nsid", offsetof(struct nvmf_rpc_ns_visible_ctx, nsid), spdk_json_decode_uint32},
	{"host", offsetof(struct nvmf_rpc_ns_visible_ctx, host), spdk_json_decode_string},
	{"tgt_name", offsetof(struct nvmf_rpc_ns_visible_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
nvmf_rpc_ns_visible_ctx_free(struct nvmf_rpc_ns_visible_ctx *ctx)
{
	free(ctx->nqn);
	free(ctx->host);
	free(ctx->tgt_name);
	free(ctx);
}

static void
nvmf_rpc_ns_visible_resumed(struct spdk_nvmf_subsystem *subsystem,
			    void *cb_arg, int status)
{
	struct nvmf_rpc_ns_visible_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request = ctx->request;
	bool response_sent = ctx->response_sent;

	nvmf_rpc_ns_visible_ctx_free(ctx);

	if (!response_sent) {
		spdk_jsonrpc_send_bool_response(request, true);
	}
}

static void
nvmf_rpc_ns_visible_paused(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	struct nvmf_rpc_ns_visible_ctx *ctx = cb_arg;
	int ret;

	if (ctx->visible) {
		ret = spdk_nvmf_ns_add_host(subsystem, ctx->nsid, ctx->host, 0);
	} else {
		ret = spdk_nvmf_ns_remove_host(subsystem, ctx->nsid, ctx->host, 0);
	}
	if (ret < 0) {
		SPDK_ERRLOG("Unable to add/remove %s to namespace ID %u\n", ctx->host, ctx->nsid);
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ctx->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_ns_visible_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		}
		nvmf_rpc_ns_visible_ctx_free(ctx);
	}
}

static void
nvmf_rpc_ns_visible(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params,
		    bool visible)
{
	struct nvmf_rpc_ns_visible_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}
	ctx->visible = visible;

	if (spdk_json_decode_object(params, nvmf_rpc_ns_visible_decoder,
				    SPDK_COUNTOF(nvmf_rpc_ns_visible_decoder),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_ns_visible_ctx_free(ctx);
		return;
	}
	ctx->request = request;

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_ns_visible_ctx_free(ctx);
		return;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_ns_visible_ctx_free(ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_pause(subsystem, ctx->nsid, nvmf_rpc_ns_visible_paused, ctx);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_ns_visible_ctx_free(ctx);
	}
}

static void
rpc_nvmf_ns_add_host(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	nvmf_rpc_ns_visible(request, params, true);
}
SPDK_RPC_REGISTER("nvmf_ns_add_host", rpc_nvmf_ns_add_host, SPDK_RPC_RUNTIME)

static void
rpc_nvmf_ns_remove_host(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	nvmf_rpc_ns_visible(request, params, false);
}
SPDK_RPC_REGISTER("nvmf_ns_remove_host", rpc_nvmf_ns_remove_host, SPDK_RPC_RUNTIME)

struct nvmf_rpc_host_ctx {
	struct spdk_jsonrpc_request *request;
	char *nqn;
	char *host;
	char *tgt_name;
	char *dhchap_key;
	char *dhchap_ctrlr_key;
	bool allow_any_host;
};

static const struct spdk_json_object_decoder nvmf_rpc_subsystem_host_decoder[] = {
	{"nqn", offsetof(struct nvmf_rpc_host_ctx, nqn), spdk_json_decode_string},
	{"host", offsetof(struct nvmf_rpc_host_ctx, host), spdk_json_decode_string},
	{"tgt_name", offsetof(struct nvmf_rpc_host_ctx, tgt_name), spdk_json_decode_string, true},
	{"dhchap_key", offsetof(struct nvmf_rpc_host_ctx, dhchap_key), spdk_json_decode_string, true},
	{"dhchap_ctrlr_key", offsetof(struct nvmf_rpc_host_ctx, dhchap_ctrlr_key), spdk_json_decode_string, true},
};

static void
nvmf_rpc_host_ctx_free(struct nvmf_rpc_host_ctx *ctx)
{
	free(ctx->nqn);
	free(ctx->host);
	free(ctx->tgt_name);
	free(ctx->dhchap_key);
	free(ctx->dhchap_ctrlr_key);
}

static void
rpc_nvmf_subsystem_add_host(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct nvmf_rpc_host_ctx ctx = {};
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_host_opts opts = {};
	struct spdk_nvmf_tgt *tgt;
	struct spdk_key *key = NULL, *ckey = NULL;
	int rc;

	if (spdk_json_decode_object_relaxed(params, nvmf_rpc_subsystem_host_decoder,
					    SPDK_COUNTOF(nvmf_rpc_subsystem_host_decoder),
					    &ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	tgt = spdk_nvmf_get_tgt(ctx.tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		goto out;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx.nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx.nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	if (ctx.dhchap_key != NULL) {
		key = spdk_keyring_get_key(ctx.dhchap_key);
		if (key == NULL) {
			SPDK_ERRLOG("Unable to find DH-HMAC-CHAP key: %s\n", ctx.dhchap_key);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			goto out;
		}
	}

	if (ctx.dhchap_ctrlr_key != NULL) {
		ckey = spdk_keyring_get_key(ctx.dhchap_ctrlr_key);
		if (ckey == NULL) {
			SPDK_ERRLOG("Unable to find DH-HMAC-CHAP ctrlr key: %s\n",
				    ctx.dhchap_ctrlr_key);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			goto out;
		}
	}

	opts.size = SPDK_SIZEOF(&opts, dhchap_ctrlr_key);
	opts.params = params;
	opts.dhchap_key = key;
	opts.dhchap_ctrlr_key = ckey;
	rc = spdk_nvmf_subsystem_add_host_ext(subsystem, ctx.host, &opts);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		goto out;
	}

	spdk_jsonrpc_send_bool_response(request, true);
out:
	spdk_keyring_put_key(ckey);
	spdk_keyring_put_key(key);
	nvmf_rpc_host_ctx_free(&ctx);
}
SPDK_RPC_REGISTER("nvmf_subsystem_add_host", rpc_nvmf_subsystem_add_host, SPDK_RPC_RUNTIME)

static void
rpc_nvmf_subsystem_remove_host_done(void *_ctx, int status)
{
	struct nvmf_rpc_host_ctx *ctx = _ctx;

	spdk_jsonrpc_send_bool_response(ctx->request, true);
	nvmf_rpc_host_ctx_free(ctx);
	free(ctx);
}

static void
rpc_nvmf_subsystem_remove_host(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct nvmf_rpc_host_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Unable to allocate context to perform RPC\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	ctx->request = request;

	if (spdk_json_decode_object(params, nvmf_rpc_subsystem_host_decoder,
				    SPDK_COUNTOF(nvmf_rpc_subsystem_host_decoder),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_host_ctx_free(ctx);
		free(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_host_ctx_free(ctx);
		free(ctx);
		return;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_host_ctx_free(ctx);
		free(ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_remove_host(subsystem, ctx->host);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_host_ctx_free(ctx);
		free(ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_disconnect_host(subsystem, ctx->host,
			rpc_nvmf_subsystem_remove_host_done,
			ctx);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_host_ctx_free(ctx);
		free(ctx);
		return;
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_remove_host", rpc_nvmf_subsystem_remove_host,
		  SPDK_RPC_RUNTIME)

static void
rpc_nvmf_subsystem_set_keys(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct nvmf_rpc_host_ctx ctx = {};
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_subsystem_key_opts opts = {};
	struct spdk_nvmf_tgt *tgt;
	struct spdk_key *key = NULL, *ckey = NULL;
	int rc;

	if (spdk_json_decode_object(params, nvmf_rpc_subsystem_host_decoder,
				    SPDK_COUNTOF(nvmf_rpc_subsystem_host_decoder), &ctx)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto out;
	}

	tgt = spdk_nvmf_get_tgt(ctx.tgt_name);
	if (!tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Invalid parameters");
		goto out;
	}
	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx.nqn);
	if (!subsystem) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto out;
	}

	if (ctx.dhchap_key != NULL) {
		key = spdk_keyring_get_key(ctx.dhchap_key);
		if (key == NULL) {
			SPDK_ERRLOG("Unable to find DH-HMAC-CHAP key: %s\n", ctx.dhchap_key);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			goto out;
		}
	}
	if (ctx.dhchap_ctrlr_key != NULL) {
		ckey = spdk_keyring_get_key(ctx.dhchap_ctrlr_key);
		if (ckey == NULL) {
			SPDK_ERRLOG("Unable to find DH-HMAC-CHAP ctrlr key: %s\n",
				    ctx.dhchap_ctrlr_key);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			goto out;
		}
	}

	opts.size = SPDK_SIZEOF(&opts, dhchap_ctrlr_key);
	opts.dhchap_key = key;
	opts.dhchap_ctrlr_key = ckey;
	rc = spdk_nvmf_subsystem_set_keys(subsystem, ctx.host, &opts);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto out;
	}

	spdk_jsonrpc_send_bool_response(request, true);
out:
	spdk_keyring_put_key(ckey);
	spdk_keyring_put_key(key);
	nvmf_rpc_host_ctx_free(&ctx);
}
SPDK_RPC_REGISTER("nvmf_subsystem_set_keys", rpc_nvmf_subsystem_set_keys, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder nvmf_rpc_subsystem_any_host_decoder[] = {
	{"nqn", offsetof(struct nvmf_rpc_host_ctx, nqn), spdk_json_decode_string},
	{"allow_any_host", offsetof(struct nvmf_rpc_host_ctx, allow_any_host), spdk_json_decode_bool},
	{"tgt_name", offsetof(struct nvmf_rpc_host_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
rpc_nvmf_subsystem_allow_any_host(struct spdk_jsonrpc_request *request,
				  const struct spdk_json_val *params)
{
	struct nvmf_rpc_host_ctx ctx = {};
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	if (spdk_json_decode_object(params, nvmf_rpc_subsystem_any_host_decoder,
				    SPDK_COUNTOF(nvmf_rpc_subsystem_any_host_decoder),
				    &ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_host_ctx_free(&ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx.tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_host_ctx_free(&ctx);
		return;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx.nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx.nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_host_ctx_free(&ctx);
		return;
	}

	rc = spdk_nvmf_subsystem_set_allow_any_host(subsystem, ctx.allow_any_host);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_host_ctx_free(&ctx);
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	nvmf_rpc_host_ctx_free(&ctx);
}
SPDK_RPC_REGISTER("nvmf_subsystem_allow_any_host", rpc_nvmf_subsystem_allow_any_host,
		  SPDK_RPC_RUNTIME)

struct nvmf_rpc_target_ctx {
	char *name;
	uint32_t max_subsystems;
	char *discovery_filter;
};

static int
decode_discovery_filter(const struct spdk_json_val *val, void *out)
{
	uint32_t *_filter = out;
	uint32_t filter = SPDK_NVMF_TGT_DISCOVERY_MATCH_ANY;
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

static const struct spdk_json_object_decoder nvmf_rpc_create_target_decoder[] = {
	{"name", offsetof(struct nvmf_rpc_target_ctx, name), spdk_json_decode_string},
	{"max_subsystems", offsetof(struct nvmf_rpc_target_ctx, max_subsystems), spdk_json_decode_uint32, true},
	{"discovery_filter", offsetof(struct nvmf_rpc_target_ctx, discovery_filter), decode_discovery_filter, true}
};

static void
rpc_nvmf_create_target(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct spdk_nvmf_target_opts	opts;
	struct nvmf_rpc_target_ctx	ctx = {0};
	struct spdk_nvmf_tgt		*tgt;
	struct spdk_json_write_ctx	*w;

	/* Decode parameters the first time to get the transport type */
	if (spdk_json_decode_object(params, nvmf_rpc_create_target_decoder,
				    SPDK_COUNTOF(nvmf_rpc_create_target_decoder),
				    &ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	snprintf(opts.name, NVMF_TGT_NAME_MAX_LENGTH, "%s", ctx.name);
	opts.max_subsystems = ctx.max_subsystems;
	opts.size = SPDK_SIZEOF(&opts, discovery_filter);

	if (spdk_nvmf_get_tgt(opts.name) != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Target already exists.");
		goto out;
	}

	tgt = spdk_nvmf_tgt_create(&opts);

	if (tgt == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to create the requested target.");
		goto out;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_nvmf_tgt_get_name(tgt));
	spdk_jsonrpc_end_result(request, w);
out:
	free(ctx.name);
	free(ctx.discovery_filter);
}
/* private */ SPDK_RPC_REGISTER("nvmf_create_target", rpc_nvmf_create_target, SPDK_RPC_RUNTIME);

static const struct spdk_json_object_decoder nvmf_rpc_destroy_target_decoder[] = {
	{"name", offsetof(struct nvmf_rpc_target_ctx, name), spdk_json_decode_string},
};

static void
nvmf_rpc_destroy_target_done(void *ctx, int status)
{
	struct spdk_jsonrpc_request	*request = ctx;

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_nvmf_delete_target(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct nvmf_rpc_target_ctx	ctx = {0};
	struct spdk_nvmf_tgt		*tgt;

	/* Decode parameters the first time to get the transport type */
	if (spdk_json_decode_object(params, nvmf_rpc_destroy_target_decoder,
				    SPDK_COUNTOF(nvmf_rpc_destroy_target_decoder),
				    &ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		free(ctx.name);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx.name);

	if (tgt == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "The specified target doesn't exist, cannot delete it.");
		free(ctx.name);
		return;
	}

	spdk_nvmf_tgt_destroy(tgt, nvmf_rpc_destroy_target_done, request);
	free(ctx.name);
}
/* private */ SPDK_RPC_REGISTER("nvmf_delete_target", rpc_nvmf_delete_target, SPDK_RPC_RUNTIME);

static void
rpc_nvmf_get_targets(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx	*w;
	struct spdk_nvmf_tgt		*tgt;
	const char			*name;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "nvmf_get_targets has no parameters.");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	tgt = spdk_nvmf_get_first_tgt();

	while (tgt != NULL) {
		name = spdk_nvmf_tgt_get_name(tgt);
		spdk_json_write_string(w, name);
		tgt = spdk_nvmf_get_next_tgt(tgt);
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}
/* private */ SPDK_RPC_REGISTER("nvmf_get_targets", rpc_nvmf_get_targets, SPDK_RPC_RUNTIME);

struct nvmf_rpc_create_transport_ctx {
	char				*trtype;
	char				*tgt_name;
	struct spdk_nvmf_transport_opts	opts;
	struct spdk_jsonrpc_request	*request;
	struct spdk_nvmf_transport	*transport;
	int				status;
};

/**
 * `max_qpairs_per_ctrlr` represents both admin and IO qpairs, that confuses
 * users when they configure a transport using RPC. So it was decided to
 * deprecate `max_qpairs_per_ctrlr` RPC parameter and use `max_io_qpairs_per_ctrlr`
 * But internal logic remains unchanged and SPDK expects that
 * spdk_nvmf_transport_opts::max_qpairs_per_ctrlr includes an admin qpair.
 * This function parses the number of IO qpairs and adds +1 for admin qpair.
 */
static int
nvmf_rpc_decode_max_io_qpairs(const struct spdk_json_val *val, void *out)
{
	uint16_t *i = out;
	int rc;

	rc = spdk_json_number_to_uint16(val, i);
	if (rc == 0) {
		(*i)++;
	}

	return rc;
}

static const struct spdk_json_object_decoder nvmf_rpc_create_transport_decoder[] = {
	{	"trtype", offsetof(struct nvmf_rpc_create_transport_ctx, trtype), spdk_json_decode_string},
	{
		"max_queue_depth", offsetof(struct nvmf_rpc_create_transport_ctx, opts.max_queue_depth),
		spdk_json_decode_uint16, true
	},
	{
		"max_io_qpairs_per_ctrlr", offsetof(struct nvmf_rpc_create_transport_ctx, opts.max_qpairs_per_ctrlr),
		nvmf_rpc_decode_max_io_qpairs, true
	},
	{
		"in_capsule_data_size", offsetof(struct nvmf_rpc_create_transport_ctx, opts.in_capsule_data_size),
		spdk_json_decode_uint32, true
	},
	{
		"max_io_size", offsetof(struct nvmf_rpc_create_transport_ctx, opts.max_io_size),
		spdk_json_decode_uint32, true
	},
	{
		"io_unit_size", offsetof(struct nvmf_rpc_create_transport_ctx, opts.io_unit_size),
		spdk_json_decode_uint32, true
	},
	{
		"max_aq_depth", offsetof(struct nvmf_rpc_create_transport_ctx, opts.max_aq_depth),
		spdk_json_decode_uint32, true
	},
	{
		"num_shared_buffers", offsetof(struct nvmf_rpc_create_transport_ctx, opts.num_shared_buffers),
		spdk_json_decode_uint32, true
	},
	{
		"buf_cache_size", offsetof(struct nvmf_rpc_create_transport_ctx, opts.buf_cache_size),
		spdk_json_decode_uint32, true
	},
	{
		"dif_insert_or_strip", offsetof(struct nvmf_rpc_create_transport_ctx, opts.dif_insert_or_strip),
		spdk_json_decode_bool, true
	},
	{
		"abort_timeout_sec", offsetof(struct nvmf_rpc_create_transport_ctx, opts.abort_timeout_sec),
		spdk_json_decode_uint32, true
	},
	{
		"zcopy", offsetof(struct nvmf_rpc_create_transport_ctx, opts.zcopy),
		spdk_json_decode_bool, true
	},
	{
		"tgt_name", offsetof(struct nvmf_rpc_create_transport_ctx, tgt_name),
		spdk_json_decode_string, true
	},
	{
		"acceptor_poll_rate", offsetof(struct nvmf_rpc_create_transport_ctx, opts.acceptor_poll_rate),
		spdk_json_decode_uint32, true
	},
	{
		"ack_timeout", offsetof(struct nvmf_rpc_create_transport_ctx, opts.ack_timeout),
		spdk_json_decode_uint32, true
	},
	{
		"data_wr_pool_size", offsetof(struct nvmf_rpc_create_transport_ctx, opts.data_wr_pool_size),
		spdk_json_decode_uint32, true
	},
	{
		"disable_command_passthru", offsetof(struct nvmf_rpc_create_transport_ctx, opts.disable_command_passthru),
		spdk_json_decode_bool, true
	}
};

static void
nvmf_rpc_create_transport_ctx_free(struct nvmf_rpc_create_transport_ctx *ctx)
{
	free(ctx->trtype);
	free(ctx->tgt_name);
	free(ctx);
}

static void
nvmf_rpc_transport_destroy_done_cb(void *cb_arg)
{
	struct nvmf_rpc_create_transport_ctx *ctx = cb_arg;

	spdk_jsonrpc_send_error_response_fmt(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					     "Failed to add transport to tgt.(%d)", ctx->status);
	nvmf_rpc_create_transport_ctx_free(ctx);
}

static void
nvmf_rpc_tgt_add_transport_done(void *cb_arg, int status)
{
	struct nvmf_rpc_create_transport_ctx *ctx = cb_arg;

	if (status) {
		SPDK_ERRLOG("Failed to add transport to tgt.(%d)\n", status);
		ctx->status = status;
		spdk_nvmf_transport_destroy(ctx->transport, nvmf_rpc_transport_destroy_done_cb, ctx);
		return;
	}

	spdk_jsonrpc_send_bool_response(ctx->request, true);
	nvmf_rpc_create_transport_ctx_free(ctx);
}

static void
nvmf_rpc_create_transport_done(void *cb_arg, struct spdk_nvmf_transport *transport)
{
	struct nvmf_rpc_create_transport_ctx *ctx = cb_arg;

	if (!transport) {
		SPDK_ERRLOG("Failed to create transport.\n");
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to create transport.");
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	ctx->transport = transport;

	spdk_nvmf_tgt_add_transport(spdk_nvmf_get_tgt(ctx->tgt_name), transport,
				    nvmf_rpc_tgt_add_transport_done, ctx);
}

static void
rpc_nvmf_create_transport(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct nvmf_rpc_create_transport_ctx *ctx;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	/* Decode parameters the first time to get the transport type */
	if (spdk_json_decode_object_relaxed(params, nvmf_rpc_create_transport_decoder,
					    SPDK_COUNTOF(nvmf_rpc_create_transport_decoder),
					    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	/* Initialize all the transport options (based on transport type) and decode the
	 * parameters again to update any options passed in rpc create transport call.
	 */
	if (!spdk_nvmf_transport_opts_init(ctx->trtype, &ctx->opts, sizeof(ctx->opts))) {
		/* This can happen if user specifies PCIE transport type which isn't valid for
		 * NVMe-oF.
		 */
		SPDK_ERRLOG("Invalid transport type '%s'\n", ctx->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid transport type '%s'", ctx->trtype);
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	if (spdk_json_decode_object_relaxed(params, nvmf_rpc_create_transport_decoder,
					    SPDK_COUNTOF(nvmf_rpc_create_transport_decoder),
					    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	if (spdk_nvmf_tgt_get_transport(tgt, ctx->trtype)) {
		SPDK_ERRLOG("Transport type '%s' already exists\n", ctx->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Transport type '%s' already exists", ctx->trtype);
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	/* Transport can parse additional params themselves */
	ctx->opts.transport_specific = params;
	ctx->request = request;

	rc = spdk_nvmf_transport_create_async(ctx->trtype, &ctx->opts, nvmf_rpc_create_transport_done, ctx);
	if (rc) {
		SPDK_ERRLOG("Transport type '%s' create failed\n", ctx->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Transport type '%s' create failed", ctx->trtype);
		nvmf_rpc_create_transport_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_create_transport", rpc_nvmf_create_transport, SPDK_RPC_RUNTIME)

struct rpc_get_transport {
	char *trtype;
	char *tgt_name;
};

static const struct spdk_json_object_decoder rpc_get_transport_decoders[] = {
	{"trtype", offsetof(struct rpc_get_transport, trtype), spdk_json_decode_string, true},
	{"tgt_name", offsetof(struct rpc_get_transport, tgt_name), spdk_json_decode_string, true},
};

static void
rpc_nvmf_get_transports(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_get_transport req = { 0 };
	struct spdk_json_write_ctx *w;
	struct spdk_nvmf_transport *transport = NULL;
	struct spdk_nvmf_tgt *tgt;

	if (params) {
		if (spdk_json_decode_object(params, rpc_get_transport_decoders,
					    SPDK_COUNTOF(rpc_get_transport_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			return;
		}
	}

	tgt = spdk_nvmf_get_tgt(req.tgt_name);
	if (!tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free(req.trtype);
		free(req.tgt_name);
		return;
	}

	if (req.trtype) {
		transport = spdk_nvmf_tgt_get_transport(tgt, req.trtype);
		if (transport == NULL) {
			SPDK_ERRLOG("transport '%s' does not exist\n", req.trtype);
			spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
			free(req.trtype);
			free(req.tgt_name);
			return;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (transport) {
		nvmf_transport_dump_opts(transport, w, false);
	} else {
		for (transport = spdk_nvmf_transport_get_first(tgt); transport != NULL;
		     transport = spdk_nvmf_transport_get_next(transport)) {
			nvmf_transport_dump_opts(transport, w, false);
		}
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	free(req.trtype);
	free(req.tgt_name);
}
SPDK_RPC_REGISTER("nvmf_get_transports", rpc_nvmf_get_transports, SPDK_RPC_RUNTIME)

struct rpc_nvmf_get_stats_ctx {
	char *tgt_name;
	struct spdk_nvmf_tgt *tgt;
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;
};

static const struct spdk_json_object_decoder rpc_get_stats_decoders[] = {
	{"tgt_name", offsetof(struct rpc_nvmf_get_stats_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
free_get_stats_ctx(struct rpc_nvmf_get_stats_ctx *ctx)
{
	free(ctx->tgt_name);
	free(ctx);
}

static void
rpc_nvmf_get_stats_done(struct spdk_io_channel_iter *i, int status)
{
	struct rpc_nvmf_get_stats_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	spdk_json_write_array_end(ctx->w);
	spdk_json_write_object_end(ctx->w);
	spdk_jsonrpc_end_result(ctx->request, ctx->w);
	free_get_stats_ctx(ctx);
}

static void
_rpc_nvmf_get_stats(struct spdk_io_channel_iter *i)
{
	struct rpc_nvmf_get_stats_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch;
	struct spdk_nvmf_poll_group *group;

	ch = spdk_get_io_channel(ctx->tgt);
	group = spdk_io_channel_get_ctx(ch);

	spdk_nvmf_poll_group_dump_stat(group, ctx->w);

	spdk_put_io_channel(ch);
	spdk_for_each_channel_continue(i, 0);
}


static void
rpc_nvmf_get_stats(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_nvmf_get_stats_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error");
		return;
	}
	ctx->request = request;

	if (params) {
		if (spdk_json_decode_object(params, rpc_get_stats_decoders,
					    SPDK_COUNTOF(rpc_get_stats_decoders),
					    ctx)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			free_get_stats_ctx(ctx);
			return;
		}
	}

	ctx->tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!ctx->tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free_get_stats_ctx(ctx);
		return;
	}

	ctx->w = spdk_jsonrpc_begin_result(ctx->request);
	spdk_json_write_object_begin(ctx->w);
	spdk_json_write_named_uint64(ctx->w, "tick_rate", spdk_get_ticks_hz());
	spdk_json_write_named_array_begin(ctx->w, "poll_groups");

	spdk_for_each_channel(ctx->tgt,
			      _rpc_nvmf_get_stats,
			      ctx,
			      rpc_nvmf_get_stats_done);
}

SPDK_RPC_REGISTER("nvmf_get_stats", rpc_nvmf_get_stats, SPDK_RPC_RUNTIME)

static void
dump_nvmf_ctrlr(struct spdk_json_write_ctx *w, struct spdk_nvmf_ctrlr *ctrlr)
{
	uint32_t count;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_uint32(w, "cntlid", ctrlr->cntlid);
	spdk_json_write_named_string(w, "hostnqn", ctrlr->hostnqn);
	spdk_json_write_named_uuid(w, "hostid", &ctrlr->hostid);

	count = spdk_bit_array_count_set(ctrlr->qpair_mask);
	spdk_json_write_named_uint32(w, "num_io_qpairs", count);

	spdk_json_write_object_end(w);
}

static const char *
nvmf_qpair_state_str(enum spdk_nvmf_qpair_state state)
{
	switch (state) {
	case SPDK_NVMF_QPAIR_UNINITIALIZED:
		return "uninitialized";
	case SPDK_NVMF_QPAIR_CONNECTING:
		return "connecting";
	case SPDK_NVMF_QPAIR_ENABLED:
		return "enabled";
	case SPDK_NVMF_QPAIR_DEACTIVATING:
		return "deactivating";
	case SPDK_NVMF_QPAIR_ERROR:
		return "error";
	default:
		return NULL;
	}
}

static void
dump_nvmf_qpair(struct spdk_json_write_ctx *w, struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvme_transport_id trid = {};

	spdk_json_write_object_begin(w);

	spdk_json_write_named_uint32(w, "cntlid", qpair->ctrlr->cntlid);
	spdk_json_write_named_uint32(w, "qid", qpair->qid);
	spdk_json_write_named_string(w, "state", nvmf_qpair_state_str(qpair->state));
	spdk_json_write_named_string(w, "thread", spdk_thread_get_name(spdk_get_thread()));
	spdk_json_write_named_string(w, "hostnqn", qpair->ctrlr->hostnqn);

	if (spdk_nvmf_qpair_get_listen_trid(qpair, &trid) == 0) {
		spdk_json_write_named_object_begin(w, "listen_address");
		nvmf_transport_listen_dump_trid(&trid, w);
		spdk_json_write_object_end(w);
		if (qpair->transport->ops->listen_dump_opts) {
			qpair->transport->ops->listen_dump_opts(qpair->transport, &trid, w);
		}
	}

	memset(&trid, 0, sizeof(trid));
	if (spdk_nvmf_qpair_get_peer_trid(qpair, &trid) == 0) {
		spdk_json_write_named_object_begin(w, "peer_address");
		nvmf_transport_listen_dump_trid(&trid, w);
		spdk_json_write_object_end(w);
	}

	nvmf_qpair_auth_dump(qpair, w);
	spdk_json_write_object_end(w);
}

static const char *
nvme_ana_state_str(enum spdk_nvme_ana_state ana_state)
{
	switch (ana_state) {
	case SPDK_NVME_ANA_OPTIMIZED_STATE:
		return "optimized";
	case SPDK_NVME_ANA_NON_OPTIMIZED_STATE:
		return "non_optimized";
	case SPDK_NVME_ANA_INACCESSIBLE_STATE:
		return "inaccessible";
	case SPDK_NVME_ANA_PERSISTENT_LOSS_STATE:
		return "persistent_loss";
	case SPDK_NVME_ANA_CHANGE_STATE:
		return "change";
	default:
		return NULL;
	}
}

static void
dump_nvmf_subsystem_listener(struct spdk_json_write_ctx *w,
			     struct spdk_nvmf_subsystem_listener *listener)
{
	const struct spdk_nvme_transport_id *trid = listener->trid;
	uint32_t i;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_object_begin(w, "address");
	nvmf_transport_listen_dump_trid(trid, w);
	spdk_json_write_object_end(w);

	if (spdk_nvmf_subsystem_get_ana_reporting(listener->subsystem)) {
		spdk_json_write_named_array_begin(w, "ana_states");
		for (i = 0; i < listener->subsystem->max_nsid; i++) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_uint32(w, "ana_group", i + 1);
			spdk_json_write_named_string(w, "ana_state",
						     nvme_ana_state_str(listener->ana_state[i]));
			spdk_json_write_object_end(w);
		}
		spdk_json_write_array_end(w);
	}

	spdk_json_write_object_end(w);
}

struct rpc_subsystem_query_ctx {
	char *nqn;
	char *tgt_name;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;
};

static const struct spdk_json_object_decoder rpc_subsystem_query_decoders[] = {
	{"nqn", offsetof(struct rpc_subsystem_query_ctx, nqn), spdk_json_decode_string},
	{"tgt_name", offsetof(struct rpc_subsystem_query_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
free_rpc_subsystem_query_ctx(struct rpc_subsystem_query_ctx *ctx)
{
	free(ctx->nqn);
	free(ctx->tgt_name);
	free(ctx);
}

static void
rpc_nvmf_get_controllers_paused(struct spdk_nvmf_subsystem *subsystem,
				void *cb_arg, int status)
{
	struct rpc_subsystem_query_ctx *ctx = cb_arg;
	struct spdk_json_write_ctx *w;
	struct spdk_nvmf_ctrlr *ctrlr;

	w = spdk_jsonrpc_begin_result(ctx->request);

	spdk_json_write_array_begin(w);
	TAILQ_FOREACH(ctrlr, &ctx->subsystem->ctrlrs, link) {
		dump_nvmf_ctrlr(w, ctrlr);
	}
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(ctx->request, w);

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, NULL, NULL)) {
		SPDK_ERRLOG("Resuming subsystem with NQN %s failed\n", ctx->nqn);
		/* FIXME: RPC should fail if resuming the subsystem failed. */
	}

	free_rpc_subsystem_query_ctx(ctx);
}

static void
rpc_nvmf_get_qpairs_done(struct spdk_io_channel_iter *i, int status)
{
	struct rpc_subsystem_query_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	spdk_json_write_array_end(ctx->w);
	spdk_jsonrpc_end_result(ctx->request, ctx->w);

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, NULL, NULL)) {
		SPDK_ERRLOG("Resuming subsystem with NQN %s failed\n", ctx->nqn);
		/* FIXME: RPC should fail if resuming the subsystem failed. */
	}

	free_rpc_subsystem_query_ctx(ctx);
}

static void
rpc_nvmf_get_qpairs(struct spdk_io_channel_iter *i)
{
	struct rpc_subsystem_query_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch;
	struct spdk_nvmf_poll_group *group;
	struct spdk_nvmf_qpair *qpair;

	ch = spdk_io_channel_iter_get_channel(i);
	if (ch) {
		group = spdk_io_channel_get_ctx(ch);
		TAILQ_FOREACH(qpair, &group->qpairs, link) {
			if (qpair->ctrlr && qpair->ctrlr->subsys == ctx->subsystem) {
				dump_nvmf_qpair(ctx->w, qpair);
			}
		}
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
rpc_nvmf_get_qpairs_paused(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	struct rpc_subsystem_query_ctx *ctx = cb_arg;

	ctx->w = spdk_jsonrpc_begin_result(ctx->request);

	spdk_json_write_array_begin(ctx->w);

	spdk_for_each_channel(ctx->subsystem->tgt,
			      rpc_nvmf_get_qpairs,
			      ctx,
			      rpc_nvmf_get_qpairs_done);
}

static void
rpc_nvmf_get_listeners_paused(struct spdk_nvmf_subsystem *subsystem,
			      void *cb_arg, int status)
{
	struct rpc_subsystem_query_ctx *ctx = cb_arg;
	struct spdk_json_write_ctx *w;
	struct spdk_nvmf_subsystem_listener *listener;

	w = spdk_jsonrpc_begin_result(ctx->request);

	spdk_json_write_array_begin(w);

	for (listener = spdk_nvmf_subsystem_get_first_listener(ctx->subsystem);
	     listener != NULL;
	     listener = spdk_nvmf_subsystem_get_next_listener(ctx->subsystem, listener)) {
		dump_nvmf_subsystem_listener(w, listener);
	}
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(ctx->request, w);

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, NULL, NULL)) {
		SPDK_ERRLOG("Resuming subsystem with NQN %s failed\n", ctx->nqn);
		/* FIXME: RPC should fail if resuming the subsystem failed. */
	}

	free_rpc_subsystem_query_ctx(ctx);
}

static void
_rpc_nvmf_subsystem_query(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params,
			  spdk_nvmf_subsystem_state_change_done cb_fn)
{
	struct rpc_subsystem_query_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Out of memory");
		return;
	}

	ctx->request = request;

	if (spdk_json_decode_object(params, rpc_subsystem_query_decoders,
				    SPDK_COUNTOF(rpc_subsystem_query_decoders),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free_rpc_subsystem_query_ctx(ctx);
		return;
	}

	tgt = spdk_nvmf_get_tgt(ctx->tgt_name);
	if (!tgt) {
		SPDK_ERRLOG("Unable to find a target object.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target");
		free_rpc_subsystem_query_ctx(ctx);
		return;
	}

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		free_rpc_subsystem_query_ctx(ctx);
		return;
	}

	ctx->subsystem = subsystem;

	if (spdk_nvmf_subsystem_pause(subsystem, 0, cb_fn, ctx)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Internal error");
		free_rpc_subsystem_query_ctx(ctx);
		return;
	}
}

static void
rpc_nvmf_subsystem_get_controllers(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	_rpc_nvmf_subsystem_query(request, params, rpc_nvmf_get_controllers_paused);
}
SPDK_RPC_REGISTER("nvmf_subsystem_get_controllers", rpc_nvmf_subsystem_get_controllers,
		  SPDK_RPC_RUNTIME);

static void
rpc_nvmf_subsystem_get_qpairs(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	_rpc_nvmf_subsystem_query(request, params, rpc_nvmf_get_qpairs_paused);
}
SPDK_RPC_REGISTER("nvmf_subsystem_get_qpairs", rpc_nvmf_subsystem_get_qpairs, SPDK_RPC_RUNTIME);

static void
rpc_nvmf_subsystem_get_listeners(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	_rpc_nvmf_subsystem_query(request, params, rpc_nvmf_get_listeners_paused);
}
SPDK_RPC_REGISTER("nvmf_subsystem_get_listeners", rpc_nvmf_subsystem_get_listeners,
		  SPDK_RPC_RUNTIME);

struct rpc_mdns_prr {
	char *tgt_name;
};

static const struct spdk_json_object_decoder rpc_mdns_prr_decoders[] = {
	{"tgt_name", offsetof(struct rpc_mdns_prr, tgt_name), spdk_json_decode_string, true},
};

static void
rpc_nvmf_publish_mdns_prr(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	int rc;
	struct rpc_mdns_prr req = { 0 };
	struct spdk_nvmf_tgt *tgt;

	if (params) {
		if (spdk_json_decode_object(params, rpc_mdns_prr_decoders,
					    SPDK_COUNTOF(rpc_mdns_prr_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			return;
		}
	}

	tgt = spdk_nvmf_get_tgt(req.tgt_name);
	if (!tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free(req.tgt_name);
		return;
	}

	rc = nvmf_publish_mdns_prr(tgt);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		free(req.tgt_name);
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	free(req.tgt_name);
}
SPDK_RPC_REGISTER("nvmf_publish_mdns_prr", rpc_nvmf_publish_mdns_prr, SPDK_RPC_RUNTIME);

static void
rpc_nvmf_stop_mdns_prr(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_mdns_prr req = { 0 };
	struct spdk_nvmf_tgt *tgt;

	if (params) {
		if (spdk_json_decode_object(params, rpc_mdns_prr_decoders,
					    SPDK_COUNTOF(rpc_mdns_prr_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			return;
		}
	}

	tgt = spdk_nvmf_get_tgt(req.tgt_name);
	if (!tgt) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to find a target.");
		free(req.tgt_name);
		return;
	}

	nvmf_tgt_stop_mdns_prr(tgt);

	spdk_jsonrpc_send_bool_response(request, true);
	free(req.tgt_name);
}
SPDK_RPC_REGISTER("nvmf_stop_mdns_prr", rpc_nvmf_stop_mdns_prr, SPDK_RPC_RUNTIME);
