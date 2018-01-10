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

SPDK_DECLARE_BDEV_MODULE(lvol);

static TAILQ_HEAD(, lvol_store_bdev) g_spdk_lvol_pairs = TAILQ_HEAD_INITIALIZER(
			g_spdk_lvol_pairs);

struct lvol_store_bdev *
vbdev_get_lvs_bdev_by_lvs(struct spdk_lvol_store *lvs_orig)
{
	struct spdk_lvol_store *lvs = NULL;
	struct lvol_store_bdev *lvs_bdev = vbdev_lvol_store_first();

	while (lvs_bdev != NULL) {
		lvs = lvs_bdev->lvs;
		if (lvs == lvs_orig) {
			if (lvs_bdev->req != NULL) {
				/* We do not allow access to lvs that are being destroyed */
				return NULL;
			} else {
				return lvs_bdev;
			}
		}
		lvs_bdev = vbdev_lvol_store_next(lvs_bdev);
	}

	return NULL;
}

static int
_vbdev_lvol_change_bdev_alias(struct spdk_lvol *lvol, const char *new_lvol_name)
{
	struct spdk_bdev_alias *tmp;
	char *old_alias;
	char *alias;
	int rc;
	int alias_number = 0;

	/* bdev representing lvols have only one alias,
	 * while we changed lvs name earlier, we have to iterate alias list to get one,
	 * and check if there is only one alias */

	TAILQ_FOREACH(tmp, &lvol->bdev->aliases, tailq) {
		if (++alias_number > 1) {
			SPDK_ERRLOG("There is more than 1 alias in bdev %s\n", lvol->bdev->name);
			return -EINVAL;
		}

		old_alias = tmp->alias;
	}

	if (alias_number == 0) {
		SPDK_ERRLOG("There are no aliases in bdev %s\n", lvol->bdev->name);
		return -EINVAL;
	}

	alias = spdk_sprintf_alloc("%s/%s", lvol->lvol_store->name, new_lvol_name);
	if (alias == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for alias\n");
		return -ENOMEM;
	}

	rc = spdk_bdev_alias_add(lvol->bdev, alias);
	if (rc != 0) {
		SPDK_ERRLOG("cannot add alias '%s'\n", alias);
		free(alias);
		return rc;
	}
	free(alias);

	rc = spdk_bdev_alias_del(lvol->bdev, old_alias);
	if (rc != 0) {
		SPDK_ERRLOG("cannot remove alias '%s'\n", old_alias);
		return rc;
	}

	return 0;
}

static struct lvol_store_bdev *
vbdev_get_lvs_bdev_by_bdev(struct spdk_bdev *bdev_orig)
{
	struct lvol_store_bdev *lvs_bdev = vbdev_lvol_store_first();

	while (lvs_bdev != NULL) {
		if (lvs_bdev->bdev == bdev_orig) {
			if (lvs_bdev->req != NULL) {
				/* We do not allow access to lvs that are being destroyed */
				return NULL;
			} else {
				return lvs_bdev;
			}
		}
		lvs_bdev = vbdev_lvol_store_next(lvs_bdev);
	}

	return NULL;
}

static void
vbdev_lvs_hotremove_cb(void *ctx)
{
	struct spdk_bdev *bdev = ctx;
	struct lvol_store_bdev *lvs_bdev;

	lvs_bdev = vbdev_get_lvs_bdev_by_bdev(bdev);
	if (lvs_bdev != NULL) {
		vbdev_lvs_unload(lvs_bdev->lvs, NULL, NULL);
	}
}

static void
_vbdev_lvs_create_cb(void *cb_arg, struct spdk_lvol_store *lvs, int lvserrno)
{
	struct spdk_lvs_with_handle_req *req = cb_arg;
	struct lvol_store_bdev *lvs_bdev;
	struct spdk_bdev *bdev = req->base_bdev;
	struct spdk_bs_dev *bs_dev = req->bs_dev;

	if (lvserrno != 0) {
		assert(lvs == NULL);
		SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "Cannot create lvol store bdev\n");
		goto end;
	}

	lvserrno = spdk_bs_bdev_claim(bs_dev, SPDK_GET_BDEV_MODULE(lvol));
	if (lvserrno != 0) {
		SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "Lvol store base bdev already claimed by another bdev\n");
		req->bs_dev->destroy(req->bs_dev);
		goto end;
	}

	assert(lvs != NULL);

	lvs_bdev = calloc(1, sizeof(*lvs_bdev));
	if (!lvs_bdev) {
		lvserrno = -ENOMEM;
		goto end;
	}
	lvs_bdev->lvs = lvs;
	lvs_bdev->bdev = bdev;
	lvs_bdev->req = NULL;

	TAILQ_INSERT_TAIL(&g_spdk_lvol_pairs, lvs_bdev, lvol_stores);
	SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "Lvol store bdev inserted\n");

end:
	req->cb_fn(req->cb_arg, lvs, lvserrno);
	free(req);

	return;
}

int
vbdev_lvs_create(struct spdk_bdev *base_bdev, const char *name, uint32_t cluster_sz,
		 spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_dev *bs_dev;
	struct spdk_lvs_with_handle_req *lvs_req;
	struct spdk_lvs_opts opts;
	int rc;
	int len;

	if (base_bdev == NULL) {
		SPDK_ERRLOG("Bdev does not exist\n");
		return -ENODEV;
	}

	spdk_lvs_opts_init(&opts);
	if (cluster_sz != 0) {
		opts.cluster_sz = cluster_sz;
	}

	if (name == NULL) {
		SPDK_ERRLOG("missing name param\n");
		return -EINVAL;
	}

	len = strnlen(name, SPDK_LVS_NAME_MAX);

	if (len == 0 || len == SPDK_LVS_NAME_MAX) {
		SPDK_ERRLOG("name must be between 1 and %d characters\n", SPDK_LVS_NAME_MAX - 1);
		return -EINVAL;
	}
	strncpy(opts.name, name, sizeof(opts.name));

	lvs_req = calloc(1, sizeof(*lvs_req));
	if (!lvs_req) {
		SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
		return -ENOMEM;
	}

	bs_dev = spdk_bdev_create_bs_dev(base_bdev, vbdev_lvs_hotremove_cb, base_bdev);
	if (!bs_dev) {
		SPDK_ERRLOG("Cannot create blobstore device\n");
		free(lvs_req);
		return -ENODEV;
	}

	lvs_req->bs_dev = bs_dev;
	lvs_req->base_bdev = base_bdev;
	lvs_req->cb_fn = cb_fn;
	lvs_req->cb_arg = cb_arg;

	rc = spdk_lvs_init(bs_dev, &opts, _vbdev_lvs_create_cb, lvs_req);
	if (rc < 0) {
		free(lvs_req);
		bs_dev->destroy(bs_dev);
		return rc;
	}

	return 0;
}

static void
_vbdev_lvs_remove_cb(void *cb_arg, int lvserrno)
{
	struct lvol_store_bdev *lvs_bdev = cb_arg;
	struct spdk_lvs_req *req = lvs_bdev->req;

	if (lvserrno != 0) {
		SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "Could not remove lvol store bdev\n");
	} else {
		TAILQ_REMOVE(&g_spdk_lvol_pairs, lvs_bdev, lvol_stores);
		free(lvs_bdev);
	}

	if (req->cb_fn != NULL) {
		req->cb_fn(req->cb_arg, lvserrno);
	}
	free(req);
}

static void
_vbdev_lvs_remove(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg,
		  bool destroy)
{
	struct spdk_lvs_req *req;
	struct lvol_store_bdev *lvs_bdev;
	struct spdk_lvol *lvol, *tmp;
	bool all_lvols_closed = true;

	lvs_bdev = vbdev_get_lvs_bdev_by_lvs(lvs);
	if (!lvs_bdev) {
		SPDK_ERRLOG("No such lvol store found\n");
		if (cb_fn != NULL) {
			cb_fn(cb_arg, -ENODEV);
		}
		return;
	}

	req = calloc(1, sizeof(*req));
	if (!req) {
		SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
		if (cb_fn != NULL) {
			cb_fn(cb_arg, -ENOMEM);
		}
		return;
	}

	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;
	lvs_bdev->req = req;

	TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
		if (lvol->ref_count != 0) {
			all_lvols_closed = false;
		}
	}

	if (all_lvols_closed == true) {
		if (destroy) {
			spdk_lvs_destroy(lvs, _vbdev_lvs_remove_cb, lvs_bdev);
		} else {
			spdk_lvs_unload(lvs, _vbdev_lvs_remove_cb, lvs_bdev);
		}
	} else {
		lvs->destruct_req = calloc(1, sizeof(*lvs->destruct_req));
		if (!lvs->destruct_req) {
			SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
			_vbdev_lvs_remove_cb(lvs_bdev, -ENOMEM);
			return;
		}
		lvs->destruct_req->cb_fn = _vbdev_lvs_remove_cb;
		lvs->destruct_req->cb_arg = lvs_bdev;
		lvs->destruct = destroy;
		TAILQ_FOREACH_SAFE(lvol, &lvs->lvols, link, tmp) {
			lvol->close_only = !destroy;
			spdk_bdev_unregister(lvol->bdev, NULL, NULL);
		}
	}
}

void
vbdev_lvs_unload(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg)
{
	_vbdev_lvs_remove(lvs, cb_fn, cb_arg, false);
}

void
vbdev_lvs_destruct(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn, void *cb_arg)
{
	_vbdev_lvs_remove(lvs, cb_fn, cb_arg, true);
}

struct lvol_store_bdev *
vbdev_lvol_store_first(void)
{
	struct lvol_store_bdev *lvs_bdev;

	lvs_bdev = TAILQ_FIRST(&g_spdk_lvol_pairs);
	if (lvs_bdev) {
		SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "Starting lvolstore iteration at %p\n", lvs_bdev->lvs);
	}

	return lvs_bdev;
}

struct lvol_store_bdev *
vbdev_lvol_store_next(struct lvol_store_bdev *prev)
{
	struct lvol_store_bdev *lvs_bdev;

	if (prev == NULL) {
		SPDK_ERRLOG("prev argument cannot be NULL\n");
		return NULL;
	}

	lvs_bdev = TAILQ_NEXT(prev, lvol_stores);
	if (lvs_bdev) {
		SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "Continuing lvolstore iteration at %p\n", lvs_bdev->lvs);
	}

	return lvs_bdev;
}

static struct spdk_lvol_store *
_vbdev_get_lvol_store_by_uuid(uuid_t uuid)
{
	struct spdk_lvol_store *lvs = NULL;
	struct lvol_store_bdev *lvs_bdev = vbdev_lvol_store_first();

	while (lvs_bdev != NULL) {
		lvs = lvs_bdev->lvs;
		if (uuid_compare(lvs->uuid, uuid) == 0) {
			return lvs;
		}
		lvs_bdev = vbdev_lvol_store_next(lvs_bdev);
	}
	return NULL;
}

struct spdk_lvol_store *
vbdev_get_lvol_store_by_uuid(const char *uuid_str)
{
	uuid_t uuid;

	if (uuid_parse(uuid_str, uuid)) {
		return NULL;
	}

	return _vbdev_get_lvol_store_by_uuid(uuid);
}

struct spdk_lvol_store *
vbdev_get_lvol_store_by_name(const char *name)
{
	struct spdk_lvol_store *lvs = NULL;
	struct lvol_store_bdev *lvs_bdev = vbdev_lvol_store_first();

	while (lvs_bdev != NULL) {
		lvs = lvs_bdev->lvs;
		if (strncmp(lvs->name, name, sizeof(lvs->name)) == 0) {
			return lvs;
		}
		lvs_bdev = vbdev_lvol_store_next(lvs_bdev);
	}
	return NULL;
}

static struct spdk_lvol *
vbdev_get_lvol_by_unique_id(const char *name)
{
	struct spdk_lvol *lvol, *tmp_lvol;
	struct lvol_store_bdev *lvs_bdev, *tmp_lvs_bdev;

	TAILQ_FOREACH_SAFE(lvs_bdev, &g_spdk_lvol_pairs, lvol_stores, tmp_lvs_bdev) {
		if (lvs_bdev->req != NULL) {
			continue;
		}
		TAILQ_FOREACH_SAFE(lvol, &lvs_bdev->lvs->lvols, link, tmp_lvol) {
			if (!strcmp(lvol->unique_id, name)) {
				return lvol;
			}
		}
	}

	return NULL;
}

static void
_vbdev_lvol_close_cb(void *cb_arg, int lvserrno)
{
	struct spdk_lvol_store *lvs;

	if (cb_arg == NULL) {
		/*
		 * This close cb is from unload/destruct - so do not continue to check
		 *  the lvol open counts.
		 */
		return;
	}

	lvs = cb_arg;

	if (lvs->lvols_opened >= lvs->lvol_count) {
		SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "Opening lvols finished\n");
		spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(lvol));
	}
}

static void
_vbdev_lvol_destroy_cb(void *cb_arg, int lvserrno)
{
	struct spdk_bdev *bdev = cb_arg;

	SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "Lvol destroyed\n");

	spdk_bdev_unregister_done(bdev, lvserrno);
	free(bdev->name);
	free(bdev);
}

static void
_vbdev_lvol_destroy_after_close_cb(void *cb_arg, int lvserrno)
{
	struct spdk_lvol *lvol = cb_arg;
	struct spdk_bdev *bdev = lvol->bdev;

	if (lvserrno != 0) {
		SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "Could not close Lvol %s\n", lvol->unique_id);
		spdk_bdev_unregister_done(bdev, lvserrno);
		free(bdev->name);
		free(bdev);
		return;
	}

	SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "Lvol %s closed, begin destroying\n", lvol->unique_id);
	spdk_lvol_destroy(lvol, _vbdev_lvol_destroy_cb, bdev);
}

static int
vbdev_lvol_destruct(void *ctx)
{
	struct spdk_lvol *lvol = ctx;
	char *alias;

	assert(lvol != NULL);

	alias = spdk_sprintf_alloc("%s/%s", lvol->lvol_store->name, lvol->name);
	if (alias != NULL) {
		spdk_bdev_alias_del(lvol->bdev, alias);
		free(alias);
	} else {
		SPDK_ERRLOG("Cannot alloc memory for alias\n");
	}

	if (lvol->close_only) {
		free(lvol->bdev->name);
		free(lvol->bdev);
		spdk_lvol_close(lvol, _vbdev_lvol_close_cb, NULL);
	} else {
		spdk_lvol_close(lvol, _vbdev_lvol_destroy_after_close_cb, lvol);
	}

	/* return 1 to indicate we have an operation that must finish asynchronously before the
	 *  lvol is closed
	 */
	return 1;
}

static int
vbdev_lvol_dump_config_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct spdk_lvol *lvol = ctx;
	struct lvol_store_bdev *lvs_bdev;
	struct spdk_bdev *bdev;
	char lvol_store_uuid[UUID_STRING_LEN];

	spdk_json_write_name(w, "lvol");
	spdk_json_write_object_begin(w);

	lvs_bdev = vbdev_get_lvs_bdev_by_lvs(lvol->lvol_store);
	bdev = lvs_bdev->bdev;

	uuid_unparse(lvol->lvol_store->uuid, lvol_store_uuid);
	spdk_json_write_name(w, "lvol_store_uuid");
	spdk_json_write_string(w, lvol_store_uuid);

	spdk_json_write_name(w, "base_bdev");
	spdk_json_write_string(w, spdk_bdev_get_name(bdev));

	spdk_json_write_name(w, "thin_provision");
	spdk_json_write_bool(w, lvol->thin_provision);

	spdk_json_write_object_end(w);

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
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		return true;
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
		if (bserrno == -ENOMEM) {
			task->status = SPDK_BDEV_IO_STATUS_NOMEM;
		} else {
			task->status = SPDK_BDEV_IO_STATUS_FAILED;
		}
	}

	SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "Vbdev processing callback on device %s with type %d\n",
		     bdev_io->bdev->name, bdev_io->type);
	spdk_bdev_io_complete(bdev_io, task->status);
}

static void
lvol_unmap(struct spdk_lvol *lvol, struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	uint64_t start_page, num_pages;
	struct spdk_blob *blob = lvol->blob;
	struct lvol_task *task = (struct lvol_task *)bdev_io->driver_ctx;

	start_page = bdev_io->u.bdev.offset_blocks;
	num_pages = bdev_io->u.bdev.num_blocks;

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;

	SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL,
		     "Vbdev doing unmap at offset %" PRIu64 " using %" PRIu64 " pages on device %s\n", start_page,
		     num_pages, bdev_io->bdev->name);
	spdk_bs_io_unmap_blob(blob, ch, start_page, num_pages, lvol_op_comp, task);
}

static void
lvol_write_zeroes(struct spdk_lvol *lvol, struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	uint64_t start_page, num_pages;
	struct spdk_blob *blob = lvol->blob;
	struct lvol_task *task = (struct lvol_task *)bdev_io->driver_ctx;

	start_page = bdev_io->u.bdev.offset_blocks;
	num_pages = bdev_io->u.bdev.num_blocks;

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;

	SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL,
		     "Vbdev doing write zeros at offset %" PRIu64 " using %" PRIu64 " pages on device %s\n", start_page,
		     num_pages, bdev_io->bdev->name);
	spdk_bs_io_write_zeroes_blob(blob, ch, start_page, num_pages, lvol_op_comp, task);
}

static void
lvol_read(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	uint64_t start_page, num_pages;
	struct spdk_lvol *lvol = bdev_io->bdev->ctxt;
	struct spdk_blob *blob = lvol->blob;
	struct lvol_task *task = (struct lvol_task *)bdev_io->driver_ctx;

	start_page = bdev_io->u.bdev.offset_blocks;
	num_pages = bdev_io->u.bdev.num_blocks;

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;

	SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL,
		     "Vbdev doing read at offset %" PRIu64 " using %" PRIu64 " pages on device %s\n", start_page,
		     num_pages, bdev_io->bdev->name);
	spdk_bs_io_readv_blob(blob, ch, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, start_page,
			      num_pages,
			      lvol_op_comp, task);
}

static void
lvol_write(struct spdk_lvol *lvol, struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	uint64_t start_page, num_pages;
	struct spdk_blob *blob = lvol->blob;
	struct lvol_task *task = (struct lvol_task *)bdev_io->driver_ctx;

	start_page = bdev_io->u.bdev.offset_blocks;
	num_pages = bdev_io->u.bdev.num_blocks;

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;

	SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL,
		     "Vbdev doing write at offset %" PRIu64 " using %" PRIu64 " pages on device %s\n", start_page,
		     num_pages, bdev_io->bdev->name);
	spdk_bs_io_writev_blob(blob, ch, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, start_page,
			       num_pages, lvol_op_comp, task);
}

static int
lvol_reset(struct spdk_bdev_io *bdev_io)
{
	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);

	return 0;
}

static void
vbdev_lvol_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct spdk_lvol *lvol = bdev_io->bdev->ctxt;

	SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "Vbdev request type %d submitted\n", bdev_io->type);

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, lvol_read,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		lvol_write(lvol, ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		lvol_reset(bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		lvol_unmap(lvol, ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		lvol_write_zeroes(lvol, ch, bdev_io);
		break;
	default:
		SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "lvol: unsupported I/O type %d\n", bdev_io->type);
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
	uint64_t total_size;
	unsigned char *alias;
	int rc;

	if (!lvol->unique_id) {
		return NULL;
	}

	lvs_bdev = vbdev_get_lvs_bdev_by_lvs(lvol->lvol_store);
	if (lvs_bdev == NULL) {
		SPDK_ERRLOG("No spdk lvs-bdev pair found for lvol %s\n", lvol->unique_id);
		return NULL;
	}

	bdev = calloc(1, sizeof(struct spdk_bdev));
	if (!bdev) {
		SPDK_ERRLOG("Cannot alloc memory for lvol bdev\n");
		return NULL;
	}

	bdev->name = strdup(lvol->unique_id);
	if (!bdev->name) {
		SPDK_ERRLOG("Cannot alloc memory for bdev name\n");
		free(bdev);
		return NULL;
	}
	bdev->product_name = "Logical Volume";
	bdev->blocklen = spdk_bs_get_page_size(lvol->lvol_store->blobstore);
	total_size = lvol->num_clusters * spdk_bs_get_cluster_size(lvol->lvol_store->blobstore);
	assert((total_size % bdev->blocklen) == 0);
	bdev->blockcnt = total_size / bdev->blocklen;

	bdev->ctxt = lvol;
	bdev->fn_table = &vbdev_lvol_fn_table;
	bdev->module = SPDK_GET_BDEV_MODULE(lvol);

	rc = spdk_vbdev_register(bdev, &lvs_bdev->bdev, 1);
	if (rc) {
		free(bdev->name);
		free(bdev);
		return NULL;
	}

	alias = spdk_sprintf_alloc("%s/%s", lvs_bdev->lvs->name, lvol->name);
	if (alias == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for alias\n");
		free(bdev->name);
		free(bdev);
		return NULL;
	}

	rc = spdk_bdev_alias_add(bdev, alias);
	if (rc != 0) {
		SPDK_ERRLOG("Cannot add alias to lvol bdev\n");
		free(bdev->name);
		free(bdev);
		free(alias);
		return NULL;
	}
	free(alias);

	return bdev;
}

static void
_vbdev_lvol_create_cb(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	struct spdk_lvol_with_handle_req *req = cb_arg;
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
	req->cb_fn(req->cb_arg, lvol, lvolerrno);
	free(req);
}

int
vbdev_lvol_create(struct spdk_lvol_store *lvs, const char *name, size_t sz,
		  bool thin_provision, spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_with_handle_req *req;
	int rc;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		return -ENOMEM;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	rc = spdk_lvol_create(lvs, name, sz, thin_provision, _vbdev_lvol_create_cb, req);
	if (rc != 0) {
		free(req);
	}

	return rc;
}

static void
_vbdev_lvol_rename_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;

	if (lvolerrno != 0) {
		SPDK_ERRLOG("Renaming lvol failed\n");
	}

	req->cb_fn(req->cb_arg, lvolerrno);
	free(req);
}

void
vbdev_lvol_rename(struct spdk_lvol *lvol, const char *new_lvol_name,
		  spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;
	int rc;

	rc = _vbdev_lvol_change_bdev_alias(lvol, new_lvol_name);
	if (rc != 0) {
		SPDK_ERRLOG("renaming lvol to '%s' does not succeed\n", new_lvol_name);
		cb_fn(cb_arg, rc);
		return;
	}

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	spdk_lvol_rename(lvol, new_lvol_name, _vbdev_lvol_rename_cb, req);
}

static void
_vbdev_lvol_resize_cb(void *cb_arg, int lvolerrno)
{
	struct spdk_lvol_req *req = cb_arg;

	req->cb_fn(req->cb_arg,  lvolerrno);
	free(req);
}

int
vbdev_lvol_resize(char *name, size_t sz,
		  spdk_lvol_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_req *req;
	struct spdk_bdev *bdev;
	struct spdk_lvol *lvol;
	struct spdk_lvol_store *lvs;
	uint64_t cluster_size;
	int rc;

	lvol = vbdev_get_lvol_by_unique_id(name);
	if (lvol == NULL) {
		SPDK_ERRLOG("lvol '%s' does not exist\n", name);
		return -ENODEV;
	}

	bdev = spdk_bdev_get_by_name(name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev '%s' does not exist\n", name);
		return -ENODEV;
	}

	lvs = lvol->lvol_store;
	cluster_size = spdk_bs_get_cluster_size(lvs->blobstore);

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		cb_fn(cb_arg, -1);
		return -ENOMEM;
	}
	req->cb_fn = cb_fn;
	req->cb_arg = cb_arg;

	rc = spdk_lvol_resize(lvol, sz, _vbdev_lvol_resize_cb, req);

	if (rc == 0) {
		rc = spdk_bdev_notify_blockcnt_change(bdev, sz * cluster_size / bdev->blocklen);
		if (rc != 0) {
			SPDK_ERRLOG("Could not change num blocks for bdev_lvol.\n");
		}
	}

	return rc;
}

static int
vbdev_lvs_init(void)
{
	return 0;
}

static int
vbdev_lvs_get_ctx_size(void)
{
	return sizeof(struct lvol_task);
}

static void
_vbdev_lvs_examine_finish(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
	struct spdk_lvol_store *lvs = cb_arg;
	struct spdk_bdev *bdev;

	if (lvolerrno != 0) {
		SPDK_ERRLOG("Error opening lvol %s\n", lvol->unique_id);
		TAILQ_REMOVE(&lvs->lvols, lvol, link);
		lvs->lvol_count--;
		free(lvol->unique_id);
		free(lvol);
		goto end;
	}

	bdev = _create_lvol_disk(lvol);
	if (bdev == NULL) {
		SPDK_ERRLOG("Cannot create bdev for lvol %s\n", lvol->unique_id);
		TAILQ_REMOVE(&lvs->lvols, lvol, link);
		lvs->lvol_count--;
		spdk_blob_close(lvol->blob, _vbdev_lvol_close_cb, lvs);
		SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "Opening lvol %s failed\n", lvol->unique_id);
		free(lvol->unique_id);
		free(lvol);
		return;
	}

	lvol->bdev = bdev;
	lvs->lvols_opened++;
	SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "Opening lvol %s succeeded\n", lvol->unique_id);

end:

	if (lvs->lvols_opened >= lvs->lvol_count) {
		SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "Opening lvols finished\n");
		spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(lvol));
	}
}

static void
_vbdev_lvs_examine_cb(void *arg, struct spdk_lvol_store *lvol_store, int lvserrno)
{
	struct lvol_store_bdev *lvs_bdev;
	struct spdk_lvs_with_handle_req *req = (struct spdk_lvs_with_handle_req *)arg;
	struct spdk_lvol *lvol, *tmp;

	if (lvserrno == -EEXIST) {
		SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL,
			     "Name for lvolstore on device %s conflicts with name for already loaded lvs\n",
			     req->base_bdev->name);
		spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(lvol));
		goto end;
	} else if (lvserrno != 0) {
		SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "Lvol store not found on %s\n", req->base_bdev->name);
		spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(lvol));
		goto end;
	}

	lvserrno = spdk_bs_bdev_claim(lvol_store->bs_dev, SPDK_GET_BDEV_MODULE(lvol));
	if (lvserrno != 0) {
		SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "Lvol store base bdev already claimed by another bdev\n");
		lvol_store->bs_dev->destroy(lvol_store->bs_dev);
		spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(lvol));
		goto end;
	}

	lvs_bdev = calloc(1, sizeof(*lvs_bdev));
	if (!lvs_bdev) {
		SPDK_ERRLOG("Cannot alloc memory for lvs_bdev\n");
		spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(lvol));
		goto end;
	}

	lvs_bdev->lvs = lvol_store;
	lvs_bdev->bdev = req->base_bdev;

	TAILQ_INSERT_TAIL(&g_spdk_lvol_pairs, lvs_bdev, lvol_stores);

	SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "Lvol store found on %s - begin parsing\n",
		     req->base_bdev->name);

	lvol_store->lvols_opened = 0;

	if (TAILQ_EMPTY(&lvol_store->lvols)) {
		SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "Lvol store examination done\n");
		spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(lvol));
	} else {
		/* Open all lvols */
		TAILQ_FOREACH_SAFE(lvol, &lvol_store->lvols, link, tmp) {
			spdk_lvol_open(lvol, _vbdev_lvs_examine_finish, lvol_store);
		}
	}

end:
	free(req);
}

static void
vbdev_lvs_examine(struct spdk_bdev *bdev)
{
	struct spdk_bs_dev *bs_dev;
	struct spdk_lvs_with_handle_req *req;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(lvol));
		SPDK_ERRLOG("Cannot alloc memory for vbdev lvol store request pointer\n");
		return;
	}

	bs_dev = spdk_bdev_create_bs_dev(bdev, vbdev_lvs_hotremove_cb, bdev);
	if (!bs_dev) {
		SPDK_INFOLOG(SPDK_LOG_VBDEV_LVOL, "Cannot create bs dev on %s\n", bdev->name);
		spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(lvol));
		free(req);
		return;
	}

	req->base_bdev = bdev;

	spdk_lvs_load(bs_dev, _vbdev_lvs_examine_cb, req);
}

struct spdk_lvol *
vbdev_lvol_get_from_bdev(struct spdk_bdev *bdev)
{
	if (!bdev || bdev->module != SPDK_GET_BDEV_MODULE(lvol)) {
		return NULL;
	}

	if (bdev->ctxt == NULL) {
		SPDK_ERRLOG("No lvol ctx assigned to bdev %s\n", bdev->name);
		return NULL;
	}

	return (struct spdk_lvol *)bdev->ctxt;
}

SPDK_BDEV_MODULE_REGISTER(lvol, vbdev_lvs_init, NULL, NULL, vbdev_lvs_get_ctx_size,
			  vbdev_lvs_examine)
SPDK_LOG_REGISTER_COMPONENT("vbdev_lvol", SPDK_LOG_VBDEV_LVOL);
