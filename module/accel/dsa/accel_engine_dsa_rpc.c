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

#include "accel_engine_dsa.h"

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/event.h"
#include "spdk/stdinc.h"
#include "spdk/env.h"

struct rpc_dsa_scan_accel_engine {
	bool config_kernel_mode;
};

static const struct spdk_json_object_decoder rpc_dsa_scan_accel_engine_decoder[] = {
	{"config_kernel_mode", offsetof(struct rpc_dsa_scan_accel_engine, config_kernel_mode), spdk_json_decode_bool, true},
};

static void
rpc_dsa_scan_accel_engine(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_dsa_scan_accel_engine req = {};

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_dsa_scan_accel_engine_decoder,
					    SPDK_COUNTOF(rpc_dsa_scan_accel_engine_decoder),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object() failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			return;
		}
	}

	if (req.config_kernel_mode) {
		SPDK_NOTICELOG("Enabling DSA kernel-mode\n");
	} else {
		SPDK_NOTICELOG("Enabling DSA user-mode\n");
	}

	accel_engine_dsa_enable_probe(req.config_kernel_mode);
	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("dsa_scan_accel_engine", rpc_dsa_scan_accel_engine, SPDK_RPC_STARTUP)
