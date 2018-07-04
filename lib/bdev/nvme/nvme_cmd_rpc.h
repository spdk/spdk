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

#include "spdk/stdinc.h"

struct nvme_cmd_rpc_ctx;

struct spdk_nvme_cmd_rpc_operator {
	/* Search whether name specified device is mastered by operator */
	void *(*dev_hit_func)(const char *name);
	/* Process admin type nvme-cmd */
	int (*admin_cmd_func)(void *dev, const struct spdk_nvme_cmd *cmd, void *buf,
			      size_t nbytes, uint32_t timeout_ms, struct nvme_cmd_rpc_ctx *ctx);
	/* Process passthrough io type nvme-cmd */
	int (*io_raw_cmd_func)(void *dev, const struct spdk_nvme_cmd *cmd, void *buf,
			       size_t nbytes, void *md_buf, size_t md_len, uint32_t timeout_ms,
			       struct nvme_cmd_rpc_ctx *ctx);

	TAILQ_ENTRY(spdk_nvme_cmd_rpc_operator) tailq;
};

TAILQ_HEAD(spdk_nvme_cmd_rpc_operator_list, spdk_nvme_cmd_rpc_operator);
extern struct spdk_nvme_cmd_rpc_operator_list g_nvme_cmd_rpc_operators;
void spdk_add_nvme_cmd_rpc_operator(struct spdk_nvme_cmd_rpc_operator *operator);

/**
 * \brief Register a new spdk_nvme_cmd_rpc_operator
 */
#define SPDK_NVME_CMD_RPC_OPERATOR_REGISTER(_name) \
	__attribute__((constructor)) static void _name ## _register(void)	\
	{									\
		spdk_add_nvme_cmd_rpc_operator(&_name);				\
	}

void spdk_nvme_cmd_rpc_complete(struct nvme_cmd_rpc_ctx *ctx, uint32_t status, uint32_t result);
