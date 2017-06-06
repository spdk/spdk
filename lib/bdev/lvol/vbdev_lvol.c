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

/*
 * This is a simple example of a virtual block device that takes a single
 * bdev and slices it into multiple smaller bdevs.
 */


#include "spdk/stdinc.h"

#include "vbdev_lvol.h"
#include "spdk/rpc.h"
#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/io_channel.h"
#include "spdk/lvol.h"
#include "spdk/blob.h"
#include "spdk/blob_bdev.h"
#include "vbdev_lvol.h"

#include "spdk_internal/log.h"

static TAILQ_HEAD(, spdk_lvol_store) g_spdk_lvol_stores = TAILQ_HEAD_INITIALIZER(
			g_spdk_lvol_stores);

/* TODO:
static struct spdk_bdev_fn_table vbdev_lvol_fn_table = {
	.destruct		= vbdev_lvol_destruct,
	.io_type_supported	= vbdev_lvol_io_type_supported,
	.submit_request		= vbdev_lvol_submit_request,
	.get_io_channel		= vbdev_lvol_get_io_channel,
	.dump_config_json	= vbdev_lvol_dump_config_json,
};
 */

static void
vbdev_lvol_store_free_cb(void *cb_arg, int bserrno)
{
	struct vbdev_lvol_store_req *req = cb_arg;
	struct spdk_bdev *base_bdev = req->base_bdev;

	spdk_bdev_unclaim(base_bdev);
	SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL, "lvol store bdev deleted\n");
	req->cb_fn2(req->cb_arg, 0);
	free(req);
}

static void
vbdev_lvol_store_op_complete_cb(void *cb_arg, struct spdk_lvol_store *lvol_store, int bserrno)
{
	struct vbdev_lvol_store_req *req = cb_arg;
	struct spdk_bs_dev *bs_dev = req->bs_dev;

	if (bserrno != 0) {
		assert(lvol_store == NULL);
		spdk_bdev_unclaim(req->base_bdev);
		bs_dev->destroy(bs_dev);
		goto end;
	}

	assert(lvol_store != NULL);

	lvol_store->bs_dev = bs_dev;
	lvol_store->base_bdev = req->base_bdev;
	TAILQ_INSERT_TAIL(&g_spdk_lvol_stores, lvol_store, lvol_stores);
	SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL, "lvol store bdev inserted\n");

end:
	req->cb_fn(req->cb_arg, lvol_store, bserrno);
	free(req);

	return;
}

int
vbdev_construct_lvol_store(struct spdk_bdev *base_bdev,
			   vbdev_lvol_store_op_with_handle_complete cb_fn,
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
	vbdev_ls_req->cb_fn = cb_fn;
	vbdev_ls_req->cb_arg = cb_arg;

	if (lvol_store_initialize(bs_dev, vbdev_lvol_store_op_complete_cb, vbdev_ls_req)) {
		spdk_bdev_unclaim(base_bdev);
		free(vbdev_ls_req);
		bs_dev->destroy(bs_dev);
		return -1;
	}

	return 0;
}

void
vbdev_destruct_lvol_store(struct spdk_lvol_store *lvol_store, vbdev_lvol_store_op_complete cb_fn,
			  void *cb_arg)
{

	struct vbdev_lvol_store_req *vbdev_ls_req;

	vbdev_ls_req = calloc(1, sizeof(*vbdev_ls_req));
	if (!vbdev_ls_req) {
		SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
		return;
	}
	vbdev_ls_req->cb_fn2 = cb_fn;
	vbdev_ls_req->cb_arg = cb_arg;
	vbdev_ls_req->base_bdev = lvol_store->base_bdev;
	TAILQ_REMOVE(&g_spdk_lvol_stores, lvol_store, lvol_stores);

	lvol_store_free(lvol_store, vbdev_lvol_store_free_cb, vbdev_ls_req);
	return;
}

static void
vbdev_lvol_store_init(void)
{
	int rc = 0;
	/* Automatic tasting will be done here */
	/*
		struct spdk_conf_section *sp;
		const char *base_bdev_name;
		int i, rc = 0;
		struct spdk_bdev *base_bdev;



		sp = spdk_conf_find_section(NULL, "Lvol");
		if (sp == NULL) {
			rc = 0;
			goto end;
		}

		for (i = 0; ; i++) {
			if (!spdk_conf_section_get_nval(sp, "Split", i)) {
				break;
			}

			base_bdev_name = spdk_conf_section_get_nmval(sp, "Split", i, 0);
			if (!base_bdev_name) {
				SPDK_ERRLOG("lvol configuration missing blockdev name\n");
				rc = -1;
				goto end;
			}

			base_bdev = spdk_bdev_get_by_name(base_bdev_name);
			if (!base_bdev) {
				SPDK_ERRLOG("Could not find lvol bdev %s\n", base_bdev_name);
				rc = -1;
				goto end;
			}
			vbdev_construct_lvol_store(base_bdev, NULL);
		}

	end:
	 */
	spdk_vbdev_module_init_next(rc);
}

void
vbdev_empty_destroy(void *cb_arg, int bserrno)
{
	return;
}

static void
vbdev_lvol_store_fini(void)
{
	struct spdk_lvol_store *spdk_lvol_store, *tmp;

	TAILQ_FOREACH_SAFE(spdk_lvol_store, &g_spdk_lvol_stores, lvol_stores, tmp) {
		vbdev_destruct_lvol_store(spdk_lvol_store, vbdev_empty_destroy, NULL);
	}
}

struct spdk_lvol_store *
vbdev_lvol_store_first(void)
{
	struct spdk_lvol_store *ls;

	ls = TAILQ_FIRST(&g_spdk_lvol_stores);
	if (ls) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Starting lvolstore iteration at %p\n", ls);
	}

	return ls;
}

struct spdk_lvol_store *
vbdev_lvol_store_next(struct spdk_lvol_store *prev)
{
	struct spdk_lvol_store *ls;

	ls = TAILQ_NEXT(prev, lvol_stores);
	if (ls) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "Continuing lvolstore iteration at %p\n", ls);
	}

	return ls;
}

struct spdk_lvol_store *
vbdev_get_lvol_store_by_guid(struct spdk_lvol_store_guid *guid)
{
	struct spdk_lvol_store *ls = vbdev_lvol_store_first();

	while (ls != NULL) {
		if (memcmp(&ls->guid, guid, sizeof(struct spdk_lvol_store_guid)) == 0) {
			return ls;
		}
		ls = vbdev_lvol_store_next(ls);
	}
	return NULL;
}


struct spdk_bdev *create_lvol_disk(struct spdk_lvol_store *ls, struct spdk_blob *blob, size_t num_blocks)
{

	struct spdk_lvol	*ldisk;

	if (num_blocks == 0) {
		SPDK_ERRLOG("Disk must be more than 0 blocks\n");
		return NULL;
	}

	ldisk = calloc(1, sizeof(struct spdk_lvol));
	ldisk->disk = calloc(1, sizeof(struct spdk_bdev));
	if (!ldisk) {
		perror("ldisk");
		return NULL;
	}

/*
	ldisk->disk.name = spdk_sprintf_alloc("GUID", NULL);
	if (!ldisk->disk->name) {
		lvol_disk_free(ldisk);
		return NULL;
	}
*/
	ldisk->disk->product_name = "Logical Volume";
/*	lvol_disk_count++; */

	ldisk->disk->write_cache = 1;
	ldisk->disk->blocklen = ls->base_bdev->blocklen;
	ldisk->disk->blockcnt = num_blocks;
/*	ldisk->disk->max_unmap_bdesc_count = MALLOC_MAX_UNMAP_BDESC;*/

	ldisk->disk->ctxt = ldisk;
/*	ldisk->disk->fn_table = &lvol_fn_table;*/

/*	ldisk->next = g_malloc_disk_head;
	g_malloc_disk_head = ldisk;
*/
	return ldisk->disk;
}


void
vbdev_lvol_create_cb(void *cb_arg, int bserrno)
{
	struct vbdev_lvol_req *req = (struct vbdev_lvol_req*) cb_arg;
	int rc = 0;
	/*create_lvol_disk(ls, blob, sz); */

	req->cb_fn(req->cb_arg, rc);
}

void
vbdev_lvol_create(struct spdk_lvol_store_guid *guid, size_t sz,
		vbdev_lvol_op_complete cb_fn, void *cb_arg)
{
	struct vbdev_lvol_req *req = calloc(1, sizeof(struct vbdev_lvol_req));
	struct spdk_lvol_store *ls;

	ls = vbdev_get_lvol_store_by_guid(guid);
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	lvol_create_lvol(ls, sz, vbdev_lvol_create_cb, req);

}

     SPDK_VBDEV_MODULE_REGISTER(vbdev_lvol_store_init, vbdev_lvol_store_fini, NULL, NULL, NULL)
     SPDK_LOG_REGISTER_TRACE_FLAG("vbdev_lvol", SPDK_TRACE_VBDEV_LVOL)
