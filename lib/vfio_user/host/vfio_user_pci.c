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

/*
 * vfio-user transport for PCI devices.
 */

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/env.h"
#include "spdk/queue.h"
#include "spdk/util.h"
#include "spdk/vfio_user_pci.h"

#include "vfio_user_internal.h"

static uint32_t g_vfio_dev_id;

int
spdk_vfio_user_pci_bar_access(struct vfio_device *dev, uint32_t index, uint64_t offset,
			      size_t len, void *buf, bool is_write)
{
	struct vfio_pci_region *region = &dev->regions[index];
	uint32_t i;

	if (offset + len > region->size) {
		return -EINVAL;
	}

	if (!region->nr_mmaps || (offset < region->mmaps[0].offset)) {
		return vfio_user_dev_mmio_access(dev, index, offset, len, buf, is_write);
	}

	/* SPARSE MMAP */
	for (i = 0; i < region->nr_mmaps; i++) {
		if ((offset >= region->mmaps[i].offset) &&
		    (offset + len <= region->mmaps[i].offset + region->mmaps[i].size)) {
			assert(region->mmaps[i].mem != NULL);
			void *bar_addr = region->mmaps[i].mem + offset;
			if (is_write) {
				memcpy(bar_addr, buf, len);
			} else {
				memcpy(buf, bar_addr, len);
			}
			return 0;
		}
	}

	return -EFAULT;
}

static int
vfio_add_mr(struct vfio_device *dev, struct vfio_memory_region *mr)
{
	if (dev->nr_mrs == VFIO_MAXIMUM_MEMORY_REGIONS) {
		SPDK_ERRLOG("Maximum supported memory regions %d\n", VFIO_MAXIMUM_MEMORY_REGIONS);
		return -EINVAL;
	}

	TAILQ_INSERT_TAIL(&dev->mrs_head, mr, link);
	dev->nr_mrs++;

	SPDK_DEBUGLOG(vfio_pci, "Add memory region: FD %d, VADDR 0x%lx, IOVA 0x%lx, Size 0x%lx\n",
		      mr->fd, mr->vaddr, mr->iova, mr->size);

	return 0;
}

static struct vfio_memory_region *
vfio_get_mr(struct vfio_device *dev, uint64_t addr, size_t len)
{
	struct vfio_memory_region *mr, *tmp_mr;

	if (dev->nr_mrs == 0) {
		return false;
	}

	TAILQ_FOREACH_SAFE(mr, &dev->mrs_head, link, tmp_mr) {
		if ((mr->vaddr == addr) || (mr->iova == addr)) {
			return mr;
		}
	}

	return false;
}

static void
vfio_remove_mr(struct vfio_device *dev, uint64_t addr, size_t len)
{
	struct vfio_memory_region *mr, *tmp_mr;

	TAILQ_FOREACH_SAFE(mr, &dev->mrs_head, link, tmp_mr) {
		if ((mr->vaddr == addr) || (mr->iova == addr)) {
			SPDK_DEBUGLOG(vfio_pci, "Remove memory region: FD %d, VADDR 0x%lx, IOVA 0x%lx, Size 0x%lx\n",
				      mr->fd, mr->vaddr, mr->iova, mr->size);
			TAILQ_REMOVE(&dev->mrs_head, mr, link);
			assert(dev->nr_mrs > 0);
			dev->nr_mrs--;
			free(mr);
			return;
		}
	}
}

static int
vfio_mr_map_notify(void *cb_ctx, struct spdk_mem_map *map,
		   enum spdk_mem_map_notify_action action,
		   void *vaddr, size_t size)
{
	int ret;
	struct vfio_device *dev = cb_ctx;
	struct vfio_memory_region *mr;
	uint64_t offset;

	mr = vfio_get_mr(dev, (uint64_t)vaddr, size);
	if (action == SPDK_MEM_MAP_NOTIFY_UNREGISTER) {
		if (!mr) {
			SPDK_ERRLOG("Memory region VADDR %p doesn't exist\n", vaddr);
			return -EEXIST;
		}

		ret = vfio_user_dev_dma_map_unmap(dev, mr, false);
		/* remove the memory region */
		vfio_remove_mr(dev, (uint64_t)vaddr, size);
		return ret;
	}

	/* SPDK_MEM_MAP_NOTIFY_REGISTER */
	if (mr != NULL) {
		SPDK_ERRLOG("Memory region VADDR 0x%lx already exist\n", mr->vaddr);
		return -EEXIST;
	}

	mr = calloc(1, sizeof(*mr));
	if (mr == NULL) {
		return -ENOMEM;
	}
	mr->vaddr = (uint64_t)(uintptr_t)vaddr;
	mr->iova = mr->vaddr;
	mr->size = size;
	mr->fd = spdk_mem_get_fd_and_offset(vaddr, &offset);
	if (mr->fd < 0) {
		SPDK_ERRLOG("Error to get the memory map offset\n");
		free(mr);
		return -EFAULT;
	}
	mr->offset = offset;

	ret = vfio_add_mr(dev, mr);
	if (ret) {
		free(mr);
		return ret;
	}

	return vfio_user_dev_dma_map_unmap(dev, mr, true);
}

static int
vfio_device_dma_map(struct vfio_device *device)
{
	const struct spdk_mem_map_ops vfio_map_ops = {
		.notify_cb = vfio_mr_map_notify,
		.are_contiguous = NULL,
	};

	device->map = spdk_mem_map_alloc((uint64_t)NULL, &vfio_map_ops, device);
	if (device->map == NULL) {
		SPDK_ERRLOG("Failed to allocate memory map structure\n");
		return -EFAULT;
	}

	return 0;
}

static struct vfio_info_cap_header *
vfio_device_get_info_cap(struct vfio_region_info *info, int cap)
{
	struct vfio_info_cap_header *h;
	size_t offset;

	if ((info->flags & VFIO_REGION_INFO_FLAG_CAPS) == 0) {
		return NULL;
	}

	offset = info->cap_offset;
	while (offset != 0) {
		h = (struct vfio_info_cap_header *)((uintptr_t)info + offset);
		if (h->id == cap) {
			return h;
		}
		offset = h->next;
	}

	return NULL;
}

static int
vfio_device_setup_sparse_mmaps(struct vfio_device *device, int index,
			       struct vfio_region_info *info, int *fds)
{
	struct vfio_info_cap_header *hdr;
	struct vfio_region_info_cap_sparse_mmap *sparse;
	struct vfio_pci_region *region = &device->regions[index];
	uint32_t i, j = 0;
	int prot = 0;

	hdr = vfio_device_get_info_cap(info, VFIO_REGION_INFO_CAP_SPARSE_MMAP);
	if (!hdr) {
		SPDK_NOTICELOG("Device doesn't have sparse mmap\n");
		return -EEXIST;
	}

	sparse = SPDK_CONTAINEROF(hdr, struct vfio_region_info_cap_sparse_mmap, header);
	for (i = 0; i < sparse->nr_areas; i++) {
		if (sparse->areas[i].size) {
			region->mmaps[j].offset = sparse->areas[i].offset;
			region->mmaps[j].size = sparse->areas[i].size;
			prot |= info->flags & VFIO_REGION_INFO_FLAG_READ ? PROT_READ : 0;
			prot |= info->flags & VFIO_REGION_INFO_FLAG_WRITE ? PROT_WRITE : 0;
			if (*fds) {
				region->mmaps[j].mem = mmap(NULL, region->mmaps[j].size, prot, MAP_SHARED,
							    fds[i], region->offset + region->mmaps[j].offset);
				if (region->mmaps[j].mem == MAP_FAILED) {
					SPDK_ERRLOG("Device SPARSE MMAP failed\n");
					return -EIO;
				}
			} else {
				SPDK_DEBUGLOG(vfio_pci, "No valid fd, skip mmap for bar %d region %u\n", index, i);
			}
			SPDK_DEBUGLOG(vfio_pci, "Sparse region %u, Size 0x%llx, Offset 0x%llx, Map addr %p\n",
				      i, sparse->areas[i].size, sparse->areas[i].offset,
				      region->mmaps[j].mem);
			j++;
		}
	}
	device->regions[index].nr_mmaps = j;

	return 0;
}

static int
vfio_device_map_region(struct vfio_device *device, struct vfio_pci_region *region, int fd)
{
	int prot = 0;

	prot |= region->flags & VFIO_REGION_INFO_FLAG_READ ? PROT_READ : 0;
	prot |= region->flags & VFIO_REGION_INFO_FLAG_WRITE ? PROT_WRITE : 0;

	region->mmaps[0].offset = 0;
	region->mmaps[0].size = region->size;

	region->mmaps[0].mem = mmap(NULL, region->size, prot, MAP_SHARED,
				    fd, region->offset);
	if (region->mmaps[0].mem == MAP_FAILED) {
		SPDK_ERRLOG("Device Region MMAP failed\n");
		return -EFAULT;
	}
	SPDK_DEBUGLOG(vfio_pci, "Memory mapped to %p\n", region->mmaps[0].mem);
	region->nr_mmaps = 1;

	return 0;
}

static int
vfio_device_map_bars_and_config_region(struct vfio_device *device)
{
	uint32_t i;
	int ret;
	size_t len = 4096;
	int fds[VFIO_MAXIMUM_SPARSE_MMAP_REGIONS];
	struct vfio_region_info *info;
	uint8_t *buf;

	buf = calloc(1, len);
	if (!buf) {
		return -ENOMEM;
	}

	info = (struct vfio_region_info *)buf;
	for (i = 0; i < device->pci_regions; i++) {
		memset(info, 0, len);
		memset(fds, 0, sizeof(fds));

		info->index = i;
		ret = vfio_user_get_dev_region_info(device, info, len, fds, VFIO_MAXIMUM_SPARSE_MMAP_REGIONS);
		if (ret) {
			SPDK_ERRLOG("Device setup bar %d failed\n", ret);
			free(buf);
			return ret;
		}

		device->regions[i].size = info->size;
		device->regions[i].offset = info->offset;
		device->regions[i].flags = info->flags;

		SPDK_DEBUGLOG(vfio_pci, "Bar %d, Size 0x%llx, Offset 0x%llx, Flags 0x%x, Cap offset %u\n",
			      i, info->size, info->offset, info->flags, info->cap_offset);

		/* Setup MMAP if any */
		if (info->size && (info->flags & VFIO_REGION_INFO_FLAG_MMAP)) {
			/* try to map sparse memory region first */
			ret = vfio_device_setup_sparse_mmaps(device, i, info, fds);
			if (ret < 0) {
				ret = vfio_device_map_region(device, &device->regions[i], fds[0]);
			}

			if (ret != 0) {
				SPDK_ERRLOG("Setup Device %s region %d failed\n", device->name, i);
				free(buf);
				return ret;
			}
		}
	}

	free(buf);
	return 0;
}

static void
vfio_device_unmap_bars(struct vfio_device *dev)
{
	uint32_t i, j;
	struct vfio_pci_region *region;

	for (i = 0; i < dev->pci_regions; i++) {
		region = &dev->regions[i];
		for (j = 0; j < region->nr_mmaps; j++) {
			if (region->mmaps[j].mem) {
				munmap(region->mmaps[j].mem, region->mmaps[j].size);
			}
		}
	}
	memset(dev->regions, 0, sizeof(dev->regions));
}

struct vfio_device *
spdk_vfio_user_setup(const char *path)
{
	int ret;
	struct vfio_device *device = NULL;
	struct vfio_user_device_info dev_info = {};

	device = calloc(1, sizeof(*device));
	if (!device) {
		return NULL;
	}
	TAILQ_INIT(&device->mrs_head);
	snprintf(device->path, PATH_MAX, "%s", path);
	snprintf(device->name, sizeof(device->name), "vfio-user%u", g_vfio_dev_id++);

	ret = vfio_user_dev_setup(device);
	if (ret) {
		free(device);
		SPDK_ERRLOG("Error to setup vfio-user via path %s\n", path);
		return NULL;
	}

	ret = vfio_user_get_dev_info(device, &dev_info, sizeof(dev_info));
	if (ret) {
		SPDK_ERRLOG("Device get info failed\n");
		goto cleanup;
	}
	device->pci_regions = dev_info.num_regions;
	device->flags = dev_info.flags;

	ret = vfio_device_map_bars_and_config_region(device);
	if (ret) {
		goto cleanup;
	}

	/* Register DMA Region */
	ret = vfio_device_dma_map(device);
	if (ret) {
		SPDK_ERRLOG("Container DMA map failed\n");
		goto cleanup;
	}

	SPDK_DEBUGLOG(vfio_pci, "Device %s, Path %s Setup Successfully\n", device->name, device->path);

	return device;

cleanup:
	close(device->fd);
	free(device);
	return NULL;
}

void
spdk_vfio_user_release(struct vfio_device *dev)
{
	SPDK_DEBUGLOG(vfio_pci, "Release file %s\n", dev->path);

	vfio_device_unmap_bars(dev);
	if (dev->map) {
		spdk_mem_map_free(&dev->map);
	}
	close(dev->fd);

	free(dev);
}

void *
spdk_vfio_user_get_bar_addr(struct vfio_device *dev, uint32_t index, uint64_t offset, uint32_t len)
{
	struct vfio_pci_region *region = &dev->regions[index];
	uint32_t i;

	if (!region->size || !(region->flags & VFIO_REGION_INFO_FLAG_MMAP)) {
		return NULL;
	}

	for (i = 0; i < region->nr_mmaps; i++) {
		if (region->mmaps[i].mem && (region->mmaps[i].offset <= offset) &&
		    ((offset + len) <= (region->mmaps[i].offset + region->mmaps[i].size))) {
			return (void *)((uintptr_t)region->mmaps[i].mem + offset - region->mmaps[i].offset);
		}
	}

	return NULL;
}

SPDK_LOG_REGISTER_COMPONENT(vfio_pci)
