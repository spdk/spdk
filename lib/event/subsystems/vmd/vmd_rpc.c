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

#include "spdk/vmd.h"

#include "spdk/rpc.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

struct spdk_rpc_vmd_opts {
	bool	enable;
};

static const struct spdk_json_object_decoder rpc_set_vmd_opts_decoders[] = {
	{"enable", offsetof(struct spdk_rpc_vmd_opts, enable), spdk_json_decode_bool, true},
};

static void
spdk_rpc_vmd_enable(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct spdk_rpc_vmd_opts opts = {};
	struct spdk_json_write_ctx *w;
	int rc = 0;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_set_vmd_opts_decoders,
					    SPDK_COUNTOF(rpc_set_vmd_opts_decoders), &opts)) {
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			return;
		}
	}

	if (opts.enable) {
		rc = spdk_vmd_init();
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, rc == 0);
	spdk_jsonrpc_end_result(request, w);
}

SPDK_RPC_REGISTER("enable_vmd", spdk_rpc_vmd_enable, SPDK_RPC_STARTUP)
