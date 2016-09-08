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

#include "spdk/env.h"
#include "spdk/env.h"
#include "spdk/nvme_spec.h"

#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <rte_config.h>
#include <rte_mempool.h>
#include <rte_version.h>
#include <rte_eal.h>

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
#define nvme_malloc 			spdk_zmalloc

/**
 * Free a memory buffer previously allocated with nvme_malloc.
 */
#define nvme_free			spdk_free

/**
 * Reserve a named, process shared memory zone with the given size,
 *   socket_id and flags.
 * Return a pointer to the allocated memory address. If the allocation
 *   cannot be done, return NULL.
 */
#define nvme_memzone_reserve		spdk_memzone_reserve

/**
 * Lookup the memory zone identified by the given name.
 * Return a pointer to the reserved memory address. If the reservation
 *   cannot be found, return NULL.
 */
#define nvme_memzone_lookup		spdk_memzone_lookup

/**
 * Free the memory zone identified by the given name.
 */
#define nvme_memzone_free		spdk_memzone_free

/**
 * Return true if the calling process is primary process
 */
#define nvme_process_is_primary		spdk_process_is_primary

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
#define nvme_get_tsc()			spdk_get_ticks()

/**
 * Get the tick rate of nvme_get_tsc() per second.
 */
#define nvme_get_tsc_hz()		spdk_get_ticks_hz()

#endif /* __NVME_IMPL_H__ */
