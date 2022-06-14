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

#include "spdk/bdev_module.h"
#include "spdk/ftl.h"

#include "ftl_internal.h"
#include "ftl_mngt_steps.h"
#include "ftl_internal.h"
#include "ftl_core.h"

/*  Dummy bdev module used to to claim bdevs. */
static struct spdk_bdev_module g_ftl_bdev_module = {
	.name   = "ftl_lib",
};

static void base_bdev_event_cb(enum spdk_bdev_event_type type,
			       struct spdk_bdev *bdev,
			       void *event_ctx)
{
	struct spdk_ftl_dev *dev = event_ctx;

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		assert(0);
		break;
	case SPDK_BDEV_EVENT_MEDIA_MANAGEMENT:
		assert(bdev == spdk_bdev_desc_get_bdev(dev->base_bdev_desc));
		ftl_get_media_events(dev);
		break;
	default:
		break;
	}
}

void ftl_mngt_open_base_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	uint32_t block_size;
	uint64_t num_blocks;
	const char *bdev_name = dev->conf.base_bdev;
	struct spdk_bdev *bdev;

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (!bdev) {
		FTL_ERRLOG(dev, "Unable to find bdev: %s\n", bdev_name);
		goto error;
	}

	if (spdk_bdev_open_ext(bdev_name, true, base_bdev_event_cb,
			       dev, &dev->base_bdev_desc)) {
		FTL_ERRLOG(dev, "Unable to open bdev: %s\n", bdev_name);
		goto error;
	}

	if (spdk_bdev_module_claim_bdev(bdev, dev->base_bdev_desc,
					&g_ftl_bdev_module)) {
		spdk_bdev_close(dev->base_bdev_desc);
		dev->base_bdev_desc = NULL;
		FTL_ERRLOG(dev, "Unable to claim bdev %s\n", bdev_name);
		goto error;
	}

	dev->base_ioch = spdk_bdev_get_io_channel(dev->base_bdev_desc);
	if (!dev->base_ioch) {
		FTL_ERRLOG(dev, "Failed to create base bdev IO channel\n");
		goto error;
	}


	dev->xfer_size = ftl_get_write_unit_size(bdev);
	dev->md_size = spdk_bdev_get_md_size(bdev);

	block_size = spdk_bdev_get_block_size(bdev);
	if (block_size != FTL_BLOCK_SIZE) {
		FTL_ERRLOG(dev, "Unsupported block size (%"PRIu32")\n", block_size);
		goto error;
	}

	/* Cache frequently used values */
	dev->num_blocks_in_band = ftl_calculate_num_blocks_in_band(dev->base_bdev_desc);
	dev->num_punits = ftl_calculate_num_punits(dev->base_bdev_desc);
	dev->num_blocks_in_zone = ftl_calculate_num_blocks_in_zone(dev->base_bdev_desc);
	dev->is_zoned = spdk_bdev_is_zoned(spdk_bdev_desc_get_bdev(dev->base_bdev_desc));

	num_blocks = spdk_bdev_get_num_blocks(bdev);
	if (num_blocks % ftl_get_num_punits(dev)) {
		FTL_ERRLOG(dev, "Unsupported geometry. Base bdev block count must be multiple "
			   "of optimal number of zones.\n");
		goto error;
	}

	if (ftl_is_append_supported(dev) &&
	    !spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_ZONE_APPEND)) {
		FTL_ERRLOG(dev, "Bdev dosen't support append: %s\n",
			   spdk_bdev_get_name(bdev));
		goto error;
	}

	dev->num_bands = num_blocks / (ftl_get_num_punits(dev) *
				       ftl_get_num_blocks_in_zone(dev));

	/* Save a band worth of space for metadata */
	dev->num_bands--;

	ftl_mngt_next_step(mngt);
	return;
error:
	ftl_mngt_fail_step(mngt);
}

void ftl_mngt_close_base_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	if (dev->base_ioch) {
		spdk_put_io_channel(dev->base_ioch);
		dev->base_ioch = NULL;
	}

	if (dev->base_bdev_desc) {
		struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(
						 dev->base_bdev_desc);
		spdk_bdev_module_release_bdev(bdev);
		spdk_bdev_close(dev->base_bdev_desc);

		dev->base_bdev_desc = NULL;
	}

	ftl_mngt_next_step(mngt);
}

static void
nv_cache_bdev_event_cb(enum spdk_bdev_event_type type,
		       struct spdk_bdev *bdev, void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		assert(0);
		break;
	default:
		break;
	}
}

void ftl_mngt_open_cache_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct spdk_bdev *bdev;
	const char *bdev_name = dev->conf.cache_bdev;

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (!bdev) {
		FTL_ERRLOG(dev, "Unable to find bdev: %s\n", bdev_name);
		goto error;
	}

	if (spdk_bdev_open_ext(bdev_name, true, nv_cache_bdev_event_cb, dev,
			       &dev->cache_bdev_desc)) {
		FTL_ERRLOG(dev, "Unable to open bdev: %s\n", bdev_name);
		goto error;
	}

	if (spdk_bdev_module_claim_bdev(bdev, dev->cache_bdev_desc,
					&g_ftl_bdev_module)) {
		spdk_bdev_close(dev->cache_bdev_desc);
		dev->cache_bdev_desc = NULL;
		FTL_ERRLOG(dev, "Unable to claim bdev %s\n", bdev_name);
		goto error;
	}

	FTL_NOTICELOG(dev, "Using %s as write buffer cache\n", spdk_bdev_get_name(bdev));

	if (spdk_bdev_get_block_size(bdev) != FTL_BLOCK_SIZE) {
		FTL_ERRLOG(dev, "Unsupported block size (%d)\n",
			   spdk_bdev_get_block_size(bdev));
		goto error;
	}

	dev->cache_ioch = spdk_bdev_get_io_channel(dev->cache_bdev_desc);
	if (!dev->cache_ioch) {
		FTL_ERRLOG(dev, "Failed to create cache IO channel for NV Cache\n");
		goto error;
	}

#ifndef SPDK_FTL_VSS_EMU
	if (!spdk_bdev_is_md_separate(bdev)) {
		FTL_ERRLOG(dev, "Bdev %s doesn't support separate metadata buffer IO\n",
			   spdk_bdev_get_name(bdev));
		goto error;
	}

	dev->cache_md_size = spdk_bdev_get_md_size(bdev);
	if (dev->cache_md_size != sizeof(union ftl_md_vss)) {
		FTL_ERRLOG(dev, "Bdev's %s metadata is invalid size (%"PRIu32")\n",
			    spdk_bdev_get_name(bdev), spdk_bdev_get_md_size(bdev));
		goto error;
	}

	if (spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE) {
		FTL_ERRLOG(dev, "Unsupported DIF type used by bdev %s\n",
			   spdk_bdev_get_name(bdev));
		goto error;
	}

	if (ftl_md_xfer_blocks(dev) * dev->cache_md_size > FTL_ZERO_BUFFER_SIZE) {
		FTL_ERRLOG(dev, "Zero buffer too small for bdev %s metadata transfer\n",
			   spdk_bdev_get_name(bdev));
		goto error;
	}
#else
	if (spdk_bdev_is_md_separate(bdev)) {
		FTL_ERRLOG(dev, "FTL VSS emulation but NV cache supports VSS\n");
		goto error;
	}

	dev->cache_md_size = 64;
	FTL_NOTICELOG(dev, "FTL uses VSS emulation\n");
#endif

	ftl_mngt_next_step(mngt);
	return;
error:
	ftl_mngt_fail_step(mngt);
}

void ftl_mngt_close_cache_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	if (dev->cache_ioch) {
		spdk_put_io_channel(dev->cache_ioch);
		dev->cache_ioch = NULL;
	}

	if (dev->cache_bdev_desc) {
		spdk_bdev_module_release_bdev(
			spdk_bdev_desc_get_bdev(dev->cache_bdev_desc));
		spdk_bdev_close(dev->cache_bdev_desc);
	}

	ftl_mngt_next_step(mngt);
}
