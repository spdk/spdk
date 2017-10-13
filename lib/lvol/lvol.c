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

/* Length of string returned from uuid_unparse() */
#define UUID_STRING_LEN 37

SPDK_LOG_REGISTER_TRACE_FLAG("lvol", SPDK_TRACE_LVOL)

static inline size_t
divide_round_up(size_t num, size_t divisor)
{
	return (num + divisor - 1) / divisor;
}

static void
_spdk_super_create_close_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Lvol store init failed: could not close super blob\n");
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		free(lvs);
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
		free(lvs);
		free(req);
		return;
	}

	spdk_bs_md_close_blob(&blob, _spdk_super_create_close_cb, req);
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
		free(lvs);
		free(req);
		return;
	}

	uuid_unparse(lvs->uuid, uuid);

	spdk_blob_md_set_xattr(blob, "uuid", uuid, UUID_STRING_LEN);
	spdk_bs_md_sync_blob(blob, _spdk_super_blob_set_cb, req);
}

static void
_spdk_super_blob_create_open_cb(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct spdk_lvol_store *lvs = req->lvol_store;

	if (lvolerrno < 0) {
		req->cb_fn(req->cb_arg, NULL, lvolerrno);
		SPDK_ERRLOG("Lvol store init failed: could not open super blob\n");
		free(lvs);
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
		free(lvs);
		free(req);
		return;
	}

	bs = req->lvol_store->blobstore;

	spdk_bs_md_open_blob(bs, blobid, _spdk_super_blob_create_open_cb, req);
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
		free(lvs);
		free(lvs_req);
		return;
	}

	assert(bs != NULL);
	lvs->blobstore = bs;
	TAILQ_INIT(&lvs->lvols);
	lvs->total_blocks = (spdk_bs_get_cluster_size(bs) * spdk_bs_free_cluster_count(
				     bs)) / spdk_bs_get_page_size(bs);

	SPDK_INFOLOG(SPDK_TRACE_LVOL, "Lvol store initialized\n");

	/* create super blob */
	spdk_bs_md_create_blob(lvs->blobstore, _spdk_super_blob_create_cb, lvs_req);
}

static void
spdk_setup_lvs_opts(struct spdk_bs_opts *bs_opts, struct spdk_lvs_opts *o)
{
	spdk_bs_opts_init(bs_opts);
	if (o) {
		bs_opts->cluster_sz = o->cluster_sz;
	} else {
		bs_opts->cluster_sz = SPDK_LVS_OPTS_CLUSTER_SZ;
	}
}

int
spdk_lvs_init(struct spdk_bs_dev *bs_dev, struct spdk_lvs_opts *o,
	      spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvs_with_handle_req *lvs_req;
	struct spdk_bs_opts opts;

	if (bs_dev == NULL) {
		SPDK_ERRLOG("Blobstore device does not exist\n");
		return -ENODEV;
	}

	spdk_setup_lvs_opts(&opts, o);

	lvs = calloc(1, sizeof(*lvs));
	if (!lvs) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store base pointer\n");
		return -ENOMEM;
	}

	uuid_generate_time(lvs->uuid);

	lvs_req = calloc(1, sizeof(*lvs_req));
	if (!lvs_req) {
		free(lvs);
		SPDK_ERRLOG("Cannot alloc memory for lvol store request pointer\n");
		return -ENOMEM;
	}

	assert(cb_fn != NULL);
	lvs_req->cb_fn = cb_fn;
	lvs_req->cb_arg = cb_arg;
	lvs_req->lvol_store = lvs;
	lvs->bs_dev = bs_dev;

	SPDK_INFOLOG(SPDK_TRACE_LVOL, "Initializing lvol store\n");
	spdk_bs_init(bs_dev, &opts, _spdk_lvs_init_cb, lvs_req);

	return 0;
}

static void
_lvs_unload_cb(void *cb_arg, int lvserrno)
{
	struct spdk_lvs_req *lvs_req = cb_arg;

	SPDK_INFOLOG(SPDK_TRACE_LVOL, "Lvol store unloaded\n");
	assert(lvs_req->cb_fn != NULL);
	lvs_req->cb_fn(lvs_req->cb_arg, lvserrno);
	free(lvs_req);
}

int
spdk_lvs_unload(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn,
		void *cb_arg)
{
	struct spdk_lvs_req *lvs_req;

	if (lvs == NULL) {
		SPDK_ERRLOG("Lvol store is NULL\n");
		return -ENODEV;
	}

	lvs_req = calloc(1, sizeof(*lvs_req));
	if (!lvs_req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store request pointer\n");
		return -ENOMEM;
	}

	lvs_req->cb_fn = cb_fn;
	lvs_req->cb_arg = cb_arg;

	SPDK_INFOLOG(SPDK_TRACE_LVOL, "Unloading lvol store\n");
	spdk_bs_unload(lvs->blobstore, _lvs_unload_cb, lvs_req);
	free(lvs);

	return 0;
}

static void
_spdk_lvol_return_to_caller(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_with_handle_req *req = cb_arg;

	assert(req->cb_fn != NULL);
	req->cb_fn(req->cb_arg, req->lvol, lvolerrno);
	free(req);
}

static void
_spdk_lvs_destruct_cb(void *cb_arg, int lvserrno)
{
	struct spdk_lvs_req *req = cb_arg;

	SPDK_INFOLOG(SPDK_TRACE_LVOL, "Lvol store bdev deleted\n");

	if (req->cb_fn != NULL)
		req->cb_fn(req->cb_arg, lvserrno);
	free(req);
}

static void
_spdk_lvol_close_blob_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol *lvol = cb_arg;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Could not close blob on lvol\n");
		free(lvol->name);
		free(lvol);
		return;
	}

	TAILQ_REMOVE(&lvol->lvol_store->lvols, lvol, link);

	if (lvol->lvol_store->destruct_req && TAILQ_EMPTY(&lvol->lvol_store->lvols)) {
		spdk_lvs_unload(lvol->lvol_store, _spdk_lvs_destruct_cb, lvol->lvol_store->destruct_req);
	}

	free(lvol->name);
	free(lvol);

	SPDK_INFOLOG(SPDK_TRACE_LVOL, "Blob closed on lvol\n");
}

static void
_spdk_lvol_delete_blob_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol *lvol = cb_arg;
	struct spdk_blob_store *bs = lvol->lvol_store->blobstore;

	if (lvolerrno < 0) {
		SPDK_ERRLOG("Could not delete blob on lvol\n");
		free(lvol->name);
		free(lvol);
		return;
	}
	SPDK_INFOLOG(SPDK_TRACE_LVOL, "Blob closed on lvol\n");
	spdk_bs_md_delete_blob(bs, lvol->blob_id, _spdk_lvol_close_blob_cb, lvol);
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
		goto invalid;
	}

	lvol->blob = blob;
	lvol->blob_id = blob_id;

	uuid_unparse(lvol->lvol_store->uuid, uuid);
	lvol->name = spdk_sprintf_alloc("%s_%"PRIu64, uuid, (uint64_t)blob_id);
	if (!lvol->name) {
		spdk_bs_md_close_blob(&blob, _spdk_lvol_delete_blob_cb, lvol);
		SPDK_ERRLOG("Cannot alloc memory for lvol name\n");
		lvolerrno = -ENOMEM;
		goto invalid;
	}

	lvolerrno = spdk_bs_md_resize_blob(blob, lvol->num_clusters);
	if (lvolerrno < 0) {
		spdk_bs_md_close_blob(&blob, _spdk_lvol_delete_blob_cb, lvol);
		goto invalid;
	}

	TAILQ_INSERT_TAIL(&lvol->lvol_store->lvols, lvol, link);

	spdk_bs_md_sync_blob(blob, _spdk_lvol_return_to_caller, req);

	return;

invalid:
	assert(req->cb_fn != NULL);
	req->cb_fn(req->cb_arg, NULL, lvolerrno);
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

	spdk_bs_md_open_blob(bs, blobid, _spdk_lvol_create_open_cb, req);
}

int
spdk_lvol_create(struct spdk_lvol_store *lvs, uint64_t sz,
		 spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;
	struct spdk_blob_store *bs;
	struct spdk_lvol *lvol;
	uint64_t num_clusters, free_clusters;

	if (lvs == NULL) {
		SPDK_ERRLOG("lvol store does not exist\n");
		return -ENODEV;
	}
	bs = lvs->blobstore;

	num_clusters = divide_round_up(sz, spdk_bs_get_cluster_size(bs));
	free_clusters = spdk_bs_free_cluster_count(bs);
	if (num_clusters > free_clusters) {
		SPDK_ERRLOG("Not enough free clusters left (%zu) on lvol store to add lvol %zu clusters\n",
			    free_clusters, num_clusters);
		return -ENOMEM;
	}

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
	lvol->num_clusters = num_clusters;
	lvol->close_only = false;
	req->lvol = lvol;

	spdk_bs_md_create_blob(lvs->blobstore, _spdk_lvol_create_cb, req);

	return 0;
}

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

	rc = spdk_bs_md_resize_blob(blob, sz);
	if (rc < 0) {
		goto invalid;
	}

	lvol->num_clusters = new_clusters;

	spdk_blob_md_set_xattr(blob, "length", &sz, sizeof(sz));

	spdk_bs_md_sync_blob(blob, _spdk_lvol_resize_cb, req);

	return rc;

invalid:
	req->cb_fn(req->cb_arg, rc);
	free(req);
	return rc;
}

void
spdk_lvol_destroy(struct spdk_lvol *lvol)
{
	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		return;
	}

	TAILQ_REMOVE(&lvol->lvol_store->lvols, lvol, link);
	spdk_bs_md_close_blob(&(lvol->blob), _spdk_lvol_delete_blob_cb, lvol);
}

void
spdk_lvol_close(struct spdk_lvol *lvol)
{
	if (lvol == NULL) {
		SPDK_ERRLOG("lvol does not exist\n");
		return;
	}

	spdk_bs_md_close_blob(&(lvol->blob), _spdk_lvol_close_blob_cb, lvol);
}

struct spdk_io_channel *
spdk_lvol_get_io_channel(struct spdk_lvol *lvol)
{
	return spdk_bs_alloc_io_channel(lvol->lvol_store->blobstore);
}
