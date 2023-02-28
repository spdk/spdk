/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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
