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
parse_single_request(struct spdk_jsonrpc_server_conn *conn, struct spdk_json_val *values)
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
		if (req.id->type != SPDK_JSON_VAL_STRING &&
		    req.id->type != SPDK_JSON_VAL_NUMBER &&
		    req.id->type != SPDK_JSON_VAL_NULL) {
			req.id = NULL;
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
		spdk_jsonrpc_server_handle_error(conn, SPDK_JSONRPC_ERROR_INVALID_REQUEST, req.method, req.params,
						 req.id);
	} else {
		spdk_jsonrpc_server_handle_request(conn, req.method, req.params, req.id);
	}
}

static void
parse_batch_request(struct spdk_jsonrpc_server_conn *conn, struct spdk_json_val *values)
{
	size_t num_values, i;

	assert(values[0].type == SPDK_JSON_VAL_ARRAY_BEGIN);
	num_values = values[0].len;
	values++;

	assert(conn->json_writer == NULL);

	if (num_values == 0) {
		SPDK_TRACELOG(SPDK_TRACE_RPC, "empty batch array not allowed");
		spdk_jsonrpc_server_handle_error(conn, SPDK_JSONRPC_ERROR_INVALID_REQUEST, NULL, NULL, NULL);
		return;
	}

	i = 0;
	while (i < num_values) {
		struct spdk_json_val *v = &values[i];

		parse_single_request(conn, v);
		i += spdk_json_val_len(v);
	}

	if (conn->json_writer) {
		/*
		 * There was at least one response - finish the batch array.
		 */
		spdk_json_write_array_end(conn->json_writer);
		spdk_json_write_end(conn->json_writer);
		conn->json_writer = NULL;
	}
}

int
spdk_jsonrpc_parse_request(struct spdk_jsonrpc_server_conn *conn, void *json, size_t size)
{
	ssize_t rc;
	void *end = NULL;

	assert(conn->json_writer == NULL);

	conn->batch = false;

	/* Check to see if we have received a full JSON value. */
	rc = spdk_json_parse(json, size, NULL, 0, &end, 0);
	if (rc == SPDK_JSON_PARSE_INCOMPLETE) {
		return 0;
	} else if (rc < 0 || rc > SPDK_JSONRPC_MAX_VALUES) {
		SPDK_TRACELOG(SPDK_TRACE_RPC, "JSON parse error\n");
		spdk_jsonrpc_server_handle_error(conn, SPDK_JSONRPC_ERROR_PARSE_ERROR, NULL, NULL, NULL);

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
		SPDK_TRACELOG(SPDK_TRACE_RPC, "JSON parse error on second pass\n");
		spdk_jsonrpc_server_handle_error(conn, SPDK_JSONRPC_ERROR_PARSE_ERROR, NULL, NULL, NULL);
		return -1;
	}

	assert(end != NULL);

	if (conn->values[0].type == SPDK_JSON_VAL_OBJECT_BEGIN) {
		parse_single_request(conn, conn->values);
	} else if (conn->values[0].type == SPDK_JSON_VAL_ARRAY_BEGIN) {
		conn->batch = true;
		parse_batch_request(conn, conn->values);
	} else {
		SPDK_TRACELOG(SPDK_TRACE_RPC, "top-level JSON value was not array or object\n");
		spdk_jsonrpc_server_handle_error(conn, SPDK_JSONRPC_ERROR_INVALID_REQUEST, NULL, NULL, NULL);
	}

	return end - json;
}

static struct spdk_json_write_ctx *
begin_response(struct spdk_jsonrpc_server_conn *conn, const struct spdk_json_val *id)
{
	struct spdk_json_write_ctx *w = conn->json_writer;

	if (w == NULL) {
		conn->json_writer = w = spdk_json_write_begin(spdk_jsonrpc_server_write_cb, conn, 0);
	}

	if (w == NULL) {
		return NULL;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_name(w, "jsonrpc");
	spdk_json_write_string(w, "2.0");

	if (id) {
		spdk_json_write_name(w, "id");
		spdk_json_write_val(w, id);
	}

	return w;
}

static void
end_response(struct spdk_jsonrpc_server_conn *conn, struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_end(w);

	if (!conn->batch) {
		spdk_json_write_end(w);
		spdk_jsonrpc_server_write_cb(conn, "\n", 1);
		conn->json_writer = NULL;
	}
}

struct spdk_json_write_ctx *
spdk_jsonrpc_begin_result(struct spdk_jsonrpc_server_conn *conn, const struct spdk_json_val *id)
{
	struct spdk_json_write_ctx *w;

	w = begin_response(conn, id);
	if (w == NULL) {
		return NULL;
	}

	spdk_json_write_name(w, "result");

	return w;
}

void
spdk_jsonrpc_end_result(struct spdk_jsonrpc_server_conn *conn, struct spdk_json_write_ctx *w)
{
	assert(w != NULL);
	assert(w == conn->json_writer);

	end_response(conn, w);
}

void
spdk_jsonrpc_send_error_response(struct spdk_jsonrpc_server_conn *conn,
				 const struct spdk_json_val *id,
				 int error_code, const char *msg)
{
	struct spdk_json_write_ctx *w;
	struct spdk_json_val v_null;

	if (id == NULL) {
		/* For error responses, if id is missing, explicitly respond with "id": null. */
		v_null.type = SPDK_JSON_VAL_NULL;
		id = &v_null;
	}

	w = begin_response(conn, id);
	if (w == NULL) {
		return;
	}

	spdk_json_write_name(w, "error");
	spdk_json_write_object_begin(w);
	spdk_json_write_name(w, "code");
	spdk_json_write_int32(w, error_code);
	spdk_json_write_name(w, "message");
	spdk_json_write_string(w, msg);
	spdk_json_write_object_end(w);

	end_response(conn, w);
}

SPDK_LOG_REGISTER_TRACE_FLAG("rpc", SPDK_TRACE_RPC)
