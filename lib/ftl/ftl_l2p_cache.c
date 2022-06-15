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
#include "spdk/cpuset.h"
#include "spdk/queue.h"
#include "spdk/thread.h"
#include "spdk/event.h"
#include "spdk/ftl.h"
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/util.h"

#include "ftl_core.h"
#include "ftl_l2p_cache.h"
#include "ftl_layout.h"
#include "ftl_nv_cache_io.h"
#include "mngt/ftl_mngt_steps.h"
#include "utils/ftl_defs.h"
#include "utils/ftl_addr_utils.h"

struct ftl_l2p_cache_page_io_ctx {
	struct ftl_l2p_cache *cache;
	uint64_t updates;
	struct spdk_bdev_io_wait_entry bdev_io_wait;
};

enum ftl_l2p_page_state {
	L2P_CACHE_PAGE_INIT,               /* Page in memory not initialized from disk page */
	L2P_CACHE_PAGE_READY,              /* Page initialized from disk */
	L2P_CACHE_PAGE_IN_FLUSH,           /* Page to be flushed and removed from memory */
	L2P_CACHE_PAGE_IN_PERSIST,         /* Page to be flushed to disk and not removed from memory */
	L2P_CACHE_PAGE_IN_CLEAR,           /* Page to be initialized with INVALID addresses */
	L2P_CACHE_PAGE_CRPTD               /* Page corrupted */
};

struct ftl_l2p_page {
	uint64_t updates;  /* updates as a result of write IOs */
	TAILQ_HEAD(, ftl_l2p_page_pinner_entry) ppe_list; /* for deferred pins */
	TAILQ_ENTRY(ftl_l2p_page) list_entry;
	uint64_t page_no;
	enum ftl_l2p_page_state state;
	uint64_t pin_ref_cnt;
	struct ftl_l2p_cache_page_io_ctx ctx;
	bool on_rank_list;
	void *l1;
	ftl_df_obj_id obj_id;
};

struct ftl_l2p_page_pinner_entry {
	uint16_t	idx;
	uint16_t	pg_pin_issued;
	uint16_t	pg_pin_completed;
	uint64_t	pg_no;
	TAILQ_ENTRY(ftl_l2p_page_pinner_entry)	lst_entry;
};

#define L2P_MAX_PAGES_TO_PIN 4
struct ftl_l2p_page_pinner {
	uint16_t to_pin_cnt;
	uint16_t pinned_cnt;
	uint16_t pin_fault_cnt;
	uint8_t locked;
	uint8_t deffered;
	struct ftl_l2p_pin_ctx *pin_ctx;
	TAILQ_ENTRY(ftl_l2p_page_pinner) list_entry;
	struct ftl_l2p_page_pinner_entry entry[L2P_MAX_PAGES_TO_PIN];
};

struct ftl_l2p_l1_map_entry {
	ftl_df_obj_id page_obj_id;
};

enum ftl_l2p_cache_state {
	L2P_CACHE_INIT,
	L2P_CACHE_RUNNING,
	L2P_CACHE_IN_SHTDW,
	L2P_CACHE_SHTDW_DONE,
};

struct ftl_l2p_cache_process_ctx {
	int status;
	ftl_l2p_cb cb;
	void *cb_ctx;
	uint64_t idx;
	uint64_t qd;
};

struct ftl_l2p_cache {
	struct spdk_ftl_dev *dev;
	void *l2;
	struct ftl_md *l2_md;
	struct ftl_md *l2_ctx_md;
	struct ftl_mempool *l2_ctx_pool;
	struct ftl_md *l1_md;

	TAILQ_HEAD(l2p_lru_list, ftl_l2p_page) lru_list;
	uint64_t lbas_in_page;
	uint64_t num_pages;       /* num pages to hold the entire L2P */

	uint64_t ios_in_flight;   /* IOS in flight for the cache lifetime */
	enum ftl_l2p_cache_state state;
	uint32_t current_qd;      /* control QD, reset on CLEAR/PERSIST/SHUTDOWN */
	uint32_t l2_pgs_avail;
	uint32_t l2_pgs_evicting;
	uint32_t l2_pgs_resident_max;
	uint32_t evict_keep;
	struct ftl_mempool *page_pinners_pool;
	TAILQ_HEAD(, ftl_l2p_page_pinner) dfrd_pinner_list; /* for deferred pinners */

	/* This is a context for a management process */
	struct ftl_l2p_cache_process_ctx mctx;

	/* MD layout cache: Offset on a device in FTL_BLOCK_SIZE unit */
	uint64_t cache_layout_offset;

	/* MD layout cache: Device of region */
	struct spdk_bdev_desc *cache_layout_bdev_desc;

	/* MD layout cache: IO channel of region */
	struct spdk_io_channel *cache_layout_ioch;
};

typedef void (*ftl_l2p_cache_clear_cb)(struct ftl_l2p_cache *cache, int status, void *ctx_page);
typedef void (*ftl_l2p_cache_persist_cb)(struct ftl_l2p_cache *cache, int status, void *ctx_page);
typedef void (*ftl_l2p_cache_sync_cb)(struct spdk_ftl_dev *dev, int status, void *page,
				      void *user_ctx);

static inline uint64_t ftl_l2p_cache_get_l1_page_size(void)
{
	return 1UL << 12;
}

static inline size_t ftl_l2p_cache_get_page_all_size(void)
{
	return sizeof(struct ftl_l2p_page) + ftl_l2p_cache_get_l1_page_size();
}

static void *_ftl_l2p_cache_init(struct spdk_ftl_dev *dev, size_t addr_size, uint64_t l2p_size)
{
	struct ftl_l2p_cache *cache;
	uint64_t l2_pages = spdk_divide_round_up(l2p_size, ftl_l2p_cache_get_l1_page_size());
	size_t l2_size = l2_pages * sizeof(struct ftl_l2p_l1_map_entry);

	cache = calloc(1, sizeof(struct ftl_l2p_cache));
	if (cache == NULL) {
		return NULL;
	}
	cache->dev = dev;

	cache->l2_md = ftl_md_create(dev,
				     spdk_divide_round_up(l2_size, FTL_BLOCK_SIZE), 0,
				     FTL_L2P_CACHE_MD_NAME_L2,
				     ftl_md_create_shm_flags(dev));

	if (cache->l2_md == NULL) {
		goto fail_l2_md;
	}
	cache->l2 = ftl_md_get_buffer(cache->l2_md);

	cache->lbas_in_page = dev->layout.l2p.lbas_in_page;
	cache->num_pages = l2_pages;

	return cache;
fail_l2_md:
	free(cache);
	return NULL;
}

int ftl_l2p_cache_init(struct spdk_ftl_dev *dev)
{
	uint64_t l2p_size = dev->num_lbas * dev->layout.l2p.addr_size;

	void *l2p = _ftl_l2p_cache_init(dev, dev->layout.l2p.addr_size, l2p_size);
	if (!l2p) {
		return -1;
	}
	dev->l2p = l2p;

	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;
	size_t page_pinners_pool_size = 1 << 15;
	cache->page_pinners_pool = ftl_mempool_create(page_pinners_pool_size,
				   sizeof(struct ftl_l2p_page_pinner),
				   64, SPDK_ENV_SOCKET_ID_ANY);
	if (!cache->page_pinners_pool) {
		return -1;
	}

	size_t max_resident_size = dev->conf.l2p_dram_limit << 20;
	size_t max_resident_pgs = max_resident_size / ftl_l2p_cache_get_page_all_size();

	if (max_resident_pgs > cache->num_pages) {
		SPDK_NOTICELOG("l2p memory limit higher than entire L2P size\n");
		max_resident_pgs = cache->num_pages;
	}

	/* Round down max res pgs to the nearest # of l2/l1 pgs */
	max_resident_size = max_resident_pgs * ftl_l2p_cache_get_page_all_size();
	SPDK_NOTICELOG("l2p maximum resident size is: %"PRIu64" (of %"PRIu64") MiB\n",
		       max_resident_size >> 20, dev->conf.l2p_dram_limit);

	TAILQ_INIT(&cache->dfrd_pinner_list);
	TAILQ_INIT(&cache->lru_list);

	cache->l2_ctx_md = ftl_md_create(dev,
					 spdk_divide_round_up(max_resident_pgs * (spdk_divide_round_up(sizeof(struct ftl_l2p_page),
							 64) * 64), FTL_BLOCK_SIZE), 0,
					 FTL_L2P_CACHE_MD_NAME_L2_CTX,
					 ftl_md_create_shm_flags(dev));

	if (cache->l2_ctx_md == NULL) {
		return -1;
	}

	cache->l2_pgs_resident_max = cache->l2_pgs_avail = max_resident_pgs;
	cache->l2_pgs_evicting = 0;
	cache->l2_ctx_pool = ftl_mempool_create_ext(ftl_md_get_buffer(cache->l2_ctx_md),
			     max_resident_pgs, sizeof(struct ftl_l2p_page), 64);

	if (cache->l2_ctx_pool == NULL) {
		return -1;
	}

#define FTL_L2P_CACHE_PAGE_AVAIL_MAX            16UL << 10
#define FTL_L2P_CACHE_PAGE_AVAIL_RATIO          5UL
	cache->evict_keep = spdk_divide_round_up(cache->num_pages * FTL_L2P_CACHE_PAGE_AVAIL_RATIO, 100);
	cache->evict_keep = spdk_min(FTL_L2P_CACHE_PAGE_AVAIL_MAX, cache->evict_keep);

	if (!ftl_fast_startup(dev) && !ftl_fast_recovery(dev)) {
		memset(cache->l2, (int)FTL_DF_OBJ_ID_INVALID, ftl_md_get_buffer_size(cache->l2_md));
		ftl_mempool_initialize_ext(cache->l2_ctx_pool);
	}

	cache->l1_md = ftl_md_create(dev,
				     max_resident_pgs, 0,
				     FTL_L2P_CACHE_MD_NAME_L1,
				     ftl_md_create_shm_flags(dev));

	if (cache->l1_md == NULL) {
		return -1;
	}

	/* Cache MD layout */
	const struct ftl_layout_region *reg = &dev->layout.region[ftl_layout_region_type_l2p];
	cache->cache_layout_offset = reg->current.offset;
	cache->cache_layout_bdev_desc = reg->bdev_desc;
	cache->cache_layout_ioch = reg->ioch;

	cache->state = L2P_CACHE_RUNNING;
	return 0;
}

static void ftl_l2p_cache_deinit_l2(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache)
{
	ftl_md_destroy(cache->l2_ctx_md, ftl_md_destroy_shm_flags(dev));
	cache->l2_ctx_md = NULL;

	ftl_mempool_destroy_ext(cache->l2_ctx_pool);
	cache->l2_ctx_pool = NULL;

	ftl_md_destroy(cache->l1_md, ftl_md_destroy_shm_flags(dev));
	cache->l1_md = NULL;

	ftl_mempool_destroy(cache->page_pinners_pool);
	cache->page_pinners_pool = NULL;
}

static void _ftl_l2p_cache_deinit(struct spdk_ftl_dev *dev)
{
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;

	ftl_l2p_cache_deinit_l2(dev, cache);
	ftl_md_destroy(cache->l2_md, ftl_md_destroy_shm_flags(dev));
	free(cache);
}

void ftl_l2p_cache_deinit(struct spdk_ftl_dev *dev)
{
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;

	if (!cache) {
		return;
	}
	assert(cache->state == L2P_CACHE_SHTDW_DONE || cache->state == L2P_CACHE_INIT);

	_ftl_l2p_cache_deinit(dev);
	dev->l2p = 0;
}

static void clear_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	md->owner.cb(dev, status, md->owner.cb_ctx);
}

void ftl_l2p_cache_clear(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	struct ftl_l2p_cache *cache = dev->l2p;
	struct ftl_md *md = dev->layout.md[ftl_layout_region_type_l2p];

	md->cb =  clear_cb;
	md->owner.cb = cb;
	md->owner.cb_ctx = cb_ctx;
	md->owner.private = cache;

	ftl_addr invalid_addr = FTL_ADDR_INVALID;

	ftl_md_clear(md, &invalid_addr, sizeof(invalid_addr), NULL);
}

bool ftl_l2p_cache_is_halted(struct spdk_ftl_dev *dev)
{
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;

	return cache->state == L2P_CACHE_SHTDW_DONE;
}

void ftl_l2p_cache_halt(struct spdk_ftl_dev *dev)
{
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;


	if (cache->state != L2P_CACHE_SHTDW_DONE) {
		cache->state = L2P_CACHE_IN_SHTDW;
		if (!cache->ios_in_flight && !cache->l2_pgs_evicting) {
			cache->state = L2P_CACHE_SHTDW_DONE;
		}
	}
}

void ftl_l2p_cache_process(struct spdk_ftl_dev *dev)
{
	struct ftl_l2p_cache *cache = dev->l2p;

	if (spdk_unlikely(cache->state != L2P_CACHE_RUNNING)) {
		return;
	}
}
