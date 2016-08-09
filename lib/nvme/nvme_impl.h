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

/**
 * \file
 * NVMe driver integration callbacks
 *
 * This file describes the callback functions required to integrate
 * the userspace NVMe driver for a specific implementation.  This
 * implementation is specific for DPDK.  Users would
 * revise it as necessary for their own particular environment if not
 * using it within the DPDK framework.
 */

#ifndef __NVME_IMPL_H__
#define __NVME_IMPL_H__

#include "spdk/vtophys.h"
#include "spdk/pci.h"
#include "spdk/nvme_spec.h"
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_version.h>
#include <rte_memzone.h>
#include <rte_eal.h>

#ifdef SPDK_CONFIG_PCIACCESS
#include <pciaccess.h>
#else
#include <rte_pci.h>
#endif

#include "spdk/pci_ids.h"

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
 * Note: these calls are only made during driver initialization.
 */
static inline void *
nvme_malloc(const char *tag, size_t size, unsigned align, uint64_t *phys_addr)
{
	void *buf = rte_malloc(tag, size, align);
	if (buf) {
		memset(buf, 0, size);
		*phys_addr = rte_malloc_virt2phy(buf);
	}
	return buf;
}

/**
 * Free a memory buffer previously allocated with nvme_malloc.
 */
#define nvme_free(buf)			rte_free(buf)

/**
 * Reserve a named, process shared memory zone with the given size,
 *   socket_id and flags.
 * Return a pointer to the allocated memory address. If the allocation
 *   cannot be done, return NULL.
 */
static inline void *
nvme_memzone_reserve(const char *name, size_t len, int socket_id, unsigned flags)
{
	const struct rte_memzone *mz = rte_memzone_reserve(name, len, socket_id, flags);

	if (mz != NULL) {
		return mz->addr;
	} else {
		return NULL;
	}
}

/**
 * Lookup the memory zone identified by the given name.
 * Return a pointer to the reserved memory address. If the reservation
 *   cannot be found, return NULL.
 */
static inline void *
nvme_memzone_lookup(const char *name)
{
	const struct rte_memzone *mz = rte_memzone_lookup(name);

	if (mz != NULL) {
		return mz->addr;
	} else {
		return NULL;
	}
}

/**
 * Free the memory zone identified by the given name.
 */
static inline int
nvme_memzone_free(const char *name)
{
	const struct rte_memzone *mz = rte_memzone_lookup(name);

	if (mz != NULL) {
		return rte_memzone_free(mz);
	}

	return -1;
}

/**
 * Return true if the calling process is primary process
 */
static inline bool
nvme_process_is_primary(void)
{
	return (rte_eal_process_type() == RTE_PROC_PRIMARY);
}

/**
 * Log or print a message from the NVMe driver.
 */
#define nvme_printf(ctrlr, fmt, args...) printf(fmt, ##args)

/**
 * Return the physical address for the specified virtual address.
 */
#define nvme_vtophys(buf)		spdk_vtophys(buf)
#define NVME_VTOPHYS_ERROR		SPDK_VTOPHYS_ERROR

typedef struct rte_mempool nvme_mempool_t;

/**
 * Create a mempool with the given configuration.
 * Return a pointer to the allocated memory address. If the allocation
 *   cannot be done, return NULL.
 */
static inline nvme_mempool_t *
nvme_mempool_create(const char *name, unsigned n, unsigned elt_size,
		    unsigned cache_size)
{
	struct rte_mempool *mp;

	mp = rte_mempool_create(name, n, elt_size, cache_size,
				0, NULL, NULL, NULL, NULL,
				SOCKET_ID_ANY, 0);

	if (mp == NULL) {
		return NULL;
	}

	return (nvme_mempool_t *)mp;
}

static inline void
nvme_mempool_get(nvme_mempool_t *mp, void **buf)
{
	rte_mempool_get(mp, buf);
}

static inline void
nvme_mempool_put(nvme_mempool_t *mp, void *buf)
{
	rte_mempool_put(mp, buf);
}

/**
 * Get a monotonic timestamp counter (used for measuring timeouts during initialization).
 */
#define nvme_get_tsc()			rte_get_timer_cycles()

/**
 * Get the tick rate of nvme_get_tsc() per second.
 */
#define nvme_get_tsc_hz()		rte_get_timer_hz()

/**
 *
 */
#define nvme_pcicfg_read32(handle, var, offset)  spdk_pci_device_cfg_read32(handle, var, offset)
#define nvme_pcicfg_write32(handle, var, offset) spdk_pci_device_cfg_write32(handle, var, offset)

struct nvme_pci_enum_ctx {
	int (*user_enum_cb)(void *enum_ctx, struct spdk_pci_device *pci_dev);
	void *user_enum_ctx;
};

#ifdef SPDK_CONFIG_PCIACCESS

static int
nvme_pci_enum_cb(void *enum_ctx, struct spdk_pci_device *pci_dev)
{
	struct nvme_pci_enum_ctx *ctx = enum_ctx;

	if (spdk_pci_device_get_class(pci_dev) != SPDK_PCI_CLASS_NVME) {
		return 0;
	}

	return ctx->user_enum_cb(ctx->user_enum_ctx, pci_dev);
}

static inline int
nvme_pci_enumerate(int (*enum_cb)(void *enum_ctx, struct spdk_pci_device *pci_dev), void *enum_ctx)
{
	struct nvme_pci_enum_ctx nvme_enum_ctx;

	nvme_enum_ctx.user_enum_cb = enum_cb;
	nvme_enum_ctx.user_enum_ctx = enum_ctx;

	return spdk_pci_enumerate(nvme_pci_enum_cb, &nvme_enum_ctx);
}

static inline int
nvme_pcicfg_map_bar(void *devhandle, uint32_t bar, uint32_t read_only, void **mapped_addr)
{
	struct pci_device *dev = devhandle;
	uint32_t flags = (read_only ? 0 : PCI_DEV_MAP_FLAG_WRITABLE);

	return pci_device_map_range(dev, dev->regions[bar].base_addr, dev->regions[bar].size,
				    flags, mapped_addr);
}

static inline int
nvme_pcicfg_map_bar_write_combine(void *devhandle, uint32_t bar, void **mapped_addr)
{
	struct pci_device *dev = devhandle;
	uint32_t flags = PCI_DEV_MAP_FLAG_WRITABLE | PCI_DEV_MAP_FLAG_WRITE_COMBINE;

	return pci_device_map_range(dev, dev->regions[bar].base_addr, dev->regions[bar].size,
				    flags, mapped_addr);
}

static inline int
nvme_pcicfg_unmap_bar(void *devhandle, uint32_t bar, void *addr)
{
	struct pci_device *dev = devhandle;

	return pci_device_unmap_range(dev, addr, dev->regions[bar].size);
}

static inline void
nvme_pcicfg_get_bar_addr_len(void *devhandle, uint32_t bar, uint64_t *addr, uint64_t *size)
{
	struct pci_device *dev = devhandle;

	*addr = (uint64_t)dev->regions[bar].base_addr;
	*size = (uint64_t)dev->regions[bar].size;
}

#else /* !SPDK_CONFIG_PCIACCESS */

static inline int
nvme_pcicfg_map_bar(void *devhandle, uint32_t bar, uint32_t read_only, void **mapped_addr)
{
	struct rte_pci_device *dev = devhandle;

	*mapped_addr = dev->mem_resource[bar].addr;
	return 0;
}

static inline int
nvme_pcicfg_map_bar_write_combine(void *devhandle, uint32_t bar, void **mapped_addr)
{
	nvme_printf(NULL, "DPDK cannot support write combine now\n");
	return -1;
}

static inline int
nvme_pcicfg_unmap_bar(void *devhandle, uint32_t bar, void *addr)
{
	return 0;
}

static inline void
nvme_pcicfg_get_bar_addr_len(void *devhandle, uint32_t bar, uint64_t *addr, uint64_t *size)
{
	struct rte_pci_device *dev = devhandle;

	*addr = (uint64_t)dev->mem_resource[bar].phys_addr;
	*size = (uint64_t)dev->mem_resource[bar].len;
}

static struct rte_pci_id nvme_pci_driver_id[] = {
#if RTE_VERSION >= RTE_VERSION_NUM(16, 7, 0, 1)
	{
		.class_id = SPDK_PCI_CLASS_NVME,
		.vendor_id = PCI_ANY_ID,
		.device_id = PCI_ANY_ID,
		.subsystem_vendor_id = PCI_ANY_ID,
		.subsystem_device_id = PCI_ANY_ID,
	},
#else
	{RTE_PCI_DEVICE(0x8086, 0x0953)},
#endif
	{ .vendor_id = 0, /* sentinel */ },
};

/*
 * TODO: eliminate this global if possible (does rte_pci_driver have a context field for this?)
 *
 * This should be protected by the NVMe driver lock, since nvme_probe() holds the lock
 *  while calling nvme_pci_enumerate(), but we shouldn't have to depend on that.
 */
static struct nvme_pci_enum_ctx g_nvme_pci_enum_ctx;

static int
nvme_driver_init(struct rte_pci_driver *dr, struct rte_pci_device *rte_dev)
{
	/*
	 * These are actually the same type internally.
	 * TODO: refactor this so it's inside pci.c
	 */
	struct spdk_pci_device *pci_dev = (struct spdk_pci_device *)rte_dev;

	/*
	 * TODO: This is a workaround for an issue where the device is not ready after VFIO reset.
	 * Figure out what is actually going wrong and remove this sleep.
	 */
	usleep(500 * 1000);

	return g_nvme_pci_enum_ctx.user_enum_cb(g_nvme_pci_enum_ctx.user_enum_ctx, pci_dev);
}

static struct rte_pci_driver nvme_rte_driver = {
	.name = "nvme_driver",
	.devinit = nvme_driver_init,
	.id_table = nvme_pci_driver_id,
	.drv_flags = RTE_PCI_DRV_NEED_MAPPING,
};

static inline int
nvme_pci_enumerate(int (*enum_cb)(void *enum_ctx, struct spdk_pci_device *pci_dev), void *enum_ctx)
{
	int rc;

	g_nvme_pci_enum_ctx.user_enum_cb = enum_cb;
	g_nvme_pci_enum_ctx.user_enum_ctx = enum_ctx;

	rte_eal_pci_register(&nvme_rte_driver);
	rc = rte_eal_pci_probe();
	rte_eal_pci_unregister(&nvme_rte_driver);

	return rc;
}

#endif /* !SPDK_CONFIG_PCIACCESS */

#endif /* __NVME_IMPL_H__ */
