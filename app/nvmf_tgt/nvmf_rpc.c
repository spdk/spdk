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

#include <stdlib.h>
#include <stdio.h>

#include "spdk/bdev.h"
#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/nvmf.h"

#include "nvmf_tgt.h"

static void
dump_nvmf_subsystem(struct spdk_json_write_ctx *w, struct nvmf_tgt_subsystem *tgt_subsystem)
{
	struct spdk_nvmf_listen_addr 	*listen_addr;
	struct spdk_nvmf_host		*host;
	struct spdk_nvmf_subsystem	*subsystem = tgt_subsystem->subsystem;

	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "core");
	spdk_json_write_int32(w, tgt_subsystem->lcore);

	spdk_json_write_name(w, "nqn");
	spdk_json_write_string(w, spdk_nvmf_subsystem_get_nqn(subsystem));
	if (spdk_nvmf_subsystem_get_type(subsystem) == SPDK_NVMF_SUBTYPE_NVME) {
		spdk_json_write_name(w, "mode");
		if (spdk_nvmf_subsystem_get_mode(subsystem) == NVMF_SUBSYSTEM_MODE_DIRECT) {
			spdk_json_write_string(w, "direct");
		} else {
			spdk_json_write_string(w, "virtual");
		}
	}
	spdk_json_write_name(w, "subtype");
	if (spdk_nvmf_subsystem_get_type(subsystem) == SPDK_NVMF_SUBTYPE_NVME) {
		spdk_json_write_string(w, "NVMe");
	} else {
		spdk_json_write_string(w, "Discovery");
	}

	spdk_json_write_name(w, "listen_addresses");
	spdk_json_write_array_begin(w);

	TAILQ_FOREACH(listen_addr, &subsystem->listen_addrs, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_name(w, "transport");
		spdk_json_write_string(w, listen_addr->trname);
		spdk_json_write_name(w, "traddr");
		spdk_json_write_string(w, listen_addr->traddr);
		spdk_json_write_name(w, "trsvcid");
		spdk_json_write_string(w, listen_addr->trsvcid);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	spdk_json_write_name(w, "hosts");
	spdk_json_write_array_begin(w);

	TAILQ_FOREACH(host, &subsystem->hosts, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_name(w, "nqn");
		spdk_json_write_string(w, host->nqn);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	if (spdk_nvmf_subsystem_get_type(subsystem) == SPDK_NVMF_SUBTYPE_NVME) {
		if (spdk_nvmf_subsystem_get_mode(subsystem) == NVMF_SUBSYSTEM_MODE_DIRECT) {
			spdk_json_write_name(w, "pci_address");
			spdk_json_write_string_fmt(w, "%04x:%02x:%02x.%x",
						   subsystem->dev.direct.pci_addr.domain,
						   subsystem->dev.direct.pci_addr.bus,
						   subsystem->dev.direct.pci_addr.dev,
						   subsystem->dev.direct.pci_addr.func);
		} else {
			int i;
			spdk_json_write_name(w, "serial_number");
			spdk_json_write_string(w, subsystem->dev.virt.sn);
			spdk_json_write_name(w, "namespaces");
			spdk_json_write_array_begin(w);
			for (i = 0; i < subsystem->dev.virt.ns_count; i++) {
				spdk_json_write_object_begin(w);
				spdk_json_write_name(w, "nsid");
				spdk_json_write_int32(w, i + 1);
				spdk_json_write_name(w, "name");
				spdk_json_write_string(w, subsystem->dev.virt.ns_list[i]->name);
				spdk_json_write_object_end(w);
			}
			spdk_json_write_array_end(w);

		}
	}
	spdk_json_write_object_end(w);
}

static void
spdk_rpc_get_nvmf_subsystems(struct spdk_jsonrpc_server_conn *conn,
			     const struct spdk_json_val *params,
			     const struct spdk_json_val *id)
{
	struct spdk_json_write_ctx *w;
	struct nvmf_tgt_subsystem	*tgt_subsystem;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_nvmf_subsystems requires no parameters");
		return;
	}

	if (id == NULL) {
		return;
	}

	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_array_begin(w);
	tgt_subsystem = nvmf_tgt_subsystem_first();
	while (tgt_subsystem) {
		dump_nvmf_subsystem(w, tgt_subsystem);
		tgt_subsystem = nvmf_tgt_subsystem_next(tgt_subsystem);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(conn, w);
}
SPDK_RPC_REGISTER("get_nvmf_subsystems", spdk_rpc_get_nvmf_subsystems)

#define RPC_MAX_LISTEN_ADDRESSES 255
#define RPC_MAX_HOSTS 255

struct rpc_listen_addresses {
	size_t num_listen_address;
	struct rpc_listen_address addresses[RPC_MAX_LISTEN_ADDRESSES];
};

static const struct spdk_json_object_decoder rpc_listen_address_decoders[] = {
	{"transport", offsetof(struct rpc_listen_address, transport), spdk_json_decode_string},
	{"traddr", offsetof(struct rpc_listen_address, traddr), spdk_json_decode_string},
	{"trsvcid", offsetof(struct rpc_listen_address, trsvcid), spdk_json_decode_string},
};

static int
decode_rpc_listen_address(const struct spdk_json_val *val, void *out)
{
	struct rpc_listen_address *req = (struct rpc_listen_address *)out;
	if (spdk_json_decode_object(val, rpc_listen_address_decoders,
				    sizeof(rpc_listen_address_decoders) / sizeof(*rpc_listen_address_decoders),
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



struct rpc_dev_names {
	size_t num_names;
	char *names[MAX_VIRTUAL_NAMESPACE];
};

static int
decode_rpc_dev_names(const struct spdk_json_val *val, void *out)
{
	struct rpc_dev_names *dev_names = out;

	return spdk_json_decode_array(val, spdk_json_decode_string, dev_names->names,
				      MAX_VIRTUAL_NAMESPACE,
				      &dev_names->num_names, sizeof(char *));
}

static void
free_rpc_dev_names(struct rpc_dev_names *r)
{
	size_t i;

	for (i = 0; i < r->num_names; i++) {
		free(r->names[i]);
	}
}

static void
free_rpc_listen_addresses(struct rpc_listen_addresses *r)
{
	size_t i;

	for (i = 0; i < r->num_listen_address; i++) {
		free(r->addresses[i].transport);
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
	char *pci_address;
	char *serial_number;
	struct rpc_dev_names namespaces;
};

static void
free_rpc_subsystem(struct rpc_subsystem *req)
{
	free_rpc_dev_names(&req->namespaces);
	free_rpc_listen_addresses(&req->listen_addresses);
	free_rpc_hosts(&req->hosts);
}

static const struct spdk_json_object_decoder rpc_subsystem_decoders[] = {
	{"core", offsetof(struct rpc_subsystem, core), spdk_json_decode_int32, true},
	{"mode", offsetof(struct rpc_subsystem, mode), spdk_json_decode_string},
	{"nqn", offsetof(struct rpc_subsystem, nqn), spdk_json_decode_string},
	{"listen_addresses", offsetof(struct rpc_subsystem, listen_addresses), decode_rpc_listen_addresses},
	{"hosts", offsetof(struct rpc_subsystem, hosts), decode_rpc_hosts, true},
	{"pci_address", offsetof(struct rpc_subsystem, pci_address), spdk_json_decode_string, true},
	{"serial_number", offsetof(struct rpc_subsystem, serial_number), spdk_json_decode_string, true},
	{"namespaces", offsetof(struct rpc_subsystem, namespaces), decode_rpc_dev_names, true},
};

static void
spdk_rpc_construct_nvmf_subsystem(struct spdk_jsonrpc_server_conn *conn,
				  const struct spdk_json_val *params,
				  const struct spdk_json_val *id)
{
	struct rpc_subsystem req = {};
	struct spdk_json_write_ctx *w;
	int ret;

	if (spdk_json_decode_object(params, rpc_subsystem_decoders,
				    sizeof(rpc_subsystem_decoders) / sizeof(*rpc_subsystem_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	ret = spdk_nvmf_parse_subsystem_for_rpc(req.nqn, req.mode, req.core,
						req.listen_addresses.num_listen_address,
						req.listen_addresses.addresses,
						req.hosts.num_hosts, req.hosts.hosts, req.pci_address,
						req.serial_number,
						req.namespaces.num_names, req.namespaces.names);
	if (ret) {
		goto invalid;
	}

	free_rpc_subsystem(&req);

	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(conn, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
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

static const struct spdk_json_object_decoder rpc_delete_subsystem_decoders[] = {
	{"nqn", offsetof(struct rpc_delete_subsystem, nqn), spdk_json_decode_string},
};

static void
spdk_rpc_delete_nvmf_subsystem(struct spdk_jsonrpc_server_conn *conn,
			       const struct spdk_json_val *params,
			       const struct spdk_json_val *id)
{
	struct rpc_delete_subsystem req = {};
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_delete_subsystem_decoders,
				    sizeof(rpc_delete_subsystem_decoders) / sizeof(*rpc_delete_subsystem_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.nqn == NULL) {
		SPDK_ERRLOG("missing name param\n");
		goto invalid;
	}

	if (nvmf_tgt_shutdown_subsystem_by_nqn(req.nqn)) {
		SPDK_ERRLOG("shutdown_subsystem failed\n");
		goto invalid;
	}

	free_rpc_delete_subsystem(&req);

	if (id == NULL) {
		return;
	}

	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(conn, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_delete_subsystem(&req);
}
SPDK_RPC_REGISTER("delete_nvmf_subsystem", spdk_rpc_delete_nvmf_subsystem)
