/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/bdev_module.h"
#include "spdk/ftl.h"

#include "ftl_nv_cache.h"
#include "ftl_internal.h"
#include "ftl_mngt_steps.h"
#include "ftl_internal.h"
#include "ftl_core.h"
#include "utils/ftl_defs.h"

#define MINIMUM_CACHE_SIZE_GIB 5
#define MINIMUM_BASE_SIZE_GIB 20

/*  Dummy bdev module used to to claim bdevs. */
static struct spdk_bdev_module g_ftl_bdev_module = {
	.name   = "ftl_lib",
};

static inline uint64_t
ftl_calculate_num_blocks_in_band(struct spdk_bdev_desc *desc)
{
	/* TODO: this should be passed via input parameter */
#ifdef SPDK_FTL_ZONE_EMU_BLOCKS
	return SPDK_FTL_ZONE_EMU_BLOCKS;
#else
	return (1ULL << 30) / FTL_BLOCK_SIZE;
#endif
}

static void
base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		assert(0);
		break;
	default:
		break;
	}
}

void
ftl_mngt_open_base_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	uint32_t block_size;
	uint64_t num_blocks;
	const char *bdev_name = dev->conf.base_bdev;
	struct spdk_bdev *bdev;

	if (spdk_bdev_open_ext(bdev_name, true, base_bdev_event_cb,
			       dev, &dev->base_bdev_desc)) {
		FTL_ERRLOG(dev, "Unable to open bdev: %s\n", bdev_name);
		goto error;
	}

	bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);

	if (spdk_bdev_module_claim_bdev(bdev, dev->base_bdev_desc, &g_ftl_bdev_module)) {
		/* clear the desc so that we don't try to release the claim on cleanup */
		spdk_bdev_close(dev->base_bdev_desc);
		dev->base_bdev_desc = NULL;
		FTL_ERRLOG(dev, "Unable to claim bdev %s\n", bdev_name);
		goto error;
	}

	block_size = spdk_bdev_get_block_size(bdev);
	if (block_size != FTL_BLOCK_SIZE) {
		FTL_ERRLOG(dev, "Unsupported block size (%"PRIu32")\n", block_size);
		goto error;
	}

	num_blocks = spdk_bdev_get_num_blocks(bdev);

	if (num_blocks * block_size < MINIMUM_BASE_SIZE_GIB * GiB) {
		FTL_ERRLOG(dev, "Bdev %s is too small, requires, at least %uGiB capacity\n",
			   spdk_bdev_get_name(bdev), MINIMUM_BASE_SIZE_GIB);
		goto error;
	}

	dev->base_ioch = spdk_bdev_get_io_channel(dev->base_bdev_desc);
	if (!dev->base_ioch) {
		FTL_ERRLOG(dev, "Failed to create base bdev IO channel\n");
		goto error;
	}

	dev->xfer_size = ftl_get_write_unit_size(bdev);
	if (dev->xfer_size != FTL_NUM_LBA_IN_BLOCK) {
		FTL_ERRLOG(dev, "Unsupported xfer_size (%"PRIu64")\n", dev->xfer_size);
		goto error;
	}

	/* TODO: validate size when base device VSS usage gets added */
	dev->md_size = spdk_bdev_get_md_size(bdev);

	/* Cache frequently used values */
	dev->num_blocks_in_band = ftl_calculate_num_blocks_in_band(dev->base_bdev_desc);
	dev->is_zoned = spdk_bdev_is_zoned(spdk_bdev_desc_get_bdev(dev->base_bdev_desc));

	if (dev->is_zoned) {
		/* TODO - current FTL code isn't fully compatible with ZNS drives */
		FTL_ERRLOG(dev, "Creating FTL on Zoned devices is not supported\n");
		goto error;
	}

	ftl_mngt_next_step(mngt);
	return;
error:
	ftl_mngt_fail_step(mngt);
}

void
ftl_mngt_close_base_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (dev->base_ioch) {
		spdk_put_io_channel(dev->base_ioch);
		dev->base_ioch = NULL;
	}

	if (dev->base_bdev_desc) {
		struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);

		spdk_bdev_module_release_bdev(bdev);
		spdk_bdev_close(dev->base_bdev_desc);

		dev->base_bdev_desc = NULL;
	}

	ftl_mngt_next_step(mngt);
}

static void
nv_cache_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		assert(0);
		break;
	default:
		break;
	}
}

void
ftl_mngt_open_cache_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct spdk_bdev *bdev;
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;
	const char *bdev_name = dev->conf.cache_bdev;

	if (spdk_bdev_open_ext(bdev_name, true, nv_cache_bdev_event_cb, dev,
			       &nv_cache->bdev_desc)) {
		FTL_ERRLOG(dev, "Unable to open bdev: %s\n", bdev_name);
		goto error;
	}

	bdev = spdk_bdev_desc_get_bdev(nv_cache->bdev_desc);

	if (spdk_bdev_module_claim_bdev(bdev, nv_cache->bdev_desc, &g_ftl_bdev_module)) {
		/* clear the desc so that we don't try to release the claim on cleanup */
		spdk_bdev_close(nv_cache->bdev_desc);
		nv_cache->bdev_desc = NULL;
		FTL_ERRLOG(dev, "Unable to claim bdev %s\n", bdev_name);
		goto error;
	}

	FTL_NOTICELOG(dev, "Using %s as write buffer cache\n", spdk_bdev_get_name(bdev));

	if (spdk_bdev_get_block_size(bdev) != FTL_BLOCK_SIZE) {
		FTL_ERRLOG(dev, "Unsupported block size (%d)\n",
			   spdk_bdev_get_block_size(bdev));
		goto error;
	}

	nv_cache->cache_ioch = spdk_bdev_get_io_channel(nv_cache->bdev_desc);
	if (!nv_cache->cache_ioch) {
		FTL_ERRLOG(dev, "Failed to create cache IO channel for NV Cache\n");
		goto error;
	}

#ifndef SPDK_FTL_VSS_EMU
	if (!spdk_bdev_is_md_separate(bdev)) {
		FTL_ERRLOG(dev, "Bdev %s doesn't support separate metadata buffer IO\n",
			   spdk_bdev_get_name(bdev));
		goto error;
	}

	nv_cache->md_size = spdk_bdev_get_md_size(bdev);
	if (nv_cache->md_size != sizeof(union ftl_md_vss)) {
		FTL_ERRLOG(dev, "Bdev's %s metadata is invalid size (%"PRIu32")\n",
			   spdk_bdev_get_name(bdev), spdk_bdev_get_md_size(bdev));
		goto error;
	}

	if (spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE) {
		FTL_ERRLOG(dev, "Unsupported DIF type used by bdev %s\n",
			   spdk_bdev_get_name(bdev));
		goto error;
	}

	if (bdev->blockcnt * bdev->blocklen < MINIMUM_CACHE_SIZE_GIB * GiB) {
		FTL_ERRLOG(dev, "Bdev %s is too small, requires, at least %uGiB capacity\n",
			   spdk_bdev_get_name(bdev), MINIMUM_CACHE_SIZE_GIB);
		goto error;
	}

	if (ftl_md_xfer_blocks(dev) * nv_cache->md_size > FTL_ZERO_BUFFER_SIZE) {
		FTL_ERRLOG(dev, "Zero buffer too small for bdev %s metadata transfer\n",
			   spdk_bdev_get_name(bdev));
		goto error;
	}
#else
	if (spdk_bdev_is_md_separate(bdev)) {
		FTL_ERRLOG(dev, "FTL VSS emulation but NV cache supports VSS\n");
		goto error;
	}

	nv_cache->md_size = 64;
	FTL_NOTICELOG(dev, "FTL uses VSS emulation\n");
#endif

	ftl_mngt_next_step(mngt);
	return;
error:
	ftl_mngt_fail_step(mngt);
}

void
ftl_mngt_close_cache_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (dev->nv_cache.cache_ioch) {
		spdk_put_io_channel(dev->nv_cache.cache_ioch);
		dev->nv_cache.cache_ioch = NULL;
	}

	if (dev->nv_cache.bdev_desc) {
		struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(dev->nv_cache.bdev_desc);

		spdk_bdev_module_release_bdev(bdev);
		spdk_bdev_close(dev->nv_cache.bdev_desc);

		dev->nv_cache.bdev_desc = NULL;
	}

	ftl_mngt_next_step(mngt);
}
