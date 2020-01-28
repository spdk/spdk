/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
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

#include "spdk/sock.h"

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"

#include "spdk_internal/log.h"


static const struct spdk_json_object_decoder rpc_sock_get_opts_decoders[] = {
	{ "impl_name", 0, spdk_json_decode_string, false },
};

static void
spdk_rpc_sock_get_options(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	char *impl_name = NULL;
	struct spdk_sock_opts sock_opts = {};
	struct spdk_json_write_ctx *w;
	size_t len;
	int rc;

	if (spdk_json_decode_object(params, rpc_sock_get_opts_decoders,
				    SPDK_COUNTOF(rpc_sock_get_opts_decoders), &impl_name)) {
		SPDK_ERRLOG("spdk_json_decode_object() failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	len = sizeof(sock_opts);
	rc = spdk_sock_get_opts(impl_name, &sock_opts, &len);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint32(w, "recv_pipe_size", sock_opts.recv_pipe_size);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
	free(impl_name);
}
SPDK_RPC_REGISTER("sock_get_options", spdk_rpc_sock_get_options,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

struct spdk_rpc_sock_set_opts {
	char *impl_name;
	struct spdk_sock_opts sock_opts;
};

static const struct spdk_json_object_decoder rpc_sock_set_opts_decoders[] = {
	{ "impl_name", offsetof(struct spdk_rpc_sock_set_opts, impl_name), spdk_json_decode_string, false },
	{ "recv_pipe_size", offsetof(struct spdk_rpc_sock_set_opts, sock_opts.recv_pipe_size), spdk_json_decode_uint32, true },
};

static void
spdk_rpc_sock_set_options(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct spdk_rpc_sock_set_opts opts = {};
	struct spdk_json_write_ctx *w;
	size_t len;
	int rc;

	/* Get type */
	if (spdk_json_decode_object(params, rpc_sock_set_opts_decoders,
				    SPDK_COUNTOF(rpc_sock_set_opts_decoders), &opts)) {
		SPDK_ERRLOG("spdk_json_decode_object() failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	/* Retrieve default opts for requested socket implementation */
	len = sizeof(opts.sock_opts);
	rc = spdk_sock_get_opts(opts.impl_name, &opts.sock_opts, &len);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	/* Decode opts */
	if (spdk_json_decode_object(params, rpc_sock_set_opts_decoders,
				    SPDK_COUNTOF(rpc_sock_set_opts_decoders), &opts)) {
		SPDK_ERRLOG("spdk_json_decode_object() failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	rc = spdk_sock_set_opts(opts.impl_name, &opts.sock_opts, sizeof(opts.sock_opts));
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	free(opts.impl_name);
}
SPDK_RPC_REGISTER("sock_set_options", spdk_rpc_sock_set_options, SPDK_RPC_STARTUP)
