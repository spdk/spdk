/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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
#include "spdk/rpc.h"
#include "spdk/env_dpdk.h"
#include "spdk/log.h"

static void
rpc_env_dpdk_get_mem_stats(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	FILE *file = NULL;
	struct spdk_json_write_ctx *w;
	char default_filename[] = "/tmp/spdk_mem_dump.txt";

	if (params != NULL) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "env_dpdk_get_mem_stats doesn't accept any parameters.\n");
	}

	file = fopen(default_filename, "w");
	if (!file) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Unable to open file for writing.\n");
		return;
	}

	spdk_env_dpdk_dump_mem_stats(file);
	fclose(file);
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "filename", default_filename);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("env_dpdk_get_mem_stats", rpc_env_dpdk_get_mem_stats, SPDK_RPC_RUNTIME)
