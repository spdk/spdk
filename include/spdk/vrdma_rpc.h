/*
 *   Copyright © 2022 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
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

#ifndef _VRDMA_RPC_H
#define _VRDMA_RPC_H
#include <stdio.h>
#include <stdint.h>
#include "spdk/vrdma.h"
#include "spdk/jsonrpc.h"
#include "spdk/rpc.h"

#define VRDMA_RPC_DEFAULT_PORT	"5262"
#define VRDMA_RPC_SELECT_INTERVAL	4000 /* 4ms */
/* 1s connections timeout */
#define VRDMA_RPC_CLIENT_CONNECT_TIMEOUT_US (10U * 1000U * 1000U)
/* 30s timeout */
#define VRDMA_RPC_CLIENT_REQUEST_TIMEOUT_US (30U * 1000U * 1000U)
#define VRDMA_RPC_UNIX_PATH_MAX	108
#define VRDMA_RPC_LISTEN_LOCK_PATH_SIZE (VRDMA_RPC_UNIX_PATH_MAX + sizeof(".lock"))
#define VRDMA_RPC_IP_LEN 32
#define VRDMA_RPC_MKEY_TIMEOUT_S  (2U) /* 2s */

struct spdk_jsonrpc_client_request {
	/* Total space allocated for send_buf */
	size_t send_buf_size;

	/* Number of bytes used in send_buf (<= send_buf_size) */
	size_t send_len;

	size_t send_offset;

	uint8_t *send_buf;

	size_t send_total_len;
	uint32_t request_id;
	STAILQ_ENTRY(spdk_jsonrpc_client_request) stailq;
};

struct spdk_vrdma_rpc_client;
typedef void (*vrdma_client_resp_handler)(struct spdk_vrdma_rpc_client *,
				    struct spdk_jsonrpc_client_response *);

struct spdk_vrdma_rpc_method {
	const char *name;
	spdk_rpc_method_handler func;
	vrdma_client_resp_handler resp_cb;
	SLIST_ENTRY(spdk_vrdma_rpc_method) slist;
};

struct spdk_vrdma_rpc_client {
	struct spdk_jsonrpc_client *client_conn;
	struct spdk_poller *client_conn_poller;
	vrdma_client_resp_handler client_resp_cb;
	uint64_t timeout;/* Timeout for current RPC client action. */
};

struct spdk_vrdma_rpc_server {
	struct sockaddr_un rpc_listen_addr_unix;
	char rpc_lock_path[VRDMA_RPC_LISTEN_LOCK_PATH_SIZE];
	int rpc_lock_fd;
	uint32_t rpc_state;
	struct spdk_jsonrpc_server *rpc_server;
	struct spdk_poller *rpc_poller;
};

enum spdk_vrdma_rpc_type {
    SPDK_VRDMA_RPC_TYPE_INVALID,
    SPDK_VRDMA_RPC_TYPE_SERVER,
    SPDK_VRDMA_RPC_TYPE_CLIENT,
    SPDK_VRDMA_RPC_TYPE_MAX
};

struct spdk_vrdma_rpc {
	char node_ip[VRDMA_RPC_IP_LEN];
	char node_rip[VRDMA_RPC_IP_LEN];/* Remote node ip */
    struct spdk_vrdma_rpc_server srv;
	struct spdk_vrdma_rpc_client client;
};
extern struct spdk_vrdma_rpc g_vrdma_rpc;
extern uint64_t g_node_ip;
extern uint64_t g_node_rip;

enum spdk_vrdma_rpc_qp_state {
	SPDK_VRDMA_RPC_QP_WAIT_RQPN,
	SPDK_VRDMA_RPC_QP_READY,
	SPDK_VRDMA_RPC_QP_DESTROYED,
};

struct spdk_vrdma_rpc_qp_msg {
    char *emu_manager;
	uint32_t request_id;
    uint64_t sf_mac;
    uint32_t bk_qpn;
    uint32_t qp_state;
    uint8_t  mqp_idx;
    union ibv_gid local_tgid;
    union ibv_gid remote_tgid;
    union ibv_gid local_mgid;
    union ibv_gid remote_mgid;
};

struct spdk_vrdma_rpc_mkey_attr {
	uint32_t request_id;
	uint64_t gid_ip;
    uint32_t vqpn;
    uint32_t vkey;
	uint32_t mkey;
};

struct spdk_vrdma_rpc_mkey_msg {
	struct spdk_vrdma_rpc_mkey_attr mkey_attr;
};

int spdk_vrdma_rpc_send_qp_msg(const char *addr,
                struct spdk_vrdma_rpc_qp_msg *msg);
int spdk_vrdma_rpc_send_mkey_msg(const char *addr,
                struct spdk_vrdma_rpc_mkey_msg *msg);
#endif
