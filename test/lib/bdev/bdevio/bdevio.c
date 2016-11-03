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

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "spdk/bdev.h"
#include "spdk/copy_engine.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/io_channel.h"

#include "CUnit/Basic.h"

#define BUFFER_IOVS		1024
#define BUFFER_SIZE 		260 * 1024
#define BDEV_TASK_ARRAY_SIZE	2048


#include "../common.c"

pthread_mutex_t g_test_mutex;
pthread_cond_t g_test_cond;

struct io_target {
	struct spdk_bdev	*bdev;
	struct spdk_io_channel	*ch;
	struct io_target	*next;
};

struct bdevio_request {
	char *buf;
	int data_len;
	uint64_t offset;
	struct iovec iov[BUFFER_IOVS];
	int iovcnt;
	struct io_target *target;
};

struct io_target *g_io_targets = NULL;

static void
wake_ut_thread(void)
{
	pthread_mutex_lock(&g_test_mutex);
	pthread_cond_signal(&g_test_cond);
	pthread_mutex_unlock(&g_test_mutex);
}

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

static enum spdk_bdev_io_status g_completion_status;

static void
initialize_buffer(char **buf, int pattern, int size)
{
	*buf = spdk_zmalloc(size, 0x1000, NULL);
	memset(*buf, pattern, size);
}

static void
quick_test_complete(spdk_event_t event)
{
	struct bdevio_request *req = spdk_event_get_arg1(event);
	struct spdk_bdev_io *bdev_io = spdk_event_get_arg2(event);

	if (req->target->ch) {
		spdk_put_io_channel(req->target->ch);
		req->target->ch = NULL;
	}
	g_completion_status = bdev_io->status;
	spdk_bdev_free_io(bdev_io);
	wake_ut_thread();
}

static void
__blockdev_write(spdk_event_t event)
{
	struct bdevio_request *req = spdk_event_get_arg1(event);
	struct io_target *target = req->target;
	struct spdk_bdev_io *bdev_io;

	target->ch = spdk_bdev_get_io_channel(target->bdev, SPDK_IO_PRIORITY_DEFAULT);
	if (req->iovcnt) {
		bdev_io = spdk_bdev_writev(target->bdev, target->ch, req->iov, req->iovcnt, req->offset,
					   req->data_len, quick_test_complete, req);
	} else {
		bdev_io = spdk_bdev_write(target->bdev, target->ch, req->buf, req->offset,
					  req->data_len, quick_test_complete, req);
	}

	if (!bdev_io) {
		spdk_put_io_channel(target->ch);
		target->ch = NULL;
		g_completion_status = SPDK_BDEV_IO_STATUS_FAILED;
		wake_ut_thread();
	}
}

static void
sgl_chop_buffer(struct bdevio_request *req, int iov_len)
{
	int data_len = req->data_len;
	char *buf = req->buf;

	req->iovcnt = 0;
	if (!iov_len)
		return;

	for (; data_len > 0 && req->iovcnt < BUFFER_IOVS; req->iovcnt++) {
		if (data_len < iov_len)
			iov_len = data_len;

		req->iov[req->iovcnt].iov_base = buf;
		req->iov[req->iovcnt].iov_len = iov_len;

		buf += iov_len;
		data_len -= iov_len;
	}

	CU_ASSERT_EQUAL_FATAL(data_len, 0);
}

static void
blockdev_write(struct io_target *target, char *tx_buf,
	       uint64_t offset, int data_len, int iov_len)
{
	struct bdevio_request req;
	spdk_event_t event;

	req.target = target;
	req.buf = tx_buf;
	req.data_len = data_len;
	req.offset = offset;
	sgl_chop_buffer(&req, iov_len);

	g_completion_status = SPDK_BDEV_IO_STATUS_FAILED;

	event = spdk_event_allocate(1, __blockdev_write, &req, NULL, NULL);
	pthread_mutex_lock(&g_test_mutex);
	spdk_event_call(event);
	pthread_cond_wait(&g_test_cond, &g_test_mutex);
	pthread_mutex_unlock(&g_test_mutex);
}

static void
__blockdev_read(spdk_event_t event)
{
	struct bdevio_request *req = spdk_event_get_arg1(event);
	struct io_target *target = req->target;
	struct spdk_bdev_io *bdev_io;

	target->ch = spdk_bdev_get_io_channel(target->bdev, SPDK_IO_PRIORITY_DEFAULT);
	if (req->iovcnt) {
		bdev_io = spdk_bdev_readv(target->bdev, target->ch, req->iov, req->iovcnt, req->offset,
					  req->data_len, quick_test_complete, req);
	} else {
		bdev_io = spdk_bdev_read(target->bdev, target->ch, req->buf, req->offset,
					 req->data_len, quick_test_complete, req);
	}

	if (!bdev_io) {
		spdk_put_io_channel(target->ch);
		target->ch = NULL;
		g_completion_status = SPDK_BDEV_IO_STATUS_FAILED;
		wake_ut_thread();
	}
}

static void
blockdev_read(struct io_target *target, char *rx_buf,
	      uint64_t offset, int data_len, int iov_len)
{
	struct bdevio_request req;
	spdk_event_t event;

	req.target = target;
	req.buf = rx_buf;
	req.data_len = data_len;
	req.offset = offset;
	req.iovcnt = 0;
	sgl_chop_buffer(&req, iov_len);

	g_completion_status = SPDK_BDEV_IO_STATUS_FAILED;

	event = spdk_event_allocate(1, __blockdev_read, &req, NULL, NULL);
	pthread_mutex_lock(&g_test_mutex);
	spdk_event_call(event);
	pthread_cond_wait(&g_test_cond, &g_test_mutex);
	pthread_mutex_unlock(&g_test_mutex);
}

static int
blockdev_write_read_data_match(char *rx_buf, char *tx_buf, int data_length)
{
	int rc;
	rc = memcmp(rx_buf, tx_buf, data_length);

	spdk_free(rx_buf);
	spdk_free(tx_buf);

	return rc;
}

static void
blockdev_write_read(uint32_t data_length, uint32_t iov_len, int pattern, uint64_t offset,
		    int expected_rc)
{
	struct io_target *target;
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

		blockdev_write(target, tx_buf, offset, data_length, iov_len);

		if (expected_rc == 0) {
			CU_ASSERT_EQUAL(g_completion_status, SPDK_BDEV_IO_STATUS_SUCCESS);
		} else {
			CU_ASSERT_EQUAL(g_completion_status, SPDK_BDEV_IO_STATUS_FAILED);
		}

		blockdev_read(target, rx_buf, offset, data_length, iov_len);

		if (expected_rc == 0) {
			CU_ASSERT_EQUAL(g_completion_status, SPDK_BDEV_IO_STATUS_SUCCESS);
		} else {
			CU_ASSERT_EQUAL(g_completion_status, SPDK_BDEV_IO_STATUS_FAILED);
		}

		if (g_completion_status == SPDK_BDEV_IO_STATUS_SUCCESS) {
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
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc);
}

static void
blockdev_writev_readv_4k(void)
{
	uint32_t data_length, iov_len;
	uint64_t offset;
	int pattern;
	int expected_rc;

	/* Data size = 4K */
	data_length = 4096;
	iov_len = 4096;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = 0;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, iov_len, pattern, offset, expected_rc);
}

static void
blockdev_writev_readv_30x4k(void)
{
	uint32_t data_length, iov_len;
	uint64_t offset;
	int pattern;
	int expected_rc;

	/* Data size = 4K */
	data_length = 4096 * 30;
	iov_len = 4096;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = 0;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, iov_len, pattern, offset, expected_rc);
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
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc);
}

static void
blockdev_writev_readv_512Bytes(void)
{
	uint32_t data_length, iov_len;
	uint64_t offset;
	int pattern;
	int expected_rc;

	/* Data size = 512 */
	data_length = 512;
	iov_len = 512;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = 2048;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, iov_len, pattern, offset, expected_rc);
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
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc);
}

static void
blockdev_writev_readv_size_gt_128k(void)
{
	uint32_t data_length, iov_len;
	uint64_t offset;
	int pattern;
	int expected_rc;

	/* Data size = 132K */
	data_length = 135168;
	iov_len = 135168;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = 2048;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, iov_len, pattern, offset, expected_rc);
}

static void
blockdev_writev_readv_size_gt_128k_two_iov(void)
{
	uint32_t data_length, iov_len;
	uint64_t offset;
	int pattern;
	int expected_rc;

	/* Data size = 132K */
	data_length = 135168;
	iov_len = 128 * 1024;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = 2048;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, iov_len, pattern, offset, expected_rc);
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

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc);
}

static void
blockdev_write_read_offset_plus_nbytes_equals_bdev_size(void)
{
	struct io_target *target;
	struct spdk_bdev *bdev;
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

		blockdev_write(target, tx_buf, offset, bdev->blocklen, 0);
		CU_ASSERT_EQUAL(g_completion_status, SPDK_BDEV_IO_STATUS_SUCCESS);

		blockdev_read(target, rx_buf, offset, bdev->blocklen, 0);
		CU_ASSERT_EQUAL(g_completion_status, SPDK_BDEV_IO_STATUS_SUCCESS);

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
	char	*tx_buf = NULL;
	char	*rx_buf = NULL;
	int	data_length;
	uint64_t offset;
	int pattern;

	/* Tests the overflow condition of the blockdevs. */
	data_length = 4096;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	pattern = 0xA3;

	target = g_io_targets;
	while (target != NULL) {
		bdev = target->bdev;

		/* The start offset has been set to a valid value
		 * but offset + nbytes is greater than the Total size
		 * of the blockdev. The test should fail. */
		offset = ((bdev->blockcnt * bdev->blocklen) - 1024);

		initialize_buffer(&tx_buf, pattern, data_length);
		initialize_buffer(&rx_buf, 0, data_length);

		blockdev_write(target, tx_buf, offset, data_length, 0);
		CU_ASSERT_EQUAL(g_completion_status, SPDK_BDEV_IO_STATUS_FAILED);

		blockdev_read(target, rx_buf, offset, data_length, 0);
		CU_ASSERT_EQUAL(g_completion_status, SPDK_BDEV_IO_STATUS_FAILED);

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

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc);
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
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;
	/* Assert the write by comparing it with values read
	 * from the same offset for each blockdev */
	blockdev_write_read(data_length, 0, pattern, offset, expected_rc);

	/* Overwrite the pattern 0xbb of size 8K on an address offset overlapping
	 * with the address written above and assert the new value in
	 * the overlapped address range */
	/* Populate 8k with value 0xBB */
	pattern = 0xBB;
	/* Offset = 6144; Overlap offset addresses and write value 0xbb */
	offset = 4096;
	/* Assert the write by comparing it with values read
	 * from the overlapped offset for each blockdev */
	blockdev_write_read(data_length, 0, pattern, offset, expected_rc);
}

static void
__blockdev_reset(spdk_event_t event)
{
	struct bdevio_request *req = spdk_event_get_arg1(event);
	enum spdk_bdev_reset_type *reset_type = spdk_event_get_arg2(event);
	struct io_target *target = req->target;
	int rc;

	rc = spdk_bdev_reset(target->bdev, *reset_type, quick_test_complete, req);
	if (rc < 0) {
		spdk_put_io_channel(target->ch);
		target->ch = NULL;
		g_completion_status = SPDK_BDEV_IO_STATUS_FAILED;
		wake_ut_thread();
	}
}

static void
blockdev_reset(struct io_target *target, enum spdk_bdev_reset_type reset_type)
{
	struct bdevio_request req;
	spdk_event_t event;

	req.target = target;

	g_completion_status = SPDK_BDEV_IO_STATUS_FAILED;

	event = spdk_event_allocate(1, __blockdev_reset, &req, &reset_type, NULL);
	pthread_mutex_lock(&g_test_mutex);
	spdk_event_call(event);
	pthread_cond_wait(&g_test_cond, &g_test_mutex);
	pthread_mutex_unlock(&g_test_mutex);
}

static void
blockdev_test_reset(void)
{
	struct io_target	*target;

	target = g_io_targets;
	while (target != NULL) {
		target->bdev->gencnt = 0;
		blockdev_reset(target, SPDK_BDEV_RESET_HARD);
		CU_ASSERT_EQUAL(g_completion_status, SPDK_BDEV_IO_STATUS_SUCCESS);
		CU_ASSERT_EQUAL(target->bdev->gencnt, 1);

		target->bdev->gencnt = 0;
		blockdev_reset(target, SPDK_BDEV_RESET_SOFT);
		CU_ASSERT_EQUAL(g_completion_status, SPDK_BDEV_IO_STATUS_SUCCESS);
		CU_ASSERT_EQUAL(target->bdev->gencnt, 0);

		target = target->next;
	}
}

static void
test_main(spdk_event_t event)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	if (bdevio_construct_targets() < 0) {
		spdk_app_stop(-1);
		return;
	}

	if (CU_initialize_registry() != CUE_SUCCESS) {
		spdk_app_stop(CU_get_error());
		return;
	}

	suite = CU_add_suite("components_suite", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		spdk_app_stop(CU_get_error());
		return;
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
		|| CU_add_test(suite, "blockdev writev readv 4k", blockdev_writev_readv_4k) == NULL
		|| CU_add_test(suite, "blockdev writev readv 30 x 4k",
			       blockdev_writev_readv_30x4k) == NULL
		|| CU_add_test(suite, "blockdev writev readv 512 bytes",
			       blockdev_writev_readv_512Bytes) == NULL
		|| CU_add_test(suite, "blockdev writev readv size > 128k",
			       blockdev_writev_readv_size_gt_128k) == NULL
		|| CU_add_test(suite, "blockdev writev readv size > 128k in two iovs",
			       blockdev_writev_readv_size_gt_128k_two_iov) == NULL
		|| CU_add_test(suite, "blockdev reset",
			       blockdev_test_reset) == NULL
	) {
		CU_cleanup_registry();
		spdk_app_stop(CU_get_error());
		return;
	}

	pthread_mutex_init(&g_test_mutex, NULL);
	pthread_cond_init(&g_test_cond, NULL);
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	spdk_app_stop(num_failures);
}

int
main(int argc, char **argv)
{
	const char		*config_file;
	int			num_failures;

	if (argc == 1) {
		config_file = "/usr/local/etc/spdk/iscsi.conf";
	} else {
		config_file = argv[1];
	}
	bdevtest_init(config_file, "0x3");

	num_failures = spdk_app_start(test_main, NULL, NULL);
	spdk_app_fini();

	return num_failures;
}
