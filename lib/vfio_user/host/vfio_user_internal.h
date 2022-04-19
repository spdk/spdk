/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
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

#endif
