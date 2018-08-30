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
#include "spdk/json.h"
#include "spdk/jsonrpc.h"
#include "spdk/rpc_client.h"

#define RPC_MAX_METHODS 200

struct get_rpc_methods_resp {
	char *method_names[RPC_MAX_METHODS];
	size_t method_num;
};

static int
get_rpc_method_json_parser(void *parser_ctx,
			   const struct spdk_json_val *result)
{
	struct get_rpc_methods_resp *resp = (struct get_rpc_methods_resp *)parser_ctx;

	return spdk_json_decode_array(result, spdk_json_decode_string, resp->method_names,
				      RPC_MAX_METHODS, &resp->method_num, sizeof(char *));
}

int
_spdk_rpc_client_check_rpc_method(struct spdk_rpc_client_conn *_conn, char *method_name)
{
	int rc, i;
	struct get_rpc_methods_resp resp = {};
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_client_request *request;
	struct spdk_jsonrpc_client_conn *conn;

	conn = (struct spdk_jsonrpc_client_conn *)_conn;
	request = spdk_jsonrpc_client_get_request(conn);

	w = spdk_jsonrpc_begin_request(request, "get_rpc_methods");
	spdk_jsonrpc_end_request(request, w);
	spdk_jsonrpc_client_send_request(conn);

	rc = spdk_jsonrpc_client_recv_response(conn, get_rpc_method_json_parser, &resp);

	if (rc) {
		return rc;
	}

	for (i = 0; i < (int)resp.method_num; i++) {
		if (strcmp(method_name, resp.method_names[i]) == 0) {
			return 0;
		}
	}

	for (i = 0; i < (int)resp.method_num; i++) {
		CLIENT_DEBUGLOG("%s\n", resp.method_names[i]);
	}

	return -1;
}

int
spdk_rpc_client_check_rpc_method(const char *rpcsock_addr, char *method_name)
{
	struct spdk_rpc_client_conn *conn;
	int rc;

	conn = spdk_rpc_client_connect(rpcsock_addr);
	if (!conn) {
		return -1;
	}

	rc = _spdk_rpc_client_check_rpc_method(conn, method_name);

	spdk_rpc_client_close(conn);

	return rc;
}
