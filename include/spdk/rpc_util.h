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

#ifndef SPDK_RPC_UTIL_H_
#define SPDK_RPC_UTIL_H_

#include "spdk/rpc.h"

struct spdk_rpc_request;
typedef void (*spdk_rpc_request_handler)(struct spdk_rpc_request *request);

#define SPDK_RPC_PARAM_BOOL spdk_json_decode_bool
#define SPDK_RPC_PARAM_INT32 spdk_json_decode_int32
#define SPDK_RPC_PARAM_UINT32 spdk_json_decode_uint32
#define SPDK_RPC_PARAM_UINT64 spdk_json_decode_uint64
#define SPDK_RPC_PARAM_STRING spdk_json_decode_string


struct spdk_rpc_request_params {
	const char *name;
	spdk_json_decode_fn type;
	bool optional;
};

void spdk_rpc_register_cmd(const char *method, spdk_rpc_request_handler func,
			   const struct spdk_rpc_request_params *params);

#define SPDK_RPC_CMD(cmd, params) \
static void __attribute__((constructor)) rpc_register_##cmd(void) \
{ \
	spdk_rpc_register_cmd(#cmd, cmd, params); \
}

int spdk_jsonrpc_param_bool(struct spdk_rpc_request *req, const char *name, int default_value);
int32_t spdk_jsonrpc_param_int32(struct spdk_rpc_request *req, const char *name,
				 int32_t default_value);
uint32_t spdk_jsonrpc_param_uint32(struct spdk_rpc_request *req, const char *name,
				   uint32_t default_value);
uint64_t spdk_jsonrpc_param_uint64(struct spdk_rpc_request *req, const char *name,
				   uint64_t default_value);
const char *spdk_jsonrpc_param_str(struct spdk_rpc_request *req, const char *name,
				   const char *default_value);

void spdk_jsonrpc_bool_create(struct spdk_rpc_request *req, const char *name, bool val);
void spdk_jsonrpc_int_create(struct spdk_rpc_request *req, const char *name, int64_t val);
void spdk_jsonrpc_uint_create(struct spdk_rpc_request *req, const char *name, uint64_t val);
void spdk_jsonrpc_string_create(struct spdk_rpc_request *req, const char *name, const char *fmt,
				...) __attribute__((format(printf, 3, 4)));

void spdk_jsonrpc_object_create(struct spdk_rpc_request *req, const char *name);
void spdk_jsonrpc_object_begin(struct spdk_rpc_request *req);
void spdk_jsonrpc_object_end(struct spdk_rpc_request *req);

void spdk_jsonrpc_array_create(struct spdk_rpc_request *req, const char *name);
void spdk_jsonrpc_array_begin(struct spdk_rpc_request *req);
void spdk_jsonrpc_array_end(struct spdk_rpc_request *req);

void spdk_jsonrpc_cmd_send_response(struct spdk_rpc_request *req, bool success,
				    const char *fail_msg);

//struct spdk_json_write_ctx *spdk_jsonrpc_begin_response(struct spdk_rpc_request *req);
void spdk_jsonrpc_end_response(struct spdk_rpc_request *req);

struct spdk_json_write_ctx *spdk_jsonrpc_response_ctx(struct spdk_rpc_request *req);
#endif
