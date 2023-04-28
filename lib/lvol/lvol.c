/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk_internal/lvolstore.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/blob_bdev.h"
#include "spdk/tree.h"
#include "spdk/util.h"

/* Default blob channel opts for lvol */
#define SPDK_LVOL_BLOB_OPTS_CHANNEL_OPS 512

#define LVOL_NAME "name"

SPDK_LOG_REGISTER_COMPONENT(lvol)

struct spdk_lvs_degraded_lvol_set {
	struct spdk_lvol_store			*lvol_store;
	const void				*esnap_id;
	uint32_t				id_len;
	TAILQ_HEAD(degraded_lvols, spdk_lvol)	lvols;
	RB_ENTRY(spdk_lvs_degraded_lvol_set)	node;
};

static TAILQ_HEAD(, spdk_lvol_store) g_lvol_stores = TAILQ_HEAD_INITIALIZER(g_lvol_stores);
static pthread_mutex_t g_lvol_stores_mutex = PTHREAD_MUTEX_INITIALIZER;

static inline int lvs_opts_copy(const struct spdk_lvs_opts *src, struct spdk_lvs_opts *dst);
static int lvs_esnap_bs_dev_create(void *bs_ctx, void *blob_ctx, struct spdk_blob *blob,
				   const void *esnap_id, uint32_t id_len,
				   struct spdk_bs_dev **_bs_dev);
static struct spdk_lvol *lvs_get_lvol_by_blob_id(struct spdk_lvol_store *lvs, spdk_blob_id blob_id);
static void lvs_degraded_lvol_set_add(struct spdk_lvs_degraded_lvol_set *degraded_set,
				      struct spdk_lvol *lvol);
static void lvs_degraded_lvol_set_remove(struct spdk_lvs_degraded_lvol_set *degraded_set,
		struct spdk_lvol *lvol);

static int
add_lvs_to_list(struct spdk_lvol_store *lvs)
{
	struct spdk_lvol_store *tmp;
	bool name_conflict = false;

	pthread_mutex_lock(&g_lvol_stores_mutex);
	TAILQ_FOREACH(tmp, &g_lvol_stores, link) {
		if (!strncmp(lvs->name, tmp->name, SPDK_LVS_NAME_MAX)) {
			name_conflict = true;
			break;
		}
	}
	if (!name_conflict) {
		lvs->on_list = true;
		TAILQ_INSERT_TAIL(&g_lvol_stores, lvs, link);
	}
	pthread_mutex_unlock(&g_lvol_stores_mutex);

	return name_conflict ? -1 : 0;
}

static struct spdk_lvol_store *
lvs_alloc(void)
{
	struct spdk_lvol_store *lvs;

	lvs = calloc(1, sizeof(*lvs));
	if (lvs == NULL) {
		return NULL;
	}

	TAILQ_INIT(&lvs->lvols);
	TAILQ_INIT(&lvs->pending_lvols);
	TAILQ_INIT(&lvs->retry_open_lvols);

	lvs->load_esnaps = false;
	RB_INIT(&lvs->degraded_lvol_sets_tree);
	lvs->thread = spdk_get_thread();

	return lvs;
}

static void
lvs_free(struct spdk_lvol_store *lvs)
{
	pthread_mutex_lock(&g_lvol_stores_mutex);
	if (lvs->on_list) {
		TAILQ_REMOVE(&g_lvol_stores, lvs, link);
	}
	pthread_mutex_unlock(&g_lvol_stores_mutex);

	assert(RB_EMPTY(&lvs->degraded_lvol_sets_tree));

	free(lvs);
}

static struct spdk_lvol *
lvol_alloc(struct spdk_lvol_store *lvs, const char *name, bool thin_provision,
	   enum lvol_clear_method clear_method)
{
	struct spdk_lvol *lvol;

	lvol = calloc(1, sizeof(*lvol));
	if (lvol == NULL) {
		return NULL;
	}

	lvol->lvol_store = lvs;
	lvol->clear_method = (enum blob_clear_method)clear_method;
	snprintf(lvol->name, sizeof(lvol->name), "%s", name);
	spdk_uuid_generate(&lvol->uuid);
	spdk_uuid_fmt_lower(lvol->uuid_str, sizeof(lvol->uuid_str), &lvol->uuid);
	spdk_uuid_fmt_lower(lvol->unique_id, sizeof(lvol->uuid_str), &lvol->uuid);

	TAILQ_INSERT_TAIL(&lvs->pending_lvols, lvol, link);

	return lvol;
}

static void
lvol_free(struct spdk_lvol *lvol)
{
	free(lvol);
}

static void
lvol_open_cb(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvol_with_handle_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;

	if (lvolerrno != 0) {
		SPDK_INFOLOG(lvol, "Failed to open lvol %s\n", lvol->unique_id);
		goto end;
	}

	lvol->ref_count++;
	lvol->blob = blob;
end:
	req->cb_fn(req->cb_arg, lvol, lvolerrno);
	free(req);
}

void
spdk_lvol_open(struct spdk_lvol *lvol, spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;
	struct spdk_blob_open_opts opts;

	assert(cb_fn != NULL);

	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		cb_fn(cb_arg, NULL, -ENODEV);
		return;
	}

	if (lvol->action_in_progress == true) {
		SPDK_ERRLOG("Cannot open lvol - operations on lvol pending\n");
		cb_fn(cb_arg, lvol, -EBUSY);
		return;
	}

	if (lvol->ref_count > 0) {
		lvol->ref_count++;
		cb_fn(cb_arg, lvol, 0);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for request structure\n");
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->lvol = lvol;

	spdk_blob_open_opts_init(&opts, sizeof(opts));
	opts.clear_method = lvol->clear_method;

	spdk_bs_open_blob_ext(lvol->lvol_store->blobstore, lvol->blob_id, &opts, lvol_open_cb, req);
}

static void
bs_unload_with_error_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;

	req->cb_fn(req->cb_arg, NULL, req->lvserrno);
	free(req);
}

static void
load_next_lvol(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob_store *bs = lvs->blobstore;
	struct spdk_lvol *lvol, *tmp;
	spdk_blob_id blob_id;
	const char *attr;
	size_t value_len;
	int rc;

	if (lvolerrno == -ENOENT) {
		/* Finished iterating */
		if (req->lvserrno == 0) {
			lvs->load_esnaps = true;
			req->cb_fn(req->cb_arg, lvs, req->lvserrno);
			free(req);
		} else {
			TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
				TAILQ_REMOVE(&lvs->lvols, lvol, link);
				free(lvol);
			}
			lvs_free(lvs);
			spdk_bs_unload(bs, bs_unload_with_error_cb, req);
		}
		return;
	} else if (lvolerrno < 0) {
		SPDK_ERRLOG("Failed to fetch blobs list\n");
		req->lvserrno = lvolerrno;
		goto invalid;
	}

	blob_id = spdk_blob_get_id(blob);

	if (blob_id == lvs->super_blob_id) {
		SPDK_INFOLOG(lvol, "found superblob %"PRIu64"\n", (uint64_t)blob_id);
		spdk_bs_iter_next(bs, blob, load_next_lvol, req);
		return;
	}

	lvol = calloc(1, sizeof(*lvol));
	if (!lvol) {
		SPDK_ERRLOG("Cannot alloc memory for lvol base pointer\n");
		req->lvserrno = -ENOMEM;
		goto invalid;
	}

	/*
	 * Do not store a reference to blob now because spdk_bs_iter_next() will close it.
	 * Storing blob_id for future lookups is fine.
	 */
	lvol->blob_id = blob_id;
	lvol->lvol_store = lvs;

	rc = spdk_blob_get_xattr_value(blob, "uuid", (const void **)&attr, &value_len);
	if (rc != 0 || value_len != SPDK_UUID_STRING_LEN || attr[SPDK_UUID_STRING_LEN - 1] != '\0' ||
	    spdk_uuid_parse(&lvol->uuid, attr) != 0) {
		SPDK_INFOLOG(lvol, "Missing or corrupt lvol uuid\n");
		memset(&lvol->uuid, 0, sizeof(lvol->uuid));
	}
	spdk_uuid_fmt_lower(lvol->uuid_str, sizeof(lvol->uuid_str), &lvol->uuid);

	if (!spdk_mem_all_zero(&lvol->uuid, sizeof(lvol->uuid))) {
		snprintf(lvol->unique_id, sizeof(lvol->unique_id), "%s", lvol->uuid_str);
	} else {
		spdk_uuid_fmt_lower(lvol->unique_id, sizeof(lvol->unique_id), &lvol->lvol_store->uuid);
		value_len = strlen(lvol->unique_id);
		snprintf(lvol->unique_id + value_len, sizeof(lvol->unique_id) - value_len, "_%"PRIu64,
			 (uint64_t)blob_id);
	}

	rc = spdk_blob_get_xattr_value(blob, "name", (const void **)&attr, &value_len);
	if (rc != 0 || value_len > SPDK_LVOL_NAME_MAX) {
		SPDK_ERRLOG("Cannot assign lvol name\n");
		lvol_free(lvol);
		req->lvserrno = -EINVAL;
		goto invalid;
	}

	snprintf(lvol->name, sizeof(lvol->name), "%s", attr);

	TAILQ_INSERT_TAIL(&lvs->lvols, lvol, link);

	lvs->lvol_count++;

	SPDK_INFOLOG(lvol, "added lvol %s (%s)\n", lvol->unique_id, lvol->uuid_str);

invalid:
	spdk_bs_iter_next(bs, blob, load_next_lvol, req);
}

static void
close_super_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob_store *bs = lvs->blobstore;

	if (lvolerrno != 0) {
		SPDK_INFOLOG(lvol, "Could not close super blob\n");
		lvs_free(lvs);
		req->lvserrno = -ENODEV;
		spdk_bs_unload(bs, bs_unload_with_error_cb, req);
		return;
	}

	/* Start loading lvols */
	spdk_bs_iter_first(lvs->blobstore, load_next_lvol, req);
}

static void
close_super_blob_with_error_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob_store *bs = lvs->blobstore;

	lvs_free(lvs);

	spdk_bs_unload(bs, bs_unload_with_error_cb, req);
}

static void
lvs_read_uuid(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob_store *bs = lvs->blobstore;
	const char *attr;
	size_t value_len;
	int rc;

	if (lvolerrno != 0) {
		SPDK_INFOLOG(lvol, "Could not open super blob\n");
		lvs_free(lvs);
		req->lvserrno = -ENODEV;
		spdk_bs_unload(bs, bs_unload_with_error_cb, req);
		return;
	}

	rc = spdk_blob_get_xattr_value(blob, "uuid", (const void **)&attr, &value_len);
	if (rc != 0 || value_len != SPDK_UUID_STRING_LEN || attr[SPDK_UUID_STRING_LEN - 1] != '\0') {
		SPDK_INFOLOG(lvol, "degraded_set or incorrect UUID\n");
		req->lvserrno = -EINVAL;
		spdk_blob_close(blob, close_super_blob_with_error_cb, req);
		return;
	}

	if (spdk_uuid_parse(&lvs->uuid, attr)) {
		SPDK_INFOLOG(lvol, "incorrect UUID '%s'\n", attr);
		req->lvserrno = -EINVAL;
		spdk_blob_close(blob, close_super_blob_with_error_cb, req);
		return;
	}

	rc = spdk_blob_get_xattr_value(blob, "name", (const void **)&attr, &value_len);
	if (rc != 0 || value_len > SPDK_LVS_NAME_MAX) {
		SPDK_INFOLOG(lvol, "degraded_set or invalid name\n");
		req->lvserrno = -EINVAL;
		spdk_blob_close(blob, close_super_blob_with_error_cb, req);
		return;
	}

	snprintf(lvs->name, sizeof(lvs->name), "%s", attr);

	rc = add_lvs_to_list(lvs);
	if (rc) {
		SPDK_INFOLOG(lvol, "lvolstore with name %s already exists\n", lvs->name);
		req->lvserrno = -EEXIST;
		spdk_blob_close(blob, close_super_blob_with_error_cb, req);
		return;
	}

	lvs->super_blob_id = spdk_blob_get_id(blob);

	spdk_blob_close(blob, close_super_cb, req);
}

static void
lvs_open_super(void *cb_arg, spdk_blob_id blobid, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob_store *bs = lvs->blobstore;

	if (lvolerrno != 0) {
		SPDK_INFOLOG(lvol, "Super blob not found\n");
		lvs_free(lvs);
		req->lvserrno = -ENODEV;
		spdk_bs_unload(bs, bs_unload_with_error_cb, req);
		return;
	}

	spdk_bs_open_blob(bs, blobid, lvs_read_uuid, req);
}

static void
lvs_load_cb(void *cb_arg, struct spdk_blob_store *bs, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;

	if (lvolerrno != 0) {
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		lvs_free(lvs);
		free(req);
		return;
	}

	lvs->blobstore = bs;
	lvs->bs_dev = req->bs_dev;

	spdk_bs_get_super(bs, lvs_open_super, req);
}

static void
lvs_bs_opts_init(struct spdk_bs_opts *opts)
{
	spdk_bs_opts_init(opts, sizeof(*opts));
	opts->max_channel_ops = SPDK_LVOL_BLOB_OPTS_CHANNEL_OPS;
}

static void
lvs_load(struct spdk_bs_dev *bs_dev, const struct spdk_lvs_opts *_lvs_opts,
	 spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvs_with_handle_req *req;
	struct spdk_bs_opts bs_opts = {};
	struct spdk_lvs_opts lvs_opts;

	assert(cb_fn != NULL);

	if (bs_dev == NULL) {
		SPDK_ERRLOG("Blobstore device does not exist\n");
		cb_fn(cb_arg, NULL, -ENODEV);
		return;
	}

	spdk_lvs_opts_init(&lvs_opts);
	if (_lvs_opts != NULL) {
		if (lvs_opts_copy(_lvs_opts, &lvs_opts) != 0) {
			SPDK_ERRLOG("Invalid options\n");
			cb_fn(cb_arg, NULL, -EINVAL);
			return;
		}
	}

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for request structure\n");
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	req->lvol_store = lvs_alloc();
	if (req->lvol_store == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store\n");
		free(req);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->bs_dev = bs_dev;

	lvs_bs_opts_init(&bs_opts);
	snprintf(bs_opts.bstype.bstype, sizeof(bs_opts.bstype.bstype), "LVOLSTORE");

	if (lvs_opts.esnap_bs_dev_create != NULL) {
		req->lvol_store->esnap_bs_dev_create = lvs_opts.esnap_bs_dev_create;
		bs_opts.esnap_bs_dev_create = lvs_esnap_bs_dev_create;
		bs_opts.esnap_ctx = req->lvol_store;
	}

	spdk_bs_load(bs_dev, &bs_opts, lvs_load_cb, req);
}

void
spdk_lvs_load(struct spdk_bs_dev *bs_dev, spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	lvs_load(bs_dev, NULL, cb_fn, cb_arg);
}

void
spdk_lvs_load_ext(struct spdk_bs_dev *bs_dev, const struct spdk_lvs_opts *opts,
		  spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	lvs_load(bs_dev, opts, cb_fn, cb_arg);
}

static void
remove_bs_on_error_cb(void *cb_arg, int bserrno)
{
}

static void
exit_error_lvs_req(struct spdk_lvs_with_handle_req *req, struct spdk_lvol_store *lvs, int lvolerrno)
{
	req->cb_fn(req->cb_arg, NULL, lvolerrno);
	spdk_bs_destroy(lvs->blobstore, remove_bs_on_error_cb, NULL);
	lvs_free(lvs);
	free(req);
}

static void
super_create_close_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Lvol store init failed: could not close super blob\n");
		exit_error_lvs_req(req, lvs, lvolerrno);
		return;
	}

	req->cb_fn(req->cb_arg, lvs, lvolerrno);
	free(req);
}

static void
super_blob_set_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob *blob = lvs->super_blob;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Lvol store init failed: could not set uuid for super blob\n");
		exit_error_lvs_req(req, lvs, lvolerrno);
		return;
	}

	spdk_blob_close(blob, super_create_close_cb, req);
}

static void
super_blob_init_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob *blob = lvs->super_blob;
	char uuid[SPDK_UUID_STRING_LEN];

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Lvol store init failed: could not set super blob\n");
		exit_error_lvs_req(req, lvs, lvolerrno);
		return;
	}

	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &lvs->uuid);

	spdk_blob_set_xattr(blob, "uuid", uuid, sizeof(uuid));
	spdk_blob_set_xattr(blob, "name", lvs->name, strnlen(lvs->name, SPDK_LVS_NAME_MAX) + 1);
	spdk_blob_sync_md(blob, super_blob_set_cb, req);
}

static void
super_blob_create_open_cb(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Lvol store init failed: could not open super blob\n");
		exit_error_lvs_req(req, lvs, lvolerrno);
		return;
	}

	lvs->super_blob = blob;
	lvs->super_blob_id = spdk_blob_get_id(blob);

	spdk_bs_set_super(lvs->blobstore, lvs->super_blob_id, super_blob_init_cb, req);
}

static void
super_blob_create_cb(void *cb_arg, spdk_blob_id blobid, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob_store *bs;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Lvol store init failed: could not create super blob\n");
		exit_error_lvs_req(req, lvs, lvolerrno);
		return;
	}

	bs = req->lvol_store->blobstore;

	spdk_bs_open_blob(bs, blobid, super_blob_create_open_cb, req);
}

static void
lvs_init_cb(void *cb_arg, struct spdk_blob_store *bs, int lvserrno)
{
	struct spdk_lvs_with_handle_req *lvs_req = cb_arg;
	struct spdk_lvol_store *lvs = lvs_req->lvol_store;

	if (lvserrno != 0) {
		assert(bs == NULL);
		lvs_req->cb_fn(lvs_req->cb_arg, NULL, lvserrno);
		SPDK_ERRLOG("Lvol store init failed: could not initialize blobstore\n");
		lvs_free(lvs);
		free(lvs_req);
		return;
	}

	assert(bs != NULL);
	lvs->blobstore = bs;

	SPDK_INFOLOG(lvol, "Lvol store initialized\n");

	/* create super blob */
	spdk_bs_create_blob(lvs->blobstore, super_blob_create_cb, lvs_req);
}

void
spdk_lvs_opts_init(struct spdk_lvs_opts *o)
{
	memset(o, 0, sizeof(*o));
	o->cluster_sz = SPDK_LVS_OPTS_CLUSTER_SZ;
	o->clear_method = LVS_CLEAR_WITH_UNMAP;
	o->num_md_pages_per_cluster_ratio = 100;
	o->opts_size = sizeof(*o);
}

static inline int
lvs_opts_copy(const struct spdk_lvs_opts *src, struct spdk_lvs_opts *dst)
{
	if (src->opts_size == 0) {
		SPDK_ERRLOG("opts_size should not be zero value\n");
		return -1;
	}
#define FIELD_OK(field) \
        offsetof(struct spdk_lvs_opts, field) + sizeof(src->field) <= src->opts_size

#define SET_FIELD(field) \
        if (FIELD_OK(field)) { \
                dst->field = src->field; \
        } \

	SET_FIELD(cluster_sz);
	SET_FIELD(clear_method);
	if (FIELD_OK(name)) {
		memcpy(&dst->name, &src->name, sizeof(dst->name));
	}
	SET_FIELD(num_md_pages_per_cluster_ratio);
	SET_FIELD(opts_size);
	SET_FIELD(esnap_bs_dev_create);

	dst->opts_size = src->opts_size;

	/* You should not remove this statement, but need to update the assert statement
	 * if you add a new field, and also add a corresponding SET_FIELD statement */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_lvs_opts) == 88, "Incorrect size");

#undef FIELD_OK
#undef SET_FIELD

	return 0;
}

static void
setup_lvs_opts(struct spdk_bs_opts *bs_opts, struct spdk_lvs_opts *o, uint32_t total_clusters,
	       void *esnap_ctx)
{
	assert(o != NULL);
	lvs_bs_opts_init(bs_opts);
	bs_opts->cluster_sz = o->cluster_sz;
	bs_opts->clear_method = (enum bs_clear_method)o->clear_method;
	bs_opts->num_md_pages = (o->num_md_pages_per_cluster_ratio * total_clusters) / 100;
	bs_opts->esnap_bs_dev_create = o->esnap_bs_dev_create;
	bs_opts->esnap_ctx = esnap_ctx;
	snprintf(bs_opts->bstype.bstype, sizeof(bs_opts->bstype.bstype), "LVOLSTORE");
}

int
spdk_lvs_init(struct spdk_bs_dev *bs_dev, struct spdk_lvs_opts *o,
	      spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvs_with_handle_req *lvs_req;
	struct spdk_bs_opts opts = {};
	struct spdk_lvs_opts lvs_opts;
	uint32_t total_clusters;
	int rc;

	if (bs_dev == NULL) {
		SPDK_ERRLOG("Blobstore device does not exist\n");
		return -ENODEV;
	}

	if (o == NULL) {
		SPDK_ERRLOG("spdk_lvs_opts not specified\n");
		return -EINVAL;
	}

	spdk_lvs_opts_init(&lvs_opts);
	if (lvs_opts_copy(o, &lvs_opts) != 0) {
		SPDK_ERRLOG("spdk_lvs_opts invalid\n");
		return -EINVAL;
	}

	if (lvs_opts.cluster_sz < bs_dev->blocklen) {
		SPDK_ERRLOG("Cluster size %" PRIu32 " is smaller than blocklen %" PRIu32 "\n",
			    lvs_opts.cluster_sz, bs_dev->blocklen);
		return -EINVAL;
	}
	total_clusters = bs_dev->blockcnt / (lvs_opts.cluster_sz / bs_dev->blocklen);

	lvs = lvs_alloc();
	if (!lvs) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store base pointer\n");
		return -ENOMEM;
	}

	setup_lvs_opts(&opts, o, total_clusters, lvs);

	if (strnlen(lvs_opts.name, SPDK_LVS_NAME_MAX) == SPDK_LVS_NAME_MAX) {
		SPDK_ERRLOG("Name has no null terminator.\n");
		lvs_free(lvs);
		return -EINVAL;
	}

	if (strnlen(lvs_opts.name, SPDK_LVS_NAME_MAX) == 0) {
		SPDK_ERRLOG("No name specified.\n");
		lvs_free(lvs);
		return -EINVAL;
	}

	spdk_uuid_generate(&lvs->uuid);
	snprintf(lvs->name, sizeof(lvs->name), "%s", lvs_opts.name);

	rc = add_lvs_to_list(lvs);
	if (rc) {
		SPDK_ERRLOG("lvolstore with name %s already exists\n", lvs->name);
		lvs_free(lvs);
		return -EEXIST;
	}

	lvs_req = calloc(1, sizeof(*lvs_req));
	if (!lvs_req) {
		lvs_free(lvs);
		SPDK_ERRLOG("Cannot alloc memory for lvol store request pointer\n");
		return -ENOMEM;
	}

	assert(cb_fn != NULL);
	lvs_req->cb_fn = cb_fn;
	lvs_req->cb_arg = cb_arg;
	lvs_req->lvol_store = lvs;
	lvs->bs_dev = bs_dev;

	SPDK_INFOLOG(lvol, "Initializing lvol store\n");
	spdk_bs_init(bs_dev, &opts, lvs_init_cb, lvs_req);

	return 0;
}

static void
lvs_rename_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_req *req = cb_arg;

	if (lvolerrno != 0) {
		req->lvserrno = lvolerrno;
	}
	if (req->lvserrno != 0) {
		SPDK_ERRLOG("Lvol store rename operation failed\n");
		/* Lvs renaming failed, so we should 'clear' new_name.
		 * Otherwise it could cause a failure on the next attempt to change the name to 'new_name'  */
		snprintf(req->lvol_store->new_name,
			 sizeof(req->lvol_store->new_name),
			 "%s", req->lvol_store->name);
	} else {
		/* Update lvs name with new_name */
		snprintf(req->lvol_store->name,
			 sizeof(req->lvol_store->name),
			 "%s", req->lvol_store->new_name);
	}

	req->cb_fn(req->cb_arg, req->lvserrno);
	free(req);
}

static void
lvs_rename_sync_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_req *req = cb_arg;
	struct spdk_blob *blob = req->lvol_store->super_blob;

	if (lvolerrno < 0) {
		req->lvserrno = lvolerrno;
	}

	spdk_blob_close(blob, lvs_rename_cb, req);
}

static void
lvs_rename_open_cb(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvs_req *req = cb_arg;
	int rc;

	if (lvolerrno < 0) {
		lvs_rename_cb(cb_arg, lvolerrno);
		return;
	}

	rc = spdk_blob_set_xattr(blob, "name", req->lvol_store->new_name,
				 strlen(req->lvol_store->new_name) + 1);
	if (rc < 0) {
		req->lvserrno = rc;
		lvs_rename_sync_cb(req, rc);
		return;
	}

	req->lvol_store->super_blob = blob;

	spdk_blob_sync_md(blob, lvs_rename_sync_cb, req);
}

void
spdk_lvs_rename(struct spdk_lvol_store *lvs, const char *new_name,
		spdk_lvs_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvs_req *req;
	struct spdk_lvol_store *tmp;

	/* Check if new name is current lvs name.
	 * If so, return success immediately */
	if (strncmp(lvs->name, new_name, SPDK_LVS_NAME_MAX) == 0) {
		cb_fn(cb_arg, 0);
		return;
	}

	/* Check if new or new_name is already used in other lvs */
	pthread_mutex_lock(&g_lvol_stores_mutex);
	TAILQ_FOREACH(tmp, &g_lvol_stores, link) {
		if (!strncmp(new_name, tmp->name, SPDK_LVS_NAME_MAX) ||
		    !strncmp(new_name, tmp->new_name, SPDK_LVS_NAME_MAX)) {
			pthread_mutex_unlock(&g_lvol_stores_mutex);
			cb_fn(cb_arg, -EEXIST);
			return;
		}
	}
	pthread_mutex_unlock(&g_lvol_stores_mutex);

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	snprintf(lvs->new_name, sizeof(lvs->new_name), "%s", new_name);
	req->lvol_store = lvs;
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_bs_open_blob(lvs->blobstore, lvs->super_blob_id, lvs_rename_open_cb, req);
}

static void
_lvs_unload_cb(void *cb_arg, int lvserrno)
{
	struct spdk_lvs_req *lvs_req = cb_arg;

	SPDK_INFOLOG(lvol, "Lvol store unloaded\n");
	assert(lvs_req->cb_fn != NULL);
	lvs_req->cb_fn(lvs_req->cb_arg, lvserrno);
	free(lvs_req);
}

int
spdk_lvs_unload(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn,
		void *cb_arg)
{
	struct spdk_lvs_req *lvs_req;
	struct spdk_lvol *lvol, *tmp;

	if (lvs == NULL) {
		SPDK_ERRLOG("Lvol store is NULL\n");
		return -ENODEV;
	}

	TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
		if (lvol->action_in_progress == true) {
			SPDK_ERRLOG("Cannot unload lvol store - operations on lvols pending\n");
			cb_fn(cb_arg, -EBUSY);
			return -EBUSY;
		} else if (lvol->ref_count != 0) {
			SPDK_ERRLOG("Lvols still open on lvol store\n");
			cb_fn(cb_arg, -EBUSY);
			return -EBUSY;
		}
	}

	TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
		spdk_lvs_esnap_missing_remove(lvol);
		TAILQ_REMOVE(&lvs->lvols, lvol, link);
		lvol_free(lvol);
	}

	lvs_req = calloc(1, sizeof(*lvs_req));
	if (!lvs_req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store request pointer\n");
		return -ENOMEM;
	}

	lvs_req->cb_fn = cb_fn;
	lvs_req->cb_arg = cb_arg;

	SPDK_INFOLOG(lvol, "Unloading lvol store\n");
	spdk_bs_unload(lvs->blobstore, _lvs_unload_cb, lvs_req);
	lvs_free(lvs);

	return 0;
}

static void
_lvs_destroy_cb(void *cb_arg, int lvserrno)
{
	struct spdk_lvs_destroy_req *lvs_req = cb_arg;

	SPDK_INFOLOG(lvol, "Lvol store destroyed\n");
	assert(lvs_req->cb_fn != NULL);
	lvs_req->cb_fn(lvs_req->cb_arg, lvserrno);
	free(lvs_req);
}

static void
_lvs_destroy_super_cb(void *cb_arg, int bserrno)
{
	struct spdk_lvs_destroy_req *lvs_req = cb_arg;
	struct spdk_lvol_store *lvs = lvs_req->lvs;

	assert(lvs != NULL);

	SPDK_INFOLOG(lvol, "Destroying lvol store\n");
	spdk_bs_destroy(lvs->blobstore, _lvs_destroy_cb, lvs_req);
	lvs_free(lvs);
}

int
spdk_lvs_destroy(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn,
		 void *cb_arg)
{
	struct spdk_lvs_destroy_req *lvs_req;
	struct spdk_lvol *iter_lvol, *tmp;

	if (lvs == NULL) {
		SPDK_ERRLOG("Lvol store is NULL\n");
		return -ENODEV;
	}

	TAILQ_FOREACH_SAFE(iter_lvol, &lvs->lvols, link, tmp) {
		if (iter_lvol->action_in_progress == true) {
			SPDK_ERRLOG("Cannot destroy lvol store - operations on lvols pending\n");
			cb_fn(cb_arg, -EBUSY);
			return -EBUSY;
		} else if (iter_lvol->ref_count != 0) {
			SPDK_ERRLOG("Lvols still open on lvol store\n");
			cb_fn(cb_arg, -EBUSY);
			return -EBUSY;
		}
	}

	TAILQ_FOREACH_SAFE(iter_lvol, &lvs->lvols, link, tmp) {
		free(iter_lvol);
	}

	lvs_req = calloc(1, sizeof(*lvs_req));
	if (!lvs_req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store request pointer\n");
		return -ENOMEM;
	}

	lvs_req->cb_fn = cb_fn;
	lvs_req->cb_arg = cb_arg;
	lvs_req->lvs = lvs;

	SPDK_INFOLOG(lvol, "Deleting super blob\n");
	spdk_bs_delete_blob(lvs->blobstore, lvs->super_blob_id, _lvs_destroy_super_cb, lvs_req);

	return 0;
}

static void
lvol_close_blob_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Could not close blob on lvol\n");
		lvol_free(lvol);
		goto end;
	}

	lvol->ref_count--;
	lvol->action_in_progress = false;
	lvol->blob = NULL;
	SPDK_INFOLOG(lvol, "Lvol %s closed\n", lvol->unique_id);

end:
	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

bool
spdk_lvol_deletable(struct spdk_lvol *lvol)
{
	size_t count = 0;

	spdk_blob_get_clones(lvol->lvol_store->blobstore, lvol->blob_id, NULL, &count);
	return (count == 0);
}

static void
lvol_delete_blob_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;
	struct spdk_lvol *clone_lvol = req->clone_lvol;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Could not remove blob on lvol gracefully - forced removal\n");
	} else {
		SPDK_INFOLOG(lvol, "Lvol %s deleted\n", lvol->unique_id);
	}

	if (lvol->degraded_set != NULL) {
		if (clone_lvol != NULL) {
			/*
			 * A degraded esnap clone that has a blob clone has been deleted. clone_lvol
			 * becomes an esnap clone and needs to be associated with the
			 * spdk_lvs_degraded_lvol_set.
			 */
			struct spdk_lvs_degraded_lvol_set *degraded_set = lvol->degraded_set;

			lvs_degraded_lvol_set_remove(degraded_set, lvol);
			lvs_degraded_lvol_set_add(degraded_set, clone_lvol);
		} else {
			spdk_lvs_esnap_missing_remove(lvol);
		}
	}

	TAILQ_REMOVE(&lvol->lvol_store->lvols, lvol, link);
	lvol_free(lvol);
	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

static void
lvol_create_open_cb(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvol_with_handle_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;

	TAILQ_REMOVE(&req->lvol->lvol_store->pending_lvols, req->lvol, link);

	if (lvolerrno < 0) {
		free(lvol);
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		free(req);
		return;
	}

	lvol->blob = blob;
	lvol->blob_id = spdk_blob_get_id(blob);

	TAILQ_INSERT_TAIL(&lvol->lvol_store->lvols, lvol, link);

	lvol->ref_count++;

	assert(req->cb_fn != NULL);
	req->cb_fn(req->cb_arg, req->lvol, lvolerrno);
	free(req);
}

static void
lvol_create_cb(void *cb_arg, spdk_blob_id blobid, int lvolerrno)
{
	struct spdk_lvol_with_handle_req *req = cb_arg;
	struct spdk_blob_store *bs;
	struct spdk_blob_open_opts opts;

	if (lvolerrno < 0) {
		TAILQ_REMOVE(&req->lvol->lvol_store->pending_lvols, req->lvol, link);
		free(req->lvol);
		assert(req->cb_fn != NULL);
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		free(req);
		return;
	}

	spdk_blob_open_opts_init(&opts, sizeof(opts));
	opts.clear_method = req->lvol->clear_method;
	/*
	 * If the lvol that is being created is an esnap clone, the blobstore needs to be able to
	 * pass the lvol to the esnap_bs_dev_create callback. In order for that to happen, we need
	 * to pass it here.
	 *
	 * This does set ensap_ctx in cases where it's not needed, but we don't know that it's not
	 * needed until after the blob is open. When the blob is not an esnap clone, a reference to
	 * the value stored in opts.esnap_ctx is not retained by the blobstore.
	 */
	opts.esnap_ctx = req->lvol;
	bs = req->lvol->lvol_store->blobstore;

	if (req->origlvol != NULL && req->origlvol->degraded_set != NULL) {
		/*
		 * A snapshot was created from a degraded esnap clone. The new snapshot is now a
		 * degraded esnap clone. The previous clone is now a regular clone of a blob. Update
		 * the set of directly-related clones to the missing external snapshot.
		 */
		struct spdk_lvs_degraded_lvol_set *degraded_set = req->origlvol->degraded_set;

		lvs_degraded_lvol_set_remove(degraded_set, req->origlvol);
		lvs_degraded_lvol_set_add(degraded_set, req->lvol);
	}

	spdk_bs_open_blob_ext(bs, blobid, &opts, lvol_create_open_cb, req);
}

static void
lvol_get_xattr_value(void *xattr_ctx, const char *name,
		     const void **value, size_t *value_len)
{
	struct spdk_lvol *lvol = xattr_ctx;

	if (!strcmp(LVOL_NAME, name)) {
		*value = lvol->name;
		*value_len = SPDK_LVOL_NAME_MAX;
		return;
	}
	if (!strcmp("uuid", name)) {
		*value = lvol->uuid_str;
		*value_len = sizeof(lvol->uuid_str);
		return;
	}
	*value = NULL;
	*value_len = 0;
}

static int
lvs_verify_lvol_name(struct spdk_lvol_store *lvs, const char *name)
{
	struct spdk_lvol *tmp;

	if (name == NULL || strnlen(name, SPDK_LVOL_NAME_MAX) == 0) {
		SPDK_INFOLOG(lvol, "lvol name not provided.\n");
		return -EINVAL;
	}

	if (strnlen(name, SPDK_LVOL_NAME_MAX) == SPDK_LVOL_NAME_MAX) {
		SPDK_ERRLOG("Name has no null terminator.\n");
		return -EINVAL;
	}

	TAILQ_FOREACH(tmp, &lvs->lvols, link) {
		if (!strncmp(name, tmp->name, SPDK_LVOL_NAME_MAX)) {
			SPDK_ERRLOG("lvol with name %s already exists\n", name);
			return -EEXIST;
		}
	}

	TAILQ_FOREACH(tmp, &lvs->pending_lvols, link) {
		if (!strncmp(name, tmp->name, SPDK_LVOL_NAME_MAX)) {
			SPDK_ERRLOG("lvol with name %s is being already created\n", name);
			return -EEXIST;
		}
	}

	return 0;
}

int
spdk_lvol_create(struct spdk_lvol_store *lvs, const char *name, uint64_t sz,
		 bool thin_provision, enum lvol_clear_method clear_method, spdk_lvol_op_with_handle_complete cb_fn,
		 void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;
	struct spdk_blob_store *bs;
	struct spdk_lvol *lvol;
	struct spdk_blob_opts opts;
	char *xattr_names[] = {LVOL_NAME, "uuid"};
	int rc;

	if (lvs == NULL) {
		SPDK_ERRLOG("lvol store does not exist\n");
		return -EINVAL;
	}

	rc = lvs_verify_lvol_name(lvs, name);
	if (rc < 0) {
		return rc;
	}

	bs = lvs->blobstore;

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		return -ENOMEM;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	lvol = lvol_alloc(lvs, name, thin_provision, clear_method);
	if (!lvol) {
		free(req);
		SPDK_ERRLOG("Cannot alloc memory for lvol base pointer\n");
		return -ENOMEM;
	}

	req->lvol = lvol;
	spdk_blob_opts_init(&opts, sizeof(opts));
	opts.thin_provision = thin_provision;
	opts.num_clusters = spdk_divide_round_up(sz, spdk_bs_get_cluster_size(bs));
	opts.clear_method = lvol->clear_method;
	opts.xattrs.count = SPDK_COUNTOF(xattr_names);
	opts.xattrs.names = xattr_names;
	opts.xattrs.ctx = lvol;
	opts.xattrs.get_value = lvol_get_xattr_value;

	spdk_bs_create_blob_ext(lvs->blobstore, &opts, lvol_create_cb, req);

	return 0;
}

int
spdk_lvol_create_esnap_clone(const void *esnap_id, uint32_t id_len, uint64_t size_bytes,
			     struct spdk_lvol_store *lvs, const char *clone_name,
			     spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;
	struct spdk_blob_store *bs;
	struct spdk_lvol *lvol;
	struct spdk_blob_opts opts;
	uint64_t cluster_sz;
	char *xattr_names[] = {LVOL_NAME, "uuid"};
	int rc;

	if (lvs == NULL) {
		SPDK_ERRLOG("lvol store does not exist\n");
		return -EINVAL;
	}

	rc = lvs_verify_lvol_name(lvs, clone_name);
	if (rc < 0) {
		return rc;
	}

	bs = lvs->blobstore;

	cluster_sz = spdk_bs_get_cluster_size(bs);
	if ((size_bytes % cluster_sz) != 0) {
		SPDK_ERRLOG("Cannot create '%s/%s': size %" PRIu64 " is not an integer multiple of "
			    "cluster size %" PRIu64 "\n", lvs->name, clone_name, size_bytes,
			    cluster_sz);
		return -EINVAL;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		return -ENOMEM;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	lvol = lvol_alloc(lvs, clone_name, true, LVOL_CLEAR_WITH_DEFAULT);
	if (!lvol) {
		free(req);
		SPDK_ERRLOG("Cannot alloc memory for lvol base pointer\n");
		return -ENOMEM;
	}
	req->lvol = lvol;

	spdk_blob_opts_init(&opts, sizeof(opts));
	opts.esnap_id = esnap_id;
	opts.esnap_id_len = id_len;
	opts.thin_provision = true;
	opts.num_clusters = spdk_divide_round_up(size_bytes, cluster_sz);
	opts.clear_method = lvol->clear_method;
	opts.xattrs.count = SPDK_COUNTOF(xattr_names);
	opts.xattrs.names = xattr_names;
	opts.xattrs.ctx = lvol;
	opts.xattrs.get_value = lvol_get_xattr_value;

	spdk_bs_create_blob_ext(lvs->blobstore, &opts, lvol_create_cb, req);

	return 0;
}

void
spdk_lvol_create_snapshot(struct spdk_lvol *origlvol, const char *snapshot_name,
			  spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvol *newlvol;
	struct spdk_blob *origblob;
	struct spdk_lvol_with_handle_req *req;
	struct spdk_blob_xattr_opts snapshot_xattrs;
	char *xattr_names[] = {LVOL_NAME, "uuid"};
	int rc;

	if (origlvol == NULL) {
		SPDK_INFOLOG(lvol, "Lvol not provided.\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	origblob = origlvol->blob;
	lvs = origlvol->lvol_store;
	if (lvs == NULL) {
		SPDK_ERRLOG("lvol store does not exist\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	rc = lvs_verify_lvol_name(lvs, snapshot_name);
	if (rc < 0) {
		cb_fn(cb_arg, NULL, rc);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	newlvol = lvol_alloc(origlvol->lvol_store, snapshot_name, true,
			     (enum lvol_clear_method)origlvol->clear_method);
	if (!newlvol) {
		SPDK_ERRLOG("Cannot alloc memory for lvol base pointer\n");
		free(req);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	snapshot_xattrs.count = SPDK_COUNTOF(xattr_names);
	snapshot_xattrs.ctx = newlvol;
	snapshot_xattrs.names = xattr_names;
	snapshot_xattrs.get_value = lvol_get_xattr_value;
	req->lvol = newlvol;
	req->origlvol = origlvol;
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_bs_create_snapshot(lvs->blobstore, spdk_blob_get_id(origblob), &snapshot_xattrs,
				lvol_create_cb, req);
}

void
spdk_lvol_create_clone(struct spdk_lvol *origlvol, const char *clone_name,
		       spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol *newlvol;
	struct spdk_lvol_with_handle_req *req;
	struct spdk_lvol_store *lvs;
	struct spdk_blob *origblob;
	struct spdk_blob_xattr_opts clone_xattrs;
	char *xattr_names[] = {LVOL_NAME, "uuid"};
	int rc;

	if (origlvol == NULL) {
		SPDK_INFOLOG(lvol, "Lvol not provided.\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	origblob = origlvol->blob;
	lvs = origlvol->lvol_store;
	if (lvs == NULL) {
		SPDK_ERRLOG("lvol store does not exist\n");
		cb_fn(cb_arg, NULL, -EINVAL);
		return;
	}

	rc = lvs_verify_lvol_name(lvs, clone_name);
	if (rc < 0) {
		cb_fn(cb_arg, NULL, rc);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	newlvol = lvol_alloc(lvs, clone_name, true, (enum lvol_clear_method)origlvol->clear_method);
	if (!newlvol) {
		SPDK_ERRLOG("Cannot alloc memory for lvol base pointer\n");
		free(req);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	clone_xattrs.count = SPDK_COUNTOF(xattr_names);
	clone_xattrs.ctx = newlvol;
	clone_xattrs.names = xattr_names;
	clone_xattrs.get_value = lvol_get_xattr_value;
	req->lvol = newlvol;
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_bs_create_clone(lvs->blobstore, spdk_blob_get_id(origblob), &clone_xattrs,
			     lvol_create_cb,
			     req);
}

static void
lvol_resize_done(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;

	req->cb_fn(req->cb_arg,  lvolerrno);
	free(req);
}

static void
lvol_blob_resize_cb(void *cb_arg, int bserrno)
{
	struct spdk_lvol_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;

	if (bserrno != 0) {
		req->cb_fn(req->cb_arg, bserrno);
		free(req);
		return;
	}

	spdk_blob_sync_md(lvol->blob, lvol_resize_done, req);
}

void
spdk_lvol_resize(struct spdk_lvol *lvol, uint64_t sz,
		 spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_blob *blob = lvol->blob;
	struct spdk_lvol_store *lvs = lvol->lvol_store;
	struct spdk_lvol_req *req;
	uint64_t new_clusters = spdk_divide_round_up(sz, spdk_bs_get_cluster_size(lvs->blobstore));

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->lvol = lvol;

	spdk_blob_resize(blob, new_clusters, lvol_blob_resize_cb, req);
}

static void
lvol_set_read_only_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;

	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

void
spdk_lvol_set_read_only(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_blob_set_read_only(lvol->blob);
	spdk_blob_sync_md(lvol->blob, lvol_set_read_only_cb, req);
}

static void
lvol_rename_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;

	if (lvolerrno != 0) {
		SPDK_ERRLOG("Lvol rename operation failed\n");
	} else {
		snprintf(req->lvol->name, sizeof(req->lvol->name), "%s", req->name);
	}

	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

void
spdk_lvol_rename(struct spdk_lvol *lvol, const char *new_name,
		 spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol *tmp;
	struct spdk_blob *blob = lvol->blob;
	struct spdk_lvol_req *req;
	int rc;

	/* Check if new name is current lvol name.
	 * If so, return success immediately */
	if (strncmp(lvol->name, new_name, SPDK_LVOL_NAME_MAX) == 0) {
		cb_fn(cb_arg, 0);
		return;
	}

	/* Check if lvol with 'new_name' already exists in lvolstore */
	TAILQ_FOREACH(tmp, &lvol->lvol_store->lvols, link) {
		if (strncmp(tmp->name, new_name, SPDK_LVOL_NAME_MAX) == 0) {
			SPDK_ERRLOG("Lvol %s already exists in lvol store %s\n", new_name, lvol->lvol_store->name);
			cb_fn(cb_arg, -EEXIST);
			return;
		}
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->lvol = lvol;
	snprintf(req->name, sizeof(req->name), "%s", new_name);

	rc = spdk_blob_set_xattr(blob, "name", new_name, strlen(new_name) + 1);
	if (rc < 0) {
		free(req);
		cb_fn(cb_arg, rc);
		return;
	}

	spdk_blob_sync_md(blob, lvol_rename_cb, req);
}

void
spdk_lvol_destroy(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;
	struct spdk_blob_store *bs;
	struct spdk_lvol_store	*lvs = lvol->lvol_store;
	spdk_blob_id	clone_id;
	size_t		count = 1;
	int		rc;

	assert(cb_fn != NULL);

	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	if (lvol->ref_count != 0) {
		SPDK_ERRLOG("Cannot destroy lvol %s because it is still open\n", lvol->unique_id);
		cb_fn(cb_arg, -EBUSY);
		return;
	}

	lvol->action_in_progress = true;

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->lvol = lvol;
	bs = lvol->lvol_store->blobstore;

	rc = spdk_blob_get_clones(lvs->blobstore, lvol->blob_id, &clone_id, &count);
	if (rc == 0 && count == 1) {
		req->clone_lvol = lvs_get_lvol_by_blob_id(lvs, clone_id);
	} else if (rc == -ENOMEM) {
		SPDK_INFOLOG(lvol, "lvol %s: cannot destroy: has %" PRIu64 " clones\n",
			     lvol->unique_id, count);
		free(req);
		assert(count > 1);
		cb_fn(cb_arg, -EBUSY);
		return;
	}

	spdk_bs_delete_blob(bs, lvol->blob_id, lvol_delete_blob_cb, req);
}

void
spdk_lvol_close(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;

	assert(cb_fn != NULL);

	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	if (lvol->ref_count > 1) {
		lvol->ref_count--;
		cb_fn(cb_arg, 0);
		return;
	} else if (lvol->ref_count == 0) {
		cb_fn(cb_arg, -EINVAL);
		return;
	}

	lvol->action_in_progress = true;

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->lvol = lvol;

	spdk_blob_close(lvol->blob, lvol_close_blob_cb, req);
}

struct spdk_io_channel *
spdk_lvol_get_io_channel(struct spdk_lvol *lvol)
{
	return spdk_bs_alloc_io_channel(lvol->lvol_store->blobstore);
}

static void
lvol_inflate_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;

	spdk_bs_free_io_channel(req->channel);

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Could not inflate lvol\n");
	}

	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

void
spdk_lvol_inflate(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;
	spdk_blob_id blob_id;

	assert(cb_fn != NULL);

	if (lvol == NULL) {
		SPDK_ERRLOG("Lvol does not exist\n");
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->channel = spdk_bs_alloc_io_channel(lvol->lvol_store->blobstore);
	if (req->channel == NULL) {
		SPDK_ERRLOG("Cannot alloc io channel for lvol inflate request\n");
		free(req);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	blob_id = spdk_blob_get_id(lvol->blob);
	spdk_bs_inflate_blob(lvol->lvol_store->blobstore, req->channel, blob_id, lvol_inflate_cb,
			     req);
}

void
spdk_lvol_decouple_parent(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;
	spdk_blob_id blob_id;

	assert(cb_fn != NULL);

	if (lvol == NULL) {
		SPDK_ERRLOG("Lvol does not exist\n");
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->channel = spdk_bs_alloc_io_channel(lvol->lvol_store->blobstore);
	if (req->channel == NULL) {
		SPDK_ERRLOG("Cannot alloc io channel for lvol inflate request\n");
		free(req);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	blob_id = spdk_blob_get_id(lvol->blob);
	spdk_bs_blob_decouple_parent(lvol->lvol_store->blobstore, req->channel, blob_id,
				     lvol_inflate_cb, req);
}

void
spdk_lvs_grow(struct spdk_bs_dev *bs_dev, spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvs_with_handle_req *req;
	struct spdk_bs_opts opts = {};

	assert(cb_fn != NULL);

	if (bs_dev == NULL) {
		SPDK_ERRLOG("Blobstore device does not exist\n");
		cb_fn(cb_arg, NULL, -ENODEV);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for request structure\n");
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	req->lvol_store = lvs_alloc();
	if (req->lvol_store == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store\n");
		free(req);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->bs_dev = bs_dev;

	lvs_bs_opts_init(&opts);
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "LVOLSTORE");

	spdk_bs_grow(bs_dev, &opts, lvs_load_cb, req);
}

static struct spdk_lvol *
lvs_get_lvol_by_blob_id(struct spdk_lvol_store *lvs, spdk_blob_id blob_id)
{
	struct spdk_lvol *lvol;

	TAILQ_FOREACH(lvol, &lvs->lvols, link) {
		if (lvol->blob_id == blob_id) {
			return lvol;
		}
	}
	return NULL;
}

static int
lvs_esnap_bs_dev_create(void *bs_ctx, void *blob_ctx, struct spdk_blob *blob,
			const void *esnap_id, uint32_t id_len,
			struct spdk_bs_dev **bs_dev)
{
	struct spdk_lvol_store	*lvs = bs_ctx;
	struct spdk_lvol	*lvol = blob_ctx;
	spdk_blob_id		blob_id = spdk_blob_get_id(blob);

	if (lvs == NULL) {
		if (lvol == NULL) {
			SPDK_ERRLOG("Blob 0x%" PRIx64 ": no lvs context nor lvol context\n",
				    blob_id);
			return -EINVAL;
		}
		lvs = lvol->lvol_store;
	}

	/*
	 * When spdk_lvs_load() is called, it iterates through all blobs in its blobstore building
	 * up a list of lvols (lvs->lvols). During this initial iteration, each blob is opened,
	 * passed to load_next_lvol(), then closed. There is no need to open the external snapshot
	 * during this phase. Once the blobstore is loaded, lvs->load_esnaps is set to true so that
	 * future lvol opens cause the external snapshot to be loaded.
	 */
	if (!lvs->load_esnaps) {
		*bs_dev = NULL;
		return 0;
	}

	if (lvol == NULL) {
		spdk_blob_id blob_id = spdk_blob_get_id(blob);

		/*
		 * If spdk_bs_blob_open() is used instead of spdk_bs_blob_open_ext() the lvol will
		 * not have been passed in. The same is true if the open happens spontaneously due
		 * to blobstore activity.
		 */
		lvol = lvs_get_lvol_by_blob_id(lvs, blob_id);
		if (lvol == NULL) {
			SPDK_ERRLOG("lvstore %s: no lvol for blob 0x%" PRIx64 "\n",
				    lvs->name, blob_id);
			return -ENODEV;
		}
	}

	return lvs->esnap_bs_dev_create(lvs, lvol, blob, esnap_id, id_len, bs_dev);
}

/*
 * The theory of missing external snapshots
 *
 * The lvs->esnap_bs_dev_create() callback may be unable to create an external snapshot bs_dev when
 * it is called. This can happen, for instance, as when the device containing the lvolstore is
 * examined prior to spdk_bdev_register() being called on a bdev that acts as an external snapshot.
 * In such a case, the esnap_bs_dev_create() callback will call spdk_lvs_esnap_missing_add().
 *
 * Missing external snapshots are tracked in a per-lvolstore tree, lvs->degraded_lvol_sets_tree.
 * Each tree node (struct spdk_lvs_degraded_lvol_set) contains a tailq of lvols that are missing
 * that particular external snapshot.
 *
 * When a potential missing snapshot becomes available, spdk_lvs_notify_hotplug() may be called to
 * notify this library that it is available. It will then iterate through the active lvolstores and
 * search each lvs->degraded_lvol_sets_tree for a set of degraded lvols that are missing an external
 * snapshot matching the id passed in the notification. The lvols in the tailq on each matching tree
 * node are then asked to create an external snapshot bs_dev using the esnap_bs_dev_create()
 * callback that the consumer registered with the lvolstore. If lvs->esnap_bs_dev_create() returns
 * 0, the lvol is removed from the spdk_lvs_degraded_lvol_set's lvol tailq. When this tailq becomes
 * empty, the degraded lvol set node for this missing external snapshot is removed.
 */
static int
lvs_esnap_name_cmp(struct spdk_lvs_degraded_lvol_set *m1, struct spdk_lvs_degraded_lvol_set *m2)
{
	if (m1->id_len == m2->id_len) {
		return memcmp(m1->esnap_id, m2->esnap_id, m1->id_len);
	}
	return (m1->id_len > m2->id_len) ? 1 : -1;
}

RB_GENERATE_STATIC(degraded_lvol_sets_tree, spdk_lvs_degraded_lvol_set, node, lvs_esnap_name_cmp)

static void
lvs_degraded_lvol_set_add(struct spdk_lvs_degraded_lvol_set *degraded_set, struct spdk_lvol *lvol)
{
	assert(lvol->lvol_store->thread == spdk_get_thread());

	lvol->degraded_set = degraded_set;
	TAILQ_INSERT_TAIL(&degraded_set->lvols, lvol, degraded_link);
}

static void
lvs_degraded_lvol_set_remove(struct spdk_lvs_degraded_lvol_set *degraded_set,
			     struct spdk_lvol *lvol)
{
	assert(lvol->lvol_store->thread == spdk_get_thread());

	lvol->degraded_set = NULL;
	TAILQ_REMOVE(&degraded_set->lvols, lvol, degraded_link);
	/* degraded_set->lvols may be empty. Caller should check if not immediately adding a new
	 * lvol. */
}

/*
 * Record in lvs->degraded_lvol_sets_tree that a bdev of the specified name is needed by the
 * specified lvol.
 */
int
spdk_lvs_esnap_missing_add(struct spdk_lvol_store *lvs, struct spdk_lvol *lvol,
			   const void *esnap_id, uint32_t id_len)
{
	struct spdk_lvs_degraded_lvol_set find, *degraded_set;

	assert(lvs->thread == spdk_get_thread());

	find.esnap_id = esnap_id;
	find.id_len = id_len;
	degraded_set = RB_FIND(degraded_lvol_sets_tree, &lvs->degraded_lvol_sets_tree, &find);
	if (degraded_set == NULL) {
		degraded_set = calloc(1, sizeof(*degraded_set));
		if (degraded_set == NULL) {
			SPDK_ERRLOG("lvol %s: cannot create degraded_set node: out of memory\n",
				    lvol->unique_id);
			return -ENOMEM;
		}
		degraded_set->esnap_id = calloc(1, id_len);
		if (degraded_set->esnap_id == NULL) {
			free(degraded_set);
			SPDK_ERRLOG("lvol %s: cannot create degraded_set node: out of memory\n",
				    lvol->unique_id);
			return -ENOMEM;
		}
		memcpy((void *)degraded_set->esnap_id, esnap_id, id_len);
		degraded_set->id_len = id_len;
		degraded_set->lvol_store = lvs;
		TAILQ_INIT(&degraded_set->lvols);
		RB_INSERT(degraded_lvol_sets_tree, &lvs->degraded_lvol_sets_tree, degraded_set);
	}

	lvs_degraded_lvol_set_add(degraded_set, lvol);

	return 0;
}

/*
 * Remove the record of the specified lvol needing a degraded_set bdev.
 */
void
spdk_lvs_esnap_missing_remove(struct spdk_lvol *lvol)
{
	struct spdk_lvol_store		*lvs = lvol->lvol_store;
	struct spdk_lvs_degraded_lvol_set	*degraded_set = lvol->degraded_set;

	assert(lvs->thread == spdk_get_thread());

	if (degraded_set == NULL) {
		return;
	}

	lvs_degraded_lvol_set_remove(degraded_set, lvol);

	if (!TAILQ_EMPTY(&degraded_set->lvols)) {
		return;
	}

	RB_REMOVE(degraded_lvol_sets_tree, &lvs->degraded_lvol_sets_tree, degraded_set);

	free((char *)degraded_set->esnap_id);
	free(degraded_set);
}

struct lvs_esnap_hotplug_req {
	struct spdk_lvol			*lvol;
	spdk_lvol_op_with_handle_complete	cb_fn;
	void					*cb_arg;
};

static void
lvs_esnap_hotplug_done(void *cb_arg, int bserrno)
{
	struct lvs_esnap_hotplug_req *req = cb_arg;
	struct spdk_lvol	*lvol = req->lvol;
	struct spdk_lvol_store	*lvs = lvol->lvol_store;

	if (bserrno != 0) {
		SPDK_ERRLOG("lvol %s/%s: failed to hotplug blob_bdev due to error %d\n",
			    lvs->name, lvol->name, bserrno);
	}
	req->cb_fn(req->cb_arg, lvol, bserrno);
	free(req);
}

static void
lvs_esnap_degraded_hotplug(struct spdk_lvs_degraded_lvol_set *degraded_set,
			   spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store	*lvs = degraded_set->lvol_store;
	struct spdk_lvol	*lvol, *tmp, *last_missing;
	struct spdk_bs_dev	*bs_dev;
	const void		*esnap_id = degraded_set->esnap_id;
	uint32_t		id_len = degraded_set->id_len;
	struct lvs_esnap_hotplug_req *req;
	int			rc;

	assert(lvs->thread == spdk_get_thread());

	/*
	 * When lvs->esnap_bs_bdev_create() tries to load an external snapshot, it can encounter
	 * errors that lead it to calling spdk_lvs_esnap_missing_add(). This function needs to be
	 * sure that such modifications do not lead to degraded_set->lvols tailqs or references
	 * to memory that this function will free.
	 *
	 * While this function is running, no other thread can add items to degraded_set->lvols. If
	 * the list is mutated, it must have been done by this function or something in its call
	 * graph running on this thread.
	 */

	/* Remember the last lvol on the list. Iteration will stop once it has been processed. */
	last_missing = TAILQ_LAST(&degraded_set->lvols, degraded_lvols);

	TAILQ_FOREACH_SAFE(lvol, &degraded_set->lvols, degraded_link, tmp) {
		req = calloc(1, sizeof(*req));
		if (req == NULL) {
			SPDK_ERRLOG("lvol %s: failed to create esnap bs_dev: out of memory\n",
				    lvol->unique_id);
			cb_fn(cb_arg, lvol, -ENOMEM);
			/* The next one likely won't succeed either, but keep going so that all the
			 * failed hotplugs are logged.
			 */
			goto next;
		}

		/*
		 * Remove the lvol from the tailq so that tailq corruption is avoided if
		 * lvs->esnap_bs_dev_create() calls spdk_lvs_esnap_missing_add(lvol).
		 */
		TAILQ_REMOVE(&degraded_set->lvols, lvol, degraded_link);
		lvol->degraded_set = NULL;

		bs_dev = NULL;
		rc = lvs->esnap_bs_dev_create(lvs, lvol, lvol->blob, esnap_id, id_len, &bs_dev);
		if (rc != 0) {
			SPDK_ERRLOG("lvol %s: failed to create esnap bs_dev: error %d\n",
				    lvol->unique_id, rc);
			lvol->degraded_set = degraded_set;
			TAILQ_INSERT_TAIL(&degraded_set->lvols, lvol, degraded_link);
			cb_fn(cb_arg, lvol, rc);
			free(req);
			goto next;
		}

		req->lvol = lvol;
		req->cb_fn = cb_fn;
		req->cb_arg = cb_arg;
		spdk_blob_set_esnap_bs_dev(lvol->blob, bs_dev, lvs_esnap_hotplug_done, req);

next:
		if (lvol == last_missing) {
			/*
			 * Anything after last_missing was added due to some problem encountered
			 * while trying to create the esnap bs_dev.
			 */
			break;
		}
	}

	if (TAILQ_EMPTY(&degraded_set->lvols)) {
		RB_REMOVE(degraded_lvol_sets_tree, &lvs->degraded_lvol_sets_tree, degraded_set);
		free((void *)degraded_set->esnap_id);
		free(degraded_set);
	}
}

/*
 * Notify each lvstore created on this thread that is missing a bdev by the specified name or uuid
 * that the bdev now exists.
 */
bool
spdk_lvs_notify_hotplug(const void *esnap_id, uint32_t id_len,
			spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvs_degraded_lvol_set *found;
	struct spdk_lvs_degraded_lvol_set find = { 0 };
	struct spdk_lvol_store	*lvs;
	struct spdk_thread	*thread = spdk_get_thread();
	bool			ret = false;

	find.esnap_id = esnap_id;
	find.id_len = id_len;

	pthread_mutex_lock(&g_lvol_stores_mutex);
	TAILQ_FOREACH(lvs, &g_lvol_stores, link) {
		if (thread != lvs->thread) {
			/*
			 * It is expected that this is called from vbdev_lvol's examine_config()
			 * callback. The lvstore was likely loaded do a creation happening as a
			 * result of an RPC call or opening of an existing lvstore via
			 * examine_disk() callback. RPC calls, examine_disk(), and examine_config()
			 * should all be happening only on the app thread. The "wrong thread"
			 * condition will only happen when an application is doing something weird.
			 */
			SPDK_NOTICELOG("Discarded examine for lvstore %s: wrong thread\n",
				       lvs->name);
			continue;
		}

		found = RB_FIND(degraded_lvol_sets_tree, &lvs->degraded_lvol_sets_tree, &find);
		if (found == NULL) {
			continue;
		}

		ret = true;
		lvs_esnap_degraded_hotplug(found, cb_fn, cb_arg);
	}
	pthread_mutex_unlock(&g_lvol_stores_mutex);

	return ret;
}

int
spdk_lvol_iter_immediate_clones(struct spdk_lvol *lvol, spdk_lvol_iter_cb cb_fn, void *cb_arg)
{
	struct spdk_lvol_store *lvs = lvol->lvol_store;
	struct spdk_blob_store *bs = lvs->blobstore;
	struct spdk_lvol *clone;
	spdk_blob_id *ids;
	size_t id_cnt = 0;
	size_t i;
	int rc;

	rc = spdk_blob_get_clones(bs, lvol->blob_id, NULL, &id_cnt);
	if (rc != -ENOMEM) {
		/* -ENOMEM says id_cnt is valid, no other errors should be returned. */
		assert(rc == 0);
		return rc;
	}

	ids = calloc(id_cnt, sizeof(*ids));
	if (ids == NULL) {
		SPDK_ERRLOG("lvol %s: out of memory while iterating clones\n", lvol->unique_id);
		return -ENOMEM;
	}

	rc = spdk_blob_get_clones(bs, lvol->blob_id, ids, &id_cnt);
	if (rc != 0) {
		SPDK_ERRLOG("lvol %s: unable to get clone blob IDs: %d\n", lvol->unique_id, rc);
		free(ids);
		return rc;
	}

	for (i = 0; i < id_cnt; i++) {
		clone = lvs_get_lvol_by_blob_id(lvs, ids[i]);
		if (clone == NULL) {
			SPDK_NOTICELOG("lvol %s: unable to find clone lvol with blob id 0x%"
				       PRIx64 "\n", lvol->unique_id, ids[i]);
			continue;
		}
		rc = cb_fn(cb_arg, clone);
		if (rc != 0) {
			SPDK_DEBUGLOG(lvol, "lvol %s: iteration stopped when lvol %s (blob 0x%"
				      PRIx64 ") returned %d\n", lvol->unique_id, clone->unique_id,
				      ids[i], rc);
			break;
		}
	}

	free(ids);
	return rc;
}

struct spdk_lvol *
spdk_lvol_get_by_uuid(const struct spdk_uuid *uuid)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvol *lvol;

	pthread_mutex_lock(&g_lvol_stores_mutex);

	TAILQ_FOREACH(lvs, &g_lvol_stores, link) {
		TAILQ_FOREACH(lvol, &lvs->lvols, link) {
			if (spdk_uuid_compare(uuid, &lvol->uuid) == 0) {
				pthread_mutex_unlock(&g_lvol_stores_mutex);
				return lvol;
			}
		}
	}

	pthread_mutex_unlock(&g_lvol_stores_mutex);
	return NULL;
}

struct spdk_lvol *
spdk_lvol_get_by_names(const char *lvs_name, const char *lvol_name)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvol *lvol;

	pthread_mutex_lock(&g_lvol_stores_mutex);

	TAILQ_FOREACH(lvs, &g_lvol_stores, link) {
		if (strcmp(lvs_name, lvs->name) != 0) {
			continue;
		}
		TAILQ_FOREACH(lvol, &lvs->lvols, link) {
			if (strcmp(lvol_name, lvol->name) == 0) {
				pthread_mutex_unlock(&g_lvol_stores_mutex);
				return lvol;
			}
		}
	}

	pthread_mutex_unlock(&g_lvol_stores_mutex);
	return NULL;
}

bool
spdk_lvol_is_degraded(const struct spdk_lvol *lvol)
{
	struct spdk_blob *blob = lvol->blob;

	if (blob == NULL) {
		return true;
	}
	return spdk_blob_is_degraded(blob);
}
