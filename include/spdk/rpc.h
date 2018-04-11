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

#ifndef SPDK_RPC_CONFIG_H_
#define SPDK_RPC_CONFIG_H_

#include "spdk/stdinc.h"

#include "spdk/jsonrpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start listening for RPC connections.
 *
 * \param listen_addr Listening address.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_rpc_listen(const char *listen_addr);

/**
 * Poll the RPC server.
 */
void spdk_rpc_accept(void);

/**
 * Stop listening for RPC connections.
 */
void spdk_rpc_close(void);

/**
 * Function signature for RPC request handlers.
 *
 * \param request RPC request to handle.
 * \param params Parameters associated with the RPC request.
 */
typedef void (*spdk_rpc_method_handler)(struct spdk_jsonrpc_request *request,
					const struct spdk_json_val *params);

/**
 * Register an RPC method.
 *
 * \param method Name for the registered method.
 * \param func Function registered for this method to handle the RPC request.
 */
void spdk_rpc_register_method(const char *method, spdk_rpc_method_handler func);

#define SPDK_RPC_REGISTER(method, func) \
static void __attribute__((constructor)) rpc_register_##func(void) \
{ \
	spdk_rpc_register_method(method, func); \
}

#ifdef __cplusplus
}
#endif

#endif
