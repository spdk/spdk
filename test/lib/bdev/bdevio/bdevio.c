/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <rte_config.h>
#include <rte_eal.h>
#include <rte_malloc.h>
#include <rte_ring.h>

#include "spdk/bdev.h"
#include "spdk/copy_engine.h"
#include "spdk/log.h"

#include "CUnit/Basic.h"

#define BUFFER_SIZE 		260 * 1024
#define BDEV_TASK_ARRAY_SIZE	2048

#include "../common.c"

struct io_target {
	struct spdk_bdev	*bdev;
	struct io_target	*next;
};

struct io_target *g_io_targets = NULL;

static int
bdevio_construct_targets(void)
{
	struct spdk_bdev *bdev;
	struct io_target *target;

	bdev = spdk_bdev_first();
	while (bdev != NULL) {

		if (bdev->claimed) {
			bdev = spdk_bdev_next(bdev);
			continue;
		}

		target = malloc(sizeof(struct io_target));
		if (target == NULL) {
			return -ENOMEM;
		}
		target->bdev = bdev;
		target->next = g_io_targets;
		g_io_targets = target;

		bdev = spdk_bdev_next(bdev);
	}

	return 0;
}

static int complete;
static enum spdk_bdev_io_status completion_status_per_io;

static void
initialize_buffer(char **buf, int pattern, int size)
{
	*buf = rte_malloc(NULL, size, 0x1000);
	memset(*buf, pattern, size);
}

static void
quick_test_complete(spdk_event_t event)
{
	struct spdk_bdev_io *bdev_io = spdk_event_get_arg2(event);

	completion_status_per_io = bdev_io->status;
	complete = 1;

	spdk_bdev_free_io(bdev_io);
}

static int
check_io_completion(void)
{
	int rc;
	struct spdk_bdev *bdev;

	rc = 0;
	while (!complete) {
		bdev = spdk_bdev_first();
		while (bdev != NULL) {
			spdk_bdev_do_work(bdev);
			bdev = spdk_bdev_next(bdev);
		}
		spdk_event_queue_run_all(rte_lcore_id());
	}
	return rc;
}

struct iovec iov;

static int
blockdev_write(struct io_target *target, void *bdev_task_ctx, char *tx_buf,
	       uint64_t offset, int data_len)
{
	struct spdk_bdev_io *bdev_io;

	complete = 0;
	completion_status_per_io = SPDK_BDEV_IO_STATUS_FAILED;

	iov.iov_base = tx_buf;
	iov.iov_len = data_len;
	bdev_io = spdk_bdev_writev(target->bdev, &iov, 1, (uint64_t)offset,
				   iov.iov_len, quick_test_complete,
				   bdev_task_ctx);
	if (!bdev_io) {
		return -1;
	}

	return data_len;
}

static int
blockdev_read(struct io_target *target, void *bdev_task_ctx, char *rx_buf,
	      uint64_t offset, int data_len)
{
	struct spdk_bdev_io *bdev_io;

	complete = 0;
	completion_status_per_io = SPDK_BDEV_IO_STATUS_FAILED;

	bdev_io = spdk_bdev_read(target->bdev, rx_buf, offset, data_len,
				 quick_test_complete, bdev_task_ctx);

	if (!bdev_io) {
		return -1;
	}

	return data_len;
}

static int
blockdev_write_read_data_match(char *rx_buf, char *tx_buf, int data_length)
{
	int rc;
	rc = memcmp(rx_buf, tx_buf, data_length);

	rte_free(rx_buf);
	rte_free(tx_buf);

	return rc;
}

static void
blockdev_write_read(uint32_t data_length, int pattern, uint64_t offset,
		    int expected_rc)
{
	struct io_target *target;
	char	bdev_task_ctx[BDEV_TASK_ARRAY_SIZE];
	char	*tx_buf = NULL;
	char	*rx_buf = NULL;
	int	rc;

	target = g_io_targets;
	while (target != NULL) {
		if (data_length < target->bdev->blocklen) {
			target = target->next;
			continue;
		}

		initialize_buffer(&tx_buf, pattern, data_length);
		initialize_buffer(&rx_buf, 0, data_length);

		rc = blockdev_write(target, (void *)bdev_task_ctx, tx_buf,
				    offset, data_length);

		/* Assert the rc of the respective blockdev */
		CU_ASSERT_EQUAL(rc, expected_rc);

		/* If the write was successful, the function returns the data_length
		 * and the completion_status_per_io is 0 */
		if (rc < (int)data_length) {
			CU_ASSERT_EQUAL(completion_status_per_io, SPDK_BDEV_IO_STATUS_FAILED);
		} else {
			check_io_completion();
			CU_ASSERT_EQUAL(completion_status_per_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		}

		rc = blockdev_read(target, (void *)bdev_task_ctx, rx_buf,
				   offset, data_length);

		/* Assert the rc of the respective blockdev */
		CU_ASSERT_EQUAL(rc, expected_rc);

		/* If the read was successful, the function returns the data_length
		 * and the completion_status_per_io is 0 */
		if (rc < (int)data_length) {
			CU_ASSERT_EQUAL(completion_status_per_io, SPDK_BDEV_IO_STATUS_FAILED);
		} else {
			check_io_completion();
			CU_ASSERT_EQUAL(completion_status_per_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		}

		if (completion_status_per_io == SPDK_BDEV_IO_STATUS_SUCCESS) {
			rc = blockdev_write_read_data_match(rx_buf, tx_buf, data_length);
			/* Assert the write by comparing it with values read
			 * from each blockdev */
			CU_ASSERT_EQUAL(rc, 0);
		}

		target = target->next;
	}
}

static void
blockdev_write_read_4k(void)
{
	uint32_t data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;

	/* Data size = 4K */
	data_length = 4096;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = 0;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is the data_length */
	expected_rc = data_length;

	blockdev_write_read(data_length, pattern, offset, expected_rc);
}

static void
blockdev_write_read_512Bytes(void)
{
	uint32_t data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;

	/* Data size = 512 */
	data_length = 512;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = 2048;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is the data_length */
	expected_rc = data_length;

	blockdev_write_read(data_length, pattern, offset, expected_rc);
}

static void
blockdev_write_read_size_gt_128k(void)
{
	uint32_t data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;

	/* Data size = 132K */
	data_length = 135168;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = 2048;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is the data_length */
	expected_rc = data_length;

	blockdev_write_read(data_length, pattern, offset, expected_rc);
}

static void
blockdev_write_read_invalid_size(void)
{
	uint32_t data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;

	/* Data size is not a multiple of the block size */
	data_length = 0x1015;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = 2048;
	pattern = 0xA3;
	/* Params are invalid, hence the expected return value
	 * of write and read for all blockdevs is < 0 */
	expected_rc = -1;

	blockdev_write_read(data_length, pattern, offset, expected_rc);
}

static void
blockdev_write_read_offset_plus_nbytes_equals_bdev_size(void)
{
	struct io_target *target;
	struct spdk_bdev *bdev;
	char	bdev_task_ctx[BDEV_TASK_ARRAY_SIZE];
	char	*tx_buf = NULL;
	char	*rx_buf = NULL;
	uint64_t offset;
	int rc;

	target = g_io_targets;
	while (target != NULL) {
		bdev = target->bdev;

		/* The start offset has been set to a marginal value
		 * such that offset + nbytes == Total size of
		 * blockdev. */
		offset = ((bdev->blockcnt - 1) * bdev->blocklen);

		initialize_buffer(&tx_buf, 0xA3, bdev->blocklen);
		initialize_buffer(&rx_buf, 0, bdev->blocklen);

		rc = blockdev_write(target, (void *)bdev_task_ctx, tx_buf,
				    offset, bdev->blocklen);

		/* Assert the rc of the respective blockdev */
		CU_ASSERT_EQUAL(rc, (int)bdev->blocklen);

		/* If the write was successful, the function returns the data_length
		 * and the completion_status_per_io is 0 */
		check_io_completion();
		CU_ASSERT_EQUAL(completion_status_per_io, SPDK_BDEV_IO_STATUS_SUCCESS);

		rc = blockdev_read(target, (void *)bdev_task_ctx, rx_buf,
				   offset, bdev->blocklen);

		/* Assert the rc of the respective blockdev */
		CU_ASSERT_EQUAL(rc, (int)bdev->blocklen);

		/* If the read was successful, the function returns the data_length
		 * and the completion_status_per_io is 0 */
		check_io_completion();
		CU_ASSERT_EQUAL(completion_status_per_io, SPDK_BDEV_IO_STATUS_SUCCESS);

		rc = blockdev_write_read_data_match(rx_buf, tx_buf, bdev->blocklen);
		/* Assert the write by comparing it with values read
		 * from each blockdev */
		CU_ASSERT_EQUAL(rc, 0);

		target = target->next;
	}
}

static void
blockdev_write_read_offset_plus_nbytes_gt_bdev_size(void)
{
	struct io_target *target;
	struct spdk_bdev *bdev;
	char	bdev_task_ctx[BDEV_TASK_ARRAY_SIZE];
	char	*tx_buf = NULL;
	char	*rx_buf = NULL;
	int	data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;
	int rc;

	/* Tests the overflow condition of the blockdevs. */
	data_length = 4096;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	pattern = 0xA3;
	/* Params are invalid, hence the expected return value
	 * of write and read is < 0.*/
	expected_rc = -1;

	target = g_io_targets;
	while (target != NULL) {
		bdev = target->bdev;

		/* The start offset has been set to a valid value
		 * but offset + nbytes is greater than the Total size
		 * of the blockdev. The test should fail. */
		offset = ((bdev->blockcnt * bdev->blocklen) - 1024);

		initialize_buffer(&tx_buf, pattern, data_length);
		initialize_buffer(&rx_buf, 0, data_length);

		rc = blockdev_write(target, (void *)bdev_task_ctx, tx_buf,
				    offset, data_length);

		/* Assert the rc of the respective blockdev */
		CU_ASSERT_EQUAL(rc, expected_rc);

		/* If the write failed, the function returns rc<data_length
		 * and the completion_status_per_io is SPDK_BDEV_IO_STATUS_FAILED */
		CU_ASSERT_EQUAL(completion_status_per_io, SPDK_BDEV_IO_STATUS_FAILED);

		rc = blockdev_read(target, (void *)bdev_task_ctx, rx_buf,
				   offset, data_length);

		/* Assert the rc of the respective blockdev */
		CU_ASSERT_EQUAL(rc, expected_rc);

		/* If the read failed, the function returns rc<data_length
		 * and the completion_status_per_io is SPDK_BDEV_IO_STATUS_FAILED */
		CU_ASSERT_EQUAL(completion_status_per_io, SPDK_BDEV_IO_STATUS_FAILED);

		target = target->next;
	}
}

static void
blockdev_write_read_max_offset(void)
{
	int	data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;

	data_length = 4096;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	/* The start offset has been set to UINT64_MAX such that
	 * adding nbytes wraps around and points to an invalid address. */
	offset = UINT64_MAX;
	pattern = 0xA3;
	/* Params are invalid, hence the expected return value
	 * of write and read for all blockdevs is < 0 */
	expected_rc = -1;

	blockdev_write_read(data_length, pattern, offset, expected_rc);
}

static void
blockdev_overlapped_write_read_8k(void)
{
	int	data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;

	/* Data size = 8K */
	data_length = 8192;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = 0;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is the data_length */
	expected_rc = data_length;
	/* Assert the write by comparing it with values read
	 * from the same offset for each blockdev */
	blockdev_write_read(data_length, pattern, offset, expected_rc);

	/* Overwrite the pattern 0xbb of size 8K on an address offset overlapping
	 * with the address written above and assert the new value in
	 * the overlapped address range */
	/* Populate 8k with value 0xBB */
	pattern = 0xBB;
	/* Offset = 6144; Overlap offset addresses and write value 0xbb */
	offset = 4096;
	/* Assert the write by comparing it with values read
	 * from the overlapped offset for each blockdev */
	blockdev_write_read(data_length, pattern, offset, expected_rc);
}


int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	const char *config_file;
	unsigned int num_failures;

	if (argc == 1) {
		config_file = "/usr/local/etc/spdk/iscsi.conf";
	} else {
		config_file = argv[1];
	}

	bdevtest_init(config_file, "0x1");

	if (bdevio_construct_targets() < 0) {
		return 1;
	}

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("components_suite", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "blockdev write read 4k", blockdev_write_read_4k) == NULL
		|| CU_add_test(suite, "blockdev write read 512 bytes",
			       blockdev_write_read_512Bytes) == NULL
		|| CU_add_test(suite, "blockdev write read size > 128k",
			       blockdev_write_read_size_gt_128k) == NULL
		|| CU_add_test(suite, "blockdev write read invalid size",
			       blockdev_write_read_invalid_size) == NULL
		|| CU_add_test(suite, "blockdev write read offset + nbytes == size of blockdev",
			       blockdev_write_read_offset_plus_nbytes_equals_bdev_size) == NULL
		|| CU_add_test(suite, "blockdev write read offset + nbytes > size of blockdev",
			       blockdev_write_read_offset_plus_nbytes_gt_bdev_size) == NULL
		|| CU_add_test(suite, "blockdev write read max offset",
			       blockdev_write_read_max_offset) == NULL
		|| CU_add_test(suite, "blockdev write read 8k on overlapped address offset",
			       blockdev_overlapped_write_read_8k) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
