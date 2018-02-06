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

#include "spdk/stdinc.h"

#include "env_internal.h"

#include <rte_config.h>
#include <rte_eal_memconfig.h>

#include "spdk_internal/assert.h"

#include "spdk/assert.h"
#include "spdk/likely.h"
#include "spdk/queue.h"
#include "spdk/util.h"

#ifdef __FreeBSD__
#define SPDK_VFIO_ENABLED 0
#else
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0)
#define SPDK_VFIO_ENABLED 1
#include <linux/vfio.h>

/* Internal DPDK function forward declaration */
int pci_vfio_is_enabled(void);

struct spdk_vfio_dma_map {
	struct vfio_iommu_type1_dma_map map;
	TAILQ_ENTRY(spdk_vfio_dma_map) tailq;
};

struct vfio_cfg {
	int fd;
	bool enabled;
	unsigned device_ref;
	TAILQ_HEAD(, spdk_vfio_dma_map) maps;
	pthread_mutex_t mutex;
};

static struct vfio_cfg g_vfio = {
	.fd = -1,
	.enabled = false,
	.device_ref = 0,
	.maps = TAILQ_HEAD_INITIALIZER(g_vfio.maps),
	.mutex = PTHREAD_MUTEX_INITIALIZER
};

#else
#define SPDK_VFIO_ENABLED 0
#endif
#endif

#if DEBUG
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...)
#endif

static struct spdk_mem_map *g_vtophys_map;

#if SPDK_VFIO_ENABLED
static int
vtophys_iommu_map_dma(uint64_t vaddr, uint64_t iova, uint64_t size)
{
	struct spdk_vfio_dma_map *dma_map;
	int ret;

	dma_map = calloc(1, sizeof(*dma_map));
	if (dma_map == NULL) {
		return -ENOMEM;
	}

	dma_map->map.argsz = sizeof(dma_map->map);
	dma_map->map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
	dma_map->map.vaddr = vaddr;
	dma_map->map.iova = iova;
	dma_map->map.size = size;

	pthread_mutex_lock(&g_vfio.mutex);
	if (g_vfio.device_ref == 0) {
		/* VFIO requires at least one device (IOMMU group) to be added to
		 * a VFIO container before it is possible to perform any IOMMU
		 * operations on that container. This memory will be mapped once
		 * the first device (IOMMU group) is hotplugged.
		 *
		 * Since the vfio container is managed internally by DPDK, it is
		 * also possible that some device is already in that container, but
		 * it's not managed by SPDK -  e.g. an NIC attached internally
		 * inside DPDK. We could map the memory straight away in such
		 * scenario, but there's no need to do it. DPDK devices clearly
		 * don't need our mappings and hence we defer the mapping
		 * unconditionally until the first SPDK-managed device is
		 * hotplugged.
		 */
		goto out_insert;
	}

	ret = ioctl(g_vfio.fd, VFIO_IOMMU_MAP_DMA, &dma_map->map);
	if (ret) {
		DEBUG_PRINT("Cannot set up DMA mapping, error %d\n", errno);
		pthread_mutex_unlock(&g_vfio.mutex);
		free(dma_map);
		return ret;
	}

out_insert:
	TAILQ_INSERT_TAIL(&g_vfio.maps, dma_map, tailq);
	pthread_mutex_unlock(&g_vfio.mutex);
	return 0;
}

static int
vtophys_iommu_unmap_dma(uint64_t iova, uint64_t size)
{
	struct vfio_iommu_type1_dma_unmap dma_unmap;
	struct spdk_vfio_dma_map *dma_map;
	int ret;

	pthread_mutex_lock(&g_vfio.mutex);
	TAILQ_FOREACH(dma_map, &g_vfio.maps, tailq) {
		if (dma_map->map.iova == iova) {
			break;
		}
	}

	if (dma_map == NULL) {
		DEBUG_PRINT("Cannot clear DMA mapping for IOVA %"PRIx64" - it's not mapped\n", iova);
		pthread_mutex_unlock(&g_vfio.mutex);
		return -ENXIO;
	}

	/** don't support partial or multiple-page unmap for now */
	assert(dma_map->map.size == size);

	if (g_vfio.device_ref == 0) {
		/* Memory is not mapped anymore, just remove it's references */
		goto out_remove;
	}

	dma_unmap.argsz = sizeof(dma_unmap);
	dma_unmap.flags = 0;
	dma_unmap.iova = iova;
	dma_unmap.size = size;

	ret = ioctl(g_vfio.fd, VFIO_IOMMU_UNMAP_DMA, &dma_unmap);
	if (ret) {
		DEBUG_PRINT("Cannot clear DMA mapping, error %d\n", errno);
		pthread_mutex_unlock(&g_vfio.mutex);
		return ret;
	}

out_remove:
	TAILQ_REMOVE(&g_vfio.maps, dma_map, tailq);
	pthread_mutex_unlock(&g_vfio.mutex);
	free(dma_map);
	return 0;
}
#endif

static uint64_t
vtophys_get_paddr_memseg(uint64_t vaddr)
{
	uintptr_t paddr;
	struct rte_mem_config *mcfg;
	struct rte_memseg *seg;
	uint32_t seg_idx;

	mcfg = rte_eal_get_configuration()->mem_config;

	for (seg_idx = 0; seg_idx < RTE_MAX_MEMSEG; seg_idx++) {
		seg = &mcfg->memseg[seg_idx];
		if (seg->addr == NULL) {
			break;
		}

		if (vaddr >= (uintptr_t)seg->addr &&
		    vaddr < ((uintptr_t)seg->addr + seg->len)) {
			paddr = seg->phys_addr;
			paddr += (vaddr - (uintptr_t)seg->addr);
			return paddr;
		}
	}

	return SPDK_VTOPHYS_ERROR;
}

/* Try to get the paddr from /proc/self/pagemap */
static uint64_t
vtophys_get_paddr_pagemap(uint64_t vaddr)
{
	uintptr_t paddr;

	paddr = rte_mem_virt2phy((void *)vaddr);
	if (paddr == 0) {
		/*
		 * The vaddr was valid but returned 0.  Touch the page
		 * to ensure a backing page gets assigned, then call
		 * rte_mem_virt2phy() again.
		 */
		rte_atomic64_read((rte_atomic64_t *)vaddr);
		paddr = rte_mem_virt2phy((void *)vaddr);
	}
	if (paddr == RTE_BAD_PHYS_ADDR) {
		/* Unable to get to the physical address. */
		return SPDK_VTOPHYS_ERROR;
	}

	return paddr;
}

/* Try to get the paddr from pci devices */
static uint64_t
vtophys_get_paddr_pci(uint64_t vaddr)
{
	uintptr_t paddr;
	struct rte_pci_device	*dev;
	struct rte_mem_resource *res;
	unsigned r;

#if RTE_VERSION >= RTE_VERSION_NUM(17, 05, 0, 2)
	FOREACH_DEVICE_ON_PCIBUS(dev) {
#else
	TAILQ_FOREACH(dev, &pci_device_list, next) {
#endif
		for (r = 0; r < PCI_MAX_RESOURCE; r++) {
			res = &dev->mem_resource[r];
			if (res->phys_addr && vaddr >= (uint64_t)res->addr &&
			    vaddr < (uint64_t)res->addr + res->len) {
				paddr = res->phys_addr + (vaddr - (uint64_t)res->addr);
				DEBUG_PRINT("%s: %p -> %p\n", __func__, (void *)vaddr,
					    (void *)paddr);
				return paddr;
			}
		}
	}
	return  SPDK_VTOPHYS_ERROR;
}

static int
spdk_vtophys_notify(void *cb_ctx, struct spdk_mem_map *map,
		    enum spdk_mem_map_notify_action action,
		    void *vaddr, size_t len)
{
	int rc = 0, phys = 0;
	uint64_t paddr;

	if ((uintptr_t)vaddr & ~MASK_256TB) {
		DEBUG_PRINT("invalid usermode virtual address %p\n", vaddr);
		return -EINVAL;
	}

	if (((uintptr_t)vaddr & MASK_2MB) || (len & MASK_2MB)) {
		DEBUG_PRINT("invalid %s parameters, vaddr=%p len=%ju\n",
			    __func__, vaddr, len);
		return -EINVAL;
	}

	while (len > 0) {
		/* Get the physical address from the DPDK memsegs */
		paddr = vtophys_get_paddr_memseg((uint64_t)vaddr);

		switch (action) {
		case SPDK_MEM_MAP_NOTIFY_REGISTER:
			if (paddr == SPDK_VTOPHYS_ERROR) {
				/* This is not an address that DPDK is managing. */
#if SPDK_VFIO_ENABLED
				if (g_vfio.enabled) {
					/* We'll use the virtual address as the iova. DPDK
					 * currently uses physical addresses as the iovas (or counts
					 * up from 0 if it can't get physical addresses), so
					 * the range of user space virtual addresses and physical
					 * addresses will never overlap.
					 */
					paddr = (uint64_t)vaddr;
					rc = vtophys_iommu_map_dma((uint64_t)vaddr, paddr, VALUE_2MB);
					if (rc) {
						return -EFAULT;
					}
				} else
#endif
				{
					/* Get the physical address from /proc/self/pagemap. */
					paddr = vtophys_get_paddr_pagemap((uint64_t)vaddr);
					if (paddr == SPDK_VTOPHYS_ERROR) {
						/* Get the physical address from PCI devices */
						paddr = vtophys_get_paddr_pci((uint64_t)vaddr);
						if (paddr == SPDK_VTOPHYS_ERROR) {
							DEBUG_PRINT("could not get phys addr for %p\n", vaddr);
							return -EFAULT;
						}
						phys = 1;
					}
				}
			}
			/* Since PHYS can break the 2MiB physical alginment skip this check for that. */
			if (!phys && (paddr & MASK_2MB)) {
				DEBUG_PRINT("invalid paddr 0x%" PRIx64 " - must be 2MB aligned\n", paddr);
				return -EINVAL;
			}

			rc = spdk_mem_map_set_translation(map, (uint64_t)vaddr, VALUE_2MB, paddr);
			break;
		case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
#if SPDK_VFIO_ENABLED
			if (paddr == SPDK_VTOPHYS_ERROR) {
				/*
				 * This is not an address that DPDK is managing. If vfio is enabled,
				 * we need to unmap the range from the IOMMU
				 */
				if (g_vfio.enabled) {
					paddr = spdk_mem_map_translate(map, (uint64_t)vaddr);
					rc = vtophys_iommu_unmap_dma(paddr, VALUE_2MB);
					if (rc) {
						return -EFAULT;
					}
				}
			}
#endif
			rc = spdk_mem_map_clear_translation(map, (uint64_t)vaddr, VALUE_2MB);
			break;
		default:
			SPDK_UNREACHABLE();
		}

		if (rc != 0) {
			return rc;
		}
		vaddr += VALUE_2MB;
		len -= VALUE_2MB;
	}

	return rc;
}

#if SPDK_VFIO_ENABLED
static void
spdk_vtophys_iommu_init(void)
{
	char proc_fd_path[PATH_MAX + 1];
	char link_path[PATH_MAX + 1];
	const char vfio_path[] = "/dev/vfio/vfio";
	DIR *dir;
	struct dirent *d;

	if (!pci_vfio_is_enabled()) {
		return;
	}

	dir = opendir("/proc/self/fd");
	if (!dir) {
		DEBUG_PRINT("Failed to open /proc/self/fd (%d)\n", errno);
		return;
	}

	while ((d = readdir(dir)) != NULL) {
		if (d->d_type != DT_LNK) {
			continue;
		}

		snprintf(proc_fd_path, sizeof(proc_fd_path), "/proc/self/fd/%s", d->d_name);
		if (readlink(proc_fd_path, link_path, sizeof(link_path)) != (sizeof(vfio_path) - 1)) {
			continue;
		}

		if (memcmp(link_path, vfio_path, sizeof(vfio_path) - 1) == 0) {
			sscanf(d->d_name, "%d", &g_vfio.fd);
			break;
		}
	}

	closedir(dir);

	if (g_vfio.fd < 0) {
		DEBUG_PRINT("Failed to discover DPDK VFIO container fd.\n");
		return;
	}

	g_vfio.enabled = true;

	return;
}
#endif

void
spdk_vtophys_get_ref(void)
{
#if SPDK_VFIO_ENABLED
	struct spdk_vfio_dma_map *dma_map;
	int ret;

	if (!g_vfio.enabled) {
		return;
	}

	pthread_mutex_lock(&g_vfio.mutex);
	g_vfio.device_ref++;
	if (g_vfio.device_ref > 1) {
		pthread_mutex_unlock(&g_vfio.mutex);
		return;
	}

	/* This is the first SPDK device using DPDK vfio. This means that the first
	 * IOMMU group might have been just been added to the DPDK vfio container.
	 * From this point it is certain that the memory can be mapped now.
	 */
	TAILQ_FOREACH(dma_map, &g_vfio.maps, tailq) {
		ret = ioctl(g_vfio.fd, VFIO_IOMMU_MAP_DMA, &dma_map->map);
		if (ret) {
			DEBUG_PRINT("Cannot update DMA mapping, error %d\n", errno);
			break;
		}
	}
	pthread_mutex_unlock(&g_vfio.mutex);
#endif
}

void
spdk_vtophys_put_ref(void)
{
#if SPDK_VFIO_ENABLED
	struct spdk_vfio_dma_map *dma_map;
	int ret;

	if (!g_vfio.enabled) {
		return;
	}

	pthread_mutex_lock(&g_vfio.mutex);
	assert(g_vfio.device_ref > 0);
	g_vfio.device_ref--;
	if (g_vfio.device_ref > 0) {
		pthread_mutex_unlock(&g_vfio.mutex);
		return;
	}

	/* This is the last SPDK device using DPDK vfio. If DPDK doesn't have
	 * any additional devices using it's vfio container, all the mappings
	 * will be automatically removed by the Linux vfio driver. We unmap
	 * the memory manually to be able to easily re-map it later regardless
	 * of other, external factors.
	 */
	TAILQ_FOREACH(dma_map, &g_vfio.maps, tailq) {
		ret = ioctl(g_vfio.fd, VFIO_IOMMU_UNMAP_DMA, &dma_map->map);
		if (ret) {
			DEBUG_PRINT("Cannot unmap DMA memory, error %d\n", errno);
			break;
		}
	}
	pthread_mutex_unlock(&g_vfio.mutex);
#endif
}

int
spdk_vtophys_init(void)
{
#if SPDK_VFIO_ENABLED
	spdk_vtophys_iommu_init();
#endif

	g_vtophys_map = spdk_mem_map_alloc(SPDK_VTOPHYS_ERROR, spdk_vtophys_notify, NULL);
	if (g_vtophys_map == NULL) {
		DEBUG_PRINT("vtophys map allocation failed\n");
		return -1;
	}
	return 0;
}

uint64_t
spdk_vtophys(void *buf)
{
	uint64_t vaddr, paddr_2mb;

	vaddr = (uint64_t)buf;

	paddr_2mb = spdk_mem_map_translate(g_vtophys_map, vaddr);

	/*
	 * SPDK_VTOPHYS_ERROR has all bits set, so if the lookup returned SPDK_VTOPHYS_ERROR,
	 * we will still bitwise-or it with the buf offset below, but the result will still be
	 * SPDK_VTOPHYS_ERROR.
	 */
	SPDK_STATIC_ASSERT(SPDK_VTOPHYS_ERROR == UINT64_C(-1), "SPDK_VTOPHYS_ERROR should be all 1s");
	return paddr_2mb | ((uint64_t)buf & MASK_2MB);
}
