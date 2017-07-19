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
#include "spdk/string.h"

#include "vbdev_lvol.c"

int g_lvolerrno;
struct spdk_lvol_store *g_lvs = NULL;
struct spdk_lvol *g_lvol = NULL;
struct lvol_store_bdev_pair *g_lvs_pair = NULL;
struct spdk_bdev *g_base_bdev = NULL;

int
spdk_lvol_resize(struct spdk_lvol *lvol, size_t sz,
		 spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	return 0;
}

uint64_t
spdk_bs_get_cluster_size(struct spdk_blob_store *bs)
{
	return 0;
}

struct spdk_bdev *
spdk_bdev_get_by_name(const char *bdev_name)
{
	return NULL;
}

void
spdk_lvol_close(struct spdk_lvol *lvol)
{
}

void
spdk_lvol_destroy(struct spdk_lvol *lvol)
{
	free(g_lvs_pair);
	free(g_base_bdev);
	CU_ASSERT_FATAL(lvol == g_lvol);
	free(lvol->name);
	free(lvol);
	g_lvol = NULL;
}

struct lvol_store_bdev_pair *
vbdev_get_lvs_pair_by_lvs(struct spdk_lvol_store *lvs)
{

	g_base_bdev = calloc(1, sizeof(*g_base_bdev));
	g_base_bdev->blocklen = 4096;
	g_base_bdev->max_unmap_bdesc_count = 10;

	g_lvs_pair = calloc(1, sizeof(*g_lvs_pair));
	g_lvs_pair->bdev = g_base_bdev;
	g_lvs_pair->lvs = lvs;

	return g_lvs_pair;
}

bool
is_bdev_opened(struct spdk_bdev *bdev)
{
	return false;
}

struct spdk_lvol *
vbdev_get_lvol_by_name(char *name)
{
	return NULL;
}

void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
}

struct spdk_io_channel *spdk_lvol_get_io_channel(struct spdk_lvol *lvol)
{
	return NULL;
}

uint64_t
spdk_bs_get_page_size(struct spdk_blob_store *bs)
{
	return 0;
}

void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb)
{
}

void
spdk_bs_io_read_blob(struct spdk_blob *blob, struct spdk_io_channel *channel,
		     void *payload, uint64_t offset, uint64_t length,
		     spdk_blob_op_complete cb_fn, void *cb_arg)
{
}

void
spdk_bs_io_write_blob(struct spdk_blob *blob, struct spdk_io_channel *channel,
		      void *payload, uint64_t offset, uint64_t length,
		      spdk_blob_op_complete cb_fn, void *cb_arg)
{
}

void
spdk_bs_io_flush_channel(struct spdk_io_channel *channel, spdk_blob_op_complete cb_fn, void *cb_arg)
{
}

void
spdk_bdev_module_list_add(struct spdk_bdev_module_if *bdev_module)
{
}

int
spdk_json_write_name(struct spdk_json_write_ctx *w, const char *name)
{
	return 0;
}

int
spdk_json_write_string(struct spdk_json_write_ctx *w, const char *val)
{
	return 0;
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return "test";
}

void
spdk_bdev_register(struct spdk_bdev *bdev)
{
}

struct spdk_lvol_store *
vbdev_get_lvol_store_by_guid(uuid_t uuid)
{
	if (uuid_compare(uuid, g_lvs->uuid) == 0)
		return g_lvs;
	else
		return NULL;
}

int
spdk_lvol_create(struct spdk_lvol_store *lvs, size_t sz, spdk_lvol_op_with_handle_complete cb_fn,
		 void *cb_arg)
{
	struct spdk_lvol *lvol = calloc(1, sizeof(*lvol));

	lvol->lvol_store = lvs;
	lvol->sz = sz * 1024 * 1024;
	lvol->name = spdk_sprintf_alloc("%s", "UNIT_TEST_GUID");

	cb_fn(cb_arg, lvol, 0);

	return 0;
}

static void
vbdev_lvol_create_complete(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	g_lvolerrno = lvolerrno;
	g_lvol = lvol;
}

static void
lvol_init(void)
{
	struct spdk_lvol_store *lvs;
	uuid_t wrong_uuid;
	int sz = 10;

	lvs = calloc(1, sizeof(*lvs));
	g_lvs = lvs;

	uuid_generate_time(lvs->uuid);
	uuid_generate_time(wrong_uuid);

	/* Incorrect uuid set */
	g_lvolerrno = 0;
	vbdev_lvol_create(wrong_uuid, sz, vbdev_lvol_create_complete, NULL);
	CU_ASSERT(g_lvol == NULL);
	CU_ASSERT(g_lvolerrno == -1);

	/* Successful lvol create */
	g_lvolerrno = -1;
	vbdev_lvol_create(lvs->uuid, sz, vbdev_lvol_create_complete, NULL);
	CU_ASSERT(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);

	/* Successful lvol destruct */
	vbdev_lvol_destruct(g_lvol);
	CU_ASSERT(g_lvol == NULL);

	free(lvs);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("lvol", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "lvol_init", lvol_init) == NULL
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
