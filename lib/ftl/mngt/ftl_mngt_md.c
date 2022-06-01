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
#include "ftl_band.h"
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
	case ftl_layout_region_type_band_md_mirror:
#ifndef SPDK_FTL_L2P_FLAT
	case ftl_layout_region_type_l2p:
#endif
	case ftl_layout_region_type_trim_md_mirror:
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
		int md_flags = is_md(i) ? ftl_md_create_region_flags(dev, region->type) : FTL_MD_CREATE_NO_MEM;
		layout->md[i] = ftl_md_create(dev, region->current.blocks, region->vss_blksz, region->name, md_flags);
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
			ftl_md_destroy(layout->md[i], ftl_md_destroy_region_flags(dev, layout->region[i].type));
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

static int
ftl_md_restore_region(struct spdk_ftl_dev *dev, int region_type)
{
	int status = 0;
	switch (region_type) {
	case ftl_layout_region_type_nvc_md:
		status = ftl_nv_cache_load_state(&dev->nv_cache);
		break;
	case ftl_layout_region_type_valid_map:
		ftl_valid_map_load_state(dev);
		break;
	case ftl_layout_region_type_band_md:
		ftl_bands_load_state(dev);
		break;
	default:
		break;
	}
	return status;
}

static void restore_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt *mngt = md->owner.cb_ctx;
	const struct ftl_layout_region *region = ftl_md_get_region(md);

	if (status) {
		/* Restore error, end step */
		ftl_mngt_fail_step(mngt);
		return;
	}

	assert(region);
	status = ftl_md_restore_region(dev, region->type);

	if (status) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

static void restore(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt,
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
	md->cb = restore_cb;
	ftl_md_restore(md);
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

static void ftl_mngt_fast_persist_nv_cache_metadata(
	struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	if (ftl_nv_cache_save_state(&dev->nv_cache)) {
		ftl_mngt_fail_step(mngt);
		return;
	}
	ftl_mngt_next_step(mngt);
}

static void ftl_mngt_persist_vld_map_metadata(
	struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	persist(dev, mngt, ftl_layout_region_type_valid_map);
}

static void ftl_mngt_persist_p2l_metadata(
	struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	/* Sync runtime P2L to persist any invalidation that may have happened */

	struct ftl_p2l_sync_ctx *ctx = ftl_mngt_get_step_cntx(mngt);

	/* If enum is changed so ftl_layout_region_type_p2l_ckpt_min == 0, we need the equality check.
	 * ftl_mngt_persist_bands_p2l will increment the md_region before the step_continue for next regions */
	if (ctx->md_region <= ftl_layout_region_type_p2l_ckpt_min) {
		ctx->md_region = ftl_layout_region_type_p2l_ckpt_min;
	}
	ftl_mngt_persist_bands_p2l(mngt);
}

void ftl_mngt_persist_band_info_metadata(
	struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	persist(dev, mngt, ftl_layout_region_type_band_md);
}

static void ftl_mngt_persist_trim_metadata(
	struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	persist(dev, mngt, ftl_layout_region_type_trim_md);
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

static void ftl_mngt_persist_super_block(
	struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	dev->sb->lba_rsvd = dev->conf.lba_rsvd;
	dev->sb->use_append = dev->conf.use_append;
	dev->sb->gc_info = dev->sb_shm->gc_info;
	dev->sb->header.crc = get_sb_crc(dev->sb);
	persist(dev, mngt, ftl_layout_region_type_sb);
}

#ifdef SPDK_FTL_VSS_EMU
static void ftl_mngt_persist_vss(
	struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	persist(dev, mngt, ftl_layout_region_type_vss);
}
#endif

static const struct ftl_mngt_process_desc desc_persist = {
	.name = "Persist metadata",
	.steps = {
		{
			.name = "Persist NV cache metadata",
			.action = ftl_mngt_persist_nv_cache_metadata,
		},
		{
			.name = "Persist valid map metadata",
			.action = ftl_mngt_persist_vld_map_metadata,
		},
		{
			.name = "Persist P2L metadata",
			.action = ftl_mngt_persist_p2l_metadata,
			.arg_size = sizeof(struct ftl_p2l_sync_ctx),
		},
		{
			.name = "persist band info metadata",
			.action = ftl_mngt_persist_band_info_metadata,
		},
		{
			.name = "persist trim metadata",
			.action = ftl_mngt_persist_trim_metadata,
		},
		{
			.name = "Persist superblock",
			.action = ftl_mngt_persist_super_block,
		},
#ifdef SPDK_FTL_VSS_EMU
		{
			.name = "Persist VSS metadata",
			.action = ftl_mngt_persist_vss,
		},
#endif
		{}
	}
};

void ftl_mngt_persist_md(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	ftl_mngt_call(mngt, &desc_persist);
}

static const struct ftl_mngt_process_desc desc_fast_persist = {
	.name = "Fast persist metadata",
	.steps = {
		{
			.name = "Fast persist NV cache metadata",
			.action = ftl_mngt_fast_persist_nv_cache_metadata,
		},
#ifdef SPDK_FTL_VSS_EMU
		{
			.name = "Persist VSS metadata",
			.action = ftl_mngt_persist_vss,
		},
#endif
		{}
	}
};

void ftl_mngt_fast_persist_md(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	ftl_mngt_call(mngt, &desc_fast_persist);
}

void ftl_mngt_init_default_sb(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_superblock *sb = dev->sb;

	sb->header.magic = FTL_SUPERBLOCK_MAGIC;
	sb->header.version = FTL_METADATA_VERSION_CURRENT;
	sb->uuid = dev->uuid;
	sb->clean = 0;
	dev->sb_shm->shm_clean = false;
	sb->ckpt_seq_id = 0;

	/* Max 12 IO depth per band relocate */
	sb->max_reloc_qdepth = 16;

	sb->max_io_channels = 128;

	sb->lba_rsvd = dev->conf.lba_rsvd;
	sb->use_append = dev->conf.use_append;

	ftl_band_init_gc_iter(dev);

	// md layout isn't initialized yet.
	// empty region list => all regions in the default location
	sb->md_layout_head.type = ftl_layout_region_type_invalid;
	sb->md_layout_head.df_next = FTL_DF_OBJ_ID_INVALID;

	sb->header.crc = get_sb_crc(sb);

	ftl_mngt_next_step(mngt);
}

void ftl_mngt_set_dirty(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_superblock *sb = dev->sb;

	sb->clean = 0;
	dev->sb_shm->shm_clean = false;
	sb->header.crc = get_sb_crc(sb);
	persist(dev, mngt, ftl_layout_region_type_sb);
}

void ftl_mngt_set_clean(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_superblock *sb = dev->sb;

	sb->clean = 1;
	dev->sb_shm->shm_clean = false;
	sb->header.crc = get_sb_crc(sb);
	persist(dev, mngt, ftl_layout_region_type_sb);

	dev->sb_shm->shm_ready = false;
}

void ftl_mngt_set_shm_clean(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_superblock *sb = dev->sb;

	sb->clean = 1;
	dev->sb_shm->shm_clean = true;
	sb->header.crc = get_sb_crc(sb);
	ftl_mngt_next_step(mngt);
}

void ftl_mngt_load_sb(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	FTL_NOTICELOG(dev, "SHM: clean %"PRIu64", shm_clean %d\n", dev->sb->clean, dev->sb_shm->shm_clean);
	if (ftl_fast_startup(dev)) {
		FTL_NOTICELOG(dev, "SHM: found SB\n");
		if (ftl_md_restore_region(dev, ftl_layout_region_type_sb)) {
			ftl_mngt_fail_step(mngt);
			return;
		}
		ftl_mngt_next_step(mngt);
		return;
	}
	restore(dev, mngt, ftl_layout_region_type_sb);
}

void ftl_mngt_validate_sb(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_superblock *sb = dev->sb;

	if (!ftl_superblock_check_magic(sb)) {
		FTL_ERRLOG(dev, "Invalid FTL superblock magic\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	if (sb->header.crc != get_sb_crc(sb)) {
		FTL_ERRLOG(dev, "Invalid FTL superblock CRC\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	if (spdk_uuid_compare(&sb->uuid, &dev->uuid) != 0) {
		FTL_ERRLOG(dev, "Invalid FTL superblock UUID\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	if (sb->lba_cnt == 0) {
		FTL_ERRLOG(dev, "Invalid FTL superblock lba_cnt\n");
		ftl_mngt_fail_step(mngt);
		return;
	}
	dev->num_lbas = sb->lba_cnt;

	/* The sb has just been read. Validate and update the conf */
	if (sb->lba_rsvd == 0 || sb->lba_rsvd >= 100) {
		FTL_ERRLOG(dev, "Invalid FTL superblock lba_rsvd\n");
		ftl_mngt_fail_step(mngt);
		return;
	}
	dev->conf.lba_rsvd = sb->lba_rsvd;

	dev->conf.use_append = sb->use_append;

	ftl_mngt_next_step(mngt);
}

static const struct ftl_mngt_process_desc desc_restore_sb = {
	.name = "SB restore",
	.steps = {
		{
			.name = "Load super block",
			.action = ftl_mngt_load_sb
		},
		{
			.name = "Validate super block",
			.action = ftl_mngt_validate_sb
		},
		{}
	}
};

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
		ftl_md_destroy(layout->md[ftl_layout_region_type_vss], 0);
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
	int md_create_flags = ftl_md_create_region_flags(dev, ftl_layout_region_type_sb);

	/* Must generate UUID before MD create on SHM for the SB */
	if (dev->conf.mode & SPDK_FTL_MODE_CREATE) {
		spdk_uuid_generate(&dev->uuid);
		spdk_uuid_fmt_lower(uuid, sizeof(uuid), &dev->uuid);
		FTL_NOTICELOG(dev, "Create new FTL, UUID %s\n", uuid);
	}

shm_retry:
	/* Allocate md buf */
	dev->sb_shm = NULL;
	dev->sb_shm_md = ftl_md_create(dev, spdk_divide_round_up(sizeof(*dev->sb_shm), FTL_BLOCK_SIZE),
				       0, "sb_shm",
				       md_create_flags);
	if (dev->sb_shm_md == NULL) {
		/* The first attempt may fail when trying to open SHM - try to create new */
		if ((md_create_flags & FTL_MD_CREATE_SHM_NEW) == 0) {
			md_create_flags |= FTL_MD_CREATE_SHM_NEW;
			goto shm_retry;
		}
		if (dev->sb_shm_md == NULL) {
			ftl_mngt_fail_step(mngt);
			return;
		}
	}

	dev->sb_shm = ftl_md_get_buffer(dev->sb_shm_md);

	/* Setup the layout of a superblock */
	if (ftl_layout_setup_superblock(dev)) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	/* Allocate md buf */
	layout->md[ftl_layout_region_type_sb] = ftl_md_create(dev, region->current.blocks,
						region->vss_blksz, region->name,
						md_create_flags);
	if (NULL == layout->md[ftl_layout_region_type_sb]) {
		/* The first attempt may fail when trying to open SHM - try to create new */
		if ((md_create_flags & FTL_MD_CREATE_SHM_NEW) == 0) {
			md_create_flags |= FTL_MD_CREATE_SHM_NEW;
			ftl_md_destroy(dev->sb_shm_md, 0);
			goto shm_retry;
		}
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
			region->vss_blksz, NULL, 0);
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
		ftl_mngt_call(mngt, &desc_restore_sb);
	}
}

void ftl_mngt_superblock_deinit(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	struct ftl_layout *layout = &dev->layout;

	if (layout->md[ftl_layout_region_type_sb]) {
		ftl_md_destroy(layout->md[ftl_layout_region_type_sb],
			       ftl_md_destroy_region_flags(dev, ftl_layout_region_type_sb));
		layout->md[ftl_layout_region_type_sb] = NULL;
	}

	if (layout->md[ftl_layout_region_type_sb_btm]) {
		ftl_md_destroy(layout->md[ftl_layout_region_type_sb_btm], 0);
		layout->md[ftl_layout_region_type_sb_btm] = NULL;
	}

	ftl_md_destroy(dev->sb_shm_md, ftl_md_destroy_shm_flags(dev));
	dev->sb_shm_md = NULL;
	dev->sb_shm = NULL;

	ftl_mngt_next_step(mngt);
}

static void ftl_mngt_restore_nv_cache_metadata(struct spdk_ftl_dev *dev,
		struct ftl_mngt *mngt)
{
	if (ftl_fast_startup(dev)) {
		FTL_NOTICELOG(dev, "SHM: found nv cache md\n");
		if (ftl_md_restore_region(dev, ftl_layout_region_type_nvc_md)) {
			ftl_mngt_fail_step(mngt);
			return;
		}
		ftl_mngt_next_step(mngt);
		return;
	}
	restore(dev, mngt, ftl_layout_region_type_nvc_md);
}

static void ftl_mngt_restore_vld_map_metadata(struct spdk_ftl_dev *dev,
		struct ftl_mngt *mngt)
{
	if (ftl_fast_startup(dev)) {
		FTL_NOTICELOG(dev, "SHM: found vldmap\n");
		if (ftl_md_restore_region(dev, ftl_layout_region_type_valid_map)) {
			ftl_mngt_fail_step(mngt);
			return;
		}
		ftl_mngt_next_step(mngt);
		return;
	}
	restore(dev, mngt, ftl_layout_region_type_valid_map);
}

static void ftl_mngt_restore_band_info_metadata(struct spdk_ftl_dev *dev,
		struct ftl_mngt *mngt)
{
	if (ftl_fast_startup(dev)) {
		FTL_NOTICELOG(dev, "SHM: found band md\n");
		if (ftl_md_restore_region(dev, ftl_layout_region_type_band_md)) {
			ftl_mngt_fail_step(mngt);
			return;
		}
		ftl_mngt_next_step(mngt);
		return;
	}
	restore(dev, mngt, ftl_layout_region_type_band_md);
}

static void ftl_mngt_restore_trim_metadata(struct spdk_ftl_dev *dev,
		struct ftl_mngt *mngt)
{
	if (ftl_fast_startup(dev)) {
		FTL_NOTICELOG(dev, "SHM: found trim md\n");
		if (ftl_md_restore_region(dev, ftl_layout_region_type_trim_md)) {
			ftl_mngt_fail_step(mngt);
			return;
		}
		ftl_mngt_next_step(mngt);
		return;
	}
	restore(dev, mngt, ftl_layout_region_type_trim_md);
}



#ifdef SPDK_FTL_VSS_EMU
static void ftl_mngt_restore_vss_metadata(struct spdk_ftl_dev *dev,
		struct ftl_mngt *mngt)
{
	restore(dev, mngt, ftl_layout_region_type_vss);
}
#endif

static const struct ftl_mngt_process_desc desc_restore = {
	.name = "Restore metadata",
	.steps = {
#ifdef SPDK_FTL_VSS_EMU
		{
			.name = "Restore VSS metadata",
			.action = ftl_mngt_restore_vss_metadata,
		},
#endif
		{
			.name = "Restore NV cache metadata",
			.action = ftl_mngt_restore_nv_cache_metadata,
		},
		{
			.name = "Restore valid map metadata",
			.action = ftl_mngt_restore_vld_map_metadata,
		},
		{
			.name = "Restore band info metadata",
			.action = ftl_mngt_restore_band_info_metadata,
		},
		{
			.name = "Restore trim metadata",
			.action = ftl_mngt_restore_trim_metadata,
		},
		{}
	}
};

void ftl_mngt_restore_md(struct spdk_ftl_dev *dev, struct ftl_mngt *mngt)
{
	ftl_mngt_call(mngt, &desc_restore);
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
