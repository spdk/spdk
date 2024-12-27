/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk_internal/cunit.h"
#include "spdk/blob.h"
#include "spdk/util.h"

#include "spdk/bdev_module.h"
#include "thread/thread_internal.h"

#include "common/lib/ut_multithread.c"
#include "lvol/lvol.c"

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


DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), NULL);
DEFINE_STUB(spdk_bdev_get_by_name, struct spdk_bdev *, (const char *name), NULL);
DEFINE_STUB(spdk_bdev_create_bs_dev_ro, int,
	    (const char *bdev_name, spdk_bdev_event_cb_t event_cb, void *event_ctx,
	     struct spdk_bs_dev **bs_dev), -ENOTSUP);
DEFINE_STUB(spdk_blob_is_esnap_clone, bool, (const struct spdk_blob *blob), false);
DEFINE_STUB(spdk_blob_is_degraded, bool, (const struct spdk_blob *blob), false);
DEFINE_STUB_V(spdk_bs_grow_live,
	      (struct spdk_blob_store *bs, spdk_bs_op_complete cb_fn, void *cb_arg));
DEFINE_STUB_V(blob_freeze_on_failover, (struct spdk_blob *blob));
DEFINE_STUB(spdk_bs_delete_blob_non_leader, int ,(struct spdk_blob_store *bs, struct spdk_blob *blob), 0);		  


const char *uuid = "828d9766-ae50-11e7-bd8d-001e67edf350";

struct spdk_blob {
	spdk_blob_id		id;
	uint32_t		ref;
	struct spdk_blob_store *bs;
	int			close_status;
	int			open_status;
	int			load_status;
	TAILQ_ENTRY(spdk_blob)	link;
	char			uuid[SPDK_UUID_STRING_LEN];
	char			name[SPDK_LVS_NAME_MAX];
	bool			thin_provisioned;
	struct spdk_bs_dev	*back_bs_dev;
	uint64_t		num_clusters;
};

int g_lvserrno;
int g_close_super_status;
int g_resize_rc;
int g_inflate_rc;
int g_remove_rc;
bool g_lvs_rename_blob_open_error = false;
bool g_blob_read_only = false;
bool g_blob_is_snapshot = false;
struct spdk_lvol_store *g_lvol_store;
struct spdk_lvol *g_lvol;
spdk_blob_id g_blobid = 1;
struct spdk_io_channel *g_io_channel;
struct lvol_ut_bs_dev g_esnap_dev;

struct spdk_blob_store {
	struct spdk_bs_opts	bs_opts;
	spdk_blob_id		super_blobid;
	TAILQ_HEAD(, spdk_blob)	blobs;
	int			get_super_status;
	spdk_bs_esnap_dev_create esnap_bs_dev_create;
};

struct lvol_ut_bs_dev {
	struct spdk_bs_dev	bs_dev;
	int			init_status;
	int			load_status;
	struct spdk_blob_store	*bs;
};

struct ut_cb_res {
	void *data;
	int err;
};

void
spdk_bs_inflate_blob(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
		     spdk_blob_id blobid, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, g_inflate_rc);
}

void
spdk_bs_blob_decouple_parent(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
			     spdk_blob_id blobid, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, g_inflate_rc);
}

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

uint64_t
spdk_blob_get_num_clusters(struct spdk_blob *blob)
{
	return blob->num_clusters;
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

void
spdk_bs_grow(struct spdk_bs_dev *dev, struct spdk_bs_opts *o,
	     spdk_bs_op_with_handle_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, NULL, -EINVAL);
}

struct spdk_io_channel *spdk_bs_alloc_io_channel(struct spdk_blob_store *bs)
{
	if (g_io_channel == NULL) {
		g_io_channel = calloc(1, sizeof(struct spdk_io_channel));
		SPDK_CU_ASSERT_FATAL(g_io_channel != NULL);
	}
	g_io_channel->ref++;
	return g_io_channel;
}

void
spdk_bs_free_io_channel(struct spdk_io_channel *channel)
{
	g_io_channel->ref--;
	if (g_io_channel->ref == 0) {
		free(g_io_channel);
		g_io_channel = NULL;
	}
	return;
}

int
spdk_blob_set_xattr(struct spdk_blob *blob, const char *name, const void *value,
		    uint16_t value_len)
{
	if (!strcmp(name, "uuid")) {
		CU_ASSERT(value_len == SPDK_UUID_STRING_LEN);
		memcpy(blob->uuid, value, SPDK_UUID_STRING_LEN);
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
	if (!strcmp(name, "uuid") && strnlen(blob->uuid, SPDK_UUID_STRING_LEN) != 0) {
		CU_ASSERT(strnlen(blob->uuid, SPDK_UUID_STRING_LEN) == (SPDK_UUID_STRING_LEN - 1));
		*value = blob->uuid;
		*value_len = SPDK_UUID_STRING_LEN;
		return 0;
	} else if (!strcmp(name, "name") && strnlen(blob->name, SPDK_LVS_NAME_MAX) != 0) {
		*value = blob->name;
		*value_len = strnlen(blob->name, SPDK_LVS_NAME_MAX) + 1;
		return 0;
	}

	return -ENOENT;
}

void
spdk_blob_set_esnap_bs_dev(struct spdk_blob *blob, struct spdk_bs_dev *back_bs_dev,
			   spdk_blob_op_complete cb_fn, void *cb_arg)
{
	blob->back_bs_dev = back_bs_dev;
	cb_fn(cb_arg, 0);
}

bool
spdk_blob_is_thin_provisioned(struct spdk_blob *blob)
{
	return blob->thin_provisioned;
}

int
spdk_bs_blob_shallow_copy(struct spdk_blob_store *bs, struct spdk_io_channel *channel,
			  spdk_blob_id blobid, struct spdk_bs_dev *ext_dev,
			  spdk_blob_shallow_copy_status status_cb_fn, void *status_cb_arg,
			  spdk_blob_op_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, 0);
	return 0;
}

bool
spdk_blob_is_snapshot(struct spdk_blob *blob)
{
	return g_blob_is_snapshot;
}

void
spdk_bs_blob_set_parent(struct spdk_blob_store *bs, spdk_blob_id blob_id,
			spdk_blob_id snapshot_id, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, 0);
}

void
spdk_bs_blob_set_external_parent(struct spdk_blob_store *bs, spdk_blob_id blob_id,
				 struct spdk_bs_dev *back_bs_dev, const void *esnap_id,
				 uint32_t id_len, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, 0);
}

DEFINE_STUB(spdk_bs_get_page_size, uint64_t, (struct spdk_blob_store *bs), BS_PAGE_SIZE);

int
spdk_bdev_notify_blockcnt_change(struct spdk_bdev *bdev, uint64_t size)
{
	bdev->blockcnt = size;
	return 0;
}

const struct spdk_uuid *
spdk_bdev_get_uuid(const struct spdk_bdev *bdev)
{
	return &bdev->uuid;
}

uint64_t
spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev)
{
	return bdev->blockcnt;
}

uint32_t
spdk_bdev_get_block_size(const struct spdk_bdev *bdev)
{
	return bdev->blocklen;
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

static void
init_bdev(struct spdk_bdev *bdev, char *name, size_t size)
{
	memset(bdev, 0, sizeof(*bdev));
	bdev->name = name;
	spdk_uuid_generate(&bdev->uuid);
	bdev->blocklen = BS_PAGE_SIZE;
	bdev->phys_blocklen = BS_PAGE_SIZE;
	bdev->blockcnt = size / BS_PAGE_SIZE;
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
	bs->esnap_bs_dev_create = o->esnap_bs_dev_create;

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

	cb_fn(cb_arg, g_remove_rc);
}

spdk_blob_id
spdk_blob_get_id(struct spdk_blob *blob)
{
	return blob->id;
}

void
spdk_bs_opts_init(struct spdk_bs_opts *opts, size_t opts_size)
{
	memset(opts, 0, sizeof(*opts));
	opts->opts_size = opts_size;
	opts->cluster_sz = SPDK_BLOB_OPTS_CLUSTER_SZ;
	opts->num_md_pages = SPDK_BLOB_OPTS_NUM_MD_PAGES;
	opts->max_md_ops = SPDK_BLOB_OPTS_MAX_MD_OPS;
	opts->max_channel_ops = SPDK_BLOB_OPTS_MAX_CHANNEL_OPS;
}

DEFINE_STUB(spdk_bs_get_cluster_size, uint64_t, (struct spdk_blob_store *bs), BS_CLUSTER_SIZE);

void
spdk_blob_close(struct spdk_blob *b, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	b->ref--;

	cb_fn(cb_arg, b->close_status);
}

void
spdk_blob_resize(struct spdk_blob *blob, uint64_t sz, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	if (g_resize_rc != 0) {
		return cb_fn(cb_arg, g_resize_rc);
	} else if (sz > DEV_BUFFER_SIZE / BS_CLUSTER_SIZE) {
		return cb_fn(cb_arg, -ENOMEM);
	}
	cb_fn(cb_arg, 0);
}

DEFINE_STUB(spdk_blob_set_read_only, int, (struct spdk_blob *blob), 0);

void
spdk_blob_sync_md(struct spdk_blob *blob, spdk_blob_op_complete cb_fn, void *cb_arg)
{
	cb_fn(cb_arg, 0);
}

void
spdk_bs_open_blob_ext(struct spdk_blob_store *bs, spdk_blob_id blobid,
		      struct spdk_blob_open_opts *opts, spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	spdk_bs_open_blob(bs, blobid, cb_fn, cb_arg);
}

void
spdk_bs_open_blob(struct spdk_blob_store *bs, spdk_blob_id blobid,
		  spdk_blob_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_blob *blob;

	if (!g_lvs_rename_blob_open_error) {
		TAILQ_FOREACH(blob, &bs->blobs, link) {
			if (blob->id == blobid) {
				blob->ref++;
				cb_fn(cb_arg, blob, blob->open_status);
				return;
			}
		}
	}

	cb_fn(cb_arg, NULL, -ENOENT);
}

DEFINE_STUB(spdk_bs_free_cluster_count, uint64_t, (struct spdk_blob_store *bs), BS_FREE_CLUSTERS);

void
spdk_blob_opts_init(struct spdk_blob_opts *opts, size_t opts_size)
{
	opts->opts_size = opts_size;
	opts->num_clusters = 0;
	opts->thin_provision = false;
	opts->xattrs.count = 0;
	opts->xattrs.names = NULL;
	opts->xattrs.ctx = NULL;
	opts->xattrs.get_value = NULL;
}

void
spdk_blob_open_opts_init(struct spdk_blob_open_opts *opts, size_t opts_size)
{
	opts->opts_size = opts_size;
	opts->clear_method = BLOB_CLEAR_WITH_DEFAULT;
}

bool
spdk_blob_is_read_only(struct spdk_blob *blob)
{
	return g_blob_read_only;
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
	b->bs = bs;

	if (opts != NULL) {
		b->num_clusters = opts->num_clusters;
	} else {
		b->num_clusters = 1;
	}

	TAILQ_INSERT_TAIL(&bs->blobs, b, link);
	cb_fn(cb_arg, b->id, 0);
}

void
spdk_bs_create_snapshot(struct spdk_blob_store *bs, spdk_blob_id blobid,
			const struct spdk_blob_xattr_opts *snapshot_xattrs,
			spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	spdk_bs_create_blob_ext(bs, NULL, cb_fn, cb_arg);
}

void
spdk_bs_create_clone(struct spdk_blob_store *bs, spdk_blob_id blobid,
		     const struct spdk_blob_xattr_opts *clone_xattrs,
		     spdk_blob_op_with_id_complete cb_fn, void *cb_arg)
{
	spdk_bs_create_blob_ext(bs, NULL, cb_fn, cb_arg);
}

static int g_spdk_blob_get_esnap_id_errno;
static bool g_spdk_blob_get_esnap_id_called;
static void *g_spdk_blob_get_esnap_id;
static size_t g_spdk_blob_get_esnap_id_len;
int
spdk_blob_get_esnap_id(struct spdk_blob *blob, const void **id, size_t *len)
{
	g_spdk_blob_get_esnap_id_called = true;
	if (g_spdk_blob_get_esnap_id_errno == 0) {
		*id = g_spdk_blob_get_esnap_id;
		*len = g_spdk_blob_get_esnap_id_len;
	}
	return g_spdk_blob_get_esnap_id_errno;
}

static spdk_blob_id g_spdk_blob_get_clones_snap_id = 0xbad;
static size_t g_spdk_blob_get_clones_count;
static spdk_blob_id *g_spdk_blob_get_clones_ids;
int
spdk_blob_get_clones(struct spdk_blob_store *bs, spdk_blob_id blob_id, spdk_blob_id *ids,
		     size_t *count)
{
	if (blob_id != g_spdk_blob_get_clones_snap_id) {
		*count = 0;
		return 0;
	}
	if (ids == NULL || *count < g_spdk_blob_get_clones_count) {
		*count = g_spdk_blob_get_clones_count;
		return -ENOMEM;
	}
	memcpy(ids, g_spdk_blob_get_clones_ids, g_spdk_blob_get_clones_count * sizeof(*ids));
	return 0;
}

static void
lvol_store_op_with_handle_complete(void *cb_arg, struct spdk_lvol_store *lvol_store, int lvserrno)
{
	g_lvol_store = lvol_store;
	g_lvserrno = lvserrno;
	if (cb_arg != NULL) {
		struct ut_cb_res *res = cb_arg;

		res->data = lvol_store;
		res->err = lvserrno;
	}
}

static void
lvol_op_with_handle_complete(void *cb_arg, struct spdk_lvol *lvol, int lvserrno)
{
	g_lvol = lvol;
	g_lvserrno = lvserrno;
	if (cb_arg != NULL) {
		struct ut_cb_res *res = cb_arg;

		res->data = lvol;
		res->err = lvserrno;
	}
}

static void
op_complete(void *cb_arg, int lvserrno)
{
	g_lvserrno = lvserrno;
	if (cb_arg != NULL) {
		struct ut_cb_res *res = cb_arg;

		res->err = lvserrno;
	}
}

static struct ut_cb_res *
ut_cb_res_clear(struct ut_cb_res *res)
{
	memset(res, 0, sizeof(*res));
	res->data = (void *)(uintptr_t)(-1);
	res->err = 0xbad;
	return res;
}

static bool
ut_cb_res_untouched(const struct ut_cb_res *res)
{
	struct ut_cb_res pristine;

	ut_cb_res_clear(&pristine);
	return !memcmp(&pristine, res, sizeof(pristine));
}

struct count_clones_ctx {
	struct spdk_lvol *stop_on_lvol;
	int stop_errno;
	int count;
};

static int
count_clones(void *_ctx, struct spdk_lvol *lvol)
{
	struct count_clones_ctx *ctx = _ctx;

	if (ctx->stop_on_lvol == lvol) {
		return ctx->stop_errno;
	}
	ctx->count++;
	return 0;
}

static void
lvs_init_unload_success(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_lvserrno = -1;

	CU_ASSERT(TAILQ_EMPTY(&g_lvol_stores));
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(!TAILQ_EMPTY(&g_lvol_stores));

	spdk_lvol_create(g_lvol_store, "lvol", 10, false, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	/* Lvol store has an open lvol, this unload should fail. */
	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == -EBUSY);
	CU_ASSERT(g_lvserrno == -EBUSY);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(!TAILQ_EMPTY(&g_lvol_stores));

	/* Lvol has to be closed (or destroyed) before unloading lvol store. */
	spdk_lvol_close(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_stores));

	free_dev(&dev);
}

static void
lvs_init_destroy_success(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_lvserrno = -1;

	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", 10, false, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	/* Lvol store contains one lvol, this destroy should fail. */
	g_lvserrno = -1;
	rc = spdk_lvs_destroy(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == -EBUSY);
	CU_ASSERT(g_lvserrno == -EBUSY);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_close(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	spdk_lvol_destroy(g_lvol, op_complete, NULL);

	g_lvserrno = -1;
	rc = spdk_lvs_destroy(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;
}

static void
lvs_init_opts_success(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	g_lvserrno = -1;

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");
	opts.cluster_sz = 8192;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(dev.bs->bs_opts.cluster_sz == opts.cluster_sz);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);
}

static void
lvs_unload_lvs_is_null_fail(void)
{
	int rc = 0;

	g_lvserrno = -1;
	rc = spdk_lvs_unload(NULL, op_complete, NULL);
	CU_ASSERT(rc == -ENODEV);
	CU_ASSERT(g_lvserrno == -1);
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
	rc = spdk_lvs_destroy(lvs_x, op_complete, NULL);
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
	rc = spdk_lvs_unload(lvs_x, op_complete, NULL);
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
	rc = spdk_lvs_destroy(lvs_x2, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvserrno = -1;
	spdk_lvs_load(&dev_x.bs_dev, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	lvs_x = g_lvol_store;

	g_lvserrno = -1;
	rc = spdk_lvs_destroy(lvs_x, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_destroy(lvs_y, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
}

static void
lvol_create_destroy_success(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", 10, false, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	spdk_lvol_close(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	spdk_lvol_destroy(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);
}

static void
lvol_create_fail(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_lvol_store = NULL;
	g_lvserrno = 0;
	rc = spdk_lvs_init(NULL, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(g_lvol_store == NULL);

	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	g_lvol = NULL;
	rc = spdk_lvol_create(NULL, "lvol", 10, false, LVOL_CLEAR_WITH_DEFAULT,
			      lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(g_lvol == NULL);

	g_lvol = NULL;
	rc = spdk_lvol_create(g_lvol_store, "lvol", DEV_BUFFER_SIZE + 1, false, LVOL_CLEAR_WITH_DEFAULT,
			      lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno != 0);
	CU_ASSERT(g_lvol == NULL);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);
}

static void
lvol_destroy_fail(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", 10, false, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	spdk_lvol_close(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	spdk_lvol_destroy(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	spdk_lvol_create(g_lvol_store, "lvol", 10, false, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	spdk_lvol_close(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_remove_rc = -1;
	spdk_lvol_destroy(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno != 0);
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_store->lvols));
	g_remove_rc = 0;

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);
}

static void
lvol_close(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvol *lvol;
	struct spdk_lvol_store *lvs;
	struct spdk_lvs_opts opts;
	struct ut_cb_res cb_res;

	int rc = 0;

	init_dev(&dev);

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete,
			   ut_cb_res_clear(&cb_res));
	CU_ASSERT(rc == 0);
	CU_ASSERT(cb_res.err == 0);
	SPDK_CU_ASSERT_FATAL(cb_res.data != NULL);
	lvs = cb_res.data;

	spdk_lvol_create(lvs, "lvol", 10, false, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, ut_cb_res_clear(&cb_res));
	CU_ASSERT(cb_res.err == 0);
	SPDK_CU_ASSERT_FATAL(cb_res.data != NULL);
	lvol = cb_res.data;
	CU_ASSERT(lvol->action_in_progress == false);

	/* Fail - lvol does not exist */
	spdk_lvol_close(NULL, op_complete, ut_cb_res_clear(&cb_res));
	CU_ASSERT(cb_res.err == -ENODEV);
	CU_ASSERT(lvol->action_in_progress == false);

	/* Fail - lvol not open */
	lvol->ref_count = 0;
	spdk_lvol_close(lvol, op_complete, ut_cb_res_clear(&cb_res));
	CU_ASSERT(cb_res.err == -EINVAL);
	CU_ASSERT(lvol->action_in_progress == false);
	lvol->ref_count = 1;

	/* Fail - blob close fails */
	lvol->blob->close_status = -1;
	spdk_lvol_close(lvol, op_complete, ut_cb_res_clear(&cb_res));
	CU_ASSERT(cb_res.err == -1);
	CU_ASSERT(lvol->action_in_progress == false);
	lvol->blob->close_status = 0;

	/* Success */
	spdk_lvol_close(lvol, op_complete, ut_cb_res_clear(&cb_res));
	CU_ASSERT(cb_res.err == 0);

	rc = spdk_lvs_unload(lvs, op_complete, ut_cb_res_clear(&cb_res));
	CU_ASSERT(rc == 0);
	CU_ASSERT(cb_res.err == 0);

	free_dev(&dev);
}

static void
lvol_resize(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_resize_rc = 0;
	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", 10, false, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	/* Resize to same size */
	spdk_lvol_resize(g_lvol, 10, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	/* Resize to smaller size */
	spdk_lvol_resize(g_lvol, 5, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	/* Resize to bigger size */
	spdk_lvol_resize(g_lvol, 15, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	/* Resize to size = 0 */
	spdk_lvol_resize(g_lvol, 0, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	/* Resize to bigger size than available */
	g_lvserrno = 0;
	spdk_lvol_resize(g_lvol, 0xFFFFFFFF, op_complete, NULL);
	CU_ASSERT(g_lvserrno != 0);

	/* Fail resize */
	g_resize_rc = -1;
	g_lvserrno = 0;
	spdk_lvol_resize(g_lvol, 10, op_complete, NULL);
	CU_ASSERT(g_lvserrno != 0);
	g_resize_rc = 0;

	g_resize_rc = 0;
	spdk_lvol_close(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	spdk_lvol_destroy(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);
}

static void
lvol_set_read_only(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;
	struct spdk_lvol *lvol, *clone;

	init_dev(&dev);

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", 10, false, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol = g_lvol;

	/* Set lvol as read only */
	spdk_lvol_set_read_only(lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	/* Create lvol clone from read only lvol */
	spdk_lvol_create_clone(lvol, "clone", lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT_STRING_EQUAL(g_lvol->name, "clone");
	clone = g_lvol;

	spdk_lvol_close(lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	spdk_lvol_close(clone, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);
}

static void
null_cb(void *ctx, struct spdk_blob_store *bs, int bserrno)
{
	SPDK_CU_ASSERT_FATAL(bs != NULL);
}

static void
test_lvs_load(void)
{
	int rc = -1;
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_with_handle_req *req;
	struct spdk_bs_opts bs_opts = {};
	struct spdk_blob *super_blob;
	struct spdk_lvs_opts opts = {};

	req = calloc(1, sizeof(*req));
	SPDK_CU_ASSERT_FATAL(req != NULL);

	init_dev(&dev);
	spdk_bs_opts_init(&bs_opts, sizeof(bs_opts));
	snprintf(bs_opts.bstype.bstype, sizeof(bs_opts.bstype.bstype), "LVOLSTORE");
	spdk_bs_init(&dev.bs_dev, &bs_opts, null_cb, NULL);
	SPDK_CU_ASSERT_FATAL(dev.bs != NULL);

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
	spdk_blob_set_xattr(super_blob, "uuid", uuid, SPDK_UUID_STRING_LEN);
	spdk_lvs_load(&dev.bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno == -EINVAL);
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_stores));

	/* Fail on closing super blob */
	g_lvserrno = 0;
	spdk_blob_set_xattr(super_blob, "name", "lvs", strlen("lvs") + 1);
	super_blob->close_status = -1;
	spdk_lvs_load(&dev.bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno == -ENODEV);
	CU_ASSERT(g_lvol_store == NULL);
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_stores));

	/* Fail on invalid options */
	g_lvserrno = -1;
	spdk_lvs_opts_init(&opts);
	opts.opts_size = 0; /* Invalid length */
	spdk_lvs_load_ext(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == -EINVAL);
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
	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_stores));

	free(req);
	free_dev(&dev);
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
	spdk_bs_opts_init(&bs_opts, sizeof(bs_opts));
	snprintf(bs_opts.bstype.bstype, sizeof(bs_opts.bstype.bstype), "LVOLSTORE");
	spdk_bs_init(&dev.bs_dev, &bs_opts, null_cb, NULL);
	super_blob = calloc(1, sizeof(*super_blob));
	SPDK_CU_ASSERT_FATAL(super_blob != NULL);
	super_blob->id = 0x100;
	spdk_blob_set_xattr(super_blob, "uuid", uuid, SPDK_UUID_STRING_LEN);
	spdk_blob_set_xattr(super_blob, "name", "lvs", strlen("lvs") + 1);
	TAILQ_INSERT_TAIL(&dev.bs->blobs, super_blob, link);
	dev.bs->super_blobid = 0x100;

	/*
	 * Create 3 blobs, write different char values to the last char in the UUID
	 *  to make sure they are unique.
	 */
	blob1 = calloc(1, sizeof(*blob1));
	SPDK_CU_ASSERT_FATAL(blob1 != NULL);
	blob1->id = 0x1;
	spdk_blob_set_xattr(blob1, "uuid", uuid, SPDK_UUID_STRING_LEN);
	spdk_blob_set_xattr(blob1, "name", "lvol1", strlen("lvol1") + 1);
	blob1->uuid[SPDK_UUID_STRING_LEN - 2] = '1';

	blob2 = calloc(1, sizeof(*blob2));
	SPDK_CU_ASSERT_FATAL(blob2 != NULL);
	blob2->id = 0x2;
	spdk_blob_set_xattr(blob2, "uuid", uuid, SPDK_UUID_STRING_LEN);
	spdk_blob_set_xattr(blob2, "name", "lvol2", strlen("lvol2") + 1);
	blob2->uuid[SPDK_UUID_STRING_LEN - 2] = '2';

	blob3 = calloc(1, sizeof(*blob3));
	SPDK_CU_ASSERT_FATAL(blob3 != NULL);
	blob3->id = 0x3;
	spdk_blob_set_xattr(blob3, "uuid", uuid, SPDK_UUID_STRING_LEN);
	spdk_blob_set_xattr(blob3, "name", "lvol3", strlen("lvol3") + 1);
	blob3->uuid[SPDK_UUID_STRING_LEN - 2] = '3';

	/* Load lvs with 0 blobs */
	g_lvserrno = 0;
	spdk_lvs_load(&dev.bs_dev, lvol_store_op_with_handle_complete, req);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(g_lvol_store != NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
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
	/* rc = */ spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	/*
	 * Disable these two asserts for now.  lvolstore should allow unload as long
	 *  as the lvols were not opened - but this is coming a future patch.
	 */
	/* CU_ASSERT(rc == 0); */
	/* CU_ASSERT(g_lvserrno == 0); */

	free(req);
	free_dev(&dev);
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
	spdk_bs_opts_init(&bs_opts, sizeof(bs_opts));
	snprintf(bs_opts.bstype.bstype, sizeof(bs_opts.bstype.bstype), "LVOLSTORE");
	spdk_bs_init(&dev.bs_dev, &bs_opts, null_cb, NULL);
	super_blob = calloc(1, sizeof(*super_blob));
	SPDK_CU_ASSERT_FATAL(super_blob != NULL);
	super_blob->id = 0x100;
	spdk_blob_set_xattr(super_blob, "uuid", uuid, SPDK_UUID_STRING_LEN);
	spdk_blob_set_xattr(super_blob, "name", "lvs", strlen("lvs") + 1);
	TAILQ_INSERT_TAIL(&dev.bs->blobs, super_blob, link);
	dev.bs->super_blobid = 0x100;

	/*
	 * Create 3 blobs, write different char values to the last char in the UUID
	 *  to make sure they are unique.
	 */
	blob1 = calloc(1, sizeof(*blob1));
	SPDK_CU_ASSERT_FATAL(blob1 != NULL);
	blob1->id = 0x1;
	spdk_blob_set_xattr(blob1, "uuid", uuid, SPDK_UUID_STRING_LEN);
	spdk_blob_set_xattr(blob1, "name", "lvol1", strlen("lvol1") + 1);
	blob1->uuid[SPDK_UUID_STRING_LEN - 2] = '1';

	blob2 = calloc(1, sizeof(*blob2));
	SPDK_CU_ASSERT_FATAL(blob2 != NULL);
	blob2->id = 0x2;
	spdk_blob_set_xattr(blob2, "uuid", uuid, SPDK_UUID_STRING_LEN);
	spdk_blob_set_xattr(blob2, "name", "lvol2", strlen("lvol2") + 1);
	blob2->uuid[SPDK_UUID_STRING_LEN - 2] = '2';

	blob3 = calloc(1, sizeof(*blob3));
	SPDK_CU_ASSERT_FATAL(blob3 != NULL);
	blob3->id = 0x2;
	spdk_blob_set_xattr(blob3, "uuid", uuid, SPDK_UUID_STRING_LEN);
	spdk_blob_set_xattr(blob3, "name", "lvol3", strlen("lvol3") + 1);
	blob3->uuid[SPDK_UUID_STRING_LEN - 2] = '3';

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
		spdk_lvol_close(lvol, op_complete, NULL);
		CU_ASSERT(g_lvserrno == 0);
	}

	g_lvserrno = -1;
	spdk_lvs_destroy(g_lvol_store, op_complete, NULL);

	free(req);
	free(blob1);
	free(blob2);
	free(blob3);
}

static void
lvol_snapshot(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvol *lvol;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", 10, true, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	lvol = g_lvol;

	spdk_lvol_create_snapshot(lvol, "snap", lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT_STRING_EQUAL(g_lvol->name, "snap");

	/* Lvol has to be closed (or destroyed) before unloading lvol store. */
	spdk_lvol_close(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	g_lvserrno = -1;

	spdk_lvol_close(lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	g_lvserrno = -1;

	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);
}

static void
lvol_snapshot_fail(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvol *lvol, *snap;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", 10, true, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	lvol = g_lvol;

	spdk_lvol_create_snapshot(NULL, "snap", lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno < 0);
	SPDK_CU_ASSERT_FATAL(g_lvol == NULL);

	spdk_lvol_create_snapshot(lvol, "", lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno < 0);
	SPDK_CU_ASSERT_FATAL(g_lvol == NULL);

	spdk_lvol_create_snapshot(lvol, NULL, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno < 0);
	SPDK_CU_ASSERT_FATAL(g_lvol == NULL);

	spdk_lvol_create_snapshot(lvol, "snap", lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT_STRING_EQUAL(g_lvol->name, "snap");

	snap = g_lvol;

	spdk_lvol_create_snapshot(lvol, "snap", lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno < 0);

	spdk_lvol_close(lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	g_lvserrno = -1;

	spdk_lvol_close(snap, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	g_lvserrno = -1;

	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);
}

static void
lvol_clone(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvol *lvol;
	struct spdk_lvol *snap;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", 10, true, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	lvol = g_lvol;

	spdk_lvol_create_snapshot(lvol, "snap", lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT_STRING_EQUAL(g_lvol->name, "snap");

	snap = g_lvol;

	spdk_lvol_create_clone(snap, "clone", lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT_STRING_EQUAL(g_lvol->name, "clone");

	/* Lvol has to be closed (or destroyed) before unloading lvol store. */
	spdk_lvol_close(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	g_lvserrno = -1;

	spdk_lvol_close(snap, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	g_lvserrno = -1;

	spdk_lvol_close(lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	g_lvserrno = -1;

	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);
}

static void
lvol_clone_fail(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvol *lvol;
	struct spdk_lvol *snap;
	struct spdk_lvol *clone;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", 10, true, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	lvol = g_lvol;

	spdk_lvol_create_snapshot(lvol, "snap", lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT_STRING_EQUAL(g_lvol->name, "snap");

	snap = g_lvol;

	spdk_lvol_create_clone(NULL, "clone", lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno < 0);

	spdk_lvol_create_clone(snap, "", lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno < 0);

	spdk_lvol_create_clone(snap, NULL, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno < 0);

	spdk_lvol_create_clone(snap, "clone", lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT_STRING_EQUAL(g_lvol->name, "clone");

	clone = g_lvol;

	spdk_lvol_create_clone(snap, "clone", lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno < 0);

	/* Lvol has to be closed (or destroyed) before unloading lvol store. */
	spdk_lvol_close(clone, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	g_lvserrno = -1;

	spdk_lvol_close(snap, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	g_lvserrno = -1;

	spdk_lvol_close(lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	g_lvserrno = -1;

	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);
}

static void
lvol_iter_clones(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvol *lvol, *snap, *clone;
	struct spdk_lvs_opts opts;
	struct count_clones_ctx ctx = { 0 };
	spdk_blob_id mock_clones[2];
	int rc = 0;

	init_dev(&dev);

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_spdk_blob_get_clones_ids = mock_clones;

	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	/* Create a volume */
	spdk_lvol_create(g_lvol_store, "lvol", 10, true, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol = g_lvol;

	/* Create a snapshot of the volume */
	spdk_lvol_create_snapshot(lvol, "snap", lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT_STRING_EQUAL(g_lvol->name, "snap");
	snap = g_lvol;

	g_spdk_blob_get_clones_snap_id = snap->blob_id;
	g_spdk_blob_get_clones_count = 1;
	mock_clones[0] = lvol->blob_id;

	/* The snapshot turned the lvol into a clone, so the snapshot now has one clone. */
	memset(&ctx, 0, sizeof(ctx));
	rc = spdk_lvol_iter_immediate_clones(snap, count_clones, &ctx);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ctx.count == 1);

	/* The snapshotted volume still has no clones. */
	memset(&ctx, 0, sizeof(ctx));
	rc = spdk_lvol_iter_immediate_clones(lvol, count_clones, &ctx);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ctx.count == 0);

	/* Iteration can be stopped and the return value is propagated. */
	memset(&ctx, 0, sizeof(ctx));
	ctx.stop_on_lvol = lvol;
	ctx.stop_errno = 42;
	rc = spdk_lvol_iter_immediate_clones(snap, count_clones, &ctx);
	CU_ASSERT(rc == 42);
	CU_ASSERT(ctx.count == 0);

	/* Create a clone of the snapshot */
	spdk_lvol_create_clone(snap, "clone", lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT_STRING_EQUAL(g_lvol->name, "clone");
	clone = g_lvol;

	g_spdk_blob_get_clones_count = 2;
	mock_clones[1] = clone->blob_id;

	/* The snapshot now has two clones */
	memset(&ctx, 0, sizeof(ctx));
	rc = spdk_lvol_iter_immediate_clones(snap, count_clones, &ctx);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ctx.count == 2);

	/* Cleanup */
	g_spdk_blob_get_clones_snap_id = 0xbad;
	g_spdk_blob_get_clones_count = 0;
	g_spdk_blob_get_clones_ids = NULL;

	spdk_lvol_close(snap, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	spdk_lvol_close(clone, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	spdk_lvol_close(lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;
	g_lvol = NULL;

	free_dev(&dev);
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

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_lvserrno = -1;
	g_lvol_store = NULL;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	lvs = g_lvol_store;

	rc = spdk_lvol_create(lvs, NULL, 1, false, LVOL_CLEAR_WITH_DEFAULT, lvol_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == -EINVAL);

	rc = spdk_lvol_create(lvs, "", 1, false, LVOL_CLEAR_WITH_DEFAULT, lvol_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == -EINVAL);

	memset(fullname, 'x', sizeof(fullname));
	rc = spdk_lvol_create(lvs, fullname, 1, false, LVOL_CLEAR_WITH_DEFAULT,
			      lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == -EINVAL);

	g_lvserrno = -1;
	rc = spdk_lvol_create(lvs, "lvol", 1, false, LVOL_CLEAR_WITH_DEFAULT, lvol_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol = g_lvol;

	rc = spdk_lvol_create(lvs, "lvol", 1, false, LVOL_CLEAR_WITH_DEFAULT, lvol_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == -EEXIST);

	g_lvserrno = -1;
	rc = spdk_lvol_create(lvs, "lvol2", 1, false, LVOL_CLEAR_WITH_DEFAULT, lvol_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol2 = g_lvol;

	spdk_lvol_close(lvol, op_complete, NULL);
	spdk_lvol_destroy(lvol, op_complete, NULL);

	g_lvserrno = -1;
	g_lvol = NULL;
	rc = spdk_lvol_create(lvs, "lvol", 1, false, LVOL_CLEAR_WITH_DEFAULT, lvol_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol = g_lvol;

	spdk_lvol_close(lvol, op_complete, NULL);
	spdk_lvol_destroy(lvol, op_complete, NULL);

	spdk_lvol_close(lvol2, op_complete, NULL);
	spdk_lvol_destroy(lvol2, op_complete, NULL);

	/* Simulate creating two lvols with same name simultaneously. */
	lvol = calloc(1, sizeof(*lvol));
	SPDK_CU_ASSERT_FATAL(lvol != NULL);
	snprintf(lvol->name, sizeof(lvol->name), "tmp_name");
	TAILQ_INSERT_TAIL(&lvs->pending_lvols, lvol, link);
	rc = spdk_lvol_create(lvs, "tmp_name", 1, false, LVOL_CLEAR_WITH_DEFAULT,
			      lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == -EEXIST);

	/* Remove name from temporary list and try again. */
	TAILQ_REMOVE(&lvs->pending_lvols, lvol, link);
	free(lvol);

	rc = spdk_lvol_create(lvs, "tmp_name", 1, false, LVOL_CLEAR_WITH_DEFAULT,
			      lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol = g_lvol;

	spdk_lvol_close(lvol, op_complete, NULL);
	spdk_lvol_destroy(lvol, op_complete, NULL);

	g_lvserrno = -1;
	rc = spdk_lvs_destroy(lvs, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;
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

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_lvserrno = -1;
	g_lvol_store = NULL;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	lvs = g_lvol_store;

	/* Trying to create new lvol */
	g_lvserrno = -1;
	rc = spdk_lvol_create(lvs, "lvol", 1, false, LVOL_CLEAR_WITH_DEFAULT, lvol_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol = g_lvol;

	/* Trying to create second lvol with existing lvol name */
	g_lvserrno = -1;
	g_lvol = NULL;
	rc = spdk_lvol_create(lvs, "lvol", 1, false, LVOL_CLEAR_WITH_DEFAULT, lvol_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == -EEXIST);
	CU_ASSERT(g_lvserrno == -1);
	SPDK_CU_ASSERT_FATAL(g_lvol == NULL);

	/* Trying to create second lvol with non existing name */
	g_lvserrno = -1;
	rc = spdk_lvol_create(lvs, "lvol2", 1, false, LVOL_CLEAR_WITH_DEFAULT, lvol_op_with_handle_complete,
			      NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol2 = g_lvol;

	/* Trying to rename lvol with not existing name */
	spdk_lvol_rename(lvol, "lvol_new", op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT_STRING_EQUAL(lvol->name, "lvol_new");

	/* Trying to rename lvol with other lvol name */
	spdk_lvol_rename(lvol2, "lvol_new", op_complete, NULL);
	CU_ASSERT(g_lvserrno == -EEXIST);
	CU_ASSERT_STRING_NOT_EQUAL(lvol2->name, "lvol_new");

	spdk_lvol_close(lvol, op_complete, NULL);
	spdk_lvol_destroy(lvol, op_complete, NULL);

	spdk_lvol_close(lvol2, op_complete, NULL);
	spdk_lvol_destroy(lvol2, op_complete, NULL);

	g_lvserrno = -1;
	rc = spdk_lvs_destroy(lvs, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;
}

static void
lvs_rename(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	struct spdk_lvol_store *lvs, *lvs2;
	int rc = 0;

	init_dev(&dev);

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");
	g_lvserrno = -1;
	g_lvol_store = NULL;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	lvs = g_lvol_store;

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "unimportant_lvs_name");
	g_lvserrno = -1;
	g_lvol_store = NULL;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	lvs2 = g_lvol_store;

	/* Trying to rename lvs with new name */
	spdk_lvs_rename(lvs, "new_lvs_name", op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT_STRING_EQUAL(lvs->name, "new_lvs_name");

	/* Trying to rename lvs with name lvs already has */
	spdk_lvs_rename(lvs, "new_lvs_name", op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT_STRING_EQUAL(lvs->name, "new_lvs_name");

	/* Trying to rename lvs with name already existing */
	spdk_lvs_rename(lvs2, "new_lvs_name", op_complete, NULL);
	CU_ASSERT(g_lvserrno == -EEXIST);
	CU_ASSERT_STRING_EQUAL(lvs2->name, "unimportant_lvs_name");

	/* Trying to rename lvs with another rename process started with the same name */
	/* Simulate renaming process in progress */
	snprintf(lvs2->new_name, sizeof(lvs2->new_name), "another_new_lvs_name");
	CU_ASSERT_STRING_EQUAL(lvs2->new_name, "another_new_lvs_name");
	/* Start second process */
	spdk_lvs_rename(lvs, "another_new_lvs_name", op_complete, NULL);
	CU_ASSERT(g_lvserrno == -EEXIST);
	CU_ASSERT_STRING_EQUAL(lvs->name, "new_lvs_name");
	/* reverting lvs2 new name to proper value */
	snprintf(lvs2->new_name, sizeof(lvs2->new_name), "unimportant_lvs_name");
	CU_ASSERT_STRING_EQUAL(lvs2->new_name, "unimportant_lvs_name");

	/* Simulate error while lvs rename */
	g_lvs_rename_blob_open_error = true;
	spdk_lvs_rename(lvs, "complete_new_lvs_name", op_complete, NULL);
	CU_ASSERT(g_lvserrno != 0);
	CU_ASSERT_STRING_EQUAL(lvs->name, "new_lvs_name");
	CU_ASSERT_STRING_EQUAL(lvs->new_name, "new_lvs_name");
	g_lvs_rename_blob_open_error = false;

	g_lvserrno = -1;
	rc = spdk_lvs_destroy(lvs, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	g_lvserrno = -1;
	rc = spdk_lvs_destroy(lvs2, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;
}
static void
lvol_refcnt(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	struct spdk_lvol *lvol;
	int rc = 0;

	init_dev(&dev);

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);


	spdk_lvol_create(g_lvol_store, "lvol", 10, false, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);

	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT(g_lvol->ref_count == 1);

	lvol = g_lvol;
	spdk_lvol_open(g_lvol, lvol_op_with_handle_complete, NULL);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT(lvol->ref_count == 2);

	/* Trying to destroy lvol while its open should fail */
	spdk_lvol_destroy(lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno != 0);

	spdk_lvol_close(lvol, op_complete, NULL);
	CU_ASSERT(lvol->ref_count == 1);
	CU_ASSERT(g_lvserrno == 0);

	spdk_lvol_close(lvol, op_complete, NULL);
	CU_ASSERT(lvol->ref_count == 0);
	CU_ASSERT(g_lvserrno == 0);

	/* Try to close already closed lvol */
	spdk_lvol_close(lvol, op_complete, NULL);
	CU_ASSERT(lvol->ref_count == 0);
	CU_ASSERT(g_lvserrno != 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);
}

static void
lvol_create_thin_provisioned(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", 10, false, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	CU_ASSERT(g_lvol->blob->thin_provisioned == false);

	spdk_lvol_close(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	spdk_lvol_destroy(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	spdk_lvol_create(g_lvol_store, "lvol", 10, true, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	CU_ASSERT(g_lvol->blob->thin_provisioned == true);

	spdk_lvol_close(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	spdk_lvol_destroy(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);
}

static void
lvol_inflate(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", 10, false, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	g_inflate_rc = -1;
	spdk_lvol_inflate(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno != 0);

	g_inflate_rc = 0;
	spdk_lvol_inflate(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	spdk_lvol_close(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	spdk_lvol_destroy(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);

	/* Make sure that all references to the io_channel was closed after
	 * inflate call
	 */
	CU_ASSERT(g_io_channel == NULL);
}

static void
lvol_decouple_parent(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;

	init_dev(&dev);

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", 10, false, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	g_inflate_rc = -1;
	spdk_lvol_decouple_parent(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno != 0);

	g_inflate_rc = 0;
	spdk_lvol_decouple_parent(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	spdk_lvol_close(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	spdk_lvol_destroy(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);

	/* Make sure that all references to the io_channel was closed after
	 * inflate call
	 */
	CU_ASSERT(g_io_channel == NULL);
}

static void
lvol_get_xattr(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	int rc = 0;
	struct spdk_lvol *lvol;
	const char *value = NULL;
	size_t value_len = 0;

	init_dev(&dev);

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", 10, false, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol = g_lvol;

	/* Should be able to look up name */
	lvol_get_xattr_value(lvol, "name", (const void **)&value, &value_len);
	CU_ASSERT(value != NULL && strcmp(value, "lvol") == 0);
	CU_ASSERT(value_len != 0);

	/* Looking up something that doesn't exist should indicate non-existence */
	lvol_get_xattr_value(lvol, "mumble", (const void **)&value, &value_len);
	CU_ASSERT(value == NULL);
	CU_ASSERT(value_len == 0);

	/* Clean up */
	spdk_lvol_close(lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	spdk_lvol_destroy(lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);
}

struct spdk_bs_dev *g_esnap_bs_dev;
int g_esnap_bs_dev_errno = -ENOTSUP;

static int
ut_esnap_bs_dev_create(void *bs_ctx, void *blob_ctx, struct spdk_blob *blob,
		       const void *esnap_id, uint32_t id_len,
		       struct spdk_bs_dev **_bs_dev)
{
	*_bs_dev = g_esnap_bs_dev;
	return g_esnap_bs_dev_errno;
}

static void
lvol_esnap_reload(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_with_handle_req *req;
	struct spdk_lvs_opts opts;
	int rc;

	g_esnap_bs_dev = NULL;
	g_esnap_bs_dev_errno = -ENOTSUP;

	req = calloc(1, sizeof(*req));
	SPDK_CU_ASSERT_FATAL(req != NULL);

	init_dev(&dev);

	/* Create an lvstore with external snapshot support */
	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");
	opts.esnap_bs_dev_create = ut_esnap_bs_dev_create;
	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	CU_ASSERT(dev.bs->esnap_bs_dev_create == ut_esnap_bs_dev_create);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	/* Unload the lvstore */
	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	/* Load the lvstore with external snapshot support */
	g_lvserrno = -1;
	spdk_lvs_opts_init(&opts);
	opts.esnap_bs_dev_create = ut_esnap_bs_dev_create;
	spdk_lvs_load_ext(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	CU_ASSERT(dev.bs->esnap_bs_dev_create == ut_esnap_bs_dev_create);

	g_lvserrno = -1;
	rc = spdk_lvs_destroy(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free(req);
}

static void
lvol_esnap_create_bad_args(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_bdev esnap_bdev;
	struct spdk_lvs_opts opts;
	char long_name[SPDK_LVOL_NAME_MAX + 1];
	int rc;
	struct ut_cb_res lvres1, lvres2;
	struct spdk_lvol *lvol;
	char uuid_str[SPDK_UUID_STRING_LEN];
	uint64_t block_sz, cluster_sz;

	init_dev(&dev);
	block_sz = dev.bs_dev.blocklen;

	spdk_lvs_opts_init(&opts);
	cluster_sz = opts.cluster_sz;
	snprintf(opts.name, sizeof(opts.name), "lvs");
	opts.esnap_bs_dev_create = ut_esnap_bs_dev_create;
	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	init_bdev(&esnap_bdev, "bdev1", BS_CLUSTER_SIZE);
	CU_ASSERT(spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &esnap_bdev.uuid) == 0);
	MOCK_SET(spdk_bdev_get_by_name, &esnap_bdev);

	/* error with lvs == NULL */
	rc = spdk_lvol_create_esnap_clone(uuid_str, strlen(uuid_str), cluster_sz, NULL, "clone1",
					  lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == -EINVAL);

	/* error with clone name that is too short */
	rc = spdk_lvol_create_esnap_clone(uuid_str, strlen(uuid_str), cluster_sz, g_lvol_store, "",
					  lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == -EINVAL);

	/* error with clone name that is too long */
	memset(long_name, 'a', sizeof(long_name));
	rc = spdk_lvol_create_esnap_clone(uuid_str, strlen(uuid_str), cluster_sz, g_lvol_store,
					  long_name, lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == -EINVAL);

	/* error with size that is not a multiple of an integer multiple of cluster_sz */
	CU_ASSERT(((cluster_sz + block_sz) % cluster_sz) != 0);
	rc = spdk_lvol_create_esnap_clone(uuid_str, strlen(uuid_str), cluster_sz + block_sz,
					  g_lvol_store, "clone1",
					  lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == -EINVAL);

	/* error when an lvol with that name already exists */
	spdk_lvol_create(g_lvol_store, "lvol", 10, false, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol = g_lvol;
	rc = spdk_lvol_create_esnap_clone(uuid_str, strlen(uuid_str), cluster_sz, g_lvol_store,
					  "lvol", lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == -EEXIST);
	spdk_lvol_close(lvol, op_complete, ut_cb_res_clear(&lvres1));
	spdk_lvol_destroy(lvol, op_complete, ut_cb_res_clear(&lvres2));
	poll_threads();
	CU_ASSERT(lvres1.err == 0);
	CU_ASSERT(lvres2.err == 0);
	g_lvol = NULL;

	/* error when two clones created at the same time with the same name */
	rc = spdk_lvol_create_esnap_clone(uuid_str, strlen(uuid_str), cluster_sz, g_lvol_store,
					  "clone1", lvol_op_with_handle_complete,
					  ut_cb_res_clear(&lvres1));
	rc = spdk_lvol_create_esnap_clone(uuid_str, strlen(uuid_str), cluster_sz, g_lvol_store,
					  "clone1", lvol_op_with_handle_complete,
					  ut_cb_res_clear(&lvres2));
	CU_ASSERT(rc == -EEXIST);
	poll_threads();
	CU_ASSERT(g_lvol != NULL);
	CU_ASSERT(lvres1.err == 0);
	CU_ASSERT(lvres2.err == 0xbad);
	CU_ASSERT(TAILQ_EMPTY(&g_lvol_store->pending_lvols));
	spdk_lvol_close(g_lvol, op_complete, ut_cb_res_clear(&lvres1));
	spdk_lvol_destroy(g_lvol, op_complete, ut_cb_res_clear(&lvres2));
	poll_threads();
	CU_ASSERT(lvres1.err == 0);
	CU_ASSERT(lvres2.err == 0);
	g_lvol = NULL;

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&dev);
}

static void
lvol_esnap_create_delete(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_bdev esnap_bdev;
	struct spdk_lvs_opts opts;
	char uuid_str[SPDK_UUID_STRING_LEN];
	int rc;
	uint64_t cluster_sz;

	init_dev(&dev);
	init_dev(&g_esnap_dev);

	spdk_lvs_opts_init(&opts);
	cluster_sz = opts.cluster_sz;
	snprintf(opts.name, sizeof(opts.name), "lvs");
	opts.esnap_bs_dev_create = ut_esnap_bs_dev_create;
	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	g_lvserrno = 0xbad;
	init_bdev(&esnap_bdev, "bdev1", BS_CLUSTER_SIZE);
	CU_ASSERT(spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &esnap_bdev.uuid) == 0);
	MOCK_SET(spdk_bdev_get_by_name, &esnap_bdev);
	rc = spdk_lvol_create_esnap_clone(uuid_str, strlen(uuid_str), cluster_sz, g_lvol_store,
					  "clone1", lvol_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	MOCK_CLEAR(spdk_bdev_get_by_name);

	g_lvserrno = 0xbad;
	spdk_lvol_close(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	g_lvserrno = 0xbad;
	spdk_lvol_destroy(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol = NULL;

	g_lvserrno = -1;
	rc = spdk_lvs_destroy(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;
}

static void
lvol_esnap_load_esnaps(void)
{
	struct spdk_blob	blob = { .id = 42 };
	struct spdk_lvol_store	*lvs;
	struct spdk_lvol	*lvol;
	struct spdk_bs_dev	*bs_dev = NULL;
	struct spdk_bs_dev	esnap_bs_dev = { 0 };
	int			rc;
	uint64_t		esnap_id = 42;

	lvs = lvs_alloc();
	SPDK_CU_ASSERT_FATAL(lvs != NULL);
	lvs->esnap_bs_dev_create = ut_esnap_bs_dev_create;
	lvol = lvol_alloc(lvs, __func__, true, LVOL_CLEAR_WITH_DEFAULT, NULL);
	SPDK_CU_ASSERT_FATAL(lvol != NULL);

	/* Handle missing bs_ctx and blob_ctx gracefully */
	rc = lvs_esnap_bs_dev_create(NULL, NULL, &blob, &esnap_id, sizeof(esnap_id), &bs_dev);
	CU_ASSERT(rc == -EINVAL);

	/* Do not try to load external snapshot when load_esnaps is false */
	g_spdk_blob_get_esnap_id_called = false;
	bs_dev = NULL;
	rc = lvs_esnap_bs_dev_create(lvs, lvol, &blob, &esnap_id, sizeof(esnap_id), &bs_dev);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bs_dev == NULL);
	CU_ASSERT(!g_spdk_blob_get_esnap_id_called);

	/* Same, with only lvs */
	bs_dev = NULL;
	rc = lvs_esnap_bs_dev_create(lvs, NULL, &blob, &esnap_id, sizeof(esnap_id), &bs_dev);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bs_dev == NULL);
	CU_ASSERT(!g_spdk_blob_get_esnap_id_called);

	/* Same, with only lvol */
	bs_dev = NULL;
	rc = lvs_esnap_bs_dev_create(NULL, lvol, &blob, &esnap_id, sizeof(esnap_id), &bs_dev);
	CU_ASSERT(rc == 0);
	CU_ASSERT(bs_dev == NULL);
	CU_ASSERT(!g_spdk_blob_get_esnap_id_called);

	/* Happy path */
	g_esnap_bs_dev = &esnap_bs_dev;
	g_esnap_bs_dev_errno = 0;

	lvs->load_esnaps = true;
	ut_spdk_bdev_create_bs_dev_ro = 0;
	g_spdk_blob_get_esnap_id_errno = 0;
	bs_dev = NULL;
	rc = lvs_esnap_bs_dev_create(lvs, lvol, &blob, &esnap_id, sizeof(esnap_id), &bs_dev);
	CU_ASSERT(rc == 0);

	/* Clean up */
	lvol_free(lvol);
	lvs_free(lvs);
	g_esnap_bs_dev = NULL;
	g_esnap_bs_dev_errno = -ENOTSUP;
}

struct ut_degraded_dev {
	struct spdk_bs_dev	bs_dev;
	struct spdk_lvol	*lvol;
};

static void
ut_destroy_degraded(struct spdk_bs_dev *ddev)
{
	free(ddev);
}

static int
ut_create_degraded(struct spdk_lvol_store *lvs, struct spdk_lvol *lvol,
		   struct spdk_blob *blob, const char *name, struct spdk_bs_dev **bs_dev)
{
	struct ut_degraded_dev	*ddev;

	ddev = calloc(1, sizeof(*ddev));
	SPDK_CU_ASSERT_FATAL(ddev != NULL);

	ddev->lvol = lvol;
	ddev->bs_dev.destroy = ut_destroy_degraded;
	ddev->bs_dev.blockcnt = UINT64_MAX / 512;
	ddev->bs_dev.blocklen = 512;
	*bs_dev = &ddev->bs_dev;
	return 0;
}

static void
lvol_esnap_missing(void)
{
	struct lvol_ut_bs_dev	dev;
	struct spdk_lvs_opts	opts;
	struct spdk_blob	blob = { .id = 42 };
	struct ut_cb_res	cb_res;
	struct spdk_lvol_store	*lvs;
	struct spdk_lvol	*lvol1, *lvol2;
	struct spdk_bs_dev	*bs_dev;
	struct spdk_bdev	esnap_bdev;
	struct spdk_lvs_degraded_lvol_set *degraded_set;
	const char		*name1 = "lvol1";
	const char		*name2 = "lvol2";
	char			uuid_str[SPDK_UUID_STRING_LEN];
	uint64_t		cluster_sz;
	int			rc;

	/* Create an lvstore */
	init_dev(&dev);
	spdk_lvs_opts_init(&opts);
	cluster_sz = opts.cluster_sz;
	snprintf(opts.name, sizeof(opts.name), "lvs");
	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	lvs = g_lvol_store;
	lvs->load_esnaps = true;

	/* Pre-populate the lvstore with a degraded device */
	lvol1 = lvol_alloc(lvs, name1, true, LVOL_CLEAR_WITH_DEFAULT, NULL);
	SPDK_CU_ASSERT_FATAL(lvol1 != NULL);
	lvol1->blob_id = blob.id;
	TAILQ_REMOVE(&lvs->pending_lvols, lvol1, link);
	TAILQ_INSERT_TAIL(&lvs->lvols, lvol1, link);
	rc = ut_create_degraded(lvs, lvol1, &blob, name1, &bs_dev);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(bs_dev != NULL);

	/* A clone with a missing external snapshot prevents a conflicting clone's creation */
	init_bdev(&esnap_bdev, "bdev1", BS_CLUSTER_SIZE);
	CU_ASSERT(spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &esnap_bdev.uuid) == 0);
	MOCK_SET(spdk_bdev_get_by_name, &esnap_bdev);
	rc = spdk_lvol_create_esnap_clone(uuid_str, sizeof(uuid_str), cluster_sz, g_lvol_store,
					  name1, lvol_op_with_handle_complete,
					  ut_cb_res_clear(&cb_res));
	CU_ASSERT(rc == -EEXIST);
	CU_ASSERT(ut_cb_res_untouched(&cb_res));
	MOCK_CLEAR(spdk_bdev_get_by_name);

	/* A clone with a missing external snapshot prevents a conflicting lvol's creation */
	rc = spdk_lvol_create(lvs, name1, 10, false, LVOL_CLEAR_WITH_DEFAULT,
			      lvol_op_with_handle_complete, ut_cb_res_clear(&cb_res));
	CU_ASSERT(rc == -EEXIST);
	CU_ASSERT(ut_cb_res_untouched(&cb_res));

	/* Using a unique lvol name allows the clone to be created. */
	MOCK_SET(spdk_bdev_get_by_name, &esnap_bdev);
	MOCK_SET(spdk_blob_is_esnap_clone, true);
	rc = spdk_lvol_create_esnap_clone(uuid_str, sizeof(uuid_str), cluster_sz, g_lvol_store,
					  name2, lvol_op_with_handle_complete,
					  ut_cb_res_clear(&cb_res));
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(cb_res.err == 0);
	SPDK_CU_ASSERT_FATAL(cb_res.data != NULL);
	lvol2 = cb_res.data;
	CU_ASSERT(lvol2->degraded_set == NULL);
	spdk_lvol_close(lvol2, op_complete, ut_cb_res_clear(&cb_res));
	CU_ASSERT(cb_res.err == 0);
	spdk_lvol_destroy(lvol2, op_complete, ut_cb_res_clear(&cb_res));
	CU_ASSERT(cb_res.err == 0);
	MOCK_CLEAR(spdk_blob_is_esnap_clone);
	MOCK_CLEAR(spdk_bdev_get_by_name);

	/* Destroying the esnap clone removes it from the degraded_set esnaps tree. */
	spdk_lvol_destroy(lvol1, op_complete, ut_cb_res_clear(&cb_res));
	CU_ASSERT(cb_res.err == 0);
	CU_ASSERT(RB_EMPTY(&lvs->degraded_lvol_sets_tree));
	bs_dev->destroy(bs_dev);

	/* Create a missing device again */
	lvol1 = lvol_alloc(lvs, name1, true, LVOL_CLEAR_WITH_DEFAULT, NULL);
	SPDK_CU_ASSERT_FATAL(lvol1 != NULL);
	lvol1->blob_id = blob.id;
	TAILQ_REMOVE(&lvs->pending_lvols, lvol1, link);
	TAILQ_INSERT_TAIL(&lvs->lvols, lvol1, link);
	rc = ut_create_degraded(lvs, lvol1, &blob, name1, &bs_dev);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(bs_dev != NULL);
	lvol1->blob = &blob;
	rc = spdk_lvs_esnap_missing_add(lvs, lvol1, esnap_bdev.name, strlen(esnap_bdev.name) + 1);
	CU_ASSERT(rc == 0);
	lvol1->ref_count = 1;

	/*
	 * Creating a snapshot of lvol1 makes lvol1 a clone of the new snapshot. What was a clone of
	 * the external snapshot is now a clone of the snapshot. The snapshot is a clone of the
	 * external snapshot.  Now the snapshot is degraded_set its external snapshot.
	 */
	degraded_set = lvol1->degraded_set;
	CU_ASSERT(degraded_set != NULL);
	spdk_lvol_create_snapshot(lvol1, name2, lvol_op_with_handle_complete,
				  ut_cb_res_clear(&cb_res));
	CU_ASSERT(cb_res.err == 0);
	SPDK_CU_ASSERT_FATAL(cb_res.data != NULL);
	lvol2 = cb_res.data;
	CU_ASSERT(lvol1->degraded_set == NULL);
	CU_ASSERT(lvol2->degraded_set == degraded_set);

	/*
	 * Removing the snapshot (lvol2) makes the first lvol (lvol1) back into a clone of an
	 * external snapshot.
	 */
	MOCK_SET(spdk_blob_is_esnap_clone, true);
	g_spdk_blob_get_clones_snap_id = lvol2->blob_id;
	g_spdk_blob_get_clones_ids = &lvol1->blob_id;
	g_spdk_blob_get_clones_count = 1;
	spdk_lvol_close(lvol2, op_complete, ut_cb_res_clear(&cb_res));
	CU_ASSERT(cb_res.err == 0);
	spdk_lvol_destroy(lvol2, op_complete, ut_cb_res_clear(&cb_res));
	CU_ASSERT(cb_res.err == 0);
	CU_ASSERT(lvol1->degraded_set == degraded_set);
	g_spdk_blob_get_clones_snap_id = 0xbad;
	g_spdk_blob_get_clones_ids = NULL;
	g_spdk_blob_get_clones_count = 0;

	/* Clean up */
	spdk_lvol_close(lvol1, op_complete, ut_cb_res_clear(&cb_res));
	CU_ASSERT(cb_res.err == 0);
	spdk_lvol_destroy(lvol1, op_complete, ut_cb_res_clear(&cb_res));
	CU_ASSERT(cb_res.err == 0);
	bs_dev->destroy(bs_dev);
	rc = spdk_lvs_destroy(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	MOCK_CLEAR(spdk_blob_is_esnap_clone);
}

struct hotplug_lvol {
	/*
	 * These fields must be set before calling lvol_esnap_hotplug_scenario().
	 */
	char *lvol_name;
	char *esnap_id;
	/* How many times hotplug is expected to be called, likely 1. */
	int expect_hp_count;
	/* If not 0, return this during hotplug without registering esnap_dev. */
	int hotplug_retval;
	/* If true, call spdk_lvs_esnap_missing_add(), return 0, NULL bs_dev */
	bool register_missing;

	/*
	 * These fields set are set by lvol_esnap_hotplug_scenario().
	 */
	struct spdk_lvol *lvol;
	int id_len;
	int hp_count;
	bool created;
};

struct missing_esnap {
	char *esnap_id;
	struct spdk_bs_dev *esnap_dev;
	int expect_missing_lvol_count_after_create;
	int expect_missing_lvol_count_after_hotplug;
};

/* Arrays. Terminate with a zeroed struct. */
struct hotplug_lvol *g_hotplug_lvols;
struct missing_esnap *g_missing_esnap;

static int
missing_get_lvol_count(struct spdk_lvol_store *lvs, char *esnap_id)
{
	struct spdk_lvs_degraded_lvol_set find = { 0 };
	struct spdk_lvs_degraded_lvol_set *found;
	struct spdk_lvol *lvol;
	int count = 0;

	find.esnap_id = esnap_id;
	find.id_len = strlen(esnap_id) + 1;

	found = RB_FIND(degraded_lvol_sets_tree, &lvs->degraded_lvol_sets_tree, &find);
	if (found == NULL) {
		return 0;
	}
	TAILQ_FOREACH(lvol, &found->lvols, degraded_link) {
		count++;
	}
	return count;
}

static struct missing_esnap *
get_missing_esnap(struct missing_esnap *missing_esnap, const char *esnap_id)
{
	for (; missing_esnap->esnap_id != NULL; missing_esnap++) {
		if (strcmp(missing_esnap->esnap_id, esnap_id) == 0) {
			return missing_esnap;
		}
	}
	return NULL;
}

static int
ut_esnap_hotplug_dev_create(void *bs_ctx, void *blob_ctx, struct spdk_blob *blob,
			    const void *esnap_id, uint32_t id_len, struct spdk_bs_dev **bs_dev)
{
	struct spdk_lvol_store *lvs = bs_ctx;
	struct spdk_lvol *lvol = blob_ctx;
	struct hotplug_lvol *hp_lvol;
	struct missing_esnap *missing_esnap;
	int rc;

	CU_ASSERT(lvs != NULL);
	CU_ASSERT(lvol != NULL);

	for (hp_lvol = g_hotplug_lvols; hp_lvol->lvol != NULL; hp_lvol++) {
		if (hp_lvol->lvol->blob == lvol->blob) {
			break;
		}
	}
	if (hp_lvol->lvol == NULL) {
		return -EINVAL;
	}

	if (!hp_lvol->created) {
		hp_lvol->created = true;
		rc = spdk_lvs_esnap_missing_add(lvs, lvol, hp_lvol->esnap_id, hp_lvol->id_len);
		CU_ASSERT(rc == 0);
		*bs_dev = NULL;
		return 0;
	}

	hp_lvol->hp_count++;

	if (hp_lvol->hotplug_retval != 0) {
		return hp_lvol->hotplug_retval;
	}

	missing_esnap = get_missing_esnap(g_missing_esnap, esnap_id);
	if (missing_esnap == NULL) {
		return -ENODEV;
	}

	if (hp_lvol->register_missing) {
		rc = spdk_lvs_esnap_missing_add(hp_lvol->lvol->lvol_store, hp_lvol->lvol,
						hp_lvol->esnap_id, hp_lvol->id_len);
		CU_ASSERT(rc == 0);
		*bs_dev = NULL;
		return 0;
	}

	*bs_dev = missing_esnap->esnap_dev;
	return 0;
}

/*
 * Creates an lvolstore with the specified esnap clone lvols. They are all initially missing their
 * external snapshots, similar to what would happen if an lvolstore's device is examined before the
 * devices that act as external snapshots. After the lvols are loaded, the blobstore is notified of
 * each missing esnap (degraded_set).
 *
 * @param hotplug_lvols An array of esnap clone lvols to create. The array is terminated by zeroed
 * element.
 * @parm degraded_lvol_sets_tree An array of external snapshots that will be hotplugged. The array is
 * terminated by a zeroed element.
 * @desc Unused, but is very helpful when displaying stack traces in a debugger.
 */
static bool
lvol_esnap_hotplug_scenario(struct hotplug_lvol *hotplug_lvols,
			    struct missing_esnap *degraded_lvol_sets_tree,
			    char *desc)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvs_opts opts;
	struct spdk_lvol_store *lvs;
	struct spdk_lvs_degraded_lvol_set *degraded_set;
	struct hotplug_lvol *hp_lvol;
	struct missing_esnap *m_esnap;
	int count;
	int rc;
	uint32_t num_failures = CU_get_number_of_failures();

	g_hotplug_lvols = hotplug_lvols;
	g_missing_esnap = degraded_lvol_sets_tree;

	/* Create the lvstore */
	init_dev(&dev);
	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");
	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	lvs = g_lvol_store;
	lvs->esnap_bs_dev_create = ut_esnap_hotplug_dev_create;

	/* Create the lvols */
	for (hp_lvol = hotplug_lvols; hp_lvol->lvol_name != NULL; hp_lvol++) {
		if (hp_lvol->id_len == 0) {
			hp_lvol->id_len = strlen(hp_lvol->esnap_id) + 1;
		}

		g_lvserrno = 0xbad;
		rc = spdk_lvol_create_esnap_clone(hp_lvol->esnap_id, hp_lvol->id_len,
						  opts.cluster_sz, lvs, hp_lvol->lvol_name,
						  lvol_op_with_handle_complete, NULL);
		CU_ASSERT(rc == 0);
		poll_threads();
		CU_ASSERT(g_lvserrno == 0);
		CU_ASSERT(g_lvol != NULL);
		if (g_lvol == NULL) {
			break;
		}
		hp_lvol->lvol = g_lvol;
		/* This is normally triggered by the blobstore in blob_load_esnap(), but that part
		 * of blobstore is not mocked by lvol_ut. Later commits will further exercise
		 * hotplug with a functional blobstore. See test/lvol/esnap/esnap.c and
		 * test/lvol/external_snapshot.sh in later commits.
		 */
		rc = ut_esnap_hotplug_dev_create(lvs, hp_lvol->lvol, hp_lvol->lvol->blob,
						 hp_lvol->esnap_id, hp_lvol->id_len,
						 &hp_lvol->lvol->blob->back_bs_dev);
		CU_ASSERT(rc == 0);
	}

	/* Verify lvol count in lvs->degraded_lvol_sets_tree tree. */
	for (m_esnap = degraded_lvol_sets_tree; m_esnap->esnap_id != NULL; m_esnap++) {
		count = missing_get_lvol_count(lvs, m_esnap->esnap_id);
		CU_ASSERT(m_esnap->expect_missing_lvol_count_after_create == count);
	}

	/* Verify lvs->degraded_lvol_sets_tree tree has nothing extra */
	RB_FOREACH(degraded_set, degraded_lvol_sets_tree, &lvs->degraded_lvol_sets_tree) {
		m_esnap = get_missing_esnap(degraded_lvol_sets_tree, degraded_set->esnap_id);
		CU_ASSERT(m_esnap != NULL);
		if (m_esnap != NULL) {
			count = missing_get_lvol_count(lvs, m_esnap->esnap_id);
			CU_ASSERT(m_esnap->expect_missing_lvol_count_after_create == count);
		}
	}

	/* Perform hotplug */
	for (m_esnap = degraded_lvol_sets_tree; m_esnap->esnap_id != NULL; m_esnap++) {
		spdk_lvs_notify_hotplug(m_esnap->esnap_id, strlen(m_esnap->esnap_id) + 1,
					lvol_op_with_handle_complete, NULL);
	}

	/* Verify lvol->degraded_set and back_bs_dev */
	for (hp_lvol = hotplug_lvols; hp_lvol->lvol != NULL; hp_lvol++) {
		if (hp_lvol->register_missing || hp_lvol->hotplug_retval != 0) {
			CU_ASSERT(hp_lvol->lvol->degraded_set != NULL);
			CU_ASSERT(hp_lvol->lvol->blob->back_bs_dev == NULL);
		} else {
			CU_ASSERT(hp_lvol->lvol->degraded_set == NULL);
			m_esnap = get_missing_esnap(degraded_lvol_sets_tree, hp_lvol->esnap_id);
			CU_ASSERT(m_esnap != NULL);
			if (m_esnap != NULL) {
				CU_ASSERT(hp_lvol->lvol->blob->back_bs_dev == m_esnap->esnap_dev);
			}
		}
	}

	/* Verify hotplug count on lvols */
	for (hp_lvol = hotplug_lvols; hp_lvol->lvol != NULL; hp_lvol++) {
		CU_ASSERT(hp_lvol->hp_count == 1);
	}

	/* Verify lvol count in lvs->degraded_lvol_sets_tree tree. */
	for (m_esnap = degraded_lvol_sets_tree; m_esnap->esnap_id != NULL; m_esnap++) {
		count = missing_get_lvol_count(lvs, m_esnap->esnap_id);
		CU_ASSERT(m_esnap->expect_missing_lvol_count_after_hotplug == count);
	}

	/* Verify lvs->degraded_lvol_sets_tree tree has nothing extra */
	RB_FOREACH(degraded_set, degraded_lvol_sets_tree, &lvs->degraded_lvol_sets_tree) {
		m_esnap = get_missing_esnap(degraded_lvol_sets_tree, degraded_set->esnap_id);
		CU_ASSERT(m_esnap != NULL);
		if (m_esnap != NULL) {
			count = missing_get_lvol_count(lvs, m_esnap->esnap_id);
			CU_ASSERT(m_esnap->expect_missing_lvol_count_after_hotplug == count);
		}
	}

	/* Clean up */
	for (hp_lvol = hotplug_lvols; hp_lvol->lvol != NULL; hp_lvol++) {
		g_lvserrno = 0xbad;
		spdk_lvol_close(hp_lvol->lvol, op_complete, NULL);
		CU_ASSERT(g_lvserrno == 0);
		g_lvserrno = 0xbad;
		spdk_lvol_destroy(hp_lvol->lvol, op_complete, NULL);
		CU_ASSERT(g_lvserrno == 0);
	}
	g_lvserrno = 0xabad;
	rc = spdk_lvs_destroy(g_lvol_store, op_complete, NULL);
	poll_threads();
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol = NULL;
	g_lvol_store = NULL;

	return num_failures == CU_get_number_of_failures();
}

static void
lvol_esnap_hotplug(void)
{
	struct spdk_bs_dev bs_dev = { 0 };
	struct spdk_bs_dev bs_dev2 = { 0 };
	uint64_t i;
	bool ok;
#define HOTPLUG_LVOL(_lvol_name, _esnap_id, _hotplug_retval, _register_missing) { \
	.lvol_name = _lvol_name, \
	.esnap_id = _esnap_id, \
	.hotplug_retval = _hotplug_retval, \
	.register_missing = _register_missing, \
}
#define MISSING_ESNAP(_esnap_id, _esnap_dev, _after_create, _after_hotplug) { \
	.esnap_id = _esnap_id, \
	.esnap_dev = _esnap_dev, \
	.expect_missing_lvol_count_after_create = _after_create, \
	.expect_missing_lvol_count_after_hotplug = _after_hotplug, \
}
	struct {
		char *desc;
		struct hotplug_lvol h[4];
		struct missing_esnap m[3];
	} scenario[] = {
		{
			"one missing, happy path",
			{ HOTPLUG_LVOL("lvol1", "esnap1", 0, false) },
			{ MISSING_ESNAP("esnap1", &bs_dev, 1, 0) }
		},
		{
			"one missing, cb registers degraded_set",
			{ HOTPLUG_LVOL("lvol1", "esnap1", 0, true) },
			{ MISSING_ESNAP("esnap1", &bs_dev, 1, 1) }
		},
		{
			"one missing, cb retuns -ENOMEM",
			{ HOTPLUG_LVOL("lvol1", "esnap1", -ENOMEM, true) },
			{ MISSING_ESNAP("esnap1", &bs_dev, 1, 1) }
		},
		{
			"two missing with same esnap, happy path",
			{
				HOTPLUG_LVOL("lvol1", "esnap1", 0, false),
				HOTPLUG_LVOL("lvol2", "esnap1", 0, false)
			},
			{ MISSING_ESNAP("esnap1", &bs_dev, 2, 0) }
		},
		{
			"two missing with same esnap, first -ENOMEM",
			{
				HOTPLUG_LVOL("lvol1", "esnap1", -ENOMEM, false),
				HOTPLUG_LVOL("lvol2", "esnap1", 0, false)
			},
			{ MISSING_ESNAP("esnap1", &bs_dev, 2, 1) }
		},
		{
			"two missing with same esnap, second -ENOMEM",
			{
				HOTPLUG_LVOL("lvol1", "esnap1", 0, false),
				HOTPLUG_LVOL("lvol2", "esnap1", -ENOMEM, false)
			},
			{ MISSING_ESNAP("esnap1", &bs_dev, 2, 1) }
		},
		{
			"two missing with different esnaps, happy path",
			{
				HOTPLUG_LVOL("lvol1", "esnap1", 0, false),
				HOTPLUG_LVOL("lvol2", "esnap2", 0, false)
			},
			{
				MISSING_ESNAP("esnap1", &bs_dev, 1, 0),
				MISSING_ESNAP("esnap2", &bs_dev2, 1, 0)
			}
		},
		{
			"two missing with different esnaps, first still missing",
			{
				HOTPLUG_LVOL("lvol1", "esnap1", 0, true),
				HOTPLUG_LVOL("lvol2", "esnap2", 0, false)
			},
			{
				MISSING_ESNAP("esnap1", &bs_dev, 1, 1),
				MISSING_ESNAP("esnap2", &bs_dev2, 1, 0)
			}
		},
		{
			"three missing with same esnap, happy path",
			{
				HOTPLUG_LVOL("lvol1", "esnap1", 0, false),
				HOTPLUG_LVOL("lvol2", "esnap1", 0, false),
				HOTPLUG_LVOL("lvol3", "esnap1", 0, false)
			},
			{ MISSING_ESNAP("esnap1", &bs_dev, 3, 0) }
		},
		{
			"three missing with same esnap, first still missing",
			{
				HOTPLUG_LVOL("lvol1", "esnap1", 0, true),
				HOTPLUG_LVOL("lvol2", "esnap1", 0, false),
				HOTPLUG_LVOL("lvol3", "esnap1", 0, false)
			},
			{ MISSING_ESNAP("esnap1", &bs_dev, 3, 1) }
		},
		{
			"three missing with same esnap, first two still missing",
			{
				HOTPLUG_LVOL("lvol1", "esnap1", 0, true),
				HOTPLUG_LVOL("lvol2", "esnap1", 0, true),
				HOTPLUG_LVOL("lvol3", "esnap1", 0, false)
			},
			{ MISSING_ESNAP("esnap1", &bs_dev, 3, 2) }
		},
		{
			"three missing with same esnap, middle still missing",
			{
				HOTPLUG_LVOL("lvol1", "esnap1", 0, false),
				HOTPLUG_LVOL("lvol2", "esnap1", 0, true),
				HOTPLUG_LVOL("lvol3", "esnap1", 0, false)
			},
			{ MISSING_ESNAP("esnap1", &bs_dev, 3, 1) }
		},
		{
			"three missing with same esnap, last still missing",
			{
				HOTPLUG_LVOL("lvol1", "esnap1", 0, false),
				HOTPLUG_LVOL("lvol2", "esnap1", 0, false),
				HOTPLUG_LVOL("lvol3", "esnap1", 0, true)
			},
			{ MISSING_ESNAP("esnap1", &bs_dev, 3, 1) }
		},
	};
#undef HOTPLUG_LVOL
#undef MISSING_ESNAP

	printf("\n");
	for (i = 0; i < SPDK_COUNTOF(scenario); i++) {
		ok = lvol_esnap_hotplug_scenario(scenario[i].h, scenario[i].m, scenario[i].desc);
		/* Add markers in the output to help correlate failures to scenarios. */
		CU_ASSERT(ok);
		printf("\t%s scenario %" PRIu64 ": %s - %s\n", __func__, i,
		       ok ? "PASS" : "FAIL", scenario[i].desc);
	}
}

static void
lvol_get_by(void)
{
	struct lvol_ut_bs_dev dev1, dev2;
	struct spdk_lvol_store *lvs1, *lvs2;
	struct spdk_lvol *lvol1, *lvol2, *lvol3;
	struct spdk_lvs_opts opts;
	int rc = 0;
	struct spdk_uuid uuid;

	init_dev(&dev1);

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev1.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	lvs1 = g_lvol_store;

	/* Create lvol name "lvol" */
	spdk_lvol_create(lvs1, "lvol", 10, true, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol1 = g_lvol;

	/* Should be able to look up lvol1 by its name and UUID */
	CU_ASSERT(spdk_lvol_get_by_names("lvs", "lvol") == lvol1);
	/* Be sure a pointer comparison isn't used. */
	memcpy(&uuid, &lvol1->uuid, sizeof(uuid));
	CU_ASSERT(spdk_lvol_get_by_uuid(&uuid) == lvol1);

	/* Shorter and longer values for lvol_name must not match. */
	CU_ASSERT(spdk_lvol_get_by_names("lvs", "lvoll") == NULL);
	CU_ASSERT(spdk_lvol_get_by_names("lvs", "lvo") == NULL);

	/* Shorter and longer values for lvs_name must not match. */
	CU_ASSERT(spdk_lvol_get_by_names("lvss", "lvol") == NULL);
	CU_ASSERT(spdk_lvol_get_by_names("lv", "lvol") == NULL);

	/* Create lvol name "lvol2" */
	spdk_lvol_create(lvs1, "lvol2", 10, true, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol2 = g_lvol;

	/* When there are multiple lvols, the right one is found */
	CU_ASSERT(spdk_lvol_get_by_names("lvs", "lvol") == lvol1);
	CU_ASSERT(spdk_lvol_get_by_names("lvs", "lvol2") == lvol2);

	/* Create a second lvolstore */
	init_dev(&dev2);
	snprintf(opts.name, sizeof(opts.name), "lvs2");
	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev2.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);
	lvs2 = g_lvol_store;

	/* Lookups that worked with one lvstore still work */
	memcpy(&uuid, &lvol1->uuid, sizeof(uuid));
	CU_ASSERT(spdk_lvol_get_by_uuid(&uuid) == lvol1);
	CU_ASSERT(spdk_lvol_get_by_names("lvs", "lvol") == lvol1);
	CU_ASSERT(spdk_lvol_get_by_names("lvs", "lvol2") == lvol2);

	/* Add an lvol name "lvol" in the second lvstore */
	spdk_lvol_create(lvs2, "lvol", 10, true, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	lvol3 = g_lvol;

	/* Lookups by name find the lvols in the right lvstores */
	CU_ASSERT(spdk_lvol_get_by_names("lvs", "lvol") == lvol1);
	CU_ASSERT(spdk_lvol_get_by_names("lvs", "lvol2") == lvol2);
	CU_ASSERT(spdk_lvol_get_by_names("lvs2", "lvol") == lvol3);

	/* Clean up */
	g_lvserrno = -1;
	spdk_lvol_close(lvol1, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	spdk_lvol_close(lvol2, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	spdk_lvol_close(lvol3, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(lvs1, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(lvs2, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);

	g_lvol_store = NULL;
	g_lvol = NULL;

	free_dev(&dev1);
	free_dev(&dev2);
}

static void
lvol_shallow_copy(void)
{
	struct lvol_ut_bs_dev bs_dev;
	struct spdk_lvs_opts opts;
	struct spdk_bs_dev ext_dev;
	int rc = 0;

	init_dev(&bs_dev);

	ext_dev.blocklen = DEV_BUFFER_BLOCKLEN;
	ext_dev.blockcnt = BS_CLUSTER_SIZE / DEV_BUFFER_BLOCKLEN;

	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_lvserrno = -1;
	rc = spdk_lvs_init(&bs_dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	spdk_lvol_create(g_lvol_store, "lvol", BS_CLUSTER_SIZE, false, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	/* Successful shallow copy */
	g_blob_read_only = true;
	rc = spdk_lvol_shallow_copy(g_lvol, &ext_dev, NULL, NULL, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);

	/* Shallow copy with null lvol */
	rc = spdk_lvol_shallow_copy(NULL, &ext_dev, NULL, NULL, op_complete, NULL);
	CU_ASSERT(rc == -EINVAL);

	/* Shallow copy with null ext_dev */
	rc = spdk_lvol_shallow_copy(g_lvol, NULL, NULL, NULL, op_complete, NULL);
	CU_ASSERT(rc == -EINVAL);

	spdk_lvol_close(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	spdk_lvol_destroy(g_lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_unload(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;

	free_dev(&bs_dev);

	/* Make sure that all references to the io_channel was closed after
	 * shallow copy call
	 */
	CU_ASSERT(g_io_channel == NULL);
}

static void
lvol_set_parent(void)
{
	struct lvol_ut_bs_dev bs1_dev;
	struct spdk_lvol_store *lvol_store1;
	struct spdk_lvol *lvol1, *lvol2, *snapshot1;
	struct spdk_lvs_opts opts;
	uint64_t cluster_sz = BS_CLUSTER_SIZE;
	int rc = 0;

	init_dev(&bs1_dev);

	/* Create lvol store 1 */
	spdk_lvs_opts_init(&opts);
	snprintf(opts.name, sizeof(opts.name), "lvs1");

	g_lvserrno = -1;
	rc = spdk_lvs_init(&bs1_dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	lvol_store1 = g_lvol_store;

	/* Create lvol1 */
	spdk_lvol_create(lvol_store1, "lvol1", cluster_sz, true, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	lvol1 = g_lvol;

	/* Create lvol2 with same size of lvol1 */
	spdk_lvol_create(lvol_store1, "lvol2", cluster_sz, true, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	lvol2 = g_lvol;

	/* Create a snapshot of lvol2 */
	spdk_lvol_create_snapshot(lvol2, "snap1", lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);
	CU_ASSERT_STRING_EQUAL(g_lvol->name, "snap1");

	snapshot1 = g_lvol;

	/* Set parent with a NULL lvol */
	g_lvserrno = 0;
	spdk_lvol_set_parent(NULL, snapshot1, op_complete, NULL);
	CU_ASSERT(g_lvserrno == -EINVAL);

	/* Set parent with a NULL parent snapshot */
	g_lvserrno = 0;
	spdk_lvol_set_parent(lvol1, NULL, op_complete, NULL);
	CU_ASSERT(g_lvserrno == -EINVAL);

	/* Set parent successful */
	g_blob_is_snapshot = true;
	g_lvserrno = -1;
	spdk_lvol_set_parent(lvol1, snapshot1, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	/* Clean up */
	spdk_lvol_close(lvol1, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	spdk_lvol_destroy(lvol1, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	spdk_lvol_close(lvol2, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	spdk_lvol_destroy(lvol2, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	spdk_lvol_close(snapshot1, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	spdk_lvol_destroy(snapshot1, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_destroy(lvol_store1, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	lvol_store1 = NULL;
}

static void
lvol_set_external_parent(void)
{
	struct lvol_ut_bs_dev dev;
	struct spdk_lvol *lvol;
	struct spdk_lvs_opts opts;
	uint64_t cluster_sz;
	int rc;

	g_spdk_blob_get_esnap_id = (void *)uuid;
	g_spdk_blob_get_esnap_id_len = SPDK_UUID_STRING_LEN;
	init_dev(&dev);

	/* Create lvol store */
	spdk_lvs_opts_init(&opts);
	cluster_sz = opts.cluster_sz;
	snprintf(opts.name, sizeof(opts.name), "lvs");

	g_lvserrno = -1;
	rc = spdk_lvs_init(&dev.bs_dev, &opts, lvol_store_op_with_handle_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol_store != NULL);

	/* Create lvol */
	spdk_lvol_create(g_lvol_store, "lvol", cluster_sz, false, LVOL_CLEAR_WITH_DEFAULT,
			 lvol_op_with_handle_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_lvol != NULL);

	lvol = g_lvol;

	/* Set external parent with NULL lvol */
	spdk_lvol_set_external_parent(NULL, uuid, SPDK_UUID_STRING_LEN, op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_lvserrno == -EINVAL);

	/* Set external parent with NULL esnap id */
	spdk_lvol_set_external_parent(lvol, NULL, SPDK_UUID_STRING_LEN, op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_lvserrno == -EINVAL);

	/* Set external parent with equal lvol and esnap */
	spdk_lvol_set_external_parent(lvol, lvol->uuid_str, SPDK_UUID_STRING_LEN, op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_lvserrno == -EINVAL);

	/* Set external parent successful */
	spdk_lvol_set_external_parent(lvol, uuid, SPDK_UUID_STRING_LEN, op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_lvserrno == 0);

	/* Clean up */
	spdk_lvol_close(lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);
	spdk_lvol_destroy(lvol, op_complete, NULL);
	CU_ASSERT(g_lvserrno == 0);

	g_lvserrno = -1;
	rc = spdk_lvs_destroy(g_lvol_store, op_complete, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_lvserrno == 0);
	g_lvol_store = NULL;
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("lvol", NULL, NULL);

	CU_ADD_TEST(suite, lvs_init_unload_success);
	CU_ADD_TEST(suite, lvs_init_destroy_success);
	CU_ADD_TEST(suite, lvs_init_opts_success);
	CU_ADD_TEST(suite, lvs_unload_lvs_is_null_fail);
	CU_ADD_TEST(suite, lvs_names);
	CU_ADD_TEST(suite, lvol_create_destroy_success);
	CU_ADD_TEST(suite, lvol_create_fail);
	CU_ADD_TEST(suite, lvol_destroy_fail);
	CU_ADD_TEST(suite, lvol_close);
	CU_ADD_TEST(suite, lvol_resize);
	CU_ADD_TEST(suite, lvol_set_read_only);
	CU_ADD_TEST(suite, test_lvs_load);
	CU_ADD_TEST(suite, lvols_load);
	CU_ADD_TEST(suite, lvol_open);
	CU_ADD_TEST(suite, lvol_snapshot);
	CU_ADD_TEST(suite, lvol_snapshot_fail);
	CU_ADD_TEST(suite, lvol_clone);
	CU_ADD_TEST(suite, lvol_clone_fail);
	CU_ADD_TEST(suite, lvol_iter_clones);
	CU_ADD_TEST(suite, lvol_refcnt);
	CU_ADD_TEST(suite, lvol_names);
	CU_ADD_TEST(suite, lvol_create_thin_provisioned);
	CU_ADD_TEST(suite, lvol_rename);
	CU_ADD_TEST(suite, lvs_rename);
	CU_ADD_TEST(suite, lvol_inflate);
	CU_ADD_TEST(suite, lvol_decouple_parent);
	CU_ADD_TEST(suite, lvol_get_xattr);
	CU_ADD_TEST(suite, lvol_esnap_reload);
	CU_ADD_TEST(suite, lvol_esnap_create_bad_args);
	CU_ADD_TEST(suite, lvol_esnap_create_delete);
	CU_ADD_TEST(suite, lvol_esnap_load_esnaps);
	CU_ADD_TEST(suite, lvol_esnap_missing);
	CU_ADD_TEST(suite, lvol_esnap_hotplug);
	CU_ADD_TEST(suite, lvol_get_by);
	CU_ADD_TEST(suite, lvol_shallow_copy);
	CU_ADD_TEST(suite, lvol_set_parent);
	CU_ADD_TEST(suite, lvol_set_external_parent);

	allocate_threads(1);
	set_thread(0);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	free_threads();

	return num_failures;
}
