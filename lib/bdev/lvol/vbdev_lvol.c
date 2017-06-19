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

#include "vbdev_lvol.h"

static TAILQ_HEAD(, spdk_lvol_store) g_spdk_lvol_stores = TAILQ_HEAD_INITIALIZER(
			g_spdk_lvol_stores);

static void
vbdev_lvs_destruct_cb(void *cb_arg, int lvserrno)
{
	struct vbdev_lvol_store_req *req = cb_arg;
	struct spdk_bdev *base_bdev = req->base_bdev;

	spdk_bdev_unclaim(base_bdev);
	SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL, "lvol store bdev deleted\n");
	req->u.lvs_basic.cb_fn(req->u.lvs_basic.cb_arg, 0);
	free(req);
}

static void
vbdev_lvs_create_cb(void *cb_arg, struct spdk_lvol_store *lvs, int lvserrno)
{
	struct vbdev_lvol_store_req *req = cb_arg;
	struct spdk_bs_dev *bs_dev = req->bs_dev;

	if (lvserrno != 0) {
		assert(lvs == NULL);
		spdk_bdev_unclaim(req->base_bdev);
		bs_dev->destroy(bs_dev);
		goto end;
	}

	assert(lvs != NULL);

	lvs->bs_dev = bs_dev;
	lvs->base_bdev = req->base_bdev;
	TAILQ_INSERT_TAIL(&g_spdk_lvol_stores, lvs, lvol_stores);
	SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL, "lvol store bdev inserted\n");

end:
	req->u.lvs_handle.cb_fn(req->u.lvs_handle.cb_arg, lvs, lvserrno);
	free(req);

	return;
}

int
vbdev_lvs_create(struct spdk_bdev *base_bdev,
		 vbdev_lvs_op_with_handle_complete cb_fn,
		 void *cb_arg)
{
	struct spdk_bs_dev *bs_dev;
	struct vbdev_lvol_store_req *vbdev_ls_req;

	if (!spdk_bdev_claim(base_bdev, NULL, NULL)) {
		SPDK_ERRLOG("Lvol store bdev %s is already claimed\n", spdk_bdev_get_name(base_bdev));
		return -1;
	}

	vbdev_ls_req = calloc(1, sizeof(*vbdev_ls_req));
	if (!vbdev_ls_req) {
		spdk_bdev_unclaim(base_bdev);
		SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
		return -1;
	}

	bs_dev = spdk_bdev_create_bs_dev(base_bdev);
	if (!bs_dev) {
		spdk_bdev_unclaim(base_bdev);
		free(vbdev_ls_req);
		return -1;
	}

	vbdev_ls_req->base_bdev = base_bdev;
	vbdev_ls_req->bs_dev = bs_dev;
	vbdev_ls_req->u.lvs_handle.cb_fn = cb_fn;
	vbdev_ls_req->u.lvs_handle.cb_arg = cb_arg;

	if (spdk_lvs_init(bs_dev, vbdev_lvs_create_cb, vbdev_ls_req)) {
		spdk_bdev_unclaim(base_bdev);
		free(vbdev_ls_req);
		bs_dev->destroy(bs_dev);
		return -1;
	}

	return 0;
}

void
vbdev_lvs_destruct(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn,
		   void *cb_arg)
{

	struct vbdev_lvol_store_req *vbdev_ls_req;

	vbdev_ls_req = calloc(1, sizeof(*vbdev_ls_req));
	if (!vbdev_ls_req) {
		SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
		return;
	}
	vbdev_ls_req->u.lvs_basic.cb_fn = cb_fn;
	vbdev_ls_req->u.lvs_basic.cb_arg = cb_arg;
	vbdev_ls_req->base_bdev = lvs->base_bdev;
	TAILQ_REMOVE(&g_spdk_lvol_stores, lvs, lvol_stores);

	spdk_lvs_unload(lvs, vbdev_lvs_destruct_cb, vbdev_ls_req);
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
	struct spdk_lvol_store *lvs, *tmp;

	TAILQ_FOREACH_SAFE(lvs, &g_spdk_lvol_stores, lvol_stores, tmp) {
		vbdev_lvs_destruct(lvs, vbdev_empty_destroy, NULL);
	}
}

struct spdk_lvol_store *
vbdev_lvol_store_first(void)
{
	struct spdk_lvol_store *lvs;

	lvs = TAILQ_FIRST(&g_spdk_lvol_stores);
	if (lvs) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Starting lvolstore iteration at %p\n", lvs);
	}

	return lvs;
}

struct spdk_lvol_store *
vbdev_lvol_store_next(struct spdk_lvol_store *prev)
{
	struct spdk_lvol_store *lvs;

	lvs = TAILQ_NEXT(prev, lvol_stores);
	if (lvs) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Continuing lvolstore iteration at %p\n", lvs);
	}

	return lvs;
}

struct spdk_lvol_store *
vbdev_get_lvol_store_by_guid(struct spdk_gpt_guid *guid)
{
	struct spdk_lvol_store *lvs = vbdev_lvol_store_first();

	while (lvs != NULL) {
		if (strncmp((char *)lvs->guid.raw, (char *) guid->raw, 16) == 0) {
			return lvs;
		}
		lvs = vbdev_lvol_store_next(lvs);
	}
	return NULL;
}


SPDK_VBDEV_MODULE_REGISTER(vbdev_lvs_init, vbdev_lvs_fini, NULL, NULL, NULL)
SPDK_LOG_REGISTER_TRACE_FLAG("vbdev_lvol", SPDK_TRACE_VBDEV_LVOL)
