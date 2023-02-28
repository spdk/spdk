/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2020, 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/sock.h"

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"

#include "spdk/log.h"


static const struct spdk_json_object_decoder rpc_sock_impl_get_opts_decoders[] = {
	{ "impl_name", 0, spdk_json_decode_string, false },
};

static void
rpc_sock_impl_get_options(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	char *impl_name = NULL;
	struct spdk_sock_impl_opts sock_opts = {};
	struct spdk_json_write_ctx *w;
	size_t len;
	int rc;

	if (spdk_json_decode_object(params, rpc_sock_impl_get_opts_decoders,
				    SPDK_COUNTOF(rpc_sock_impl_get_opts_decoders), &impl_name)) {
		SPDK_ERRLOG("spdk_json_decode_object() failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	len = sizeof(sock_opts);
	rc = spdk_sock_impl_get_opts(impl_name, &sock_opts, &len);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint32(w, "recv_buf_size", sock_opts.recv_buf_size);
	spdk_json_write_named_uint32(w, "send_buf_size", sock_opts.send_buf_size);
	spdk_json_write_named_bool(w, "enable_recv_pipe", sock_opts.enable_recv_pipe);
	spdk_json_write_named_bool(w, "enable_quickack", sock_opts.enable_quickack);
	spdk_json_write_named_uint32(w, "enable_placement_id", sock_opts.enable_placement_id);
	spdk_json_write_named_bool(w, "enable_zerocopy_send_server", sock_opts.enable_zerocopy_send_server);
	spdk_json_write_named_bool(w, "enable_zerocopy_send_client", sock_opts.enable_zerocopy_send_client);
	spdk_json_write_named_uint32(w, "zerocopy_threshold", sock_opts.zerocopy_threshold);
	spdk_json_write_named_uint32(w, "tls_version", sock_opts.tls_version);
	spdk_json_write_named_bool(w, "enable_ktls", sock_opts.enable_ktls);
	if (sock_opts.psk_key) {
		spdk_json_write_named_string(w, "psk_key", sock_opts.psk_key);
	}
	if (sock_opts.psk_identity) {
		spdk_json_write_named_string(w, "psk_identity", sock_opts.psk_identity);
	}
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
	free(impl_name);
}
SPDK_RPC_REGISTER("sock_impl_get_options", rpc_sock_impl_get_options,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)

struct spdk_rpc_sock_impl_set_opts {
	char *impl_name;
	struct spdk_sock_impl_opts sock_opts;
};

static const struct spdk_json_object_decoder rpc_sock_impl_set_opts_decoders[] = {
	{
		"impl_name", offsetof(struct spdk_rpc_sock_impl_set_opts, impl_name),
		spdk_json_decode_string, false
	},
	{
		"recv_buf_size", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.recv_buf_size),
		spdk_json_decode_uint32, true
	},
	{
		"send_buf_size", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.send_buf_size),
		spdk_json_decode_uint32, true
	},
	{
		"enable_recv_pipe", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.enable_recv_pipe),
		spdk_json_decode_bool, true
	},
	{
		"enable_quickack", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.enable_quickack),
		spdk_json_decode_bool, true
	},
	{
		"enable_placement_id", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.enable_placement_id),
		spdk_json_decode_uint32, true
	},
	{
		"enable_zerocopy_send_server", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.enable_zerocopy_send_server),
		spdk_json_decode_bool, true
	},
	{
		"enable_zerocopy_send_client", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.enable_zerocopy_send_client),
		spdk_json_decode_bool, true
	},
	{
		"zerocopy_threshold", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.zerocopy_threshold),
		spdk_json_decode_uint32, true
	},
	{
		"tls_version", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.tls_version),
		spdk_json_decode_uint32, true
	},
	{
		"enable_ktls", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.enable_ktls),
		spdk_json_decode_bool, true
	},
	{
		"psk_key", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.psk_key),
		spdk_json_decode_string, true
	},
	{
		"psk_identity", offsetof(struct spdk_rpc_sock_impl_set_opts, sock_opts.psk_identity),
		spdk_json_decode_string, true
	}
};

static void
rpc_sock_impl_set_options(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct spdk_rpc_sock_impl_set_opts opts = {};
	size_t len;
	int rc;

	/* Get type */
	if (spdk_json_decode_object(params, rpc_sock_impl_set_opts_decoders,
				    SPDK_COUNTOF(rpc_sock_impl_set_opts_decoders), &opts)) {
		SPDK_ERRLOG("spdk_json_decode_object() failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	/* Retrieve default opts for requested socket implementation */
	len = sizeof(opts.sock_opts);
	rc = spdk_sock_impl_get_opts(opts.impl_name, &opts.sock_opts, &len);
	if (rc) {
		free(opts.impl_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	/* Decode opts */
	if (spdk_json_decode_object(params, rpc_sock_impl_set_opts_decoders,
				    SPDK_COUNTOF(rpc_sock_impl_set_opts_decoders), &opts)) {
		SPDK_ERRLOG("spdk_json_decode_object() failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	rc = spdk_sock_impl_set_opts(opts.impl_name, &opts.sock_opts, sizeof(opts.sock_opts));
	if (rc != 0) {
		free(opts.impl_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	free(opts.impl_name);
}
SPDK_RPC_REGISTER("sock_impl_set_options", rpc_sock_impl_set_options, SPDK_RPC_STARTUP)

static void
rpc_sock_set_default_impl(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	char *impl_name = NULL;
	int rc;

	/* Reuse get_opts decoder */
	if (spdk_json_decode_object(params, rpc_sock_impl_get_opts_decoders,
				    SPDK_COUNTOF(rpc_sock_impl_get_opts_decoders), &impl_name)) {
		SPDK_ERRLOG("spdk_json_decode_object() failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	rc = spdk_sock_set_default_impl(impl_name);
	if (rc) {
		free(impl_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
	free(impl_name);
}
SPDK_RPC_REGISTER("sock_set_default_impl", rpc_sock_set_default_impl, SPDK_RPC_STARTUP)
