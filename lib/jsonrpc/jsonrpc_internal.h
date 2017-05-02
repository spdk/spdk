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

#ifndef SPDK_JSONRPC_INTERNAL_H_
#define SPDK_JSONRPC_INTERNAL_H_

#include "spdk/stdinc.h"

#include "spdk/jsonrpc.h"

#include "spdk_internal/log.h"

#define SPDK_JSONRPC_RECV_BUF_SIZE	(32 * 1024)
#define SPDK_JSONRPC_SEND_BUF_SIZE	(32 * 1024)
#define SPDK_JSONRPC_MAX_CONNS		64
#define SPDK_JSONRPC_MAX_VALUES		1024

struct spdk_jsonrpc_server_conn {
	struct spdk_jsonrpc_server *server;
	int sockfd;
	struct spdk_json_val values[SPDK_JSONRPC_MAX_VALUES];
	size_t recv_len;
	uint8_t recv_buf[SPDK_JSONRPC_RECV_BUF_SIZE];
	size_t send_len;
	uint8_t send_buf[SPDK_JSONRPC_SEND_BUF_SIZE];
	struct spdk_json_write_ctx *json_writer;
	bool batch;
};

struct spdk_jsonrpc_server {
	int sockfd;
	spdk_jsonrpc_handle_request_fn handle_request;
	struct spdk_jsonrpc_server_conn conns[SPDK_JSONRPC_MAX_CONNS];
	struct pollfd pollfds[SPDK_JSONRPC_MAX_CONNS + 1];
	int num_conns;
};

/* jsonrpc_server_tcp */
int spdk_jsonrpc_server_write_cb(void *cb_ctx, const void *data, size_t size);
void spdk_jsonrpc_server_handle_request(struct spdk_jsonrpc_server_conn *conn,
					const struct spdk_json_val *method,
					const struct spdk_json_val *params,
					const struct spdk_json_val *id);
void spdk_jsonrpc_server_handle_error(struct spdk_jsonrpc_server_conn *conn, int error,
				      const struct spdk_json_val *method,
				      const struct spdk_json_val *params,
				      const struct spdk_json_val *id);

/* jsonrpc_server */
int spdk_jsonrpc_parse_request(struct spdk_jsonrpc_server_conn *conn, void *json, size_t size);

#endif
