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

/* Defined error codes in JSON-RPC specification 2.0 */
#define SPDK_JSONRPC_ERROR_PARSE_ERROR		-32700
#define SPDK_JSONRPC_ERROR_INVALID_REQUEST	-32600
#define SPDK_JSONRPC_ERROR_METHOD_NOT_FOUND	-32601
#define SPDK_JSONRPC_ERROR_INVALID_PARAMS	-32602
#define SPDK_JSONRPC_ERROR_INTERNAL_ERROR	-32603

/* Custom error codes in SPDK

 * Error codes from and including -32768 to -32000 are reserved for
 * predefined errors, hence custom error codes must be outside of the range.
 */
#define SPDK_JSONRPC_ERROR_INVALID_STATE	-1

struct spdk_jsonrpc_server;
struct spdk_jsonrpc_request;

struct spdk_jsonrpc_client;
struct spdk_jsonrpc_client_request;

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
 * Function for specific RPC method response parsing handlers.
 *
 * \param parser_ctx context where analysis are put.
 * \param result json values responsed to this method.
 *
 * \return 0 on success.
 *         SPDK_JSON_PARSE_INVALID on failure.
 */
typedef int (*spdk_jsonrpc_client_response_parser)(
	void *parser_ctx,
	const struct spdk_json_val *result);

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

/**
 * Begin building a JSON-RPC request.
 *
 * If this function returns non-NULL, the user must call spdk_jsonrpc_end_request()
 * on the request after writing the desired request object to the spdk_json_write_ctx.
 *
 * \param request JSON-RPC request.
 * \param id ID index for the request. If < 0 skip ID.
 * \param method Name of the RPC method. If NULL caller will have to create "method" key.
 *
 * \return JSON write context to write the parameter object to, or NULL if no
 * parameter is necessary.
 */
struct spdk_json_write_ctx *
spdk_jsonrpc_begin_request(struct spdk_jsonrpc_client_request *request, int32_t id,
			   const char *method);

/**
 * Complete a JSON-RPC request.
 *
 * \param request JSON-RPC request.
 * \param w JSON write context returned from spdk_jsonrpc_begin_request().
 */
void spdk_jsonrpc_end_request(struct spdk_jsonrpc_client_request *request,
			      struct spdk_json_write_ctx *w);

/**
 * Connect to the specified RPC server.
 *
 * \param rpc_sock_addr RPC socket address.
 * \param addr_family Protocol families of address.
 *
 * \return JSON-RPC client on success, NULL on failure.
 */
struct spdk_jsonrpc_client *spdk_jsonrpc_client_connect(const char *rpc_sock_addr,
		int addr_family);

/**
 * Close the JSON-RPC client.
 *
 * \param client JSON-RPC client.
 */
void spdk_jsonrpc_client_close(struct spdk_jsonrpc_client *client);

/**
 * Create one JSON-RPC request
 *
 * \return Created JSON-RPC request.
 */
struct spdk_jsonrpc_client_request *spdk_jsonrpc_client_create_request(void);

/**
 * free one JSON-RPC request
 *
 * \param req Created JSON-RPC request.
 */
void spdk_jsonrpc_client_free_request(
	struct spdk_jsonrpc_client_request *req);

/**
 * Send the JSON-RPC request in JSON-RPC client.
 *
 * \param client JSON-RPC client.
 * \param req JSON-RPC request.
 *
 * \return 0 on success.
 */
int spdk_jsonrpc_client_send_request(struct spdk_jsonrpc_client *client,
				     struct spdk_jsonrpc_client_request *req);

/**
 * Receive the JSON-RPC response in JSON-RPC client.
 *
 * \param client JSON-RPC client.
 * \param parser_fn Specific function used to parse the result inside response.
 * \param parser_ctx Parameter for parser_fn.
 *
 * \return 0 on success.
 */
int spdk_jsonrpc_client_recv_response(struct spdk_jsonrpc_client *client,
				      spdk_jsonrpc_client_response_parser parser_fn,
				      void *parser_ctx);

#ifdef __cplusplus
}
#endif

#endif
