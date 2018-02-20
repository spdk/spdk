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

#include "jsonrpc_internal.h"

#include "spdk/util.h"

struct jsonrpc_request {
	const struct spdk_json_val *version;
	const struct spdk_json_val *method;
	const struct spdk_json_val *params;
	const struct spdk_json_val *id;
};

static int
capture_val(const struct spdk_json_val *val, void *out)
{
	const struct spdk_json_val **vptr = out;

	*vptr = val;
	return 0;
}

static const struct spdk_json_object_decoder jsonrpc_request_decoders[] = {
	{"jsonrpc", offsetof(struct jsonrpc_request, version), capture_val},
	{"method", offsetof(struct jsonrpc_request, method), capture_val},
	{"params", offsetof(struct jsonrpc_request, params), capture_val, true},
	{"id", offsetof(struct jsonrpc_request, id), capture_val, true},
};

static void
parse_single_request(struct spdk_jsonrpc_request *request, struct spdk_json_val *values)
{
	bool invalid = false;
	struct jsonrpc_request req = {};

	if (spdk_json_decode_object(values, jsonrpc_request_decoders,
				    SPDK_COUNTOF(jsonrpc_request_decoders),
				    &req)) {
		invalid = true;
		goto done;
	}

	if (!req.version || req.version->type != SPDK_JSON_VAL_STRING ||
	    !spdk_json_strequal(req.version, "2.0")) {
		invalid = true;
	}

	if (!req.method || req.method->type != SPDK_JSON_VAL_STRING) {
		req.method = NULL;
		invalid = true;
	}

	if (req.id) {
		if (req.id->type == SPDK_JSON_VAL_STRING ||
		    req.id->type == SPDK_JSON_VAL_NUMBER) {
			/* Copy value into request */
			if (req.id->len <= SPDK_JSONRPC_ID_MAX_LEN) {
				request->id.type = req.id->type;
				request->id.len = req.id->len;
				memcpy(request->id.start, req.id->start, req.id->len);
			} else {
				SPDK_DEBUGLOG(SPDK_LOG_RPC, "JSON-RPC request id too long (%u)\n",
					      req.id->len);
				invalid = true;
			}
		} else if (req.id->type == SPDK_JSON_VAL_NULL) {
			request->id.type = SPDK_JSON_VAL_NULL;
		} else  {
			invalid = true;
		}
	}

	if (req.params) {
		if (req.params->type != SPDK_JSON_VAL_ARRAY_BEGIN &&
		    req.params->type != SPDK_JSON_VAL_OBJECT_BEGIN) {
			req.params = NULL;
			invalid = true;
		}
	}

done:
	if (invalid) {
		spdk_jsonrpc_server_handle_error(request, SPDK_JSONRPC_ERROR_INVALID_REQUEST);
	} else {
		spdk_jsonrpc_server_handle_request(request, req.method, req.params);
	}
}

int
spdk_jsonrpc_parse_request(struct spdk_jsonrpc_server_conn *conn, void *json, size_t size)
{
	struct spdk_jsonrpc_request *request;
	ssize_t rc;
	void *end = NULL;

	/* Check to see if we have received a full JSON value. */
	rc = spdk_json_parse(json, size, NULL, 0, &end, 0);
	if (rc == SPDK_JSON_PARSE_INCOMPLETE) {
		return 0;
	}

	request = calloc(1, sizeof(*request));
	if (request == NULL) {
		SPDK_DEBUGLOG(SPDK_LOG_RPC, "Out of memory allocating request\n");
		return -1;
	}

	conn->outstanding_requests++;

	request->conn = conn;
	request->id.start = request->id_data;
	request->id.len = 0;
	request->id.type = SPDK_JSON_VAL_INVALID;
	request->send_offset = 0;
	request->send_len = 0;
	request->send_buf_size = SPDK_JSONRPC_SEND_BUF_SIZE_INIT;
	request->send_buf = malloc(request->send_buf_size);
	if (request->send_buf == NULL) {
		SPDK_ERRLOG("Failed to allocate send_buf (%zu bytes)\n", request->send_buf_size);
		free(request);
		return -1;
	}

	if (rc < 0 || rc > SPDK_JSONRPC_MAX_VALUES) {
		SPDK_DEBUGLOG(SPDK_LOG_RPC, "JSON parse error\n");
		spdk_jsonrpc_server_handle_error(request, SPDK_JSONRPC_ERROR_PARSE_ERROR);

		/*
		 * Can't recover from parse error (no guaranteed resync point in streaming JSON).
		 * Return an error to indicate that the connection should be closed.
		 */
		return -1;
	}

	/* Decode a second time now that there is a full JSON value available. */
	rc = spdk_json_parse(json, size, conn->values, SPDK_JSONRPC_MAX_VALUES, &end,
			     SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE);
	if (rc < 0 || rc > SPDK_JSONRPC_MAX_VALUES) {
		SPDK_DEBUGLOG(SPDK_LOG_RPC, "JSON parse error on second pass\n");
		spdk_jsonrpc_server_handle_error(request, SPDK_JSONRPC_ERROR_PARSE_ERROR);
		return -1;
	}

	assert(end != NULL);

	if (conn->values[0].type == SPDK_JSON_VAL_OBJECT_BEGIN) {
		parse_single_request(request, conn->values);
	} else if (conn->values[0].type == SPDK_JSON_VAL_ARRAY_BEGIN) {
		SPDK_DEBUGLOG(SPDK_LOG_RPC, "Got batch array (not currently supported)\n");
		spdk_jsonrpc_server_handle_error(request, SPDK_JSONRPC_ERROR_INVALID_REQUEST);
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_RPC, "top-level JSON value was not array or object\n");
		spdk_jsonrpc_server_handle_error(request, SPDK_JSONRPC_ERROR_INVALID_REQUEST);
	}

	return end - json;
}

static int
spdk_jsonrpc_server_write_cb(void *cb_ctx, const void *data, size_t size)
{
	struct spdk_jsonrpc_request *request = cb_ctx;
	size_t new_size = request->send_buf_size;

	while (new_size - request->send_len < size) {
		if (new_size >= SPDK_JSONRPC_SEND_BUF_SIZE_MAX) {
			SPDK_ERRLOG("Send buf exceeded maximum size (%zu)\n",
				    (size_t)SPDK_JSONRPC_SEND_BUF_SIZE_MAX);
			return -1;
		}

		new_size *= 2;
	}

	if (new_size != request->send_buf_size) {
		uint8_t *new_buf;

		new_buf = realloc(request->send_buf, new_size);
		if (new_buf == NULL) {
			SPDK_ERRLOG("Resizing send_buf failed (current size %zu, new size %zu)\n",
				    request->send_buf_size, new_size);
			return -1;
		}

		request->send_buf = new_buf;
		request->send_buf_size = new_size;
	}

	memcpy(request->send_buf + request->send_len, data, size);
	request->send_len += size;

	return 0;
}

static struct spdk_json_write_ctx *
begin_response(struct spdk_jsonrpc_request *request)
{
	struct spdk_json_write_ctx *w;

	w = spdk_json_write_begin(spdk_jsonrpc_server_write_cb, request, 0);
	if (w == NULL) {
		return NULL;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_name(w, "jsonrpc");
	spdk_json_write_string(w, "2.0");

	spdk_json_write_name(w, "id");
	spdk_json_write_val(w, &request->id);

	return w;
}

static void
end_response(struct spdk_jsonrpc_request *request, struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_end(w);
	spdk_json_write_end(w);
	spdk_jsonrpc_server_write_cb(request, "\n", 1);
	spdk_jsonrpc_server_send_response(request->conn, request);
}

void
spdk_jsonrpc_free_request(struct spdk_jsonrpc_request *request)
{
	request->conn->outstanding_requests--;
	free(request->send_buf);
	free(request);
}

struct spdk_json_write_ctx *
spdk_jsonrpc_begin_result(struct spdk_jsonrpc_request *request)
{
	struct spdk_json_write_ctx *w;

	if (request->id.type == SPDK_JSON_VAL_INVALID) {
		/* Notification - no response required */
		spdk_jsonrpc_free_request(request);
		return NULL;
	}

	w = begin_response(request);
	if (w == NULL) {
		spdk_jsonrpc_free_request(request);
		return NULL;
	}

	spdk_json_write_name(w, "result");

	return w;
}

void
spdk_jsonrpc_end_result(struct spdk_jsonrpc_request *request, struct spdk_json_write_ctx *w)
{
	assert(w != NULL);

	end_response(request, w);
}

void
spdk_jsonrpc_send_error_response(struct spdk_jsonrpc_request *request,
				 int error_code, const char *msg)
{
	struct spdk_json_write_ctx *w;

	if (request->id.type == SPDK_JSON_VAL_INVALID) {
		/* For error responses, if id is missing, explicitly respond with "id": null. */
		request->id.type = SPDK_JSON_VAL_NULL;
	}

	w = begin_response(request);
	if (w == NULL) {
		free(request);
		return;
	}

	spdk_json_write_name(w, "error");
	spdk_json_write_object_begin(w);
	spdk_json_write_name(w, "code");
	spdk_json_write_int32(w, error_code);
	spdk_json_write_name(w, "message");
	spdk_json_write_string(w, msg);
	spdk_json_write_object_end(w);

	end_response(request, w);
}

void
spdk_jsonrpc_send_error_response_fmt(struct spdk_jsonrpc_request *request,
				     int error_code, const char *fmt, ...)
{
	struct spdk_json_write_ctx *w;
	va_list args;

	if (request->id.type == SPDK_JSON_VAL_INVALID) {
		/* For error responses, if id is missing, explicitly respond with "id": null. */
		request->id.type = SPDK_JSON_VAL_NULL;
	}

	w = begin_response(request);
	if (w == NULL) {
		free(request);
		return;
	}

	spdk_json_write_name(w, "error");
	spdk_json_write_object_begin(w);
	spdk_json_write_name(w, "code");
	spdk_json_write_int32(w, error_code);
	spdk_json_write_name(w, "message");
	va_start(args, fmt);
	spdk_json_write_string_fmt_v(w, fmt, args);
	va_end(args);
	spdk_json_write_object_end(w);

	end_response(request, w);
}

SPDK_LOG_REGISTER_COMPONENT("rpc", SPDK_LOG_RPC)
