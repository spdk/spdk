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

#define DEFAULT_BB_SIZE (128 * 1024)

/*
 * NVMf target supports a maximum transfer size that is equal to
 * a single allocated bounce buffer per request.
 */
#define SPDK_NVMF_MAX_RECV_DATA_TRANSFER_SIZE  DEFAULT_BB_SIZE

#define SPDK_NVMF_DEFAULT_NUM_SESSIONS_PER_LCORE 1
#define SPDK_NVMF_DEFAULT_NODEBASE "nqn.2016-06.io.spdk"
#define SPDK_NVMF_DEFAULT_MAX_QUEUE_DEPTH 128
#define SPDK_NVMF_DEFAULT_MAX_CONNECTIONS_PER_SESSION 4
#define SPDK_NVMF_DEFAULT_SIN_PORT ((uint16_t)4420)

#define OBJECT_NVMF_IO				0x30

#define TRACE_GROUP_NVMF			0x3
#define TRACE_NVMF_IO_START			SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x0)
#define TRACE_RDMA_READ_START			SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x1)
#define TRACE_RDMA_WRITE_START			SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x2)
#define TRACE_RDMA_READ_COMPLETE      		SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x3)
#define TRACE_RDMA_WRITE_COMPLETE		SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x4)
#define TRACE_NVMF_LIB_READ_START		SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x5)
#define TRACE_NVMF_LIB_WRITE_START		SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x6)
#define TRACE_NVMF_LIB_COMPLETE			SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x7)
#define TRACE_NVMF_IO_COMPLETE			SPDK_TPOINT_ID(TRACE_GROUP_NVMF, 0x8)

#define NVMF_CNTLID_SUBS_SHIFT 8

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
	char *nodebase;

	pthread_mutex_t mutex;

	int MaxQueueDepth;
	int MaxConnectionsPerSession;

	uint16_t	   sin_port;
};

int nvmf_tgt_init(char *nodebase, int max_queue_depth, int max_conn_per_sess);

extern struct spdk_nvmf_globals g_nvmf_tgt;

#endif /* __NVMF_INTERNAL_H__ */
