/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2023, 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "jsonrpc_internal.h"

#include "spdk/util.h"

static enum spdk_log_level g_rpc_log_level = SPDK_LOG_DISABLED;
static FILE *g_rpc_log_file = NULL;

struct jsonrpc_request {
	const struct spdk_json_val *version;
	const struct spdk_json_val *method;
	const struct spdk_json_val *params;
	const struct spdk_json_val *id;
};

void
spdk_jsonrpc_set_log_level(enum spdk_log_level level)
{
	assert(level >= SPDK_LOG_DISABLED);
	assert(level <= SPDK_LOG_DEBUG);
	g_rpc_log_level = level;
}

void
spdk_jsonrpc_set_log_file(FILE *file)
{
	g_rpc_log_file = file;
}

static void
remove_newlines(char *text)
{
	int i = 0, j = 0;

	while (text[i] != '\0') {
		if (text[i] != '\n') {
			text[j++] = text[i];
		}
		i++;
	}
	text[j] = '\0';
}

static void
jsonrpc_log(char *buf, const char *prefix)
{
	/* Some custom applications have enabled SPDK_JSON_PARSE_FLAG_ALLOW_COMMENTS
	 * to allow comments in JSON RPC objects. To keep backward compatibility of
	 * these applications, remove newlines only if JSON RPC logging is enabled.
	 */
	if (g_rpc_log_level != SPDK_LOG_DISABLED || g_rpc_log_file != NULL) {
		remove_newlines(buf);
	}

	if (g_rpc_log_level != SPDK_LOG_DISABLED) {
		spdk_log(g_rpc_log_level, NULL, 0, NULL, "%s%s\n", prefix, buf);
	}

	if (g_rpc_log_file != NULL) {
		spdk_flog(g_rpc_log_file, NULL, 0, NULL, "%s%s\n", prefix, buf);
	}
}

static int
capture_val(const struct spdk_json_val *val, void *out)
{
	const struct spdk_json_val **vptr = out;

	*vptr = val;
	return 0;
}

static const struct spdk_json_object_decoder jsonrpc_request_decoders[] = {
	{"jsonrpc", offsetof(struct jsonrpc_request, version), capture_val, true},
	{"method", offsetof(struct jsonrpc_request, method), capture_val},
	{"params", offsetof(struct jsonrpc_request, params), capture_val, true},
	{"id", offsetof(struct jsonrpc_request, id), capture_val, true},
};

static void
parse_single_request(struct spdk_jsonrpc_request *request, const struct spdk_json_val *values)
{
	struct jsonrpc_request req = {};
	const struct spdk_json_val *params = NULL;

	if (spdk_json_decode_object(values, jsonrpc_request_decoders,
				    SPDK_COUNTOF(jsonrpc_request_decoders),
				    &req)) {
		goto invalid;
	}

	if (req.version && (req.version->type != SPDK_JSON_VAL_STRING ||
			    !spdk_json_strequal(req.version, "2.0"))) {
		goto invalid;
	}

	if (!req.method || req.method->type != SPDK_JSON_VAL_STRING) {
		goto invalid;
	}

	if (req.id) {
		if (req.id->type == SPDK_JSON_VAL_STRING ||
		    req.id->type == SPDK_JSON_VAL_NUMBER ||
		    req.id->type == SPDK_JSON_VAL_NULL) {
			request->id = req.id;
		} else  {
			goto invalid;
		}
	}

	if (req.params) {
		/* null json value is as if there were no parameters */
		if (req.params->type != SPDK_JSON_VAL_NULL) {
			if (req.params->type != SPDK_JSON_VAL_ARRAY_BEGIN &&
			    req.params->type != SPDK_JSON_VAL_OBJECT_BEGIN) {
				goto invalid;
			}
			params = req.params;
		}
	}

	jsonrpc_server_handle_request(request, req.method, params);
	return;

invalid:
	jsonrpc_server_handle_error(request, SPDK_JSONRPC_ERROR_INVALID_REQUEST);
}

static int
jsonrpc_grow_send_buf(uint8_t **buf, size_t *buf_size, size_t current_len, size_t required_len)
{
	size_t new_size = *buf_size;
	uint8_t *new_buf;

	while (new_size - current_len < required_len) {
		if (new_size >= SPDK_JSONRPC_SEND_BUF_SIZE_MAX) {
			SPDK_ERRLOG("Send buf exceeded maximum size (%zu)\n",
				    (size_t)SPDK_JSONRPC_SEND_BUF_SIZE_MAX);
			return -1;
		}
		new_size *= 2;
	}

	if (new_size != *buf_size) {
		/* Add extra byte for the null terminator. */
		new_buf = realloc(*buf, new_size + 1);
		if (new_buf == NULL) {
			SPDK_ERRLOG("Failed to grow send buffer (current size %zu, new size %zu)\n",
				    *buf_size, new_size);
			return -1;
		}
		*buf = new_buf;
		*buf_size = new_size;
	}

	return 0;
}

static int
jsonrpc_server_write_cb(void *cb_ctx, const void *data, size_t size)
{
	struct spdk_jsonrpc_request *request = cb_ctx;

	if (jsonrpc_grow_send_buf(&request->send_buf, &request->send_buf_size,
				  request->send_len, size) != 0) {
		return -1;
	}

	memcpy(request->send_buf + request->send_len, data, size);
	request->send_len += size;

	return 0;
}

static struct spdk_jsonrpc_request *
jsonrpc_alloc_request(struct spdk_jsonrpc_server_conn *conn)
{
	struct spdk_jsonrpc_request *request;

	request = calloc(1, sizeof(*request));
	if (request == NULL) {
		return NULL;
	}

	request->conn = conn;

	pthread_spin_lock(&conn->queue_lock);
	conn->outstanding_requests++;
	STAILQ_INSERT_TAIL(&conn->outstanding_queue, request, link);
	pthread_spin_unlock(&conn->queue_lock);

	request->send_buf_size = SPDK_JSONRPC_SEND_BUF_SIZE_INIT;
	/* Add extra byte for the null terminator. */
	request->send_buf = malloc(request->send_buf_size + 1);
	if (request->send_buf == NULL) {
		jsonrpc_free_request(request);
		return NULL;
	}

	request->response = spdk_json_write_begin(jsonrpc_server_write_cb, request, 0);
	if (request->response == NULL) {
		jsonrpc_free_request(request);
		return NULL;
	}

	return request;
}

void
jsonrpc_free_batch(struct spdk_jsonrpc_batch_request *batch)
{
	if (batch == NULL) {
		return;
	}

	pthread_spin_destroy(&batch->lock);
	free(batch->recv_buffer);
	free(batch->values);
	free(batch->send_buf);
	free(batch);
}

static struct spdk_jsonrpc_batch_request *
jsonrpc_alloc_batch(struct spdk_jsonrpc_server_conn *conn, uint32_t count)
{
	struct spdk_jsonrpc_batch_request *batch;

	batch = calloc(1, sizeof(*batch));
	if (batch == NULL) {
		return NULL;
	}

	batch->conn = conn;
	batch->count = count;

	if (pthread_spin_init(&batch->lock, PTHREAD_PROCESS_PRIVATE)) {
		free(batch);
		return NULL;
	}

	batch->send_buf_size = SPDK_JSONRPC_SEND_BUF_SIZE_INIT;
	/* Add extra byte for the null terminator. */
	batch->send_buf = malloc(batch->send_buf_size + 1);
	if (batch->send_buf == NULL) {
		jsonrpc_free_batch(batch);
		return NULL;
	}

	/* Start the response array with '[' */
	batch->send_buf[0] = '[';
	batch->send_len = 1;

	return batch;
}

static int
jsonrpc_batch_append_response(struct spdk_jsonrpc_batch_request *batch,
			      const uint8_t *response, size_t response_len)
{
	size_t needed;

	/* Skip empty responses (notifications) */
	if (response_len == 0) {
		return 0;
	}

	/* Strip trailing newlines from response if present */
	while (response_len > 0 && response[response_len - 1] == '\n') {
		response_len--;
	}

	if (response_len == 0) {
		return 0;
	}

	/* Calculate space needed: comma (if not first) + response + 2 for closing ']' and '\n' */
	needed = response_len + 2;
	if (batch->num_responses > 0) {
		needed += 1; /* comma separator */
	}

	if (jsonrpc_grow_send_buf(&batch->send_buf, &batch->send_buf_size,
				  batch->send_len, needed) != 0) {
		return -1;
	}

	/* Add comma separator if not first response */
	if (batch->num_responses > 0) {
		batch->send_buf[batch->send_len++] = ',';
	}

	/* Append the response */
	memcpy(batch->send_buf + batch->send_len, response, response_len);
	batch->send_len += response_len;
	assert(batch->num_responses < batch->count);
	batch->num_responses++;

	return 0;
}

static void
jsonrpc_batch_finalize_and_send(struct spdk_jsonrpc_batch_request *batch)
{
	struct spdk_jsonrpc_server_conn *conn = batch->conn;
	struct spdk_jsonrpc_request *send_request;

	/* If no responses were collected (all notifications), don't send anything */
	if (batch->num_responses == 0) {
		SPDK_DEBUGLOG(rpc, "Batch contained only notifications, no response sent\n");
		jsonrpc_free_batch(batch);
		return;
	}

	/* Close the JSON array and add newline */
	if (batch->send_len + 2 > batch->send_buf_size) {
		SPDK_ERRLOG("Batch send buffer too small for closing bracket\n");
		jsonrpc_free_batch(batch);
		return;
	}
	batch->send_buf[batch->send_len++] = ']';
	batch->send_buf[batch->send_len++] = '\n';
	batch->send_buf[batch->send_len] = '\0';

	jsonrpc_log((char *)batch->send_buf, "batch response: ");

	if (conn == NULL) {
		SPDK_WARNLOG("Unable to send batch response: connection closed.\n");
		jsonrpc_free_batch(batch);
		return;
	}

	/*
	 * Create a pseudo-request to send the batch response.
	 * This request holds the aggregated batch response and will be
	 * queued for sending like a normal response.
	 */
	send_request = calloc(1, sizeof(*send_request));
	if (send_request == NULL) {
		SPDK_ERRLOG("Failed to allocate batch response request\n");
		jsonrpc_free_batch(batch);
		return;
	}

	send_request->conn = conn;
	send_request->send_buf = batch->send_buf;
	send_request->send_buf_size = batch->send_buf_size;
	send_request->send_len = batch->send_len;

	/* Transfer ownership of send_buf to send_request */
	batch->send_buf = NULL;

	/* Queue the batch response for sending */
	pthread_spin_lock(&conn->queue_lock);
	conn->outstanding_requests++;
	STAILQ_INSERT_TAIL(&conn->send_queue, send_request, link);
	pthread_spin_unlock(&conn->queue_lock);

	jsonrpc_free_batch(batch);
}

static void
jsonrpc_complete_batch(struct spdk_jsonrpc_batch_request *batch)
{
	bool is_last;

	pthread_spin_lock(&batch->lock);
	assert(batch->completed < batch->count);
	batch->completed++;
	is_last = (batch->completed == batch->count);
	pthread_spin_unlock(&batch->lock);

	if (is_last) {
		jsonrpc_batch_finalize_and_send(batch);
	}
}

void
jsonrpc_complete_batched_request(struct spdk_jsonrpc_request *request)
{
	struct spdk_jsonrpc_batch_request *batch = request->batch;
	int rc;

	assert(batch != NULL);

	pthread_spin_lock(&batch->lock);

	rc = jsonrpc_batch_append_response(batch, request->send_buf, request->send_len);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to append response to batch\n");
	}

	pthread_spin_unlock(&batch->lock);

	jsonrpc_complete_batch(batch);
	jsonrpc_free_request(request);
}

static struct spdk_jsonrpc_request *
jsonrpc_alloc_request_for_batch(struct spdk_jsonrpc_server_conn *conn,
				struct spdk_jsonrpc_batch_request *batch)
{
	struct spdk_jsonrpc_request *request;

	request = jsonrpc_alloc_request(conn);
	if (request == NULL) {
		return NULL;
	}

	request->batch = batch;
	return request;
}

static int
decode_batch_element(const struct spdk_json_val *val, void *out)
{
	struct spdk_jsonrpc_batch_request *batch = out;
	struct spdk_jsonrpc_server_conn *conn = batch->conn;
	struct spdk_jsonrpc_request *request;

	request = jsonrpc_alloc_request_for_batch(conn, batch);
	if (request == NULL) {
		SPDK_ERRLOG("Failed to allocate request for batch item\n");
		/* Mark this as completed with no response */
		jsonrpc_complete_batch(batch);
		return 0;
	}

	parse_single_request(request, val);
	return 0;
}

static int
jsonrpc_process_batch_array(struct spdk_jsonrpc_request *request)
{
	struct spdk_jsonrpc_server_conn *conn = request->conn;
	uint8_t *recv_buffer = request->recv_buffer;
	struct spdk_json_val *values = request->values;
	size_t values_cnt = request->values_cnt;
	struct spdk_jsonrpc_batch_request *batch;
	struct spdk_jsonrpc_request *batch_request;
	uint32_t batch_size;
	size_t count;
	int rc;

	assert(values[0].type == SPDK_JSON_VAL_ARRAY_BEGIN);

	/*
	 * Take ownership of recv_buffer and values from the original request,
	 * then free it. Batch processing creates its own individual requests.
	 */
	request->recv_buffer = NULL;
	request->values = NULL;
	spdk_json_write_end(request->response);
	request->response = NULL;
	jsonrpc_free_request(request);

	batch_size = spdk_json_array_count(values);

	/* Empty array is an invalid request per JSON-RPC spec */
	if (batch_size == 0) {
		batch_request = jsonrpc_alloc_request(conn);
		if (batch_request == NULL) {
			free(recv_buffer);
			free(values);
			return -1;
		}

		batch_request->recv_buffer = recv_buffer;
		batch_request->values = values;
		batch_request->values_cnt = values_cnt;

		jsonrpc_server_handle_error(batch_request, SPDK_JSONRPC_ERROR_INVALID_REQUEST);
		return 0;
	}

	/*
	 * Allocate batch with count + 1 to prevent premature finalization.
	 * The extra count is consumed by the final jsonrpc_complete_batch()
	 * call after all elements have been decoded.
	 */
	batch = jsonrpc_alloc_batch(conn, batch_size + 1);
	if (batch == NULL) {
		SPDK_ERRLOG("Failed to allocate batch context\n");
		free(recv_buffer);
		free(values);
		return -1;
	}

	/* Store recv_buffer and values in batch for later cleanup */
	batch->recv_buffer = recv_buffer;
	batch->values = values;
	batch->values_cnt = values_cnt;

	/*
	 * Process each request in the batch using spdk_json_decode_array().
	 * The decode_batch_element callback handles each element, including
	 * non-object elements which will result in error responses.
	 */
	rc = spdk_json_decode_array(values, decode_batch_element, batch, batch_size, &count, 0);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to decode batch array\n");
		jsonrpc_free_batch(batch);
		return rc;
	}

	/*
	 * Consume the extra count we added above. If all requests completed
	 * synchronously during decode, this will trigger finalization.
	 */
	jsonrpc_complete_batch(batch);

	return 0;
}

int
jsonrpc_parse_request(struct spdk_jsonrpc_server_conn *conn, const void *json, size_t size)
{
	struct spdk_jsonrpc_request *request;
	ssize_t rc;
	size_t len;
	void *end = NULL;

	/* Check to see if we have received a full JSON value. It is safe to cast away const
	 * as we don't decode in place. */
	rc = spdk_json_parse((void *)json, size, NULL, 0, &end, 0);
	if (rc == SPDK_JSON_PARSE_INCOMPLETE) {
		return 0;
	}

	request = jsonrpc_alloc_request(conn);
	if (request == NULL) {
		SPDK_DEBUGLOG(rpc, "Out of memory allocating request\n");
		return -1;
	}

	len = end - json;
	request->recv_buffer = malloc(len + 1);
	if (request->recv_buffer == NULL) {
		SPDK_ERRLOG("Failed to allocate buffer to copy request (%zu bytes)\n", len + 1);
		jsonrpc_free_request(request);
		return -1;
	}

	memcpy(request->recv_buffer, json, len);
	request->recv_buffer[len] = '\0';

	jsonrpc_log(request->recv_buffer, "request: ");

	if (rc > 0 && rc <= SPDK_JSONRPC_MAX_VALUES) {
		request->values_cnt = rc;
		request->values = malloc(request->values_cnt * sizeof(request->values[0]));
		if (request->values == NULL) {
			SPDK_ERRLOG("Failed to allocate buffer for JSON values (%zu bytes)\n",
				    request->values_cnt * sizeof(request->values[0]));
			jsonrpc_free_request(request);
			return -1;
		}
	}

	if (rc <= 0 || rc > SPDK_JSONRPC_MAX_VALUES) {
		SPDK_DEBUGLOG(rpc, "JSON parse error\n");
		jsonrpc_server_handle_error(request, SPDK_JSONRPC_ERROR_PARSE_ERROR);

		/*
		 * Can't recover from parse error (no guaranteed resync point in streaming JSON).
		 * Return an error to indicate that the connection should be closed.
		 */
		return -1;
	}

	/* Decode a second time now that there is a full JSON value available. */
	rc = spdk_json_parse(request->recv_buffer, size, request->values, request->values_cnt, &end,
			     SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE);
	if (rc < 0 || rc > SPDK_JSONRPC_MAX_VALUES) {
		SPDK_DEBUGLOG(rpc, "JSON parse error on second pass\n");
		jsonrpc_server_handle_error(request, SPDK_JSONRPC_ERROR_PARSE_ERROR);
		return -1;
	}

	assert(end != NULL);

	if (request->values[0].type == SPDK_JSON_VAL_OBJECT_BEGIN) {
		parse_single_request(request, request->values);
	} else if (request->values[0].type == SPDK_JSON_VAL_ARRAY_BEGIN) {
		/* Batch request - handle according to JSON-RPC 2.0 spec */
		if (jsonrpc_process_batch_array(request) != 0) {
			return -1;
		}
	} else {
		SPDK_DEBUGLOG(rpc, "top-level JSON value was not array or object\n");
		jsonrpc_server_handle_error(request, SPDK_JSONRPC_ERROR_INVALID_REQUEST);
	}

	return len;
}

struct spdk_jsonrpc_server_conn *
spdk_jsonrpc_get_conn(struct spdk_jsonrpc_request *request)
{
	return request->conn;
}

/* Never return NULL */
static struct spdk_json_write_ctx *
begin_response(struct spdk_jsonrpc_request *request)
{
	struct spdk_json_write_ctx *w = request->response;

	/* The assertion below ensures that no response data has been written yet.
	 * Otherwise, it would result in malformed JSON.
	 */
	assert(request->send_len == 0);

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "jsonrpc", "2.0");

	spdk_json_write_name(w, "id");
	if (request->id) {
		spdk_json_write_val(w, request->id);
	} else {
		spdk_json_write_null(w);
	}

	return w;
}

static void
skip_response(struct spdk_jsonrpc_request *request)
{
	spdk_json_write_end(request->response);
	request->response = NULL;
	request->send_len = 0;

	if (request->batch != NULL) {
		jsonrpc_complete_batched_request(request);
	} else {
		jsonrpc_server_send_response(request);
	}
}

static void
end_response(struct spdk_jsonrpc_request *request)
{
	spdk_json_write_object_end(request->response);
	spdk_json_write_end(request->response);
	request->response = NULL;

	if (request->batch != NULL) {
		jsonrpc_complete_batched_request(request);
	} else {
		jsonrpc_server_write_cb(request, "\n", 1);
		jsonrpc_server_send_response(request);
	}
}

void
jsonrpc_free_request(struct spdk_jsonrpc_request *request)
{
	struct spdk_jsonrpc_request *req;
	struct spdk_jsonrpc_server_conn *conn;

	if (!request) {
		return;
	}

	/* We must send or skip response explicitly */
	assert(request->response == NULL);

	conn = request->conn;
	if (conn != NULL) {
		pthread_spin_lock(&conn->queue_lock);
		conn->outstanding_requests--;
		STAILQ_FOREACH(req, &conn->outstanding_queue, link) {
			if (req == request) {
				STAILQ_REMOVE(&conn->outstanding_queue,
					      req, spdk_jsonrpc_request, link);
				break;
			}
		}
		pthread_spin_unlock(&conn->queue_lock);
	}
	free(request->recv_buffer);
	free(request->values);
	free(request->send_buf);
	free(request);
}

void
jsonrpc_complete_request(struct spdk_jsonrpc_request *request)
{
	jsonrpc_log(request->send_buf, "response: ");

	jsonrpc_free_request(request);
}

struct spdk_json_write_ctx *
spdk_jsonrpc_begin_result(struct spdk_jsonrpc_request *request)
{
	struct spdk_json_write_ctx *w = begin_response(request);

	spdk_json_write_name(w, "result");
	return w;
}

void
spdk_jsonrpc_end_result(struct spdk_jsonrpc_request *request, struct spdk_json_write_ctx *w)
{
	assert(w != NULL);
	assert(w == request->response);

	/* If there was no ID in request we skip response. */
	if (request->id && request->id->type != SPDK_JSON_VAL_NULL) {
		end_response(request);
	} else {
		skip_response(request);
	}
}

void
spdk_jsonrpc_send_bool_response(struct spdk_jsonrpc_request *request, bool value)
{
	struct spdk_json_write_ctx *w;

	w = spdk_jsonrpc_begin_result(request);
	assert(w != NULL);
	spdk_json_write_bool(w, value);
	spdk_jsonrpc_end_result(request, w);
}

static void
jsonrpc_reset_response(struct spdk_jsonrpc_request *request)
{
	spdk_json_write_reset(request->response);
	request->send_len = 0; /* to skip all previous data previously written by jsonrpc_server_write_cb */
}

void
spdk_jsonrpc_send_error_response(struct spdk_jsonrpc_request *request,
				 int error_code, const char *msg)
{
	struct spdk_json_write_ctx *w;

	jsonrpc_reset_response(request);

	w = begin_response(request);

	spdk_json_write_named_object_begin(w, "error");
	spdk_json_write_named_int32(w, "code", error_code);
	spdk_json_write_named_string(w, "message", msg);
	spdk_json_write_object_end(w);

	end_response(request);
}

void
spdk_jsonrpc_send_error_response_fmt(struct spdk_jsonrpc_request *request,
				     int error_code, const char *fmt, ...)
{
	struct spdk_json_write_ctx *w;
	va_list args;

	jsonrpc_reset_response(request);

	w = begin_response(request);

	spdk_json_write_named_object_begin(w, "error");
	spdk_json_write_named_int32(w, "code", error_code);
	va_start(args, fmt);
	spdk_json_write_named_string_fmt_v(w, "message", fmt, args);
	va_end(args);
	spdk_json_write_object_end(w);

	end_response(request);
}

SPDK_LOG_REGISTER_COMPONENT(rpc)
