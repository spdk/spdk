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

#include "spdk/util.h"
#include "jsonrpc_internal.h"

struct jsonrpc_response {
	const struct spdk_json_val *version;
	const struct spdk_json_val *id;
	const struct spdk_json_val *result;
};

static int
capture_string(const struct spdk_json_val *val, void *out)
{
	const struct spdk_json_val **vptr = out;

	if (spdk_json_strequal(val, "2.0") != true) {
		return SPDK_JSON_PARSE_INVALID;
	}

	*vptr = val;
	return 0;
}

static int
capture_id(const struct spdk_json_val *val, void *out)
{
	const struct spdk_json_val **vptr = out;

	if (val->type != SPDK_JSON_VAL_STRING && val->type != SPDK_JSON_VAL_NUMBER) {
		return SPDK_JSON_PARSE_INVALID;
	}

	*vptr = val;
	return 0;
}

static int
capture_any(const struct spdk_json_val *val, void *out)
{
	const struct spdk_json_val **vptr = out;

	*vptr = val;
	return 0;
}

static const struct spdk_json_object_decoder jsonrpc_response_decoders[] = {
	{"jsonrpc", offsetof(struct jsonrpc_response, version), capture_string},
	{"id", offsetof(struct jsonrpc_response, id), capture_id},
	{"result", offsetof(struct jsonrpc_response, result), capture_any},
};

static int
parse_single_response(struct spdk_json_val *values,
		      spdk_jsonrpc_client_response_parser parser_fn,
		      void *parser_ctx)
{
	struct jsonrpc_response resp = {};

	if (spdk_json_decode_object(values, jsonrpc_response_decoders,
				    SPDK_COUNTOF(jsonrpc_response_decoders),
				    &resp)) {
		return SPDK_JSON_PARSE_INVALID;
	}

	return parser_fn(parser_ctx, resp.result);
}

int
spdk_jsonrpc_parse_response(struct spdk_jsonrpc_client *client, void *json, size_t size)
{
	ssize_t rc;
	void *end = NULL;

	/* Check to see if we have received a full JSON value. */
	rc = spdk_json_parse(json, size, NULL, 0, &end, 0);
	if (rc == SPDK_JSON_PARSE_INCOMPLETE) {
		return rc;
	}

	SPDK_DEBUGLOG(SPDK_LOG_RPC_CLIENT, "Json string is :\n%s\n", (char *)json);
	if (rc < 0 || rc > SPDK_JSONRPC_MAX_VALUES) {
		SPDK_ERRLOG("JSON parse error\n");
		/*
		 * Can't recover from parse error (no guaranteed resync point in streaming JSON).
		 * Return an error to indicate that the connection should be closed.
		 */
		return SPDK_JSON_PARSE_INVALID;
	}

	/* Decode a second time now that there is a full JSON value available. */
	rc = spdk_json_parse(json, size, client->values, SPDK_JSONRPC_MAX_VALUES, &end,
			     SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE);
	if (rc < 0 || rc > SPDK_JSONRPC_MAX_VALUES) {
		SPDK_ERRLOG("JSON parse error on second pass\n");
		return SPDK_JSON_PARSE_INVALID;
	}

	assert(end != NULL);

	if (client->values[0].type != SPDK_JSON_VAL_OBJECT_BEGIN) {
		SPDK_ERRLOG("top-level JSON value was not object\n");
		return SPDK_JSON_PARSE_INVALID;
	}

	rc = parse_single_response(client->values, client->parser_fn, client->parser_ctx);

	return rc;
}

static int
jsonrpc_client_write_cb(void *cb_ctx, const void *data, size_t size)
{
	struct spdk_jsonrpc_client_request *request = cb_ctx;
	size_t new_size = request->send_buf_size;

	while (new_size - request->send_len < size) {
		if (new_size >= SPDK_JSONRPC_SEND_BUF_SIZE_MAX) {
			SPDK_ERRLOG("Send buf exceeded maximum size (%zu)\n",
				    (size_t)SPDK_JSONRPC_SEND_BUF_SIZE_MAX);
			return -ENOSPC;
		}

		new_size *= 2;
	}

	if (new_size != request->send_buf_size) {
		uint8_t *new_buf;

		new_buf = realloc(request->send_buf, new_size);
		if (new_buf == NULL) {
			SPDK_ERRLOG("Resizing send_buf failed (current size %zu, new size %zu)\n",
				    request->send_buf_size, new_size);
			return -ENOMEM;
		}

		request->send_buf = new_buf;
		request->send_buf_size = new_size;
	}

	memcpy(request->send_buf + request->send_len, data, size);
	request->send_len += size;

	return 0;
}

struct spdk_json_write_ctx *
spdk_jsonrpc_begin_request(struct spdk_jsonrpc_client_request *request, int32_t id,
			   const char *method)
{
	struct spdk_json_write_ctx *w;

	w = spdk_json_write_begin(jsonrpc_client_write_cb, request, 0);
	if (w == NULL) {
		return NULL;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "jsonrpc", "2.0");

	if (id >= 0) {
		spdk_json_write_named_int32(w, "id", id);
	}

	if (method) {
		spdk_json_write_named_string(w, "method", method);
	}

	return w;
}

void
spdk_jsonrpc_end_request(struct spdk_jsonrpc_client_request *request, struct spdk_json_write_ctx *w)
{
	assert(w != NULL);

	spdk_json_write_object_end(w);
	spdk_json_write_end(w);
	jsonrpc_client_write_cb(request, "\n", 1);
}

SPDK_LOG_REGISTER_COMPONENT("rpc_client", SPDK_LOG_RPC_CLIENT)
