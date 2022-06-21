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

#include "spdk/bdev.h"

#include "ftl_core.h"
#include "ftl_utils.h"
#include "ftl_band.h"
#include "ftl_layout.h"
#include "ftl_nv_cache.h"
#include "ftl_sb.h"

#define FTL_NV_CACHE_CHUNK_DATA_SIZE(blocks) ((uint64_t)blocks * FTL_BLOCK_SIZE)
#define FTL_NV_CACHE_CHUNK_SIZE(blocks) \
	(FTL_NV_CACHE_CHUNK_DATA_SIZE(blocks) + (2 * FTL_NV_CACHE_CHUNK_META_SIZE))

static inline float blocks2mib(uint64_t blocks)
{
	float result;

	result = blocks;
	result *= FTL_BLOCK_SIZE;
	result /= 1024UL;
	result /= 1024UL;

	return result;
}

#define FTL_LAYOUT_REGION_ALIGNMENT_BLOCKS 32ULL
#define FTL_LAYOUT_REGION_ALIGNMENT_BYTES (FTL_LAYOUT_REGION_ALIGNMENT_BLOCKS * FTL_BLOCK_SIZE)


static inline uint64_t blocks_region(uint64_t bytes)
{
	const uint64_t alignment = FTL_LAYOUT_REGION_ALIGNMENT_BYTES;
	uint64_t result;

	result = spdk_divide_round_up(bytes, alignment);
	result *= alignment;
	result /= FTL_BLOCK_SIZE;

	return result;
}

static void dump_region(struct spdk_ftl_dev *dev, struct ftl_layout_region *region)
{
	assert(!(region->current.offset % FTL_LAYOUT_REGION_ALIGNMENT_BLOCKS));
	assert(!(region->current.blocks % FTL_LAYOUT_REGION_ALIGNMENT_BLOCKS));

	FTL_NOTICELOG(dev, "Region %s\n", region->name);
	FTL_NOTICELOG(dev, "	offset:                      %.2f MiB\n",
		      blocks2mib(region->current.offset));
	FTL_NOTICELOG(dev, "	blocks:                      %.2f MiB\n",
		      blocks2mib(region->current.blocks));
}

int validate_regions(struct spdk_ftl_dev *dev, struct ftl_layout *layout)
{
	uint64_t i, j;

	/* Validate if regions doesn't overlap each other  */
	// TODO: major upgrades: keep track of and validate free_nvc/free_btm regions
	for (i = 0; i < ftl_layout_region_type_max; i++) {
		struct ftl_layout_region *r1 = &layout->region[i];

		for (j = 0; j < ftl_layout_region_type_max; j++) {
			struct ftl_layout_region *r2 = &layout->region[j];

			if (r1->bdev_desc != r2->bdev_desc) {
				continue;
			}

			if (i == j) {
				continue;
			}

			uint64_t r1_begin = r1->current.offset;
			uint64_t r1_end = r1->current.offset + r1->current.blocks - 1;
			uint64_t r2_begin = r2->current.offset;
			uint64_t r2_end = r2->current.offset + r2->current.blocks - 1;

			if (spdk_max(r1_begin, r2_begin) <= spdk_min(r1_end, r2_end)) {
				FTL_ERRLOG(dev, "Layout initialization ERROR, two regions overlap, "
					   "%s and %s\n", r1->name, r2->name);
				return -1;
			}
		}
	}

	return 0;
}

static uint64_t get_num_lbas(struct spdk_ftl_dev *dev)
{
	uint64_t blocks = 0;

	blocks = dev->layout.btm.total_blocks;
	blocks = (blocks * (100 - dev->conf.lba_rsvd)) / 100;


	return blocks;
}

static void set_region_version(struct ftl_layout_region *reg, uint64_t version)
{
	reg->current.version = reg->prev.version = version;
}

static void set_region_bdev_nvc(struct ftl_layout_region *reg, struct spdk_ftl_dev *dev)
{
	reg->bdev_desc = dev->nv_cache.bdev_desc;
	reg->ioch = dev->nv_cache.cache_ioch;
	reg->vss_blksz = dev->nv_cache.md_size;
}

static void set_region_bdev_btm(struct ftl_layout_region *reg, struct spdk_ftl_dev *dev)
{
	reg->bdev_desc = dev->base_bdev_desc;
	reg->ioch = dev->base_ioch;
	reg->vss_blksz = 0;
}

static int setup_layout_nvc(struct spdk_ftl_dev *dev)
{
	uint64_t left, offset = 0;
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region, *mirror;

#ifdef SPDK_FTL_VSS_EMU
	/* Skip the already init`d VSS region */
	region = &layout->region[ftl_layout_region_type_vss];
	offset += region->current.blocks;

	if (offset >= layout->nvc.total_blocks) {
		goto ERROR;
	}
#endif

	/* Skip the superblock region. Already init`d in ftl_layout_setup_superblock */
	region = &layout->region[ftl_layout_region_type_sb];
	offset += region->current.blocks;

	/* Initialize L2P region */
	if (offset >= layout->nvc.total_blocks) {
		goto ERROR;
	}
	region = &layout->region[ftl_layout_region_type_l2p];
	region->type = ftl_layout_region_type_l2p;
	region->name = "l2p";
	set_region_version(region, 0);
	region->current.offset = offset;
	region->current.blocks = blocks_region(layout->l2p.addr_size * dev->num_lbas);
	set_region_bdev_nvc(region, dev);
	offset += region->current.blocks;

	/* Initialize band info metadata */
	if (offset >= layout->nvc.total_blocks) {
		goto ERROR;
	}
	region = &layout->region[ftl_layout_region_type_band_md];
	region->type = ftl_layout_region_type_band_md;
	region->mirror_type = ftl_layout_region_type_band_md_mirror;
	region->name = "band_md";
	set_region_version(region, FTL_BAND_VERSION_CURRENT);
	region->current.offset = offset;
	region->current.blocks = blocks_region(ftl_get_num_bands(dev) * sizeof(struct ftl_band_md));
	region->entry_size = sizeof(struct ftl_band_md) / FTL_BLOCK_SIZE;
	region->num_entries = ftl_get_num_bands(dev);
	set_region_bdev_nvc(region, dev);
	offset += region->current.blocks;

	/* Initialize band info metadata mirror */
	if (offset >= layout->nvc.total_blocks) {
		goto ERROR;
	}
	mirror = &layout->region[ftl_layout_region_type_band_md_mirror];
	*mirror = *region;
	mirror->type = ftl_layout_region_type_band_md_mirror;
	mirror->mirror_type = ftl_layout_region_type_invalid;
	mirror->name = "band_md_mirror";
	mirror->current.offset += region->current.blocks;
	offset += mirror->current.blocks;

	if (offset >= layout->nvc.total_blocks) {
		goto ERROR;
	}

	/*
	 * Initialize NV Cache metadata
	 */
	if (offset >= layout->nvc.total_blocks) {
		goto ERROR;
	}

	left = layout->nvc.total_blocks - offset;
	layout->nvc.chunk_data_blocks =
		FTL_NV_CACHE_CHUNK_DATA_SIZE(ftl_get_num_blocks_in_zone(dev)) / FTL_BLOCK_SIZE;
	layout->nvc.chunk_meta_size = FTL_NV_CACHE_CHUNK_META_SIZE;
	layout->nvc.chunk_count = (left * FTL_BLOCK_SIZE) /
				  FTL_NV_CACHE_CHUNK_SIZE(ftl_get_num_blocks_in_zone(dev));
	layout->nvc.chunk_tail_md_num_blocks = ftl_nv_cache_chunk_tail_md_num_blocks(&dev->nv_cache);

	if (0 == layout->nvc.chunk_count) {
		goto ERROR;
	}
	region = &layout->region[ftl_layout_region_type_nvc_md];
	region->type = ftl_layout_region_type_nvc_md;
	region->mirror_type = ftl_layout_region_type_nvc_md_mirror;
	region->name = "nvc_md";
	set_region_version(region, FTL_NVC_VERSION_CURRENT);
	region->current.offset = offset;
	region->current.blocks = blocks_region(layout->nvc.chunk_count *
				       sizeof(struct ftl_nv_cache_chunk_md));
	region->entry_size = sizeof(struct ftl_nv_cache_chunk_md) / FTL_BLOCK_SIZE;
	region->num_entries = layout->nvc.chunk_count;
	set_region_bdev_nvc(region, dev);
	offset += region->current.blocks;

	/*
	 * Initialize NV Cache metadata mirror
	 */
	mirror = &layout->region[ftl_layout_region_type_nvc_md_mirror];
	*mirror = *region;
	mirror->type = ftl_layout_region_type_nvc_md_mirror;
	mirror->mirror_type = ftl_layout_region_type_invalid;
	mirror->name = "nvc_md_mirror";
	mirror->current.offset += region->current.blocks;
	offset += mirror->current.blocks;

	/*
	 * Initialize data region on NV cache
	 */
	if (offset >= layout->nvc.total_blocks) {
		goto ERROR;
	}
	region = &layout->region[ftl_layout_region_type_data_nvc];
	region->type = ftl_layout_region_type_data_nvc;
	region->name = "data_nvc";
	set_region_version(region, 0);
	region->current.offset = offset;
	region->current.blocks = layout->nvc.chunk_count * layout->nvc.chunk_data_blocks;
	set_region_bdev_nvc(region, dev);
	offset += region->current.blocks;

	left = layout->nvc.total_blocks - offset;
	if (left > layout->nvc.chunk_data_blocks) {
		FTL_ERRLOG(dev, "Error when setup NV cache layout\n");
		return -1;
	}

	if (offset > layout->nvc.total_blocks) {
		FTL_ERRLOG(dev, "Error when setup NV cache layout\n");
		return -1;
	}

	return 0;

ERROR:
	FTL_ERRLOG(dev, "Insufficient NV Cache capacity to preserve metadata\n");
	return -1;
}

static ftl_addr layout_base_offset(struct spdk_ftl_dev *dev)
{
	ftl_addr addr;

	addr = dev->num_bands * ftl_get_num_blocks_in_band(dev);
	return addr;
}

static int setup_layout_base(struct spdk_ftl_dev *dev)
{
	uint64_t left, offset;
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region;

	/* Base device layout is following:
	 * - data
	 * - superblock
	 * - valid map
	 *
	 * Super block has been already configured, and it begin is the end of data region.
	 */
	offset = layout->region[ftl_layout_region_type_sb_btm].current.offset;

	/* Setup data region on base device */
	region = &layout->region[ftl_layout_region_type_data_btm];
	region->type = ftl_layout_region_type_data_btm;
	region->name = "data_btm";
	set_region_version(region, 0);
	region->current.offset = 0;
	region->current.blocks = offset;
	set_region_bdev_btm(region, dev);

	/* Move offset after base superblock */
	offset += layout->region[ftl_layout_region_type_sb_btm].current.blocks;

	left = layout->btm.total_blocks - offset;
	if (left > layout->btm.total_blocks) {
		FTL_ERRLOG(dev, "Error when setup base device layout\n");
		return -1;
	}

	if (offset > layout->btm.total_blocks) {
		FTL_ERRLOG(dev, "Error when setup base device layout\n");
		return -1;
	}

	return 0;
}

int ftl_layout_setup(struct spdk_ftl_dev *dev)
{
	const struct spdk_bdev *bdev;
	struct ftl_layout *layout = &dev->layout;
	uint64_t i;
	uint64_t num_lbas;

	bdev = spdk_bdev_desc_get_bdev(dev->base_bdev_desc);
	layout->btm.total_blocks = spdk_bdev_get_num_blocks(bdev);

	bdev = spdk_bdev_desc_get_bdev(dev->nv_cache.bdev_desc);
	layout->nvc.total_blocks = spdk_bdev_get_num_blocks(bdev);

	/* Initialize mirrors types */
	for (i = 0; i < ftl_layout_region_type_max; ++i) {
		if (i == ftl_layout_region_type_sb) {
			/* Super block has been already initialized */
			continue;
		}

		layout->region[i].mirror_type = ftl_layout_region_type_invalid;
	}

	/*
	 * Initialize L2P information
	 */
	num_lbas = get_num_lbas(dev);
	if (dev->num_lbas == 0) {
		assert(dev->conf.mode & SPDK_FTL_MODE_CREATE);
		dev->num_lbas = num_lbas;
		dev->sb->lba_cnt = num_lbas;
	} else if (dev->num_lbas != num_lbas) {
		FTL_ERRLOG(dev, "Mismatched FTL num_lbas\n");
		return -1;
	}
	layout->l2p.addr_length = spdk_u64log2(layout->btm.total_blocks + layout->nvc.total_blocks) + 1;
	layout->l2p.addr_size = layout->l2p.addr_length > 32 ? 8 : 4;
	layout->l2p.lbas_in_page = FTL_BLOCK_SIZE / layout->l2p.addr_size;

	if (setup_layout_nvc(dev)) {
		return -1;
	}

	if (setup_layout_base(dev)) {
		return -1;
	}

	if (validate_regions(dev, layout)) {
		return -1;
	}

	FTL_NOTICELOG(dev, "Base device capacity:         %.2f MiB\n",
		      blocks2mib(layout->btm.total_blocks));
	FTL_NOTICELOG(dev, "NV cache device capacity:       %.2f MiB\n",
		      blocks2mib(layout->nvc.total_blocks));
	FTL_NOTICELOG(dev, "L2P entries:                    %"PRIu64"\n",
		      dev->num_lbas);
	FTL_NOTICELOG(dev, "L2P address size:               %"PRIu64"\n",
		      layout->l2p.addr_size);

	return 0;
}

#ifdef SPDK_FTL_VSS_EMU
void ftl_layout_setup_vss_emu(struct spdk_ftl_dev *dev)
{
	const struct spdk_bdev *bdev;
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region = &layout->region[ftl_layout_region_type_vss];

	assert(layout->md[ftl_layout_region_type_vss] == NULL);

	region = &layout->region[ftl_layout_region_type_vss];
	region->type = ftl_layout_region_type_vss;
	region->name = "vss";
	set_region_version(region, 0);
	region->current.offset = 0;

	bdev = spdk_bdev_desc_get_bdev(dev->nv_cache.bdev_desc);
	layout->nvc.total_blocks = spdk_bdev_get_num_blocks(bdev);
	region->current.blocks = blocks_region(dev->nv_cache.md_size * layout->nvc.total_blocks);

	region->vss_blksz = 0;
	region->bdev_desc = dev->nv_cache.bdev_desc;
	region->ioch = dev->nv_cache.cache_ioch;

	assert(region->bdev_desc != NULL);
	assert(region->ioch != NULL);
}
#endif

int ftl_layout_setup_superblock(struct spdk_ftl_dev *dev)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region = &layout->region[ftl_layout_region_type_sb];

	assert(layout->md[ftl_layout_region_type_sb] == NULL);

	/* Initialize superblock region */
	region->type = ftl_layout_region_type_sb;
	region->mirror_type = ftl_layout_region_type_sb_btm;
	region->name = "sb";
	set_region_version(region, FTL_METADATA_VERSION_CURRENT);
	region->current.offset = 0;

	/*
	 * VSS region must go first in case SB restore accesses VSS
	 * in context of calls ftl_nv_cache_bdev_read_blocks_with_md()
	 */
#ifdef SPDK_FTL_VSS_EMU
	region->current.offset = layout->region[ftl_layout_region_type_vss].current.offset +
			 layout->region[ftl_layout_region_type_vss].current.blocks;
#endif

	region->current.blocks = blocks_region(FTL_SUPERBLOCK_SIZE);
	region->vss_blksz = 0;
	region->bdev_desc = dev->nv_cache.bdev_desc;
	region->ioch = dev->nv_cache.cache_ioch;

	assert(region->bdev_desc != NULL);
	assert(region->ioch != NULL);

	region = &layout->region[ftl_layout_region_type_sb_btm];
	region->type = ftl_layout_region_type_sb_btm;
	region->mirror_type = ftl_layout_region_type_max;
	region->name = "sb_mirror";
	set_region_version(region, FTL_METADATA_VERSION_CURRENT);
	region->current.offset = layout_base_offset(dev);
	region->current.blocks = blocks_region(FTL_SUPERBLOCK_SIZE);
	set_region_bdev_btm(region, dev);

	/* Check if SB can be stored at the end of base device */
	uint64_t total_blocks = spdk_bdev_get_num_blocks(
					spdk_bdev_desc_get_bdev(dev->base_bdev_desc));
	uint64_t offset = region->current.offset + region->current.blocks;
	uint64_t left = total_blocks - offset;
	if ((left > total_blocks) || (offset > total_blocks)) {
		FTL_ERRLOG(dev, "Error when setup base device super block\n");
		return -1;
	}

	return 0;
}

void layout_dump(struct spdk_ftl_dev *dev)
{
	struct ftl_layout *layout = &dev->layout;
	int i;
	FTL_NOTICELOG(dev, "NV cache layout:\n");
	for (i = 0; i < ftl_layout_region_type_max; ++i) {
		if (layout->region[i].bdev_desc == dev->nv_cache.bdev_desc) {
			dump_region(dev, &layout->region[i]);
		}
	}
	FTL_NOTICELOG(dev, "Bottom device layout:\n");
	for (i = 0; i < ftl_layout_region_type_max; ++i) {
		if (layout->region[i].bdev_desc == dev->base_bdev_desc) {
			dump_region(dev, &layout->region[i]);
		}
	}
}
