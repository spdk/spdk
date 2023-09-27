/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/bdev_module.h"
#include "spdk/ftl.h"

#include "ftl_nv_cache.h"
#include "ftl_internal.h"
#include "ftl_mngt_steps.h"
#include "ftl_core.h"
#include "utils/ftl_defs.h"
#include "utils/ftl_layout_tracker_bdev.h"

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

	dev->base_type = ftl_base_device_get_type_by_bdev(dev, bdev);
	if (!dev->base_type) {
		FTL_ERRLOG(dev, "Failed to get base device type\n");
		goto error;
	}
	/* TODO: validate size when base device VSS usage gets added */
	dev->md_size = spdk_bdev_get_md_size(bdev);

	if (!dev->base_type->ops.md_layout_ops.region_create) {
		FTL_ERRLOG(dev, "Base device doesn't implement md_layout_ops\n");
		goto error;
	}

	/* Cache frequently used values */
	dev->num_blocks_in_band = ftl_calculate_num_blocks_in_band(dev->base_bdev_desc);
	dev->is_zoned = spdk_bdev_is_zoned(spdk_bdev_desc_get_bdev(dev->base_bdev_desc));

	if (dev->is_zoned) {
		/* TODO - current FTL code isn't fully compatible with ZNS drives */
		FTL_ERRLOG(dev, "Creating FTL on Zoned devices is not supported\n");
		goto error;
	}

	dev->base_layout_tracker = ftl_layout_tracker_bdev_init(spdk_bdev_get_num_blocks(bdev));
	if (!dev->base_layout_tracker) {
		FTL_ERRLOG(dev, "Failed to instantiate layout tracker for base device\n");
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

	if (dev->base_layout_tracker) {
		ftl_layout_tracker_bdev_fini(dev->base_layout_tracker);
		dev->base_layout_tracker = NULL;
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
	const struct ftl_md_layout_ops *md_ops;

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

	if (bdev->blockcnt * bdev->blocklen < MINIMUM_CACHE_SIZE_GIB * GiB) {
		FTL_ERRLOG(dev, "Bdev %s is too small, requires, at least %uGiB capacity\n",
			   spdk_bdev_get_name(bdev), MINIMUM_CACHE_SIZE_GIB);
		goto error;
	}
	nv_cache->md_size = spdk_bdev_get_md_size(bdev);

	/* Get FTL NVC bdev descriptor */
	nv_cache->nvc_desc = ftl_nv_cache_device_get_desc_by_bdev(dev, bdev);
	if (!nv_cache->nvc_desc) {
		FTL_ERRLOG(dev, "Failed to get NV Cache device descriptor\n");
		goto error;
	}
	nv_cache->md_size = sizeof(union ftl_md_vss);

	md_ops = &nv_cache->nvc_desc->ops.md_layout_ops;
	if (!md_ops->region_create) {
		FTL_ERRLOG(dev, "NV Cache device doesn't implement md_layout_ops\n");
		goto error;
	}

	dev->nvc_layout_tracker = ftl_layout_tracker_bdev_init(spdk_bdev_get_num_blocks(bdev));
	if (!dev->nvc_layout_tracker) {
		FTL_ERRLOG(dev, "Failed to instantiate layout tracker for nvc device\n");
		goto error;
	}

	FTL_NOTICELOG(dev, "Using %s as NV Cache device\n", nv_cache->nvc_desc->name);
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

	if (dev->nvc_layout_tracker) {
		ftl_layout_tracker_bdev_fini(dev->nvc_layout_tracker);
		dev->nvc_layout_tracker = NULL;
	}

	ftl_mngt_next_step(mngt);
}
