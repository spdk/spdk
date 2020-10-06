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
#include "spdk/nvme.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/ftl.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/bdev_zone.h"
#include "spdk/bdev_module.h"
#include "spdk/config.h"

#include "ftl_core.h"
#include "ftl_io.h"
#include "ftl_reloc.h"
#include "ftl_band.h"
#include "ftl_debug.h"

#ifdef SPDK_CONFIG_PMDK
#include "libpmem.h"
#endif /* SPDK_CONFIG_PMDK */

#define FTL_CORE_RING_SIZE	4096
#define FTL_INIT_TIMEOUT	30
#define FTL_NSID		1
#define FTL_ZONE_INFO_COUNT	64

/* Dummy bdev module used to to claim bdevs. */
static struct spdk_bdev_module g_ftl_bdev_module = {
	.name	= "ftl_lib",
};

struct ftl_dev_init_ctx {
	/* Owner */
	struct spdk_ftl_dev		*dev;
	/* Initial arguments */
	struct spdk_ftl_dev_init_opts	opts;
	/* IO channel for zone info retrieving */
	struct spdk_io_channel		*ioch;
	/* Buffer for reading zone info  */
	struct spdk_bdev_zone_info	info[FTL_ZONE_INFO_COUNT];
	/* Currently read zone */
	size_t				zone_id;
	/* User's callback */
	spdk_ftl_init_fn		cb_fn;
	/* Callback's argument */
	void				*cb_arg;
	/* Thread to call the callback on */
	struct spdk_thread		*thread;
	/* Poller to check if the device has been destroyed/initialized */
	struct spdk_poller		*poller;
	/* Status to return for halt completion callback */
	int				halt_complete_status;
};

static STAILQ_HEAD(, spdk_ftl_dev)	g_ftl_queue = STAILQ_HEAD_INITIALIZER(g_ftl_queue);
static pthread_mutex_t			g_ftl_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static const struct spdk_ftl_conf	g_default_conf = {
	.limits = {
		/* 5 free bands  / 0 % host writes */
		[SPDK_FTL_LIMIT_CRIT]  = { .thld = 5,  .limit = 0 },
		/* 10 free bands / 5 % host writes */
		[SPDK_FTL_LIMIT_HIGH]  = { .thld = 10, .limit = 5 },
		/* 20 free bands / 40 % host writes */
		[SPDK_FTL_LIMIT_LOW]   = { .thld = 20, .limit = 40 },
		/* 40 free bands / 100 % host writes - defrag starts running */
		[SPDK_FTL_LIMIT_START] = { .thld = 40, .limit = 100 },
	},
	/* 10 percent valid blocks */
	.invalid_thld = 10,
	/* 20% spare blocks */
	.lba_rsvd = 20,
	/* 6M write buffer per each IO channel */
	.write_buffer_size = 6 * 1024 * 1024,
	/* 90% band fill threshold */
	.band_thld = 90,
	/* Max 32 IO depth per band relocate */
	.max_reloc_qdepth = 32,
	/* Max 3 active band relocates */
	.max_active_relocs = 3,
	/* IO pool size per user thread (this should be adjusted to thread IO qdepth) */
	.user_io_pool_size = 2048,
	/*
	 * If clear ftl will return error when restoring after a dirty shutdown
	 * If set, last band will be padded, ftl will restore based only on closed bands - this
	 * will result in lost data after recovery.
	 */
	.allow_open_bands = false,
	.max_io_channels = 128,
	.nv_cache = {
		/* Maximum number of concurrent requests */
		.max_request_cnt = 2048,
		/* Maximum number of blocks per request */
		.max_request_size = 16,
	}
};

static int
ftl_band_init_md(struct ftl_band *band)
{
	struct ftl_lba_map *lba_map = &band->lba_map;
	int rc;

	lba_map->vld = spdk_bit_array_create(ftl_get_num_blocks_in_band(band->dev));
	if (!lba_map->vld) {
		return -ENOMEM;
	}

	rc = pthread_spin_init(&lba_map->lock, PTHREAD_PROCESS_PRIVATE);
	if (rc) {
		spdk_bit_array_free(&lba_map->vld);
		return rc;
	}
	ftl_band_md_clear(band);
	return 0;
}

static int
ftl_check_conf(const struct spdk_ftl_dev *dev, const struct spdk_ftl_conf *conf)
{
	size_t i;

	if (conf->invalid_thld >= 100) {
		return -1;
	}
	if (conf->lba_rsvd >= 100) {
		return -1;
	}
	if (conf->lba_rsvd == 0) {
		return -1;
	}
	if (conf->write_buffer_size == 0) {
		return -1;
	}
	if (conf->write_buffer_size % FTL_BLOCK_SIZE != 0) {
		return -1;
	}

	for (i = 0; i < SPDK_FTL_LIMIT_MAX; ++i) {
		if (conf->limits[i].limit > 100) {
			return -1;
		}
	}

	return 0;
}

static int
ftl_dev_init_bands(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band, *pband;
	unsigned int i;
	int rc = 0;

	LIST_INIT(&dev->free_bands);
	LIST_INIT(&dev->shut_bands);

	dev->num_free = 0;
	dev->bands = calloc(ftl_get_num_bands(dev), sizeof(*dev->bands));
	if (!dev->bands) {
		return -1;
	}

	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		band = &dev->bands[i];
		band->id = i;
		band->dev = dev;
		band->state = FTL_BAND_STATE_CLOSED;

		if (LIST_EMPTY(&dev->shut_bands)) {
			LIST_INSERT_HEAD(&dev->shut_bands, band, list_entry);
		} else {
			LIST_INSERT_AFTER(pband, band, list_entry);
		}
		pband = band;

		CIRCLEQ_INIT(&band->zones);
		band->zone_buf = calloc(ftl_get_num_punits(dev), sizeof(*band->zone_buf));
		if (!band->zone_buf) {
			SPDK_ERRLOG("Failed to allocate block state table for band: [%u]\n", i);
			rc = -1;
			break;
		}

		rc = ftl_band_init_md(band);
		if (rc) {
			SPDK_ERRLOG("Failed to initialize metadata structures for band [%u]\n", i);
			break;
		}

		band->reloc_bitmap = spdk_bit_array_create(ftl_get_num_bands(dev));
		if (!band->reloc_bitmap) {
			SPDK_ERRLOG("Failed to allocate band relocation bitmap\n");
			break;
		}
	}

	return rc;
}

static void
ftl_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	struct spdk_ftl_dev *dev = event_ctx;

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		assert(0);
		break;
	case SPDK_BDEV_EVENT_MEDIA_MANAGEMENT:
		assert(bdev == spdk_bdev_desc_get_bdev(dev->base_bdev_desc));
		ftl_get_media_events(dev);
	default:
		break;
	}
}

static int
ftl_dev_init_nv_cache(struct spdk_ftl_dev *dev, const char *bdev_name)
{
	struct spdk_bdev *bdev;
	struct spdk_ftl_conf *conf = &dev->conf;
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;
	char pool_name[128];
	int rc;

	if (!bdev_name) {
		return 0;
	}

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (!bdev) {
		SPDK_ERRLOG("Unable to find bdev: %s\n", bdev_name);
		return -1;
	}

	if (spdk_bdev_open_ext(bdev_name, true, ftl_bdev_event_cb,
			       dev, &nv_cache->bdev_desc)) {
		SPDK_ERRLOG("Unable to open bdev: %s\n", bdev_name);
		return -1;
	}

	if (spdk_bdev_module_claim_bdev(bdev, nv_cache->bdev_desc, &g_ftl_bdev_module)) {
		spdk_bdev_close(nv_cache->bdev_desc);
		nv_cache->bdev_desc = NULL;
		SPDK_ERRLOG("Unable to claim bdev %s\n", bdev_name);
		return -1;
	}

	SPDK_INFOLOG(ftl_init, "Using %s as write buffer cache\n",
		     spdk_bdev_get_name(bdev));

	if (spdk_bdev_get_block_size(bdev) != FTL_BLOCK_SIZE) {
		SPDK_ERRLOG("Unsupported block size (%d)\n", spdk_bdev_get_block_size(bdev));
		return -1;
	}

	if (!spdk_bdev_is_md_separate(bdev)) {
		SPDK_ERRLOG("Bdev %s doesn't support separate metadata buffer IO\n",
			    spdk_bdev_get_name(bdev));
		return -1;
	}

	if (spdk_bdev_get_md_size(bdev) < sizeof(uint64_t)) {
		SPDK_ERRLOG("Bdev's %s metadata is too small (%"PRIu32")\n",
			    spdk_bdev_get_name(bdev), spdk_bdev_get_md_size(bdev));
		return -1;
	}

	if (spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE) {
		SPDK_ERRLOG("Unsupported DIF type used by bdev %s\n",
			    spdk_bdev_get_name(bdev));
		return -1;
	}

	/* The cache needs to be capable of storing at least two full bands. This requirement comes
	 * from the fact that cache works as a protection against power loss, so before the data
	 * inside the cache can be overwritten, the band it's stored on has to be closed. Plus one
	 * extra block is needed to store the header.
	 */
	if (spdk_bdev_get_num_blocks(bdev) < ftl_get_num_blocks_in_band(dev) * 2 + 1) {
		SPDK_ERRLOG("Insufficient number of blocks for write buffer cache (available: %"
			    PRIu64", required: %"PRIu64")\n", spdk_bdev_get_num_blocks(bdev),
			    ftl_get_num_blocks_in_band(dev) * 2 + 1);
		return -1;
	}

	rc = snprintf(pool_name, sizeof(pool_name), "ftl-nvpool-%p", dev);
	if (rc < 0 || rc >= 128) {
		return -1;
	}

	nv_cache->md_pool = spdk_mempool_create(pool_name, conf->nv_cache.max_request_cnt,
						spdk_bdev_get_md_size(bdev) *
						conf->nv_cache.max_request_size,
						SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
						SPDK_ENV_SOCKET_ID_ANY);
	if (!nv_cache->md_pool) {
		SPDK_ERRLOG("Failed to initialize non-volatile cache metadata pool\n");
		return -1;
	}

	nv_cache->dma_buf = spdk_dma_zmalloc(FTL_BLOCK_SIZE, spdk_bdev_get_buf_align(bdev), NULL);
	if (!nv_cache->dma_buf) {
		SPDK_ERRLOG("Memory allocation failure\n");
		return -1;
	}

	if (pthread_spin_init(&nv_cache->lock, PTHREAD_PROCESS_PRIVATE)) {
		SPDK_ERRLOG("Failed to initialize cache lock\n");
		return -1;
	}

	nv_cache->current_addr = FTL_NV_CACHE_DATA_OFFSET;
	nv_cache->num_data_blocks = spdk_bdev_get_num_blocks(bdev) - 1;
	nv_cache->num_available = nv_cache->num_data_blocks;
	nv_cache->ready = false;

	return 0;
}

void
spdk_ftl_conf_init_defaults(struct spdk_ftl_conf *conf)
{
	*conf = g_default_conf;
}

static void
ftl_lba_map_request_ctor(struct spdk_mempool *mp, void *opaque, void *obj, unsigned obj_idx)
{
	struct ftl_lba_map_request *request = obj;
	struct spdk_ftl_dev *dev = opaque;

	request->segments = spdk_bit_array_create(spdk_divide_round_up(
				    ftl_get_num_blocks_in_band(dev), FTL_NUM_LBA_IN_BLOCK));
}

static int
ftl_init_media_events_pool(struct spdk_ftl_dev *dev)
{
	char pool_name[128];
	int rc;

	rc = snprintf(pool_name, sizeof(pool_name), "ftl-media-%p", dev);
	if (rc < 0 || rc >= (int)sizeof(pool_name)) {
		SPDK_ERRLOG("Failed to create media pool name\n");
		return -1;
	}

	dev->media_events_pool = spdk_mempool_create(pool_name, 1024,
				 sizeof(struct ftl_media_event),
				 SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
				 SPDK_ENV_SOCKET_ID_ANY);
	if (!dev->media_events_pool) {
		SPDK_ERRLOG("Failed to create media events pool\n");
		return -1;
	}

	return 0;
}

static int
ftl_init_lba_map_pools(struct spdk_ftl_dev *dev)
{
#define POOL_NAME_LEN 128
	char pool_name[POOL_NAME_LEN];
	int rc;

	rc = snprintf(pool_name, sizeof(pool_name), "%s-%s", dev->name, "ftl-lba-pool");
	if (rc < 0 || rc >= POOL_NAME_LEN) {
		return -ENAMETOOLONG;
	}

	/* We need to reserve at least 2 buffers for band close / open sequence
	 * alone, plus additional (8) buffers for handling write errors.
	 * TODO: This memory pool is utilized only by core thread - it introduce
	 * unnecessary overhead and should be replaced by different data structure.
	 */
	dev->lba_pool = spdk_mempool_create(pool_name, 2 + 8,
					    ftl_lba_map_pool_elem_size(dev),
					    SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					    SPDK_ENV_SOCKET_ID_ANY);
	if (!dev->lba_pool) {
		return -ENOMEM;
	}

	rc = snprintf(pool_name, sizeof(pool_name), "%s-%s", dev->name, "ftl-lbareq-pool");
	if (rc < 0 || rc >= POOL_NAME_LEN) {
		return -ENAMETOOLONG;
	}

	dev->lba_request_pool = spdk_mempool_create_ctor(pool_name,
				dev->conf.max_reloc_qdepth * dev->conf.max_active_relocs,
				sizeof(struct ftl_lba_map_request),
				SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
				SPDK_ENV_SOCKET_ID_ANY,
				ftl_lba_map_request_ctor,
				dev);
	if (!dev->lba_request_pool) {
		return -ENOMEM;
	}

	return 0;
}

static void
ftl_init_wptr_list(struct spdk_ftl_dev *dev)
{
	LIST_INIT(&dev->wptr_list);
	LIST_INIT(&dev->flush_list);
	LIST_INIT(&dev->band_flush_list);
}

static size_t
ftl_dev_band_max_seq(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;
	size_t seq = 0;

	LIST_FOREACH(band, &dev->shut_bands, list_entry) {
		if (band->seq > seq) {
			seq = band->seq;
		}
	}

	return seq;
}

static void
_ftl_init_bands_state(void *ctx)
{
	struct ftl_band *band, *temp_band;
	struct spdk_ftl_dev *dev = ctx;

	dev->seq = ftl_dev_band_max_seq(dev);

	LIST_FOREACH_SAFE(band, &dev->shut_bands, list_entry, temp_band) {
		if (!band->lba_map.num_vld) {
			ftl_band_set_state(band, FTL_BAND_STATE_FREE);
		}
	}

	ftl_reloc_resume(dev->reloc);
	/* Clear the limit applications as they're incremented incorrectly by */
	/* the initialization code */
	memset(dev->stats.limits, 0, sizeof(dev->stats.limits));
}

static int
ftl_init_num_free_bands(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;
	int cnt = 0;

	LIST_FOREACH(band, &dev->shut_bands, list_entry) {
		if (band->num_zones && !band->lba_map.num_vld) {
			cnt++;
		}
	}
	return cnt;
}

static int
ftl_init_bands_state(struct spdk_ftl_dev *dev)
{
	/* TODO: Should we abort initialization or expose read only device */
	/* if there is no free bands? */
	/* If we abort initialization should we depend on condition that */
	/* we have no free bands or should we have some minimal number of */
	/* free bands? */
	if (!ftl_init_num_free_bands(dev)) {
		return -1;
	}

	spdk_thread_send_msg(ftl_get_core_thread(dev), _ftl_init_bands_state, dev);
	return 0;
}

static void
_ftl_dev_init_core_thread(void *ctx)
{
	struct spdk_ftl_dev *dev = ctx;

	dev->core_poller = SPDK_POLLER_REGISTER(ftl_task_core, dev, 0);
	if (!dev->core_poller) {
		SPDK_ERRLOG("Unable to register core poller\n");
		assert(0);
	}

	dev->ioch = spdk_get_io_channel(dev);
}

static int
ftl_dev_init_core_thread(struct spdk_ftl_dev *dev, const struct spdk_ftl_dev_init_opts *opts)
{
	if (!opts->core_thread) {
		return -1;
	}

	dev->core_thread = opts->core_thread;

	spdk_thread_send_msg(opts->core_thread, _ftl_dev_init_core_thread, dev);
	return 0;
}

static int
ftl_dev_l2p_alloc_pmem(struct spdk_ftl_dev *dev, size_t l2p_size, const char *l2p_path)
{
#ifdef SPDK_CONFIG_PMDK
	int is_pmem;

	if ((dev->l2p = pmem_map_file(l2p_path, 0,
				      0, 0, &dev->l2p_pmem_len, &is_pmem)) == NULL) {
		SPDK_ERRLOG("Failed to mmap l2p_path\n");
		return -1;
	}

	if (!is_pmem) {
		SPDK_NOTICELOG("l2p_path mapped on non-pmem device\n");
	}

	if (dev->l2p_pmem_len < l2p_size) {
		SPDK_ERRLOG("l2p_path file is too small\n");
		return -1;
	}

	pmem_memset_persist(dev->l2p, FTL_ADDR_INVALID, l2p_size);

	return 0;
#else /* SPDK_CONFIG_PMDK */
	SPDK_ERRLOG("Libpmem not available, cannot use pmem l2p_path\n");
	return -1;
#endif /* SPDK_CONFIG_PMDK */
}

static int
ftl_dev_l2p_alloc_dram(struct spdk_ftl_dev *dev, size_t l2p_size)
{
	dev->l2p = malloc(l2p_size);
	if (!dev->l2p) {
		SPDK_ERRLOG("Failed to allocate l2p table\n");
		return -1;
	}

	memset(dev->l2p, FTL_ADDR_INVALID, l2p_size);

	return 0;
}

static int
ftl_dev_l2p_alloc(struct spdk_ftl_dev *dev)
{
	size_t addr_size = dev->addr_len >= 32 ? 8 : 4;
	size_t l2p_size = dev->num_lbas * addr_size;
	const char *l2p_path = dev->conf.l2p_path;

	if (dev->num_lbas == 0) {
		SPDK_ERRLOG("Invalid l2p table size\n");
		return -1;
	}

	if (dev->l2p) {
		SPDK_ERRLOG("L2p table already allocated\n");
		return -1;
	}

	dev->l2p_pmem_len = 0;
	if (l2p_path) {
		return ftl_dev_l2p_alloc_pmem(dev, l2p_size, l2p_path);
	} else {
		return ftl_dev_l2p_alloc_dram(dev, l2p_size);
	}
}

static void
ftl_dev_free_init_ctx(struct ftl_dev_init_ctx *init_ctx)
{
	if (!init_ctx) {
		return;
	}

	if (init_ctx->ioch) {
		spdk_put_io_channel(init_ctx->ioch);
	}

	free(init_ctx);
}

static void
ftl_call_init_complete_cb(void *ctx)
{
	struct ftl_dev_init_ctx *init_ctx = ctx;
	struct spdk_ftl_dev *dev = init_ctx->dev;

	if (init_ctx->cb_fn != NULL) {
		init_ctx->cb_fn(dev, init_ctx->cb_arg, 0);
	}

	ftl_dev_free_init_ctx(init_ctx);
}

static void
ftl_init_complete(struct ftl_dev_init_ctx *init_ctx)
{
	struct spdk_ftl_dev *dev = init_ctx->dev;

	pthread_mutex_lock(&g_ftl_queue_lock);
	STAILQ_INSERT_HEAD(&g_ftl_queue, dev, stailq);
	pthread_mutex_unlock(&g_ftl_queue_lock);

	dev->initialized = 1;

	spdk_thread_send_msg(init_ctx->thread, ftl_call_init_complete_cb, init_ctx);
}

static void
ftl_init_fail_cb(struct spdk_ftl_dev *dev, void *ctx, int status)
{
	struct ftl_dev_init_ctx *init_ctx = ctx;

	if (init_ctx->cb_fn != NULL) {
		init_ctx->cb_fn(NULL, init_ctx->cb_arg, -ENODEV);
	}

	ftl_dev_free_init_ctx(init_ctx);
}

static int ftl_dev_free(struct spdk_ftl_dev *dev, spdk_ftl_init_fn cb_fn, void *cb_arg,
			struct spdk_thread *thread);

static void
ftl_init_fail(struct ftl_dev_init_ctx *init_ctx)
{
	if (ftl_dev_free(init_ctx->dev, ftl_init_fail_cb, init_ctx, init_ctx->thread)) {
		SPDK_ERRLOG("Unable to free the device\n");
		assert(0);
	}
}

static void
ftl_write_nv_cache_md_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_dev_init_ctx *init_ctx = cb_arg;
	struct spdk_ftl_dev *dev = init_ctx->dev;

	spdk_bdev_free_io(bdev_io);
	if (spdk_unlikely(!success)) {
		SPDK_ERRLOG("Writing non-volatile cache's metadata header failed\n");
		ftl_init_fail(init_ctx);
		return;
	}

	dev->nv_cache.ready = true;
	ftl_init_complete(init_ctx);
}

static void
ftl_clear_nv_cache_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_dev_init_ctx *init_ctx = cb_arg;
	struct spdk_ftl_dev *dev = init_ctx->dev;
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;

	spdk_bdev_free_io(bdev_io);
	if (spdk_unlikely(!success)) {
		SPDK_ERRLOG("Unable to clear the non-volatile cache bdev\n");
		ftl_init_fail(init_ctx);
		return;
	}

	nv_cache->phase = 1;
	if (ftl_nv_cache_write_header(nv_cache, false, ftl_write_nv_cache_md_cb, init_ctx)) {
		SPDK_ERRLOG("Unable to write non-volatile cache metadata header\n");
		ftl_init_fail(init_ctx);
	}
}

static void
_ftl_nv_cache_scrub(void *ctx)
{
	struct ftl_dev_init_ctx *init_ctx = ctx;
	struct spdk_ftl_dev *dev = init_ctx->dev;
	int rc;

	rc = ftl_nv_cache_scrub(&dev->nv_cache, ftl_clear_nv_cache_cb, init_ctx);

	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Unable to clear the non-volatile cache bdev: %s\n",
			    spdk_strerror(-rc));
		ftl_init_fail(init_ctx);
	}
}

static int
ftl_setup_initial_state(struct ftl_dev_init_ctx *init_ctx)
{
	struct spdk_ftl_dev *dev = init_ctx->dev;
	struct spdk_ftl_conf *conf = &dev->conf;
	size_t i;

	spdk_uuid_generate(&dev->uuid);

	dev->num_lbas = 0;
	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		dev->num_lbas += ftl_band_num_usable_blocks(&dev->bands[i]);
	}

	dev->num_lbas = (dev->num_lbas * (100 - conf->lba_rsvd)) / 100;

	if (ftl_dev_l2p_alloc(dev)) {
		SPDK_ERRLOG("Unable to init l2p table\n");
		return -1;
	}

	if (ftl_init_bands_state(dev)) {
		SPDK_ERRLOG("Unable to finish the initialization\n");
		return -1;
	}

	if (!ftl_dev_has_nv_cache(dev)) {
		ftl_init_complete(init_ctx);
	} else {
		spdk_thread_send_msg(ftl_get_core_thread(dev), _ftl_nv_cache_scrub, init_ctx);
	}

	return 0;
}

static void
ftl_restore_nv_cache_cb(struct ftl_restore *restore, int status, void *cb_arg)
{
	struct ftl_dev_init_ctx *init_ctx = cb_arg;

	if (spdk_unlikely(status != 0)) {
		SPDK_ERRLOG("Failed to restore the non-volatile cache state\n");
		ftl_init_fail(init_ctx);
		return;
	}

	ftl_init_complete(init_ctx);
}

static void
ftl_restore_device_cb(struct ftl_restore *restore, int status, void *cb_arg)
{
	struct ftl_dev_init_ctx *init_ctx = cb_arg;
	struct spdk_ftl_dev *dev = init_ctx->dev;

	if (status) {
		SPDK_ERRLOG("Failed to restore the device from the SSD\n");
		ftl_init_fail(init_ctx);
		return;
	}

	if (ftl_init_bands_state(dev)) {
		SPDK_ERRLOG("Unable to finish the initialization\n");
		ftl_init_fail(init_ctx);
		return;
	}

	if (!ftl_dev_has_nv_cache(dev)) {
		ftl_init_complete(init_ctx);
		return;
	}

	ftl_restore_nv_cache(restore, ftl_restore_nv_cache_cb, init_ctx);
}

static void
ftl_restore_md_cb(struct ftl_restore *restore, int status, void *cb_arg)
{
	struct ftl_dev_init_ctx *init_ctx = cb_arg;

	if (status) {
		SPDK_ERRLOG("Failed to restore the metadata from the SSD\n");
		goto error;
	}

	/* After the metadata is read it should be possible to allocate the L2P */
	if (ftl_dev_l2p_alloc(init_ctx->dev)) {
		SPDK_ERRLOG("Failed to allocate the L2P\n");
		goto error;
	}

	if (ftl_restore_device(restore, ftl_restore_device_cb, init_ctx)) {
		SPDK_ERRLOG("Failed to start device restoration from the SSD\n");
		goto error;
	}

	return;
error:
	ftl_init_fail(init_ctx);
}

static int
ftl_restore_state(struct ftl_dev_init_ctx *init_ctx)
{
	struct spdk_ftl_dev *dev = init_ctx->dev;

	dev->uuid = init_ctx->opts.uuid;

	if (ftl_restore_md(dev, ftl_restore_md_cb, init_ctx)) {
		SPDK_ERRLOG("Failed to start metadata restoration from the SSD\n");
		return -1;
	}

	return 0;
}

static void
ftl_dev_update_bands(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band, *temp_band;
	size_t i;

	for (i = 0; i < ftl_get_num_bands(dev); ++i) {
		band = &dev->bands[i];
		band->tail_md_addr = ftl_band_tail_md_addr(band);
	}

	/* Remove band from shut_bands list to prevent further processing */
	/* if all blocks on this band are bad */
	LIST_FOREACH_SAFE(band, &dev->shut_bands, list_entry, temp_band) {
		if (!band->num_zones) {
			dev->num_bands--;
			LIST_REMOVE(band, list_entry);
		}
	}
}

static void
ftl_dev_init_state(struct ftl_dev_init_ctx *init_ctx)
{
	struct spdk_ftl_dev *dev = init_ctx->dev;

	ftl_dev_update_bands(dev);

	if (ftl_dev_init_core_thread(dev, &init_ctx->opts)) {
		SPDK_ERRLOG("Unable to initialize device thread\n");
		ftl_init_fail(init_ctx);
		return;
	}

	if (init_ctx->opts.mode & SPDK_FTL_MODE_CREATE) {
		if (ftl_setup_initial_state(init_ctx)) {
			SPDK_ERRLOG("Failed to setup initial state of the device\n");
			ftl_init_fail(init_ctx);
			return;
		}
	} else {
		if (ftl_restore_state(init_ctx)) {
			SPDK_ERRLOG("Unable to restore device's state from the SSD\n");
			ftl_init_fail(init_ctx);
			return;
		}
	}
}

static void ftl_dev_get_zone_info(struct ftl_dev_init_ctx *init_ctx);

static void
ftl_dev_get_zone_info_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_dev_init_ctx *init_ctx = cb_arg;
	struct spdk_ftl_dev *dev = init_ctx->dev;
	struct ftl_band *band;
	struct ftl_zone *zone;
	struct ftl_addr addr;
	size_t i, zones_left, num_zones;

	spdk_bdev_free_io(bdev_io);

	if (spdk_unlikely(!success)) {
		SPDK_ERRLOG("Unable to read zone info for zone id: %"PRIu64"\n", init_ctx->zone_id);
		ftl_init_fail(init_ctx);
		return;
	}

	zones_left = ftl_get_num_zones(dev) - (init_ctx->zone_id / ftl_get_num_blocks_in_zone(dev));
	num_zones = spdk_min(zones_left, FTL_ZONE_INFO_COUNT);

	for (i = 0; i < num_zones; ++i) {
		addr.offset = init_ctx->info[i].zone_id;
		band = &dev->bands[ftl_addr_get_band(dev, addr)];
		zone = &band->zone_buf[ftl_addr_get_punit(dev, addr)];
		zone->info = init_ctx->info[i];

		/* TODO: add support for zone capacity less than zone size */
		if (zone->info.capacity != ftl_get_num_blocks_in_zone(dev)) {
			zone->info.state = SPDK_BDEV_ZONE_STATE_OFFLINE;
			SPDK_ERRLOG("Zone capacity is not equal zone size for "
				    "zone id: %"PRIu64"\n", init_ctx->zone_id);
		}

		/* Set write pointer to the last block plus one for zone in full state */
		if (zone->info.state == SPDK_BDEV_ZONE_STATE_FULL) {
			zone->info.write_pointer = zone->info.zone_id + zone->info.capacity;
		}

		if (zone->info.state != SPDK_BDEV_ZONE_STATE_OFFLINE) {
			band->num_zones++;
			CIRCLEQ_INSERT_TAIL(&band->zones, zone, circleq);
		}
	}

	init_ctx->zone_id = init_ctx->zone_id + num_zones * ftl_get_num_blocks_in_zone(dev);

	ftl_dev_get_zone_info(init_ctx);
}

static void
ftl_dev_get_zone_info(struct ftl_dev_init_ctx *init_ctx)
{
	struct spdk_ftl_dev *dev = init_ctx->dev;
	size_t zones_left, num_zones;
	int rc;

	zones_left = ftl_get_num_zones(dev) - (init_ctx->zone_id / ftl_get_num_blocks_in_zone(dev));
	if (zones_left == 0) {
		ftl_dev_init_state(init_ctx);
		return;
	}

	num_zones = spdk_min(zones_left, FTL_ZONE_INFO_COUNT);

	rc = spdk_bdev_get_zone_info(dev->base_bdev_desc, init_ctx->ioch,
				     init_ctx->zone_id, num_zones, init_ctx->info,
				     ftl_dev_get_zone_info_cb, init_ctx);

	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Unable to read zone info for zone id: %"PRIu64"\n", init_ctx->zone_id);
		ftl_init_fail(init_ctx);
	}
}

static int
ftl_dev_init_zones(struct ftl_dev_init_ctx *init_ctx)
{
	struct spdk_ftl_dev *dev =  init_ctx->dev;

	init_ctx->zone_id = 0;
	init_ctx->ioch = spdk_bdev_get_io_channel(dev->base_bdev_desc);
	if (!init_ctx->ioch) {
		SPDK_ERRLOG("Failed to get base bdev IO channel\n");
		return -1;
	}

	ftl_dev_get_zone_info(init_ctx);

	return 0;
}

struct _ftl_io_channel {
	struct ftl_io_channel *ioch;
};

struct ftl_io_channel *
ftl_io_channel_get_ctx(struct spdk_io_channel *ioch)
{
	struct _ftl_io_channel *_ioch = spdk_io_channel_get_ctx(ioch);

	return _ioch->ioch;
}

static void
ftl_io_channel_register(void *ctx)
{
	struct ftl_io_channel *ioch = ctx;
	struct spdk_ftl_dev *dev = ioch->dev;
	uint32_t ioch_index;

	for (ioch_index = 0; ioch_index < dev->conf.max_io_channels; ++ioch_index) {
		if (dev->ioch_array[ioch_index] == NULL) {
			dev->ioch_array[ioch_index] = ioch;
			ioch->index = ioch_index;
			break;
		}
	}

	assert(ioch_index < dev->conf.max_io_channels);
	TAILQ_INSERT_TAIL(&dev->ioch_queue, ioch, tailq);
}

static int
ftl_io_channel_init_wbuf(struct ftl_io_channel *ioch)
{
	struct spdk_ftl_dev *dev = ioch->dev;
	struct ftl_wbuf_entry *entry;
	uint32_t i;
	int rc;

	ioch->num_entries = dev->conf.write_buffer_size / FTL_BLOCK_SIZE;
	ioch->wbuf_entries = calloc(ioch->num_entries, sizeof(*ioch->wbuf_entries));
	if (ioch->wbuf_entries == NULL) {
		SPDK_ERRLOG("Failed to allocate write buffer entry array\n");
		return -1;
	}

	ioch->qdepth_limit = ioch->num_entries;
	ioch->wbuf_payload = spdk_zmalloc(dev->conf.write_buffer_size, FTL_BLOCK_SIZE, NULL,
					  SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (ioch->wbuf_payload == NULL) {
		SPDK_ERRLOG("Failed to allocate write buffer payload\n");
		goto error_entries;
	}

	ioch->free_queue = spdk_ring_create(SPDK_RING_TYPE_SP_SC,
					    spdk_align32pow2(ioch->num_entries + 1),
					    SPDK_ENV_SOCKET_ID_ANY);
	if (ioch->free_queue == NULL) {
		SPDK_ERRLOG("Failed to allocate free queue\n");
		goto error_payload;
	}

	ioch->submit_queue = spdk_ring_create(SPDK_RING_TYPE_SP_SC,
					      spdk_align32pow2(ioch->num_entries + 1),
					      SPDK_ENV_SOCKET_ID_ANY);
	if (ioch->submit_queue == NULL) {
		SPDK_ERRLOG("Failed to allocate submit queue\n");
		goto error_free_queue;
	}

	for (i = 0; i < ioch->num_entries; ++i) {
		entry = &ioch->wbuf_entries[i];
		entry->payload = (char *)ioch->wbuf_payload + i * FTL_BLOCK_SIZE;
		entry->ioch = ioch;
		entry->index = i;
		entry->addr.offset = FTL_ADDR_INVALID;

		rc = pthread_spin_init(&entry->lock, PTHREAD_PROCESS_PRIVATE);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to initialize spinlock\n");
			goto error_spinlock;
		}

		spdk_ring_enqueue(ioch->free_queue, (void **)&entry, 1, NULL);
	}

	return 0;
error_spinlock:
	for (; i > 0; --i) {
		pthread_spin_destroy(&ioch->wbuf_entries[i - 1].lock);
	}

	spdk_ring_free(ioch->submit_queue);
error_free_queue:
	spdk_ring_free(ioch->free_queue);
error_payload:
	spdk_free(ioch->wbuf_payload);
error_entries:
	free(ioch->wbuf_entries);

	return -1;
}

static int
ftl_io_channel_create_cb(void *io_device, void *ctx)
{
	struct spdk_ftl_dev *dev = io_device;
	struct _ftl_io_channel *_ioch = ctx;
	struct ftl_io_channel *ioch;
	uint32_t num_io_channels;
	char mempool_name[32];
	int rc;

	num_io_channels = __atomic_fetch_add(&dev->num_io_channels, 1, __ATOMIC_SEQ_CST);
	if (num_io_channels >= dev->conf.max_io_channels) {
		SPDK_ERRLOG("Reached maximum number of IO channels\n");
		__atomic_fetch_sub(&dev->num_io_channels, 1, __ATOMIC_SEQ_CST);
		return -1;
	}

	ioch = calloc(1, sizeof(*ioch));
	if (ioch == NULL) {
		SPDK_ERRLOG("Failed to allocate IO channel\n");
		return -1;
	}

	rc = snprintf(mempool_name, sizeof(mempool_name), "ftl_io_%p", ioch);
	if (rc < 0 || rc >= (int)sizeof(mempool_name)) {
		SPDK_ERRLOG("Failed to create IO channel pool name\n");
		free(ioch);
		return -1;
	}

	ioch->cache_ioch = NULL;
	ioch->index = FTL_IO_CHANNEL_INDEX_INVALID;
	ioch->dev = dev;
	ioch->elem_size = sizeof(struct ftl_md_io);
	ioch->io_pool = spdk_mempool_create(mempool_name,
					    dev->conf.user_io_pool_size,
					    ioch->elem_size,
					    0,
					    SPDK_ENV_SOCKET_ID_ANY);
	if (!ioch->io_pool) {
		SPDK_ERRLOG("Failed to create IO channel's IO pool\n");
		free(ioch);
		return -1;
	}

	ioch->base_ioch = spdk_bdev_get_io_channel(dev->base_bdev_desc);
	if (!ioch->base_ioch) {
		SPDK_ERRLOG("Failed to create base bdev IO channel\n");
		goto fail_ioch;
	}

	if (ftl_dev_has_nv_cache(dev)) {
		ioch->cache_ioch = spdk_bdev_get_io_channel(dev->nv_cache.bdev_desc);
		if (!ioch->cache_ioch) {
			SPDK_ERRLOG("Failed to create cache IO channel\n");
			goto fail_cache;
		}
	}

	TAILQ_INIT(&ioch->write_cmpl_queue);
	TAILQ_INIT(&ioch->retry_queue);
	ioch->poller = SPDK_POLLER_REGISTER(ftl_io_channel_poll, ioch, 0);
	if (!ioch->poller) {
		SPDK_ERRLOG("Failed to register IO channel poller\n");
		goto fail_poller;
	}

	if (ftl_io_channel_init_wbuf(ioch)) {
		SPDK_ERRLOG("Failed to initialize IO channel's write buffer\n");
		goto fail_wbuf;
	}

	_ioch->ioch = ioch;

	spdk_thread_send_msg(ftl_get_core_thread(dev), ftl_io_channel_register, ioch);

	return 0;
fail_wbuf:
	spdk_poller_unregister(&ioch->poller);
fail_poller:
	if (ioch->cache_ioch) {
		spdk_put_io_channel(ioch->cache_ioch);
	}
fail_cache:
	spdk_put_io_channel(ioch->base_ioch);
fail_ioch:
	spdk_mempool_free(ioch->io_pool);
	free(ioch);

	return -1;
}

static void
ftl_io_channel_unregister(void *ctx)
{
	struct ftl_io_channel *ioch = ctx;
	struct spdk_ftl_dev *dev = ioch->dev;
	uint32_t i, num_io_channels __attribute__((unused));

	assert(ioch->index < dev->conf.max_io_channels);
	assert(dev->ioch_array[ioch->index] == ioch);

	dev->ioch_array[ioch->index] = NULL;
	TAILQ_REMOVE(&dev->ioch_queue, ioch, tailq);

	num_io_channels = __atomic_fetch_sub(&dev->num_io_channels, 1, __ATOMIC_SEQ_CST);
	assert(num_io_channels > 0);

	for (i = 0; i < ioch->num_entries; ++i) {
		pthread_spin_destroy(&ioch->wbuf_entries[i].lock);
	}

	spdk_mempool_free(ioch->io_pool);
	spdk_ring_free(ioch->free_queue);
	spdk_ring_free(ioch->submit_queue);
	spdk_free(ioch->wbuf_payload);
	free(ioch->wbuf_entries);
	free(ioch);
}

static void
_ftl_io_channel_destroy_cb(void *ctx)
{
	struct ftl_io_channel *ioch = ctx;
	struct spdk_ftl_dev *dev = ioch->dev;
	uint32_t i;

	/* Do not destroy the channel if some of its entries are still in use */
	if (spdk_ring_count(ioch->free_queue) != ioch->num_entries) {
		spdk_thread_send_msg(spdk_get_thread(), _ftl_io_channel_destroy_cb, ctx);
		return;
	}

	/* Evict all valid entries from cache */
	for (i = 0; i < ioch->num_entries; ++i) {
		ftl_evict_cache_entry(dev, &ioch->wbuf_entries[i]);
	}

	spdk_poller_unregister(&ioch->poller);

	spdk_put_io_channel(ioch->base_ioch);
	if (ioch->cache_ioch) {
		spdk_put_io_channel(ioch->cache_ioch);
	}

	ioch->base_ioch = NULL;
	ioch->cache_ioch = NULL;

	spdk_thread_send_msg(ftl_get_core_thread(dev), ftl_io_channel_unregister, ioch);
}

static void
ftl_io_channel_destroy_cb(void *io_device, void *ctx)
{
	struct _ftl_io_channel *_ioch = ctx;
	struct ftl_io_channel *ioch = _ioch->ioch;

	/* Mark the IO channel as being flush to force out any unwritten entries */
	ioch->flush = true;

	_ftl_io_channel_destroy_cb(ioch);
}

static int
ftl_dev_init_io_channel(struct spdk_ftl_dev *dev)
{
	struct ftl_batch *batch;
	uint32_t i;

	/* Align the IO channels to nearest power of 2 to allow for easy addr bit shift */
	dev->conf.max_io_channels = spdk_align32pow2(dev->conf.max_io_channels);
	dev->ioch_shift = spdk_u32log2(dev->conf.max_io_channels);

	dev->ioch_array = calloc(dev->conf.max_io_channels, sizeof(*dev->ioch_array));
	if (!dev->ioch_array) {
		SPDK_ERRLOG("Failed to allocate IO channel array\n");
		return -1;
	}

	if (dev->md_size > 0) {
		dev->md_buf = spdk_zmalloc(dev->md_size * dev->xfer_size * FTL_BATCH_COUNT,
					   dev->md_size, NULL, SPDK_ENV_LCORE_ID_ANY,
					   SPDK_MALLOC_DMA);
		if (dev->md_buf == NULL) {
			SPDK_ERRLOG("Failed to allocate metadata buffer\n");
			return -1;
		}
	}

	dev->iov_buf = calloc(FTL_BATCH_COUNT, dev->xfer_size * sizeof(struct iovec));
	if (!dev->iov_buf) {
		SPDK_ERRLOG("Failed to allocate iovec buffer\n");
		return -1;
	}

	TAILQ_INIT(&dev->free_batches);
	TAILQ_INIT(&dev->pending_batches);
	TAILQ_INIT(&dev->ioch_queue);

	for (i = 0; i < FTL_BATCH_COUNT; ++i) {
		batch = &dev->batch_array[i];
		batch->iov = &dev->iov_buf[i * dev->xfer_size];
		batch->num_entries = 0;
		batch->index = i;
		TAILQ_INIT(&batch->entries);
		if (dev->md_buf != NULL) {
			batch->metadata = (char *)dev->md_buf + i * dev->xfer_size * dev->md_size;
		}

		TAILQ_INSERT_TAIL(&dev->free_batches, batch, tailq);
	}

	dev->num_io_channels = 0;

	spdk_io_device_register(dev, ftl_io_channel_create_cb, ftl_io_channel_destroy_cb,
				sizeof(struct _ftl_io_channel),
				NULL);

	return 0;
}

static int
ftl_dev_init_base_bdev(struct spdk_ftl_dev *dev, const char *bdev_name)
{
	uint32_t block_size;
	uint64_t num_blocks;
	struct spdk_bdev *bdev;

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (!bdev) {
		SPDK_ERRLOG("Unable to find bdev: %s\n", bdev_name);
		return -1;
	}

	if (!spdk_bdev_is_zoned(bdev)) {
		SPDK_ERRLOG("Bdev dosen't support zone capabilities: %s\n",
			    spdk_bdev_get_name(bdev));
		return -1;
	}

	if (spdk_bdev_open_ext(bdev_name, true, ftl_bdev_event_cb,
			       dev, &dev->base_bdev_desc)) {
		SPDK_ERRLOG("Unable to open bdev: %s\n", bdev_name);
		return -1;
	}

	if (spdk_bdev_module_claim_bdev(bdev, dev->base_bdev_desc, &g_ftl_bdev_module)) {
		spdk_bdev_close(dev->base_bdev_desc);
		dev->base_bdev_desc = NULL;
		SPDK_ERRLOG("Unable to claim bdev %s\n", bdev_name);
		return -1;
	}

	dev->xfer_size = spdk_bdev_get_write_unit_size(bdev);
	dev->md_size = spdk_bdev_get_md_size(bdev);

	block_size = spdk_bdev_get_block_size(bdev);
	if (block_size != FTL_BLOCK_SIZE) {
		SPDK_ERRLOG("Unsupported block size (%"PRIu32")\n", block_size);
		return -1;
	}

	num_blocks = spdk_bdev_get_num_blocks(bdev);
	if (num_blocks % ftl_get_num_punits(dev)) {
		SPDK_ERRLOG("Unsupported geometry. Base bdev block count must be multiple "
			    "of optimal number of zones.\n");
		return -1;
	}

	if (ftl_is_append_supported(dev) &&
	    !spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_ZONE_APPEND)) {
		SPDK_ERRLOG("Bdev dosen't support append: %s\n",
			    spdk_bdev_get_name(bdev));
		return -1;
	}

	dev->num_bands = num_blocks / (ftl_get_num_punits(dev) * ftl_get_num_blocks_in_zone(dev));
	dev->addr_len = spdk_u64log2(num_blocks) + 1;

	return 0;
}

static void
ftl_lba_map_request_dtor(struct spdk_mempool *mp, void *opaque, void *obj, unsigned obj_idx)
{
	struct ftl_lba_map_request *request = obj;

	spdk_bit_array_free(&request->segments);
}

static void
ftl_release_bdev(struct spdk_bdev_desc *bdev_desc)
{
	if (!bdev_desc) {
		return;
	}

	spdk_bdev_module_release_bdev(spdk_bdev_desc_get_bdev(bdev_desc));
	spdk_bdev_close(bdev_desc);
}

static void
ftl_dev_free_sync(struct spdk_ftl_dev *dev)
{
	struct spdk_ftl_dev *iter;
	size_t i;

	if (!dev) {
		return;
	}

	pthread_mutex_lock(&g_ftl_queue_lock);
	STAILQ_FOREACH(iter, &g_ftl_queue, stailq) {
		if (iter == dev) {
			STAILQ_REMOVE(&g_ftl_queue, dev, spdk_ftl_dev, stailq);
			break;
		}
	}
	pthread_mutex_unlock(&g_ftl_queue_lock);

	assert(LIST_EMPTY(&dev->wptr_list));
	assert(dev->current_batch == NULL);

	ftl_dev_dump_bands(dev);
	ftl_dev_dump_stats(dev);

	if (dev->bands) {
		for (i = 0; i < ftl_get_num_bands(dev); ++i) {
			free(dev->bands[i].zone_buf);
			spdk_bit_array_free(&dev->bands[i].lba_map.vld);
			spdk_bit_array_free(&dev->bands[i].reloc_bitmap);
		}
	}

	spdk_dma_free(dev->nv_cache.dma_buf);

	spdk_mempool_free(dev->lba_pool);
	spdk_mempool_free(dev->nv_cache.md_pool);
	spdk_mempool_free(dev->media_events_pool);
	if (dev->lba_request_pool) {
		spdk_mempool_obj_iter(dev->lba_request_pool, ftl_lba_map_request_dtor, NULL);
	}
	spdk_mempool_free(dev->lba_request_pool);

	ftl_reloc_free(dev->reloc);

	ftl_release_bdev(dev->nv_cache.bdev_desc);
	ftl_release_bdev(dev->base_bdev_desc);

	spdk_free(dev->md_buf);

	assert(dev->num_io_channels == 0);
	free(dev->ioch_array);
	free(dev->iov_buf);
	free(dev->name);
	free(dev->bands);
	if (dev->l2p_pmem_len != 0) {
#ifdef SPDK_CONFIG_PMDK
		pmem_unmap(dev->l2p, dev->l2p_pmem_len);
#endif /* SPDK_CONFIG_PMDK */
	} else {
		free(dev->l2p);
	}
	free((char *)dev->conf.l2p_path);
	free(dev);
}

int
spdk_ftl_dev_init(const struct spdk_ftl_dev_init_opts *_opts, spdk_ftl_init_fn cb_fn, void *cb_arg)
{
	struct spdk_ftl_dev *dev;
	struct spdk_ftl_dev_init_opts opts = *_opts;
	struct ftl_dev_init_ctx *init_ctx = NULL;
	int rc = -ENOMEM;

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		return -ENOMEM;
	}

	init_ctx = calloc(1, sizeof(*init_ctx));
	if (!init_ctx) {
		goto fail_sync;
	}

	init_ctx->dev = dev;
	init_ctx->opts = *_opts;
	init_ctx->cb_fn = cb_fn;
	init_ctx->cb_arg = cb_arg;
	init_ctx->thread = spdk_get_thread();

	if (!opts.conf) {
		opts.conf = &g_default_conf;
	}

	if (!opts.base_bdev) {
		SPDK_ERRLOG("Lack of underlying device in configuration\n");
		rc = -EINVAL;
		goto fail_sync;
	}

	dev->conf = *opts.conf;
	dev->limit = SPDK_FTL_LIMIT_MAX;

	dev->name = strdup(opts.name);
	if (!dev->name) {
		SPDK_ERRLOG("Unable to set device name\n");
		goto fail_sync;
	}

	if (ftl_dev_init_base_bdev(dev, opts.base_bdev)) {
		SPDK_ERRLOG("Unsupported underlying device\n");
		goto fail_sync;
	}

	if (opts.conf->l2p_path) {
		dev->conf.l2p_path = strdup(opts.conf->l2p_path);
		if (!dev->conf.l2p_path) {
			rc = -ENOMEM;
			goto fail_sync;
		}
	}

	/* In case of errors, we free all of the memory in ftl_dev_free_sync(), */
	/* so we don't have to clean up in each of the init functions. */
	if (ftl_check_conf(dev, opts.conf)) {
		SPDK_ERRLOG("Invalid device configuration\n");
		goto fail_sync;
	}

	if (ftl_init_lba_map_pools(dev)) {
		SPDK_ERRLOG("Unable to init LBA map pools\n");
		goto fail_sync;
	}

	if (ftl_init_media_events_pool(dev)) {
		SPDK_ERRLOG("Unable to init media events pools\n");
		goto fail_sync;
	}

	ftl_init_wptr_list(dev);

	if (ftl_dev_init_bands(dev)) {
		SPDK_ERRLOG("Unable to initialize band array\n");
		goto fail_sync;
	}

	if (ftl_dev_init_nv_cache(dev, opts.cache_bdev)) {
		SPDK_ERRLOG("Unable to initialize persistent cache\n");
		goto fail_sync;
	}

	dev->reloc = ftl_reloc_init(dev);
	if (!dev->reloc) {
		SPDK_ERRLOG("Unable to initialize reloc structures\n");
		goto fail_sync;
	}

	if (ftl_dev_init_io_channel(dev)) {
		SPDK_ERRLOG("Unable to initialize IO channels\n");
		goto fail_sync;
	}

	if (ftl_dev_init_zones(init_ctx)) {
		SPDK_ERRLOG("Failed to initialize zones\n");
		goto fail_async;
	}

	return 0;
fail_sync:
	ftl_dev_free_sync(dev);
	ftl_dev_free_init_ctx(init_ctx);
	return rc;
fail_async:
	ftl_init_fail(init_ctx);
	return 0;
}

static void
_ftl_halt_defrag(void *arg)
{
	ftl_reloc_halt(((struct spdk_ftl_dev *)arg)->reloc);
}

static void
ftl_halt_complete_cb(void *ctx)
{
	struct ftl_dev_init_ctx *fini_ctx = ctx;
	struct spdk_ftl_dev *dev = fini_ctx->dev;

	/* Make sure core IO channel has already been released */
	if (dev->num_io_channels > 0) {
		spdk_thread_send_msg(spdk_get_thread(), ftl_halt_complete_cb, ctx);
		return;
	}

	spdk_io_device_unregister(fini_ctx->dev, NULL);

	ftl_dev_free_sync(fini_ctx->dev);
	if (fini_ctx->cb_fn != NULL) {
		fini_ctx->cb_fn(NULL, fini_ctx->cb_arg, fini_ctx->halt_complete_status);
	}

	ftl_dev_free_init_ctx(fini_ctx);
}

static void
ftl_put_io_channel_cb(void *ctx)
{
	struct ftl_dev_init_ctx *fini_ctx = ctx;
	struct spdk_ftl_dev *dev = fini_ctx->dev;

	spdk_put_io_channel(dev->ioch);
	spdk_thread_send_msg(spdk_get_thread(), ftl_halt_complete_cb, ctx);
}

static void
ftl_nv_cache_header_fini_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_dev_init_ctx *fini_ctx = cb_arg;
	int rc = 0;

	spdk_bdev_free_io(bdev_io);
	if (spdk_unlikely(!success)) {
		SPDK_ERRLOG("Failed to write non-volatile cache metadata header\n");
		rc = -EIO;
	}

	fini_ctx->halt_complete_status = rc;
	spdk_thread_send_msg(fini_ctx->thread, ftl_put_io_channel_cb, fini_ctx);
}

static int
ftl_halt_poller(void *ctx)
{
	struct ftl_dev_init_ctx *fini_ctx = ctx;
	struct spdk_ftl_dev *dev = fini_ctx->dev;

	if (!dev->core_poller) {
		spdk_poller_unregister(&fini_ctx->poller);

		if (ftl_dev_has_nv_cache(dev)) {
			ftl_nv_cache_write_header(&dev->nv_cache, true,
						  ftl_nv_cache_header_fini_cb, fini_ctx);
		} else {
			fini_ctx->halt_complete_status = 0;
			spdk_thread_send_msg(fini_ctx->thread, ftl_put_io_channel_cb, fini_ctx);
		}
	}

	return SPDK_POLLER_BUSY;
}

static void
ftl_add_halt_poller(void *ctx)
{
	struct ftl_dev_init_ctx *fini_ctx = ctx;
	struct spdk_ftl_dev *dev = fini_ctx->dev;

	dev->halt = 1;

	_ftl_halt_defrag(dev);

	assert(!fini_ctx->poller);
	fini_ctx->poller = SPDK_POLLER_REGISTER(ftl_halt_poller, fini_ctx, 100);
}

static int
ftl_dev_free(struct spdk_ftl_dev *dev, spdk_ftl_init_fn cb_fn, void *cb_arg,
	     struct spdk_thread *thread)
{
	struct ftl_dev_init_ctx *fini_ctx;

	if (dev->halt_started) {
		dev->halt_started = true;
		return -EBUSY;
	}

	fini_ctx = calloc(1, sizeof(*fini_ctx));
	if (!fini_ctx) {
		return -ENOMEM;
	}

	fini_ctx->dev = dev;
	fini_ctx->cb_fn = cb_fn;
	fini_ctx->cb_arg = cb_arg;
	fini_ctx->thread = thread;

	spdk_thread_send_msg(ftl_get_core_thread(dev), ftl_add_halt_poller, fini_ctx);
	return 0;
}

int
spdk_ftl_dev_free(struct spdk_ftl_dev *dev, spdk_ftl_init_fn cb_fn, void *cb_arg)
{
	return ftl_dev_free(dev, cb_fn, cb_arg, spdk_get_thread());
}

SPDK_LOG_REGISTER_COMPONENT(ftl_init)
