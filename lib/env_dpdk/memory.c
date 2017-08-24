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

#define FN_2MB_TO_4KB(fn)	(fn << (SHIFT_2MB - SHIFT_4KB))
#define FN_4KB_TO_2MB(fn)	(fn >> (SHIFT_2MB - SHIFT_4KB))

#define MAP_128TB_IDX(vfn_2mb)	((vfn_2mb) >> (SHIFT_1GB - SHIFT_2MB))
#define MAP_1GB_IDX(vfn_2mb)	((vfn_2mb) & ((1ULL << (SHIFT_1GB - SHIFT_2MB + 1)) - 1))

/* Max value for a 16-bit ref count. */
#define MEM_MAX_REF_COUNT (0xFFFF)

/* Translation of a single 2MB page. */
struct map_2mb {
	uint64_t translation_2mb;
};

/* Second-level map table indexed by bits [21..29] of the virtual address.
 * Each entry contains the address translation or SPDK_MEM_TRANSLATION_ERROR for entries that haven't
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

static struct spdk_mem_map *g_mem_reg_map;
static TAILQ_HEAD(, spdk_mem_map) g_spdk_mem_maps = TAILQ_HEAD_INITIALIZER(g_spdk_mem_maps);
static pthread_mutex_t g_spdk_mem_map_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Walk the currently registered memory via the main memory registration map
 * and call the new map's notify callback for each virtually contiguous region.
 */
static void
spdk_mem_map_notify_walk(struct spdk_mem_map *map, enum spdk_mem_map_notify_action action)
{
	size_t idx_128tb;
	uint64_t contig_start = SPDK_MEM_TRANSLATION_ERROR;
	uint64_t contig_end = SPDK_MEM_TRANSLATION_ERROR;

#define END_RANGE()										\
         do {											\
                 if (contig_start != SPDK_MEM_TRANSLATION_ERROR) {					\
                         /* End of of a virtually contiguous range */				\
                         map->notify_cb(map->cb_ctx, map, action,				\
                                        (void *)contig_start,					\
                                        contig_end - contig_start + 2 * 1024 * 1024);		\
                 }										\
                 contig_start = SPDK_MEM_TRANSLATION_ERROR;						\
         } while (0)


	if (!g_mem_reg_map) {
		return;
	}

	/* Hold the memory registration map mutex so no new registrations can be added while we are looping. */
	pthread_mutex_lock(&g_mem_reg_map->mutex);

	for (idx_128tb = 0;
	     idx_128tb < sizeof(g_mem_reg_map->map_128tb.map) / sizeof(g_mem_reg_map->map_128tb.map[0]);
	     idx_128tb++) {
		const struct map_1gb *map_1gb = g_mem_reg_map->map_128tb.map[idx_128tb];
		uint64_t idx_1gb;

		if (!map_1gb) {
			END_RANGE();
			continue;
		}

		for (idx_1gb = 0; idx_1gb < sizeof(map_1gb->map) / sizeof(map_1gb->map[0]); idx_1gb++) {
			if (map_1gb->map[idx_1gb].translation_2mb != SPDK_MEM_TRANSLATION_ERROR) {
				/* Rebuild the virtual address from the indexes */
				uint64_t vaddr = (idx_128tb << SHIFT_1GB) | (idx_1gb << SHIFT_2MB);

				if (contig_start == SPDK_MEM_TRANSLATION_ERROR) {
					contig_start = vaddr;
				}
				contig_end = vaddr;
			} else {
				END_RANGE();
			}
		}
	}

	pthread_mutex_unlock(&g_mem_reg_map->mutex);
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

	if (notify_cb) {
		spdk_mem_map_notify_walk(map, SPDK_MEM_MAP_NOTIFY_REGISTER);
		TAILQ_INSERT_TAIL(&g_spdk_mem_maps, map, tailq);
	}

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

	if (!map) {
		return;
	}

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

int
spdk_mem_register(void *vaddr, size_t len)
{
	struct spdk_mem_map *map;
	int rc;
	void *seg_vaddr;
	size_t seg_len;

	if ((uintptr_t)vaddr & ~MASK_128TB) {
		DEBUG_PRINT("invalid usermode virtual address %p\n", vaddr);
		return -EINVAL;
	}

	if (((uintptr_t)vaddr & MASK_2MB) || (len & MASK_2MB)) {
		DEBUG_PRINT("invalid %s parameters, vaddr=%p len=%ju\n",
			    __func__, vaddr, len);
		return -EINVAL;
	}

	pthread_mutex_lock(&g_spdk_mem_map_mutex);

	seg_vaddr = vaddr;
	seg_len = 0;
	while (len > 0) {
		uint64_t ref_count;

		/* In g_mem_reg_map, the "translation" is the reference count */
		ref_count = spdk_mem_map_translate(g_mem_reg_map, (uint64_t)vaddr);
		spdk_mem_map_set_translation(g_mem_reg_map, (uint64_t)vaddr, VALUE_2MB, ref_count + 1);

		if (ref_count > 0) {
			if (seg_len > 0) {
				TAILQ_FOREACH(map, &g_spdk_mem_maps, tailq) {
					rc = map->notify_cb(map->cb_ctx, map, SPDK_MEM_MAP_NOTIFY_REGISTER, seg_vaddr, seg_len);
					if (rc != 0) {
						pthread_mutex_unlock(&g_spdk_mem_map_mutex);
						return rc;
					}
				}
			}

			seg_vaddr = vaddr + VALUE_2MB;
			seg_len = 0;
		} else {
			seg_len += VALUE_2MB;
		}

		vaddr += VALUE_2MB;
		len -= VALUE_2MB;
	}

	pthread_mutex_unlock(&g_spdk_mem_map_mutex);
	return 0;
}

int
spdk_mem_unregister(void *vaddr, size_t len)
{
	struct spdk_mem_map *map;
	int rc;
	void *seg_vaddr;
	size_t seg_len;

	pthread_mutex_lock(&g_spdk_mem_map_mutex);

	seg_vaddr = vaddr;
	seg_len = 0;
	while (len > 0) {
		uint64_t ref_count;

		/* In g_mem_reg_map, the "translation" is the reference count */
		ref_count = spdk_mem_map_translate(g_mem_reg_map, (uint64_t)vaddr);
		assert(ref_count > 0);
		spdk_mem_map_set_translation(g_mem_reg_map, (uint64_t)vaddr, VALUE_2MB, ref_count - 1);

		if (ref_count > 1) {
			if (seg_len > 0) {
				TAILQ_FOREACH(map, &g_spdk_mem_maps, tailq) {
					rc = map->notify_cb(map->cb_ctx, map, SPDK_MEM_MAP_NOTIFY_UNREGISTER, seg_vaddr, seg_len);
					if (rc != 0) {
						pthread_mutex_unlock(&g_spdk_mem_map_mutex);
						return rc;
					}
				}
			}

			seg_vaddr = vaddr + VALUE_2MB;
			seg_len = 0;
		} else {
			seg_len += VALUE_2MB;
		}

		vaddr += VALUE_2MB;
		len -= VALUE_2MB;
	}

	pthread_mutex_unlock(&g_spdk_mem_map_mutex);
	return 0;
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
			DEBUG_PRINT("allocation failed\n");
			return NULL;
		}
	}

	return map_1gb;
}

int
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
			DEBUG_PRINT("could not get %p map\n", (void *)vaddr);
			return -ENOMEM;
		}

		idx_1gb = MAP_1GB_IDX(vfn_2mb);
		map_2mb = &map_1gb->map[idx_1gb];
		ref_count = &map_1gb->ref_count[idx_1gb];

		if (*ref_count == MEM_MAX_REF_COUNT) {
			DEBUG_PRINT("ref count for %p already at %d\n",
				    (void *)vaddr, MEM_MAX_REF_COUNT);
			return -EBUSY;
		}

		map_2mb->translation_2mb = translation;

		(*ref_count)++;

		size -= 2 * 1024 * 1024;
		vfn_2mb++;
	}

	return 0;
}

int
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
			DEBUG_PRINT("could not get %p map\n", (void *)vaddr);
			return -ENOMEM;
		}

		idx_1gb = MAP_1GB_IDX(vfn_2mb);
		map_2mb = &map_1gb->map[idx_1gb];
		ref_count = &map_1gb->ref_count[idx_1gb];

		if (*ref_count == 0) {
			DEBUG_PRINT("vaddr %p not registered\n", (void *)vaddr);
			return -EINVAL;
		}

		(*ref_count)--;
		if (*ref_count == 0) {
			map_2mb->translation_2mb = map->default_translation;
		}

		size -= 2 * 1024 * 1024;
		vfn_2mb++;
	}

	return 0;
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
		DEBUG_PRINT("invalid usermode virtual address %p\n", (void *)vaddr);
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

void
spdk_mem_map_init(void)
{
	struct rte_mem_config *mcfg;
	size_t seg_idx;

	g_mem_reg_map = spdk_mem_map_alloc(SPDK_MEM_TRANSLATION_ERROR, NULL, NULL);
	if (g_mem_reg_map == NULL) {
		DEBUG_PRINT("memory registration map allocation failed\n");
		abort();
	}

	/*
	 * Walk all DPDK memory segments and register them
	 * with the master memory map
	 */
	mcfg = rte_eal_get_configuration()->mem_config;

	for (seg_idx = 0; seg_idx < RTE_MAX_MEMSEG; seg_idx++) {
		struct rte_memseg *seg = &mcfg->memseg[seg_idx];

		if (seg->addr == NULL) {
			break;
		}

		spdk_mem_register(seg->addr, seg->len);
	}
}
