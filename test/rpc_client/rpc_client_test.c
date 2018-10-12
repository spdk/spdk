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

#define RPC_MAX_METHODS 200

static const char *g_rpcsock_addr = SPDK_DEFAULT_RPC_ADDR;
static int g_addr_family = AF_UNIX;

#define RPC_MAX_METHODS 200

struct get_jsonrpc_methods_resp {
	char *method_names[RPC_MAX_METHODS];
	size_t method_num;
};

static int
get_jsonrpc_method_json_parser(void *parser_ctx,
			       const struct spdk_json_val *result)
{
	struct get_jsonrpc_methods_resp *resp = parser_ctx;

	return spdk_json_decode_array(result, spdk_json_decode_string, resp->method_names,
				      RPC_MAX_METHODS, &resp->method_num, sizeof(char *));
}

static int
spdk_jsonrpc_client_check_rpc_method(struct spdk_jsonrpc_client *client, char *method_name)
{
	int rc, i;
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
	spdk_jsonrpc_client_free_request(request);

	rc = spdk_jsonrpc_client_recv_response(client, get_jsonrpc_method_json_parser, &resp);

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

	return rc;
}

int main(int argc, char **argv)
{
	struct spdk_jsonrpc_client *client;
	int rc;
	char *method_name = "get_rpc_methods";

	client = spdk_jsonrpc_client_connect(g_rpcsock_addr, g_addr_family);
	if (!client) {
		return EXIT_FAILURE;
	}

	rc = spdk_jsonrpc_client_check_rpc_method(client, method_name);

	spdk_jsonrpc_client_close(client);

	return rc ? EXIT_FAILURE : 0;
}
