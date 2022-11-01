/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include <sys/queue.h>

#include "spdk/stdinc.h"

#include "spdk_cunit.h"
#include "common/lib/test_env.c"

#include "ftl/ftl_layout.h"
#include "ftl/upgrade/ftl_layout_upgrade.h"
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
#ifdef SPDK_FTL_VSS_EMU
	[FTL_LAYOUT_REGION_TYPE_VSS] = {},
#endif
	[FTL_LAYOUT_REGION_TYPE_SB] = {
		.count = FTL_SB_VERSION_CURRENT,
		.desc = sb_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_SB_BASE] = {
		.count = FTL_SB_VERSION_CURRENT,
		.desc = sb_upgrade_desc,
	},
	[FTL_LAYOUT_REGION_TYPE_L2P] = {
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
DEFINE_STUB(ftl_validate_regions, int, (struct spdk_ftl_dev *dev, struct ftl_layout *layout), 0);
DEFINE_STUB_V(ftl_layout_dump, (struct spdk_ftl_dev *dev));
DEFINE_STUB(ftl_layout_setup, int, (struct spdk_ftl_dev *dev), 0);
DEFINE_STUB(ftl_md_create_region_flags, int, (struct spdk_ftl_dev *dev, int region_type), 0);
DEFINE_STUB(ftl_md_create, struct ftl_md *, (struct spdk_ftl_dev *dev, uint64_t blocks,
		uint64_t vss_blksz, const char *name, int flags, const struct ftl_layout_region *region), NULL);
DEFINE_STUB(ftl_md_destroy_region_flags, int, (struct spdk_ftl_dev *dev, int region_type), 0);
DEFINE_STUB(ftl_md_destroy_shm_flags, int, (struct spdk_ftl_dev *dev), 0);
DEFINE_STUB_V(ftl_md_destroy, (struct ftl_md *md, int flags));
DEFINE_STUB_V(ftl_mngt_call_process, (struct ftl_mngt_process *mngt,
				      const struct ftl_mngt_process_desc *process));
DEFINE_STUB(ftl_md_get_buffer, void *, (struct ftl_md *md), NULL);
DEFINE_STUB(ftl_layout_setup_superblock, int, (struct spdk_ftl_dev *dev), 0);

struct spdk_ftl_dev g_dev;
struct ftl_superblock_shm g_sb_shm = {0};
static uint8_t g_sb_buf[FTL_SUPERBLOCK_SIZE] = {0};

#define TEST_OP 0x1984
#define TEST_REG_BLKS 0x10000
#define TEST_NVC_BLKS 0x1000000;
#define TEST_BASE_BLKS 0x1000000000;

static int
test_setup(void)
{
	/* setup a dummy dev: */
	g_dev.sb = (void *)g_sb_buf;
	g_dev.sb_shm = &g_sb_shm;
	g_dev.conf.overprovisioning = TEST_OP;
	for (uint64_t n = 0; n < sizeof(g_dev.conf.uuid); n++) {
		g_dev.conf.uuid.u.raw[n] = n;
	}

	g_dev.layout.nvc.total_blocks = TEST_NVC_BLKS;
	g_dev.layout.base.total_blocks = TEST_BASE_BLKS;

	for (int regno = 0; regno < FTL_LAYOUT_REGION_TYPE_MAX; regno++) {
		struct ftl_layout_region *reg = &g_dev.layout.region[regno];
		uint32_t reg_ver = layout_upgrade_desc[regno].count;

		reg->current.blocks = TEST_REG_BLKS;
		reg->current.offset = regno * TEST_REG_BLKS;
		reg->current.version = reg_ver;
		reg->prev.version = reg_ver;
		reg->type = regno;
		reg->name = "region_test";
		reg->bdev_desc = 0;
		reg->ioch = 0;
	}
	return 0;
}

static void
test_setup_sb_v3(uint64_t clean)
{
	struct ftl_superblock *sb = (void *)g_sb_buf;

	memset(&g_sb_buf, 0, sizeof(g_sb_buf));
	ftl_mngt_init_default_sb(&g_dev, NULL);
	sb->clean = clean;
	sb->header.crc = get_sb_crc(sb);
}

static void
test_l2p_upgrade(void)
{
	union ftl_superblock_ver *sb = (void *)g_sb_buf;
	struct ftl_superblock_md_region *sb_reg;
	struct ftl_layout_region *reg;
	struct ftl_layout_upgrade_ctx ctx = {0};
	ftl_df_obj_id df_next;
	uint32_t md_type;
	uint64_t upgrades;
	int rc;

	test_setup_sb_v3(true);
	CU_ASSERT_EQUAL(ftl_superblock_md_layout_is_empty(&sb->v3), true);

	/* load failed: empty md list: */
	rc = ftl_superblock_md_layout_load_all(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);

	/* create md layout: */
	ftl_superblock_md_layout_build(&g_dev);
	CU_ASSERT_EQUAL(ftl_superblock_md_layout_is_empty(&sb->v3), false);

	df_next = sb->v3.md_layout_head.df_next;

	/* unsupported/fixed md region: */
	md_type = sb->v3.md_layout_head.type;
	reg = &g_dev.layout.region[md_type];
	assert(md_type == FTL_LAYOUT_REGION_TYPE_L2P);
	sb->v3.md_layout_head.df_next = FTL_SUPERBLOCK_SIZE - sizeof(sb->v3.md_layout_head);
	sb->v3.md_layout_head.version = 0;
	sb_reg = ftl_df_get_obj_ptr(sb, sb->v3.md_layout_head.df_next);
	rc = superblock_md_layout_add(&g_dev, sb_reg, md_type, 2, sb->v3.md_layout_head.blk_offs,
				      sb->v3.md_layout_head.blk_sz);
	CU_ASSERT_EQUAL(rc, 0);
	sb_reg->df_next = df_next;
	sb_reg->blk_offs = 0x1984;
	sb_reg->blk_sz = 0x0514;
	rc = ftl_superblock_md_layout_load_all(&g_dev);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(reg->current.version, 3);
	CU_ASSERT_EQUAL(reg->prev.version, 0);

	ctx.reg = &g_dev.layout.region[0];
	ctx.upgrade = &layout_upgrade_desc[0];
	for (int reg_type = 0; reg_type < FTL_LAYOUT_REGION_TYPE_MAX;
	     reg_type++, ctx.reg++, ctx.upgrade++) {
		if (reg_type == FTL_LAYOUT_REGION_TYPE_SB || reg_type == FTL_LAYOUT_REGION_TYPE_SB_BASE) {
			ctx.reg->prev.version = g_dev.sb->header.version;
		}
		rc = region_verify(&g_dev, &ctx);
		CU_ASSERT_EQUAL(rc, 0);
	}

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

		prev_ver = ctx.reg->prev.version;
		rc = ftl_region_upgrade(&g_dev, &ctx);
		CU_ASSERT_EQUAL(rc, 0);
		CU_ASSERT_TRUE(prev_ver < ctx.reg->prev.version);
		CU_ASSERT_EQUAL(upgrades, ctx.reg->prev.version);
	}
	CU_ASSERT_EQUAL(upgrades, 3);
	CU_ASSERT_EQUAL(reg->prev.sb_md_reg, reg->current.sb_md_reg);
	CU_ASSERT_NOT_EQUAL(reg->current.sb_md_reg, NULL);
	CU_ASSERT_EQUAL(reg->current.offset, 0x1984);
	CU_ASSERT_EQUAL(reg->current.blocks, 0x0514);

	/* no more upgrades: */
	ctx.reg = &g_dev.layout.region[0];
	ctx.upgrade = &layout_upgrade_desc[0];
	rc = layout_upgrade_select_next_region(&g_dev, &ctx);
	CU_ASSERT_EQUAL(rc, FTL_LAYOUT_UPGRADE_DONE);

	/* restore the sb: */
	sb->v3.md_layout_head.df_next = df_next;
}

int
l2p_upgrade_v0_to_v1(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	struct ftl_layout_region *region = ctx->reg;
	/* minor */
	CU_ASSERT_EQUAL(region->prev.version, 0);
	CU_ASSERT_NOT_EQUAL(region->prev.sb_md_reg, NULL);
	CU_ASSERT_NOT_EQUAL(region->prev.offset, 0x1984);
	CU_ASSERT_NOT_EQUAL(region->prev.blocks, 0x0514);

	CU_ASSERT_EQUAL(region->current.sb_md_reg, NULL);
	CU_ASSERT_NOT_EQUAL(region->current.offset, 0x1984);
	CU_ASSERT_NOT_EQUAL(region->current.blocks, 0x0514);
	ftl_region_upgrade_completed(dev, ctx, 0);
	return 0;
}

int
l2p_upgrade_v1_to_v2(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	struct ftl_layout_region *region = ctx->reg;
	/* major */
	CU_ASSERT_EQUAL(region->prev.version, 1);
	CU_ASSERT_NOT_EQUAL(region->prev.sb_md_reg, NULL);
	CU_ASSERT_NOT_EQUAL(region->prev.offset, 0x1984);
	CU_ASSERT_NOT_EQUAL(region->prev.blocks, 0x0514);

	CU_ASSERT_EQUAL(region->current.sb_md_reg, NULL);
	CU_ASSERT_NOT_EQUAL(region->current.offset, 0x1984);
	CU_ASSERT_NOT_EQUAL(region->current.blocks, 0x0514);
	ftl_region_upgrade_completed(dev, ctx, 0);
	return 0;
}

int
l2p_upgrade_v2_to_v3(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	struct ftl_layout_region *region = ctx->reg;
	/* minor */
	CU_ASSERT_EQUAL(region->prev.version, 2);
	CU_ASSERT_NOT_EQUAL(region->prev.sb_md_reg, NULL);
	CU_ASSERT_EQUAL(region->prev.offset, 0x1984);
	CU_ASSERT_EQUAL(region->prev.blocks, 0x0514);

	CU_ASSERT_EQUAL(region->current.sb_md_reg, NULL);
	CU_ASSERT_NOT_EQUAL(region->current.offset, 0x1984);
	CU_ASSERT_NOT_EQUAL(region->current.blocks, 0x0514);
	ftl_region_upgrade_completed(dev, ctx, 0);
	return 0;
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("ftl_sb", test_setup, NULL);

	CU_ADD_TEST(suite, test_l2p_upgrade);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
