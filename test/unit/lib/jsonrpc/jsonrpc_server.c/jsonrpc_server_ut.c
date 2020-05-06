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

#include "jsonrpc/jsonrpc_server.c"

static struct spdk_jsonrpc_request *g_request;
static int g_parse_error;
const struct spdk_json_val *g_method;
const struct spdk_json_val *g_params;

const struct spdk_json_val *g_cur_param;

#define PARSE_PASS(in, trailing) \
	CU_ASSERT(g_cur_param == NULL); \
	g_cur_param = NULL; \
	CU_ASSERT(jsonrpc_parse_request(conn, in, sizeof(in) - 1) == sizeof(in) - sizeof(trailing))

#define REQ_BEGIN(expected_error) \
	if (expected_error != 0 ) { \
		CU_ASSERT(g_parse_error == expected_error); \
		CU_ASSERT(g_params == NULL); \
	}

#define PARSE_FAIL(in) \
	CU_ASSERT(jsonrpc_parse_request(conn, in, sizeof(in) - 1) < 0);

#define REQ_BEGIN_VALID() \
		REQ_BEGIN(0); \
		SPDK_CU_ASSERT_FATAL(g_params != NULL);

#define REQ_BEGIN_INVALID(expected_error) \
	REQ_BEGIN(expected_error); \
	REQ_METHOD_MISSING(); \
	REQ_ID_MISSING(); \
	REQ_PARAMS_MISSING()


#define REQ_METHOD(name) \
	CU_ASSERT(g_method && spdk_json_strequal(g_method, name) == true)

#define REQ_METHOD_MISSING() \
	CU_ASSERT(g_method == NULL)

#define REQ_ID_NUM(num) \
	CU_ASSERT(g_request->id && g_request->id->type == SPDK_JSON_VAL_NUMBER); \
	CU_ASSERT(g_request->id && memcmp(g_request->id->start, num, sizeof(num) - 1) == 0)


#define REQ_ID_STRING(str) \
	CU_ASSERT(g_request->id && g_request->id->type == SPDK_JSON_VAL_STRING); \
	CU_ASSERT(g_request->id && memcmp(g_request->id->start, num, strlen(num) - 1) == 0))

#define REQ_ID_NULL() \
	CU_ASSERT(g_request->id && g_request->id->type == SPDK_JSON_VAL_NULL)

#define REQ_ID_MISSING() \
	CU_ASSERT(g_request->id == NULL)

#define REQ_PARAMS_MISSING() \
	CU_ASSERT(g_params == NULL)

#define REQ_PARAMS_BEGIN() \
	SPDK_CU_ASSERT_FATAL(g_params != NULL); \
	CU_ASSERT(g_cur_param == NULL); \
	g_cur_param = g_params

#define PARAM_ARRAY_BEGIN() \
	CU_ASSERT(g_cur_param->type == SPDK_JSON_VAL_ARRAY_BEGIN); \
	g_cur_param++

#define PARAM_ARRAY_END() \
	CU_ASSERT(g_cur_param->type == SPDK_JSON_VAL_ARRAY_END); \
	g_cur_param++

#define PARAM_OBJECT_BEGIN() \
	CU_ASSERT(g_cur_param->type == SPDK_JSON_VAL_OBJECT_BEGIN); \
	g_cur_param++

#define PARAM_OBJECT_END() \
	CU_ASSERT(g_cur_param->type == SPDK_JSON_VAL_OBJECT_END); \
	g_cur_param++

#define PARAM_NUM(num) \
	CU_ASSERT(g_cur_param->type == SPDK_JSON_VAL_NUMBER); \
	CU_ASSERT(g_cur_param->len == sizeof(num) - 1); \
	CU_ASSERT(memcmp(g_cur_param->start, num, sizeof(num) - 1) == 0); \
	g_cur_param++

#define PARAM_NAME(str) \
	CU_ASSERT(g_cur_param->type == SPDK_JSON_VAL_NAME); \
	CU_ASSERT(g_cur_param->len == sizeof(str) - 1); \
	CU_ASSERT(g_cur_param && memcmp(g_cur_param->start, str, sizeof(str) - 1) == 0); \
	g_cur_param++

#define PARAM_STRING(str) \
	CU_ASSERT(g_cur_param->type == SPDK_JSON_VAL_STRING); \
	CU_ASSERT(g_cur_param->len == sizeof(str) - 1); \
	CU_ASSERT(memcmp(g_cur_param->start, str, g_params->len) == 0); \
	g_cur_param++

#define FREE_REQUEST() \
	ut_jsonrpc_free_request(g_request, g_parse_error); \
	g_request = NULL; \
	g_cur_param = NULL; \
	g_parse_error = 0; \
	g_method = NULL; \
	g_cur_param = g_params = NULL

static void
ut_jsonrpc_free_request(struct spdk_jsonrpc_request *request, int err)
{
	struct spdk_json_write_ctx *w;

	if (!request) {
		return;
	}

	/* Need to emulate response to get the response write contex free */
	if (err == 0) {
		w = spdk_jsonrpc_begin_result(request);
		spdk_json_write_string(w, "UT PASS response");
		spdk_jsonrpc_end_result(request, w);
	} else {
		spdk_jsonrpc_send_error_response_fmt(request, err, "UT error response");
	}

	jsonrpc_free_request(request);
}

static void
ut_handle(struct spdk_jsonrpc_request *request, int error, const struct spdk_json_val *method,
	  const struct spdk_json_val *params)
{
	CU_ASSERT(g_request == NULL);
	g_request = request;
	g_parse_error = error;
	g_method = method;
	g_params = params;
}

void
jsonrpc_server_handle_error(struct spdk_jsonrpc_request *request, int error)
{
	ut_handle(request, error, NULL, NULL);
}

void
jsonrpc_server_handle_request(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *method, const struct spdk_json_val *params)
{
	ut_handle(request, 0, method, params);
}

void
jsonrpc_server_send_response(struct spdk_jsonrpc_request *request)
{
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

	/* rpc call with no parameters. */
	PARSE_PASS("{   }", "");
	REQ_BEGIN_INVALID(SPDK_JSONRPC_ERROR_INVALID_REQUEST);
	FREE_REQUEST();

	/* rpc call with method that is not a string. */
	PARSE_PASS("{\"jsonrpc\":\"2.0\", \"method\": null  }", "");
	REQ_BEGIN_INVALID(SPDK_JSONRPC_ERROR_INVALID_REQUEST);
	FREE_REQUEST();

	/* rpc call with invalid JSON RPC version. */
	PARSE_PASS("{\"jsonrpc\":\"42\", \"method\": \"subtract\"}", "");
	REQ_BEGIN_INVALID(SPDK_JSONRPC_ERROR_INVALID_REQUEST);
	FREE_REQUEST();

	/* rpc call with embedded zeros. */
	PARSE_FAIL("{\"jsonrpc\":\"2.0\",\"method\":\"foo\",\"params\":{\"bar\": \"\0\0baz\"}}");
	REQ_BEGIN_INVALID(SPDK_JSONRPC_ERROR_PARSE_ERROR);
	FREE_REQUEST();

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
	FREE_REQUEST();

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
	FREE_REQUEST();

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
	FREE_REQUEST();

	/* notification with explicit NULL ID. This is discouraged by JSON RPC spec but allowed. */
	PARSE_PASS("{\"jsonrpc\": \"2.0\", \"method\": \"update\", \"params\": [1,2,3,4,5], \"id\": null}",
		   "");
	REQ_BEGIN_VALID();
	REQ_METHOD("update");
	REQ_ID_NULL();
	REQ_PARAMS_BEGIN();
	PARAM_ARRAY_BEGIN();
	PARAM_NUM("1");
	PARAM_NUM("2");
	PARAM_NUM("3");
	PARAM_NUM("4");
	PARAM_NUM("5");
	PARAM_ARRAY_END();
	FREE_REQUEST();

	/* invalid JSON */
	PARSE_FAIL("{\"jsonrpc\": \"2.0\", \"method\": \"foobar, \"params\": \"bar\", \"baz]");
	REQ_BEGIN_INVALID(SPDK_JSONRPC_ERROR_PARSE_ERROR);
	FREE_REQUEST();

	/* invalid request (method must be a string; params must be array or object) */
	PARSE_PASS("{\"jsonrpc\": \"2.0\", \"method\": 1, \"params\": \"bar\"}", "");
	REQ_BEGIN_INVALID(SPDK_JSONRPC_ERROR_INVALID_REQUEST);
	FREE_REQUEST();

	/* batch, invalid JSON */
	PARSE_FAIL(
		"["
		"{\"jsonrpc\": \"2.0\", \"method\": \"sum\", \"params\": [1,2,4], \"id\": \"1\"},"
		"{\"jsonrpc\": \"2.0\", \"method\""
		"]");
	REQ_BEGIN_INVALID(SPDK_JSONRPC_ERROR_PARSE_ERROR);
	FREE_REQUEST();

	/* empty array */
	PARSE_PASS("[]", "");
	REQ_BEGIN_INVALID(SPDK_JSONRPC_ERROR_INVALID_REQUEST);
	FREE_REQUEST();

	/* batch - not supported */
	PARSE_PASS(
		"["
		"{\"jsonrpc\": \"2.0\", \"method\": \"sum\", \"params\": [1,2,4], \"id\": \"1\"},"
		"{\"jsonrpc\": \"2.0\", \"method\": \"notify_hello\", \"params\": [7]},"
		"{\"jsonrpc\": \"2.0\", \"method\": \"subtract\", \"params\": [42,23], \"id\": \"2\"},"
		"{\"foo\": \"boo\"},"
		"{\"jsonrpc\": \"2.0\", \"method\": \"foo.get\", \"params\": {\"name\": \"myself\"}, \"id\": \"5\"},"
		"{\"jsonrpc\": \"2.0\", \"method\": \"get_data\", \"id\": \"9\"}"
		"]", "");

	REQ_BEGIN_INVALID(SPDK_JSONRPC_ERROR_INVALID_REQUEST);
	FREE_REQUEST();

	CU_ASSERT(conn->outstanding_requests == 0);
	free(conn);
	free(server);
}

static void
test_parse_request_streaming(void)
{
	struct spdk_jsonrpc_server *server;
	struct spdk_jsonrpc_server_conn *conn;
	const char *json_req;
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
	FREE_REQUEST();

	/* Partial (but not invalid) requests - parse should not consume anything. */
	json_req = "    {\"jsonrpc\":\"2.0\",\"method\":\"b\",\"params\":[2],\"id\":2}";
	len = strlen(json_req);

	/* Try every partial length up to the full request length */
	for (i = 0; i < len; i++) {
		int rc = jsonrpc_parse_request(conn, json_req, i);
		/* Partial request - no data consumed */
		CU_ASSERT(rc == 0);
		CU_ASSERT(g_request == NULL);

		/* In case of faile, don't fload console with ussless CU assert fails. */
		FREE_REQUEST();
	}

	/* Verify that full request can be parsed successfully */
	CU_ASSERT(jsonrpc_parse_request(conn, json_req, len) == (ssize_t)len);
	FREE_REQUEST();

	CU_ASSERT(conn->outstanding_requests == 0);
	free(conn);
	free(server);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("jsonrpc", NULL, NULL);

	CU_ADD_TEST(suite, test_parse_request);
	CU_ADD_TEST(suite, test_parse_request_streaming);
	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	/* This is for ASAN. Don't know why but if pointer is left in global varaible
	 * it won't be detected as leak. */
	g_request = NULL;
	return num_failures;
}
