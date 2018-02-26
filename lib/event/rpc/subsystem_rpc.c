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
#include "spdk/util.h"

#define RPC_MAX_SUBSYSTEMS 255

struct rpc_get_subsystems {
	size_t num_subsystems;
	char *subsystems[RPC_MAX_SUBSYSTEMS];
	bool no_config;
};

static int
decode_rpc_subsystems(const struct spdk_json_val *val, void *out)
{
	struct rpc_get_subsystems *req = out;

	return spdk_json_decode_array(val, spdk_json_decode_string, req->subsystems, RPC_MAX_SUBSYSTEMS,
				      &req->num_subsystems, sizeof(char *));
}

static const struct spdk_json_object_decoder rpc_get_subsystems_config[] = {
	{"subsystems", 0, decode_rpc_subsystems, true},
	{"no_config", offsetof(struct rpc_get_subsystems, no_config), spdk_json_decode_bool, true},
};

static void
spdk_subsystem_write_dependency_json(struct spdk_json_write_ctx *w,
				     struct spdk_subsystem *subsystem)
{
	struct spdk_subsystem_depend *deps;

	spdk_json_write_named_array_begin(w, "depends_on");
	TAILQ_FOREACH(deps, &g_subsystems_deps, tailq) {
		if (strcmp(subsystem->name, deps->name) == 0) {
			spdk_json_write_string(w, deps->depends_on);
		}
	}
	spdk_json_write_array_end(w);
}

static void
spdk_subsystem_write_config_json(struct spdk_json_write_ctx *w, struct spdk_subsystem *subsystem)
{

	if (subsystem->write_config_json) {
		spdk_json_write_named_object_begin(w, "config");
		subsystem->write_config_json(w);
		spdk_json_write_object_end(w);
	} else {
		spdk_json_write_named_null(w, "config");
	}
}

static void
spdk_subsystem_write_json(struct spdk_json_write_ctx *w, struct spdk_subsystem *subsystem,
			  bool config)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "subsystem", subsystem->name);

	if (config) {
		spdk_subsystem_write_config_json(w, subsystem);
	}

	spdk_subsystem_write_dependency_json(w, subsystem);
	spdk_json_write_object_end(w);
}

static void
spdk_rpc_get_subsystems(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_get_subsystems req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_subsystem *subsystem;
	size_t i;

	if (params &&
	    spdk_json_decode_object(params, rpc_get_subsystems_config, SPDK_COUNTOF(rpc_get_subsystems_config),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		return;
	}

	for (i = 0; i < req.num_subsystems; i++) {
		if (!spdk_subsystem_find(&g_subsystems, req.subsystems[i])) {
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							     "Subsystem '%s' not found", req.subsystems[i]);
			goto out;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		goto out;
	}

	spdk_json_write_array_begin(w);
	TAILQ_FOREACH(subsystem, &g_subsystems, tailq) {
		for (i = 0; i < req.num_subsystems; i++) {
			if (strcmp(req.subsystems[i], subsystem->name) == 0) {
				break;
			}
		}

		if (req.num_subsystems && i == req.num_subsystems) {
			continue;
		}

		spdk_subsystem_write_json(w, subsystem, !req.no_config);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
out:
	for (i = 0; i < req.num_subsystems; i++) {
		free(req.subsystems[i]);
	}
}

SPDK_RPC_REGISTER("get_subsystems", spdk_rpc_get_subsystems)
