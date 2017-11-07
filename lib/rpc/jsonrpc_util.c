/*-
#include <jsonrpc_util.h>
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

#include "spdk/log.h"
#include "spdk/jsonrpc_util.h"
#include "spdk/rpc.h"

#include "spdk/string.h"

struct spdk_jsonrpc_cmd {
	const char *name;
	spdk_jsonrpc_request_handler_fn func;
	size_t num_decoders;
	struct spdk_json_object_decoder decoders[0];
};

union spdk_jsonrpc_util_req_param {
	int boolean;
	int32_t i32;
	uint32_t u32;
	uint64_t u64;
	char *str;
};

struct spdk_jsonrpc_util_req {
	struct spdk_jsonrpc_request *json_req;
	struct spdk_json_write_ctx *json_resp;
	struct spdk_jsonrpc_cmd *cmd;
	bool response_started;
	union spdk_jsonrpc_util_req_param params[0];
};

static void
jsonrpc_req_free(struct spdk_jsonrpc_util_req *req)
{
	size_t i;

	for (i = 0; i < req->cmd->num_decoders; i++) {
		if (req->cmd->decoders[i].decode_func == spdk_json_decode_string) {
			free(req->params[i].str);
		}
	}

	free(req);
}

static int
jsonrpc_req_find_param(struct spdk_jsonrpc_util_req *req, const char *name,
		       spdk_json_decode_fn json_param_type)
{
	size_t i;

	for (i = 0; i < req->cmd->num_decoders; i++) {
		if (strcmp(name, req->cmd->decoders[i].name) != 0) {
			continue;
		}

		if (req->cmd->decoders[i].decode_func != json_param_type) {
			return -EINVAL;
		}

		return i;
	}

	return -ENOENT;
}

static void
jsonrpc_cmd_handler(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params,
		    void *ctx)
{
	struct spdk_jsonrpc_cmd *cmd = ctx;
	size_t alloc_size = sizeof(struct spdk_jsonrpc_util_req) + sizeof(union spdk_jsonrpc_util_req_param)
			    *
			    cmd->num_decoders;
	struct spdk_jsonrpc_util_req *req;
	char buf[64];

	if (params != NULL && cmd->num_decoders == 0) {
		snprintf(buf, sizeof(buf), "%s: requires no parameters", cmd->name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, buf);
		return;
	}

	req =  calloc(alloc_size, 1);
	if (!req) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_get_strerror(ENOMEM));
		return;
	}

	req->json_req = request;
	req->cmd = cmd;


	if (params && spdk_json_decode_object(params, cmd->decoders, cmd->num_decoders, req->params)) {
		snprintf(buf, sizeof(buf), "%s: decoding parameters failed", cmd->name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, buf);

		jsonrpc_req_free(req);
		return;
	}

	req->cmd->func(req);
}

void
spdk_jsonrpc_register_cmd(const char *name, spdk_jsonrpc_request_handler_fn func,
			  const struct spdk_jsonrpc_params *params)
{
	size_t alloc_size = sizeof(struct spdk_jsonrpc_cmd);
	struct spdk_jsonrpc_cmd *cmd;
	size_t i, num_params = 0;

	if (params) {
		for (; params[num_params].name != NULL; num_params++) {
			alloc_size += sizeof(struct spdk_json_object_decoder);
		}
	}

	cmd = calloc(alloc_size, 1);
	assert(cmd != NULL);

	cmd->name = name;
	cmd->func = func;
	cmd->num_decoders = num_params;

	for (i = 0; i < num_params; i++) {
		cmd->decoders[i].name = params[i].name;
		cmd->decoders[i].offset = offsetof(struct spdk_jsonrpc_util_req, params[i]);
		cmd->decoders[i].decode_func = params->type;
		cmd->decoders[i].optional = params[i].optional;
	}

	spdk_rpc_register_ctx_method(name, jsonrpc_cmd_handler, cmd);
}

int
spdk_jsonrpc_param_bool(struct spdk_jsonrpc_util_req *req, const char *name, int default_value)
{
	int ret = jsonrpc_req_find_param(req, name, spdk_json_decode_bool);

	if (ret < 0) {
		errno = -ret;
		return ret == -ENOENT ? default_value : INT_MIN;
	}

	return req->params[ret].boolean;
}

int32_t
spdk_jsonrpc_param_int32(struct spdk_jsonrpc_util_req *req, const char *name, int32_t default_value)
{
	int ret = jsonrpc_req_find_param(req, name, spdk_json_decode_int32);

	if (ret < 0) {
		errno = -ret;
		return ret == -ENOENT ? default_value : INT_MIN;
	}

	return req->params[ret].i32;
}

uint32_t
spdk_jsonrpc_param_uint32(struct spdk_jsonrpc_util_req *req, const char *name,
			  uint32_t default_value)
{
	int ret = jsonrpc_req_find_param(req, name, spdk_json_decode_int32);

	if (ret < 0) {
		errno = -ret;
		return ret == -ENOENT ? default_value : UINT32_MAX;
	}

	return req->params[ret].u32;
}

uint64_t
spdk_jsonrpc_param_uint64(struct spdk_jsonrpc_util_req *req, const char *name,
			  uint64_t default_value)
{
	int ret = jsonrpc_req_find_param(req, name, spdk_json_decode_int32);

	if (ret < 0) {
		errno = -ret;
		return ret == -ENOENT ? default_value : UINT64_MAX;
	}

	return req->params[ret].u64;
}

const char *
spdk_jsonrpc_param_str(struct spdk_jsonrpc_util_req *req, const char *name,
		       const char *default_value)
{
	int ret = jsonrpc_req_find_param(req, name, spdk_json_decode_int32);

	if (ret < 0) {
		errno = -ret;
		return ret == -ENOENT ? default_value : NULL;
	}

	return req->params[ret].str;
}

static int
jsonrpc_response(struct spdk_jsonrpc_util_req *req)
{
	if (req->response_started == false) {
		req->response_started = true;
		req->json_resp = spdk_jsonrpc_begin_result(req->json_req);
		if (req->json_resp == NULL) {
			req->json_req = NULL;
		}
	}

	return req->json_req != NULL;
}

void
spdk_jsonrpc_end_response(struct spdk_jsonrpc_util_req *req)
{
	assert(req->response_started);
	if (req->json_resp) {
		spdk_jsonrpc_end_result(req->json_req, req->json_resp);
	} else {
		assert(req->json_req == NULL);
	}

	jsonrpc_req_free(req);
}

void
spdk_jsonrpc_string_create(struct spdk_jsonrpc_util_req *req, const char *name, const char *fmt,
			   ...)
{
	char *val;
	va_list args;

	if (!jsonrpc_response(req)) {
		return;
	}

	va_start(args, fmt);
	val = spdk_vsprintf_alloc(fmt, args);
	va_end(args);

	if (val == NULL) {
		SPDK_ERRLOG("No memory to create new RPC string parameter.\n");
		return;
	}

	spdk_json_write_name(req->json_resp, name);
	spdk_json_write_string(req->json_resp, val);

	free(val);
}

void spdk_jsonrpc_bool_create(struct spdk_jsonrpc_util_req *req, const char *name, bool val)
{
	if (!jsonrpc_response(req)) {
		return;
	}

	spdk_json_write_name(req->json_resp, name);
	spdk_json_write_bool(req->json_resp, val);
}

void spdk_jsonrpc_int_create(struct spdk_jsonrpc_util_req *req, const char *name, int64_t val)
{
	if (!jsonrpc_response(req)) {
		return;
	}

	spdk_json_write_name(req->json_resp, name);
	spdk_json_write_int64(req->json_resp, val);
}

void spdk_jsonrpc_uint_create(struct spdk_jsonrpc_util_req *req, const char *name, uint64_t val)
{
	if (!jsonrpc_response(req)) {
		return;
	}

	spdk_json_write_name(req->json_resp, name);
	spdk_json_write_uint64(req->json_resp, val);
}

void
spdk_jsonrpc_object_create(struct spdk_jsonrpc_util_req *req, const char *name)
{
	if (!jsonrpc_response(req)) {
		return;
	}

	spdk_json_write_name(req->json_resp, name);
	spdk_json_write_object_begin(req->json_resp);
}

void
spdk_jsonrpc_object_begin(struct spdk_jsonrpc_util_req *req)
{
	if (!jsonrpc_response(req)) {
		return;
	}

	spdk_json_write_object_begin(req->json_resp);
}

void
spdk_jsonrpc_object_end(struct spdk_jsonrpc_util_req *req)
{
	assert(req->response_started);
	if (!jsonrpc_response(req)) {
		return;
	}

	spdk_json_write_object_end(req->json_resp);
}

void
spdk_jsonrpc_array_create(struct spdk_jsonrpc_util_req *req, const char *name)
{
	if (!jsonrpc_response(req)) {
		return;
	}

	spdk_json_write_name(req->json_resp, name);
	spdk_json_write_array_begin(req->json_resp);
}

void
spdk_jsonrpc_array_begin(struct spdk_jsonrpc_util_req *req)
{
	if (!jsonrpc_response(req)) {
		return;
	}

	spdk_json_write_array_begin(req->json_resp);
}


void
spdk_jsonrpc_array_end(struct spdk_jsonrpc_util_req *req)
{
	assert(req->response_started);
	if (!req->json_resp) {
		return;
	}

	spdk_json_write_array_end(req->json_resp);
}

void
spdk_jsonrpc_send_response(struct spdk_jsonrpc_util_req *req, bool success, const char *fmt, ...)
{
	struct spdk_jsonrpc_request *json_req = req->json_req;
	struct spdk_json_write_ctx *w = req->json_resp;
	char *str;
	va_list args;

	assert(req->response_started == false);
	jsonrpc_req_free(req);

	if (!success) {

		va_start(args, fmt);
		str = spdk_vsprintf_alloc(fmt, args);
		va_end(args);

		if (str == NULL) {
			SPDK_ERRLOG("No memory to create error response string.\n");
			spdk_jsonrpc_send_error_response(json_req, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Invalid parameter");
		} else {
			spdk_jsonrpc_send_error_response(json_req, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, str);
			free(str);
		}
	} else {
		w = spdk_jsonrpc_begin_result(json_req);
		if (w == NULL) {
			return;
		}

		spdk_json_write_bool(w, true);
		spdk_jsonrpc_end_result(json_req, w);
	}
}

void spdk_jsonrpc_send_errno_response(struct spdk_jsonrpc_util_req *req, int code)
{
	bool success = code >= 0;
	struct spdk_jsonrpc_request *json_req = req->json_req;
	struct spdk_json_write_ctx *w;

	assert(req->response_started == false);

	jsonrpc_req_free(req);
	code = abs(code);

	if (!success) {
		spdk_jsonrpc_send_error_response(json_req, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_get_strerror(code));
		return;
	}

	w = spdk_jsonrpc_begin_result(json_req);
	if (w) {
		spdk_json_write_bool(w, success);

		if (code) {
			spdk_json_write_name(w, "message");
			spdk_json_write_string(w, spdk_get_strerror(code));
		}

		spdk_jsonrpc_end_result(json_req, w);
	}
}

struct spdk_json_write_ctx *spdk_jsonrpc_response_ctx(struct spdk_jsonrpc_util_req *req)
{
	jsonrpc_response(req);
	return req->json_resp;
}
