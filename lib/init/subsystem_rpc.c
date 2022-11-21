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

#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/env.h"
#include "spdk/log.h"

#include "spdk_internal/init.h"

#include "subsystem.h"

static void
rpc_framework_get_subsystems(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct spdk_subsystem *subsystem;
	struct spdk_subsystem_depend *deps;

	if (params) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "'framework_get_subsystems' requires no arguments");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	subsystem = subsystem_get_first();
	while (subsystem != NULL) {
		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "subsystem", subsystem->name);
		spdk_json_write_named_array_begin(w, "depends_on");
		deps = subsystem_get_first_depend();
		while (deps != NULL) {
			if (strcmp(subsystem->name, deps->name) == 0) {
				spdk_json_write_string(w, deps->depends_on);
			}
			deps = subsystem_get_next_depend(deps);
		}
		spdk_json_write_array_end(w);
		spdk_json_write_object_end(w);
		subsystem = subsystem_get_next(subsystem);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}

SPDK_RPC_REGISTER("framework_get_subsystems", rpc_framework_get_subsystems, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(framework_get_subsystems, get_subsystems)

struct rpc_framework_get_config_ctx {
	char *name;
};

static const struct spdk_json_object_decoder rpc_framework_get_config_ctx[] = {
	{"name", offsetof(struct rpc_framework_get_config_ctx, name), spdk_json_decode_string},
};

static void
rpc_framework_get_config(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_framework_get_config_ctx req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_subsystem *subsystem;

	if (spdk_json_decode_object(params, rpc_framework_get_config_ctx,
				    SPDK_COUNTOF(rpc_framework_get_config_ctx), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid arguments");
		return;
	}

	subsystem = subsystem_find(req.name);
	if (!subsystem) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Subsystem '%s' not found", req.name);
		free(req.name);
		return;
	}

	free(req.name);

	w = spdk_jsonrpc_begin_result(request);
	subsystem_config_json(w, subsystem);
	spdk_jsonrpc_end_result(request, w);
}

SPDK_RPC_REGISTER("framework_get_config", rpc_framework_get_config, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(framework_get_config, get_subsystem_config)

static void
dump_pci_device(void *ctx, struct spdk_pci_device *dev)
{
	struct spdk_json_write_ctx *w = ctx;
	struct spdk_pci_addr addr;
	char config[4096], bdf[32];
	int rc;

	addr = spdk_pci_device_get_addr(dev);
	spdk_pci_addr_fmt(bdf, sizeof(bdf), &addr);

	rc = spdk_pci_device_cfg_read(dev, config, sizeof(config), 0);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to read config space of device: %s\n", bdf);
		return;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "address", bdf);
	spdk_json_write_named_string(w, "type", spdk_pci_device_get_type(dev));

	/* Don't write the extended config space if it's all zeroes */
	if (spdk_mem_all_zero(&config[256], sizeof(config) - 256)) {
		spdk_json_write_named_bytearray(w, "config_space", config, 256);
	} else {
		spdk_json_write_named_bytearray(w, "config_space", config, sizeof(config));
	}

	spdk_json_write_object_end(w);
}

static void
rpc_framework_get_pci_devices(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "framework_get_pci_devices doesn't accept any parameters.\n");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_array_begin(w);
	spdk_pci_for_each_device(w, dump_pci_device);
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("framework_get_pci_devices", rpc_framework_get_pci_devices, SPDK_RPC_RUNTIME)
