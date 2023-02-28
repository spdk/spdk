/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_JSONRPC_INTERNAL_H_
#define SPDK_JSONRPC_INTERNAL_H_

#include "spdk/stdinc.h"

#include "spdk/jsonrpc.h"

#include "spdk/log.h"

#define SPDK_JSONRPC_RECV_BUF_SIZE	(32 * 1024)
#define SPDK_JSONRPC_SEND_BUF_SIZE_INIT	(32 * 1024)
#define SPDK_JSONRPC_SEND_BUF_SIZE_MAX	(32 * 1024 * 1024)
#define SPDK_JSONRPC_ID_MAX_LEN		128
#define SPDK_JSONRPC_MAX_CONNS		64
#define SPDK_JSONRPC_MAX_VALUES		1024
#define SPDK_JSONRPC_CLIENT_MAX_VALUES		8192

struct spdk_jsonrpc_request {
	struct spdk_jsonrpc_server_conn *conn;

	/* Copy of request id value */
	const struct spdk_json_val *id;

	/* Total space allocated for send_buf */
	size_t send_buf_size;

	/* Number of bytes used in send_buf (<= send_buf_size) */
	size_t send_len;

	size_t send_offset;

	uint8_t *recv_buffer;
	struct spdk_json_val *values;
	size_t values_cnt;

	uint8_t *send_buf;

	struct spdk_json_write_ctx *response;

	STAILQ_ENTRY(spdk_jsonrpc_request) link;
};

struct spdk_jsonrpc_server_conn {
	struct spdk_jsonrpc_server *server;
	int sockfd;
	bool closed;
	size_t recv_len;
	uint8_t recv_buf[SPDK_JSONRPC_RECV_BUF_SIZE];
	uint32_t outstanding_requests;

	pthread_spinlock_t queue_lock;
	STAILQ_HEAD(, spdk_jsonrpc_request) send_queue;

	struct spdk_jsonrpc_request *send_request;

	spdk_jsonrpc_conn_closed_fn close_cb;
	void *close_cb_ctx;

	TAILQ_ENTRY(spdk_jsonrpc_server_conn) link;
};

struct spdk_jsonrpc_server {
	int sockfd;
	spdk_jsonrpc_handle_request_fn handle_request;

	TAILQ_HEAD(, spdk_jsonrpc_server_conn) free_conns;
	TAILQ_HEAD(, spdk_jsonrpc_server_conn) conns;

	struct spdk_jsonrpc_server_conn conns_array[SPDK_JSONRPC_MAX_CONNS];
};

struct spdk_jsonrpc_client_request {
	/* Total space allocated for send_buf */
	size_t send_buf_size;

	/* Number of bytes used in send_buf (<= send_buf_size) */
	size_t send_len;

	size_t send_offset;

	uint8_t *send_buf;
};

struct spdk_jsonrpc_client_response_internal {
	struct spdk_jsonrpc_client_response jsonrpc;
	bool ready;
	uint8_t *buf;
	size_t values_cnt;
	struct spdk_json_val values[];
};

struct spdk_jsonrpc_client {
	int sockfd;
	bool connected;

	size_t recv_buf_size;
	size_t recv_offset;
	char *recv_buf;

	/* Parsed response */
	struct spdk_jsonrpc_client_response_internal *resp;
	struct spdk_jsonrpc_client_request *request;
};

/* jsonrpc_server_tcp */
void jsonrpc_server_handle_request(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *method,
				   const struct spdk_json_val *params);
void jsonrpc_server_handle_error(struct spdk_jsonrpc_request *request, int error);

/* Might be called from any thread */
void jsonrpc_server_send_response(struct spdk_jsonrpc_request *request);

/* jsonrpc_server */
int jsonrpc_parse_request(struct spdk_jsonrpc_server_conn *conn, const void *json,
			  size_t size);

/* Must be called only from server poll thread */
void jsonrpc_free_request(struct spdk_jsonrpc_request *request);

/*
 * Parse JSON data as RPC command response.
 *
 * \param client structure pointer of jsonrpc client
 *
 * \return 0 On success. Negative error code in error
 * -EAGAIN - If the provided data is not a complete JSON value (SPDK_JSON_PARSE_INCOMPLETE)
 * -EINVAL - If the provided data has invalid JSON syntax and can't be parsed (SPDK_JSON_PARSE_INVALID).
 * -ENOSPC - No space left to store parsed response.
 */
int jsonrpc_parse_response(struct spdk_jsonrpc_client *client);

#endif
