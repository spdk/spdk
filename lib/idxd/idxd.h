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
#include "spdk/idxd_spec.h"

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

/* The following sets up a max desc count per batch of 32 */
#define LOG2_WQ_MAX_BATCH	5  /* 2^5 = 32 */
#define DESC_PER_BATCH		(1 << LOG2_WQ_MAX_BATCH)

#define LOG2_WQ_MAX_XFER	30 /* 2^30 = 1073741824 */
#define WQ_PRIORITY_1		1
#define IDXD_MAX_QUEUES		64

/* Each pre-allocated batch structure goes on a per channel list and
 * contains the memory for both user descriptors.
 */
struct idxd_batch {
	struct idxd_hw_desc		*user_desc;
	struct idxd_ops			*user_ops;
	uint64_t			user_desc_addr;
	uint8_t				index;
	struct spdk_idxd_io_channel	*chan;
	bool				transparent;
	TAILQ_ENTRY(idxd_batch)		link;
};

struct device_config {
	uint8_t		config_num;
	uint8_t		num_groups;
	uint16_t	total_wqs;
	uint16_t	total_engines;
};

struct idxd_ops;

struct spdk_idxd_io_channel {
	struct spdk_idxd_device			*idxd;
	/* The portal is the address that we write descriptors to for submission. */
	void					*portal;
	uint32_t				portal_offset;

	/* The currently open batch */
	struct idxd_batch			*batch;

	/*
	 * User descriptors (those included in a batch) are managed independently from
	 * data descriptors and are located in the batch structure.
	 */
	void					*desc_base;
	TAILQ_HEAD(, idxd_ops)			ops_pool;
	/* Current list of outstanding operations to poll. */
	TAILQ_HEAD(op_head, idxd_ops)		ops_outstanding;
	void					*ops_base;

	TAILQ_HEAD(, idxd_batch)		batch_pool;
	void					*batch_base;
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
struct idxd_ops {
	struct idxd_hw_comp_record	hw;
	void				*cb_arg;
	spdk_idxd_req_cb		cb_fn;
	struct idxd_batch		*batch;
	struct idxd_hw_desc		*desc;
	uint32_t			*crc_dst;
	char				pad[8];
	TAILQ_ENTRY(idxd_ops)		link;
};
SPDK_STATIC_ASSERT(sizeof(struct idxd_ops) == 96, "size mismatch");

struct idxd_wq {
	struct spdk_idxd_device		*idxd;
	struct idxd_group		*group;
	union idxd_wqcfg		wqcfg;
};

struct spdk_idxd_impl {
	const char *name;
	void (*set_config)(struct device_config *g_dev_cfg, uint32_t config_num);
	int (*probe)(void *cb_ctx, spdk_idxd_attach_cb attach_cb);
	void (*destruct)(struct spdk_idxd_device *idxd);
	void (*dump_sw_error)(struct spdk_idxd_device *idxd, void *portal);
	char *(*portal_get_addr)(struct spdk_idxd_device *idxd);
	/* It is a workaround for simulator */
	bool (*nop_check)(struct spdk_idxd_device *idxd);

	STAILQ_ENTRY(spdk_idxd_impl) link;
};

struct spdk_idxd_device {
	struct spdk_idxd_impl		*impl;
	void				*portals;
	uint32_t                        socket_id;
	int				wq_id;
	uint32_t			num_channels;
	uint32_t			total_wq_size;
	uint32_t			chan_per_device;
	pthread_mutex_t			num_channels_lock;

	struct idxd_group		*groups;
	struct idxd_wq			*queues;
};

void idxd_impl_register(struct spdk_idxd_impl *impl);

#define SPDK_IDXD_IMPL_REGISTER(name, impl) \
static void __attribute__((constructor)) idxd_impl_register_##name(void) \
{ \
	idxd_impl_register(impl); \
}

#ifdef __cplusplus
}
#endif

#endif /* __IDXD_H__ */
