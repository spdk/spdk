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

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include <rte_config.h>
#include <rte_eal.h>
#include <rte_eal_memconfig.h>

#include "spdk/env.h"

/* x86-64 userspace virtual addresses use only the low 47 bits [0..46],
 * which is enough to cover 128 TB.
 */
#define SHIFT_128TB	47 /* (1 << 47) == 128 TB */
#define MASK_128TB	((1ULL << SHIFT_128TB) - 1)

#define SHIFT_1GB	30 /* (1 << 30) == 1 GB */
#define MASK_1GB	((1ULL << SHIFT_1GB) - 1)

#define SHIFT_2MB	21 /* (1 << 21) == 2MB */
#define MASK_2MB	((1ULL << SHIFT_2MB) - 1)

#define SHIFT_4KB	12 /* (1 << 12) == 4KB */
#define MASK_4KB	((1ULL << SHIFT_4KB) - 1)

#define FN_2MB_TO_4KB(fn)	(fn << (SHIFT_2MB - SHIFT_4KB))
#define FN_4KB_TO_2MB(fn)	(fn >> (SHIFT_2MB - SHIFT_4KB))

#define MAP_128TB_IDX(vfn_2mb)	((vfn_2mb) >> (SHIFT_1GB - SHIFT_2MB))
#define MAP_1GB_IDX(vfn_2mb)	((vfn_2mb) & ((1ULL << (SHIFT_1GB - SHIFT_2MB + 1)) - 1))

/* Max value for a 48-bit PFN. */
#define INVALID_PFN	(0xFFFFFFFFFFFFULL)

/* Max value for a 16-bit ref count. */
#define VTOPHYS_MAX_REF_COUNT (0xFFFF)

/* Physical page frame number of a single 2MB page. */
struct map_2mb {
	uint64_t pfn_2mb : 48;
	uint64_t ref_count : 16;
};

/* Second-level map table indexed by bits [21..29] of the virtual address.
 * Each entry contains the 2MB physical page frame number or SPDK_VTOPHYS_ERROR for entries that haven't
 * been retrieved yet.
 */
struct map_1gb {
	struct map_2mb map[1ULL << (SHIFT_1GB - SHIFT_2MB + 1)];
};

/* Top-level map table indexed by bits [30..46] of the virtual address.
 * Each entry points to a second-level map table or NULL.
 */
struct map_128tb {
	struct map_1gb *map[1ULL << (SHIFT_128TB - SHIFT_1GB + 1)];
};

static struct map_128tb vtophys_map_128tb = {};
static pthread_mutex_t vtophys_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct map_2mb *
vtophys_get_map(uint64_t vfn_2mb)
{
	struct map_1gb *map_1gb;
	struct map_2mb *map_2mb;
	uint64_t idx_128tb = MAP_128TB_IDX(vfn_2mb);
	uint64_t idx_1gb = MAP_1GB_IDX(vfn_2mb);

	map_1gb = vtophys_map_128tb.map[idx_128tb];

	if (!map_1gb) {
		pthread_mutex_lock(&vtophys_mutex);

		/* Recheck to make sure nobody else got the mutex first. */
		map_1gb = vtophys_map_128tb.map[idx_128tb];
		if (!map_1gb) {
			map_1gb = malloc(sizeof(struct map_1gb));
			if (map_1gb) {
				/* initialize all entries to all 0xFF (SPDK_VTOPHYS_ERROR) */
				memset(map_1gb, 0xFF, sizeof(struct map_1gb));
				vtophys_map_128tb.map[idx_128tb] = map_1gb;
			}
		}

		pthread_mutex_unlock(&vtophys_mutex);

		if (!map_1gb) {
			printf("allocation failed\n");
			return NULL;
		}
	}

	map_2mb = &map_1gb->map[idx_1gb];
	return map_2mb;
}

static uint64_t
vtophys_get_dpdk_paddr(void *vaddr)
{
	uintptr_t paddr;

	paddr = rte_mem_virt2phy(vaddr);
	if (paddr == 0) {
		/*
		 * The vaddr was valid but returned 0.  Touch the page
		 *  to ensure a backing page gets assigned, then call
		 *  rte_mem_virt2phy() again.
		 */
		rte_atomic64_read((rte_atomic64_t *)vaddr);
		paddr = rte_mem_virt2phy(vaddr);
	}

	return paddr;
}

static uint64_t
vtophys_get_pfn_2mb(uint64_t vfn_2mb)
{
	uintptr_t vaddr, paddr;
	struct rte_mem_config *mcfg;
	struct rte_memseg *seg;
	uint32_t seg_idx;

	vaddr = vfn_2mb << SHIFT_2MB;
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
			return paddr >> SHIFT_2MB;
		}
	}

	fprintf(stderr, "could not find 2MB vfn 0x%jx in DPDK mem config\n", vfn_2mb);
	return INVALID_PFN;
}

static void
_spdk_vtophys_register_one(uint64_t vfn_2mb)
{
	struct map_2mb *map_2mb;
	void *vaddr;
	uint64_t paddr;

	map_2mb = vtophys_get_map(vfn_2mb);
	if (!map_2mb) {
		fprintf(stderr, "could not get vfn_2mb %p map\n", (void *)vfn_2mb);
		return;
	}

	if (map_2mb->pfn_2mb == INVALID_PFN) {
		vaddr = (void *)(vfn_2mb << SHIFT_2MB);
		paddr = vtophys_get_dpdk_paddr(vaddr);
		if (paddr == RTE_BAD_PHYS_ADDR) {
			fprintf(stderr, "could not get phys addr for %p\n", vaddr);
			return;
		}

		map_2mb->pfn_2mb = paddr >> SHIFT_2MB;
		map_2mb->ref_count = 0;
	}

	if (map_2mb->ref_count == VTOPHYS_MAX_REF_COUNT) {
		fprintf(stderr, "ref count for %p already at %d\n",
			(void *)(vfn_2mb << SHIFT_2MB), VTOPHYS_MAX_REF_COUNT);
		return;
	}

	map_2mb->ref_count++;
}

static void
_spdk_vtophys_unregister_one(uint64_t vfn_2mb)
{
	struct map_2mb *map_2mb;

	map_2mb = vtophys_get_map(vfn_2mb);
	if (!map_2mb) {
		fprintf(stderr, "could not get vfn_2mb %p map\n", (void *)vfn_2mb);
		return;
	}

	if (map_2mb->pfn_2mb == INVALID_PFN || map_2mb->ref_count == 0) {
		fprintf(stderr, "vaddr %p not registered\n", (void *)(vfn_2mb << SHIFT_2MB));
		return;
	}

	map_2mb->ref_count--;
	if (map_2mb->ref_count == 0) {
		map_2mb->pfn_2mb = INVALID_PFN;
	}
}

void
spdk_vtophys_register(void *vaddr, uint64_t len)
{
	uint64_t vfn_2mb;

	if ((uintptr_t)vaddr & ~MASK_128TB) {
		printf("invalid usermode virtual address %p\n", vaddr);
		return;
	}

	if (((uintptr_t)vaddr & MASK_2MB) || (len & MASK_2MB)) {
		fprintf(stderr, "invalid %s parameters, vaddr=%p len=%ju\n",
			__func__, vaddr, len);
		return;
	}

	vfn_2mb = (uintptr_t)vaddr >> SHIFT_2MB;
	len = len >> SHIFT_2MB;

	while (len > 0) {
		_spdk_vtophys_register_one(vfn_2mb);
		vfn_2mb++;
		len--;
	}
}

void
spdk_vtophys_unregister(void *vaddr, uint64_t len)
{
	uint64_t vfn_2mb;

	if ((uintptr_t)vaddr & ~MASK_128TB) {
		printf("invalid usermode virtual address %p\n", vaddr);
		return;
	}

	if (((uintptr_t)vaddr & MASK_2MB) || (len & MASK_2MB)) {
		fprintf(stderr, "invalid %s parameters, vaddr=%p len=%ju\n",
			__func__, vaddr, len);
		return;
	}

	vfn_2mb = (uintptr_t)vaddr >> SHIFT_2MB;
	len = len >> SHIFT_2MB;

	while (len > 0) {
		_spdk_vtophys_unregister_one(vfn_2mb);
		vfn_2mb++;
		len--;
	}
}

uint64_t
spdk_vtophys(void *buf)
{
	struct map_2mb *map_2mb;
	uint64_t vaddr, vfn_2mb, pfn_2mb;

	vaddr = (uint64_t)buf;
	if (vaddr & ~MASK_128TB) {
		printf("invalid usermode virtual address %p\n", buf);
		return SPDK_VTOPHYS_ERROR;
	}

	vfn_2mb = vaddr >> SHIFT_2MB;

	map_2mb = vtophys_get_map(vfn_2mb);
	if (!map_2mb) {
		return SPDK_VTOPHYS_ERROR;
	}

	pfn_2mb = map_2mb->pfn_2mb;
	if (pfn_2mb == INVALID_PFN) {
		pfn_2mb = vtophys_get_pfn_2mb(vfn_2mb);
		if (pfn_2mb == INVALID_PFN) {
			return SPDK_VTOPHYS_ERROR;
		}
		map_2mb->pfn_2mb = pfn_2mb;
	}

	return (pfn_2mb << SHIFT_2MB) | ((uint64_t)buf & MASK_2MB);
}
