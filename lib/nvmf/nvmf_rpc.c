/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2018-2020 Mellanox Technologies LTD. All rights reserved.
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

#include "spdk/bdev.h"
#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/nvmf.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/bit_array.h"

#include "spdk_internal/assert.h"

#include "nvmf_internal.h"

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

static int
decode_ns_uuid(const struct spdk_json_val *val, void *out)
{
	char *str = NULL;
	int rc;

	rc = spdk_json_decode_string(val, &str);
	if (rc == 0) {
		rc = spdk_uuid_parse(out, str);
	}

	free(str);
	return rc;
}

struct rpc_get_subsystem {
	char *tgt_name;
};

static const struct spdk_json_object_decoder rpc_get_subsystem_decoders[] = {
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
		const char *adrfam;

		trid = spdk_nvmf_subsystem_listener_get_trid(listener);

		spdk_json_write_object_begin(w);
		adrfam = spdk_nvme_transport_id_adrfam_str(trid->adrfam);
		if (adrfam == NULL) {
			adrfam = "unknown";
		}
		/* NOTE: "transport" is kept for compatibility; new code should use "trtype" */
		spdk_json_write_named_string(w, "transport", trid->trstring);
		spdk_json_write_named_string(w, "trtype", trid->trstring);
		spdk_json_write_named_string(w, "adrfam", adrfam);
		spdk_json_write_named_string(w, "traddr", trid->traddr);
		spdk_json_write_named_string(w, "trsvcid", trid->trsvcid);
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

			if (!spdk_mem_all_zero(&ns_opts.uuid, sizeof(ns_opts.uuid))) {
				char uuid_str[SPDK_UUID_STRING_LEN];

				spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &ns_opts.uuid);
				spdk_json_write_named_string(w, "uuid", uuid_str);
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
	struct spdk_nvmf_subsystem *subsystem;
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
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	subsystem = spdk_nvmf_subsystem_get_first(tgt);
	while (subsystem) {
		dump_nvmf_subsystem(w, subsystem);
		subsystem = spdk_nvmf_subsystem_get_next(subsystem);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	free(req.tgt_name);
}
SPDK_RPC_REGISTER("nvmf_get_subsystems", rpc_nvmf_get_subsystems, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(nvmf_get_subsystems, get_nvmf_subsystems)

struct rpc_subsystem_create {
	char *nqn;
	char *serial_number;
	char *model_number;
	char *tgt_name;
	uint32_t max_namespaces;
	bool allow_any_host;
	bool ana_reporting;
};

static const struct spdk_json_object_decoder rpc_subsystem_create_decoders[] = {
	{"nqn", offsetof(struct rpc_subsystem_create, nqn), spdk_json_decode_string},
	{"serial_number", offsetof(struct rpc_subsystem_create, serial_number), spdk_json_decode_string, true},
	{"model_number", offsetof(struct rpc_subsystem_create, model_number), spdk_json_decode_string, true},
	{"tgt_name", offsetof(struct rpc_subsystem_create, tgt_name), spdk_json_decode_string, true},
	{"max_namespaces", offsetof(struct rpc_subsystem_create, max_namespaces), spdk_json_decode_uint32, true},
	{"allow_any_host", offsetof(struct rpc_subsystem_create, allow_any_host), spdk_json_decode_bool, true},
	{"ana_reporting", offsetof(struct rpc_subsystem_create, ana_reporting), spdk_json_decode_bool, true},
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
		spdk_nvmf_subsystem_destroy(subsystem);
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

	rc = spdk_nvmf_subsystem_start(subsystem,
				       rpc_nvmf_subsystem_started,
				       request);

cleanup:
	free(req->nqn);
	free(req->tgt_name);
	free(req->serial_number);
	free(req->model_number);
	free(req);

	if (rc && subsystem) {
		spdk_nvmf_subsystem_destroy(subsystem);
	}
}
SPDK_RPC_REGISTER("nvmf_create_subsystem", rpc_nvmf_create_subsystem, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(nvmf_create_subsystem, nvmf_subsystem_create)

struct rpc_subsystem_set_options {
	char *nqn;
	char *tgt_name;
	char *trtype;
};

static const struct spdk_json_object_decoder rpc_subsystem_set_options_decoders[] = {
	{"nqn", offsetof(struct rpc_subsystem_set_options, nqn), spdk_json_decode_string},
	{"tgt_name", offsetof(struct rpc_subsystem_set_options, tgt_name), spdk_json_decode_string, true},
	{"trtype", offsetof(struct rpc_subsystem_set_options, trtype), spdk_json_decode_string},
};

static void
rpc_nvmf_subsystem_set_options(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_subsystem_set_options *req;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Memory allocation failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation failed");
		return;
	}

	if (spdk_json_decode_object_relaxed(params, rpc_subsystem_set_options_decoders,
					    SPDK_COUNTOF(rpc_subsystem_set_options_decoders), req)) {
		SPDK_ERRLOG("spdk_json_decode_object_relaxed failed\n");
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

	subsystem = spdk_nvmf_tgt_find_subsystem(tgt, req->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", req->nqn);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Unable to find subsystem with NQN %s", req->nqn);
		goto cleanup;
	}

	transport = spdk_nvmf_tgt_get_transport(tgt, req->trtype);
	if (!transport) {
		SPDK_ERRLOG("Unable to find transport with trtype %s\n", req->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Unable to find transport with trtype %s", req->trtype);
		goto cleanup;
	}

	if (!transport->ops->subsystem_opts_parse) {
		SPDK_ERRLOG("Unable to find transport ops\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Unable to find transport ops");
		goto cleanup;
	}

	rc = transport->ops->subsystem_opts_parse(transport, subsystem, params);
	if (rc) {
		SPDK_ERRLOG("Unable to parse opts %d\n", rc);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Unable to parse opts %d", rc);
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free(req->nqn);
	free(req->tgt_name);
	free(req->trtype);
	free(req);
}
SPDK_RPC_REGISTER("nvmf_subsystem_set_options", rpc_nvmf_subsystem_set_options, SPDK_RPC_RUNTIME)

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
rpc_nvmf_subsystem_stopped(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	nvmf_subsystem_remove_all_listeners(subsystem, true);
	spdk_nvmf_subsystem_destroy(subsystem);

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
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(nvmf_delete_subsystem, delete_nvmf_subsystem)

struct rpc_listen_address {
	char *transport;
	char *adrfam;
	char *traddr;
	char *trsvcid;
};

static const struct spdk_json_object_decoder rpc_listen_address_decoders[] = {
	/* NOTE: "transport" is kept for compatibility; new code should use "trtype" */
	{"transport", offsetof(struct rpc_listen_address, transport), spdk_json_decode_string, true},
	{"trtype", offsetof(struct rpc_listen_address, transport), spdk_json_decode_string, true},
	{"adrfam", offsetof(struct rpc_listen_address, adrfam), spdk_json_decode_string, true},
	{"traddr", offsetof(struct rpc_listen_address, traddr), spdk_json_decode_string},
	{"trsvcid", offsetof(struct rpc_listen_address, trsvcid), spdk_json_decode_string},
};

static int
decode_rpc_listen_address(const struct spdk_json_val *val, void *out)
{
	struct rpc_listen_address *req = (struct rpc_listen_address *)out;
	if (spdk_json_decode_object(val, rpc_listen_address_decoders,
				    SPDK_COUNTOF(rpc_listen_address_decoders),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		return -1;
	}
	return 0;
}

static void
free_rpc_listen_address(struct rpc_listen_address *r)
{
	free(r->transport);
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

	struct spdk_jsonrpc_request	*request;
	struct spdk_nvme_transport_id	trid;
	enum nvmf_rpc_listen_op		op;
	bool				response_sent;
};

static const struct spdk_json_object_decoder nvmf_rpc_listener_decoder[] = {
	{"nqn", offsetof(struct nvmf_rpc_listener_ctx, nqn), spdk_json_decode_string},
	{"listen_address", offsetof(struct nvmf_rpc_listener_ctx, address), decode_rpc_listen_address},
	{"tgt_name", offsetof(struct nvmf_rpc_listener_ctx, tgt_name), spdk_json_decode_string, true},
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

	if (ctx->op == NVMF_RPC_LISTEN_ADD) {
		if (!nvmf_subsystem_find_listener(subsystem, &ctx->trid)) {
			rc = spdk_nvmf_tgt_listen(ctx->tgt, &ctx->trid);
			if (rc == 0) {
				spdk_nvmf_subsystem_add_listener(ctx->subsystem, &ctx->trid, nvmf_rpc_subsystem_listen, ctx);
				return;
			}

			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			ctx->response_sent = true;
		}
	} else if (ctx->op == NVMF_RPC_LISTEN_REMOVE) {
		if (spdk_nvmf_subsystem_remove_listener(subsystem, &ctx->trid)) {
			SPDK_ERRLOG("Unable to remove listener.\n");
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			ctx->response_sent = true;
		}
		spdk_nvmf_transport_stop_listen_async(ctx->transport, &ctx->trid, nvmf_rpc_stop_listen_async_done,
						      ctx);
		return;
	} else if (ctx->op == NVMF_RPC_LISTEN_SET_ANA_STATE) {
		nvmf_subsystem_set_ana_state(subsystem, &ctx->trid, ctx->ana_state,
					     nvmf_rpc_set_ana_state_done, ctx);
		return;
	} else {
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

	if (spdk_nvme_transport_id_populate_trstring(trid, address->transport)) {
		SPDK_ERRLOG("Invalid transport string: %s\n", address->transport);
		return -EINVAL;
	}

	if (spdk_nvme_transport_id_parse_trtype(&trid->trtype, address->transport)) {
		SPDK_ERRLOG("Invalid transport type: %s\n", address->transport);
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

	len = strlen(address->trsvcid);
	if (len > sizeof(trid->trsvcid) - 1) {
		SPDK_ERRLOG("Transport service id longer than %zu characters: %s\n",
			    sizeof(trid->trsvcid) - 1, address->trsvcid);
		return -EINVAL;
	}
	memcpy(trid->trsvcid, address->trsvcid, len + 1);

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

	ctx->op = NVMF_RPC_LISTEN_ADD;

	rc = spdk_nvmf_subsystem_pause(subsystem, nvmf_rpc_listen_paused, ctx);
	if (rc != 0) {
		if (rc == -EBUSY) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "subsystem busy, retry later.\n");
		} else {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		}
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
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	ctx->op = NVMF_RPC_LISTEN_REMOVE;

	rc = spdk_nvmf_subsystem_pause(subsystem, nvmf_rpc_listen_paused, ctx);
	if (rc != 0) {
		if (rc == -EBUSY) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "subsystem busy, retry later.\n");
		} else {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		}
		nvmf_rpc_listener_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_remove_listener", rpc_nvmf_subsystem_remove_listener,
		  SPDK_RPC_RUNTIME);

static const struct spdk_json_object_decoder nvmf_rpc_set_ana_state_decoder[] = {
	{"nqn", offsetof(struct nvmf_rpc_listener_ctx, nqn), spdk_json_decode_string},
	{"listen_address", offsetof(struct nvmf_rpc_listener_ctx, address), decode_rpc_listen_address},
	{"ana_state", offsetof(struct nvmf_rpc_listener_ctx, ana_state_str), spdk_json_decode_string},
	{"tgt_name", offsetof(struct nvmf_rpc_listener_ctx, tgt_name), spdk_json_decode_string, true},
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
						 "Unable to find a target.\n");
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

	if (spdk_nvmf_subsystem_pause(subsystem, nvmf_rpc_listen_paused, ctx)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Internal error");
		nvmf_rpc_listener_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_listener_set_ana_state",
		  rpc_nvmf_subsystem_listener_set_ana_state, SPDK_RPC_RUNTIME);

struct spdk_nvmf_ns_params {
	char *bdev_name;
	char *ptpl_file;
	uint32_t nsid;
	char nguid[16];
	char eui64[8];
	struct spdk_uuid uuid;
};

static const struct spdk_json_object_decoder rpc_ns_params_decoders[] = {
	{"nsid", offsetof(struct spdk_nvmf_ns_params, nsid), spdk_json_decode_uint32, true},
	{"bdev_name", offsetof(struct spdk_nvmf_ns_params, bdev_name), spdk_json_decode_string},
	{"ptpl_file", offsetof(struct spdk_nvmf_ns_params, ptpl_file), spdk_json_decode_string, true},
	{"nguid", offsetof(struct spdk_nvmf_ns_params, nguid), decode_ns_nguid, true},
	{"eui64", offsetof(struct spdk_nvmf_ns_params, eui64), decode_ns_eui64, true},
	{"uuid", offsetof(struct spdk_nvmf_ns_params, uuid), decode_ns_uuid, true},
};

static int
decode_rpc_ns_params(const struct spdk_json_val *val, void *out)
{
	struct spdk_nvmf_ns_params *ns_params = out;

	return spdk_json_decode_object(val, rpc_ns_params_decoders,
				       SPDK_COUNTOF(rpc_ns_params_decoders),
				       ns_params);
}

struct nvmf_rpc_ns_ctx {
	char *nqn;
	char *tgt_name;
	struct spdk_nvmf_ns_params ns_params;

	struct spdk_jsonrpc_request *request;
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

	SPDK_STATIC_ASSERT(sizeof(ns_opts.nguid) == sizeof(ctx->ns_params.nguid), "size mismatch");
	memcpy(ns_opts.nguid, ctx->ns_params.nguid, sizeof(ns_opts.nguid));

	SPDK_STATIC_ASSERT(sizeof(ns_opts.eui64) == sizeof(ctx->ns_params.eui64), "size mismatch");
	memcpy(ns_opts.eui64, ctx->ns_params.eui64, sizeof(ns_opts.eui64));

	if (!spdk_mem_all_zero(&ctx->ns_params.uuid, sizeof(ctx->ns_params.uuid))) {
		ns_opts.uuid = ctx->ns_params.uuid;
	}

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

	if (spdk_json_decode_object(params, nvmf_rpc_subsystem_ns_decoder,
				    SPDK_COUNTOF(nvmf_rpc_subsystem_ns_decoder),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_ns_ctx_free(ctx);
		return;
	}

	ctx->request = request;
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

	rc = spdk_nvmf_subsystem_pause(subsystem, nvmf_rpc_ns_paused, ctx);
	if (rc != 0) {
		if (rc == -EBUSY) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "subsystem busy, retry later.\n");
		} else {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		}
		nvmf_rpc_ns_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_add_ns", rpc_nvmf_subsystem_add_ns, SPDK_RPC_RUNTIME)

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

	rc = spdk_nvmf_subsystem_pause(subsystem, nvmf_rpc_remove_ns_paused, ctx);
	if (rc != 0) {
		if (rc == -EBUSY) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "subsystem busy, retry later.\n");
		} else {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		}
		nvmf_rpc_remove_ns_ctx_free(ctx);
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_remove_ns", rpc_nvmf_subsystem_remove_ns, SPDK_RPC_RUNTIME)

struct nvmf_rpc_host_ctx {
	struct spdk_jsonrpc_request *request;
	char *nqn;
	char *host;
	char *tgt_name;
	bool allow_any_host;
};

static const struct spdk_json_object_decoder nvmf_rpc_subsystem_host_decoder[] = {
	{"nqn", offsetof(struct nvmf_rpc_host_ctx, nqn), spdk_json_decode_string},
	{"host", offsetof(struct nvmf_rpc_host_ctx, host), spdk_json_decode_string},
	{"tgt_name", offsetof(struct nvmf_rpc_host_ctx, tgt_name), spdk_json_decode_string, true},
};

static void
nvmf_rpc_host_ctx_free(struct nvmf_rpc_host_ctx *ctx)
{
	free(ctx->nqn);
	free(ctx->host);
	free(ctx->tgt_name);
}

static void
rpc_nvmf_subsystem_add_host(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct nvmf_rpc_host_ctx ctx = {};
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_nvmf_tgt *tgt;
	int rc;

	if (spdk_json_decode_object(params, nvmf_rpc_subsystem_host_decoder,
				    SPDK_COUNTOF(nvmf_rpc_subsystem_host_decoder),
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

	rc = spdk_nvmf_subsystem_add_host(subsystem, ctx.host);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_host_ctx_free(&ctx);
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
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
};

static const struct spdk_json_object_decoder nvmf_rpc_create_target_decoder[] = {
	{"name", offsetof(struct nvmf_rpc_target_ctx, name), spdk_json_decode_string},
	{"max_subsystems", offsetof(struct nvmf_rpc_target_ctx, max_subsystems), spdk_json_decode_uint32, true},
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
		free(ctx.name);
		return;
	}

	snprintf(opts.name, NVMF_TGT_NAME_MAX_LENGTH, "%s", ctx.name);
	opts.max_subsystems = ctx.max_subsystems;

	if (spdk_nvmf_get_tgt(opts.name) != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Target already exists.");
		free(ctx.name);
		return;
	}

	tgt = spdk_nvmf_tgt_create(&opts);

	if (tgt == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to create the requested target.");
		free(ctx.name);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_nvmf_tgt_get_name(tgt));
	spdk_jsonrpc_end_result(request, w);
	free(ctx.name);
}
SPDK_RPC_REGISTER("nvmf_create_target", rpc_nvmf_create_target, SPDK_RPC_RUNTIME);

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
SPDK_RPC_REGISTER("nvmf_delete_target", rpc_nvmf_delete_target, SPDK_RPC_RUNTIME);

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
SPDK_RPC_REGISTER("nvmf_get_targets", rpc_nvmf_get_targets, SPDK_RPC_RUNTIME);

struct nvmf_rpc_create_transport_ctx {
	char				*trtype;
	char				*tgt_name;
	struct spdk_nvmf_transport_opts	opts;
	struct spdk_jsonrpc_request	*request;
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

/**
 * This function parses deprecated `max_qpairs_per_ctrlr` and warns the user to use
 * the new parameter `max_io_qpairs_per_ctrlr`
 */
static int
nvmf_rpc_decode_max_qpairs(const struct spdk_json_val *val, void *out)
{
	uint16_t *i = out;
	int rc;

	rc = spdk_json_number_to_uint16(val, i);
	if (rc == 0) {
		SPDK_WARNLOG("Parameter max_qpairs_per_ctrlr is deprecated, use max_io_qpairs_per_ctrlr instead.\n");
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
		"max_qpairs_per_ctrlr", offsetof(struct nvmf_rpc_create_transport_ctx, opts.max_qpairs_per_ctrlr),
		nvmf_rpc_decode_max_qpairs, true
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
		"tgt_name", offsetof(struct nvmf_rpc_create_transport_ctx, tgt_name),
		spdk_json_decode_string, true
	},
};

static void
nvmf_rpc_create_transport_ctx_free(struct nvmf_rpc_create_transport_ctx *ctx)
{
	free(ctx->trtype);
	free(ctx->tgt_name);
	free(ctx);
}

static void
nvmf_rpc_tgt_add_transport_done(void *cb_arg, int status)
{
	struct nvmf_rpc_create_transport_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request;

	request = ctx->request;
	nvmf_rpc_create_transport_ctx_free(ctx);

	if (status) {
		SPDK_ERRLOG("Failed to add transport to tgt.(%d)\n", status);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Failed to add transport to tgt.(%d)",
						     status);
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_nvmf_create_transport(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct nvmf_rpc_create_transport_ctx *ctx;
	enum spdk_nvme_transport_type trtype;
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_tgt *tgt;

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

	if (spdk_nvme_transport_id_parse_trtype(&trtype, ctx->trtype)) {
		SPDK_ERRLOG("Invalid transport type '%s'\n", ctx->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid transport type '%s'", ctx->trtype);
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	/* Initialize all the transport options (based on transport type) and decode the
	 * parameters again to update any options passed in rpc create transport call.
	 */
	if (!spdk_nvmf_transport_opts_init(ctx->trtype, &ctx->opts)) {
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

	transport = spdk_nvmf_transport_create(ctx->trtype, &ctx->opts);

	if (!transport) {
		SPDK_ERRLOG("Transport type '%s' create failed\n", ctx->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Transport type '%s' create failed", ctx->trtype);
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	/* add transport to target */
	ctx->request = request;
	spdk_nvmf_tgt_add_transport(tgt, transport, nvmf_rpc_tgt_add_transport_done, ctx);
}
SPDK_RPC_REGISTER("nvmf_create_transport", rpc_nvmf_create_transport, SPDK_RPC_RUNTIME)

static void
dump_nvmf_transport(struct spdk_json_write_ctx *w, struct spdk_nvmf_transport *transport)
{
	const struct spdk_nvmf_transport_opts *opts = spdk_nvmf_get_transport_opts(transport);

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "trtype", spdk_nvmf_get_transport_name(transport));
	spdk_json_write_named_uint32(w, "max_queue_depth", opts->max_queue_depth);
	spdk_json_write_named_uint32(w, "max_io_qpairs_per_ctrlr", opts->max_qpairs_per_ctrlr - 1);
	spdk_json_write_named_uint32(w, "in_capsule_data_size", opts->in_capsule_data_size);
	spdk_json_write_named_uint32(w, "max_io_size", opts->max_io_size);
	spdk_json_write_named_uint32(w, "io_unit_size", opts->io_unit_size);
	spdk_json_write_named_uint32(w, "max_aq_depth", opts->max_aq_depth);
	spdk_json_write_named_uint32(w, "num_shared_buffers", opts->num_shared_buffers);
	spdk_json_write_named_uint32(w, "buf_cache_size", opts->buf_cache_size);
	spdk_json_write_named_bool(w, "dif_insert_or_strip", opts->dif_insert_or_strip);

	if (transport->ops->dump_opts) {
		transport->ops->dump_opts(transport, w);
	}

	spdk_json_write_named_uint32(w, "abort_timeout_sec", opts->abort_timeout_sec);

	spdk_json_write_object_end(w);
}

struct rpc_get_transport {
	char *tgt_name;
};

static const struct spdk_json_object_decoder rpc_get_transport_decoders[] = {
	{"tgt_name", offsetof(struct rpc_get_transport, tgt_name), spdk_json_decode_string, true},
};

static void
rpc_nvmf_get_transports(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_get_transport req = { 0 };
	struct spdk_json_write_ctx *w;
	struct spdk_nvmf_transport *transport;
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
		free(req.tgt_name);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	transport = spdk_nvmf_transport_get_first(tgt);
	while (transport) {
		dump_nvmf_transport(w, transport);
		transport = spdk_nvmf_transport_get_next(transport);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	free(req.tgt_name);
}
SPDK_RPC_REGISTER("nvmf_get_transports", rpc_nvmf_get_transports, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(nvmf_get_transports, get_nvmf_transports)

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
write_nvmf_transport_stats(struct spdk_json_write_ctx *w,
			   struct spdk_nvmf_transport_poll_group_stat *stat)
{
	uint64_t i;

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "trtype",
				     spdk_nvme_transport_id_trtype_str(stat->trtype));
	switch (stat->trtype) {
	case SPDK_NVME_TRANSPORT_RDMA:
		spdk_json_write_named_uint64(w, "pending_data_buffer", stat->rdma.pending_data_buffer);
		spdk_json_write_named_array_begin(w, "devices");
		for (i = 0; i < stat->rdma.num_devices; ++i) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "name", stat->rdma.devices[i].name);
			spdk_json_write_named_uint64(w, "polls", stat->rdma.devices[i].polls);
			spdk_json_write_named_uint64(w, "completions", stat->rdma.devices[i].completions);
			spdk_json_write_named_uint64(w, "requests",
						     stat->rdma.devices[i].requests);
			spdk_json_write_named_uint64(w, "request_latency",
						     stat->rdma.devices[i].request_latency);
			spdk_json_write_named_uint64(w, "pending_free_request",
						     stat->rdma.devices[i].pending_free_request);
			spdk_json_write_named_uint64(w, "pending_rdma_read",
						     stat->rdma.devices[i].pending_rdma_read);
			spdk_json_write_named_uint64(w, "pending_rdma_write",
						     stat->rdma.devices[i].pending_rdma_write);
			spdk_json_write_object_end(w);
		}
		spdk_json_write_array_end(w);
		break;
	default:
		break;
	}
	spdk_json_write_object_end(w);
}

static void
_rpc_nvmf_get_stats(struct spdk_io_channel_iter *i)
{
	struct rpc_nvmf_get_stats_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_poll_group_stat stat;
	struct spdk_nvmf_transport_poll_group_stat *trstat;
	int rc;

	if (0 == spdk_nvmf_poll_group_get_stat(ctx->tgt, &stat)) {
		spdk_json_write_object_begin(ctx->w);
		spdk_json_write_named_string(ctx->w, "name", spdk_thread_get_name(spdk_get_thread()));
		spdk_json_write_named_uint32(ctx->w, "admin_qpairs", stat.admin_qpairs);
		spdk_json_write_named_uint32(ctx->w, "io_qpairs", stat.io_qpairs);
		spdk_json_write_named_uint64(ctx->w, "pending_bdev_io", stat.pending_bdev_io);

		spdk_json_write_named_array_begin(ctx->w, "transports");
		transport = spdk_nvmf_transport_get_first(ctx->tgt);
		while (transport) {
			rc = spdk_nvmf_transport_poll_group_get_stat(ctx->tgt, transport, &trstat);
			if (0 == rc) {
				write_nvmf_transport_stats(ctx->w, trstat);
				spdk_nvmf_transport_poll_group_free_stat(transport, trstat);
			} else if (-ENOTSUP != rc) {
				SPDK_ERRLOG("Failed to get poll group statistics for transport %s, errno %d\n",
					    spdk_nvme_transport_id_trtype_str(spdk_nvmf_get_transport_type(transport)),
					    rc);
			}
			transport = spdk_nvmf_transport_get_next(transport);
		}
		spdk_json_write_array_end(ctx->w);
		spdk_json_write_object_end(ctx->w);
	}

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
	char uuid_str[SPDK_UUID_STRING_LEN] = {};
	uint32_t count;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_uint32(w, "cntlid", ctrlr->cntlid);

	spdk_json_write_named_string(w, "hostnqn", ctrlr->hostnqn);

	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &ctrlr->hostid);
	spdk_json_write_named_string(w, "hostid", uuid_str);

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
	case SPDK_NVMF_QPAIR_ACTIVE:
		return "active";
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
	const struct spdk_nvme_transport_id *trid = qpair->trid;
	const char *adrfam;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_uint32(w, "cntlid", qpair->ctrlr->cntlid);
	spdk_json_write_named_uint32(w, "qid", qpair->qid);
	spdk_json_write_named_string(w, "state", nvmf_qpair_state_str(qpair->state));

	spdk_json_write_named_object_begin(w, "listen_address");
	adrfam = spdk_nvme_transport_id_adrfam_str(trid->adrfam);
	if (adrfam == NULL) {
		adrfam = "unknown";
	}
	spdk_json_write_named_string(w, "trtype", trid->trstring);
	spdk_json_write_named_string(w, "adrfam", adrfam);
	spdk_json_write_named_string(w, "traddr", trid->traddr);
	spdk_json_write_named_string(w, "trsvcid", trid->trsvcid);
	spdk_json_write_object_end(w);

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
	const char *adrfam;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_object_begin(w, "address");
	adrfam = spdk_nvme_transport_id_adrfam_str(trid->adrfam);
	if (adrfam == NULL) {
		adrfam = "unknown";
	}
	spdk_json_write_named_string(w, "trtype", trid->trstring);
	spdk_json_write_named_string(w, "adrfam", adrfam);
	spdk_json_write_named_string(w, "traddr", trid->traddr);
	spdk_json_write_named_string(w, "trsvcid", trid->trsvcid);
	spdk_json_write_object_end(w);

	spdk_json_write_named_string(w, "ana_state",
				     nvme_ana_state_str(listener->ana_state));

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

	ch = spdk_get_io_channel(ctx->subsystem->tgt);
	group = spdk_io_channel_get_ctx(ch);

	TAILQ_FOREACH(qpair, &group->qpairs, link) {
		if (qpair->ctrlr->subsys == ctx->subsystem) {
			dump_nvmf_qpair(ctx->w, qpair);
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

	if (spdk_nvmf_subsystem_pause(subsystem, cb_fn, ctx)) {
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
