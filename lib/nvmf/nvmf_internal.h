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

#ifndef __NVMF_INTERNAL_H__
#define __NVMF_INTERNAL_H__

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include "spdk/nvmf_spec.h"
#include "spdk/assert.h"
#include "spdk/queue.h"

#define nvmf_min(a,b) (((a)<(b))?(a):(b))

struct nvmf_request;

typedef void (*nvmf_cb_fn_t)(struct nvmf_request *);

union sgl_shift {
	struct spdk_nvmf_keyed_sgl_descriptor	nvmf_sgl;
	struct spdk_nvme_sgl_descriptor		nvme_sgl;
};
SPDK_STATIC_ASSERT(sizeof(union sgl_shift) == 16, "Incorrect size");

union nvmf_h2c_msg {
	struct spdk_nvmf_capsule_cmd			nvmf_cmd;
	struct spdk_nvme_cmd				nvme_cmd;
	struct spdk_nvmf_fabric_prop_set_cmd		prop_set_cmd;
	struct spdk_nvmf_fabric_prop_get_cmd		prop_get_cmd;
	struct spdk_nvmf_fabric_connect_cmd		connect_cmd;
};
SPDK_STATIC_ASSERT(sizeof(union nvmf_h2c_msg) == 64, "Incorrect size");

union nvmf_c2h_msg {
	struct spdk_nvmf_capsule_rsp			nvmf_rsp;
	struct spdk_nvme_cpl				nvme_cpl;
	struct spdk_nvmf_fabric_prop_set_rsp		prop_set_rsp;
	struct spdk_nvmf_fabric_prop_get_rsp		prop_get_rsp;
	struct spdk_nvmf_fabric_connect_rsp		connect_rsp;
};
SPDK_STATIC_ASSERT(sizeof(union nvmf_c2h_msg) == 16, "Incorrect size");

#define NVMF_H2C_MAX_MSG (sizeof(union nvmf_h2c_msg))
#define NVMF_C2H_MAX_MSG (sizeof(union nvmf_c2h_msg))

#define NVMF_CNTLID_SUBS_SHIFT 8

enum pending_rdma_action {
	NVMF_PENDING_NONE = 0,
	NVMF_PENDING_CONNECT,
	NVMF_PENDING_READ,
	NVMF_PENDING_WRITE,
	NVMF_PENDING_ADMIN,
};

struct nvmf_request {
	struct nvmf_session		*session;
	void				*fabric_tx_ctx;
	void				*fabric_rx_ctx;
	uint16_t			cid;		/* command identifier */
	uint64_t			remote_addr;
	uint32_t			rkey;
	uint32_t			length;
	union nvmf_h2c_msg		*cmd;
	union nvmf_c2h_msg		*rsp;
	enum pending_rdma_action	pending;
	nvmf_cb_fn_t			cb_fn;

	TAILQ_ENTRY(nvmf_request) 	entries;
};

/*
 * Some NVMe command definitions not provided in the nvme_spec.h file
 */

/* read command dword 12 */
struct __attribute__((packed)) nvme_read_cdw12 {
	uint16_t	nlb;		/* number of logical blocks */
	uint16_t	rsvd	: 10;
	uint8_t		prinfo	: 4;	/* protection information field */
	uint8_t		fua	: 1;	/* force unit access */
	uint8_t		lr	: 1;	/* limited retry */
};

/* read command dword 13 */
struct __attribute__((packed)) nvme_read_cdw13 {
	uint8_t		dsm_af	: 4;	/* access frequency */
	uint8_t		dsm_lat	: 2;	/* access latency */
	uint8_t		dsm_seq	: 1;	/* sequential request */
	uint8_t		dsm_inc	: 1;	/* incompressible */
	uint8_t		rsvd[3];
};

struct spdk_nvmf_globals {
	char *authfile;

	char *nodebase;

	pthread_mutex_t mutex;

	int MaxInCapsuleData;
	int MaxSessionsPerSubsystem;
	int MaxQueueDepth;
	int MaxConnectionsPerSession;
	int MaxRecvDataSegmentLength;

	struct rte_mempool *rx_desc_pool;
	struct rte_mempool *tx_desc_pool;
	struct rte_mempool *nvme_request_pool;
	struct rte_mempool *bb_small_pool;
	struct rte_mempool *bb_large_pool;
	uint16_t	   sin_port;
};

void
nvmf_complete_cmd(void *rsp, const struct spdk_nvme_cpl *cmp);

extern struct spdk_nvmf_globals g_nvmf_tgt;

#endif /* __NVMF_INTERNAL_H__ */
