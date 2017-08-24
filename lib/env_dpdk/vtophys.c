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

#if DEBUG
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...)
#endif

static struct spdk_mem_map *g_vtophys_map;

/* Try to get the paddr from the DPDK memsegs */
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

	return SPDK_MEM_TRANSLATION_ERROR;
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
	if (paddr != RTE_BAD_PHYS_ADDR) {
		return paddr;
	}

	return SPDK_MEM_TRANSLATION_ERROR;
}

static int
spdk_vtophys_notify(void *cb_ctx, struct spdk_mem_map *map,
		    enum spdk_mem_map_notify_action action,
		    void *vaddr, size_t len)
{
	int rc = 0;
	uint64_t paddr;

	if ((uintptr_t)vaddr & ~MASK_128TB) {
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
			if (paddr == SPDK_MEM_TRANSLATION_ERROR) {
				paddr = vtophys_get_paddr_pagemap((uint64_t)vaddr);
				if (paddr == SPDK_MEM_TRANSLATION_ERROR) {
					DEBUG_PRINT("could not get phys addr for %p\n", vaddr);
					return -EFAULT;
				}
			}

			if (paddr & MASK_2MB) {
				DEBUG_PRINT("invalid paddr 0x%" PRIx64 " - must be 2MB aligned\n", paddr);
				return -EINVAL;
			}

			rc = spdk_mem_map_set_translation(g_vtophys_map, (uint64_t)vaddr, VALUE_2MB, paddr);
			break;
		case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
			rc = spdk_mem_map_clear_translation(g_vtophys_map, (uint64_t)vaddr, VALUE_2MB);
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

void
spdk_vtophys_init(void)
{
	g_vtophys_map = spdk_mem_map_alloc(SPDK_MEM_TRANSLATION_ERROR, spdk_vtophys_notify, NULL);
	if (g_vtophys_map == NULL) {
		DEBUG_PRINT("vtophys map allocation failed\n");
		abort();
	}
}

uint64_t
spdk_vtophys(void *buf)
{
	uint64_t vaddr, paddr_2mb;

	vaddr = (uint64_t)buf;

	paddr_2mb = spdk_mem_map_translate(g_vtophys_map, vaddr);

	/*
	 * SPDK_MEM_TRANSLATION_ERROR has all bits set, so if the lookup returned SPDK_MEM_TRANSLATION_ERROR,
	 * we will still bitwise-or it with the buf offset below, but the result will still be
	 * SPDK_MEM_TRANSLATION_ERROR.
	 */
	SPDK_STATIC_ASSERT(SPDK_MEM_TRANSLATION_ERROR == UINT64_C(-1),
			   "SPDK_MEM_TRANSLATION_ERROR should be all 1s");
	return paddr_2mb | ((uint64_t)buf & MASK_2MB);
}
