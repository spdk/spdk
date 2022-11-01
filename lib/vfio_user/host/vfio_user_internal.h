/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation. All rights reserved.
 */

#ifndef _VFIO_INTERNAL_H
#define _VFIO_INTERNAL_H

#include <linux/vfio.h>
#include "spdk/vfio_user_spec.h"

#define VFIO_USER_MAJOR_VER			0
#define VFIO_USER_MINOR_VER			1

/* Maximum memory regions supported */
#define VFIO_MAXIMUM_MEMORY_REGIONS		128
/* Maximum sparse memory regions in one BAR region */
#define VFIO_MAXIMUM_SPARSE_MMAP_REGIONS	8

struct vfio_memory_region {
	uint64_t iova;
	uint64_t size; /* bytes */
	uint64_t vaddr;
	uint64_t offset;
	int fd;
	TAILQ_ENTRY(vfio_memory_region) link;
};

struct vfio_sparse_mmaps {
	void *mem;
	uint64_t offset;
	size_t size;
};

struct vfio_pci_region {
	uint64_t offset;
	size_t size;
	uint64_t flags;
	uint32_t nr_mmaps;
	struct vfio_sparse_mmaps mmaps[VFIO_MAXIMUM_SPARSE_MMAP_REGIONS];
};

struct vfio_device {
	int fd;

	char name[64];
	char path[PATH_MAX];

	TAILQ_ENTRY(vfio_device) link;

	/* PCI Regions */
	uint32_t pci_regions;
	struct vfio_pci_region regions[VFIO_PCI_NUM_REGIONS + 1];
	uint64_t flags;

	struct spdk_mem_map *map;
	TAILQ_HEAD(, vfio_memory_region) mrs_head;
	uint32_t nr_mrs;
};

int vfio_user_dev_setup(struct vfio_device *dev);
int vfio_user_get_dev_info(struct vfio_device *dev, struct vfio_user_device_info *dev_info,
			   size_t buf_len);
int vfio_user_get_dev_region_info(struct vfio_device *dev, struct vfio_region_info *region_info,
				  size_t buf_len, int *fds, int num_fds);
int vfio_user_dev_dma_map_unmap(struct vfio_device *dev, struct vfio_memory_region *mr, bool map);
int vfio_user_dev_mmio_access(struct vfio_device *dev, uint32_t index, uint64_t offset, size_t len,
			      void *buf, bool is_write);
/* For fuzzing only */
int vfio_user_dev_send_request(struct vfio_device *dev, enum vfio_user_command command,
			       void *arg, size_t arg_len, size_t buf_len, int *fds,
			       int max_fds);

#endif
