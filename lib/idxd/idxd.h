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

#ifndef __IDXD_H__
#define __IDXD_H__

#include "spdk/stdinc.h"

#include "spdk/idxd.h"
#include "spdk/queue.h"
#include "spdk/mmio.h"
#include "spdk/bit_array.h"

#include "idxd_spec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TODO: get the gcc intrinsic to work. */
#define nop() asm volatile ("nop")
static inline void movdir64b(void *dst, const void *src)
{
	asm volatile(".byte 0x66, 0x0f, 0x38, 0xf8, 0x02"
		     : "=m"(*(char *)dst)
		     : "d"(src), "a"(dst));
}

#define IDXD_REGISTER_TIMEOUT_US		50
#define IDXD_DRAIN_TIMEOUT_US			500000

#define WQ_MODE_DEDICATED	1

/* TODO: consider setting the max per batch limit via RPC. */

/* The following sets up a max desc count per batch of 16 */
#define LOG2_WQ_MAX_BATCH	4  /* 2^4 = 16 */
#define DESC_PER_BATCH		(1 << LOG2_WQ_MAX_BATCH)
/* We decide how many batches we want to support based on what max queue
 * depth makes sense resource wise. There is a small price to pay with
 * larger numbers wrt polling for completions.
 */
#define NUM_BATCHES_PER_CHANNEL	0x400
#define MIN_USER_DESC_COUNT	2

#define LOG2_WQ_MAX_XFER	30 /* 2^30 = 1073741824 */
#define WQCFG_NUM_DWORDS	8
#define WQ_PRIORITY_1		1
#define IDXD_MAX_QUEUES		64

/* Each pre-allocated batch structure goes on a per channel list and
 * contains the memory for both user descriptors.
 */
struct idxd_batch {
	struct idxd_hw_desc		*user_desc;
	struct idxd_comp		*user_completions;
	uint32_t			remaining;
	uint8_t				index;
	TAILQ_ENTRY(idxd_batch)		link;
};

struct device_config {
	uint8_t		config_num;
	uint8_t		num_wqs_per_group;
	uint8_t		num_engines_per_group;
	uint8_t		num_groups;
	uint16_t	total_wqs;
	uint16_t	total_engines;
};

struct idxd_comp ;

struct spdk_idxd_io_channel {
	struct spdk_idxd_device		*idxd;
	/* The portal is the address that we write descriptors to for submission. */
	void				*portal;
	uint16_t			ring_size;

	/*
	 * Descriptors and completions share the same index. User descriptors
	 * (those included in a batch) are managed independently from data descriptors
	 * and are located in the batch structure.
	 */
	struct idxd_hw_desc		*desc;
	struct idxd_comp		*completions;

	/* Current list of oustanding completion addresses to poll. */
	TAILQ_HEAD(, idxd_comp)		comp_ctx_oustanding;

	/*
	 * We use one bit array to track ring slots for both
	 * desc and completions.
	 *
	 * TODO: We can get rid of the bit array and just use a uint
	 * to manage flow control as the current implementation saves
	 * enough info in comp_ctx that it doesn't need the index. Keeping
	 * the bit arrays for now as (a) they provide some extra debug benefit
	 * until we have silicon and (b) they may still be needed depending on
	 * polling implementation experiments that we need to run with real silicon.
	 */
	struct spdk_bit_array		*ring_slots;
	uint32_t			max_ring_slots;

	/* Lists of batches, free and in use. */
	TAILQ_HEAD(, idxd_batch)	batch_pool;
	TAILQ_HEAD(, idxd_batch)	batches;

	void				*batch_base;
};

struct pci_dev_id {
	int vendor_id;
	int device_id;
};

struct idxd_group {
	struct spdk_idxd_device	*idxd;
	struct idxd_grpcfg	grpcfg;
	struct pci_dev_id	pcidev;
	int			num_engines;
	int			num_wqs;
	int			id;
	uint8_t			tokens_allowed;
	bool			use_token_limit;
	uint8_t			tokens_reserved;
	int			tc_a;
	int			tc_b;
};

/*
 * This struct wraps the hardware completion record which is 32 bytes in
 * size and must be 32 byte aligned.
 */
struct idxd_comp {
	struct idxd_hw_comp_record	hw;
	void				*cb_arg;
	spdk_idxd_req_cb		cb_fn;
	struct idxd_batch		*batch;
	bool				batch_op;
	struct idxd_hw_desc		*desc;
	uint32_t			index;
	char				pad[3];
	TAILQ_ENTRY(idxd_comp)		link;
};
SPDK_STATIC_ASSERT(sizeof(struct idxd_comp) == 96, "size mismatch");

struct idxd_wq {
	struct spdk_idxd_device		*idxd;
	struct idxd_group		*group;
	union idxd_wqcfg		wqcfg;
};

struct spdk_idxd_device {
	struct spdk_pci_device		*device;
	void				*reg_base;
	void				*portals;
	int				socket_id;
	int				wq_id;

	struct idxd_registers		registers;
	uint32_t			ims_offset;
	uint32_t			msix_perm_offset;
	uint32_t			wqcfg_offset;
	uint32_t			grpcfg_offset;
	uint32_t			perfmon_offset;
	struct idxd_group		*groups;
	struct idxd_wq			*queues;
};

#ifdef __cplusplus
}
#endif

#endif /* __IDXD_H__ */
