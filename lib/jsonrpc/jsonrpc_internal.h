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

#include "spdk/env.h"
#include "spdk/jsonrpc.h"

#include "spdk_internal/log.h"

#define SPDK_JSONRPC_RECV_BUF_SIZE	(32 * 1024)
#define SPDK_JSONRPC_SEND_BUF_SIZE	(32 * 1024)
#define SPDK_JSONRPC_ID_MAX_LEN		128
#define SPDK_JSONRPC_MAX_CONNS		64
#define SPDK_JSONRPC_MAX_VALUES		1024

struct spdk_jsonrpc_request {
	struct spdk_jsonrpc_server_conn *conn;

	/* Copy of request id value */
	struct spdk_json_val id;
	uint8_t id_data[SPDK_JSONRPC_ID_MAX_LEN];

	size_t send_len;
	size_t send_offset;
	uint8_t send_buf[SPDK_JSONRPC_SEND_BUF_SIZE];
};

struct spdk_jsonrpc_server_conn {
	struct spdk_jsonrpc_server *server;
	int sockfd;
	bool closed;
	struct spdk_json_val values[SPDK_JSONRPC_MAX_VALUES];
	size_t recv_len;
	uint8_t recv_buf[SPDK_JSONRPC_RECV_BUF_SIZE];
	uint32_t outstanding_requests;
	struct spdk_ring *send_queue;
	struct spdk_jsonrpc_request *send_request;
};

struct spdk_jsonrpc_server {
	int sockfd;
	spdk_jsonrpc_handle_request_fn handle_request;
	struct spdk_jsonrpc_server_conn conns[SPDK_JSONRPC_MAX_CONNS];
	int num_conns;
};

/* jsonrpc_server_tcp */
void spdk_jsonrpc_server_handle_request(struct spdk_jsonrpc_request *request,
					const struct spdk_json_val *method,
					const struct spdk_json_val *params);
void spdk_jsonrpc_server_handle_error(struct spdk_jsonrpc_request *request, int error);
void spdk_jsonrpc_server_send_response(struct spdk_jsonrpc_server_conn *conn,
				       struct spdk_jsonrpc_request *request);

/* jsonrpc_server */
int spdk_jsonrpc_parse_request(struct spdk_jsonrpc_server_conn *conn, void *json, size_t size);
void spdk_jsonrpc_free_request(struct spdk_jsonrpc_request *request);

#endif
