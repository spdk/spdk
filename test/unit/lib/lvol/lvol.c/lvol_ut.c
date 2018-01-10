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
#include "spdk/blob.h"
#include "spdk/io_channel.h"
#include "spdk/util.h"

#include "lib/test_env.c"

#include "lvol.c"

#define DEV_BUFFER_SIZE (64 * 1024 * 1024)
#define DEV_BUFFER_BLOCKLEN (4096)
#define DEV_BUFFER_BLOCKCNT (DEV_BUFFER_SIZE / DEV_BUFFER_BLOCKLEN)
#define BS_CLUSTER_SIZE (1024 * 1024)
#define BS_FREE_CLUSTERS (DEV_BUFFER_SIZE / BS_CLUSTER_SIZE)
#define BS_PAGE_SIZE (4096)

#define SPDK_BLOB_OPTS_CLUSTER_SZ (1024 * 1024)
#define SPDK_BLOB_OPTS_NUM_MD_PAGES UINT32_MAX
#define SPDK_BLOB_OPTS_MAX_MD_OPS 32
#define SPDK_BLOB_OPTS_MAX_CHANNEL_OPS 512

#define SPDK_BLOB_THIN_PROV (1ULL << 0)

const char *uuid = "828d9766-ae50-11e7-bd8d-001e67edf350";

struct spdk_blob {
	spdk_blob_id		id;
	uint32_t		ref;
	int			close_status;
	int			open_status;
	int 			load_status;
	TAILQ_ENTRY(spdk_blob)	link;
	char			uuid[UUID_STRING_LEN];
	char			name[SPDK_LVS_NAME_MAX];
	bool			thin_provisioned;
};

int g_lvolerrno;
int g_lvserrno;
int g_close_super_status;
int g_resize_rc;
struct spdk_lvol_store *g_lvol_store;
struct spdk_lvol *g_lvol;
spdk_blob_id g_blobid = 1;

struct spdk_blob_store {
	struct spdk_bs_opts	bs_opts;
	spdk_blob_id		super_blobid;
	TAILQ_HEAD(, spdk_blob)	blobs;
	int			get_super_status;
};

struct lvol_ut_bs_dev {
	struct spdk_bs_dev	bs_dev;
	int			init_status;
	int			load_status;
	struct spdk_blob_store	*bs;
};

void
spdk_bs_iter_next(struct spdk_blob_store *bs, struct spdk_blob *b,
		  spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_blob *next;
	int _errno = 0;

	next = TAILQ_NEXT(b, link);
	if (next == NULL) {
		_errno = -ENOENT;
	} else if (next->load_status != 0) {
		_errno = next->load_status;
	}

	cb_fn(cb_arg, next, _errno);
}

void
spdk_bs_iter_first(struct spdk_blob_store *bs,
		   spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_blob *first;
	int _errno = 0;

	first = TAILQ_FIRST(&bs->blobs);
	if (first == NULL) {
		_errno = -ENOENT;
	} else if (first->load_status != 0) {
		_errno = first->load_status;
	}

	cb_fn(cb_arg, first, _errno);
}

uint64_t spdk_blob_get_num_clusters(struct spdk_blob *blob)
{
	return 0;
}

void
spdk_bs_get_super(struct spdk_blob_store *bs,
		  spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	if (bs->get_super_status != 0) {
		cb_fn(cb_arg, 0, bs->get_super_status);
	} else {
		cb_fn(cb_arg, bs->super_blobid, 0);
	}
}

void
spdk_bs_set_super(struct spdk_blob_store *bs, spdk_blob_id blobid,
		  spdk_bs_op_complete cb_fn, void *cb_arg)
{
	bs->super_blobid = blobid;
	cb_fn(cb_arg, 0);
}

void
spdk_bs_load(struct spdk_bs_dev *dev, struct spdk_bs_opts *opts,
	     spdk_bs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct lvol_ut_bs_dev *ut_dev = SPDK_CONTAINEROF(dev, struct lvol_ut_bs_dev, bs_dev);
	struct spdk_blob_store *bs = NULL;

	if (ut_dev->load_status == 0) {
		bs = ut_dev->bs;
	}

	cb_fn(cb_arg, bs, ut_dev->load_status);
}

struct spdk_io_channel *spdk_bs_alloc_io_channel(struct spdk_blob_store *bs)
{
	return NULL;
}

int
spdk_blob_set_xattr(struct spdk_blob *blob, const char *name, const void *value,
		    uint16_t value_len)
{
	if (!strcmp(name, "uuid")) {
		CU_ASSERT(value_len == UUID_STRING_LEN);
		memcpy(blob->uuid, value, UUID_STRING_LEN);
	} else if (!strcmp(name, "name")) {
		CU_ASSERT(value_len <= SPDK_LVS_NAME_MAX);
		memcpy(blob->name, value, value_len);
	}

	return 0;
}

int
spdk_blob_get_xattr_value(struct spdk_blob *blob, const char *name,
			  const void **value, size_t *value_len)
{
	if (!strcmp(name, "uuid") && strnlen(blob->uuid, UUID_STRING_LEN) != 0) {
		CU_ASSERT(strnlen(blob->uuid, UUID_STRING_LEN) == (UUID_STRING_LEN - 1));
		*value = blob->uuid;
		*value_len = UUID_STRING_LEN;
		return 0;
	} else if (!strcmp(name, "name") && strnlen(blob->name, SPDK_LVS_NAME_MAX) != 0) {
		*value = blob->name;
		*value_len = strnlen(blob->name, SPDK_LVS_NAME_MAX) + 1;
		return 0;
	}

	return -ENOENT;
}

uint64_t
spdk_bs_get_page_size(struct spdk_blob_store *bs)
{
	return BS_PAGE_SIZE;
}

int
spdk_bdev_notify_blockcnt_change(struct spdk_bdev *bdev, uint64_t size)
{
	bdev->blockcnt = size;
	return 0;
}

static void
init_dev(struct lvol_ut_bs_dev *dev)
{
	memset(dev, 0, sizeof(*dev));
	dev->bs_dev.blockcnt = DEV_BUFFER_BLOCKCNT;
	dev->bs_dev.blocklen = DEV_BUFFER_BLOCKLEN;
}

static void
free_dev(struct lvol_ut_bs_dev *dev)
{
	struct spdk_blob_store *bs = dev->bs;
	struct spdk_blob *blob, *tmp;

	if (bs == NULL) {
		return;
	}

	TAILQ_FOREACH_SAFE(blob, &bs->blobs, link, tmp) {
		TAILQ_REMOVE(&bs->blobs, blob, link);
		free(blob);
	}

	free(bs);
	dev->bs = NULL;
}

void
spdk_bs_init(struct spdk_bs_dev *dev, struct spdk_bs_opts *o,
	     spdk_bs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct lvol_ut_bs_dev *ut_dev = SPDK_CONTAINEROF(dev, struct lvol_ut_bs_dev, bs_dev);
	struct spdk_blob_store *bs;

	bs = calloc(1, sizeof(*bs));
	SPDK_CU_ASSERT_FATAL(bs != NULL);

	TAILQ_INIT(&bs->blobs);

	ut_dev->bs = bs;

	memcpy(&bs->bs_opts, o, sizeof(struct spdk_bs_opts));

	cb_fn(cb_arg, bs, 0);
}

void
spdk_bs_unload(struct spdk_blob_store *bs, spdk_bs_op_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, 0);
}

void
spdk_bs_destroy(struct spdk_blob_store *bs, spdk_bs_op_complete cb_fn,
		void *cb_arg)
{
	free(bs);

	cb_fn(cb_arg, 0);
}

void
spdk_bs_delete_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
		    spdk_blob_op_complete cb_fn, void *cb_arg)
{
	struct spdk_blob *blob;

	TAILQ_FOREACH(blob, &bs->blobs, link) {
		if (blob->id == blobid) {
			TAILQ_REMOVE(&bs->blobs, blob, link);
			free(blob);
			break;
		}
	}

	cb_fn(cb_arg, 0);
}

spdk_blob_id
spdk_blob_get_id(struct spdk_blob *blob)
{
	return blob->id;
}

void
spdk_bs_opts_init(struct spdk_bs_opts *opts)
{
	opts->cluster_sz = SPDK_BLOB_OPTS_CLUSTER_SZ;
	opts->num_md_pages = SPDK_BLOB_OPTS_NUM_MD_PAGES;
	opts->max_md_ops = SPDK_BLOB_OPTS_MAX_MD_OPS;
	opts->max_channel_ops = SPDK_BLOB_OPTS_MAX_CHANNEL_OPS;
	memset(&opts->bstype, 0, sizeof(opts->bstype));
}

uint64_t
spdk_bs_get_cluster_size(struct spdk_blob_store *bs)
{
	return BS_CLUSTER_SIZE;
}

void spdk_blob_close(struct spdk_blob *b, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	b->ref--;

	cb_fn(cb_arg, b->close_status);
}

int
spdk_blob_resize(struct spdk_blob *blob, uint64_t sz)
{
	if (g_resize_rc != 0) {
		return g_resize_rc;
	} else if (sz > DEV_BUFFER_SIZE / BS_CLUSTER_SIZE) {
		return -1;
	}
	return 0;
}

void
spdk_blob_sync_md(struct spdk_blob *blob, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, 0);
}

void
spdk_bs_open_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
		  spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_blob *blob;

	TAILQ_FOREACH(blob, &bs->blobs, link) {
		if (blob->id == blobid) {
			blob->ref++;
			cb_fn(cb_arg, blob, blob->open_status);
			return;
		}
	}

	cb_fn(cb_arg, NULL, -ENOENT);
}

uint64_t
spdk_bs_free_cluster_count(struct spdk_blob_store *bs)
{
	return BS_FREE_CLUSTERS;
}

void
spdk_blob_opts_init(struct spdk_blob_opts *opts)
{
	opts->num_clusters = 0;
	opts->thin_provision = false;
	opts->xattrs.count = 0;
	opts->xattrs.names = NULL;
	opts->xattrs.ctx = NULL;
	opts->xattrs.get_value = NULL;
}

void
spdk_bs_create_blob(struct spdk_blob_store *bs,
		    spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	spdk_bs_create_blob_ext(bs, NULL, cb_fn, cb_arg);
}

void
spdk_bs_create_blob_ext(struct spdk_blob_store *bs, const struct spdk_blob_opts *opts,
			spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	struct spdk_blob *b;

	if (opts && opts->num_clusters > DEV_BUFFER_SIZE / BS_CLUSTER_SIZE) {
		cb_fn(cb_arg, 0, -1);
		return;
	}

	b = calloc(1, sizeof(*b));
	SPDK_CU_ASSERT_FATAL(b != NULL);

	b->id = g_blobid++;
	if (opts != NULL && opts->thin_provision) {
		b->thin_provisioned = true;
	}

	TAILQ_INSERT_TAIL(&bs->blobs, b, link);
	cb_fn(cb_arg, b->id, 0);
}

static void
_lvol_send_msg(spdk_thread_fn fn, void *ctx, void *thread_ctx)
{
	fn(ctx);
}

static void
lvol_store_op_with_handle_complete(void *cb_arg, struct spdk_lvol_store *lvol_store, int lvserrno)
{
	g_lvol_store = lvol_store;
	g_lvserrno = lvserrno;
}

static void
lvol_op_complete(void *cb_arg, int lvolerrno)
{
	g_lvolerrno = lvolerrno;
}

static void
lvol_op_with_handle_complete(void *cb_arg, struct spdk_lvol *lvol, int lvserrno)
{
	g_lvol = lvol;
	g_lvserrno = lvserrno;
}

static void
lvol_store_op_complete(void *cb_arg, int lvserrno)
{
	g_lvserrno = lvserrno;
}

static void
close_cb(void *cb_arg, int lvolerrno)
{
	g_lvserrno = lvolerrno;
}

static void
destroy_cb(void *cb_arg, int lvolerrno)
{
	g_lvserrno = lvolerrno;
}

static void
lvs_init_unload_success(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL, NULL, NULL);
	spdk_lvs_opts_init(&opts);
	strncpy(opts.name, "lvs", sizeof(opts.name));

	g_lvserrno = -1;

	CU_ASSERT(TAILQ_EMPTY(&g_lvol_stores));
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(!TAILQ_EMPTY(&g_lvol_stores));

	spdk_lvol_create(g_lvol_store, "lvol", 10, false, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	/* Lvol store has an open lvol, this unload should fail. */
	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == -EBUSY);
	CU_ASSERT(g_lvserrno == -EBUSY);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(!TAILQ_EMPTY(&g_lvol_stores));

	/* Lvol has to be closed (or destroyed) before unloading lvol store. */
	spdk_lvol_close(g_lvol, close_cb, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_stores));

	free_dev(&dev);

	spdk_free_thread();
}

static void
lvs_init_destroy_success(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL, NULL, NULL);
	spdk_lvs_opts_init(&opts);
	strncpy(opts.name, "lvs", sizeof(opts.name));

	g_lvserrno = -1;

	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", 10, false, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	/* Lvol store contains one lvol, this destroy should fail. */
	g_lvserrno = -1;
	rc = spdk_lvs_destroy(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == -EBUSY);
	CU_ASSERT(g_lvserrno == -EBUSY);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_close(g_lvol, close_cb, NULL);
	CU_ASSERT(g_lvserrno == 0);

	spdk_lvol_destroy(g_lvol, destroy_cb, NULL);

	g_lvserrno = -1;
	rc = spdk_lvs_destroy(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	spdk_free_thread();
}

static void
lvs_init_opts_success(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL, NULL, NULL);

	g_lvserrno = -1;

	spdk_lvs_opts_init(&opts);
	strncpy(opts.name, "lvs", sizeof(opts.name));
	opts.cluster_sz = 8192;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(dev.bs->bs_opts.cluster_sz == opts.cluster_sz);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);

	spdk_free_thread();
}

static void
lvs_unload_lvs_is_null_fail(void)
{
	int rc = 0;

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL, NULL, NULL);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(NULL, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == -ENODEV);
	CU_ASSERT(g_lvserrno == -1);

	spdk_free_thread();
}

static void
lvs_names(void)
{
	struct lvol_ut_bs_dev dev_x, dev_y, dev_x2;
	struct spdk_lvs_opts opts_none, opts_x, opts_y, opts_full;
	struct spdk_lvol_store *lvs_x, *lvs_y, *lvs_x2;
	int rc = 0;

	init_dev(&dev_x);
	init_dev(&dev_y);
	init_dev(&dev_x2);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL, NULL, NULL);

	spdk_lvs_opts_init(&opts_none);
	spdk_lvs_opts_init(&opts_x);
	opts_x.name[0] = 'x';
	spdk_lvs_opts_init(&opts_y);
	opts_y.name[0] = 'y';
	spdk_lvs_opts_init(&opts_full);
	memset(opts_full.name, 'a', sizeof(opts_full.name));

	/* Test that opts with no name fails spdk_lvs_init(). */
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_stores));
	rc = spdk_lvs_init(&dev_x.bs_dev, &opts_none, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_stores));

	/* Test that opts with no null terminator for name fails spdk_lvs_init(). */
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_stores));
	rc = spdk_lvs_init(&dev_x.bs_dev, &opts_full, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_stores));

	/* Test that we can create an lvolstore with name 'x'. */
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_stores));
	g_lvol_store = NULL;
	rc = spdk_lvs_init(&dev_x.bs_dev, &opts_x, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(!TAILQ_EMPTY(&g_lvol_stores));
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	lvs_x = g_lvol_store;

	/* Test that we can create an lvolstore with name 'y'. */
	g_lvol_store = NULL;
	rc = spdk_lvs_init(&dev_y.bs_dev, &opts_y, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	lvs_y = g_lvol_store;

	/* Test that we cannot create another lvolstore with name 'x'. */
	rc = spdk_lvs_init(&dev_x2.bs_dev, &opts_x, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == -EEXIST);

	/* Now destroy lvolstore 'x' and then confirm we can create a new lvolstore with name 'x'. */
	g_lvserrno = -1;
	rc = spdk_lvs_destroy(lvs_x, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;
	rc = spdk_lvs_init(&dev_x.bs_dev, &opts_x, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	lvs_x = g_lvol_store;

	/*
	 * Unload lvolstore 'x'.  Then we should be able to create another lvolstore with name 'x'.
	 */
	g_lvserrno = -1;
	rc = spdk_lvs_unload(lvs_x, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;
	rc = spdk_lvs_init(&dev_x2.bs_dev, &opts_x, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	lvs_x2 = g_lvol_store;

	/* Confirm that we cannot load the first lvolstore 'x'. */
	g_lvserrno = 0;
	spdk_lvs_load(&dev_x.bs_dev, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno != 0);

	/* Destroy the second lvolstore 'x'.  Then we should be able to load the first lvolstore 'x'. */
	g_lvserrno = -1;
	rc = spdk_lvs_destroy(lvs_x2, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvserrno = -1;
	spdk_lvs_load(&dev_x.bs_dev, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	lvs_x = g_lvol_store;

	g_lvserrno = -1;
	rc = spdk_lvs_destroy(lvs_x, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_destroy(lvs_y, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);

	spdk_free_thread();
}

static void
lvol_create_destroy_success(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL, NULL, NULL);

	spdk_lvs_opts_init(&opts);
	strncpy(opts.name, "lvs", sizeof(opts.name));

	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", 10, false, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	spdk_lvol_close(g_lvol, close_cb, NULL);
	CU_ASSERT(g_lvserrno == 0);
	spdk_lvol_destroy(g_lvol, destroy_cb, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);

	spdk_free_thread();
}

static void
lvol_create_fail(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL, NULL, NULL);

	spdk_lvs_opts_init(&opts);
	strncpy(opts.name, "lvs", sizeof(opts.name));

	g_lvol_store = NULL;
	g_lvserrno = 0;
	rc = spdk_lvs_init(NULL, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(g_lvol_store == NULL);

	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	g_lvol = NULL;
	rc = spdk_lvol_create(NULL, "lvol", 10, false, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(g_lvol == NULL);

	g_lvol = NULL;
	rc = spdk_lvol_create(g_lvol_store, "lvol", DEV_BUFFER_SIZE + 1, false,
			      lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno != 0);
	CU_ASSERT(g_lvol == NULL);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);

	spdk_free_thread();
}

static void
lvol_destroy_fail(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL, NULL, NULL);

	spdk_lvs_opts_init(&opts);
	strncpy(opts.name, "lvs", sizeof(opts.name));

	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", 10, false, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	spdk_lvol_close(g_lvol, close_cb, NULL);
	CU_ASSERT(g_lvserrno == 0);
	spdk_lvol_destroy(g_lvol, destroy_cb, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);

	spdk_free_thread();
}

static void
lvol_close_fail(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL, NULL, NULL);

	spdk_lvs_opts_init(&opts);
	strncpy(opts.name, "lvs", sizeof(opts.name));

	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", 10, false, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	spdk_lvol_close(g_lvol, close_cb, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);

	spdk_free_thread();
}

static void
lvol_close_success(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL, NULL, NULL);

	spdk_lvs_opts_init(&opts);
	strncpy(opts.name, "lvs", sizeof(opts.name));

	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", 10, false, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	spdk_lvol_close(g_lvol, close_cb, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);

	spdk_free_thread();
}

static void
lvol_resize(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL, NULL, NULL);

	spdk_lvs_opts_init(&opts);
	strncpy(opts.name, "lvs", sizeof(opts.name));

	g_resize_rc = 0;
	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", 10, false, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	/* Resize to same size */
	rc = spdk_lvol_resize(g_lvol, 10, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);

	/* Resize to smaller size */
	rc = spdk_lvol_resize(g_lvol, 5, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);

	/* Resize to bigger size */
	rc = spdk_lvol_resize(g_lvol, 15, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);

	/* Resize to size = 0 */
	rc = spdk_lvol_resize(g_lvol, 0, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);

	/* Resize to bigger size than available */
	rc = spdk_lvol_resize(g_lvol, 0xFFFFFFFF, lvol_store_op_complete, NULL);
	CU_ASSERT(rc != 0);

	/* Fail resize */
	g_resize_rc = -1;
	g_lvserrno = 0;
	rc = spdk_lvol_resize(g_lvol, 10, lvol_store_op_complete, NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(g_lvserrno != 0);
	g_resize_rc = 0;

	g_resize_rc = 0;
	spdk_lvol_close(g_lvol, close_cb, NULL);
	CU_ASSERT(g_lvserrno == 0);
	spdk_lvol_destroy(g_lvol, destroy_cb, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);

	spdk_free_thread();
}

static void
null_cb(void *ctx, struct spdk_blob_store *bs, int bserrno)
{
	SPDK_CU_ASSERT_FATAL(bs != NULL);
}

static void
lvs_load(void)
{
	int rc = -1;
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_with_handle_req *req;
	struct spdk_bs_opts bs_opts = {};
	struct spdk_blob *super_blob;

	req = calloc(1, sizeof(*req));
	SPDK_CU_ASSERT_FATAL(req != NULL);

	init_dev(&dev);
	spdk_bs_opts_init(&bs_opts);
	strncpy(bs_opts.bstype.bstype, "LVOLSTORE", SPDK_BLOBSTORE_TYPE_LENGTH);
	spdk_bs_init(&dev.bs_dev, &bs_opts, null_cb, NULL);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL, NULL, NULL);

	/* Fail on bs load */
	dev.load_status = -1;
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_stores));
	spdk_lvs_load(&dev.bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno != 0);
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_stores));

	/* Fail on getting super blob */
	dev.load_status = 0;
	dev.bs->get_super_status = -1;
	spdk_lvs_load(&dev.bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno == -ENODEV);
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_stores));

	/* Fail on opening super blob */
	g_lvserrno = 0;
	super_blob = calloc(1, sizeof(*super_blob));
	super_blob->id = 0x100;
	super_blob->open_status = -1;
	TAILQ_INSERT_TAIL(&dev.bs->blobs, super_blob, link);
	dev.bs->super_blobid = 0x100;
	dev.bs->get_super_status = 0;
	spdk_lvs_load(&dev.bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno == -ENODEV);
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_stores));

	/* Fail on getting uuid */
	g_lvserrno = 0;
	super_blob->open_status = 0;
	spdk_lvs_load(&dev.bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno == -EINVAL);
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_stores));

	/* Fail on getting name */
	g_lvserrno = 0;
	spdk_blob_set_xattr(super_blob, "uuid", uuid, UUID_STRING_LEN);
	spdk_lvs_load(&dev.bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno == -EINVAL);
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_stores));

	/* Fail on closing super blob */
	g_lvserrno = 0;
	spdk_blob_set_xattr(super_blob, "name", "lvs", strnlen("lvs", SPDK_LVS_NAME_MAX) + 1);
	super_blob->close_status = -1;
	spdk_lvs_load(&dev.bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno == -ENODEV);
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_stores));

	/* Load successfully */
	g_lvserrno = 0;
	super_blob->close_status = 0;
	spdk_lvs_load(&dev.bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store != NULL);
	CU_ASSERT(!TAILQ_EMPTY(&g_lvol_stores));

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_stores));

	free(req);
	free_dev(&dev);

	spdk_free_thread();
}

static void
lvols_load(void)
{
	int rc = -1;
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_with_handle_req *req;
	struct spdk_bs_opts bs_opts;
	struct spdk_blob *super_blob, *blob1, *blob2, *blob3;

	req = calloc(1, sizeof(*req));
	SPDK_CU_ASSERT_FATAL(req != NULL);

	init_dev(&dev);
	spdk_bs_opts_init(&bs_opts);
	strncpy(bs_opts.bstype.bstype, "LVOLSTORE", SPDK_BLOBSTORE_TYPE_LENGTH);
	spdk_bs_init(&dev.bs_dev, &bs_opts, null_cb, NULL);
	super_blob = calloc(1, sizeof(*super_blob));
	SPDK_CU_ASSERT_FATAL(super_blob != NULL);
	super_blob->id = 0x100;
	spdk_blob_set_xattr(super_blob, "uuid", uuid, UUID_STRING_LEN);
	spdk_blob_set_xattr(super_blob, "name", "lvs", strnlen("lvs", SPDK_LVS_NAME_MAX) + 1);
	TAILQ_INSERT_TAIL(&dev.bs->blobs, super_blob, link);
	dev.bs->super_blobid = 0x100;

	/*
	 * Create 3 blobs, write different char values to the last char in the UUID
	 *  to make sure they are unique.
	 */
	blob1 = calloc(1, sizeof(*blob1));
	SPDK_CU_ASSERT_FATAL(blob1 != NULL);
	blob1->id = 0x1;
	spdk_blob_set_xattr(blob1, "uuid", uuid, UUID_STRING_LEN);
	spdk_blob_set_xattr(blob1, "name", "lvol1", strnlen("lvol1", SPDK_LVOL_NAME_MAX) + 1);
	blob1->uuid[UUID_STRING_LEN - 2] = '1';

	blob2 = calloc(1, sizeof(*blob2));
	SPDK_CU_ASSERT_FATAL(blob2 != NULL);
	blob2->id = 0x2;
	spdk_blob_set_xattr(blob2, "uuid", uuid, UUID_STRING_LEN);
	spdk_blob_set_xattr(blob2, "name", "lvol2", strnlen("lvol2", SPDK_LVOL_NAME_MAX) + 1);
	blob2->uuid[UUID_STRING_LEN - 2] = '2';

	blob3 = calloc(1, sizeof(*blob3));
	SPDK_CU_ASSERT_FATAL(blob3 != NULL);
	blob3->id = 0x2;
	spdk_blob_set_xattr(blob3, "uuid", uuid, UUID_STRING_LEN);
	spdk_blob_set_xattr(blob3, "name", "lvol3", strnlen("lvol3", SPDK_LVOL_NAME_MAX) + 1);
	blob3->uuid[UUID_STRING_LEN - 2] = '3';

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL, NULL, NULL);

	/* Load lvs with 0 blobs */
	g_lvserrno = 0;
	spdk_lvs_load(&dev.bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store != NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);

	TAILQ_INSERT_TAIL(&dev.bs->blobs, blob1, link);
	TAILQ_INSERT_TAIL(&dev.bs->blobs, blob2, link);
	TAILQ_INSERT_TAIL(&dev.bs->blobs, blob3, link);

	/* Load lvs again with 3 blobs, but fail on 1st one */
	g_lvol_store = NULL;
	g_lvserrno = 0;
	blob1->load_status = -1;
	spdk_lvs_load(&dev.bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno != 0);
	CU_ASSERT(g_lvol_store == NULL);

	/* Load lvs again with 3 blobs, but fail on 3rd one */
	g_lvol_store = NULL;
	g_lvserrno = 0;
	blob1->load_status = 0;
	blob2->load_status = 0;
	blob3->load_status = -1;
	spdk_lvs_load(&dev.bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno != 0);
	CU_ASSERT(g_lvol_store == NULL);

	/* Load lvs again with 3 blobs, with success */
	g_lvol_store = NULL;
	g_lvserrno = 0;
	blob1->load_status = 0;
	blob2->load_status = 0;
	blob3->load_status = 0;
	spdk_lvs_load(&dev.bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(!TAILQ_EMPTY(&g_lvol_store->lvols));

	g_lvserrno = -1;
	/* rc = */ spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	/*
	 * Disable these two asserts for now.  lvolstore should allow unload as long
	 *  as the lvols were not opened - but this is coming a future patch.
	 */
	/* CU_ASSERT(rc == 0); */
	/* CU_ASSERT(g_lvserrno == 0); */

	free(req);
	free_dev(&dev);

	spdk_free_thread();
}

static void
lvol_open(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_with_handle_req *req;
	struct spdk_bs_opts bs_opts;
	struct spdk_blob *super_blob, *blob1, *blob2, *blob3;
	struct spdk_lvol *lvol, *tmp;

	req = calloc(1, sizeof(*req));
	SPDK_CU_ASSERT_FATAL(req != NULL);

	init_dev(&dev);
	spdk_bs_opts_init(&bs_opts);
	strncpy(bs_opts.bstype.bstype, "LVOLSTORE", SPDK_BLOBSTORE_TYPE_LENGTH);
	spdk_bs_init(&dev.bs_dev, &bs_opts, null_cb, NULL);
	super_blob = calloc(1, sizeof(*super_blob));
	SPDK_CU_ASSERT_FATAL(super_blob != NULL);
	super_blob->id = 0x100;
	spdk_blob_set_xattr(super_blob, "uuid", uuid, UUID_STRING_LEN);
	spdk_blob_set_xattr(super_blob, "name", "lvs", strnlen("lvs", SPDK_LVS_NAME_MAX) + 1);
	TAILQ_INSERT_TAIL(&dev.bs->blobs, super_blob, link);
	dev.bs->super_blobid = 0x100;

	/*
	 * Create 3 blobs, write different char values to the last char in the UUID
	 *  to make sure they are unique.
	 */
	blob1 = calloc(1, sizeof(*blob1));
	SPDK_CU_ASSERT_FATAL(blob1 != NULL);
	blob1->id = 0x1;
	spdk_blob_set_xattr(blob1, "uuid", uuid, UUID_STRING_LEN);
	spdk_blob_set_xattr(blob1, "name", "lvol1", strnlen("lvol1", SPDK_LVOL_NAME_MAX) + 1);
	blob1->uuid[UUID_STRING_LEN - 2] = '1';

	blob2 = calloc(1, sizeof(*blob2));
	SPDK_CU_ASSERT_FATAL(blob2 != NULL);
	blob2->id = 0x2;
	spdk_blob_set_xattr(blob2, "uuid", uuid, UUID_STRING_LEN);
	spdk_blob_set_xattr(blob2, "name", "lvol2", strnlen("lvol2", SPDK_LVOL_NAME_MAX) + 1);
	blob2->uuid[UUID_STRING_LEN - 2] = '2';

	blob3 = calloc(1, sizeof(*blob3));
	SPDK_CU_ASSERT_FATAL(blob3 != NULL);
	blob3->id = 0x2;
	spdk_blob_set_xattr(blob3, "uuid", uuid, UUID_STRING_LEN);
	spdk_blob_set_xattr(blob3, "name", "lvol3", strnlen("lvol3", SPDK_LVOL_NAME_MAX) + 1);
	blob3->uuid[UUID_STRING_LEN - 2] = '3';

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL, NULL, NULL);

	TAILQ_INSERT_TAIL(&dev.bs->blobs, blob1, link);
	TAILQ_INSERT_TAIL(&dev.bs->blobs, blob2, link);
	TAILQ_INSERT_TAIL(&dev.bs->blobs, blob3, link);

	/* Load lvs with 3 blobs */
	g_lvol_store = NULL;
	g_lvserrno = 0;
	spdk_lvs_load(&dev.bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	SPDK_CU_ASSERT_FATAL(!TAILQ_EMPTY(&g_lvol_stores));

	blob1->open_status = -1;
	blob2->open_status = -1;
	blob3->open_status = -1;

	/* Fail opening all lvols */
	TAILQ_FOREACH_SAFE(lvol, &g_lvol_store->lvols, link, tmp) {
		spdk_lvol_open(lvol, lvol_op_with_handle_complete, NULL);
		CU_ASSERT(g_lvserrno != 0);
	}

	blob1->open_status = 0;
	blob2->open_status = 0;
	blob3->open_status = 0;

	/* Open all lvols */
	TAILQ_FOREACH_SAFE(lvol, &g_lvol_store->lvols, link, tmp) {
		spdk_lvol_open(lvol, lvol_op_with_handle_complete, NULL);
		CU_ASSERT(g_lvserrno == 0);
	}

	/* Close all lvols */
	TAILQ_FOREACH_SAFE(lvol, &g_lvol_store->lvols, link, tmp) {
		spdk_lvol_close(lvol, lvol_op_complete, NULL);
		CU_ASSERT(g_lvserrno == 0);
	}

	g_lvserrno = -1;
	spdk_lvs_destroy(g_lvol_store, lvol_store_op_complete, NULL);

	free(req);
	free(blob1);
	free(blob2);
	free(blob3);

	spdk_free_thread();
}

static void
lvol_names(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	struct spdk_lvol_store *lvs;
	struct spdk_lvol *lvol, *lvol2;
	char fullname[SPDK_LVOL_NAME_MAX];
	int rc = 0;

	init_dev(&dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL, NULL, NULL);

	spdk_lvs_opts_init(&opts);
	strncpy(opts.name, "lvs", sizeof(opts.name));

	g_lvserrno = -1;
	g_lvol_store = NULL;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	lvs = g_lvol_store;

	rc = spdk_lvol_create(lvs, NULL, 1, false, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == -EINVAL);

	rc = spdk_lvol_create(lvs, "", 1, false, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == -EINVAL);

	memset(fullname, 'x', sizeof(fullname));
	rc = spdk_lvol_create(lvs, fullname, 1, false, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == -EINVAL);

	g_lvserrno = -1;
	rc = spdk_lvol_create(lvs, "lvol", 1, false, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol = g_lvol;

	rc = spdk_lvol_create(lvs, "lvol", 1, false, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == -EINVAL);

	g_lvserrno = -1;
	rc = spdk_lvol_create(lvs, "lvol2", 1, false, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol2 = g_lvol;

	spdk_lvol_close(lvol, close_cb, NULL);
	spdk_lvol_destroy(lvol, lvol_op_complete, NULL);

	g_lvserrno = -1;
	g_lvol = NULL;
	rc = spdk_lvol_create(lvs, "lvol", 1, false, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol = g_lvol;

	spdk_lvol_close(lvol, close_cb, NULL);
	spdk_lvol_destroy(lvol, destroy_cb, NULL);

	spdk_lvol_close(lvol2, close_cb, NULL);
	spdk_lvol_destroy(lvol2, destroy_cb, NULL);

	g_lvserrno = -1;
	rc = spdk_lvs_destroy(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	spdk_free_thread();
}

static void
lvol_rename(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	struct spdk_lvol_store *lvs;
	struct spdk_lvol *lvol, *lvol2;
	int rc = 0;

	init_dev(&dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL, NULL, NULL);

	spdk_lvs_opts_init(&opts);
	strncpy(opts.name, "lvs", sizeof(opts.name));

	g_lvserrno = -1;
	g_lvol_store = NULL;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	lvs = g_lvol_store;

	/* Trying to create new lvol */
	g_lvserrno = -1;
	rc = spdk_lvol_create(lvs, "lvol", 1, false, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol = g_lvol;

	/* Trying to create second lvol with existing lvol name */
	g_lvserrno = -1;
	g_lvol = NULL;
	rc = spdk_lvol_create(lvs, "lvol", 1, false, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(g_lvserrno == -1);
	SPDK_CU_ASSERT_FATAL(g_lvol == NULL);

	/* Trying to create second lvol with non existing name */
	g_lvserrno = -1;
	rc = spdk_lvol_create(lvs, "lvol2", 1, false, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol2 = g_lvol;

	/* Trying to rename lvol with not existing name */
	spdk_lvol_rename(lvol, "lvol_new", lvol_op_complete, NULL);
	CU_ASSERT(g_lvolerrno == 0);
	CU_ASSERT_STRING_EQUAL(lvol->name, "lvol_new");

	/* Trying to rename lvol with other lvol name */
	spdk_lvol_rename(lvol2, "lvol_new", lvol_op_complete, NULL);
	CU_ASSERT(g_lvolerrno == -EEXIST);
	CU_ASSERT_STRING_NOT_EQUAL(lvol2->name, "lvol_new");

	spdk_lvol_close(lvol, close_cb, NULL);
	spdk_lvol_destroy(lvol, lvol_op_complete, NULL);

	spdk_lvol_close(lvol2, close_cb, NULL);
	spdk_lvol_destroy(lvol2, lvol_op_complete, NULL);

	g_lvserrno = -1;
	rc = spdk_lvs_destroy(lvs, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	spdk_free_thread();
}

static void lvol_refcnt(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL, NULL, NULL);
	spdk_lvs_opts_init(&opts);
	strncpy(opts.name, "lvs", sizeof(opts.name));

	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);


	spdk_lvol_create(g_lvol_store, "lvol", 10, false, lvol_op_with_handle_complete, NULL);

	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT(g_lvol->ref_count == 1);

	spdk_lvol_open(g_lvol, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvol->ref_count == 2);

	/* Trying to destroy lvol while its open should fail */
	spdk_lvol_destroy(g_lvol, lvol_op_complete, NULL);
	CU_ASSERT(g_lvolerrno != 0);

	spdk_lvol_close(g_lvol, lvol_op_complete, NULL);
	CU_ASSERT(g_lvol->ref_count == 1);
	CU_ASSERT(g_lvolerrno == 0);

	spdk_lvol_close(g_lvol, lvol_op_complete, NULL);
	CU_ASSERT(g_lvol->ref_count == 0);
	CU_ASSERT(g_lvolerrno == 0);

	/* Try to close already closed lvol */
	spdk_lvol_close(g_lvol, lvol_op_complete, NULL);
	CU_ASSERT(g_lvol->ref_count == 0);
	CU_ASSERT(g_lvolerrno != 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);

	spdk_free_thread();
}

static void
lvol_create_thin_provisioned(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_allocate_thread(_lvol_send_msg, NULL, NULL, NULL, NULL);

	spdk_lvs_opts_init(&opts);
	strncpy(opts.name, "lvs", sizeof(opts.name));

	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", 10, false, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	CU_ASSERT(g_lvol->blob->thin_provisioned == false);

	spdk_lvol_close(g_lvol, close_cb, NULL);
	CU_ASSERT(g_lvserrno == 0);
	spdk_lvol_destroy(g_lvol, destroy_cb, NULL);
	CU_ASSERT(g_lvserrno == 0);

	spdk_lvol_create(g_lvol_store, "lvol", 10, true, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	CU_ASSERT(g_lvol->blob->thin_provisioned == true);

	spdk_lvol_close(g_lvol, close_cb, NULL);
	CU_ASSERT(g_lvserrno == 0);
	spdk_lvol_destroy(g_lvol, destroy_cb, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, lvol_store_op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);

	spdk_free_thread();
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
		CU_add_test(suite, "lvs_init_unload_success", lvs_init_unload_success) == NULL ||
		CU_add_test(suite, "lvs_init_destroy_success", lvs_init_destroy_success) == NULL ||
		CU_add_test(suite, "lvs_init_opts_success", lvs_init_opts_success) == NULL ||
		CU_add_test(suite, "lvs_unload_lvs_is_null_fail", lvs_unload_lvs_is_null_fail) == NULL ||
		CU_add_test(suite, "lvs_names", lvs_names) == NULL ||
		CU_add_test(suite, "lvol_create_destroy_success", lvol_create_destroy_success) == NULL ||
		CU_add_test(suite, "lvol_create_fail", lvol_create_fail) == NULL ||
		CU_add_test(suite, "lvol_destroy_fail", lvol_destroy_fail) == NULL ||
		CU_add_test(suite, "lvol_close_fail", lvol_close_fail) == NULL ||
		CU_add_test(suite, "lvol_close_success", lvol_close_success) == NULL ||
		CU_add_test(suite, "lvol_resize", lvol_resize) == NULL ||
		CU_add_test(suite, "lvs_load", lvs_load) == NULL ||
		CU_add_test(suite, "lvols_load", lvols_load) == NULL ||
		CU_add_test(suite, "lvol_open", lvol_open) == NULL ||
		CU_add_test(suite, "lvol_load", lvs_load) == NULL ||
		CU_add_test(suite, "lvs_load", lvols_load) == NULL ||
		CU_add_test(suite, "lvol_open", lvol_open) == NULL ||
		CU_add_test(suite, "lvol_refcnt", lvol_refcnt) == NULL ||
		CU_add_test(suite, "lvol_names", lvol_names) == NULL ||
		CU_add_test(suite, "lvol_create_thin_provisioned", lvol_create_thin_provisioned) == NULL ||
		CU_add_test(suite, "lvol_rename", lvol_rename) == NULL
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
