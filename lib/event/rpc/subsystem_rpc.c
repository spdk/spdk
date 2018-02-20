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

#include "spdk/event.h"
#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"


struct rpc_get_subsystem_dependency {
	char *subsystem;
};

static const struct spdk_json_object_decoder rpc_get_subsystem_dependency_decoders[] = {
	{"subsystem", offsetof(struct rpc_get_subsystem_dependency, subsystem), spdk_json_decode_string, true},
};

static void
spdk_rpc_get_subsystem_dependency(struct spdk_jsonrpc_request *request,
				  const struct spdk_json_val *params)
{
	struct rpc_get_subsystem_dependency req = {};
	struct spdk_json_write_ctx *w;
	char **deps = NULL;
	int n = 0;
	int i;

	if (params && spdk_json_decode_object(params, rpc_get_subsystem_dependency_decoders,
					      SPDK_COUNTOF(rpc_get_subsystem_dependency_decoders),
					      &req)) {
		SPDK_DEBUGLOG(SPDK_LOG_REACTOR, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	n = spdk_get_subsystem_dependency(&deps, req.subsystem);
	if (n < 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-n));
		goto out;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		goto out;
	}

	spdk_json_write_array_begin(w);
	for (i = 0; i < n; i++) {
		spdk_json_write_string(w, deps[i]);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
out:
	free(req.subsystem);
	for (i = 0; i < n; i++) {
		free(deps[i]);
	}
	free(deps);
}

SPDK_RPC_REGISTER("get_subsystem_dependency", spdk_rpc_get_subsystem_dependency)
