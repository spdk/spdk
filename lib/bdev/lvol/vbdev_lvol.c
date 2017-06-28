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

#include "spdk/blob_bdev.h"
#include "spdk/rpc.h"
#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

#include "vbdev_lvol.h"

struct lvol_store_bdev_pair {
	struct spdk_lvol_store	*lvs;
	struct spdk_bdev 	*bdev;

	TAILQ_ENTRY(lvol_store_bdev_pair)	lvol_stores;
};

static TAILQ_HEAD(, lvol_store_bdev_pair) g_spdk_lvol_pairs = TAILQ_HEAD_INITIALIZER(
			g_spdk_lvol_pairs);

static int
vbdev_lvol_destruct(void *ctx)
{
	struct spdk_lvol *lvol = ctx;

	free(lvol->bdev);
	spdk_lvol_destroy(lvol);

	return 0;
}

static int
vbdev_lvol_dump_config_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct spdk_lvol *lvol = ctx;
	struct lvol_store_bdev_pair *lvs_pair;
	struct spdk_bdev *bdev;

	lvs_pair = vbdev_get_lvs_pair_by_lvs(lvol->lvol_store);
	bdev = lvs_pair->bdev;

	spdk_json_write_name(w, "base_bdev");
	spdk_json_write_string(w, spdk_bdev_get_name(bdev));

	return 0;
}

static struct spdk_bdev_fn_table vbdev_lvol_fn_table = {
	.destruct		= vbdev_lvol_destruct,
	.io_type_supported	= NULL,
	.submit_request		= NULL,
	.get_io_channel		= NULL,
	.dump_config_json	= vbdev_lvol_dump_config_json,
};

static void
vbdev_lvs_create_cb(void *cb_arg, struct spdk_lvol_store *lvs, int lvserrno)
{
	struct vbdev_lvol_store_req *req = cb_arg;
	struct spdk_bs_dev *bs_dev = req->bs_dev;
	struct lvol_store_bdev_pair *lvs_pair;

	if (lvserrno != 0) {
		assert(lvs == NULL);
		spdk_bdev_unclaim(req->base_bdev);
		SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL, "Cannot create lvol store bdev\n");
		bs_dev->destroy(bs_dev);
		goto end;
	}

	assert(lvs != NULL);

	lvs_pair = calloc(1, sizeof(*lvs_pair));
	lvs_pair->lvs = lvs;
	lvs_pair->bdev = req->base_bdev;

	TAILQ_INSERT_TAIL(&g_spdk_lvol_pairs, lvs_pair, lvol_stores);
	SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL, "Lvol store bdev inserted\n");

end:
	req->u.lvs_handle.cb_fn(req->u.lvs_handle.cb_arg, lvs, lvserrno);
	free(req);

	return;
}

int
vbdev_lvs_create(struct spdk_bdev *base_bdev,
		 spdk_lvs_op_with_handle_complete cb_fn,
		 void *cb_arg)
{
	struct spdk_bs_dev *bs_dev;
	struct vbdev_lvol_store_req *vbdev_lvs_req;

	if (!spdk_bdev_claim(base_bdev, NULL, NULL)) {
		SPDK_ERRLOG("Bdev %s is already claimed\n", spdk_bdev_get_name(base_bdev));
		return -1;
	}

	vbdev_lvs_req = calloc(1, sizeof(*vbdev_lvs_req));
	if (!vbdev_lvs_req) {
		spdk_bdev_unclaim(base_bdev);
		SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
		return -1;
	}

	bs_dev = spdk_bdev_create_bs_dev(base_bdev);
	if (!bs_dev) {
		SPDK_ERRLOG("Cannot create blobstore device\n");
		spdk_bdev_unclaim(base_bdev);
		free(vbdev_lvs_req);
		return -1;
	}

	vbdev_lvs_req->base_bdev = base_bdev;
	vbdev_lvs_req->bs_dev = bs_dev;
	vbdev_lvs_req->u.lvs_handle.cb_fn = cb_fn;
	vbdev_lvs_req->u.lvs_handle.cb_arg = cb_arg;

	if (spdk_lvs_init(bs_dev, vbdev_lvs_create_cb, vbdev_lvs_req)) {
		spdk_bdev_unclaim(base_bdev);
		free(vbdev_lvs_req);
		bs_dev->destroy(bs_dev);
		return -1;
	}

	return 0;
}

static void
vbdev_lvs_destruct_cb(void *cb_arg, int lvserrno)
{
	struct vbdev_lvol_store_req *req = cb_arg;
	struct spdk_bdev *base_bdev = req->base_bdev;

	spdk_bdev_unclaim(base_bdev);
	SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL, "Lvol store bdev deleted\n");
	req->u.lvs_basic.cb_fn(req->u.lvs_basic.cb_arg, 0);
	free(req);
}

void
vbdev_lvs_destruct(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn,
		   void *cb_arg)
{

	struct vbdev_lvol_store_req *vbdev_lvs_req;
	struct lvol_store_bdev_pair *lvs_pair;

	vbdev_lvs_req = calloc(1, sizeof(*vbdev_lvs_req));
	if (!vbdev_lvs_req) {
		SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
		return;
	}
	vbdev_lvs_req->u.lvs_basic.cb_fn = cb_fn;
	vbdev_lvs_req->u.lvs_basic.cb_arg = cb_arg;

	lvs_pair = vbdev_get_lvs_pair_by_lvs(lvs);
	vbdev_lvs_req->base_bdev = lvs_pair->bdev;
	TAILQ_REMOVE(&g_spdk_lvol_pairs, lvs_pair, lvol_stores);

	free(lvs_pair);

	spdk_lvs_unload(lvs, vbdev_lvs_destruct_cb, vbdev_lvs_req);
	return;
}

static void
vbdev_lvs_init(void)
{
	int rc = 0;
	/* TODO: Automatic tasting */
	spdk_vbdev_module_init_next(rc);
}

void
vbdev_empty_destroy(void *cb_arg, int lvserrno)
{
	return;
}

static void
vbdev_lvs_fini(void)
{
	struct lvol_store_bdev_pair *lvs_pair, *tmp;

	TAILQ_FOREACH_SAFE(lvs_pair, &g_spdk_lvol_pairs, lvol_stores, tmp) {
		vbdev_lvs_destruct(lvs_pair->lvs, vbdev_empty_destroy, NULL);
	}
}

struct lvol_store_bdev_pair *
vbdev_lvol_store_first(void)
{
	struct lvol_store_bdev_pair *lvs_pair;

	lvs_pair = TAILQ_FIRST(&g_spdk_lvol_pairs);
	if (lvs_pair) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Starting lvolstore iteration at %p\n", lvs_pair->lvs);
	}

	return lvs_pair;
}

struct lvol_store_bdev_pair *
vbdev_lvol_store_next(struct lvol_store_bdev_pair *prev)
{
	struct lvol_store_bdev_pair *lvs_pair;

	lvs_pair = TAILQ_NEXT(prev, lvol_stores);
	if (lvs_pair) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Continuing lvolstore iteration at %p\n", lvs_pair->lvs);
	}

	return lvs_pair;
}

struct spdk_lvol_store *
vbdev_get_lvol_store_by_guid(uuid_t uuid)
{
	struct spdk_lvol_store *lvs = NULL;
	struct lvol_store_bdev_pair *lvs_pair = vbdev_lvol_store_first();

	while (lvs_pair != NULL) {
		lvs = lvs_pair->lvs;
		if (uuid_compare(lvs->uuid, uuid) == 0) {
			return lvs;
		}
		lvs_pair = vbdev_lvol_store_next(lvs_pair);
	}
	return NULL;
}

struct lvol_store_bdev_pair *
vbdev_get_lvs_pair_by_lvs(struct spdk_lvol_store *lvs_orig)
{
	struct spdk_lvol_store *lvs = NULL;
	struct lvol_store_bdev_pair *lvs_pair = vbdev_lvol_store_first();

	while (lvs_pair != NULL) {
		lvs = lvs_pair->lvs;
		if (lvs == lvs_orig) {
			return lvs_pair;
		}
		lvs_pair = vbdev_lvol_store_next(lvs_pair);
	}
	return NULL;
}

struct spdk_bdev *
create_lvol_disk(struct spdk_lvol *lvol)
{
	struct spdk_bdev *bdev;
	struct lvol_store_bdev_pair *lvs_pair;

	if (lvol->sz == 0) {
		SPDK_ERRLOG("Disk must be more than 0 blocks\n");
		return NULL;
	}

	bdev = calloc(1, sizeof(struct spdk_bdev));
	if (!bdev) {
		perror("disk");
		return NULL;
	}

	bdev->name = lvol->name;
	if (!bdev->name) {
		free(bdev);
		return NULL;
	}

	bdev->product_name = "Logical Volume";

	bdev->write_cache = 1;
	lvs_pair = vbdev_get_lvs_pair_by_lvs(lvol->lvol_store);
	bdev->blocklen = lvs_pair->bdev->blocklen;
	bdev->max_unmap_bdesc_count = lvs_pair->bdev->max_unmap_bdesc_count;
	bdev->blockcnt = lvol->sz;

	bdev->ctxt = lvol;
	bdev->fn_table = &vbdev_lvol_fn_table;

	spdk_bdev_register(bdev);

	return bdev;
}

void
vbdev_lvol_create_cb(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	struct vbdev_lvol_store_req *req = cb_arg;
	struct spdk_bdev *bdev = NULL;

	if (lvolerrno < 0) {
		goto invalid;
	}

	bdev = create_lvol_disk(lvol);
	if (bdev == NULL) {
		lvolerrno = -1;
		goto invalid;
	}
	lvol->bdev = bdev;

	req->u.lvol_handle.cb_fn(req->u.lvol_handle.cb_arg, lvol, lvolerrno);

	free(req);

	return;

invalid:
	req->u.lvol_handle.cb_fn(req->u.lvol_handle.cb_arg, NULL, lvolerrno);
	free(req);

}

void
vbdev_lvol_create(uuid_t guid, size_t sz,
		  spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct vbdev_lvol_store_req *req = calloc(1, sizeof(struct vbdev_lvol_store_req));
	struct spdk_lvol_store *lvs;

	lvs = vbdev_get_lvol_store_by_guid(guid);
	if (lvs == NULL) {
		cb_fn(cb_arg, NULL, -1);
		return;
	}

	req->u.lvol_handle.cb_fn = cb_fn;
	req->u.lvol_handle.cb_arg = cb_arg;

	spdk_lvol_create(lvs, sz, vbdev_lvol_create_cb, req);
}

SPDK_VBDEV_MODULE_REGISTER(vbdev_lvs_init, vbdev_lvs_fini, NULL, NULL, NULL)
SPDK_LOG_REGISTER_TRACE_FLAG("vbdev_lvol", SPDK_TRACE_VBDEV_LVOL)
