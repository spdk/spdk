/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "spdk/env.h"

#include "common/lib/ut_multithread.c"

#include "bdev/raid/raid1.c"
#include "../common.c"

static enum spdk_bdev_io_status g_io_status;
static struct spdk_bdev_desc *g_last_io_desc;
static spdk_bdev_io_completion_cb g_last_io_cb;

DEFINE_STUB_V(raid_bdev_module_list_add, (struct raid_bdev_module *raid_module));
DEFINE_STUB_V(raid_bdev_module_stop_done, (struct raid_bdev *raid_bdev));
DEFINE_STUB_V(spdk_bdev_free_io, (struct spdk_bdev_io *bdev_io));
DEFINE_STUB_V(raid_bdev_queue_io_wait, (struct raid_bdev_io *raid_io, struct spdk_bdev *bdev,
					struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn));
DEFINE_STUB_V(raid_bdev_process_request_complete, (struct raid_bdev_process_request *process_req,
		int status));
DEFINE_STUB_V(raid_bdev_io_init, (struct raid_bdev_io *raid_io,
				  struct raid_bdev_io_channel *raid_ch,
				  enum spdk_bdev_io_type type, uint64_t offset_blocks,
				  uint64_t num_blocks, struct iovec *iovs, int iovcnt, void *md_buf,
				  struct spdk_memory_domain *memory_domain, void *memory_domain_ctx));
DEFINE_STUB(raid_bdev_remap_dix_reftag, int, (void *md_buf, uint64_t num_blocks,
		struct spdk_bdev *bdev, uint32_t remapped_offset), -1);
DEFINE_STUB(spdk_bdev_notify_blockcnt_change, int, (struct spdk_bdev *bdev, uint64_t size), 0);
DEFINE_STUB(spdk_bdev_flush_blocks, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
		void *cb_arg), 0);
DEFINE_STUB(spdk_bdev_unmap_blocks, int, (struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		uint64_t offset_blocks, uint64_t num_blocks, spdk_bdev_io_completion_cb cb,
		void *cb_arg), 0);

int
spdk_bdev_readv_blocks_ext(struct spdk_bdev_desc *desc,
			   struct spdk_io_channel *ch,
			   struct iovec *iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg, struct spdk_bdev_ext_io_opts *opts)
{
	g_last_io_desc = desc;
	g_last_io_cb = cb;

	return 0;
}

int
spdk_bdev_writev_blocks_ext(struct spdk_bdev_desc *desc,
			    struct spdk_io_channel *ch,
			    struct iovec *iov, int iovcnt, uint64_t offset_blocks, uint64_t num_blocks,
			    spdk_bdev_io_completion_cb cb, void *cb_arg, struct spdk_bdev_ext_io_opts *opts)
{
	g_last_io_desc = desc;
	g_last_io_cb = cb;

	return 0;
}

void
raid_bdev_fail_base_bdev(struct raid_base_bdev_info *base_info)
{
	base_info->is_failed = true;
}

static int
test_setup(void)
{
	uint8_t num_base_bdevs_values[] = { 2, 3 };
	uint64_t base_bdev_blockcnt_values[] = { 1, 1024, 1024 * 1024 };
	uint32_t base_bdev_blocklen_values[] = { 512, 4096 };
	uint8_t *num_base_bdevs;
	uint64_t *base_bdev_blockcnt;
	uint32_t *base_bdev_blocklen;
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
				struct raid_params params = {
					.num_base_bdevs = *num_base_bdevs,
					.base_bdev_blockcnt = *base_bdev_blockcnt,
					.base_bdev_blocklen = *base_bdev_blocklen,
				};
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

static struct raid_bdev_io *
get_raid_io(struct raid1_info *r1_info, struct raid_bdev_io_channel *raid_ch,
	    enum spdk_bdev_io_type io_type, uint64_t num_blocks)
{
	struct raid_bdev_io *raid_io;

	raid_io = calloc(1, sizeof(*raid_io));
	SPDK_CU_ASSERT_FATAL(raid_io != NULL);

	raid_test_bdev_io_init(raid_io, r1_info->raid_bdev, raid_ch, io_type, 0, num_blocks, NULL, 0, NULL);

	return raid_io;
}

static void
put_raid_io(struct raid_bdev_io *raid_io)
{
	free(raid_io);
}

void
raid_test_bdev_io_complete(struct raid_bdev_io *raid_io, enum spdk_bdev_io_status status)
{
	g_io_status = status;

	put_raid_io(raid_io);
}

static void
run_for_each_raid1_config(void (*test_fn)(struct raid_bdev *raid_bdev,
			  struct raid_bdev_io_channel *raid_ch))
{
	struct raid_params *params;

	RAID_PARAMS_FOR_EACH(params) {
		struct raid1_info *r1_info;
		struct raid_bdev_io_channel *raid_ch;

		r1_info = create_raid1(params);
		raid_ch = raid_test_create_io_channel(r1_info->raid_bdev);

		test_fn(r1_info->raid_bdev, raid_ch);

		raid_test_destroy_io_channel(raid_ch);
		delete_raid1(r1_info);
	}
}

static void
_test_raid1_read_balancing(struct raid_bdev *raid_bdev, struct raid_bdev_io_channel *raid_ch)
{
	struct raid1_info *r1_info = raid_bdev->module_private;
	struct raid1_io_channel *raid1_ch = raid_bdev_channel_get_module_ctx(raid_ch);
	uint8_t big_io_base_bdev_idx;
	const uint64_t big_io_blocks = 256;
	const uint64_t small_io_blocks = 4;
	uint64_t blocks_remaining;
	struct raid_bdev_io *raid_io;
	uint8_t i;
	int n;

	/* same sized IOs should be be spread evenly across all base bdevs */
	for (n = 0; n < 3; n++) {
		for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
			raid_io = get_raid_io(r1_info, raid_ch, SPDK_BDEV_IO_TYPE_READ, small_io_blocks);
			raid1_submit_read_request(raid_io);
			CU_ASSERT(raid_io->base_bdev_io_submitted == i);
			put_raid_io(raid_io);
		}
	}

	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		CU_ASSERT(raid1_ch->read_blocks_outstanding[i] == n * small_io_blocks);
		raid1_ch->read_blocks_outstanding[i] = 0;
	}

	/*
	 * Submit one big and many small IOs. The small IOs should not land on the same base bdev
	 * as the big until the submitted block count is matched.
	 */
	raid_io = get_raid_io(r1_info, raid_ch, SPDK_BDEV_IO_TYPE_READ, big_io_blocks);
	raid1_submit_read_request(raid_io);
	big_io_base_bdev_idx = raid_io->base_bdev_io_submitted;
	put_raid_io(raid_io);

	blocks_remaining = big_io_blocks * (raid_bdev->num_base_bdevs - 1);
	while (blocks_remaining > 0) {
		raid_io = get_raid_io(r1_info, raid_ch, SPDK_BDEV_IO_TYPE_READ, small_io_blocks);
		raid1_submit_read_request(raid_io);
		CU_ASSERT(raid_io->base_bdev_io_submitted != big_io_base_bdev_idx);
		put_raid_io(raid_io);
		blocks_remaining -= small_io_blocks;
	}

	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		CU_ASSERT(raid1_ch->read_blocks_outstanding[i] == big_io_blocks);
	}

	raid_io = get_raid_io(r1_info, raid_ch, SPDK_BDEV_IO_TYPE_READ, small_io_blocks);
	raid1_submit_read_request(raid_io);
	CU_ASSERT(raid_io->base_bdev_io_submitted == big_io_base_bdev_idx);
	put_raid_io(raid_io);
}

static void
test_raid1_read_balancing(void)
{
	run_for_each_raid1_config(_test_raid1_read_balancing);
}

static void
_test_raid1_write_error(struct raid_bdev *raid_bdev, struct raid_bdev_io_channel *raid_ch)
{
	struct raid1_info *r1_info = raid_bdev->module_private;
	struct raid_bdev_io *raid_io;
	struct raid_base_bdev_info *base_info;
	struct spdk_bdev_io bdev_io = {};
	bool bdev_io_success;

	/* first completion failed */
	g_io_status = SPDK_BDEV_IO_STATUS_PENDING;
	raid_io = get_raid_io(r1_info, raid_ch, SPDK_BDEV_IO_TYPE_WRITE, 64);
	raid1_submit_write_request(raid_io);

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		base_info->is_failed = false;
		if (raid_bdev_base_bdev_slot(base_info) == 0) {
			bdev_io_success = false;
		} else {
			bdev_io_success = true;
		}
		bdev_io.bdev = base_info->desc->bdev;
		raid1_write_bdev_io_completion(&bdev_io, bdev_io_success, raid_io);
		CU_ASSERT(base_info->is_failed == !bdev_io_success);
	}
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);

	/* all except first completion failed */
	g_io_status = SPDK_BDEV_IO_STATUS_PENDING;
	raid_io = get_raid_io(r1_info, raid_ch, SPDK_BDEV_IO_TYPE_WRITE, 64);
	raid1_submit_write_request(raid_io);

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		base_info->is_failed = false;
		if (raid_bdev_base_bdev_slot(base_info) != 0) {
			bdev_io_success = false;
		} else {
			bdev_io_success = true;
		}
		bdev_io.bdev = base_info->desc->bdev;
		raid1_write_bdev_io_completion(&bdev_io, bdev_io_success, raid_io);
		CU_ASSERT(base_info->is_failed == !bdev_io_success);
	}
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);

	/* all completions failed */
	g_io_status = SPDK_BDEV_IO_STATUS_PENDING;
	raid_io = get_raid_io(r1_info, raid_ch, SPDK_BDEV_IO_TYPE_WRITE, 64);
	raid1_submit_write_request(raid_io);

	bdev_io_success = false;
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		base_info->is_failed = false;
		bdev_io.bdev = base_info->desc->bdev;
		raid1_write_bdev_io_completion(&bdev_io, bdev_io_success, raid_io);
		CU_ASSERT(base_info->is_failed == !bdev_io_success);
	}
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_FAILED);
}

static void
test_raid1_write_error(void)
{
	run_for_each_raid1_config(_test_raid1_write_error);
}

static void
_test_raid1_read_error(struct raid_bdev *raid_bdev, struct raid_bdev_io_channel *raid_ch)
{
	struct raid1_info *r1_info = raid_bdev->module_private;
	struct raid_base_bdev_info *base_info = &raid_bdev->base_bdev_info[0];
	struct raid1_io_channel *raid1_ch = raid_bdev_channel_get_module_ctx(raid_ch);
	struct raid_bdev_io *raid_io;
	struct spdk_bdev_io bdev_io = {};

	/* first read fails, the second succeeds */
	base_info->is_failed = false;
	g_io_status = SPDK_BDEV_IO_STATUS_PENDING;
	raid_io = get_raid_io(r1_info, raid_ch, SPDK_BDEV_IO_TYPE_READ, 64);
	raid1_submit_read_request(raid_io);
	CU_ASSERT(raid_io->base_bdev_io_submitted == 0);
	CU_ASSERT(raid_io->base_bdev_io_remaining == 0);

	CU_ASSERT(g_last_io_desc == base_info->desc);
	CU_ASSERT(g_last_io_cb == raid1_read_bdev_io_completion);
	raid1_read_bdev_io_completion(&bdev_io, false, raid_io);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_PENDING);
	CU_ASSERT((uint8_t)raid_io->base_bdev_io_remaining == (raid_bdev->num_base_bdevs - 1));

	CU_ASSERT(g_last_io_desc == raid_bdev->base_bdev_info[1].desc);
	CU_ASSERT(g_last_io_cb == raid1_read_other_completion);
	raid1_read_other_completion(&bdev_io, true, raid_io);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_PENDING);

	CU_ASSERT(g_last_io_desc == base_info->desc);
	CU_ASSERT(g_last_io_cb == raid1_correct_read_error_completion);
	raid1_correct_read_error_completion(&bdev_io, true, raid_io);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(base_info->is_failed == false);

	/* rewrite fails */
	base_info->is_failed = false;
	g_io_status = SPDK_BDEV_IO_STATUS_PENDING;
	raid_io = get_raid_io(r1_info, raid_ch, SPDK_BDEV_IO_TYPE_READ, 64);
	raid1_submit_read_request(raid_io);
	CU_ASSERT(raid_io->base_bdev_io_submitted == 0);
	CU_ASSERT(raid_io->base_bdev_io_remaining == 0);

	CU_ASSERT(g_last_io_desc == base_info->desc);
	CU_ASSERT(g_last_io_cb == raid1_read_bdev_io_completion);
	raid1_read_bdev_io_completion(&bdev_io, false, raid_io);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_PENDING);
	CU_ASSERT((uint8_t)raid_io->base_bdev_io_remaining == (raid_bdev->num_base_bdevs - 1));

	CU_ASSERT(g_last_io_desc == raid_bdev->base_bdev_info[1].desc);
	CU_ASSERT(g_last_io_cb == raid1_read_other_completion);
	raid1_read_other_completion(&bdev_io, true, raid_io);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_PENDING);

	CU_ASSERT(g_last_io_desc == base_info->desc);
	CU_ASSERT(g_last_io_cb == raid1_correct_read_error_completion);
	raid1_correct_read_error_completion(&bdev_io, false, raid_io);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(base_info->is_failed == true);

	/* only the last read succeeds */
	base_info->is_failed = false;
	g_io_status = SPDK_BDEV_IO_STATUS_PENDING;
	raid_io = get_raid_io(r1_info, raid_ch, SPDK_BDEV_IO_TYPE_READ, 64);
	raid1_submit_read_request(raid_io);
	CU_ASSERT(raid_io->base_bdev_io_submitted == 0);
	CU_ASSERT(raid_io->base_bdev_io_remaining == 0);

	CU_ASSERT(g_last_io_desc == base_info->desc);
	CU_ASSERT(g_last_io_cb == raid1_read_bdev_io_completion);
	raid1_read_bdev_io_completion(&bdev_io, false, raid_io);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_PENDING);
	CU_ASSERT((uint8_t)raid_io->base_bdev_io_remaining == (raid_bdev->num_base_bdevs - 1));

	while (raid_io->base_bdev_io_remaining > 1) {
		CU_ASSERT(g_last_io_cb == raid1_read_other_completion);
		raid1_read_other_completion(&bdev_io, false, raid_io);
		CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_PENDING);
	}

	CU_ASSERT(g_last_io_desc == raid_bdev->base_bdev_info[raid_bdev->num_base_bdevs - 1].desc);
	CU_ASSERT(g_last_io_cb == raid1_read_other_completion);
	raid1_read_other_completion(&bdev_io, true, raid_io);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_PENDING);

	CU_ASSERT(g_last_io_desc == base_info->desc);
	CU_ASSERT(g_last_io_cb == raid1_correct_read_error_completion);
	raid1_correct_read_error_completion(&bdev_io, true, raid_io);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(base_info->is_failed == false);

	/* all reads fail */
	base_info->is_failed = false;
	g_io_status = SPDK_BDEV_IO_STATUS_PENDING;
	raid_io = get_raid_io(r1_info, raid_ch, SPDK_BDEV_IO_TYPE_READ, 64);
	raid1_submit_read_request(raid_io);
	CU_ASSERT(raid_io->base_bdev_io_submitted == 0);
	CU_ASSERT(raid_io->base_bdev_io_remaining == 0);

	CU_ASSERT(g_last_io_desc == base_info->desc);
	CU_ASSERT(g_last_io_cb == raid1_read_bdev_io_completion);
	raid1_read_bdev_io_completion(&bdev_io, false, raid_io);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_PENDING);
	CU_ASSERT((uint8_t)raid_io->base_bdev_io_remaining == (raid_bdev->num_base_bdevs - 1));

	while (raid_io->base_bdev_io_remaining > 1) {
		CU_ASSERT(g_last_io_cb == raid1_read_other_completion);
		raid1_read_other_completion(&bdev_io, false, raid_io);
		CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_PENDING);
	}

	CU_ASSERT(g_last_io_desc == raid_bdev->base_bdev_info[raid_bdev->num_base_bdevs - 1].desc);
	CU_ASSERT(g_last_io_cb == raid1_read_other_completion);
	raid1_read_other_completion(&bdev_io, false, raid_io);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_FAILED);
	CU_ASSERT(base_info->is_failed == true);

	/* read from base bdev #1 fails, read from #0 succeeds */
	base_info->is_failed = false;
	base_info = &raid_bdev->base_bdev_info[1];
	raid1_ch->read_blocks_outstanding[0] = 123;
	g_io_status = SPDK_BDEV_IO_STATUS_PENDING;
	raid_io = get_raid_io(r1_info, raid_ch, SPDK_BDEV_IO_TYPE_READ, 64);
	raid1_submit_read_request(raid_io);
	CU_ASSERT(raid_io->base_bdev_io_submitted == 1);
	CU_ASSERT(raid_io->base_bdev_io_remaining == 0);

	CU_ASSERT(g_last_io_desc == base_info->desc);
	CU_ASSERT(g_last_io_cb == raid1_read_bdev_io_completion);
	raid1_read_bdev_io_completion(&bdev_io, false, raid_io);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_PENDING);
	CU_ASSERT((uint8_t)raid_io->base_bdev_io_remaining == raid_bdev->num_base_bdevs);

	CU_ASSERT(g_last_io_desc == raid_bdev->base_bdev_info[0].desc);
	CU_ASSERT(g_last_io_cb == raid1_read_other_completion);
	raid1_read_other_completion(&bdev_io, true, raid_io);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_PENDING);

	CU_ASSERT(g_last_io_desc == base_info->desc);
	CU_ASSERT(g_last_io_cb == raid1_correct_read_error_completion);
	raid1_correct_read_error_completion(&bdev_io, true, raid_io);
	CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);
	CU_ASSERT(base_info->is_failed == false);

	/* base bdev #0 is failed, read from #1 fails, read from next succeeds if N > 2 */
	base_info->is_failed = false;
	raid_ch->_base_channels[0] = NULL;
	g_io_status = SPDK_BDEV_IO_STATUS_PENDING;
	raid_io = get_raid_io(r1_info, raid_ch, SPDK_BDEV_IO_TYPE_READ, 64);
	raid1_submit_read_request(raid_io);
	CU_ASSERT(raid_io->base_bdev_io_submitted == 1);
	CU_ASSERT(raid_io->base_bdev_io_remaining == 0);

	CU_ASSERT(g_last_io_desc == base_info->desc);
	CU_ASSERT(g_last_io_cb == raid1_read_bdev_io_completion);
	raid1_read_bdev_io_completion(&bdev_io, false, raid_io);
	if (raid_bdev->num_base_bdevs > 2) {
		CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_PENDING);
		CU_ASSERT((uint8_t)raid_io->base_bdev_io_remaining == (raid_bdev->num_base_bdevs - 2));

		CU_ASSERT(g_last_io_desc == raid_bdev->base_bdev_info[2].desc);
		CU_ASSERT(g_last_io_cb == raid1_read_other_completion);
		raid1_read_other_completion(&bdev_io, true, raid_io);
		CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_PENDING);

		CU_ASSERT(g_last_io_desc == base_info->desc);
		CU_ASSERT(g_last_io_cb == raid1_correct_read_error_completion);
		raid1_correct_read_error_completion(&bdev_io, true, raid_io);
		CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_SUCCESS);
		CU_ASSERT(base_info->is_failed == false);
	} else {
		CU_ASSERT(g_io_status == SPDK_BDEV_IO_STATUS_FAILED);
		CU_ASSERT(base_info->is_failed == true);
	}
}

static void
test_raid1_read_error(void)
{
	run_for_each_raid1_config(_test_raid1_read_error);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("raid1", test_setup, test_cleanup);
	CU_ADD_TEST(suite, test_raid1_start);
	CU_ADD_TEST(suite, test_raid1_read_balancing);
	CU_ADD_TEST(suite, test_raid1_write_error);
	CU_ADD_TEST(suite, test_raid1_read_error);

	allocate_threads(1);
	set_thread(0);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	free_threads();

	return num_failures;
}
