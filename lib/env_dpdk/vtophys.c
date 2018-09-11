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
/*
 * DPDK versions before 17.11 don't provide a way to get VFIO information in the public API,
 * and we can't link to internal symbols when built against shared library DPDK,
 * so disable VFIO entirely in that case.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0) && \
    (RTE_VERSION >= RTE_VERSION_NUM(17, 11, 0, 3) || !defined(RTE_BUILD_SHARED_LIB))

#define SPDK_VFIO_ENABLED 1
#include <linux/vfio.h>

#if RTE_VERSION >= RTE_VERSION_NUM(17, 11, 0, 3)
#include <rte_vfio.h>
#else
/* Internal DPDK function forward declaration */
int pci_vfio_is_enabled(void);
#endif

struct spdk_vfio_dma_map {
	struct vfio_iommu_type1_dma_map map;
	struct vfio_iommu_type1_dma_unmap unmap;
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

struct spdk_vtophys_pci_device {
	struct rte_pci_device *pci_device;
	TAILQ_ENTRY(spdk_vtophys_pci_device) tailq;
	uint64_t ref;
};

static pthread_mutex_t g_vtophys_pci_devices_mutex = PTHREAD_MUTEX_INITIALIZER;
static TAILQ_HEAD(, spdk_vtophys_pci_device) g_vtophys_pci_devices =
	TAILQ_HEAD_INITIALIZER(g_vtophys_pci_devices);

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

	dma_map->unmap.argsz = sizeof(dma_map->unmap);
	dma_map->unmap.flags = 0;
	dma_map->unmap.iova = iova;
	dma_map->unmap.size = size;

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


	ret = ioctl(g_vfio.fd, VFIO_IOMMU_UNMAP_DMA, &dma_map->unmap);
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
	struct rte_memseg *seg;

#if RTE_VERSION >= RTE_VERSION_NUM(18, 05, 0, 0)
	seg = rte_mem_virt2memseg((void *)(uintptr_t)vaddr, NULL);
	if (seg != NULL) {
		paddr = seg->phys_addr;
		if (paddr == RTE_BAD_IOVA) {
			return SPDK_VTOPHYS_ERROR;
		}
		paddr += (vaddr - (uintptr_t)seg->addr);
		return paddr;
	}
#else
	struct rte_mem_config *mcfg;
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
#if RTE_VERSION >= RTE_VERSION_NUM(17, 11, 0, 3)
			if (paddr == RTE_BAD_IOVA) {
#else
			if (paddr == RTE_BAD_PHYS_ADDR) {
#endif
				return SPDK_VTOPHYS_ERROR;
			}
			paddr += (vaddr - (uintptr_t)seg->addr);
			return paddr;
		}
	}
#endif

	return SPDK_VTOPHYS_ERROR;
}

/* Try to get the paddr from /proc/self/pagemap */
static uint64_t
vtophys_get_paddr_pagemap(uint64_t vaddr)
{
	uintptr_t paddr;

#if RTE_VERSION >= RTE_VERSION_NUM(17, 11, 0, 3)
#define BAD_ADDR RTE_BAD_IOVA
#define VTOPHYS rte_mem_virt2iova
#else
#define BAD_ADDR RTE_BAD_PHYS_ADDR
#define VTOPHYS rte_mem_virt2phy
#endif

	/*
	 * Note: the virt2phy/virt2iova functions have changed over time, such
	 * that older versions may return 0 while recent versions will never
	 * return 0 but RTE_BAD_PHYS_ADDR/IOVA instead.  To support older and
	 * newer versions, check for both return values.
	 */
	paddr = VTOPHYS((void *)vaddr);
	if (paddr == 0 || paddr == BAD_ADDR) {
		/*
		 * The vaddr may be valid but doesn't have a backing page
		 * assigned yet.  Touch the page to ensure a backing page
		 * gets assigned, then try to translate again.
		 */
		rte_atomic64_read((rte_atomic64_t *)vaddr);
		paddr = VTOPHYS((void *)vaddr);
	}
	if (paddr == 0 || paddr == BAD_ADDR) {
		/* Unable to get to the physical address. */
		return SPDK_VTOPHYS_ERROR;
	}

#undef BAD_ADDR
#undef VTOPHYS

	return paddr;
}

/* Try to get the paddr from pci devices */
static uint64_t
vtophys_get_paddr_pci(uint64_t vaddr)
{
	struct spdk_vtophys_pci_device *vtophys_dev;
	uintptr_t paddr;
	struct rte_pci_device	*dev;
#if RTE_VERSION >= RTE_VERSION_NUM(16, 11, 0, 1)
	struct rte_mem_resource *res;
#else
	struct rte_pci_resource *res;
#endif
	unsigned r;

	pthread_mutex_lock(&g_vtophys_pci_devices_mutex);
	TAILQ_FOREACH(vtophys_dev, &g_vtophys_pci_devices, tailq) {
		dev = vtophys_dev->pci_device;

		for (r = 0; r < PCI_MAX_RESOURCE; r++) {
			res = &dev->mem_resource[r];
			if (res->phys_addr && vaddr >= (uint64_t)res->addr &&
			    vaddr < (uint64_t)res->addr + res->len) {
				paddr = res->phys_addr + (vaddr - (uint64_t)res->addr);
				DEBUG_PRINT("%s: %p -> %p\n", __func__, (void *)vaddr,
					    (void *)paddr);
				pthread_mutex_unlock(&g_vtophys_pci_devices_mutex);
				return paddr;
			}
		}
	}
	pthread_mutex_unlock(&g_vtophys_pci_devices_mutex);

	return  SPDK_VTOPHYS_ERROR;
}

static int
spdk_vtophys_notify(void *cb_ctx, struct spdk_mem_map *map,
		    enum spdk_mem_map_notify_action action,
		    void *vaddr, size_t len)
{
	int rc = 0, pci_phys = 0;
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
						pci_phys = 1;
					}
				}
			}
			/* Since PCI paddr can break the 2MiB physical alignment skip this check for that. */
			if (!pci_phys && (paddr & MASK_2MB)) {
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
					uint64_t buffer_len;
					paddr = spdk_mem_map_translate(map, (uint64_t)vaddr, &buffer_len);
					if (buffer_len != VALUE_2MB) {
						return -EINVAL;
					}
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

static bool
spdk_vfio_enabled(void)
{
#if RTE_VERSION >= RTE_VERSION_NUM(17, 11, 0, 3)
	return rte_vfio_is_enabled("vfio_pci");
#else
	return pci_vfio_is_enabled();
#endif
}

static void
spdk_vtophys_iommu_init(void)
{
	char proc_fd_path[PATH_MAX + 1];
	char link_path[PATH_MAX + 1];
	const char vfio_path[] = "/dev/vfio/vfio";
	DIR *dir;
	struct dirent *d;

	if (!spdk_vfio_enabled()) {
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
spdk_vtophys_pci_device_added(struct rte_pci_device *pci_device)
{
	struct spdk_vtophys_pci_device *vtophys_dev;
	bool found = false;

	pthread_mutex_lock(&g_vtophys_pci_devices_mutex);
	TAILQ_FOREACH(vtophys_dev, &g_vtophys_pci_devices, tailq) {
		if (vtophys_dev->pci_device == pci_device) {
			vtophys_dev->ref++;
			found = true;
			break;
		}
	}

	if (!found) {
		vtophys_dev = calloc(1, sizeof(*vtophys_dev));
		if (vtophys_dev) {
			vtophys_dev->pci_device = pci_device;
			vtophys_dev->ref = 1;
			TAILQ_INSERT_TAIL(&g_vtophys_pci_devices, vtophys_dev, tailq);
		} else {
			DEBUG_PRINT("Memory allocation error\n");
		}
	}
	pthread_mutex_unlock(&g_vtophys_pci_devices_mutex);

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
spdk_vtophys_pci_device_removed(struct rte_pci_device *pci_device)
{
	struct spdk_vtophys_pci_device *vtophys_dev;

	pthread_mutex_lock(&g_vtophys_pci_devices_mutex);
	TAILQ_FOREACH(vtophys_dev, &g_vtophys_pci_devices, tailq) {
		if (vtophys_dev->pci_device == pci_device) {
			assert(vtophys_dev->ref > 0);
			if (--vtophys_dev->ref == 0) {
				TAILQ_REMOVE(&g_vtophys_pci_devices, vtophys_dev, tailq);
				free(vtophys_dev);
			}
			break;
		}
	}
	pthread_mutex_unlock(&g_vtophys_pci_devices_mutex);

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
		ret = ioctl(g_vfio.fd, VFIO_IOMMU_UNMAP_DMA, &dma_map->unmap);
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
	const struct spdk_mem_map_ops vtophys_map_ops = {
		.notify_cb = spdk_vtophys_notify,
		.are_contiguous = NULL
	};

#if SPDK_VFIO_ENABLED
	spdk_vtophys_iommu_init();
#endif

	g_vtophys_map = spdk_mem_map_alloc(SPDK_VTOPHYS_ERROR, &vtophys_map_ops, NULL);
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

	paddr_2mb = spdk_mem_map_translate(g_vtophys_map, vaddr, NULL);

	/*
	 * SPDK_VTOPHYS_ERROR has all bits set, so if the lookup returned SPDK_VTOPHYS_ERROR,
	 * we will still bitwise-or it with the buf offset below, but the result will still be
	 * SPDK_VTOPHYS_ERROR. However now that we do + rather than | (due to PCI vtophys being
	 * unaligned) we must now check the return value before addition.
	 */
	SPDK_STATIC_ASSERT(SPDK_VTOPHYS_ERROR == UINT64_C(-1), "SPDK_VTOPHYS_ERROR should be all 1s");
	if (paddr_2mb == SPDK_VTOPHYS_ERROR) {
		return SPDK_VTOPHYS_ERROR;
	} else {
		return paddr_2mb + ((uint64_t)buf & MASK_2MB);
	}
}

static int
spdk_bus_scan(void)
{
	return 0;
}

static int
spdk_bus_probe(void)
{
	return 0;
}

static struct rte_device *
spdk_bus_find_device(const struct rte_device *start,
		     rte_dev_cmp_t cmp, const void *data)
{
	return NULL;
}

#if RTE_VERSION >= RTE_VERSION_NUM(17, 11, 0, 3)
static enum rte_iova_mode
spdk_bus_get_iommu_class(void) {
	/* Since we register our PCI drivers after EAL init, we have no chance
	 * of switching into RTE_IOVA_VA (virtual addresses as iova) iommu
	 * class. DPDK uses RTE_IOVA_PA by default because for some platforms
	 * it's the only supported mode, but then SPDK does not support those
	 * platforms and doesn't mind defaulting to RTE_IOVA_VA. The rte_pci bus
	 * will force RTE_IOVA_PA if RTE_IOVA_VA simply can not be used
	 * (i.e. at least one device on the system is bound to uio_pci_generic),
	 * so we simply return RTE_IOVA_VA here.
	 */
	return RTE_IOVA_VA;
}
#endif

struct rte_bus spdk_bus = {
	.scan = spdk_bus_scan,
	.probe = spdk_bus_probe,
	.find_device = spdk_bus_find_device,
#if RTE_VERSION >= RTE_VERSION_NUM(17, 11, 0, 3)
	.get_iommu_class = spdk_bus_get_iommu_class,
#endif
};

RTE_REGISTER_BUS(spdk, spdk_bus);
