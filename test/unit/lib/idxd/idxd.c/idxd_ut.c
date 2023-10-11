/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2023 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk_internal/cunit.h"
#include "spdk_internal/mock.h"
#include "spdk_internal/idxd.h"
#include "common/lib/test_env.c"

#include "idxd/idxd.c"

static void
test_idxd_validate_dif_common_params(void)
{
	struct spdk_dif_ctx dif_ctx;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	int rc;

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	/* Check all supported combinations of the block size and metadata size */
	/* ## supported: block-size = 512, metadata = 8 */
	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_512 + METADATA_SIZE_8,
			       METADATA_SIZE_8,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);
	rc = idxd_validate_dif_common_params(&dif_ctx);
	CU_ASSERT(rc == 0);

	/* ## supported: block-size = 512, metadata = 16 */
	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_512 + METADATA_SIZE_16,
			       METADATA_SIZE_16,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);
	rc = idxd_validate_dif_common_params(&dif_ctx);
	CU_ASSERT(rc == 0);

	/* ## supported: block-size = 4096, metadata = 8 */
	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_4096 + METADATA_SIZE_8,
			       METADATA_SIZE_8,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);
	rc = idxd_validate_dif_common_params(&dif_ctx);
	CU_ASSERT(rc == 0);

	/* ## supported: block-size = 4096, metadata = 16 */
	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_4096 + METADATA_SIZE_16,
			       METADATA_SIZE_16,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);
	rc = idxd_validate_dif_common_params(&dif_ctx);
	CU_ASSERT(rc == 0);

	/* Check byte offset from the start of the whole data buffer */
	/* ## not-supported: data_offset != 0 */
	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_512 + METADATA_SIZE_8,
			       METADATA_SIZE_8,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 10, 0, &dif_opts);
	CU_ASSERT(rc == 0);
	rc = idxd_validate_dif_common_params(&dif_ctx);
	CU_ASSERT(rc == -EINVAL);

	/* Check seed value for guard computation */
	/* ## not-supported: guard_seed != 0 */
	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_512 + METADATA_SIZE_8,
			       METADATA_SIZE_8,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 10, &dif_opts);
	CU_ASSERT(rc == 0);
	rc = idxd_validate_dif_common_params(&dif_ctx);
	CU_ASSERT(rc == -EINVAL);

	/* Check for supported metadata sizes */
	/* ## not-supported: md_size != 8 or md_size != 16 */
	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_4096 + 32,
			       32,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);
	rc = idxd_validate_dif_common_params(&dif_ctx);
	CU_ASSERT(rc == -EINVAL);

	/* Check for supported metadata locations */
	/* ## not-supported: md_interleave == false (separated metadata location) */
	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_4096,
			       METADATA_SIZE_16,
			       false,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);
	rc = idxd_validate_dif_common_params(&dif_ctx);
	CU_ASSERT(rc == -EINVAL);

	/* Check for supported DIF alignments */
	/* ## not-supported: dif_loc == true (DIF left alignment) */
	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_4096 + METADATA_SIZE_16,
			       METADATA_SIZE_16,
			       true,
			       true,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);
	rc = idxd_validate_dif_common_params(&dif_ctx);
	CU_ASSERT(rc == -EINVAL);

	/* Check for supported DIF block sizes */
	/* ## not-supported: block_size (without metadata) != 512,520,4096,4104 */
	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_512 + 10,
			       METADATA_SIZE_8,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);
	rc = idxd_validate_dif_common_params(&dif_ctx);
	CU_ASSERT(rc == -EINVAL);

	/* Check for supported DIF PI formats */
	/* ## not-supported: DIF PI format == 32 */
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_32;
	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_4096 + METADATA_SIZE_16,
			       METADATA_SIZE_16,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);
	rc = idxd_validate_dif_common_params(&dif_ctx);
	CU_ASSERT(rc == -EINVAL);

	/* ## not-supported: DIF PI format == 64 */
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_64;
	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_4096 + METADATA_SIZE_16,
			       METADATA_SIZE_16,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);
	rc = idxd_validate_dif_common_params(&dif_ctx);
	CU_ASSERT(rc == -EINVAL);
}

static void
test_idxd_validate_dif_check_params(void)
{
	struct spdk_dif_ctx dif_ctx;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	int rc;

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_512 + METADATA_SIZE_8,
			       METADATA_SIZE_8,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);

	rc = idxd_validate_dif_check_params(&dif_ctx);
	CU_ASSERT(rc == 0);
}

static void
test_idxd_validate_dif_insert_params(void)
{
	struct spdk_dif_ctx dif_ctx;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	int rc;

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	/* Check for required DIF flags */
	/* ## supported: Guard, ApplicationTag, ReferenceTag check flags set */
	rc = spdk_dif_ctx_init(&dif_ctx,
			       512 + 8,
			       8,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);
	rc = idxd_validate_dif_insert_params(&dif_ctx);
	CU_ASSERT(rc == 0);

	/* ## not-supported: Guard check flag not set */
	rc = spdk_dif_ctx_init(&dif_ctx,
			       512 + 8,
			       8,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_APPTAG_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);
	rc = idxd_validate_dif_insert_params(&dif_ctx);
	CU_ASSERT(rc == -EINVAL);

	/* ## not-supported: Application Tag check flag not set */
	rc = spdk_dif_ctx_init(&dif_ctx,
			       512 + 8,
			       8,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);
	rc = idxd_validate_dif_insert_params(&dif_ctx);
	CU_ASSERT(rc == -EINVAL);

	/* ## not-supported: Reference Tag check flag not set */
	rc = spdk_dif_ctx_init(&dif_ctx,
			       512 + 8,
			       8,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);
	rc = idxd_validate_dif_insert_params(&dif_ctx);
	CU_ASSERT(rc == -EINVAL);
}

static void
test_idxd_validate_dif_check_buf_align(void)
{
	struct spdk_dif_ctx dif_ctx;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	int rc;

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_512 + METADATA_SIZE_8,
			       METADATA_SIZE_8,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);

	rc = idxd_validate_dif_check_buf_align(&dif_ctx, 4 * (512 + 8));
	CU_ASSERT(rc == 0);

	/* The memory buffer length is not a multiple of block size with metadata */
	rc = idxd_validate_dif_check_buf_align(&dif_ctx, 4 * (512 + 8) + 10);
	CU_ASSERT(rc == -EINVAL);
}

static void
test_idxd_validate_dif_insert_buf_align(void)
{
	struct spdk_dif_ctx dif_ctx;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	int rc;

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&dif_ctx,
			       512 + 8,
			       8,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);

	/* The memory source and destination buffer length set correctly */
	rc = idxd_validate_dif_insert_buf_align(&dif_ctx, 4 * 512, 4 * 520);
	CU_ASSERT(rc == 0);

	/* The memory source buffer length is not a multiple of block size without metadata */
	rc = idxd_validate_dif_insert_buf_align(&dif_ctx, 4 * 512 + 10, 4 * 520);
	CU_ASSERT(rc == -EINVAL);

	/* The memory destination buffer length is not a multiple of block size with metadata */
	rc = idxd_validate_dif_insert_buf_align(&dif_ctx, 4 * 512, 4 * 520 + 10);
	CU_ASSERT(rc == -EINVAL);

	/* The memory source and destiantion must hold the same number of blocks */
	rc = idxd_validate_dif_insert_buf_align(&dif_ctx, 4 * 512, 5 * 520);
	CU_ASSERT(rc == -EINVAL);
}

static void
test_idxd_get_dif_flags(void)
{
	struct spdk_dif_ctx dif_ctx;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	uint8_t flags;
	int rc;

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_512 + METADATA_SIZE_8,
			       METADATA_SIZE_8,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);

	rc = idxd_get_dif_flags(&dif_ctx, &flags);
	CU_ASSERT(rc == 0);
	CU_ASSERT(flags == IDXD_DIF_FLAG_DIF_BLOCK_SIZE_512);

	dif_ctx.guard_interval = 100;
	rc = idxd_get_dif_flags(&dif_ctx, &flags);
	CU_ASSERT(rc == -EINVAL);

	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_520 + METADATA_SIZE_8,
			       METADATA_SIZE_8,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);

	rc = idxd_get_dif_flags(&dif_ctx, &flags);
	CU_ASSERT(rc == 0);
	CU_ASSERT(flags == IDXD_DIF_FLAG_DIF_BLOCK_SIZE_520);

	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_4096 + METADATA_SIZE_8,
			       METADATA_SIZE_8,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);

	rc = idxd_get_dif_flags(&dif_ctx, &flags);
	CU_ASSERT(rc == 0);
	CU_ASSERT(flags == IDXD_DIF_FLAG_DIF_BLOCK_SIZE_4096);

	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_4104 + METADATA_SIZE_8,
			       METADATA_SIZE_8,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK |
			       SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);

	rc = idxd_get_dif_flags(&dif_ctx, &flags);
	CU_ASSERT(rc == 0);
	CU_ASSERT(flags == IDXD_DIF_FLAG_DIF_BLOCK_SIZE_4104);
}

static void
test_idxd_get_source_dif_flags(void)
{
	struct spdk_dif_ctx dif_ctx;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	uint8_t flags;
	int rc;

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_512 + METADATA_SIZE_8,
			       METADATA_SIZE_8,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       0,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);

	rc = idxd_get_source_dif_flags(&dif_ctx, &flags);
	CU_ASSERT(rc == 0);
	CU_ASSERT(flags == (IDXD_DIF_SOURCE_FLAG_GUARD_CHECK_DISABLE |
			    IDXD_DIF_SOURCE_FLAG_REF_TAG_CHECK_DISABLE |
			    IDXD_DIF_SOURCE_FLAG_APP_TAG_F_DETECT));

	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_512 + METADATA_SIZE_8,
			       METADATA_SIZE_8,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);

	rc = idxd_get_source_dif_flags(&dif_ctx, &flags);
	CU_ASSERT(rc == 0);
	CU_ASSERT(flags == (IDXD_DIF_SOURCE_FLAG_APP_TAG_F_DETECT));

	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_512 + METADATA_SIZE_8,
			       METADATA_SIZE_8,
			       true,
			       false,
			       SPDK_DIF_TYPE3,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_REFTAG_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);

	rc = idxd_get_source_dif_flags(&dif_ctx, &flags);
	CU_ASSERT(rc == 0);
	CU_ASSERT(flags == (IDXD_DIF_SOURCE_FLAG_APP_AND_REF_TAG_F_DETECT));

	dif_ctx.dif_type = 0xF;
	rc = idxd_get_source_dif_flags(&dif_ctx, &flags);
	CU_ASSERT(rc == -EINVAL);
}

static void
test_idxd_get_app_tag_mask(void)
{
	struct spdk_dif_ctx dif_ctx;
	struct spdk_dif_ctx_init_ext_opts dif_opts;
	uint16_t app_tag_mask, app_tag_mask_expected;
	int rc;

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;

	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_512 + METADATA_SIZE_8,
			       METADATA_SIZE_8,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK,
			       0, 0, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);

	rc = idxd_get_app_tag_mask(&dif_ctx, &app_tag_mask);
	CU_ASSERT(rc == 0);
	app_tag_mask_expected = 0xFFFF;
	CU_ASSERT(app_tag_mask == app_tag_mask_expected);

	rc = spdk_dif_ctx_init(&dif_ctx,
			       DATA_BLOCK_SIZE_512 + METADATA_SIZE_8,
			       METADATA_SIZE_8,
			       true,
			       false,
			       SPDK_DIF_TYPE1,
			       SPDK_DIF_FLAGS_GUARD_CHECK | SPDK_DIF_FLAGS_APPTAG_CHECK,
			       0, 10, 0, 0, 0, &dif_opts);
	CU_ASSERT(rc == 0);

	rc = idxd_get_app_tag_mask(&dif_ctx, &app_tag_mask);
	CU_ASSERT(rc == 0);
	app_tag_mask_expected = ~dif_ctx.apptag_mask;
	CU_ASSERT(app_tag_mask == app_tag_mask_expected);
}

int
main(int argc, char **argv)
{
	CU_pSuite   suite = NULL;
	unsigned int    num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("idxd", NULL, NULL);

	CU_ADD_TEST(suite, test_idxd_validate_dif_common_params);
	CU_ADD_TEST(suite, test_idxd_validate_dif_check_params);
	CU_ADD_TEST(suite, test_idxd_validate_dif_check_buf_align);
	CU_ADD_TEST(suite, test_idxd_validate_dif_insert_params);
	CU_ADD_TEST(suite, test_idxd_validate_dif_insert_buf_align);
	CU_ADD_TEST(suite, test_idxd_get_dif_flags);
	CU_ADD_TEST(suite, test_idxd_get_source_dif_flags);
	CU_ADD_TEST(suite, test_idxd_get_app_tag_mask);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
