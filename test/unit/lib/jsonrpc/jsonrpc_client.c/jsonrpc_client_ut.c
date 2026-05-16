/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_internal/cunit.h"

#include "jsonrpc/jsonrpc_client.c"

static struct spdk_jsonrpc_client_request *
ut_create_client_request(void)
{
	struct spdk_jsonrpc_client_request *request;

	request = calloc(1, sizeof(*request));
	SPDK_CU_ASSERT_FATAL(request != NULL);

	request->send_buf_size = SPDK_JSONRPC_SEND_BUF_SIZE_INIT;
	request->send_buf = malloc(request->send_buf_size);
	SPDK_CU_ASSERT_FATAL(request->send_buf != NULL);

	return request;
}

static void
ut_free_client_request(struct spdk_jsonrpc_client_request *request)
{
	free(request->send_buf);
	free(request);
}

static struct spdk_jsonrpc_client *
ut_create_client(void)
{
	struct spdk_jsonrpc_client *client;

	client = calloc(1, sizeof(*client));
	SPDK_CU_ASSERT_FATAL(client != NULL);

	return client;
}

static void
ut_free_client(struct spdk_jsonrpc_client *client)
{
	free(client->recv_buf);
	if (client->resp) {
		spdk_jsonrpc_client_free_response(&client->resp->jsonrpc);
	}
	free(client);
}

static void
ut_client_set_response(struct spdk_jsonrpc_client *client, const char *response)
{
	size_t len = strlen(response);

	free(client->recv_buf);
	client->recv_buf = malloc(len + 1);
	SPDK_CU_ASSERT_FATAL(client->recv_buf != NULL);
	memcpy(client->recv_buf, response, len);
	client->recv_buf[len] = '\0';
	client->recv_buf_size = len + 1;
	client->recv_offset = len;
}

static void
test_begin_end_request(void)
{
	struct spdk_jsonrpc_client_request *request;
	struct spdk_json_write_ctx *w;
	int rc;

	request = ut_create_client_request();

	/* Test request with ID and method */
	w = spdk_jsonrpc_begin_request(request, 1, "test_method");
	CU_ASSERT(w != NULL);
	spdk_jsonrpc_end_request(request, w);

	/* Verify output is valid JSON */
	CU_ASSERT(request->send_len > 0);
	CU_ASSERT(request->send_buf[0] == '{');
	CU_ASSERT(request->send_buf[request->send_len - 1] == '\n');

	request->send_buf[request->send_len] = '\0';
	rc = spdk_json_parse(request->send_buf, request->send_len - 1, NULL, 0, NULL, 0);
	CU_ASSERT(rc > 0);

	ut_free_client_request(request);
}

static void
test_request_no_id(void)
{
	struct spdk_jsonrpc_client_request *request;
	struct spdk_json_write_ctx *w;
	int rc;

	request = ut_create_client_request();

	/* Test request without ID (notification style, id < 0) */
	w = spdk_jsonrpc_begin_request(request, -1, "notify_method");
	CU_ASSERT(w != NULL);
	spdk_jsonrpc_end_request(request, w);

	/* Verify output is valid JSON */
	CU_ASSERT(request->send_len > 0);
	request->send_buf[request->send_len] = '\0';
	rc = spdk_json_parse(request->send_buf, request->send_len - 1, NULL, 0, NULL, 0);
	CU_ASSERT(rc > 0);

	/* Verify no "id" field by checking it's not in the output */
	CU_ASSERT(strstr((char *)request->send_buf, "\"id\"") == NULL);

	ut_free_client_request(request);
}

static void
test_request_no_method(void)
{
	struct spdk_jsonrpc_client_request *request;
	struct spdk_json_write_ctx *w;
	int rc;

	request = ut_create_client_request();

	/* Test request with NULL method (caller adds it manually) */
	w = spdk_jsonrpc_begin_request(request, 5, NULL);
	CU_ASSERT(w != NULL);
	/* Caller would add method here: spdk_json_write_named_string(w, "method", "..."); */
	spdk_jsonrpc_end_request(request, w);

	/* Verify output is valid JSON */
	CU_ASSERT(request->send_len > 0);
	request->send_buf[request->send_len] = '\0';
	rc = spdk_json_parse(request->send_buf, request->send_len - 1, NULL, 0, NULL, 0);
	CU_ASSERT(rc > 0);

	ut_free_client_request(request);
}

static void
test_parse_response_success(void)
{
	struct spdk_jsonrpc_client *client;
	int rc;

	client = ut_create_client();

	/* Set up a successful response */
	ut_client_set_response(client, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\"success\"}");

	rc = jsonrpc_parse_response(client);
	CU_ASSERT(rc == 1);
	CU_ASSERT(client->resp != NULL);
	CU_ASSERT(client->resp->jsonrpc.result != NULL);
	CU_ASSERT(client->resp->jsonrpc.error == NULL);

	ut_free_client(client);
}

static void
test_parse_response_error(void)
{
	struct spdk_jsonrpc_client *client;
	int rc;

	client = ut_create_client();

	/* Set up an error response */
	ut_client_set_response(client,
			       "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32600,\"message\":\"Invalid Request\"}}");

	rc = jsonrpc_parse_response(client);
	CU_ASSERT(rc == 1);
	CU_ASSERT(client->resp != NULL);
	CU_ASSERT(client->resp->jsonrpc.result == NULL);
	CU_ASSERT(client->resp->jsonrpc.error != NULL);

	ut_free_client(client);
}

static void
test_parse_response_incomplete(void)
{
	struct spdk_jsonrpc_client *client;
	int rc;

	client = ut_create_client();

	/* Set up an incomplete response */
	ut_client_set_response(client, "{\"jsonrpc\":\"2.0\",\"id\":");

	rc = jsonrpc_parse_response(client);
	CU_ASSERT(rc == 0); /* Incomplete, need more data */
	CU_ASSERT(client->resp == NULL);

	ut_free_client(client);
}

static void
test_parse_response_invalid_json(void)
{
	struct spdk_jsonrpc_client *client;
	int rc;

	client = ut_create_client();

	/* Set up invalid JSON */
	ut_client_set_response(client, "{\"jsonrpc\":\"2.0\",\"id\":1,invalid}");

	rc = jsonrpc_parse_response(client);
	CU_ASSERT(rc == -EINVAL);

	ut_free_client(client);
}

static void
test_parse_response_wrong_version(void)
{
	struct spdk_jsonrpc_client *client;
	int rc;

	client = ut_create_client();

	/* Set up response with wrong JSON-RPC version */
	ut_client_set_response(client, "{\"jsonrpc\":\"1.0\",\"id\":1,\"result\":\"test\"}");

	rc = jsonrpc_parse_response(client);
	CU_ASSERT(rc == -EINVAL);

	ut_free_client(client);
}

static void
test_parse_response_not_object(void)
{
	struct spdk_jsonrpc_client *client;
	int rc;

	client = ut_create_client();

	/* Set up response that's not an object */
	ut_client_set_response(client, "\"just a string\"");

	rc = jsonrpc_parse_response(client);
	CU_ASSERT(rc == -EINVAL);

	ut_free_client(client);
}

static void
test_begin_end_batch(void)
{
	struct spdk_jsonrpc_client_request *request;
	int rc;

	request = ut_create_client_request();

	/* Begin batch should succeed and set batch_write_ctx */
	rc = spdk_jsonrpc_begin_batch(request);
	CU_ASSERT(rc == 0);
	CU_ASSERT(request->batch_write_ctx != NULL);
	CU_ASSERT(request->batch_id == 0);

	/* End batch should clear batch_write_ctx */
	spdk_jsonrpc_end_batch(request);
	CU_ASSERT(request->batch_write_ctx == NULL);

	/* Verify output is a valid JSON array (empty batch) */
	CU_ASSERT(request->send_len > 0);
	CU_ASSERT(request->send_buf[0] == '[');
	CU_ASSERT(request->send_buf[request->send_len - 2] == ']');
	CU_ASSERT(request->send_buf[request->send_len - 1] == '\n');

	ut_free_client_request(request);
}

static void
test_batch_single_request(void)
{
	struct spdk_jsonrpc_client_request *request;
	struct spdk_json_write_ctx *w;
	int rc;

	request = ut_create_client_request();

	/* Begin batch */
	rc = spdk_jsonrpc_begin_batch(request);
	CU_ASSERT(rc == 0);

	/* Add one request with auto-assigned ID */
	w = spdk_jsonrpc_begin_request(request, -1, "test_method");
	CU_ASSERT(w != NULL);
	CU_ASSERT(request->batch_id == 1); /* Should have been incremented */
	spdk_jsonrpc_end_request(request, w);

	/* End batch */
	spdk_jsonrpc_end_batch(request);

	/* Verify output is valid JSON */
	CU_ASSERT(request->send_len > 0);
	request->send_buf[request->send_len] = '\0';

	/* Parse and verify structure */
	rc = spdk_json_parse(request->send_buf, request->send_len - 1, NULL, 0, NULL, 0);
	CU_ASSERT(rc > 0);

	ut_free_client_request(request);
}

static void
test_batch_multiple_requests(void)
{
	struct spdk_jsonrpc_client_request *request;
	struct spdk_json_write_ctx *w;
	int rc;

	request = ut_create_client_request();

	/* Begin batch */
	rc = spdk_jsonrpc_begin_batch(request);
	CU_ASSERT(rc == 0);

	/* Add first request with auto-assigned ID (should be 0) */
	w = spdk_jsonrpc_begin_request(request, -1, "method1");
	CU_ASSERT(w != NULL);
	CU_ASSERT(request->batch_id == 1);
	spdk_jsonrpc_end_request(request, w);

	/* Add second request with auto-assigned ID (should be 1) */
	w = spdk_jsonrpc_begin_request(request, -1, "method2");
	CU_ASSERT(w != NULL);
	CU_ASSERT(request->batch_id == 2);
	spdk_jsonrpc_end_request(request, w);

	/* Add third request with explicit ID */
	w = spdk_jsonrpc_begin_request(request, 99, "method3");
	CU_ASSERT(w != NULL);
	CU_ASSERT(request->batch_id == 2); /* Should not change with explicit ID */
	spdk_jsonrpc_end_request(request, w);

	/* End batch */
	spdk_jsonrpc_end_batch(request);

	/* Verify output is valid JSON */
	CU_ASSERT(request->send_len > 0);
	request->send_buf[request->send_len] = '\0';

	rc = spdk_json_parse(request->send_buf, request->send_len - 1, NULL, 0, NULL, 0);
	CU_ASSERT(rc > 0);

	ut_free_client_request(request);
}

static void
test_parse_batch_response_success(void)
{
	struct spdk_jsonrpc_client *client;
	int rc;

	client = ut_create_client();

	/* Set up a batch response with multiple successful results */
	ut_client_set_response(client,
			       "["
			       "{\"jsonrpc\":\"2.0\",\"id\":0,\"result\":\"result1\"},"
			       "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\"result2\"}"
			       "]");

	rc = jsonrpc_parse_response(client);
	CU_ASSERT(rc == 1);
	CU_ASSERT(client->resp != NULL);
	CU_ASSERT(client->resp->jsonrpc.result != NULL);
	CU_ASSERT(client->resp->jsonrpc.error == NULL);

	ut_free_client(client);
}

static void
test_parse_batch_response_with_error(void)
{
	struct spdk_jsonrpc_client *client;
	int rc;

	client = ut_create_client();

	/* Set up a batch response with one error */
	ut_client_set_response(client,
			       "["
			       "{\"jsonrpc\":\"2.0\",\"id\":0,\"result\":\"success\"},"
			       "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32600,\"message\":\"error\"}}"
			       "]");

	rc = jsonrpc_parse_response(client);
	CU_ASSERT(rc == 1);
	CU_ASSERT(client->resp != NULL);
	/* Should capture the first error found */
	CU_ASSERT(client->resp->jsonrpc.error != NULL);

	ut_free_client(client);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("jsonrpc_client", NULL, NULL);

	CU_ADD_TEST(suite, test_begin_end_request);
	CU_ADD_TEST(suite, test_request_no_id);
	CU_ADD_TEST(suite, test_request_no_method);
	CU_ADD_TEST(suite, test_parse_response_success);
	CU_ADD_TEST(suite, test_parse_response_error);
	CU_ADD_TEST(suite, test_parse_response_incomplete);
	CU_ADD_TEST(suite, test_parse_response_invalid_json);
	CU_ADD_TEST(suite, test_parse_response_wrong_version);
	CU_ADD_TEST(suite, test_parse_response_not_object);
	CU_ADD_TEST(suite, test_begin_end_batch);
	CU_ADD_TEST(suite, test_batch_single_request);
	CU_ADD_TEST(suite, test_batch_multiple_requests);
	CU_ADD_TEST(suite, test_parse_batch_response_success);
	CU_ADD_TEST(suite, test_parse_batch_response_with_error);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);

	CU_cleanup_registry();

	return num_failures;
}
