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

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/copy_engine.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/event.h"

#include "CUnit/Basic.h"

#define BUFFER_IOVS		1024
#define BUFFER_SIZE		260 * 1024
#define BDEV_TASK_ARRAY_SIZE	2048

pthread_mutex_t g_test_mutex;
pthread_cond_t g_test_cond;

static uint32_t g_lcore_id_init;
static uint32_t g_lcore_id_ut;
static uint32_t g_lcore_id_io;

struct io_target {
	struct spdk_bdev	*bdev;
	struct spdk_bdev_desc	*bdev_desc;
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
execute_spdk_function(spdk_event_fn fn, void *arg1, void *arg2)
{
	struct spdk_event *event;

	event = spdk_event_allocate(g_lcore_id_io, fn, arg1, arg2);
	pthread_mutex_lock(&g_test_mutex);
	spdk_event_call(event);
	pthread_cond_wait(&g_test_cond, &g_test_mutex);
	pthread_mutex_unlock(&g_test_mutex);
}

static void
wake_ut_thread(void)
{
	pthread_mutex_lock(&g_test_mutex);
	pthread_cond_signal(&g_test_cond);
	pthread_mutex_unlock(&g_test_mutex);
}

static void
__get_io_channel(void *arg1, void *arg2)
{
	struct io_target *target = arg1;

	target->ch = spdk_bdev_get_io_channel(target->bdev_desc);
	assert(target->ch);
	wake_ut_thread();
}

static int
bdevio_construct_targets(void)
{
	struct spdk_bdev *bdev;
	struct io_target *target;
	int rc;

	printf("I/O targets:\n");

	bdev = spdk_bdev_first_leaf();
	while (bdev != NULL) {
		uint64_t num_blocks = spdk_bdev_get_num_blocks(bdev);
		uint32_t block_size = spdk_bdev_get_block_size(bdev);

		target = malloc(sizeof(struct io_target));
		if (target == NULL) {
			return -ENOMEM;
		}

		rc = spdk_bdev_open(bdev, true, NULL, NULL, &target->bdev_desc);
		if (rc != 0) {
			free(target);
			SPDK_ERRLOG("Could not open leaf bdev %s, error=%d\n", spdk_bdev_get_name(bdev), rc);
			bdev = spdk_bdev_next_leaf(bdev);
			continue;
		}

		printf("  %s: %" PRIu64 " blocks of %" PRIu32 " bytes (%" PRIu64 " MiB)\n",
		       spdk_bdev_get_name(bdev),
		       num_blocks, block_size,
		       (num_blocks * block_size + 1024 * 1024 - 1) / (1024 * 1024));

		target->bdev = bdev;
		target->next = g_io_targets;
		execute_spdk_function(__get_io_channel, target, NULL);
		g_io_targets = target;

		bdev = spdk_bdev_next_leaf(bdev);
	}

	return 0;
}

static void
__put_io_channel(void *arg1, void *arg2)
{
	struct io_target *target = arg1;

	spdk_put_io_channel(target->ch);
	wake_ut_thread();
}

static void
bdevio_cleanup_targets(void)
{
	struct io_target *target;

	target = g_io_targets;
	while (target != NULL) {
		execute_spdk_function(__put_io_channel, target, NULL);
		spdk_bdev_close(target->bdev_desc);
		g_io_targets = target->next;
		free(target);
		target = g_io_targets;
	}
}

static bool g_completion_success;

static void
initialize_buffer(char **buf, int pattern, int size)
{
	*buf = spdk_dma_zmalloc(size, 0x1000, NULL);
	memset(*buf, pattern, size);
}

static void
quick_test_complete(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	g_completion_success = success;
	spdk_bdev_free_io(bdev_io);
	wake_ut_thread();
}

static void
__blockdev_write(void *arg1, void *arg2)
{
	struct bdevio_request *req = arg1;
	struct io_target *target = req->target;
	int rc;

	if (req->iovcnt) {
		rc = spdk_bdev_writev(target->bdev_desc, target->ch, req->iov, req->iovcnt, req->offset,
				      req->data_len, quick_test_complete, NULL);
	} else {
		rc = spdk_bdev_write(target->bdev_desc, target->ch, req->buf, req->offset,
				     req->data_len, quick_test_complete, NULL);
	}

	if (rc) {
		g_completion_success = false;
		wake_ut_thread();
	}
}

static void
__blockdev_write_zeroes(void *arg1, void *arg2)
{
	struct bdevio_request *req = arg1;
	struct io_target *target = req->target;
	int rc;

	rc = spdk_bdev_write_zeroes(target->bdev_desc, target->ch, req->offset,
				    req->data_len, quick_test_complete, NULL);
	if (rc) {
		g_completion_success = false;
		wake_ut_thread();
	}
}

static void
sgl_chop_buffer(struct bdevio_request *req, int iov_len)
{
	int data_len = req->data_len;
	char *buf = req->buf;

	req->iovcnt = 0;
	if (!iov_len) {
		return;
	}

	for (; data_len > 0 && req->iovcnt < BUFFER_IOVS; req->iovcnt++) {
		if (data_len < iov_len) {
			iov_len = data_len;
		}

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

	req.target = target;
	req.buf = tx_buf;
	req.data_len = data_len;
	req.offset = offset;
	sgl_chop_buffer(&req, iov_len);

	g_completion_success = false;

	execute_spdk_function(__blockdev_write, &req, NULL);
}

static void
blockdev_write_zeroes(struct io_target *target, char *tx_buf,
		      uint64_t offset, int data_len)
{
	struct bdevio_request req;

	req.target = target;
	req.buf = tx_buf;
	req.data_len = data_len;
	req.offset = offset;

	g_completion_success = false;

	execute_spdk_function(__blockdev_write_zeroes, &req, NULL);
}

static void
__blockdev_read(void *arg1, void *arg2)
{
	struct bdevio_request *req = arg1;
	struct io_target *target = req->target;
	int rc;

	if (req->iovcnt) {
		rc = spdk_bdev_readv(target->bdev_desc, target->ch, req->iov, req->iovcnt, req->offset,
				     req->data_len, quick_test_complete, NULL);
	} else {
		rc = spdk_bdev_read(target->bdev_desc, target->ch, req->buf, req->offset,
				    req->data_len, quick_test_complete, NULL);
	}

	if (rc) {
		g_completion_success = false;
		wake_ut_thread();
	}
}

static void
blockdev_read(struct io_target *target, char *rx_buf,
	      uint64_t offset, int data_len, int iov_len)
{
	struct bdevio_request req;

	req.target = target;
	req.buf = rx_buf;
	req.data_len = data_len;
	req.offset = offset;
	req.iovcnt = 0;
	sgl_chop_buffer(&req, iov_len);

	g_completion_success = false;

	execute_spdk_function(__blockdev_read, &req, NULL);
}

static int
blockdev_write_read_data_match(char *rx_buf, char *tx_buf, int data_length)
{
	int rc;
	rc = memcmp(rx_buf, tx_buf, data_length);

	spdk_dma_free(rx_buf);
	spdk_dma_free(tx_buf);

	return rc;
}

static void
blockdev_write_read(uint32_t data_length, uint32_t iov_len, int pattern, uint64_t offset,
		    int expected_rc, bool write_zeroes)
{
	struct io_target *target;
	char	*tx_buf = NULL;
	char	*rx_buf = NULL;
	int	rc;

	target = g_io_targets;
	while (target != NULL) {
		if (data_length < spdk_bdev_get_block_size(target->bdev) ||
		    data_length / spdk_bdev_get_block_size(target->bdev) > spdk_bdev_get_num_blocks(target->bdev)) {
			target = target->next;
			continue;
		}

		if (!write_zeroes) {
			initialize_buffer(&tx_buf, pattern, data_length);
			initialize_buffer(&rx_buf, 0, data_length);

			blockdev_write(target, tx_buf, offset, data_length, iov_len);
		} else {
			initialize_buffer(&tx_buf, 0, data_length);
			initialize_buffer(&rx_buf, pattern, data_length);

			blockdev_write_zeroes(target, tx_buf, offset, data_length);
		}


		if (expected_rc == 0) {
			CU_ASSERT_EQUAL(g_completion_success, true);
		} else {
			CU_ASSERT_EQUAL(g_completion_success, false);
		}
		blockdev_read(target, rx_buf, offset, data_length, iov_len);

		if (expected_rc == 0) {
			CU_ASSERT_EQUAL(g_completion_success, true);
		} else {
			CU_ASSERT_EQUAL(g_completion_success, false);
		}

		if (g_completion_success) {
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

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 0);
}

static void
blockdev_write_zeroes_read_4k(void)
{
	uint32_t data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;

	/* Data size = 4K */
	data_length = 4096;
	offset = 0;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write_zeroes and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 1);
}

/*
 * This i/o will not have to split at the bdev layer.
 */
static void
blockdev_write_zeroes_read_1m(void)
{
	uint32_t data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;

	/* Data size = 1M */
	data_length = 1048576;
	offset = 0;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write_zeroes and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 1);
}

/*
 * This i/o will have to split at the bdev layer if
 * write-zeroes is not supported by the bdev.
 */
static void
blockdev_write_zeroes_read_3m(void)
{
	uint32_t data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;

	/* Data size = 3M */
	data_length = 3145728;
	offset = 0;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write_zeroes and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 1);
}

/*
 * This i/o will have to split at the bdev layer if
 * write-zeroes is not supported by the bdev. It also
 * tests a write size that is not an even multiple of
 * the bdev layer zero buffer size.
 */
static void
blockdev_write_zeroes_read_3m_500k(void)
{
	uint32_t data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;

	/* Data size = 3.5M */
	data_length = 3670016;
	offset = 0;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write_zeroes and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 1);
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

	blockdev_write_read(data_length, iov_len, pattern, offset, expected_rc, 0);
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

	blockdev_write_read(data_length, iov_len, pattern, offset, expected_rc, 0);
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
	offset = 8192;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 0);
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
	offset = 8192;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, iov_len, pattern, offset, expected_rc, 0);
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
	offset = 8192;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 0);
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
	offset = 8192;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, iov_len, pattern, offset, expected_rc, 0);
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
	offset = 8192;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, iov_len, pattern, offset, expected_rc, 0);
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
	offset = 8192;
	pattern = 0xA3;
	/* Params are invalid, hence the expected return value
	 * of write and read for all blockdevs is < 0 */
	expected_rc = -1;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 0);
}

static void
blockdev_write_read_offset_plus_nbytes_equals_bdev_size(void)
{
	struct io_target *target;
	struct spdk_bdev *bdev;
	char	*tx_buf = NULL;
	char	*rx_buf = NULL;
	uint64_t offset;
	uint32_t block_size;
	int rc;

	target = g_io_targets;
	while (target != NULL) {
		bdev = target->bdev;

		block_size = spdk_bdev_get_block_size(bdev);

		/* The start offset has been set to a marginal value
		 * such that offset + nbytes == Total size of
		 * blockdev. */
		offset = ((spdk_bdev_get_num_blocks(bdev) - 1) * block_size);

		initialize_buffer(&tx_buf, 0xA3, block_size);
		initialize_buffer(&rx_buf, 0, block_size);

		blockdev_write(target, tx_buf, offset, block_size, 0);
		CU_ASSERT_EQUAL(g_completion_success, true);

		blockdev_read(target, rx_buf, offset, block_size, 0);
		CU_ASSERT_EQUAL(g_completion_success, true);

		rc = blockdev_write_read_data_match(rx_buf, tx_buf, block_size);
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
		offset = ((spdk_bdev_get_num_blocks(bdev) * spdk_bdev_get_block_size(bdev)) - 1024);

		initialize_buffer(&tx_buf, pattern, data_length);
		initialize_buffer(&rx_buf, 0, data_length);

		blockdev_write(target, tx_buf, offset, data_length, 0);
		CU_ASSERT_EQUAL(g_completion_success, false);

		blockdev_read(target, rx_buf, offset, data_length, 0);
		CU_ASSERT_EQUAL(g_completion_success, false);

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

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 0);
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
	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 0);

	/* Overwrite the pattern 0xbb of size 8K on an address offset overlapping
	 * with the address written above and assert the new value in
	 * the overlapped address range */
	/* Populate 8k with value 0xBB */
	pattern = 0xBB;
	/* Offset = 6144; Overlap offset addresses and write value 0xbb */
	offset = 4096;
	/* Assert the write by comparing it with values read
	 * from the overlapped offset for each blockdev */
	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 0);
}

static void
__blockdev_reset(void *arg1, void *arg2)
{
	struct bdevio_request *req = arg1;
	struct io_target *target = req->target;
	int rc;

	rc = spdk_bdev_reset(target->bdev_desc, target->ch, quick_test_complete, NULL);
	if (rc < 0) {
		g_completion_success = false;
		wake_ut_thread();
	}
}

static void
blockdev_reset(struct io_target *target)
{
	struct bdevio_request req;

	req.target = target;

	g_completion_success = false;

	execute_spdk_function(__blockdev_reset, &req, NULL);
}

static void
blockdev_test_reset(void)
{
	struct io_target	*target;

	target = g_io_targets;
	while (target != NULL) {
		blockdev_reset(target);
		CU_ASSERT_EQUAL(g_completion_success, true);

		target = target->next;
	}
}

static void
__stop_init_thread(void *arg1, void *arg2)
{
	unsigned num_failures = (unsigned)(uintptr_t)arg1;

	bdevio_cleanup_targets();
	spdk_app_stop(num_failures);
}

static void
stop_init_thread(unsigned num_failures)
{
	struct spdk_event *event;

	event = spdk_event_allocate(g_lcore_id_init, __stop_init_thread,
				    (void *)(uintptr_t)num_failures, NULL);
	spdk_event_call(event);
}

static void
__run_ut_thread(void *arg1, void *arg2)
{
	CU_pSuite suite = NULL;
	unsigned num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		stop_init_thread(CU_get_error());
		return;
	}

	suite = CU_add_suite("components_suite", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		stop_init_thread(CU_get_error());
		return;
	}

	if (
		CU_add_test(suite, "blockdev write read 4k", blockdev_write_read_4k) == NULL
		|| CU_add_test(suite, "blockdev write zeroes read 4k", blockdev_write_zeroes_read_4k) == NULL
		|| CU_add_test(suite, "blockdev write zeroes read 1m", blockdev_write_zeroes_read_1m) == NULL
		|| CU_add_test(suite, "blockdev write zeroes read 3m", blockdev_write_zeroes_read_3m) == NULL
		|| CU_add_test(suite, "blockdev write zeroes read 3.5m", blockdev_write_zeroes_read_3m_500k) == NULL
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
		stop_init_thread(CU_get_error());
		return;
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	stop_init_thread(num_failures);
}

static void
test_main(void *arg1, void *arg2)
{
	struct spdk_event *event;

	pthread_mutex_init(&g_test_mutex, NULL);
	pthread_cond_init(&g_test_cond, NULL);

	g_lcore_id_init = spdk_env_get_first_core();
	g_lcore_id_ut = spdk_env_get_next_core(g_lcore_id_init);
	g_lcore_id_io = spdk_env_get_next_core(g_lcore_id_ut);

	if (g_lcore_id_init == SPDK_ENV_LCORE_ID_ANY ||
	    g_lcore_id_ut == SPDK_ENV_LCORE_ID_ANY ||
	    g_lcore_id_io == SPDK_ENV_LCORE_ID_ANY) {
		SPDK_ERRLOG("Could not reserve 3 separate threads.\n");
		spdk_app_stop(-1);
	}

	if (bdevio_construct_targets() < 0) {
		spdk_app_stop(-1);
		return;
	}

	event = spdk_event_allocate(g_lcore_id_ut, __run_ut_thread, NULL, NULL);
	spdk_event_call(event);
}

static void
bdevio_usage(void)
{
}

static void
bdevio_parse_arg(int ch, char *arg)
{
}

int
main(int argc, char **argv)
{
	int			rc;
	struct spdk_app_opts	opts = {};

	spdk_app_opts_init(&opts);
	opts.name = "bdevtest";
	opts.rpc_addr = NULL;
	opts.reactor_mask = "0x7";

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "", NULL,
				      bdevio_parse_arg, bdevio_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		return rc;
	}

	rc = spdk_app_start(&opts, test_main, NULL, NULL);
	spdk_app_fini();

	return rc;
}
