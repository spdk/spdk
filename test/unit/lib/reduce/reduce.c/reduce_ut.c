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
#include "common/lib/test_env.c"

static struct spdk_reduce_vol *g_vol;
static int g_ziperrno;

static void
get_pm_file_size(void)
{
	struct spdk_reduce_vol_params params;
	int64_t pm_size, expected_pm_size;

	params.vol_size = 0;
	params.chunk_size = 0;
	params.backing_io_unit_size = 0;
	CU_ASSERT(spdk_reduce_get_pm_file_size(&params) == -1);

	/*
	 * Select a valid backing_io_unit_size.  This should still fail since
	 *  vol_size and chunk_size are still 0.
	 */
	params.backing_io_unit_size = 4096;
	CU_ASSERT(spdk_reduce_get_pm_file_size(&params) == -1);

	/*
	 * Select a valid chunk_size.  This should still fail since val_size
	 *  is still 0.
	 */
	params.chunk_size = 4096 * 4;
	CU_ASSERT(spdk_reduce_get_pm_file_size(&params) == -1);

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
	expected_pm_size += REDUCE_EXTRA_CHUNKS * 4 * sizeof(uint64_t);
	/* reduce will add some padding so numbers may not match exactly.  Make sure
	 * they are close though.
	 */
	CU_ASSERT((pm_size - expected_pm_size) < 128);
}

static void
get_backing_device_size(void)
{
	struct spdk_reduce_vol_params params;
	int64_t backing_size, expected_backing_size;

	params.vol_size = 0;
	params.chunk_size = 0;
	params.backing_io_unit_size = 0;
	CU_ASSERT(spdk_reduce_get_backing_device_size(&params) == -1);

	/*
	 * Select a valid backing_io_unit_size.  This should still fail since
	 *  vol_size and chunk_size are still 0.
	 */
	params.backing_io_unit_size = 4096;
	CU_ASSERT(spdk_reduce_get_backing_device_size(&params) == -1);

	/*
	 * Select a valid chunk_size.  This should still fail since val_size
	 *  is still 0.
	 */
	params.chunk_size = 4096 * 4;
	CU_ASSERT(spdk_reduce_get_backing_device_size(&params) == -1);

	/* Select a valid vol_size.  This should return a proper backing device size. */
	params.vol_size = 4096 * 4 * 100;
	backing_size = spdk_reduce_get_backing_device_size(&params);
	expected_backing_size = params.vol_size;
	/* reduce allocates some extra chunks too for in-flight writes when logical map
	 * is full.  REDUCE_EXTRA_CHUNKS is a private #ifdef in reduce.c.  Backing device
	 * must have space allocated for these extra chunks.
	 */
	expected_backing_size += REDUCE_EXTRA_CHUNKS * params.chunk_size;
	CU_ASSERT(backing_size == expected_backing_size);
}

static void
init_cb(void *cb_arg, struct spdk_reduce_vol *vol, int ziperrno)
{
	g_vol = vol;
	g_ziperrno = ziperrno;
}

static void
unload_cb(void *cb_arg, int ziperrno)
{
	g_ziperrno = ziperrno;
}

static void
init(void)
{
	struct spdk_reduce_vol_params params = {};
	struct spdk_reduce_backing_dev backing_dev = {};
	struct spdk_reduce_pm_file pm_file = {};

	backing_dev.blocklen = 512;

	params.vol_size = 1024 * 1024; /* 1MB */
	params.chunk_size = 16 * 1024;
	params.backing_io_unit_size = backing_dev.blocklen;

	/* backing_dev and pm_file have an invalid size.  This should fail. */
	g_vol = NULL;
	g_ziperrno = 0;
	spdk_reduce_vol_init(&params, &backing_dev, &pm_file, init_cb, NULL);
	CU_ASSERT(g_ziperrno == -EINVAL);
	SPDK_CU_ASSERT_FATAL(g_vol == NULL);

	/* backing_dev now has valid size, but pm_file is still invalid.
	 * This should fail.
	 */
	backing_dev.blockcnt = spdk_reduce_get_backing_device_size(&params) / backing_dev.blocklen;

	g_vol = NULL;
	g_ziperrno = 0;
	spdk_reduce_vol_init(&params, &backing_dev, &pm_file, init_cb, NULL);
	CU_ASSERT(g_ziperrno == -EINVAL);
	SPDK_CU_ASSERT_FATAL(g_vol == NULL);

	/* pm_file now has valid size, but uuid is still all zeroes.
	 * This should fail.
	 */
	pm_file.size = spdk_reduce_get_pm_file_size(&params);

	g_vol = NULL;
	g_ziperrno = 0;
	spdk_reduce_vol_init(&params, &backing_dev, &pm_file, init_cb, NULL);
	CU_ASSERT(g_ziperrno == -EINVAL);
	SPDK_CU_ASSERT_FATAL(g_vol == NULL);

	/* Now specify a uuid.  spdk_reduce_vol_init() should then pass. */
	spdk_uuid_generate(&params.uuid);

	g_vol = NULL;
	g_ziperrno = -1;
	spdk_reduce_vol_init(&params, &backing_dev, &pm_file, init_cb, NULL);
	CU_ASSERT(g_ziperrno == 0);
	SPDK_CU_ASSERT_FATAL(g_vol != NULL);
	CU_ASSERT(spdk_uuid_compare(&params.uuid, &g_vol->uuid) == 0);

	g_ziperrno = -1;
	spdk_reduce_vol_unload(g_vol, unload_cb, NULL);
	CU_ASSERT(g_ziperrno == 0);
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
		CU_add_test(suite, "init", init) == NULL
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
