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

static struct ftl_layout_upgrade_desc layout_upgrade_desc[] = {
#ifdef SPDK_FTL_VSS_EMU
	[ftl_layout_region_type_vss] = {},
#endif
	[ftl_layout_region_type_sb] = {
		.reg_upgrade_desc_sz = FTL_METADATA_VERSION_CURRENT,
		.reg_upgrade_desc = sb_upgrade_desc,
	},
	[ftl_layout_region_type_sb_btm] = {
		.reg_upgrade_desc_sz = FTL_METADATA_VERSION_CURRENT,
		.reg_upgrade_desc = sb_upgrade_desc,
	},
	[ftl_layout_region_type_l2p] = {
		.reg_upgrade_desc_sz = 3,
		.reg_upgrade_desc = l2p_upgrade_desc,
	},
	[ftl_layout_region_type_band_md] = {},
	[ftl_layout_region_type_band_md_mirror] = {},
	[ftl_layout_region_type_valid_map] = {},
	[ftl_layout_region_type_nvc_md] = {},
	[ftl_layout_region_type_nvc_md_mirror] = {},
	[ftl_layout_region_type_data_nvc] = {},
	[ftl_layout_region_type_data_btm] = {},
	[ftl_layout_region_type_p2l_ckpt_gc] = {},
	[ftl_layout_region_type_p2l_ckpt_gc_next] = {},
	[ftl_layout_region_type_p2l_ckpt_comp] = {},
	[ftl_layout_region_type_p2l_ckpt_comp_next] = {},
	[ftl_layout_region_type_trim_md] = {},
	[ftl_layout_region_type_trim_md_mirror] = {},
};

SPDK_STATIC_ASSERT(sizeof(layout_upgrade_desc) / sizeof(*layout_upgrade_desc) == ftl_layout_region_type_max,
	"Missing layout upgrade descriptors");

#include "ftl/upgrade/ftl_sb_upgrade.c"
#include "ftl/upgrade/ftl_layout_upgrade.c"
#include "ftl/mngt/ftl_mngt_md.c"

DEFINE_STUB_V(ftl_mngt_fail_step, (struct ftl_mngt * mngt));
DEFINE_STUB_V(ftl_mngt_next_step, (struct ftl_mngt * mngt));
DEFINE_STUB_V(ftl_md_persist, (struct ftl_md * md));
DEFINE_STUB(ftl_nv_cache_load_state, int, (struct ftl_nv_cache * nv_cache), 0);
DEFINE_STUB_V(ftl_valid_map_load_state, (struct spdk_ftl_dev * dev));
DEFINE_STUB_V(ftl_bands_load_state, (struct spdk_ftl_dev * dev));
DEFINE_STUB(ftl_md_get_region, const struct ftl_layout_region *, (struct ftl_md * md), 0);
DEFINE_STUB_V(ftl_md_restore, (struct ftl_md * md));
DEFINE_STUB(ftl_nv_cache_save_state, int, (struct ftl_nv_cache * nv_cache), 0);
DEFINE_STUB(ftl_mngt_get_step_cntx, void *, (struct ftl_mngt * mngt), 0);
DEFINE_STUB_V(ftl_mngt_persist_bands_p2l, (struct ftl_mngt * mngt));
DEFINE_STUB_V(ftl_band_init_gc_iter, (struct spdk_ftl_dev * dev));

struct spdk_ftl_dev g_dev;
struct ftl_superblock_shm g_sb_shm = {0};
static uint8_t g_sb_buf[FTL_SUPERBLOCK_SIZE] = {0};

#define TEST_OP 0x1984
#define TEST_REG_BLKS 0x10000
#define TEST_NVC_BLKS 0x1000000;
#define TEST_BTM_BLKS 0x1000000000;

static int
test_setup(void)
{
	// setup a dummy dev:
	g_dev.sb = (void *)g_sb_buf;
	g_dev.sb_shm = &g_sb_shm;
	g_dev.conf.use_append = 0;
	g_dev.conf.lba_rsvd = TEST_OP;
	for (uint64_t n = 0; n < sizeof(g_dev.uuid); n++)
		g_dev.uuid.u.raw[n] = n;

	g_dev.layout.nvc.total_blocks = TEST_NVC_BLKS;
	g_dev.layout.btm.total_blocks = TEST_BTM_BLKS;

	for (int regno = 0; regno < ftl_layout_region_type_max; regno++) {
		struct ftl_layout_region *reg = &g_dev.layout.region[regno];
		reg->current.blocks = TEST_REG_BLKS;
		reg->current.offset = regno * TEST_REG_BLKS;
		uint32_t reg_ver = layout_upgrade_desc[regno].reg_upgrade_desc_sz;
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
	test_setup_sb_v3(true);
	CU_ASSERT_EQUAL(ftl_superblock_md_layout_is_empty(&sb->v3), true);

	// load failed: empty md list:
	int rc = ftl_superblock_md_layout_load_all(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);

	// create md layout:
	ftl_superblock_md_layout_build(&g_dev);
	CU_ASSERT_EQUAL(ftl_superblock_md_layout_is_empty(&sb->v3), false);

	ftl_df_obj_id df_next = sb->v3.md_layout_head.df_next;

	// unsupported/fixed md region:
	uint32_t md_type = sb->v3.md_layout_head.type;
	struct ftl_layout_region *reg = &g_dev.layout.region[md_type];
	assert(md_type == ftl_layout_region_type_l2p);
	sb->v3.md_layout_head.df_next = FTL_SUPERBLOCK_SIZE - sizeof(sb->v3.md_layout_head);
	sb->v3.md_layout_head.version = 0;
	struct ftl_superblock_md_region *sb_reg = ftl_df_get_obj_ptr(sb, sb->v3.md_layout_head.df_next);
	rc = superblock_md_layout_add(&g_dev, sb_reg, md_type, 2,
								  sb->v3.md_layout_head.blk_offs, sb->v3.md_layout_head.blk_sz);
	CU_ASSERT_EQUAL(rc, 0);
	sb_reg->df_next = df_next;
	sb_reg->blk_offs = 0x1984;
	sb_reg->blk_sz = 0x0514;
	rc = ftl_superblock_md_layout_load_all(&g_dev);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(reg->current.version, 3);
	CU_ASSERT_EQUAL(reg->prev.version, 0);

	struct ftl_layout_upgrade_ctx ctx = {0};
	ctx.reg = &g_dev.layout.region[0];
	ctx.upgrade = &layout_upgrade_desc[0];
	for (int reg_type = 0; reg_type < ftl_layout_region_type_max; reg_type++, ctx.reg++, ctx.upgrade++) {
		if (reg_type == ftl_layout_region_type_sb || reg_type == ftl_layout_region_type_sb_btm) {
			ctx.reg->prev.version = g_dev.sb->header.version;
		}
		rc = region_verify(&g_dev, &ctx);
		CU_ASSERT_EQUAL(rc, 0);
	}

	ctx.reg = &g_dev.layout.region[0];
	ctx.upgrade = &layout_upgrade_desc[0];
	int upgrades = 0;
	while (true) {
		rc = layout_upgrade_select_next_region(&g_dev, &ctx);
		if (rc == ftl_layout_upgrade_done) {
			break;
		}
		CU_ASSERT_EQUAL(rc, ftl_layout_upgrade_continue);
		CU_ASSERT_EQUAL(ctx.reg->type, ftl_layout_region_type_l2p);
		upgrades++;

		uint64_t prev_ver = ctx.reg->prev.version;
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

	// no more upgrades:
	ctx.reg = &g_dev.layout.region[0];
	ctx.upgrade = &layout_upgrade_desc[0];
	rc = layout_upgrade_select_next_region(&g_dev, &ctx);
	CU_ASSERT_EQUAL(rc, ftl_layout_upgrade_done);

	// restore the sb:
	sb->v3.md_layout_head.df_next = df_next;
}

int l2p_upgrade_v0_to_v1(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	struct ftl_layout_region *region = ctx->reg;
	// minor
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

int l2p_upgrade_v1_to_v2(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	struct ftl_layout_region *region = ctx->reg;
	// major
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

int l2p_upgrade_v2_to_v3(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx)
{
	struct ftl_layout_region *region = ctx->reg;
	// minor
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

int main(int argc, char **argv)
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
