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
#include "spdk/accel_engine.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/event.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"

#include "CUnit/Basic.h"

#define BUFFER_IOVS		1024
#define BUFFER_SIZE		260 * 1024
#define BDEV_TASK_ARRAY_SIZE	2048

pthread_mutex_t g_test_mutex;
pthread_cond_t g_test_cond;

static struct spdk_thread *g_thread_init;
static struct spdk_thread *g_thread_ut;
static struct spdk_thread *g_thread_io;
static bool g_wait_for_tests = false;
static int g_num_failures = 0;
static bool g_shutdown = false;

struct io_target {
	struct spdk_bdev	*bdev;
	struct spdk_bdev_desc	*bdev_desc;
	struct spdk_io_channel	*ch;
	struct io_target	*next;
};

struct bdevio_request {
	char *buf;
	char *fused_buf;
	int data_len;
	uint64_t offset;
	struct iovec iov[BUFFER_IOVS];
	int iovcnt;
	struct iovec fused_iov[BUFFER_IOVS];
	int fused_iovcnt;
	struct io_target *target;
};

struct io_target *g_io_targets = NULL;
struct io_target *g_current_io_target = NULL;
static void rpc_perform_tests_cb(unsigned num_failures, struct spdk_jsonrpc_request *request);

static void
execute_spdk_function(spdk_msg_fn fn, void *arg)
{
	pthread_mutex_lock(&g_test_mutex);
	spdk_thread_send_msg(g_thread_io, fn, arg);
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
__get_io_channel(void *arg)
{
	struct io_target *target = arg;

	target->ch = spdk_bdev_get_io_channel(target->bdev_desc);
	assert(target->ch);
	wake_ut_thread();
}

static int
bdevio_construct_target(struct spdk_bdev *bdev)
{
	struct io_target *target;
	int rc;
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
		return rc;
	}

	printf("  %s: %" PRIu64 " blocks of %" PRIu32 " bytes (%" PRIu64 " MiB)\n",
	       spdk_bdev_get_name(bdev),
	       num_blocks, block_size,
	       (num_blocks * block_size + 1024 * 1024 - 1) / (1024 * 1024));

	target->bdev = bdev;
	target->next = g_io_targets;
	execute_spdk_function(__get_io_channel, target);
	g_io_targets = target;

	return 0;
}

static int
bdevio_construct_targets(void)
{
	struct spdk_bdev *bdev;
	int rc;

	printf("I/O targets:\n");

	bdev = spdk_bdev_first_leaf();
	while (bdev != NULL) {
		rc = bdevio_construct_target(bdev);
		if (rc < 0) {
			SPDK_ERRLOG("Could not construct bdev %s, error=%d\n", spdk_bdev_get_name(bdev), rc);
			return rc;
		}
		bdev = spdk_bdev_next_leaf(bdev);
	}

	if (g_io_targets == NULL) {
		SPDK_ERRLOG("No bdevs to perform tests on\n");
		return -1;
	}

	return 0;
}

static void
__put_io_channel(void *arg)
{
	struct io_target *target = arg;

	spdk_put_io_channel(target->ch);
	wake_ut_thread();
}

static void
bdevio_cleanup_targets(void)
{
	struct io_target *target;

	target = g_io_targets;
	while (target != NULL) {
		execute_spdk_function(__put_io_channel, target);
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
	*buf = spdk_zmalloc(size, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
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
__blockdev_write(void *arg)
{
	struct bdevio_request *req = arg;
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
__blockdev_write_zeroes(void *arg)
{
	struct bdevio_request *req = arg;
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
__blockdev_compare_and_write(void *arg)
{
	struct bdevio_request *req = arg;
	struct io_target *target = req->target;
	int rc;

	rc = spdk_bdev_comparev_and_writev_blocks(target->bdev_desc, target->ch, req->iov, req->iovcnt,
			req->fused_iov, req->fused_iovcnt, req->offset, req->data_len, quick_test_complete, NULL);

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
sgl_chop_fused_buffer(struct bdevio_request *req, int iov_len)
{
	int data_len = req->data_len;
	char *buf = req->fused_buf;

	req->fused_iovcnt = 0;
	if (!iov_len) {
		return;
	}

	for (; data_len > 0 && req->fused_iovcnt < BUFFER_IOVS; req->fused_iovcnt++) {
		if (data_len < iov_len) {
			iov_len = data_len;
		}

		req->fused_iov[req->fused_iovcnt].iov_base = buf;
		req->fused_iov[req->fused_iovcnt].iov_len = iov_len;

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

	execute_spdk_function(__blockdev_write, &req);
}

static void
_blockdev_compare_and_write(struct io_target *target, char *cmp_buf, char *write_buf,
			    uint64_t offset, int data_len, int iov_len)
{
	struct bdevio_request req;

	req.target = target;
	req.buf = cmp_buf;
	req.fused_buf = write_buf;
	req.data_len = data_len;
	req.offset = offset;
	sgl_chop_buffer(&req, iov_len);
	sgl_chop_fused_buffer(&req, iov_len);

	g_completion_success = false;

	execute_spdk_function(__blockdev_compare_and_write, &req);
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

	execute_spdk_function(__blockdev_write_zeroes, &req);
}

static void
__blockdev_read(void *arg)
{
	struct bdevio_request *req = arg;
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

	execute_spdk_function(__blockdev_read, &req);
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

static bool
blockdev_io_valid_blocks(struct spdk_bdev *bdev, uint64_t data_length)
{
	if (data_length < spdk_bdev_get_block_size(bdev) ||
	    data_length % spdk_bdev_get_block_size(bdev) ||
	    data_length / spdk_bdev_get_block_size(bdev) > spdk_bdev_get_num_blocks(bdev)) {
		return false;
	}

	return true;
}

static void
blockdev_write_read(uint32_t data_length, uint32_t iov_len, int pattern, uint64_t offset,
		    int expected_rc, bool write_zeroes)
{
	struct io_target *target;
	char	*tx_buf = NULL;
	char	*rx_buf = NULL;
	int	rc;

	target = g_current_io_target;

	if (!blockdev_io_valid_blocks(target->bdev, data_length)) {
		return;
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
}

static void
blockdev_compare_and_write(uint32_t data_length, uint32_t iov_len, uint64_t offset)
{
	struct io_target *target;
	char	*tx_buf = NULL;
	char	*write_buf = NULL;
	char	*rx_buf = NULL;
	int	rc;

	target = g_current_io_target;

	if (!blockdev_io_valid_blocks(target->bdev, data_length)) {
		return;
	}

	initialize_buffer(&tx_buf, 0xAA, data_length);
	initialize_buffer(&rx_buf, 0, data_length);
	initialize_buffer(&write_buf, 0xBB, data_length);

	blockdev_write(target, tx_buf, offset, data_length, iov_len);
	CU_ASSERT_EQUAL(g_completion_success, true);

	_blockdev_compare_and_write(target, tx_buf, write_buf, offset, data_length, iov_len);
	CU_ASSERT_EQUAL(g_completion_success, true);

	_blockdev_compare_and_write(target, tx_buf, write_buf, offset, data_length, iov_len);
	CU_ASSERT_EQUAL(g_completion_success, false);

	blockdev_read(target, rx_buf, offset, data_length, iov_len);
	CU_ASSERT_EQUAL(g_completion_success, true);
	rc = blockdev_write_read_data_match(rx_buf, write_buf, data_length);
	/* Assert the write by comparing it with values read
	 * from each blockdev */
	CU_ASSERT_EQUAL(rc, 0);
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
blockdev_comparev_and_writev(void)
{
	uint32_t data_length, iov_len;
	uint64_t offset;

	data_length = 1;
	iov_len = 1;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = 0;

	blockdev_compare_and_write(data_length, iov_len, offset);
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

	target = g_current_io_target;
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

	target = g_current_io_target;
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
__blockdev_reset(void *arg)
{
	struct bdevio_request *req = arg;
	struct io_target *target = req->target;
	int rc;

	rc = spdk_bdev_reset(target->bdev_desc, target->ch, quick_test_complete, NULL);
	if (rc < 0) {
		g_completion_success = false;
		wake_ut_thread();
	}
}

static void
blockdev_test_reset(void)
{
	struct bdevio_request req;
	struct io_target *target;

	target = g_current_io_target;
	req.target = target;

	g_completion_success = false;

	execute_spdk_function(__blockdev_reset, &req);

	/* Workaround: NVMe-oF target doesn't support reset yet - so for now
	 *  don't fail the test if it's an NVMe bdev.
	 */
	if (!spdk_bdev_io_type_supported(target->bdev, SPDK_BDEV_IO_TYPE_NVME_IO)) {
		CU_ASSERT_EQUAL(g_completion_success, true);
	}
}

struct bdevio_passthrough_request {
	struct spdk_nvme_cmd cmd;
	void *buf;
	uint32_t len;
	struct io_target *target;
	int sct;
	int sc;
	uint32_t cdw0;
};

static void
nvme_pt_test_complete(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct bdevio_passthrough_request *pt_req = arg;

	spdk_bdev_io_get_nvme_status(bdev_io, &pt_req->cdw0, &pt_req->sct, &pt_req->sc);
	spdk_bdev_free_io(bdev_io);
	wake_ut_thread();
}

static void
__blockdev_nvme_passthru(void *arg)
{
	struct bdevio_passthrough_request *pt_req = arg;
	struct io_target *target = pt_req->target;
	int rc;

	rc = spdk_bdev_nvme_io_passthru(target->bdev_desc, target->ch,
					&pt_req->cmd, pt_req->buf, pt_req->len,
					nvme_pt_test_complete, pt_req);
	if (rc) {
		wake_ut_thread();
	}
}

static void
blockdev_test_nvme_passthru_rw(void)
{
	struct bdevio_passthrough_request pt_req;
	void *write_buf, *read_buf;
	struct io_target *target;

	target = g_current_io_target;

	if (!spdk_bdev_io_type_supported(target->bdev, SPDK_BDEV_IO_TYPE_NVME_IO)) {
		return;
	}

	memset(&pt_req, 0, sizeof(pt_req));
	pt_req.target = target;
	pt_req.cmd.opc = SPDK_NVME_OPC_WRITE;
	pt_req.cmd.nsid = 1;
	*(uint64_t *)&pt_req.cmd.cdw10 = 4;
	pt_req.cmd.cdw12 = 0;

	pt_req.len = spdk_bdev_get_block_size(target->bdev);
	write_buf = spdk_malloc(pt_req.len, 0, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	memset(write_buf, 0xA5, pt_req.len);
	pt_req.buf = write_buf;

	pt_req.sct = SPDK_NVME_SCT_VENDOR_SPECIFIC;
	pt_req.sc = SPDK_NVME_SC_INVALID_FIELD;
	execute_spdk_function(__blockdev_nvme_passthru, &pt_req);
	CU_ASSERT(pt_req.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(pt_req.sc == SPDK_NVME_SC_SUCCESS);

	pt_req.cmd.opc = SPDK_NVME_OPC_READ;
	read_buf = spdk_zmalloc(pt_req.len, 0, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	pt_req.buf = read_buf;

	pt_req.sct = SPDK_NVME_SCT_VENDOR_SPECIFIC;
	pt_req.sc = SPDK_NVME_SC_INVALID_FIELD;
	execute_spdk_function(__blockdev_nvme_passthru, &pt_req);
	CU_ASSERT(pt_req.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(pt_req.sc == SPDK_NVME_SC_SUCCESS);

	CU_ASSERT(!memcmp(read_buf, write_buf, pt_req.len));
	spdk_free(read_buf);
	spdk_free(write_buf);
}

static void
blockdev_test_nvme_passthru_vendor_specific(void)
{
	struct bdevio_passthrough_request pt_req;
	struct io_target *target;

	target = g_current_io_target;

	if (!spdk_bdev_io_type_supported(target->bdev, SPDK_BDEV_IO_TYPE_NVME_IO)) {
		return;
	}

	memset(&pt_req, 0, sizeof(pt_req));
	pt_req.target = target;
	pt_req.cmd.opc = 0x7F; /* choose known invalid opcode */
	pt_req.cmd.nsid = 1;

	pt_req.sct = SPDK_NVME_SCT_VENDOR_SPECIFIC;
	pt_req.sc = SPDK_NVME_SC_SUCCESS;
	pt_req.cdw0 = 0xbeef;
	execute_spdk_function(__blockdev_nvme_passthru, &pt_req);
	CU_ASSERT(pt_req.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(pt_req.sc == SPDK_NVME_SC_INVALID_OPCODE);
	CU_ASSERT(pt_req.cdw0 == 0x0);
}

static void
__blockdev_nvme_admin_passthru(void *arg)
{
	struct bdevio_passthrough_request *pt_req = arg;
	struct io_target *target = pt_req->target;
	int rc;

	rc = spdk_bdev_nvme_admin_passthru(target->bdev_desc, target->ch,
					   &pt_req->cmd, pt_req->buf, pt_req->len,
					   nvme_pt_test_complete, pt_req);
	if (rc) {
		wake_ut_thread();
	}
}

static void
blockdev_test_nvme_admin_passthru(void)
{
	struct io_target *target;
	struct bdevio_passthrough_request pt_req;

	target = g_current_io_target;

	if (!spdk_bdev_io_type_supported(target->bdev, SPDK_BDEV_IO_TYPE_NVME_ADMIN)) {
		return;
	}

	memset(&pt_req, 0, sizeof(pt_req));
	pt_req.target = target;
	pt_req.cmd.opc = SPDK_NVME_OPC_IDENTIFY;
	pt_req.cmd.nsid = 0;
	*(uint64_t *)&pt_req.cmd.cdw10 = SPDK_NVME_IDENTIFY_CTRLR;

	pt_req.len = sizeof(struct spdk_nvme_ctrlr_data);
	pt_req.buf = spdk_malloc(pt_req.len, 0, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

	pt_req.sct = SPDK_NVME_SCT_GENERIC;
	pt_req.sc = SPDK_NVME_SC_SUCCESS;
	execute_spdk_function(__blockdev_nvme_admin_passthru, &pt_req);
	CU_ASSERT(pt_req.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(pt_req.sc == SPDK_NVME_SC_SUCCESS);
}

static void
__stop_init_thread(void *arg)
{
	unsigned num_failures = g_num_failures;
	struct spdk_jsonrpc_request *request = arg;

	g_num_failures = 0;

	bdevio_cleanup_targets();
	if (g_wait_for_tests && !g_shutdown) {
		/* Do not stop the app yet, wait for another RPC */
		rpc_perform_tests_cb(num_failures, request);
		return;
	}
	spdk_app_stop(num_failures);
}

static void
stop_init_thread(unsigned num_failures, struct spdk_jsonrpc_request *request)
{
	g_num_failures = num_failures;

	spdk_thread_send_msg(g_thread_init, __stop_init_thread, request);
}

static int
suite_init(void)
{
	if (g_current_io_target == NULL) {
		g_current_io_target = g_io_targets;
	}
	return 0;
}

static int
suite_fini(void)
{
	g_current_io_target = g_current_io_target->next;
	return 0;
}

#define SUITE_NAME_MAX 64

static int
__setup_ut_on_single_target(struct io_target *target)
{
	unsigned rc = 0;
	CU_pSuite suite = NULL;
	char name[SUITE_NAME_MAX];

	snprintf(name, sizeof(name), "bdevio tests on: %s", spdk_bdev_get_name(target->bdev));
	suite = CU_add_suite(name, suite_init, suite_fini);
	if (suite == NULL) {
		CU_cleanup_registry();
		rc = CU_get_error();
		return -rc;
	}

	if (
		CU_add_test(suite, "blockdev write read 4k", blockdev_write_read_4k) == NULL
		|| CU_add_test(suite, "blockdev write zeroes read 4k", blockdev_write_zeroes_read_4k) == NULL
		|| CU_add_test(suite, "blockdev write zeroes read 1m", blockdev_write_zeroes_read_1m) == NULL
		|| CU_add_test(suite, "blockdev write zeroes read 3m", blockdev_write_zeroes_read_3m) == NULL
		|| CU_add_test(suite, "blockdev write zeroes read 3.5m", blockdev_write_zeroes_read_3m_500k) == NULL
		|| CU_add_test(suite, "blockdev reset",
			       blockdev_test_reset) == NULL
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
		|| CU_add_test(suite, "blockdev comparev and writev", blockdev_comparev_and_writev) == NULL
		|| CU_add_test(suite, "blockdev nvme passthru rw",
			       blockdev_test_nvme_passthru_rw) == NULL
		|| CU_add_test(suite, "blockdev nvme passthru vendor specific",
			       blockdev_test_nvme_passthru_vendor_specific) == NULL
		|| CU_add_test(suite, "blockdev nvme admin passthru",
			       blockdev_test_nvme_admin_passthru) == NULL
	) {
		CU_cleanup_registry();
		rc = CU_get_error();
		return -rc;
	}
	return 0;
}

static void
__run_ut_thread(void *arg)
{
	struct spdk_jsonrpc_request *request = arg;
	int rc = 0;
	struct io_target *target;
	unsigned num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		/* CUnit error, probably won't recover */
		rc = CU_get_error();
		stop_init_thread(-rc, request);
	}

	target = g_io_targets;
	while (target != NULL) {
		rc = __setup_ut_on_single_target(target);
		if (rc < 0) {
			/* CUnit error, probably won't recover */
			stop_init_thread(-rc, request);
		}
		target = target->next;
	}
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	stop_init_thread(num_failures, request);
}

static void
__construct_targets(void *arg)
{
	if (bdevio_construct_targets() < 0) {
		spdk_app_stop(-1);
		return;
	}

	spdk_thread_send_msg(g_thread_ut, __run_ut_thread, NULL);
}

static void
test_main(void *arg1)
{
	struct spdk_cpuset tmpmask = {};
	const struct spdk_cpuset *appmask;
	uint32_t cpu, init_cpu;

	pthread_mutex_init(&g_test_mutex, NULL);
	pthread_cond_init(&g_test_cond, NULL);

	appmask = spdk_app_get_core_mask();

	if (spdk_cpuset_count(appmask) < 3) {
		spdk_app_stop(-1);
		return;
	}

	init_cpu = spdk_env_get_current_core();
	g_thread_init = spdk_get_thread();

	for (cpu = 0; cpu < SPDK_ENV_LCORE_ID_ANY; cpu++) {
		if (cpu != init_cpu && spdk_cpuset_get_cpu(appmask, cpu)) {
			spdk_cpuset_zero(&tmpmask);
			spdk_cpuset_set_cpu(&tmpmask, cpu, true);
			g_thread_ut = spdk_thread_create("ut_thread", &tmpmask);
			break;
		}
	}

	if (cpu == SPDK_ENV_LCORE_ID_ANY) {
		spdk_app_stop(-1);
		return;
	}

	for (cpu++; cpu < SPDK_ENV_LCORE_ID_ANY; cpu++) {
		if (cpu != init_cpu && spdk_cpuset_get_cpu(appmask, cpu)) {
			spdk_cpuset_zero(&tmpmask);
			spdk_cpuset_set_cpu(&tmpmask, cpu, true);
			g_thread_io = spdk_thread_create("io_thread", &tmpmask);
			break;
		}
	}

	if (cpu == SPDK_ENV_LCORE_ID_ANY) {
		spdk_app_stop(-1);
		return;
	}

	if (g_wait_for_tests) {
		/* Do not perform any tests until RPC is received */
		return;
	}

	spdk_thread_send_msg(g_thread_init, __construct_targets, NULL);
}

static void
bdevio_usage(void)
{
	printf(" -w                        start bdevio app and wait for RPC to start the tests\n");
}

static int
bdevio_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 'w':
		g_wait_for_tests =  true;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

struct rpc_perform_tests {
	char *name;
};

static void
free_rpc_perform_tests(struct rpc_perform_tests *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_perform_tests_decoders[] = {
	{"name", offsetof(struct rpc_perform_tests, name), spdk_json_decode_string, true},
};

static void
rpc_perform_tests_cb(unsigned num_failures, struct spdk_jsonrpc_request *request)
{
	struct spdk_json_write_ctx *w;

	if (num_failures == 0) {
		w = spdk_jsonrpc_begin_result(request);
		spdk_json_write_uint32(w, num_failures);
		spdk_jsonrpc_end_result(request, w);
	} else {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "%d test cases failed", num_failures);
	}
}

static void
rpc_perform_tests(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_perform_tests req = {NULL};
	struct spdk_bdev *bdev;
	int rc;

	if (params && spdk_json_decode_object(params, rpc_perform_tests_decoders,
					      SPDK_COUNTOF(rpc_perform_tests_decoders),
					      &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto invalid;
	}

	if (req.name) {
		bdev = spdk_bdev_get_by_name(req.name);
		if (bdev == NULL) {
			SPDK_ERRLOG("Bdev '%s' does not exist\n", req.name);
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							     "Bdev '%s' does not exist: %s",
							     req.name, spdk_strerror(ENODEV));
			goto invalid;
		}
		rc = bdevio_construct_target(bdev);
		if (rc < 0) {
			SPDK_ERRLOG("Could not construct target for bdev '%s'\n", spdk_bdev_get_name(bdev));
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							     "Could not construct target for bdev '%s': %s",
							     spdk_bdev_get_name(bdev), spdk_strerror(-rc));
			goto invalid;
		}
	} else {
		rc = bdevio_construct_targets();
		if (rc < 0) {
			SPDK_ERRLOG("Could not construct targets for all bdevs\n");
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							     "Could not construct targets for all bdevs: %s",
							     spdk_strerror(-rc));
			goto invalid;
		}
	}
	free_rpc_perform_tests(&req);

	spdk_thread_send_msg(g_thread_ut, __run_ut_thread, request);

	return;

invalid:
	free_rpc_perform_tests(&req);
}
SPDK_RPC_REGISTER("perform_tests", rpc_perform_tests, SPDK_RPC_RUNTIME)

static void
spdk_bdevio_shutdown_cb(void)
{
	g_shutdown = true;
	spdk_thread_send_msg(g_thread_init, __stop_init_thread, NULL);
}

int
main(int argc, char **argv)
{
	int			rc;
	struct spdk_app_opts	opts = {};

	spdk_app_opts_init(&opts);
	opts.name = "bdevio";
	opts.reactor_mask = "0x7";
	opts.shutdown_cb = spdk_bdevio_shutdown_cb;

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "w", NULL,
				      bdevio_parse_arg, bdevio_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		return rc;
	}

	rc = spdk_app_start(&opts, test_main, NULL);
	spdk_app_fini();

	return rc;
}
