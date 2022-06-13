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

#include "spdk/thread.h"
#include "spdk/crc32.h"

#include "ftl_core.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "ftl_utils.h"
#include "ftl_internal.h"
#include "ftl_sb.h"

void ftl_mngt_init_layout(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	if (ftl_layout_setup(dev)) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

static bool is_md(enum ftl_layout_region_type type)
{
	switch (type) {
#ifdef SPDK_FTL_VSS_EMU
	case ftl_layout_region_type_vss:
#endif
	case ftl_layout_region_type_sb:
	case ftl_layout_region_type_sb_btm:
	case ftl_layout_region_type_data_nvc:
	case ftl_layout_region_type_data_btm:
	case ftl_layout_region_type_nvc_md_mirror:
		return false;

	default:
		return true;
	}
}

void ftl_mngt_init_md(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region = layout->region;
	uint64_t i;

	for (i = 0; i < ftl_layout_region_type_max; i++, region++) {
		if (layout->md[i]) {
			/*
			* Some metadata objects are initialized by other FTL
			* components, e.g. L2P is set by L2P impl itself.
			*/
			continue;
		}
		layout->md[i] = ftl_md_create(dev, region->current.blocks, region->vss_blksz, region->name, !is_md(i));
		if (NULL == layout->md[i]) {
			ftl_mngt_fail_step(mngt);
			return;
		}
		if (ftl_md_set_region(layout->md[i], region)) {
			ftl_mngt_fail_step(mngt);
			return;
		}
	}

	ftl_mngt_next_step(mngt);
}

void ftl_mngt_deinit_md(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region = layout->region;
	uint64_t i;

	for (i = 0; i < ftl_layout_region_type_max; i++, region++) {
		if (layout->md[i]) {
			ftl_md_destroy(layout->md[i]);
			layout->md[i] = NULL;
		}
	}

	ftl_mngt_next_step(mngt);
}

static void persist_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt *mngt = md->owner.cb_ctx;

	if (status) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

static void persist(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt,
		    enum ftl_layout_region_type type)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_md *md = layout->md[type];

	assert(type < ftl_layout_region_type_max);

	if (!md) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	md->owner.cb_ctx = mngt;
	md->cb = persist_cb;
	ftl_md_persist(md);
}

void ftl_mngt_persist_nv_cache_metadata(
	struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	if (ftl_nv_cache_save_state(&dev->nv_cache)) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	persist(dev, mngt, ftl_layout_region_type_nvc_md);
}

static uint32_t get_sb_crc(struct ftl_superblock *sb)
{
	uint32_t crc = 0;

	/* Calculate CRC excluding CRC field in superblock */
	void *buffer = sb;
	size_t offset = offsetof(struct ftl_superblock, header.crc);
	size_t size = offset;
	crc = spdk_crc32c_update(buffer, size, crc);

	buffer += offset + sizeof(sb->header.crc);
	size = FTL_SUPERBLOCK_SIZE - offset - sizeof(sb->header.crc);
	crc = spdk_crc32c_update(buffer, size, crc);

	return crc;
}

void ftl_mngt_init_default_sb(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_superblock *sb = dev->sb;

	sb->header.magic = FTL_SUPERBLOCK_MAGIC;
	sb->header.version = FTL_METADATA_VERSION_CURRENT;
	sb->uuid = dev->uuid;
	sb->clean = 0;

	/* Max 12 IO depth per band relocate */
	sb->max_reloc_qdepth = 16;

	sb->max_io_channels = 128;

	sb->lba_rsvd = dev->conf.lba_rsvd;
	sb->use_append = dev->conf.use_append;

	// md layout isn't initialized yet.
	// empty region list => all regions in the default location
	sb->md_layout_head.type = ftl_layout_region_type_invalid;

	sb->header.crc = get_sb_crc(sb);

	ftl_mngt_next_step(mngt);
}

void ftl_mngt_set_dirty(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_superblock *sb = dev->sb;

	sb->clean = 0;
	sb->header.crc = get_sb_crc(sb);
	persist(dev, mngt, ftl_layout_region_type_sb);
}

static const struct ftl_mngt_process_desc desc_init_sb = {
	.name = "SB initialize",
	.steps = {
		{
			.name = "Default-initialize superblock",
			.action = ftl_mngt_init_default_sb,
		},
		{}
	}
};

#ifdef SPDK_FTL_VSS_EMU
void ftl_mngt_md_init_vss_emu(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region = &layout->region[ftl_layout_region_type_vss];

	/* Initialize VSS layout */
	ftl_layout_setup_vss_emu(dev);

	/* Allocate md buf */
	layout->md[ftl_layout_region_type_vss] = ftl_md_create(dev, region->current.blocks,
			region->vss_blksz, NULL, 0);
	if (NULL == layout->md[ftl_layout_region_type_vss]) {
		ftl_mngt_fail_step(mngt);
		return;
	}
	ftl_md_set_region(layout->md[ftl_layout_region_type_vss], region);
	ftl_mngt_next_step(mngt);
}

void ftl_mngt_md_deinit_vss_emu(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_layout *layout = &dev->layout;

	if (layout->md[ftl_layout_region_type_vss]) {
		ftl_md_destroy(layout->md[ftl_layout_region_type_vss]);
		layout->md[ftl_layout_region_type_vss] = NULL;
	}

	ftl_mngt_next_step(mngt);
}
#endif

void ftl_mngt_superblock_init(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region = &layout->region[ftl_layout_region_type_sb];
	char uuid[SPDK_UUID_STRING_LEN];

	/* Must generate UUID before MD create on SHM for the SB */
	if (dev->conf.mode & SPDK_FTL_MODE_CREATE) {
		spdk_uuid_generate(&dev->uuid);
		spdk_uuid_fmt_lower(uuid, sizeof(uuid), &dev->uuid);
		FTL_NOTICELOG(dev, "Create new FTL, UUID %s\n", uuid);
	}

	/* Setup the layout of a superblock */
	if (ftl_layout_setup_superblock(dev)) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	/* Allocate md buf */
	layout->md[ftl_layout_region_type_sb] = ftl_md_create(dev, region->current.blocks,
						region->vss_blksz, region->name, false);
	if (NULL == layout->md[ftl_layout_region_type_sb]) {
		ftl_mngt_fail_step(mngt);
		return;
	}
	if (ftl_md_set_region(layout->md[ftl_layout_region_type_sb], region)) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	/* Link the md buf to the device */
	dev->sb = ftl_md_get_buffer(layout->md[ftl_layout_region_type_sb]);

	/* Setup superblock mirror to QLC */
	region = &layout->region[ftl_layout_region_type_sb_btm];
	layout->md[ftl_layout_region_type_sb_btm] = ftl_md_create(dev, region->current.blocks,
			region->vss_blksz, NULL, false);
	if (NULL == layout->md[ftl_layout_region_type_sb_btm]) {
		ftl_mngt_fail_step(mngt);
		return;
	}
	if (ftl_md_set_region(layout->md[ftl_layout_region_type_sb_btm], region)) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	/* Initialize the superblock */
	if (dev->conf.mode & SPDK_FTL_MODE_CREATE) {
		ftl_mngt_call(mngt, &desc_init_sb);
	} else {
		ftl_mngt_fail_step(mngt);
	}
}

void ftl_mngt_superblock_deinit(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_layout *layout = &dev->layout;

	if (layout->md[ftl_layout_region_type_sb]) {
		ftl_md_destroy(layout->md[ftl_layout_region_type_sb]);
		layout->md[ftl_layout_region_type_sb] = NULL;
	}

	if (layout->md[ftl_layout_region_type_sb_btm]) {
		ftl_md_destroy(layout->md[ftl_layout_region_type_sb_btm]);
		layout->md[ftl_layout_region_type_sb_btm] = NULL;
	}

	ftl_mngt_next_step(mngt);
}

static const struct ftl_mngt_process_desc desc_update_sb = {
	.name = "FTL update superblock",
	.steps = {
		{
			.name = "Update superblock",
			.action = ftl_mngt_set_dirty
		},
		{}
	}
};

void ftl_mngt_update_supeblock(struct spdk_ftl_dev *dev,
			       ftl_mngt_fn cb, void *cb_cntx)
{
	ftl_mngt_execute(dev, &desc_update_sb, cb, cb_cntx);
}

void ftl_mngt_persist_superblock(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	dev->sb->header.crc = get_sb_crc(dev->sb);
	persist(dev, mngt, ftl_layout_region_type_sb);
}
