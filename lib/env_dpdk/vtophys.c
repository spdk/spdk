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

#include "spdk/assert.h"
#include "spdk/likely.h"
#include "spdk/queue.h"
#include "spdk/util.h"

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

/* Translation of a single 2MB page. */
struct map_2mb {
	uint64_t translation_2mb;
};

/* Second-level map table indexed by bits [21..29] of the virtual address.
 * Each entry contains the address translation or SPDK_VTOPHYS_ERROR for entries that haven't
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

/* Page-granularity memory address translation */
struct spdk_mem_map {
	struct map_128tb map_128tb;
	pthread_mutex_t mutex;
	uint64_t default_translation;
	spdk_mem_map_notify_cb notify_cb;
	void *cb_ctx;
	TAILQ_ENTRY(spdk_mem_map) tailq;
};

static struct spdk_mem_map *g_vtophys_map;
static TAILQ_HEAD(, spdk_mem_map) g_spdk_mem_maps = TAILQ_HEAD_INITIALIZER(g_spdk_mem_maps);
static pthread_mutex_t g_spdk_mem_map_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Walk the currently registered memory via the main vtophys map
 * and call the new map's notify callback for each virtually contiguous region.
 */
static void
spdk_mem_map_notify_walk(struct spdk_mem_map *map, enum spdk_mem_map_notify_action action)
{
	size_t idx_128tb;
	uint64_t contig_start = SPDK_VTOPHYS_ERROR;
	uint64_t contig_end = SPDK_VTOPHYS_ERROR;

#define END_RANGE()										\
	do {											\
		if (contig_start != SPDK_VTOPHYS_ERROR) {					\
			/* End of of a virtually contiguous range */				\
			map->notify_cb(map->cb_ctx, map, action,				\
				       (void *)contig_start,					\
				       contig_end - contig_start + 2 * 1024 * 1024);		\
		}										\
		contig_start = SPDK_VTOPHYS_ERROR;						\
	} while (0)


	if (!g_vtophys_map) {
		return;
	}

	/* Hold the vtophys mutex so no new registrations can be added while we are looping. */
	pthread_mutex_lock(&g_vtophys_map->mutex);

	for (idx_128tb = 0;
	     idx_128tb < sizeof(g_vtophys_map->map_128tb.map) / sizeof(g_vtophys_map->map_128tb.map[0]);
	     idx_128tb++) {
		const struct map_1gb *map_1gb = g_vtophys_map->map_128tb.map[idx_128tb];
		uint64_t idx_1gb;

		if (!map_1gb) {
			END_RANGE();
			continue;
		}

		for (idx_1gb = 0; idx_1gb < sizeof(map_1gb->map) / sizeof(map_1gb->map[0]); idx_1gb++) {
			if (map_1gb->map[idx_1gb].translation_2mb != SPDK_VTOPHYS_ERROR) {
				/* Rebuild the virtual address from the indexes */
				uint64_t vaddr = (idx_128tb << SHIFT_1GB) | (idx_1gb << SHIFT_2MB);

				if (contig_start == SPDK_VTOPHYS_ERROR) {
					contig_start = vaddr;
				}
				contig_end = vaddr;
			} else {
				END_RANGE();
			}
		}
	}

	pthread_mutex_unlock(&g_vtophys_map->mutex);
}

struct spdk_mem_map *
spdk_mem_map_alloc(uint64_t default_translation, spdk_mem_map_notify_cb notify_cb, void *cb_ctx)
{
	struct spdk_mem_map *map;

	map = calloc(1, sizeof(*map));
	if (map == NULL) {
		return NULL;
	}

	if (pthread_mutex_init(&map->mutex, NULL)) {
		free(map);
		return NULL;
	}

	map->default_translation = default_translation;
	map->notify_cb = notify_cb;
	map->cb_ctx = cb_ctx;

	pthread_mutex_lock(&g_spdk_mem_map_mutex);

	spdk_mem_map_notify_walk(map, SPDK_MEM_MAP_NOTIFY_REGISTER);
	TAILQ_INSERT_TAIL(&g_spdk_mem_maps, map, tailq);

	pthread_mutex_unlock(&g_spdk_mem_map_mutex);

	return map;
}

void
spdk_mem_map_free(struct spdk_mem_map **pmap)
{
	struct spdk_mem_map *map;
	size_t i;

	if (!pmap) {
		return;
	}

	map = *pmap;

	pthread_mutex_lock(&g_spdk_mem_map_mutex);
	spdk_mem_map_notify_walk(map, SPDK_MEM_MAP_NOTIFY_UNREGISTER);
	TAILQ_REMOVE(&g_spdk_mem_maps, map, tailq);
	pthread_mutex_unlock(&g_spdk_mem_map_mutex);

	for (i = 0; i < sizeof(map->map_128tb.map) / sizeof(map->map_128tb.map[0]); i++) {
		free(map->map_128tb.map[i]);
	}

	pthread_mutex_destroy(&map->mutex);

	free(map);
	*pmap = NULL;
}

void
spdk_mem_register(void *vaddr, size_t len)
{
	struct spdk_mem_map *map;

	pthread_mutex_lock(&g_spdk_mem_map_mutex);
	TAILQ_FOREACH(map, &g_spdk_mem_maps, tailq) {
		map->notify_cb(map->cb_ctx, map, SPDK_MEM_MAP_NOTIFY_REGISTER, vaddr, len);
	}
	pthread_mutex_unlock(&g_spdk_mem_map_mutex);
}

void
spdk_mem_unregister(void *vaddr, size_t len)
{
	struct spdk_mem_map *map;

	pthread_mutex_lock(&g_spdk_mem_map_mutex);
	TAILQ_FOREACH(map, &g_spdk_mem_maps, tailq) {
		map->notify_cb(map->cb_ctx, map, SPDK_MEM_MAP_NOTIFY_UNREGISTER, vaddr, len);
	}
	pthread_mutex_unlock(&g_spdk_mem_map_mutex);
}

static struct map_1gb *
spdk_mem_map_get_map_1gb(struct spdk_mem_map *map, uint64_t vfn_2mb)
{
	struct map_1gb *map_1gb;
	uint64_t idx_128tb = MAP_128TB_IDX(vfn_2mb);
	size_t i;

	map_1gb = map->map_128tb.map[idx_128tb];

	if (!map_1gb) {
		pthread_mutex_lock(&map->mutex);

		/* Recheck to make sure nobody else got the mutex first. */
		map_1gb = map->map_128tb.map[idx_128tb];
		if (!map_1gb) {
			map_1gb = malloc(sizeof(struct map_1gb));
			if (map_1gb) {
				/* initialize all entries to default translation */
				for (i = 0; i < SPDK_COUNTOF(map_1gb->map); i++) {
					map_1gb->map[i].translation_2mb = map->default_translation;
				}
				memset(map_1gb->ref_count, 0, sizeof(map_1gb->ref_count));
				map->map_128tb.map[idx_128tb] = map_1gb;
			}
		}

		pthread_mutex_unlock(&map->mutex);

		if (!map_1gb) {
#ifdef DEBUG
			printf("allocation failed\n");
#endif
			return NULL;
		}
	}

	return map_1gb;
}

void
spdk_mem_map_set_translation(struct spdk_mem_map *map, uint64_t vaddr, uint64_t size,
			     uint64_t translation)
{
	uint64_t vfn_2mb;
	struct map_1gb *map_1gb;
	uint64_t idx_1gb;
	struct map_2mb *map_2mb;
	uint16_t *ref_count;

	/* For now, only 2 MB-aligned registrations are supported */
	assert(size % (2 * 1024 * 1024) == 0);
	assert((vaddr & MASK_2MB) == 0);

	vfn_2mb = vaddr >> SHIFT_2MB;

	while (size) {
		map_1gb = spdk_mem_map_get_map_1gb(map, vfn_2mb);
		if (!map_1gb) {
#ifdef DEBUG
			fprintf(stderr, "could not get %p map\n", (void *)vaddr);
#endif
			return;
		}

		idx_1gb = MAP_1GB_IDX(vfn_2mb);
		map_2mb = &map_1gb->map[idx_1gb];
		ref_count = &map_1gb->ref_count[idx_1gb];

		if (*ref_count == VTOPHYS_MAX_REF_COUNT) {
#ifdef DEBUG
			fprintf(stderr, "ref count for %p already at %d\n",
				(void *)vaddr, VTOPHYS_MAX_REF_COUNT);
#endif
			return;
		}

		map_2mb->translation_2mb = translation;

		(*ref_count)++;

		size -= 2 * 1024 * 1024;
		vfn_2mb++;
	}
}

void
spdk_mem_map_clear_translation(struct spdk_mem_map *map, uint64_t vaddr, uint64_t size)
{
	uint64_t vfn_2mb;
	struct map_1gb *map_1gb;
	uint64_t idx_1gb;
	struct map_2mb *map_2mb;
	uint16_t *ref_count;

	/* For now, only 2 MB-aligned registrations are supported */
	assert(size % (2 * 1024 * 1024) == 0);
	assert((vaddr & MASK_2MB) == 0);

	vfn_2mb = vaddr >> SHIFT_2MB;

	while (size) {
		map_1gb = spdk_mem_map_get_map_1gb(map, vfn_2mb);
		if (!map_1gb) {
#ifdef DEBUG
			fprintf(stderr, "could not get %p map\n", (void *)vaddr);
#endif
			return;
		}

		idx_1gb = MAP_1GB_IDX(vfn_2mb);
		map_2mb = &map_1gb->map[idx_1gb];
		ref_count = &map_1gb->ref_count[idx_1gb];

		if (*ref_count == 0) {
#ifdef DEBUG
			fprintf(stderr, "vaddr %p not registered\n", (void *)vaddr);
#endif
			return;
		}

		(*ref_count)--;
		if (*ref_count == 0) {
			map_2mb->translation_2mb = map->default_translation;
		}

		size -= 2 * 1024 * 1024;
		vfn_2mb++;
	}
}

uint64_t
spdk_mem_map_translate(const struct spdk_mem_map *map, uint64_t vaddr)
{
	const struct map_1gb *map_1gb;
	const struct map_2mb *map_2mb;
	uint64_t idx_128tb;
	uint64_t idx_1gb;
	uint64_t vfn_2mb;

	if (spdk_unlikely(vaddr & ~MASK_128TB)) {
#ifdef DEBUG
		printf("invalid usermode virtual address %p\n", (void *)vaddr);
#endif
		return map->default_translation;
	}

	vfn_2mb = vaddr >> SHIFT_2MB;
	idx_128tb = MAP_128TB_IDX(vfn_2mb);
	idx_1gb = MAP_1GB_IDX(vfn_2mb);

	map_1gb = map->map_128tb.map[idx_128tb];
	if (spdk_unlikely(!map_1gb)) {
		return map->default_translation;
	}

	map_2mb = &map_1gb->map[idx_1gb];

	return map_2mb->translation_2mb;
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
	if (paddr & MASK_2MB) {
#ifdef DEBUG
		fprintf(stderr, "invalid paddr 0x%" PRIx64 " - must be 2MB aligned\n", paddr);
#endif
		return;
	}

	spdk_mem_map_set_translation(g_vtophys_map, vfn_2mb << SHIFT_2MB, 2 * 1024 * 1024, paddr);
}

static void
_spdk_vtophys_unregister_one(uint64_t vfn_2mb)
{
	spdk_mem_map_clear_translation(g_vtophys_map, vfn_2mb << SHIFT_2MB, 2 * 1024 * 1024);
}

static void
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

static void
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

static void
spdk_vtophys_notify(void *cb_ctx, struct spdk_mem_map *map,
		    enum spdk_mem_map_notify_action action,
		    void *vaddr, size_t len)
{
	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		spdk_vtophys_register(vaddr, len);
		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
		spdk_vtophys_unregister(vaddr, len);
		break;
	}
}

void
spdk_vtophys_register_dpdk_mem(void)
{
	struct rte_mem_config *mcfg;
	size_t seg_idx;

	g_vtophys_map = spdk_mem_map_alloc(SPDK_VTOPHYS_ERROR, spdk_vtophys_notify, NULL);
	if (g_vtophys_map == NULL) {
		fprintf(stderr, "vtophys map allocation failed\n");
		abort();
	}

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
	uint64_t vaddr, paddr_2mb;

	vaddr = (uint64_t)buf;

	paddr_2mb = spdk_mem_map_translate(g_vtophys_map, vaddr);

	/*
	 * SPDK_VTOPHYS_ERROR has all bits set, so if the lookup returned SPDK_VTOPHYS_ERROR,
	 * we will still bitwise-or it with the buf offset below, but the result will still be
	 * SPDK_VTOPHYS_ERROR.
	 */
	SPDK_STATIC_ASSERT(SPDK_VTOPHYS_ERROR == UINT64_C(-1), "SPDK_VTOPHYS_ERROR should be all 1s");
	return paddr_2mb | ((uint64_t)buf & MASK_2MB);
}
