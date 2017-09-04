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
#include "spdk/string.h"

#include "vbdev_lvol.h"
#include "lvs_bdev.h"

SPDK_DECLARE_BDEV_MODULE(lvol);

static int
vbdev_lvol_destruct(void *ctx)
{
	struct spdk_lvol *lvol = ctx;

	assert(lvol != NULL);
	free(lvol->bdev);
	if (lvol->close_only) {
		spdk_lvol_close(lvol);
	} else {
		spdk_lvol_destroy(lvol);
	}

	return 0;
}

static int
vbdev_lvol_dump_config_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct spdk_lvol *lvol = ctx;
	struct lvol_store_bdev *lvs_bdev;
	struct spdk_bdev *bdev;

	lvs_bdev = vbdev_get_lvs_bdev_by_lvs(lvol->lvol_store);
	bdev = lvs_bdev->bdev;

	spdk_json_write_name(w, "base_bdev");
	spdk_json_write_string(w, spdk_bdev_get_name(bdev));

	return 0;
}

static struct spdk_io_channel *
vbdev_lvol_get_io_channel(void *ctx)
{
	struct spdk_lvol *lvol = ctx;

	return spdk_lvol_get_io_channel(lvol);
}

static bool
vbdev_lvol_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		return true;
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_NVME_ADMIN:
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	default:
		return false;
	}
}

static void
lvol_op_comp(void *cb_arg, int bserrno)
{
	struct lvol_task *task = cb_arg;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(task);

	if (bserrno != 0) {
		task->status = SPDK_BDEV_IO_STATUS_FAILED;
	}

	SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL, "Vbdev processing callback on device %s with type %d\n",
		      bdev_io->bdev->name, bdev_io->type);
	spdk_bdev_io_complete(bdev_io, task->status);
}

static void
lvol_read(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	uint64_t start_page, num_pages, end_page;
	uint32_t page_size;
	struct spdk_lvol *lvol = bdev_io->bdev->ctxt;
	struct spdk_blob *blob = lvol->blob;
	struct lvol_task *task = (struct lvol_task *)bdev_io->driver_ctx;

	page_size = lvol->lvol_store->page_size;
	start_page = bdev_io->u.read.offset / page_size;
	end_page = (bdev_io->u.read.offset + bdev_io->u.read.len - 1) / page_size;
	num_pages = (end_page - start_page + 1);

	assert((bdev_io->u.read.offset % page_size) == 0);

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;

	SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL,
		      "Vbdev doing read at offset %" PRIu64 " using %" PRIu64 " pages on device %s\n",
		      start_page, num_pages, bdev_io->bdev->name);
	spdk_bs_io_readv_blob(blob, ch, bdev_io->u.read.iovs, bdev_io->u.read.iovcnt, start_page, num_pages,
			      lvol_op_comp, task);
}

static void
lvol_write(struct spdk_lvol *lvol, struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	uint64_t start_page, num_pages, end_page;
	uint32_t page_size;
	struct spdk_blob *blob = lvol->blob;
	struct lvol_task *task = (struct lvol_task *)bdev_io->driver_ctx;

	page_size = lvol->lvol_store->page_size;
	start_page = bdev_io->u.write.offset / page_size;
	end_page = (bdev_io->u.write.offset + bdev_io->u.write.len - 1) / page_size;
	num_pages = (end_page - start_page + 1);

	assert((bdev_io->u.write.offset % page_size) == 0);

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;

	SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL,
		      "Vbdev doing write at offset %" PRIu64 " using %" PRIu64 " pages on device %s\n",
		      start_page, num_pages, bdev_io->bdev->name);
	spdk_bs_io_writev_blob(blob, ch, bdev_io->u.write.iovs, bdev_io->u.write.iovcnt, start_page,
			       num_pages,
			       lvol_op_comp, task);
}

static void
lvol_flush(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct lvol_task *task = (struct lvol_task *)bdev_io->driver_ctx;

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;

	spdk_bs_io_flush_channel(ch, lvol_op_comp, task);
}

static void
vbdev_lvol_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct spdk_lvol *lvol = bdev_io->bdev->ctxt;

	SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL, "Vbdev request type %d submitted\n", bdev_io->type);

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, lvol_read);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		lvol_write(lvol, ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		lvol_flush(ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_RESET:
	default:
		SPDK_ERRLOG("lvol: unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	return;
}

static struct spdk_bdev_fn_table vbdev_lvol_fn_table = {
	.destruct		= vbdev_lvol_destruct,
	.io_type_supported	= vbdev_lvol_io_type_supported,
	.submit_request		= vbdev_lvol_submit_request,
	.get_io_channel		= vbdev_lvol_get_io_channel,
	.dump_config_json	= vbdev_lvol_dump_config_json,
};

static struct spdk_bdev *
_create_lvol_disk(struct spdk_lvol *lvol)
{
	struct spdk_bdev *bdev;
	struct lvol_store_bdev *lvs_bdev;

	if (!lvol->name) {
		return NULL;
	}

	lvs_bdev = vbdev_get_lvs_bdev_by_lvs(lvol->lvol_store);
	if (lvs_bdev == NULL) {
		SPDK_ERRLOG("No spdk lvs-bdev pair found for lvol %s\n", lvol->name);
		return NULL;
	}

	bdev = calloc(1, sizeof(struct spdk_bdev));
	if (!bdev) {
		SPDK_ERRLOG("Cannot alloc memory for lvol bdev\n");
		return NULL;
	}

	bdev->name = lvol->name;
	bdev->product_name = "Logical Volume";
	bdev->write_cache = 1;
	bdev->blocklen = lvol->lvol_store->page_size;
	assert((lvol->sz % bdev->blocklen) == 0);
	bdev->blockcnt = lvol->sz / bdev->blocklen;

	bdev->ctxt = lvol;
	bdev->fn_table = &vbdev_lvol_fn_table;
	bdev->module = SPDK_GET_BDEV_MODULE(lvol);

	spdk_bdev_register(bdev);

	return bdev;
}

static void
_vbdev_lvol_create_cb(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	struct spdk_lvol_store_req *req = cb_arg;
	struct spdk_bdev *bdev = NULL;

	if (lvolerrno < 0) {
		goto end;
	}

	bdev = _create_lvol_disk(lvol);
	if (bdev == NULL) {
		lvolerrno = -ENODEV;
		goto end;
	}
	lvol->bdev = bdev;

end:
	req->u.lvol_handle.cb_fn(req->u.lvol_handle.cb_arg, lvol, lvolerrno);
	free(req);
}

int
vbdev_lvol_create(uuid_t uuid, size_t sz,
		  spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store_req *req;
	struct spdk_lvol_store *lvs;
	int rc;

	lvs = vbdev_get_lvol_store_by_uuid(uuid);
	if (lvs == NULL) {
		return -ENODEV;
	}

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		return -ENOMEM;
	}
	req->u.lvol_handle.cb_fn = cb_fn;
	req->u.lvol_handle.cb_arg = cb_arg;

	rc = spdk_lvol_create(lvs, sz, _vbdev_lvol_create_cb, req);

	return rc;
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

	lvol = vbdev_get_lvol_by_name(name);
	if (lvol == NULL) {
		SPDK_ERRLOG("lvol '%s' does not exist\n", name);
		return -1;
	}

	bdev = spdk_bdev_get_by_name(name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev '%s' does not exist\n", name);
		return -1;
	}

	if (is_bdev_opened(bdev)) {
		SPDK_ERRLOG("bdev '%s' cannot be resized because it is currently opened\n", name);
		return -1;
	}

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

	if (rc == 0) {
		bdev->blockcnt = sz * cluster_size / bdev->blocklen;
	}

	return rc;
}

static void
_spdk_open_lvols_cb(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvol *lvol = cb_arg;
	struct spdk_bdev *bdev;

	if (lvolerrno == 0) {
		lvol->blob = blob;
		bdev = _create_lvol_disk(lvol);
		if (bdev == NULL) {
			SPDK_ERRLOG("Cannot create bdev for lvol\n");
			TAILQ_REMOVE(&lvol->lvol_store->lvols, lvol, link);
			return;
		}
		lvol->bdev = bdev;
		SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL, "Opening lvol %s succeeded\n", lvol->name);
	} else {
		SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL, "Failed to open lvol %s\n", lvol->name);
	}
}

static void
_spdk_open_lvols(struct spdk_lvol_store *lvs)
{
	struct spdk_blob_store *bs = lvs->blobstore;
	struct spdk_lvol *lvol, *tmp;

	TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
		spdk_blob_open(bs, lvol->blob_id, _spdk_open_lvols_cb, lvol);
	}
}

static void
_spdk_load_lvols_cb(void *cb_arg, struct spdk_blob *blob, int lvolerrno)
{
	struct spdk_lvol_store *lvs = cb_arg;
	struct spdk_blob_store *bs = lvs->blobstore;
	struct spdk_lvol *lvol;
	spdk_blob_id blob_id;
	char uuid[UUID_STRING_LEN];

	if (lvolerrno == -ENOENT) {
		/* Finished iterating - open all loaded blobs */
		_spdk_open_lvols(lvs);
		return;
	} else if (lvolerrno < 0) {
		SPDK_ERRLOG("Failed to fetch blob\n");
		return;
	}

	blob_id = spdk_blob_get_id(blob);

	if (blob_id == lvs->super_blob_id) {
		SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL, "found superblob %"PRIu64"\n", (uint64_t)blob_id);
		spdk_blob_iter_next(bs, &blob, _spdk_load_lvols_cb, lvs);
		return;
	}

	lvol = calloc(1, sizeof(*lvol));
	if (!lvol) {
		SPDK_ERRLOG("Cannot alloc memory for lvol base pointer\n");
		return;
	}

	lvol->blob = blob;
	lvol->blob_id = blob_id;
	lvol->lvol_store = lvs;
	lvol->sz = spdk_blob_get_num_clusters(blob) * spdk_bs_get_cluster_size(bs);
	lvol->close_only = false;
	uuid_unparse(lvol->lvol_store->uuid, uuid);
	lvol->name = spdk_sprintf_alloc("%s_%"PRIu64, uuid, (uint64_t)blob_id);
	if (!lvol->name) {
		SPDK_ERRLOG("Cannot assign lvol name\n");
		return;
	}

	TAILQ_INSERT_TAIL(&lvs->lvols, lvol, link);

	SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL, "added lvol %s\n", lvol->name);

	spdk_blob_iter_next(bs, &blob, _spdk_load_lvols_cb, lvs);
}

void
spdk_load_lvols(struct spdk_lvol_store *lvs)
{
	spdk_blob_iter_first(lvs->blobstore, _spdk_load_lvols_cb, (void *)lvs);
}

SPDK_LOG_REGISTER_TRACE_FLAG("vbdev_lvol", SPDK_TRACE_VBDEV_LVOL);
