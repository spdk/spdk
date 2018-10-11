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

#define MAP_256TB_IDX(vfn_2mb)	((vfn_2mb) >> (SHIFT_1GB - SHIFT_2MB))
#define MAP_1GB_IDX(vfn_2mb)	((vfn_2mb) & ((1ULL << (SHIFT_1GB - SHIFT_2MB)) - 1))

/* Page is registered */
#define REG_MAP_REGISTERED	(1ULL << 62)

/* A notification region barrier. The 2MB translation entry that's marked
 * with this flag must be unregistered separately. This allows contiguous
 * regions to be unregistered in the same chunks they were registered.
 */
#define REG_MAP_NOTIFY_START	(1ULL << 63)

/* Translation of a single 2MB page. */
struct map_2mb {
	uint64_t translation_2mb;
};

/* Second-level map table indexed by bits [21..29] of the virtual address.
 * Each entry contains the address translation or error for entries that haven't
 * been retrieved yet.
 */
struct map_1gb {
	struct map_2mb map[1ULL << (SHIFT_1GB - SHIFT_2MB)];
};

/* Top-level map table indexed by bits [30..47] of the virtual address.
 * Each entry points to a second-level map table or NULL.
 */
struct map_256tb {
	struct map_1gb *map[1ULL << (SHIFT_256TB - SHIFT_1GB)];
};

/* Page-granularity memory address translation */
struct spdk_mem_map {
	struct map_256tb map_256tb;
	pthread_mutex_t mutex;
	uint64_t default_translation;
	struct spdk_mem_map_ops ops;
	void *cb_ctx;
	TAILQ_ENTRY(spdk_mem_map) tailq;
};

/* Registrations map. The 64 bit translations are bit fields with the
 * following layout (starting with the low bits):
 *    0 - 61 : reserved
 *   62 - 63 : flags
 */
static struct spdk_mem_map *g_mem_reg_map;
static TAILQ_HEAD(, spdk_mem_map) g_spdk_mem_maps = TAILQ_HEAD_INITIALIZER(g_spdk_mem_maps);
static pthread_mutex_t g_spdk_mem_map_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Walk the currently registered memory via the main memory registration map
 * and call the new map's notify callback for each virtually contiguous region.
 */
static int
spdk_mem_map_notify_walk(struct spdk_mem_map *map, enum spdk_mem_map_notify_action action)
{
	size_t idx_256tb;
	uint64_t idx_1gb;
	uint64_t contig_start = UINT64_MAX;
	uint64_t contig_end = UINT64_MAX;
	struct map_1gb *map_1gb;
	int rc;

	if (!g_mem_reg_map) {
		return -EINVAL;
	}

	/* Hold the memory registration map mutex so no new registrations can be added while we are looping. */
	pthread_mutex_lock(&g_mem_reg_map->mutex);

	for (idx_256tb = 0;
	     idx_256tb < sizeof(g_mem_reg_map->map_256tb.map) / sizeof(g_mem_reg_map->map_256tb.map[0]);
	     idx_256tb++) {
		map_1gb = g_mem_reg_map->map_256tb.map[idx_256tb];

		if (!map_1gb) {
			if (contig_start != UINT64_MAX) {
				/* End of of a virtually contiguous range */
				rc = map->ops.notify_cb(map->cb_ctx, map, action,
							(void *)contig_start,
							contig_end - contig_start + VALUE_2MB);
				/* Don't bother handling unregister failures. It can't be any worse */
				if (rc != 0 && action == SPDK_MEM_MAP_NOTIFY_REGISTER) {
					goto err_unregister;
				}
			}
			contig_start = UINT64_MAX;
			continue;
		}

		for (idx_1gb = 0; idx_1gb < sizeof(map_1gb->map) / sizeof(map_1gb->map[0]); idx_1gb++) {
			if ((map_1gb->map[idx_1gb].translation_2mb & REG_MAP_REGISTERED) &&
			    (contig_start == UINT64_MAX ||
			     (map_1gb->map[idx_1gb].translation_2mb & REG_MAP_NOTIFY_START) == 0)) {
				/* Rebuild the virtual address from the indexes */
				uint64_t vaddr = (idx_256tb << SHIFT_1GB) | (idx_1gb << SHIFT_2MB);

				if (contig_start == UINT64_MAX) {
					contig_start = vaddr;
				}

				contig_end = vaddr;
			} else {
				if (contig_start != UINT64_MAX) {
					/* End of of a virtually contiguous range */
					rc = map->ops.notify_cb(map->cb_ctx, map, action,
								(void *)contig_start,
								contig_end - contig_start + VALUE_2MB);
					/* Don't bother handling unregister failures. It can't be any worse */
					if (rc != 0 && action == SPDK_MEM_MAP_NOTIFY_REGISTER) {
						goto err_unregister;
					}

					/* This page might be a part of a neighbour region, so process
					 * it again. The idx_1gb will be incremented immediately.
					 */
					idx_1gb--;
				}
				contig_start = UINT64_MAX;
			}
		}
	}

	pthread_mutex_unlock(&g_mem_reg_map->mutex);
	return 0;

err_unregister:
	/* Unwind to the first empty translation so we don't unregister
	 * a region that just failed to register.
	 */
	idx_256tb = MAP_256TB_IDX((contig_start >> SHIFT_2MB) - 1);
	idx_1gb = MAP_1GB_IDX((contig_start >> SHIFT_2MB) - 1);
	contig_start = UINT64_MAX;
	contig_end = UINT64_MAX;

	/* Unregister any memory we managed to register before the failure */
	for (; idx_256tb < SIZE_MAX; idx_256tb--) {
		map_1gb = g_mem_reg_map->map_256tb.map[idx_256tb];

		if (!map_1gb) {
			if (contig_end != UINT64_MAX) {
				/* End of of a virtually contiguous range */
				map->ops.notify_cb(map->cb_ctx, map,
						   SPDK_MEM_MAP_NOTIFY_UNREGISTER,
						   (void *)contig_start,
						   contig_end - contig_start + VALUE_2MB);
			}
			contig_end = UINT64_MAX;
			continue;
		}

		for (; idx_1gb < UINT64_MAX; idx_1gb--) {
			if ((map_1gb->map[idx_1gb].translation_2mb & REG_MAP_REGISTERED) &&
			    (contig_end == UINT64_MAX || (map_1gb->map[idx_1gb].translation_2mb & REG_MAP_NOTIFY_START) == 0)) {
				/* Rebuild the virtual address from the indexes */
				uint64_t vaddr = (idx_256tb << SHIFT_1GB) | (idx_1gb << SHIFT_2MB);

				if (contig_end == UINT64_MAX) {
					contig_end = vaddr;
				}
				contig_start = vaddr;
			} else {
				if (contig_end != UINT64_MAX) {
					/* End of of a virtually contiguous range */
					map->ops.notify_cb(map->cb_ctx, map,
							   SPDK_MEM_MAP_NOTIFY_UNREGISTER,
							   (void *)contig_start,
							   contig_end - contig_start + VALUE_2MB);
					idx_1gb++;
				}
				contig_end = UINT64_MAX;
			}
		}
		idx_1gb = sizeof(map_1gb->map) / sizeof(map_1gb->map[0]) - 1;
	}

	pthread_mutex_unlock(&g_mem_reg_map->mutex);
	return rc;
}

struct spdk_mem_map *
spdk_mem_map_alloc(uint64_t default_translation, const struct spdk_mem_map_ops *ops, void *cb_ctx)
{
	struct spdk_mem_map *map;
	int rc;

	map = calloc(1, sizeof(*map));
	if (map == NULL) {
		return NULL;
	}

	if (pthread_mutex_init(&map->mutex, NULL)) {
		free(map);
		return NULL;
	}

	map->default_translation = default_translation;
	map->cb_ctx = cb_ctx;
	if (ops) {
		map->ops = *ops;
	}

	if (ops && ops->notify_cb) {
		pthread_mutex_lock(&g_spdk_mem_map_mutex);
		rc = spdk_mem_map_notify_walk(map, SPDK_MEM_MAP_NOTIFY_REGISTER);
		if (rc != 0) {
			pthread_mutex_unlock(&g_spdk_mem_map_mutex);
			DEBUG_PRINT("Initial mem_map notify failed\n");
			pthread_mutex_destroy(&map->mutex);
			free(map);
			return NULL;
		}
		TAILQ_INSERT_TAIL(&g_spdk_mem_maps, map, tailq);
		pthread_mutex_unlock(&g_spdk_mem_map_mutex);
	}

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

	if (map->ops.notify_cb) {
		pthread_mutex_lock(&g_spdk_mem_map_mutex);
		spdk_mem_map_notify_walk(map, SPDK_MEM_MAP_NOTIFY_UNREGISTER);
		TAILQ_REMOVE(&g_spdk_mem_maps, map, tailq);
		pthread_mutex_unlock(&g_spdk_mem_map_mutex);
	}

	for (i = 0; i < sizeof(map->map_256tb.map) / sizeof(map->map_256tb.map[0]); i++) {
		free(map->map_256tb.map[i]);
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
	uint64_t reg;

	if ((uintptr_t)vaddr & ~MASK_256TB) {
		DEBUG_PRINT("invalid usermode virtual address %p\n", vaddr);
		return -EINVAL;
	}

	if (((uintptr_t)vaddr & MASK_2MB) || (len & MASK_2MB)) {
		DEBUG_PRINT("invalid %s parameters, vaddr=%p len=%ju\n",
			    __func__, vaddr, len);
		return -EINVAL;
	}

	if (len == 0) {
		return 0;
	}

	pthread_mutex_lock(&g_spdk_mem_map_mutex);

	seg_vaddr = vaddr;
	seg_len = len;
	while (seg_len > 0) {
		reg = spdk_mem_map_translate(g_mem_reg_map, (uint64_t)seg_vaddr, NULL);
		if (reg & REG_MAP_REGISTERED) {
			pthread_mutex_unlock(&g_spdk_mem_map_mutex);
			return -EBUSY;
		}
		seg_vaddr += VALUE_2MB;
		seg_len -= VALUE_2MB;
	}

	seg_vaddr = vaddr;
	seg_len = 0;
	while (len > 0) {
		spdk_mem_map_set_translation(g_mem_reg_map, (uint64_t)vaddr, VALUE_2MB,
					     seg_len == 0 ? REG_MAP_REGISTERED | REG_MAP_NOTIFY_START : REG_MAP_REGISTERED);
		seg_len += VALUE_2MB;
		vaddr += VALUE_2MB;
		len -= VALUE_2MB;
	}

	TAILQ_FOREACH(map, &g_spdk_mem_maps, tailq) {
		rc = map->ops.notify_cb(map->cb_ctx, map, SPDK_MEM_MAP_NOTIFY_REGISTER, seg_vaddr, seg_len);
		if (rc != 0) {
			pthread_mutex_unlock(&g_spdk_mem_map_mutex);
			return rc;
		}
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
	uint64_t reg, newreg;

	if ((uintptr_t)vaddr & ~MASK_256TB) {
		DEBUG_PRINT("invalid usermode virtual address %p\n", vaddr);
		return -EINVAL;
	}

	if (((uintptr_t)vaddr & MASK_2MB) || (len & MASK_2MB)) {
		DEBUG_PRINT("invalid %s parameters, vaddr=%p len=%ju\n",
			    __func__, vaddr, len);
		return -EINVAL;
	}

	pthread_mutex_lock(&g_spdk_mem_map_mutex);

	/* The first page must be a start of a region. Also check if it's
	 * registered to make sure we don't return -ERANGE for non-registered
	 * regions.
	 */
	reg = spdk_mem_map_translate(g_mem_reg_map, (uint64_t)vaddr, NULL);
	if ((reg & REG_MAP_REGISTERED) && (reg & REG_MAP_NOTIFY_START) == 0) {
		pthread_mutex_unlock(&g_spdk_mem_map_mutex);
		return -ERANGE;
	}

	seg_vaddr = vaddr;
	seg_len = len;
	while (seg_len > 0) {
		reg = spdk_mem_map_translate(g_mem_reg_map, (uint64_t)seg_vaddr, NULL);
		if ((reg & REG_MAP_REGISTERED) == 0) {
			pthread_mutex_unlock(&g_spdk_mem_map_mutex);
			return -EINVAL;
		}
		seg_vaddr += VALUE_2MB;
		seg_len -= VALUE_2MB;
	}

	newreg = spdk_mem_map_translate(g_mem_reg_map, (uint64_t)seg_vaddr, NULL);
	/* If the next page is registered, it must be a start of a region as well,
	 * otherwise we'd be unregistering only a part of a region.
	 */
	if ((newreg & REG_MAP_NOTIFY_START) == 0 && (newreg & REG_MAP_REGISTERED)) {
		pthread_mutex_unlock(&g_spdk_mem_map_mutex);
		return -ERANGE;
	}
	seg_vaddr = vaddr;
	seg_len = 0;

	while (len > 0) {
		reg = spdk_mem_map_translate(g_mem_reg_map, (uint64_t)vaddr, NULL);
		spdk_mem_map_set_translation(g_mem_reg_map, (uint64_t)vaddr, VALUE_2MB, 0);

		if (seg_len > 0 && (reg & REG_MAP_NOTIFY_START)) {
			TAILQ_FOREACH(map, &g_spdk_mem_maps, tailq) {
				rc = map->ops.notify_cb(map->cb_ctx, map, SPDK_MEM_MAP_NOTIFY_UNREGISTER, seg_vaddr, seg_len);
				if (rc != 0) {
					pthread_mutex_unlock(&g_spdk_mem_map_mutex);
					return rc;
				}
			}

			seg_vaddr = vaddr;
			seg_len = VALUE_2MB;
		} else {
			seg_len += VALUE_2MB;
		}

		vaddr += VALUE_2MB;
		len -= VALUE_2MB;
	}

	if (seg_len > 0) {
		TAILQ_FOREACH(map, &g_spdk_mem_maps, tailq) {
			rc = map->ops.notify_cb(map->cb_ctx, map, SPDK_MEM_MAP_NOTIFY_UNREGISTER, seg_vaddr, seg_len);
			if (rc != 0) {
				pthread_mutex_unlock(&g_spdk_mem_map_mutex);
				return rc;
			}
		}
	}

	pthread_mutex_unlock(&g_spdk_mem_map_mutex);
	return 0;
}

static struct map_1gb *
spdk_mem_map_get_map_1gb(struct spdk_mem_map *map, uint64_t vfn_2mb)
{
	struct map_1gb *map_1gb;
	uint64_t idx_256tb = MAP_256TB_IDX(vfn_2mb);
	size_t i;

	if (spdk_unlikely(idx_256tb >= SPDK_COUNTOF(map->map_256tb.map))) {
		return NULL;
	}

	map_1gb = map->map_256tb.map[idx_256tb];

	if (!map_1gb) {
		pthread_mutex_lock(&map->mutex);

		/* Recheck to make sure nobody else got the mutex first. */
		map_1gb = map->map_256tb.map[idx_256tb];
		if (!map_1gb) {
			map_1gb = malloc(sizeof(struct map_1gb));
			if (map_1gb) {
				/* initialize all entries to default translation */
				for (i = 0; i < SPDK_COUNTOF(map_1gb->map); i++) {
					map_1gb->map[i].translation_2mb = map->default_translation;
				}
				map->map_256tb.map[idx_256tb] = map_1gb;
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

	if ((uintptr_t)vaddr & ~MASK_256TB) {
		DEBUG_PRINT("invalid usermode virtual address %lu\n", vaddr);
		return -EINVAL;
	}

	/* For now, only 2 MB-aligned registrations are supported */
	if (((uintptr_t)vaddr & MASK_2MB) || (size & MASK_2MB)) {
		DEBUG_PRINT("invalid %s parameters, vaddr=%lu len=%ju\n",
			    __func__, vaddr, size);
		return -EINVAL;
	}

	vfn_2mb = vaddr >> SHIFT_2MB;

	while (size) {
		map_1gb = spdk_mem_map_get_map_1gb(map, vfn_2mb);
		if (!map_1gb) {
			DEBUG_PRINT("could not get %p map\n", (void *)vaddr);
			return -ENOMEM;
		}

		idx_1gb = MAP_1GB_IDX(vfn_2mb);
		map_2mb = &map_1gb->map[idx_1gb];
		map_2mb->translation_2mb = translation;

		size -= VALUE_2MB;
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

	if ((uintptr_t)vaddr & ~MASK_256TB) {
		DEBUG_PRINT("invalid usermode virtual address %lu\n", vaddr);
		return -EINVAL;
	}

	/* For now, only 2 MB-aligned registrations are supported */
	if (((uintptr_t)vaddr & MASK_2MB) || (size & MASK_2MB)) {
		DEBUG_PRINT("invalid %s parameters, vaddr=%lu len=%ju\n",
			    __func__, vaddr, size);
		return -EINVAL;
	}

	vfn_2mb = vaddr >> SHIFT_2MB;

	while (size) {
		map_1gb = spdk_mem_map_get_map_1gb(map, vfn_2mb);
		if (!map_1gb) {
			DEBUG_PRINT("could not get %p map\n", (void *)vaddr);
			return -ENOMEM;
		}

		idx_1gb = MAP_1GB_IDX(vfn_2mb);
		map_2mb = &map_1gb->map[idx_1gb];
		map_2mb->translation_2mb = map->default_translation;

		size -= VALUE_2MB;
		vfn_2mb++;
	}

	return 0;
}

uint64_t
spdk_mem_map_translate(const struct spdk_mem_map *map, uint64_t vaddr, uint64_t *size)
{
	const struct map_1gb *map_1gb;
	const struct map_2mb *map_2mb;
	uint64_t idx_256tb;
	uint64_t idx_1gb;
	uint64_t vfn_2mb;
	uint64_t total_size = 0;
	uint64_t cur_size;
	uint64_t prev_translation;

	if (size != NULL) {
		total_size = *size;
		*size = 0;
	}

	if (spdk_unlikely(vaddr & ~MASK_256TB)) {
		DEBUG_PRINT("invalid usermode virtual address %p\n", (void *)vaddr);
		return map->default_translation;
	}

	vfn_2mb = vaddr >> SHIFT_2MB;
	idx_256tb = MAP_256TB_IDX(vfn_2mb);
	idx_1gb = MAP_1GB_IDX(vfn_2mb);

	map_1gb = map->map_256tb.map[idx_256tb];
	if (spdk_unlikely(!map_1gb)) {
		return map->default_translation;
	}

	cur_size = VALUE_2MB;
	if (size != NULL) {
		*size = VALUE_2MB;
	}

	map_2mb = &map_1gb->map[idx_1gb];
	if (size == NULL || map->ops.are_contiguous == NULL ||
	    map_2mb->translation_2mb == map->default_translation) {
		return map_2mb->translation_2mb;
	}

	prev_translation = map_2mb->translation_2mb;;
	while (cur_size < total_size) {
		vfn_2mb++;
		idx_256tb = MAP_256TB_IDX(vfn_2mb);
		idx_1gb = MAP_1GB_IDX(vfn_2mb);

		map_1gb = map->map_256tb.map[idx_256tb];
		if (spdk_unlikely(!map_1gb)) {
			break;
		}

		map_2mb = &map_1gb->map[idx_1gb];
		if (!map->ops.are_contiguous(prev_translation, map_2mb->translation_2mb)) {
			break;
		}

		cur_size += VALUE_2MB;
		prev_translation = map_2mb->translation_2mb;
	}

	*size = cur_size;
	return prev_translation;
}

#if RTE_VERSION >= RTE_VERSION_NUM(18, 05, 0, 0)
static void
memory_hotplug_cb(enum rte_mem_event event_type,
		  const void *addr, size_t len, void *arg)
{
	if (event_type == RTE_MEM_EVENT_ALLOC) {
		while (len > 0) {
			struct rte_memseg *seg;

			seg = rte_mem_virt2memseg(addr, NULL);
			assert(seg != NULL);
			assert(len >= seg->hugepage_sz);

			spdk_mem_register((void *)seg->addr, seg->hugepage_sz);
			addr = (void *)((uintptr_t)addr + seg->hugepage_sz);
			len -= seg->hugepage_sz;
		}
	} else if (event_type == RTE_MEM_EVENT_FREE) {
		spdk_mem_unregister((void *)addr, len);
	}
}

static int
memory_iter_cb(const struct rte_memseg_list *msl,
	       const struct rte_memseg *ms, size_t len, void *arg)
{
	return spdk_mem_register(ms->addr, len);
}
#endif

int
spdk_mem_map_init(void)
{
	g_mem_reg_map = spdk_mem_map_alloc(0, NULL, NULL);
	if (g_mem_reg_map == NULL) {
		DEBUG_PRINT("memory registration map allocation failed\n");
		return -1;
	}

	/*
	 * Walk all DPDK memory segments and register them
	 * with the master memory map
	 */
#if RTE_VERSION >= RTE_VERSION_NUM(18, 05, 0, 0)
	rte_mem_event_callback_register("spdk", memory_hotplug_cb, NULL);
	rte_memseg_contig_walk(memory_iter_cb, NULL);
#else
	struct rte_mem_config *mcfg;
	size_t seg_idx;

	mcfg = rte_eal_get_configuration()->mem_config;
	for (seg_idx = 0; seg_idx < RTE_MAX_MEMSEG; seg_idx++) {
		struct rte_memseg *seg = &mcfg->memseg[seg_idx];

		if (seg->addr == NULL) {
			break;
		}

		spdk_mem_register(seg->addr, seg->len);
	}
#endif
	return 0;
}
