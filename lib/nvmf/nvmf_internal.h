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

#include "spdk/stdinc.h"

#include "spdk/nvmf_spec.h"
#include "spdk/assert.h"
#include "spdk/queue.h"
#include "spdk/util.h"

#define SPDK_NVMF_DEFAULT_NUM_SESSIONS_PER_LCORE 1

struct spdk_nvmf_tgt {
	uint16_t				max_queue_depth;
	uint16_t				max_queues_per_session;
	uint32_t				in_capsule_data_size;
	uint32_t				max_io_size;
	uint64_t				discovery_genctr;
	TAILQ_HEAD(, spdk_nvmf_subsystem)	subsystems;
	struct spdk_nvmf_discovery_log_page	*discovery_log_page;
	size_t					discovery_log_page_size;
	TAILQ_HEAD(, spdk_nvmf_listen_addr)	listen_addrs;
	uint32_t				current_subsystem_id;
};

extern struct spdk_nvmf_tgt g_nvmf_tgt;

struct spdk_nvmf_listen_addr *spdk_nvmf_listen_addr_create(const char *trname, const char *traddr,
		const char *trsvcid);
void spdk_nvmf_listen_addr_destroy(struct spdk_nvmf_listen_addr *addr);
void spdk_nvmf_listen_addr_cleanup(struct spdk_nvmf_listen_addr *addr);

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

#endif /* __NVMF_INTERNAL_H__ */
