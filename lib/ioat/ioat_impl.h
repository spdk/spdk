#ifndef __IOAT_IMPL_H__
#define __IOAT_IMPL_H__

#include <assert.h>
#include <pthread.h>
#include <pciaccess.h>
#include <stdio.h>
#include <rte_malloc.h>
#include <rte_config.h>
#include <rte_atomic.h>
#include <rte_cycles.h>

#include "spdk/vtophys.h"

/**
 * \file
 *
 * This file describes the functions required to integrate
 * the userspace IOAT driver for a specific implementation.  This
 * implementation is specific for DPDK.  Users would revise it as
 * necessary for their own particular environment if not using it
 * within the SPDK framework.
 */

/**
 * Allocate a pinned, physically contiguous memory buffer with the
 * given size and alignment.
 */
static inline void *
ioat_zmalloc(const char *tag, size_t size, unsigned align, uint64_t *phys_addr)
{
	void *buf = rte_zmalloc(tag, size, align);
	*phys_addr = rte_malloc_virt2phy(buf);
	return buf;
}

/**
 * Free a memory buffer previously allocated with ioat_zmalloc.
 */
#define ioat_free(buf)			rte_free(buf)

/**
 * Return the physical address for the specified virtual address.
 */
#define ioat_vtophys(buf)		vtophys(buf)

/**
 * Delay us.
 */
#define ioat_delay_us(us)        rte_delay_us(us)

/**
 * Assert a condition and panic/abort as desired.  Failures of these
 *  assertions indicate catastrophic failures within the driver.
 */
#define ioat_assert(check)		assert(check)

/**
 * Log or print a message from the driver.
 */
#define ioat_printf(chan, fmt, args...) printf(fmt, ##args)

/**
 *
 */
#define ioat_pcicfg_read32(handle, var, offset)  pci_device_cfg_read_u32(handle, var, offset)
#define ioat_pcicfg_write32(handle, var, offset) pci_device_cfg_write_u32(handle, var, offset)

static inline int
ioat_pcicfg_map_bar(void *devhandle, uint32_t bar, uint32_t read_only, void **mapped_addr)
{
	struct pci_device *dev = devhandle;
	uint32_t flags = (read_only ? 0 : PCI_DEV_MAP_FLAG_WRITABLE);

	return pci_device_map_range(dev, dev->regions[bar].base_addr, 4096,
				    flags, mapped_addr);
}

static inline int
ioat_pcicfg_unmap_bar(void *devhandle, uint32_t bar, void *addr)
{
	struct pci_device *dev = devhandle;

	return pci_device_unmap_range(dev, addr, dev->regions[bar].size);
}

typedef pthread_mutex_t ioat_mutex_t;

#define ioat_mutex_lock pthread_mutex_lock
#define ioat_mutex_unlock pthread_mutex_unlock
#define IOAT_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER

#endif /* __IOAT_IMPL_H__ */
