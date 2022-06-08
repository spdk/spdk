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

#include "ftl_core.h"
#include "ftl_utils.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "ftl_band.h"
#include "ftl_internal.h"
#include "ftl_nv_cache.h"
#include "ftl_debug.h"

void ftl_mngt_check_conf(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	if (ftl_conf_is_valid(&dev->conf)) {
		ftl_mngt_next_step(mngt);
	} else {
		ftl_mngt_fail_step(mngt);
	}
}

static int init_lba_map_pool(struct spdk_ftl_dev *dev)
{
	/* We need to reserve at least 2 buffers for band close / open sequence
	 * alone, plus additional (8) buffers for handling write errors.
	 */
	size_t lba_pool_el_blks = ftl_lba_map_pool_elem_size(dev);
	if (lba_pool_el_blks % FTL_BLOCK_SIZE) {
		lba_pool_el_blks += FTL_BLOCK_SIZE;
	}
	lba_pool_el_blks /= FTL_BLOCK_SIZE;

	size_t lba_pool_buf_blks = LBA_MEMPOOL_SIZE * lba_pool_el_blks;

	dev->lba_pool_md = ftl_md_create(dev, lba_pool_buf_blks, 0, "lba_pool",
					 ftl_md_create_shm_flags(dev));
	if (!dev->lba_pool_md) {
		return -ENOMEM;
	}

	void *lba_pool_buf = ftl_md_get_buffer(dev->lba_pool_md);
	dev->lba_pool = ftl_mempool_create_ext(lba_pool_buf, LBA_MEMPOOL_SIZE,
					       lba_pool_el_blks * FTL_BLOCK_SIZE,
					       FTL_BLOCK_SIZE);
	if (!dev->lba_pool) {
		return -ENOMEM;
	}

	if (!ftl_fast_startup(dev)) {
		ftl_mempool_initialize_ext(dev->lba_pool);
	}

	return 0;
}

static int init_band_md_pool(struct spdk_ftl_dev *dev)
{
	/* We need to reserve at least 2 buffers for band close / open sequence
	 * alone, plus additional (8) buffers for handling write errors.
	 */
	dev->band_md_pool = ftl_mempool_create(LBA_MEMPOOL_SIZE,
					       sizeof(struct ftl_band_md),
					       FTL_BLOCK_SIZE,
					       SPDK_ENV_SOCKET_ID_ANY);
	if (!dev->band_md_pool) {
		return -ENOMEM;
	}

	return 0;
}

static int init_media_events_pool(struct spdk_ftl_dev *dev)
{
	char pool_name[128];
	int rc;

	rc = snprintf(pool_name, sizeof(pool_name), "ftl-media-%p", dev);
	if (rc < 0 || rc >= (int)sizeof(pool_name)) {
		FTL_ERRLOG(dev, "Failed to create media pool name\n");
		return -1;
	}

	dev->media_events_pool = spdk_mempool_create(pool_name, 1024,
				 sizeof(struct ftl_media_event),
				 SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
				 SPDK_ENV_SOCKET_ID_ANY);
	if (!dev->media_events_pool) {
		FTL_ERRLOG(dev, "Failed to create media events pool\n");
		return -1;
	}

	return 0;
}

void ftl_mngt_init_mem_pools(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	if (init_lba_map_pool(dev)) {
		ftl_mngt_fail_step(mngt);
	}

	if (init_band_md_pool(dev)) {
		ftl_mngt_fail_step(mngt);
	}

	if (init_media_events_pool(dev)) {
		ftl_mngt_fail_step(mngt);
	}

	ftl_mngt_next_step(mngt);
}

void ftl_mngt_deinit_mem_pools(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	if (dev->lba_pool) {
		ftl_mempool_destroy_ext(dev->lba_pool);
		dev->lba_pool = NULL;
	}

	if (dev->lba_pool_md) {
		assert(dev->lba_pool_md);
		ftl_md_destroy(dev->lba_pool_md, ftl_md_destroy_shm_flags(dev));
		dev->lba_pool_md = NULL;
	}

	if (dev->band_md_pool) {
		ftl_mempool_destroy(dev->band_md_pool);
		dev->band_md_pool = NULL;
	}

	if (dev->media_events_pool) {
		spdk_mempool_free(dev->media_events_pool);
		dev->media_events_pool = NULL;
	}

	ftl_mngt_next_step(mngt);
}

void ftl_mngt_init_reloc(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	dev->reloc = ftl_reloc_init(dev);
	if (!dev->reloc) {
		FTL_ERRLOG(dev, "Unable to initialize reloc structures\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

void ftl_mngt_deinit_reloc(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	ftl_reloc_free(dev->reloc);
	ftl_mngt_next_step(mngt);
}

void ftl_mngt_init_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	if (ftl_nv_cache_init(dev)) {
		FTL_ERRLOG(dev, "Unable to initialize persistent cache\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

void ftl_mngt_deinit_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	ftl_nv_cache_deinit(dev);
	ftl_mngt_next_step(mngt);
}

static void user_clear_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt *mngt = md->owner.cb_ctx;

	if (status) {
		FTL_ERRLOG(ftl_mngt_get_dev(mngt), "FTL NV Cache: ERROR of clearing user cache data\n");
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

void ftl_mngt_scrub_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_layout_region *region = &dev->layout.region[ftl_layout_region_type_data_nvc];
	struct ftl_md *md = dev->layout.md[ftl_layout_region_type_data_nvc];

	FTL_NOTICELOG(dev, "First startup needs to scrub nv cache data region, this may take some time.\n");
	FTL_NOTICELOG(dev, "Scrubbing %lluGiB\n", region->current.blocks * FTL_BLOCK_SIZE / GiB);

	/* Need to scrub user data, so in case of dirty shutdown the recovery won't
	 * pull in data during open chunks recovery from any previous instance
	 */
	md->cb = user_clear_cb;
	md->owner.cb_ctx = mngt;

	union ftl_md_vss vss;
	vss.version.md_version = region->current.version;
	vss.nv_cache.lba = FTL_ADDR_INVALID;
	uint64_t block_data = 0;
	ftl_md_clear(md, &block_data, sizeof(block_data), &vss);
}

void ftl_mngt_finalize_init(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	dev->initialized = 1;
	dev->sb_shm->shm_ready = true;

	ftl_reloc_resume(dev->reloc);
	ftl_writer_resume(&dev->writer_user);
	ftl_writer_resume(&dev->writer_gc);
	ftl_nv_cache_resume(&dev->nv_cache);

	ftl_mngt_next_step(mngt);
}

void ftl_mngt_start_task_core(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	dev->core_poller = SPDK_POLLER_REGISTER(ftl_task_core, dev, 0);
	if (!dev->core_poller) {
		FTL_ERRLOG(dev, "Unable to register core poller\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

void ftl_mngt_stop_task_core(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	dev->halt = true;

	if (dev->core_poller) {
		ftl_mngt_continue_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

void ftl_mngt_dump_stats(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	ftl_dev_dump_bands(dev);
	ftl_dev_dump_stats(dev);
	ftl_mngt_next_step(mngt);
}
