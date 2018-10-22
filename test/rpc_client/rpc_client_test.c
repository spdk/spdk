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
		rc = -errno;
		goto out;

	}

	/* Check for error response */
	if (json_resp->error != NULL) {
		rc = -1;
		goto out;
	}

	assert(json_resp->result);

	rc = get_jsonrpc_method_json_parser(&resp, json_resp->result);
	if (rc) {
		goto out;
	}

	for (i = 0; i < (int)resp.method_num; i++) {
		if (strcmp(method_name, resp.method_names[i]) == 0) {
			rc = 0;
			goto out;
		}
	}

	rc = -1;

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

volatile int rpc_server_poll_done;

static void *
rpc_server_poll(void *arg)
{

	while (!rpc_server_poll_done) {
		spdk_rpc_accept();
		usleep(1000);
	}

	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t tid;
	int tid_valid;
	struct spdk_jsonrpc_client *client;
	int rc;
	char *method_name = "get_rpc_methods";

	rc = spdk_rpc_listen(g_rpcsock_addr);
	if (rc) {
		fprintf(stderr, "spdk_rpc_listen() failed: %d\n", rc);
		return EXIT_FAILURE;
	}

	tid_valid = pthread_create(&tid, NULL, rpc_server_poll, NULL);
	if (!tid_valid) {
		fprintf(stderr, "pthread_create() failed: %d\n", tid_valid);
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
		fprintf(stderr, "spdk_jsonrpc_client_connect() failed: %d\n", errno);
		goto out;
	}

out:
	spdk_jsonrpc_client_close(client);

	if (tid_valid == 0) {
		rpc_server_poll_done = 1;
		pthread_join(tid, NULL);
	}

	spdk_rpc_close();

	return rc ? EXIT_FAILURE : 0;
}
