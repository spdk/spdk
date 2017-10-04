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

#define SPDK_BS_PAGE_SIZE 0x1000

int g_lvolerrno;
int g_lvserrno;
int g_cluster_size;
struct spdk_lvol_store *g_lvs = NULL;
struct spdk_lvol *g_lvol = NULL;
struct lvol_store_bdev *g_lvs_bdev = NULL;
struct spdk_bdev *g_base_bdev = NULL;


static struct spdk_bdev g_bdev = {};
static struct spdk_bs_dev *g_bs_dev = NULL;
static struct spdk_lvol_store *g_lvol_store = NULL;
bool lvol_store_initialize_fail = false;
bool lvol_store_initialize_cb_fail = false;
bool lvol_already_opened = false;

int
spdk_bs_bdev_claim(struct spdk_bs_dev *bs_dev, struct spdk_bdev_module_if *module)
{
	if (lvol_already_opened == true)
		return -1;

	lvol_already_opened = true;

	return 0;
}

void
spdk_vbdev_unregister(struct spdk_bdev *vbdev)
{
	return;
}

uint64_t
spdk_bs_get_page_size(struct spdk_blob_store *bs)
{
	return SPDK_BS_PAGE_SIZE;
}

static void
bdev_blob_destroy(struct spdk_bs_dev *bs_dev)
{
	CU_ASSERT(g_bs_dev != NULL);
	CU_ASSERT(bs_dev != NULL);
	CU_ASSERT(g_bs_dev == bs_dev);
	free(bs_dev);
	g_bs_dev = NULL;
	lvol_already_opened = false;
}

struct spdk_bs_dev *
spdk_bdev_create_bs_dev(struct spdk_bdev *bdev, spdk_bdev_remove_cb_t remove_cb, void *remove_ctx)
{
	struct spdk_bs_dev *bs_dev;

	if (lvol_already_opened == true)
		return NULL;

	bs_dev = calloc(1, sizeof(*bs_dev));
	bs_dev->destroy = bdev_blob_destroy;

	CU_ASSERT(g_bs_dev == NULL);
	g_bs_dev = bs_dev;
	return bs_dev;
}

int
spdk_lvs_init(struct spdk_bs_dev *bs_dev, struct spdk_lvs_opts *o,
	      spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store *lvs;
	int error = 0;

	if (lvol_store_initialize_fail)
		return -1;

	if (lvol_store_initialize_cb_fail) {
		bs_dev->destroy(bs_dev);
		lvs = NULL;
		error = -1;
	} else {
		lvs = calloc(1, sizeof(*lvs));
		lvs->bs_dev = bs_dev;
		error = 0;
	}
	cb_fn(cb_arg, lvs, error);

	return 0;
}

int
spdk_lvs_unload(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn,
		void *cb_arg)
{
	struct spdk_lvol_store_req *req = cb_arg;
	free(req);
	free(lvs);
	g_lvol_store = NULL;

	return 0;
}

int
spdk_lvol_resize(struct spdk_lvol *lvol, size_t sz,
		 spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, 0);

	return 0;
}

uint64_t
spdk_bs_get_cluster_size(struct spdk_blob_store *bs)
{
	return g_cluster_size;
}

struct spdk_bdev *
spdk_bdev_get_by_name(const char *bdev_name)
{
	if (!strcmp(g_base_bdev->name, bdev_name)) {
		return g_base_bdev;
	}

	return NULL;
}

void
spdk_lvol_close(struct spdk_lvol *lvol)
{
}

void
spdk_lvol_destroy(struct spdk_lvol *lvol)
{
	SPDK_CU_ASSERT_FATAL(lvol == g_lvol);
	free(lvol->name);
	free(lvol);
	g_lvol = NULL;
}

void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
}

struct spdk_io_channel *spdk_lvol_get_io_channel(struct spdk_lvol *lvol)
{
	return NULL;
}

void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb, uint64_t len)
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
spdk_bs_io_writev_blob(struct spdk_blob *blob, struct spdk_io_channel *channel,
		       struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
		       spdk_blob_op_complete cb_fn, void *cb_arg)
{
}

void
spdk_bs_io_readv_blob(struct spdk_blob *blob, struct spdk_io_channel *channel,
		      struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
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
spdk_vbdev_register(struct spdk_bdev *vbdev, struct spdk_bdev **base_bdevs, int base_bdev_count)
{
}

void
spdk_bdev_module_examine_done(struct spdk_bdev_module_if *module)
{
}

int
spdk_lvol_create(struct spdk_lvol_store *lvs, size_t sz, spdk_lvol_op_with_handle_complete cb_fn,
		 void *cb_arg)
{
	struct spdk_lvol *lvol = calloc(1, sizeof(*lvol));

	SPDK_CU_ASSERT_FATAL(lvol != NULL);

	lvol->lvol_store = lvs;
	lvol->name = spdk_sprintf_alloc("%s", "UNIT_TEST_UUID");
	SPDK_CU_ASSERT_FATAL(lvol->name != NULL);

	TAILQ_INIT(&lvs->lvols);
	TAILQ_INSERT_TAIL(&lvol->lvol_store->lvols, lvol, link);

	cb_fn(cb_arg, lvol, 0);

	return 0;
}

static void
lvol_store_op_complete(void *cb_arg, int lvserrno)
{
	g_lvserrno = lvserrno;
	return;
}

static void
lvol_store_op_with_handle_complete(void *cb_arg, struct spdk_lvol_store *lvs, int lvserrno)
{
	g_lvserrno = lvserrno;
	g_lvol_store = lvs;
	return;
}

static void
vbdev_lvol_create_complete(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	g_lvolerrno = lvolerrno;
	g_lvol = lvol;
}

static void
vbdev_lvol_resize_complete(void *cb_arg, int lvolerrno)
{
	g_lvolerrno = lvolerrno;
}

static void
ut_lvol_init(void)
{
	uuid_t wrong_uuid;
	int sz = 10;
	int rc;

	g_lvs = calloc(1, sizeof(*g_lvs));
	g_lvs_bdev = calloc(1, sizeof(*g_lvs_bdev));
	g_base_bdev = calloc(1, sizeof(*g_base_bdev));

	g_lvs_bdev->lvs = g_lvs;
	g_lvs_bdev->bdev = g_base_bdev;

	SPDK_CU_ASSERT_FATAL(g_lvs != NULL);

	uuid_generate_time(g_lvs->uuid);
	uuid_generate_time(wrong_uuid);

	/* Incorrect uuid set */
	g_lvolerrno = 0;
	rc = vbdev_lvol_create(wrong_uuid, sz, vbdev_lvol_create_complete, NULL);
	CU_ASSERT(rc == -ENODEV);

	TAILQ_INSERT_TAIL(&g_spdk_lvol_pairs, g_lvs_bdev, lvol_stores);

	/* Successful lvol create */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(g_lvs->uuid, sz, vbdev_lvol_create_complete, NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);

	/* Successful lvol destruct */
	vbdev_lvol_destruct(g_lvol);
	CU_ASSERT(g_lvol == NULL);

	TAILQ_REMOVE(&g_spdk_lvol_pairs, g_lvs_bdev, lvol_stores);

	free(g_lvs);
	free(g_lvs_bdev);
	free(g_base_bdev);


}

static void
ut_lvol_hotremove(void)
{
	int rc = 0;

	lvol_store_initialize_fail = false;
	lvol_store_initialize_cb_fail = false;
	lvol_already_opened = false;
	g_bs_dev = NULL;

	/* Lvol store is succesfully created */
	rc = vbdev_lvs_create(&g_bdev, 0, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store != NULL);
	CU_ASSERT(g_bs_dev != NULL);

	/* Hot remove callback with NULL - stability check */
	vbdev_lvs_hotremove_cb(NULL);

	/* Hot remove lvs on bdev removal */
	vbdev_lvs_hotremove_cb(&g_bdev);

	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_lvol_pairs));

}

static void
ut_lvol_resize(void)
{
	int sz = 10;
	int rc = 0;

	g_lvs = calloc(1, sizeof(*g_lvs));
	g_lvs_bdev = calloc(1, sizeof(*g_lvs_bdev));
	g_base_bdev = calloc(1, sizeof(*g_base_bdev));
	g_lvs_bdev->lvs = g_lvs;
	g_lvs_bdev->bdev = g_base_bdev;


	uuid_generate_time(g_lvs->uuid);
	g_base_bdev->blocklen = 4096;
	TAILQ_INSERT_TAIL(&g_spdk_lvol_pairs, g_lvs_bdev, lvol_stores);

	/* Successful lvol create */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(g_lvs->uuid, sz, vbdev_lvol_create_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvolerrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	g_base_bdev->ctxt = g_lvol;

	g_base_bdev->name = spdk_sprintf_alloc("%s", g_lvol->name);

	/* Successful lvol resize */
	rc = vbdev_lvol_resize(g_lvol->name, 20, vbdev_lvol_resize_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_base_bdev->blockcnt == 20 * g_cluster_size / g_base_bdev->blocklen);

	/* Resize with wrong bdev name */
	rc = vbdev_lvol_resize("wrong name", 20, vbdev_lvol_resize_complete, NULL);
	CU_ASSERT(rc != 0);

	/* Resize with correct bdev name, but wrong lvol name */
	sprintf(g_lvol->name, "wrong name");
	rc = vbdev_lvol_resize(g_base_bdev->name, 20, vbdev_lvol_resize_complete, NULL);
	CU_ASSERT(rc != 0);

	/* Successful lvol destruct */
	vbdev_lvol_destruct(g_lvol);
	CU_ASSERT(g_lvol == NULL);

	TAILQ_REMOVE(&g_spdk_lvol_pairs, g_lvs_bdev, lvol_stores);
	free(g_lvs);
	free(g_lvs_bdev);
	free(g_base_bdev->name);
	free(g_base_bdev);
}

static void
ut_lvs_init(void)
{
	int rc = 0;
	struct spdk_lvol_store *lvs;
	struct spdk_bs_dev *bs_dev_temp;

	/* spdk_lvs_init() fails */
	lvol_store_initialize_fail = true;

	rc = vbdev_lvs_create(&g_bdev, 0, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(g_bs_dev == NULL);

	lvol_store_initialize_fail = false;

	/* spdk_lvs_init_cb() fails */
	lvol_store_initialize_cb_fail = true;

	rc = vbdev_lvs_create(&g_bdev, 0, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno != 0);
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(g_bs_dev == NULL);

	lvol_store_initialize_cb_fail = false;

	/* Lvol store is succesfully created */
	rc = vbdev_lvs_create(&g_bdev, 0, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store != NULL);
	CU_ASSERT(g_bs_dev != NULL);

	lvs = g_lvol_store;
	g_lvol_store = NULL;
	bs_dev_temp = g_bs_dev;
	g_bs_dev = NULL;

	/* Bdev with lvol store already claimed */
	rc = vbdev_lvs_create(&g_bdev, 0, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(g_bs_dev == NULL);

	/* Destruct lvol store */
	g_bs_dev = bs_dev_temp;

	vbdev_lvs_destruct(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
	free(g_bs_dev);

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
		CU_add_test(suite, "ut_lvs_init", ut_lvs_init) == NULL ||
		CU_add_test(suite, "ut_lvol_init", ut_lvol_init) == NULL ||
		CU_add_test(suite, "ut_lvol_resize", ut_lvol_resize) == NULL ||
		CU_add_test(suite, "lvol_hotremove", ut_lvol_hotremove) == NULL
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
