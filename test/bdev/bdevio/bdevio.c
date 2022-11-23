/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/accel.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/event.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"

#include "bdev_internal.h"
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
	uint64_t src_offset;
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
__exit_io_thread(void *arg)
{
	assert(spdk_get_thread() == g_thread_io);
	spdk_thread_exit(g_thread_io);
	wake_ut_thread();
}

static void
__get_io_channel(void *arg)
{
	struct io_target *target = arg;

	target->ch = spdk_bdev_get_io_channel(target->bdev_desc);
	assert(target->ch);
	wake_ut_thread();
}

static void
bdevio_construct_target_open_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
				void *event_ctx)
{
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

	rc = spdk_bdev_open_ext(spdk_bdev_get_name(bdev), true, bdevio_construct_target_open_cb, NULL,
				&target->bdev_desc);
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
initialize_buffer(char **buf, int pattern, int size, uint32_t block_size)
{
	CU_ASSERT(block_size != 0);

	*buf = spdk_zmalloc(size, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	memset(*buf, pattern, size);

	if (pattern) {
		for (int offset = 0, block = 0; offset < size; offset += block_size, block++) {
			*(*buf + offset) = block;
		}
	}
}

static void
quick_test_complete(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	g_completion_success = success;
	spdk_bdev_free_io(bdev_io);
	wake_ut_thread();
}

static uint64_t
bdev_bytes_to_blocks(struct spdk_bdev *bdev, uint64_t bytes)
{
	uint32_t block_size = spdk_bdev_get_block_size(bdev);

	CU_ASSERT(bytes % block_size == 0);
	return bytes / block_size;
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
	struct spdk_bdev *bdev = target->bdev;
	int rc;

	rc = spdk_bdev_comparev_and_writev_blocks(target->bdev_desc, target->ch, req->iov, req->iovcnt,
			req->fused_iov, req->fused_iovcnt, bdev_bytes_to_blocks(bdev, req->offset),
			bdev_bytes_to_blocks(bdev, req->data_len), quick_test_complete, NULL);

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

static void
_blockdev_copy(void *arg)
{
	struct bdevio_request *req = arg;
	struct io_target *target = req->target;
	struct spdk_bdev *bdev = target->bdev;
	int rc;

	rc = spdk_bdev_copy_blocks(target->bdev_desc, target->ch,
				   bdev_bytes_to_blocks(bdev, req->offset),
				   bdev_bytes_to_blocks(bdev, req->src_offset),
				   bdev_bytes_to_blocks(bdev, req->data_len),
				   quick_test_complete, NULL);

	if (rc) {
		g_completion_success = false;
		wake_ut_thread();
	}
}

static void
blockdev_copy(struct io_target *target, uint64_t dst_offset, uint64_t src_offset, int data_len)
{
	struct bdevio_request req;

	req.target = target;
	req.data_len = data_len;
	req.offset = dst_offset;
	req.src_offset = src_offset;

	g_completion_success = false;

	execute_spdk_function(_blockdev_copy, &req);
}

static int
blockdev_write_read_data_match(char *rx_buf, char *tx_buf, int data_length)
{
	return memcmp(rx_buf, tx_buf, data_length);
}

static void
blockdev_write_read(uint32_t data_length, uint32_t iov_len, int pattern, uint64_t offset,
		    int expected_rc, bool write_zeroes, uint32_t block_size)
{
	struct io_target *target;
	char	*tx_buf = NULL;
	char	*rx_buf = NULL;
	int	rc;
	uint64_t write_offset = offset;
	uint32_t write_data_len = data_length;

	target = g_current_io_target;

	if (spdk_bdev_get_write_unit_size(target->bdev) > 1 && expected_rc == 0) {
		uint32_t write_unit_bytes;

		write_unit_bytes = spdk_bdev_get_write_unit_size(target->bdev) *
				   spdk_bdev_get_block_size(target->bdev);
		write_offset -= offset % write_unit_bytes;
		write_data_len += (offset - write_offset);

		if (write_data_len % write_unit_bytes) {
			write_data_len += write_unit_bytes - write_data_len % write_unit_bytes;
		}
	}

	if (!write_zeroes) {
		initialize_buffer(&tx_buf, pattern, write_data_len, block_size);
		initialize_buffer(&rx_buf, 0, data_length, block_size);

		blockdev_write(target, tx_buf, write_offset, write_data_len, iov_len);
	} else {
		initialize_buffer(&tx_buf, 0, write_data_len, block_size);
		initialize_buffer(&rx_buf, pattern, data_length, block_size);

		blockdev_write_zeroes(target, tx_buf, write_offset, write_data_len);
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
		rc = blockdev_write_read_data_match(rx_buf, tx_buf + (offset - write_offset), data_length);
		/* Assert the write by comparing it with values read
		 * from each blockdev */
		CU_ASSERT_EQUAL(rc, 0);
	}

	spdk_free(rx_buf);
	spdk_free(tx_buf);
}

static void
blockdev_compare_and_write(uint32_t data_length, uint32_t iov_len, uint64_t offset)
{
	struct io_target *target = g_current_io_target;
	struct spdk_bdev *bdev = target->bdev;
	char	*tx_buf = NULL;
	char	*write_buf = NULL;
	char	*rx_buf = NULL;
	int	rc;
	const uint32_t block_size = spdk_bdev_get_block_size(bdev);

	initialize_buffer(&tx_buf, 0xAA, data_length, block_size);
	initialize_buffer(&rx_buf, 0, data_length, block_size);
	initialize_buffer(&write_buf, 0xBB, data_length, block_size);

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

	spdk_free(rx_buf);
	spdk_free(tx_buf);
	spdk_free(write_buf);
}

static void
blockdev_write_read_block(void)
{
	uint32_t data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;
	struct io_target *target = g_current_io_target;
	struct spdk_bdev *bdev = target->bdev;
	const uint32_t block_size = spdk_bdev_get_block_size(bdev);

	/* Data size = 1 block */
	data_length = block_size;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = 0;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 0, block_size);
}

static void
blockdev_write_zeroes_read_block(void)
{
	uint32_t data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;
	struct io_target *target = g_current_io_target;
	struct spdk_bdev *bdev = target->bdev;
	const uint32_t block_size = spdk_bdev_get_block_size(bdev);

	/* Data size = 1 block */
	data_length = block_size;
	offset = 0;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write_zeroes and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 1, block_size);
}

/*
 * This i/o will not have to split at the bdev layer.
 */
static void
blockdev_write_zeroes_read_no_split(void)
{
	uint32_t data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;
	struct io_target *target = g_current_io_target;
	struct spdk_bdev *bdev = target->bdev;
	const uint32_t block_size = spdk_bdev_get_block_size(bdev);

	/* Data size = block size aligned ZERO_BUFFER_SIZE */
	data_length = ZERO_BUFFER_SIZE; /* from bdev_internal.h */
	data_length -= ZERO_BUFFER_SIZE % block_size;
	offset = 0;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write_zeroes and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 1, block_size);
}

/*
 * This i/o will have to split at the bdev layer if
 * write-zeroes is not supported by the bdev.
 */
static void
blockdev_write_zeroes_read_split(void)
{
	uint32_t data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;
	struct io_target *target = g_current_io_target;
	struct spdk_bdev *bdev = target->bdev;
	const uint32_t block_size = spdk_bdev_get_block_size(bdev);

	/* Data size = block size aligned 3 * ZERO_BUFFER_SIZE */
	data_length = 3 * ZERO_BUFFER_SIZE; /* from bdev_internal.h */
	data_length -= data_length % block_size;
	offset = 0;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write_zeroes and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 1, block_size);
}

/*
 * This i/o will have to split at the bdev layer if
 * write-zeroes is not supported by the bdev. It also
 * tests a write size that is not an even multiple of
 * the bdev layer zero buffer size.
 */
static void
blockdev_write_zeroes_read_split_partial(void)
{
	uint32_t data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;
	struct io_target *target = g_current_io_target;
	struct spdk_bdev *bdev = target->bdev;
	uint32_t block_size = spdk_bdev_get_block_size(bdev);

	/* Data size = block size aligned 7 * ZERO_BUFFER_SIZE / 2 */
	data_length = ZERO_BUFFER_SIZE * 7 / 2;
	data_length -= data_length % block_size;
	offset = 0;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write_zeroes and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 1, block_size);
}

static void
blockdev_writev_readv_block(void)
{
	uint32_t data_length, iov_len;
	uint64_t offset;
	int pattern;
	int expected_rc;
	struct io_target *target = g_current_io_target;
	struct spdk_bdev *bdev = target->bdev;
	const uint32_t block_size = spdk_bdev_get_block_size(bdev);

	/* Data size = 1 block */
	data_length = block_size;
	iov_len = data_length;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = 0;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, iov_len, pattern, offset, expected_rc, 0, block_size);
}

static void
blockdev_comparev_and_writev(void)
{
	uint32_t data_length, iov_len;
	uint64_t offset;
	struct io_target *target = g_current_io_target;
	struct spdk_bdev *bdev = target->bdev;

	if (spdk_bdev_is_md_separate(bdev)) {
		/* TODO: remove this check once bdev layer properly supports
		 * compare and write for bdevs with separate md.
		 */
		SPDK_ERRLOG("skipping comparev_and_writev on bdev %s since it has\n"
			    "separate metadata which is not supported yet.\n",
			    spdk_bdev_get_name(bdev));
		return;
	}

	/* Data size = acwu size */
	data_length = spdk_bdev_get_block_size(bdev) * spdk_bdev_get_acwu(bdev);
	iov_len = data_length;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = 0;

	blockdev_compare_and_write(data_length, iov_len, offset);
}

static void
blockdev_writev_readv_30x1block(void)
{
	uint32_t data_length, iov_len;
	uint64_t offset;
	int pattern;
	int expected_rc;
	struct io_target *target = g_current_io_target;
	struct spdk_bdev *bdev = target->bdev;
	uint32_t block_size = spdk_bdev_get_block_size(bdev);

	/* Data size = 30 * block size */
	data_length = block_size * 30;
	iov_len = block_size;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = 0;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, iov_len, pattern, offset, expected_rc, 0, block_size);
}

static void
blockdev_write_read_8blocks(void)
{
	uint32_t data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;
	struct io_target *target = g_current_io_target;
	struct spdk_bdev *bdev = target->bdev;
	const uint32_t block_size = spdk_bdev_get_block_size(bdev);

	/* Data size = 8 * block size */
	data_length = block_size * 8;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = data_length;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 0, block_size);
}

static void
blockdev_writev_readv_8blocks(void)
{
	uint32_t data_length, iov_len;
	uint64_t offset;
	int pattern;
	int expected_rc;
	struct io_target *target = g_current_io_target;
	struct spdk_bdev *bdev = target->bdev;
	const uint32_t block_size = spdk_bdev_get_block_size(bdev);


	/* Data size = 8 * block size */
	data_length = block_size * 8;
	iov_len = data_length;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = data_length;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, iov_len, pattern, offset, expected_rc, 0, block_size);
}

static void
blockdev_write_read_size_gt_128k(void)
{
	uint32_t data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;
	struct io_target *target = g_current_io_target;
	struct spdk_bdev *bdev = target->bdev;
	uint32_t block_size = spdk_bdev_get_block_size(bdev);

	/* Data size = block size aligned 128K + 1 block */
	data_length = 128 * 1024;
	data_length -= data_length % block_size;
	data_length += block_size;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = block_size * 2;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 0, block_size);
}

static void
blockdev_writev_readv_size_gt_128k(void)
{
	uint32_t data_length, iov_len;
	uint64_t offset;
	int pattern;
	int expected_rc;
	struct io_target *target = g_current_io_target;
	struct spdk_bdev *bdev = target->bdev;
	uint32_t block_size = spdk_bdev_get_block_size(bdev);

	/* Data size = block size aligned 128K + 1 block */
	data_length = 128 * 1024;
	data_length -= data_length % block_size;
	data_length += block_size;
	iov_len = data_length;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = block_size * 2;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, iov_len, pattern, offset, expected_rc, 0, block_size);
}

static void
blockdev_writev_readv_size_gt_128k_two_iov(void)
{
	uint32_t data_length, iov_len;
	uint64_t offset;
	int pattern;
	int expected_rc;
	struct io_target *target = g_current_io_target;
	struct spdk_bdev *bdev = target->bdev;
	uint32_t block_size = spdk_bdev_get_block_size(bdev);

	/* Data size = block size aligned 128K + 1 block */
	data_length = 128 * 1024;
	data_length -= data_length % block_size;
	iov_len = data_length;
	data_length += block_size;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = block_size * 2;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, iov_len, pattern, offset, expected_rc, 0, block_size);
}

static void
blockdev_write_read_invalid_size(void)
{
	uint32_t data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;
	struct io_target *target = g_current_io_target;
	struct spdk_bdev *bdev = target->bdev;
	uint32_t block_size = spdk_bdev_get_block_size(bdev);

	/* Data size is not a multiple of the block size */
	data_length = block_size - 1;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = block_size * 2;
	pattern = 0xA3;
	/* Params are invalid, hence the expected return value
	 * of write and read for all blockdevs is < 0 */
	expected_rc = -1;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 0, block_size);
}

static void
blockdev_write_read_offset_plus_nbytes_equals_bdev_size(void)
{
	uint32_t data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;
	struct io_target *target = g_current_io_target;
	struct spdk_bdev *bdev = target->bdev;
	uint32_t block_size = spdk_bdev_get_block_size(bdev);

	data_length = block_size;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	/* The start offset has been set to a marginal value
	 * such that offset + nbytes == Total size of
	 * blockdev. */
	offset = ((spdk_bdev_get_num_blocks(bdev) - 1) * block_size);
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 0, block_size);
}

static void
blockdev_write_read_offset_plus_nbytes_gt_bdev_size(void)
{
	uint32_t data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;
	struct io_target *target = g_current_io_target;
	struct spdk_bdev *bdev = target->bdev;
	uint32_t block_size = spdk_bdev_get_block_size(bdev);

	/* Tests the overflow condition of the blockdevs. */
	data_length = block_size * 2;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	pattern = 0xA3;

	/* The start offset has been set to a valid value
	 * but offset + nbytes is greater than the Total size
	 * of the blockdev. The test should fail. */
	offset = (spdk_bdev_get_num_blocks(bdev) - 1) * block_size;
	/* Params are invalid, hence the expected return value
	 * of write and read for all blockdevs is < 0 */
	expected_rc = -1;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 0, block_size);
}

static void
blockdev_write_read_max_offset(void)
{
	int	data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;
	struct io_target *target = g_current_io_target;
	struct spdk_bdev *bdev = target->bdev;
	const uint32_t block_size = spdk_bdev_get_block_size(bdev);

	data_length = block_size;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	/* The start offset has been set to UINT64_MAX such that
	 * adding nbytes wraps around and points to an invalid address. */
	offset = UINT64_MAX;
	pattern = 0xA3;
	/* Params are invalid, hence the expected return value
	 * of write and read for all blockdevs is < 0 */
	expected_rc = -1;

	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 0, block_size);
}

static void
blockdev_overlapped_write_read_2blocks(void)
{
	int	data_length;
	uint64_t offset;
	int pattern;
	int expected_rc;
	struct io_target *target = g_current_io_target;
	struct spdk_bdev *bdev = target->bdev;
	const uint32_t block_size = spdk_bdev_get_block_size(bdev);

	/* Data size = 2 blocks */
	data_length = block_size * 2;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	offset = 0;
	pattern = 0xA3;
	/* Params are valid, hence the expected return value
	 * of write and read for all blockdevs is 0. */
	expected_rc = 0;
	/* Assert the write by comparing it with values read
	 * from the same offset for each blockdev */
	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 0, block_size);

	/* Overwrite the pattern 0xbb of size 2*block size on an address offset
	 * overlapping with the address written above and assert the new value in
	 * the overlapped address range */
	/* Populate 2*block size with value 0xBB */
	pattern = 0xBB;
	/* Offset = 1 block; Overlap offset addresses and write value 0xbb */
	offset = spdk_bdev_get_block_size(bdev);
	/* Assert the write by comparing it with values read
	 * from the overlapped offset for each blockdev */
	blockdev_write_read(data_length, 0, pattern, offset, expected_rc, 0, block_size);
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
	bool reset_supported;

	target = g_current_io_target;
	req.target = target;

	reset_supported = spdk_bdev_io_type_supported(target->bdev, SPDK_BDEV_IO_TYPE_RESET);
	g_completion_success = false;

	execute_spdk_function(__blockdev_reset, &req);

	CU_ASSERT_EQUAL(g_completion_success, reset_supported);
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
blockdev_test_copy(void)
{
	uint32_t data_length;
	uint64_t src_offset, dst_offset;
	struct io_target *target = g_current_io_target;
	struct spdk_bdev *bdev = target->bdev;
	char *tx_buf = NULL;
	char *rx_buf = NULL;
	int rc;
	const uint32_t block_size = spdk_bdev_get_block_size(bdev);

	if (!spdk_bdev_io_type_supported(target->bdev, SPDK_BDEV_IO_TYPE_COPY)) {
		return;
	}

	data_length = block_size;
	CU_ASSERT_TRUE(data_length < BUFFER_SIZE);
	src_offset = 0;
	dst_offset = block_size;

	initialize_buffer(&tx_buf, 0xAA, data_length, block_size);
	initialize_buffer(&rx_buf, 0, data_length, block_size);

	blockdev_write(target, tx_buf, src_offset, data_length, data_length);
	CU_ASSERT_EQUAL(g_completion_success, true);

	blockdev_copy(target, dst_offset, src_offset, data_length);
	CU_ASSERT_EQUAL(g_completion_success, true);

	blockdev_read(target, rx_buf, dst_offset, data_length, data_length);
	CU_ASSERT_EQUAL(g_completion_success, true);

	rc = blockdev_write_read_data_match(rx_buf, tx_buf, data_length);
	CU_ASSERT_EQUAL(rc, 0);
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
	assert(spdk_get_thread() == g_thread_init);
	assert(spdk_get_thread() == spdk_thread_get_app_thread());
	execute_spdk_function(__exit_io_thread, NULL);
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
		CU_add_test(suite, "blockdev write read block",
			    blockdev_write_read_block) == NULL
		|| CU_add_test(suite, "blockdev write zeroes read block",
			       blockdev_write_zeroes_read_block) == NULL
		|| CU_add_test(suite, "blockdev write zeroes read no split",
			       blockdev_write_zeroes_read_no_split) == NULL
		|| CU_add_test(suite, "blockdev write zeroes read split",
			       blockdev_write_zeroes_read_split) == NULL
		|| CU_add_test(suite, "blockdev write zeroes read split partial",
			       blockdev_write_zeroes_read_split_partial) == NULL
		|| CU_add_test(suite, "blockdev reset",
			       blockdev_test_reset) == NULL
		|| CU_add_test(suite, "blockdev write read 8 blocks",
			       blockdev_write_read_8blocks) == NULL
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
		|| CU_add_test(suite, "blockdev write read 2 blocks on overlapped address offset",
			       blockdev_overlapped_write_read_2blocks) == NULL
		|| CU_add_test(suite, "blockdev writev readv 8 blocks",
			       blockdev_writev_readv_8blocks) == NULL
		|| CU_add_test(suite, "blockdev writev readv 30 x 1block",
			       blockdev_writev_readv_30x1block) == NULL
		|| CU_add_test(suite, "blockdev writev readv block",
			       blockdev_writev_readv_block) == NULL
		|| CU_add_test(suite, "blockdev writev readv size > 128k",
			       blockdev_writev_readv_size_gt_128k) == NULL
		|| CU_add_test(suite, "blockdev writev readv size > 128k in two iovs",
			       blockdev_writev_readv_size_gt_128k_two_iov) == NULL
		|| CU_add_test(suite, "blockdev comparev and writev",
			       blockdev_comparev_and_writev) == NULL
		|| CU_add_test(suite, "blockdev nvme passthru rw",
			       blockdev_test_nvme_passthru_rw) == NULL
		|| CU_add_test(suite, "blockdev nvme passthru vendor specific",
			       blockdev_test_nvme_passthru_vendor_specific) == NULL
		|| CU_add_test(suite, "blockdev nvme admin passthru",
			       blockdev_test_nvme_admin_passthru) == NULL
		|| CU_add_test(suite, "blockdev copy",
			       blockdev_test_copy) == NULL
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

	if (CU_initialize_registry() != CUE_SUCCESS) {
		/* CUnit error, probably won't recover */
		rc = CU_get_error();
		rc = -rc;
		goto ret;
	}

	target = g_io_targets;
	while (target != NULL) {
		rc = __setup_ut_on_single_target(target);
		if (rc < 0) {
			/* CUnit error, probably won't recover */
			rc = -rc;
			goto ret;
		}
		target = target->next;
	}
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	rc = CU_get_number_of_failures();
	CU_cleanup_registry();

ret:
	stop_init_thread(rc, request);
	assert(spdk_get_thread() == g_thread_ut);
	spdk_thread_exit(g_thread_ut);
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
	uint32_t i;

	pthread_mutex_init(&g_test_mutex, NULL);
	pthread_cond_init(&g_test_cond, NULL);

	/* This test runs specifically on at least three cores.
	 * g_thread_init is the app_thread on main core from event framework.
	 * Next two are only for the tests and should always be on separate CPU cores. */
	if (spdk_env_get_core_count() < 3) {
		spdk_app_stop(-1);
		return;
	}

	SPDK_ENV_FOREACH_CORE(i) {
		if (i == spdk_env_get_current_core()) {
			g_thread_init = spdk_get_thread();
			continue;
		}
		spdk_cpuset_zero(&tmpmask);
		spdk_cpuset_set_cpu(&tmpmask, i, true);
		if (g_thread_ut == NULL) {
			g_thread_ut = spdk_thread_create("ut_thread", &tmpmask);
		} else if (g_thread_io == NULL) {
			g_thread_io = spdk_thread_create("io_thread", &tmpmask);
		}

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

	spdk_app_opts_init(&opts, sizeof(opts));
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
