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

#include <ocf/ocf.h>
#include <ocf/ocf_types.h>
#include <ocf/ocf_mngt.h>

#include "ctx.h"
#include "data.h"
#include "volume.h"
#include "utils.h"
#include "vbdev_ocf.h"

#include "spdk/bdev_module.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/cpuset.h"

static struct spdk_bdev_module ocf_if;

static TAILQ_HEAD(, vbdev_ocf) g_ocf_vbdev_head
	= TAILQ_HEAD_INITIALIZER(g_ocf_vbdev_head);

static TAILQ_HEAD(, examining_bdev) g_ocf_examining_bdevs_head
	= TAILQ_HEAD_INITIALIZER(g_ocf_examining_bdevs_head);

bool g_fini_started = false;

/* Structure for keeping list of bdevs that are claimed but not used yet */
struct examining_bdev {
	struct spdk_bdev           *bdev;
	TAILQ_ENTRY(examining_bdev) tailq;
};

/* Add bdev to list of claimed */
static void
examine_start(struct spdk_bdev *bdev)
{
	struct examining_bdev *entry = malloc(sizeof(*entry));

	assert(entry);
	entry->bdev = bdev;
	TAILQ_INSERT_TAIL(&g_ocf_examining_bdevs_head, entry, tailq);
}

/* Find bdev on list of claimed bdevs, then remove it,
 * if it was the last one on list then report examine done */
static void
examine_done(int status, struct vbdev_ocf *vbdev, void *cb_arg)
{
	struct spdk_bdev *bdev = cb_arg;
	struct examining_bdev *entry, *safe, *found = NULL;

	TAILQ_FOREACH_SAFE(entry, &g_ocf_examining_bdevs_head, tailq, safe) {
		if (entry->bdev == bdev) {
			if (found) {
				goto remove;
			} else {
				found = entry;
			}
		}
	}

	assert(found);
	spdk_bdev_module_examine_done(&ocf_if);

remove:
	TAILQ_REMOVE(&g_ocf_examining_bdevs_head, found, tailq);
	free(found);
}

/* Free allocated strings and structure itself
 * Used at shutdown only */
static void
free_vbdev(struct vbdev_ocf *vbdev)
{
	if (!vbdev) {
		return;
	}

	free(vbdev->name);
	free(vbdev->cache.name);
	free(vbdev->core.name);
	free(vbdev);
}

/* Get existing cache base
 * that is attached to other vbdev */
static struct vbdev_ocf_base *
get_other_cache_base(struct vbdev_ocf_base *base)
{
	struct vbdev_ocf *vbdev;

	TAILQ_FOREACH(vbdev, &g_ocf_vbdev_head, tailq) {
		if (&vbdev->cache == base || !vbdev->cache.attached) {
			continue;
		}
		if (!strcmp(vbdev->cache.name, base->name)) {
			return &vbdev->cache;
		}
	}

	return NULL;
}

static bool
is_ocf_cache_running(struct vbdev_ocf *vbdev)
{
	if (vbdev->cache.attached && vbdev->ocf_cache) {
		return ocf_cache_is_running(vbdev->ocf_cache);
	}
	return false;
}

/* Get existing OCF cache instance
 * that is started by other vbdev */
static ocf_cache_t
get_other_cache_instance(struct vbdev_ocf *vbdev)
{
	struct vbdev_ocf *cmp;

	TAILQ_FOREACH(cmp, &g_ocf_vbdev_head, tailq) {
		if (cmp->state.doing_finish || cmp == vbdev) {
			continue;
		}
		if (strcmp(cmp->cache.name, vbdev->cache.name)) {
			continue;
		}
		if (is_ocf_cache_running(cmp)) {
			return cmp->ocf_cache;
		}
	}

	return NULL;
}

static void
_remove_base_bdev(void *ctx)
{
	struct spdk_bdev_desc *desc = ctx;

	spdk_bdev_close(desc);
}

/* Close and unclaim base bdev */
static void
remove_base_bdev(struct vbdev_ocf_base *base)
{
	if (base->attached) {
		if (base->management_channel) {
			spdk_put_io_channel(base->management_channel);
		}

		spdk_bdev_module_release_bdev(base->bdev);
		/* Close the underlying bdev on its same opened thread. */
		if (base->thread && base->thread != spdk_get_thread()) {
			spdk_thread_send_msg(base->thread, _remove_base_bdev, base->desc);
		} else {
			spdk_bdev_close(base->desc);
		}
		base->attached = false;
	}
}

/* Finish unregister operation */
static void
unregister_finish(struct vbdev_ocf *vbdev)
{
	spdk_bdev_destruct_done(&vbdev->exp_bdev, vbdev->state.stop_status);

	if (vbdev->ocf_cache) {
		ocf_mngt_cache_put(vbdev->ocf_cache);
	}

	if (vbdev->cache_ctx) {
		vbdev_ocf_cache_ctx_put(vbdev->cache_ctx);
	}
	vbdev_ocf_mngt_continue(vbdev, 0);
}

static void
close_core_bdev(struct vbdev_ocf *vbdev)
{
	remove_base_bdev(&vbdev->core);
	vbdev_ocf_mngt_continue(vbdev, 0);
}

static void
remove_core_cmpl(void *priv, int error)
{
	struct vbdev_ocf *vbdev = priv;

	ocf_mngt_cache_unlock(vbdev->ocf_cache);
	vbdev_ocf_mngt_continue(vbdev, error);
}

/* Try to lock cache, then remove core */
static void
remove_core_cache_lock_cmpl(ocf_cache_t cache, void *priv, int error)
{
	struct vbdev_ocf *vbdev = (struct vbdev_ocf *)priv;

	if (error) {
		SPDK_ERRLOG("Error %d, can not lock cache instance %s\n",
			    error, vbdev->name);
		vbdev_ocf_mngt_continue(vbdev, error);
		return;
	}

	ocf_mngt_cache_remove_core(vbdev->ocf_core, remove_core_cmpl, vbdev);
}

/* Detach core base */
static void
detach_core(struct vbdev_ocf *vbdev)
{
	if (is_ocf_cache_running(vbdev)) {
		ocf_mngt_cache_lock(vbdev->ocf_cache, remove_core_cache_lock_cmpl, vbdev);
	} else {
		vbdev_ocf_mngt_continue(vbdev, 0);
	}
}

static void
close_cache_bdev(struct vbdev_ocf *vbdev)
{
	remove_base_bdev(&vbdev->cache);
	vbdev_ocf_mngt_continue(vbdev, 0);
}

/* Detach cache base */
static void
detach_cache(struct vbdev_ocf *vbdev)
{
	vbdev->state.stop_status = vbdev->mngt_ctx.status;

	/* If some other vbdev references this cache bdev,
	 * we detach this only by changing the flag, without actual close */
	if (get_other_cache_base(&vbdev->cache)) {
		vbdev->cache.attached = false;
	}

	vbdev_ocf_mngt_continue(vbdev, 0);
}

static void
stop_vbdev_cmpl(ocf_cache_t cache, void *priv, int error)
{
	struct vbdev_ocf *vbdev = priv;

	vbdev_ocf_queue_put(vbdev->cache_ctx->mngt_queue);
	ocf_mngt_cache_unlock(cache);

	vbdev_ocf_mngt_continue(vbdev, error);
}

/* Try to lock cache, then stop it */
static void
stop_vbdev_cache_lock_cmpl(ocf_cache_t cache, void *priv, int error)
{
	struct vbdev_ocf *vbdev = (struct vbdev_ocf *)priv;

	if (error) {
		SPDK_ERRLOG("Error %d, can not lock cache instance %s\n",
			    error, vbdev->name);
		vbdev_ocf_mngt_continue(vbdev, error);
		return;
	}

	ocf_mngt_cache_stop(vbdev->ocf_cache, stop_vbdev_cmpl, vbdev);
}

/* Stop OCF cache object
 * vbdev_ocf is not operational after this */
static void
stop_vbdev(struct vbdev_ocf *vbdev)
{
	if (!is_ocf_cache_running(vbdev)) {
		vbdev_ocf_mngt_continue(vbdev, 0);
		return;
	}

	if (!g_fini_started && get_other_cache_instance(vbdev)) {
		SPDK_NOTICELOG("Not stopping cache instance '%s'"
			       " because it is referenced by other OCF bdev\n",
			       vbdev->cache.name);
		vbdev_ocf_mngt_continue(vbdev, 0);
		return;
	}

	ocf_mngt_cache_lock(vbdev->ocf_cache, stop_vbdev_cache_lock_cmpl, vbdev);
}

static void
flush_vbdev_cmpl(ocf_cache_t cache, void *priv, int error)
{
	struct vbdev_ocf *vbdev = priv;

	ocf_mngt_cache_unlock(cache);
	vbdev_ocf_mngt_continue(vbdev, error);
}

static void
flush_vbdev_cache_lock_cmpl(ocf_cache_t cache, void *priv, int error)
{
	struct vbdev_ocf *vbdev = (struct vbdev_ocf *)priv;

	if (error) {
		SPDK_ERRLOG("Error %d, can not lock cache instance %s\n",
			    error, vbdev->name);
		vbdev_ocf_mngt_continue(vbdev, error);
		return;
	}

	ocf_mngt_cache_flush(vbdev->ocf_cache, flush_vbdev_cmpl, vbdev);
}

static void
flush_vbdev(struct vbdev_ocf *vbdev)
{
	if (!is_ocf_cache_running(vbdev)) {
		vbdev_ocf_mngt_continue(vbdev, -EINVAL);
		return;
	}

	ocf_mngt_cache_lock(vbdev->ocf_cache, flush_vbdev_cache_lock_cmpl, vbdev);
}

/* Procedures called during dirty unregister */
vbdev_ocf_mngt_fn unregister_path_dirty[] = {
	flush_vbdev,
	stop_vbdev,
	detach_cache,
	close_cache_bdev,
	detach_core,
	close_core_bdev,
	unregister_finish,
	NULL
};

/* Procedures called during clean unregister */
vbdev_ocf_mngt_fn unregister_path_clean[] = {
	flush_vbdev,
	detach_core,
	close_core_bdev,
	stop_vbdev,
	detach_cache,
	close_cache_bdev,
	unregister_finish,
	NULL
};

/* Start asynchronous management operation using unregister_path */
static void
unregister_cb(void *opaque)
{
	struct vbdev_ocf *vbdev = opaque;
	vbdev_ocf_mngt_fn *unregister_path;
	int rc;

	unregister_path = vbdev->state.doing_clean_delete ?
			  unregister_path_clean : unregister_path_dirty;

	rc = vbdev_ocf_mngt_start(vbdev, unregister_path, NULL, NULL);
	if (rc) {
		SPDK_ERRLOG("Unable to unregister OCF bdev: %d\n", rc);
		spdk_bdev_destruct_done(&vbdev->exp_bdev, rc);
	}
}

/* Clean remove case - remove core and then cache, this order
 * will remove instance permanently */
static void
_vbdev_ocf_destruct_clean(struct vbdev_ocf *vbdev)
{
	if (vbdev->core.attached) {
		detach_core(vbdev);
		close_core_bdev(vbdev);
	}

	if (vbdev->cache.attached) {
		detach_cache(vbdev);
		close_cache_bdev(vbdev);
	}
}

/* Dirty shutdown/hot remove case - remove cache and then core, this order
 * will allow us to recover this instance in the future */
static void
_vbdev_ocf_destruct_dirty(struct vbdev_ocf *vbdev)
{
	if (vbdev->cache.attached) {
		detach_cache(vbdev);
		close_cache_bdev(vbdev);
	}

	if (vbdev->core.attached) {
		detach_core(vbdev);
		close_core_bdev(vbdev);
	}
}

/* Unregister io device with callback to unregister_cb
 * This function is called during spdk_bdev_unregister */
static int
vbdev_ocf_destruct(void *opaque)
{
	struct vbdev_ocf *vbdev = opaque;

	if (vbdev->state.doing_finish) {
		return -EALREADY;
	}

	if (vbdev->state.starting && !vbdev->state.started) {
		/* Prevent before detach cache/core during register path of
		  this bdev */
		return -EBUSY;
	}

	vbdev->state.doing_finish = true;

	if (vbdev->state.started) {
		spdk_io_device_unregister(vbdev, unregister_cb);
		/* Return 1 because unregister is delayed */
		return 1;
	}

	if (vbdev->state.doing_clean_delete) {
		_vbdev_ocf_destruct_clean(vbdev);
	} else {
		_vbdev_ocf_destruct_dirty(vbdev);
	}

	return 0;
}

/* Stop OCF cache and unregister SPDK bdev */
int
vbdev_ocf_delete(struct vbdev_ocf *vbdev, void (*cb)(void *, int), void *cb_arg)
{
	int rc = 0;

	if (vbdev->state.started) {
		spdk_bdev_unregister(&vbdev->exp_bdev, cb, cb_arg);
	} else {
		rc = vbdev_ocf_destruct(vbdev);
		if (rc == 0 && cb) {
			cb(cb_arg, 0);
		}
	}

	return rc;
}

/* Remove cores permanently and then stop OCF cache and unregister SPDK bdev */
int
vbdev_ocf_delete_clean(struct vbdev_ocf *vbdev, void (*cb)(void *, int),
		       void *cb_arg)
{
	vbdev->state.doing_clean_delete = true;

	return vbdev_ocf_delete(vbdev, cb, cb_arg);
}


/* If vbdev is online, return its object */
struct vbdev_ocf *
vbdev_ocf_get_by_name(const char *name)
{
	struct vbdev_ocf *vbdev;

	if (name == NULL) {
		assert(false);
		return NULL;
	}

	TAILQ_FOREACH(vbdev, &g_ocf_vbdev_head, tailq) {
		if (vbdev->name == NULL || vbdev->state.doing_finish) {
			continue;
		}
		if (strcmp(vbdev->name, name) == 0) {
			return vbdev;
		}
	}
	return NULL;
}

/* Return matching base if parent vbdev is online */
struct vbdev_ocf_base *
vbdev_ocf_get_base_by_name(const char *name)
{
	struct vbdev_ocf *vbdev;

	if (name == NULL) {
		assert(false);
		return NULL;
	}

	TAILQ_FOREACH(vbdev, &g_ocf_vbdev_head, tailq) {
		if (vbdev->state.doing_finish) {
			continue;
		}

		if (vbdev->cache.name && strcmp(vbdev->cache.name, name) == 0) {
			return &vbdev->cache;
		}
		if (vbdev->core.name && strcmp(vbdev->core.name, name) == 0) {
			return &vbdev->core;
		}
	}
	return NULL;
}

/* Execute fn for each OCF device that is online or waits for base devices */
void
vbdev_ocf_foreach(vbdev_ocf_foreach_fn fn, void *ctx)
{
	struct vbdev_ocf *vbdev;

	assert(fn != NULL);

	TAILQ_FOREACH(vbdev, &g_ocf_vbdev_head, tailq) {
		if (!vbdev->state.doing_finish) {
			fn(vbdev, ctx);
		}
	}
}

/* Called from OCF when SPDK_IO is completed */
static void
vbdev_ocf_io_submit_cb(struct ocf_io *io, int error)
{
	struct spdk_bdev_io *bdev_io = io->priv1;

	if (error == 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else if (error == -ENOMEM) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
	} else {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}

	ocf_io_put(io);
}

/* Configure io parameters and send it to OCF */
static int
io_submit_to_ocf(struct spdk_bdev_io *bdev_io, struct ocf_io *io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_READ:
		ocf_core_submit_io(io);
		return 0;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		ocf_core_submit_flush(io);
		return 0;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		ocf_core_submit_discard(io);
		return 0;
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	default:
		SPDK_ERRLOG("Unsupported IO type: %d\n", bdev_io->type);
		return -EINVAL;
	}
}

/* Submit SPDK-IO to OCF */
static void
io_handle(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_ocf *vbdev = bdev_io->bdev->ctxt;
	struct ocf_io *io = NULL;
	struct bdev_ocf_data *data = NULL;
	struct vbdev_ocf_qctx *qctx = spdk_io_channel_get_ctx(ch);
	uint64_t len = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;
	uint64_t offset = bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen;
	int dir, flags = 0;
	int err;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		dir = OCF_READ;
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		dir = OCF_WRITE;
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		dir = OCF_WRITE;
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		dir = OCF_WRITE;
		break;
	default:
		err = -EINVAL;
		goto fail;
	}

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_FLUSH) {
		flags = OCF_WRITE_FLUSH;
	}

	io = ocf_core_new_io(vbdev->ocf_core, qctx->queue, offset, len, dir, 0, flags);
	if (!io) {
		err = -ENOMEM;
		goto fail;
	}

	data = vbdev_ocf_data_from_spdk_io(bdev_io);
	if (!data) {
		err = -ENOMEM;
		goto fail;
	}

	err = ocf_io_set_data(io, data, 0);
	if (err) {
		goto fail;
	}

	ocf_io_set_cmpl(io, bdev_io, NULL, vbdev_ocf_io_submit_cb);

	err = io_submit_to_ocf(bdev_io, io);
	if (err) {
		goto fail;
	}

	return;

fail:
	if (io) {
		ocf_io_put(io);
	}

	if (err == -ENOMEM) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
	} else {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
vbdev_ocf_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		     bool success)
{
	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	io_handle(ch, bdev_io);
}

/* Called from bdev layer when an io to Cache vbdev is submitted */
static void
vbdev_ocf_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		/* User does not have to allocate io vectors for the request,
		 * so in case they are not allocated, we allocate them here */
		spdk_bdev_io_get_buf(bdev_io, vbdev_ocf_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		io_handle(ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	default:
		SPDK_ERRLOG("Unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

/* Called from bdev layer */
static bool
vbdev_ocf_io_type_supported(void *opaque, enum spdk_bdev_io_type io_type)
{
	struct vbdev_ocf *vbdev = opaque;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		return spdk_bdev_io_type_supported(vbdev->core.bdev, io_type);
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	default:
		return false;
	}
}

/* Called from bdev layer */
static struct spdk_io_channel *
vbdev_ocf_get_io_channel(void *opaque)
{
	struct vbdev_ocf *bdev = opaque;

	return spdk_get_io_channel(bdev);
}

static int
vbdev_ocf_dump_info_json(void *opaque, struct spdk_json_write_ctx *w)
{
	struct vbdev_ocf *vbdev = opaque;

	spdk_json_write_named_string(w, "cache_device", vbdev->cache.name);
	spdk_json_write_named_string(w, "core_device", vbdev->core.name);

	spdk_json_write_named_string(w, "mode",
				     ocf_get_cache_modename(ocf_cache_get_mode(vbdev->ocf_cache)));
	spdk_json_write_named_uint32(w, "cache_line_size",
				     ocf_get_cache_line_size(vbdev->ocf_cache));
	spdk_json_write_named_bool(w, "metadata_volatile",
				   vbdev->cfg.cache.metadata_volatile);

	return 0;
}

static void
vbdev_ocf_write_json_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct vbdev_ocf *vbdev = bdev->ctxt;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_ocf_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", vbdev->name);
	spdk_json_write_named_string(w, "mode",
				     ocf_get_cache_modename(ocf_cache_get_mode(vbdev->ocf_cache)));
	spdk_json_write_named_uint32(w, "cache_line_size",
				     ocf_get_cache_line_size(vbdev->ocf_cache));
	spdk_json_write_named_string(w, "cache_bdev_name", vbdev->cache.name);
	spdk_json_write_named_string(w, "core_bdev_name", vbdev->core.name);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

/* Cache vbdev function table
 * Used by bdev layer */
static struct spdk_bdev_fn_table cache_dev_fn_table = {
	.destruct = vbdev_ocf_destruct,
	.io_type_supported = vbdev_ocf_io_type_supported,
	.submit_request	= vbdev_ocf_submit_request,
	.get_io_channel	= vbdev_ocf_get_io_channel,
	.write_config_json = vbdev_ocf_write_json_config,
	.dump_info_json = vbdev_ocf_dump_info_json,
};

/* Poller function for the OCF queue
 * We execute OCF requests here synchronously */
static int
queue_poll(void *opaque)
{
	struct vbdev_ocf_qctx *qctx = opaque;
	uint32_t iono = ocf_queue_pending_io(qctx->queue);
	int i, max = spdk_min(32, iono);

	for (i = 0; i < max; i++) {
		ocf_queue_run_single(qctx->queue);
	}

	if (iono > 0) {
		return SPDK_POLLER_BUSY;
	} else {
		return SPDK_POLLER_IDLE;
	}
}

/* Called during ocf_submit_io, ocf_purge*
 * and any other requests that need to submit io */
static void
vbdev_ocf_ctx_queue_kick(ocf_queue_t q)
{
}

/* OCF queue deinitialization
 * Called at ocf_cache_stop */
static void
vbdev_ocf_ctx_queue_stop(ocf_queue_t q)
{
	struct vbdev_ocf_qctx *qctx = ocf_queue_get_priv(q);

	if (qctx) {
		spdk_put_io_channel(qctx->cache_ch);
		spdk_put_io_channel(qctx->core_ch);
		spdk_poller_unregister(&qctx->poller);
		if (qctx->allocated) {
			free(qctx);
		}
	}
}

/* Queue ops is an interface for running queue thread
 * stop() operation in called just before queue gets destroyed */
const struct ocf_queue_ops queue_ops = {
	.kick_sync = vbdev_ocf_ctx_queue_kick,
	.kick = vbdev_ocf_ctx_queue_kick,
	.stop = vbdev_ocf_ctx_queue_stop,
};

/* Called on cache vbdev creation at every thread
 * We allocate OCF queues here and SPDK poller for it */
static int
io_device_create_cb(void *io_device, void *ctx_buf)
{
	struct vbdev_ocf *vbdev = io_device;
	struct vbdev_ocf_qctx *qctx = ctx_buf;
	int rc;

	rc = vbdev_ocf_queue_create(vbdev->ocf_cache, &qctx->queue, &queue_ops);
	if (rc) {
		return rc;
	}

	ocf_queue_set_priv(qctx->queue, qctx);

	qctx->vbdev      = vbdev;
	qctx->cache_ch   = spdk_bdev_get_io_channel(vbdev->cache.desc);
	qctx->core_ch    = spdk_bdev_get_io_channel(vbdev->core.desc);
	qctx->poller     = SPDK_POLLER_REGISTER(queue_poll, qctx, 0);

	return rc;
}

/* Called per thread
 * Put OCF queue and relaunch poller with new context to finish pending requests */
static void
io_device_destroy_cb(void *io_device, void *ctx_buf)
{
	/* Making a copy of context to use it after io channel will be destroyed */
	struct vbdev_ocf_qctx *copy = malloc(sizeof(*copy));
	struct vbdev_ocf_qctx *qctx = ctx_buf;

	if (copy) {
		ocf_queue_set_priv(qctx->queue, copy);
		memcpy(copy, qctx, sizeof(*copy));
		spdk_poller_unregister(&qctx->poller);
		copy->poller = SPDK_POLLER_REGISTER(queue_poll, copy, 0);
		copy->allocated = true;
	} else {
		SPDK_ERRLOG("Unable to stop OCF queue properly: %s\n",
			    spdk_strerror(ENOMEM));
	}

	vbdev_ocf_queue_put(qctx->queue);
}

/* OCF management queue deinitialization */
static void
vbdev_ocf_ctx_mngt_queue_stop(ocf_queue_t q)
{
	struct spdk_poller *poller = ocf_queue_get_priv(q);

	if (poller) {
		spdk_poller_unregister(&poller);
	}
}

static int
mngt_queue_poll(void *opaque)
{
	ocf_queue_t q = opaque;
	uint32_t iono = ocf_queue_pending_io(q);
	int i, max = spdk_min(32, iono);

	for (i = 0; i < max; i++) {
		ocf_queue_run_single(q);
	}

	if (iono > 0) {
		return SPDK_POLLER_BUSY;
	} else {
		return SPDK_POLLER_IDLE;
	}
}

static void
vbdev_ocf_ctx_mngt_queue_kick(ocf_queue_t q)
{
}

/* Queue ops is an interface for running queue thread
 * stop() operation in called just before queue gets destroyed */
const struct ocf_queue_ops mngt_queue_ops = {
	.kick_sync = NULL,
	.kick = vbdev_ocf_ctx_mngt_queue_kick,
	.stop = vbdev_ocf_ctx_mngt_queue_stop,
};

static void
vbdev_ocf_mngt_exit(struct vbdev_ocf *vbdev, vbdev_ocf_mngt_fn *rollback_path, int rc)
{
	vbdev->state.starting = false;
	vbdev_ocf_mngt_stop(vbdev, rollback_path, rc);
}

/* Create exported spdk object */
static void
finish_register(struct vbdev_ocf *vbdev)
{
	int result;

	/* Copy properties of the base bdev */
	vbdev->exp_bdev.blocklen = vbdev->core.bdev->blocklen;
	vbdev->exp_bdev.write_cache = vbdev->core.bdev->write_cache;
	vbdev->exp_bdev.required_alignment = vbdev->core.bdev->required_alignment;

	vbdev->exp_bdev.name = vbdev->name;
	vbdev->exp_bdev.product_name = "SPDK OCF";

	vbdev->exp_bdev.blockcnt = vbdev->core.bdev->blockcnt;
	vbdev->exp_bdev.ctxt = vbdev;
	vbdev->exp_bdev.fn_table = &cache_dev_fn_table;
	vbdev->exp_bdev.module = &ocf_if;

	/* Finally register vbdev in SPDK */
	spdk_io_device_register(vbdev, io_device_create_cb, io_device_destroy_cb,
				sizeof(struct vbdev_ocf_qctx), vbdev->name);
	result = spdk_bdev_register(&vbdev->exp_bdev);
	if (result) {
		SPDK_ERRLOG("Could not register exposed bdev %s\n",
			    vbdev->name);
		vbdev_ocf_mngt_exit(vbdev, unregister_path_dirty, result);
		return;
	} else {
		vbdev->state.started = true;
	}

	vbdev_ocf_mngt_continue(vbdev, result);
}

static void
add_core_cmpl(ocf_cache_t cache, ocf_core_t core, void *priv, int error)
{
	struct vbdev_ocf *vbdev = priv;

	ocf_mngt_cache_unlock(cache);

	if (error) {
		SPDK_ERRLOG("Error %d, failed to add core device to cache instance %s,"
			    "starting rollback\n", error, vbdev->name);
		vbdev_ocf_mngt_exit(vbdev, unregister_path_dirty, error);
		return;
	} else {
		vbdev->ocf_core = core;
	}

	vbdev_ocf_mngt_continue(vbdev, error);
}

/* Try to lock cache, then add core */
static void
add_core_cache_lock_cmpl(ocf_cache_t cache, void *priv, int error)
{
	struct vbdev_ocf *vbdev = (struct vbdev_ocf *)priv;

	if (error) {
		SPDK_ERRLOG("Error %d, can not lock cache instance %s,"
			    "starting rollback\n", error, vbdev->name);
		vbdev_ocf_mngt_exit(vbdev, unregister_path_dirty, error);
	}
	ocf_mngt_cache_add_core(vbdev->ocf_cache, &vbdev->cfg.core, add_core_cmpl, vbdev);
}

/* Add core for existing OCF cache instance */
static void
add_core(struct vbdev_ocf *vbdev)
{
	ocf_mngt_cache_lock(vbdev->ocf_cache, add_core_cache_lock_cmpl, vbdev);
}

static void
start_cache_cmpl(ocf_cache_t cache, void *priv, int error)
{
	struct vbdev_ocf *vbdev = priv;
	uint64_t mem_needed;

	ocf_mngt_cache_unlock(cache);

	if (error) {
		SPDK_ERRLOG("Error %d during start cache %s, starting rollback\n",
			    error, vbdev->name);

		if (error == -OCF_ERR_NO_MEM) {
			ocf_mngt_get_ram_needed(cache, &vbdev->cfg.device, &mem_needed);

			SPDK_NOTICELOG("Try to increase hugepage memory size or cache line size. "
				       "For your configuration:\nDevice size: %"PRIu64" bytes\n"
				       "Cache line size: %"PRIu64" bytes\nFree memory needed to start "
				       "cache: %"PRIu64" bytes\n", vbdev->cache.bdev->blockcnt *
				       vbdev->cache.bdev->blocklen, vbdev->cfg.cache.cache_line_size,
				       mem_needed);
		}

		vbdev_ocf_mngt_exit(vbdev, unregister_path_dirty, error);
		return;
	}

	vbdev_ocf_mngt_continue(vbdev, error);
}

static int
create_management_queue(struct vbdev_ocf *vbdev)
{
	struct spdk_poller *mngt_poller;
	int rc;

	rc = vbdev_ocf_queue_create(vbdev->ocf_cache, &vbdev->cache_ctx->mngt_queue, &mngt_queue_ops);
	if (rc) {
		SPDK_ERRLOG("Unable to create mngt_queue: %d\n", rc);
		return rc;
	}

	mngt_poller = SPDK_POLLER_REGISTER(mngt_queue_poll, vbdev->cache_ctx->mngt_queue, 100);
	if (mngt_poller == NULL) {
		SPDK_ERRLOG("Unable to initiate mngt request: %s", spdk_strerror(ENOMEM));
		return -ENOMEM;
	}

	ocf_queue_set_priv(vbdev->cache_ctx->mngt_queue, mngt_poller);
	ocf_mngt_cache_set_mngt_queue(vbdev->ocf_cache, vbdev->cache_ctx->mngt_queue);

	return 0;
}

/* Start OCF cache, attach caching device */
static void
start_cache(struct vbdev_ocf *vbdev)
{
	ocf_cache_t existing;
	uint32_t cache_block_size = vbdev->cache.bdev->blocklen;
	uint32_t core_block_size = vbdev->core.bdev->blocklen;
	int rc;

	if (is_ocf_cache_running(vbdev)) {
		vbdev_ocf_mngt_stop(vbdev, NULL, -EALREADY);
		return;
	}

	if (cache_block_size > core_block_size) {
		SPDK_ERRLOG("Cache bdev block size (%d) is bigger then core bdev block size (%d)\n",
			    cache_block_size, core_block_size);
		vbdev_ocf_mngt_exit(vbdev, unregister_path_dirty, -EINVAL);
		return;
	}

	existing = get_other_cache_instance(vbdev);
	if (existing) {
		SPDK_NOTICELOG("OCF bdev %s connects to existing cache device %s\n",
			       vbdev->name, vbdev->cache.name);
		vbdev->ocf_cache = existing;
		ocf_mngt_cache_get(vbdev->ocf_cache);
		vbdev->cache_ctx = ocf_cache_get_priv(existing);
		vbdev_ocf_cache_ctx_get(vbdev->cache_ctx);
		vbdev_ocf_mngt_continue(vbdev, 0);
		return;
	}

	vbdev->cache_ctx = calloc(1, sizeof(struct vbdev_ocf_cache_ctx));
	if (vbdev->cache_ctx == NULL) {
		vbdev_ocf_mngt_exit(vbdev, unregister_path_dirty, -ENOMEM);
		return;
	}

	vbdev_ocf_cache_ctx_get(vbdev->cache_ctx);
	pthread_mutex_init(&vbdev->cache_ctx->lock, NULL);

	rc = ocf_mngt_cache_start(vbdev_ocf_ctx, &vbdev->ocf_cache, &vbdev->cfg.cache, NULL);
	if (rc) {
		SPDK_ERRLOG("Could not start cache %s: %d\n", vbdev->name, rc);
		vbdev_ocf_mngt_exit(vbdev, unregister_path_dirty, rc);
		return;
	}
	ocf_mngt_cache_get(vbdev->ocf_cache);

	ocf_cache_set_priv(vbdev->ocf_cache, vbdev->cache_ctx);

	rc = create_management_queue(vbdev);
	if (rc) {
		SPDK_ERRLOG("Unable to create mngt_queue: %d\n", rc);
		vbdev_ocf_mngt_exit(vbdev, unregister_path_dirty, rc);
		return;
	}

	if (vbdev->cfg.loadq) {
		ocf_mngt_cache_load(vbdev->ocf_cache, &vbdev->cfg.device, start_cache_cmpl, vbdev);
	} else {
		ocf_mngt_cache_attach(vbdev->ocf_cache, &vbdev->cfg.device, start_cache_cmpl, vbdev);
	}
}

/* Procedures called during register operation */
vbdev_ocf_mngt_fn register_path[] = {
	start_cache,
	add_core,
	finish_register,
	NULL
};

/* Start cache instance and register OCF bdev */
static void
register_vbdev(struct vbdev_ocf *vbdev, vbdev_ocf_mngt_callback cb, void *cb_arg)
{
	int rc;

	if (!(vbdev->core.attached && vbdev->cache.attached) || vbdev->state.started) {
		cb(-EPERM, vbdev, cb_arg);
		return;
	}

	vbdev->state.starting = true;
	rc = vbdev_ocf_mngt_start(vbdev, register_path, cb, cb_arg);
	if (rc) {
		cb(rc, vbdev, cb_arg);
	}
}

/* Init OCF configuration options
 * for core and cache devices */
static void
init_vbdev_config(struct vbdev_ocf *vbdev)
{
	struct vbdev_ocf_config *cfg = &vbdev->cfg;

	/* Initialize OCF defaults first */
	ocf_mngt_cache_device_config_set_default(&cfg->device);
	ocf_mngt_cache_config_set_default(&cfg->cache);
	ocf_mngt_core_config_set_default(&cfg->core);

	snprintf(cfg->cache.name, sizeof(cfg->cache.name), "%s", vbdev->name);
	snprintf(cfg->core.name, sizeof(cfg->core.name), "%s", vbdev->core.name);

	cfg->device.open_cores = false;
	cfg->device.perform_test = false;
	cfg->device.discard_on_start = false;

	vbdev->cfg.cache.locked = true;

	cfg->core.volume_type = SPDK_OBJECT;
	cfg->device.volume_type = SPDK_OBJECT;

	if (vbdev->cfg.loadq) {
		/* When doing cache_load(), we need to set try_add to true,
		 * otherwise OCF will interpret this core as new
		 * instead of the inactive one */
		vbdev->cfg.core.try_add = true;
	} else {
		/* When cache is initialized as new, set force flag to true,
		 * to ignore warnings about existing metadata */
		cfg->device.force = true;
	}

	/* Serialize bdev names in OCF UUID to interpret on future loads
	 * Core UUID is a triple of (core name, vbdev name, cache name)
	 * Cache UUID is cache bdev name */
	cfg->device.uuid.size = strlen(vbdev->cache.name) + 1;
	cfg->device.uuid.data = vbdev->cache.name;

	snprintf(vbdev->uuid, VBDEV_OCF_MD_MAX_LEN, "%s %s %s",
		 vbdev->core.name, vbdev->name, vbdev->cache.name);
	cfg->core.uuid.size = strlen(vbdev->uuid) + 1;
	cfg->core.uuid.data = vbdev->uuid;
	vbdev->uuid[strlen(vbdev->core.name)] = 0;
	vbdev->uuid[strlen(vbdev->core.name) + 1 + strlen(vbdev->name)] = 0;
}

/* Allocate vbdev structure object and add it to the global list */
static int
init_vbdev(const char *vbdev_name,
	   const char *cache_mode_name,
	   const uint64_t cache_line_size,
	   const char *cache_name,
	   const char *core_name,
	   bool loadq)
{
	struct vbdev_ocf *vbdev;
	int rc = 0;

	if (spdk_bdev_get_by_name(vbdev_name) || vbdev_ocf_get_by_name(vbdev_name)) {
		SPDK_ERRLOG("Device with name '%s' already exists\n", vbdev_name);
		return -EPERM;
	}

	vbdev = calloc(1, sizeof(*vbdev));
	if (!vbdev) {
		goto error_mem;
	}

	vbdev->name = strdup(vbdev_name);
	if (!vbdev->name) {
		goto error_mem;
	}

	vbdev->cache.name = strdup(cache_name);
	if (!vbdev->cache.name) {
		goto error_mem;
	}

	vbdev->core.name = strdup(core_name);
	if (!vbdev->core.name) {
		goto error_mem;
	}

	vbdev->cache.parent = vbdev;
	vbdev->core.parent = vbdev;
	vbdev->cache.is_cache = true;
	vbdev->core.is_cache = false;
	vbdev->cfg.loadq = loadq;

	init_vbdev_config(vbdev);

	if (cache_mode_name) {
		vbdev->cfg.cache.cache_mode
			= ocf_get_cache_mode(cache_mode_name);
	} else if (!loadq) { /* In load path it is OK to pass NULL as cache mode */
		SPDK_ERRLOG("No cache mode specified\n");
		rc = -EINVAL;
		goto error_free;
	}
	if (vbdev->cfg.cache.cache_mode < 0) {
		SPDK_ERRLOG("Incorrect cache mode '%s'\n", cache_mode_name);
		rc = -EINVAL;
		goto error_free;
	}

	ocf_cache_line_size_t set_cache_line_size = cache_line_size ?
			(ocf_cache_line_size_t)cache_line_size * KiB :
			ocf_cache_line_size_default;
	if (set_cache_line_size == 0) {
		SPDK_ERRLOG("Cache line size should be non-zero.\n");
		rc = -EINVAL;
		goto error_free;
	}
	vbdev->cfg.device.cache_line_size = set_cache_line_size;
	vbdev->cfg.cache.cache_line_size = set_cache_line_size;

	TAILQ_INSERT_TAIL(&g_ocf_vbdev_head, vbdev, tailq);
	return rc;

error_mem:
	rc = -ENOMEM;
error_free:
	free_vbdev(vbdev);
	return rc;
}

/* Read configuration file at the start of SPDK application
 * This adds vbdevs to global list if some mentioned in config */
static int
vbdev_ocf_init(void)
{
	int status;

	status = vbdev_ocf_ctx_init();
	if (status) {
		SPDK_ERRLOG("OCF ctx initialization failed with=%d\n", status);
		return status;
	}

	status = vbdev_ocf_volume_init();
	if (status) {
		vbdev_ocf_ctx_cleanup();
		SPDK_ERRLOG("OCF volume initialization failed with=%d\n", status);
		return status;
	}

	return status;
}

/* Called after application shutdown started
 * Release memory of allocated structures here */
static void
vbdev_ocf_module_fini(void)
{
	struct vbdev_ocf *vbdev;

	while ((vbdev = TAILQ_FIRST(&g_ocf_vbdev_head))) {
		TAILQ_REMOVE(&g_ocf_vbdev_head, vbdev, tailq);
		free_vbdev(vbdev);
	}

	vbdev_ocf_volume_cleanup();
	vbdev_ocf_ctx_cleanup();
}

/* When base device gets unplugged this is called
 * We will unregister cache vbdev here
 * When cache device is removed, we delete every OCF bdev that used it */
static void
hotremove_cb(struct vbdev_ocf_base *base)
{
	struct vbdev_ocf *vbdev;

	if (!base->is_cache) {
		if (base->parent->state.doing_finish) {
			return;
		}

		SPDK_NOTICELOG("Deinitializing '%s' because its core device '%s' was removed\n",
			       base->parent->name, base->name);
		vbdev_ocf_delete(base->parent, NULL, NULL);
		return;
	}

	TAILQ_FOREACH(vbdev, &g_ocf_vbdev_head, tailq) {
		if (vbdev->state.doing_finish) {
			continue;
		}
		if (strcmp(base->name, vbdev->cache.name) == 0) {
			SPDK_NOTICELOG("Deinitializing '%s' because"
				       " its cache device '%s' was removed\n",
				       vbdev->name, base->name);
			vbdev_ocf_delete(vbdev, NULL, NULL);
		}
	}
}

static void
base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		   void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		if (event_ctx) {
			hotremove_cb(event_ctx);
		}
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

/* Open base SPDK bdev and claim it */
static int
attach_base(struct vbdev_ocf_base *base)
{
	int status;

	if (base->attached) {
		return -EALREADY;
	}

	/* If base cache bdev was already opened by other vbdev,
	 * we just copy its descriptor here */
	if (base->is_cache) {
		struct vbdev_ocf_base *existing = get_other_cache_base(base);
		if (existing) {
			base->desc = existing->desc;
			base->management_channel = existing->management_channel;
			base->attached = true;
			return 0;
		}
	}

	status = spdk_bdev_open_ext(base->name, true, base_bdev_event_cb, base, &base->desc);
	if (status) {
		SPDK_ERRLOG("Unable to open device '%s' for writing\n", base->name);
		return status;
	}

	status = spdk_bdev_module_claim_bdev(base->bdev, base->desc,
					     &ocf_if);
	if (status) {
		SPDK_ERRLOG("Unable to claim device '%s'\n", base->name);
		spdk_bdev_close(base->desc);
		return status;
	}

	base->management_channel = spdk_bdev_get_io_channel(base->desc);
	if (!base->management_channel) {
		SPDK_ERRLOG("Unable to get io channel '%s'\n", base->name);
		spdk_bdev_module_release_bdev(base->bdev);
		spdk_bdev_close(base->desc);
		return -ENOMEM;
	}

	/* Save the thread where the base device is opened */
	base->thread = spdk_get_thread();

	base->attached = true;
	return status;
}

/* Attach base bdevs */
static int
attach_base_bdevs(struct vbdev_ocf *vbdev,
		  struct spdk_bdev *cache_bdev,
		  struct spdk_bdev *core_bdev)
{
	int rc = 0;

	if (cache_bdev) {
		vbdev->cache.bdev = cache_bdev;
		rc |= attach_base(&vbdev->cache);
	}

	if (core_bdev) {
		vbdev->core.bdev = core_bdev;
		rc |= attach_base(&vbdev->core);
	}

	return rc;
}

/* Init and then start vbdev if all base devices are present */
void
vbdev_ocf_construct(const char *vbdev_name,
		    const char *cache_mode_name,
		    const uint64_t cache_line_size,
		    const char *cache_name,
		    const char *core_name,
		    bool loadq,
		    void (*cb)(int, struct vbdev_ocf *, void *),
		    void *cb_arg)
{
	int rc;
	struct spdk_bdev *cache_bdev = spdk_bdev_get_by_name(cache_name);
	struct spdk_bdev *core_bdev = spdk_bdev_get_by_name(core_name);
	struct vbdev_ocf *vbdev;

	rc = init_vbdev(vbdev_name, cache_mode_name, cache_line_size, cache_name, core_name, loadq);
	if (rc) {
		cb(rc, NULL, cb_arg);
		return;
	}

	vbdev = vbdev_ocf_get_by_name(vbdev_name);
	if (vbdev == NULL) {
		cb(-ENODEV, NULL, cb_arg);
		return;
	}

	if (cache_bdev == NULL) {
		SPDK_NOTICELOG("OCF bdev '%s' is waiting for cache device '%s' to connect\n",
			       vbdev->name, cache_name);
	}
	if (core_bdev == NULL) {
		SPDK_NOTICELOG("OCF bdev '%s' is waiting for core device '%s' to connect\n",
			       vbdev->name, core_name);
	}

	rc = attach_base_bdevs(vbdev, cache_bdev, core_bdev);
	if (rc) {
		cb(rc, vbdev, cb_arg);
		return;
	}

	if (core_bdev && cache_bdev) {
		register_vbdev(vbdev, cb, cb_arg);
	} else {
		cb(0, vbdev, cb_arg);
	}
}

/* Set new cache mode on OCF cache */
void
vbdev_ocf_set_cache_mode(struct vbdev_ocf *vbdev,
			 const char *cache_mode_name,
			 void (*cb)(int, struct vbdev_ocf *, void *),
			 void *cb_arg)
{
	ocf_cache_t cache;
	ocf_cache_mode_t cache_mode;
	int rc;

	cache = vbdev->ocf_cache;
	cache_mode = ocf_get_cache_mode(cache_mode_name);

	rc = ocf_mngt_cache_trylock(cache);
	if (rc) {
		cb(rc, vbdev, cb_arg);
		return;
	}

	rc = ocf_mngt_cache_set_mode(cache, cache_mode);
	ocf_mngt_cache_unlock(cache);
	cb(rc, vbdev, cb_arg);
}

/* This called if new device is created in SPDK application
 * If that device named as one of base bdevs of OCF vbdev,
 * claim and open them */
static void
vbdev_ocf_examine(struct spdk_bdev *bdev)
{
	const char *bdev_name = spdk_bdev_get_name(bdev);
	struct vbdev_ocf *vbdev;

	TAILQ_FOREACH(vbdev, &g_ocf_vbdev_head, tailq) {
		if (vbdev->state.doing_finish) {
			continue;
		}

		if (!strcmp(bdev_name, vbdev->cache.name)) {
			attach_base_bdevs(vbdev, bdev, NULL);
			continue;
		}
		if (!strcmp(bdev_name, vbdev->core.name)) {
			attach_base_bdevs(vbdev, NULL, bdev);
			break;
		}
	}
	spdk_bdev_module_examine_done(&ocf_if);
}

struct metadata_probe_ctx {
	struct vbdev_ocf_base base;
	ocf_volume_t volume;

	struct ocf_volume_uuid *core_uuids;
	unsigned int uuid_count;

	int result;
	int refcnt;
};

static void
_examine_ctx_put(void *ctx)
{
	struct spdk_bdev_desc *desc = ctx;

	spdk_bdev_close(desc);
}

static void
examine_ctx_put(struct metadata_probe_ctx *ctx)
{
	unsigned int i;

	ctx->refcnt--;
	if (ctx->refcnt > 0) {
		return;
	}

	if (ctx->result) {
		SPDK_ERRLOG("OCF metadata probe for bdev '%s' failed with %d\n",
			    spdk_bdev_get_name(ctx->base.bdev), ctx->result);
	}

	if (ctx->base.desc) {
		/* Close the underlying bdev on its same opened thread. */
		if (ctx->base.thread && ctx->base.thread != spdk_get_thread()) {
			spdk_thread_send_msg(ctx->base.thread, _examine_ctx_put, ctx->base.desc);
		} else {
			spdk_bdev_close(ctx->base.desc);
		}
	}

	if (ctx->volume) {
		ocf_volume_destroy(ctx->volume);
	}

	if (ctx->core_uuids) {
		for (i = 0; i < ctx->uuid_count; i++) {
			free(ctx->core_uuids[i].data);
		}
	}
	free(ctx->core_uuids);

	examine_done(ctx->result, NULL, ctx->base.bdev);
	free(ctx);
}

static void
metadata_probe_construct_cb(int rc, struct vbdev_ocf *vbdev, void *vctx)
{
	struct metadata_probe_ctx *ctx = vctx;

	examine_ctx_put(ctx);
}

/* This is second callback for ocf_metadata_probe_cores()
 * Here we create vbdev configurations based on UUIDs */
static void
metadata_probe_cores_construct(void *priv, int error, unsigned int num_cores)
{
	struct metadata_probe_ctx *ctx = priv;
	const char *vbdev_name;
	const char *core_name;
	const char *cache_name;
	unsigned int i;

	if (error) {
		ctx->result = error;
		examine_ctx_put(ctx);
		return;
	}

	for (i = 0; i < num_cores; i++) {
		core_name = ocf_uuid_to_str(&ctx->core_uuids[i]);
		vbdev_name = core_name + strlen(core_name) + 1;
		cache_name = vbdev_name + strlen(vbdev_name) + 1;

		if (strcmp(ctx->base.bdev->name, cache_name)) {
			SPDK_NOTICELOG("OCF metadata found on %s belongs to bdev named '%s'\n",
				       ctx->base.bdev->name, cache_name);
		}

		ctx->refcnt++;
		vbdev_ocf_construct(vbdev_name, NULL, 0, cache_name, core_name, true,
				    metadata_probe_construct_cb, ctx);
	}

	examine_ctx_put(ctx);
}

/* This callback is called after OCF reads cores UUIDs from cache metadata
 * Here we allocate memory for those UUIDs and call ocf_metadata_probe_cores() again */
static void
metadata_probe_cores_get_num(void *priv, int error, unsigned int num_cores)
{
	struct metadata_probe_ctx *ctx = priv;
	unsigned int i;

	if (error) {
		ctx->result = error;
		examine_ctx_put(ctx);
		return;
	}

	ctx->uuid_count = num_cores;
	ctx->core_uuids = calloc(num_cores, sizeof(struct ocf_volume_uuid));
	if (!ctx->core_uuids) {
		ctx->result = -ENOMEM;
		examine_ctx_put(ctx);
		return;
	}

	for (i = 0; i < ctx->uuid_count; i++) {
		ctx->core_uuids[i].size = OCF_VOLUME_UUID_MAX_SIZE;
		ctx->core_uuids[i].data = malloc(OCF_VOLUME_UUID_MAX_SIZE);
		if (!ctx->core_uuids[i].data) {
			ctx->result = -ENOMEM;
			examine_ctx_put(ctx);
			return;
		}
	}

	ocf_metadata_probe_cores(vbdev_ocf_ctx, ctx->volume, ctx->core_uuids, ctx->uuid_count,
				 metadata_probe_cores_construct, ctx);
}

static void
metadata_probe_cb(void *priv, int rc,
		  struct ocf_metadata_probe_status *status)
{
	struct metadata_probe_ctx *ctx = priv;

	if (rc) {
		/* -ENODATA means device does not have cache metadata on it */
		if (rc != -OCF_ERR_NO_METADATA) {
			ctx->result = rc;
		}
		examine_ctx_put(ctx);
		return;
	}

	ocf_metadata_probe_cores(vbdev_ocf_ctx, ctx->volume, NULL, 0,
				 metadata_probe_cores_get_num, ctx);
}

/* This is called after vbdev_ocf_examine
 * It allows to delay application initialization
 * until all OCF bdevs get registered
 * If vbdev has all of its base devices it starts asynchronously here
 * We first check if bdev appears in configuration,
 * if not we do metadata_probe() to create its configuration from bdev metadata */
static void
vbdev_ocf_examine_disk(struct spdk_bdev *bdev)
{
	const char *bdev_name = spdk_bdev_get_name(bdev);
	struct vbdev_ocf *vbdev;
	struct metadata_probe_ctx *ctx;
	bool created_from_config = false;
	int rc;

	examine_start(bdev);

	TAILQ_FOREACH(vbdev, &g_ocf_vbdev_head, tailq) {
		if (vbdev->state.doing_finish || vbdev->state.started) {
			continue;
		}

		if (!strcmp(bdev_name, vbdev->cache.name)) {
			examine_start(bdev);
			register_vbdev(vbdev, examine_done, bdev);
			created_from_config = true;
			continue;
		}
		if (!strcmp(bdev_name, vbdev->core.name)) {
			examine_start(bdev);
			register_vbdev(vbdev, examine_done, bdev);
			examine_done(0, NULL, bdev);
			return;
		}
	}

	/* If devices is discovered during config we do not check for metadata */
	if (created_from_config) {
		examine_done(0, NULL, bdev);
		return;
	}

	/* Metadata probe path
	 * We create temporary OCF volume and a temporary base structure
	 * to use them for ocf_metadata_probe() and for bottom adapter IOs
	 * Then we get UUIDs of core devices an create configurations based on them */
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		examine_done(-ENOMEM, NULL, bdev);
		return;
	}

	ctx->base.bdev = bdev;
	ctx->refcnt = 1;

	rc = spdk_bdev_open_ext(bdev_name, true, base_bdev_event_cb, NULL, &ctx->base.desc);
	if (rc) {
		ctx->result = rc;
		examine_ctx_put(ctx);
		return;
	}

	rc = ocf_ctx_volume_create(vbdev_ocf_ctx, &ctx->volume, NULL, SPDK_OBJECT);
	if (rc) {
		ctx->result = rc;
		examine_ctx_put(ctx);
		return;
	}

	rc = ocf_volume_open(ctx->volume, &ctx->base);
	if (rc) {
		ctx->result = rc;
		examine_ctx_put(ctx);
		return;
	}

	/* Save the thread where the base device is opened */
	ctx->base.thread = spdk_get_thread();

	ocf_metadata_probe(vbdev_ocf_ctx, ctx->volume, metadata_probe_cb, ctx);
}

static int
vbdev_ocf_get_ctx_size(void)
{
	return sizeof(struct bdev_ocf_data);
}

static void
fini_start(void)
{
	g_fini_started = true;
}

/* Module-global function table
 * Does not relate to vbdev instances */
static struct spdk_bdev_module ocf_if = {
	.name = "ocf",
	.module_init = vbdev_ocf_init,
	.fini_start = fini_start,
	.module_fini = vbdev_ocf_module_fini,
	.get_ctx_size = vbdev_ocf_get_ctx_size,
	.examine_config = vbdev_ocf_examine,
	.examine_disk   = vbdev_ocf_examine_disk,
};
SPDK_BDEV_MODULE_REGISTER(ocf, &ocf_if);
