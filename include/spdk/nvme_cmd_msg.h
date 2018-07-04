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
 * NVMe Passthrough CMD message structs
 */

#ifndef SPDK_NVME_CMD_MSG_H
#define SPDK_NVME_CMD_MSG_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NVME_CMD_RPC_REQ_HEAD_LEN (sizeof(uint16_t) * 2 + sizeof(uint32_t) * 4)
#define NVME_CMD_RPC_RESP_HEAD_LEN (sizeof(uint32_t) * 4)

enum spdk_nvme_cmd_rpc_type {
	NVME_ADMIN_CMD = 0,
	NVME_IO_RAW_CMD,
	NVME_IO_CMD,
};

struct spdk_nvme_passthru_rpc_cmd {
	uint8_t		opcode;
	uint8_t		flags;
	uint16_t	rsvd1;
	uint32_t	nsid;
	uint32_t	cdw2;
	uint32_t	cdw3;
	uint64_t	metadata;
	uint64_t	addr;
	uint32_t	metadata_len;
	uint32_t	data_len;
	uint32_t	cdw10;
	uint32_t	cdw11;
	uint32_t	cdw12;
	uint32_t	cdw13;
	uint32_t	cdw14;
	uint32_t	cdw15;
};

struct spdk_nvme_cmd_rpc_req {
	uint16_t	cmd_type;
	uint16_t	data_direction;
	uint32_t	timeout_ms;
	uint32_t	cmdbuf_len;
	uint32_t	data_len;
	uint32_t	md_len;

	void		*cmdbuf;
	char		*data;
	char		*md;
};

struct spdk_nvme_cmd_rpc_resp {
	uint32_t	status;
	uint32_t	result;
	uint32_t	data_len;
	uint32_t	md_len;

	char		*data;
	char		*md;
};

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVME_CMD_MSG_H */
