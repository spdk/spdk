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

#define _GNU_SOURCE
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

	w = spdk_jsonrpc_begin_request(request, 1, "get_rpc_methods");
	spdk_jsonrpc_end_request(request, w);
	spdk_jsonrpc_client_send_request(client, request);

	rc = spdk_jsonrpc_client_recv_response(client);
	if (rc) {
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
	SPDK_ERRLOG("Method '%s' not found in response \n", method_name);

out:
	for (i = 0; i < (int)resp.method_num; i++) {
		SPDK_NOTICELOG("%s\n", resp.method_names[i]);
		free(resp.method_names[i]);
	}

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

volatile int rpc_server_th_stop;
volatile int rpc_server_th_listening;

static void *
rpc_server_th(void *arg)
{
	int rc;

	rc = spdk_rpc_listen(g_rpcsock_addr);
	if (rc) {
		fprintf(stderr, "spdk_rpc_listen() failed: %d\n", rc);
		goto out;
	}

	rpc_server_th_listening = 1;

	while (!rpc_server_th_stop) {
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
	struct spdk_jsonrpc_client *client;
	char *method_name = "get_rpc_methods";
	int rc, i;

	for (i = 0; i < 10; i++) {
		usleep(50);
		if (rpc_server_th_listening != 0) {
			break;
		}
	}

	client = spdk_jsonrpc_client_connect(g_rpcsock_addr, g_addr_family);
	if (!client) {
		fprintf(stderr, "spdk_jsonrpc_client_connect() failed: %d\n", errno);
		rc = -1;
		goto out;
	}

	rc = spdk_jsonrpc_client_check_rpc_method(client, method_name);
	if (rc) {
		fprintf(stderr, "spdk_jsonrpc_client_check_rpc_method() failed: %d\n", errno);
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
	struct timespec timeout;
	pthread_t srv_tid, client_tid;
	int srv_tid_valid;
	int client_tid_valid = INT_MIN, srv_rc, client_rc = INT_MIN;

	int rc;

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

	/* Give threads some time before joining */
	usleep(500000);
out:
	if (client_tid_valid == 0) {
		clock_gettime(CLOCK_REALTIME, &timeout);
		timeout.tv_sec += JOIN_TIMEOUT_S;

		rc = pthread_timedjoin_np(client_tid, (void **)&client_rc, &timeout);
		if (rc) {
			fprintf(stderr, "failed to join client thread (rc: %d, client_rc:%d)\n", rc, client_rc);
		}
	}

	rpc_server_th_stop = 1;

	if (srv_tid_valid == 0) {
		clock_gettime(CLOCK_REALTIME, &timeout);
		timeout.tv_sec += JOIN_TIMEOUT_S;

		rc = pthread_timedjoin_np(srv_tid, (void **)&srv_rc, &timeout);
		if (rc) {
			fprintf(stderr, "failed to join server thread: (rc: %d, srv_rc:%d)\n", rc, srv_rc);
		}
	}

	rc = rc || client_rc || srv_rc ? EXIT_FAILURE : 0;
	fprintf(stderr, "%s\n", rc == 0 ? "OK" : "FAILED");
	return rc || client_rc || srv_rc ? EXIT_FAILURE : 0;
}
