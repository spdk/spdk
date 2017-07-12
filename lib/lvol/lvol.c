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
#include "vbdev_lvol.h"

static void
_lvs_init_cb(void *cb_arg, struct spdk_blob_store *bs, int lvserrno)
{
	struct spdk_lvol_store_req *lvs_req = cb_arg;
	struct spdk_lvol_store *lvs = lvs_req->u.lvs_handle.lvol_store;

	if (lvserrno != 0) {
		assert(bs == NULL);
		SPDK_ERRLOG("Lvol store init failed: could not initialize blobstore\n");
		free(lvs);
		lvs = NULL;
	} else {
		assert(bs != NULL);
		lvs->blobstore = bs;
		TAILQ_INIT(&lvs->lvols);

		SPDK_TRACELOG(SPDK_TRACE_LVOL, "Lvol store initialized\n");
	}
	lvs_req->u.lvs_handle.cb_fn(lvs_req->u.lvs_handle.cb_arg, lvs, lvserrno);
	free(lvs_req);
}

int
spdk_lvs_init(struct spdk_bs_dev *bs_dev, spdk_lvs_op_with_handle_complete cb_fn,
	      void *cb_arg)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvol_store_req *lvs_req;

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

	lvs_req->u.lvs_handle.cb_fn = cb_fn;
	lvs_req->u.lvs_handle.cb_arg = cb_arg;
	lvs_req->u.lvs_handle.lvol_store = lvs;
	lvs->bs_dev = bs_dev;

	SPDK_TRACELOG(SPDK_TRACE_LVOL, "Initializing lvol store\n");
	spdk_bs_init(bs_dev, NULL, _lvs_init_cb, lvs_req);

	return 0;
}

static void
_lvs_unload_cb(void *cb_arg, int lvserrno)
{
	struct spdk_lvol_store_req *lvs_req = cb_arg;

	SPDK_TRACELOG(SPDK_TRACE_LVOL, "Lvol store unloaded\n");
	lvs_req->u.lvs_basic.cb_fn(lvs_req->u.lvs_basic.cb_arg, lvserrno);
	free(lvs_req);
}

void
spdk_lvs_remove_own_lvols(struct spdk_lvol_store *lvs)
{
	struct spdk_lvol *lvol, *tmp;

	TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
		vbdev_lvol_close(lvol);
	}
}

int
spdk_lvs_unload(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn,
		void *cb_arg)
{
	struct spdk_lvol_store_req *lvs_req;

	if (lvs == NULL) {
		SPDK_ERRLOG("Lvol store is NULL\n");
		return -ENODEV;
	}

	lvs_req = calloc(1, sizeof(*lvs_req));
	if (!lvs_req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store request pointer\n");
		return -ENOMEM;
	}

	lvs_req->u.lvs_basic.cb_fn = cb_fn;
	lvs_req->u.lvs_basic.cb_arg = cb_arg;

	SPDK_TRACELOG(SPDK_TRACE_LVOL, "Unloading lvol store\n");
	spdk_bs_unload(lvs->blobstore, _lvs_unload_cb, lvs_req);
	free(lvs);

	return 0;
}

static void
_spdk_lvol_return_to_caller(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_store_req *req = cb_arg;
	req->u.lvol_handle.cb_fn(req->u.lvol_handle.cb_arg, req->u.lvol_handle.lvol, lvolerrno);
	free(req);
}

static void
_spdk_lvol_close_blob_cb(void *cb_arg, int lvolerrno)
{
	if (lvolerrno < 0) {
		SPDK_ERRLOG("Could not close blob on lvol\n");
		return;
	}
	SPDK_TRACELOG(SPDK_TRACE_LVOL, "Blob closed on lvol\n");
}

static void
_spdk_lvol_create_open_cb(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvol_store_req *req = cb_arg;
	spdk_blob_id blob_id = spdk_blob_get_id(blob);
	struct spdk_lvol *lvol = req->u.lvol_handle.lvol;
	uint64_t cluster_size = spdk_bs_get_cluster_size(lvol->lvol_store->blobstore);
	uint64_t number_of_clusters = lvol->sz / cluster_size;
	char guid[UUID_STRING_LEN];
	uint64_t length = lvol->sz;

	if (lvolerrno < 0) {
		goto invalid;
	}

	lvol->blob = blob;

	uuid_unparse(lvol->lvol_store->uuid, guid);
	lvol->name = spdk_sprintf_alloc("%s_%lu", guid, (uint64_t)blob_id);


	lvolerrno = spdk_bs_md_resize_blob(blob, number_of_clusters);
	if (lvolerrno < 0) {
		spdk_bs_md_close_blob(&blob, _spdk_lvol_close_blob_cb, lvol);
		spdk_bs_md_delete_blob(lvol->lvol_store->blobstore, blob_id, _spdk_lvol_close_blob_cb, NULL);
		free(lvol->name);
		goto invalid;
	}

	TAILQ_INSERT_TAIL(&lvol->lvol_store->lvols, lvol, link);

	spdk_blob_md_set_xattr(blob, "length", &length, sizeof(length));

	spdk_bs_md_sync_blob(blob, _spdk_lvol_return_to_caller, req);

	return;

invalid:
	free(lvol);
	req->u.lvol_handle.cb_fn(req->u.lvol_handle.cb_arg, NULL, lvolerrno);
	free(req);
	return;

}

static void
_spdk_lvol_create_cb(void *cb_arg, spdk_blob_id blobid, int lvolerrno)
{
	struct spdk_lvol_store_req *req = cb_arg;
	struct spdk_blob_store *bs;

	if (lvolerrno < 0) {
		free(req->u.lvol_handle.lvol);
		req->u.lvol_handle.cb_fn(req->u.lvol_handle.cb_arg, NULL, lvolerrno);
		free(req);
		return;
	}

	bs = req->u.lvol_handle.lvol->lvol_store->blobstore;

	spdk_bs_md_open_blob(bs, blobid, _spdk_lvol_create_open_cb, req);
}

int
spdk_lvol_create(struct spdk_lvol_store *lvs, size_t sz,
		 spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store_req *req;
	struct spdk_lvol *lvol;
	uint64_t free_clusters;

	free_clusters = spdk_bs_free_cluster_count(lvs->blobstore);
	if (sz > free_clusters) {
		SPDK_ERRLOG("Not enough free clusters left on lvol store to add lvol with %zu clusters\n", sz);
		return -ENOMEM;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol request pointer\n");
		return -ENOMEM;
	}
	req->u.lvol_handle.cb_fn = cb_fn;
	req->u.lvol_handle.cb_arg = cb_arg;

	lvol = calloc(1, sizeof(*lvol));
	if (!lvol) {
		free(req);
		SPDK_ERRLOG("Cannot alloc memory for lvol base pointer\n");
		return -ENOMEM;
	}

	lvol->lvol_store = lvs;
	lvol->sz = sz * spdk_bs_get_cluster_size(lvs->blobstore);
	lvol->close_only = false;
	req->u.lvol_handle.lvol = lvol;

	spdk_bs_md_create_blob(lvs->blobstore, _spdk_lvol_create_cb, req);

	return 0;
}

void
spdk_lvol_destroy(struct spdk_lvol *lvol)
{
	struct spdk_blob_store *bs = lvol->lvol_store->blobstore;
	spdk_blob_id blobid = spdk_blob_get_id(lvol->blob);

	spdk_bs_md_close_blob(&(lvol->blob), _spdk_lvol_close_blob_cb, lvol);
	if (lvol->close_only == false) {
		spdk_bs_md_delete_blob(bs, blobid, _spdk_lvol_close_blob_cb, lvol);
	}

	TAILQ_REMOVE(&lvol->lvol_store->lvols, lvol, link);

	free(lvol->name);
	free(lvol);
}

SPDK_LOG_REGISTER_TRACE_FLAG("lvol", SPDK_TRACE_LVOL)
