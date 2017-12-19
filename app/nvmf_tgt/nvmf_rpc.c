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
#include "spdk/util.h"

#include "nvmf_tgt.h"

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

		spdk_json_write_name(w, "serial_number");
		spdk_json_write_string(w, spdk_nvmf_subsystem_get_sn(subsystem));
		spdk_json_write_name(w, "namespaces");
		spdk_json_write_array_begin(w);
		for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
		     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
			spdk_json_write_object_begin(w);
			spdk_json_write_name(w, "nsid");
			spdk_json_write_int32(w, spdk_nvmf_ns_get_id(ns));
			spdk_json_write_name(w, "bdev_name");
			spdk_json_write_string(w, spdk_bdev_get_name(spdk_nvmf_ns_get_bdev(ns)));
			/* NOTE: "name" is kept for compatibility only - new code should use bdev_name. */
			spdk_json_write_name(w, "name");
			spdk_json_write_string(w, spdk_bdev_get_name(spdk_nvmf_ns_get_bdev(ns)));
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



struct rpc_namespaces {
	size_t num_ns;
	struct spdk_nvmf_ns_params ns_params[RPC_MAX_NAMESPACES];
};


static const struct spdk_json_object_decoder rpc_ns_params_decoders[] = {
	{"nsid", offsetof(struct spdk_nvmf_ns_params, nsid), spdk_json_decode_uint32, true},
	{"bdev_name", offsetof(struct spdk_nvmf_ns_params, bdev_name), spdk_json_decode_string},
};

static void
free_rpc_namespaces(struct rpc_namespaces *r)
{
	size_t i;

	for (i = 0; i < r->num_ns; i++) {
		free(r->ns_params[i].bdev_name);
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
		free(r->addresses[i].transport);
		free(r->addresses[i].adrfam);
		free(r->addresses[i].traddr);
		free(r->addresses[i].trsvcid);
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
	{"listen_addresses", offsetof(struct rpc_subsystem, listen_addresses), decode_rpc_listen_addresses},
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
			req.serial_number,
			req.namespaces.num_ns, req.namespaces.ns_params);
	if (!subsystem) {
		goto invalid;
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
