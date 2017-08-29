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

#include "spdk/likely.h"
#include "spdk/nvmf.h"
#include "spdk/nvmf_spec.h"
#include "spdk/assert.h"
#include "spdk/queue.h"
#include "spdk/util.h"

#define SPDK_NVMF_DEFAULT_NUM_CTRLRS_PER_LCORE 1

struct spdk_nvmf_tgt {
	struct spdk_nvmf_tgt_opts		opts;

	struct spdk_thread			*master_thread;

	uint16_t				next_cntlid;
	uint64_t				discovery_genctr;
	TAILQ_HEAD(, spdk_nvmf_subsystem)	subsystems;
	struct spdk_nvmf_discovery_log_page	*discovery_log_page;
	size_t					discovery_log_page_size;
	uint32_t				current_subsystem_id;
	TAILQ_HEAD(, spdk_nvmf_transport)	transports;
};

struct spdk_nvmf_host {
	char				*nqn;
	TAILQ_ENTRY(spdk_nvmf_host)	link;
};

struct spdk_nvmf_listener {
	struct spdk_nvme_transport_id	trid;
	TAILQ_ENTRY(spdk_nvmf_listener)	link;
};

struct spdk_nvmf_transport_poll_group {
	struct spdk_nvmf_transport			*transport;
	TAILQ_ENTRY(spdk_nvmf_transport_poll_group)	link;
};

struct spdk_nvmf_poll_group {
	TAILQ_HEAD(, spdk_nvmf_transport_poll_group) tgroups;

	TAILQ_ENTRY(spdk_nvmf_poll_group)	link;
};

struct spdk_nvmf_ns {
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	uint32_t id;
	bool allocated;
};

struct spdk_nvmf_subsystem {
	uint32_t id;
	char subnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
	enum spdk_nvmf_subtype subtype;
	bool is_removed;

	struct spdk_nvmf_tgt			*tgt;

	char sn[SPDK_NVME_CTRLR_SN_LEN + 1];

	/* Array of namespaces of size max_nsid indexed by nsid - 1 */
	struct spdk_nvmf_ns			*ns;
	uint32_t 				max_nsid;
	uint32_t				num_allocated_nsid;

	TAILQ_HEAD(, spdk_nvmf_ctrlr)		ctrlrs;

	TAILQ_HEAD(, spdk_nvmf_host)		hosts;

	TAILQ_HEAD(, spdk_nvmf_listener)	listeners;

	TAILQ_ENTRY(spdk_nvmf_subsystem)	entries;
};

uint16_t spdk_nvmf_tgt_gen_cntlid(struct spdk_nvmf_tgt *tgt);

struct spdk_nvmf_transport *spdk_nvmf_tgt_get_transport(struct spdk_nvmf_tgt *tgt,
		enum spdk_nvme_transport_type);

struct spdk_nvmf_poll_group *spdk_nvmf_poll_group_create(
	struct spdk_nvmf_tgt *tgt);

void spdk_nvmf_poll_group_destroy(struct spdk_nvmf_poll_group *group);

int spdk_nvmf_poll_group_add(struct spdk_nvmf_poll_group *group,
			     struct spdk_nvmf_qpair *qpair);

int spdk_nvmf_poll_group_remove(struct spdk_nvmf_poll_group *group,
				struct spdk_nvmf_qpair *qpair);

int spdk_nvmf_poll_group_poll(struct spdk_nvmf_poll_group *group);

static inline struct spdk_nvmf_ns *
_spdk_nvmf_subsystem_get_ns(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid)
{
	struct spdk_nvmf_ns *ns;

	/* NOTE: This implicitly also checks for 0, since 0 - 1 wraps around to UINT32_MAX. */
	if (spdk_unlikely(nsid - 1 >= subsystem->max_nsid)) {
		return NULL;
	}

	ns = &subsystem->ns[nsid - 1];
	if (!ns->allocated) {
		return NULL;
	}

	return ns;
}

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
