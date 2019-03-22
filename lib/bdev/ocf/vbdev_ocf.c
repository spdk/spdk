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
#include "spdk/conf.h"
#include "spdk/io_channel.h"
#include "spdk/string.h"
#include "spdk_internal/log.h"
#include "spdk/cpuset.h"

static struct spdk_bdev_module ocf_if;

static TAILQ_HEAD(, vbdev_ocf) g_ocf_vbdev_head
	= TAILQ_HEAD_INITIALIZER(g_ocf_vbdev_head);

static TAILQ_HEAD(, examining_bdev) g_ocf_examining_bdevs_head
	= TAILQ_HEAD_INITIALIZER(g_ocf_examining_bdevs_head);

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

/* Tells if current bdev can be marked as done at examine */
static bool
examine_isdone(struct spdk_bdev *bdev)
{
	struct examining_bdev *entry;

	TAILQ_FOREACH(entry, &g_ocf_examining_bdevs_head, tailq) {
		if (entry->bdev == bdev) {
			return false;
		}
	}
	return true;
}

/* If bdev exists on list of claimed bdevs, remove it and report examine done */
static void
examine_done(int status, void *cb_arg)
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
		if (cmp->ocf_cache) {
			return cmp->ocf_cache;
		}
	}

	return NULL;
}

static void
stop_vbdev_cmpl(ocf_cache_t cache, void *priv, int error)
{
	struct vbdev_ocf *vbdev = priv;

	ocf_mngt_cache_unlock(cache);
	vbdev_ocf_mngt_continue(vbdev, error);
}

/* Try to lock cache, then stop it */
static void
stop_vbdev_poll(struct vbdev_ocf *vbdev)
{
	if (!ocf_cache_is_running(vbdev->ocf_cache)) {
		vbdev_ocf_mngt_continue(vbdev, 0);
		return;
	}

	if (get_other_cache_instance(vbdev)) {
		SPDK_NOTICELOG("Not stopping cache instance '%s'"
			       " because it is referenced by other OCF bdev\n",
			       vbdev->cache.name);
		vbdev_ocf_mngt_continue(vbdev, 0);
		return;
	}

	if (ocf_mngt_cache_trylock(vbdev->ocf_cache)) {
		return;
	}

	ocf_mngt_cache_stop(vbdev->ocf_cache, stop_vbdev_cmpl, vbdev);
}

/* Stop OCF cache object
 * vbdev_ocf is not operational after this */
static void
stop_vbdev(struct vbdev_ocf *vbdev)
{
	if (vbdev == NULL) {
		vbdev_ocf_mngt_continue(vbdev, -EFAULT);
		return;
	}

	if (vbdev->ocf_cache == NULL) {
		vbdev_ocf_mngt_continue(vbdev, -EFAULT);
		return;
	}

	if (!ocf_cache_is_running(vbdev->ocf_cache)) {
		vbdev_ocf_mngt_continue(vbdev, -EINVAL);
		return;
	}

	vbdev_ocf_mngt_poll(vbdev, stop_vbdev_poll);
}

/* Close and unclaim base bdev */
static void
remove_base_bdev(struct vbdev_ocf_base *base)
{
	if (base->attached) {
		spdk_bdev_module_release_bdev(base->bdev);
		spdk_bdev_close(base->desc);
		base->attached = false;
	}
}

static void
close_core_bdev(struct vbdev_ocf *vbdev)
{
	remove_base_bdev(&vbdev->core);
	vbdev_ocf_mngt_continue(vbdev, 0);
}

static void
close_cache_bdev(struct vbdev_ocf *vbdev)
{
	remove_base_bdev(&vbdev->cache);
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
remove_core_poll(struct vbdev_ocf *vbdev)
{
	ocf_core_t core;
	int rc;

	rc = ocf_core_get(vbdev->ocf_cache, vbdev->core.id, &core);
	if (rc) {
		vbdev_ocf_mngt_continue(vbdev, rc);
		return;
	}

	rc = ocf_mngt_cache_trylock(vbdev->ocf_cache);
	if (rc) {
		return;
	}

	ocf_mngt_cache_remove_core(core, remove_core_cmpl, vbdev);
}

/* Release SPDK and OCF objects associated with base */
static void
detach_base(struct vbdev_ocf_base *base)
{
	struct vbdev_ocf *vbdev = base->parent;

	if (base->is_cache && get_other_cache_base(base)) {
		base->attached = false;
		vbdev_ocf_mngt_continue(vbdev, 0);
		return;
	}

	if (vbdev->ocf_cache && ocf_cache_is_running(vbdev->ocf_cache)) {
		if (base->is_cache) {
			vbdev_ocf_mngt_continue(vbdev, 0);
		} else {
			vbdev_ocf_mngt_poll(vbdev, remove_core_poll);
		}
	} else {
		vbdev_ocf_mngt_continue(vbdev, 0);
	}
}

/* Finish unregister operation */
static void
unregister_finish(struct vbdev_ocf *vbdev)
{
	spdk_bdev_destruct_done(&vbdev->exp_bdev, vbdev->state.stop_status);
	vbdev_ocf_mngt_continue(vbdev, 0);
}

/* Detach core base */
static void
detach_core(struct vbdev_ocf *vbdev)
{
	detach_base(&vbdev->core);
}

/* Detach cache base */
static void
detach_cache(struct vbdev_ocf *vbdev)
{
	vbdev->state.stop_status = vbdev->mngt_ctx.status;
	detach_base(&vbdev->cache);
}

/* Wait for all OCF requests to finish */
static void
wait_for_requests_poll(struct vbdev_ocf *vbdev)
{
	if (ocf_cache_has_pending_requests(vbdev->ocf_cache)) {
		return;
	}

	vbdev_ocf_mngt_continue(vbdev, 0);
}

/* Start waiting for OCF requests to finish */
static void
wait_for_requests(struct vbdev_ocf *vbdev)
{
	vbdev_ocf_mngt_poll(vbdev, wait_for_requests_poll);
}

/* Procedures called during unregister */
vbdev_ocf_mngt_fn unregister_path[] = {
	wait_for_requests,
	stop_vbdev,
	detach_cache,
	close_cache_bdev,
	detach_core,
	close_core_bdev,
	unregister_finish,
	NULL
};

/* Start asynchronous management operation using unregister_path */
static void
unregister_cb(void *opaque)
{
	struct vbdev_ocf *vbdev = opaque;
	int rc;

	rc = vbdev_ocf_mngt_start(vbdev, unregister_path, NULL, NULL);
	if (rc) {
		SPDK_ERRLOG("Unable to unregister OCF bdev: %d\n", rc);
		spdk_bdev_destruct_done(&vbdev->exp_bdev, rc);
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
	vbdev->state.doing_finish = true;

	if (vbdev->state.started) {
		spdk_io_device_unregister(vbdev, unregister_cb);
		/* Return 1 because unregister is delayed */
		return 1;
	}

	if (vbdev->cache.attached) {
		detach_cache(vbdev);
		close_cache_bdev(vbdev);
	}
	if (vbdev->core.attached) {
		detach_core(vbdev);
		close_core_bdev(vbdev);
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
	int dir;
	uint64_t len = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;
	uint64_t offset = bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_READ:
		dir = OCF_READ;
		if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
			dir = OCF_WRITE;
		}
		ocf_io_configure(io, offset, len, dir, 0, 0);
		ocf_core_submit_io(io);
		return 0;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		ocf_io_configure(io, offset, len, OCF_WRITE, 0, OCF_WRITE_FLUSH);
		ocf_core_submit_flush(io);
		return 0;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		ocf_io_configure(io, offset, len, 0, 0, 0);
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
	struct vbdev_ocf_qcxt *qctx = spdk_io_channel_get_ctx(ch);
	int err;

	io = ocf_core_new_io(vbdev->ocf_core);
	if (!io) {
		err = -ENOMEM;
		goto fail;
	}

	ocf_io_set_queue(io, qctx->queue);

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
				     ocf_cache_get_line_size(vbdev->ocf_cache));
	spdk_json_write_named_bool(w, "metadata_volatile",
				   vbdev->cfg.cache.metadata_volatile);

	return 0;
}

static void
vbdev_ocf_write_json_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct vbdev_ocf *vbdev = bdev->ctxt;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "construct_ocf_bdev");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", vbdev->name);
	spdk_json_write_named_string(w, "mode",
				     ocf_get_cache_modename(vbdev->cfg.cache.cache_mode));
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

static void
start_cache_cmpl(ocf_cache_t cache, void *priv, int error)
{
	struct vbdev_ocf *vbdev = priv;

	ocf_mngt_cache_unlock(cache);
	vbdev_ocf_mngt_continue(vbdev, error);
}

/* Start OCF cache, attach caching device */
static void
start_cache(struct vbdev_ocf *vbdev)
{
	ocf_cache_t existing;
	int rc;

	if (vbdev->ocf_cache) {
		vbdev_ocf_mngt_continue(vbdev, -EALREADY);
		return;
	}

	existing = get_other_cache_instance(vbdev);
	if (existing) {
		SPDK_NOTICELOG("OCF bdev %s connects to existing cache device %s\n",
			       vbdev->name, vbdev->cache.name);
		vbdev->ocf_cache = existing;
		vbdev->cache.id = ocf_cache_get_id(vbdev->ocf_cache);
		vbdev_ocf_mngt_continue(vbdev, 0);
		return;
	}

	rc = ocf_mngt_cache_start(vbdev_ocf_ctx, &vbdev->ocf_cache, &vbdev->cfg.cache);
	if (rc) {
		vbdev_ocf_mngt_continue(vbdev, rc);
		return;
	}

	vbdev->cache.id = ocf_cache_get_id(vbdev->ocf_cache);

	ocf_mngt_cache_attach(vbdev->ocf_cache, &vbdev->cfg.device, start_cache_cmpl, vbdev);
}

static void
add_core_cmpl(ocf_cache_t cache, ocf_core_t core, void *priv, int error)
{
	struct vbdev_ocf *vbdev = priv;

	ocf_mngt_cache_unlock(cache);

	if (error) {
		SPDK_ERRLOG("Failed to add core device to cache instance\n");
	} else {
		vbdev->ocf_core = core;
		vbdev->core.id  = ocf_core_get_id(core);
	}

	vbdev_ocf_mngt_continue(vbdev, error);
}

/* Try to lock cache, then add core */
static void
attach_core_poll(struct vbdev_ocf *vbdev)
{
	if (ocf_mngt_cache_trylock(vbdev->ocf_cache)) {
		return;
	}

	ocf_mngt_cache_add_core(vbdev->ocf_cache, &vbdev->cfg.core, add_core_cmpl, vbdev);
}

/* Add core for existing OCF cache instance */
static void
attach_core(struct vbdev_ocf *vbdev)
{
	vbdev_ocf_mngt_poll(vbdev, attach_core_poll);
}

/* Poller function for the OCF queue
 * We execute OCF requests here synchronously */
static int queue_poll(void *opaque)
{
	struct vbdev_ocf_qcxt *qctx = opaque;
	uint32_t iono = ocf_queue_pending_io(qctx->queue);
	int i, max = spdk_min(32, iono);

	for (i = 0; i < max; i++) {
		ocf_queue_run_single(qctx->queue);
	}

	if (iono > 0) {
		return 1;
	} else {
		return 0;
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
	struct vbdev_ocf_qcxt *qctx = ocf_queue_get_priv(q);

	if (qctx) {
		spdk_put_io_channel(qctx->cache_ch);
		spdk_put_io_channel(qctx->core_ch);
		spdk_poller_unregister(&qctx->poller);
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
	struct vbdev_ocf_qcxt *qctx = ctx_buf;
	int rc;

	rc = ocf_queue_create(vbdev->ocf_cache, &qctx->queue, &queue_ops);
	if (rc) {
		return rc;
	}

	ocf_queue_set_priv(qctx->queue, qctx);

	qctx->vbdev      = vbdev;
	qctx->cache_ch   = spdk_bdev_get_io_channel(vbdev->cache.desc);
	qctx->core_ch    = spdk_bdev_get_io_channel(vbdev->core.desc);
	qctx->poller     = spdk_poller_register(queue_poll, qctx, 0);

	return rc;
}

/* Called per thread
 * Put OCF queue and relaunch poller with new context to finish pending requests */
static void
io_device_destroy_cb(void *io_device, void *ctx_buf)
{
	/* Making a copy of context to use it after io channel will be destroyed */
	struct vbdev_ocf_qcxt *copy = malloc(sizeof(*copy));
	struct vbdev_ocf_qcxt *qctx = ctx_buf;

	if (copy) {
		ocf_queue_set_priv(qctx->queue, copy);
		memcpy(copy, qctx, sizeof(*copy));
		spdk_poller_unregister(&qctx->poller);
		copy->poller = spdk_poller_register(queue_poll, copy, 0);
	} else {
		SPDK_ERRLOG("Unable to stop OCF queue properly: %s\n",
			    spdk_strerror(ENOMEM));
	}

	ocf_queue_put(qctx->queue);
}

/* Create exported spdk object */
static void
register_ocf_bdev(struct vbdev_ocf *vbdev)
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
				sizeof(struct vbdev_ocf_qcxt), vbdev->name);
	result = spdk_bdev_register(&vbdev->exp_bdev);
	if (result) {
		SPDK_ERRLOG("Could not register exposed bdev\n");
	} else {
		vbdev->state.started = true;
	}

	vbdev_ocf_mngt_continue(vbdev, result);
}

/* Procedures called during register operation */
vbdev_ocf_mngt_fn register_path[] = {
	start_cache,
	attach_core,
	register_ocf_bdev,
	NULL
};

/* Init OCF configuration options
 * for core and cache devices */
static void
init_vbdev_config(struct vbdev_ocf *vbdev)
{
	struct vbdev_ocf_config *cfg = &vbdev->cfg;

	/* Id 0 means OCF decides the id */
	cfg->cache.id = 0;
	cfg->cache.name = vbdev->name;

	/* TODO [metadata]: make configurable with persistent
	 * metadata support */
	cfg->cache.metadata_volatile = true;

	/* TODO [cache line size]: make cache line size configurable
	 * Using standard 4KiB for now */
	cfg->cache.cache_line_size = ocf_cache_line_size_4;

	/* This are suggested values that
	 * should be sufficient for most use cases */
	cfg->cache.backfill.max_queue_size = 65536;
	cfg->cache.backfill.queue_unblock_size = 60000;

	/* TODO [cache line size] */
	cfg->device.cache_line_size = ocf_cache_line_size_4;
	cfg->device.force = true;
	cfg->device.min_free_ram = 0;
	cfg->device.perform_test = false;
	cfg->device.discard_on_start = false;

	vbdev->cfg.cache.locked = true;

	cfg->core.volume_type = SPDK_OBJECT;
	cfg->device.volume_type = SPDK_OBJECT;
	cfg->core.core_id = OCF_CORE_MAX;

	cfg->device.uuid.size = strlen(vbdev->cache.name) + 1;
	cfg->device.uuid.data = vbdev->cache.name;
	cfg->core.uuid.size = strlen(vbdev->core.name) + 1;
	cfg->core.uuid.data = vbdev->core.name;
}

/* Allocate vbdev structure object and add it to the global list */
static int
init_vbdev(const char *vbdev_name,
	   const char *cache_mode_name,
	   const char *cache_name,
	   const char *core_name)
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

	vbdev->cache.parent = vbdev;
	vbdev->core.parent = vbdev;
	vbdev->cache.is_cache = true;
	vbdev->core.is_cache = false;

	if (cache_mode_name) {
		vbdev->cfg.cache.cache_mode
			= ocf_get_cache_mode(cache_mode_name);
	} else {
		SPDK_ERRLOG("No cache mode specified\n");
		rc = -EINVAL;
		goto error_free;
	}
	if (vbdev->cfg.cache.cache_mode < 0) {
		SPDK_ERRLOG("Incorrect cache mode '%s'\n", cache_mode_name);
		rc = -EINVAL;
		goto error_free;
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

	init_vbdev_config(vbdev);
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
	const char *vbdev_name, *modename, *cache_name, *core_name;
	struct spdk_conf_section *sp;
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

	sp = spdk_conf_find_section(NULL, "OCF");
	if (sp == NULL) {
		return 0;
	}

	for (int i = 0; ; i++) {
		if (!spdk_conf_section_get_nval(sp, "OCF", i)) {
			break;
		}

		vbdev_name = spdk_conf_section_get_nmval(sp, "OCF", i, 0);
		if (!vbdev_name) {
			SPDK_ERRLOG("No vbdev name specified\n");
			continue;
		}

		modename = spdk_conf_section_get_nmval(sp, "OCF", i, 1);
		if (!modename) {
			SPDK_ERRLOG("No modename specified for OCF vbdev '%s'\n", vbdev_name);
			continue;
		}

		cache_name = spdk_conf_section_get_nmval(sp, "OCF", i, 2);
		if (!cache_name) {
			SPDK_ERRLOG("No cache device specified for OCF vbdev '%s'\n", vbdev_name);
			continue;
		}

		core_name = spdk_conf_section_get_nmval(sp, "OCF", i, 3);
		if (!core_name) {
			SPDK_ERRLOG("No core devices specified for OCF vbdev '%s'\n", vbdev_name);
			continue;
		}

		status = init_vbdev(vbdev_name, modename, cache_name, core_name);
		if (status) {
			SPDK_ERRLOG("Config initialization failed with code: %d\n", status);
		}
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

/* When base device gets unpluged this is called
 * We will unregister cache vbdev here
 * When cache device is removed, we delete every OCF bdev that used it */
static void
hotremove_cb(void *ctx)
{
	struct vbdev_ocf_base *base = ctx;
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
			base->attached = true;
			return 0;
		}
	}

	status = spdk_bdev_open(base->bdev, true, hotremove_cb, base, &base->desc);
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

	base->attached = true;
	return status;
}

/* Start cache instance and register OCF bdev
 * Callback is not called if this function returns error */
static int
register_vbdev(struct vbdev_ocf *vbdev, void (*cb)(int, void *), void *cb_arg)
{
	int rc;

	if (!(vbdev->core.attached && vbdev->cache.attached)) {
		return -EINVAL;
	}

	rc = vbdev_ocf_mngt_start(vbdev, register_path, cb, cb_arg);
	if (rc) {
		SPDK_ERRLOG("Unable to register OCF bdev: %d\n", rc);
	}

	return rc;
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
int
vbdev_ocf_construct(const char *vbdev_name,
		    const char *cache_mode_name,
		    const char *cache_name,
		    const char *core_name,
		    void (*cb)(int, void *),
		    void *cb_arg)
{
	int rc;
	struct spdk_bdev *cache_bdev = spdk_bdev_get_by_name(cache_name);
	struct spdk_bdev *core_bdev = spdk_bdev_get_by_name(core_name);
	struct vbdev_ocf *vbdev;

	rc = init_vbdev(vbdev_name, cache_mode_name, cache_name, core_name);
	if (rc) {
		return rc;
	}

	vbdev = vbdev_ocf_get_by_name(vbdev_name);
	if (vbdev == NULL) {
		return -ENODEV;
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
		return rc;
	}

	if (core_bdev && cache_bdev) {
		rc = register_vbdev(vbdev, cb, cb_arg);
	} else if (cb) {
		cb(0, cb_arg);
	}

	return rc;
}

/* Attach base bdevs and start OCF vbdev if all base devices are present
 * This is simmilar to vbdev_ocf_construct, but also reports examine_start() and examine_done() */
static int
examine_construct(struct vbdev_ocf *vbdev, struct spdk_bdev *cache, struct spdk_bdev *core)
{
	int rc;
	struct spdk_bdev *bdev = core;

	if (core == NULL) {
		bdev = cache;
	}

	rc = attach_base_bdevs(vbdev, cache, core);
	if (rc) {
		return rc;
	}

	rc = register_vbdev(vbdev, examine_done, bdev);
	if (rc) {
		return rc;
	}

	examine_start(bdev);
	return rc;
}

/* This called if new device is created in SPDK application
 * If that device named as one of base bdevs of OCF vbdev,
 * claim and open them
 * If last device attached here, vbdev starts asynchronously here */
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
			examine_construct(vbdev, bdev, NULL);
			continue;
		}
		if (!strcmp(bdev_name, vbdev->core.name)) {
			examine_construct(vbdev, NULL, bdev);
			break;
		}
	}
	spdk_bdev_module_examine_done(&ocf_if);
}

/* This is called after vbdev_ocf_examine
 * It allows to delay application initialization
 * until all OCF bdevs get registered
 * We do module_examine_done() on all bdevs
 * except for ones that are still used in register_vbdev */
static void
vbdev_ocf_examine_disk(struct spdk_bdev *bdev)
{
	if (examine_isdone(bdev)) {
		spdk_bdev_module_examine_done(&ocf_if);
	}
}

static int
vbdev_ocf_get_ctx_size(void)
{
	return sizeof(struct bdev_ocf_data);
}

/* Module-global function table
 * Does not relate to vbdev instances */
static struct spdk_bdev_module ocf_if = {
	.name = "ocf",
	.module_init = vbdev_ocf_init,
	.fini_start = NULL,
	.module_fini = vbdev_ocf_module_fini,
	.config_text = NULL,
	.get_ctx_size = vbdev_ocf_get_ctx_size,
	.examine_config = vbdev_ocf_examine,
	.examine_disk   = vbdev_ocf_examine_disk,
};
SPDK_BDEV_MODULE_REGISTER(ocf, &ocf_if);

SPDK_LOG_REGISTER_COMPONENT("vbdev_ocf", SPDK_TRACE_VBDEV_OCF)
