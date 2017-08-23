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
int g_cluster_size;
struct spdk_lvol_store *g_lvs = NULL;
struct spdk_lvol *g_lvol = NULL;
struct lvol_store_bdev *g_lvs_bdev = NULL;
struct spdk_bdev *g_base_bdev = NULL;

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
	free(g_lvs_bdev);
	free(g_base_bdev->name);
	free(g_base_bdev);
	SPDK_CU_ASSERT_FATAL(lvol == g_lvol);
	free(lvol->name);
	free(lvol);
	g_lvol = NULL;
}

struct lvol_store_bdev *
vbdev_get_lvs_bdev_by_lvs(struct spdk_lvol_store *lvs)
{

	g_base_bdev = calloc(1, sizeof(*g_base_bdev));
	SPDK_CU_ASSERT_FATAL(g_base_bdev != NULL);
	g_base_bdev->blocklen = 4096;

	g_lvs_bdev = calloc(1, sizeof(*g_lvs_bdev));
	SPDK_CU_ASSERT_FATAL(g_lvs_bdev != NULL);
	g_lvs_bdev->bdev = g_base_bdev;
	g_lvs_bdev->lvs = lvs;

	return g_lvs_bdev;
}

bool
is_bdev_opened(struct spdk_bdev *bdev)
{
	struct spdk_bdev *base;

	if (bdev->bdev_opened) {
		return true;
	}

	TAILQ_FOREACH(base, &bdev->base_bdevs, base_bdev_link) {
		if (is_bdev_opened(base)) {
			return true;
		}
	}

	return false;
}

struct spdk_lvol *
vbdev_get_lvol_by_name(const char *name)
{
	if (!strcmp(g_lvol->name, name)) {
		return g_lvol;
	}

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
vbdev_get_lvol_store_by_uuid(uuid_t uuid)
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

	SPDK_CU_ASSERT_FATAL(lvol != NULL);

	lvol->lvol_store = lvs;
	lvol->sz = sz * 1024 * 1024;
	lvol->name = spdk_sprintf_alloc("%s", "UNIT_TEST_UUID");
	SPDK_CU_ASSERT_FATAL(lvol->name != NULL);

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
vbdev_lvol_resize_complete(void *cb_arg, int lvolerrno)
{
	g_lvolerrno = lvolerrno;
}

static void
lvol_init(void)
{
	uuid_t wrong_uuid;
	int sz = 10;
	int rc;

	g_lvs = calloc(1, sizeof(*g_lvs));
	SPDK_CU_ASSERT_FATAL(g_lvs != NULL);

	uuid_generate_time(g_lvs->uuid);
	uuid_generate_time(wrong_uuid);
	g_lvs->page_size = 4096;

	/* Incorrect uuid set */
	g_lvolerrno = 0;
	rc = vbdev_lvol_create(wrong_uuid, sz, vbdev_lvol_create_complete, NULL);
	CU_ASSERT(rc == -ENODEV);

	/* Successful lvol create */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(g_lvs->uuid, sz, vbdev_lvol_create_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);

	/* Successful lvol destruct */
	vbdev_lvol_destruct(g_lvol);
	CU_ASSERT(g_lvol == NULL);

	free(g_lvs);
}

static void
lvol_resize(void)
{
	int sz = 10;
	int rc = 0;

	g_lvs = calloc(1, sizeof(*g_lvs));

	uuid_generate_time(g_lvs->uuid);
	g_lvs->page_size = 4096;

	/* Successful lvol create */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(g_lvs->uuid, sz, vbdev_lvol_create_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvolerrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	g_base_bdev->bdev_opened = false;
	g_base_bdev->ctxt = g_lvol;

	g_base_bdev->name = spdk_sprintf_alloc("%s", g_lvol->name);

	/* Successful lvol resize */
	rc = vbdev_lvol_resize(g_lvol->name, 20, vbdev_lvol_resize_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_base_bdev->blockcnt == 20 * g_cluster_size / g_base_bdev->blocklen);

	/* Resize while bdev is open */
	g_base_bdev->bdev_opened = true;
	rc = vbdev_lvol_resize(g_lvol->name, 20, vbdev_lvol_resize_complete, NULL);
	CU_ASSERT(rc != 0);

	/* Resize with wrong bdev name */
	g_base_bdev->bdev_opened = false;
	rc = vbdev_lvol_resize("wrong name", 20, vbdev_lvol_resize_complete, NULL);
	CU_ASSERT(rc != 0);

	/* Resize with correct bdev name, but wrong lvol name */
	sprintf(g_lvol->name, "wrong name");
	rc = vbdev_lvol_resize(g_base_bdev->name, 20, vbdev_lvol_resize_complete, NULL);
	CU_ASSERT(rc != 0);

	/* Successful lvol destruct */
	vbdev_lvol_destruct(g_lvol);
	CU_ASSERT(g_lvol == NULL);

	free(g_lvs);
}

static void
lvol_submit_io(void)
{
	struct spdk_bdev_io *io = NULL;
	struct spdk_bdev *bdev = NULL;

	io = calloc(1, sizeof(*io));
	bdev = calloc(1, sizeof(struct spdk_bdev));

	io->bdev = bdev;
	io->type = SPDK_BDEV_IO_TYPE_READ;

	/* Successful io operation */
	vbdev_lvol_submit_request(NULL, io);

	free(io);
	free(bdev);
}


static void
lvol_submit_io_read(void)
{
	struct spdk_bdev_io *io = NULL;
	struct spdk_bdev *bdev = NULL;
	struct spdk_lvol *lvol = NULL;
	struct spdk_lvol_store *lvs = NULL;
	struct lvol_task *task = NULL;

	io = calloc(1, sizeof(*io));
	bdev = calloc(1, sizeof(struct spdk_bdev));
	lvol = calloc(1, sizeof(struct spdk_lvol));
	lvs = calloc(1, sizeof(struct spdk_lvol_store));
	task = calloc(1, sizeof(struct lvol_task));

	io->type = SPDK_BDEV_IO_TYPE_READ;
	io->bdev = bdev;
	bdev->ctxt = lvol;
	lvol->lvol_store = lvs;
	lvs->page_size = 4096;
	*(struct lvol_task **)io->driver_ctx = task;

	/* Successful read from lvol */
	lvol_read(NULL, io);

	CU_ASSERT(task->status != SPDK_BDEV_IO_STATUS_SUCCESS);

	free(io);
	free(bdev);
	free(lvol);
	free(lvs);
	free(task);
}

static void
lvol_submit_io_write(void)
{

	struct spdk_bdev_io *io = NULL;
	struct spdk_bdev *bdev = NULL;
	struct spdk_lvol *lvol = NULL;
	struct spdk_lvol_store *lvs = NULL;
	struct lvol_task *task = NULL;

	io = calloc(1, sizeof(*io));
	bdev = calloc(1, sizeof(struct spdk_bdev));
	lvol = calloc(1, sizeof(struct spdk_lvol));
	lvs = calloc(1, sizeof(struct spdk_lvol_store));
	task = calloc(1, sizeof(struct lvol_task));

	io->type = SPDK_BDEV_IO_TYPE_WRITE;
	io->bdev = bdev;
	bdev->ctxt = lvol;
	lvol->lvol_store = lvs;
	lvs->page_size = 4096;
	*(struct lvol_task **)io->driver_ctx = task;

	/* Successful write to lvol */
	lvol_write(lvol, NULL, io);

	CU_ASSERT(task->status != SPDK_BDEV_IO_STATUS_SUCCESS);

	free(io);
	free(bdev);
	free(lvol);
	free(lvs);
	free(task);
}
static void
lvol_submit_io_flush(void)
{
	struct spdk_bdev_io *io = NULL;
	struct lvol_task *task = NULL;

	io = calloc(1, sizeof(*io));
	task = calloc(1, sizeof(struct lvol_task));

	*(struct lvol_task **)io->driver_ctx = task;
	/* Successful flush to lvol */
	lvol_flush(NULL, io);

	CU_ASSERT(task->status != SPDK_BDEV_IO_STATUS_SUCCESS);

	free(io);
	free(task);
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
		CU_add_test(suite, "lvol_init", lvol_init) == NULL ||
		CU_add_test(suite, "lvol_resize", lvol_resize) == NULL ||
		CU_add_test(suite, "lvol_submit_io", lvol_submit_io) == NULL ||
		CU_add_test(suite, "lvol_submit_io_read", lvol_submit_io_read) == NULL ||
		CU_add_test(suite, "lvol_submit_io_write", lvol_submit_io_write) == NULL ||
		CU_add_test(suite, "lvol_submit_io_flush", lvol_submit_io_flush) == NULL
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
