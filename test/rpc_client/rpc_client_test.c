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
#include "spdk/event.h"
#include "spdk/jsonrpc.h"
#include "spdk/util.h"
#include "spdk/rpc.h"


#define RPC_MAX_METHODS 200
#define JOIN_TIMEOUT_S 1

static const char *g_rpcsock_addr = SPDK_DEFAULT_RPC_ADDR;
static int g_addr_family = AF_UNIX;

#define RPC_MAX_METHODS 200

struct get_jsonrpc_methods_resp {
	char *method_names[RPC_MAX_METHODS];
	size_t method_num;
};

static int
_rpc_client_wait_for_response(struct spdk_jsonrpc_client *client)
{
	int rc;

	do {
		rc = spdk_jsonrpc_client_poll(client, 1);
	} while (rc == 0 || rc == -ENOTCONN);

	if (rc <= 0) {
		SPDK_ERRLOG("Failed to get response: %d\n", rc);
	}

	return rc;
}

static int
get_jsonrpc_method_json_parser(struct get_jsonrpc_methods_resp *resp,
			       const struct spdk_json_val *result)
{
	return spdk_json_decode_array(result, spdk_json_decode_string, resp->method_names,
				      RPC_MAX_METHODS, &resp->method_num, sizeof(char *));
}

static int
spdk_jsonrpc_client_check_rpc_method(struct spdk_jsonrpc_client *client, char *method_name)
{
	int rc, i;
	struct spdk_jsonrpc_client_response *json_resp = NULL;
	struct get_jsonrpc_methods_resp resp = {};
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_client_request *request;

	request = spdk_jsonrpc_client_create_request();
	if (request == NULL) {
		return -ENOMEM;
	}

	w = spdk_jsonrpc_begin_request(request, 1, "rpc_get_methods");
	spdk_jsonrpc_end_request(request, w);
	spdk_jsonrpc_client_send_request(client, request);

	rc = _rpc_client_wait_for_response(client);
	if (rc <= 0) {
		goto out;
	}

	json_resp = spdk_jsonrpc_client_get_response(client);
	if (json_resp == NULL) {
		SPDK_ERRLOG("spdk_jsonrpc_client_get_response() failed\n");
		rc = -1;
		goto out;

	}

	/* Check for error response */
	if (json_resp->error != NULL) {
		SPDK_ERRLOG("Unexpected error response\n");
		rc = -1;
		goto out;
	}

	assert(json_resp->result);

	rc = get_jsonrpc_method_json_parser(&resp, json_resp->result);
	if (rc) {
		SPDK_ERRLOG("get_jsonrpc_method_json_parser() failed\n");
		goto out;
	}

	for (i = 0; i < (int)resp.method_num; i++) {
		if (strcmp(method_name, resp.method_names[i]) == 0) {
			rc = 0;
			goto out;
		}
	}

	rc = -1;
	SPDK_ERRLOG("Method '%s' not found in response\n", method_name);

out:
	for (i = 0; i < (int)resp.method_num; i++) {
		SPDK_NOTICELOG("%s\n", resp.method_names[i]);
		free(resp.method_names[i]);
	}

	spdk_jsonrpc_client_free_response(json_resp);
	return rc;
}

static int
spdk_jsonrpc_client_check_null_params_method(struct spdk_jsonrpc_client *client)
{
	int rc;
	bool res = false;
	struct spdk_jsonrpc_client_response *json_resp = NULL;
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_client_request *request;

	request = spdk_jsonrpc_client_create_request();
	if (request == NULL) {
		return -ENOMEM;
	}

	w = spdk_jsonrpc_begin_request(request, 1, "test_null_params");
	spdk_json_write_name(w, "params");
	spdk_json_write_null(w);
	spdk_jsonrpc_end_request(request, w);
	spdk_jsonrpc_client_send_request(client, request);

	rc = _rpc_client_wait_for_response(client);
	if (rc <= 0) {
		goto out;
	}

	json_resp = spdk_jsonrpc_client_get_response(client);
	if (json_resp == NULL) {
		SPDK_ERRLOG("spdk_jsonrpc_client_get_response() failed\n");
		rc = -1;
		goto out;

	}

	/* Check for error response */
	if (json_resp->error != NULL) {
		SPDK_ERRLOG("Unexpected error response\n");
		rc = -1;
		goto out;
	}

	assert(json_resp->result);

	if (spdk_json_decode_bool(json_resp->result, &res) != 0 || res != true) {
		SPDK_ERRLOG("Response is not a boolean or it is not 'true'\n");
		rc = -EINVAL;
		goto out;
	} else {
		rc = 0;
	}

out:
	spdk_jsonrpc_client_free_response(json_resp);
	return rc;
}

static void
rpc_test_method_startup(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 "rpc_test_method_startup(): Method body not implemented");
}
SPDK_RPC_REGISTER("test_method_startup", rpc_test_method_startup, SPDK_RPC_STARTUP)

static void
rpc_test_method_runtime(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 "rpc_test_method_runtime(): Method body not implemented");
}
SPDK_RPC_REGISTER("test_method_runtime", rpc_test_method_runtime, SPDK_RPC_RUNTIME)

static void
rpc_test_method_null_params(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "rpc_test_method_null_params(): Parameters are not NULL");
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("test_null_params", rpc_test_method_null_params, SPDK_RPC_RUNTIME)

static bool g_conn_close_detected;

static void
rpc_test_conn_close_cb(struct spdk_jsonrpc_server_conn *conn, void *ctx)
{
	assert((intptr_t)ctx == 42);
	g_conn_close_detected = true;
}

static void
rpc_hook_conn_close(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct spdk_jsonrpc_server_conn *conn = spdk_jsonrpc_get_conn(request);
	int rc;

	rc = spdk_jsonrpc_conn_add_close_cb(conn, rpc_test_conn_close_cb, (void *)(intptr_t)(42));
	if (rc != 0) {

		rc = spdk_jsonrpc_conn_add_close_cb(conn, rpc_test_conn_close_cb, (void *)(intptr_t)(42));
		assert(rc == -ENOSPC);
	}

	rc = spdk_jsonrpc_conn_add_close_cb(conn, rpc_test_conn_close_cb, (void *)(intptr_t)(42));
	if (rc != -EEXIST) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "rpc_test_method_conn_close_detect(): rc != -EEXIST");
		return;
	}

	rc = spdk_jsonrpc_conn_add_close_cb(conn, rpc_test_conn_close_cb, (void *)(intptr_t)(43));
	if (rc != -ENOSPC) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "rpc_test_method_conn_close_detect(): rc != -ENOSPC");
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("hook_conn_close", rpc_hook_conn_close, SPDK_RPC_RUNTIME | SPDK_RPC_STARTUP)

static int
spdk_jsonrpc_client_hook_conn_close(struct spdk_jsonrpc_client *client)
{
	int rc;
	bool res = false;
	struct spdk_jsonrpc_client_response *json_resp = NULL;
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_client_request *request;

	request = spdk_jsonrpc_client_create_request();
	if (request == NULL) {
		return -ENOMEM;
	}

	w = spdk_jsonrpc_begin_request(request, 1, "hook_conn_close");
	spdk_jsonrpc_end_request(request, w);
	spdk_jsonrpc_client_send_request(client, request);

	rc = _rpc_client_wait_for_response(client);
	if (rc <= 0) {
		goto out;
	}

	json_resp = spdk_jsonrpc_client_get_response(client);
	if (json_resp == NULL) {
		SPDK_ERRLOG("spdk_jsonrpc_client_get_response() failed\n");
		rc = -errno;
		goto out;

	}

	/* Check for error response */
	if (json_resp->error != NULL) {
		SPDK_ERRLOG("Unexpected error response: %.*s\n", json_resp->error->len,
			    (char *)json_resp->error->start);
		rc = -EIO;
		goto out;
	}

	assert(json_resp->result);
	if (spdk_json_decode_bool(json_resp->result, &res) != 0 || res != true) {
		SPDK_ERRLOG("Response is not and boolean or if not 'true'\n");
		rc = -EINVAL;
		goto out;
	}

	rc = 0;
out:
	spdk_jsonrpc_client_free_response(json_resp);
	return rc;
}

volatile int g_rpc_server_th_stop;
static sem_t g_rpc_server_th_listening;

static void *
rpc_server_th(void *arg)
{
	int rc;

	rc = spdk_rpc_listen(g_rpcsock_addr);
	if (rc) {
		fprintf(stderr, "spdk_rpc_listen() failed: %d\n", rc);
		sem_post(&g_rpc_server_th_listening);
		goto out;
	}

	sem_post(&g_rpc_server_th_listening);

	while (!g_rpc_server_th_stop) {
		spdk_rpc_accept();
		usleep(50);
	}

	spdk_rpc_close();
out:
	return (void *)(intptr_t)rc;
}

static void *
rpc_client_th(void *arg)
{
	struct spdk_jsonrpc_client *client = NULL;
	char *method_name = "rpc_get_methods";
	int rc;


	rc = sem_wait(&g_rpc_server_th_listening);
	if (rc == -1) {
		fprintf(stderr, "Timeout waiting for server thread to start listening: rc=%d errno=%d\n", rc,
			errno);
		goto out;
	}

	client = spdk_jsonrpc_client_connect(g_rpcsock_addr, g_addr_family);
	if (!client) {
		fprintf(stderr, "spdk_jsonrpc_client_connect() failed: %d\n", errno);
		rc = -1;
		goto out;
	}

	rc = spdk_jsonrpc_client_check_rpc_method(client, method_name);
	if (rc) {
		fprintf(stderr, "spdk_jsonrpc_client_check_rpc_method() failed: rc=%d errno=%d\n", rc, errno);
		goto out;
	}

	rc = spdk_jsonrpc_client_check_null_params_method(client);
	if (rc) {
		fprintf(stderr, "spdk_jsonrpc_client_null_params_method() failed: rc=%d errno=%d\n", rc, errno);
		goto out;
	}

	rc = spdk_jsonrpc_client_hook_conn_close(client);
	if (rc) {
		fprintf(stderr, "spdk_jsonrpc_client_hook_conn_close() failed: rc=%d errno=%d\n", rc, errno);
		goto out;
	}

out:
	if (client) {
		spdk_jsonrpc_client_close(client);
	}

	return (void *)(intptr_t)rc;
}

int main(int argc, char **argv)
{
	pthread_t srv_tid, client_tid;
	int srv_tid_valid;
	int client_tid_valid = -1;
	intptr_t th_rc = INTPTR_MIN;
	int rc = 0, err_cnt = 0;

	sem_init(&g_rpc_server_th_listening, 0, 0);

	srv_tid_valid = pthread_create(&srv_tid, NULL, rpc_server_th, NULL);
	if (srv_tid_valid != 0) {
		fprintf(stderr, "pthread_create() failed to create server thread: %d\n", srv_tid_valid);
		goto out;
	}

	client_tid_valid = pthread_create(&client_tid, NULL, rpc_client_th, NULL);
	if (client_tid_valid != 0) {
		fprintf(stderr, "pthread_create(): failed to create client thread: %d\n", client_tid_valid);
		goto out;
	}

out:
	if (client_tid_valid == 0) {
		rc = pthread_join(client_tid, (void **)&th_rc);
		if (rc) {
			fprintf(stderr, "pthread_join() on client thread failed (rc: %d)\n", rc);
			err_cnt++;
		} else if (th_rc) {
			fprintf(stderr, "client thread failed reported failure(thread rc: %d)\n", (int)th_rc);
			err_cnt++;
		}
	}

	g_rpc_server_th_stop = 1;

	if (srv_tid_valid == 0) {
		rc = pthread_join(srv_tid, (void **)&th_rc);
		if (rc) {
			fprintf(stderr, "pthread_join() on server thread failed (rc: %d)\n", rc);
			err_cnt++;
		} else if (th_rc) {
			fprintf(stderr, "server thread failed reported failure(thread rc: %d)\n", (int)th_rc);
			err_cnt++;
		}
	}

	if (g_conn_close_detected == false) {
		fprintf(stderr, "Connection close not detected\n");
		err_cnt++;
	}

	sem_destroy(&g_rpc_server_th_listening);

	fprintf(stderr, "%s\n", err_cnt == 0 ? "OK" : "FAILED");
	return err_cnt ? EXIT_FAILURE : 0;
}
