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

#ifndef SPDK_JSONRPC_CLIENT_INTERNAL_H_
#define SPDK_JSONRPC_CLIENT_INTERNAL_H_

#include "spdk/stdinc.h"
#include "spdk/json.h"
#include "spdk/jsonrpc_client_cmd.h"

#ifndef DEBUG
#define CLIENT_ERRLOG(...) do { } while (0)
#define CLIENT_DEBUGLOG(...) do { } while (0)
#else

#define MAX_TMPBUF 1024

static inline void
spdk_client_log(int level, const char *file, const int line, const char *func,
		const char *format, ...)
{
	char buf[MAX_TMPBUF];
	va_list ap;

	va_start(ap, format);
	vsnprintf(buf, sizeof(buf), format, ap);
	syslog(level, "%s:%4d:%s: %s", file, line, func, buf);
	va_end(ap);
}

#define CLIENT_ERRLOG(...) \
	spdk_client_log(LOG_ERR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define CLIENT_DEBUGLOG(...) \
	spdk_client_log(LOG_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)

#endif

/* Mirror definitions with jsonrpc/jsonrpc_internal.h */
#define SPDK_JSONRPC_CLIENT_BUF_SIZE_INIT	(32)
#define SPDK_JSONRPC_CLIENT_BUF_SIZE_MAX	(32 * 1024 * 1024)
#define SPDK_JSONRPC_CLIENT_MAX_VALUES		1024

/**
 * Function for specific RPC method response parsing handlers.
 *
 * \param parser_ctx context where analysis are put.
 * \param result json values responsed to this method.
 */
typedef int (*spdk_json_method_parser)(void *parser_ctx,
				       const struct spdk_json_val *result);

struct spdk_jsonrpc_client_request {
	/* Total space allocated for send_buf */
	size_t send_buf_size;

	/* Number of bytes used in send_buf (<= send_buf_size) */
	size_t send_len;

	size_t send_offset;

	uint8_t *send_buf;
};

struct spdk_jsonrpc_client_response {
	/* Total space allocated for send_buf */
	size_t recv_buf_size;

	size_t recv_offset;

	uint8_t *recv_buf;

	struct spdk_json_val values[SPDK_JSONRPC_CLIENT_MAX_VALUES];
};

struct spdk_jsonrpc_client_conn {
	int sockfd;

	struct spdk_jsonrpc_client_request request;
	struct spdk_jsonrpc_client_response response;

	spdk_json_method_parser method_parser;
	void *parser_ctx;
};

int spdk_jsonrpc_client_send_request(struct spdk_jsonrpc_client_conn *conn);
int spdk_jsonrpc_client_recv_response(struct spdk_jsonrpc_client_conn *conn);

struct spdk_json_write_ctx *spdk_jsonrpc_begin_request(struct spdk_jsonrpc_client_request *request,
		const char *method);
void spdk_jsonrpc_end_request(struct spdk_jsonrpc_client_request *request,
			      struct spdk_json_write_ctx *w);

/*
 * Parse JSON data as RPC command response.
 *
 * \param conn structure pointer of jsonrpc client connection
 * \param json Raw JSON data; must be encoded in UTF-8.
 * Note that the data may be modified to perform in-place string decoding.
 * \param size Size of data in bytes.
 *
 * \return 1 On success
 *         0 If the provided data was not a complete JSON value
 *         or negative on failure
 */
int spdk_jsonrpc_client_parse_response(struct spdk_jsonrpc_client_conn *conn, void *json,
				       size_t size);

#endif
