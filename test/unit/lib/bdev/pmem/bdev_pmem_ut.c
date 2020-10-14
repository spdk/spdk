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

#include "spdk_cunit.h"

#include "common/lib/ut_multithread.c"
#include "unit/lib/json_mock.c"

#include "spdk_internal/thread.h"

#include "bdev/pmem/bdev_pmem.c"

static struct spdk_bdev_module *g_bdev_pmem_module;
static int g_bdev_module_cnt;

struct pmemblk {
	const char *name;
	bool is_open;
	bool is_consistent;
	size_t bsize;
	long long nblock;

	uint8_t *buffer;
};

static const char *g_bdev_name = "pmem0";

/* PMEMblkpool is a typedef of struct pmemblk */
static PMEMblkpool g_pool_ok = {
	.name = "/pools/ok_pool",
	.is_open = false,
	.is_consistent = true,
	.bsize = 4096,
	.nblock = 150
};

static PMEMblkpool g_pool_nblock_0 = {
	.name = "/pools/nblock_0",
	.is_open = false,
	.is_consistent = true,
	.bsize = 4096,
	.nblock = 0
};

static PMEMblkpool g_pool_bsize_0 = {
	.name = "/pools/nblock_0",
	.is_open = false,
	.is_consistent = true,
	.bsize = 0,
	.nblock = 100
};

static PMEMblkpool g_pool_inconsistent = {
	.name = "/pools/inconsistent",
	.is_open = false,
	.is_consistent = false,
	.bsize = 512,
	.nblock = 1
};

static int g_opened_pools;
static struct spdk_bdev *g_bdev;
static const char *g_check_version_msg;
static bool g_pmemblk_open_allow_open = true;

static PMEMblkpool *
find_pmemblk_pool(const char *path)
{
	if (path == NULL) {
		errno = EINVAL;
		return NULL;
	} else if (strcmp(g_pool_ok.name, path) == 0) {
		return &g_pool_ok;
	} else if (strcmp(g_pool_nblock_0.name, path) == 0) {
		return &g_pool_nblock_0;
	} else if (strcmp(g_pool_bsize_0.name, path) == 0) {
		return &g_pool_bsize_0;
	} else if (strcmp(g_pool_inconsistent.name, path) == 0) {
		return &g_pool_inconsistent;
	}

	errno = ENOENT;
	return NULL;
}

PMEMblkpool *
pmemblk_open(const char *path, size_t bsize)
{
	PMEMblkpool *pool;

	if (!g_pmemblk_open_allow_open) {
		errno = EIO;
		return NULL;
	}

	pool = find_pmemblk_pool(path);
	if (!pool) {
		errno = ENOENT;
		return NULL;
	}

	CU_ASSERT_TRUE_FATAL(pool->is_consistent);
	CU_ASSERT_FALSE(pool->is_open);
	if (pool->is_open == false) {
		pool->is_open = true;
		g_opened_pools++;
	} else {
		errno = EBUSY;
		pool = NULL;
	}

	return pool;
}
void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb, uint64_t len)
{
	cb(NULL, bdev_io, true);
}

static void
check_open_pool_fatal(PMEMblkpool *pool)
{
	SPDK_CU_ASSERT_FATAL(pool != NULL);
	SPDK_CU_ASSERT_FATAL(find_pmemblk_pool(pool->name) == pool);
	SPDK_CU_ASSERT_FATAL(pool->is_open == true);
}

void
pmemblk_close(PMEMblkpool *pool)
{
	check_open_pool_fatal(pool);
	pool->is_open = false;
	CU_ASSERT(g_opened_pools > 0);
	g_opened_pools--;
}

size_t
pmemblk_bsize(PMEMblkpool *pool)
{
	check_open_pool_fatal(pool);
	return pool->bsize;
}

size_t
pmemblk_nblock(PMEMblkpool *pool)
{
	check_open_pool_fatal(pool);
	return pool->nblock;
}

int
pmemblk_read(PMEMblkpool *pool, void *buf, long long blockno)
{
	check_open_pool_fatal(pool);
	if (blockno >= pool->nblock) {
		errno = EINVAL;
		return -1;
	}

	memcpy(buf, &pool->buffer[blockno * pool->bsize], pool->bsize);
	return 0;
}

int
pmemblk_write(PMEMblkpool *pool, const void *buf, long long blockno)
{
	check_open_pool_fatal(pool);
	if (blockno >= pool->nblock) {
		errno = EINVAL;
		return -1;
	}

	memcpy(&pool->buffer[blockno * pool->bsize], buf, pool->bsize);
	return 0;
}

int
pmemblk_set_zero(PMEMblkpool *pool, long long blockno)
{
	check_open_pool_fatal(pool);
	if (blockno >= pool->nblock) {

		errno = EINVAL;
		return -1;
	}

	memset(&pool->buffer[blockno * pool->bsize], 0, pool->bsize);
	return 0;
}

const char *
pmemblk_errormsg(void)
{
	return strerror(errno);
}

const char *
pmemblk_check_version(unsigned major_required, unsigned minor_required)
{
	return g_check_version_msg;
}

int
pmemblk_check(const char *path, size_t bsize)
{
	PMEMblkpool *pool = find_pmemblk_pool(path);

	if (!pool) {
		errno = ENOENT;
		return -1;
	}

	if (!pool->is_consistent) {
		/* errno ? */
		return 0;
	}

	if (bsize != 0 && pool->bsize != bsize) {
		/* errno ? */
		return 0;
	}

	return 1;
}

void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	bdev_io->internal.status = status;
}

int
spdk_bdev_register(struct spdk_bdev *bdev)
{
	CU_ASSERT_PTR_NULL(g_bdev);
	g_bdev = bdev;

	return 0;
}

void
spdk_bdev_unregister(struct spdk_bdev *bdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
}

void
spdk_bdev_module_finish_done(void)
{
}

int
spdk_bdev_notify_blockcnt_change(struct spdk_bdev *bdev, uint64_t size)
{
	bdev->blockcnt = size;
	return 0;
}

static void
ut_bdev_pmem_destruct(struct spdk_bdev *bdev)
{
	SPDK_CU_ASSERT_FATAL(g_bdev != NULL);
	CU_ASSERT_EQUAL(bdev_pmem_destruct(bdev->ctxt), 0);
	g_bdev = NULL;
}

void
spdk_bdev_module_list_add(struct spdk_bdev_module *bdev_module)
{
	g_bdev_pmem_module = bdev_module;
	g_bdev_module_cnt++;
}

static int
bdev_submit_request(struct spdk_bdev *bdev, int16_t io_type, uint64_t offset_blocks,
		    uint64_t num_blocks, struct iovec *iovs, size_t iov_cnt)
{
	struct spdk_bdev_io bio = { 0 };

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
		bio.u.bdev.iovs = iovs;
		bio.u.bdev.iovcnt = iov_cnt;
		bio.u.bdev.offset_blocks = offset_blocks;
		bio.u.bdev.num_blocks = num_blocks;
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		bio.u.bdev.iovs = iovs;
		bio.u.bdev.iovcnt = iov_cnt;
		bio.u.bdev.offset_blocks = offset_blocks;
		bio.u.bdev.num_blocks = num_blocks;
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		bio.u.bdev.offset_blocks = offset_blocks;
		bio.u.bdev.num_blocks = num_blocks;
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		bio.u.bdev.offset_blocks = offset_blocks;
		bio.u.bdev.num_blocks = num_blocks;
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		bio.u.bdev.offset_blocks = offset_blocks;
		bio.u.bdev.num_blocks = num_blocks;
		break;
	default:
		CU_FAIL_FATAL("BUG:Unexpected IO type");
		break;
	}

	/*
	 * Set status to value that shouldn't be returned
	 */
	bio.type = io_type;
	bio.internal.status = SPDK_BDEV_IO_STATUS_PENDING;
	bio.bdev = bdev;
	bdev_pmem_submit_request(NULL, &bio);
	return bio.internal.status;
}


static int
ut_pmem_blk_clean(void)
{
	free(g_pool_ok.buffer);
	g_pool_ok.buffer = NULL;

	/* Unload module to free IO channel */
	g_bdev_pmem_module->module_fini();
	poll_threads();

	free_threads();

	return 0;
}

static int
ut_pmem_blk_init(void)
{
	errno = 0;

	allocate_threads(1);
	set_thread(0);

	g_pool_ok.buffer = calloc(g_pool_ok.nblock, g_pool_ok.bsize);
	if (g_pool_ok.buffer == NULL) {
		ut_pmem_blk_clean();
		return -1;
	}

	return 0;
}

static void
ut_pmem_init(void)
{
	SPDK_CU_ASSERT_FATAL(g_bdev_pmem_module != NULL);
	CU_ASSERT_EQUAL(g_bdev_module_cnt, 1);

	/* Make pmemblk_check_version fail with provided error message */
	g_check_version_msg = "TEST FAIL MESSAGE";
	CU_ASSERT_NOT_EQUAL(g_bdev_pmem_module->module_init(), 0);

	/* This init must success */
	g_check_version_msg = NULL;
	CU_ASSERT_EQUAL(g_bdev_pmem_module->module_init(), 0);
}

static void
ut_pmem_open_close(void)
{
	struct spdk_bdev *bdev = NULL;
	int pools_cnt;
	int rc;

	pools_cnt = g_opened_pools;

	/* Try opening with NULL name */
	rc = create_pmem_disk(NULL, NULL, &bdev);
	CU_ASSERT_PTR_NULL(bdev);
	CU_ASSERT_EQUAL(pools_cnt, g_opened_pools);
	CU_ASSERT_NOT_EQUAL(rc, 0);

	/* Open non-existent pool */
	rc = create_pmem_disk("non existent pool", NULL, &bdev);
	CU_ASSERT_PTR_NULL(bdev);
	CU_ASSERT_EQUAL(pools_cnt, g_opened_pools);
	CU_ASSERT_NOT_EQUAL(rc, 0);

	/* Open inconsistent pool */
	rc = create_pmem_disk(g_pool_inconsistent.name, NULL, &bdev);
	CU_ASSERT_PTR_NULL(bdev);
	CU_ASSERT_EQUAL(pools_cnt, g_opened_pools);
	CU_ASSERT_NOT_EQUAL(rc, 0);

	/* Open consistent pool fail the open from unknown reason. */
	g_pmemblk_open_allow_open = false;
	rc = create_pmem_disk(g_pool_inconsistent.name, NULL, &bdev);
	g_pmemblk_open_allow_open = true;
	CU_ASSERT_PTR_NULL(bdev);
	CU_ASSERT_EQUAL(pools_cnt, g_opened_pools);
	CU_ASSERT_NOT_EQUAL(rc, 0);

	/* Open pool with nblocks = 0 */
	rc = create_pmem_disk(g_pool_nblock_0.name, NULL, &bdev);
	CU_ASSERT_PTR_NULL(bdev);
	CU_ASSERT_EQUAL(pools_cnt, g_opened_pools);
	CU_ASSERT_NOT_EQUAL(rc, 0);

	/* Open pool with bsize = 0 */
	rc = create_pmem_disk(g_pool_bsize_0.name, NULL, &bdev);
	CU_ASSERT_PTR_NULL(bdev);
	CU_ASSERT_EQUAL(pools_cnt, g_opened_pools);
	CU_ASSERT_NOT_EQUAL(rc, 0);

	/* Open pool with NULL name */
	rc = create_pmem_disk(g_pool_ok.name, NULL, &bdev);
	CU_ASSERT_PTR_NULL(bdev);
	CU_ASSERT_EQUAL(pools_cnt, g_opened_pools);
	CU_ASSERT_NOT_EQUAL(rc, 0);

	/* Open good pool */
	rc = create_pmem_disk(g_pool_ok.name, g_bdev_name, &bdev);
	SPDK_CU_ASSERT_FATAL(bdev != NULL);
	CU_ASSERT_TRUE(g_pool_ok.is_open);
	CU_ASSERT_EQUAL(pools_cnt + 1, g_opened_pools);
	CU_ASSERT_EQUAL(rc, 0);

	/* Now remove this bdev */
	ut_bdev_pmem_destruct(bdev);
	CU_ASSERT_FALSE(g_pool_ok.is_open);
	CU_ASSERT_EQUAL(pools_cnt, g_opened_pools);
}

static void
ut_pmem_write_read(void)
{
	uint8_t *write_buf, *read_buf;
	struct spdk_bdev *bdev;
	int rc;
	size_t unaligned_aligned_size = 100;
	size_t buf_size = g_pool_ok.bsize *  g_pool_ok.nblock;
	size_t i;
	const uint64_t nblock_offset = 10;
	uint64_t offset;
	size_t io_size, nblock, total_io_size, bsize;

	bsize = 4096;
	struct iovec iov[] = {
		{ 0, 2 * bsize },
		{ 0, 3 * bsize },
		{ 0, 4 * bsize },
	};

	rc = create_pmem_disk(g_pool_ok.name, g_bdev_name, &bdev);
	CU_ASSERT_EQUAL(rc, 0);

	SPDK_CU_ASSERT_FATAL(g_pool_ok.nblock > 40);

	write_buf = calloc(1, buf_size);
	read_buf = calloc(1, buf_size);

	SPDK_CU_ASSERT_FATAL(bdev != NULL);
	SPDK_CU_ASSERT_FATAL(write_buf != NULL);
	SPDK_CU_ASSERT_FATAL(read_buf != NULL);

	total_io_size = 0;
	offset = nblock_offset * g_pool_ok.bsize;
	for (i = 0; i < 3; i++) {
		iov[i].iov_base =  &write_buf[offset + total_io_size];
		total_io_size += iov[i].iov_len;
	}

	for (i = 0; i < total_io_size + unaligned_aligned_size; i++) {
		write_buf[offset + i] = 0x42 + i;
	}

	SPDK_CU_ASSERT_FATAL(total_io_size < buf_size);

	/*
	 * Write outside pool.
	 */
	rc = bdev_submit_request(bdev, SPDK_BDEV_IO_TYPE_WRITE, g_pool_ok.nblock, 1, &iov[0], 2);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_FAILED);

	/*
	 * Write with insufficient IOV buffers length.
	 */
	rc = bdev_submit_request(bdev, SPDK_BDEV_IO_TYPE_WRITE, 0, g_pool_ok.nblock, &iov[0], 2);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_FAILED);

	/*
	 * Try to write two IOV with first one iov_len % bsize != 0.
	 */
	io_size = iov[0].iov_len + iov[1].iov_len;
	nblock = io_size / g_pool_ok.bsize;
	iov[0].iov_len += unaligned_aligned_size;
	rc = bdev_submit_request(bdev, SPDK_BDEV_IO_TYPE_WRITE, 0, nblock, &iov[0], 2);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_FAILED);
	iov[0].iov_len -= unaligned_aligned_size;

	/*
	 * Try to write one IOV.
	 */
	nblock = iov[0].iov_len / g_pool_ok.bsize;
	rc = bdev_submit_request(bdev, SPDK_BDEV_IO_TYPE_WRITE, nblock_offset, nblock, &iov[0], 1);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_SUCCESS);

	/*
	 * Try to write 2 IOV.
	 * Sum of IOV length is larger than IO size and last IOV is larger and iov_len % bsize != 0
	 */
	offset = iov[0].iov_len / g_pool_ok.bsize;
	io_size = iov[1].iov_len + iov[2].iov_len;
	nblock = io_size / g_pool_ok.bsize;
	iov[2].iov_len += unaligned_aligned_size;
	rc = bdev_submit_request(bdev, SPDK_BDEV_IO_TYPE_WRITE, nblock_offset + offset, nblock,
				 &iov[1], 2);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_SUCCESS);
	iov[2].iov_len -= unaligned_aligned_size;

	/*
	 * Examine pool state:
	 * 1. Written area should have expected values.
	 * 2. Anything else should contain zeros.
	 */
	offset = nblock_offset * g_pool_ok.bsize + total_io_size;
	rc = memcmp(&g_pool_ok.buffer[0], write_buf, offset);
	CU_ASSERT_EQUAL(rc, 0);

	for (i = offset; i < buf_size; i++) {
		if (g_pool_ok.buffer[i] != 0) {
			CU_ASSERT_EQUAL(g_pool_ok.buffer[i], 0);
			break;
		}
	}

	/* Setup IOV for reads */
	memset(read_buf, 0xAB, buf_size);
	offset = nblock_offset * g_pool_ok.bsize;
	for (i = 0; i < 3; i++) {
		iov[i].iov_base =  &read_buf[offset];
		offset += iov[i].iov_len;
	}

	/*
	  * Write outside pool.
	 */
	rc = bdev_submit_request(bdev, SPDK_BDEV_IO_TYPE_READ, g_pool_ok.nblock, 1, &iov[0], 2);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_FAILED);

	/*
	 * Read with insufficient IOV buffers length.
	 */
	rc = bdev_submit_request(bdev, SPDK_BDEV_IO_TYPE_READ, 0, g_pool_ok.nblock, &iov[0], 2);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_FAILED);

	/*
	 * Try to read two IOV with first one iov_len % bsize != 0.
	 */
	io_size = iov[0].iov_len + iov[1].iov_len;
	nblock = io_size / g_pool_ok.bsize;
	iov[0].iov_len += unaligned_aligned_size;
	rc = bdev_submit_request(bdev, SPDK_BDEV_IO_TYPE_READ, 0, nblock, &iov[0], 2);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_FAILED);
	iov[0].iov_len -= unaligned_aligned_size;

	/*
	 * Try to write one IOV.
	 */
	nblock = iov[0].iov_len / g_pool_ok.bsize;
	rc = bdev_submit_request(bdev, SPDK_BDEV_IO_TYPE_READ, nblock_offset, nblock, &iov[0], 1);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_SUCCESS);

	/*
	 * Try to read 2 IOV.
	 * Sum of IOV length is larger than IO size and last IOV is larger and iov_len % bsize != 0
	 */
	offset = iov[0].iov_len / g_pool_ok.bsize;
	io_size = iov[1].iov_len + iov[2].iov_len;
	nblock = io_size / g_pool_ok.bsize;
	iov[2].iov_len += unaligned_aligned_size;
	rc = bdev_submit_request(bdev, SPDK_BDEV_IO_TYPE_READ, nblock_offset + offset, nblock,
				 &iov[1], 2);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_SUCCESS);
	iov[2].iov_len -= unaligned_aligned_size;


	/*
	 * Examine what we read state:
	 * 1. Written area should have expected values.
	 * 2. Anything else should contain zeros.
	 */
	offset = nblock_offset * g_pool_ok.bsize;
	for (i = 0; i < offset; i++) {
		if (read_buf[i] != 0xAB) {
			CU_ASSERT_EQUAL(read_buf[i], 0xAB);
			break;
		}
	}

	rc = memcmp(&read_buf[offset], &write_buf[offset], total_io_size);
	CU_ASSERT_EQUAL(rc, 0);

	offset += total_io_size;
	for (i = offset; i < buf_size; i++) {
		if (read_buf[i] != 0xAB) {
			CU_ASSERT_EQUAL(read_buf[i], 0xAB);
			break;
		}
	}

	memset(g_pool_ok.buffer, 0, g_pool_ok.bsize *  g_pool_ok.nblock);
	free(write_buf);
	free(read_buf);

	/* Now remove this bdev */
	ut_bdev_pmem_destruct(bdev);
	CU_ASSERT_FALSE(g_pool_ok.is_open);
	CU_ASSERT_EQUAL(g_opened_pools, 0);
}

static void
ut_pmem_reset(void)
{
	struct spdk_bdev *bdev;
	int rc;

	rc = create_pmem_disk(g_pool_ok.name, g_bdev_name, &bdev);
	CU_ASSERT_EQUAL(rc, 0);
	SPDK_CU_ASSERT_FATAL(bdev != NULL);

	rc = bdev_submit_request(bdev, SPDK_BDEV_IO_TYPE_RESET, 0, 0, NULL, 0);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_SUCCESS);

	ut_bdev_pmem_destruct(bdev);
}

static void
ut_pmem_unmap_write_zero(int16_t io_type)
{
	struct spdk_bdev *bdev;
	size_t buff_size = g_pool_ok.nblock * g_pool_ok.bsize;
	size_t i;
	uint8_t *buffer;
	int rc;

	CU_ASSERT(io_type == SPDK_BDEV_IO_TYPE_UNMAP || io_type == SPDK_BDEV_IO_TYPE_WRITE_ZEROES);
	rc = create_pmem_disk(g_pool_ok.name, g_bdev_name, &bdev);
	CU_ASSERT_EQUAL(rc, 0);
	SPDK_CU_ASSERT_FATAL(bdev != NULL);
	SPDK_CU_ASSERT_FATAL(g_pool_ok.nblock > 40);

	buffer = calloc(1, buff_size);
	SPDK_CU_ASSERT_FATAL(buffer != NULL);

	for (i = 10 * g_pool_ok.bsize; i < 30 * g_pool_ok.bsize; i++) {
		buffer[i] = 0x30 + io_type + i;
	}
	memcpy(g_pool_ok.buffer, buffer, buff_size);

	/*
	 * Block outside of pool.
	 */
	rc = bdev_submit_request(bdev, io_type, g_pool_ok.nblock, 1, NULL, 0);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_FAILED);

	rc = memcmp(buffer, g_pool_ok.buffer, buff_size);
	CU_ASSERT_EQUAL(rc, 0);

	/*
	 * Blocks 15 to 25
	 */
	memset(&buffer[15 * g_pool_ok.bsize], 0, 10 * g_pool_ok.bsize);
	rc = bdev_submit_request(bdev, io_type, 15, 10, NULL, 0);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_SUCCESS);

	rc = memcmp(buffer, g_pool_ok.buffer, buff_size);
	CU_ASSERT_EQUAL(rc, 0);

	/*
	 * All blocks.
	 */
	memset(buffer, 0, buff_size);
	rc = bdev_submit_request(bdev, io_type, 0, g_pool_ok.nblock, NULL, 0);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_SUCCESS);

	rc = memcmp(buffer, g_pool_ok.buffer, buff_size);
	CU_ASSERT_EQUAL(rc, 0);

	/* Now remove this bdev */
	ut_bdev_pmem_destruct(bdev);
	CU_ASSERT_FALSE(g_pool_ok.is_open);
	CU_ASSERT_EQUAL(g_opened_pools, 0);

	free(buffer);
}

static void
ut_pmem_write_zero(void)
{
	ut_pmem_unmap_write_zero(SPDK_BDEV_IO_TYPE_WRITE_ZEROES);
}

static void
ut_pmem_unmap(void)
{
	ut_pmem_unmap_write_zero(SPDK_BDEV_IO_TYPE_UNMAP);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("bdev_pmem", ut_pmem_blk_init, ut_pmem_blk_clean);

	CU_ADD_TEST(suite, ut_pmem_init);
	CU_ADD_TEST(suite, ut_pmem_open_close);
	CU_ADD_TEST(suite, ut_pmem_write_read);
	CU_ADD_TEST(suite, ut_pmem_reset);
	CU_ADD_TEST(suite, ut_pmem_write_zero);
	CU_ADD_TEST(suite, ut_pmem_unmap);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
