/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_cunit.h"

#include "reduce/reduce.c"
#include "spdk_internal/mock.h"
#define UNIT_TEST_NO_VTOPHYS
#include "common/lib/test_env.c"
#undef UNIT_TEST_NO_VTOPHYS

static struct spdk_reduce_vol *g_vol;
static int g_reduce_errno;
static char *g_volatile_pm_buf;
static size_t g_volatile_pm_buf_len;
static char *g_persistent_pm_buf;
static size_t g_persistent_pm_buf_len;
static char *g_backing_dev_buf;
static char g_path[REDUCE_PATH_MAX];
static char *g_decomp_buf;
static int g_decompressed_len;

#define TEST_MD_PATH "/tmp"

uint64_t
spdk_vtophys(const void *buf, uint64_t *size)
{
	/* add + 1 to buf addr for cases where buf is the start of the page, that will give us correct end of the page */
	const uint8_t *page_2mb_end = (const uint8_t *)SPDK_ALIGN_CEIL((uintptr_t)buf + 1, VALUE_2MB);
	uint64_t bytes_to_page_end = page_2mb_end - (const uint8_t *)buf;
	uint64_t _size;

	if (*size) {
		_size = *size;
		_size = spdk_min(_size, bytes_to_page_end);
		*size = _size;
	}

	return (uintptr_t)buf;
}

enum ut_reduce_bdev_io_type {
	UT_REDUCE_IO_READV = 1,
	UT_REDUCE_IO_WRITEV = 2,
	UT_REDUCE_IO_UNMAP = 3,
};

struct ut_reduce_bdev_io {
	enum ut_reduce_bdev_io_type type;
	struct spdk_reduce_backing_dev *backing_dev;
	struct iovec *iov;
	int iovcnt;
	uint64_t lba;
	uint32_t lba_count;
	struct spdk_reduce_vol_cb_args *args;
	TAILQ_ENTRY(ut_reduce_bdev_io)	link;
};

static bool g_defer_bdev_io = false;
static TAILQ_HEAD(, ut_reduce_bdev_io) g_pending_bdev_io =
	TAILQ_HEAD_INITIALIZER(g_pending_bdev_io);
static uint32_t g_pending_bdev_io_count = 0;

static void
sync_pm_buf(const void *addr, size_t length)
{
	uint64_t offset = (char *)addr - g_volatile_pm_buf;

	memcpy(&g_persistent_pm_buf[offset], addr, length);
}

int
pmem_msync(const void *addr, size_t length)
{
	sync_pm_buf(addr, length);
	return 0;
}

void
pmem_persist(const void *addr, size_t len)
{
	sync_pm_buf(addr, len);
}

static void
get_pm_file_size(void)
{
	struct spdk_reduce_vol_params params;
	uint64_t pm_size, expected_pm_size;

	params.backing_io_unit_size = 4096;
	params.chunk_size = 4096 * 4;
	params.vol_size = 4096 * 4 * 100;

	pm_size = _get_pm_file_size(&params);
	expected_pm_size = sizeof(struct spdk_reduce_vol_superblock);
	/* 100 chunks in logical map * 8 bytes per chunk */
	expected_pm_size += 100 * sizeof(uint64_t);
	/* 100 chunks * (chunk struct size + 4 backing io units per chunk * 8 bytes per backing io unit) */
	expected_pm_size += 100 * (sizeof(struct spdk_reduce_chunk_map) + 4 * sizeof(uint64_t));
	/* reduce allocates some extra chunks too for in-flight writes when logical map
	 * is full.  REDUCE_EXTRA_CHUNKS is a private #ifdef in reduce.c Here we need the num chunks
	 * times (chunk struct size + 4 backing io units per chunk * 8 bytes per backing io unit).
	 */
	expected_pm_size += REDUCE_NUM_EXTRA_CHUNKS *
			    (sizeof(struct spdk_reduce_chunk_map) + 4 * sizeof(uint64_t));
	/* reduce will add some padding so numbers may not match exactly.  Make sure
	 * they are close though.
	 */
	CU_ASSERT((pm_size - expected_pm_size) <= REDUCE_PM_SIZE_ALIGNMENT);
}

static void
get_vol_size(void)
{
	uint64_t chunk_size, backing_dev_size;

	chunk_size = 16 * 1024;
	backing_dev_size = 16 * 1024 * 1000;
	CU_ASSERT(_get_vol_size(chunk_size, backing_dev_size) < backing_dev_size);
}

void *
pmem_map_file(const char *path, size_t len, int flags, mode_t mode,
	      size_t *mapped_lenp, int *is_pmemp)
{
	CU_ASSERT(g_volatile_pm_buf == NULL);
	snprintf(g_path, sizeof(g_path), "%s", path);
	*is_pmemp = 1;

	if (g_persistent_pm_buf == NULL) {
		g_persistent_pm_buf = calloc(1, len);
		g_persistent_pm_buf_len = len;
		SPDK_CU_ASSERT_FATAL(g_persistent_pm_buf != NULL);
	}

	*mapped_lenp = g_persistent_pm_buf_len;
	g_volatile_pm_buf = calloc(1, g_persistent_pm_buf_len);
	SPDK_CU_ASSERT_FATAL(g_volatile_pm_buf != NULL);
	memcpy(g_volatile_pm_buf, g_persistent_pm_buf, g_persistent_pm_buf_len);
	g_volatile_pm_buf_len = g_persistent_pm_buf_len;

	return g_volatile_pm_buf;
}

int
pmem_unmap(void *addr, size_t len)
{
	CU_ASSERT(addr == g_volatile_pm_buf);
	CU_ASSERT(len == g_volatile_pm_buf_len);
	free(g_volatile_pm_buf);
	g_volatile_pm_buf = NULL;
	g_volatile_pm_buf_len = 0;

	return 0;
}

static void
persistent_pm_buf_destroy(void)
{
	CU_ASSERT(g_persistent_pm_buf != NULL);
	free(g_persistent_pm_buf);
	g_persistent_pm_buf = NULL;
	g_persistent_pm_buf_len = 0;
}

static void
unlink_cb(void)
{
	persistent_pm_buf_destroy();
}

static void
init_cb(void *cb_arg, struct spdk_reduce_vol *vol, int reduce_errno)
{
	g_vol = vol;
	g_reduce_errno = reduce_errno;
}

static void
load_cb(void *cb_arg, struct spdk_reduce_vol *vol, int reduce_errno)
{
	g_vol = vol;
	g_reduce_errno = reduce_errno;
}

static void
unload_cb(void *cb_arg, int reduce_errno)
{
	g_reduce_errno = reduce_errno;
}

static void
init_failure(void)
{
	struct spdk_reduce_vol_params params = {};
	struct spdk_reduce_backing_dev backing_dev = {};

	backing_dev.blocklen = 512;
	/* This blockcnt is too small for a reduce vol - there needs to be
	 *  enough space for at least REDUCE_NUM_EXTRA_CHUNKS + 1 chunks.
	 */
	backing_dev.blockcnt = 20;

	params.vol_size = 0;
	params.chunk_size = 16 * 1024;
	params.backing_io_unit_size = backing_dev.blocklen;
	params.logical_block_size = 512;

	/* backing_dev has an invalid size.  This should fail. */
	g_vol = NULL;
	g_reduce_errno = 0;
	spdk_reduce_vol_init(&params, &backing_dev, TEST_MD_PATH, init_cb, NULL);
	CU_ASSERT(g_reduce_errno == -EINVAL);
	SPDK_CU_ASSERT_FATAL(g_vol == NULL);

	/* backing_dev now has valid size, but backing_dev still has null
	 *  function pointers.  This should fail.
	 */
	backing_dev.blockcnt = 20000;

	g_vol = NULL;
	g_reduce_errno = 0;
	spdk_reduce_vol_init(&params, &backing_dev, TEST_MD_PATH, init_cb, NULL);
	CU_ASSERT(g_reduce_errno == -EINVAL);
	SPDK_CU_ASSERT_FATAL(g_vol == NULL);
}

static void
backing_dev_readv_execute(struct spdk_reduce_backing_dev *backing_dev,
			  struct iovec *iov, int iovcnt,
			  uint64_t lba, uint32_t lba_count,
			  struct spdk_reduce_vol_cb_args *args)
{
	char *offset;
	int i;

	offset = g_backing_dev_buf + lba * backing_dev->blocklen;
	for (i = 0; i < iovcnt; i++) {
		memcpy(iov[i].iov_base, offset, iov[i].iov_len);
		offset += iov[i].iov_len;
	}
	args->cb_fn(args->cb_arg, 0);
}

static void
backing_dev_insert_io(enum ut_reduce_bdev_io_type type, struct spdk_reduce_backing_dev *backing_dev,
		      struct iovec *iov, int iovcnt, uint64_t lba, uint32_t lba_count,
		      struct spdk_reduce_vol_cb_args *args)
{
	struct ut_reduce_bdev_io *ut_bdev_io;

	ut_bdev_io = calloc(1, sizeof(*ut_bdev_io));
	SPDK_CU_ASSERT_FATAL(ut_bdev_io != NULL);

	ut_bdev_io->type = type;
	ut_bdev_io->backing_dev = backing_dev;
	ut_bdev_io->iov = iov;
	ut_bdev_io->iovcnt = iovcnt;
	ut_bdev_io->lba = lba;
	ut_bdev_io->lba_count = lba_count;
	ut_bdev_io->args = args;
	TAILQ_INSERT_TAIL(&g_pending_bdev_io, ut_bdev_io, link);
	g_pending_bdev_io_count++;
}

static void
backing_dev_readv(struct spdk_reduce_backing_dev *backing_dev, struct iovec *iov, int iovcnt,
		  uint64_t lba, uint32_t lba_count, struct spdk_reduce_vol_cb_args *args)
{
	if (g_defer_bdev_io == false) {
		CU_ASSERT(g_pending_bdev_io_count == 0);
		CU_ASSERT(TAILQ_EMPTY(&g_pending_bdev_io));
		backing_dev_readv_execute(backing_dev, iov, iovcnt, lba, lba_count, args);
		return;
	}

	backing_dev_insert_io(UT_REDUCE_IO_READV, backing_dev, iov, iovcnt, lba, lba_count, args);
}

static void
backing_dev_writev_execute(struct spdk_reduce_backing_dev *backing_dev,
			   struct iovec *iov, int iovcnt,
			   uint64_t lba, uint32_t lba_count,
			   struct spdk_reduce_vol_cb_args *args)
{
	char *offset;
	int i;

	offset = g_backing_dev_buf + lba * backing_dev->blocklen;
	for (i = 0; i < iovcnt; i++) {
		memcpy(offset, iov[i].iov_base, iov[i].iov_len);
		offset += iov[i].iov_len;
	}
	args->cb_fn(args->cb_arg, 0);
}

static void
backing_dev_writev(struct spdk_reduce_backing_dev *backing_dev, struct iovec *iov, int iovcnt,
		   uint64_t lba, uint32_t lba_count, struct spdk_reduce_vol_cb_args *args)
{
	if (g_defer_bdev_io == false) {
		CU_ASSERT(g_pending_bdev_io_count == 0);
		CU_ASSERT(TAILQ_EMPTY(&g_pending_bdev_io));
		backing_dev_writev_execute(backing_dev, iov, iovcnt, lba, lba_count, args);
		return;
	}

	backing_dev_insert_io(UT_REDUCE_IO_WRITEV, backing_dev, iov, iovcnt, lba, lba_count, args);
}

static void
backing_dev_unmap_execute(struct spdk_reduce_backing_dev *backing_dev,
			  uint64_t lba, uint32_t lba_count,
			  struct spdk_reduce_vol_cb_args *args)
{
	char *offset;

	offset = g_backing_dev_buf + lba * backing_dev->blocklen;
	memset(offset, 0, lba_count * backing_dev->blocklen);
	args->cb_fn(args->cb_arg, 0);
}

static void
backing_dev_unmap(struct spdk_reduce_backing_dev *backing_dev,
		  uint64_t lba, uint32_t lba_count, struct spdk_reduce_vol_cb_args *args)
{
	if (g_defer_bdev_io == false) {
		CU_ASSERT(g_pending_bdev_io_count == 0);
		CU_ASSERT(TAILQ_EMPTY(&g_pending_bdev_io));
		backing_dev_unmap_execute(backing_dev, lba, lba_count, args);
		return;
	}

	backing_dev_insert_io(UT_REDUCE_IO_UNMAP, backing_dev, NULL, 0, lba, lba_count, args);
}

static void
backing_dev_io_execute(uint32_t count)
{
	struct ut_reduce_bdev_io *ut_bdev_io;
	uint32_t done = 0;

	CU_ASSERT(g_defer_bdev_io == true);
	while (!TAILQ_EMPTY(&g_pending_bdev_io) && (count == 0 || done < count)) {
		ut_bdev_io = TAILQ_FIRST(&g_pending_bdev_io);
		TAILQ_REMOVE(&g_pending_bdev_io, ut_bdev_io, link);
		g_pending_bdev_io_count--;
		switch (ut_bdev_io->type) {
		case UT_REDUCE_IO_READV:
			backing_dev_readv_execute(ut_bdev_io->backing_dev,
						  ut_bdev_io->iov, ut_bdev_io->iovcnt,
						  ut_bdev_io->lba, ut_bdev_io->lba_count,
						  ut_bdev_io->args);
			break;
		case UT_REDUCE_IO_WRITEV:
			backing_dev_writev_execute(ut_bdev_io->backing_dev,
						   ut_bdev_io->iov, ut_bdev_io->iovcnt,
						   ut_bdev_io->lba, ut_bdev_io->lba_count,
						   ut_bdev_io->args);
			break;
		case UT_REDUCE_IO_UNMAP:
			backing_dev_unmap_execute(ut_bdev_io->backing_dev,
						  ut_bdev_io->lba, ut_bdev_io->lba_count,
						  ut_bdev_io->args);
			break;
		default:
			CU_ASSERT(false);
			break;
		}
		free(ut_bdev_io);
		done++;
	}
}

static int
ut_compress(char *outbuf, uint32_t *compressed_len, char *inbuf, uint32_t inbuflen)
{
	uint32_t len = 0;
	uint8_t count;
	char last;

	while (true) {
		if (inbuflen == 0) {
			*compressed_len = len;
			return 0;
		}

		if (*compressed_len < (len + 2)) {
			return -ENOSPC;
		}

		last = *inbuf;
		count = 1;
		inbuflen--;
		inbuf++;

		while (inbuflen > 0 && *inbuf == last && count < UINT8_MAX) {
			count++;
			inbuflen--;
			inbuf++;
		}

		outbuf[len] = count;
		outbuf[len + 1] = last;
		len += 2;
	}
}

static int
ut_decompress(uint8_t *outbuf, uint32_t *compressed_len, uint8_t *inbuf, uint32_t inbuflen)
{
	uint32_t len = 0;

	SPDK_CU_ASSERT_FATAL(inbuflen % 2 == 0);

	while (true) {
		if (inbuflen == 0) {
			*compressed_len = len;
			return 0;
		}

		if ((len + inbuf[0]) > *compressed_len) {
			return -ENOSPC;
		}

		memset(outbuf, inbuf[1], inbuf[0]);
		outbuf += inbuf[0];
		len += inbuf[0];
		inbuflen -= 2;
		inbuf += 2;
	}
}

static void
ut_build_data_buffer(uint8_t *data, uint32_t data_len, uint8_t init_val, uint32_t repeat)
{
	uint32_t _repeat = repeat;

	SPDK_CU_ASSERT_FATAL(repeat > 0);

	while (data_len > 0) {
		*data = init_val;
		data++;
		data_len--;
		_repeat--;
		if (_repeat == 0) {
			init_val++;
			_repeat = repeat;
		}
	}
}

static void
backing_dev_compress(struct spdk_reduce_backing_dev *backing_dev,
		     struct iovec *src_iov, int src_iovcnt,
		     struct iovec *dst_iov, int dst_iovcnt,
		     struct spdk_reduce_vol_cb_args *args)
{
	uint32_t compressed_len;
	uint64_t total_length = 0;
	char *buf = g_decomp_buf;
	int rc, i;

	CU_ASSERT(dst_iovcnt == 1);

	for (i = 0; i < src_iovcnt; i++) {
		memcpy(buf, src_iov[i].iov_base, src_iov[i].iov_len);
		buf += src_iov[i].iov_len;
		total_length += src_iov[i].iov_len;
	}

	compressed_len = dst_iov[0].iov_len;
	rc = ut_compress(dst_iov[0].iov_base, &compressed_len,
			 g_decomp_buf, total_length);

	args->output_size = compressed_len;

	args->cb_fn(args->cb_arg, rc);
}

static void
backing_dev_decompress(struct spdk_reduce_backing_dev *backing_dev,
		       struct iovec *src_iov, int src_iovcnt,
		       struct iovec *dst_iov, int dst_iovcnt,
		       struct spdk_reduce_vol_cb_args *args)
{
	uint32_t decompressed_len = 0;
	char *buf = g_decomp_buf;
	int rc, i;

	CU_ASSERT(src_iovcnt == 1);

	for (i = 0; i < dst_iovcnt; i++) {
		decompressed_len += dst_iov[i].iov_len;
	}

	rc = ut_decompress(g_decomp_buf, &decompressed_len,
			   src_iov[0].iov_base, src_iov[0].iov_len);

	for (i = 0; i < dst_iovcnt; i++) {
		memcpy(dst_iov[i].iov_base, buf, dst_iov[i].iov_len);
		buf += dst_iov[i].iov_len;
	}

	args->output_size = decompressed_len;

	args->cb_fn(args->cb_arg, rc);
}

static void
backing_dev_destroy(struct spdk_reduce_backing_dev *backing_dev)
{
	/* We don't free this during backing_dev_close so that we can test init/unload/load
	 *  scenarios.
	 */
	free(g_backing_dev_buf);
	free(g_decomp_buf);
	g_backing_dev_buf = NULL;
}

static void
backing_dev_init(struct spdk_reduce_backing_dev *backing_dev, struct spdk_reduce_vol_params *params,
		 uint32_t backing_blocklen)
{
	int64_t size;

	size = 4 * 1024 * 1024;
	backing_dev->blocklen = backing_blocklen;
	backing_dev->blockcnt = size / backing_dev->blocklen;
	backing_dev->readv = backing_dev_readv;
	backing_dev->writev = backing_dev_writev;
	backing_dev->unmap = backing_dev_unmap;
	backing_dev->compress = backing_dev_compress;
	backing_dev->decompress = backing_dev_decompress;
	backing_dev->sgl_in = true;
	backing_dev->sgl_out = true;

	g_decomp_buf = calloc(1, params->chunk_size);
	SPDK_CU_ASSERT_FATAL(g_decomp_buf != NULL);

	g_backing_dev_buf = calloc(1, size);
	SPDK_CU_ASSERT_FATAL(g_backing_dev_buf != NULL);
}

static void
init_md(void)
{
	struct spdk_reduce_vol_params params = {};
	struct spdk_reduce_vol_params *persistent_params;
	struct spdk_reduce_backing_dev backing_dev = {};
	struct spdk_uuid uuid;
	uint64_t *entry;

	params.chunk_size = 16 * 1024;
	params.backing_io_unit_size = 512;
	params.logical_block_size = 512;

	backing_dev_init(&backing_dev, &params, 512);

	g_vol = NULL;
	g_reduce_errno = -1;
	spdk_reduce_vol_init(&params, &backing_dev, TEST_MD_PATH, init_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	SPDK_CU_ASSERT_FATAL(g_vol != NULL);
	/* Confirm that reduce persisted the params to metadata. */
	CU_ASSERT(memcmp(g_persistent_pm_buf, SPDK_REDUCE_SIGNATURE, 8) == 0);
	persistent_params = (struct spdk_reduce_vol_params *)(g_persistent_pm_buf + 8);
	CU_ASSERT(memcmp(persistent_params, &params, sizeof(params)) == 0);
	/* Now confirm that contents of pm_file after the superblock have been initialized
	 *  to REDUCE_EMPTY_MAP_ENTRY.
	 */
	entry = (uint64_t *)(g_persistent_pm_buf + sizeof(struct spdk_reduce_vol_superblock));
	while (entry != (uint64_t *)(g_persistent_pm_buf + g_vol->pm_file.size)) {
		CU_ASSERT(*entry == REDUCE_EMPTY_MAP_ENTRY);
		entry++;
	}

	/* Check that the pm file path was constructed correctly.  It should be in
	 * the form:
	 * TEST_MD_PATH + "/" + <uuid string>
	 */
	CU_ASSERT(strncmp(&g_path[0], TEST_MD_PATH, strlen(TEST_MD_PATH)) == 0);
	CU_ASSERT(g_path[strlen(TEST_MD_PATH)] == '/');
	CU_ASSERT(spdk_uuid_parse(&uuid, &g_path[strlen(TEST_MD_PATH) + 1]) == 0);
	CU_ASSERT(spdk_uuid_compare(&uuid, spdk_reduce_vol_get_uuid(g_vol)) == 0);

	g_reduce_errno = -1;
	spdk_reduce_vol_unload(g_vol, unload_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	CU_ASSERT(g_volatile_pm_buf == NULL);

	persistent_pm_buf_destroy();
	backing_dev_destroy(&backing_dev);
}

static void
_init_backing_dev(uint32_t backing_blocklen)
{
	struct spdk_reduce_vol_params params = {};
	struct spdk_reduce_vol_params *persistent_params;
	struct spdk_reduce_backing_dev backing_dev = {};

	params.chunk_size = 16 * 1024;
	params.backing_io_unit_size = 512;
	params.logical_block_size = 512;
	spdk_uuid_generate(&params.uuid);

	backing_dev_init(&backing_dev, &params, backing_blocklen);

	g_vol = NULL;
	memset(g_path, 0, sizeof(g_path));
	g_reduce_errno = -1;
	spdk_reduce_vol_init(&params, &backing_dev, TEST_MD_PATH, init_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	SPDK_CU_ASSERT_FATAL(g_vol != NULL);
	CU_ASSERT(strncmp(TEST_MD_PATH, g_path, strlen(TEST_MD_PATH)) == 0);
	/* Confirm that libreduce persisted the params to the backing device. */
	CU_ASSERT(memcmp(g_backing_dev_buf, SPDK_REDUCE_SIGNATURE, 8) == 0);
	persistent_params = (struct spdk_reduce_vol_params *)(g_backing_dev_buf + 8);
	CU_ASSERT(memcmp(persistent_params, &params, sizeof(params)) == 0);
	/* Confirm that the path to the persistent memory metadata file was persisted to
	 *  the backing device.
	 */
	CU_ASSERT(strncmp(g_path,
			  g_backing_dev_buf + REDUCE_BACKING_DEV_PATH_OFFSET,
			  REDUCE_PATH_MAX) == 0);

	g_reduce_errno = -1;
	spdk_reduce_vol_unload(g_vol, unload_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	persistent_pm_buf_destroy();
	backing_dev_destroy(&backing_dev);
}

static void
init_backing_dev(void)
{
	_init_backing_dev(512);
	_init_backing_dev(4096);
}

static void
_load(uint32_t backing_blocklen)
{
	struct spdk_reduce_vol_params params = {};
	struct spdk_reduce_backing_dev backing_dev = {};
	char pmem_file_path[REDUCE_PATH_MAX];

	params.chunk_size = 16 * 1024;
	params.backing_io_unit_size = 512;
	params.logical_block_size = 512;
	spdk_uuid_generate(&params.uuid);

	backing_dev_init(&backing_dev, &params, backing_blocklen);

	g_vol = NULL;
	g_reduce_errno = -1;
	spdk_reduce_vol_init(&params, &backing_dev, TEST_MD_PATH, init_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	SPDK_CU_ASSERT_FATAL(g_vol != NULL);
	CU_ASSERT(strncmp(TEST_MD_PATH, g_path, strlen(TEST_MD_PATH)) == 0);
	memcpy(pmem_file_path, g_path, sizeof(pmem_file_path));

	g_reduce_errno = -1;
	spdk_reduce_vol_unload(g_vol, unload_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	g_vol = NULL;
	memset(g_path, 0, sizeof(g_path));
	g_reduce_errno = -1;
	spdk_reduce_vol_load(&backing_dev, load_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	SPDK_CU_ASSERT_FATAL(g_vol != NULL);
	CU_ASSERT(strncmp(g_path, pmem_file_path, sizeof(pmem_file_path)) == 0);
	CU_ASSERT(g_vol->params.vol_size == params.vol_size);
	CU_ASSERT(g_vol->params.chunk_size == params.chunk_size);
	CU_ASSERT(g_vol->params.backing_io_unit_size == params.backing_io_unit_size);

	g_reduce_errno = -1;
	spdk_reduce_vol_unload(g_vol, unload_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	persistent_pm_buf_destroy();
	backing_dev_destroy(&backing_dev);
}

static void
load(void)
{
	_load(512);
	_load(4096);
}

static uint64_t
_vol_get_chunk_map_index(struct spdk_reduce_vol *vol, uint64_t offset)
{
	uint64_t logical_map_index = offset / vol->logical_blocks_per_chunk;

	return vol->pm_logical_map[logical_map_index];
}

static void
write_cb(void *arg, int reduce_errno)
{
	g_reduce_errno = reduce_errno;
}

static void
read_cb(void *arg, int reduce_errno)
{
	g_reduce_errno = reduce_errno;
}

static void
_write_maps(uint32_t backing_blocklen)
{
	struct spdk_reduce_vol_params params = {};
	struct spdk_reduce_backing_dev backing_dev = {};
	struct iovec iov;
	const int bufsize = 16 * 1024; /* chunk size */
	char buf[bufsize];
	uint32_t num_lbas, i;
	uint64_t old_chunk0_map_index, new_chunk0_map_index;
	struct spdk_reduce_chunk_map *old_chunk0_map, *new_chunk0_map;

	params.chunk_size = bufsize;
	params.backing_io_unit_size = 4096;
	params.logical_block_size = 512;
	num_lbas = bufsize / params.logical_block_size;
	spdk_uuid_generate(&params.uuid);

	backing_dev_init(&backing_dev, &params, backing_blocklen);

	g_vol = NULL;
	g_reduce_errno = -1;
	spdk_reduce_vol_init(&params, &backing_dev, TEST_MD_PATH, init_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	SPDK_CU_ASSERT_FATAL(g_vol != NULL);

	for (i = 0; i < g_vol->params.vol_size / g_vol->params.chunk_size; i++) {
		CU_ASSERT(_vol_get_chunk_map_index(g_vol, i) == REDUCE_EMPTY_MAP_ENTRY);
	}

	ut_build_data_buffer(buf, bufsize, 0x00, 1);
	iov.iov_base = buf;
	iov.iov_len = bufsize;
	g_reduce_errno = -1;
	spdk_reduce_vol_writev(g_vol, &iov, 1, 0, num_lbas, write_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	old_chunk0_map_index = _vol_get_chunk_map_index(g_vol, 0);
	CU_ASSERT(old_chunk0_map_index != REDUCE_EMPTY_MAP_ENTRY);
	CU_ASSERT(spdk_bit_array_get(g_vol->allocated_chunk_maps, old_chunk0_map_index) == true);

	old_chunk0_map = _reduce_vol_get_chunk_map(g_vol, old_chunk0_map_index);
	for (i = 0; i < g_vol->backing_io_units_per_chunk; i++) {
		CU_ASSERT(old_chunk0_map->io_unit_index[i] != REDUCE_EMPTY_MAP_ENTRY);
		CU_ASSERT(spdk_bit_array_get(g_vol->allocated_backing_io_units,
					     old_chunk0_map->io_unit_index[i]) == true);
	}

	g_reduce_errno = -1;
	spdk_reduce_vol_writev(g_vol, &iov, 1, 0, num_lbas, write_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	new_chunk0_map_index = _vol_get_chunk_map_index(g_vol, 0);
	CU_ASSERT(new_chunk0_map_index != REDUCE_EMPTY_MAP_ENTRY);
	CU_ASSERT(new_chunk0_map_index != old_chunk0_map_index);
	CU_ASSERT(spdk_bit_array_get(g_vol->allocated_chunk_maps, new_chunk0_map_index) == true);
	CU_ASSERT(spdk_bit_array_get(g_vol->allocated_chunk_maps, old_chunk0_map_index) == false);

	for (i = 0; i < g_vol->backing_io_units_per_chunk; i++) {
		CU_ASSERT(spdk_bit_array_get(g_vol->allocated_backing_io_units,
					     old_chunk0_map->io_unit_index[i]) == false);
	}

	new_chunk0_map = _reduce_vol_get_chunk_map(g_vol, new_chunk0_map_index);
	for (i = 0; i < g_vol->backing_io_units_per_chunk; i++) {
		CU_ASSERT(new_chunk0_map->io_unit_index[i] != REDUCE_EMPTY_MAP_ENTRY);
		CU_ASSERT(spdk_bit_array_get(g_vol->allocated_backing_io_units,
					     new_chunk0_map->io_unit_index[i]) == true);
	}

	g_reduce_errno = -1;
	spdk_reduce_vol_unload(g_vol, unload_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	g_vol = NULL;
	g_reduce_errno = -1;
	spdk_reduce_vol_load(&backing_dev, load_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	SPDK_CU_ASSERT_FATAL(g_vol != NULL);
	CU_ASSERT(g_vol->params.vol_size == params.vol_size);
	CU_ASSERT(g_vol->params.chunk_size == params.chunk_size);
	CU_ASSERT(g_vol->params.backing_io_unit_size == params.backing_io_unit_size);

	g_reduce_errno = -1;
	spdk_reduce_vol_unload(g_vol, unload_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	persistent_pm_buf_destroy();
	backing_dev_destroy(&backing_dev);
}

static void
write_maps(void)
{
	_write_maps(512);
	_write_maps(4096);
}

static void
_read_write(uint32_t backing_blocklen)
{
	struct spdk_reduce_vol_params params = {};
	struct spdk_reduce_backing_dev backing_dev = {};
	struct iovec iov;
	char buf[16 * 1024]; /* chunk size */
	char compare_buf[16 * 1024];
	uint32_t i;

	params.chunk_size = 16 * 1024;
	params.backing_io_unit_size = 4096;
	params.logical_block_size = 512;
	spdk_uuid_generate(&params.uuid);

	backing_dev_init(&backing_dev, &params, backing_blocklen);

	g_vol = NULL;
	g_reduce_errno = -1;
	spdk_reduce_vol_init(&params, &backing_dev, TEST_MD_PATH, init_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	SPDK_CU_ASSERT_FATAL(g_vol != NULL);

	/* Write 0xAA to 2 512-byte logical blocks, starting at LBA 2. */
	memset(buf, 0xAA, 2 * params.logical_block_size);
	iov.iov_base = buf;
	iov.iov_len = 2 * params.logical_block_size;
	g_reduce_errno = -1;
	spdk_reduce_vol_writev(g_vol, &iov, 1, 2, 2, write_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	memset(compare_buf, 0xAA, sizeof(compare_buf));
	for (i = 0; i < params.chunk_size / params.logical_block_size; i++) {
		memset(buf, 0xFF, params.logical_block_size);
		iov.iov_base = buf;
		iov.iov_len = params.logical_block_size;
		g_reduce_errno = -1;
		spdk_reduce_vol_readv(g_vol, &iov, 1, i, 1, read_cb, NULL);
		CU_ASSERT(g_reduce_errno == 0);

		switch (i) {
		case 2:
		case 3:
			CU_ASSERT(memcmp(buf, compare_buf, params.logical_block_size) == 0);
			break;
		default:
			CU_ASSERT(spdk_mem_all_zero(buf, params.logical_block_size));
			break;
		}
	}

	g_reduce_errno = -1;
	spdk_reduce_vol_unload(g_vol, unload_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	/* Overwrite what we just wrote with 0xCC */
	g_vol = NULL;
	g_reduce_errno = -1;
	spdk_reduce_vol_load(&backing_dev, load_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	SPDK_CU_ASSERT_FATAL(g_vol != NULL);
	CU_ASSERT(g_vol->params.vol_size == params.vol_size);
	CU_ASSERT(g_vol->params.chunk_size == params.chunk_size);
	CU_ASSERT(g_vol->params.backing_io_unit_size == params.backing_io_unit_size);

	memset(buf, 0xCC, 2 * params.logical_block_size);
	iov.iov_base = buf;
	iov.iov_len = 2 * params.logical_block_size;
	g_reduce_errno = -1;
	spdk_reduce_vol_writev(g_vol, &iov, 1, 2, 2, write_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	memset(compare_buf, 0xCC, sizeof(compare_buf));
	for (i = 0; i < params.chunk_size / params.logical_block_size; i++) {
		memset(buf, 0xFF, params.logical_block_size);
		iov.iov_base = buf;
		iov.iov_len = params.logical_block_size;
		g_reduce_errno = -1;
		spdk_reduce_vol_readv(g_vol, &iov, 1, i, 1, read_cb, NULL);
		CU_ASSERT(g_reduce_errno == 0);

		switch (i) {
		case 2:
		case 3:
			CU_ASSERT(memcmp(buf, compare_buf, params.logical_block_size) == 0);
			break;
		default:
			CU_ASSERT(spdk_mem_all_zero(buf, params.logical_block_size));
			break;
		}
	}

	g_reduce_errno = -1;
	spdk_reduce_vol_unload(g_vol, unload_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	g_vol = NULL;
	g_reduce_errno = -1;
	spdk_reduce_vol_load(&backing_dev, load_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	SPDK_CU_ASSERT_FATAL(g_vol != NULL);
	CU_ASSERT(g_vol->params.vol_size == params.vol_size);
	CU_ASSERT(g_vol->params.chunk_size == params.chunk_size);
	CU_ASSERT(g_vol->params.backing_io_unit_size == params.backing_io_unit_size);

	g_reduce_errno = -1;

	/* Write 0xBB to 2 512-byte logical blocks, starting at LBA 37.
	 * This is writing into the second chunk of the volume.  This also
	 * enables implicitly checking that we reloaded the bit arrays
	 * correctly - making sure we don't use the first chunk map again
	 * for this new write - the first chunk map was already used by the
	 * write from before we unloaded and reloaded.
	 */
	memset(buf, 0xBB, 2 * params.logical_block_size);
	iov.iov_base = buf;
	iov.iov_len = 2 * params.logical_block_size;
	g_reduce_errno = -1;
	spdk_reduce_vol_writev(g_vol, &iov, 1, 37, 2, write_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	for (i = 0; i < 2 * params.chunk_size / params.logical_block_size; i++) {
		memset(buf, 0xFF, params.logical_block_size);
		iov.iov_base = buf;
		iov.iov_len = params.logical_block_size;
		g_reduce_errno = -1;
		spdk_reduce_vol_readv(g_vol, &iov, 1, i, 1, read_cb, NULL);
		CU_ASSERT(g_reduce_errno == 0);

		switch (i) {
		case 2:
		case 3:
			memset(compare_buf, 0xCC, sizeof(compare_buf));
			CU_ASSERT(memcmp(buf, compare_buf, params.logical_block_size) == 0);
			break;
		case 37:
		case 38:
			memset(compare_buf, 0xBB, sizeof(compare_buf));
			CU_ASSERT(memcmp(buf, compare_buf, params.logical_block_size) == 0);
			break;
		default:
			CU_ASSERT(spdk_mem_all_zero(buf, params.logical_block_size));
			break;
		}
	}

	g_reduce_errno = -1;
	spdk_reduce_vol_unload(g_vol, unload_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	persistent_pm_buf_destroy();
	backing_dev_destroy(&backing_dev);
}

static void
read_write(void)
{
	_read_write(512);
	_read_write(4096);
}

static void
_readv_writev(uint32_t backing_blocklen)
{
	struct spdk_reduce_vol_params params = {};
	struct spdk_reduce_backing_dev backing_dev = {};
	struct iovec iov[REDUCE_MAX_IOVECS + 1];

	params.chunk_size = 16 * 1024;
	params.backing_io_unit_size = 4096;
	params.logical_block_size = 512;
	spdk_uuid_generate(&params.uuid);

	backing_dev_init(&backing_dev, &params, backing_blocklen);

	g_vol = NULL;
	g_reduce_errno = -1;
	spdk_reduce_vol_init(&params, &backing_dev, TEST_MD_PATH, init_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	SPDK_CU_ASSERT_FATAL(g_vol != NULL);

	g_reduce_errno = -1;
	spdk_reduce_vol_writev(g_vol, iov, REDUCE_MAX_IOVECS + 1, 2, REDUCE_MAX_IOVECS + 1, write_cb, NULL);
	CU_ASSERT(g_reduce_errno == -EINVAL);

	g_reduce_errno = -1;
	spdk_reduce_vol_unload(g_vol, unload_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	persistent_pm_buf_destroy();
	backing_dev_destroy(&backing_dev);
}

static void
readv_writev(void)
{
	_readv_writev(512);
	_readv_writev(4096);
}

static void
destroy_cb(void *ctx, int reduce_errno)
{
	g_reduce_errno = reduce_errno;
}

static void
destroy(void)
{
	struct spdk_reduce_vol_params params = {};
	struct spdk_reduce_backing_dev backing_dev = {};

	params.chunk_size = 16 * 1024;
	params.backing_io_unit_size = 512;
	params.logical_block_size = 512;
	spdk_uuid_generate(&params.uuid);

	backing_dev_init(&backing_dev, &params, 512);

	g_vol = NULL;
	g_reduce_errno = -1;
	spdk_reduce_vol_init(&params, &backing_dev, TEST_MD_PATH, init_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	SPDK_CU_ASSERT_FATAL(g_vol != NULL);

	g_reduce_errno = -1;
	spdk_reduce_vol_unload(g_vol, unload_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	g_vol = NULL;
	g_reduce_errno = -1;
	spdk_reduce_vol_load(&backing_dev, load_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	SPDK_CU_ASSERT_FATAL(g_vol != NULL);

	g_reduce_errno = -1;
	spdk_reduce_vol_unload(g_vol, unload_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	g_reduce_errno = -1;
	MOCK_CLEAR(spdk_malloc);
	MOCK_CLEAR(spdk_zmalloc);
	spdk_reduce_vol_destroy(&backing_dev, destroy_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	g_reduce_errno = 0;
	spdk_reduce_vol_load(&backing_dev, load_cb, NULL);
	CU_ASSERT(g_reduce_errno == -EILSEQ);

	backing_dev_destroy(&backing_dev);
}

/* This test primarily checks that the reduce unit test infrastructure for asynchronous
 * backing device I/O operations is working correctly.
 */
static void
defer_bdev_io(void)
{
	struct spdk_reduce_vol_params params = {};
	struct spdk_reduce_backing_dev backing_dev = {};
	const uint32_t logical_block_size = 512;
	struct iovec iov;
	char buf[logical_block_size];
	char compare_buf[logical_block_size];

	params.chunk_size = 16 * 1024;
	params.backing_io_unit_size = 4096;
	params.logical_block_size = logical_block_size;
	spdk_uuid_generate(&params.uuid);

	backing_dev_init(&backing_dev, &params, 512);

	g_vol = NULL;
	g_reduce_errno = -1;
	spdk_reduce_vol_init(&params, &backing_dev, TEST_MD_PATH, init_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	SPDK_CU_ASSERT_FATAL(g_vol != NULL);

	/* Write 0xAA to 1 512-byte logical block. */
	memset(buf, 0xAA, params.logical_block_size);
	iov.iov_base = buf;
	iov.iov_len = params.logical_block_size;
	g_reduce_errno = -100;
	g_defer_bdev_io = true;
	spdk_reduce_vol_writev(g_vol, &iov, 1, 0, 1, write_cb, NULL);
	/* Callback should not have executed, so this should still equal -100. */
	CU_ASSERT(g_reduce_errno == -100);
	CU_ASSERT(!TAILQ_EMPTY(&g_pending_bdev_io));
	/* We wrote to just 512 bytes of one chunk which was previously unallocated.  This
	 * should result in 1 pending I/O since the rest of this chunk will be zeroes and
	 * very compressible.
	 */
	CU_ASSERT(g_pending_bdev_io_count == 1);

	backing_dev_io_execute(0);
	CU_ASSERT(TAILQ_EMPTY(&g_pending_bdev_io));
	CU_ASSERT(g_reduce_errno == 0);

	g_defer_bdev_io = false;
	memset(compare_buf, 0xAA, sizeof(compare_buf));
	memset(buf, 0xFF, sizeof(buf));
	iov.iov_base = buf;
	iov.iov_len = params.logical_block_size;
	g_reduce_errno = -100;
	spdk_reduce_vol_readv(g_vol, &iov, 1, 0, 1, read_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	CU_ASSERT(memcmp(buf, compare_buf, sizeof(buf)) == 0);

	g_reduce_errno = -1;
	spdk_reduce_vol_unload(g_vol, unload_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	persistent_pm_buf_destroy();
	backing_dev_destroy(&backing_dev);
}

static void
overlapped(void)
{
	struct spdk_reduce_vol_params params = {};
	struct spdk_reduce_backing_dev backing_dev = {};
	const uint32_t logical_block_size = 512;
	struct iovec iov;
	char buf[2 * logical_block_size];
	char compare_buf[2 * logical_block_size];

	params.chunk_size = 16 * 1024;
	params.backing_io_unit_size = 4096;
	params.logical_block_size = logical_block_size;
	spdk_uuid_generate(&params.uuid);

	backing_dev_init(&backing_dev, &params, 512);

	g_vol = NULL;
	g_reduce_errno = -1;
	spdk_reduce_vol_init(&params, &backing_dev, TEST_MD_PATH, init_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	SPDK_CU_ASSERT_FATAL(g_vol != NULL);

	/* Write 0xAA to 1 512-byte logical block. */
	memset(buf, 0xAA, logical_block_size);
	iov.iov_base = buf;
	iov.iov_len = logical_block_size;
	g_reduce_errno = -100;
	g_defer_bdev_io = true;
	spdk_reduce_vol_writev(g_vol, &iov, 1, 0, 1, write_cb, NULL);
	/* Callback should not have executed, so this should still equal -100. */
	CU_ASSERT(g_reduce_errno == -100);
	CU_ASSERT(!TAILQ_EMPTY(&g_pending_bdev_io));
	/* We wrote to just 512 bytes of one chunk which was previously unallocated.  This
	 * should result in 1 pending I/O since the rest of this chunk will be zeroes and
	 * very compressible.
	 */
	CU_ASSERT(g_pending_bdev_io_count == 1);

	/* Now do an overlapped I/O to the same chunk. */
	spdk_reduce_vol_writev(g_vol, &iov, 1, 1, 1, write_cb, NULL);
	/* Callback should not have executed, so this should still equal -100. */
	CU_ASSERT(g_reduce_errno == -100);
	CU_ASSERT(!TAILQ_EMPTY(&g_pending_bdev_io));
	/* The second I/O overlaps with the first one.  So we should only see pending bdev_io
	 * related to the first I/O here - the second one won't start until the first one is completed.
	 */
	CU_ASSERT(g_pending_bdev_io_count == 1);

	backing_dev_io_execute(0);
	CU_ASSERT(g_reduce_errno == 0);

	g_defer_bdev_io = false;
	memset(compare_buf, 0xAA, sizeof(compare_buf));
	memset(buf, 0xFF, sizeof(buf));
	iov.iov_base = buf;
	iov.iov_len = 2 * logical_block_size;
	g_reduce_errno = -100;
	spdk_reduce_vol_readv(g_vol, &iov, 1, 0, 2, read_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	CU_ASSERT(memcmp(buf, compare_buf, 2 * logical_block_size) == 0);

	g_reduce_errno = -1;
	spdk_reduce_vol_unload(g_vol, unload_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	persistent_pm_buf_destroy();
	backing_dev_destroy(&backing_dev);
}

#define BUFSIZE 4096

static void
compress_algorithm(void)
{
	uint8_t original_data[BUFSIZE];
	uint8_t compressed_data[BUFSIZE];
	uint8_t decompressed_data[BUFSIZE];
	uint32_t compressed_len, decompressed_len;
	int rc;

	ut_build_data_buffer(original_data, BUFSIZE, 0xAA, BUFSIZE);
	compressed_len = sizeof(compressed_data);
	rc = ut_compress(compressed_data, &compressed_len, original_data, UINT8_MAX);
	CU_ASSERT(rc == 0);
	CU_ASSERT(compressed_len == 2);
	CU_ASSERT(compressed_data[0] == UINT8_MAX);
	CU_ASSERT(compressed_data[1] == 0xAA);

	decompressed_len = sizeof(decompressed_data);
	rc = ut_decompress(decompressed_data, &decompressed_len, compressed_data, compressed_len);
	CU_ASSERT(rc == 0);
	CU_ASSERT(decompressed_len == UINT8_MAX);
	CU_ASSERT(memcmp(original_data, decompressed_data, decompressed_len) == 0);

	compressed_len = sizeof(compressed_data);
	rc = ut_compress(compressed_data, &compressed_len, original_data, UINT8_MAX + 1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(compressed_len == 4);
	CU_ASSERT(compressed_data[0] == UINT8_MAX);
	CU_ASSERT(compressed_data[1] == 0xAA);
	CU_ASSERT(compressed_data[2] == 1);
	CU_ASSERT(compressed_data[3] == 0xAA);

	decompressed_len = sizeof(decompressed_data);
	rc = ut_decompress(decompressed_data, &decompressed_len, compressed_data, compressed_len);
	CU_ASSERT(rc == 0);
	CU_ASSERT(decompressed_len == UINT8_MAX + 1);
	CU_ASSERT(memcmp(original_data, decompressed_data, decompressed_len) == 0);

	ut_build_data_buffer(original_data, BUFSIZE, 0x00, 1);
	compressed_len = sizeof(compressed_data);
	rc = ut_compress(compressed_data, &compressed_len, original_data, 2048);
	CU_ASSERT(rc == 0);
	CU_ASSERT(compressed_len == 4096);
	CU_ASSERT(compressed_data[0] == 1);
	CU_ASSERT(compressed_data[1] == 0);
	CU_ASSERT(compressed_data[4094] == 1);
	CU_ASSERT(compressed_data[4095] == 0xFF);

	decompressed_len = sizeof(decompressed_data);
	rc = ut_decompress(decompressed_data, &decompressed_len, compressed_data, compressed_len);
	CU_ASSERT(rc == 0);
	CU_ASSERT(decompressed_len == 2048);
	CU_ASSERT(memcmp(original_data, decompressed_data, decompressed_len) == 0);

	compressed_len = sizeof(compressed_data);
	rc = ut_compress(compressed_data, &compressed_len, original_data, 2049);
	CU_ASSERT(rc == -ENOSPC);
}

static void
test_prepare_compress_chunk(void)
{
	struct spdk_reduce_vol vol = {};
	struct spdk_reduce_backing_dev backing_dev = {};
	struct spdk_reduce_vol_request req = {};
	void *buf;
	char *buffer_end, *aligned_user_buffer, *unaligned_user_buffer;
	char decomp_buffer[16 * 1024] = {};
	char comp_buffer[16 * 1024] = {};
	struct iovec user_iov[2] = {};
	size_t user_buffer_iov_len = 8192;
	size_t remainder_bytes;
	size_t offset_bytes;
	size_t memcmp_offset;
	uint32_t i;

	vol.params.chunk_size = 16 * 1024;
	vol.params.backing_io_unit_size = 4096;
	vol.params.logical_block_size = 512;
	backing_dev_init(&backing_dev, &vol.params, 512);
	vol.backing_dev = &backing_dev;
	vol.logical_blocks_per_chunk = vol.params.chunk_size / vol.params.logical_block_size;

	/* Allocate 1 extra byte to test a case when buffer crosses huge page boundary */
	SPDK_CU_ASSERT_FATAL(posix_memalign(&buf, VALUE_2MB, VALUE_2MB + 1) == 0);
	buffer_end = (char *)buf + VALUE_2MB + 1;
	aligned_user_buffer = (char *)buf;
	memset(aligned_user_buffer, 0xc, vol.params.chunk_size);
	unaligned_user_buffer = buffer_end - vol.params.chunk_size;
	memset(unaligned_user_buffer, 0xc, vol.params.chunk_size);

	req.vol = &vol;
	req.decomp_buf = decomp_buffer;
	req.comp_buf = comp_buffer;
	req.iov = user_iov;
	req.iovcnt = 2;
	req.offset = 0;

	/* Part 1 - backing dev supports sgl_in */
	/* Test 1 - user's buffers length equals to chunk_size */
	for (i = 0; i < 2; i++) {
		req.iov[i].iov_base = aligned_user_buffer + i * user_buffer_iov_len;
		req.iov[i].iov_len = user_buffer_iov_len;
	}

	_prepare_compress_chunk(&req, false);
	CU_ASSERT(req.decomp_iovcnt == 2);
	for (i = 0; i < 2; i++) {
		CU_ASSERT(req.decomp_iov[i].iov_base == req.iov[i].iov_base);
		CU_ASSERT(req.decomp_iov[i].iov_len == req.iov[i].iov_len);
	}

	_prepare_compress_chunk(&req, true);
	CU_ASSERT(req.decomp_iovcnt == 2);
	for (i = 0; i < 2; i++) {
		CU_ASSERT(req.decomp_iov[i].iov_base == req.iov[i].iov_base);
		CU_ASSERT(req.decomp_iov[i].iov_len == req.iov[i].iov_len);
	}

	/* Test 2 - user's buffer less than chunk_size, without offset */
	user_buffer_iov_len = 4096;
	remainder_bytes = vol.params.chunk_size - user_buffer_iov_len * 2;
	for (i = 0; i < 2; i++) {
		req.iov[i].iov_base = aligned_user_buffer + i * user_buffer_iov_len;
		req.iov[i].iov_len = user_buffer_iov_len;
	}

	_prepare_compress_chunk(&req, false);
	CU_ASSERT(req.decomp_iovcnt == 3);
	for (i = 0; i < 2; i++) {
		CU_ASSERT(req.decomp_iov[i].iov_base == req.iov[i].iov_base);
		CU_ASSERT(req.decomp_iov[i].iov_len == req.iov[i].iov_len);
	}
	CU_ASSERT(req.decomp_iov[i].iov_base == req.decomp_buf + user_buffer_iov_len * 2);
	CU_ASSERT(req.decomp_iov[i].iov_len == remainder_bytes);

	_prepare_compress_chunk(&req, true);
	CU_ASSERT(req.decomp_iovcnt == 3);
	for (i = 0; i < 2; i++) {
		CU_ASSERT(req.decomp_iov[i].iov_base == req.iov[i].iov_base);
		CU_ASSERT(req.decomp_iov[i].iov_len == req.iov[i].iov_len);
	}
	CU_ASSERT(req.decomp_iov[i].iov_base == g_zero_buf + user_buffer_iov_len * 2);
	CU_ASSERT(req.decomp_iov[i].iov_len == remainder_bytes);

	/* Test 3 - user's buffer less than chunk_size, non zero offset */
	user_buffer_iov_len = 4096;
	req.offset = 3;
	offset_bytes = req.offset * vol.params.logical_block_size;
	remainder_bytes = vol.params.chunk_size - offset_bytes - user_buffer_iov_len * 2;

	_prepare_compress_chunk(&req, false);
	CU_ASSERT(req.decomp_iovcnt == 4);
	CU_ASSERT(req.decomp_iov[0].iov_base == req.decomp_buf);
	CU_ASSERT(req.decomp_iov[0].iov_len == offset_bytes);
	for (i = 0; i < 2; i++) {
		CU_ASSERT(req.decomp_iov[i + 1].iov_base == req.iov[i].iov_base);
		CU_ASSERT(req.decomp_iov[i + 1].iov_len == req.iov[i].iov_len);
	}
	CU_ASSERT(req.decomp_iov[3].iov_base == req.decomp_buf + offset_bytes + user_buffer_iov_len * 2);
	CU_ASSERT(req.decomp_iov[3].iov_len == remainder_bytes);

	_prepare_compress_chunk(&req, true);
	CU_ASSERT(req.decomp_iovcnt == 4);
	CU_ASSERT(req.decomp_iov[0].iov_base == g_zero_buf);
	CU_ASSERT(req.decomp_iov[0].iov_len == offset_bytes);
	for (i = 0; i < 2; i++) {
		CU_ASSERT(req.decomp_iov[i + 1].iov_base == req.iov[i].iov_base);
		CU_ASSERT(req.decomp_iov[i + 1].iov_len == req.iov[i].iov_len);
	}
	CU_ASSERT(req.decomp_iov[3].iov_base == g_zero_buf + offset_bytes + user_buffer_iov_len * 2);
	CU_ASSERT(req.decomp_iov[3].iov_len == remainder_bytes);

	/* Part 2 - backing dev doesn't support sgl_in */
	/* Test 1 - user's buffers length equals to chunk_size
	 * user's buffers are copied */
	vol.backing_dev->sgl_in = false;
	req.offset = 0;
	user_buffer_iov_len = 8192;
	for (i = 0; i < 2; i++) {
		req.iov[i].iov_base = aligned_user_buffer + i * user_buffer_iov_len;
		req.iov[i].iov_len = user_buffer_iov_len;
		memset(req.iov[i].iov_base, 0xb + i, req.iov[i].iov_len);
	}

	memset(req.decomp_buf, 0xa, vol.params.chunk_size);

	_prepare_compress_chunk(&req, false);
	CU_ASSERT(req.decomp_iovcnt == 1);
	CU_ASSERT(req.decomp_iov[0].iov_base == req.decomp_buf);
	CU_ASSERT(req.decomp_iov[0].iov_len == vol.params.chunk_size);
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base, req.iov[0].iov_base, req.iov[0].iov_len) == 0);
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base + req.iov[0].iov_len, req.iov[1].iov_base,
			 req.iov[1].iov_len) == 0);

	memset(req.decomp_buf, 0xa, vol.params.chunk_size);

	_prepare_compress_chunk(&req, true);
	CU_ASSERT(req.decomp_iovcnt == 1);
	CU_ASSERT(req.decomp_iov[0].iov_base == req.decomp_buf);
	CU_ASSERT(req.decomp_iov[0].iov_len == vol.params.chunk_size);
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base, req.iov[0].iov_base, req.iov[0].iov_len) == 0);
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base + req.iov[0].iov_len, req.iov[1].iov_base,
			 req.iov[1].iov_len) == 0);

	/* Test 2 - single user's buffer length equals to chunk_size, buffer is not aligned
	* User's buffer is copied */
	req.iov[0].iov_base = unaligned_user_buffer;
	req.iov[0].iov_len = vol.params.chunk_size;
	req.iovcnt = 1;
	memset(req.decomp_buf, 0xa, vol.params.chunk_size);

	_prepare_compress_chunk(&req, false);
	CU_ASSERT(req.decomp_iovcnt == 1);
	CU_ASSERT(req.decomp_iov[0].iov_base == req.decomp_buf);
	CU_ASSERT(req.decomp_iov[0].iov_len == vol.params.chunk_size);
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base, req.iov[0].iov_base,
			 req.iov[0].iov_len) == 0);

	memset(req.decomp_buf, 0xa, vol.params.chunk_size);

	_prepare_compress_chunk(&req, true);
	CU_ASSERT(req.decomp_iovcnt == 1);
	CU_ASSERT(req.decomp_iov[0].iov_base == req.decomp_buf);
	CU_ASSERT(req.decomp_iov[0].iov_len == vol.params.chunk_size);
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base, req.iov[0].iov_base,
			 req.iov[0].iov_len) == 0);

	/* Test 3 - single user's buffer length equals to chunk_size
	 * User's buffer is not copied */
	req.iov[0].iov_base = aligned_user_buffer;
	req.iov[0].iov_len = vol.params.chunk_size;
	req.iovcnt = 1;
	memset(req.decomp_buf, 0xa, vol.params.chunk_size);

	_prepare_compress_chunk(&req, false);
	CU_ASSERT(req.decomp_iovcnt == 1);
	CU_ASSERT(req.decomp_iov[0].iov_base == req.iov[0].iov_base);
	CU_ASSERT(req.decomp_iov[0].iov_len == vol.params.chunk_size);

	memset(req.decomp_buf, 0xa, vol.params.chunk_size);

	_prepare_compress_chunk(&req, true);
	CU_ASSERT(req.decomp_iovcnt == 1);
	CU_ASSERT(req.decomp_iov[0].iov_base == req.iov[0].iov_base);
	CU_ASSERT(req.decomp_iov[0].iov_len == vol.params.chunk_size);

	/* Test 4 - user's buffer less than chunk_size, without offset
	 * User's buffers are copied */
	memset(req.decomp_buf, 0xa, vol.params.chunk_size);
	user_buffer_iov_len = 4096;
	req.iovcnt = 2;
	remainder_bytes = vol.params.chunk_size - user_buffer_iov_len * 2;
	for (i = 0; i < 2; i++) {
		req.iov[i].iov_base = aligned_user_buffer + i * user_buffer_iov_len;
		req.iov[i].iov_len = user_buffer_iov_len;
	}

	memset(req.decomp_buf, 0xa, vol.params.chunk_size);

	_prepare_compress_chunk(&req, false);
	CU_ASSERT(req.decomp_iovcnt == 1);
	CU_ASSERT(req.decomp_iov[0].iov_base == req.decomp_buf);
	CU_ASSERT(req.decomp_iov[0].iov_len == vol.params.chunk_size);
	memcmp_offset = 0;
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base + memcmp_offset, req.iov[0].iov_base,
			 req.iov[0].iov_len) == 0);
	memcmp_offset += req.iov[0].iov_len;
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base + memcmp_offset, req.iov[1].iov_base,
			 req.iov[1].iov_len) == 0);
	memcmp_offset += req.iov[0].iov_len;
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base + memcmp_offset, req.decomp_buf + memcmp_offset,
			 remainder_bytes) == 0);

	memset(req.decomp_buf, 0xa, vol.params.chunk_size);

	_prepare_compress_chunk(&req, true);
	CU_ASSERT(req.decomp_iovcnt == 1);
	CU_ASSERT(req.decomp_iov[0].iov_base == req.decomp_buf);
	CU_ASSERT(req.decomp_iov[0].iov_len == vol.params.chunk_size);
	memcmp_offset = 0;
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base + memcmp_offset, req.iov[0].iov_base,
			 req.iov[0].iov_len) == 0);
	memcmp_offset += req.iov[0].iov_len;
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base + memcmp_offset, req.iov[1].iov_base,
			 req.iov[1].iov_len) == 0);
	memcmp_offset += req.iov[0].iov_len;
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base + memcmp_offset, g_zero_buf + memcmp_offset,
			 remainder_bytes) == 0);

	/* Test 5 - user's buffer less than chunk_size, non zero offset
	 * user's buffers are copied */
	req.offset = 3;
	offset_bytes = req.offset * vol.params.logical_block_size;
	remainder_bytes = vol.params.chunk_size - offset_bytes - user_buffer_iov_len * 2;

	memset(req.decomp_buf, 0xa, vol.params.chunk_size);

	_prepare_compress_chunk(&req, false);
	CU_ASSERT(req.decomp_iovcnt == 1);
	CU_ASSERT(req.decomp_iov[0].iov_base == req.decomp_buf);
	CU_ASSERT(req.decomp_iov[0].iov_len == vol.params.chunk_size);
	memcmp_offset = 0;
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base + memcmp_offset, req.decomp_buf, offset_bytes) == 0);
	memcmp_offset += offset_bytes;
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base + memcmp_offset, req.iov[0].iov_base,
			 req.iov[0].iov_len) == 0);
	memcmp_offset += req.iov[0].iov_len;
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base + memcmp_offset, req.iov[1].iov_base,
			 req.iov[1].iov_len) == 0);
	memcmp_offset += req.iov[1].iov_len;
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base + memcmp_offset, req.decomp_buf + memcmp_offset,
			 remainder_bytes) == 0);

	memset(req.decomp_buf, 0xa, vol.params.chunk_size);

	_prepare_compress_chunk(&req, true);
	CU_ASSERT(req.decomp_iovcnt == 1);
	CU_ASSERT(req.decomp_iov[0].iov_base == req.decomp_buf);
	CU_ASSERT(req.decomp_iov[0].iov_len == vol.params.chunk_size);
	memcmp_offset = 0;
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base + memcmp_offset, g_zero_buf, offset_bytes) == 0);
	memcmp_offset += offset_bytes;
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base + memcmp_offset, req.iov[0].iov_base,
			 req.iov[0].iov_len) == 0);
	memcmp_offset += req.iov[0].iov_len;
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base + memcmp_offset, req.iov[1].iov_base,
			 req.iov[1].iov_len) == 0);
	memcmp_offset += req.iov[1].iov_len;
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base + memcmp_offset, g_zero_buf + memcmp_offset,
			 remainder_bytes) == 0);
	backing_dev_destroy(&backing_dev);
	free(buf);
}

static void
_reduce_vol_op_complete(void *ctx, int reduce_errno)
{
	g_reduce_errno = reduce_errno;
}

static void
dummy_backing_dev_decompress(struct spdk_reduce_backing_dev *backing_dev,
			     struct iovec *src_iov, int src_iovcnt,
			     struct iovec *dst_iov, int dst_iovcnt,
			     struct spdk_reduce_vol_cb_args *args)
{
	args->output_size = g_decompressed_len;
	args->cb_fn(args->cb_arg, 0);
}
static void
test_reduce_decompress_chunk(void)
{
	struct spdk_reduce_vol vol = {};
	struct spdk_reduce_backing_dev backing_dev = {};
	struct spdk_reduce_vol_request req = {};
	void *buf;
	char *buffer_end, *aligned_user_buffer, *unaligned_user_buffer;
	char decomp_buffer[16 * 1024] = {};
	char comp_buffer[16 * 1024] = {};
	struct iovec user_iov[2] = {};
	struct iovec comp_buf_iov = {};
	struct spdk_reduce_chunk_map chunk = {};
	size_t user_buffer_iov_len = 8192;
	size_t remainder_bytes;
	size_t offset_bytes;
	uint32_t i;

	vol.params.chunk_size = 16 * 1024;
	vol.params.backing_io_unit_size = 4096;
	vol.params.logical_block_size = 512;
	backing_dev_init(&backing_dev, &vol.params, 512);
	backing_dev.decompress = dummy_backing_dev_decompress;
	vol.backing_dev = &backing_dev;
	vol.logical_blocks_per_chunk = vol.params.chunk_size / vol.params.logical_block_size;
	TAILQ_INIT(&vol.executing_requests);
	TAILQ_INIT(&vol.queued_requests);
	TAILQ_INIT(&vol.free_requests);

	/* Allocate 1 extra byte to test a case when buffer crosses huge page boundary */
	SPDK_CU_ASSERT_FATAL(posix_memalign(&buf, VALUE_2MB, VALUE_2MB + 1) == 0);
	buffer_end = (char *)buf + VALUE_2MB + 1;
	aligned_user_buffer = (char *)buf;
	unaligned_user_buffer = buffer_end - vol.params.chunk_size;

	chunk.compressed_size = user_buffer_iov_len / 2;
	req.chunk = &chunk;
	req.vol = &vol;
	req.decomp_buf = decomp_buffer;
	req.comp_buf = comp_buffer;
	req.comp_buf_iov = &comp_buf_iov;
	req.iov = user_iov;
	req.iovcnt = 2;
	req.offset = 0;
	req.cb_fn = _reduce_vol_op_complete;

	/* Part 1 - backing dev supports sgl_out */
	/* Test 1 - user's buffers length equals to chunk_size */
	for (i = 0; i < 2; i++) {
		req.iov[i].iov_base = aligned_user_buffer + i * user_buffer_iov_len;
		req.iov[i].iov_len = user_buffer_iov_len;
		memset(req.iov[i].iov_base, 0, req.iov[i].iov_len);
	}
	TAILQ_INSERT_HEAD(&vol.executing_requests, &req, tailq);
	g_reduce_errno = -1;
	g_decompressed_len = vol.params.chunk_size;

	_reduce_vol_decompress_chunk(&req, _read_decompress_done);
	CU_ASSERT(g_reduce_errno == 0);
	CU_ASSERT(req.copy_after_decompress == false);
	CU_ASSERT(req.decomp_iovcnt == 2);
	for (i = 0; i < 2; i++) {
		CU_ASSERT(req.decomp_iov[i].iov_base == req.iov[i].iov_base);
		CU_ASSERT(req.decomp_iov[i].iov_len == req.iov[i].iov_len);
	}
	CU_ASSERT(TAILQ_EMPTY(&vol.executing_requests));
	CU_ASSERT(TAILQ_FIRST(&vol.free_requests) == &req);

	/* Test 2 - user's buffer less than chunk_size, without offset */
	TAILQ_INSERT_HEAD(&vol.executing_requests, &req, tailq);
	g_reduce_errno = -1;
	user_buffer_iov_len = 4096;
	for (i = 0; i < 2; i++) {
		req.iov[i].iov_base = aligned_user_buffer + i * user_buffer_iov_len;
		req.iov[i].iov_len = user_buffer_iov_len;
		memset(req.iov[i].iov_base, 0, req.iov[i].iov_len);
	}
	remainder_bytes = vol.params.chunk_size - user_buffer_iov_len * 2;

	_reduce_vol_decompress_chunk(&req, _read_decompress_done);
	CU_ASSERT(g_reduce_errno == 0);
	CU_ASSERT(req.copy_after_decompress == false);
	CU_ASSERT(req.decomp_iovcnt == 3);
	for (i = 0; i < 2; i++) {
		CU_ASSERT(req.decomp_iov[i].iov_base == req.iov[i].iov_base);
		CU_ASSERT(req.decomp_iov[i].iov_len == req.iov[i].iov_len);
	}
	CU_ASSERT(req.decomp_iov[i].iov_base == req.decomp_buf + user_buffer_iov_len * 2);
	CU_ASSERT(req.decomp_iov[i].iov_len == remainder_bytes);
	CU_ASSERT(TAILQ_EMPTY(&vol.executing_requests));
	CU_ASSERT(TAILQ_FIRST(&vol.free_requests) == &req);

	/* Test 3 - user's buffer less than chunk_size, non zero offset */
	req.offset = 3;
	offset_bytes = req.offset * vol.params.logical_block_size;
	remainder_bytes = vol.params.chunk_size - offset_bytes - user_buffer_iov_len * 2;
	TAILQ_INSERT_HEAD(&vol.executing_requests, &req, tailq);
	g_reduce_errno = -1;

	_reduce_vol_decompress_chunk(&req, _read_decompress_done);
	CU_ASSERT(g_reduce_errno == 0);
	CU_ASSERT(req.copy_after_decompress == false);
	CU_ASSERT(req.decomp_iovcnt == 4);
	CU_ASSERT(req.decomp_iov[0].iov_base == req.decomp_buf);
	CU_ASSERT(req.decomp_iov[0].iov_len == offset_bytes);
	for (i = 0; i < 2; i++) {
		CU_ASSERT(req.decomp_iov[i + 1].iov_base == req.iov[i].iov_base);
		CU_ASSERT(req.decomp_iov[i + 1].iov_len == req.iov[i].iov_len);
	}
	CU_ASSERT(req.decomp_iov[3].iov_base == req.decomp_buf + offset_bytes + user_buffer_iov_len * 2);
	CU_ASSERT(req.decomp_iov[3].iov_len == remainder_bytes);
	CU_ASSERT(TAILQ_EMPTY(&vol.executing_requests));
	CU_ASSERT(TAILQ_FIRST(&vol.free_requests) == &req);

	/* Part 2 - backing dev doesn't support sgl_out */
	/* Test 1 - user's buffers length equals to chunk_size
	 * user's buffers are copied */
	vol.backing_dev->sgl_out = false;
	req.offset = 0;
	user_buffer_iov_len = 8192;

	memset(req.decomp_buf, 0xa, vol.params.chunk_size);
	for (i = 0; i < 2; i++) {
		req.iov[i].iov_base = aligned_user_buffer + i * user_buffer_iov_len;
		req.iov[i].iov_len = user_buffer_iov_len;
		memset(req.iov[i].iov_base, 0xb + i, req.iov[i].iov_len);
	}
	TAILQ_INSERT_HEAD(&vol.executing_requests, &req, tailq);
	g_reduce_errno = -1;

	_reduce_vol_decompress_chunk(&req, _read_decompress_done);
	CU_ASSERT(g_reduce_errno == 0);
	CU_ASSERT(req.copy_after_decompress == true);
	CU_ASSERT(req.decomp_iovcnt == 1);
	CU_ASSERT(req.decomp_iov[0].iov_base == req.decomp_buf);
	CU_ASSERT(req.decomp_iov[0].iov_len == vol.params.chunk_size);
	CU_ASSERT(memcmp(req.iov[0].iov_base, req.decomp_iov[0].iov_base, req.iov[0].iov_len) == 0);
	CU_ASSERT(memcmp(req.iov[1].iov_base, req.decomp_iov[0].iov_base + req.iov[0].iov_len,
			 req.iov[1].iov_len) == 0);
	CU_ASSERT(TAILQ_EMPTY(&vol.executing_requests));
	CU_ASSERT(TAILQ_FIRST(&vol.free_requests) == &req);

	/* Test 2 - single user's buffer length equals to chunk_size, buffer is not aligned
	* User's buffer is copied */
	memset(unaligned_user_buffer, 0xc, vol.params.chunk_size);
	req.iov[0].iov_base = unaligned_user_buffer;
	req.iov[0].iov_len = vol.params.chunk_size;
	req.iovcnt = 1;
	memset(req.decomp_buf, 0xa, vol.params.chunk_size);
	TAILQ_INSERT_HEAD(&vol.executing_requests, &req, tailq);
	g_reduce_errno = -1;

	_reduce_vol_decompress_chunk(&req, _read_decompress_done);
	CU_ASSERT(g_reduce_errno == 0);
	CU_ASSERT(req.copy_after_decompress == true);
	CU_ASSERT(req.decomp_iovcnt == 1);
	CU_ASSERT(req.decomp_iov[0].iov_base == req.decomp_buf);
	CU_ASSERT(req.decomp_iov[0].iov_len == vol.params.chunk_size);
	CU_ASSERT(memcmp(req.iov[0].iov_base, req.decomp_iov[0].iov_base,
			 req.iov[0].iov_len) == 0);

	/* Test 3 - single user's buffer length equals to chunk_size
	* User's buffer is not copied */
	req.iov[0].iov_base = aligned_user_buffer;
	req.iov[0].iov_len = vol.params.chunk_size;
	req.iovcnt = 1;
	memset(req.decomp_buf, 0xa, vol.params.chunk_size);
	TAILQ_INSERT_HEAD(&vol.executing_requests, &req, tailq);
	g_reduce_errno = -1;

	_reduce_vol_decompress_chunk(&req, _read_decompress_done);
	CU_ASSERT(g_reduce_errno == 0);
	CU_ASSERT(req.copy_after_decompress == false);
	CU_ASSERT(req.decomp_iovcnt == 1);
	CU_ASSERT(req.decomp_iov[0].iov_base == req.iov[0].iov_base);
	CU_ASSERT(req.decomp_iov[0].iov_len == vol.params.chunk_size);

	/* Test 4 - user's buffer less than chunk_size, without offset
	 * User's buffers are copied */
	user_buffer_iov_len = 4096;
	req.iovcnt = 2;
	remainder_bytes = vol.params.chunk_size - user_buffer_iov_len * 2;
	for (i = 0; i < 2; i++) {
		req.iov[i].iov_base = aligned_user_buffer + i * user_buffer_iov_len;
		req.iov[i].iov_len = user_buffer_iov_len;
		memset(req.iov[i].iov_base, 0xb + i, req.iov[i].iov_len);
	}

	memset(req.decomp_buf, 0xa, vol.params.chunk_size);
	TAILQ_INSERT_HEAD(&vol.executing_requests, &req, tailq);
	g_reduce_errno = -1;

	_reduce_vol_decompress_chunk(&req, _read_decompress_done);
	CU_ASSERT(g_reduce_errno == 0);
	CU_ASSERT(req.copy_after_decompress == true);
	CU_ASSERT(req.decomp_iovcnt == 1);
	CU_ASSERT(req.decomp_iov[0].iov_base == req.decomp_buf);
	CU_ASSERT(req.decomp_iov[0].iov_len == vol.params.chunk_size);
	CU_ASSERT(memcmp(req.iov[0].iov_base, req.decomp_iov[0].iov_base,
			 req.iov[0].iov_len) == 0);
	CU_ASSERT(memcmp(req.iov[1].iov_base, req.decomp_iov[0].iov_base + req.iov[0].iov_len,
			 req.iov[1].iov_len) == 0);
	CU_ASSERT(TAILQ_EMPTY(&vol.executing_requests));
	CU_ASSERT(TAILQ_FIRST(&vol.free_requests) == &req);

	/* Test 5 - user's buffer less than chunk_size, non zero offset
	* user's buffers are copied */
	req.offset = 3;
	offset_bytes = req.offset * vol.params.logical_block_size;
	remainder_bytes = vol.params.chunk_size - offset_bytes - user_buffer_iov_len * 2;

	for (i = 0; i < 2; i++) {
		req.iov[i].iov_base = aligned_user_buffer + i * user_buffer_iov_len;
		req.iov[i].iov_len = user_buffer_iov_len;
		memset(req.iov[i].iov_base, 0xb + i, req.iov[i].iov_len);
	}

	memset(req.decomp_buf, 0xa, vol.params.chunk_size);
	TAILQ_INSERT_HEAD(&vol.executing_requests, &req, tailq);
	g_reduce_errno = -1;

	_prepare_compress_chunk(&req, false);
	_reduce_vol_decompress_chunk(&req, _read_decompress_done);
	CU_ASSERT(g_reduce_errno == 0);
	CU_ASSERT(req.copy_after_decompress == true);
	CU_ASSERT(req.decomp_iovcnt == 1);
	CU_ASSERT(req.decomp_iov[0].iov_base == req.decomp_buf);
	CU_ASSERT(req.decomp_iov[0].iov_len == vol.params.chunk_size);
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base + offset_bytes, req.iov[0].iov_base,
			 req.iov[0].iov_len) == 0);
	CU_ASSERT(memcmp(req.decomp_iov[0].iov_base + offset_bytes + req.iov[0].iov_len,
			 req.iov[1].iov_base,
			 req.iov[1].iov_len) == 0);
	CU_ASSERT(TAILQ_EMPTY(&vol.executing_requests));
	CU_ASSERT(TAILQ_FIRST(&vol.free_requests) == &req);

	free(buf);
}

static void
test_allocate_vol_requests(void)
{
	struct spdk_reduce_vol *vol;
	/* include chunk_sizes which are not power of 2 */
	uint32_t chunk_sizes[] = {8192, 8320, 16384, 16416, 32768};
	uint32_t io_unit_sizes[] = {512, 520, 4096, 4104, 4096};
	uint32_t i;

	for (i = 0; i < 4; i++) {
		vol = calloc(1, sizeof(*vol));
		SPDK_CU_ASSERT_FATAL(vol);

		vol->params.chunk_size = chunk_sizes[i];
		vol->params.logical_block_size = io_unit_sizes[i];
		vol->params.backing_io_unit_size = io_unit_sizes[i];
		vol->backing_io_units_per_chunk = vol->params.chunk_size / vol->params.backing_io_unit_size;
		vol->logical_blocks_per_chunk = vol->params.chunk_size / vol->params.logical_block_size;

		CU_ASSERT(_validate_vol_params(&vol->params) == 0);
		CU_ASSERT(_allocate_vol_requests(vol) == 0);
		_init_load_cleanup(vol, NULL);
	}
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("reduce", NULL, NULL);

	CU_ADD_TEST(suite, get_pm_file_size);
	CU_ADD_TEST(suite, get_vol_size);
	CU_ADD_TEST(suite, init_failure);
	CU_ADD_TEST(suite, init_md);
	CU_ADD_TEST(suite, init_backing_dev);
	CU_ADD_TEST(suite, load);
	CU_ADD_TEST(suite, write_maps);
	CU_ADD_TEST(suite, read_write);
	CU_ADD_TEST(suite, readv_writev);
	CU_ADD_TEST(suite, destroy);
	CU_ADD_TEST(suite, defer_bdev_io);
	CU_ADD_TEST(suite, overlapped);
	CU_ADD_TEST(suite, compress_algorithm);
	CU_ADD_TEST(suite, test_prepare_compress_chunk);
	CU_ADD_TEST(suite, test_reduce_decompress_chunk);
	CU_ADD_TEST(suite, test_allocate_vol_requests);

	g_unlink_path = g_path;
	g_unlink_callback = unlink_cb;

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
