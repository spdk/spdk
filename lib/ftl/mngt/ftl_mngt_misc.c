/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_core.h"
#include "ftl_utils.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "ftl_band.h"
#include "ftl_internal.h"
#include "ftl_nv_cache.h"
#include "ftl_debug.h"

void
ftl_mngt_check_conf(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_conf_is_valid(&dev->conf)) {
		ftl_mngt_next_step(mngt);
	} else {
		ftl_mngt_fail_step(mngt);
	}
}

static int
init_p2l_map_pool(struct spdk_ftl_dev *dev)
{
	size_t p2l_pool_el_blks = spdk_divide_round_up(ftl_p2l_map_pool_elem_size(dev), FTL_BLOCK_SIZE);
	size_t p2l_pool_buf_blks = P2L_MEMPOOL_SIZE * p2l_pool_el_blks;
	void *p2l_pool_buf;

	dev->p2l_pool_md = ftl_md_create(dev, p2l_pool_buf_blks, 0, "p2l_pool",
					 FTL_MD_CREATE_SHM | FTL_MD_CREATE_SHM_NEW, NULL);
	if (!dev->p2l_pool_md) {
		return -ENOMEM;
	}

	p2l_pool_buf = ftl_md_get_buffer(dev->p2l_pool_md);
	dev->p2l_pool = ftl_mempool_create_ext(p2l_pool_buf, P2L_MEMPOOL_SIZE,
					       p2l_pool_el_blks * FTL_BLOCK_SIZE,
					       FTL_BLOCK_SIZE);
	if (!dev->p2l_pool) {
		return -ENOMEM;
	}

	ftl_mempool_initialize_ext(dev->p2l_pool);

	return 0;
}

static int
init_band_md_pool(struct spdk_ftl_dev *dev)
{
	dev->band_md_pool = ftl_mempool_create(P2L_MEMPOOL_SIZE,
					       sizeof(struct ftl_band_md),
					       FTL_BLOCK_SIZE,
					       SPDK_ENV_SOCKET_ID_ANY);
	if (!dev->band_md_pool) {
		return -ENOMEM;
	}

	return 0;
}

void
ftl_mngt_init_mem_pools(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (init_p2l_map_pool(dev)) {
		ftl_mngt_fail_step(mngt);
	}

	if (init_band_md_pool(dev)) {
		ftl_mngt_fail_step(mngt);
	}

	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_deinit_mem_pools(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (dev->p2l_pool) {
		ftl_mempool_destroy_ext(dev->p2l_pool);
		dev->p2l_pool = NULL;
	}

	if (dev->p2l_pool_md) {
		ftl_md_destroy(dev->p2l_pool_md);
		dev->p2l_pool_md = NULL;
	}

	if (dev->band_md_pool) {
		ftl_mempool_destroy(dev->band_md_pool);
		dev->band_md_pool = NULL;
	}

	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_init_reloc(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	dev->reloc = ftl_reloc_init(dev);
	if (!dev->reloc) {
		FTL_ERRLOG(dev, "Unable to initialize reloc structures\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_deinit_reloc(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_reloc_free(dev->reloc);
	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_init_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_nv_cache_init(dev)) {
		FTL_ERRLOG(dev, "Unable to initialize persistent cache\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_deinit_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_nv_cache_deinit(dev);
	ftl_mngt_next_step(mngt);
}

static void
user_clear_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt_process *mngt = md->owner.cb_ctx;

	if (status) {
		FTL_ERRLOG(ftl_mngt_get_dev(mngt), "FTL NV Cache: ERROR of clearing user cache data\n");
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

void
ftl_mngt_scrub_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_layout_region *region = &dev->layout.region[FTL_LAYOUT_REGION_TYPE_DATA_NVC];
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_DATA_NVC];
	union ftl_md_vss vss;

	FTL_NOTICELOG(dev, "First startup needs to scrub nv cache data region, this may take some time.\n");
	FTL_NOTICELOG(dev, "Scrubbing %lluGiB\n", region->current.blocks * FTL_BLOCK_SIZE / GiB);

	/* Need to scrub user data, so in case of dirty shutdown the recovery won't
	 * pull in data during open chunks recovery from any previous instance
	 */
	md->cb = user_clear_cb;
	md->owner.cb_ctx = mngt;

	vss.version.md_version = region->current.version;
	vss.nv_cache.lba = FTL_ADDR_INVALID;
	ftl_md_clear(md, 0, &vss);
}

void
ftl_mngt_finalize_startup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	dev->initialized = 1;
	dev->sb_shm->shm_ready = true;

	ftl_reloc_resume(dev->reloc);
	ftl_writer_resume(&dev->writer_user);
	ftl_writer_resume(&dev->writer_gc);
	ftl_nv_cache_resume(&dev->nv_cache);

	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_start_core_poller(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	dev->core_poller = SPDK_POLLER_REGISTER(ftl_core_poller, dev, 0);
	if (!dev->core_poller) {
		FTL_ERRLOG(dev, "Unable to register core poller\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_stop_core_poller(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	dev->halt = true;

	if (dev->core_poller) {
		ftl_mngt_continue_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

void
ftl_mngt_dump_stats(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_dev_dump_bands(dev);
	ftl_dev_dump_stats(dev);
	ftl_mngt_next_step(mngt);
}
