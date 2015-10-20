/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
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

#ifndef __NVME_IMPL_H__
#define __NVME_IMPL_H__

#include "spdk/vtophys.h"
#include <assert.h>
#include <pciaccess.h>
#include <rte_malloc.h>
#include <rte_config.h>
#include <rte_mempool.h>
#include <rte_memcpy.h>

/**
 * \file
 *
 * This file describes the callback functions required to integrate
 * the userspace NVMe driver for a specific implementation.  This
 * implementation is specific for DPDK for Storage.  Users would
 * revise it as necessary for their own particular environment if not
 * using it within the DPDK for Storage framework.
 */

/**
 * \page nvme_driver_integration NVMe Driver Integration
 *
 * Users can integrate the userspace NVMe driver into their environment
 * by implementing the callbacks in nvme_impl.h.  These callbacks
 * enable users to specify how to allocate pinned and physically
 * contiguous memory, performance virtual to physical address
 * translations, log messages, PCI configuration and register mapping,
 * and a number of other facilities that may differ depending on the
 * environment.
 */

/**
 * Allocate a pinned, physically contiguous memory buffer with the
 *   given size and alignment.
 * Note: these calls are only made during driver initialization.  Per
 *   I/O allocations during driver operation use the nvme_alloc_request
 *   callback.
 */
static inline void *
nvme_malloc(const char *tag, size_t size, unsigned align, uint64_t *phys_addr)
{
	void *buf = rte_zmalloc(tag, size, align);
	*phys_addr = rte_malloc_virt2phy(buf);
	return buf;
}

/**
 * Free a memory buffer previously allocated with nvme_malloc.
 */
#define nvme_free(buf)			rte_free(buf)

/**
 * Log or print a message from the NVMe driver.
 */
#define nvme_printf(ctrlr, fmt, args...) printf(fmt, ##args)

/**
 * Assert a condition and panic/abort as desired.  Failures of these
 *  assertions indicate catastrophic failures within the driver.
 */
#define nvme_assert(check, str) assert(check)

/**
 * Return the physical address for the specified virtual address.
 */
#define nvme_vtophys(buf)		vtophys(buf)

extern struct rte_mempool *request_mempool;

/**
 * Return a buffer for an nvme_request object.  These objects are allocated
 *  for each I/O.  They do not need to be pinned nor physically contiguous.
 */
#define nvme_alloc_request(bufp)	rte_mempool_get(request_mempool, (void **)(bufp));

/**
 * Free a buffer previously allocated with nvme_alloc_request().
 */
#define nvme_dealloc_request(buf)	rte_mempool_put(request_mempool, buf)

/**
 *
 */
#define nvme_pcicfg_read32(handle, var, offset)  pci_device_cfg_read_u32(handle, var, offset)
#define nvme_pcicfg_write32(handle, var, offset) pci_device_cfg_write_u32(handle, var, offset)

static inline int
nvme_pcicfg_map_bar(void *devhandle, uint32_t bar, uint32_t read_only, void **mapped_addr)
{
	struct pci_device *dev = devhandle;
	uint32_t flags = (read_only ? 0 : PCI_DEV_MAP_FLAG_WRITABLE);

	return pci_device_map_range(dev, dev->regions[bar].base_addr, dev->regions[bar].size,
				    flags, mapped_addr);
}

static inline int
nvme_pcicfg_unmap_bar(void *devhandle, uint32_t bar, void *addr)
{
	struct pci_device *dev = devhandle;

	return pci_device_unmap_range(dev, addr, dev->regions[bar].size);
}

typedef pthread_mutex_t nvme_mutex_t;

#define nvme_mutex_init(x) pthread_mutex_init((x), NULL)
#define nvme_mutex_destroy(x) pthread_mutex_destroy((x))
#define nvme_mutex_lock pthread_mutex_lock
#define nvme_mutex_unlock pthread_mutex_unlock
#define NVME_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER

static inline int
nvme_mutex_init_recursive(nvme_mutex_t *mtx)
{
	pthread_mutexattr_t attr;
	int rc = 0;

	if (pthread_mutexattr_init(&attr)) {
		return -1;
	}
	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) ||
	    pthread_mutex_init(mtx, &attr)) {
		rc = -1;
	}
	pthread_mutexattr_destroy(&attr);
	return rc;
}

/**
 * Copy a struct nvme_command from one memory location to another.
 */
#define nvme_copy_command(dst, src)	rte_memcpy((dst), (src), sizeof(struct nvme_command))

#endif /* __NVME_IMPL_H__ */
