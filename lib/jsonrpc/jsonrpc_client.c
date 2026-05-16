/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *   All rights reserved.
 */

#include "spdk/util.h"
#include "jsonrpc_internal.h"

static int
capture_version(const struct spdk_json_val *val, void *out)
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
		return -EINVAL;
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
	{"jsonrpc", offsetof(struct spdk_jsonrpc_client_response, version), capture_version},
	{"id", offsetof(struct spdk_jsonrpc_client_response, id), capture_id, true},
	{"result", offsetof(struct spdk_jsonrpc_client_response, result), capture_any, true},
	{"error", offsetof(struct spdk_jsonrpc_client_response, error), capture_any, true},
};

/*
 * Note: This simplified handling is sufficient for the current use case (JSON
 * config loading) where we only need to know if the batch as a whole succeeded.
 * A more complete implementation would return all individual responses.
 */
struct batch_response_ctx {
	struct spdk_jsonrpc_client_response *out;
	bool found_error;
};

static int
decode_batch_response_element(const struct spdk_json_val *val, void *out)
{
	struct batch_response_ctx *ctx = out;
	struct spdk_jsonrpc_client_response temp_resp = {};

	if (spdk_json_decode_object(val, jsonrpc_response_decoders,
				    SPDK_COUNTOF(jsonrpc_response_decoders), &temp_resp)) {
		SPDK_ERRLOG("failed to decode batch response element\n");
		return -EINVAL;
	}

	/* If this response has an error and we haven't captured one yet, save it */
	if (temp_resp.error != NULL && !ctx->found_error) {
		ctx->out->error = temp_resp.error;
		ctx->out->id = temp_resp.id;
		ctx->found_error = true;
	} else if (!ctx->found_error && ctx->out->result == NULL) {
		/* Callers expect a non-NULL result on success, so keep the first one */
		ctx->out->result = temp_resp.result;
		ctx->out->id = temp_resp.id;
	}

	return 0;
}

int
jsonrpc_parse_response(struct spdk_jsonrpc_client *client)
{
	struct spdk_jsonrpc_client_response_internal *r;
	ssize_t rc;
	size_t buf_len;
	size_t values_cnt;
	void *end = NULL;


	/* Check to see if we have received a full JSON value. */
	rc = spdk_json_parse(client->recv_buf, client->recv_offset, NULL, 0, &end, 0);
	if (rc == SPDK_JSON_PARSE_INCOMPLETE) {
		return 0;
	}

	SPDK_DEBUGLOG(rpc_client, "JSON string is :\n%s\n", client->recv_buf);
	if (rc < 0 || rc > SPDK_JSONRPC_CLIENT_MAX_VALUES) {
		SPDK_ERRLOG("JSON parse error (rc: %zd)\n", rc);
		/*
		 * Can't recover from parse error (no guaranteed resync point in streaming JSON).
		 * Return an error to indicate that the connection should be closed.
		 */
		return -EINVAL;
	}

	values_cnt = rc;

	r = calloc(1, sizeof(*r) + sizeof(struct spdk_json_val) * (values_cnt + 1));
	if (!r) {
		return -errno;
	}

	if (client->resp) {
		free(r);
		return -ENOSPC;
	}

	client->resp = r;

	r->buf = client->recv_buf;
	buf_len = client->recv_offset;
	r->values_cnt = values_cnt;

	client->recv_buf_size = 0;
	client->recv_offset = 0;
	client->recv_buf = NULL;

	/* Decode a second time now that there is a full JSON value available. */
	rc = spdk_json_parse(r->buf, buf_len, r->values, values_cnt, &end,
			     SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE);
	if (rc != (ssize_t)values_cnt) {
		SPDK_ERRLOG("JSON parse error on second pass (rc: %zd, expected: %zu)\n", rc, values_cnt);
		goto err;
	}

	assert(end != NULL);

	if (r->values[0].type == SPDK_JSON_VAL_ARRAY_BEGIN) {
		/* Batch response - an array of response objects */
		struct batch_response_ctx ctx = { .out = &r->jsonrpc };
		size_t count;

		if (spdk_json_decode_array(r->values, decode_batch_response_element, &ctx,
					   SPDK_JSONRPC_MAX_VALUES, &count, 0)) {
			SPDK_ERRLOG("failed to decode batch response array\n");
			goto err;
		}

		r->ready = 1;
		return 1;
	} else if (r->values[0].type != SPDK_JSON_VAL_OBJECT_BEGIN) {
		SPDK_ERRLOG("top-level JSON value was not object or array\n");
		goto err;
	}

	if (spdk_json_decode_object(r->values, jsonrpc_response_decoders,
				    SPDK_COUNTOF(jsonrpc_response_decoders), &r->jsonrpc)) {
		goto err;
	}

	r->ready = 1;
	return 1;

err:
	client->resp = NULL;
	spdk_jsonrpc_client_free_response(&r->jsonrpc);
	return -EINVAL;
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

	w = request->batch_write_ctx;
	if (w == NULL) {
		/* Single request mode - create new write context */
		w = spdk_json_write_begin(jsonrpc_client_write_cb, request, 0);
		if (w == NULL) {
			return NULL;
		}
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "jsonrpc", "2.0");

	if (request->batch_write_ctx != NULL && id < 0) {
		/* Batch mode with auto-ID */
		spdk_json_write_named_uint32(w, "id", request->batch_id++);
	} else if (id >= 0) {
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

	if (request->batch_write_ctx == NULL) {
		/* Single request mode - finalize */
		spdk_json_write_end(w);
		jsonrpc_client_write_cb(request, "\n", 1);
	}
	/* In batch mode, just close the object - don't finalize yet */
}

int
spdk_jsonrpc_begin_batch(struct spdk_jsonrpc_client_request *request)
{
	struct spdk_json_write_ctx *w;
	int rc;

	w = spdk_json_write_begin(jsonrpc_client_write_cb, request, 0);
	if (w == NULL) {
		return -ENOMEM;
	}

	rc = spdk_json_write_array_begin(w);
	if (rc) {
		spdk_json_write_end(w);
		return rc;
	}

	request->batch_write_ctx = w;
	request->batch_id = 0;
	return 0;
}

void
spdk_jsonrpc_end_batch(struct spdk_jsonrpc_client_request *request)
{
	struct spdk_json_write_ctx *w = request->batch_write_ctx;

	assert(w != NULL);

	spdk_json_write_array_end(w);
	spdk_json_write_end(w);
	jsonrpc_client_write_cb(request, "\n", 1);
	request->batch_write_ctx = NULL;
}

SPDK_LOG_REGISTER_COMPONENT(rpc_client)
