/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

#ifndef _VFIO_USER_SPEC_H
#define _VFIO_USER_SPEC_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

enum vfio_user_command {
	VFIO_USER_VERSION			= 1,
	VFIO_USER_DMA_MAP			= 2,
	VFIO_USER_DMA_UNMAP			= 3,
	VFIO_USER_DEVICE_GET_INFO		= 4,
	VFIO_USER_DEVICE_GET_REGION_INFO	= 5,
	VFIO_USER_DEVICE_GET_REGION_IO_FDS	= 6,
	VFIO_USER_DEVICE_GET_IRQ_INFO		= 7,
	VFIO_USER_DEVICE_SET_IRQS		= 8,
	VFIO_USER_REGION_READ			= 9,
	VFIO_USER_REGION_WRITE			= 10,
	VFIO_USER_DMA_READ			= 11,
	VFIO_USER_DMA_WRITE			= 12,
	VFIO_USER_DEVICE_RESET			= 13,
	VFIO_USER_DIRTY_PAGES			= 14,
	VFIO_USER_MAX,
};

enum vfio_user_message_type {
	VFIO_USER_MESSAGE_COMMAND	= 0,
	VFIO_USER_MESSAGE_REPLY		= 1,
};

#define VFIO_USER_FLAGS_NO_REPLY	(0x1)

struct vfio_user_header {
	uint16_t	msg_id;
	uint16_t	cmd;
	uint32_t	msg_size;
	struct {
		uint32_t	type     : 4;
#define VFIO_USER_F_TYPE_COMMAND	0
#define VFIO_USER_F_TYPE_REPLY		1
		uint32_t	no_reply : 1;
		uint32_t	error    : 1;
		uint32_t	resvd    : 26;
	} flags;
	uint32_t	error_no;
} __attribute__((packed));

struct vfio_user_version {
	uint16_t	major;
	uint16_t	minor;
	uint8_t		data[];
} __attribute__((packed));

/*
 * Similar to vfio_device_info, but without caps (yet).
 */
struct vfio_user_device_info {
	uint32_t	argsz;
	/* VFIO_DEVICE_FLAGS_* */
	uint32_t	flags;
	uint32_t	num_regions;
	uint32_t	num_irqs;
} __attribute__((packed));

/* based on struct vfio_bitmap */
struct vfio_user_bitmap {
	uint64_t	pgsize;
	uint64_t	size;
	char		data[];
} __attribute__((packed));

/* based on struct vfio_iommu_type1_dma_map */
struct vfio_user_dma_map {
	uint32_t	argsz;
#define VFIO_USER_F_DMA_REGION_READ	(1 << 0)
#define VFIO_USER_F_DMA_REGION_WRITE	(1 << 1)
	uint32_t	flags;
	uint64_t	offset;
	uint64_t	addr;
	uint64_t	size;
} __attribute__((packed));

/* based on struct vfio_iommu_type1_dma_unmap */
struct vfio_user_dma_unmap {
	uint32_t	argsz;
#ifndef VFIO_DMA_UNMAP_FLAG_GET_DIRTY_BITMAP
#define VFIO_DMA_UNMAP_FLAG_GET_DIRTY_BITMAP	(1 << 0)
#endif
	uint32_t	flags;
	uint64_t	addr;
	uint64_t	size;
	struct vfio_user_bitmap	bitmap[];
};

struct vfio_user_region_access {
	uint64_t	offset;
	uint32_t	region;
	uint32_t	count;
	uint8_t		data[];
} __attribute__((packed));

struct vfio_user_dma_region_access {
	uint64_t	addr;
	uint64_t	count;
	uint8_t		data[];
} __attribute__((packed));

struct vfio_user_irq_info {
	uint32_t	subindex;
} __attribute__((packed));

/* based on struct vfio_iommu_type1_dirty_bitmap_get */
struct vfio_user_bitmap_range {
	uint64_t	iova;
	uint64_t	size;
	struct vfio_user_bitmap	bitmap;
} __attribute__((packed));


#ifdef __cplusplus
}
#endif

#endif
