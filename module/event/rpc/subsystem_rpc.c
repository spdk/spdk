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

#include "spdk_internal/event.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/env.h"

static void
spdk_rpc_get_subsystems(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct spdk_subsystem *subsystem;
	struct spdk_subsystem_depend *deps;

	if (params) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "'get_subsystems' requires no arguments");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	TAILQ_FOREACH(subsystem, &g_subsystems, tailq) {
		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "subsystem", subsystem->name);
		spdk_json_write_named_array_begin(w, "depends_on");
		TAILQ_FOREACH(deps, &g_subsystems_deps, tailq) {
			if (strcmp(subsystem->name, deps->name) == 0) {
				spdk_json_write_string(w, deps->depends_on);
			}
		}
		spdk_json_write_array_end(w);

		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}

SPDK_RPC_REGISTER("get_subsystems", spdk_rpc_get_subsystems, SPDK_RPC_RUNTIME)

struct rpc_get_subsystem_config {
	char *name;
};

static const struct spdk_json_object_decoder rpc_get_subsystem_config[] = {
	{"name", offsetof(struct rpc_get_subsystem_config, name), spdk_json_decode_string},
};

static void
spdk_rpc_get_subsystem_config(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_get_subsystem_config req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_subsystem *subsystem;

	if (spdk_json_decode_object(params, rpc_get_subsystem_config,
				    SPDK_COUNTOF(rpc_get_subsystem_config), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid arguments");
		return;
	}

	subsystem = spdk_subsystem_find(&g_subsystems, req.name);
	if (!subsystem) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Subsystem '%s' not found", req.name);
		free(req.name);
		return;
	}

	free(req.name);

	w = spdk_jsonrpc_begin_result(request);
	spdk_subsystem_config_json(w, subsystem);
	spdk_jsonrpc_end_result(request, w);
}

SPDK_RPC_REGISTER("get_subsystem_config", spdk_rpc_get_subsystem_config, SPDK_RPC_RUNTIME)
