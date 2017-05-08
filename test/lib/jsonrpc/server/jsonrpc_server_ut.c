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

#include "spdk/stdinc.h"

#include "spdk_cunit.h"

#include "jsonrpc_server.c"

#define MAX_PARAMS	100
#define MAX_REQS	100

struct req {
	int error;
	bool got_method;
	bool got_id;
	bool got_params;
	struct spdk_json_val method;
	struct spdk_json_val id;
	struct spdk_json_val params[MAX_PARAMS];
};

static uint8_t g_buf[1000];
static struct req g_reqs[MAX_REQS];
static struct req *g_cur_req;
static struct spdk_json_val *g_params;
static size_t g_num_reqs;

#define PARSE_PASS(in, trailing) \
	memcpy(g_buf, in, sizeof(in) - 1); \
	g_num_reqs = 0; \
	g_cur_req = NULL; \
	CU_ASSERT(spdk_jsonrpc_parse_request(conn, g_buf, sizeof(in) - 1) == sizeof(in) - sizeof(trailing))

#define PARSE_FAIL(in) \
	memcpy(g_buf, in, sizeof(in) - 1); \
	g_num_reqs = 0; \
	g_cur_req = 0; \
	CU_ASSERT(spdk_jsonrpc_parse_request(conn, g_buf, sizeof(in) - 1) < 0)


#define REQ_BEGIN(expected_error) \
	if (g_cur_req == NULL) { \
		g_cur_req = g_reqs; \
	} else { \
		g_cur_req++; \
	} \
	CU_ASSERT(g_cur_req - g_reqs <= (ptrdiff_t)g_num_reqs); \
	CU_ASSERT(g_cur_req->error == expected_error)

#define REQ_BEGIN_VALID() REQ_BEGIN(0)
#define REQ_BEGIN_INVALID(expected_error) REQ_BEGIN(expected_error)

#define REQ_METHOD(name) \
	CU_ASSERT(g_cur_req->got_method); \
	CU_ASSERT(spdk_json_strequal(&g_cur_req->method, name) == true)

#define REQ_METHOD_MISSING() \
	CU_ASSERT(g_cur_req->got_method == false)

#define REQ_ID_NUM(num) \
	CU_ASSERT(g_cur_req->got_id); \
	CU_ASSERT(g_cur_req->id.type == SPDK_JSON_VAL_NUMBER); \
	CU_ASSERT(memcmp(g_cur_req->id.start, num, sizeof(num) - 1) == 0)

#define REQ_ID_STRING(str) \
	CU_ASSERT(g_cur_req->got_id); \
	CU_ASSERT(g_cur_req->id.type == SPDK_JSON_VAL_STRING); \
	CU_ASSERT(memcmp(g_cur_req->id.start, str, sizeof(str) - 1) == 0)

#define REQ_ID_MISSING() \
	CU_ASSERT(g_cur_req->got_id == false)

#define REQ_PARAMS_MISSING() \
	CU_ASSERT(g_cur_req->got_params == false)

#define REQ_PARAMS_BEGIN() \
	CU_ASSERT(g_cur_req->got_params); \
	g_params = g_cur_req->params

#define PARAM_ARRAY_BEGIN() \
	CU_ASSERT(g_params->type == SPDK_JSON_VAL_ARRAY_BEGIN); \
	g_params++

#define PARAM_ARRAY_END() \
	CU_ASSERT(g_params->type == SPDK_JSON_VAL_ARRAY_END); \
	g_params++

#define PARAM_OBJECT_BEGIN() \
	CU_ASSERT(g_params->type == SPDK_JSON_VAL_OBJECT_BEGIN); \
	g_params++

#define PARAM_OBJECT_END() \
	CU_ASSERT(g_params->type == SPDK_JSON_VAL_OBJECT_END); \
	g_params++

#define PARAM_NUM(num) \
	CU_ASSERT(g_params->type == SPDK_JSON_VAL_NUMBER); \
	CU_ASSERT(g_params->len == sizeof(num) - 1); \
	CU_ASSERT(memcmp(g_params->start, num, g_params->len) == 0); \
	g_params++

#define PARAM_NAME(str) \
	CU_ASSERT(g_params->type == SPDK_JSON_VAL_NAME); \
	CU_ASSERT(g_params->len == sizeof(str) - 1); \
	CU_ASSERT(memcmp(g_params->start, str, g_params->len) == 0); \
	g_params++

#define PARAM_STRING(str) \
	CU_ASSERT(g_params->type == SPDK_JSON_VAL_STRING); \
	CU_ASSERT(g_params->len == sizeof(str) - 1); \
	CU_ASSERT(memcmp(g_params->start, str, g_params->len) == 0); \
	g_params++

static void
ut_handle(struct spdk_jsonrpc_server_conn *conn, int error, const struct spdk_json_val *method,
	  const struct spdk_json_val *params, const struct spdk_json_val *id)
{
	struct req *r;

	SPDK_CU_ASSERT_FATAL(g_num_reqs != MAX_REQS);
	r = &g_reqs[g_num_reqs++];

	r->error = error;

	if (method) {
		r->got_method = true;
		r->method = *method;
	} else {
		r->got_method = false;
	}

	if (params) {
		r->got_params = true;
		SPDK_CU_ASSERT_FATAL(spdk_json_val_len(params) < MAX_PARAMS);
		memcpy(r->params, params, spdk_json_val_len(params) * sizeof(struct spdk_json_val));
	} else {
		r->got_params = false;
	}

	if (id) {
		r->got_id = true;
		r->id = *id;
	} else {
		r->got_id = false;
	}
}

void
spdk_jsonrpc_server_handle_error(struct spdk_jsonrpc_server_conn *conn, int error,
				 const struct spdk_json_val *method, const struct spdk_json_val *params,
				 const struct spdk_json_val *id)
{
	ut_handle(conn, error, method, params,  id);
}

void
spdk_jsonrpc_server_handle_request(struct spdk_jsonrpc_server_conn *conn,
				   const struct spdk_json_val *method, const struct spdk_json_val *params,
				   const struct spdk_json_val *id)
{
	ut_handle(conn, 0, method, params, id);
}

int
spdk_jsonrpc_server_write_cb(void *cb_ctx, const void *data, size_t size)
{
	/* TODO */
	return -1;
}

static void
test_parse_request(void)
{
	struct spdk_jsonrpc_server *server;
	struct spdk_jsonrpc_server_conn *conn;

	server = calloc(1, sizeof(*server));
	SPDK_CU_ASSERT_FATAL(server != NULL);

	conn = calloc(1, sizeof(*conn));
	SPDK_CU_ASSERT_FATAL(conn != NULL);

	conn->server = server;

	/* rpc call with positional parameters */
	PARSE_PASS("{\"jsonrpc\":\"2.0\",\"method\":\"subtract\",\"params\":[42,23],\"id\":1}", "");
	REQ_BEGIN_VALID();
	REQ_METHOD("subtract");
	REQ_ID_NUM("1");
	REQ_PARAMS_BEGIN();
	PARAM_ARRAY_BEGIN();
	PARAM_NUM("42");
	PARAM_NUM("23");
	PARAM_ARRAY_END();

	/* rpc call with named parameters */
	PARSE_PASS("{\"jsonrpc\": \"2.0\", \"method\": \"subtract\", \"params\": {\"subtrahend\": 23, \"minuend\": 42}, \"id\": 3}",
		   "");
	REQ_BEGIN_VALID();
	REQ_METHOD("subtract");
	REQ_ID_NUM("3");
	REQ_PARAMS_BEGIN();
	PARAM_OBJECT_BEGIN();
	PARAM_NAME("subtrahend");
	PARAM_NUM("23");
	PARAM_NAME("minuend");
	PARAM_NUM("42");
	PARAM_OBJECT_END();

	/* notification */
	PARSE_PASS("{\"jsonrpc\": \"2.0\", \"method\": \"update\", \"params\": [1,2,3,4,5]}", "");
	REQ_BEGIN_VALID();
	REQ_METHOD("update");
	REQ_ID_MISSING();
	REQ_PARAMS_BEGIN();
	PARAM_ARRAY_BEGIN();
	PARAM_NUM("1");
	PARAM_NUM("2");
	PARAM_NUM("3");
	PARAM_NUM("4");
	PARAM_NUM("5");
	PARAM_ARRAY_END();

	/* invalid JSON */
	PARSE_FAIL("{\"jsonrpc\": \"2.0\", \"method\": \"foobar, \"params\": \"bar\", \"baz]");
	REQ_BEGIN_INVALID(SPDK_JSONRPC_ERROR_PARSE_ERROR);
	REQ_METHOD_MISSING();
	REQ_ID_MISSING();
	REQ_PARAMS_MISSING();

	/* invalid request (method must be a string; params must be array or object) */
	PARSE_PASS("{\"jsonrpc\": \"2.0\", \"method\": 1, \"params\": \"bar\"}", "");
	REQ_BEGIN_INVALID(SPDK_JSONRPC_ERROR_INVALID_REQUEST);
	REQ_METHOD_MISSING();
	REQ_ID_MISSING();
	REQ_PARAMS_MISSING();

	/* batch, invalid JSON */
	PARSE_FAIL(
		"["
		"{\"jsonrpc\": \"2.0\", \"method\": \"sum\", \"params\": [1,2,4], \"id\": \"1\"},"
		"{\"jsonrpc\": \"2.0\", \"method\""
		"]");
	REQ_BEGIN_INVALID(SPDK_JSONRPC_ERROR_PARSE_ERROR);
	REQ_METHOD_MISSING();
	REQ_ID_MISSING();
	REQ_PARAMS_MISSING();

	/* empty array */
	PARSE_PASS("[]", "");
	REQ_BEGIN_INVALID(SPDK_JSONRPC_ERROR_INVALID_REQUEST);
	REQ_METHOD_MISSING();
	REQ_ID_MISSING();
	REQ_PARAMS_MISSING();

	/* invalid batch */
	PARSE_PASS("[1]", "");
	REQ_BEGIN_INVALID(SPDK_JSONRPC_ERROR_INVALID_REQUEST);
	REQ_METHOD_MISSING();
	REQ_ID_MISSING();
	REQ_PARAMS_MISSING();

	/* invalid batch */
	PARSE_PASS("[1,2,3]", "");

	REQ_BEGIN_INVALID(SPDK_JSONRPC_ERROR_INVALID_REQUEST);
	REQ_METHOD_MISSING();
	REQ_ID_MISSING();
	REQ_PARAMS_MISSING();

	REQ_BEGIN_INVALID(SPDK_JSONRPC_ERROR_INVALID_REQUEST);
	REQ_METHOD_MISSING();
	REQ_ID_MISSING();
	REQ_PARAMS_MISSING();

	REQ_BEGIN_INVALID(SPDK_JSONRPC_ERROR_INVALID_REQUEST);
	REQ_METHOD_MISSING();
	REQ_ID_MISSING();
	REQ_PARAMS_MISSING();

	/* batch */
	PARSE_PASS(
		"["
		"{\"jsonrpc\": \"2.0\", \"method\": \"sum\", \"params\": [1,2,4], \"id\": \"1\"},"
		"{\"jsonrpc\": \"2.0\", \"method\": \"notify_hello\", \"params\": [7]},"
		"{\"jsonrpc\": \"2.0\", \"method\": \"subtract\", \"params\": [42,23], \"id\": \"2\"},"
		"{\"foo\": \"boo\"},"
		"{\"jsonrpc\": \"2.0\", \"method\": \"foo.get\", \"params\": {\"name\": \"myself\"}, \"id\": \"5\"},"
		"{\"jsonrpc\": \"2.0\", \"method\": \"get_data\", \"id\": \"9\"}"
		"]", "");

	REQ_BEGIN_VALID();
	REQ_METHOD("sum");
	REQ_ID_STRING("1");
	REQ_PARAMS_BEGIN();
	PARAM_ARRAY_BEGIN();
	PARAM_NUM("1");
	PARAM_NUM("2");
	PARAM_NUM("4");
	PARAM_ARRAY_END();

	REQ_BEGIN_VALID();
	REQ_METHOD("notify_hello");
	REQ_ID_MISSING();
	REQ_PARAMS_BEGIN();
	PARAM_ARRAY_BEGIN();
	PARAM_NUM("7");
	PARAM_ARRAY_END();

	REQ_BEGIN_VALID();
	REQ_METHOD("subtract");
	REQ_ID_STRING("2");
	REQ_PARAMS_BEGIN();
	PARAM_ARRAY_BEGIN();
	PARAM_NUM("42");
	PARAM_NUM("23");
	PARAM_ARRAY_END();

	REQ_BEGIN_INVALID(SPDK_JSONRPC_ERROR_INVALID_REQUEST);
	REQ_METHOD_MISSING();
	REQ_ID_MISSING();
	REQ_PARAMS_MISSING();

	REQ_BEGIN_VALID();
	REQ_METHOD("foo.get");
	REQ_ID_STRING("5");
	REQ_PARAMS_BEGIN();
	PARAM_OBJECT_BEGIN();
	PARAM_NAME("name");
	PARAM_STRING("myself");
	PARAM_OBJECT_END();

	REQ_BEGIN_VALID();
	REQ_METHOD("get_data");
	REQ_ID_STRING("9");
	REQ_PARAMS_MISSING();

	free(conn);
	free(server);
}

static void
test_parse_request_streaming(void)
{
	struct spdk_jsonrpc_server *server;
	struct spdk_jsonrpc_server_conn *conn;
	size_t len, i;

	server = calloc(1, sizeof(*server));
	SPDK_CU_ASSERT_FATAL(server != NULL);

	conn = calloc(1, sizeof(*conn));
	SPDK_CU_ASSERT_FATAL(conn != NULL);

	conn->server = server;

	/*
	 * Two valid requests end to end in the same buffer.
	 * Parse should return the first one and point to the beginning of the second one.
	 */
	PARSE_PASS(
		"{\"jsonrpc\":\"2.0\",\"method\":\"a\",\"params\":[1],\"id\":1}"
		"{\"jsonrpc\":\"2.0\",\"method\":\"b\",\"params\":[2],\"id\":2}",
		"{\"jsonrpc\":\"2.0\",\"method\":\"b\",\"params\":[2],\"id\":2}");
	REQ_BEGIN_VALID();
	REQ_METHOD("a");
	REQ_ID_NUM("1");
	REQ_PARAMS_BEGIN();
	PARAM_ARRAY_BEGIN();
	PARAM_NUM("1");
	PARAM_ARRAY_END();

	/* Partial (but not invalid) requests - parse should not consume anything. */
	strcpy(g_buf, "{\"jsonrpc\":\"2.0\",\"method\":\"b\",\"params\":[2],\"id\":2}");
	len = strlen(g_buf);

	/* Try every partial length up to the full request length */
	for (i = 0; i < len; i++) {
		int rc = spdk_jsonrpc_parse_request(conn, g_buf, i);
		/* Partial request - no data consumed */
		CU_ASSERT(rc == 0);
	}

	/* Verify that full request can be parsed successfully */
	CU_ASSERT(spdk_jsonrpc_parse_request(conn, g_buf, len) == (ssize_t)len);

	free(conn);
	free(server);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("jsonrpc", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "parse_request", test_parse_request) == NULL ||
		CU_add_test(suite, "parse_request_streaming", test_parse_request_streaming) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
