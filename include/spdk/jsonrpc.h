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

/**
 * \file
 * JSON-RPC 2.0 server implementation
 */

#ifndef SPDK_JSONRPC_H_
#define SPDK_JSONRPC_H_

#include "spdk/stdinc.h"

#include "spdk/json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_JSONRPC_ERROR_PARSE_ERROR		-32700
#define SPDK_JSONRPC_ERROR_INVALID_REQUEST	-32600
#define SPDK_JSONRPC_ERROR_METHOD_NOT_FOUND	-32601
#define SPDK_JSONRPC_ERROR_INVALID_PARAMS	-32602
#define SPDK_JSONRPC_ERROR_INTERNAL_ERROR	-32603

struct spdk_jsonrpc_server;
struct spdk_jsonrpc_request;

/**
 * User callback to handle a single JSON-RPC request.
 *
 * The user should respond by calling one of spdk_jsonrpc_begin_result() or
 * spdk_jsonrpc_send_error_response().
 *
 * \param request JSON-RPC request to handle.
 * \param method Function to handle the request.
 * \param param Parameters passed to the function 'method'.
 */
typedef void (*spdk_jsonrpc_handle_request_fn)(
	struct spdk_jsonrpc_request *request,
	const struct spdk_json_val *method,
	const struct spdk_json_val *params);

/**
 * Create a JSON-RPC server listening on the required address.
 *
 * \param domain Socket family.
 * \param protocol Protocol.
 * \param listen_addr Listening address.
 * \param addrlen Length of address.
 * \param handle_request User callback to handle a JSON-RPC request.
 *
 * \return a pointer to the JSON-RPC server.
 */
struct spdk_jsonrpc_server *spdk_jsonrpc_server_listen(int domain, int protocol,
		struct sockaddr *listen_addr, socklen_t addrlen, spdk_jsonrpc_handle_request_fn handle_request);

/**
 * Poll the requests to the JSON-RPC server.
 *
 * This function does accept, receive, handle the requests and reply to them.
 *
 * \param server JSON-RPC server.
 *
 * \return 0 on success.
 */
int spdk_jsonrpc_server_poll(struct spdk_jsonrpc_server *server);

/**
 * Shutdown the JSON-RPC server.
 *
 * \param server JSON-RPC server.
 */
void spdk_jsonrpc_server_shutdown(struct spdk_jsonrpc_server *server);

/**
 * Begin building a response to a JSON-RPC request.
 *
 * If this function returns non-NULL, the user must call spdk_jsonrpc_end_result()
 * on the request after writing the desired response object to the spdk_json_write_ctx.
 *
 * \param request JSON-RPC request to respond to.

 * \return JSON write context to write the response object to, or NULL if no
 * response is necessary.
 */
struct spdk_json_write_ctx *spdk_jsonrpc_begin_result(struct spdk_jsonrpc_request *request);

/**
 * Complete and send a JSON-RPC response.
 *
 * \param request Request to complete the response for.
 * \param w JSON write context returned from spdk_jsonrpc_begin_result().
 */
void spdk_jsonrpc_end_result(struct spdk_jsonrpc_request *request, struct spdk_json_write_ctx *w);

/**
 * Send an error response to a JSON-RPC request.
 *
 * This is shorthand for spdk_jsonrpc_begin_result() + spdk_jsonrpc_end_result()
 * with an error object.
 *
 * \param request JSON-RPC request to respond to.
 * \param error_code Integer error code to return (may be one of the
 * SPDK_JSONRPC_ERROR_ errors, or a custom error code).
 * \param msg String error message to return.
 */
void spdk_jsonrpc_send_error_response(struct spdk_jsonrpc_request *request,
				      int error_code, const char *msg);

/**
 * Send an error response to a JSON-RPC request.
 *
 * This is shorthand for printf() + spdk_jsonrpc_send_error_response().
 *
 * \param request JSON-RPC request to respond to.
 * \param error_code Integer error code to return (may be one of the
 * SPDK_JSONRPC_ERROR_ errors, or a custom error code).
 * \param fmt Printf-like format string.
 */
void spdk_jsonrpc_send_error_response_fmt(struct spdk_jsonrpc_request *request,
		int error_code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));


#ifdef __cplusplus
}
#endif

#endif
