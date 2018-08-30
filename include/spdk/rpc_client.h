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
 * SPDK RPC-Client C API
 */

#ifndef SPDK_RPC_CLIENT_H_
#define SPDK_RPC_CLIENT_H_

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_rpc_client_conn;

/**
 * Start connection the RPC server.
 *
 * \param rpc_sock_addr RPC socket address.
 *
 * \return RPC connection on success, NULL on failure.
 */
struct spdk_rpc_client_conn *spdk_rpc_client_connect(const char *rpc_sock_addr);

/**
 * Close the RPC connection.
 *
 * \param conn RPC connection.
 */
void spdk_rpc_client_close(struct spdk_rpc_client_conn *conn);

/**
 * This function can be used to check whether one rpc method is supported
 * by the running SPDK app.
 *
 * \param rpc_sock_addr Connecting address.
 * \param method_name Name of rpc method.
 *
 * \return 0 on success.
 */
int spdk_rpc_client_check_rpc_method(const char *rpc_sock_addr, char *method_name);
int _spdk_rpc_client_check_rpc_method(struct spdk_rpc_client_conn *_conn, char *method_name);

#ifdef __cplusplus
}
#endif

#endif
