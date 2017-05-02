/*-
 *   BSD LICENSE
 *
 *   Copyright(c) Intel Corporation. All rights reserved.
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

#include <linux/vfio.h>

#include "spdk/env.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

#include "vhost_iommu.h"

static struct {
	int need_init;
	int container_fd;
} vfio_cfg = { 1, -1 };

/* Internal DPDK function forward declaration */
int pci_vfio_is_enabled(void);

/* Discover DPDK vfio container fd. This is to be removed if DPDK API
 * provides interface for memory registration in VFIO container.
 *
 * Return -1 on error, 0 on success (VFIO is used or not)
 */
static int
vfio_cfg_init(void)
{
	char proc_fd_path[PATH_MAX + 1];
	char link_path[PATH_MAX + 1];
	const char vfio_path[] = "/dev/vfio/vfio";
	const int vfio_path_len = sizeof(vfio_path) - 1;
	DIR *dir;
	struct dirent *d;

	if (!vfio_cfg.need_init) {
		return 0;
	}

	vfio_cfg.need_init = 0;
	if (!pci_vfio_is_enabled()) {
		return 0;
	}

	dir = opendir("/proc/self/fd");
	while ((d = readdir(dir)) != NULL) {
		if (d->d_type != DT_LNK)
			continue;

		snprintf(proc_fd_path, sizeof(proc_fd_path), "/proc/self/fd/%s", d->d_name);
		if (readlink(proc_fd_path, link_path, sizeof(link_path)) != vfio_path_len)
			continue;

		if (memcmp(link_path, vfio_path, vfio_path_len) == 0) {
			sscanf(d->d_name, "%d", &vfio_cfg.container_fd);
			break;
		}
	}

	closedir(dir);

	if (vfio_cfg.container_fd < 0) {
		SPDK_ERRLOG("Failed to discover DPDK VFIO container fd.\n");
		return -1;
	}

	return 0;
}

static int
vfio_pci_memory_region_map(int vfio_container_fd, uint64_t vaddr, uint64_t phys_addr, uint64_t size)
{
	struct vfio_iommu_type1_dma_map dma_map;
	int ret;

	dma_map.argsz = sizeof(dma_map);
	dma_map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
	dma_map.vaddr = vaddr;
	dma_map.iova = phys_addr;
	dma_map.size = size;

	SPDK_TRACELOG(SPDK_TRACE_VHOST_VFIO, "MAP vaddr:%p phys:%p len:%#"PRIx64"\n", (void *)vaddr,
		      (void *)phys_addr, size);
	ret = ioctl(vfio_container_fd, VFIO_IOMMU_MAP_DMA, &dma_map);

	if (ret) {
		SPDK_ERRLOG("Cannot set up DMA mapping, error %d (%s)\n", errno, strerror(errno));
	}

	return ret;
}

static int
vfio_pci_memory_region_unmap(int vfio_container_fd, uint64_t phys_addr, uint64_t size)
{
	struct vfio_iommu_type1_dma_unmap dma_unmap;
	int ret;

	dma_unmap.argsz = sizeof(dma_unmap);
	dma_unmap.flags = 0;
	dma_unmap.iova = phys_addr;
	dma_unmap.size = size;

	SPDK_TRACELOG(SPDK_TRACE_VHOST_VFIO, "UNMAP phys:%p len:%#"PRIx64"\n", (void *)phys_addr, size);
	ret = ioctl(vfio_container_fd, VFIO_IOMMU_UNMAP_DMA, &dma_unmap);

	if (ret) {
		SPDK_ERRLOG("Cannot clear DMA mapping, error %d (%s)\n", errno, strerror(errno));
	}

	return ret;
}

static int
vfio_pci_memory_region_op(uint64_t vaddr, uint64_t phys_addr, uint64_t size, int op)
{

	if (vfio_cfg.container_fd == -1) {
		return 0;
	}

	if (op == VFIO_IOMMU_MAP_DMA) {
		return vfio_pci_memory_region_map(vfio_cfg.container_fd, vaddr, phys_addr, size);
	} else {
		return vfio_pci_memory_region_unmap(vfio_cfg.container_fd, phys_addr, size);
	}
}


#define SHIFT_2MB	21 /* (1 << 21) == 2MB */
#define MASK_2MB	((1ULL << SHIFT_2MB) - 1)

static int
spdk_vfio_mem_op(uint64_t addr, uint64_t len, int dma_op)
{
	const uint64_t len_2mb = 1 << SHIFT_2MB;
	uint64_t vaddr, vend, phaddr, phend, vlen;
	int ret = 0;

	if (vfio_cfg_init() != 0) {
		return -1;
	}

	if (vfio_cfg.container_fd == -1) {
		return 0;
	}

	vaddr = addr;
	while (len > 0) {
		vlen = spdk_min(len_2mb - (vaddr & MASK_2MB), len);
		vend = vaddr + vlen;

		phaddr = spdk_vtophys((void *)vaddr);
		phend = spdk_vtophys((void *)(vend - 1));

		if (phaddr == SPDK_VTOPHYS_ERROR || phend == SPDK_VTOPHYS_ERROR ||
		    phend - phaddr > vlen - 1) {
			SPDK_ERRLOG("Invalid memory region addr: %p len:%"PRIu64" "
				    "spdk_vtophys(%p) = %p spdk_vtophys(%p) = %p\n",
				    (void *)addr, len, (void *)vaddr, (void *)phaddr,
				    (void *)vend, (void *)phend);
			ret = -1;
			break;
		}

		ret = vfio_pci_memory_region_op(vaddr, phaddr, vlen, dma_op);
		if (ret) {
			SPDK_ERRLOG("Failed to %s region region vaddr=%p phys_addr=%p len=%#"PRIx64"\n",
				    (dma_op == VFIO_IOMMU_MAP_DMA ? "map" : "unmap"), (void *)vaddr,
				    (void *)phaddr, vlen);
			break;
		}

		vaddr += vlen;
		len -= vlen;

		assert(len == 0 || (vaddr & MASK_2MB) == 0);
	}

	if (ret) {
		spdk_vfio_mem_op(addr, vaddr - addr, VFIO_IOMMU_UNMAP_DMA);
	}

	return ret;
}

int spdk_iommu_mem_register(uint64_t addr, uint64_t len)
{
	return spdk_vfio_mem_op(addr, len, VFIO_IOMMU_MAP_DMA);
}

int spdk_iommu_mem_unregister(uint64_t addr, uint64_t len)
{
	return spdk_vfio_mem_op(addr, len, VFIO_IOMMU_UNMAP_DMA);
}

SPDK_LOG_REGISTER_TRACE_FLAG("vhost_vfio", SPDK_TRACE_VHOST_VFIO)
