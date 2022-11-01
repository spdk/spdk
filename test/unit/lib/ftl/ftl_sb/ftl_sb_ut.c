/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include <sys/queue.h>

#include "spdk/stdinc.h"

#include "spdk_cunit.h"
#include "common/lib/test_env.c"

#include "ftl/ftl_sb.c"
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
		reg->current.blocks = TEST_REG_BLKS;
		reg->current.offset = regno * TEST_REG_BLKS;
		reg->current.version = FTL_SB_VERSION_CURRENT;
		reg->prev.version = FTL_SB_VERSION_CURRENT;
		reg->type = regno;
		reg->name = "region_test";
		reg->bdev_desc = 0;
		reg->ioch = 0;
	}
	return 0;
}

static void
test_setup_sb_ver(uint64_t ver, uint64_t clean)
{
	union ftl_superblock_ver *sb = (void *)g_sb_buf;

	memset(&g_sb_buf, 0, sizeof(g_sb_buf));
	ftl_mngt_init_default_sb(&g_dev, NULL);
	if (ver <= FTL_SB_VERSION_3) {
		sb->header.magic = FTL_SUPERBLOCK_MAGIC_V2;
	}
	sb->header.version = ver;
	sb->v2.clean = clean;
	sb->v3.md_layout_head.type = 0;
	sb->v3.md_layout_head.df_next = 0;
	sb->header.crc = get_sb_crc(&sb->v3);
}

static void
test_setup_sb_v2(uint64_t clean)
{
	test_setup_sb_ver(FTL_SB_VERSION_2, clean);
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
test_sb_crc_v2(void)
{
	union ftl_superblock_ver *sb = (void *)g_sb_buf;
	uint64_t crc;

	/* v2-specific crc: it's not really working */
	test_setup_sb_v2(true);
	crc = sb->header.crc;

	sb->header.crc++;
	sb->header.crc = get_sb_crc(&sb->v3);
	CU_ASSERT_EQUAL(crc, sb->header.crc);

	g_sb_buf[sizeof(struct ftl_superblock_v2)]++;
	sb->header.crc = get_sb_crc(&sb->v3);
	CU_ASSERT_EQUAL(crc, sb->header.crc);

	g_sb_buf[sizeof(g_sb_buf) - 1]++;
	sb->header.crc = get_sb_crc(&sb->v3);
	CU_ASSERT_EQUAL(crc, sb->header.crc);

	sb->header.version += 0x19840514;
	sb->v2.seq_id++;
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
	sb->header.crc = get_sb_crc(&sb->v3);
	CU_ASSERT_EQUAL(crc, sb->header.crc);
	crc = sb->header.crc;

	g_sb_buf[sizeof(struct ftl_superblock_v2)]++;
	sb->header.crc = get_sb_crc(&sb->v3);
	CU_ASSERT_NOT_EQUAL(crc, sb->header.crc);
	crc = sb->header.crc;

	g_sb_buf[sizeof(g_sb_buf) - 1]++;
	sb->header.crc = get_sb_crc(&sb->v3);
	CU_ASSERT_NOT_EQUAL(crc, sb->header.crc);
	crc = sb->header.crc;

	sb->header.version += 500;
	sb->v2.seq_id++;
	CU_ASSERT_EQUAL(crc, sb->header.crc);
	crc = sb->header.crc;
}

static void
test_sb_v3_md_layout(void)
{
	struct ftl_superblock_md_region *sb_reg, *sb_reg2;
	struct ftl_layout_region *reg;
	union ftl_superblock_ver *sb = (void *)g_sb_buf;
	ftl_df_obj_id df_next;
	uint32_t md_type;
	int rc;

	test_setup_sb_v3(false);
	CU_ASSERT_EQUAL(ftl_superblock_md_layout_is_empty(&sb->v3), true);

	/* load failed: empty md list: */
	rc = ftl_superblock_md_layout_load_all(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);

	/* create md layout: */
	ftl_superblock_md_layout_build(&g_dev);
	CU_ASSERT_EQUAL(ftl_superblock_md_layout_is_empty(&sb->v3), false);

	/* buf overflow, sb_reg = 1 byte overflow: */
	df_next = sb->v3.md_layout_head.df_next;
	sb->v3.md_layout_head.df_next = FTL_SUPERBLOCK_SIZE - sizeof(sb->v3.md_layout_head) + 1;
	rc = ftl_superblock_md_layout_load_all(&g_dev);
	CU_ASSERT_EQUAL(rc, -EOVERFLOW);

	/* buf underflow, sb_reg = -1: */
	sb->v3.md_layout_head.df_next = UINTPTR_MAX - (uintptr_t)sb;
	rc = ftl_superblock_md_layout_load_all(&g_dev);
	CU_ASSERT_EQUAL(rc, -EOVERFLOW);

	/* buf underflow, sb_reg = 2 bytes underflow */
	sb->v3.md_layout_head.df_next = UINTPTR_MAX - 1;
	rc = ftl_superblock_md_layout_load_all(&g_dev);
	CU_ASSERT_EQUAL(rc, -EOVERFLOW);

	/* looping md layout list: */
	sb->v3.md_layout_head.df_next = ftl_df_get_obj_id(sb, &sb->v3.md_layout_head);
	rc = ftl_superblock_md_layout_load_all(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);

	sb->v3.md_layout_head.df_next = df_next;

	/* unsupported/fixed md region: */
	md_type = sb->v3.md_layout_head.type;
	sb->v3.md_layout_head.type = FTL_LAYOUT_REGION_TYPE_SB;
	rc = ftl_superblock_md_layout_load_all(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);

	/* unsupported/invalid md region: */
	sb->v3.md_layout_head.type = FTL_LAYOUT_REGION_TYPE_MAX;
	rc = ftl_superblock_md_layout_load_all(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);

	/* restore the sb: */
	sb->v3.md_layout_head.type = md_type;

	/* load succeeded, no prev version found: */
	reg = &g_dev.layout.region[md_type];
	rc = ftl_superblock_md_layout_load_all(&g_dev);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_EQUAL(reg->current.version, reg->prev.version);
	CU_ASSERT_NOT_EQUAL(reg->current.sb_md_reg, NULL);
	CU_ASSERT_EQUAL(reg->prev.sb_md_reg, NULL);

	/* load succeeded, prev (upgrade, i.e. no current) version discovery: */
	reg = &g_dev.layout.region[md_type];
	sb->v3.md_layout_head.version--;
	rc = ftl_superblock_md_layout_load_all(&g_dev);
	sb->v3.md_layout_head.version++;
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_NOT_EQUAL(reg->current.version, reg->prev.version);
	CU_ASSERT_EQUAL(reg->current.sb_md_reg, NULL);
	CU_ASSERT_NOT_EQUAL(reg->prev.sb_md_reg, NULL);

	/* load failed, unknown (newer) version found: */
	sb->v3.md_layout_head.df_next = FTL_SUPERBLOCK_SIZE - sizeof(sb->v3.md_layout_head);
	sb_reg = ftl_df_get_obj_ptr(sb, sb->v3.md_layout_head.df_next);
	rc = superblock_md_layout_add(&g_dev, sb_reg, md_type, FTL_SB_VERSION_CURRENT + 1,
				      sb->v3.md_layout_head.blk_offs, sb->v3.md_layout_head.blk_sz);
	CU_ASSERT_EQUAL(rc, 0);
	sb_reg->df_next = df_next;
	rc = ftl_superblock_md_layout_load_all(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);

	/* load succeeded, prev version discovery: */
	sb_reg->version = FTL_SB_VERSION_2;
	rc = ftl_superblock_md_layout_load_all(&g_dev);
	CU_ASSERT_EQUAL(rc, 0);
	CU_ASSERT_NOT_EQUAL(reg->current.version, reg->prev.version);
	CU_ASSERT_EQUAL(reg->current.version, FTL_SB_VERSION_CURRENT);
	CU_ASSERT_EQUAL(reg->prev.version, FTL_SB_VERSION_2);

	/* looping/multiple (same ver) prev regions found: */
	sb_reg->df_next = FTL_SUPERBLOCK_SIZE - 2 * sizeof(sb->v3.md_layout_head);
	sb_reg2 = ftl_df_get_obj_ptr(sb, sb_reg->df_next);
	rc = superblock_md_layout_add(&g_dev, sb_reg2, md_type, FTL_SB_VERSION_2,
				      sb->v3.md_layout_head.blk_offs, sb->v3.md_layout_head.blk_sz);
	CU_ASSERT_EQUAL(rc, 0);
	sb_reg2->df_next = df_next;
	rc = ftl_superblock_md_layout_load_all(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);

	/* multiple (different ver) prev regions found: */
	sb_reg->df_next = FTL_SUPERBLOCK_SIZE - 2 * sizeof(sb->v3.md_layout_head);
	sb_reg2 = ftl_df_get_obj_ptr(sb, sb_reg->df_next);
	rc = superblock_md_layout_add(&g_dev, sb_reg2, md_type, FTL_SB_VERSION_1,
				      sb->v3.md_layout_head.blk_offs, sb->v3.md_layout_head.blk_sz);
	CU_ASSERT_EQUAL(rc, 0);
	sb_reg2->df_next = df_next;
	rc = ftl_superblock_md_layout_load_all(&g_dev);
	CU_ASSERT_EQUAL(rc, 0);

	/* multiple current regions found: */
	sb->v3.md_layout_head.df_next = FTL_SUPERBLOCK_SIZE - sizeof(sb->v3.md_layout_head);
	sb_reg2->version = FTL_SB_VERSION_CURRENT;
	rc = ftl_superblock_md_layout_load_all(&g_dev);
	CU_ASSERT_NOT_EQUAL(rc, 0);

	/* restore the sb: */
	sb->v3.md_layout_head.df_next = df_next;
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("ftl_sb", test_setup, NULL);

	CU_ADD_TEST(suite, test_sb_crc_v2);
	CU_ADD_TEST(suite, test_sb_crc_v3);
	CU_ADD_TEST(suite, test_sb_v3_md_layout);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
