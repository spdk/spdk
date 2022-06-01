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

static bool pinner_is_done(struct ftl_l2p_page_pinner *pinner);
static void pinner_end(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache,
		       struct ftl_l2p_page_pinner *pinner);
static void page_out_io_retry(void *arg);
static void page_in_io_retry(void *arg);

static inline void ftl_l2p_page_queue_ppe(struct ftl_l2p_page *page,
		struct ftl_l2p_page_pinner_entry *ppe)
{
	TAILQ_INSERT_TAIL(&page->ppe_list, ppe, lst_entry);
}

static inline uint64_t ftl_l2p_cache_get_l1_page_size(void)
{
	return 1UL << 12;
}

static inline uint64_t ftl_l2p_cache_get_lbas_in_page(struct ftl_l2p_cache *cache)
{
	return cache->lbas_in_page;
}

static inline size_t ftl_l2p_cache_get_page_all_size(void)
{
	return sizeof(struct ftl_l2p_page) + ftl_l2p_cache_get_l1_page_size();
}

static void ftl_l2p_cache_page_remove_rank(struct ftl_l2p_cache *cache, struct ftl_l2p_page *page)
{
	assert(page);
	assert(page->on_rank_list);

	TAILQ_REMOVE(&cache->lru_list, page, list_entry);
	page->on_rank_list = false;
}

static void ftl_l2p_cache_page_append_rank(struct ftl_l2p_cache *cache, struct ftl_l2p_page *page)
{
	assert(page);
	assert(!page->on_rank_list);

	TAILQ_INSERT_TAIL(&cache->lru_list, page, list_entry);

	page->on_rank_list = true;
}

static void ftl_l2p_cache_page_rank_up(struct ftl_l2p_cache *cache, struct ftl_l2p_page *page)
{
	if (!page->on_rank_list) {
		return;
	}

	ftl_l2p_cache_page_remove_rank(cache, page);
	ftl_l2p_cache_page_append_rank(cache, page);
}

static inline void ftl_l2p_cache_page_insert(struct ftl_l2p_cache *cache, struct ftl_l2p_page *page)
{
	struct ftl_l2p_l1_map_entry *me = (struct ftl_l2p_l1_map_entry *)cache->l2;
	assert(me);

	assert(me[page->page_no].page_obj_id == FTL_DF_OBJ_ID_INVALID);
	me[page->page_no].page_obj_id = page->obj_id;
}

static void ftl_l2p_cache_page_remove(struct ftl_l2p_cache *cache, struct ftl_l2p_page *page)
{
	struct ftl_l2p_l1_map_entry *me = (struct ftl_l2p_l1_map_entry *)cache->l2;
	assert(me);
	assert(me[page->page_no].page_obj_id != FTL_DF_OBJ_ID_INVALID);
	assert(TAILQ_EMPTY(&page->ppe_list));

	me[page->page_no].page_obj_id = FTL_DF_OBJ_ID_INVALID;
	cache->l2_pgs_avail++;
	ftl_mempool_put(cache->l2_ctx_pool, page);
}

static inline struct ftl_l2p_page *ftl_l2p_cache_page_cold(struct ftl_l2p_cache *cache)
{
	return TAILQ_FIRST(&cache->lru_list);
}

static inline struct ftl_l2p_page *ftl_l2p_cache_page_next(struct ftl_l2p_page *page)
{
	return TAILQ_NEXT(page, list_entry);
}

static inline uint64_t ftl_l2p_cache_page_get_bdev_offset(struct ftl_l2p_cache *cache,
		struct ftl_l2p_page *page)
{
	return cache->cache_layout_offset + page->page_no;
}

static inline struct spdk_bdev_desc *ftl_l2p_cache_get_bdev_desc(struct ftl_l2p_cache *cache)
{
	return cache->cache_layout_bdev_desc;
}

static inline struct spdk_io_channel *ftl_l2p_cache_get_bdev_iochannel(struct ftl_l2p_cache *cache)
{
	return cache->cache_layout_ioch;
}

static struct ftl_l2p_page *ftl_l2p_cache_page_alloc(struct ftl_l2p_cache *cache, size_t page_no)
{
	struct ftl_l2p_page *page = ftl_mempool_get(cache->l2_ctx_pool);
	ftl_bug(!page);

	cache->l2_pgs_avail--;

	memset(page, '\0', sizeof(*page));

	page->obj_id = ftl_mempool_get_df_obj_id(cache->l2_ctx_pool, page);

	page->l1 = (char *)ftl_md_get_buffer(cache->l1_md) + ftl_mempool_get_elem_id(cache->l2_ctx_pool,
			page) * FTL_BLOCK_SIZE;

	TAILQ_INIT(&page->ppe_list);

	page->page_no = page_no;
	page->state = L2P_CACHE_PAGE_INIT;

	return page;
}

static inline bool ftl_l2p_cache_page_can_remove(struct ftl_l2p_page *page)
{
	return (!page->updates &&
		page->state != L2P_CACHE_PAGE_INIT &&
		!page->pin_ref_cnt);
}

static inline ftl_addr ftl_l2p_cache_get_addr(struct spdk_ftl_dev *dev,
		struct ftl_l2p_cache *cache, struct ftl_l2p_page *page, uint64_t lba)
{
	return ftl_addr_load(dev, page->l1, lba % cache->lbas_in_page);
}

static inline void ftl_l2p_cache_set_addr(struct spdk_ftl_dev *dev,
		struct ftl_l2p_cache *cache, struct ftl_l2p_page *page,
		uint64_t lba, ftl_addr addr)
{
	ftl_addr_store(dev, page->l1, lba % cache->lbas_in_page, addr);
}

static inline void ftl_l2p_cache_page_pin(struct ftl_l2p_cache *cache,
		struct ftl_l2p_page *page)
{
	page->pin_ref_cnt++;
	if (page->on_rank_list) {
		ftl_l2p_cache_page_remove_rank(cache, page);
	}
}

static inline void ftl_l2p_cache_page_unpin(struct ftl_l2p_cache *cache,
		struct ftl_l2p_page *page)
{
	page->pin_ref_cnt--;
	if (!page->pin_ref_cnt && !page->on_rank_list && page->state != L2P_CACHE_PAGE_IN_FLUSH) {
		/* L2P_CACHE_PAGE_IN_FLUSH: the page is currently being evicted.
		 * In such a case, the page can't be returned to the rank list, because
		 * the ongoing eviction will remove it if no pg updates had happened.
		 * Moreover, the page could make it to the top of the rank list and be
		 * selected for another eviction, while the ongoing one did not finish yet.
		 *
		 * Depending on the page updates tracker, the page will be evicted
		 * or returned to the rank list in context of the eviction completion
		 * cb - see page_out_io_complete().
		 */
		ftl_l2p_cache_page_append_rank(cache, page);
	}
}

static inline bool ftl_l2p_cache_page_can_evict(struct ftl_l2p_page *page)
{
	return (page->state == L2P_CACHE_PAGE_IN_FLUSH ||
		page->state == L2P_CACHE_PAGE_IN_PERSIST ||
		page->state == L2P_CACHE_PAGE_INIT ||
		page->pin_ref_cnt) ? false : true;
}

static bool ftl_l2p_cache_evict_continue(struct ftl_l2p_cache *cache)
{
	return cache->l2_pgs_avail + cache->l2_pgs_evicting < cache->evict_keep;
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

static struct ftl_l2p_page *
get_l2p_page_by_df_id(struct ftl_l2p_cache *cache, size_t page_no)
{
	struct ftl_l2p_l1_map_entry *me = (struct ftl_l2p_l1_map_entry *)cache->l2;
	ftl_df_obj_id obj_id = me[page_no].page_obj_id;

	if (obj_id != FTL_DF_OBJ_ID_INVALID) {
		return ftl_mempool_get_df_ptr(cache->l2_ctx_pool, obj_id);
	}

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

static void process_init_ctx(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache,
			     ftl_l2p_cb cb, void *cb_ctx)
{
	struct ftl_l2p_cache_process_ctx *ctx = &cache->mctx;

	assert(NULL == ctx->cb_ctx);
	assert(0 == cache->current_qd);
	assert(0 == cache->l2_pgs_evicting);

	memset(ctx, 0, sizeof(*ctx));

	ctx->cb = cb;
	ctx->cb_ctx = cb_ctx;
}

static void process_finish(struct ftl_l2p_cache *cache)
{
	struct ftl_l2p_cache_process_ctx ctx = cache->mctx;

	assert(cache->l2_pgs_avail == cache->l2_pgs_resident_max);
	assert(0 == ctx.qd);

	memset(&cache->mctx, 0, sizeof(cache->mctx));
	ctx.cb(cache->dev, ctx.status, ctx.cb_ctx);
}

static void process_page_out(struct ftl_l2p_page *page, spdk_bdev_io_completion_cb cb)
{
	struct ftl_l2p_cache *cache = page->ctx.cache;
	struct spdk_ftl_dev *dev = cache->dev;
	int rc;

	assert(page->l1);

	rc = ftl_nv_cache_bdev_write_blocks_with_md(dev, ftl_l2p_cache_get_bdev_desc(cache),
			ftl_l2p_cache_get_bdev_iochannel(cache),
			page->l1, NULL, ftl_l2p_cache_page_get_bdev_offset(cache, page),
			1, cb, page);

	if (rc) {
		cb(NULL, false, page);
	}
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

static void process_persist(struct ftl_l2p_cache *cache);

static void process_persist_page_out_cb(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct ftl_l2p_page *page = (struct ftl_l2p_page *)arg;
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)page->ctx.cache;
	struct ftl_l2p_cache_process_ctx *ctx = &cache->mctx;

	if (bdev_io) {
		spdk_bdev_free_io(bdev_io);
	}
	if (!success) {
		ctx->status = -EIO;
	}

	ftl_l2p_cache_page_remove(cache, page);

	ctx->qd--;
	process_persist(cache);
}

static void process_persist(struct ftl_l2p_cache *cache)
{
	struct ftl_l2p_cache_process_ctx *ctx = &cache->mctx;

	while (ctx->idx < cache->num_pages && ctx->qd < 64) {
		struct ftl_l2p_page *page = get_l2p_page_by_df_id(cache, ctx->idx);
		if (!page) {
			ctx->idx++;
			continue;
		}

		if (page->on_rank_list) {
			ftl_l2p_cache_page_remove_rank(cache, page);
		}

		if (page->updates) {
			/* Need to persist the page */
			page->state = L2P_CACHE_PAGE_IN_PERSIST;
			page->ctx.cache = cache;
			ctx->qd++;
			process_page_out(page, process_persist_page_out_cb);
		} else {
			ftl_l2p_cache_page_remove(cache, page);
		}

		ctx->idx++;
	}

	if (0 == ctx->qd) {
		process_finish(cache);
	}
}

void ftl_l2p_cache_persist(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;

	process_init_ctx(dev, cache, cb, cb_ctx);
	process_persist(cache);
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

static inline struct ftl_l2p_page *get_page(struct ftl_l2p_cache *cache, uint64_t lba)
{
	return get_l2p_page_by_df_id(cache, lba / cache->lbas_in_page);
}

static inline void ftl_l2p_cache_init_pinner(struct ftl_l2p_page_pinner *pinner,
		struct ftl_l2p_pin_ctx *pin_ctx)
{
	pinner->to_pin_cnt = 0;
	pinner->pinned_cnt = 0;
	pinner->pin_fault_cnt = 0;
	pinner->locked = 0;
	pinner->deffered = 0;
	pinner->pin_ctx = pin_ctx;
}

static inline bool ftl_l2p_cache_running(struct ftl_l2p_cache *cache)
{
	return cache->state == L2P_CACHE_RUNNING;
}

static inline bool ftl_l2p_cache_page_is_pinnable(struct ftl_l2p_page *page)
{
	return page->state != L2P_CACHE_PAGE_INIT;
}

void ftl_l2p_cache_pin(struct spdk_ftl_dev *dev, struct ftl_l2p_pin_ctx *pin_ctx)
{
	assert(dev->num_lbas >= pin_ctx->lba + pin_ctx->count);
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;
	struct ftl_l2p_page_pinner *pinner;

	/* Calculate first and last page to pin, count of them */
	uint64_t start = pin_ctx->lba / cache->lbas_in_page;
	uint64_t end = (pin_ctx->lba + pin_ctx->count - 1) / cache->lbas_in_page;
	uint64_t count = end - start + 1;
	uint64_t i;

	if (spdk_unlikely(count > L2P_MAX_PAGES_TO_PIN)) {
		ftl_l2p_pin_complete(dev, -E2BIG, pin_ctx);
		return;
	}

	/* Get and initialize pinners */
	assert(ftl_l2p_cache_running(cache));
	pinner = ftl_mempool_get(cache->page_pinners_pool);
	if (!pinner) {
		ftl_l2p_pin_complete(dev, -EAGAIN, pin_ctx);
		return;
	}
	ftl_l2p_cache_init_pinner(pinner, pin_ctx);

	bool defer_pin = false;

	struct ftl_l2p_page_pinner_entry *entry = pinner->entry;
	for (i = start; i <= end; i++, entry++) {
		entry->idx = pinner->to_pin_cnt;
		entry->pg_no = i;
		entry->pg_pin_completed = false;
		entry->pg_pin_issued = false;

		pinner->to_pin_cnt++;

		/* Try get page and pin */
		struct ftl_l2p_page *page = get_l2p_page_by_df_id(cache, i);
		if (page) {
			if (ftl_l2p_cache_page_is_pinnable(page)) {
				/* Page available and we can pin it */
				pinner->pinned_cnt++;
				entry->pg_pin_issued = true;
				entry->pg_pin_completed = true;
				ftl_l2p_cache_page_pin(cache, page);
			} else {
				/* The page is being loaded */
				/* Queue the page pin entry to be executed on page in */
				ftl_l2p_page_queue_ppe(page, entry);
				entry->pg_pin_issued = true;
			}
		} else {
			/* The page is not in the cache, queue the pinner to page in */
			defer_pin = true;
		}
	}

	/* Check if pinner is done */
	if (pinner_is_done(pinner)) {
		pinner_end(dev, cache, pinner);
	} else if (defer_pin) {
		TAILQ_INSERT_TAIL(&cache->dfrd_pinner_list, pinner, list_entry);
		pinner->deffered = 1;
	}
}

void ftl_l2p_cache_unpin(struct spdk_ftl_dev *dev, uint64_t lba, uint64_t count)
{
	assert(dev->num_lbas >= lba + count);
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;
	struct ftl_l2p_page *page;

	uint64_t start = lba / cache->lbas_in_page;
	uint64_t end = (lba + count - 1) / cache->lbas_in_page;
	assert(count);
	assert(start < cache->num_pages);
	assert(end < cache->num_pages);

	uint64_t i;
	for (i = start; i <= end; i++) {
		page = get_l2p_page_by_df_id(cache, i);
		ftl_bug(!page);
		ftl_l2p_cache_page_unpin(cache, page);
	}
}

ftl_addr ftl_l2p_cache_get(struct spdk_ftl_dev *dev, uint64_t lba)
{
	assert(dev->num_lbas > lba);
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;
	struct ftl_l2p_page *page = get_page(cache, lba);

	ftl_bug(!page);
	assert(ftl_l2p_cache_running(cache));
	assert(page->pin_ref_cnt);

	ftl_l2p_cache_page_rank_up(cache, page);
	ftl_addr addr = ftl_l2p_cache_get_addr(dev, cache, page, lba);

	return addr;
}

void ftl_l2p_cache_set(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr addr)
{
	assert(dev->num_lbas > lba);
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;
	struct ftl_l2p_page *page = get_page(cache, lba);

	ftl_bug(!page);
	assert(ftl_l2p_cache_running(cache));
	assert(page->pin_ref_cnt);

	page->updates++;
	ftl_l2p_cache_page_rank_up(cache, page);
	ftl_l2p_cache_set_addr(dev, cache, page, lba, addr);
}

static struct ftl_l2p_page *page_allocate(struct ftl_l2p_cache *cache,
		uint64_t page_no)
{
	struct ftl_l2p_page *page = ftl_l2p_cache_page_alloc(cache, page_no);
	ftl_l2p_cache_page_insert(cache, page);
	return page;
}

static bool pinner_is_done(struct ftl_l2p_page_pinner *pinner)
{
	if (pinner->locked) {
		return false;
	}

	assert(pinner->pinned_cnt + pinner->pin_fault_cnt <= pinner->to_pin_cnt);
	return (pinner->pinned_cnt + pinner->pin_fault_cnt == pinner->to_pin_cnt);
}

static void pinner_unpin(struct ftl_l2p_cache *cache, struct ftl_l2p_page_pinner *pinner)
{
	uint64_t i;
	struct ftl_l2p_page_pinner_entry *pentry = pinner->entry;
	for (i = 0; i < pinner->to_pin_cnt; i++, pentry++) {
		if (false == pentry->pg_pin_completed) {
			continue;
		}

		struct ftl_l2p_page *pinned_page = get_l2p_page_by_df_id(cache, pentry->pg_no);
		ftl_bug(!pinned_page);

		ftl_l2p_cache_page_unpin(cache, pinned_page);
	}
}

static void pinner_end(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache,
		       struct ftl_l2p_page_pinner *pinner)
{
	if (spdk_likely(0 == pinner->pin_fault_cnt)) {
		ftl_l2p_pin_complete(dev, 0, pinner->pin_ctx);
	} else {
		pinner_unpin(cache, pinner);
		ftl_l2p_pin_complete(dev, -EIO, pinner->pin_ctx);
	}

	if (pinner->deffered) {
		TAILQ_REMOVE(&cache->dfrd_pinner_list, pinner, list_entry);
	}

	assert(0 == pinner->locked);
	ftl_mempool_put(cache->page_pinners_pool, pinner);
}

static struct ftl_l2p_page_pinner *pinner_from_entry(struct ftl_l2p_page_pinner_entry *pentry)
{
	uint64_t idx = pentry->idx;
	struct ftl_l2p_page_pinner *pinner;

	pinner = SPDK_CONTAINEROF(pentry, struct ftl_l2p_page_pinner, entry[idx]);

	assert(idx < L2P_MAX_PAGES_TO_PIN);

	return pinner;
}

static void page_in_io_complate(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache,
				struct ftl_l2p_page *page, bool success)
{
	cache->ios_in_flight--;

	assert(0 == page->pin_ref_cnt);
	assert(L2P_CACHE_PAGE_INIT == page->state);
	assert(false == page->on_rank_list);

	if (spdk_likely(success)) {
		page->state = L2P_CACHE_PAGE_READY;
	}

	struct ftl_l2p_page_pinner *pinner;
	struct ftl_l2p_page_pinner_entry *pentry;

	while ((pentry = TAILQ_FIRST(&page->ppe_list))) {
		TAILQ_REMOVE(&page->ppe_list, pentry, lst_entry);

		pinner = pinner_from_entry(pentry);

		assert(pentry->idx < pinner->to_pin_cnt);
		assert(false == pentry->pg_pin_completed);

		if (success) {
			ftl_l2p_cache_page_pin(cache, page);
			pinner->pinned_cnt++;
			pentry->pg_pin_completed = true;
		} else {
			pinner->pin_fault_cnt++;
		}

		/* Check if pinner is done */
		if (pinner_is_done(pinner)) {
			pinner_end(dev, cache, pinner);
		}
	}

	if (spdk_unlikely(!success)) {
		ftl_bug(page->on_rank_list);
		ftl_l2p_cache_page_remove(cache, page);
	}
}

static void page_in_io_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_l2p_page *page = cb_arg;
	struct ftl_l2p_cache *cache = page->ctx.cache;
	struct spdk_ftl_dev *dev = cache->dev;

	spdk_bdev_free_io(bdev_io);
	page_in_io_complate(dev, cache, page, success);
}

static void page_in_io(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache,
		       struct ftl_l2p_page *page)
{
	page->ctx.cache = cache;

	int rc = ftl_nv_cache_bdev_read_blocks_with_md(cache->dev, ftl_l2p_cache_get_bdev_desc(cache),
				       ftl_l2p_cache_get_bdev_iochannel(cache),
				       page->l1, NULL, ftl_l2p_cache_page_get_bdev_offset(cache, page),
				       1, page_in_io_cb, page);
	cache->ios_in_flight++;
	if (spdk_likely(0 == rc)) {
		return;
	}

	if (rc == -ENOMEM) {
		struct spdk_io_channel *ioch = ftl_l2p_cache_get_bdev_iochannel(cache);
		struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(ftl_l2p_cache_get_bdev_desc(cache));
		struct spdk_bdev_io_wait_entry *bdev_io_wait = &page->ctx.bdev_io_wait;
		bdev_io_wait->bdev = bdev;
		bdev_io_wait->cb_fn = page_in_io_retry;
		bdev_io_wait->cb_arg = page;

		rc = spdk_bdev_queue_io_wait(bdev, ioch, bdev_io_wait);
		ftl_bug(rc);
	} else {
		ftl_abort();
	}
}

static void page_in_io_retry(void *arg)
{
	struct ftl_l2p_page *page = arg;
	struct ftl_l2p_cache *cache = page->ctx.cache;
	struct spdk_ftl_dev *dev = cache->dev;

	cache->ios_in_flight--;
	page_in_io(dev, cache, page);
}

static void page_in(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache,
		    struct ftl_l2p_page_pinner *pinner, struct ftl_l2p_page_pinner_entry *pentry)
{
	struct ftl_l2p_page *page;
	bool page_in = false;

	/* Get page */
	page = get_l2p_page_by_df_id(cache, pentry->pg_no);
	if (!page) {
		/* Page not allocated yet, do it */
		page = page_allocate(cache, pentry->pg_no);
		page_in = true;
	}

	if (ftl_l2p_cache_page_is_pinnable(page)) {
		ftl_l2p_cache_page_pin(cache, page);
		pinner->pinned_cnt++;
		pentry->pg_pin_issued = true;
		pentry->pg_pin_completed = true;
	} else {
		pentry->pg_pin_issued = true;
		ftl_l2p_page_queue_ppe(page, pentry);
	}

	if (page_in) {
		page_in_io(dev, cache, page);
	}
}

static void ftl_l2p_cache_process_pinners(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache)
{
	struct ftl_l2p_page_pinner *pinner;

	pinner = TAILQ_FIRST(&cache->dfrd_pinner_list);
	if (!pinner) {
		/* No pinner */
		return;
	}

	if (pinner->to_pin_cnt > cache->l2_pgs_avail) {
		/* No enough page to pin, wait */
		return;
	}
	if (cache->ios_in_flight > 512) {
		/* To big QD */
		return;
	}

	TAILQ_REMOVE(&cache->dfrd_pinner_list, pinner, list_entry);
	pinner->deffered = 0;
	pinner->locked = 1;

	/* Now we can start pining */
	uint64_t i;
	struct ftl_l2p_page_pinner_entry *pentry = pinner->entry;
	for (i = 0; i < pinner->to_pin_cnt; i++, pentry++) {
		if (!pentry->pg_pin_issued) {
			page_in(dev, cache, pinner, pentry);
		}
	}

	pinner->locked = 0;

	/* Check if pinner is done */
	if (pinner_is_done(pinner)) {
		pinner_end(dev, cache, pinner);
	}

	return;
}

static struct ftl_l2p_page *
eviction_get_page(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache)
{
	uint64_t i = 0;
	struct ftl_l2p_page *page = ftl_l2p_cache_page_cold(cache);

	while (page) {
		ftl_bug(L2P_CACHE_PAGE_READY != page->state);
		ftl_bug(page->pin_ref_cnt);

		if (ftl_l2p_cache_page_can_evict(page)) {
			ftl_l2p_cache_page_remove_rank(cache, page);
			return page;
		}

		/*
		 * Practically only one iterations is needed to find a page. It is because
		 * the rank of pages contains only ready and unpined pages
		 */
		ftl_bug(++i > 1024);

		page = ftl_l2p_cache_page_next(page);
	}

	return NULL;
}

static void page_out_io_complete(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache,
				 struct ftl_l2p_page *page, bool success)
{
	cache->l2_pgs_evicting--;

	ftl_bug(page->ctx.updates > page->updates);
	ftl_bug(!TAILQ_EMPTY(&page->ppe_list));
	ftl_bug(page->on_rank_list);

	if (spdk_likely(success)) {
		page->updates -= page->ctx.updates;
	}

	if (success && ftl_l2p_cache_page_can_remove(page)) {
		ftl_l2p_cache_page_remove(cache, page);
	} else {
		if (!page->pin_ref_cnt) {
			ftl_l2p_cache_page_append_rank(cache, page);
		}
		page->state = L2P_CACHE_PAGE_READY;
	}
}

static void page_out_io_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_l2p_page *page = cb_arg;
	struct ftl_l2p_cache *cache = page->ctx.cache;
	struct spdk_ftl_dev *dev = cache->dev;

	spdk_bdev_free_io(bdev_io);
	page_out_io_complete(dev, cache, page, success);
}

static void page_out_io(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache,
			struct ftl_l2p_page *page)
{
	page->ctx.cache = cache;

	int rc = ftl_nv_cache_bdev_write_blocks_with_md(dev, ftl_l2p_cache_get_bdev_desc(cache),
			ftl_l2p_cache_get_bdev_iochannel(cache),
			page->l1, NULL, ftl_l2p_cache_page_get_bdev_offset(cache, page),
			1, page_out_io_cb, page);

	cache->l2_pgs_evicting++;
	if (spdk_likely(0 == rc)) {
		return;
	}

	if (rc == -ENOMEM) {
		struct spdk_io_channel *ioch = ftl_l2p_cache_get_bdev_iochannel(cache);
		struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(ftl_l2p_cache_get_bdev_desc(cache));
		struct spdk_bdev_io_wait_entry *bdev_io_wait = &page->ctx.bdev_io_wait;
		bdev_io_wait->bdev = bdev;
		bdev_io_wait->cb_fn = page_out_io_retry;
		bdev_io_wait->cb_arg = page;

		rc = spdk_bdev_queue_io_wait(bdev, ioch, bdev_io_wait);
		ftl_bug(rc);
	} else {
		ftl_abort();
	}
}

static void page_out_io_retry(void *arg)
{
	struct ftl_l2p_page *page = arg;
	struct ftl_l2p_cache *cache = page->ctx.cache;
	struct spdk_ftl_dev *dev = cache->dev;

	cache->l2_pgs_evicting--;
	page_out_io(dev, cache, page);
}

static void ftl_l2p_cache_process_eviction(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache)
{
	if (!ftl_l2p_cache_evict_continue(cache)) {
		return;
	}

	if (cache->l2_pgs_evicting > 512) {
		return;
	}

	struct ftl_l2p_page *page = eviction_get_page(dev, cache);
	if (spdk_unlikely(!page)) {
		return;
	}

	if (page->updates) {
		page->state = L2P_CACHE_PAGE_IN_FLUSH;
		page->ctx.updates = page->updates;
		page_out_io(dev, cache, page);
	} else {
		/* Page clean and we can remove it */
		ftl_l2p_cache_page_remove(cache, page);
	}
}

void ftl_l2p_cache_process(struct spdk_ftl_dev *dev)
{
	struct ftl_l2p_cache *cache = dev->l2p;

	if (spdk_unlikely(cache->state != L2P_CACHE_RUNNING)) {
		return;
	}

	int i;
	for (i = 0; i < 256; i++) {
		if (cache->ios_in_flight > 512) {
			break;
		}

		if (TAILQ_EMPTY(&cache->dfrd_pinner_list)) {
			break;
		}

		ftl_l2p_cache_process_pinners(dev, cache);
	}

	ftl_l2p_cache_process_eviction(dev, cache);
}
