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

#include "spdk_cunit.h"

#include "reduce/reduce.c"
#include "spdk_internal/mock.h"
#include "common/lib/test_env.c"

static struct spdk_reduce_vol *g_vol;
static int g_reduce_errno;
static char *g_volatile_pm_buf;
static size_t g_volatile_pm_buf_len;
static char *g_persistent_pm_buf;
static size_t g_persistent_pm_buf_len;
static bool g_backing_dev_closed;
static char *g_backing_dev_buf;
static const char *g_path;

#define TEST_MD_PATH "/tmp"

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
	int64_t pm_size, expected_pm_size;

	params.vol_size = 0;
	params.chunk_size = 0;
	params.backing_io_unit_size = 0;
	CU_ASSERT(spdk_reduce_get_pm_file_size(&params) == -EINVAL);

	/*
	 * Select a valid backing_io_unit_size.  This should still fail since
	 *  vol_size and chunk_size are still 0.
	 */
	params.backing_io_unit_size = 4096;
	CU_ASSERT(spdk_reduce_get_pm_file_size(&params) == -EINVAL);

	/*
	 * Select a valid chunk_size.  This should still fail since val_size
	 *  is still 0.
	 */
	params.chunk_size = 4096 * 4;
	CU_ASSERT(spdk_reduce_get_pm_file_size(&params) == -EINVAL);

	/* Select a valid vol_size.  This should return a proper pm_size. */
	params.vol_size = 4096 * 4 * 100;
	pm_size = spdk_reduce_get_pm_file_size(&params);
	expected_pm_size = sizeof(struct spdk_reduce_vol_superblock);
	/* 100 chunks in logical map * 8 bytes per chunk */
	expected_pm_size += 100 * sizeof(uint64_t);
	/* 100 chunks * 4 backing io units per chunk * 8 bytes per backing io unit */
	expected_pm_size += 100 * 4 * sizeof(uint64_t);
	/* reduce allocates some extra chunks too for in-flight writes when logical map
	 * is full.  REDUCE_EXTRA_CHUNKS is a private #ifdef in reduce.c.
	 */
	expected_pm_size += REDUCE_NUM_EXTRA_CHUNKS * 4 * sizeof(uint64_t);
	/* reduce will add some padding so numbers may not match exactly.  Make sure
	 * they are close though.
	 */
	CU_ASSERT((pm_size - expected_pm_size) < REDUCE_PM_SIZE_ALIGNMENT);
}

static void
get_backing_device_size(void)
{
	struct spdk_reduce_vol_params params;
	int64_t backing_size, expected_backing_size;

	params.vol_size = 0;
	params.chunk_size = 0;
	params.backing_io_unit_size = 0;
	params.logical_block_size = 512;
	CU_ASSERT(spdk_reduce_get_backing_device_size(&params) == -EINVAL);

	/*
	 * Select a valid backing_io_unit_size.  This should still fail since
	 *  vol_size and chunk_size are still 0.
	 */
	params.backing_io_unit_size = 4096;
	CU_ASSERT(spdk_reduce_get_backing_device_size(&params) == -EINVAL);

	/*
	 * Select a valid chunk_size.  This should still fail since val_size
	 *  is still 0.
	 */
	params.chunk_size = 4096 * 4;
	CU_ASSERT(spdk_reduce_get_backing_device_size(&params) == -EINVAL);

	/* Select a valid vol_size.  This should return a proper backing device size. */
	params.vol_size = 4096 * 4 * 100;
	backing_size = spdk_reduce_get_backing_device_size(&params);
	expected_backing_size = params.vol_size;
	/* reduce allocates some extra chunks too for in-flight writes when logical map
	 * is full.  REDUCE_EXTRA_CHUNKS is a private #ifdef in reduce.c.  Backing device
	 * must have space allocated for these extra chunks.
	 */
	expected_backing_size += REDUCE_NUM_EXTRA_CHUNKS * params.chunk_size;
	/* Account for superblock as well. */
	expected_backing_size += sizeof(struct spdk_reduce_vol_superblock);
	CU_ASSERT(backing_size == expected_backing_size);
}

void *
pmem_map_file(const char *path, size_t len, int flags, mode_t mode,
	      size_t *mapped_lenp, int *is_pmemp)
{
	CU_ASSERT(g_volatile_pm_buf == NULL);
	g_path = path;
	*is_pmemp = 1;

	if (g_persistent_pm_buf == NULL) {
		g_persistent_pm_buf = calloc(1, len);
		g_persistent_pm_buf_len = len;
		SPDK_CU_ASSERT_FATAL(g_persistent_pm_buf != NULL);
	}

	*mapped_lenp = g_persistent_pm_buf_len;
	g_volatile_pm_buf = calloc(1, g_persistent_pm_buf_len);
	SPDK_CU_ASSERT_FATAL(g_volatile_pm_buf != NULL);
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

	params.vol_size = 1024 * 1024; /* 1MB */
	params.chunk_size = 16 * 1024;
	params.backing_io_unit_size = backing_dev.blocklen;
	params.logical_block_size = 512;

	/* backing_dev and pm_file have an invalid size.  This should fail. */
	g_vol = NULL;
	g_reduce_errno = 0;
	spdk_reduce_vol_init(&params, &backing_dev, TEST_MD_PATH, init_cb, NULL);
	CU_ASSERT(g_reduce_errno == -EINVAL);
	SPDK_CU_ASSERT_FATAL(g_vol == NULL);

	/* backing_dev now has valid size, but backing_dev still has null
	 *  function pointers.  This should fail.
	 */
	backing_dev.blockcnt = spdk_reduce_get_backing_device_size(&params) / backing_dev.blocklen;

	g_vol = NULL;
	g_reduce_errno = 0;
	spdk_reduce_vol_init(&params, &backing_dev, TEST_MD_PATH, init_cb, NULL);
	CU_ASSERT(g_reduce_errno == -EINVAL);
	SPDK_CU_ASSERT_FATAL(g_vol == NULL);
}

static void
backing_dev_readv(struct spdk_reduce_backing_dev *backing_dev, struct iovec *iov, int iovcnt,
		  uint64_t lba, uint32_t lba_count, struct spdk_reduce_vol_cb_args *args)
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
backing_dev_writev(struct spdk_reduce_backing_dev *backing_dev, struct iovec *iov, int iovcnt,
		   uint64_t lba, uint32_t lba_count, struct spdk_reduce_vol_cb_args *args)
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
backing_dev_unmap(struct spdk_reduce_backing_dev *backing_dev,
		  uint64_t lba, uint32_t lba_count, struct spdk_reduce_vol_cb_args *args)
{
	char *offset;

	offset = g_backing_dev_buf + lba * backing_dev->blocklen;
	memset(offset, 0, lba_count * backing_dev->blocklen);
	args->cb_fn(args->cb_arg, 0);
}

static void
backing_dev_close(struct spdk_reduce_backing_dev *backing_dev)
{
	g_backing_dev_closed = true;
}

static void
backing_dev_destroy(struct spdk_reduce_backing_dev *backing_dev)
{
	/* We don't free this during backing_dev_close so that we can test init/unload/load
	 *  scenarios.
	 */
	free(g_backing_dev_buf);
	g_backing_dev_buf = NULL;
}

static void
backing_dev_init(struct spdk_reduce_backing_dev *backing_dev, struct spdk_reduce_vol_params *params)
{
	int64_t size;

	size = spdk_reduce_get_backing_device_size(params);
	backing_dev->blocklen = params->backing_io_unit_size;
	backing_dev->blockcnt = size / backing_dev->blocklen;
	backing_dev->readv = backing_dev_readv;
	backing_dev->writev = backing_dev_writev;
	backing_dev->unmap = backing_dev_unmap;
	backing_dev->close = backing_dev_close;

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

	params.vol_size = 1024 * 1024; /* 1MB */
	params.chunk_size = 16 * 1024;
	params.backing_io_unit_size = 512;
	params.logical_block_size = 512;

	backing_dev_init(&backing_dev, &params);

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
	g_backing_dev_closed = false;
	spdk_reduce_vol_unload(g_vol, unload_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	CU_ASSERT(g_backing_dev_closed == true);
	CU_ASSERT(g_volatile_pm_buf == NULL);

	persistent_pm_buf_destroy();
	backing_dev_destroy(&backing_dev);
}

static void
init_backing_dev(void)
{
	struct spdk_reduce_vol_params params = {};
	struct spdk_reduce_vol_params *persistent_params;
	struct spdk_reduce_backing_dev backing_dev = {};

	params.vol_size = 1024 * 1024; /* 1MB */
	params.chunk_size = 16 * 1024;
	params.backing_io_unit_size = 512;
	params.logical_block_size = 512;
	spdk_uuid_generate(&params.uuid);

	backing_dev_init(&backing_dev, &params);

	g_vol = NULL;
	g_path = NULL;
	g_reduce_errno = -1;
	spdk_reduce_vol_init(&params, &backing_dev, TEST_MD_PATH, init_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	SPDK_CU_ASSERT_FATAL(g_vol != NULL);
	SPDK_CU_ASSERT_FATAL(g_path != NULL);
	/* Confirm that libreduce persisted the params to the backing device. */
	CU_ASSERT(memcmp(g_backing_dev_buf, SPDK_REDUCE_SIGNATURE, 8) == 0);
	persistent_params = (struct spdk_reduce_vol_params *)(g_backing_dev_buf + 8);
	CU_ASSERT(memcmp(persistent_params, &params, sizeof(params)) == 0);
	CU_ASSERT(backing_dev.close != NULL);
	/* Confirm that the path to the persistent memory metadata file was persisted to
	 *  the backing device.
	 */
	CU_ASSERT(strncmp(g_path,
			  g_backing_dev_buf + REDUCE_BACKING_DEV_PATH_OFFSET,
			  REDUCE_PATH_MAX) == 0);

	g_reduce_errno = -1;
	g_backing_dev_closed = false;
	spdk_reduce_vol_unload(g_vol, unload_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	CU_ASSERT(g_backing_dev_closed == true);

	persistent_pm_buf_destroy();
	backing_dev_destroy(&backing_dev);
}

static void
load(void)
{
	struct spdk_reduce_vol_params params = {};
	struct spdk_reduce_backing_dev backing_dev = {};
	char pmem_file_path[REDUCE_PATH_MAX];

	params.vol_size = 1024 * 1024; /* 1MB */
	params.chunk_size = 16 * 1024;
	params.backing_io_unit_size = 512;
	params.logical_block_size = 512;
	spdk_uuid_generate(&params.uuid);

	backing_dev_init(&backing_dev, &params);

	g_vol = NULL;
	g_reduce_errno = -1;
	spdk_reduce_vol_init(&params, &backing_dev, TEST_MD_PATH, init_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	SPDK_CU_ASSERT_FATAL(g_vol != NULL);
	SPDK_CU_ASSERT_FATAL(g_path != NULL);
	memcpy(pmem_file_path, g_path, sizeof(pmem_file_path));

	g_reduce_errno = -1;
	spdk_reduce_vol_unload(g_vol, unload_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	g_vol = NULL;
	g_path = NULL;
	g_reduce_errno = -1;
	spdk_reduce_vol_load(&backing_dev, load_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	SPDK_CU_ASSERT_FATAL(g_vol != NULL);
	SPDK_CU_ASSERT_FATAL(g_path != NULL);
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

static uint64_t
_vol_get_chunk_map_index(struct spdk_reduce_vol *vol, uint64_t offset)
{
	uint64_t logical_map_index = offset / vol->logical_blocks_per_chunk;

	return vol->pm_logical_map[logical_map_index];
}

static uint64_t *
_vol_get_chunk_map(struct spdk_reduce_vol *vol, uint64_t chunk_map_index)
{
	return &vol->pm_chunk_maps[chunk_map_index * vol->backing_io_units_per_chunk];
}

static void
write_cb(void *arg, int reduce_errno)
{
	g_reduce_errno = reduce_errno;
}

static void
write_maps(void)
{
	struct spdk_reduce_vol_params params = {};
	struct spdk_reduce_backing_dev backing_dev = {};
	struct iovec iov;
	char buf[16 * 1024]; /* chunk size */
	uint32_t i;
	uint64_t old_chunk0_map_index, new_chunk0_map_index;
	uint64_t *old_chunk0_map, *new_chunk0_map;

	params.vol_size = 1024 * 1024; /* 1MB */
	params.chunk_size = 16 * 1024;
	params.backing_io_unit_size = 4096;
	params.logical_block_size = 512;
	spdk_uuid_generate(&params.uuid);

	backing_dev_init(&backing_dev, &params);

	g_vol = NULL;
	g_reduce_errno = -1;
	spdk_reduce_vol_init(&params, &backing_dev, TEST_MD_PATH, init_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);
	SPDK_CU_ASSERT_FATAL(g_vol != NULL);

	for (i = 0; i < g_vol->params.vol_size / g_vol->params.chunk_size; i++) {
		CU_ASSERT(_vol_get_chunk_map_index(g_vol, i) == REDUCE_EMPTY_MAP_ENTRY);
	}

	iov.iov_base = buf;
	iov.iov_len = params.logical_block_size;
	g_reduce_errno = -1;
	spdk_reduce_vol_writev(g_vol, &iov, 1, 0, 1, write_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	old_chunk0_map_index = _vol_get_chunk_map_index(g_vol, 0);
	CU_ASSERT(old_chunk0_map_index != REDUCE_EMPTY_MAP_ENTRY);
	CU_ASSERT(spdk_bit_array_get(g_vol->allocated_chunk_maps, old_chunk0_map_index) == true);

	old_chunk0_map = _vol_get_chunk_map(g_vol, old_chunk0_map_index);
	for (i = 0; i < g_vol->backing_io_units_per_chunk; i++) {
		CU_ASSERT(old_chunk0_map[i] != REDUCE_EMPTY_MAP_ENTRY);
		CU_ASSERT(spdk_bit_array_get(g_vol->allocated_backing_io_units, old_chunk0_map[i]) == true);
	}

	g_reduce_errno = -1;
	spdk_reduce_vol_writev(g_vol, &iov, 1, 0, 1, write_cb, NULL);
	CU_ASSERT(g_reduce_errno == 0);

	new_chunk0_map_index = _vol_get_chunk_map_index(g_vol, 0);
	CU_ASSERT(new_chunk0_map_index != REDUCE_EMPTY_MAP_ENTRY);
	CU_ASSERT(new_chunk0_map_index != old_chunk0_map_index);
	CU_ASSERT(spdk_bit_array_get(g_vol->allocated_chunk_maps, new_chunk0_map_index) == true);
	CU_ASSERT(spdk_bit_array_get(g_vol->allocated_chunk_maps, old_chunk0_map_index) == false);

	for (i = 0; i < g_vol->backing_io_units_per_chunk; i++) {
		CU_ASSERT(spdk_bit_array_get(g_vol->allocated_backing_io_units, old_chunk0_map[i]) == false);
	}

	new_chunk0_map = _vol_get_chunk_map(g_vol, new_chunk0_map_index);
	for (i = 0; i < g_vol->backing_io_units_per_chunk; i++) {
		CU_ASSERT(new_chunk0_map[i] != REDUCE_EMPTY_MAP_ENTRY);
		CU_ASSERT(spdk_bit_array_get(g_vol->allocated_backing_io_units, new_chunk0_map[i]) == true);
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

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("reduce", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "get_pm_file_size", get_pm_file_size) == NULL ||
		CU_add_test(suite, "get_backing_device_size", get_backing_device_size) == NULL ||
		CU_add_test(suite, "init_failure", init_failure) == NULL ||
		CU_add_test(suite, "init_md", init_md) == NULL ||
		CU_add_test(suite, "init_backing_dev", init_backing_dev) == NULL ||
		CU_add_test(suite, "load", load) == NULL ||
		CU_add_test(suite, "write_maps", write_maps) == NULL
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
