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

#include "bdev/lvol/vbdev_lvol.c"

#include "unit/lib/json_mock.c"

#define SPDK_BS_PAGE_SIZE 0x1000

int g_lvolerrno;
int g_lvserrno;
int g_cluster_size;
int g_registered_bdevs;
int g_num_lvols = 0;
struct spdk_lvol_store *g_lvs = NULL;
struct spdk_lvol *g_lvol = NULL;
struct lvol_store_bdev *g_lvs_bdev = NULL;
struct spdk_bdev *g_base_bdev = NULL;
struct spdk_bdev_io *g_io = NULL;
struct spdk_io_channel *g_ch = NULL;

static struct spdk_bdev g_bdev = {};
static struct spdk_lvol_store *g_lvol_store = NULL;
bool lvol_store_initialize_fail = false;
bool lvol_store_initialize_cb_fail = false;
bool lvol_already_opened = false;
bool g_examine_done = false;
bool g_bdev_alias_already_exists = false;
bool g_lvs_with_name_already_exists = false;

int
spdk_bdev_alias_add(struct spdk_bdev *bdev, const char *alias)
{
	struct spdk_bdev_alias *tmp;

	CU_ASSERT(alias != NULL);
	CU_ASSERT(bdev != NULL);
	if (g_bdev_alias_already_exists) {
		return -EEXIST;
	}

	tmp = calloc(1, sizeof(*tmp));
	SPDK_CU_ASSERT_FATAL(tmp != NULL);

	tmp->alias = strdup(alias);
	SPDK_CU_ASSERT_FATAL(tmp->alias != NULL);

	TAILQ_INSERT_TAIL(&bdev->aliases, tmp, tailq);

	return 0;
}

int
spdk_bdev_alias_del(struct spdk_bdev *bdev, const char *alias)
{
	struct spdk_bdev_alias *tmp;

	CU_ASSERT(bdev != NULL);

	TAILQ_FOREACH(tmp, &bdev->aliases, tailq) {
		SPDK_CU_ASSERT_FATAL(alias != NULL);
		if (strncmp(alias, tmp->alias, SPDK_LVOL_NAME_MAX) == 0) {
			TAILQ_REMOVE(&bdev->aliases, tmp, tailq);
			free(tmp->alias);
			free(tmp);
			return 0;
		}
	}

	return -ENOENT;
}

void
spdk_bdev_alias_del_all(struct spdk_bdev *bdev)
{
	struct spdk_bdev_alias *p, *tmp;

	TAILQ_FOREACH_SAFE(p, &bdev->aliases, tailq, tmp) {
		TAILQ_REMOVE(&bdev->aliases, p, tailq);
		free(p->alias);
		free(p);
	}
}

void
spdk_bdev_destruct_done(struct spdk_bdev *bdev, int bdeverrno)
{
}

void
spdk_lvs_rename(struct spdk_lvol_store *lvs, const char *new_name,
		spdk_lvs_op_complete cb_fn, void *cb_arg)
{
	if (g_lvs_with_name_already_exists) {
		g_lvolerrno = -EEXIST;
	} else {
		snprintf(lvs->name, sizeof(lvs->name), "%s", new_name);
		g_lvolerrno = 0;
	}

	cb_fn(cb_arg, g_lvolerrno);
}

void
spdk_lvol_rename(struct spdk_lvol *lvol, const char *new_name,
		 spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol *tmp;

	if (strncmp(lvol->name, new_name, SPDK_LVOL_NAME_MAX) == 0) {
		cb_fn(cb_arg, 0);
		return;
	}

	TAILQ_FOREACH(tmp, &lvol->lvol_store->lvols, link) {
		if (strncmp(tmp->name, new_name, SPDK_LVOL_NAME_MAX) == 0) {
			SPDK_ERRLOG("Lvol %s already exists in lvol store %s\n", new_name, lvol->lvol_store->name);
			cb_fn(cb_arg, -EEXIST);
			return;
		}
	}

	snprintf(lvol->name, sizeof(lvol->name), "%s", new_name);

	cb_fn(cb_arg, g_lvolerrno);
}

void
spdk_lvol_open(struct spdk_lvol *lvol, spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, lvol, g_lvolerrno);
}

uint64_t
spdk_blob_get_num_clusters(struct spdk_blob *b)
{
	return 0;
}

int
spdk_blob_get_clones(struct spdk_blob_store *bs, spdk_blob_id blobid, spdk_blob_id *ids,
		     size_t *count)
{
	*count = 0;
	return 0;
}

spdk_blob_id
spdk_blob_get_parent_snapshot(struct spdk_blob_store *bs, spdk_blob_id blobid)
{
	return 0;
}

bool g_blob_is_read_only = false;

bool
spdk_blob_is_read_only(struct spdk_blob *blob)
{
	return g_blob_is_read_only;
}

bool
spdk_blob_is_snapshot(struct spdk_blob *blob)
{
	return false;
}

bool
spdk_blob_is_clone(struct spdk_blob *blob)
{
	return false;
}

bool
spdk_blob_is_thin_provisioned(struct spdk_blob *blob)
{
	return false;
}

static struct spdk_lvol *_lvol_create(struct spdk_lvol_store *lvs);

void
spdk_lvs_load(struct spdk_bs_dev *dev,
	      spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store *lvs = NULL;
	int i;
	int lvserrno = g_lvserrno;

	if (lvserrno != 0) {
		/* On error blobstore destroys bs_dev itself,
		 * by puttin back io channels.
		 * This operation is asynchronous, and completed
		 * after calling the callback for lvol. */
		cb_fn(cb_arg, g_lvol_store, lvserrno);
		dev->destroy(dev);
		return;
	}

	lvs = calloc(1, sizeof(*lvs));
	SPDK_CU_ASSERT_FATAL(lvs != NULL);
	TAILQ_INIT(&lvs->lvols);
	TAILQ_INIT(&lvs->pending_lvols);
	spdk_uuid_generate(&lvs->uuid);
	lvs->bs_dev = dev;
	for (i = 0; i < g_num_lvols; i++) {
		_lvol_create(lvs);
	}

	cb_fn(cb_arg, lvs, lvserrno);
}

int
spdk_bs_bdev_claim(struct spdk_bs_dev *bs_dev, struct spdk_bdev_module *module)
{
	if (lvol_already_opened == true) {
		return -1;
	}

	lvol_already_opened = true;

	return 0;
}

void
spdk_bdev_unregister(struct spdk_bdev *vbdev, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	int rc;

	SPDK_CU_ASSERT_FATAL(vbdev != NULL);
	rc = vbdev->fn_table->destruct(vbdev->ctxt);

	SPDK_CU_ASSERT_FATAL(cb_fn != NULL);
	cb_fn(cb_arg, rc);
}

void
spdk_bdev_module_finish_done(void)
{
	return;
}

uint64_t
spdk_bs_get_page_size(struct spdk_blob_store *bs)
{
	return SPDK_BS_PAGE_SIZE;
}

uint64_t
spdk_bs_get_io_unit_size(struct spdk_blob_store *bs)
{
	return SPDK_BS_PAGE_SIZE;
}

static void
bdev_blob_destroy(struct spdk_bs_dev *bs_dev)
{
	CU_ASSERT(bs_dev != NULL);
	free(bs_dev);
	lvol_already_opened = false;
}

static struct spdk_bdev *
bdev_blob_get_base_bdev(struct spdk_bs_dev *bs_dev)
{
	CU_ASSERT(bs_dev != NULL);
	return &g_bdev;
}

int
spdk_bdev_create_bs_dev_ext(const char *bdev_name, spdk_bdev_event_cb_t event_cb,
			    void *event_ctx, struct spdk_bs_dev **_bs_dev)
{
	struct spdk_bs_dev *bs_dev;

	if (lvol_already_opened == true) {
		return -EINVAL;
	}

	bs_dev = calloc(1, sizeof(*bs_dev));
	SPDK_CU_ASSERT_FATAL(bs_dev != NULL);
	bs_dev->destroy = bdev_blob_destroy;
	bs_dev->get_base_bdev = bdev_blob_get_base_bdev;

	*_bs_dev = bs_dev;

	return 0;
}

void
spdk_lvs_opts_init(struct spdk_lvs_opts *opts)
{
}

int
spdk_lvs_init(struct spdk_bs_dev *bs_dev, struct spdk_lvs_opts *o,
	      spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store *lvs;
	int error = 0;

	if (lvol_store_initialize_fail) {
		return -1;
	}

	if (lvol_store_initialize_cb_fail) {
		bs_dev->destroy(bs_dev);
		lvs = NULL;
		error = -1;
	} else {
		lvs = calloc(1, sizeof(*lvs));
		SPDK_CU_ASSERT_FATAL(lvs != NULL);
		TAILQ_INIT(&lvs->lvols);
		TAILQ_INIT(&lvs->pending_lvols);
		spdk_uuid_generate(&lvs->uuid);
		snprintf(lvs->name, sizeof(lvs->name), "%s", o->name);
		lvs->bs_dev = bs_dev;
		error = 0;
	}
	cb_fn(cb_arg, lvs, error);

	return 0;
}

int
spdk_lvs_unload(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol *lvol, *tmp;

	TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
		TAILQ_REMOVE(&lvs->lvols, lvol, link);
		free(lvol);
	}
	g_lvol_store = NULL;

	lvs->bs_dev->destroy(lvs->bs_dev);
	free(lvs);

	if (cb_fn != NULL) {
		cb_fn(cb_arg, 0);
	}

	return 0;
}

int
spdk_lvs_destroy(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn,
		 void *cb_arg)
{
	struct spdk_lvol *lvol, *tmp;
	char *alias;

	TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
		TAILQ_REMOVE(&lvs->lvols, lvol, link);

		alias = spdk_sprintf_alloc("%s/%s", lvs->name, lvol->name);
		if (alias == NULL) {
			SPDK_ERRLOG("Cannot alloc memory for alias\n");
			return -1;
		}
		spdk_bdev_alias_del(lvol->bdev, alias);

		free(alias);
		free(lvol);
	}
	g_lvol_store = NULL;

	lvs->bs_dev->destroy(lvs->bs_dev);
	free(lvs);

	if (cb_fn != NULL) {
		cb_fn(cb_arg, 0);
	}

	return 0;
}

void
spdk_lvol_resize(struct spdk_lvol *lvol, size_t sz,  spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, 0);
}

void
spdk_lvol_set_read_only(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, 0);
}

int
spdk_bdev_notify_blockcnt_change(struct spdk_bdev *bdev, uint64_t size)
{
	bdev->blockcnt = size;
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
spdk_lvol_close(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	lvol->ref_count--;

	SPDK_CU_ASSERT_FATAL(cb_fn != NULL);
	cb_fn(cb_arg, 0);
}

bool
spdk_lvol_deletable(struct spdk_lvol *lvol)
{
	return true;
}

void
spdk_lvol_destroy(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	if (lvol->ref_count != 0) {
		cb_fn(cb_arg, -ENODEV);
	}

	TAILQ_REMOVE(&lvol->lvol_store->lvols, lvol, link);

	SPDK_CU_ASSERT_FATAL(cb_fn != NULL);
	cb_fn(cb_arg, 0);

	g_lvol = NULL;
	free(lvol);
}

void
spdk_bdev_io_complete(struct spdk_bdev_io *bdev_io, enum spdk_bdev_io_status status)
{
	bdev_io->internal.status = status;
}

struct spdk_io_channel *spdk_lvol_get_io_channel(struct spdk_lvol *lvol)
{
	CU_ASSERT(lvol == g_lvol);
	return g_ch;
}

void
spdk_bdev_io_get_buf(struct spdk_bdev_io *bdev_io, spdk_bdev_io_get_buf_cb cb, uint64_t len)
{
	CU_ASSERT(cb == lvol_get_buf_cb);
}

void
spdk_blob_io_read(struct spdk_blob *blob, struct spdk_io_channel *channel,
		  void *payload, uint64_t offset, uint64_t length,
		  spdk_blob_op_complete cb_fn, void *cb_arg)
{
	CU_ASSERT(blob == NULL);
	CU_ASSERT(channel == g_ch);
	CU_ASSERT(offset == g_io->u.bdev.offset_blocks);
	CU_ASSERT(length == g_io->u.bdev.num_blocks);
	cb_fn(cb_arg, 0);
}

void
spdk_blob_io_write(struct spdk_blob *blob, struct spdk_io_channel *channel,
		   void *payload, uint64_t offset, uint64_t length,
		   spdk_blob_op_complete cb_fn, void *cb_arg)
{
	CU_ASSERT(blob == NULL);
	CU_ASSERT(channel == g_ch);
	CU_ASSERT(offset == g_io->u.bdev.offset_blocks);
	CU_ASSERT(length == g_io->u.bdev.num_blocks);
	cb_fn(cb_arg, 0);
}

void
spdk_blob_io_unmap(struct spdk_blob *blob, struct spdk_io_channel *channel,
		   uint64_t offset, uint64_t length, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	CU_ASSERT(blob == NULL);
	CU_ASSERT(channel == g_ch);
	CU_ASSERT(offset == g_io->u.bdev.offset_blocks);
	CU_ASSERT(length == g_io->u.bdev.num_blocks);
	cb_fn(cb_arg, 0);
}

void
spdk_blob_io_write_zeroes(struct spdk_blob *blob, struct spdk_io_channel *channel,
			  uint64_t offset, uint64_t length, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	CU_ASSERT(blob == NULL);
	CU_ASSERT(channel == g_ch);
	CU_ASSERT(offset == g_io->u.bdev.offset_blocks);
	CU_ASSERT(length == g_io->u.bdev.num_blocks);
	cb_fn(cb_arg, 0);
}

void
spdk_blob_io_writev(struct spdk_blob *blob, struct spdk_io_channel *channel,
		    struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
		    spdk_blob_op_complete cb_fn, void *cb_arg)
{
	CU_ASSERT(blob == NULL);
	CU_ASSERT(channel == g_ch);
	CU_ASSERT(offset == g_io->u.bdev.offset_blocks);
	CU_ASSERT(length == g_io->u.bdev.num_blocks);
	cb_fn(cb_arg, 0);
}

void
spdk_blob_io_readv(struct spdk_blob *blob, struct spdk_io_channel *channel,
		   struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
		   spdk_blob_op_complete cb_fn, void *cb_arg)
{
	CU_ASSERT(blob == NULL);
	CU_ASSERT(channel == g_ch);
	CU_ASSERT(offset == g_io->u.bdev.offset_blocks);
	CU_ASSERT(length == g_io->u.bdev.num_blocks);
	cb_fn(cb_arg, 0);
}

void
spdk_bdev_module_list_add(struct spdk_bdev_module *bdev_module)
{
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return "test";
}

int
spdk_bdev_register(struct spdk_bdev *vbdev)
{
	TAILQ_INIT(&vbdev->aliases);

	g_registered_bdevs++;
	return 0;
}

void
spdk_bdev_module_examine_done(struct spdk_bdev_module *module)
{
	SPDK_CU_ASSERT_FATAL(g_examine_done != true);
	g_examine_done = true;
}

static struct spdk_lvol *
_lvol_create(struct spdk_lvol_store *lvs)
{
	struct spdk_lvol *lvol = calloc(1, sizeof(*lvol));

	SPDK_CU_ASSERT_FATAL(lvol != NULL);

	lvol->lvol_store = lvs;
	lvol->ref_count++;
	snprintf(lvol->unique_id, sizeof(lvol->unique_id), "%s", "UNIT_TEST_UUID");

	TAILQ_INSERT_TAIL(&lvol->lvol_store->lvols, lvol, link);

	return lvol;
}

int
spdk_lvol_create(struct spdk_lvol_store *lvs, const char *name, size_t sz,
		 bool thin_provision, enum lvol_clear_method clear_method, spdk_lvol_op_with_handle_complete cb_fn,
		 void *cb_arg)
{
	struct spdk_lvol *lvol;

	lvol = _lvol_create(lvs);
	snprintf(lvol->name, sizeof(lvol->name), "%s", name);
	cb_fn(cb_arg, lvol, 0);

	return 0;
}

void
spdk_lvol_create_snapshot(struct spdk_lvol *lvol, const char *snapshot_name,
			  spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol *snap;

	snap = _lvol_create(lvol->lvol_store);
	snprintf(snap->name, sizeof(snap->name), "%s", snapshot_name);
	cb_fn(cb_arg, snap, 0);
}

void
spdk_lvol_create_clone(struct spdk_lvol *lvol, const char *clone_name,
		       spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol *clone;

	clone = _lvol_create(lvol->lvol_store);
	snprintf(clone->name, sizeof(clone->name), "%s", clone_name);
	cb_fn(cb_arg, clone, 0);
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
vbdev_lvol_set_read_only_complete(void *cb_arg, int lvolerrno)
{
	g_lvolerrno = lvolerrno;
}

static void
vbdev_lvol_rename_complete(void *cb_arg, int lvolerrno)
{
	g_lvolerrno = lvolerrno;
}

static void
ut_lvs_destroy(void)
{
	int rc = 0;
	int sz = 10;
	struct spdk_lvol_store *lvs;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, lvol_store_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);

	lvs = g_lvol_store;
	g_lvol_store = NULL;

	spdk_uuid_generate(&lvs->uuid);

	/* Successfully create lvol, which should be unloaded with lvs later */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol", sz, false, LVOL_CLEAR_WITH_DEFAULT, vbdev_lvol_create_complete,
			       NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvolerrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	/* Unload lvol store */
	vbdev_lvs_destruct(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
}

static void
ut_lvol_init(void)
{
	struct spdk_lvol_store *lvs;
	int sz = 10;
	int rc;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, lvol_store_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);
	lvs = g_lvol_store;

	/* Successful lvol create */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol", sz, false, LVOL_CLEAR_WITH_DEFAULT, vbdev_lvol_create_complete,
			       NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);

	/* Successful lvol destroy */
	vbdev_lvol_destroy(g_lvol, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvol == NULL);

	/* Destroy lvol store */
	vbdev_lvs_destruct(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
}

static void
ut_lvol_snapshot(void)
{
	struct spdk_lvol_store *lvs;
	int sz = 10;
	int rc;
	struct spdk_lvol *lvol = NULL;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, lvol_store_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);
	lvs = g_lvol_store;

	/* Successful lvol create */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol", sz, false, LVOL_CLEAR_WITH_DEFAULT, vbdev_lvol_create_complete,
			       NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);

	lvol = g_lvol;

	/* Successful snap create */
	vbdev_lvol_create_snapshot(lvol, "snap", vbdev_lvol_create_complete, NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);

	/* Successful lvol destroy */
	vbdev_lvol_destroy(g_lvol, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvol == NULL);

	/* Successful snap destroy */
	g_lvol = lvol;
	vbdev_lvol_destroy(g_lvol, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvol == NULL);

	/* Destroy lvol store */
	vbdev_lvs_destruct(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
}

static void
ut_lvol_clone(void)
{
	struct spdk_lvol_store *lvs;
	int sz = 10;
	int rc;
	struct spdk_lvol *lvol = NULL;
	struct spdk_lvol *snap = NULL;
	struct spdk_lvol *clone = NULL;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, lvol_store_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);
	lvs = g_lvol_store;

	/* Successful lvol create */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol", sz, false, LVOL_CLEAR_WITH_DEFAULT, vbdev_lvol_create_complete,
			       NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);

	lvol = g_lvol;

	/* Successful snap create */
	vbdev_lvol_create_snapshot(lvol, "snap", vbdev_lvol_create_complete, NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);

	snap = g_lvol;

	/* Successful clone create */
	vbdev_lvol_create_clone(snap, "clone", vbdev_lvol_create_complete, NULL);

	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);

	clone = g_lvol;

	/* Successful lvol destroy */
	g_lvol = lvol;
	vbdev_lvol_destroy(g_lvol, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvol == NULL);

	/* Successful clone destroy */
	g_lvol = clone;
	vbdev_lvol_destroy(g_lvol, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvol == NULL);

	/* Successful lvol destroy */
	g_lvol = snap;
	vbdev_lvol_destroy(g_lvol, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvol == NULL);

	/* Destroy lvol store */
	vbdev_lvs_destruct(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
}

static void
ut_lvol_hotremove(void)
{
	int rc = 0;

	lvol_store_initialize_fail = false;
	lvol_store_initialize_cb_fail = false;
	lvol_already_opened = false;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, lvol_store_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);

	/* Hot remove callback with NULL - stability check */
	vbdev_lvs_hotremove_cb(NULL);

	/* Hot remove lvs on bdev removal */
	vbdev_lvs_hotremove_cb(&g_bdev);

	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_lvol_pairs));

}

static void
ut_lvs_examine_check(bool success)
{
	struct lvol_store_bdev *lvs_bdev;

	/* Examine was finished regardless of result */
	CU_ASSERT(g_examine_done == true);
	g_examine_done = false;

	if (success) {
		SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&g_spdk_lvol_pairs));
		lvs_bdev = TAILQ_FIRST(&g_spdk_lvol_pairs);
		SPDK_CU_ASSERT_FATAL(lvs_bdev != NULL);
		g_lvol_store = lvs_bdev->lvs;
		SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
		CU_ASSERT(g_lvol_store->bs_dev != NULL);
	} else {
		SPDK_CU_ASSERT_FATAL(TAILQ_EMPTY(&g_spdk_lvol_pairs));
		g_lvol_store = NULL;
	}
}

static void
ut_lvol_examine(void)
{
	/* Examine unsuccessfully - bdev already opened */
	g_lvserrno = -1;
	lvol_already_opened = true;
	vbdev_lvs_examine(&g_bdev);
	ut_lvs_examine_check(false);

	/* Examine unsuccessfully - fail on lvol store */
	g_lvserrno = -1;
	lvol_already_opened = false;
	vbdev_lvs_examine(&g_bdev);
	ut_lvs_examine_check(false);

	/* Examine successfully
	 * - one lvol fails to load
	 * - lvs is loaded with no lvols present */
	g_lvserrno = 0;
	g_lvolerrno = -1;
	g_num_lvols = 1;
	lvol_already_opened = false;
	g_registered_bdevs = 0;
	vbdev_lvs_examine(&g_bdev);
	ut_lvs_examine_check(true);
	CU_ASSERT(g_registered_bdevs == 0);
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_store->lvols));
	vbdev_lvs_destruct(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);

	/* Examine successfully */
	g_lvserrno = 0;
	g_lvolerrno = 0;
	g_registered_bdevs = 0;
	lvol_already_opened = false;
	vbdev_lvs_examine(&g_bdev);
	ut_lvs_examine_check(true);
	CU_ASSERT(g_registered_bdevs != 0);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&g_lvol_store->lvols));
	vbdev_lvs_destruct(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
}

static void
ut_lvol_rename(void)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvol *lvol;
	struct spdk_lvol *lvol2;
	int sz = 10;
	int rc;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, lvol_store_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);
	lvs = g_lvol_store;

	/* Successful lvols create */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol", sz, false, LVOL_CLEAR_WITH_DEFAULT, vbdev_lvol_create_complete,
			       NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);
	lvol = g_lvol;

	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol2", sz, false, LVOL_CLEAR_WITH_DEFAULT, vbdev_lvol_create_complete,
			       NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);
	lvol2 = g_lvol;

	/* Successful rename lvol */
	vbdev_lvol_rename(lvol, "new_lvol_name", vbdev_lvol_rename_complete, NULL);
	SPDK_CU_ASSERT_FATAL(g_lvolerrno == 0);
	CU_ASSERT_STRING_EQUAL(lvol->name, "new_lvol_name");

	/* Renaming lvol with name already existing */
	g_bdev_alias_already_exists = true;
	vbdev_lvol_rename(lvol2, "new_lvol_name", vbdev_lvol_rename_complete, NULL);
	g_bdev_alias_already_exists = false;
	SPDK_CU_ASSERT_FATAL(g_lvolerrno != 0);
	CU_ASSERT_STRING_NOT_EQUAL(lvol2->name, "new_lvol_name");

	/* Renaming lvol with it's own name */
	vbdev_lvol_rename(lvol, "new_lvol_name", vbdev_lvol_rename_complete, NULL);
	SPDK_CU_ASSERT_FATAL(g_lvolerrno == 0);
	CU_ASSERT_STRING_EQUAL(lvol->name, "new_lvol_name");

	/* Successful lvols destroy */
	vbdev_lvol_destroy(lvol, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvol == NULL);

	vbdev_lvol_destroy(lvol2, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvol == NULL);

	/* Destroy lvol store */
	vbdev_lvs_destruct(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
}

static void
ut_lvol_destroy(void)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvol *lvol;
	struct spdk_lvol *lvol2;
	int sz = 10;
	int rc;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, lvol_store_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);
	lvs = g_lvol_store;

	/* Successful lvols create */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol", sz, false, LVOL_CLEAR_WITH_DEFAULT, vbdev_lvol_create_complete,
			       NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);
	lvol = g_lvol;

	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol2", sz, false, LVOL_CLEAR_WITH_DEFAULT, vbdev_lvol_create_complete,
			       NULL);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(g_lvol != NULL);
	CU_ASSERT(g_lvolerrno == 0);
	lvol2 = g_lvol;

	/* Successful lvols destroy */
	vbdev_lvol_destroy(lvol, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvol == NULL);
	CU_ASSERT(g_lvolerrno == 0);

	/* Hot remove lvol bdev */
	vbdev_lvol_unregister(lvol2);

	/* Unload lvol store */
	vbdev_lvs_unload(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
}

static void
ut_lvol_resize(void)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvol *lvol;
	int sz = 10;
	int rc = 0;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, lvol_store_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);
	lvs = g_lvol_store;

	/* Successful lvol create */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol", sz, false, LVOL_CLEAR_WITH_DEFAULT, vbdev_lvol_create_complete,
			       NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvolerrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol = g_lvol;

	/* Successful lvol resize */
	g_lvolerrno = -1;
	vbdev_lvol_resize(lvol, 20, vbdev_lvol_resize_complete, NULL);
	CU_ASSERT(g_lvolerrno == 0);
	CU_ASSERT(lvol->bdev->blockcnt == 20 * g_cluster_size / lvol->bdev->blocklen);

	/* Resize with NULL lvol */
	vbdev_lvol_resize(NULL, 20, vbdev_lvol_resize_complete, NULL);
	CU_ASSERT(g_lvolerrno != 0);

	/* Successful lvol destroy */
	vbdev_lvol_destroy(lvol, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvol == NULL);

	/* Destroy lvol store */
	vbdev_lvs_destruct(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
}

static void
ut_lvol_set_read_only(void)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvol *lvol;
	int sz = 10;
	int rc = 0;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, lvol_store_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);
	lvs = g_lvol_store;

	/* Successful lvol create */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol", sz, false, LVOL_CLEAR_WITH_DEFAULT, vbdev_lvol_create_complete,
			       NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvolerrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol = g_lvol;

	/* Successful set lvol as read only */
	g_lvolerrno = -1;
	vbdev_lvol_set_read_only(lvol, vbdev_lvol_set_read_only_complete, NULL);
	CU_ASSERT(g_lvolerrno == 0);

	/* Successful lvol destroy */
	vbdev_lvol_destroy(lvol, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvol == NULL);

	/* Destroy lvol store */
	vbdev_lvs_destruct(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
}

static void
ut_lvs_unload(void)
{
	int rc = 0;
	int sz = 10;
	struct spdk_lvol_store *lvs;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, lvol_store_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);

	lvs = g_lvol_store;
	g_lvol_store = NULL;

	spdk_uuid_generate(&lvs->uuid);

	/* Successfully create lvol, which should be destroyed with lvs later */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol", sz, false, LVOL_CLEAR_WITH_DEFAULT, vbdev_lvol_create_complete,
			       NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvolerrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	/* Unload lvol store */
	vbdev_lvs_unload(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(g_lvol != NULL);
}

static void
ut_lvs_init(void)
{
	int rc = 0;
	struct spdk_lvol_store *lvs;

	/* spdk_lvs_init() fails */
	lvol_store_initialize_fail = true;

	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, lvol_store_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);

	lvol_store_initialize_fail = false;

	/* spdk_lvs_init_cb() fails */
	lvol_store_initialize_cb_fail = true;

	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, lvol_store_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno != 0);
	CU_ASSERT(g_lvol_store == NULL);

	lvol_store_initialize_cb_fail = false;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, lvol_store_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);

	lvs = g_lvol_store;
	g_lvol_store = NULL;

	/* Bdev with lvol store already claimed */
	rc = vbdev_lvs_create("bdev", "lvs", 0, LVS_CLEAR_WITH_UNMAP, lvol_store_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);

	/* Destruct lvol store */
	vbdev_lvs_destruct(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);
}

static void
ut_vbdev_lvol_get_io_channel(void)
{
	struct spdk_io_channel *ch;

	g_lvol = calloc(1, sizeof(struct spdk_lvol));
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	ch = vbdev_lvol_get_io_channel(g_lvol);
	CU_ASSERT(ch == g_ch);

	free(g_lvol);
}

static void
ut_vbdev_lvol_io_type_supported(void)
{
	struct spdk_lvol *lvol;
	bool ret;

	lvol = calloc(1, sizeof(struct spdk_lvol));
	SPDK_CU_ASSERT_FATAL(lvol != NULL);

	g_blob_is_read_only = false;

	/* Supported types */
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_READ);
	CU_ASSERT(ret == true);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_WRITE);
	CU_ASSERT(ret == true);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_RESET);
	CU_ASSERT(ret == true);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_UNMAP);
	CU_ASSERT(ret == true);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_WRITE_ZEROES);
	CU_ASSERT(ret == true);

	/* Unsupported types */
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_FLUSH);
	CU_ASSERT(ret == false);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_NVME_ADMIN);
	CU_ASSERT(ret == false);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_NVME_IO);
	CU_ASSERT(ret == false);

	g_blob_is_read_only = true;

	/* Supported types */
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_READ);
	CU_ASSERT(ret == true);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_RESET);
	CU_ASSERT(ret == true);

	/* Unsupported types */
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_WRITE);
	CU_ASSERT(ret == false);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_UNMAP);
	CU_ASSERT(ret == false);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_WRITE_ZEROES);
	CU_ASSERT(ret == false);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_FLUSH);
	CU_ASSERT(ret == false);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_NVME_ADMIN);
	CU_ASSERT(ret == false);
	ret = vbdev_lvol_io_type_supported(lvol, SPDK_BDEV_IO_TYPE_NVME_IO);
	CU_ASSERT(ret == false);

	free(lvol);
}

static void
ut_lvol_read_write(void)
{
	g_io = calloc(1, sizeof(struct spdk_bdev_io));
	SPDK_CU_ASSERT_FATAL(g_io != NULL);
	g_base_bdev = calloc(1, sizeof(struct spdk_bdev));
	SPDK_CU_ASSERT_FATAL(g_base_bdev != NULL);
	g_lvol = calloc(1, sizeof(struct spdk_lvol));
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	g_io->bdev = g_base_bdev;
	g_io->bdev->ctxt = g_lvol;
	g_io->u.bdev.offset_blocks = 20;
	g_io->u.bdev.num_blocks = 20;

	lvol_read(g_ch, g_io);
	CU_ASSERT(g_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS);

	lvol_write(g_lvol, g_ch, g_io);
	CU_ASSERT(g_io->internal.status = SPDK_BDEV_IO_STATUS_SUCCESS);

	free(g_io);
	free(g_base_bdev);
	free(g_lvol);
}

static void
ut_vbdev_lvol_submit_request(void)
{
	struct spdk_lvol request_lvol = {};
	g_io = calloc(1, sizeof(struct spdk_bdev_io));
	SPDK_CU_ASSERT_FATAL(g_io != NULL);
	g_base_bdev = calloc(1, sizeof(struct spdk_bdev));
	SPDK_CU_ASSERT_FATAL(g_base_bdev != NULL);
	g_io->bdev = g_base_bdev;

	g_io->type = SPDK_BDEV_IO_TYPE_READ;
	g_base_bdev->ctxt = &request_lvol;
	vbdev_lvol_submit_request(g_ch, g_io);

	free(g_io);
	free(g_base_bdev);
}

static void
ut_lvs_rename(void)
{
	int rc = 0;
	int sz = 10;
	struct spdk_lvol_store *lvs;

	/* Lvol store is successfully created */
	rc = vbdev_lvs_create("bdev", "old_lvs_name", 0, LVS_CLEAR_WITH_UNMAP,
			      lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(g_lvol_store->bs_dev != NULL);

	lvs = g_lvol_store;
	g_lvol_store = NULL;

	g_base_bdev = calloc(1, sizeof(*g_base_bdev));
	SPDK_CU_ASSERT_FATAL(g_base_bdev != NULL);

	/* Successfully create lvol, which should be destroyed with lvs later */
	g_lvolerrno = -1;
	rc = vbdev_lvol_create(lvs, "lvol", sz, false, LVOL_CLEAR_WITH_DEFAULT, vbdev_lvol_create_complete,
			       NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvolerrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	/* Trying to rename lvs with lvols created */
	vbdev_lvs_rename(lvs, "new_lvs_name", lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT_STRING_EQUAL(lvs->name, "new_lvs_name");
	CU_ASSERT_STRING_EQUAL(TAILQ_FIRST(&g_lvol->bdev->aliases)->alias, "new_lvs_name/lvol");

	/* Trying to rename lvs with name already used by another lvs */
	/* This is a bdev_lvol test, so g_lvs_with_name_already_exists simulates
	 * existing lvs with name 'another_new_lvs_name' and this name in fact is not compared */
	g_lvs_with_name_already_exists = true;
	vbdev_lvs_rename(lvs, "another_new_lvs_name", lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == -EEXIST);
	CU_ASSERT_STRING_EQUAL(lvs->name, "new_lvs_name");
	CU_ASSERT_STRING_EQUAL(TAILQ_FIRST(&g_lvol->bdev->aliases)->alias, "new_lvs_name/lvol");
	g_lvs_with_name_already_exists = false;

	/* Unload lvol store */
	g_lvol_store = lvs;
	vbdev_lvs_destruct(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store == NULL);

	free(g_base_bdev->name);
	free(g_base_bdev);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("lvol", NULL, NULL);

	CU_ADD_TEST(suite, ut_lvs_init);
	CU_ADD_TEST(suite, ut_lvol_init);
	CU_ADD_TEST(suite, ut_lvol_snapshot);
	CU_ADD_TEST(suite, ut_lvol_clone);
	CU_ADD_TEST(suite, ut_lvs_destroy);
	CU_ADD_TEST(suite, ut_lvs_unload);
	CU_ADD_TEST(suite, ut_lvol_resize);
	CU_ADD_TEST(suite, ut_lvol_set_read_only);
	CU_ADD_TEST(suite, ut_lvol_hotremove);
	CU_ADD_TEST(suite, ut_vbdev_lvol_get_io_channel);
	CU_ADD_TEST(suite, ut_vbdev_lvol_io_type_supported);
	CU_ADD_TEST(suite, ut_lvol_read_write);
	CU_ADD_TEST(suite, ut_vbdev_lvol_submit_request);
	CU_ADD_TEST(suite, ut_lvol_examine);
	CU_ADD_TEST(suite, ut_lvol_rename);
	CU_ADD_TEST(suite, ut_lvol_destroy);
	CU_ADD_TEST(suite, ut_lvs_rename);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
