/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
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

enum idxd_dev {
	IDXD_DEV_TYPE_DSA	= 0,
	IDXD_DEV_TYPE_IAA	= 1,
};

/* Each pre-allocated batch structure goes on a per channel list and
 * contains the memory for both user descriptors.
 */
struct idxd_batch {
	struct idxd_hw_desc		*user_desc;
	struct idxd_ops			*user_ops;
	uint64_t			user_desc_addr;
	uint8_t				index;
	uint8_t				refcnt;
	struct spdk_idxd_io_channel	*chan;
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

	bool					pasid_enabled;

	/* The currently open batch */
	struct idxd_batch			*batch;

	/*
	 * User descriptors (those included in a batch) are managed independently from
	 * data descriptors and are located in the batch structure.
	 */
	void					*desc_base;
	STAILQ_HEAD(, idxd_ops)			ops_pool;
	/* Current list of outstanding operations to poll. */
	STAILQ_HEAD(op_head, idxd_ops)		ops_outstanding;
	void					*ops_base;

	TAILQ_HEAD(, idxd_batch)		batch_pool;
	void					*batch_base;
};

struct pci_dev_id {
	int vendor_id;
	int device_id;
};

/*
 * This struct wraps the hardware completion record which is 32 bytes in
 * size and must be 32 byte aligned.
 */
struct idxd_ops {
	union {
		struct dsa_hw_comp_record	hw;
		struct iaa_hw_comp_record	iaa_hw;
	};
	void				*cb_arg;
	spdk_idxd_req_cb		cb_fn;
	struct idxd_batch		*batch;
	struct idxd_hw_desc		*desc;
	union {
		uint32_t		*crc_dst;
		uint32_t		*output_size;
	};
	struct idxd_ops			*parent;
	uint32_t			count;
	STAILQ_ENTRY(idxd_ops)		link;
};
SPDK_STATIC_ASSERT(sizeof(struct idxd_ops) == 128, "size mismatch");

struct spdk_idxd_impl {
	const char *name;
	int (*probe)(void *cb_ctx, spdk_idxd_attach_cb attach_cb,
		     spdk_idxd_probe_cb probe_cb);
	void (*destruct)(struct spdk_idxd_device *idxd);
	void (*dump_sw_error)(struct spdk_idxd_device *idxd, void *portal);
	char *(*portal_get_addr)(struct spdk_idxd_device *idxd);

	STAILQ_ENTRY(spdk_idxd_impl) link;
};

struct spdk_idxd_device {
	struct spdk_idxd_impl		*impl;
	void				*portal;
	uint32_t			socket_id;
	uint32_t			num_channels;
	uint32_t			total_wq_size;
	uint32_t			chan_per_device;
	pthread_mutex_t			num_channels_lock;
	bool				pasid_enabled;
	enum idxd_dev			type;
	struct iaa_aecs			*aecs;
	uint64_t			aecs_addr;
	uint32_t			version;
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
