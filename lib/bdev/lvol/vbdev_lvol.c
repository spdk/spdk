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
#include "lvs_bdev.h"

void
vbdev_lvol_close(struct spdk_lvol *lvol)
{
	lvol->close_only = true;
	spdk_bdev_unregister(lvol->bdev);
}

static int
vbdev_lvol_destruct(void *ctx)
{
	struct spdk_lvol *lvol = ctx;

	assert(lvol != NULL);
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

static struct spdk_bdev *
_create_lvol_disk(struct spdk_lvol *lvol)
{
	struct spdk_bdev *bdev;
	struct lvol_store_bdev_pair *lvs_pair;

	if (!lvol->name) {
		return NULL;
	}

	lvs_pair = vbdev_get_lvs_pair_by_lvs(lvol->lvol_store);
	if (lvs_pair == NULL) {
		SPDK_ERRLOG("No spdk lvs-bdev pair found for lvol %s\n", lvol->name);
		return NULL;
	}

	bdev = calloc(1, sizeof(struct spdk_bdev));
	if (!bdev) {
		perror("disk");
		return NULL;
	}

	bdev->name = lvol->name;
	bdev->product_name = "Logical Volume";
	bdev->write_cache = 1;
	bdev->blocklen = lvs_pair->bdev->blocklen;
	bdev->max_unmap_bdesc_count = lvs_pair->bdev->max_unmap_bdesc_count;
	assert((lvol->sz % bdev->blocklen) == 0);
	bdev->blockcnt = lvol->sz / bdev->blocklen;

	bdev->ctxt = lvol;
	bdev->fn_table = &vbdev_lvol_fn_table;

	spdk_bdev_register(bdev);

	return bdev;
}

static void
_vbdev_lvol_create_cb(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	struct spdk_lvol_store_req *req = cb_arg;
	struct spdk_bdev *bdev = NULL;

	if (lvolerrno < 0) {
		goto invalid;
	}

	bdev = _create_lvol_disk(lvol);
	if (bdev == NULL) {
		lvolerrno = -ENODEV;
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

int
vbdev_lvol_create(uuid_t guid, size_t sz,
		  spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store_req *req;
	struct spdk_lvol_store *lvs;
	int rc;

	lvs = vbdev_get_lvol_store_by_guid(guid);
	if (lvs == NULL) {
		cb_fn(cb_arg, NULL, -1);
		return -ENODEV;
	}

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		cb_fn(cb_arg, NULL, -1);
		return -ENOMEM;
	}
	req->u.lvol_handle.cb_fn = cb_fn;
	req->u.lvol_handle.cb_arg = cb_arg;

	rc = spdk_lvol_create(lvs, sz, _vbdev_lvol_create_cb, req);
	if (rc < 0) {
		return rc;
	}

	return 0;
}

static void
_vbdev_lvol_resize_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_store_req *req = cb_arg;

	req->u.lvol_basic.cb_fn(req->u.lvol_basic.cb_arg,  lvolerrno);
	free(req);
}

int
vbdev_lvol_resize(char *name, size_t sz,
		  spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store_req *req;
	struct spdk_bdev *bdev;
	struct spdk_lvol *lvol;
	struct spdk_lvol_store *lvs;
	uint64_t cluster_size;
	int rc;

	bdev = spdk_bdev_get_by_name(name);
	if (bdev == NULL) {
		SPDK_ERRLOG("lvol '%s' does not exist\n", name);
		return -1;
	}

	lvol = (struct spdk_lvol *)bdev->ctxt;
	lvs = lvol->lvol_store;
	cluster_size = spdk_bs_get_cluster_size(lvs->blobstore);

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		cb_fn(cb_arg, -1);
		return -ENOMEM;
	}
	req->u.lvol_basic.cb_fn = cb_fn;
	req->u.lvol_basic.cb_arg = cb_arg;

	rc = spdk_lvol_resize(lvol, sz, _vbdev_lvol_resize_cb, req);

	if (rc >= 0) {
		bdev->blockcnt = sz * cluster_size / bdev->blocklen;
	}

	return rc;
}
