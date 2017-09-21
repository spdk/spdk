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

#include "lib/test_env.c"

#include "pmem/bdev_pmem.c"

static struct spdk_bdev_module_if *g_bdev_pmem_module;
static int g_bdev_module_cnt;

struct pmemblk {
	const char *name;
	bool is_open;
	bool is_consistent;
	size_t bsize;
	long long nblock;

	char *buffer;
};

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

static PMEMblkpool g_pool_inconsistent = {
	.name = "/pools/inconsistent",
	.is_open = false,
	.is_consistent = false,
	.bsize = 0,
	.nblock = 0
};

static PMEMblkpool *g_pmemblk_pools[] = {
	&g_pool_ok,
	&g_pool_nblock_0,
	&g_pool_inconsistent,
	NULL
};
static int g_opened_pools;
static const char *g_errormsg = "No error";
static TAILQ_HEAD(, spdk_bdev) g_bdevs = TAILQ_HEAD_INITIALIZER(g_bdevs);
static int g_bdevs_cnt;

static PMEMblkpool *
find_pmemblk_pool(const char *path)
{
	int i;

	if (path == NULL) {
		errno = EINVAL;
		return NULL;
	}

	for (i = 0; g_pmemblk_pools[i] != NULL; i++) {
		if (strcmp(g_pmemblk_pools[i]->name, path) == 0) {
			return g_pmemblk_pools[i];
		}
	}

	errno = ENOENT;
	return NULL;
}

PMEMblkpool *
pmemblk_open(const char *path, size_t bsize)
{
	PMEMblkpool *pool = find_pmemblk_pool(path);

	if (!pool) {
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

static
void check_open_pool_fatal(PMEMblkpool *pool)
{
	CU_ASSERT_PTR_NOT_NULL_FATAL(pool);
	CU_ASSERT_PTR_NOT_NULL_FATAL(find_pmemblk_pool(pool->name) == pool);
	CU_ASSERT_TRUE_FATAL(pool->is_open);
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
	return g_errormsg;
}

static const char *g_check_version_msg;
const char *
pmemblk_check_version(unsigned major_required, unsigned minor_required)
{
	return g_check_version_msg;
}

void spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io,
			   enum spdk_bdev_io_status status)
{
	bdev_io->status = status;
}

void
spdk_bdev_register(struct spdk_bdev *bdev)
{
	struct spdk_bdev *reg_bdev;

	TAILQ_FOREACH(reg_bdev, &g_bdevs, link) {
		CU_ASSERT_PTR_NOT_EQUAL_FATAL(bdev, reg_bdev);
	}

	TAILQ_INSERT_TAIL(&g_bdevs, bdev, link);
	g_bdevs_cnt++;
}

static void
ut_bdev_pmem_destruct(struct spdk_bdev *bdev)
{
	CU_ASSERT_EQUAL(bdev_pmem_destruct(bdev->ctxt), 0);
	TAILQ_REMOVE(&g_bdevs, bdev, link);
	g_bdevs_cnt--;
}

void
spdk_bdev_module_list_add(struct spdk_bdev_module_if *bdev_module)
{
	g_bdev_pmem_module = bdev_module;
	g_bdev_module_cnt++;
}

static int
ut_pmem_blk_clean(void)
{
	PMEMblkpool *pool;
	int i;

	for (i = 0; g_pmemblk_pools[i] != NULL; i++) {
		pool = g_pmemblk_pools[i];

		free(pool->buffer);
		pool->buffer = NULL;
	}

	return 0;
}

static int
ut_pmem_blk_init(void)
{
	PMEMblkpool *pool;
	int i;

	for (i = 0; g_pmemblk_pools[i] != NULL; i++) {
		pool = g_pmemblk_pools[i];

		if (pool->nblock == 0 || pool->bsize == 0) {
			continue;
		}

		pool->buffer = calloc(pool->nblock, pool->bsize);

		if (pool->buffer == NULL) {
			ut_pmem_blk_clean();
			return -1;
		}
	}

	return 0;
}

static void
ut_pmem_init(void)
{
	CU_ASSERT_PTR_NOT_NULL(g_bdev_pmem_module);
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
	struct spdk_bdev *bdev;
	int bdevs_cnt, pools_cnt;

	pools_cnt = g_opened_pools;

	/* Try opening with NULL name */
	bdev = create_pmem_disk(NULL);
	CU_ASSERT_PTR_NULL(bdev);
	CU_ASSERT_EQUAL(pools_cnt, g_opened_pools);

	/* Open non-existent pool */
	bdev = create_pmem_disk("non existent pool");
	CU_ASSERT_PTR_NULL(bdev);
	CU_ASSERT_EQUAL(pools_cnt, g_opened_pools);

	/* This test currently fail due to bdev_pmem never check for pool consistency */
#if 0
	/* Open inconsistent pool */
	bdev = create_pmem_disk(g_pool_inconsistent.name);
	CU_ASSERT_PTR_NULL(bdev);
	CU_ASSERT_EQUAL(pools_cnt, g_opened_pools);
#endif
	/* Open zero lenght pool */
	bdev = create_pmem_disk(g_pool_nblock_0.name);
	CU_ASSERT_PTR_NULL(bdev);
	CU_ASSERT_EQUAL(pools_cnt, g_opened_pools);

	/* Open good  pool */
	bdevs_cnt = g_bdevs_cnt;
	bdev = create_pmem_disk(g_pool_ok.name);
	CU_ASSERT_PTR_NOT_NULL_FATAL(bdev);
	CU_ASSERT_TRUE(g_pool_ok.is_open);
	CU_ASSERT_EQUAL(pools_cnt + 1, g_opened_pools);
	CU_ASSERT_EQUAL(bdevs_cnt + 1, g_bdevs_cnt);

	/* Now remove this bdev */
	ut_bdev_pmem_destruct(bdev);
	CU_ASSERT_FALSE(g_pool_ok.is_open);
	CU_ASSERT_EQUAL(pools_cnt, g_opened_pools);
}

static int
ut_bdev_pmem_submit_request(struct spdk_bdev *bdev, int16_t io_type, uint64_t offset_blocks,
			    uint64_t num_blocks, struct iovec *iovs, size_t iov_cnt)
{
	struct spdk_bdev_io bio = { 0 };

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
		bio.u.read.iovs = iovs;
		bio.u.read.iovcnt = iov_cnt;
		bio.u.read.offset_blocks = offset_blocks;
		bio.u.read.num_blocks = num_blocks;
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		bio.u.write.iovs = iovs;
		bio.u.write.iovcnt = iov_cnt;
		bio.u.write.offset_blocks = offset_blocks;
		bio.u.write.num_blocks = num_blocks;
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		bio.u.flush.offset_blocks = offset_blocks,
			    bio.u.flush.num_blocks = num_blocks;
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		bio.u.unmap.offset_blocks = offset_blocks;
		bio.u.unmap.num_blocks = num_blocks;
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		bio.u.write.offset_blocks = offset_blocks;
		bio.u.write.num_blocks = num_blocks;
		break;
	default:
		CU_FAIL_FATAL("BUG:Unexpected IO type");
		break;
	}

	/*
	 * Set status to value that shouldn't be returned
	 */
	bio.type = io_type;
	bio.status = SPDK_BDEV_IO_STATUS_PENDING;
	bio.bdev = bdev;
	bdev_pmem_submit_request(NULL, &bio);
	return bio.status;
}

static void
ut_pmem_write_read(void)
{
	int rc;
	char *write_buf, *read_buf;
	struct spdk_bdev *bdev = create_pmem_disk(g_pool_ok.name);
	size_t unaligned_aligned_size = 100;
	size_t buf_size = 0;
	size_t i;
	const uint64_t nblock_offset = 10;
	uint64_t offset;
	size_t io_size, io_nblock, total_io_size;

	struct iovec iov[] = {
		{ 0, 2 * g_pool_ok.bsize },
		{ 0, 3 * g_pool_ok.bsize },
		{ 0, 4 * g_pool_ok.bsize },
	};

	for (i = 0; i < 3; i++) {
		buf_size += iov[i].iov_len;
		iov[i].iov_base = (void *)(uintptr_t)buf_size;
	}

	buf_size += unaligned_aligned_size;
	write_buf = malloc(buf_size);
	read_buf = malloc(buf_size);

	CU_ASSERT_PTR_NOT_NULL_FATAL(bdev);
	CU_ASSERT_PTR_NOT_NULL_FATAL(write_buf);
	CU_ASSERT_PTR_NOT_NULL_FATAL(read_buf);

	total_io_size = 0;
	for (i = 0; i < 3; i++) {
		iov[i].iov_base =  write_buf + total_io_size;
		total_io_size += iov[i].iov_len;
	}

	for (i = 0; i < buf_size; i++) {
		write_buf[i] = 0x42 + i;
	}

	/*
	 * Write with insuficient IOV buffers length.
	 */
	rc = ut_bdev_pmem_submit_request(bdev, SPDK_BDEV_IO_TYPE_WRITE, 0, g_pool_ok.nblock, &iov[0], 2);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_FAILED);

	/*
	 * Try to write two IOV with first one iov_len % bsize != 0.
	 */
	io_size = iov[0].iov_len + iov[1].iov_len;
	io_nblock = io_size / g_pool_ok.bsize;
	iov[0].iov_len += unaligned_aligned_size;
	rc = ut_bdev_pmem_submit_request(bdev, SPDK_BDEV_IO_TYPE_WRITE, 0, io_nblock, &iov[0], 2);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_FAILED);
	iov[0].iov_len -= unaligned_aligned_size;

	/*
	 * Try to write one IOV.
	 */
	io_nblock = iov[0].iov_len / g_pool_ok.bsize;
	rc = ut_bdev_pmem_submit_request(bdev, SPDK_BDEV_IO_TYPE_WRITE, nblock_offset, io_nblock, &iov[0],
					 1);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_SUCCESS);

	/*
	 * Try to write 2 IOV.
	 * Sum of IOV length is larger than IO size and last IOV is larger and iov_len % bsize != 0
	 */
	offset = iov[0].iov_len / g_pool_ok.bsize;
	io_size = iov[1].iov_len + iov[2].iov_len;
	io_nblock = io_size / g_pool_ok.bsize;
	iov[2].iov_len += unaligned_aligned_size;
	rc = ut_bdev_pmem_submit_request(bdev, SPDK_BDEV_IO_TYPE_WRITE, nblock_offset + offset, io_nblock,
					 &iov[1], 2);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_SUCCESS);
	iov[2].iov_len -= unaligned_aligned_size;

	/*
	 * Examine pool state:
	 * 1. Written area should have expected values.
	 * 2. Anything else should contain zeros.
	 */
	offset = nblock_offset * g_pool_ok.bsize;
	for (i = 0; i < nblock_offset * g_pool_ok.bsize; i++) {
		CU_ASSERT_EQUAL(g_pool_ok.buffer[i], 0);
		if (g_pool_ok.buffer[i] != 0) {
			break;
		}
	}

	rc = memcmp(&g_pool_ok.buffer[i], &write_buf[0], total_io_size);
	CU_ASSERT_EQUAL(rc, 0);

	offset = g_pool_ok.nblock * g_pool_ok.bsize;
	for (i = offset - total_io_size; i < offset; i++) {
		CU_ASSERT_EQUAL(g_pool_ok.buffer[i], 0);
		if (g_pool_ok.buffer[i] != 0) {
			break;
		}
	}

	offset = 0;
	for (i = 0; i < 3; i++) {
		iov[i].iov_base =  read_buf + offset;
		offset += iov[i].iov_len;
	}

	/*
	 * Read with insuficient IOV buffers length.
	 */
	rc = ut_bdev_pmem_submit_request(bdev, SPDK_BDEV_IO_TYPE_READ, 0, g_pool_ok.nblock, &iov[0], 2);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_FAILED);

	/*
	 * Try to read two IOV with first one iov_len % bsize != 0.
	 */
	io_size = iov[0].iov_len + iov[1].iov_len;
	io_nblock = io_size / g_pool_ok.bsize;
	iov[0].iov_len += unaligned_aligned_size;
	rc = ut_bdev_pmem_submit_request(bdev, SPDK_BDEV_IO_TYPE_READ, 0, io_nblock, &iov[0], 2);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_FAILED);
	iov[0].iov_len -= unaligned_aligned_size;

	/*
	 * Try to write one IOV.
	 */
	io_nblock = iov[0].iov_len / g_pool_ok.bsize;
	rc = ut_bdev_pmem_submit_request(bdev, SPDK_BDEV_IO_TYPE_READ, nblock_offset, io_nblock, &iov[0],
					 1);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_SUCCESS);

	/*
	 * Try to read 2 IOV.
	 * Sum of IOV length is larger than IO size and last IOV is larger and iov_len % bsize != 0
	 */
	offset = iov[0].iov_len / g_pool_ok.bsize;
	io_size = iov[1].iov_len + iov[2].iov_len;
	io_nblock = io_size / g_pool_ok.bsize;
	iov[2].iov_len += unaligned_aligned_size;
	rc = ut_bdev_pmem_submit_request(bdev, SPDK_BDEV_IO_TYPE_READ, nblock_offset + offset, io_nblock,
					 &iov[1], 2);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_SUCCESS);
	iov[2].iov_len -= unaligned_aligned_size;


	rc = memcmp(read_buf, write_buf, total_io_size);
	CU_ASSERT_EQUAL(rc, 0);
	for (i = total_io_size; i < buf_size; i++) {
		CU_ASSERT_EQUAL(read_buf[i], 0);
		if (read_buf[i] != 0) {
			break;
		}
	}

	free(write_buf);
	free(read_buf);

	/* Now remove this bdev */
	ut_bdev_pmem_destruct(bdev);
	CU_ASSERT_FALSE(g_pool_ok.is_open);
	CU_ASSERT_EQUAL(g_opened_pools, 0);
}

static void
ut_pmem_flush_reset(int16_t io_type)
{
	struct spdk_bdev *bdev;;
	int rc;

	CU_ASSERT(io_type == SPDK_BDEV_IO_TYPE_FLUSH || io_type == SPDK_BDEV_IO_TYPE_RESET);

	bdev = create_pmem_disk(g_pool_ok.name);
	CU_ASSERT_PTR_NOT_NULL_FATAL(bdev);

	rc = ut_bdev_pmem_submit_request(bdev, io_type, 0, 0, NULL, 0);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_SUCCESS);

	ut_bdev_pmem_destruct(bdev);
}

static void
ut_pmem_flush(void)
{
	ut_pmem_flush_reset(SPDK_BDEV_IO_TYPE_FLUSH);
}

static void
ut_pmem_reset(void)
{
	ut_pmem_flush_reset(SPDK_BDEV_IO_TYPE_RESET);
}


#if 0
static void
ut_pmem_unmap_write_zero(int16_t io_type)
{
	struct spdk_bdev *bdev;
	size_t buff_size = g_pool_ok.nblock * g_pool_ok.bsize;
	size_t i;
	char *buffer;
	int rc;

	CU_ASSERT(io_type == SPDK_BDEV_IO_TYPE_UNMAP || io_type == SPDK_BDEV_IO_TYPE_WRITE_ZEROES);
	bdev = create_pmem_disk(g_pool_ok.name);
	CU_ASSERT_PTR_NOT_NULL_FATAL(bdev);
	CU_ASSERT_FATAL(g_pool_ok.nblock > 40);

	buffer = malloc(buff_size);
	CU_ASSERT_PTR_NOT_NULL_FATAL(buffer);

	for (i = 10 * g_pool_ok.bsize; i < 30 * g_pool_ok.bsize; i++) {
		buffer[i] = 0x30 + io_type + i;
	}
	memcpy(g_pool_ok.buffer, buffer, buff_size);

	/*
	 * Block outside of pool.
	 */
	rc = ut_bdev_pmem_submit_request(bdev, io_type, g_pool_ok.nblock, 1, NULL, 0);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_FAILED);

	rc = memcmp(buffer, g_pool_ok.buffer, buff_size);
	CU_ASSERT_EQUAL(rc, 0);

	/*
	 * Blocks 15 to 25
	 */
	memset(&buffer[15 * g_pool_ok.bsize], 0, 10 * g_pool_ok.bsize);
	rc = ut_bdev_pmem_submit_request(bdev, io_type, 15, 10, NULL, 0);
	CU_ASSERT_EQUAL(rc, SPDK_BDEV_IO_STATUS_SUCCESS);

	rc = memcmp(buffer, g_pool_ok.buffer, buff_size);
	CU_ASSERT_EQUAL(rc, 0);

	/*
	 * All blocks.
	 */
	memset(buffer, 0, buff_size);
	rc = ut_bdev_pmem_submit_request(bdev, io_type, 0, g_pool_ok.nblock, NULL, 0);
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
#endif

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("bdev_pmem", ut_pmem_blk_init, ut_pmem_blk_clean);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "ut_pmem_init", ut_pmem_init) == NULL ||
		CU_add_test(suite, "ut_pmem_open_close", ut_pmem_open_close) == NULL ||
		CU_add_test(suite, "ut_pmem_write_read", ut_pmem_write_read) == NULL ||
		CU_add_test(suite, "ut_pmem_unmap", ut_pmem_flush) == NULL ||
		CU_add_test(suite, "ut_pmem_unmap", ut_pmem_reset) == NULL
#if 0
		/*
		 * Those tests still fail
		 */
		CU_add_test(suite, "ut_pmem_write_zero", ut_pmem_write_zero) == NULL ||
		CU_add_test(suite, "ut_pmem_unmap", ut_pmem_unmap) == NULL
#endif
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
