/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
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
#include "upgrade/ftl_layout_upgrade.h"
#include "upgrade/ftl_sb_upgrade.h"

void
ftl_mngt_init_layout(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_layout_setup(dev)) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

static bool
is_buffer_needed(enum ftl_layout_region_type type)
{
	switch (type) {
#ifdef SPDK_FTL_VSS_EMU
	case FTL_LAYOUT_REGION_TYPE_VSS:
#endif
	case FTL_LAYOUT_REGION_TYPE_SB:
	case FTL_LAYOUT_REGION_TYPE_SB_BASE:
	case FTL_LAYOUT_REGION_TYPE_DATA_NVC:
	case FTL_LAYOUT_REGION_TYPE_DATA_BASE:
	case FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR:
	case FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR:
#ifndef SPDK_FTL_L2P_FLAT
	case FTL_LAYOUT_REGION_TYPE_L2P:
#endif
	case FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR:
		return false;

	default:
		return true;
	}
}

void
ftl_mngt_init_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region = layout->region;
	uint64_t i;
	int md_flags;

	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; i++, region++) {
		if (layout->md[i]) {
			/*
			 * Some metadata objects are initialized by other FTL
			 * components. At the moment it's only used by superblock (and its mirror) -
			 * during load time we need to read it earlier in order to get the layout for the
			 * other regions.
			 */
			continue;
		}
		md_flags = is_buffer_needed(i) ? ftl_md_create_region_flags(dev,
				region->type) : FTL_MD_CREATE_NO_MEM;
		layout->md[i] = ftl_md_create(dev, region->current.blocks, region->vss_blksz, region->name,
					      md_flags, region);
		if (NULL == layout->md[i]) {
			ftl_mngt_fail_step(mngt);
			return;
		}
	}

	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_deinit_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region = layout->region;
	uint64_t i;

	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; i++, region++) {
		if (layout->md[i]) {
			ftl_md_destroy(layout->md[i], ftl_md_destroy_region_flags(dev, layout->region[i].type));
			layout->md[i] = NULL;
		}
	}

	ftl_mngt_next_step(mngt);
}

static void
persist_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt_process *mngt = md->owner.cb_ctx;

	if (status) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

static void
persist(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
	enum ftl_layout_region_type type)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_md *md;

	assert(type < FTL_LAYOUT_REGION_TYPE_MAX);

	md = layout->md[type];
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
	case FTL_LAYOUT_REGION_TYPE_NVC_MD:
		status = ftl_nv_cache_load_state(&dev->nv_cache);
		break;
	case FTL_LAYOUT_REGION_TYPE_VALID_MAP:
		ftl_valid_map_load_state(dev);
		break;
	case FTL_LAYOUT_REGION_TYPE_BAND_MD:
		ftl_bands_load_state(dev);
		break;
	default:
		break;
	}
	return status;
}

static void
restore_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt_process *mngt = md->owner.cb_ctx;
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

static void
restore(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt, enum ftl_layout_region_type type)
{
	struct ftl_layout *layout = &dev->layout;
	assert(type < FTL_LAYOUT_REGION_TYPE_MAX);
	struct ftl_md *md = layout->md[type];

	if (!md) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	md->owner.cb_ctx = mngt;
	md->cb = restore_cb;
	ftl_md_restore(md);
}

void
ftl_mngt_persist_nv_cache_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_nv_cache_save_state(&dev->nv_cache)) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	persist(dev, mngt, FTL_LAYOUT_REGION_TYPE_NVC_MD);
}

static void
ftl_mngt_fast_persist_nv_cache_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_nv_cache_save_state(&dev->nv_cache)) {
		ftl_mngt_fail_step(mngt);
		return;
	}
	ftl_mngt_next_step(mngt);
}

static void
ftl_mngt_persist_vld_map_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	persist(dev, mngt, FTL_LAYOUT_REGION_TYPE_VALID_MAP);
}

static void
ftl_mngt_persist_p2l_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	/* Sync runtime P2L to persist any invalidation that may have happened */

	struct ftl_p2l_sync_ctx *ctx = ftl_mngt_get_step_ctx(mngt);

	/*
	 * ftl_mngt_persist_bands_p2l will increment the md_region before the step_continue for next regions
	 */
	if (ctx->md_region <= FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN) {
		ctx->md_region = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_MIN;
	}
	ftl_mngt_persist_bands_p2l(mngt);
}

void
ftl_mngt_persist_band_info_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	persist(dev, mngt, FTL_LAYOUT_REGION_TYPE_BAND_MD);
}

static void
ftl_mngt_persist_trim_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	persist(dev, mngt, FTL_LAYOUT_REGION_TYPE_TRIM_MD);
}

static uint32_t
get_sb_crc(struct ftl_superblock *sb)
{
	uint32_t crc = 0;

	/* Calculate CRC excluding CRC field in superblock */
	void *buffer = sb;
	size_t offset = offsetof(struct ftl_superblock, header.crc);
	size_t size = offset;
	crc = spdk_crc32c_update(buffer, size, crc);

	buffer += offset + sizeof(sb->header.crc);
	if (sb->header.version > FTL_SB_VERSION_2) {
		/* whole buf for v3 and on: */
		size = FTL_SUPERBLOCK_SIZE - offset - sizeof(sb->header.crc);
		crc = spdk_crc32c_update(buffer, size, crc);
	} else {
		/* special for sb v2 only: */
		size = sizeof(struct ftl_superblock_v2) - offset - sizeof(sb->header.crc);
		sb->header.crc = spdk_crc32c_update(buffer, size, crc);
	}

	return crc;
}

static void
ftl_mngt_persist_super_block(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	dev->sb->overprovisioning = dev->conf.overprovisioning;
	dev->sb->gc_info = dev->sb_shm->gc_info;
	dev->sb->header.crc = get_sb_crc(dev->sb);
	persist(dev, mngt, FTL_LAYOUT_REGION_TYPE_SB);
}

#ifdef SPDK_FTL_VSS_EMU
static void
ftl_mngt_persist_vss(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	persist(dev, mngt, FTL_LAYOUT_REGION_TYPE_VSS);
}
#endif

/*
 * Persists all necessary metadata (band state, P2L, etc) during FTL's clean shutdown.
 */
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
			.ctx_size = sizeof(struct ftl_p2l_sync_ctx),
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

void
ftl_mngt_persist_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_mngt_call_process(mngt, &desc_persist);
}

/*
 * Fast clean shutdown path - skips the persistence of most metadata regions and
 * relies on their shared memory state instead.
 */
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

void
ftl_mngt_fast_persist_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_mngt_call_process(mngt, &desc_fast_persist);
}

void
ftl_mngt_init_default_sb(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_superblock *sb = dev->sb;

	sb->header.magic = FTL_SUPERBLOCK_MAGIC;
	sb->header.version = FTL_SB_VERSION_CURRENT;
	sb->uuid = dev->conf.uuid;
	sb->clean = 0;
	dev->sb_shm->shm_clean = false;
	sb->ckpt_seq_id = 0;

	/* Max 16 IO depth per band relocate */
	sb->max_reloc_qdepth = 16;

	sb->overprovisioning = dev->conf.overprovisioning;

	ftl_band_init_gc_iter(dev);

	/* md layout isn't initialized yet.
	 * empty region list => all regions in the default location */
	sb->md_layout_head.type = FTL_LAYOUT_REGION_TYPE_INVALID;
	sb->md_layout_head.df_next = FTL_DF_OBJ_ID_INVALID;

	sb->header.crc = get_sb_crc(sb);

	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_set_dirty(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_superblock *sb = dev->sb;

	sb->clean = 0;
	dev->sb_shm->shm_clean = false;
	sb->header.crc = get_sb_crc(sb);
	persist(dev, mngt, FTL_LAYOUT_REGION_TYPE_SB);
}

void
ftl_mngt_set_clean(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_superblock *sb = dev->sb;

	sb->clean = 1;
	dev->sb_shm->shm_clean = false;
	sb->header.crc = get_sb_crc(sb);
	persist(dev, mngt, FTL_LAYOUT_REGION_TYPE_SB);

	dev->sb_shm->shm_ready = false;
}

void
ftl_mngt_set_shm_clean(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_superblock *sb = dev->sb;

	sb->clean = 1;
	dev->sb_shm->shm_clean = true;
	sb->header.crc = get_sb_crc(sb);
	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_load_sb(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	FTL_NOTICELOG(dev, "SHM: clean %"PRIu64", shm_clean %d\n", dev->sb->clean, dev->sb_shm->shm_clean);

	if (!ftl_fast_startup(dev)) {
		restore(dev, mngt, FTL_LAYOUT_REGION_TYPE_SB);
		return;
	}

	FTL_DEBUGLOG(dev, "SHM: found SB\n");
	if (ftl_md_restore_region(dev, FTL_LAYOUT_REGION_TYPE_SB)) {
		ftl_mngt_fail_step(mngt);
		return;
	}
	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_validate_sb(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
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

	if (ftl_superblock_upgrade(dev)) {
		FTL_ERRLOG(dev, "FTL superblock dirty or invalid version\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	if (spdk_uuid_compare(&sb->uuid, &dev->conf.uuid) != 0) {
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
	if (sb->overprovisioning == 0 || sb->overprovisioning >= 100) {
		FTL_ERRLOG(dev, "Invalid FTL superblock lba_rsvd\n");
		ftl_mngt_fail_step(mngt);
		return;
	}
	dev->conf.overprovisioning = sb->overprovisioning;

	ftl_mngt_next_step(mngt);
}

/*
 * Loads and verifies superblock contents - utilized during the load of an FTL
 * instance (both from a clean and dirty shutdown).
 */
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

/*
 * Initializes the superblock fields during first startup of FTL
 */
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
void
ftl_mngt_md_init_vss_emu(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region = &layout->region[FTL_LAYOUT_REGION_TYPE_VSS];

	/* Initialize VSS layout */
	ftl_layout_setup_vss_emu(dev);

	/* Allocate md buf */
	layout->md[FTL_LAYOUT_REGION_TYPE_VSS] = ftl_md_create(dev, region->current.blocks,
			region->vss_blksz, NULL, FTL_MD_CREATE_HEAP, region);
	if (NULL == layout->md[FTL_LAYOUT_REGION_TYPE_VSS]) {
		ftl_mngt_fail_step(mngt);
		return;
	}
	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_md_deinit_vss_emu(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_layout *layout = &dev->layout;

	if (layout->md[FTL_LAYOUT_REGION_TYPE_VSS]) {
		ftl_md_destroy(layout->md[FTL_LAYOUT_REGION_TYPE_VSS], 0);
		layout->md[FTL_LAYOUT_REGION_TYPE_VSS] = NULL;
	}

	ftl_mngt_next_step(mngt);
}
#endif

void
ftl_mngt_superblock_init(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region = &layout->region[FTL_LAYOUT_REGION_TYPE_SB];
	char uuid[SPDK_UUID_STRING_LEN];
	int md_create_flags = ftl_md_create_region_flags(dev, FTL_LAYOUT_REGION_TYPE_SB);

	/* Must generate UUID before MD create on SHM for the SB */
	if (dev->conf.mode & SPDK_FTL_MODE_CREATE) {
		spdk_uuid_generate(&dev->conf.uuid);
		spdk_uuid_fmt_lower(uuid, sizeof(uuid), &dev->conf.uuid);
		FTL_NOTICELOG(dev, "Create new FTL, UUID %s\n", uuid);
	}

shm_retry:
	/* Allocate md buf */
	dev->sb_shm = NULL;
	dev->sb_shm_md = ftl_md_create(dev, spdk_divide_round_up(sizeof(*dev->sb_shm), FTL_BLOCK_SIZE),
				       0, "sb_shm",
				       md_create_flags, NULL);
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
	layout->md[FTL_LAYOUT_REGION_TYPE_SB] = ftl_md_create(dev, region->current.blocks,
						region->vss_blksz, region->name,
						md_create_flags, region);
	if (NULL == layout->md[FTL_LAYOUT_REGION_TYPE_SB]) {
		/* The first attempt may fail when trying to open SHM - try to create new */
		if ((md_create_flags & FTL_MD_CREATE_SHM_NEW) == 0) {
			md_create_flags |= FTL_MD_CREATE_SHM_NEW;
			ftl_md_destroy(dev->sb_shm_md, 0);
			dev->sb_shm_md = NULL;
			goto shm_retry;
		}
		ftl_mngt_fail_step(mngt);
		return;
	}

	/* Link the md buf to the device */
	dev->sb = ftl_md_get_buffer(layout->md[FTL_LAYOUT_REGION_TYPE_SB]);

	/* Setup superblock mirror to QLC */
	region = &layout->region[FTL_LAYOUT_REGION_TYPE_SB_BASE];
	layout->md[FTL_LAYOUT_REGION_TYPE_SB_BASE] = ftl_md_create(dev, region->current.blocks,
			region->vss_blksz, NULL, FTL_MD_CREATE_HEAP, region);
	if (NULL == layout->md[FTL_LAYOUT_REGION_TYPE_SB_BASE]) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	/* Initialize the superblock */
	if (dev->conf.mode & SPDK_FTL_MODE_CREATE) {
		ftl_mngt_call_process(mngt, &desc_init_sb);
	} else {
		ftl_mngt_call_process(mngt, &desc_restore_sb);
	}
}

void
ftl_mngt_superblock_deinit(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_layout *layout = &dev->layout;

	if (layout->md[FTL_LAYOUT_REGION_TYPE_SB]) {
		ftl_md_destroy(layout->md[FTL_LAYOUT_REGION_TYPE_SB],
			       ftl_md_destroy_region_flags(dev, FTL_LAYOUT_REGION_TYPE_SB));
		layout->md[FTL_LAYOUT_REGION_TYPE_SB] = NULL;
	}

	if (layout->md[FTL_LAYOUT_REGION_TYPE_SB_BASE]) {
		ftl_md_destroy(layout->md[FTL_LAYOUT_REGION_TYPE_SB_BASE], 0);
		layout->md[FTL_LAYOUT_REGION_TYPE_SB_BASE] = NULL;
	}

	ftl_md_destroy(dev->sb_shm_md, ftl_md_destroy_shm_flags(dev));
	dev->sb_shm_md = NULL;
	dev->sb_shm = NULL;

	ftl_mngt_next_step(mngt);
}

static void
ftl_mngt_restore_nv_cache_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_fast_startup(dev)) {
		FTL_DEBUGLOG(dev, "SHM: found nv cache md\n");
		if (ftl_md_restore_region(dev, FTL_LAYOUT_REGION_TYPE_NVC_MD)) {
			ftl_mngt_fail_step(mngt);
			return;
		}
		ftl_mngt_next_step(mngt);
		return;
	}
	restore(dev, mngt, FTL_LAYOUT_REGION_TYPE_NVC_MD);
}

static void
ftl_mngt_restore_vld_map_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_fast_startup(dev)) {
		FTL_DEBUGLOG(dev, "SHM: found vldmap\n");
		if (ftl_md_restore_region(dev, FTL_LAYOUT_REGION_TYPE_VALID_MAP)) {
			ftl_mngt_fail_step(mngt);
			return;
		}
		ftl_mngt_next_step(mngt);
		return;
	}
	restore(dev, mngt, FTL_LAYOUT_REGION_TYPE_VALID_MAP);
}

static void
ftl_mngt_restore_band_info_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_fast_startup(dev)) {
		FTL_DEBUGLOG(dev, "SHM: found band md\n");
		if (ftl_md_restore_region(dev, FTL_LAYOUT_REGION_TYPE_BAND_MD)) {
			ftl_mngt_fail_step(mngt);
			return;
		}
		ftl_mngt_next_step(mngt);
		return;
	}
	restore(dev, mngt, FTL_LAYOUT_REGION_TYPE_BAND_MD);
}

static void
ftl_mngt_restore_trim_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_fast_startup(dev)) {
		FTL_DEBUGLOG(dev, "SHM: found trim md\n");
		if (ftl_md_restore_region(dev, FTL_LAYOUT_REGION_TYPE_TRIM_MD)) {
			ftl_mngt_fail_step(mngt);
			return;
		}
		ftl_mngt_next_step(mngt);
		return;
	}
	restore(dev, mngt, FTL_LAYOUT_REGION_TYPE_TRIM_MD);
}



#ifdef SPDK_FTL_VSS_EMU
static void
ftl_mngt_restore_vss_metadata(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	restore(dev, mngt, FTL_LAYOUT_REGION_TYPE_VSS);
}
#endif

/*
 * Loads metadata after a clean shutdown.
 */
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

void
ftl_mngt_restore_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_mngt_call_process(mngt, &desc_restore);
}

void
ftl_mngt_persist_superblock(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	dev->sb->header.crc = get_sb_crc(dev->sb);
	persist(dev, mngt, FTL_LAYOUT_REGION_TYPE_SB);
}
