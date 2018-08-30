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
#include "spdk/jsonrpc_client_cmd.h"
#include "jsonrpc_client_internal.h"

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
_spdk_rpc_client_get_rpc_method(struct spdk_jsonrpc_client_conn *conn)
{
	struct spdk_json_write_ctx *w;
	int rc;
	struct get_rpc_methods_resp resp = {};

	w = spdk_jsonrpc_begin_request(&conn->request, "get_rpc_methods");
	spdk_jsonrpc_end_request(&conn->request, w);
	spdk_jsonrpc_client_send_request(conn);

	conn->method_parser = get_rpc_method_json_parser;
	conn->parser_ctx = &resp;
	rc = spdk_jsonrpc_client_recv_response(conn);

	for (; resp.method_num > 0; resp.method_num--) {
		CLIENT_DEBUGLOG("%s\n", resp.method_names[resp.method_num - 1]);
	}

	return rc;
}

int
spdk_rpc_client_get_rpc_method(const char *rpcsock_addr)
{
	struct spdk_jsonrpc_client_conn *conn;
	int rc;

	conn = spdk_jsonrpc_client_connect(rpcsock_addr);
	if (!conn) {
		return -1;
	}

	rc = _spdk_rpc_client_get_rpc_method(conn);

	spdk_jsonrpc_client_close(conn);

	return rc;
}
