/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2018 Mellanox Technologies LTD. All rights reserved.
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

#include "spdk/bdev.h"
#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/nvmf.h"
#include "spdk/string.h"
#include "spdk/util.h"

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

static void
dump_nvmf_subsystem(struct spdk_json_write_ctx *w, struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_host		*host;
	struct spdk_nvmf_listener	*listener;

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
		const char *trtype;
		const char *adrfam;

		trid = spdk_nvmf_listener_get_trid(listener);

		spdk_json_write_object_begin(w);
		trtype = spdk_nvme_transport_id_trtype_str(trid->trtype);
		if (trtype == NULL) {
			trtype = "unknown";
		}
		adrfam = spdk_nvme_transport_id_adrfam_str(trid->adrfam);
		if (adrfam == NULL) {
			adrfam = "unknown";
		}
		/* NOTE: "transport" is kept for compatibility; new code should use "trtype" */
		spdk_json_write_named_string(w, "transport", trtype);
		spdk_json_write_named_string(w, "trtype", trtype);
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
spdk_rpc_get_nvmf_subsystems(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct spdk_nvmf_subsystem *subsystem;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_nvmf_subsystems requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);
	subsystem = spdk_nvmf_subsystem_get_first(g_spdk_nvmf_tgt);
	while (subsystem) {
		dump_nvmf_subsystem(w, subsystem);
		subsystem = spdk_nvmf_subsystem_get_next(subsystem);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_nvmf_subsystems", spdk_rpc_get_nvmf_subsystems, SPDK_RPC_RUNTIME)

struct rpc_subsystem_create {
	char *nqn;
	char *serial_number;
	char *model_number;
	uint32_t max_namespaces;
	bool allow_any_host;
};

static const struct spdk_json_object_decoder rpc_subsystem_create_decoders[] = {
	{"nqn", offsetof(struct rpc_subsystem_create, nqn), spdk_json_decode_string},
	{"serial_number", offsetof(struct rpc_subsystem_create, serial_number), spdk_json_decode_string, true},
	{"model_number", offsetof(struct rpc_subsystem_create, model_number), spdk_json_decode_string, true},
	{"max_namespaces", offsetof(struct rpc_subsystem_create, max_namespaces), spdk_json_decode_uint32, true},
	{"allow_any_host", offsetof(struct rpc_subsystem_create, allow_any_host), spdk_json_decode_bool, true},
};

static void
spdk_rpc_nvmf_subsystem_started(struct spdk_nvmf_subsystem *subsystem,
				void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w;

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}

static void
spdk_rpc_nvmf_subsystem_create(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_subsystem_create *req;
	struct spdk_nvmf_subsystem *subsystem;

	req = calloc(1, sizeof(*req));
	if (!req) {
		goto invalid;
	}

	if (spdk_json_decode_object(params, rpc_subsystem_create_decoders,
				    SPDK_COUNTOF(rpc_subsystem_create_decoders),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	subsystem = spdk_nvmf_subsystem_create(g_spdk_nvmf_tgt, req->nqn, SPDK_NVMF_SUBTYPE_NVME,
					       req->max_namespaces);
	if (!subsystem) {
		goto invalid;
	}

	if (req->serial_number) {
		if (spdk_nvmf_subsystem_set_sn(subsystem, req->serial_number)) {
			SPDK_ERRLOG("Subsystem %s: invalid serial number '%s'\n", req->nqn, req->serial_number);
			goto invalid;
		}
	}

	if (req->model_number) {
		if (spdk_nvmf_subsystem_set_mn(subsystem, req->model_number)) {
			SPDK_ERRLOG("Subsystem %s: invalid model number '%s'\n", req->nqn, req->model_number);
			goto invalid;
		}
	}

	spdk_nvmf_subsystem_set_allow_any_host(subsystem, req->allow_any_host);

	free(req->nqn);
	free(req->serial_number);
	free(req->model_number);
	free(req);

	spdk_nvmf_subsystem_start(subsystem,
				  spdk_rpc_nvmf_subsystem_started,
				  request);

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	if (req) {
		free(req->nqn);
		free(req->serial_number);
		free(req->model_number);
	}
	free(req);
}
SPDK_RPC_REGISTER("nvmf_subsystem_create", spdk_rpc_nvmf_subsystem_create, SPDK_RPC_RUNTIME)

struct rpc_delete_subsystem {
	char *nqn;
};

static void
free_rpc_delete_subsystem(struct rpc_delete_subsystem *r)
{
	free(r->nqn);
}

static void
spdk_rpc_nvmf_subsystem_stopped(struct spdk_nvmf_subsystem *subsystem,
				void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w;

	spdk_nvmf_subsystem_destroy(subsystem);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}

static const struct spdk_json_object_decoder rpc_delete_subsystem_decoders[] = {
	{"nqn", offsetof(struct rpc_delete_subsystem, nqn), spdk_json_decode_string},
};

static void
spdk_rpc_delete_nvmf_subsystem(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_delete_subsystem req = {};
	struct spdk_nvmf_subsystem *subsystem;

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

	subsystem = spdk_nvmf_tgt_find_subsystem(g_spdk_nvmf_tgt, req.nqn);
	if (!subsystem) {
		goto invalid;
	}

	free_rpc_delete_subsystem(&req);

	spdk_nvmf_subsystem_stop(subsystem,
				 spdk_rpc_nvmf_subsystem_stopped,
				 request);

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_delete_subsystem(&req);
}
SPDK_RPC_REGISTER("delete_nvmf_subsystem", spdk_rpc_delete_nvmf_subsystem, SPDK_RPC_RUNTIME)

struct rpc_listen_address {
	char *transport;
	char *adrfam;
	char *traddr;
	char *trsvcid;
};

#define RPC_MAX_LISTEN_ADDRESSES 255
#define RPC_MAX_NAMESPACES 255

struct rpc_listen_addresses {
	size_t num_listen_address;
	struct rpc_listen_address addresses[RPC_MAX_LISTEN_ADDRESSES];
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
};

struct nvmf_rpc_listener_ctx {
	char				*nqn;
	struct spdk_nvmf_subsystem	*subsystem;
	struct rpc_listen_address	address;

	struct spdk_jsonrpc_request	*request;
	struct spdk_nvme_transport_id	trid;
	enum nvmf_rpc_listen_op		op;
	bool				response_sent;
};

static const struct spdk_json_object_decoder nvmf_rpc_listener_decoder[] = {
	{"nqn", offsetof(struct nvmf_rpc_listener_ctx, nqn), spdk_json_decode_string},
	{"listen_address", offsetof(struct nvmf_rpc_listener_ctx, address), decode_rpc_listen_address},
};

static void
nvmf_rpc_listener_ctx_free(struct nvmf_rpc_listener_ctx *ctx)
{
	free(ctx->nqn);
	free_rpc_listen_address(&ctx->address);
	free(ctx);
}

static void
nvmf_rpc_listen_resumed(struct spdk_nvmf_subsystem *subsystem,
			void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;

	request = ctx->request;
	if (ctx->response_sent) {
		/* If an error occurred, the response has already been sent. */
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	nvmf_rpc_listener_ctx_free(ctx);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}

static void
nvmf_rpc_tgt_listen(void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;

	if (status) {
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ctx->response_sent = true;
	} else {
		if (spdk_nvmf_subsystem_add_listener(ctx->subsystem, &ctx->trid)) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			ctx->response_sent = true;
		}
	}

	if (spdk_nvmf_subsystem_resume(ctx->subsystem, nvmf_rpc_listen_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
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

	if (ctx->op == NVMF_RPC_LISTEN_ADD) {
		spdk_nvmf_tgt_listen(g_spdk_nvmf_tgt, &ctx->trid, nvmf_rpc_tgt_listen, ctx);
		return;
	} else if (ctx->op == NVMF_RPC_LISTEN_REMOVE) {
		if (spdk_nvmf_subsystem_remove_listener(subsystem, &ctx->trid)) {
			SPDK_ERRLOG("Unable to remove listener.\n");
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			ctx->response_sent = true;
		}
	} else {
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ctx->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_listen_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
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
nvmf_rpc_subsystem_add_listener(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct nvmf_rpc_listener_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;

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

	subsystem = spdk_nvmf_tgt_find_subsystem(g_spdk_nvmf_tgt, ctx->nqn);
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

	if (spdk_nvmf_subsystem_pause(subsystem, nvmf_rpc_listen_paused, ctx)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_add_listener", nvmf_rpc_subsystem_add_listener, SPDK_RPC_RUNTIME);

static void
nvmf_rpc_subsystem_remove_listener(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct nvmf_rpc_listener_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;

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

	subsystem = spdk_nvmf_tgt_find_subsystem(g_spdk_nvmf_tgt, ctx->nqn);
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

	ctx->op = NVMF_RPC_LISTEN_REMOVE;

	if (spdk_nvmf_subsystem_pause(subsystem, nvmf_rpc_listen_paused, ctx)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

}
SPDK_RPC_REGISTER("nvmf_subsystem_remove_listener", nvmf_rpc_subsystem_remove_listener,
		  SPDK_RPC_RUNTIME);

struct spdk_nvmf_ns_params {
	char *bdev_name;
	uint32_t nsid;
	char nguid[16];
	char eui64[8];
	struct spdk_uuid uuid;
};

struct rpc_namespaces {
	size_t num_ns;
	struct spdk_nvmf_ns_params ns_params[RPC_MAX_NAMESPACES];
};


static const struct spdk_json_object_decoder rpc_ns_params_decoders[] = {
	{"nsid", offsetof(struct spdk_nvmf_ns_params, nsid), spdk_json_decode_uint32, true},
	{"bdev_name", offsetof(struct spdk_nvmf_ns_params, bdev_name), spdk_json_decode_string},
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
	struct spdk_nvmf_ns_params ns_params;

	struct spdk_jsonrpc_request *request;
	bool response_sent;
};

static const struct spdk_json_object_decoder nvmf_rpc_subsystem_ns_decoder[] = {
	{"nqn", offsetof(struct nvmf_rpc_ns_ctx, nqn), spdk_json_decode_string},
	{"namespace", offsetof(struct nvmf_rpc_ns_ctx, ns_params), decode_rpc_ns_params},
};

static void
nvmf_rpc_ns_ctx_free(struct nvmf_rpc_ns_ctx *ctx)
{
	free(ctx->nqn);
	free(ctx->ns_params.bdev_name);
	free(ctx);
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

	nvmf_rpc_ns_ctx_free(ctx);

	if (response_sent) {
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_uint32(w, nsid);
	spdk_jsonrpc_end_result(request, w);
}

static void
nvmf_rpc_ns_paused(struct spdk_nvmf_subsystem *subsystem,
		   void *cb_arg, int status)
{
	struct nvmf_rpc_ns_ctx *ctx = cb_arg;
	struct spdk_nvmf_ns_opts ns_opts;
	struct spdk_bdev *bdev;

	bdev = spdk_bdev_get_by_name(ctx->ns_params.bdev_name);
	if (!bdev) {
		SPDK_ERRLOG("No bdev with name %s\n", ctx->ns_params.bdev_name);
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ctx->response_sent = true;
		goto resume;
	}

	spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
	ns_opts.nsid = ctx->ns_params.nsid;

	SPDK_STATIC_ASSERT(sizeof(ns_opts.nguid) == sizeof(ctx->ns_params.nguid), "size mismatch");
	memcpy(ns_opts.nguid, ctx->ns_params.nguid, sizeof(ns_opts.nguid));

	SPDK_STATIC_ASSERT(sizeof(ns_opts.eui64) == sizeof(ctx->ns_params.eui64), "size mismatch");
	memcpy(ns_opts.eui64, ctx->ns_params.eui64, sizeof(ns_opts.eui64));

	if (!spdk_mem_all_zero(&ctx->ns_params.uuid, sizeof(ctx->ns_params.uuid))) {
		ns_opts.uuid = ctx->ns_params.uuid;
	}

	ctx->ns_params.nsid = spdk_nvmf_subsystem_add_ns(subsystem, bdev, &ns_opts, sizeof(ns_opts));
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
		return;
	}
}

static void
nvmf_rpc_subsystem_add_ns(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct nvmf_rpc_ns_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;

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

	subsystem = spdk_nvmf_tgt_find_subsystem(g_spdk_nvmf_tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_ns_ctx_free(ctx);
		return;
	}

	if (spdk_nvmf_subsystem_pause(subsystem, nvmf_rpc_ns_paused, ctx)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_ns_ctx_free(ctx);
		return;
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_add_ns", nvmf_rpc_subsystem_add_ns, SPDK_RPC_RUNTIME)

struct nvmf_rpc_remove_ns_ctx {
	char *nqn;
	uint32_t nsid;

	struct spdk_jsonrpc_request *request;
	bool response_sent;
};

static const struct spdk_json_object_decoder nvmf_rpc_subsystem_remove_ns_decoder[] = {
	{"nqn", offsetof(struct nvmf_rpc_remove_ns_ctx, nqn), spdk_json_decode_string},
	{"nsid", offsetof(struct nvmf_rpc_remove_ns_ctx, nsid), spdk_json_decode_uint32},
};

static void
nvmf_rpc_remove_ns_ctx_free(struct nvmf_rpc_remove_ns_ctx *ctx)
{
	free(ctx->nqn);
	free(ctx);
}

static void
nvmf_rpc_remove_ns_resumed(struct spdk_nvmf_subsystem *subsystem,
			   void *cb_arg, int status)
{
	struct nvmf_rpc_remove_ns_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request = ctx->request;
	bool response_sent = ctx->response_sent;
	struct spdk_json_write_ctx *w;

	nvmf_rpc_remove_ns_ctx_free(ctx);

	if (response_sent) {
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}

static void
nvmf_rpc_remove_ns_remove_done(struct spdk_nvmf_subsystem *subsystem, void *cb_arg, int status)
{
	struct nvmf_rpc_remove_ns_ctx *ctx;

	ctx = cb_arg;

	if (status != 0) {
		SPDK_ERRLOG("Unable to remove namespace ID %u\n", ctx->nsid);
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ctx->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_remove_ns_resumed, ctx)) {
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_remove_ns_ctx_free(ctx);
		return;
	}
}

static void
nvmf_rpc_remove_ns_paused(struct spdk_nvmf_subsystem *subsystem,
			  void *cb_arg, int status)
{
	struct nvmf_rpc_remove_ns_ctx *ctx = cb_arg;
	int ret;

	ret = spdk_nvmf_subsystem_remove_ns(subsystem, ctx->nsid, nvmf_rpc_remove_ns_remove_done, ctx);
	if (ret < 0) {
		SPDK_ERRLOG("Unable to remove namespace ID %u\n", ctx->nsid);
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		ctx->response_sent = true;
		spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_remove_ns_resumed, ctx);
	}
}

static void
nvmf_rpc_subsystem_remove_ns(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct nvmf_rpc_remove_ns_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;

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

	ctx->request = request;
	ctx->response_sent = false;

	subsystem = spdk_nvmf_tgt_find_subsystem(g_spdk_nvmf_tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_remove_ns_ctx_free(ctx);
		return;
	}

	if (spdk_nvmf_subsystem_pause(subsystem, nvmf_rpc_remove_ns_paused, ctx)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_remove_ns_ctx_free(ctx);
		return;
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_remove_ns", nvmf_rpc_subsystem_remove_ns, SPDK_RPC_RUNTIME)

enum nvmf_rpc_host_op {
	NVMF_RPC_HOST_ADD,
	NVMF_RPC_HOST_REMOVE,
	NVMF_RPC_HOST_ALLOW_ANY,
};

struct nvmf_rpc_host_ctx {
	struct spdk_jsonrpc_request *request;

	char *nqn;
	char *host;

	enum nvmf_rpc_host_op op;

	bool allow_any_host;

	bool response_sent;
};

static const struct spdk_json_object_decoder nvmf_rpc_subsystem_host_decoder[] = {
	{"nqn", offsetof(struct nvmf_rpc_host_ctx, nqn), spdk_json_decode_string},
	{"host", offsetof(struct nvmf_rpc_host_ctx, host), spdk_json_decode_string},
};

static void
nvmf_rpc_host_ctx_free(struct nvmf_rpc_host_ctx *ctx)
{
	free(ctx->nqn);
	free(ctx->host);
	free(ctx);
}

static void
nvmf_rpc_host_resumed(struct spdk_nvmf_subsystem *subsystem,
		      void *cb_arg, int status)
{
	struct nvmf_rpc_host_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;
	bool response_sent = ctx->response_sent;

	request = ctx->request;
	nvmf_rpc_host_ctx_free(ctx);

	if (response_sent) {
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}

static void
nvmf_rpc_host_paused(struct spdk_nvmf_subsystem *subsystem,
		     void *cb_arg, int status)
{
	struct nvmf_rpc_host_ctx *ctx = cb_arg;
	int rc = -1;

	switch (ctx->op) {
	case NVMF_RPC_HOST_ADD:
		rc = spdk_nvmf_subsystem_add_host(subsystem, ctx->host);
		break;
	case NVMF_RPC_HOST_REMOVE:
		rc = spdk_nvmf_subsystem_remove_host(subsystem, ctx->host);
		break;
	case NVMF_RPC_HOST_ALLOW_ANY:
		rc = spdk_nvmf_subsystem_set_allow_any_host(subsystem, ctx->allow_any_host);
		break;
	}

	if (rc != 0) {
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		ctx->response_sent = true;
	}

	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_host_resumed, ctx)) {
		if (!ctx->response_sent) {
			spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		}
		nvmf_rpc_host_ctx_free(ctx);
		return;
	}
}

static void
nvmf_rpc_subsystem_add_host(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct nvmf_rpc_host_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object(params, nvmf_rpc_subsystem_host_decoder,
				    SPDK_COUNTOF(nvmf_rpc_subsystem_host_decoder),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_host_ctx_free(ctx);
		return;
	}

	ctx->request = request;
	ctx->op = NVMF_RPC_HOST_ADD;
	ctx->response_sent = false;

	subsystem = spdk_nvmf_tgt_find_subsystem(g_spdk_nvmf_tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_host_ctx_free(ctx);
		return;
	}

	if (spdk_nvmf_subsystem_pause(subsystem, nvmf_rpc_host_paused, ctx)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_host_ctx_free(ctx);
		return;
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_add_host", nvmf_rpc_subsystem_add_host, SPDK_RPC_RUNTIME)

static void
nvmf_rpc_subsystem_remove_host(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct nvmf_rpc_host_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object(params, nvmf_rpc_subsystem_host_decoder,
				    SPDK_COUNTOF(nvmf_rpc_subsystem_host_decoder),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_host_ctx_free(ctx);
		return;
	}

	ctx->request = request;
	ctx->op = NVMF_RPC_HOST_REMOVE;
	ctx->response_sent = false;

	subsystem = spdk_nvmf_tgt_find_subsystem(g_spdk_nvmf_tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_host_ctx_free(ctx);
		return;
	}

	if (spdk_nvmf_subsystem_pause(subsystem, nvmf_rpc_host_paused, ctx)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_host_ctx_free(ctx);
		return;
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_remove_host", nvmf_rpc_subsystem_remove_host, SPDK_RPC_RUNTIME)


static const struct spdk_json_object_decoder nvmf_rpc_subsystem_any_host_decoder[] = {
	{"nqn", offsetof(struct nvmf_rpc_host_ctx, nqn), spdk_json_decode_string},
	{"allow_any_host", offsetof(struct nvmf_rpc_host_ctx, allow_any_host), spdk_json_decode_bool},
};

static void
nvmf_rpc_subsystem_allow_any_host(struct spdk_jsonrpc_request *request,
				  const struct spdk_json_val *params)
{
	struct nvmf_rpc_host_ctx *ctx;
	struct spdk_nvmf_subsystem *subsystem;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	if (spdk_json_decode_object(params, nvmf_rpc_subsystem_any_host_decoder,
				    SPDK_COUNTOF(nvmf_rpc_subsystem_any_host_decoder),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_host_ctx_free(ctx);
		return;
	}

	ctx->request = request;
	ctx->op = NVMF_RPC_HOST_ALLOW_ANY;
	ctx->response_sent = false;

	subsystem = spdk_nvmf_tgt_find_subsystem(g_spdk_nvmf_tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_host_ctx_free(ctx);
		return;
	}

	if (spdk_nvmf_subsystem_pause(subsystem, nvmf_rpc_host_paused, ctx)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_host_ctx_free(ctx);
		return;
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_allow_any_host", nvmf_rpc_subsystem_allow_any_host,
		  SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder nvmf_rpc_subsystem_tgt_opts_decoder[] = {
	{"max_subsystems", 0, spdk_json_decode_uint32, true}
};

static void
nvmf_rpc_subsystem_set_tgt_max_subsystems(struct spdk_jsonrpc_request *request,
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
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("set_nvmf_target_max_subsystems", nvmf_rpc_subsystem_set_tgt_max_subsystems,
		  SPDK_RPC_STARTUP)

static int decode_conn_sched(const struct spdk_json_val *val, void *out)
{
	enum spdk_nvmf_connect_sched *sched = out;

	if (spdk_json_strequal(val, "roundrobin") == true) {
		*sched = CONNECT_SCHED_ROUND_ROBIN;
	} else if (spdk_json_strequal(val, "hostip") == true) {
		*sched = CONNECT_SCHED_HOST_IP;
	} else {
		SPDK_ERRLOG("Invalid connection scheduling parameter\n");
		return -EINVAL;
	}

	return 0;
}

static const struct spdk_json_object_decoder nvmf_rpc_subsystem_tgt_conf_decoder[] = {
	{"acceptor_poll_rate", offsetof(struct spdk_nvmf_tgt_conf, acceptor_poll_rate), spdk_json_decode_uint32, true},
	{"conn_sched", offsetof(struct spdk_nvmf_tgt_conf, conn_sched), decode_conn_sched, true},
};

static void
nvmf_rpc_subsystem_set_tgt_conf(struct spdk_jsonrpc_request *request,
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
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("set_nvmf_target_config", nvmf_rpc_subsystem_set_tgt_conf, SPDK_RPC_STARTUP)

struct nvmf_rpc_create_transport_ctx {
	char				*trtype;
	struct spdk_nvmf_transport_opts opts;
	struct spdk_jsonrpc_request	*request;
};

static const struct spdk_json_object_decoder nvmf_rpc_create_transport_decoder[] = {
	{	"trtype", offsetof(struct nvmf_rpc_create_transport_ctx, trtype), spdk_json_decode_string},
	{
		"max_queue_depth", offsetof(struct nvmf_rpc_create_transport_ctx, opts.max_queue_depth),
		spdk_json_decode_uint16, true
	},
	{
		"max_qpairs_per_ctrlr", offsetof(struct nvmf_rpc_create_transport_ctx, opts.max_qpairs_per_ctrlr),
		spdk_json_decode_uint16, true
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
		"max_srq_depth", offsetof(struct nvmf_rpc_create_transport_ctx, opts.max_srq_depth),
		spdk_json_decode_uint32, true
	},
};

static void
nvmf_rpc_create_transport_ctx_free(struct nvmf_rpc_create_transport_ctx *ctx)
{
	free(ctx->trtype);
	free(ctx);
}

static void
nvmf_rpc_tgt_add_transport_done(void *cb_arg, int status)
{
	struct nvmf_rpc_create_transport_ctx *ctx = cb_arg;
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;

	request = ctx->request;
	nvmf_rpc_create_transport_ctx_free(ctx);

	if (status) {
		SPDK_ERRLOG("Failed to add transport to tgt.(%d)\n", status);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Failed to add transport to tgt.(%d)\n",
						     status);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}

static void
nvmf_rpc_create_transport(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct nvmf_rpc_create_transport_ctx *ctx;
	enum spdk_nvme_transport_type trtype;
	struct spdk_nvmf_transport *transport;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		return;
	}

	/* Decode parameters the first time to get the transport type */
	if (spdk_json_decode_object(params, nvmf_rpc_create_transport_decoder,
				    SPDK_COUNTOF(nvmf_rpc_create_transport_decoder),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	if (spdk_nvme_transport_id_parse_trtype(&trtype, ctx->trtype)) {
		SPDK_ERRLOG("Invalid transport type '%s'\n", ctx->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid transport type '%s'\n", ctx->trtype);
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	/* Initialize all the transport options (based on transport type) and decode the
	 * parameters again to update any options passed in rpc create transport call.
	 */
	if (!spdk_nvmf_transport_opts_init(trtype, &ctx->opts)) {
		/* This can happen if user specifies PCIE transport type which isn't valid for
		 * NVMe-oF.
		 */
		SPDK_ERRLOG("Invalid transport type '%s'\n", ctx->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid transport type '%s'\n", ctx->trtype);
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	if (spdk_json_decode_object(params, nvmf_rpc_create_transport_decoder,
				    SPDK_COUNTOF(nvmf_rpc_create_transport_decoder),
				    ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	if (spdk_nvmf_tgt_get_transport(g_spdk_nvmf_tgt, trtype)) {
		SPDK_ERRLOG("Transport type '%s' already exists\n", ctx->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Transport type '%s' already exists\n", ctx->trtype);
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	transport = spdk_nvmf_transport_create(trtype, &ctx->opts);

	if (!transport) {
		SPDK_ERRLOG("Transport type '%s' create failed\n", ctx->trtype);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Transport type '%s' create failed\n", ctx->trtype);
		nvmf_rpc_create_transport_ctx_free(ctx);
		return;
	}

	/* add transport to target */
	ctx->request = request;
	spdk_nvmf_tgt_add_transport(g_spdk_nvmf_tgt, transport, nvmf_rpc_tgt_add_transport_done, ctx);
}

SPDK_RPC_REGISTER("nvmf_create_transport", nvmf_rpc_create_transport, SPDK_RPC_RUNTIME)

static void
dump_nvmf_transport(struct spdk_json_write_ctx *w, struct spdk_nvmf_transport *transport)
{
	const struct spdk_nvmf_transport_opts *opts = spdk_nvmf_get_transport_opts(transport);
	spdk_nvme_transport_type_t type = spdk_nvmf_get_transport_type(transport);

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "trtype", spdk_nvme_transport_id_trtype_str(type));
	spdk_json_write_named_uint32(w, "max_queue_depth", opts->max_queue_depth);
	spdk_json_write_named_uint32(w, "max_qpairs_per_ctrlr", opts->max_qpairs_per_ctrlr);
	spdk_json_write_named_uint32(w, "in_capsule_data_size", opts->in_capsule_data_size);
	spdk_json_write_named_uint32(w, "max_io_size", opts->max_io_size);
	spdk_json_write_named_uint32(w, "io_unit_size", opts->io_unit_size);
	spdk_json_write_named_uint32(w, "max_aq_depth", opts->max_aq_depth);
	spdk_json_write_named_uint32(w, "num_shared_buffers", opts->num_shared_buffers);
	spdk_json_write_named_uint32(w, "buf_cache_size", opts->buf_cache_size);

	spdk_json_write_object_end(w);
}

static void
nvmf_rpc_get_nvmf_transports(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct spdk_nvmf_transport *transport;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_nvmf_transports requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);
	transport = spdk_nvmf_transport_get_first(g_spdk_nvmf_tgt);
	while (transport) {
		dump_nvmf_transport(w, transport);
		transport = spdk_nvmf_transport_get_next(transport);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_nvmf_transports", nvmf_rpc_get_nvmf_transports, SPDK_RPC_RUNTIME)
