#ifndef __IOAT_IMPL_H__
#define __IOAT_IMPL_H__

#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_atomic.h>
#include <rte_cycles.h>

#include "spdk/assert.h"
#include "spdk/pci.h"
#include "spdk/env.h"

#include <rte_pci.h>

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
	void *buf = rte_malloc(tag, size, align);

	if (buf) {
		memset(buf, 0, size);
		*phys_addr = rte_malloc_virt2phy(buf);
	}
	return buf;
}

/**
 * Free a memory buffer previously allocated with ioat_zmalloc.
 */
#define ioat_free(buf)			rte_free(buf)

/**
 * Return the physical address for the specified virtual address.
 */
#define ioat_vtophys(buf)		spdk_vtophys(buf)

/**
 * Delay us.
 */
#define ioat_delay_us(us)        rte_delay_us(us)

#endif /* __IOAT_IMPL_H__ */
