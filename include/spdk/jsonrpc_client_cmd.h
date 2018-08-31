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
 * SPDK JSON-RPC commands
 */

#ifndef SPDK_JSONRPC_CLIENT_CMD_H_
#define SPDK_JSONRPC_CLIENT_CMD_H_

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_jsonrpc_client_conn;

struct spdk_jsonrpc_client_conn *spdk_jsonrpc_client_connect(const char *listen_addr);
void spdk_jsonrpc_client_close(struct spdk_jsonrpc_client_conn *conn);


/**
 * This function can be used to check jsonrpc workability
 *
 * \param rpc_sock_addr Connecting address.
 *
 * \return 0 on success.
 */
int spdk_rpc_client_get_rpc_methods(const char *rpc_sock_addr);
int _spdk_rpc_client_get_rpc_methods(struct spdk_jsonrpc_client_conn *conn);

enum spdk_rpc_nvme_cmd_type {
	NVME_CMD_ADMIN = 0,
	NVME_CMD_IO,
};

/**
 * Send NVMe passthrough cmd
 *
 * \param rpc_sock_addr Connecting address.
 * \param device_name Name of the operating NVMe devices
 * \param cmd_type Type of nvme cmd. Valid values are: 0 for admin, 1 for io
 * \param data_direction Direction of data transfer
 * \param cmdbuf NVMe command buffer
 * \param cmdbuf_len length of NVMe command buffer, should be 64
 * \param data Data transferring between controller and host
 * \param data_len length of data buffer
 * \param metadata Metadata transferring between controller and host
 * \param metadata_len length of metadata buffer
 * \param timeout_ms Command execution timeout value, in milliseconds,  if 0, don't track timeout
 * \param result CDW0 of nvme completion queue entry
 *
 * \return 0 on success.
 */
int spdk_rpc_client_nvme_cmd(const char *rpcsock_addr,
			     const char *device_name, int cmd_type, int data_direction,
			     const char *cmdbuf, size_t cmdbuf_len,
			     char *data, size_t data_len, char *metadata, size_t metadata_len,
			     uint32_t timeout_ms, uint32_t *result);
int _spdk_rpc_client_nvme_cmd(struct spdk_jsonrpc_client_conn *conn,
			      const char *device_name, int cmd_type, int data_direction,
			      const char *cmdbuf, size_t cmdbuf_len,
			      char *data, size_t data_len, char *metadata, size_t metadata_len,
			      uint32_t timeout_ms, uint32_t *result);

#ifdef __cplusplus
}
#endif

#endif
