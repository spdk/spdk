/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "env_internal.h"
#include "pci_dpdk.h"

#include <rte_config.h>
#include <rte_memory.h>
#include <rte_eal_memconfig.h>
#include <rte_dev.h>
#include <rte_pci.h>

#include "spdk_internal/assert.h"

#include "spdk/assert.h"
#include "spdk/likely.h"
#include "spdk/queue.h"
#include "spdk/util.h"
#include "spdk/memory.h"
#include "spdk/env_dpdk.h"
#include "spdk/log.h"

#ifdef __linux__
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0)
#include <linux/vfio.h>
#include <rte_vfio.h>

struct spdk_vfio_dma_map {
	struct vfio_iommu_type1_dma_map map;
	TAILQ_ENTRY(spdk_vfio_dma_map) tailq;
};

struct vfio_cfg {
	int fd;
	bool enabled;
	bool noiommu_enabled;
	unsigned device_ref;
	TAILQ_HEAD(, spdk_vfio_dma_map) maps;
	pthread_mutex_t mutex;
};

static struct vfio_cfg g_vfio = {
	.fd = -1,
	.enabled = false,
	.noiommu_enabled = false,
	.device_ref = 0,
	.maps = TAILQ_HEAD_INITIALIZER(g_vfio.maps),
	.mutex = PTHREAD_MUTEX_INITIALIZER
};
#endif
#endif

#if DEBUG
#define DEBUG_PRINT(...) SPDK_ERRLOG(__VA_ARGS__)
#else
#define DEBUG_PRINT(...)
#endif

#define ADDR_INVALID		((uint64_t)-1)

#define VFN_2MB(vaddr)		((vaddr) >> SHIFT_2MB)
#define VFN_4KB(vaddr)		((vaddr) >> SHIFT_4KB)

#define FN_2MB_TO_4KB(fn)	((fn) << (SHIFT_2MB - SHIFT_4KB))
#define FN_4KB_TO_2MB(fn)	((fn) >> (SHIFT_2MB - SHIFT_4KB))

#define MAP_256TB_IDX(vfn_2mb)	((vfn_2mb) >> (SHIFT_1GB - SHIFT_2MB))
#define MAP_1GB_IDX(vfn_2mb)	((vfn_2mb) & ((1ULL << (SHIFT_1GB - SHIFT_2MB)) - 1))
#define MAP_2MB_IDX(vfn_4kb)	((vfn_4kb) & ((1ULL << (SHIFT_2MB - SHIFT_4KB)) - 1))

#define MAP_256TB_SIZE		(1ULL << (SHIFT_256TB - SHIFT_1GB))
#define MAP_1GB_SIZE		(1ULL << (SHIFT_1GB - SHIFT_2MB))
#define MAP_2MB_SIZE		(1ULL << (SHIFT_2MB - SHIFT_4KB))

#define ADDR_FROM_IDX(idx_256tb, idx_1gb, idx_2mb) \
	(((idx_256tb) << SHIFT_1GB) | ((idx_1gb) << SHIFT_2MB) | ((idx_2mb) << SHIFT_4KB))

/* Page is registered */
#define REG_MAP_REGISTERED	(1ULL << 62)

/* A notification region barrier. The 2MB translation entry that's marked
 * with this flag must be unregistered separately. This allows contiguous
 * regions to be unregistered in the same chunks they were registered.
 */
#define REG_MAP_NOTIFY_START	(1ULL << 63)

/* 4KB vtophys mapping */
#define VTOPHYS_4KB		(1ULL << 63)
#define VTOPHYS_ADDR(paddr)	((paddr) & ~VTOPHYS_4KB)

/* Third-level map for 4KB translations */
struct map_2mb4kb {
	uint64_t translation_4kb[MAP_2MB_SIZE];
};

/* Second-level map table indexed by bits [21..29] of the virtual address.
 * Each entry contains the address translation for a 2MB page or an error
 * for entries that haven't been retrieved yet.
 */
struct map_1gb2mb {
	uint64_t translation_2mb[MAP_1GB_SIZE];
};

/* Second-level map containing 4KB translations. */
struct map_1gb4kb {
	struct map_2mb4kb *map[MAP_1GB_SIZE];
};

/* Top-level map table indexed by bits [30..47] of the virtual address.
 * Each entry points to a second-level map table or NULL.
 */
struct map_256tb {
	struct {
		struct map_1gb2mb	*map_1gb2mb;
		struct map_1gb4kb	*map_1gb4kb;
	} map[MAP_256TB_SIZE];
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
static TAILQ_HEAD(spdk_mem_map_head, spdk_mem_map) g_spdk_mem_maps =
	TAILQ_HEAD_INITIALIZER(g_spdk_mem_maps);
static pthread_mutex_t g_spdk_mem_map_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool g_legacy_mem;
static bool g_huge_pages = true;

static inline uint64_t
mem_map_translate(const struct spdk_mem_map *map, uint64_t vaddr, int *page_size)
{
	const struct map_1gb2mb *map_1gb2mb;
	const struct map_1gb4kb *map_1gb4kb;
	const struct map_2mb4kb *map_2mb4kb;
	uint64_t translation, vfn_4kb, vfn_2mb, idx_2mb, idx_1gb, idx_256tb;

	vfn_2mb = VFN_2MB(vaddr);
	idx_256tb = MAP_256TB_IDX(vfn_2mb);
	idx_1gb = MAP_1GB_IDX(vfn_2mb);

	/* Check the 2MB map first */
	map_1gb2mb = map->map_256tb.map[idx_256tb].map_1gb2mb;
	if (spdk_likely(map_1gb2mb != NULL)) {
		translation = map_1gb2mb->translation_2mb[idx_1gb];
		if (spdk_likely(translation != map->default_translation)) {
			*page_size = VALUE_2MB;
			return translation;
		}
	}

	/* There's no 2MB translation for this address, check the 4KB map */
	map_1gb4kb = map->map_256tb.map[idx_256tb].map_1gb4kb;
	if (spdk_likely(map_1gb4kb != NULL)) {
		map_2mb4kb = map_1gb4kb->map[idx_1gb];
		if (spdk_likely(map_2mb4kb != NULL)) {
			vfn_4kb = VFN_4KB(vaddr);
			idx_2mb = MAP_2MB_IDX(vfn_4kb);
			*page_size = VALUE_4KB;

			return map_2mb4kb->translation_4kb[idx_2mb];
		}
	}

	*page_size = VALUE_2MB;
	return map->default_translation;
}

static bool
mem_map_is_4kb_mapping(struct spdk_mem_map *map, uint64_t vaddr)
{
	int page_size;

	mem_map_translate(map, vaddr, &page_size);
	return page_size == VALUE_4KB;
}

static int
mem_map_walk_region(struct spdk_mem_map *map, uint64_t vaddr, size_t size,
		    int (*callback)(struct spdk_mem_map *map, uint64_t addr, size_t sz, void *ctx),
		    void *ctx)
{
	uint64_t vfn_4kb, vfn_2mb;
	uint64_t vfn_4kb_end, vfn_2mb_end;
	int rc;

	vfn_4kb = VFN_4KB(vaddr);
	vfn_4kb_end = spdk_min(FN_2MB_TO_4KB(VFN_2MB(vaddr + MASK_2MB)), VFN_4KB(vaddr + size));
	while (vfn_4kb < vfn_4kb_end) {
		rc = callback(map, vaddr, VALUE_4KB, ctx);
		if (rc != 0) {
			return rc;
		}
		vaddr += VALUE_4KB;
		size -= VALUE_4KB;
		vfn_4kb++;
	}

	vfn_2mb = VFN_2MB(vaddr);
	vfn_2mb_end = VFN_2MB(vaddr + size);
	while (vfn_2mb < vfn_2mb_end) {
		rc = callback(map, vaddr, VALUE_2MB, ctx);
		if (rc != 0) {
			return rc;
		}
		vaddr += VALUE_2MB;
		size -= VALUE_2MB;
		vfn_2mb++;
	}

	vfn_4kb = VFN_4KB(vaddr);
	vfn_4kb_end = VFN_4KB(vaddr + size);
	while (vfn_4kb < vfn_4kb_end) {
		rc = callback(map, vaddr, VALUE_4KB, ctx);
		if (rc != 0) {
			return rc;
		}
		vaddr += VALUE_4KB;
		size -= VALUE_4KB;
		vfn_4kb++;
	}

	return 0;
}

static uint64_t
mem_reg_map_next_region(uint64_t addr)
{
	uint64_t idx_256tb, idx_1gb, idx_2mb;
	uint64_t reg, vfn_2mb, vfn_4kb;
	int page_size;

	vfn_2mb = VFN_2MB(addr);
	vfn_4kb = VFN_4KB(addr);
	idx_256tb = MAP_256TB_IDX(vfn_2mb);
	idx_1gb = MAP_1GB_IDX(vfn_2mb);
	idx_2mb = MAP_2MB_IDX(vfn_4kb);
	for (; idx_256tb < MAP_256TB_SIZE; idx_256tb++) {
		if (!g_mem_reg_map->map_256tb.map[idx_256tb].map_1gb2mb &&
		    !g_mem_reg_map->map_256tb.map[idx_256tb].map_1gb4kb) {
			goto next_256tb;
		}

		for (; idx_1gb < MAP_1GB_SIZE; idx_1gb++) {
			addr = ADDR_FROM_IDX(idx_256tb, idx_1gb, idx_2mb);
			reg = mem_map_translate(g_mem_reg_map, addr, &page_size);

			if (reg & REG_MAP_NOTIFY_START) {
				assert(reg & REG_MAP_REGISTERED);
				return addr;
			}

			if (page_size == VALUE_4KB) {
				for (; idx_2mb < MAP_2MB_SIZE; idx_2mb++) {
					addr = ADDR_FROM_IDX(idx_256tb, idx_1gb, idx_2mb);
					reg = mem_map_translate(g_mem_reg_map, addr, &page_size);

					if (reg & REG_MAP_NOTIFY_START) {
						assert(reg & REG_MAP_REGISTERED);
						return addr;
					}
				}
			}

			idx_2mb = 0;
		}
next_256tb:
		idx_1gb = 0;
	}

	return ADDR_INVALID;
}

/*
 * Walk the currently registered memory via the main memory registration map
 * and call the new map's notify callback for each virtually contiguous region.
 */
static int
mem_map_notify_walk(struct spdk_mem_map *map, enum spdk_mem_map_notify_action action)
{
	uint64_t addr, fail_addr, size;
	int rc;

	if (!g_mem_reg_map) {
		return -EINVAL;
	}

	/* Hold the memory registration map mutex so no new registrations can be added while we are looping. */
	pthread_mutex_lock(&g_mem_reg_map->mutex);
	for (addr = mem_reg_map_next_region(0);
	     addr != ADDR_INVALID;
	     addr = mem_reg_map_next_region(addr)) {
		size = UINT64_MAX;
		spdk_mem_map_translate(g_mem_reg_map, addr, &size);
		rc = map->ops.notify_cb(map->cb_ctx, map, action,
					(void *)addr, size);
		/* Don't bother handling unregister failures. It can't be any worse */
		if (rc != 0 && action == SPDK_MEM_MAP_NOTIFY_REGISTER) {
			goto err_unregister;
		}
		addr += size;
	}

	pthread_mutex_unlock(&g_mem_reg_map->mutex);
	return 0;

err_unregister:
	fail_addr = addr;
	for (addr = mem_reg_map_next_region(0);
	     addr != ADDR_INVALID && addr != fail_addr;
	     addr = mem_reg_map_next_region(addr)) {
		size = UINT64_MAX;
		spdk_mem_map_translate(g_mem_reg_map, addr, &size);
		map->ops.notify_cb(map->cb_ctx, map,
				   SPDK_MEM_MAP_NOTIFY_UNREGISTER,
				   (void *)addr, size);
		addr += size;
	}

	pthread_mutex_unlock(&g_mem_reg_map->mutex);
	return rc;
}

static void
mem_map_free(struct spdk_mem_map *map)
{
	struct map_1gb4kb *map_1gb4kb;
	size_t i, j;

	for (i = 0; i < SPDK_COUNTOF(map->map_256tb.map); i++) {
		free(map->map_256tb.map[i].map_1gb2mb);
		map_1gb4kb = map->map_256tb.map[i].map_1gb4kb;
		if (map_1gb4kb == NULL) {
			continue;
		}
		for (j = 0; j < SPDK_COUNTOF(map_1gb4kb->map); j++) {
			free(map_1gb4kb->map[j]);
		}
		free(map_1gb4kb);
	}
	pthread_mutex_destroy(&map->mutex);
	free(map);
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
		rc = mem_map_notify_walk(map, SPDK_MEM_MAP_NOTIFY_REGISTER);
		if (rc != 0) {
			pthread_mutex_unlock(&g_spdk_mem_map_mutex);
			DEBUG_PRINT("Initial mem_map notify failed\n");
			mem_map_free(map);
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

	if (!pmap) {
		return;
	}

	map = *pmap;

	if (!map) {
		return;
	}

	if (map->ops.notify_cb) {
		pthread_mutex_lock(&g_spdk_mem_map_mutex);
		mem_map_notify_walk(map, SPDK_MEM_MAP_NOTIFY_UNREGISTER);
		TAILQ_REMOVE(&g_spdk_mem_maps, map, tailq);
		pthread_mutex_unlock(&g_spdk_mem_map_mutex);
	}

	mem_map_free(map);
	*pmap = NULL;
}

static int
mem_check_region_unregistered(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	uint64_t reg, curlen;

	while (len > 0) {
		curlen = len;
		reg = spdk_mem_map_translate(map, vaddr, &curlen);
		if (reg & REG_MAP_REGISTERED) {
			return -EBUSY;
		}

		vaddr += curlen;
		len -= curlen;
	}

	return 0;
}

static int
mem_check_region_registered(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	uint64_t reg, curlen;

	while (len > 0) {
		curlen = len;
		reg = spdk_mem_map_translate(map, vaddr, &curlen);
		if (!(reg & REG_MAP_REGISTERED)) {
			return -EINVAL;
		}

		vaddr += curlen;
		len -= curlen;
	}

	return 0;
}

static int
mem_register_page(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	int *page = ctx;

	return spdk_mem_map_set_translation(map, vaddr, len, (*page)++ == 0 ?
					    REG_MAP_REGISTERED | REG_MAP_NOTIFY_START :
					    REG_MAP_REGISTERED);
}

int
spdk_mem_register(void *vaddr, size_t len)
{
	struct spdk_mem_map *map;
	int rc, page = 0;

	if ((uintptr_t)vaddr & ~MASK_256TB) {
		DEBUG_PRINT("invalid usermode virtual address %p\n", vaddr);
		return -EINVAL;
	}

	if (((uintptr_t)vaddr & MASK_4KB) || (len & MASK_4KB)) {
		DEBUG_PRINT("invalid %s parameters, vaddr=%p len=%ju\n",
			    __func__, vaddr, len);
		return -EINVAL;
	}

	if (len == 0) {
		return 0;
	}

	pthread_mutex_lock(&g_spdk_mem_map_mutex);
	rc = mem_map_walk_region(g_mem_reg_map, (uint64_t)vaddr, len,
				 mem_check_region_unregistered, NULL);
	if (rc != 0) {
		pthread_mutex_unlock(&g_spdk_mem_map_mutex);
		return rc;
	}

	rc = mem_map_walk_region(g_mem_reg_map, (uint64_t)vaddr, len,
				 mem_register_page, &page);
	if (rc != 0) {
		pthread_mutex_unlock(&g_spdk_mem_map_mutex);
		return rc;
	}

	TAILQ_FOREACH(map, &g_spdk_mem_maps, tailq) {
		rc = map->ops.notify_cb(map->cb_ctx, map, SPDK_MEM_MAP_NOTIFY_REGISTER, vaddr, len);
		if (rc != 0) {
			pthread_mutex_unlock(&g_spdk_mem_map_mutex);
			return rc;
		}
	}

	pthread_mutex_unlock(&g_spdk_mem_map_mutex);
	return 0;
}

static int
mem_unregister_page(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	struct iovec *region = ctx;
	uint64_t off, reg;
	int rc;

	/* We've already checked that the whole region we're trying to unregister was actually
	 * registered at this point.  But if we're trying to unregister a 2MB region that uses 4KB
	 * translations, we need to check each 4KB page individually, because that 2MB region could
	 * consist of multiple smaller registrations, so we might need to send multiple
	 * notifications.
	 */
	if (len > VALUE_4KB && mem_map_is_4kb_mapping(map, vaddr)) {
		assert(len == VALUE_2MB);
		for (off = 0; off < len; off += VALUE_4KB) {
			rc = mem_unregister_page(map, vaddr + off, VALUE_4KB, ctx);
			if (rc != 0) {
				return rc;
			}
		}
		/* Set translation for the whole 2MB page to free the 4KB map */
		return spdk_mem_map_set_translation(map, vaddr, len, 0);
	}

	reg = spdk_mem_map_translate(map, vaddr, NULL);
	spdk_mem_map_set_translation(map, vaddr, len, 0);
	if (region->iov_len > 0 && (reg & REG_MAP_NOTIFY_START)) {
		TAILQ_FOREACH_REVERSE(map, &g_spdk_mem_maps, spdk_mem_map_head, tailq) {
			rc = map->ops.notify_cb(map->cb_ctx, map, SPDK_MEM_MAP_NOTIFY_UNREGISTER,
						region->iov_base, region->iov_len);
			if (rc != 0) {
				return rc;
			}
		}

		region->iov_base = (void *)vaddr;
		region->iov_len = len;
	} else {
		region->iov_len += len;
	}

	return 0;
}

int
spdk_mem_unregister(void *vaddr, size_t len)
{
	struct spdk_mem_map *map;
	struct iovec region;
	int rc;
	uint64_t reg, newreg;

	if ((uintptr_t)vaddr & ~MASK_256TB) {
		DEBUG_PRINT("invalid usermode virtual address %p\n", vaddr);
		return -EINVAL;
	}

	if (((uintptr_t)vaddr & MASK_4KB) || (len & MASK_4KB)) {
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

	rc = mem_map_walk_region(g_mem_reg_map, (uint64_t)vaddr, len,
				 mem_check_region_registered, NULL);
	if (rc != 0) {
		pthread_mutex_unlock(&g_spdk_mem_map_mutex);
		return rc;
	}

	newreg = spdk_mem_map_translate(g_mem_reg_map, (uint64_t)vaddr + len, NULL);
	/* If the next page is registered, it must be a start of a region as well,
	 * otherwise we'd be unregistering only a part of a region.
	 */
	if ((newreg & REG_MAP_NOTIFY_START) == 0 && (newreg & REG_MAP_REGISTERED)) {
		pthread_mutex_unlock(&g_spdk_mem_map_mutex);
		return -ERANGE;
	}

	region.iov_base = vaddr;
	region.iov_len = 0;
	rc = mem_map_walk_region(g_mem_reg_map, (uint64_t)vaddr, len,
				 mem_unregister_page, &region);
	if (rc != 0) {
		pthread_mutex_unlock(&g_spdk_mem_map_mutex);
		return rc;
	}

	if (region.iov_len > 0) {
		TAILQ_FOREACH_REVERSE(map, &g_spdk_mem_maps, spdk_mem_map_head, tailq) {
			rc = map->ops.notify_cb(map->cb_ctx, map, SPDK_MEM_MAP_NOTIFY_UNREGISTER,
						region.iov_base, region.iov_len);
			if (rc != 0) {
				pthread_mutex_unlock(&g_spdk_mem_map_mutex);
				return rc;
			}
		}
	}

	pthread_mutex_unlock(&g_spdk_mem_map_mutex);
	return 0;
}

int
spdk_mem_reserve(void *vaddr, size_t len)
{
	struct spdk_mem_map *map;
	int rc;

	if ((uintptr_t)vaddr & ~MASK_256TB) {
		DEBUG_PRINT("invalid usermode virtual address %p\n", vaddr);
		return -EINVAL;
	}

	if (((uintptr_t)vaddr & MASK_4KB) || (len & MASK_4KB)) {
		DEBUG_PRINT("invalid %s parameters, vaddr=%p len=%ju\n",
			    __func__, vaddr, len);
		return -EINVAL;
	}

	if (len == 0) {
		return 0;
	}

	pthread_mutex_lock(&g_spdk_mem_map_mutex);

	/* Check if any part of this range is already registered */
	rc = mem_map_walk_region(g_mem_reg_map, (uint64_t)vaddr, len,
				 mem_check_region_unregistered, NULL);
	if (rc != 0) {
		pthread_mutex_unlock(&g_spdk_mem_map_mutex);
		return rc;
	}

	/* Simply set the translation to the memory map's default. This allocates the space in the
	 * map but does not provide a valid translation. */
	spdk_mem_map_set_translation(g_mem_reg_map, (uint64_t)vaddr, len,
				     g_mem_reg_map->default_translation);

	TAILQ_FOREACH(map, &g_spdk_mem_maps, tailq) {
		spdk_mem_map_set_translation(map, (uint64_t)vaddr, len, map->default_translation);
	}

	pthread_mutex_unlock(&g_spdk_mem_map_mutex);
	return 0;
}

static struct map_1gb2mb *
mem_map_get_map_1gb2mb(struct spdk_mem_map *map, uint64_t vfn_2mb, bool alloc)
{
	struct map_1gb2mb *map_1gb2mb;
	uint64_t idx_256tb = MAP_256TB_IDX(vfn_2mb);
	size_t i;

	if (spdk_unlikely(idx_256tb >= SPDK_COUNTOF(map->map_256tb.map))) {
		return NULL;
	}

	map_1gb2mb = map->map_256tb.map[idx_256tb].map_1gb2mb;
	if (!map_1gb2mb && alloc) {
		pthread_mutex_lock(&map->mutex);

		/* Recheck to make sure nobody else got the mutex first. */
		map_1gb2mb = map->map_256tb.map[idx_256tb].map_1gb2mb;
		if (!map_1gb2mb) {
			map_1gb2mb = malloc(sizeof(struct map_1gb2mb));
			if (map_1gb2mb) {
				/* initialize all entries to default translation */
				for (i = 0; i < SPDK_COUNTOF(map_1gb2mb->translation_2mb); i++) {
					map_1gb2mb->translation_2mb[i] = map->default_translation;
				}
				map->map_256tb.map[idx_256tb].map_1gb2mb = map_1gb2mb;
			}
		}

		pthread_mutex_unlock(&map->mutex);

		if (!map_1gb2mb) {
			DEBUG_PRINT("allocation failed\n");
			return NULL;
		}
	}

	return map_1gb2mb;
}

static struct map_1gb4kb *
mem_map_get_map_1gb4kb(struct spdk_mem_map *map, uint64_t vfn_4kb, bool alloc)
{
	struct map_1gb4kb *map_1gb4kb;
	uint64_t vfn_2mb, idx_256tb;

	vfn_2mb = FN_4KB_TO_2MB(vfn_4kb);
	idx_256tb = MAP_256TB_IDX(vfn_2mb);
	if (idx_256tb >= SPDK_COUNTOF(map->map_256tb.map)) {
		return NULL;
	}

	map_1gb4kb = map->map_256tb.map[idx_256tb].map_1gb4kb;
	if (map_1gb4kb == NULL && alloc) {
		pthread_mutex_lock(&map->mutex);
		/* Recheck to make sure nobody else got the mutex first. */
		map_1gb4kb = map->map_256tb.map[idx_256tb].map_1gb4kb;
		if (map_1gb4kb == NULL) {
			map_1gb4kb = calloc(1, sizeof(*map_1gb4kb));

		}
		map->map_256tb.map[idx_256tb].map_1gb4kb = map_1gb4kb;
		pthread_mutex_unlock(&map->mutex);
	}

	return map_1gb4kb;
}

static struct map_2mb4kb *
mem_map_get_map_2mb4kb(struct spdk_mem_map *map, uint64_t vfn_4kb, bool alloc)
{
	struct map_2mb4kb *map_2mb4kb;
	struct map_1gb4kb *map_1gb4kb;
	uint64_t vfn_2mb, idx_1gb, translation;
	int page_size;
	size_t i;

	map_1gb4kb = mem_map_get_map_1gb4kb(map, vfn_4kb, alloc);
	if (map_1gb4kb == NULL) {
		return NULL;
	}

	vfn_2mb = FN_4KB_TO_2MB(vfn_4kb);
	idx_1gb = MAP_1GB_IDX(vfn_2mb);
	map_2mb4kb = map_1gb4kb->map[idx_1gb];
	if (map_2mb4kb == NULL && alloc) {
		pthread_mutex_lock(&map->mutex);
		/* Recheck to make sure nobody else got the mutex first. */
		map_2mb4kb = map_1gb4kb->map[idx_1gb];
		if (map_2mb4kb == NULL) {
			map_2mb4kb = malloc(sizeof(*map_2mb4kb));
			if (map_2mb4kb != NULL) {
				/* Fill the 4kb map with the 2mb translation, if it had any */
				translation = mem_map_translate(map, vfn_4kb << SHIFT_4KB,
								&page_size);
				for (i = 0; i < SPDK_COUNTOF(map_2mb4kb->translation_4kb); i++) {
					map_2mb4kb->translation_4kb[i] = translation;
				}
				map_1gb4kb->map[idx_1gb] = map_2mb4kb;
			}
		}
		pthread_mutex_unlock(&map->mutex);
	}

	return map_2mb4kb;
}

static int
mem_map_set_4kb_translation(struct spdk_mem_map *map, uint64_t vaddr, uint64_t translation)
{
	struct map_2mb4kb *map_2mb4kb;
	struct map_1gb2mb *map_1gb2mb;
	uint64_t vfn_4kb, vfn_2mb;
	uint64_t idx_2mb, idx_1gb;

	vfn_4kb = VFN_4KB(vaddr);
	map_2mb4kb = mem_map_get_map_2mb4kb(map, vfn_4kb, true);
	if (!map_2mb4kb) {
		DEBUG_PRINT("could not get %p map\n", (void *)vaddr);
		return -ENOMEM;
	}

	idx_2mb = MAP_2MB_IDX(vfn_4kb);
	map_2mb4kb->translation_4kb[idx_2mb] = translation;

	/* Set 2MB map to the default translation to indicate this region has 4KB mapping */
	vfn_2mb = FN_4KB_TO_2MB(vfn_4kb);
	map_1gb2mb = mem_map_get_map_1gb2mb(map, vfn_2mb, false);
	if (map_1gb2mb != NULL) {
		idx_1gb = MAP_1GB_IDX(vfn_2mb);
		map_1gb2mb->translation_2mb[idx_1gb] = map->default_translation;
	}

	return 0;
}

static int
mem_map_set_2mb_translation(struct spdk_mem_map *map, uint64_t vaddr, uint64_t translation)
{
	struct map_2mb4kb *map_2mb4kb;
	struct map_1gb2mb *map_1gb2mb;
	uint64_t i, vfn_2mb, idx_1gb;

	vfn_2mb = VFN_2MB(vaddr);
	map_1gb2mb = mem_map_get_map_1gb2mb(map, vfn_2mb, true);
	if (!map_1gb2mb) {
		DEBUG_PRINT("could not get %p map\n", (void *)vaddr);
		return -ENOMEM;
	}

	idx_1gb = MAP_1GB_IDX(vfn_2mb);
	map_1gb2mb->translation_2mb[idx_1gb] = translation;

	/* Set up 4KB translations too in case this region later uses 4KB mapping or we're
	 * setting the default translation (which is also used to indicate a 4KB mapping).
	 */
	map_2mb4kb = mem_map_get_map_2mb4kb(map, FN_2MB_TO_4KB(vfn_2mb), false);
	if (map_2mb4kb != NULL) {
		for (i = 0; i < SPDK_COUNTOF(map_2mb4kb->translation_4kb); i++) {
			map_2mb4kb->translation_4kb[i] = translation;
		}
	}

	return 0;
}

static int
mem_map_set_page_translation(struct spdk_mem_map *map, uint64_t vaddr, size_t page_size,
			     void *translation)
{
	switch (page_size) {
	case VALUE_4KB:
		return mem_map_set_4kb_translation(map, vaddr, (uint64_t)translation);
	case VALUE_2MB:
		return mem_map_set_2mb_translation(map, vaddr, (uint64_t)translation);
	default:
		assert(0 && "should never happan");
		return -EINVAL;
	}
}

int
spdk_mem_map_set_translation(struct spdk_mem_map *map, uint64_t vaddr, uint64_t size,
			     uint64_t translation)
{
	if ((uintptr_t)vaddr & ~MASK_256TB) {
		DEBUG_PRINT("invalid usermode virtual address %" PRIu64 "\n", vaddr);
		return -EINVAL;
	}

	if (((uintptr_t)vaddr & MASK_4KB) || (size & MASK_4KB)) {
		DEBUG_PRINT("invalid %s parameters, vaddr=%" PRIu64 " len=%" PRIu64 "\n",
			    __func__, vaddr, size);
		return -EINVAL;
	}

	return mem_map_walk_region(map, vaddr, size, mem_map_set_page_translation,
				   (void *)translation);
}

int
spdk_mem_map_clear_translation(struct spdk_mem_map *map, uint64_t vaddr, uint64_t size)
{
	return spdk_mem_map_set_translation(map, vaddr, size, map->default_translation);
}

inline uint64_t
spdk_mem_map_translate(const struct spdk_mem_map *map, uint64_t vaddr, uint64_t *size)
{
	uint64_t cur_size;
	uint64_t prev_translation;
	uint64_t orig_translation;
	uint64_t curr_translation;
	int page_size;

	if (spdk_unlikely(vaddr & ~MASK_256TB)) {
		DEBUG_PRINT("invalid usermode virtual address %p\n", (void *)vaddr);
		return map->default_translation;
	}

	curr_translation = mem_map_translate(map, vaddr, &page_size);
	cur_size = page_size - (page_size == VALUE_4KB ? _4KB_OFFSET(vaddr) : _2MB_OFFSET(vaddr));
	if (size == NULL || map->ops.are_contiguous == NULL ||
	    curr_translation == map->default_translation) {
		if (size != NULL) {
			*size = spdk_min(*size, cur_size);
		}
		return curr_translation;
	}

	prev_translation = orig_translation = curr_translation;
	vaddr += cur_size;
	while (cur_size < *size) {
		curr_translation = mem_map_translate(map, vaddr, &page_size);
		if (!map->ops.are_contiguous(prev_translation, curr_translation)) {
			break;
		}

		cur_size += page_size;
		vaddr += page_size;
		prev_translation = curr_translation;
	}

	*size = spdk_min(*size, cur_size);
	return orig_translation;
}

static void
memory_hotplug_cb(enum rte_mem_event event_type,
		  const void *addr, size_t len, void *arg)
{
	if (event_type == RTE_MEM_EVENT_ALLOC) {
		spdk_mem_register((void *)addr, len);

		if (!spdk_env_dpdk_external_init()) {
			return;
		}

		/* When the user initialized DPDK separately, we can't
		 * be sure that --match-allocations RTE flag was specified.
		 * Without this flag, DPDK can free memory in different units
		 * than it was allocated. It doesn't work with things like RDMA MRs.
		 *
		 * For such cases, we mark segments so they aren't freed.
		 */
		while (len > 0) {
			struct rte_memseg *seg;

			seg = rte_mem_virt2memseg(addr, NULL);
			assert(seg != NULL);
			seg->flags |= RTE_MEMSEG_FLAG_DO_NOT_FREE;
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

static bool g_mem_event_cb_registered = false;

static int
mem_map_mem_event_callback_register(void)
{
	int rc;

	rc = rte_mem_event_callback_register("spdk", memory_hotplug_cb, NULL);
	if (rc != 0) {
		return rc;
	}

	g_mem_event_cb_registered = true;
	return 0;
}

static void
mem_map_mem_event_callback_unregister(void)
{
	if (g_mem_event_cb_registered) {
		g_mem_event_cb_registered = false;
		rte_mem_event_callback_unregister("spdk", NULL);
	}
}

static int
mem_reg_map_check_contiguous(uint64_t addr1, uint64_t addr2)
{
	assert(addr1 & REG_MAP_REGISTERED);
	if (!(addr2 & REG_MAP_REGISTERED)) {
		return 0;
	}

	/* addr2 is the start of a new registration */
	return !(addr2 & REG_MAP_NOTIFY_START);
}

int
mem_map_init(bool legacy_mem)
{
	const struct spdk_mem_map_ops reg_map_ops = {
		.notify_cb = NULL,
		.are_contiguous = mem_reg_map_check_contiguous,
	};
	int rc;

	g_legacy_mem = legacy_mem;

	g_mem_reg_map = spdk_mem_map_alloc(0, &reg_map_ops, NULL);
	if (g_mem_reg_map == NULL) {
		DEBUG_PRINT("memory registration map allocation failed\n");
		return -ENOMEM;
	}

	if (!g_legacy_mem) {
		/**
		 * To prevent DPDK complaining, only register the callback when
		 * we are not in legacy mem mode.
		 */
		rc = mem_map_mem_event_callback_register();
		if (rc != 0) {
			DEBUG_PRINT("memory event callback registration failed, rc = %d\n", rc);
			goto err_free_reg_map;
		}
	}

	/*
	 * Walk all DPDK memory segments and register them
	 * with the main memory map
	 */
	rc = rte_memseg_contig_walk(memory_iter_cb, NULL);
	if (rc != 0) {
		DEBUG_PRINT("memory segments walking failed, rc = %d\n", rc);
		goto err_unregister_mem_cb;
	}

	return 0;

err_unregister_mem_cb:
	mem_map_mem_event_callback_unregister();
err_free_reg_map:
	spdk_mem_map_free(&g_mem_reg_map);
	return rc;
}

void
mem_map_fini(void)
{
	mem_map_mem_event_callback_unregister();
	spdk_mem_map_free(&g_mem_reg_map);
}

bool
spdk_iommu_is_enabled(void)
{
#if VFIO_ENABLED
	return g_vfio.enabled && !g_vfio.noiommu_enabled;
#else
	return false;
#endif
}

struct spdk_vtophys_pci_device {
	struct rte_pci_device *pci_device;
	TAILQ_ENTRY(spdk_vtophys_pci_device) tailq;
};

static pthread_mutex_t g_vtophys_pci_devices_mutex = PTHREAD_MUTEX_INITIALIZER;
static TAILQ_HEAD(, spdk_vtophys_pci_device) g_vtophys_pci_devices =
	TAILQ_HEAD_INITIALIZER(g_vtophys_pci_devices);

static struct spdk_mem_map *g_vtophys_map;
static struct spdk_mem_map *g_phys_ref_map;
static struct spdk_mem_map *g_numa_map;

#if VFIO_ENABLED
static int
_vfio_iommu_map_dma(uint64_t vaddr, uint64_t iova, uint64_t size)
{
	struct spdk_vfio_dma_map *dma_map;
	int ret;

	dma_map = calloc(1, sizeof(*dma_map));
	if (dma_map == NULL) {
		return -ENOMEM;
	}

	dma_map->map.argsz = sizeof(dma_map->map);
	dma_map->map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
	dma_map->map.vaddr = vaddr;
	dma_map->map.iova = iova;
	dma_map->map.size = size;

	if (g_vfio.device_ref == 0) {
		/* VFIO requires at least one device (IOMMU group) to be added to
		 * a VFIO container before it is possible to perform any IOMMU
		 * operations on that container. This memory will be mapped once
		 * the first device (IOMMU group) is hotplugged.
		 *
		 * Since the vfio container is managed internally by DPDK, it is
		 * also possible that some device is already in that container, but
		 * it's not managed by SPDK -  e.g. an NIC attached internally
		 * inside DPDK. We could map the memory straight away in such
		 * scenario, but there's no need to do it. DPDK devices clearly
		 * don't need our mappings and hence we defer the mapping
		 * unconditionally until the first SPDK-managed device is
		 * hotplugged.
		 */
		goto out_insert;
	}

	ret = ioctl(g_vfio.fd, VFIO_IOMMU_MAP_DMA, &dma_map->map);
	if (ret) {
		/* There are cases the vfio container doesn't have IOMMU group, it's safe for this case */
		SPDK_NOTICELOG("Cannot set up DMA mapping, error %d, ignored\n", errno);
	}

out_insert:
	TAILQ_INSERT_TAIL(&g_vfio.maps, dma_map, tailq);
	return 0;
}


static int
vtophys_iommu_map_dma(uint64_t vaddr, uint64_t iova, uint64_t size)
{
	uint64_t refcount;
	int ret;

	refcount = spdk_mem_map_translate(g_phys_ref_map, iova, NULL);
	assert(refcount < UINT64_MAX);
	if (refcount > 0) {
		spdk_mem_map_set_translation(g_phys_ref_map, iova, size, refcount + 1);
		return 0;
	}

	pthread_mutex_lock(&g_vfio.mutex);
	ret = _vfio_iommu_map_dma(vaddr, iova, size);
	pthread_mutex_unlock(&g_vfio.mutex);
	if (ret) {
		return ret;
	}

	spdk_mem_map_set_translation(g_phys_ref_map, iova, size, refcount + 1);
	return 0;
}

int
vtophys_iommu_map_dma_bar(uint64_t vaddr, uint64_t iova, uint64_t size)
{
	int ret;

	pthread_mutex_lock(&g_vfio.mutex);
	ret = _vfio_iommu_map_dma(vaddr, iova, size);
	pthread_mutex_unlock(&g_vfio.mutex);

	return ret;
}

static int
_vfio_iommu_unmap_dma(struct spdk_vfio_dma_map *dma_map)
{
	struct vfio_iommu_type1_dma_unmap unmap = {};
	int ret;

	if (g_vfio.device_ref == 0) {
		/* Memory is not mapped anymore, just remove it's references */
		goto out_remove;
	}

	unmap.argsz = sizeof(unmap);
	unmap.flags = 0;
	unmap.iova = dma_map->map.iova;
	unmap.size = dma_map->map.size;
	ret = ioctl(g_vfio.fd, VFIO_IOMMU_UNMAP_DMA, &unmap);
	if (ret) {
		SPDK_NOTICELOG("Cannot clear DMA mapping, error %d, ignored\n", errno);
	}

out_remove:
	TAILQ_REMOVE(&g_vfio.maps, dma_map, tailq);
	free(dma_map);
	return 0;
}

static int
vtophys_iommu_unmap_dma(uint64_t iova, uint64_t size)
{
	struct spdk_vfio_dma_map *dma_map;
	uint64_t refcount;
	int ret;

	pthread_mutex_lock(&g_vfio.mutex);
	TAILQ_FOREACH(dma_map, &g_vfio.maps, tailq) {
		if (dma_map->map.iova == iova) {
			break;
		}
	}

	if (dma_map == NULL) {
		DEBUG_PRINT("Cannot clear DMA mapping for IOVA %"PRIx64" - it's not mapped\n", iova);
		pthread_mutex_unlock(&g_vfio.mutex);
		return -ENXIO;
	}

	refcount = spdk_mem_map_translate(g_phys_ref_map, iova, NULL);
	assert(refcount < UINT64_MAX);
	if (refcount > 0) {
		spdk_mem_map_set_translation(g_phys_ref_map, iova, size, refcount - 1);
	}

	/* We still have outstanding references, don't clear it. */
	if (refcount > 1) {
		pthread_mutex_unlock(&g_vfio.mutex);
		return 0;
	}

	/** don't support partial or multiple-page unmap for now */
	assert(dma_map->map.size == size);

	ret = _vfio_iommu_unmap_dma(dma_map);
	pthread_mutex_unlock(&g_vfio.mutex);

	return ret;
}

int
vtophys_iommu_unmap_dma_bar(uint64_t vaddr)
{
	struct spdk_vfio_dma_map *dma_map;
	int ret;

	pthread_mutex_lock(&g_vfio.mutex);
	TAILQ_FOREACH(dma_map, &g_vfio.maps, tailq) {
		if (dma_map->map.vaddr == vaddr) {
			break;
		}
	}

	if (dma_map == NULL) {
		DEBUG_PRINT("Cannot clear DMA mapping for address %"PRIx64" - it's not mapped\n", vaddr);
		pthread_mutex_unlock(&g_vfio.mutex);
		return -ENXIO;
	}

	ret = _vfio_iommu_unmap_dma(dma_map);
	pthread_mutex_unlock(&g_vfio.mutex);
	return ret;
}
#endif

static uint64_t
vtophys_get_paddr_memseg(uint64_t vaddr, size_t *len)
{
	uintptr_t paddr, offset;
	struct rte_memseg *seg;

	seg = rte_mem_virt2memseg((void *)(uintptr_t)vaddr, NULL);
	if (seg != NULL) {
		paddr = seg->iova;
		if (paddr == RTE_BAD_IOVA) {
			return SPDK_VTOPHYS_ERROR;
		}
		offset = vaddr - (uintptr_t)seg->addr;
		if (len != NULL) {
			assert(seg->len > offset);
			*len = seg->len - offset;
		}
		paddr += offset;
		return paddr;
	}

	return SPDK_VTOPHYS_ERROR;
}

/* Try to get the paddr from /proc/self/pagemap */
static uint64_t
vtophys_get_paddr_pagemap(uint64_t vaddr)
{
	uintptr_t paddr;

	/* Silence static analyzers */
	assert(vaddr != 0);
	paddr = rte_mem_virt2iova((void *)vaddr);
	if (paddr == RTE_BAD_IOVA) {
		/*
		 * The vaddr may be valid but doesn't have a backing page
		 * assigned yet.  Touch the page to ensure a backing page
		 * gets assigned, then try to translate again.
		 */
		rte_atomic64_read((rte_atomic64_t *)vaddr);
		paddr = rte_mem_virt2iova((void *)vaddr);
	}
	if (paddr == RTE_BAD_IOVA) {
		/* Unable to get to the physical address. */
		return SPDK_VTOPHYS_ERROR;
	}

	return paddr;
}

static uint64_t
pci_device_vtophys(struct rte_pci_device *dev, uint64_t vaddr, size_t len)
{
	struct rte_mem_resource *res;
	uint64_t paddr;
	unsigned r;

	for (r = 0; r < PCI_MAX_RESOURCE; r++) {
		res = dpdk_pci_device_get_mem_resource(dev, r);

		if (res->phys_addr == 0 || vaddr < (uint64_t)res->addr ||
		    (vaddr + len) >= (uint64_t)res->addr + res->len) {
			continue;
		}

#if VFIO_ENABLED
		if (spdk_iommu_is_enabled() && rte_eal_iova_mode() == RTE_IOVA_VA) {
			/*
			 * The IOMMU is on and we're using IOVA == VA. The BAR was
			 * automatically registered when it was mapped, so just return
			 * the virtual address here.
			 */
			return vaddr;
		}
#endif
		paddr = res->phys_addr + (vaddr - (uint64_t)res->addr);
		return paddr;
	}

	return SPDK_VTOPHYS_ERROR;
}

/* Try to get the paddr from pci devices */
static uint64_t
vtophys_get_paddr_pci(uint64_t vaddr, size_t len)
{
	struct spdk_vtophys_pci_device *vtophys_dev;
	uintptr_t paddr;
	struct rte_pci_device	*dev;

	pthread_mutex_lock(&g_vtophys_pci_devices_mutex);
	TAILQ_FOREACH(vtophys_dev, &g_vtophys_pci_devices, tailq) {
		dev = vtophys_dev->pci_device;
		paddr = pci_device_vtophys(dev, vaddr, len);
		if (paddr != SPDK_VTOPHYS_ERROR) {
			pthread_mutex_unlock(&g_vtophys_pci_devices_mutex);
			return paddr;
		}
	}
	pthread_mutex_unlock(&g_vtophys_pci_devices_mutex);

	return SPDK_VTOPHYS_ERROR;
}

#if VFIO_ENABLED
static int
vtophys_unmap_pci(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	uint64_t paddr;

	paddr = vtophys_get_paddr_pci((uint64_t)vaddr, len);
	if (paddr == SPDK_VTOPHYS_ERROR) {
		DEBUG_PRINT("could not get phys addr for 0x%" PRIx64 "\n", vaddr);
		return -EFAULT;
	}

	return spdk_mem_map_clear_translation(map, vaddr, len);
}

static int
vtophys_unmap_iommu_paddr(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	uint64_t paddr;
	int rc;

	paddr = spdk_vtophys((void *)vaddr, NULL);
	if (paddr == SPDK_VTOPHYS_ERROR) {
		DEBUG_PRINT("could not get phys addr for 0x%" PRIx64 "\n", vaddr);
		return -EFAULT;
	}

	rc = vtophys_iommu_unmap_dma(paddr, len);
	if (rc) {
		DEBUG_PRINT("Failed to iommu unmap paddr 0x%" PRIx64 "\n", paddr);
		return -EFAULT;
	}

	return 0;
}
#endif

static int
vtophys_unmap_page(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	return spdk_mem_map_clear_translation(map, vaddr, len);
}

static int
vtophys_set_translation(struct spdk_mem_map *map, uint64_t vaddr, size_t len, uint64_t paddr)
{
	if (len == VALUE_4KB) {
		assert(!(paddr & VTOPHYS_4KB));
		paddr |= VTOPHYS_4KB;
	}

	return spdk_mem_map_set_translation(map, vaddr, len, paddr);
}

static int
vtophys_map_pci(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	uint64_t paddr;

	paddr = vtophys_get_paddr_pci(vaddr, len);
	if (paddr == SPDK_VTOPHYS_ERROR) {
		DEBUG_PRINT("could not get phys addr for 0x%"PRIx64"\n", vaddr);
		return -EFAULT;
	}

	return vtophys_set_translation(map, vaddr, len, paddr);
}

#if VFIO_ENABLED
static int
vtophys_map_vaddr(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	return vtophys_set_translation(map, vaddr, len, vaddr);
}
#endif

static int
vtophys_map_pagemap(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	uint64_t paddr;

	/* In iova=pa mode we can only reliably map hugepages, because we cannot guarantee that a
	 * 4KB page is pinned and isn't swapped or doesn't point to a zero page (which is likely if
	 * the memory was just mmap()-ed and hasn't been written yet).  To be totally safe, we'd
	 * have to check /proc/kpageflags, but checking the length and paddr's alignment should be
	 * enough to catch most cases.
	 */
	if (len < VALUE_2MB) {
		DEBUG_PRINT("page size 4KB is unsupported in iova=pa mode\n");
		return -EINVAL;
	}

	paddr = vtophys_get_paddr_pagemap(vaddr);
	if (paddr == SPDK_VTOPHYS_ERROR) {
		DEBUG_PRINT("could not get phys addr for 0x%"PRIx64"\n", vaddr);
		return -EFAULT;
	}

	if (paddr & MASK_2MB) {
		DEBUG_PRINT("invalid paddr 0x%" PRIx64 " - must be 2MB aligned\n", paddr);
		return -EINVAL;
	}
#if VFIO_ENABLED
	/* If the IOMMU is on, but DPDK is using iova-mode=pa, we want to register this memory
	 * with the IOMMU using the physical address to match. */
	if (spdk_iommu_is_enabled()) {
		int rc = vtophys_iommu_map_dma(vaddr, paddr, len);
		if (rc) {
			DEBUG_PRINT("Unable to assign vaddr 0x%" PRIx64" to paddr 0x%" PRIx64 "\n",
				    vaddr, paddr);
			return -EFAULT;
		}
	}
#endif
	return vtophys_set_translation(map, vaddr, len, paddr);
}

static int
vtophys_map_memseg(struct spdk_mem_map *map, uint64_t vaddr, size_t len, void *ctx)
{
	uint64_t paddr;
	size_t seglen;

	paddr = vtophys_get_paddr_memseg(vaddr, &seglen);
	if (paddr == SPDK_VTOPHYS_ERROR) {
		DEBUG_PRINT("could not get phys addr for 0x%"PRIx64"\n", vaddr);
		return -EFAULT;
	}

	if (rte_eal_iova_mode() == RTE_IOVA_PA && seglen < len) {
		DEBUG_PRINT("unexpected paddr=0x%"PRIx64" len=%zu for vaddr=0x%"PRIx64", "
			    "wanted=%zu\n", paddr, seglen, vaddr, len);
		return -EFAULT;
	}

	return vtophys_set_translation(map, vaddr, len, paddr);
}

static int
vtophys_walk_region(struct spdk_mem_map *map, void *vaddr, size_t len,
		    int (*map_page)(struct spdk_mem_map *map, uint64_t vaddr, size_t sz, void *ctx))
{
	return mem_map_walk_region(map, (uint64_t)vaddr, len, map_page, NULL);
}

static int
vtophys_notify(void *cb_ctx, struct spdk_mem_map *map,
	       enum spdk_mem_map_notify_action action,
	       void *vaddr, size_t len)
{
	int rc = 0;
	uint64_t paddr;

	if ((uintptr_t)vaddr & ~MASK_256TB) {
		DEBUG_PRINT("invalid usermode virtual address %p\n", vaddr);
		return -EINVAL;
	}

	if (((uintptr_t)vaddr & MASK_4KB) || (len & MASK_4KB)) {
		DEBUG_PRINT("invalid parameters, vaddr=%p len=%ju\n",
			    vaddr, len);
		return -EINVAL;
	}

	/* Get the physical address from the DPDK memsegs */
	paddr = vtophys_get_paddr_memseg((uint64_t)vaddr, NULL);

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		if (paddr == SPDK_VTOPHYS_ERROR) {
			/* This is not an address that DPDK is managing. */

			/* Check if this is a PCI BAR. They need special handling */
			paddr = vtophys_get_paddr_pci((uint64_t)vaddr, len);
			if (paddr != SPDK_VTOPHYS_ERROR) {
				return vtophys_walk_region(map, vaddr, len, vtophys_map_pci);
			}
#if VFIO_ENABLED
			enum rte_iova_mode iova_mode;

			iova_mode = rte_eal_iova_mode();

			if (spdk_iommu_is_enabled() && iova_mode == RTE_IOVA_VA) {
				/* We'll use the virtual address as the iova to match DPDK. */
				paddr = (uint64_t)vaddr;
				rc = vtophys_iommu_map_dma((uint64_t)vaddr, paddr, len);
				if (rc) {
					return -EFAULT;
				}

				rc = vtophys_walk_region(map, vaddr, len, vtophys_map_vaddr);
				if (rc != 0) {
					return rc;
				}
			} else
#endif
			{
				rc = vtophys_walk_region(map, vaddr, len, vtophys_map_pagemap);
				if (rc != 0) {
					return rc;
				}
			}
		} else {
			/* This is an address managed by DPDK. Just setup the translations. */
			rc = vtophys_walk_region(map, vaddr, len, vtophys_map_memseg);
			if (rc != 0) {
				return rc;
			}
		}

		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
#if VFIO_ENABLED
		if (paddr == SPDK_VTOPHYS_ERROR) {
			/*
			 * This is not an address that DPDK is managing.
			 */

			/* Check if this is a PCI BAR. They need special handling */
			paddr = vtophys_get_paddr_pci((uint64_t)vaddr, len);
			if (paddr != SPDK_VTOPHYS_ERROR) {
				return vtophys_walk_region(map, vaddr, len, vtophys_unmap_pci);
			}

			/* If vfio is enabled,
			 * we need to unmap the range from the IOMMU
			 */
			if (spdk_iommu_is_enabled()) {
				uint64_t buffer_len = len;
				uint8_t *va = vaddr;
				enum rte_iova_mode iova_mode;

				iova_mode = rte_eal_iova_mode();
				/*
				 * In virtual address mode, the region is contiguous and can be done in
				 * one unmap.
				 */
				if (iova_mode == RTE_IOVA_VA) {
					paddr = spdk_vtophys(va, &buffer_len);
					if (buffer_len != len || paddr != (uintptr_t)va) {
						DEBUG_PRINT("Unmapping %p with length %lu failed because "
							    "translation had address 0x%" PRIx64 " and length %lu\n",
							    va, len, paddr, buffer_len);
						return -EINVAL;
					}
					rc = vtophys_iommu_unmap_dma(paddr, len);
					if (rc) {
						DEBUG_PRINT("Failed to iommu unmap paddr 0x%" PRIx64 "\n", paddr);
						return -EFAULT;
					}
				} else if (iova_mode == RTE_IOVA_PA) {
					rc = vtophys_walk_region(map, vaddr, len,
								 vtophys_unmap_iommu_paddr);
					if (rc != 0) {
						return rc;
					}
				}
			}
		}
#endif
		rc = vtophys_walk_region(map, vaddr, len, vtophys_unmap_page);
		break;
	default:
		SPDK_UNREACHABLE();
	}

	return rc;
}

static int
numa_notify(void *cb_ctx, struct spdk_mem_map *map,
	    enum spdk_mem_map_notify_action action,
	    void *vaddr, size_t len)
{
	struct rte_memseg *seg;

	/* We always return 0 from here, even if we aren't able to get a
	 * memseg for the address. This can happen in non-DPDK memory
	 * registration paths, for example vhost or vfio-user. That is OK,
	 * spdk_mem_get_numa_id() just returns SPDK_ENV_NUMA_ID_ANY for
	 * that kind of memory. If we return an error here, the
	 * spdk_mem_register() from vhost or vfio-user would fail which is
	 * not what we want.
	 */
	seg = rte_mem_virt2memseg(vaddr, NULL);
	if (seg == NULL) {
		return 0;
	}

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		spdk_mem_map_set_translation(map, (uint64_t)vaddr, len, seg->socket_id);
		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
		spdk_mem_map_clear_translation(map, (uint64_t)vaddr, len);
		break;
	default:
		break;
	}

	return 0;
}

static int
vtophys_check_contiguous_entries(uint64_t paddr1, uint64_t paddr2)
{
	uint64_t page_size = (paddr1 & VTOPHYS_4KB) ? VALUE_4KB : VALUE_2MB;

	/* This function is always called with paddrs for two subsequent
	 * 4KB/2MB chunks in virtual address space, so those chunks will be only
	 * physically contiguous if the physical addresses are 4KB/2MB apart
	 * from each other as well.
	 */
	return (paddr2 - paddr1 == page_size);
}

#if VFIO_ENABLED

static bool
vfio_enabled(void)
{
	return rte_vfio_is_enabled("vfio_pci");
}

/* Check if IOMMU is enabled on the system */
static bool
has_iommu_groups(void)
{
	int count = 0;
	DIR *dir = opendir("/sys/kernel/iommu_groups");

	if (dir == NULL) {
		return false;
	}

	while (count < 3 && readdir(dir) != NULL) {
		count++;
	}

	closedir(dir);
	/* there will always be ./ and ../ entries */
	return count > 2;
}

static bool
vfio_noiommu_enabled(void)
{
	return rte_vfio_noiommu_is_enabled();
}

static void
vtophys_iommu_init(void)
{
	char proc_fd_path[PATH_MAX + 1];
	char link_path[PATH_MAX + 1];
	const char vfio_path[] = "/dev/vfio/vfio";
	DIR *dir;
	struct dirent *d;

	if (!vfio_enabled()) {
		return;
	}

	if (vfio_noiommu_enabled()) {
		g_vfio.noiommu_enabled = true;
	} else if (!has_iommu_groups()) {
		return;
	}

	dir = opendir("/proc/self/fd");
	if (!dir) {
		DEBUG_PRINT("Failed to open /proc/self/fd (%d)\n", errno);
		return;
	}

	while ((d = readdir(dir)) != NULL) {
		if (d->d_type != DT_LNK) {
			continue;
		}

		snprintf(proc_fd_path, sizeof(proc_fd_path), "/proc/self/fd/%s", d->d_name);
		if (readlink(proc_fd_path, link_path, sizeof(link_path)) != (sizeof(vfio_path) - 1)) {
			continue;
		}

		if (memcmp(link_path, vfio_path, sizeof(vfio_path) - 1) == 0) {
			sscanf(d->d_name, "%d", &g_vfio.fd);
			break;
		}
	}

	closedir(dir);

	if (g_vfio.fd < 0) {
		DEBUG_PRINT("Failed to discover DPDK VFIO container fd.\n");
		return;
	}

	g_vfio.enabled = true;

	return;
}

#endif

void
vtophys_pci_device_added(struct rte_pci_device *pci_device)
{
	struct spdk_vtophys_pci_device *vtophys_dev;

	pthread_mutex_lock(&g_vtophys_pci_devices_mutex);

	vtophys_dev = calloc(1, sizeof(*vtophys_dev));
	if (vtophys_dev) {
		vtophys_dev->pci_device = pci_device;
		TAILQ_INSERT_TAIL(&g_vtophys_pci_devices, vtophys_dev, tailq);
	} else {
		DEBUG_PRINT("Memory allocation error\n");
	}
	pthread_mutex_unlock(&g_vtophys_pci_devices_mutex);

#if VFIO_ENABLED
	struct spdk_vfio_dma_map *dma_map;
	int ret;

	if (!g_vfio.enabled) {
		return;
	}

	pthread_mutex_lock(&g_vfio.mutex);
	g_vfio.device_ref++;
	if (g_vfio.device_ref > 1) {
		pthread_mutex_unlock(&g_vfio.mutex);
		return;
	}

	/* This is the first SPDK device using DPDK vfio. This means that the first
	 * IOMMU group might have been just been added to the DPDK vfio container.
	 * From this point it is certain that the memory can be mapped now.
	 */
	TAILQ_FOREACH(dma_map, &g_vfio.maps, tailq) {
		ret = ioctl(g_vfio.fd, VFIO_IOMMU_MAP_DMA, &dma_map->map);
		if (ret) {
			DEBUG_PRINT("Cannot update DMA mapping, error %d\n", errno);
			break;
		}
	}
	pthread_mutex_unlock(&g_vfio.mutex);
#endif
}

void
vtophys_pci_device_removed(struct rte_pci_device *pci_device)
{
	struct spdk_vtophys_pci_device *vtophys_dev;

	pthread_mutex_lock(&g_vtophys_pci_devices_mutex);
	TAILQ_FOREACH(vtophys_dev, &g_vtophys_pci_devices, tailq) {
		if (vtophys_dev->pci_device == pci_device) {
			TAILQ_REMOVE(&g_vtophys_pci_devices, vtophys_dev, tailq);
			free(vtophys_dev);
			break;
		}
	}
	pthread_mutex_unlock(&g_vtophys_pci_devices_mutex);

#if VFIO_ENABLED
	struct spdk_vfio_dma_map *dma_map;
	int ret;

	if (!g_vfio.enabled) {
		return;
	}

	pthread_mutex_lock(&g_vfio.mutex);
	assert(g_vfio.device_ref > 0);
	g_vfio.device_ref--;
	if (g_vfio.device_ref > 0) {
		pthread_mutex_unlock(&g_vfio.mutex);
		return;
	}

	/* This is the last SPDK device using DPDK vfio. If DPDK doesn't have
	 * any additional devices using it's vfio container, all the mappings
	 * will be automatically removed by the Linux vfio driver. We unmap
	 * the memory manually to be able to easily re-map it later regardless
	 * of other, external factors.
	 */
	TAILQ_FOREACH(dma_map, &g_vfio.maps, tailq) {
		struct vfio_iommu_type1_dma_unmap unmap = {};
		unmap.argsz = sizeof(unmap);
		unmap.flags = 0;
		unmap.iova = dma_map->map.iova;
		unmap.size = dma_map->map.size;
		ret = ioctl(g_vfio.fd, VFIO_IOMMU_UNMAP_DMA, &unmap);
		if (ret) {
			DEBUG_PRINT("Cannot unmap DMA memory, error %d\n", errno);
			break;
		}
	}
	pthread_mutex_unlock(&g_vfio.mutex);
#endif
}

int
vtophys_init(void)
{
	const struct spdk_mem_map_ops vtophys_map_ops = {
		.notify_cb = vtophys_notify,
		.are_contiguous = vtophys_check_contiguous_entries,
	};

	const struct spdk_mem_map_ops phys_ref_map_ops = {
		.notify_cb = NULL,
		.are_contiguous = NULL,
	};

	const struct spdk_mem_map_ops numa_map_ops = {
		.notify_cb = numa_notify,
		.are_contiguous = NULL,
	};

#if VFIO_ENABLED
	vtophys_iommu_init();
#endif

	g_phys_ref_map = spdk_mem_map_alloc(0, &phys_ref_map_ops, NULL);
	if (g_phys_ref_map == NULL) {
		DEBUG_PRINT("phys_ref map allocation failed.\n");
		return -ENOMEM;
	}

	if (g_huge_pages) {
		g_numa_map = spdk_mem_map_alloc(SPDK_ENV_NUMA_ID_ANY, &numa_map_ops, NULL);
		if (g_numa_map == NULL) {
			DEBUG_PRINT("numa map allocation failed.\n");
			spdk_mem_map_free(&g_phys_ref_map);
			return -ENOMEM;
		}
	}

	g_vtophys_map = spdk_mem_map_alloc(SPDK_VTOPHYS_ERROR, &vtophys_map_ops, NULL);
	if (g_vtophys_map == NULL) {
		DEBUG_PRINT("vtophys map allocation failed\n");
		spdk_mem_map_free(&g_numa_map);
		spdk_mem_map_free(&g_phys_ref_map);
		return -ENOMEM;
	}

	return 0;
}

void
vtophys_fini(void)
{
	spdk_mem_map_free(&g_vtophys_map);
	spdk_mem_map_free(&g_numa_map);
	spdk_mem_map_free(&g_phys_ref_map);
}

uint64_t
spdk_vtophys(const void *buf, uint64_t *size)
{
	uint64_t vaddr, paddr, mask;

	vaddr = (uint64_t)buf;
	paddr = spdk_mem_map_translate(g_vtophys_map, vaddr, size);
	if (paddr == SPDK_VTOPHYS_ERROR) {
		return SPDK_VTOPHYS_ERROR;
	}

	mask = (paddr & VTOPHYS_4KB) ? MASK_4KB : MASK_2MB;
	return VTOPHYS_ADDR(paddr) + (vaddr & mask);
}

int32_t
spdk_mem_get_numa_id(const void *buf, uint64_t *size)
{
	if (!g_numa_map) {
		return SPDK_ENV_NUMA_ID_ANY;
	}

	return spdk_mem_map_translate(g_numa_map, (uint64_t)buf, size);
}

int
spdk_mem_get_fd_and_offset(void *vaddr, uint64_t *offset)
{
	struct rte_memseg *seg;
	int ret, fd;

	seg = rte_mem_virt2memseg(vaddr, NULL);
	if (!seg) {
		SPDK_ERRLOG("memory %p doesn't exist\n", vaddr);
		return -ENOENT;
	}

	fd = rte_memseg_get_fd_thread_unsafe(seg);
	if (fd < 0) {
		return fd;
	}

	ret = rte_memseg_get_fd_offset_thread_unsafe(seg, offset);
	if (ret < 0) {
		return ret;
	}

	return fd;
}

void
mem_disable_huge_pages(void)
{
	g_huge_pages = false;
}
