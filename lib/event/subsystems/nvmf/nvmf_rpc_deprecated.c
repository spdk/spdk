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

struct rpc_listen_address {
	char *transport;
	char *adrfam;
	char *traddr;
	char *trsvcid;
};

#define RPC_MAX_LISTEN_ADDRESSES 255
#define RPC_MAX_HOSTS 255
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

static int
decode_rpc_listen_addresses(const struct spdk_json_val *val, void *out)
{
	struct rpc_listen_addresses *listen_addresses = out;
	return spdk_json_decode_array(val, decode_rpc_listen_address, &listen_addresses->addresses,
				      RPC_MAX_LISTEN_ADDRESSES,
				      &listen_addresses->num_listen_address, sizeof(struct rpc_listen_address));
}

struct rpc_hosts {
	size_t num_hosts;
	char *hosts[RPC_MAX_HOSTS];
};

static int
decode_rpc_hosts(const struct spdk_json_val *val, void *out)
{
	struct rpc_hosts *rpc_hosts = out;

	return spdk_json_decode_array(val, spdk_json_decode_string, rpc_hosts->hosts, RPC_MAX_HOSTS,
				      &rpc_hosts->num_hosts, sizeof(char *));
}


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

static void
free_rpc_ns_params(struct spdk_nvmf_ns_params *ns_params)
{
	free(ns_params->bdev_name);
}

static void
free_rpc_namespaces(struct rpc_namespaces *r)
{
	size_t i;

	for (i = 0; i < r->num_ns; i++) {
		free_rpc_ns_params(&r->ns_params[i]);
	}
}

static int
decode_rpc_ns_params(const struct spdk_json_val *val, void *out)
{
	struct spdk_nvmf_ns_params *ns_params = out;

	return spdk_json_decode_object(val, rpc_ns_params_decoders,
				       SPDK_COUNTOF(rpc_ns_params_decoders),
				       ns_params);
}

static int
decode_rpc_namespaces(const struct spdk_json_val *val, void *out)
{
	struct rpc_namespaces *namespaces = out;
	char *names[RPC_MAX_NAMESPACES] = {0}; /* old format - array of strings (bdev names) */
	size_t i;
	int rc;

	/* First try to decode namespaces as an array of objects (new format) */
	if (spdk_json_decode_array(val, decode_rpc_ns_params, namespaces->ns_params,
				   SPDK_COUNTOF(namespaces->ns_params),
				   &namespaces->num_ns, sizeof(*namespaces->ns_params)) == 0) {
		return 0;
	}

	/* If that fails, try to decode namespaces as an array of strings (old format) */
	free_rpc_namespaces(namespaces);
	memset(namespaces, 0, sizeof(*namespaces));
	rc = spdk_json_decode_array(val, spdk_json_decode_string, names,
				    SPDK_COUNTOF(names),
				    &namespaces->num_ns, sizeof(char *));
	if (rc == 0) {
		/* Decoded old format - copy to ns_params (new format) */
		for (i = 0; i < namespaces->num_ns; i++) {
			namespaces->ns_params[i].bdev_name = names[i];
		}
		return 0;
	}

	/* Failed to decode - don't leave dangling string pointers around */
	for (i = 0; i < namespaces->num_ns; i++) {
		free(names[i]);
	}

	return rc;
}

static void
free_rpc_listen_addresses(struct rpc_listen_addresses *r)
{
	size_t i;

	for (i = 0; i < r->num_listen_address; i++) {
		free_rpc_listen_address(&r->addresses[i]);
	}
}

static void
free_rpc_hosts(struct rpc_hosts *r)
{
	size_t i;

	for (i = 0; i < r->num_hosts; i++) {
		free(r->hosts[i]);
	}
}

struct rpc_subsystem {
	int32_t core;
	char *mode;
	char *nqn;
	struct rpc_listen_addresses listen_addresses;
	struct rpc_hosts hosts;
	bool allow_any_host;
	char *pci_address;
	char *serial_number;
	struct rpc_namespaces namespaces;
	uint32_t num_ns;
};

static void
free_rpc_subsystem(struct rpc_subsystem *req)
{
	if (req) {
		free(req->mode);
		free(req->nqn);
		free(req->serial_number);
		free_rpc_namespaces(&req->namespaces);
		free_rpc_listen_addresses(&req->listen_addresses);
		free_rpc_hosts(&req->hosts);
	}
	free(req);
}

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

static const struct spdk_json_object_decoder rpc_subsystem_decoders[] = {
	{"core", offsetof(struct rpc_subsystem, core), spdk_json_decode_int32, true},
	{"mode", offsetof(struct rpc_subsystem, mode), spdk_json_decode_string, true},
	{"nqn", offsetof(struct rpc_subsystem, nqn), spdk_json_decode_string},
	{"listen_addresses", offsetof(struct rpc_subsystem, listen_addresses), decode_rpc_listen_addresses, true},
	{"hosts", offsetof(struct rpc_subsystem, hosts), decode_rpc_hosts, true},
	{"allow_any_host", offsetof(struct rpc_subsystem, allow_any_host), spdk_json_decode_bool, true},
	{"serial_number", offsetof(struct rpc_subsystem, serial_number), spdk_json_decode_string, true},
	{"namespaces", offsetof(struct rpc_subsystem, namespaces), decode_rpc_namespaces, true},
	{"max_namespaces", offsetof(struct rpc_subsystem, num_ns), spdk_json_decode_uint32, true},
};

struct subsystem_listen_ctx {
	struct rpc_subsystem *req;
	struct spdk_nvmf_subsystem *subsystem;
	struct spdk_jsonrpc_request *request;

	uint32_t idx;
};

static void
spdk_rpc_construct_subsystem_listen_done(void *cb_arg, int status)
{
	struct subsystem_listen_ctx *ctx = cb_arg;
	struct rpc_listen_address *addr;
	struct spdk_nvme_transport_id trid = {0};

	if (status) {
		goto invalid;
	}

	addr = &ctx->req->listen_addresses.addresses[ctx->idx];
	if (rpc_listen_address_to_trid(addr, &trid)) {
		goto invalid;
	}

	spdk_nvmf_subsystem_add_listener(ctx->subsystem, &trid);

	ctx->idx++;

	if (ctx->idx < ctx->req->listen_addresses.num_listen_address) {
		addr = &ctx->req->listen_addresses.addresses[ctx->idx];

		if (rpc_listen_address_to_trid(addr, &trid)) {
			goto invalid;
		}

		spdk_nvmf_tgt_listen(g_spdk_nvmf_tgt, &trid, spdk_rpc_construct_subsystem_listen_done, ctx);
		return;
	}

	spdk_nvmf_subsystem_start(ctx->subsystem,
				  spdk_rpc_nvmf_subsystem_started,
				  ctx->request);

	free_rpc_subsystem(ctx->req);
	free(ctx);

	return;

invalid:
	spdk_nvmf_subsystem_destroy(ctx->subsystem);
	spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 "Invalid parameters");
	free_rpc_subsystem(ctx->req);
	free(ctx);
}

static void
spdk_rpc_construct_nvmf_subsystem(struct spdk_jsonrpc_request *request,
				  const struct spdk_json_val *params)
{
	struct rpc_subsystem *req;
	struct spdk_nvmf_subsystem *subsystem;
	size_t i;

	SPDK_WARNLOG("The construct_nvmf_subsystem RPC is deprecated. Use nvmf_subsystem_create instead.\n");

	req = calloc(1, sizeof(*req));
	if (!req) {
		goto invalid;
	}

	req->core = -1;	/* Explicitly set the core as the uninitialized value */

	if (spdk_json_decode_object(params, rpc_subsystem_decoders,
				    SPDK_COUNTOF(rpc_subsystem_decoders),
				    req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	/* Mode is no longer a valid parameter, but print out a nice
	 * message if it exists to inform users.
	 */
	if (req->mode) {
		SPDK_NOTICELOG("Mode present in the construct NVMe-oF subsystem RPC.\n"
			       "Mode was removed as a valid parameter.\n");
		if (strcasecmp(req->mode, "Virtual") == 0) {
			SPDK_NOTICELOG("Your mode value is 'Virtual' which is now the only possible mode.\n"
				       "Your RPC will work as expected.\n");
		} else {
			SPDK_NOTICELOG("Please remove 'mode' from the RPC.\n");
			goto invalid;
		}
	}

	/* Core is no longer a valid parameter, but print out a nice
	 * message if it exists to inform users.
	 */
	if (req->core != -1) {
		SPDK_NOTICELOG("Core present in the construct NVMe-oF subsystem RPC.\n"
			       "Core was removed as an option. Subsystems can now run on all available cores.\n");
		SPDK_NOTICELOG("Ignoring it and continuing.\n");
	}

	subsystem = spdk_nvmf_subsystem_create(g_spdk_nvmf_tgt, req->nqn, SPDK_NVMF_SUBTYPE_NVME,
					       req->num_ns);
	if (!subsystem) {
		goto invalid;
	}

	if (spdk_nvmf_subsystem_set_sn(subsystem, req->serial_number)) {
		SPDK_ERRLOG("Subsystem %s: invalid serial number '%s'\n", req->nqn, req->serial_number);
		goto invalid;
	}

	for (i = 0; i < req->hosts.num_hosts; i++) {
		spdk_nvmf_subsystem_add_host(subsystem, req->hosts.hosts[i]);
	}

	spdk_nvmf_subsystem_set_allow_any_host(subsystem, req->allow_any_host);

	for (i = 0; i < req->namespaces.num_ns; i++) {
		struct spdk_nvmf_ns_params *ns_params = &req->namespaces.ns_params[i];
		struct spdk_bdev *bdev;
		struct spdk_nvmf_ns_opts ns_opts;

		bdev = spdk_bdev_get_by_name(ns_params->bdev_name);
		if (bdev == NULL) {
			SPDK_ERRLOG("Could not find namespace bdev '%s'\n", ns_params->bdev_name);
			spdk_nvmf_subsystem_destroy(subsystem);
			goto invalid;
		}

		spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
		ns_opts.nsid = ns_params->nsid;

		SPDK_STATIC_ASSERT(sizeof(ns_opts.nguid) == sizeof(ns_params->nguid), "size mismatch");
		memcpy(ns_opts.nguid, ns_params->nguid, sizeof(ns_opts.nguid));

		SPDK_STATIC_ASSERT(sizeof(ns_opts.eui64) == sizeof(ns_params->eui64), "size mismatch");
		memcpy(ns_opts.eui64, ns_params->eui64, sizeof(ns_opts.eui64));

		if (!spdk_mem_all_zero(&ns_params->uuid, sizeof(ns_params->uuid))) {
			ns_opts.uuid = ns_params->uuid;
		}

		if (spdk_nvmf_subsystem_add_ns(subsystem, bdev, &ns_opts, sizeof(ns_opts)) == 0) {
			SPDK_ERRLOG("Unable to add namespace\n");
			spdk_nvmf_subsystem_destroy(subsystem);
			goto invalid;
		}
	}

	if (req->listen_addresses.num_listen_address > 0) {
		struct rpc_listen_address *addr;
		struct spdk_nvme_transport_id trid = {0};
		struct subsystem_listen_ctx *ctx;

		ctx = calloc(1, sizeof(*ctx));
		if (!ctx) {
			spdk_nvmf_subsystem_destroy(subsystem);
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "No Memory");
			free_rpc_subsystem(req);
			return;
		}

		ctx->req = req;
		ctx->subsystem = subsystem;
		ctx->request = request;
		ctx->idx = 0;

		addr = &req->listen_addresses.addresses[0];

		if (rpc_listen_address_to_trid(addr, &trid)) {
			free(ctx);
			goto invalid;
		}

		spdk_nvmf_tgt_listen(g_spdk_nvmf_tgt, &trid, spdk_rpc_construct_subsystem_listen_done, ctx);
		return;
	}

	free_rpc_subsystem(req);

	spdk_nvmf_subsystem_start(subsystem,
				  spdk_rpc_nvmf_subsystem_started,
				  request);

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_subsystem(req);
}
SPDK_RPC_REGISTER("construct_nvmf_subsystem", spdk_rpc_construct_nvmf_subsystem, SPDK_RPC_RUNTIME)
