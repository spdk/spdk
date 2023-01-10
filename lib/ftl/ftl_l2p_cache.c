/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
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
	spdk_bdev_io_completion_cb cb;
	struct spdk_bdev_io_wait_entry bdev_io_wait;
};

enum ftl_l2p_page_state {
	L2P_CACHE_PAGE_INIT,		/* Page in memory not initialized from disk page */
	L2P_CACHE_PAGE_READY,		/* Page initialized from disk */
	L2P_CACHE_PAGE_FLUSHING,	/* Page is being flushed to disk and removed from memory */
	L2P_CACHE_PAGE_PERSISTING,	/* Page is being flushed to disk and not removed from memory */
	L2P_CACHE_PAGE_CLEARING,	/* Page is being initialized with INVALID addresses */
	L2P_CACHE_PAGE_CORRUPTED	/* Page corrupted */
};

struct ftl_l2p_page {
	uint64_t updates; /* Number of times an L2P entry was updated in the page since it was last persisted */
	TAILQ_HEAD(, ftl_l2p_page_wait_ctx) ppe_list; /* for deferred pins */
	TAILQ_ENTRY(ftl_l2p_page) list_entry;
	uint64_t page_no;
	enum ftl_l2p_page_state state;
	uint64_t pin_ref_cnt;
	struct ftl_l2p_cache_page_io_ctx ctx;
	bool on_lru_list;
	void *page_buffer;
	uint64_t ckpt_seq_id;
	ftl_df_obj_id obj_id;
};

struct ftl_l2p_page_set;

struct ftl_l2p_page_wait_ctx {
	uint16_t	pg_pin_issued;
	uint16_t	pg_pin_completed;
	struct ftl_l2p_page_set *parent;
	uint64_t	pg_no;
	TAILQ_ENTRY(ftl_l2p_page_wait_ctx) list_entry;
};

/* A L2P page contains 1024 4B entries (or 512 8B ones for big drives).
 * Currently internal IO will only pin 1 LBA at a time, so only one entry should be needed.
 * User IO is split on internal xfer_size boundaries, which is currently set to 1MiB (256 blocks),
 * so one entry should also be enough.
 * TODO: We should probably revisit this though, when/if the xfer_size is based on io requirements of the
 * bottom device (e.g. RAID5F), since then big IOs (especially unaligned ones) could potentially break this.
 */
#define L2P_MAX_PAGES_TO_PIN 4
struct ftl_l2p_page_set {
	uint16_t to_pin_cnt;
	uint16_t pinned_cnt;
	uint16_t pin_fault_cnt;
	uint8_t locked;
	uint8_t deferred;
	struct ftl_l2p_pin_ctx *pin_ctx;
	TAILQ_ENTRY(ftl_l2p_page_set) list_entry;
	struct ftl_l2p_page_wait_ctx entry[L2P_MAX_PAGES_TO_PIN];
};

struct ftl_l2p_l1_map_entry {
	ftl_df_obj_id page_obj_id;
};

enum ftl_l2p_cache_state {
	L2P_CACHE_INIT,
	L2P_CACHE_RUNNING,
	L2P_CACHE_IN_SHUTDOWN,
	L2P_CACHE_SHUTDOWN_DONE,
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
	struct ftl_l2p_l1_map_entry *l2_mapping;
	struct ftl_md *l2_md;
	struct ftl_md *l2_ctx_md;
	struct ftl_mempool *l2_ctx_pool;
	struct ftl_md *l1_md;

	TAILQ_HEAD(l2p_lru_list, ftl_l2p_page) lru_list;
	/* TODO: A lot of / and % operations are done on this value, consider adding a shift based field and calculactions instead */
	uint64_t lbas_in_page;
	uint64_t num_pages;		/* num pages to hold the entire L2P */

	uint64_t ios_in_flight;		/* Currently in flight IOs, to determine l2p shutdown readiness */
	enum ftl_l2p_cache_state state;
	uint32_t l2_pgs_avail;
	uint32_t l2_pgs_evicting;
	uint32_t l2_pgs_resident_max;
	uint32_t evict_keep;
	struct ftl_mempool *page_sets_pool;
	TAILQ_HEAD(, ftl_l2p_page_set) deferred_page_set_list; /* for deferred page sets */

	/* Process unmap in background */
	struct {
#define FTL_L2P_MAX_LAZY_UNMAP_QD 1
		/* Unmap queue depth */
		uint32_t qd;
		/* Currently processed page */
		uint64_t page_no;
		/* Context for page pinning */
		struct ftl_l2p_pin_ctx pin_ctx;
	} lazy_unmap;

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

static bool page_set_is_done(struct ftl_l2p_page_set *page_set);
static void page_set_end(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache,
			 struct ftl_l2p_page_set *page_set);
static void page_out_io_retry(void *arg);
static void page_in_io_retry(void *arg);

static inline void
ftl_l2p_page_queue_wait_ctx(struct ftl_l2p_page *page,
			    struct ftl_l2p_page_wait_ctx *ppe)
{
	TAILQ_INSERT_TAIL(&page->ppe_list, ppe, list_entry);
}

static inline uint64_t
ftl_l2p_cache_get_l1_page_size(void)
{
	return 1UL << 12;
}

static inline uint64_t
ftl_l2p_cache_get_lbas_in_page(struct ftl_l2p_cache *cache)
{
	return cache->lbas_in_page;
}

static inline size_t
ftl_l2p_cache_get_page_all_size(void)
{
	return sizeof(struct ftl_l2p_page) + ftl_l2p_cache_get_l1_page_size();
}

static void
ftl_l2p_cache_lru_remove_page(struct ftl_l2p_cache *cache, struct ftl_l2p_page *page)
{
	assert(page);
	assert(page->on_lru_list);

	TAILQ_REMOVE(&cache->lru_list, page, list_entry);
	page->on_lru_list = false;
}

static void
ftl_l2p_cache_lru_add_page(struct ftl_l2p_cache *cache, struct ftl_l2p_page *page)
{
	assert(page);
	assert(!page->on_lru_list);

	TAILQ_INSERT_HEAD(&cache->lru_list, page, list_entry);

	page->on_lru_list = true;
}

static void
ftl_l2p_cache_lru_promote_page(struct ftl_l2p_cache *cache, struct ftl_l2p_page *page)
{
	if (!page->on_lru_list) {
		return;
	}

	ftl_l2p_cache_lru_remove_page(cache, page);
	ftl_l2p_cache_lru_add_page(cache, page);
}

static inline void
ftl_l2p_cache_page_insert(struct ftl_l2p_cache *cache, struct ftl_l2p_page *page)
{
	struct ftl_l2p_l1_map_entry *me = cache->l2_mapping;
	assert(me);

	assert(me[page->page_no].page_obj_id == FTL_DF_OBJ_ID_INVALID);
	me[page->page_no].page_obj_id = page->obj_id;
}

static void
ftl_l2p_cache_page_remove(struct ftl_l2p_cache *cache, struct ftl_l2p_page *page)
{
	struct ftl_l2p_l1_map_entry *me = cache->l2_mapping;
	assert(me);
	assert(me[page->page_no].page_obj_id != FTL_DF_OBJ_ID_INVALID);
	assert(TAILQ_EMPTY(&page->ppe_list));

	me[page->page_no].page_obj_id = FTL_DF_OBJ_ID_INVALID;
	cache->l2_pgs_avail++;
	ftl_mempool_put(cache->l2_ctx_pool, page);
}

static inline struct ftl_l2p_page *
ftl_l2p_cache_get_coldest_page(struct ftl_l2p_cache *cache)
{
	return TAILQ_LAST(&cache->lru_list, l2p_lru_list);
}

static inline struct ftl_l2p_page *
ftl_l2p_cache_get_hotter_page(struct ftl_l2p_page *page)
{
	return TAILQ_PREV(page, l2p_lru_list, list_entry);
}

static inline uint64_t
ftl_l2p_cache_page_get_bdev_offset(struct ftl_l2p_cache *cache,
				   struct ftl_l2p_page *page)
{
	return cache->cache_layout_offset + page->page_no;
}

static inline struct spdk_bdev_desc *
ftl_l2p_cache_get_bdev_desc(struct ftl_l2p_cache *cache)
{
	return cache->cache_layout_bdev_desc;
}

static inline struct spdk_io_channel *
ftl_l2p_cache_get_bdev_iochannel(struct ftl_l2p_cache *cache)
{
	return cache->cache_layout_ioch;
}

static struct ftl_l2p_page *
ftl_l2p_cache_page_alloc(struct ftl_l2p_cache *cache, size_t page_no)
{
	struct ftl_l2p_page *page = ftl_mempool_get(cache->l2_ctx_pool);
	ftl_bug(!page);

	cache->l2_pgs_avail--;

	memset(page, 0, sizeof(*page));

	page->obj_id = ftl_mempool_get_df_obj_id(cache->l2_ctx_pool, page);

	page->page_buffer = (char *)ftl_md_get_buffer(cache->l1_md) + ftl_mempool_get_df_obj_index(
				    cache->l2_ctx_pool, page) * FTL_BLOCK_SIZE;

	TAILQ_INIT(&page->ppe_list);

	page->page_no = page_no;
	page->state = L2P_CACHE_PAGE_INIT;

	return page;
}

static inline bool
ftl_l2p_cache_page_can_remove(struct ftl_l2p_page *page)
{
	return (!page->updates &&
		page->state != L2P_CACHE_PAGE_INIT &&
		!page->pin_ref_cnt);
}

static inline ftl_addr
ftl_l2p_cache_get_addr(struct spdk_ftl_dev *dev,
		       struct ftl_l2p_cache *cache, struct ftl_l2p_page *page, uint64_t lba)
{
	return ftl_addr_load(dev, page->page_buffer, lba % cache->lbas_in_page);
}

static inline void
ftl_l2p_cache_set_addr(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache,
		       struct ftl_l2p_page *page, uint64_t lba, ftl_addr addr)
{
	ftl_addr_store(dev, page->page_buffer, lba % cache->lbas_in_page, addr);
}

static void
ftl_l2p_page_set_invalid(struct spdk_ftl_dev *dev, struct ftl_l2p_page *page)
{
	ftl_addr addr;
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;
	uint64_t naddr;

	page->updates++;

	naddr = ftl_l2p_cache_get_lbas_in_page(cache);
	for (uint64_t i = 0; i < naddr; i++) {
		addr = ftl_addr_load(dev, page->page_buffer, i);
		if (addr == FTL_ADDR_INVALID) {
			continue;
		}

		ftl_invalidate_addr(dev, addr);
		ftl_l2p_cache_set_addr(dev, cache, page, i, FTL_ADDR_INVALID);
	}
}

static inline void
ftl_l2p_cache_page_pin(struct ftl_l2p_cache *cache, struct ftl_l2p_page *page)
{
	page->pin_ref_cnt++;
	/* Pinned pages can't be evicted (since L2P sets/gets will be executed on it), so remove them from LRU */
	if (page->on_lru_list) {
		ftl_l2p_cache_lru_remove_page(cache, page);
	}
}

static inline void
ftl_l2p_cache_page_unpin(struct ftl_l2p_cache *cache, struct ftl_l2p_page *page)
{
	page->pin_ref_cnt--;
	if (!page->pin_ref_cnt && !page->on_lru_list && page->state != L2P_CACHE_PAGE_FLUSHING) {
		/* L2P_CACHE_PAGE_FLUSHING: the page is currently being evicted.
		 * In such a case, the page can't be returned to the rank list, because
		 * the ongoing eviction will remove it if no pg updates had happened.
		 * Moreover, the page could make it to the top of the rank list and be
		 * selected for another eviction, while the ongoing one did not finish yet.
		 *
		 * Depending on the page updates tracker, the page will be evicted
		 * or returned to the rank list in context of the eviction completion
		 * cb - see page_out_io_complete().
		 */
		ftl_l2p_cache_lru_add_page(cache, page);
	}
}

static inline bool
ftl_l2p_cache_page_can_evict(struct ftl_l2p_page *page)
{
	return (page->state == L2P_CACHE_PAGE_FLUSHING ||
		page->state == L2P_CACHE_PAGE_PERSISTING ||
		page->state == L2P_CACHE_PAGE_INIT ||
		page->pin_ref_cnt) ? false : true;
}

static bool
ftl_l2p_cache_evict_continue(struct ftl_l2p_cache *cache)
{
	return cache->l2_pgs_avail + cache->l2_pgs_evicting < cache->evict_keep;
}

static void *
_ftl_l2p_cache_init(struct spdk_ftl_dev *dev, size_t addr_size, uint64_t l2p_size)
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
				     ftl_md_create_shm_flags(dev), NULL);

	if (cache->l2_md == NULL) {
		goto fail_l2_md;
	}
	cache->l2_mapping = ftl_md_get_buffer(cache->l2_md);

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
	struct ftl_l2p_l1_map_entry *me = cache->l2_mapping;
	ftl_df_obj_id obj_id = me[page_no].page_obj_id;

	if (obj_id != FTL_DF_OBJ_ID_INVALID) {
		return ftl_mempool_get_df_ptr(cache->l2_ctx_pool, obj_id);
	}

	return NULL;
}

int
ftl_l2p_cache_init(struct spdk_ftl_dev *dev)
{
	uint64_t l2p_size = dev->num_lbas * dev->layout.l2p.addr_size;
	struct ftl_l2p_cache *cache;
	const struct ftl_layout_region *reg;
	void *l2p = _ftl_l2p_cache_init(dev, dev->layout.l2p.addr_size, l2p_size);
	size_t page_sets_pool_size = 1 << 15;
	size_t max_resident_size, max_resident_pgs;

	if (!l2p) {
		return -1;
	}
	dev->l2p = l2p;

	cache = (struct ftl_l2p_cache *)dev->l2p;
	cache->page_sets_pool = ftl_mempool_create(page_sets_pool_size,
				sizeof(struct ftl_l2p_page_set),
				64, SPDK_ENV_SOCKET_ID_ANY);
	if (!cache->page_sets_pool) {
		return -1;
	}

	max_resident_size = dev->conf.l2p_dram_limit << 20;
	max_resident_pgs = max_resident_size / ftl_l2p_cache_get_page_all_size();

	if (max_resident_pgs > cache->num_pages) {
		SPDK_NOTICELOG("l2p memory limit higher than entire L2P size\n");
		max_resident_pgs = cache->num_pages;
	}

	/* Round down max res pgs to the nearest # of l2/l1 pgs */
	max_resident_size = max_resident_pgs * ftl_l2p_cache_get_page_all_size();
	SPDK_NOTICELOG("l2p maximum resident size is: %"PRIu64" (of %"PRIu64") MiB\n",
		       max_resident_size >> 20, dev->conf.l2p_dram_limit);

	TAILQ_INIT(&cache->deferred_page_set_list);
	TAILQ_INIT(&cache->lru_list);

	cache->l2_ctx_md = ftl_md_create(dev,
					 spdk_divide_round_up(max_resident_pgs * SPDK_ALIGN_CEIL(sizeof(struct ftl_l2p_page), 64),
							 FTL_BLOCK_SIZE), 0, FTL_L2P_CACHE_MD_NAME_L2_CTX, ftl_md_create_shm_flags(dev), NULL);

	if (cache->l2_ctx_md == NULL) {
		return -1;
	}

	cache->l2_pgs_resident_max = max_resident_pgs;
	cache->l2_pgs_avail = max_resident_pgs;
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
		memset(cache->l2_mapping, (int)FTL_DF_OBJ_ID_INVALID, ftl_md_get_buffer_size(cache->l2_md));
		ftl_mempool_initialize_ext(cache->l2_ctx_pool);
	}

	cache->l1_md = ftl_md_create(dev,
				     max_resident_pgs, 0,
				     FTL_L2P_CACHE_MD_NAME_L1,
				     ftl_md_create_shm_flags(dev), NULL);

	if (cache->l1_md == NULL) {
		return -1;
	}

	/* Cache MD layout */
	reg = &dev->layout.region[FTL_LAYOUT_REGION_TYPE_L2P];
	cache->cache_layout_offset = reg->current.offset;
	cache->cache_layout_bdev_desc = reg->bdev_desc;
	cache->cache_layout_ioch = reg->ioch;

	return 0;
}

static void
ftl_l2p_cache_deinit_l2(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache)
{
	ftl_md_destroy(cache->l2_ctx_md, ftl_md_destroy_shm_flags(dev));
	cache->l2_ctx_md = NULL;

	ftl_mempool_destroy_ext(cache->l2_ctx_pool);
	cache->l2_ctx_pool = NULL;

	ftl_md_destroy(cache->l1_md, ftl_md_destroy_shm_flags(dev));
	cache->l1_md = NULL;

	ftl_mempool_destroy(cache->page_sets_pool);
	cache->page_sets_pool = NULL;
}

static void
_ftl_l2p_cache_deinit(struct spdk_ftl_dev *dev)
{
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;

	ftl_l2p_cache_deinit_l2(dev, cache);
	ftl_md_destroy(cache->l2_md, ftl_md_destroy_shm_flags(dev));
	free(cache);
}

void
ftl_l2p_cache_deinit(struct spdk_ftl_dev *dev)
{
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;

	if (!cache) {
		return;
	}
	assert(cache->state == L2P_CACHE_SHUTDOWN_DONE || cache->state == L2P_CACHE_INIT);

	_ftl_l2p_cache_deinit(dev);
	dev->l2p = 0;
}

static void
process_init_ctx(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache,
		 ftl_l2p_cb cb, void *cb_ctx)
{
	struct ftl_l2p_cache_process_ctx *ctx = &cache->mctx;

	assert(NULL == ctx->cb_ctx);
	assert(0 == cache->l2_pgs_evicting);

	memset(ctx, 0, sizeof(*ctx));

	ctx->cb = cb;
	ctx->cb_ctx = cb_ctx;
}

static void
process_finish(struct ftl_l2p_cache *cache)
{
	struct ftl_l2p_cache_process_ctx ctx = cache->mctx;

	assert(cache->l2_pgs_avail == cache->l2_pgs_resident_max);
	assert(0 == ctx.qd);

	memset(&cache->mctx, 0, sizeof(cache->mctx));
	ctx.cb(cache->dev, ctx.status, ctx.cb_ctx);
}

static void process_page_out_retry(void *_page);
static void process_persist(struct ftl_l2p_cache *cache);

static void
process_page_in(struct ftl_l2p_page *page, spdk_bdev_io_completion_cb cb)
{
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)page->ctx.cache;
	int rc;

	assert(page->page_buffer);

	rc = ftl_nv_cache_bdev_read_blocks_with_md(cache->dev, ftl_l2p_cache_get_bdev_desc(cache),
			ftl_l2p_cache_get_bdev_iochannel(cache),
			page->page_buffer, NULL, ftl_l2p_cache_page_get_bdev_offset(cache, page),
			1, cb, page);

	if (rc) {
		cb(NULL, false, page);
	}
}

static void
process_persist_page_out_cb(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct ftl_l2p_page *page = arg;
	struct ftl_l2p_cache *cache = page->ctx.cache;
	struct spdk_ftl_dev *dev = cache->dev;
	struct ftl_l2p_cache_process_ctx *ctx = &cache->mctx;

	assert(bdev_io);
	ftl_stats_bdev_io_completed(dev, FTL_STATS_TYPE_L2P, bdev_io);
	spdk_bdev_free_io(bdev_io);

	if (!success) {
		ctx->status = -EIO;
	}

	if (ftl_bitmap_get(dev->unmap_map, ctx->idx)) {
		/*
		 * Page had been unmapped, in persist path before IO, it was invalidated entirely
		 * now clear unmap flag
		 */
		ftl_bitmap_clear(dev->unmap_map, page->page_no);
	}
	ftl_l2p_cache_page_remove(cache, page);

	ctx->qd--;
	process_persist(cache);
}

static void
process_page_out(struct ftl_l2p_page *page, spdk_bdev_io_completion_cb cb)
{
	struct spdk_bdev *bdev;
	struct spdk_bdev_io_wait_entry *bdev_io_wait;
	struct ftl_l2p_cache *cache = page->ctx.cache;
	struct spdk_ftl_dev *dev = cache->dev;
	int rc;

	assert(page->page_buffer);

	rc = ftl_nv_cache_bdev_write_blocks_with_md(dev, ftl_l2p_cache_get_bdev_desc(cache),
			ftl_l2p_cache_get_bdev_iochannel(cache),
			page->page_buffer, NULL, ftl_l2p_cache_page_get_bdev_offset(cache, page),
			1, cb, page);

	if (spdk_likely(0 == rc)) {
		return;
	}

	if (rc == -ENOMEM) {
		bdev = spdk_bdev_desc_get_bdev(ftl_l2p_cache_get_bdev_desc(cache));
		bdev_io_wait = &page->ctx.bdev_io_wait;
		bdev_io_wait->bdev = bdev;
		bdev_io_wait->cb_fn = process_page_out_retry;
		bdev_io_wait->cb_arg = page;
		page->ctx.cb = cb;

		rc = spdk_bdev_queue_io_wait(bdev, ftl_l2p_cache_get_bdev_iochannel(cache), bdev_io_wait);
		ftl_bug(rc);
	} else {
		ftl_abort();
	}
}

static void
process_page_out_retry(void *_page)
{
	struct ftl_l2p_page *page = _page;

	process_page_out(page, page->ctx.cb);
}

static void process_unmap(struct ftl_l2p_cache *cache);

static void
process_unmap_page_out_cb(struct spdk_bdev_io *bdev_io, bool success, void *ctx_page)
{
	struct ftl_l2p_page *page = (struct ftl_l2p_page *)ctx_page;
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)page->ctx.cache;
	struct spdk_ftl_dev *dev = cache->dev;
	struct ftl_l2p_cache_process_ctx *ctx = &cache->mctx;

	assert(bdev_io);
	ftl_stats_bdev_io_completed(dev, FTL_STATS_TYPE_L2P, bdev_io);
	spdk_bdev_free_io(bdev_io);

	if (!success) {
		ctx->status = -EIO;
	}

	assert(!page->on_lru_list);
	assert(ftl_bitmap_get(dev->unmap_map, page->page_no));
	ftl_bitmap_clear(dev->unmap_map, page->page_no);
	ftl_l2p_cache_page_remove(cache, page);

	ctx->qd--;
	process_unmap(cache);
}

static void
process_unmap_page_in_cb(struct spdk_bdev_io *bdev_io, bool success, void *ctx_page)
{
	struct ftl_l2p_page *page = (struct ftl_l2p_page *)ctx_page;
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)page->ctx.cache;
	struct spdk_ftl_dev *dev = cache->dev;
	struct ftl_l2p_cache_process_ctx *ctx = &cache->mctx;

	if (bdev_io) {
		ftl_stats_bdev_io_completed(dev, FTL_STATS_TYPE_L2P, bdev_io);
		spdk_bdev_free_io(bdev_io);
	}
	if (success) {
		assert(ftl_bitmap_get(dev->unmap_map, page->page_no));
		ftl_l2p_page_set_invalid(dev, page);
		process_page_out(page, process_unmap_page_out_cb);
	} else {
		ctx->status = -EIO;
		ctx->qd--;
		process_unmap(cache);
	}
}

static void
process_unmap(struct ftl_l2p_cache *cache)
{
	struct ftl_l2p_cache_process_ctx *ctx = &cache->mctx;

	while (ctx->idx < cache->num_pages && ctx->qd < 64) {
		struct ftl_l2p_page *page;

		if (!ftl_bitmap_get(cache->dev->unmap_map, ctx->idx)) {
			/* Page had not been unmapped, continue */
			ctx->idx++;
			continue;
		}

		/* All pages were removed in persist phase */
		assert(get_l2p_page_by_df_id(cache, ctx->idx) == NULL);

		/* Allocate page to invalidate it */
		page = ftl_l2p_cache_page_alloc(cache, ctx->idx);
		if (!page) {
			/* All pages utilized so far, continue when they will be back available */
			assert(ctx->qd);
			break;
		}

		page->state = L2P_CACHE_PAGE_CLEARING;
		page->ctx.cache = cache;

		ftl_l2p_cache_page_insert(cache, page);
		process_page_in(page, process_unmap_page_in_cb);

		ctx->qd++;
		ctx->idx++;
	}

	if (0 == ctx->qd) {
		process_finish(cache);
	}
}

void
ftl_l2p_cache_unmap(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;

	process_init_ctx(dev, cache, cb, cb_ctx);
	process_unmap(cache);
}

static void
clear_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	ftl_l2p_cb cb = md->owner.private;
	void *cb_cntx = md->owner.cb_ctx;

	cb(dev, status, cb_cntx);
}

void
ftl_l2p_cache_clear(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_L2P];
	ftl_addr invalid_addr = FTL_ADDR_INVALID;

	md->cb =  clear_cb;
	md->owner.cb_ctx = cb_ctx;
	md->owner.private = cb;

	ftl_md_clear(md, invalid_addr, NULL);
}

static void
l2p_shm_restore_clean(struct spdk_ftl_dev *dev)
{
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;
	struct ftl_l2p_l1_map_entry *me = cache->l2_mapping;
	struct ftl_l2p_page *page;
	ftl_df_obj_id obj_id;
	uint64_t page_no;

	for (page_no = 0; page_no < cache->num_pages; ++page_no) {
		obj_id = me[page_no].page_obj_id;
		if (obj_id == FTL_DF_OBJ_ID_INVALID) {
			continue;
		}

		page = ftl_mempool_claim_df(cache->l2_ctx_pool, obj_id);
		assert(page);
		assert(page->obj_id == ftl_mempool_get_df_obj_id(cache->l2_ctx_pool, page));
		assert(page->page_no == page_no);
		assert(page->state != L2P_CACHE_PAGE_INIT);
		assert(page->state != L2P_CACHE_PAGE_CLEARING);
		assert(cache->l2_pgs_avail > 0);
		cache->l2_pgs_avail--;

		page->page_buffer = (char *)ftl_md_get_buffer(cache->l1_md) + ftl_mempool_get_df_obj_index(
					    cache->l2_ctx_pool, page) * FTL_BLOCK_SIZE;

		TAILQ_INIT(&page->ppe_list);

		page->pin_ref_cnt = 0;
		page->on_lru_list = 0;
		memset(&page->ctx, 0, sizeof(page->ctx));

		ftl_l2p_cache_lru_add_page(cache, page);
	}

	ftl_mempool_initialize_ext(cache->l2_ctx_pool);
}

static void
l2p_shm_restore_dirty(struct spdk_ftl_dev *dev)
{
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;
	struct ftl_l2p_l1_map_entry *me = cache->l2_mapping;
	struct ftl_l2p_page *page;
	ftl_df_obj_id obj_id;
	uint64_t page_no;

	for (page_no = 0; page_no < cache->num_pages; ++page_no) {
		obj_id = me[page_no].page_obj_id;
		if (obj_id == FTL_DF_OBJ_ID_INVALID) {
			continue;
		}

		page = ftl_mempool_claim_df(cache->l2_ctx_pool, obj_id);
		assert(page);
		assert(page->obj_id == ftl_mempool_get_df_obj_id(cache->l2_ctx_pool, page));
		assert(page->page_no == page_no);
		assert(page->state != L2P_CACHE_PAGE_CLEARING);
		assert(cache->l2_pgs_avail > 0);
		cache->l2_pgs_avail--;

		if (page->state == L2P_CACHE_PAGE_INIT) {
			me[page_no].page_obj_id = FTL_DF_OBJ_ID_INVALID;
			cache->l2_pgs_avail++;
			ftl_mempool_release_df(cache->l2_ctx_pool, obj_id);
			continue;
		}

		page->state = L2P_CACHE_PAGE_READY;
		/* Assume page is dirty after crash */
		page->updates = 1;
		page->page_buffer = (char *)ftl_md_get_buffer(cache->l1_md) + ftl_mempool_get_df_obj_index(
					    cache->l2_ctx_pool, page) * FTL_BLOCK_SIZE;

		TAILQ_INIT(&page->ppe_list);

		page->pin_ref_cnt = 0;
		page->on_lru_list = 0;
		memset(&page->ctx, 0, sizeof(page->ctx));

		ftl_l2p_cache_lru_add_page(cache, page);
	}

	ftl_mempool_initialize_ext(cache->l2_ctx_pool);
}

void
ftl_l2p_cache_restore(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	if (ftl_fast_startup(dev)) {
		l2p_shm_restore_clean(dev);
	}

	if (ftl_fast_recovery(dev)) {
		l2p_shm_restore_dirty(dev);
	}

	cb(dev, 0, cb_ctx);
}

static void
process_persist(struct ftl_l2p_cache *cache)
{
	struct ftl_l2p_cache_process_ctx *ctx = &cache->mctx;
	struct spdk_ftl_dev *dev = cache->dev;

	while (ctx->idx < cache->num_pages && ctx->qd < 64) {
		struct ftl_l2p_page *page = get_l2p_page_by_df_id(cache, ctx->idx);
		ctx->idx++;

		if (!page) {
			continue;
		}

		/* Finished unmap if the page was marked */
		if (ftl_bitmap_get(dev->unmap_map, ctx->idx)) {
			ftl_l2p_page_set_invalid(dev, page);
		}

		if (page->on_lru_list) {
			ftl_l2p_cache_lru_remove_page(cache, page);
		}

		if (page->updates) {
			/* Need to persist the page */
			page->state = L2P_CACHE_PAGE_PERSISTING;
			page->ctx.cache = cache;
			ctx->qd++;
			process_page_out(page, process_persist_page_out_cb);
		} else {
			ftl_l2p_cache_page_remove(cache, page);
		}
	}

	if (0 == ctx->qd) {
		process_finish(cache);
	}
}

void
ftl_l2p_cache_persist(struct spdk_ftl_dev *dev, ftl_l2p_cb cb, void *cb_ctx)
{
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;

	process_init_ctx(dev, cache, cb, cb_ctx);
	process_persist(cache);
}

bool
ftl_l2p_cache_is_halted(struct spdk_ftl_dev *dev)
{
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;

	return cache->state == L2P_CACHE_SHUTDOWN_DONE;
}

void
ftl_l2p_cache_halt(struct spdk_ftl_dev *dev)
{
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;

	if (cache->state != L2P_CACHE_SHUTDOWN_DONE) {
		cache->state = L2P_CACHE_IN_SHUTDOWN;
		if (!cache->ios_in_flight && !cache->l2_pgs_evicting) {
			cache->state = L2P_CACHE_SHUTDOWN_DONE;
		}
	}
}

void
ftl_l2p_cache_resume(struct spdk_ftl_dev *dev)
{
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;

	assert(cache->state == L2P_CACHE_INIT);
	cache->state = L2P_CACHE_RUNNING;
}

static inline struct ftl_l2p_page *
get_page(struct ftl_l2p_cache *cache, uint64_t lba)
{
	return get_l2p_page_by_df_id(cache, lba / cache->lbas_in_page);
}

static inline void
ftl_l2p_cache_init_page_set(struct ftl_l2p_page_set *page_set, struct ftl_l2p_pin_ctx *pin_ctx)
{
	page_set->to_pin_cnt = 0;
	page_set->pinned_cnt = 0;
	page_set->pin_fault_cnt = 0;
	page_set->locked = 0;
	page_set->deferred = 0;
	page_set->pin_ctx = pin_ctx;
}

static inline bool
ftl_l2p_cache_running(struct ftl_l2p_cache *cache)
{
	return cache->state == L2P_CACHE_RUNNING;
}

static inline bool
ftl_l2p_cache_page_is_pinnable(struct ftl_l2p_page *page)
{
	return page->state != L2P_CACHE_PAGE_INIT;
}

void
ftl_l2p_cache_pin(struct spdk_ftl_dev *dev, struct ftl_l2p_pin_ctx *pin_ctx)
{
	assert(dev->num_lbas >= pin_ctx->lba + pin_ctx->count);
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;
	struct ftl_l2p_page_set *page_set;
	bool defer_pin = false;

	/* Calculate first and last page to pin, count of them */
	uint64_t start = pin_ctx->lba / cache->lbas_in_page;
	uint64_t end = (pin_ctx->lba + pin_ctx->count - 1) / cache->lbas_in_page;
	uint64_t count = end - start + 1;
	uint64_t i;

	if (spdk_unlikely(count > L2P_MAX_PAGES_TO_PIN)) {
		ftl_l2p_pin_complete(dev, -E2BIG, pin_ctx);
		return;
	}

	/* Get and initialize page sets */
	assert(ftl_l2p_cache_running(cache));
	page_set = ftl_mempool_get(cache->page_sets_pool);
	if (!page_set) {
		ftl_l2p_pin_complete(dev, -EAGAIN, pin_ctx);
		return;
	}
	ftl_l2p_cache_init_page_set(page_set, pin_ctx);

	struct ftl_l2p_page_wait_ctx *entry = page_set->entry;
	for (i = start; i <= end; i++, entry++) {
		struct ftl_l2p_page *page;
		entry->parent = page_set;
		entry->pg_no = i;
		entry->pg_pin_completed = false;
		entry->pg_pin_issued = false;

		page_set->to_pin_cnt++;

		/* Try get page and pin */
		page = get_l2p_page_by_df_id(cache, i);
		if (page) {
			if (ftl_l2p_cache_page_is_pinnable(page)) {
				/* Page available and we can pin it */
				page_set->pinned_cnt++;
				entry->pg_pin_issued = true;
				entry->pg_pin_completed = true;
				ftl_l2p_cache_page_pin(cache, page);
			} else {
				/* The page is being loaded */
				/* Queue the page pin entry to be executed on page in */
				ftl_l2p_page_queue_wait_ctx(page, entry);
				entry->pg_pin_issued = true;
			}
		} else {
			/* The page is not in the cache, queue the page_set to page in */
			defer_pin = true;
		}
	}

	/* Check if page set is done */
	if (page_set_is_done(page_set)) {
		page_set_end(dev, cache, page_set);
	} else if (defer_pin) {
		TAILQ_INSERT_TAIL(&cache->deferred_page_set_list, page_set, list_entry);
		page_set->deferred = 1;
	}
}

void
ftl_l2p_cache_unpin(struct spdk_ftl_dev *dev, uint64_t lba, uint64_t count)
{
	assert(dev->num_lbas >= lba + count);
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;
	struct ftl_l2p_page *page;
	uint64_t start = lba / cache->lbas_in_page;
	uint64_t end = (lba + count - 1) / cache->lbas_in_page;
	uint64_t i;

	assert(count);
	assert(start < cache->num_pages);
	assert(end < cache->num_pages);

	for (i = start; i <= end; i++) {
		page = get_l2p_page_by_df_id(cache, i);
		ftl_bug(!page);
		ftl_l2p_cache_page_unpin(cache, page);
	}
}

ftl_addr
ftl_l2p_cache_get(struct spdk_ftl_dev *dev, uint64_t lba)
{
	assert(dev->num_lbas > lba);
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;
	struct ftl_l2p_page *page = get_page(cache, lba);
	ftl_addr addr;

	ftl_bug(!page);
	assert(ftl_l2p_cache_running(cache));
	assert(page->pin_ref_cnt);

	if (ftl_bitmap_get(dev->unmap_map, page->page_no)) {
		ftl_l2p_page_set_invalid(dev, page);
		ftl_bitmap_clear(dev->unmap_map, page->page_no);
	}

	ftl_l2p_cache_lru_promote_page(cache, page);
	addr = ftl_l2p_cache_get_addr(dev, cache, page, lba);

	return addr;
}

void
ftl_l2p_cache_set(struct spdk_ftl_dev *dev, uint64_t lba, ftl_addr addr)
{
	assert(dev->num_lbas > lba);
	struct ftl_l2p_cache *cache = (struct ftl_l2p_cache *)dev->l2p;
	struct ftl_l2p_page *page = get_page(cache, lba);

	ftl_bug(!page);
	assert(ftl_l2p_cache_running(cache));
	assert(page->pin_ref_cnt);

	if (ftl_bitmap_get(dev->unmap_map, page->page_no)) {
		ftl_l2p_page_set_invalid(dev, page);
		ftl_bitmap_clear(dev->unmap_map, page->page_no);
	}

	page->updates++;
	ftl_l2p_cache_lru_promote_page(cache, page);
	ftl_l2p_cache_set_addr(dev, cache, page, lba, addr);
}

static struct ftl_l2p_page *
page_allocate(struct ftl_l2p_cache *cache, uint64_t page_no)
{
	struct ftl_l2p_page *page = ftl_l2p_cache_page_alloc(cache, page_no);
	ftl_l2p_cache_page_insert(cache, page);

	return page;
}

static bool
page_set_is_done(struct ftl_l2p_page_set *page_set)
{
	if (page_set->locked) {
		return false;
	}

	assert(page_set->pinned_cnt + page_set->pin_fault_cnt <= page_set->to_pin_cnt);
	return (page_set->pinned_cnt + page_set->pin_fault_cnt == page_set->to_pin_cnt);
}

static void
page_set_unpin(struct ftl_l2p_cache *cache, struct ftl_l2p_page_set *page_set)
{
	uint64_t i;
	struct ftl_l2p_page_wait_ctx *pentry = page_set->entry;

	for (i = 0; i < page_set->to_pin_cnt; i++, pentry++) {
		struct ftl_l2p_page *pinned_page;

		if (false == pentry->pg_pin_completed) {
			continue;
		}

		pinned_page = get_l2p_page_by_df_id(cache, pentry->pg_no);
		ftl_bug(!pinned_page);

		ftl_l2p_cache_page_unpin(cache, pinned_page);
	}
}

static void
page_set_end(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache,
	     struct ftl_l2p_page_set *page_set)
{
	if (spdk_likely(0 == page_set->pin_fault_cnt)) {
		ftl_l2p_pin_complete(dev, 0, page_set->pin_ctx);
	} else {
		page_set_unpin(cache, page_set);
		ftl_l2p_pin_complete(dev, -EIO, page_set->pin_ctx);
	}

	if (page_set->deferred) {
		TAILQ_REMOVE(&cache->deferred_page_set_list, page_set, list_entry);
	}

	assert(0 == page_set->locked);
	ftl_mempool_put(cache->page_sets_pool, page_set);
}

static void
page_in_io_complete(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache,
		    struct ftl_l2p_page *page, bool success)
{
	struct ftl_l2p_page_set *page_set;
	struct ftl_l2p_page_wait_ctx *pentry;

	cache->ios_in_flight--;

	assert(0 == page->pin_ref_cnt);
	assert(L2P_CACHE_PAGE_INIT == page->state);
	assert(false == page->on_lru_list);

	if (spdk_likely(success)) {
		page->state = L2P_CACHE_PAGE_READY;
	}

	while ((pentry = TAILQ_FIRST(&page->ppe_list))) {
		TAILQ_REMOVE(&page->ppe_list, pentry, list_entry);

		page_set = pentry->parent;

		assert(false == pentry->pg_pin_completed);

		if (success) {
			ftl_l2p_cache_page_pin(cache, page);
			page_set->pinned_cnt++;
			pentry->pg_pin_completed = true;
		} else {
			page_set->pin_fault_cnt++;
		}

		/* Check if page_set is done */
		if (page_set_is_done(page_set)) {
			page_set_end(dev, cache, page_set);
		}
	}

	if (spdk_unlikely(!success)) {
		ftl_bug(page->on_lru_list);
		ftl_l2p_cache_page_remove(cache, page);
	}
}

static void
page_in_io_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_l2p_page *page = cb_arg;
	struct ftl_l2p_cache *cache = page->ctx.cache;
	struct spdk_ftl_dev *dev = cache->dev;

	ftl_stats_bdev_io_completed(dev, FTL_STATS_TYPE_L2P, bdev_io);
	spdk_bdev_free_io(bdev_io);
	page_in_io_complete(dev, cache, page, success);
}

static void
page_in_io(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache, struct ftl_l2p_page *page)
{
	struct spdk_io_channel *ioch;
	struct spdk_bdev *bdev;
	struct spdk_bdev_io_wait_entry *bdev_io_wait;
	int rc;
	page->ctx.cache = cache;

	rc = ftl_nv_cache_bdev_read_blocks_with_md(cache->dev, ftl_l2p_cache_get_bdev_desc(cache),
			ftl_l2p_cache_get_bdev_iochannel(cache),
			page->page_buffer, NULL, ftl_l2p_cache_page_get_bdev_offset(cache, page),
			1, page_in_io_cb, page);
	cache->ios_in_flight++;
	if (spdk_likely(0 == rc)) {
		return;
	}

	if (rc == -ENOMEM) {
		ioch = ftl_l2p_cache_get_bdev_iochannel(cache);
		bdev = spdk_bdev_desc_get_bdev(ftl_l2p_cache_get_bdev_desc(cache));
		bdev_io_wait = &page->ctx.bdev_io_wait;
		bdev_io_wait->bdev = bdev;
		bdev_io_wait->cb_fn = page_in_io_retry;
		bdev_io_wait->cb_arg = page;

		rc = spdk_bdev_queue_io_wait(bdev, ioch, bdev_io_wait);
		ftl_bug(rc);
	} else {
		ftl_abort();
	}
}

static void
page_in_io_retry(void *arg)
{
	struct ftl_l2p_page *page = arg;
	struct ftl_l2p_cache *cache = page->ctx.cache;
	struct spdk_ftl_dev *dev = cache->dev;

	cache->ios_in_flight--;
	page_in_io(dev, cache, page);
}

static void
page_in(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache,
	struct ftl_l2p_page_set *page_set, struct ftl_l2p_page_wait_ctx *pentry)
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
		page_set->pinned_cnt++;
		pentry->pg_pin_issued = true;
		pentry->pg_pin_completed = true;
	} else {
		pentry->pg_pin_issued = true;
		ftl_l2p_page_queue_wait_ctx(page, pentry);
	}

	if (page_in) {
		page_in_io(dev, cache, page);
	}
}

static int
ftl_l2p_cache_process_page_sets(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache)
{
	struct ftl_l2p_page_set *page_set;
	struct ftl_l2p_page_wait_ctx *pentry;
	uint64_t i;

	page_set = TAILQ_FIRST(&cache->deferred_page_set_list);
	if (!page_set) {
		/* No page_set */
		return -ECHILD;
	}

	if (page_set->to_pin_cnt > cache->l2_pgs_avail) {
		/* No enough page to pin, wait */
		return -EBUSY;
	}
	if (cache->ios_in_flight > 512) {
		/* Too big QD */
		return -EBUSY;
	}

	ftl_add_io_activity(dev);

	TAILQ_REMOVE(&cache->deferred_page_set_list, page_set, list_entry);
	page_set->deferred = 0;
	page_set->locked = 1;

	/* Now we can start pinning */
	pentry = page_set->entry;
	for (i = 0; i < page_set->to_pin_cnt; i++, pentry++) {
		if (!pentry->pg_pin_issued) {
			page_in(dev, cache, page_set, pentry);
		}
	}

	page_set->locked = 0;

	/* Check if page_set is done */
	if (page_set_is_done(page_set)) {
		page_set_end(dev, cache, page_set);
	}

	return 0;
}

static struct ftl_l2p_page *
eviction_get_page(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache)
{
	uint64_t i = 0;
	struct ftl_l2p_page *page = ftl_l2p_cache_get_coldest_page(cache);

	while (page) {
		ftl_bug(L2P_CACHE_PAGE_READY != page->state);
		ftl_bug(page->pin_ref_cnt);

		if (ftl_l2p_cache_page_can_evict(page)) {
			ftl_l2p_cache_lru_remove_page(cache, page);
			return page;
		}

		/*
		 * Practically only one iteration is needed to find a page. It is because
		 * the rank of pages contains only ready and unpinned pages
		 */
		ftl_bug(++i > 1024);

		page = ftl_l2p_cache_get_hotter_page(page);
	}

	return NULL;
}

static void
page_out_io_complete(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache,
		     struct ftl_l2p_page *page, bool success)
{
	cache->l2_pgs_evicting--;

	ftl_bug(page->ctx.updates > page->updates);
	ftl_bug(!TAILQ_EMPTY(&page->ppe_list));
	ftl_bug(page->on_lru_list);

	if (spdk_likely(success)) {
		page->updates -= page->ctx.updates;
	}

	if (success && ftl_l2p_cache_page_can_remove(page)) {
		ftl_l2p_cache_page_remove(cache, page);
	} else {
		if (!page->pin_ref_cnt) {
			ftl_l2p_cache_lru_add_page(cache, page);
		}
		page->state = L2P_CACHE_PAGE_READY;
	}
}

static void
page_out_io_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_l2p_page *page = cb_arg;
	struct ftl_l2p_cache *cache = page->ctx.cache;
	struct spdk_ftl_dev *dev = cache->dev;

	ftl_stats_bdev_io_completed(dev, FTL_STATS_TYPE_L2P, bdev_io);
	spdk_bdev_free_io(bdev_io);
	page_out_io_complete(dev, cache, page, success);
}

static void
page_out_io(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache,
	    struct ftl_l2p_page *page)
{
	struct spdk_io_channel *ioch;
	struct spdk_bdev *bdev;
	struct spdk_bdev_io_wait_entry *bdev_io_wait;
	int rc;

	page->ctx.cache = cache;

	rc = ftl_nv_cache_bdev_write_blocks_with_md(dev, ftl_l2p_cache_get_bdev_desc(cache),
			ftl_l2p_cache_get_bdev_iochannel(cache),
			page->page_buffer, NULL, ftl_l2p_cache_page_get_bdev_offset(cache, page),
			1, page_out_io_cb, page);

	cache->l2_pgs_evicting++;
	if (spdk_likely(0 == rc)) {
		return;
	}

	if (rc == -ENOMEM) {
		ioch = ftl_l2p_cache_get_bdev_iochannel(cache);
		bdev = spdk_bdev_desc_get_bdev(ftl_l2p_cache_get_bdev_desc(cache));
		bdev_io_wait = &page->ctx.bdev_io_wait;
		bdev_io_wait->bdev = bdev;
		bdev_io_wait->cb_fn = page_out_io_retry;
		bdev_io_wait->cb_arg = page;

		rc = spdk_bdev_queue_io_wait(bdev, ioch, bdev_io_wait);
		ftl_bug(rc);
	} else {
		ftl_abort();
	}
}

static void
page_out_io_retry(void *arg)
{
	struct ftl_l2p_page *page = arg;
	struct ftl_l2p_cache *cache = page->ctx.cache;
	struct spdk_ftl_dev *dev = cache->dev;

	cache->l2_pgs_evicting--;
	page_out_io(dev, cache, page);
}

static void
ftl_l2p_cache_process_eviction(struct spdk_ftl_dev *dev, struct ftl_l2p_cache *cache)
{
	struct ftl_l2p_page *page;

	if (!ftl_l2p_cache_evict_continue(cache)) {
		return;
	}

	if (cache->l2_pgs_evicting > 512) {
		return;
	}

	ftl_add_io_activity(dev);

	page = eviction_get_page(dev, cache);
	if (spdk_unlikely(!page)) {
		return;
	}

	if (page->updates) {
		page->state = L2P_CACHE_PAGE_FLUSHING;
		page->ctx.updates = page->updates;
		page_out_io(dev, cache, page);
	} else {
		/* Page clean and we can remove it */
		ftl_l2p_cache_page_remove(cache, page);
	}
}

static void
ftl_l2p_lazy_unmap_process_cb(struct spdk_ftl_dev *dev, int status, struct ftl_l2p_pin_ctx *pin_ctx)
{
	struct ftl_l2p_cache *cache = dev->l2p;

	cache->lazy_unmap.qd--;

	/* We will retry on next ftl_l2p_lazy_unmap_process */
	if (spdk_unlikely(status != 0)) {
		return;
	}

	if (ftl_l2p_cache_running(cache)) {
		ftl_l2p_cache_get(dev, pin_ctx->lba);
	}

	ftl_l2p_cache_unpin(dev, pin_ctx->lba, pin_ctx->count);
}

static void
ftl_l2p_lazy_unmap_process(struct spdk_ftl_dev *dev)
{
	struct ftl_l2p_cache *cache = dev->l2p;
	struct ftl_l2p_pin_ctx *pin_ctx;
	uint64_t page_no;

	if (spdk_likely(!dev->unmap_in_progress)) {
		return;
	}

	if (cache->lazy_unmap.qd == FTL_L2P_MAX_LAZY_UNMAP_QD) {
		return;
	}

	page_no = ftl_bitmap_find_first_set(dev->unmap_map, cache->lazy_unmap.page_no, UINT64_MAX);
	if (page_no == UINT64_MAX) {
		cache->lazy_unmap.page_no = 0;

		/* Check unmap map from beginning to detect unprocessed unmaps */
		page_no = ftl_bitmap_find_first_set(dev->unmap_map, cache->lazy_unmap.page_no, UINT64_MAX);
		if (page_no == UINT64_MAX) {
			dev->unmap_in_progress = false;
			return;
		}
	}

	cache->lazy_unmap.page_no = page_no;

	pin_ctx = &cache->lazy_unmap.pin_ctx;

	cache->lazy_unmap.qd++;
	assert(cache->lazy_unmap.qd <= FTL_L2P_MAX_LAZY_UNMAP_QD);
	assert(page_no < cache->num_pages);

	pin_ctx->lba = page_no * cache->lbas_in_page;
	pin_ctx->count = 1;
	pin_ctx->cb = ftl_l2p_lazy_unmap_process_cb;
	pin_ctx->cb_ctx = pin_ctx;

	ftl_l2p_cache_pin(dev, pin_ctx);
}

void
ftl_l2p_cache_process(struct spdk_ftl_dev *dev)
{
	struct ftl_l2p_cache *cache = dev->l2p;
	int i;

	if (spdk_unlikely(cache->state != L2P_CACHE_RUNNING)) {
		return;
	}

	for (i = 0; i < 256; i++) {
		if (ftl_l2p_cache_process_page_sets(dev, cache)) {
			break;
		}
	}

	ftl_l2p_cache_process_eviction(dev, cache);
	ftl_l2p_lazy_unmap_process(dev);
}
