/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_cunit.h"
#include "spdk/env.h"
#include "spdk_internal/mock.h"

#include "bdev/raid/raid1.c"
#include "../common.c"

DEFINE_STUB_V(raid_bdev_module_list_add, (struct raid_bdev_module *raid_module));
DEFINE_STUB_V(raid_bdev_io_complete, (struct raid_bdev_io *raid_io,
				      enum spdk_bdev_io_status status));
DEFINE_STUB(raid_bdev_io_complete_part, bool, (struct raid_bdev_io *raid_io, uint64_t completed,
		enum spdk_bdev_io_status status), true);
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *bdev_io));
DEFINE_STUB_V(raid_bdev_queue_io_wait, (struct raid_bdev_io *raid_io, struct spdk_bdev *bdev,
					struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn));
DEFINE_STUB(spdk_bdev_readv_blocks_with_md, int, (struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt, void *md,
		uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_writev_blocks_with_md, int, (struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt, void *md,
		uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_readv_blocks_ext, int, (struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg, struct spdk_bdev_ext_io_opts *opts), 0);
DEFINE_STUB(spdk_bdev_writev_blocks_ext, int, (struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg, struct spdk_bdev_ext_io_opts *opts), 0);

static int
test_setup(void)
{
	uint8_t num_base_bdevs_values[] = { 2, 3 };
	uint64_t base_bdev_blockcnt_values[] = { 1, 1024, 1024 * 1024 };
	uint32_t base_bdev_blocklen_values[] = { 512, 4096 };
	uint8_t *num_base_bdevs;
	uint64_t *base_bdev_blockcnt;
	uint32_t *base_bdev_blocklen;
	struct raid_params params;
	uint64_t params_count;
	int rc;

	params_count = SPDK_COUNTOF(num_base_bdevs_values) *
		       SPDK_COUNTOF(base_bdev_blockcnt_values) *
		       SPDK_COUNTOF(base_bdev_blocklen_values);
	rc = raid_test_params_alloc(params_count);
	if (rc) {
		return rc;
	}

	ARRAY_FOR_EACH(num_base_bdevs_values, num_base_bdevs) {
		ARRAY_FOR_EACH(base_bdev_blockcnt_values, base_bdev_blockcnt) {
			ARRAY_FOR_EACH(base_bdev_blocklen_values, base_bdev_blocklen) {
				params.num_base_bdevs = *num_base_bdevs;
				params.base_bdev_blockcnt = *base_bdev_blockcnt;
				params.base_bdev_blocklen = *base_bdev_blocklen;
				params.strip_size = 0;
				params.md_len = 0;
				raid_test_params_add(&params);
			}
		}
	}

	return 0;
}

static int
test_cleanup(void)
{
	raid_test_params_free();
	return 0;
}

static struct raid1_info *
create_raid1(struct raid_params *params)
{
	struct raid_bdev *raid_bdev = raid_test_create_raid_bdev(params, &g_raid1_module);

	SPDK_CU_ASSERT_FATAL(raid1_start(raid_bdev) == 0);

	return raid_bdev->module_private;
}

static void
delete_raid1(struct raid1_info *r1_info)
{
	struct raid_bdev *raid_bdev = r1_info->raid_bdev;

	raid1_stop(raid_bdev);

	raid_test_delete_raid_bdev(raid_bdev);
}

static void
test_raid1_start(void)
{
	struct raid_params *params;

	RAID_PARAMS_FOR_EACH(params) {
		struct raid1_info *r1_info;

		r1_info = create_raid1(params);

		SPDK_CU_ASSERT_FATAL(r1_info != NULL);

		CU_ASSERT_EQUAL(r1_info->raid_bdev->level, RAID1);
		CU_ASSERT_EQUAL(r1_info->raid_bdev->bdev.blockcnt, params->base_bdev_blockcnt);
		CU_ASSERT_PTR_EQUAL(r1_info->raid_bdev->module, &g_raid1_module);

		delete_raid1(r1_info);
	}
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("raid1", test_setup, test_cleanup);
	CU_ADD_TEST(suite, test_raid1_start);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
