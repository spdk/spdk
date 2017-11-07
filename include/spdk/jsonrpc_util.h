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

#ifndef SPDK_JSONRPC_UTIL_H_
#define SPDK_JSONRPC_UTIL_H_

#include "spdk/jsonrpc.h"

struct spdk_jsonrpc_util_req;
typedef void (*spdk_jsonrpc_request_handler_fn)(struct spdk_jsonrpc_util_req *request);

#define SPDK_RPC_PARAM_BOOL spdk_json_decode_bool
#define SPDK_RPC_PARAM_INT32 spdk_json_decode_int32
#define SPDK_RPC_PARAM_UINT32 spdk_json_decode_uint32
#define SPDK_RPC_PARAM_UINT64 spdk_json_decode_uint64
#define SPDK_RPC_PARAM_STRING spdk_json_decode_string


struct spdk_jsonrpc_params {
	const char *name;
	spdk_json_decode_fn type;
	bool optional;
};

void spdk_jsonrpc_register_cmd(const char *method, spdk_jsonrpc_request_handler_fn func,
			       const struct spdk_jsonrpc_params *params);

#define SPDK_JSONRPC_CMD(cmd, params) \
static void __attribute__((constructor)) rpc_register_##cmd(void) \
{ \
	spdk_jsonrpc_register_cmd(#cmd, cmd, params); \
}

int spdk_jsonrpc_param_bool(struct spdk_jsonrpc_util_req *req, const char *name, int default_value);
int32_t spdk_jsonrpc_param_int32(struct spdk_jsonrpc_util_req *req, const char *name,
				 int32_t default_value);
uint32_t spdk_jsonrpc_param_uint32(struct spdk_jsonrpc_util_req *req, const char *name,
				   uint32_t default_value);
uint64_t spdk_jsonrpc_param_uint64(struct spdk_jsonrpc_util_req *req, const char *name,
				   uint64_t default_value);
const char *spdk_jsonrpc_param_str(struct spdk_jsonrpc_util_req *req, const char *name,
				   const char *default_value);

void spdk_jsonrpc_bool_create(struct spdk_jsonrpc_util_req *req, const char *name, bool val);
void spdk_jsonrpc_int_create(struct spdk_jsonrpc_util_req *req, const char *name, int64_t val);
void spdk_jsonrpc_uint_create(struct spdk_jsonrpc_util_req *req, const char *name, uint64_t val);
void spdk_jsonrpc_string_create(struct spdk_jsonrpc_util_req *req, const char *name,
				const char *fmt,
				...) __attribute__((format(printf, 3, 4)));

void spdk_jsonrpc_object_create(struct spdk_jsonrpc_util_req *req, const char *name);
void spdk_jsonrpc_object_begin(struct spdk_jsonrpc_util_req *req);
void spdk_jsonrpc_object_end(struct spdk_jsonrpc_util_req *req);

void spdk_jsonrpc_array_create(struct spdk_jsonrpc_util_req *req, const char *name);
void spdk_jsonrpc_array_begin(struct spdk_jsonrpc_util_req *req);
void spdk_jsonrpc_array_end(struct spdk_jsonrpc_util_req *req);

/**
 * Send response if there is no data to attach.
 *
 * If \c res is \c true just send 'True' as result. In case of \c res is \c false
 * send error response with code -32602 and \c fail_msg as message.
 *
 * \param req The JSON RPC request object.
 * \param res Result of response.
 * \param fail_msg Message in case of failure.
 */
void spdk_jsonrpc_send_response(struct spdk_jsonrpc_util_req *req, bool res, const char *fmt,
				...)  __attribute__((format(printf, 3, 4)));

/**
 * Shortcut function for sending message based on \c errno value.
 * Negative \c errno is considere as failure, non-negative is success.
 * If \c errno is non-zero it's absolute value will be converted to coresponding
 * errno message and send along with result.
 *
 * \param req
 * \param code
 */
void spdk_jsonrpc_send_errno_response(struct spdk_jsonrpc_util_req *req, int code);

void spdk_jsonrpc_end_response(struct spdk_jsonrpc_util_req *req);

/**
 * Return JSON write context for JSON RPC response.
 *
 * \param req The JSON RPC request object.
 * \return JSON write context or NULL if there is no response will be set for \c req.
 */
struct spdk_json_write_ctx *spdk_jsonrpc_response_ctx(struct spdk_jsonrpc_util_req *req);
#endif
