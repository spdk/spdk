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
#include "spdk_internal/log.h"
#include "spdk/string.h"
#include "spdk/io_channel.h"
#include "spdk/blob_bdev.h"

/* Length of string returned from uuid_unparse() */
#define UUID_STRING_LEN 37

/* Default blob channel opts for lvol */
#define SPDK_LVOL_BLOB_OPTS_CHANNEL_OPS 512

#define LVOL_NAME "name"

SPDK_LOG_REGISTER_COMPONENT("lvol", SPDK_LOG_LVOL)

static TAILQ_HEAD(, spdk_lvol_store) g_lvol_stores = TAILQ_HEAD_INITIALIZER(g_lvol_stores);
static pthread_mutex_t g_lvol_stores_mutex = PTHREAD_MUTEX_INITIALIZER;

static inline size_t
divide_round_up(size_t num, size_t divisor)
{
	return (num + divisor - 1) / divisor;
}

static int
_spdk_add_lvs_to_list(struct spdk_lvol_store *lvs)
{
	struct spdk_lvol_store *tmp;
	bool name_conflict = false;

	pthread_mutex_lock(&g_lvol_stores_mutex);
	TAILQ_FOREACH(tmp, &g_lvol_stores, link) {
		if (!strncmp(lvs->name, tmp->name, SPDK_LVS_NAME_MAX)) {
			name_conflict = true;
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
_spdk_lvs_free(struct spdk_lvol_store *lvs)
{
	if (lvs->on_list) {
		TAILQ_REMOVE(&g_lvol_stores, lvs, link);
	}
	free(lvs);
}

static void
_spdk_lvol_open_cb(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvol_with_handle_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;

	if (lvolerrno != 0) {
		SPDK_INFOLOG(SPDK_LOG_LVOL, "Failed to open lvol %s\n", lvol->unique_id);
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

	spdk_bs_open_blob(lvol->lvol_store->blobstore, lvol->blob_id, _spdk_lvol_open_cb, req);
}

static void
_spdk_bs_unload_with_error_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;

	req->cb_fn(req->cb_arg, NULL, req->lvserrno);
	free(req);
}

static void
_spdk_load_next_lvol(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob_store *bs = lvs->blobstore;
	struct spdk_lvol *lvol, *tmp;
	spdk_blob_id blob_id;
	char uuid[UUID_STRING_LEN];
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
		SPDK_INFOLOG(SPDK_LOG_LVOL, "found superblob %"PRIu64"\n", (uint64_t)blob_id);
		spdk_bs_iter_next(bs, blob, _spdk_load_next_lvol, req);
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
	lvol->num_clusters = spdk_blob_get_num_clusters(blob);
	lvol->close_only = false;
	uuid_unparse(lvol->lvol_store->uuid, uuid);
	lvol->unique_id = spdk_sprintf_alloc("%s_%"PRIu64, uuid, (uint64_t)blob_id);
	if (!lvol->unique_id) {
		SPDK_ERRLOG("Cannot assign lvol name\n");
		free(lvol);
		req->lvserrno = -ENOMEM;
		goto invalid;
	}

	rc = spdk_blob_get_xattr_value(blob, "name", (const void **)&attr, &value_len);
	if (rc != 0 || value_len > SPDK_LVOL_NAME_MAX) {
		SPDK_ERRLOG("Cannot assign lvol name\n");
		free(lvol->unique_id);
		free(lvol);
		req->lvserrno = -EINVAL;
		goto invalid;
	}

	strncpy(lvol->name, attr, SPDK_LVOL_NAME_MAX);

	TAILQ_INSERT_TAIL(&lvs->lvols, lvol, link);

	lvs->lvol_count++;

	SPDK_INFOLOG(SPDK_LOG_LVOL, "added lvol %s\n", lvol->unique_id);

	spdk_bs_iter_next(bs, blob, _spdk_load_next_lvol, req);

	return;

invalid:
	TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
		TAILQ_REMOVE(&lvs->lvols, lvol, link);
		free(lvol->unique_id);
		free(lvol);
	}

	_spdk_lvs_free(lvs);
	spdk_bs_unload(bs, _spdk_bs_unload_with_error_cb, req);
}

static void
_spdk_close_super_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob_store *bs = lvs->blobstore;

	if (lvolerrno != 0) {
		SPDK_INFOLOG(SPDK_LOG_LVOL, "Could not close super blob\n");
		_spdk_lvs_free(lvs);
		req->lvserrno = -ENODEV;
		spdk_bs_unload(bs, _spdk_bs_unload_with_error_cb, req);
		return;
	}

	/* Start loading lvols */
	spdk_bs_iter_first(lvs->blobstore, _spdk_load_next_lvol, req);
}

static void
_spdk_close_super_blob_with_error_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob_store *bs = lvs->blobstore;

	_spdk_lvs_free(lvs);

	spdk_bs_unload(bs, _spdk_bs_unload_with_error_cb, req);
}

static void
_spdk_lvs_read_uuid(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob_store *bs = lvs->blobstore;
	const char *attr;
	size_t value_len;
	int rc;

	if (lvolerrno != 0) {
		SPDK_INFOLOG(SPDK_LOG_LVOL, "Could not open super blob\n");
		_spdk_lvs_free(lvs);
		req->lvserrno = -ENODEV;
		spdk_bs_unload(bs, _spdk_bs_unload_with_error_cb, req);
		return;
	}

	rc = spdk_blob_get_xattr_value(blob, "uuid", (const void **)&attr, &value_len);
	if (rc != 0 || value_len != UUID_STRING_LEN || attr[UUID_STRING_LEN - 1] != '\0') {
		SPDK_INFOLOG(SPDK_LOG_LVOL, "missing or incorrect UUID\n");
		req->lvserrno = -EINVAL;
		spdk_blob_close(blob, _spdk_close_super_blob_with_error_cb, req);
		return;
	}

	if (uuid_parse(attr, lvs->uuid)) {
		SPDK_INFOLOG(SPDK_LOG_LVOL, "incorrect UUID '%s'\n", attr);
		req->lvserrno = -EINVAL;
		spdk_blob_close(blob, _spdk_close_super_blob_with_error_cb, req);
		return;
	}

	rc = spdk_blob_get_xattr_value(blob, "name", (const void **)&attr, &value_len);
	if (rc != 0 || value_len > SPDK_LVS_NAME_MAX) {
		SPDK_INFOLOG(SPDK_LOG_LVOL, "missing or invalid name\n");
		req->lvserrno = -EINVAL;
		spdk_blob_close(blob, _spdk_close_super_blob_with_error_cb, req);
		return;
	}

	strncpy(lvs->name, attr, value_len);

	rc = _spdk_add_lvs_to_list(lvs);
	if (rc) {
		SPDK_INFOLOG(SPDK_LOG_LVOL, "lvolstore with name %s already exists\n", lvs->name);
		req->lvserrno = -EEXIST;
		spdk_blob_close(blob, _spdk_close_super_blob_with_error_cb, req);
		return;
	}

	lvs->super_blob_id = spdk_blob_get_id(blob);

	spdk_blob_close(blob, _spdk_close_super_cb, req);
}

static void
_spdk_lvs_open_super(void *cb_arg, spdk_blob_id blobid, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob_store *bs = lvs->blobstore;

	if (lvolerrno != 0) {
		SPDK_INFOLOG(SPDK_LOG_LVOL, "Super blob not found\n");
		_spdk_lvs_free(lvs);
		req->lvserrno = -ENODEV;
		spdk_bs_unload(bs, _spdk_bs_unload_with_error_cb, req);
		return;
	}

	spdk_bs_open_blob(bs, blobid, _spdk_lvs_read_uuid, req);
}

static void
_spdk_lvs_load_cb(void *cb_arg, struct spdk_blob_store *bs, int lvolerrno)
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
		spdk_bs_unload(bs, _spdk_bs_unload_with_error_cb, req);
		return;
	}

	lvs->blobstore = bs;
	lvs->bs_dev = req->bs_dev;
	TAILQ_INIT(&lvs->lvols);

	req->lvol_store = lvs;

	spdk_bs_get_super(bs, _spdk_lvs_open_super, req);
}

static void
spdk_lvs_bs_opts_init(struct spdk_bs_opts *opts)
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

	spdk_lvs_bs_opts_init(&opts);
	strncpy(opts.bstype.bstype, "LVOLSTORE", SPDK_BLOBSTORE_TYPE_LENGTH);

	spdk_bs_load(bs_dev, &opts, _spdk_lvs_load_cb, req);
}

static void
_spdk_super_create_close_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Lvol store init failed: could not close super blob\n");
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		_spdk_lvs_free(lvs);
		free(req);
		return;
	}

	req->cb_fn(req->cb_arg, lvs, lvolerrno);
	free(req);
}

static void
_spdk_super_blob_set_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob *blob = lvs->super_blob;

	if (lvolerrno < 0) {
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		SPDK_ERRLOG("Lvol store init failed: could not set uuid for super blob\n");
		_spdk_lvs_free(lvs);
		free(req);
		return;
	}

	spdk_blob_close(blob, _spdk_super_create_close_cb, req);
}

static void
_spdk_super_blob_init_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob *blob = lvs->super_blob;
	char uuid[UUID_STRING_LEN];

	if (lvolerrno < 0) {
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		SPDK_ERRLOG("Lvol store init failed: could not set super blob\n");
		_spdk_lvs_free(lvs);
		free(req);
		return;
	}

	uuid_unparse(lvs->uuid, uuid);

	spdk_blob_set_xattr(blob, "uuid", uuid, UUID_STRING_LEN);
	spdk_blob_set_xattr(blob, "name", lvs->name, strnlen(lvs->name, SPDK_LVS_NAME_MAX) + 1);
	spdk_blob_sync_md(blob, _spdk_super_blob_set_cb, req);
}

static void
_spdk_super_blob_create_open_cb(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;

	if (lvolerrno < 0) {
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		SPDK_ERRLOG("Lvol store init failed: could not open super blob\n");
		_spdk_lvs_free(lvs);
		free(req);
		return;
	}

	lvs->super_blob = blob;
	lvs->super_blob_id = spdk_blob_get_id(blob);

	spdk_bs_set_super(lvs->blobstore, lvs->super_blob_id, _spdk_super_blob_init_cb, req);
}

static void
_spdk_super_blob_create_cb(void *cb_arg, spdk_blob_id blobid, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;
	struct spdk_blob_store *bs;

	if (lvolerrno < 0) {
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		SPDK_ERRLOG("Lvol store init failed: could not create super blob\n");
		_spdk_lvs_free(lvs);
		free(req);
		return;
	}

	bs = req->lvol_store->blobstore;

	spdk_bs_open_blob(bs, blobid, _spdk_super_blob_create_open_cb, req);
}

static void
_spdk_lvs_init_cb(void *cb_arg, struct spdk_blob_store *bs, int lvserrno)
{
	struct spdk_lvs_with_handle_req *lvs_req = cb_arg;
	struct spdk_lvol_store *lvs = lvs_req->lvol_store;

	if (lvserrno != 0) {
		assert(bs == NULL);
		lvs_req->cb_fn(lvs_req->cb_arg, NULL, lvserrno);
		SPDK_ERRLOG("Lvol store init failed: could not initialize blobstore\n");
		_spdk_lvs_free(lvs);
		free(lvs_req);
		return;
	}

	assert(bs != NULL);
	lvs->blobstore = bs;
	TAILQ_INIT(&lvs->lvols);

	SPDK_INFOLOG(SPDK_LOG_LVOL, "Lvol store initialized\n");

	/* create super blob */
	spdk_bs_create_blob(lvs->blobstore, _spdk_super_blob_create_cb, lvs_req);
}

void
spdk_lvs_opts_init(struct spdk_lvs_opts *o)
{
	o->cluster_sz = SPDK_LVS_OPTS_CLUSTER_SZ;
	memset(o->name, 0, sizeof(o->name));
}

static void
_spdk_setup_lvs_opts(struct spdk_bs_opts *bs_opts, struct spdk_lvs_opts *o)
{
	assert(o != NULL);
	spdk_lvs_bs_opts_init(bs_opts);
	bs_opts->cluster_sz = o->cluster_sz;
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

	_spdk_setup_lvs_opts(&opts, o);

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

	uuid_generate_time(lvs->uuid);
	strncpy(lvs->name, o->name, SPDK_LVS_NAME_MAX);

	rc = _spdk_add_lvs_to_list(lvs);
	if (rc) {
		SPDK_ERRLOG("lvolstore with name %s already exists\n", lvs->name);
		_spdk_lvs_free(lvs);
		return -EEXIST;
	}

	lvs_req = calloc(1, sizeof(*lvs_req));
	if (!lvs_req) {
		_spdk_lvs_free(lvs);
		SPDK_ERRLOG("Cannot alloc memory for lvol store request pointer\n");
		return -ENOMEM;
	}

	assert(cb_fn != NULL);
	lvs_req->cb_fn = cb_fn;
	lvs_req->cb_arg = cb_arg;
	lvs_req->lvol_store = lvs;
	lvs->bs_dev = bs_dev;
	lvs->destruct = false;

	strncpy(opts.bstype.bstype, "LVOLSTORE", SPDK_BLOBSTORE_TYPE_LENGTH);

	SPDK_INFOLOG(SPDK_LOG_LVOL, "Initializing lvol store\n");
	spdk_bs_init(bs_dev, &opts, _spdk_lvs_init_cb, lvs_req);

	return 0;
}

static void
_lvs_unload_cb(void *cb_arg, int lvserrno)
{
	struct spdk_lvs_req *lvs_req = cb_arg;

	SPDK_INFOLOG(SPDK_LOG_LVOL, "Lvol store unloaded\n");
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
		free(lvol->unique_id);
		free(lvol);
	}

	lvs_req = calloc(1, sizeof(*lvs_req));
	if (!lvs_req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store request pointer\n");
		return -ENOMEM;
	}

	lvs_req->cb_fn = cb_fn;
	lvs_req->cb_arg = cb_arg;

	SPDK_INFOLOG(SPDK_LOG_LVOL, "Unloading lvol store\n");
	spdk_bs_unload(lvs->blobstore, _lvs_unload_cb, lvs_req);
	_spdk_lvs_free(lvs);

	return 0;
}

static void
_spdk_lvs_destruct_cb(void *cb_arg, int lvserrno)
{
	struct spdk_lvs_req *req = cb_arg;

	SPDK_INFOLOG(SPDK_LOG_LVOL, "Lvol store bdev deleted\n");

	if (req->cb_fn != NULL) {
		req->cb_fn(req->cb_arg, lvserrno);
	}
	free(req);
}

static void
_lvs_destroy_cb(void *cb_arg, int lvserrno)
{
	struct spdk_lvs_destroy_req *lvs_req = cb_arg;

	SPDK_INFOLOG(SPDK_LOG_LVOL, "Lvol store destroyed\n");
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

	SPDK_INFOLOG(SPDK_LOG_LVOL, "Destroying lvol store\n");
	spdk_bs_destroy(lvs->blobstore, _lvs_destroy_cb, lvs_req);
	_spdk_lvs_free(lvs);
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
		free(iter_lvol->unique_id);
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

	SPDK_INFOLOG(SPDK_LOG_LVOL, "Deleting super blob\n");
	spdk_bs_delete_blob(lvs->blobstore, lvs->super_blob_id, _lvs_destroy_super_cb, lvs_req);

	return 0;
}

static void
_spdk_lvol_close_blob_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;
	struct spdk_lvol *iter_lvol, *tmp;
	bool all_lvols_closed = true;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Could not close blob on lvol\n");
		free(lvol->unique_id);
		free(lvol);
		goto end;
	}

	lvol->ref_count--;

	TAILQ_FOREACH_SAFE(iter_lvol, &lvol->lvol_store->lvols, link, tmp) {
		if (iter_lvol->ref_count != 0) {
			all_lvols_closed = false;
		}
	}

	lvol->action_in_progress = false;

	SPDK_INFOLOG(SPDK_LOG_LVOL, "Lvol %s closed\n", lvol->unique_id);

	if (lvol->lvol_store->destruct_req && all_lvols_closed == true) {
		if (!lvol->lvol_store->destruct) {
			spdk_lvs_unload(lvol->lvol_store, _spdk_lvs_destruct_cb, lvol->lvol_store->destruct_req);
		}
	}

end:
	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

static void
_spdk_lvol_delete_blob_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Could not delete blob on lvol\n");
		if (lvolerrno == -EBUSY) {
			goto end;
		}
		free(lvol->unique_id);
		free(lvol);
		goto end;
	}

	TAILQ_REMOVE(&lvol->lvol_store->lvols, lvol, link);

	if (lvol->lvol_store->destruct_req && TAILQ_EMPTY(&lvol->lvol_store->lvols)) {
		if (lvol->lvol_store->destruct) {
			spdk_lvs_destroy(lvol->lvol_store, _spdk_lvs_destruct_cb, lvol->lvol_store->destruct_req);
		}
	}

	SPDK_INFOLOG(SPDK_LOG_LVOL, "Lvol %s deleted\n", lvol->unique_id);

	free(lvol->unique_id);
	free(lvol);

end:
	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

static void
_spdk_lvol_destroy_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;
	struct spdk_lvol *lvol = req->lvol;
	struct spdk_blob_store *bs = lvol->lvol_store->blobstore;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Could not close blob on lvol\n");
		free(lvol->unique_id);
		free(lvol);
		req->cb_fn(req->cb_arg, lvolerrno);
		free(req);
		return;
	}
	SPDK_INFOLOG(SPDK_LOG_LVOL, "Blob closed on lvol %s\n", lvol->unique_id);

	spdk_bs_delete_blob(bs, lvol->blob_id, _spdk_lvol_delete_blob_cb, req);
}

static void
_spdk_lvol_create_open_cb(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvol_with_handle_req *req = cb_arg;
	spdk_blob_id blob_id = spdk_blob_get_id(blob);
	struct spdk_lvol *lvol = req->lvol;
	char uuid[UUID_STRING_LEN];

	if (lvolerrno < 0) {
		free(lvol);
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		free(req);
		return;
	}

	lvol->blob = blob;
	lvol->blob_id = blob_id;

	TAILQ_INSERT_TAIL(&lvol->lvol_store->lvols, lvol, link);

	uuid_unparse(lvol->lvol_store->uuid, uuid);
	lvol->unique_id = spdk_sprintf_alloc("%s_%"PRIu64, uuid, (uint64_t)blob_id);
	if (!lvol->unique_id) {
		SPDK_ERRLOG("Cannot alloc memory for lvol name\n");
		spdk_blob_close(blob, _spdk_lvol_destroy_cb, req);
		return;
	}

	lvol->ref_count++;

	assert(req->cb_fn != NULL);
	req->cb_fn(req->cb_arg, req->lvol, lvolerrno);
	free(req);
}

static void
_spdk_lvol_create_cb(void *cb_arg, spdk_blob_id blobid, int lvolerrno)
{
	struct spdk_lvol_with_handle_req *req = cb_arg;
	struct spdk_blob_store *bs;

	if (lvolerrno < 0) {
		free(req->lvol);
		assert(req->cb_fn != NULL);
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		free(req);
		return;
	}

	bs = req->lvol->lvol_store->blobstore;

	spdk_bs_open_blob(bs, blobid, _spdk_lvol_create_open_cb, req);
}

static void
spdk_lvol_get_xattr_value(void *xattr_ctx, const char *name,
			  const void **value, size_t *value_len)
{
	struct spdk_lvol *lvol = xattr_ctx;

	if (!strcmp(LVOL_NAME, name)) {
		*value = lvol->name;
		*value_len = SPDK_LVOL_NAME_MAX;
	}
}

int
spdk_lvol_create(struct spdk_lvol_store *lvs, const char *name, uint64_t sz,
		 bool thin_provision, spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;
	struct spdk_blob_store *bs;
	struct spdk_lvol *lvol, *tmp;
	struct spdk_blob_opts opts;
	uint64_t num_clusters;
	char *xattr_name = LVOL_NAME;

	if (lvs == NULL) {
		SPDK_ERRLOG("lvol store does not exist\n");
		return -ENODEV;
	}

	if (name == NULL || strnlen(name, SPDK_LVS_NAME_MAX) == 0) {
		SPDK_ERRLOG("No name specified.\n");
		return -EINVAL;
	}

	if (strnlen(name, SPDK_LVOL_NAME_MAX) == SPDK_LVOL_NAME_MAX) {
		SPDK_ERRLOG("Name has no null terminator.\n");
		return -EINVAL;
	}

	TAILQ_FOREACH(tmp, &lvs->lvols, link) {
		if (!strncmp(name, tmp->name, SPDK_LVOL_NAME_MAX)) {
			SPDK_ERRLOG("lvol with name %s already exists\n", name);
			return -EINVAL;
		}
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
	num_clusters = divide_round_up(sz, spdk_bs_get_cluster_size(bs));
	lvol->num_clusters = num_clusters;
	lvol->close_only = false;
	lvol->thin_provision = thin_provision;
	strncpy(lvol->name, name, SPDK_LVS_NAME_MAX);
	req->lvol = lvol;

	spdk_blob_opts_init(&opts);
	opts.thin_provision = thin_provision;
	opts.num_clusters = num_clusters;
	opts.xattrs.count = 1;
	opts.xattrs.names = &xattr_name;
	opts.xattrs.ctx = lvol;
	opts.xattrs.get_value = spdk_lvol_get_xattr_value;

	spdk_bs_create_blob_ext(lvs->blobstore, &opts, _spdk_lvol_create_cb, req);

	return 0;
}

static int
_spdk_lvol_check_params(struct spdk_lvol *lvol, const char *name)
{
	struct spdk_lvol *tmp;
	struct spdk_lvol_store *lvs;

	if (lvol == NULL) {
		SPDK_INFOLOG(SPDK_LOG_LVOL, "Lvol not provided.\n");
		return -EINVAL;
	}

	if (name == NULL || strnlen(name, SPDK_LVOL_NAME_MAX) == 0) {
		SPDK_INFOLOG(SPDK_LOG_LVOL, "Snapshot name not provided.\n");
		return -EINVAL;
	}

	if (strnlen(name, SPDK_LVOL_NAME_MAX) == SPDK_LVOL_NAME_MAX) {
		SPDK_ERRLOG("Name has no null terminator.\n");
		return -EINVAL;
	}

	lvs = lvol->lvol_store;
	TAILQ_FOREACH(tmp, &lvs->lvols, link) {
		if (!strncmp(name, tmp->name, SPDK_LVOL_NAME_MAX)) {
			SPDK_ERRLOG("lvol with name %s already exists\n", name);
			return -EEXIST;
		}
	}

	return 0;
}

/* START spdk_lvol_create_snapshot */

void
spdk_lvol_create_snapshot(struct spdk_lvol *orglvol, const char *snapshot_name,
			  spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store *lvs = orglvol->lvol_store;
	struct spdk_lvol *newlvol;
	struct spdk_blob *orgblob = orglvol->blob;
	struct spdk_lvol_with_handle_req *req;
	struct spdk_blob_xattr_opts snapshot_xattrs;
	char *xattr_names = LVOL_NAME;
	int rc;

	rc = _spdk_lvol_check_params(orglvol, snapshot_name);
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


	newlvol->lvol_store = orglvol->lvol_store;
	newlvol->num_clusters = orglvol->num_clusters;
	strncpy(newlvol->name, snapshot_name, SPDK_LVS_NAME_MAX);
	snapshot_xattrs.count = 1;
	snapshot_xattrs.ctx = newlvol;
	snapshot_xattrs.names = &xattr_names;
	snapshot_xattrs.get_value = spdk_lvol_get_xattr_value;
	req->lvol = newlvol;
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_bs_create_snapshot(lvs->blobstore, spdk_blob_get_id(orgblob), &snapshot_xattrs,
				_spdk_lvol_create_cb, req);
}

/* END spdk_lvol_create_snapshot */

/* START spdk_lvol_create_clone */

void
spdk_lvol_create_clone(struct spdk_lvol *orglvol, const char *clone_name,
		       spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol *newlvol;
	struct spdk_lvol_with_handle_req *req;
	struct spdk_lvol_store *lvs = orglvol->lvol_store;
	struct spdk_blob *orgblob = orglvol->blob;
	struct spdk_blob_xattr_opts clone_xattrs;
	char *xattr_names = LVOL_NAME;
	int rc;

	rc = _spdk_lvol_check_params(orglvol, clone_name);
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
	newlvol->num_clusters = orglvol->num_clusters;
	strncpy(newlvol->name, clone_name, SPDK_LVS_NAME_MAX);
	clone_xattrs.count = 1;
	clone_xattrs.ctx = newlvol;
	clone_xattrs.names = &xattr_names;
	clone_xattrs.get_value = spdk_lvol_get_xattr_value;
	req->lvol = newlvol;
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_bs_create_clone(lvs->blobstore, spdk_blob_get_id(orgblob), &clone_xattrs, _spdk_lvol_create_cb,
			     req);
}

/* END spdk_lvol_create_clone */


static void
_spdk_lvol_resize_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;

	req->cb_fn(req->cb_arg,  lvolerrno);
	free(req);
}

int
spdk_lvol_resize(struct spdk_lvol *lvol, uint64_t sz,
		 spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	int rc;
	struct spdk_blob *blob = lvol->blob;
	struct spdk_lvol_store *lvs = lvol->lvol_store;
	struct spdk_lvol_req *req;
	uint64_t free_clusters = spdk_bs_free_cluster_count(lvs->blobstore);
	uint64_t used_clusters = lvol->num_clusters;
	uint64_t new_clusters = divide_round_up(sz, spdk_bs_get_cluster_size(lvs->blobstore));

	/* Check if size of lvol increasing */
	if (new_clusters > used_clusters) {
		/* Check if there is enough clusters left to resize */
		if (new_clusters - used_clusters > free_clusters) {
			SPDK_ERRLOG("Not enough free clusters left on lvol store to resize lvol to %zu clusters\n", sz);
			return -ENOMEM;
		}
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		return -ENOMEM;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	rc = spdk_blob_resize(blob, sz);
	if (rc < 0) {
		goto invalid;
	}

	lvol->num_clusters = new_clusters;

	spdk_blob_set_xattr(blob, "length", &sz, sizeof(sz));

	spdk_blob_sync_md(blob, _spdk_lvol_resize_cb, req);

	return rc;

invalid:
	req->cb_fn(req->cb_arg, rc);
	free(req);
	return rc;
}

static void
_spdk_lvol_rename_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;

	if (lvolerrno != 0) {
		SPDK_ERRLOG("Lvol rename operation failed\n");
	} else {
		strncpy(req->lvol->name, req->name, SPDK_LVOL_NAME_MAX);
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
	strncpy(req->name, new_name, SPDK_LVOL_NAME_MAX);

	rc = spdk_blob_set_xattr(blob, "name", new_name, strlen(new_name) + 1);
	if (rc < 0) {
		free(req);
		cb_fn(cb_arg, rc);
		return;
	}

	spdk_blob_sync_md(blob, _spdk_lvol_rename_cb, req);
}

void
spdk_lvol_destroy(struct spdk_lvol *lvol, spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;
	struct spdk_blob_store *bs = lvol->lvol_store->blobstore;

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

	spdk_bs_delete_blob(bs, lvol->blob_id, _spdk_lvol_delete_blob_cb, req);
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

	spdk_blob_close(lvol->blob, _spdk_lvol_close_blob_cb, req);
}

struct spdk_io_channel *
spdk_lvol_get_io_channel(struct spdk_lvol *lvol)
{
	return spdk_bs_alloc_io_channel(lvol->lvol_store->blobstore);
}
