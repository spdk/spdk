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

#include "spdk_internal/lvolstore.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/blob_bdev.h"
#include "spdk/util.h"

/* Default blob channel opts for lvol */
#define SPDK_LVOL_BLOB_OPTS_CHANNEL_OPS 512

#define LVOL_NAME "name"

SPDK_LOG_REGISTER_COMPONENT(lvol)

static TAILQ_HEAD(, spdk_lvol_store) g_lvol_stores = TAILQ_HEAD_INITIALIZER(g_lvol_stores);
static pthread_mutex_t g_lvol_stores_mutex = PTHREAD_MUTEX_INITIALIZER;

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

static void
lvs_free(struct spdk_lvol_store *lvs)
{
	pthread_mutex_lock(&g_lvol_stores_mutex);
	if (lvs->on_list) {
		TAILQ_REMOVE(&g_lvol_stores, lvs, link);
	}
	pthread_mutex_unlock(&g_lvol_stores_mutex);

	free(lvs);
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

	spdk_blob_open_opts_init(&opts);
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
		req->cb_fn(req->cb_arg, lvs, 0);
		free(req);
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

	lvol->blob = blob;
	lvol->blob_id = blob_id;
	lvol->lvol_store = lvs;
	lvol->thin_provision = spdk_blob_is_thin_provisioned(blob);

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

	spdk_bs_iter_next(bs, blob, load_next_lvol, req);

	return;

invalid:
	TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
		TAILQ_REMOVE(&lvs->lvols, lvol, link);
		free(lvol);
	}

	lvs_free(lvs);
	spdk_bs_unload(bs, bs_unload_with_error_cb, req);
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
		SPDK_INFOLOG(lvol, "missing or incorrect UUID\n");
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
		SPDK_INFOLOG(lvol, "missing or invalid name\n");
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
	struct spdk_lvol_store *lvs;

	if (lvolerrno != 0) {
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		free(req);
		return;
	}

	lvs = calloc(1, sizeof(*lvs));
	if (lvs == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store\n");
		spdk_bs_unload(bs, bs_unload_with_error_cb, req);
		return;
	}

	lvs->blobstore = bs;
	lvs->bs_dev = req->bs_dev;
	TAILQ_INIT(&lvs->lvols);
	TAILQ_INIT(&lvs->pending_lvols);

	req->lvol_store = lvs;

	spdk_bs_get_super(bs, lvs_open_super, req);
}

static void
lvs_bs_opts_init(struct spdk_bs_opts *opts)
{
	spdk_bs_opts_init(opts);
	opts->max_channel_ops = SPDK_LVOL_BLOB_OPTS_CHANNEL_OPS;
}

void
spdk_lvs_load(struct spdk_bs_dev *bs_dev, spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
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

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	req->bs_dev = bs_dev;

	lvs_bs_opts_init(&opts);
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "LVOLSTORE");

	spdk_bs_load(bs_dev, &opts, lvs_load_cb, req);
}

static void
remove_bs_on_error_cb(void *cb_arg, int bserrno)
{
}

static void
super_create_close_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Lvol store init failed: could not close super blob\n");
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		spdk_bs_destroy(lvs->blobstore, remove_bs_on_error_cb, NULL);
		lvs_free(lvs);
		free(req);
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
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		SPDK_ERRLOG("Lvol store init failed: could not set uuid for super blob\n");
		spdk_bs_destroy(lvs->blobstore, remove_bs_on_error_cb, NULL);
		lvs_free(lvs);
		free(req);
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
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		SPDK_ERRLOG("Lvol store init failed: could not set super blob\n");
		spdk_bs_destroy(lvs->blobstore, remove_bs_on_error_cb, NULL);
		lvs_free(lvs);
		free(req);
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
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		SPDK_ERRLOG("Lvol store init failed: could not open super blob\n");
		spdk_bs_destroy(lvs->blobstore, remove_bs_on_error_cb, NULL);
		lvs_free(lvs);
		free(req);
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
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		SPDK_ERRLOG("Lvol store init failed: could not create super blob\n");
		spdk_bs_destroy(lvs->blobstore, remove_bs_on_error_cb, NULL);
		lvs_free(lvs);
		free(req);
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
	TAILQ_INIT(&lvs->lvols);
	TAILQ_INIT(&lvs->pending_lvols);

	SPDK_INFOLOG(lvol, "Lvol store initialized\n");

	/* create super blob */
	spdk_bs_create_blob(lvs->blobstore, super_blob_create_cb, lvs_req);
}

void
spdk_lvs_opts_init(struct spdk_lvs_opts *o)
{
	o->cluster_sz = SPDK_LVS_OPTS_CLUSTER_SZ;
	o->clear_method = LVS_CLEAR_WITH_UNMAP;
	memset(o->name, 0, sizeof(o->name));
}

static void
setup_lvs_opts(struct spdk_bs_opts *bs_opts, struct spdk_lvs_opts *o)
{
	assert(o != NULL);
	lvs_bs_opts_init(bs_opts);
	bs_opts->cluster_sz = o->cluster_sz;
	bs_opts->clear_method = (enum bs_clear_method)o->clear_method;
}

int
spdk_lvs_init(struct spdk_bs_dev *bs_dev, struct spdk_lvs_opts *o,
	      spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvs_with_handle_req *lvs_req;
	struct spdk_bs_opts opts = {};
	int rc;

	if (bs_dev == NULL) {
		SPDK_ERRLOG("Blobstore device does not exist\n");
		return -ENODEV;
	}

	if (o == NULL) {
		SPDK_ERRLOG("spdk_lvs_opts not specified\n");
		return -EINVAL;
	}

	setup_lvs_opts(&opts, o);

	if (strnlen(o->name, SPDK_LVS_NAME_MAX) == SPDK_LVS_NAME_MAX) {
		SPDK_ERRLOG("Name has no null terminator.\n");
		return -EINVAL;
	}

	if (strnlen(o->name, SPDK_LVS_NAME_MAX) == 0) {
		SPDK_ERRLOG("No name specified.\n");
		return -EINVAL;
	}

	lvs = calloc(1, sizeof(*lvs));
	if (!lvs) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store base pointer\n");
		return -ENOMEM;
	}

	spdk_uuid_generate(&lvs->uuid);
	snprintf(lvs->name, sizeof(lvs->name), "%s", o->name);

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
	lvs->destruct = false;

	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "LVOLSTORE");

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
		 * Otherwise it could cause a failure on the next attepmt to change the name to 'new_name'  */
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

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Could not remove blob on lvol gracefully - forced removal\n");
	} else {
		SPDK_INFOLOG(lvol, "Lvol %s deleted\n", lvol->unique_id);
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

	snprintf(lvol->unique_id, sizeof(lvol->unique_id), "%s", lvol->uuid_str);
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

	spdk_blob_open_opts_init(&opts);
	opts.clear_method = req->lvol->clear_method;
	bs = req->lvol->lvol_store->blobstore;

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
	} else if (!strcmp("uuid", name)) {
		*value = lvol->uuid_str;
		*value_len = sizeof(lvol->uuid_str);
	}
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
	uint64_t num_clusters;
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

	lvol = calloc(1, sizeof(*lvol));
	if (!lvol) {
		free(req);
		SPDK_ERRLOG("Cannot alloc memory for lvol base pointer\n");
		return -ENOMEM;
	}
	lvol->lvol_store = lvs;
	num_clusters = spdk_divide_round_up(sz, spdk_bs_get_cluster_size(bs));
	lvol->thin_provision = thin_provision;
	lvol->clear_method = (enum blob_clear_method)clear_method;
	snprintf(lvol->name, sizeof(lvol->name), "%s", name);
	TAILQ_INSERT_TAIL(&lvol->lvol_store->pending_lvols, lvol, link);
	spdk_uuid_generate(&lvol->uuid);
	spdk_uuid_fmt_lower(lvol->uuid_str, sizeof(lvol->uuid_str), &lvol->uuid);
	req->lvol = lvol;

	spdk_blob_opts_init(&opts);
	opts.thin_provision = thin_provision;
	opts.num_clusters = num_clusters;
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

	newlvol = calloc(1, sizeof(*newlvol));
	if (!newlvol) {
		SPDK_ERRLOG("Cannot alloc memory for lvol base pointer\n");
		free(req);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	newlvol->lvol_store = origlvol->lvol_store;
	snprintf(newlvol->name, sizeof(newlvol->name), "%s", snapshot_name);
	TAILQ_INSERT_TAIL(&newlvol->lvol_store->pending_lvols, newlvol, link);
	spdk_uuid_generate(&newlvol->uuid);
	spdk_uuid_fmt_lower(newlvol->uuid_str, sizeof(newlvol->uuid_str), &newlvol->uuid);
	snapshot_xattrs.count = SPDK_COUNTOF(xattr_names);
	snapshot_xattrs.ctx = newlvol;
	snapshot_xattrs.names = xattr_names;
	snapshot_xattrs.get_value = lvol_get_xattr_value;
	req->lvol = newlvol;
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

	newlvol = calloc(1, sizeof(*newlvol));
	if (!newlvol) {
		SPDK_ERRLOG("Cannot alloc memory for lvol base pointer\n");
		free(req);
		cb_fn(cb_arg, NULL, -ENOMEM);
		return;
	}

	newlvol->lvol_store = lvs;
	snprintf(newlvol->name, sizeof(newlvol->name), "%s", clone_name);
	TAILQ_INSERT_TAIL(&newlvol->lvol_store->pending_lvols, newlvol, link);
	spdk_uuid_generate(&newlvol->uuid);
	spdk_uuid_fmt_lower(newlvol->uuid_str, sizeof(newlvol->uuid_str), &newlvol->uuid);
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
