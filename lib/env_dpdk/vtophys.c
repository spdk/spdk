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

#include "env_internal.h"

#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include <rte_config.h>
#include <rte_eal_memconfig.h>

#include "spdk/assert.h"
#include "spdk/likely.h"

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

/* Max value for a 16-bit ref count. */
#define VTOPHYS_MAX_REF_COUNT (0xFFFF)

/* Physical address of a single 2MB page. */
struct map_2mb {
	uint64_t paddr_2mb;
};

/* Second-level map table indexed by bits [21..29] of the virtual address.
 * Each entry contains the 2MB physical address or SPDK_VTOPHYS_ERROR for entries that haven't
 * been retrieved yet.
 */
struct map_1gb {
	struct map_2mb map[1ULL << (SHIFT_1GB - SHIFT_2MB + 1)];
	uint16_t ref_count[1ULL << (SHIFT_1GB - SHIFT_2MB + 1)];
};

/* Top-level map table indexed by bits [30..46] of the virtual address.
 * Each entry points to a second-level map table or NULL.
 */
struct map_128tb {
	struct map_1gb *map[1ULL << (SHIFT_128TB - SHIFT_1GB + 1)];
};

static struct map_128tb vtophys_map_128tb = {};
static pthread_mutex_t vtophys_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct map_1gb *
vtophys_get_map_1gb(uint64_t vfn_2mb)
{
	struct map_1gb *map_1gb;
	uint64_t idx_128tb = MAP_128TB_IDX(vfn_2mb);

	map_1gb = vtophys_map_128tb.map[idx_128tb];

	if (!map_1gb) {
		pthread_mutex_lock(&vtophys_mutex);

		/* Recheck to make sure nobody else got the mutex first. */
		map_1gb = vtophys_map_128tb.map[idx_128tb];
		if (!map_1gb) {
			map_1gb = malloc(sizeof(struct map_1gb));
			if (map_1gb) {
				/* initialize all entries to all 0xFF (SPDK_VTOPHYS_ERROR) */
				memset(map_1gb->map, 0xFF, sizeof(map_1gb->map));
				memset(map_1gb->ref_count, 0, sizeof(map_1gb->ref_count));
				vtophys_map_128tb.map[idx_128tb] = map_1gb;
			}
		}

		pthread_mutex_unlock(&vtophys_mutex);

		if (!map_1gb) {
#ifdef DEBUG
			printf("allocation failed\n");
#endif
			return NULL;
		}
	}

	return map_1gb;
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
vtophys_get_paddr(uint64_t vaddr)
{
	uintptr_t paddr;
	struct rte_mem_config *mcfg;
	struct rte_memseg *seg;
	uint32_t seg_idx;

	paddr = vtophys_get_dpdk_paddr((void *)vaddr);
	if (paddr != RTE_BAD_PHYS_ADDR) {
		return paddr;
	}

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

#ifdef DEBUG
	fprintf(stderr, "could not find vaddr 0x%" PRIx64 " in DPDK mem config\n", vaddr);
#endif
	return SPDK_VTOPHYS_ERROR;
}

static void
_spdk_vtophys_register_one(uint64_t vfn_2mb, uint64_t paddr)
{
	struct map_1gb *map_1gb;
	uint64_t idx_1gb = MAP_1GB_IDX(vfn_2mb);
	struct map_2mb *map_2mb;
	uint16_t *ref_count;

	if (paddr & MASK_2MB) {
#ifdef DEBUG
		fprintf(stderr, "invalid paddr 0x%" PRIx64 " - must be 2MB aligned\n", paddr);
#endif
		return;
	}

	map_1gb = vtophys_get_map_1gb(vfn_2mb);
	if (!map_1gb) {
#ifdef DEBUG
		fprintf(stderr, "could not get vfn_2mb %p map\n", (void *)vfn_2mb);
#endif
		return;
	}

	map_2mb = &map_1gb->map[idx_1gb];
	ref_count = &map_1gb->ref_count[idx_1gb];

	if (*ref_count == VTOPHYS_MAX_REF_COUNT) {
#ifdef DEBUG
		fprintf(stderr, "ref count for %p already at %d\n",
			(void *)(vfn_2mb << SHIFT_2MB), VTOPHYS_MAX_REF_COUNT);
#endif
		return;
	}

	map_2mb->paddr_2mb = paddr;

	(*ref_count)++;
}

static void
_spdk_vtophys_unregister_one(uint64_t vfn_2mb)
{
	struct map_1gb *map_1gb;
	uint64_t idx_1gb = MAP_1GB_IDX(vfn_2mb);
	struct map_2mb *map_2mb;
	uint16_t *ref_count;

	map_1gb = vtophys_get_map_1gb(vfn_2mb);
	if (!map_1gb) {
#ifdef DEBUG
		fprintf(stderr, "could not get vfn_2mb %p map\n", (void *)vfn_2mb);
#endif
		return;
	}

	map_2mb = &map_1gb->map[idx_1gb];
	ref_count = &map_1gb->ref_count[idx_1gb];

	if (map_2mb->paddr_2mb == SPDK_VTOPHYS_ERROR || *ref_count == 0) {
#ifdef DEBUG
		fprintf(stderr, "vaddr %p not registered\n", (void *)(vfn_2mb << SHIFT_2MB));
#endif
		return;
	}

	(*ref_count)--;
	if (*ref_count == 0) {
		map_2mb->paddr_2mb = SPDK_VTOPHYS_ERROR;
	}
}

void
spdk_vtophys_register(void *vaddr, uint64_t len)
{
	uint64_t vfn_2mb;

	if ((uintptr_t)vaddr & ~MASK_128TB) {
#ifdef DEBUG
		printf("invalid usermode virtual address %p\n", vaddr);
#endif
		return;
	}

	if (((uintptr_t)vaddr & MASK_2MB) || (len & MASK_2MB)) {
#ifdef DEBUG
		fprintf(stderr, "invalid %s parameters, vaddr=%p len=%ju\n",
			__func__, vaddr, len);
#endif
		return;
	}

	vfn_2mb = (uintptr_t)vaddr >> SHIFT_2MB;
	len = len >> SHIFT_2MB;

	while (len > 0) {
		uint64_t vaddr = vfn_2mb << SHIFT_2MB;
		uint64_t paddr = vtophys_get_paddr(vaddr);

		if (paddr == RTE_BAD_PHYS_ADDR) {
#ifdef DEBUG
			fprintf(stderr, "could not get phys addr for 0x%" PRIx64 "\n", vaddr);
#endif
			return;
		}

		_spdk_vtophys_register_one(vfn_2mb, paddr);
		vfn_2mb++;
		len--;
	}
}

void
spdk_vtophys_unregister(void *vaddr, uint64_t len)
{
	uint64_t vfn_2mb;

	if ((uintptr_t)vaddr & ~MASK_128TB) {
#ifdef DEBUG
		printf("invalid usermode virtual address %p\n", vaddr);
#endif
		return;
	}

	if (((uintptr_t)vaddr & MASK_2MB) || (len & MASK_2MB)) {
#ifdef DEBUG
		fprintf(stderr, "invalid %s parameters, vaddr=%p len=%ju\n",
			__func__, vaddr, len);
#endif
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

void
spdk_vtophys_register_dpdk_mem(void)
{
	struct rte_mem_config *mcfg;
	size_t seg_idx;

	mcfg = rte_eal_get_configuration()->mem_config;

	for (seg_idx = 0; seg_idx < RTE_MAX_MEMSEG; seg_idx++) {
		struct rte_memseg *seg = &mcfg->memseg[seg_idx];

		if (seg->addr == NULL) {
			break;
		}

		spdk_vtophys_register(seg->addr, seg->len);
	}
}

uint64_t
spdk_vtophys(void *buf)
{
	struct map_1gb *map_1gb;
	struct map_2mb *map_2mb;
	uint64_t idx_128tb;
	uint64_t idx_1gb;
	uint64_t vaddr, vfn_2mb, paddr_2mb;

	vaddr = (uint64_t)buf;
	if (spdk_unlikely(vaddr & ~MASK_128TB)) {
#ifdef DEBUG
		printf("invalid usermode virtual address %p\n", buf);
#endif
		return SPDK_VTOPHYS_ERROR;
	}

	vfn_2mb = vaddr >> SHIFT_2MB;
	idx_128tb = MAP_128TB_IDX(vfn_2mb);
	idx_1gb = MAP_1GB_IDX(vfn_2mb);

	map_1gb = vtophys_map_128tb.map[idx_128tb];
	if (spdk_unlikely(!map_1gb)) {
		return SPDK_VTOPHYS_ERROR;
	}

	map_2mb = &map_1gb->map[idx_1gb];

	paddr_2mb = map_2mb->paddr_2mb;

	/*
	 * SPDK_VTOPHYS_ERROR has all bits set, so if the lookup returned SPDK_VTOPHYS_ERROR,
	 * we will still bitwise-or it with the buf offset below, but the result will still be
	 * SPDK_VTOPHYS_ERROR.
	 */
	SPDK_STATIC_ASSERT(SPDK_VTOPHYS_ERROR == UINT64_C(-1), "SPDK_VTOPHYS_ERROR should be all 1s");
	return paddr_2mb | ((uint64_t)buf & MASK_2MB);
}
