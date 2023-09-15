/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include <sys/queue.h>

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "common/lib/test_env.c"

#include "ftl/utils/ftl_layout_tracker_bdev.c"
#include "ftl/upgrade/ftl_sb_v3.c"
#include "ftl/upgrade/ftl_sb_v5.c"
#include "ftl/ftl_sb.c"
#include "ftl/ftl_layout.c"
#include "ftl/upgrade/ftl_sb_upgrade.c"

static struct ftl_layout_upgrade_desc_list layout_upgrade_desc[] = {
	[FTL_LAYOUT_REGION_TYPE_SB] = {
		.latest_ver = FTL_SB_VERSION_CURRENT,
		.count = FTL_SB_VERSION_CURRENT,
	},
	[FTL_LAYOUT_REGION_TYPE_SB_BASE] = {
		.latest_ver = FTL_SB_VERSION_CURRENT,
		.count = FTL_SB_VERSION_CURRENT,
	},
	[FTL_LAYOUT_REGION_TYPE_L2P] = {},
	[FTL_LAYOUT_REGION_TYPE_BAND_MD] = {
		.latest_ver = FTL_BAND_VERSION_CURRENT,
		.count = FTL_BAND_VERSION_CURRENT,
	},
	[FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR] = {
		.latest_ver = FTL_BAND_VERSION_CURRENT,
		.count = FTL_BAND_VERSION_CURRENT,
	},
	[FTL_LAYOUT_REGION_TYPE_VALID_MAP] = {},
	[FTL_LAYOUT_REGION_TYPE_NVC_MD] = {
		.latest_ver = FTL_NVC_VERSION_CURRENT,
		.count = FTL_NVC_VERSION_CURRENT,
	},
	[FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR] = {
		.latest_ver = FTL_NVC_VERSION_CURRENT,
		.count = FTL_NVC_VERSION_CURRENT,
	},
	[FTL_LAYOUT_REGION_TYPE_DATA_NVC] = {},
	[FTL_LAYOUT_REGION_TYPE_DATA_BASE] = {},
	[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC] = {
		.latest_ver = FTL_P2L_VERSION_CURRENT,
		.count = FTL_P2L_VERSION_CURRENT,
	},
	[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC_NEXT] = {
		.latest_ver = FTL_P2L_VERSION_CURRENT,
		.count = FTL_P2L_VERSION_CURRENT,
	},
	[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_COMP] = {
		.latest_ver = FTL_P2L_VERSION_CURRENT,
		.count = FTL_P2L_VERSION_CURRENT,
	},
	[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_COMP_NEXT] = {
		.latest_ver = FTL_P2L_VERSION_CURRENT,
		.count = FTL_P2L_VERSION_CURRENT,
	},
	[FTL_LAYOUT_REGION_TYPE_TRIM_MD] = {},
	[FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR] = {},
};

#include "ftl/upgrade/ftl_layout_upgrade.c"
#include "ftl/mngt/ftl_mngt_md.c"

DEFINE_STUB_V(ftl_mngt_fail_step, (struct ftl_mngt_process *mngt));
DEFINE_STUB_V(ftl_mngt_next_step, (struct ftl_mngt_process *mngt));
DEFINE_STUB_V(ftl_md_persist, (struct ftl_md *md));
DEFINE_STUB(ftl_nv_cache_load_state, int, (struct ftl_nv_cache *nv_cache), 0);
DEFINE_STUB_V(ftl_valid_map_load_state, (struct spdk_ftl_dev *dev));
DEFINE_STUB_V(ftl_bands_load_state, (struct spdk_ftl_dev *dev));
DEFINE_STUB(ftl_md_get_region, const struct ftl_layout_region *, (struct ftl_md *md), 0);
DEFINE_STUB_V(ftl_md_restore, (struct ftl_md *md));
DEFINE_STUB(ftl_nv_cache_save_state, int, (struct ftl_nv_cache *nv_cache), 0);
DEFINE_STUB(ftl_mngt_get_step_ctx, void *, (struct ftl_mngt_process *mngt), 0);
DEFINE_STUB_V(ftl_mngt_persist_bands_p2l, (struct ftl_mngt_process *mngt));
DEFINE_STUB_V(ftl_band_init_gc_iter, (struct spdk_ftl_dev *dev));
DEFINE_STUB(ftl_md_create_region_flags, int, (struct spdk_ftl_dev *dev, int region_type), 0);
DEFINE_STUB(ftl_md_create, struct ftl_md *, (struct spdk_ftl_dev *dev, uint64_t blocks,
		uint64_t vss_blksz, const char *name, int flags, const struct ftl_layout_region *region), NULL);
DEFINE_STUB(ftl_md_destroy_region_flags, int, (struct spdk_ftl_dev *dev, int region_type), 0);
DEFINE_STUB(ftl_md_destroy_shm_flags, int, (struct spdk_ftl_dev *dev), 0);
DEFINE_STUB_V(ftl_md_destroy, (struct ftl_md *md, int flags));
DEFINE_STUB_V(ftl_mngt_call_process, (struct ftl_mngt_process *mngt,
				      const struct ftl_mngt_process_desc *process));
DEFINE_STUB(ftl_md_get_buffer, void *, (struct ftl_md *md), NULL);
DEFINE_STUB(spdk_bdev_desc_get_bdev, struct spdk_bdev *, (struct spdk_bdev_desc *desc), NULL);
DEFINE_STUB(spdk_bdev_get_write_unit_size, uint32_t, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_bdev_get_num_blocks, uint64_t, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(ftl_nv_cache_chunk_tail_md_num_blocks, size_t, (const struct ftl_nv_cache *nv_cache),
	    0);
DEFINE_STUB(ftl_band_user_blocks, size_t, (const struct ftl_band *band), 0);

struct spdk_bdev_desc {
	int dummy;
};

struct spdk_ftl_dev g_dev;
struct ftl_superblock_shm g_sb_shm = {0};
struct ftl_base_device_type g_base_type = { .name = "base_dev" };
struct ftl_nv_cache_device_desc g_nvc_desc = { .name = "nvc_dev" };
struct spdk_bdev_desc g_base_bdev_desc = {0};
struct spdk_bdev_desc g_nvc_bdev_desc = {0};
static uint8_t g_sb_buf[FTL_SUPERBLOCK_SIZE] = {0};

struct ftl_region_upgrade_desc p2l_upgrade_desc[0];
struct ftl_region_upgrade_desc nvc_upgrade_desc[0];
struct ftl_region_upgrade_desc band_upgrade_desc[0];

#define TEST_OP 0x1984
#define TEST_REG_BLKS 0x10000
#define TEST_NVC_BLKS 0x1000000;
#define TEST_BASE_BLKS 0x1000000000;

static int
test_setup(void)
{
	int regno_nvc = 0, regno_base = 0, *regno_dev;

	/* setup a dummy dev: */
	g_dev.sb = (void *)g_sb_buf;
	g_dev.sb_shm = &g_sb_shm;
	g_dev.conf.overprovisioning = TEST_OP;
	for (uint64_t n = 0; n < sizeof(g_dev.conf.uuid); n++) {
		g_dev.conf.uuid.u.raw[n] = n;
	}

	g_dev.layout.nvc.total_blocks = TEST_NVC_BLKS;
	g_dev.layout.base.total_blocks = TEST_BASE_BLKS;
	g_dev.base_type = &g_base_type;
	g_dev.nv_cache.nvc_desc = &g_nvc_desc;
	g_dev.base_layout_tracker = ftl_layout_tracker_bdev_init(UINT32_MAX);
	g_dev.nvc_layout_tracker = ftl_layout_tracker_bdev_init(UINT32_MAX);
	g_dev.base_bdev_desc = &g_base_bdev_desc;
	g_dev.nv_cache.bdev_desc = &g_nvc_bdev_desc;

	for (int regno = 0; regno < FTL_LAYOUT_REGION_TYPE_MAX; regno++) {
		struct ftl_layout_region *reg = &g_dev.layout.region[regno];

		reg->current.blocks = TEST_REG_BLKS;
		regno_dev = sb_v3_md_region_is_nvc(regno) ? &regno_nvc : &regno_base;
		reg->current.offset = *regno_dev * TEST_REG_BLKS;
		(*regno_dev)++;
		reg->current.version = ftl_layout_upgrade_region_get_latest_version(regno);
		reg->type = regno;
		reg->name = "region_test";
		reg->bdev_desc = sb_v3_md_region_is_nvc(regno) ? &g_nvc_bdev_desc : &g_base_bdev_desc;
		reg->ioch = 0;
	}
	return 0;
}

static int
test_teardown(void)
{
	if (g_dev.base_layout_tracker) {
		ftl_layout_tracker_bdev_fini(g_dev.base_layout_tracker);
		g_dev.base_layout_tracker = NULL;
	}
	if (g_dev.nvc_layout_tracker) {
		ftl_layout_tracker_bdev_fini(g_dev.nvc_layout_tracker);
		g_dev.nvc_layout_tracker = NULL;
	}
	return 0;
}

static void
test_setup_sb_ver(uint64_t ver, uint64_t clean)
{
	union ftl_superblock_ver *sb = (void *)g_sb_buf;
	uint64_t zero_offs;

	memset(&g_sb_buf, 0, sizeof(g_sb_buf));
	ftl_mngt_init_default_sb(&g_dev, NULL);
	if (ver <= FTL_SB_VERSION_3) {
		sb->header.magic = FTL_SUPERBLOCK_MAGIC_V2;
	}
	sb->header.version = ver;

	switch (ver) {
	case FTL_SB_VERSION_0:
	case FTL_SB_VERSION_1:
	case FTL_SB_VERSION_2:
		zero_offs = sizeof(struct ftl_superblock_v2);
		memset(g_sb_buf + zero_offs, 0, sizeof(g_sb_buf) - zero_offs);
		sb->v2.clean = clean;
		break;

	case FTL_SB_VERSION_3:
	case FTL_SB_VERSION_4:
		zero_offs = sizeof(struct ftl_superblock_v3);
		memset(g_sb_buf + zero_offs, 0, sizeof(g_sb_buf) - zero_offs);
		sb->v3.clean = clean;
		sb->v3.md_layout_head.type = FTL_LAYOUT_REGION_TYPE_INVALID;
		break;

	case FTL_SB_VERSION_5:
		zero_offs = sizeof(struct ftl_superblock_v5);
		memset(g_sb_buf + zero_offs, 0, sizeof(g_sb_buf) - zero_offs);
		sb->v5.clean = clean;
		break;
	}

	sb->header.crc = get_sb_crc(&sb->current);
}

static void
test_setup_sb_v2(uint64_t clean)
{
	test_setup_sb_ver(FTL_SB_VERSION_2, clean);
}

static void
test_setup_sb_v3(uint64_t clean)
{
	test_setup_sb_ver(FTL_SB_VERSION_3, clean);
}

static void
test_setup_sb_v5(uint64_t clean)
{
	test_setup_sb_ver(FTL_SB_VERSION_5, clean);
}

static void
test_sb_crc_v2(void)
{
	union ftl_superblock_ver *sb = (void *)g_sb_buf;
	uint64_t crc;

	/* v2-specific crc: it's not really working */
	test_setup_sb_v2(true);
	crc = sb->header.crc;

	sb->header.crc++;
	sb->header.crc = get_sb_crc(&sb->current);
	CU_ASSERT_EQUAL(crc, sb->header.crc);

	g_sb_buf[sizeof(struct ftl_superblock_v2)]++;
	sb->header.crc = get_sb_crc(&sb->current);
	CU_ASSERT_EQUAL(crc, sb->header.crc);

	g_sb_buf[sizeof(g_sb_buf) - 1]++;
	sb->header.crc = get_sb_crc(&sb->current);
	CU_ASSERT_EQUAL(crc, sb->header.crc);
}

static void
test_sb_crc_v3(void)
{
	union ftl_superblock_ver *sb = (void *)g_sb_buf;
	uint64_t crc;

	/* v3 crc: covers the entire buf */
	test_setup_sb_v3(true);
	crc = sb->header.crc;

	sb->header.crc++;
	sb->header.crc = get_sb_crc(&sb->current);
	CU_ASSERT_EQUAL(crc, sb->header.crc);
	crc = sb->header.crc;

	g_sb_buf[sizeof(struct ftl_superblock_v2)]++;
	sb->header.crc = get_sb_crc(&sb->current);
	CU_ASSERT_NOT_EQUAL(crc, sb->header.crc);
	crc = sb->header.crc;

	g_sb_buf[sizeof(g_sb_buf) - 1]++;
	sb->header.crc = get_sb_crc(&sb->current);
	CU_ASSERT_NOT_EQUAL(crc, sb->header.crc);
	crc = sb->header.crc;

	CU_ASSERT_EQUAL(crc, sb->header.crc);
}

static int
test_superblock_v3_md_layout_add(struct spdk_ftl_dev *dev,
				 struct ftl_superblock_v3_md_region *sb_reg,
				 uint32_t reg_type, uint32_t reg_version, uint64_t blk_offs, uint64_t blk_sz)
{
	if (ftl_superblock_v3_md_region_overflow(dev, sb_reg)) {
		return -EOVERFLOW;
	}

	sb_reg->type = reg_type;
	sb_reg->version = reg_version;
	sb_reg->blk_offs = blk_offs;
	sb_reg->blk_sz = blk_sz;
	return 0;
}

static int
test_superblock_v3_md_layout_add_free(struct spdk_ftl_dev *dev,
				      struct ftl_superblock_v3_md_region **sb_reg,
				      uint32_t reg_type, uint32_t free_type, uint64_t total_blocks)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *reg = &layout->region[reg_type];
	uint64_t blks_left = total_blocks - reg->current.offset - reg->current.blocks;

	if (blks_left == 0) {
		return 0;
	}

	(*sb_reg)->df_next = ftl_df_get_obj_id(dev->sb, (*sb_reg) + 1);
	(*sb_reg) = (*sb_reg) + 1;

	if (test_superblock_v3_md_layout_add(dev, *sb_reg, free_type, 0,
					     reg->current.offset + reg->current.blocks, blks_left)) {
		return -1;
	}

	(*sb_reg)->df_next = FTL_DF_OBJ_ID_INVALID;

	return 0;
}

static int
test_ftl_superblock_v3_md_layout_build(struct spdk_ftl_dev *dev)
{
	union ftl_superblock_ver *sb_ver = (union ftl_superblock_ver *)dev->sb;
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *reg;
	int n = 0;
	bool is_empty = ftl_superblock_v3_md_layout_is_empty(sb_ver);
	struct ftl_superblock_v3_md_region *sb_reg = &sb_ver->v3.md_layout_head;

	/* TODO: major upgrades: add all free regions being tracked
	 * For now SB MD layout must be empty - otherwise md free regions may be lost */
	assert(is_empty);

	for (; n < FTL_LAYOUT_REGION_TYPE_MAX_V3;) {
		reg = ftl_layout_region_get(dev, n);
		assert(reg);
		if (md_region_is_fixed(reg->type)) {
			n++;

			if (n >= FTL_LAYOUT_REGION_TYPE_MAX_V3) {
				/* For VSS emulation the last layout type is a fixed region, we need to move back the list and end the list on previous entry */
				sb_reg--;
				break;
			}
			continue;
		}

		if (test_superblock_v3_md_layout_add(dev, sb_reg, reg->type, reg->current.version,
						     reg->current.offset, reg->current.blocks)) {
			return -1;
		}

		n++;
		if (n < FTL_LAYOUT_REGION_TYPE_MAX_V3) {
			/* next region */
			sb_reg->df_next = ftl_df_get_obj_id(sb_ver, sb_reg + 1);
			sb_reg++;
		}
	}

	/* terminate the list */
	sb_reg->df_next = FTL_DF_OBJ_ID_INVALID;

	/* create free_nvc/free_base regions on the first run */
	if (is_empty) {
		test_superblock_v3_md_layout_add_free(dev, &sb_reg, FTL_LAYOUT_REGION_LAST_NVC,
						      FTL_LAYOUT_REGION_TYPE_FREE_NVC, layout->nvc.total_blocks);

		test_superblock_v3_md_layout_add_free(dev, &sb_reg, FTL_LAYOUT_REGION_LAST_BASE,
						      FTL_LAYOUT_REGION_TYPE_FREE_BASE, layout->base.total_blocks);
	}

	return 0;
}

static void
test_sb_v3_region_reinit(void)
{
	uint32_t reg_type;

	for (reg_type = 0; reg_type < FTL_LAYOUT_REGION_TYPE_MAX; reg_type++) {
		g_dev.layout.region[reg_type].type = reg_type;
	}
}

static struct ftl_superblock_v3_md_region *
test_sb_v3_find_region_ver(enum ftl_layout_region_type reg_type, uint32_t reg_ver)
{
	union ftl_superblock_ver *sb = (void *)g_sb_buf;
	struct ftl_superblock_v3_md_region *sb_reg = &sb->v3.md_layout_head;

	while (sb_reg->type != FTL_LAYOUT_REGION_TYPE_INVALID) {
		if (sb_reg->type == reg_type && sb_reg->version == reg_ver) {
			return sb_reg;
		}

		if (sb_reg->df_next == FTL_DF_OBJ_ID_INVALID) {
			break;
		}

		if (UINT64_MAX - (uintptr_t)sb <= sb_reg->df_next) {
			return NULL;
		}

		sb_reg = ftl_df_get_obj_ptr(sb, sb_reg->df_next);
		if (ftl_superblock_v3_md_region_overflow(&g_dev, sb_reg)) {
			return NULL;
		}
	}

	return NULL;
}

static struct ftl_superblock_v3_md_region *
test_sb_v3_find_region_latest(enum ftl_layout_region_type reg_type)
{
	return test_sb_v3_find_region_ver(reg_type, ftl_layout_upgrade_region_get_latest_version(reg_type));
}

static void
test_sb_v3_md_layout(void)
{
	struct ftl_superblock_v3_md_region *sb_reg, *sb_reg_next, *sb_reg_next2;
	struct ftl_layout_region *reg_head, *reg;
	union ftl_superblock_ver *sb = (void *)g_sb_buf;
	ftl_df_obj_id df_next_head, df_next_reg;
	uint32_t md_type_head;
	int rc;

	test_setup_sb_v3(false);
	CU_ASSERT_EQUAL(ftl_superblock_is_blob_area_empty(&sb->current), true);

	/* load failed: empty md list: */
	rc = ftl_superblock_v3_md_layout_load_all(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);
	test_sb_v3_region_reinit();

	/* create md layout: */
	test_ftl_superblock_v3_md_layout_build(&g_dev);
	CU_ASSERT_EQUAL(ftl_superblock_is_blob_area_empty(&sb->current), false);

	/* buf overflow, sb_reg = 1 byte overflow: */
	df_next_head = sb->v3.md_layout_head.df_next;
	sb->v3.md_layout_head.df_next = FTL_SUPERBLOCK_SIZE - sizeof(sb->v3.md_layout_head) + 1;
	rc = ftl_superblock_v3_md_layout_load_all(&g_dev);
	CU_ASSERT_EQUAL(rc, -EOVERFLOW);
	test_sb_v3_region_reinit();

	/* buf underflow, sb_reg = -1: */
	sb->v3.md_layout_head.df_next = UINTPTR_MAX - (uintptr_t)sb;
	rc = ftl_superblock_v3_md_layout_load_all(&g_dev);
	CU_ASSERT_EQUAL(rc, -EOVERFLOW);
	test_sb_v3_region_reinit();

	/* buf underflow, sb_reg = 2 bytes underflow */
	sb->v3.md_layout_head.df_next = UINTPTR_MAX - 1;
	rc = ftl_superblock_v3_md_layout_load_all(&g_dev);
	CU_ASSERT_EQUAL(rc, -EOVERFLOW);
	test_sb_v3_region_reinit();

	/* looping md layout list: */
	sb->v3.md_layout_head.df_next = ftl_df_get_obj_id(sb, &sb->v3.md_layout_head);
	rc = ftl_superblock_v3_md_layout_load_all(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);
	test_sb_v3_region_reinit();

	sb->v3.md_layout_head.df_next = df_next_head;

	/* unsupported/fixed md region: */
	md_type_head = sb->v3.md_layout_head.type;
	sb->v3.md_layout_head.type = FTL_LAYOUT_REGION_TYPE_SB;
	rc = ftl_superblock_v3_md_layout_load_all(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);
	test_sb_v3_region_reinit();

	/* unsupported/invalid md region: */
	sb->v3.md_layout_head.type = FTL_LAYOUT_REGION_TYPE_MAX;
	rc = ftl_superblock_v3_md_layout_load_all(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);
	test_sb_v3_region_reinit();

	/* unsupported/invalid md region: */
	sb->v3.md_layout_head.type = FTL_LAYOUT_REGION_TYPE_MAX_V3;
	rc = ftl_superblock_v3_md_layout_load_all(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);
	test_sb_v3_region_reinit();

	/* restore the sb: */
	sb->v3.md_layout_head.type = md_type_head;

	/* load succeeded, no prev version found: */
	reg_head = &g_dev.layout.region[md_type_head];
	rc = ftl_superblock_v3_md_layout_load_all(&g_dev);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(reg_head->current.version,
			ftl_layout_upgrade_region_get_latest_version(md_type_head));
	test_sb_v3_region_reinit();

	/* load succeeded, prev (upgrade, i.e. no current) version discovery: */
	reg = &g_dev.layout.region[FTL_LAYOUT_REGION_TYPE_BAND_MD];
	sb_reg = test_sb_v3_find_region_latest(FTL_LAYOUT_REGION_TYPE_BAND_MD);
	CU_ASSERT_NOT_EQUAL(sb_reg, NULL);
	CU_ASSERT_EQUAL(reg->type, sb_reg->type);
	df_next_reg = sb_reg->df_next;

	sb_reg->version--;
	rc = ftl_superblock_v3_md_layout_load_all(&g_dev);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(reg->current.version, sb_reg->version);
	sb_reg->version++;
	test_sb_v3_region_reinit();

	/* load succeeded, newer version found: */
	sb_reg->df_next = FTL_SUPERBLOCK_SIZE - sizeof(*sb_reg_next);
	sb_reg_next = ftl_df_get_obj_ptr(sb, sb_reg->df_next);
	rc = test_superblock_v3_md_layout_add(&g_dev, sb_reg_next, sb_reg->type, sb_reg->version + 1,
					      sb_reg->blk_offs, sb_reg->blk_sz);
	CU_ASSERT_EQUAL(rc, 0);
	sb_reg_next->df_next = df_next_reg;
	rc = ftl_superblock_v3_md_layout_load_all(&g_dev);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(reg->current.version, sb_reg->version);
	test_sb_v3_region_reinit();

	/* load succeeded, prev version discovery: */
	sb_reg_next->version = sb_reg->version - 1;
	rc = ftl_superblock_v3_md_layout_load_all(&g_dev);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(reg->current.version, sb_reg_next->version);
	test_sb_v3_region_reinit();

	/* looping regions found: */
	sb_reg_next->df_next = FTL_SUPERBLOCK_SIZE - 2 * sizeof(*sb_reg_next);
	sb_reg_next2 = ftl_df_get_obj_ptr(sb, sb_reg_next->df_next);
	rc = test_superblock_v3_md_layout_add(&g_dev, sb_reg_next2, sb_reg_next->type,
					      sb_reg_next->version + 2,
					      sb_reg_next->blk_offs, sb_reg_next->blk_sz);
	CU_ASSERT_EQUAL(rc, 0);
	sb_reg_next2->df_next = FTL_SUPERBLOCK_SIZE - 2 * sizeof(*sb_reg_next);
	rc = ftl_superblock_v3_md_layout_load_all(&g_dev);
	CU_ASSERT_EQUAL(rc, -ELOOP);
	test_sb_v3_region_reinit();

	/* multiple (same ver) regions found: */
	sb_reg_next2->version = sb_reg_next->version;
	sb_reg_next2->df_next = df_next_reg;
	rc = ftl_superblock_v3_md_layout_load_all(&g_dev);
	CU_ASSERT_EQUAL(rc, -EAGAIN);
	test_sb_v3_region_reinit();

	/* multiple current regions found: */
	sb_reg_next->version = sb_reg->version;
	sb_reg_next->df_next = df_next_reg;
	rc = ftl_superblock_v3_md_layout_load_all(&g_dev);
	CU_ASSERT_EQUAL(rc, -EAGAIN);

	/* restore the sb: */
	sb->v3.md_layout_head.df_next = df_next_head;
	test_sb_v3_region_reinit();
}

static void
test_sb_v5_md_layout(void)
{
	struct layout_tracker_blob_entry *tbe;
	struct layout_blob_entry *lbe;
	struct ftl_layout_region *reg;
	union ftl_superblock_ver *sb = (void *)g_sb_buf;
	int rc;
	const struct ftl_layout_tracker_bdev_region_props *reg_props;
	void *blob_nvc, *blob_base, *blob_regs;

	test_setup_sb_v5(false);
	CU_ASSERT_EQUAL(ftl_superblock_is_blob_area_empty(&sb->current), true);

	/* load failed: empty md list: */
	rc = ftl_superblock_v5_load_blob_area(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);

	/* create md layout: */
	for (enum ftl_layout_region_type regno = 0; regno < FTL_LAYOUT_REGION_TYPE_MAX; regno++) {
		struct ftl_layout_region *reg = &g_dev.layout.region[regno];
		CU_ASSERT_EQUAL(regno, reg->type);
		struct ftl_layout_tracker_bdev *tracker = sb_v3_md_region_is_nvc(regno) ? g_dev.nvc_layout_tracker :
				g_dev.base_layout_tracker;
		const struct ftl_layout_tracker_bdev_region_props *reg_props = ftl_layout_tracker_bdev_add_region(
					tracker, reg->type, reg->current.version, reg->current.blocks, TEST_REG_BLKS);

		CU_ASSERT_EQUAL(reg->type, reg_props->type);
		CU_ASSERT_EQUAL(reg->current.version, reg_props->ver);
		CU_ASSERT_EQUAL(reg->current.offset, reg_props->blk_offs);
		CU_ASSERT_EQUAL(reg->current.blocks, reg_props->blk_sz);
	}
	ftl_superblock_v5_store_blob_area(&g_dev);
	CU_ASSERT_EQUAL(ftl_superblock_is_blob_area_empty(&sb->current), false);

	blob_nvc = ftl_df_get_obj_ptr(sb->v5.blob_area, sb->v5.md_layout_nvc.df_id);
	blob_base = ftl_df_get_obj_ptr(sb->v5.blob_area, sb->v5.md_layout_base.df_id);
	blob_regs = ftl_df_get_obj_ptr(sb->v5.blob_area, sb->v5.layout_params.df_id);

	/* unsupported nvc md region type: */
	tbe = blob_nvc;
	tbe->type += FTL_LAYOUT_REGION_TYPE_MAX;
	sb->v3.md_layout_head.type = FTL_LAYOUT_REGION_TYPE_SB;
	rc = ftl_superblock_v5_load_blob_area(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);
	tbe->type -= FTL_LAYOUT_REGION_TYPE_MAX;

	/* unsupported base md region type: */
	tbe = blob_base;
	tbe->type += FTL_LAYOUT_REGION_TYPE_MAX;
	sb->v3.md_layout_head.type = FTL_LAYOUT_REGION_TYPE_SB;
	rc = ftl_superblock_v5_load_blob_area(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);
	tbe->type -= FTL_LAYOUT_REGION_TYPE_MAX;

	/* load succeeded, no prev version found: */
	reg = &g_dev.layout.region[FTL_LAYOUT_REGION_TYPE_BAND_MD];
	rc = ftl_superblock_v5_load_blob_area(&g_dev);
	CU_ASSERT_EQUAL(rc, 0);
	reg_props = sb_md_layout_find_region(&g_dev, reg->type, sb_md_layout_find_latest_region, NULL);
	CU_ASSERT_NOT_EQUAL(reg_props, NULL);
	CU_ASSERT_EQUAL(reg_props->ver, reg->current.version);
	reg_props = sb_md_layout_find_region(&g_dev, reg->type, sb_md_layout_find_oldest_region, NULL);
	CU_ASSERT_NOT_EQUAL(reg_props, NULL);
	CU_ASSERT_EQUAL(reg_props->ver, reg->current.version);

	/* move the sb-stored blobs around: */
	CU_ASSERT(blob_nvc < blob_base);
	CU_ASSERT(blob_base < blob_regs);
	blob_regs = memmove(blob_regs + 8192, blob_regs, sb->v5.layout_params.blob_sz);
	sb->v5.layout_params.df_id += 8192;
	blob_base = memmove(blob_base + 4096, blob_base, sb->v5.md_layout_base.blob_sz);
	sb->v5.md_layout_base.df_id += 4096;

	/* load succeeded again, no prev version found: */
	rc = ftl_superblock_v5_load_blob_area(&g_dev);
	CU_ASSERT_EQUAL(rc, 0);
	reg_props = sb_md_layout_find_region(&g_dev, reg->type, sb_md_layout_find_latest_region, NULL);
	CU_ASSERT_NOT_EQUAL(reg_props, NULL);
	CU_ASSERT_EQUAL(reg_props->ver, reg->current.version);
	reg_props = sb_md_layout_find_region(&g_dev, reg->type, sb_md_layout_find_oldest_region, NULL);
	CU_ASSERT_NOT_EQUAL(reg_props, NULL);
	CU_ASSERT_EQUAL(reg_props->ver, reg->current.version);

	/* load failed, regs overlap: */
	tbe = blob_nvc;
	tbe++;
	tbe->blk_offs -= tbe->blk_sz;
	rc = ftl_superblock_v5_load_blob_area(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);
	tbe->blk_offs += tbe->blk_sz;

	/* load failed, the same region version found twice: */
	tbe = (blob_nvc + sb->v5.md_layout_nvc.blob_sz);
	sb->v5.md_layout_nvc.blob_sz += sizeof(*tbe);
	tbe->type = reg->type;
	tbe->ver = reg->current.version;
	tbe->blk_offs = reg->current.offset + FTL_LAYOUT_REGION_TYPE_MAX * reg->current.blocks;
	tbe->blk_sz = reg->current.blocks;
	rc = ftl_superblock_v5_load_blob_area(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);

	/* load succeeded, prev (upgrade, i.e. no current) version discovery: */
	tbe->type = reg->type;
	tbe->ver = reg->current.version - 1;
	tbe->blk_offs = reg->current.offset + FTL_LAYOUT_REGION_TYPE_MAX * reg->current.blocks;
	tbe->blk_sz = reg->current.blocks;
	rc = ftl_superblock_v5_load_blob_area(&g_dev);
	CU_ASSERT_EQUAL(rc, 0);
	reg_props = sb_md_layout_find_region(&g_dev, reg->type, sb_md_layout_find_latest_region, NULL);
	CU_ASSERT_NOT_EQUAL(reg_props, NULL);
	CU_ASSERT_EQUAL(reg_props->ver, reg->current.version);
	reg_props = sb_md_layout_find_region(&g_dev, reg->type, sb_md_layout_find_oldest_region, NULL);
	CU_ASSERT_NOT_EQUAL(reg_props, NULL);
	CU_ASSERT_EQUAL(reg_props->ver, reg->current.version - 1);

	/* load succeeded, newer version found: */
	tbe->ver = reg->current.version + 1;
	rc = ftl_superblock_v5_load_blob_area(&g_dev);
	CU_ASSERT_EQUAL(rc, 0);
	reg_props = sb_md_layout_find_region(&g_dev, reg->type, sb_md_layout_find_latest_region, NULL);
	CU_ASSERT_NOT_EQUAL(reg_props, NULL);
	CU_ASSERT_EQUAL(reg_props->ver, reg->current.version + 1);
	reg_props = sb_md_layout_find_region(&g_dev, reg->type, sb_md_layout_find_oldest_region, NULL);
	CU_ASSERT_NOT_EQUAL(reg_props, NULL);
	CU_ASSERT_EQUAL(reg_props->ver, reg->current.version);

	/* load failed, invalid type in layout properties: */
	lbe = blob_regs;
	lbe += FTL_LAYOUT_REGION_TYPE_BAND_MD;
	CU_ASSERT_EQUAL(lbe->type, FTL_LAYOUT_REGION_TYPE_BAND_MD);
	lbe->type = FTL_LAYOUT_REGION_TYPE_MAX;
	rc = ftl_superblock_v5_load_blob_area(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);
	lbe->type = FTL_LAYOUT_REGION_TYPE_BAND_MD;

	/* load succeeded, restore layout properties: */
	CU_ASSERT_EQUAL(reg->num_entries, 0);
	CU_ASSERT_EQUAL(reg->entry_size, 0);
	lbe->num_entries = 0x1984;
	lbe->entry_size = 0x1405;
	rc = ftl_superblock_v5_load_blob_area(&g_dev);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(reg->num_entries, 0x1984);
	CU_ASSERT_EQUAL(reg->entry_size, 0x1405);

	/* restore the sb: */
	sb->v5.md_layout_nvc.blob_sz -= sizeof(*tbe);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures = 0;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("ftl_sb", test_setup, test_teardown);

	CU_ADD_TEST(suite, test_sb_crc_v2);
	CU_ADD_TEST(suite, test_sb_crc_v3);
	CU_ADD_TEST(suite, test_sb_v3_md_layout);
	CU_ADD_TEST(suite, test_sb_v5_md_layout);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
