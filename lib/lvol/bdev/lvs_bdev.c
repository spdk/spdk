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

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

#include "lvs_bdev.h"

static TAILQ_HEAD(, lvol_store_bdev_pair) g_spdk_lvol_pairs = TAILQ_HEAD_INITIALIZER(
			g_spdk_lvol_pairs);

static void
_vbdev_lvs_create_cb(void *cb_arg, struct spdk_lvol_store *lvs, int lvserrno)
{
	struct spdk_lvol_store_req *req = cb_arg;
	struct spdk_bs_dev *bs_dev = req->u.lvs_handle.bs_dev;
	struct lvol_store_bdev_pair *lvs_pair;
	struct spdk_bdev *bdev = req->u.lvs_handle.base_bdev;

	if (lvserrno != 0) {
		assert(lvs == NULL);
		SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL, "Cannot create lvol store bdev\n");
		bs_dev->destroy(bs_dev);
		goto end;
	}

	assert(lvs != NULL);

	lvs_pair = calloc(1, sizeof(*lvs_pair));
	if (!lvs_pair) {
		bs_dev->destroy(bs_dev);
		lvserrno = -ENOMEM;
		goto end;
	}
	lvs_pair->lvs = lvs;
	lvs_pair->bdev = bdev;

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
	struct spdk_lvol_store_req *lvs_req;
	int rc;

	lvs_req = calloc(1, sizeof(*lvs_req));
	if (!lvs_req) {
		SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
		return -ENOMEM;
	}

	bs_dev = spdk_bdev_create_bs_dev(base_bdev);
	if (!bs_dev) {
		SPDK_ERRLOG("Cannot create blobstore device\n");
		free(lvs_req);
		return -ENODEV;
	}

	lvs_req->u.lvs_handle.bs_dev = bs_dev;
	lvs_req->u.lvs_handle.base_bdev = base_bdev;
	lvs_req->u.lvs_handle.cb_fn = cb_fn;
	lvs_req->u.lvs_handle.cb_arg = cb_arg;

	rc = spdk_lvs_init(bs_dev, _vbdev_lvs_create_cb, lvs_req);
	if (rc < 0) {
		free(lvs_req);
		bs_dev->destroy(bs_dev);
		return rc;
	}

	return 0;
}

static void
_vbdev_lvs_destruct_cb(void *cb_arg, int lvserrno)
{
	struct spdk_lvol_store_req *req = cb_arg;

	SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL, "Lvol store bdev deleted\n");

	if (req->u.lvs_basic.cb_fn != NULL)
		req->u.lvs_basic.cb_fn(req->u.lvs_basic.cb_arg, lvserrno);
	free(req);
}

void
vbdev_lvs_destruct(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn,
		   void *cb_arg)
{

	struct spdk_lvol_store_req *req;
	struct lvol_store_bdev_pair *lvs_pair;
	struct spdk_lvol *lvol, *tmp;

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
		return;
	}
	req->u.lvs_basic.cb_fn = cb_fn;
	req->u.lvs_basic.cb_arg = cb_arg;

	lvs_pair = vbdev_get_lvs_pair_by_lvs(lvs);
	req->u.lvs_basic.base_bdev = lvs_pair->bdev;
	TAILQ_REMOVE(&g_spdk_lvol_pairs, lvs_pair, lvol_stores);

	free(lvs_pair);

	TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
		lvol->close_only = true;
		spdk_bdev_unregister(lvol->bdev);
	}

	spdk_lvs_unload(lvs, _vbdev_lvs_destruct_cb, req);
	return;
}

static void
vbdev_lvs_init(void)
{
	int rc = 0;
	/* TODO: Automatic tasting */
	spdk_vbdev_module_init_next(rc);
}

static void
vbdev_lvs_fini(void)
{
	struct lvol_store_bdev_pair *lvs_pair, *tmp;

	TAILQ_FOREACH_SAFE(lvs_pair, &g_spdk_lvol_pairs, lvol_stores, tmp) {
		vbdev_lvs_destruct(lvs_pair->lvs, NULL, NULL);
	}
}

static struct lvol_store_bdev_pair *
_vbdev_lvol_store_first(void)
{
	struct lvol_store_bdev_pair *lvs_pair;

	lvs_pair = TAILQ_FIRST(&g_spdk_lvol_pairs);
	if (lvs_pair) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Starting lvolstore iteration at %p\n", lvs_pair->lvs);
	}

	return lvs_pair;
}

static struct lvol_store_bdev_pair *
_vbdev_lvol_store_next(struct lvol_store_bdev_pair *prev)
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
	struct lvol_store_bdev_pair *lvs_pair = _vbdev_lvol_store_first();

	while (lvs_pair != NULL) {
		lvs = lvs_pair->lvs;
		if (uuid_compare(lvs->uuid, uuid) == 0) {
			return lvs;
		}
		lvs_pair = _vbdev_lvol_store_next(lvs_pair);
	}
	return NULL;
}

struct lvol_store_bdev_pair *
vbdev_get_lvs_pair_by_lvs(struct spdk_lvol_store *lvs_orig)
{
	struct spdk_lvol_store *lvs = NULL;
	struct lvol_store_bdev_pair *lvs_pair = _vbdev_lvol_store_first();

	while (lvs_pair != NULL) {
		lvs = lvs_pair->lvs;
		if (lvs == lvs_orig) {
			return lvs_pair;
		}
		lvs_pair = _vbdev_lvol_store_next(lvs_pair);
	}
	return NULL;
}

SPDK_VBDEV_MODULE_REGISTER(vbdev_lvs_init, vbdev_lvs_fini, NULL, NULL, NULL)
SPDK_LOG_REGISTER_TRACE_FLAG("vbdev_lvol", SPDK_TRACE_VBDEV_LVOL)
