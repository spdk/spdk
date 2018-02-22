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

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/nvmf.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "nvmf_tgt.h"

#include <uuid/uuid.h>

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
	uuid_t uuid;
	int rc;

	rc = spdk_json_decode_string(val, &str);
	if (rc == 0) {
		rc = uuid_parse(str, uuid);
		if (rc == 0) {
			SPDK_STATIC_ASSERT(sizeof(uuid) == 16, "size mismatch");
			memcpy(out, uuid, sizeof(uuid));
		}
	}

	free(str);
	return rc;
}

static void
dump_nvmf_subsystem(struct spdk_json_write_ctx *w, struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_host		*host;
	struct spdk_nvmf_listener 	*listener;

	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "nqn");
	spdk_json_write_string(w, spdk_nvmf_subsystem_get_nqn(subsystem));
	spdk_json_write_name(w, "subtype");
	if (spdk_nvmf_subsystem_get_type(subsystem) == SPDK_NVMF_SUBTYPE_NVME) {
		spdk_json_write_string(w, "NVMe");
	} else {
		spdk_json_write_string(w, "Discovery");
	}

	spdk_json_write_name(w, "listen_addresses");
	spdk_json_write_array_begin(w);

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
		spdk_json_write_name(w, "transport");
		spdk_json_write_string(w, trtype);
		spdk_json_write_name(w, "trtype");
		spdk_json_write_string(w, trtype);
		spdk_json_write_name(w, "adrfam");
		spdk_json_write_string(w, adrfam);
		spdk_json_write_name(w, "traddr");
		spdk_json_write_string(w, trid->traddr);
		spdk_json_write_name(w, "trsvcid");
		spdk_json_write_string(w, trid->trsvcid);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	spdk_json_write_name(w, "allow_any_host");
	spdk_json_write_bool(w, spdk_nvmf_subsystem_get_allow_any_host(subsystem));

	spdk_json_write_name(w, "hosts");
	spdk_json_write_array_begin(w);

	for (host = spdk_nvmf_subsystem_get_first_host(subsystem); host != NULL;
	     host = spdk_nvmf_subsystem_get_next_host(subsystem, host)) {
		spdk_json_write_object_begin(w);
		spdk_json_write_name(w, "nqn");
		spdk_json_write_string(w, spdk_nvmf_host_get_nqn(host));
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	if (spdk_nvmf_subsystem_get_type(subsystem) == SPDK_NVMF_SUBTYPE_NVME) {
		struct spdk_nvmf_ns *ns;
		struct spdk_nvmf_ns_opts ns_opts;

		spdk_json_write_name(w, "serial_number");
		spdk_json_write_string(w, spdk_nvmf_subsystem_get_sn(subsystem));
		spdk_json_write_name(w, "namespaces");
		spdk_json_write_array_begin(w);
		for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
		     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
			spdk_nvmf_ns_get_opts(ns, &ns_opts, sizeof(ns_opts));
			spdk_json_write_object_begin(w);
			spdk_json_write_name(w, "nsid");
			spdk_json_write_int32(w, spdk_nvmf_ns_get_id(ns));
			spdk_json_write_name(w, "bdev_name");
			spdk_json_write_string(w, spdk_bdev_get_name(spdk_nvmf_ns_get_bdev(ns)));
			/* NOTE: "name" is kept for compatibility only - new code should use bdev_name. */
			spdk_json_write_name(w, "name");
			spdk_json_write_string(w, spdk_bdev_get_name(spdk_nvmf_ns_get_bdev(ns)));

			if (!spdk_mem_all_zero(ns_opts.nguid, sizeof(ns_opts.nguid))) {
				spdk_json_write_name(w, "nguid");
				json_write_hex_str(w, ns_opts.nguid, sizeof(ns_opts.nguid));
			}

			if (!spdk_mem_all_zero(ns_opts.eui64, sizeof(ns_opts.eui64))) {
				spdk_json_write_name(w, "eui64");
				json_write_hex_str(w, ns_opts.eui64, sizeof(ns_opts.eui64));
			}

			if (!spdk_mem_all_zero(ns_opts.uuid, sizeof(ns_opts.uuid))) {
				char uuid_str[37];

				uuid_unparse_lower((const void *)ns_opts.uuid, uuid_str);
				spdk_json_write_name(w, "uuid");
				spdk_json_write_string(w, uuid_str);
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
	subsystem = spdk_nvmf_subsystem_get_first(g_tgt.tgt);
	while (subsystem) {
		dump_nvmf_subsystem(w, subsystem);
		subsystem = spdk_nvmf_subsystem_get_next(subsystem);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_nvmf_subsystems", spdk_rpc_get_nvmf_subsystems)

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
	char uuid[16];
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
	char *names[RPC_MAX_NAMESPACES]; /* old format - array of strings (bdev names) */
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
};

static void
free_rpc_subsystem(struct rpc_subsystem *req)
{
	free(req->mode);
	free(req->nqn);
	free(req->serial_number);
	free_rpc_namespaces(&req->namespaces);
	free_rpc_listen_addresses(&req->listen_addresses);
	free_rpc_hosts(&req->hosts);
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
};

static void
spdk_rpc_construct_nvmf_subsystem(struct spdk_jsonrpc_request *request,
				  const struct spdk_json_val *params)
{
	struct rpc_subsystem req = {};
	struct spdk_nvmf_subsystem *subsystem;
	size_t i;

	req.core = -1;	/* Explicitly set the core as the uninitialized value */

	if (spdk_json_decode_object(params, rpc_subsystem_decoders,
				    SPDK_COUNTOF(rpc_subsystem_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	/* Mode is no longer a valid parameter, but print out a nice
	 * message if it exists to inform users.
	 */
	if (req.mode) {
		SPDK_NOTICELOG("Mode present in the construct NVMe-oF subsystem RPC.\n"
			       "Mode was removed as a valid parameter.\n");
		if (strcasecmp(req.mode, "Virtual") == 0) {
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
	if (req.core != -1) {
		SPDK_NOTICELOG("Core present in the construct NVMe-oF subsystem RPC.\n"
			       "Core was removed as an option. Subsystems can now run on all available cores.\n");
		SPDK_NOTICELOG("Ignoring it and continuing.\n");
	}

	subsystem = spdk_nvmf_construct_subsystem(req.nqn,
			req.listen_addresses.num_listen_address,
			req.listen_addresses.addresses,
			req.hosts.num_hosts, req.hosts.hosts, req.allow_any_host,
			req.serial_number);
	if (!subsystem) {
		goto invalid;
	}

	for (i = 0; i < req.namespaces.num_ns; i++) {
		struct spdk_nvmf_ns_params *ns_params = &req.namespaces.ns_params[i];
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

		SPDK_STATIC_ASSERT(sizeof(ns_opts.uuid) == sizeof(ns_params->uuid), "size mismatch");
		memcpy(ns_opts.uuid, ns_params->uuid, sizeof(ns_opts.uuid));

		if (spdk_nvmf_subsystem_add_ns(subsystem, bdev, &ns_opts, sizeof(ns_opts)) == 0) {
			SPDK_ERRLOG("Unable to add namespace\n");
			spdk_nvmf_subsystem_destroy(subsystem);
			goto invalid;
		}
	}

	free_rpc_subsystem(&req);

	spdk_nvmf_subsystem_start(subsystem,
				  spdk_rpc_nvmf_subsystem_started,
				  request);

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_subsystem(&req);
}
SPDK_RPC_REGISTER("construct_nvmf_subsystem", spdk_rpc_construct_nvmf_subsystem)

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

	subsystem = spdk_nvmf_tgt_find_subsystem(g_tgt.tgt, req.nqn);
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
SPDK_RPC_REGISTER("delete_nvmf_subsystem", spdk_rpc_delete_nvmf_subsystem)

struct nvmf_rpc_listener_ctx {
	char				*nqn;
	struct rpc_listen_address	address;

	struct spdk_jsonrpc_request	*request;
	struct spdk_nvme_transport_id	trid;
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
nvmf_rpc_listen_paused(struct spdk_nvmf_subsystem *subsystem,
		       void *cb_arg, int status)
{
	struct nvmf_rpc_listener_ctx *ctx = cb_arg;

	if (spdk_nvmf_tgt_listen(g_tgt.tgt, &ctx->trid)) {
		SPDK_ERRLOG("Unable to add listener.\n");
		goto invalid;
	}

	if (spdk_nvmf_subsystem_add_listener(subsystem, &ctx->trid)) {
		goto invalid;
	}

	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_listen_resumed, ctx)) {
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	return;

invalid:
	spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 "Invalid parameters");
	ctx->response_sent = true;
	if (spdk_nvmf_subsystem_resume(subsystem, nvmf_rpc_listen_resumed, ctx)) {
		SPDK_ERRLOG("Failed to resume subsystem\n");
		/* Can't really do anything to recover here - subsystem will remain paused. */
	}
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

	subsystem = spdk_nvmf_tgt_find_subsystem(g_tgt.tgt, ctx->nqn);
	if (!subsystem) {
		SPDK_ERRLOG("Unable to find subsystem with NQN %s\n", ctx->nqn);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	if (rpc_listen_address_to_trid(&ctx->address, &ctx->trid)) {
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}

	if (spdk_nvmf_subsystem_pause(subsystem, nvmf_rpc_listen_paused, ctx)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		nvmf_rpc_listener_ctx_free(ctx);
		return;
	}
}
SPDK_RPC_REGISTER("nvmf_subsystem_add_listener", nvmf_rpc_subsystem_add_listener);


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
	free_rpc_ns_params(&ctx->ns_params);
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

	subsystem = spdk_nvmf_tgt_find_subsystem(g_tgt.tgt, ctx->nqn);
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
SPDK_RPC_REGISTER("nvmf_subsystem_add_ns", nvmf_rpc_subsystem_add_ns)
