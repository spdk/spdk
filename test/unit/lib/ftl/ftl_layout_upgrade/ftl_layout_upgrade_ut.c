/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include <sys/queue.h>

#include "spdk/stdinc.h"

#include "CUnit/Basic.h"
#include "common/lib/test_env.c"

#include "ftl/ftl_layout.c"
#include "ftl/upgrade/ftl_layout_upgrade.h"
#include "ftl/utils/ftl_layout_tracker_bdev.c"
#include "ftl/upgrade/ftl_sb_v3.c"
#include "ftl/upgrade/ftl_sb_v5.c"
#include "ftl/ftl_sb.c"

extern struct ftl_region_upgrade_desc sb_upgrade_desc[];

int l2p_upgrade_v0_to_v1(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx);
int l2p_upgrade_v1_to_v2(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx);
int l2p_upgrade_v2_to_v3(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx);

struct ftl_region_upgrade_desc l2p_upgrade_desc[] = {
	[0] = {.verify = ftl_region_upgrade_enabled, .upgrade = l2p_upgrade_v0_to_v1, .new_version = 1},
	[1] = {.verify = ftl_region_upgrade_enabled, .upgrade = l2p_upgrade_v1_to_v2, .new_version = 2},
	[2] = {.verify = ftl_region_upgrade_enabled, .upgrade = l2p_upgrade_v2_to_v3, .new_version = 3},
};

static struct ftl_layout_upgrade_desc_list layout_upgrade_desc[] = {
	[FTL_LAYOUT_REGION_TYPE_SB] = {
		.latest_ver = FTL_SB_VERSION_CURRENT,
		.count = FTL_SB_VERSION_CURRENT,
		.desc = sb_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_SB_BASE] = {
		.latest_ver = FTL_SB_VERSION_CURRENT,
		.count = FTL_SB_VERSION_CURRENT,
		.desc = sb_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_L2P] = {
		.latest_ver = 3,
		.count = 3,
		.desc = l2p_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_BAND_MD] = {},
	[FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR] = {},
	[FTL_LAYOUT_REGION_TYPE_VALID_MAP] = {},
	[FTL_LAYOUT_REGION_TYPE_NVC_MD] = {},
	[FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR] = {},
	[FTL_LAYOUT_REGION_TYPE_DATA_NVC] = {},
	[FTL_LAYOUT_REGION_TYPE_DATA_BASE] = {},
	[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC] = {},
	[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC_NEXT] = {},
	[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_COMP] = {},
	[FTL_LAYOUT_REGION_TYPE_P2L_CKPT_COMP_NEXT] = {},
	[FTL_LAYOUT_REGION_TYPE_TRIM_MD] = {},
	[FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR] = {},
};

SPDK_STATIC_ASSERT(sizeof(layout_upgrade_desc) / sizeof(*layout_upgrade_desc) ==
		   FTL_LAYOUT_REGION_TYPE_MAX,
		   "Missing layout upgrade descriptors");

#include "ftl/upgrade/ftl_sb_upgrade.c"
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
test_setup_sb_v5(uint64_t clean)
{
	union ftl_superblock_ver *sb = (void *)g_sb_buf;
	uint64_t zero_offs;

	memset(&g_sb_buf, 0, sizeof(g_sb_buf));
	ftl_mngt_init_default_sb(&g_dev, NULL);
	sb->header.version = FTL_SB_VERSION_5;

	zero_offs = sizeof(struct ftl_superblock_v5);
	memset(g_sb_buf + zero_offs, 0, sizeof(g_sb_buf) - zero_offs);
	sb->v5.clean = clean;

	sb->header.crc = get_sb_crc(&sb->current);
}

static void
test_l2p_upgrade(void)
{
	union ftl_superblock_ver *sb = (void *)g_sb_buf;
	struct layout_tracker_blob_entry *tbe;
	struct layout_blob_entry *lbe;
	struct ftl_layout_region *reg;
	struct ftl_layout_upgrade_ctx ctx = {0};
	void *blob_nvc, *blob_base, *blob_regs;
	uint64_t upgrades;
	int rc;

	test_setup_sb_v5(true);
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

	/* move the sb-stored blobs around: */
	CU_ASSERT(blob_nvc < blob_base);
	CU_ASSERT(blob_base < blob_regs);
	blob_regs = memmove(blob_regs + 8192, blob_regs, sb->v5.layout_params.blob_sz);
	sb->v5.layout_params.df_id += 8192;
	blob_base = memmove(blob_base + 4096, blob_base, sb->v5.md_layout_base.blob_sz);
	sb->v5.md_layout_base.df_id += 4096;

	/* fix l2p region version to v0 */
	tbe = blob_nvc;
	tbe++;
	CU_ASSERT_EQUAL(tbe->type, FTL_LAYOUT_REGION_TYPE_L2P);
	tbe->ver = 0;
	reg = &g_dev.layout.region[FTL_LAYOUT_REGION_TYPE_L2P];
	reg->current.version = 0;

	/* fix l2p num entries and size */
	lbe = blob_regs;
	lbe += FTL_LAYOUT_REGION_TYPE_L2P;
	CU_ASSERT_EQUAL(lbe->type, FTL_LAYOUT_REGION_TYPE_L2P);
	lbe->entry_size = 1;
	lbe->num_entries = 0x1000;

	/* add l2p v2 region for a major upgrade */
	tbe = (blob_nvc + sb->v5.md_layout_nvc.blob_sz);
	sb->v5.md_layout_nvc.blob_sz += sizeof(*tbe);
	tbe->type = FTL_LAYOUT_REGION_TYPE_L2P;
	tbe->ver = 2;

	/* region overlap */
	tbe->blk_offs = 0x1984;
	tbe->blk_sz = 0x0514;
	rc = ftl_superblock_v5_load_blob_area(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);

	/* correct region placement */
	tbe->blk_offs = 0x19840514;
	tbe->blk_sz = 0xc0ffee;
	rc = ftl_superblock_v5_load_blob_area(&g_dev);
	CU_ASSERT_EQUAL(rc, 0);

	/* upgrade verification */
	ctx.reg = &g_dev.layout.region[0];
	ctx.upgrade = &layout_upgrade_desc[0];
	for (int reg_type = 0; reg_type < FTL_LAYOUT_REGION_TYPE_MAX;
	     reg_type++, ctx.reg++, ctx.upgrade++) {
		if (reg_type == FTL_LAYOUT_REGION_TYPE_SB || reg_type == FTL_LAYOUT_REGION_TYPE_SB_BASE) {
			ctx.reg->current.version = g_dev.sb->header.version;
		}
		rc = region_verify(&g_dev, &ctx);
		CU_ASSERT_EQUAL(rc, 0);
	}

	/* region upgrade */
	CU_ASSERT_EQUAL(reg->num_entries, 0x1000);
	CU_ASSERT_EQUAL(reg->entry_size, 1);

	ctx.reg = &g_dev.layout.region[0];
	ctx.upgrade = &layout_upgrade_desc[0];
	upgrades = 0;
	while (true) {
		uint64_t prev_ver;

		rc = layout_upgrade_select_next_region(&g_dev, &ctx);
		if (rc == FTL_LAYOUT_UPGRADE_DONE) {
			break;
		}
		CU_ASSERT_EQUAL(rc, FTL_LAYOUT_UPGRADE_CONTINUE);
		CU_ASSERT_EQUAL(ctx.reg->type, FTL_LAYOUT_REGION_TYPE_L2P);
		upgrades++;

		prev_ver = ctx.reg->current.version;
		rc = ftl_region_upgrade(&g_dev, &ctx);
		CU_ASSERT_EQUAL(rc, 0);
		CU_ASSERT_TRUE(prev_ver < ctx.reg->current.version);
		CU_ASSERT_EQUAL(upgrades, ctx.reg->current.version);
	}
	CU_ASSERT_EQUAL(upgrades, 3);
	CU_ASSERT_EQUAL(reg->current.offset, 0x19840514);
	CU_ASSERT_EQUAL(reg->current.blocks, 0xc0ffee);
	CU_ASSERT_EQUAL(reg->num_entries, 0x1984);
	CU_ASSERT_EQUAL(reg->entry_size, 0x1405);

	/* no more upgrades: */
	ctx.reg = &g_dev.layout.region[0];
	ctx.upgrade = &layout_upgrade_desc[0];
	rc = layout_upgrade_select_next_region(&g_dev, &ctx);
	CU_ASSERT_EQUAL(rc, FTL_LAYOUT_UPGRADE_DONE);

	/* restore the sb: */
	sb->v5.md_layout_nvc.blob_sz -= sizeof(*tbe);
}

int
l2p_upgrade_v0_to_v1(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	struct ftl_layout_region *region = ctx->reg;

	/* minor */
	CU_ASSERT_EQUAL(region->current.version, 0);
	CU_ASSERT_EQUAL(ctx->next_reg_ver, 1);
	CU_ASSERT_NOT_EQUAL(region->current.offset, 0x1984);
	CU_ASSERT_NOT_EQUAL(region->current.blocks, 0x0514);

	ftl_region_upgrade_completed(dev, ctx, 0, 0, 0);
	return 0;
}

int
l2p_upgrade_v1_to_v2(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	struct ftl_layout_region *region = ctx->reg;

	/* major */
	CU_ASSERT_EQUAL(region->current.version, 1);
	CU_ASSERT_EQUAL(ctx->next_reg_ver, 2);
	CU_ASSERT_NOT_EQUAL(region->current.offset, 0x1984);
	CU_ASSERT_NOT_EQUAL(region->current.blocks, 0x0514);

	ftl_region_upgrade_completed(dev, ctx, 0x1405, 0x1984, 0);
	return 0;
}

int
l2p_upgrade_v2_to_v3(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	struct ftl_layout_region *region = ctx->reg;

	/* minor */
	CU_ASSERT_EQUAL(region->current.version, 2);
	CU_ASSERT_EQUAL(ctx->next_reg_ver, 3);
	CU_ASSERT_EQUAL(region->current.offset, 0x19840514);
	CU_ASSERT_EQUAL(region->current.blocks, 0xc0ffee);

	ftl_region_upgrade_completed(dev, ctx, 0, 0, 0);
	return 0;
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures = 0;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("ftl_layout_upgrade", test_setup, test_teardown);

	CU_ADD_TEST(suite, test_l2p_upgrade);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
