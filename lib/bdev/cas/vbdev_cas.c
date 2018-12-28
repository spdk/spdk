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
#include "dobj.h"
#include "utils.h"
#include "vbdev_cas.h"

#include "spdk/bdev_module.h"
#include "spdk/conf.h"
#include "spdk/io_channel.h"
#include "spdk/string.h"
#include "spdk_internal/log.h"

#define INTEL_OCF_PRODUCT_NAME "Intel(R) CAS"

#define SPDK_GET_BDEV_MODULE(name) &name ## _if

static struct spdk_bdev_module cache_if;

static uint32_t opencas_refcnt = 0;

static int g_queues_count = 100;

static TAILQ_HEAD(, vbdev_cas) g_ocf_vbdev_head
	= TAILQ_HEAD_INITIALIZER(g_ocf_vbdev_head);

static bool g_shutdown_started = false;

/* Free allocated strings and structure itself
 * Used at shutdown only */
static void
free_vbdev(struct vbdev_cas *vbdev)
{
	if (!vbdev) {
		return;
	}

	free(vbdev->name);
	free(vbdev->cache.name);
	free(vbdev->core.name);
	free((void *)vbdev->cfg.device.uuid.data);
	free((void *)vbdev->cfg.core.uuid.data);
	free(vbdev);
}

/* Stop OCF cache object
 * vbdev_cas is not operational after this */
static int
stop_vbdev(struct vbdev_cas *vbdev)
{
	int rc;

	if (vbdev == NULL) {
		return -EFAULT;
	}

	if (vbdev->ocf_cache == NULL) {
		return -EFAULT;
	}

	if (!ocf_cache_is_running(vbdev->ocf_cache)) {
		return -EINVAL;
	}

	rc = ocf_mngt_cache_stop(vbdev->ocf_cache);
	if (rc) {
		SPDK_ERRLOG("Could not stop cache for \"%s\"\n", vbdev->name);
		return rc;
	}

	return rc;
}

/* Release SPDK and OCF objects associated with base */
static int
remove_base(struct vbdev_cas_base *base)
{
	int rc = 0;

	if (base == NULL) {
		return -EFAULT;
	}

	if (!base->attached) {
		SPDK_ERRLOG("base to remove '%s' is already detached\n", base->name);
		return -EINVAL;
	}

	/* Release OCF-part */
	if (base->is_cache) {
		rc = stop_vbdev(base->parent);
	} else if (base->parent->ocf_cache && ocf_cache_is_running(base->parent->ocf_cache)) {
		rc = ocf_mngt_cache_remove_core(base->parent->ocf_cache, base->id, false);
		if (rc) {
			SPDK_ERRLOG("Could not remove core for \"%s\"\n", base->parent->name);
		}
	}

	/* Release SPDK-part */
	if (base->bdev->internal.claim_module != NULL) {
		spdk_bdev_module_release_bdev(base->bdev);
	}
	if (base->desc) {
		spdk_bdev_close(base->desc);
	}

	base->attached = false;
	return rc;
}

/* Context argument of descruct-poller */
struct destruct_context {
	struct vbdev_cas            *vbdev;
	struct spdk_poller          *destruct_poller;
	void (*callback)(int, void *);
	void                        *callback_context;
};

/* Wait for CAS io completion and then stop vbdev */
static int
destruct_poll(void *opaque)
{
	struct destruct_context *ctx = opaque;
	struct vbdev_cas *vbdev = ctx->vbdev;
	struct vbdev_cas_qcxt *q;
	int status = 0;

	TAILQ_FOREACH(q, &vbdev->queues, tailq) {
		if (q->unfinished_io_cnt > 0) {
			return 0;
		}
	}

	if (vbdev->state.started) {
		status = stop_vbdev(vbdev);
		remove_base(&vbdev->core);
		remove_base(&vbdev->cache);
		spdk_bdev_close(vbdev->exp_bdev_desc);
		spdk_io_device_unregister(vbdev, NULL);
	}

	spdk_poller_unregister(&ctx->destruct_poller);

	if (ctx->callback) {
		ctx->callback(status, ctx->callback_context);
	}

	free(ctx);
	return 1;
}

/* Initialize context and register destruct poller */
static int
start_destruct_poller(struct vbdev_cas *vbdev, void (*cb_fn)(int, void *), void *cb_arg)
{
	struct destruct_context *ctx;

	if (vbdev->state.doing_finish) {
		return 0;
	}
	vbdev->state.doing_finish = true;

	ctx = malloc(sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	ctx->vbdev = vbdev;
	ctx->callback = cb_fn;
	ctx->callback_context = cb_arg;

	ctx->destruct_poller = spdk_poller_register(destruct_poll, ctx, 0);
	if (ctx->destruct_poller == NULL) {
		SPDK_ERRLOG("Could not register destruct-poller for CAS bdev %s\n",
			    vbdev->name);
		free(ctx);
		return -EINVAL;
	}

	return 0;
}

/* Stop CAS cache and unregister SPDK bdev */
int
vbdev_cas_delete(struct vbdev_cas *vbdev, void (*cb_fn)(int, void *), void *cb_arg)
{
	int rc;

	rc = start_destruct_poller(vbdev, cb_fn, cb_arg);

	if (vbdev->state.started) {
		spdk_bdev_unregister(&vbdev->exp_bdev, NULL, NULL);
	}

	return rc;
}

/* Register unregister-poller if not already running
 * This function is called during spdk_bdev_unregister */
static int
vbdev_cas_destruct(void *opaque)
{
	struct vbdev_cas *vbdev = opaque;

	return start_destruct_poller(vbdev, NULL, NULL);
}

/* Register unregister-poller if not already running */
static void
selfhotremove(void *opaque)
{
	vbdev_cas_destruct(opaque);
}

/* If vbdev is online, return its object */
struct vbdev_cas *
vbdev_cas_get_by_name(const char *name)
{
	struct vbdev_cas *vbdev;

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
struct vbdev_cas_base *
vbdev_cas_get_base_by_name(const char *name)
{
	struct vbdev_cas *vbdev;

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

/* OCF queue initialization procedure
 * Called during ocf_cache_start */
int
opencas_ctx_queue_init(ocf_queue_t q)
{
	return 0;
}

/* Called during ocf_submit_io, ocf_purge*
 * and any other requests that need to submit io */
void
opencas_ctx_queue_kick(ocf_queue_t q)
{
	ocf_queue_run(q);
}

/* OCF queue deinitialization
 * Called at ocf_cache_stop */
void
opencas_ctx_queue_stop(ocf_queue_t q)
{
}

/* Called from OCF when SPDK_IO is completed */
static void
opencas_io_submit_cb(struct ocf_io *io, int error)
{
	struct spdk_bdev_io *bdev_io = io->priv1;
	struct bdev_ocf_data *data = io->priv2;
	struct vbdev_cas *vbdev = bdev_io->bdev->ctxt;
	struct ocf_queue *q;
	struct vbdev_cas_qcxt *qctx;

	if (error == 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}

	if (!ocf_cache_get_queue(vbdev->ocf_cache, io->io_queue, &q)) {
		qctx = ocf_queue_get_priv(q);
		if (qctx) {
			qctx->unfinished_io_cnt--;
		}
	}

	ocf_io_put(io);
	opencas_data_free(data);
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
		if (SPDK_BDEV_IO_TYPE_WRITE) {
			dir = OCF_WRITE;
		}
		ocf_io_configure(io, offset, len, dir, 0, 0);
		return ocf_submit_io(io);
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
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
	struct vbdev_cas *vbdev = bdev_io->bdev->ctxt;
	struct ocf_io *io;
	struct bdev_ocf_data *data;
	struct vbdev_cas_qcxt *qctx = spdk_io_channel_get_ctx(ch);
	int err;

	io = ocf_new_io(vbdev->ocf_core);
	if (!io) {
		err = -ENOMEM;
		goto fail;
	}

	ocf_io_set_queue(io, qctx->queue_id);
	qctx->unfinished_io_cnt++;

	data = opencas_data_from_spdk_io(bdev_io);
	err = ocf_io_set_data(io, data, 0);
	if (err) {
		goto fail;
	}

	ocf_io_set_cmpl(io, bdev_io, data, opencas_io_submit_cb);

	err = io_submit_to_ocf(bdev_io, io);
	if (err) {
		goto fail;
	}

	return;

fail:
	opencas_io_submit_cb(io, err);
}

/* Called from bdev layer when an io to Cache vbdev is submitted */
static void
vbdev_cas_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		/* User does not have to allocate io vectors for the request,
		 * so in case they are not allocated, we allocate them here */
		spdk_bdev_io_get_buf(bdev_io, io_handle,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		io_handle(ch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_FLUSH:
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
vbdev_cas_io_type_supported(void *opaque, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	default:
		return false;
	}
}

/* Called from bdev layer */
static struct spdk_io_channel *
vbdev_cas_get_io_channel(void *opaque)
{
	struct vbdev_cas *bdev = opaque;

	return spdk_get_io_channel(bdev);
}

static int
vbdev_cas_dump_config_json(void *opaque, struct spdk_json_write_ctx *w)
{
	struct vbdev_cas *vbdev = opaque;

	spdk_json_write_named_object_begin(w, "cache_device");
	spdk_json_write_named_string(w, "name", vbdev->cache.name);
	spdk_json_write_named_uint32(w, "internal_id", vbdev->cache.id);
	spdk_json_write_named_uint32(w, "core_count", ocf_cache_get_core_count(vbdev->ocf_cache));
	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "core_device");
	spdk_json_write_named_string(w, "name", vbdev->core.name);
	spdk_json_write_named_uint32(w, "internal_id", vbdev->core.id);
	spdk_json_write_object_end(w);

	spdk_json_write_named_string(w, "mode",
				     ocf_get_cache_modename(ocf_cache_get_mode(vbdev->ocf_cache)));
	spdk_json_write_named_uint32(w, "cache_line_size",
				     ocf_cache_get_line_size(vbdev->ocf_cache));
	spdk_json_write_named_bool(w, "metadata_volatile",
				   vbdev->cfg.cache.metadata_volatile);

	return 0;
}

/* OCF module cleanup */
static void
opencas_cleanup(void)
{
	opencas_refcnt--;
	if (opencas_refcnt) {
		return;
	}

	opencas_dobj_cleanup();
	opencas_ctx_cleanup();
}

/* OCF module initialization */
static int
opencas_init(void)
{
	int ret;

	opencas_refcnt++;
	if (opencas_refcnt > 1) {
		return 0;
	}

	ret = opencas_ctx_init();
	if (ret) {
		opencas_refcnt--;
		return ret;
	}

	ret = opencas_dobj_init();
	if (ret) {
		opencas_ctx_cleanup();
		opencas_refcnt--;
		return ret;
	}

	return 0;
}

/* Cache vbdev function table
 * Used by bdev layer */
static struct spdk_bdev_fn_table cache_dev_fn_table = {
	.destruct = vbdev_cas_destruct,
	.io_type_supported = vbdev_cas_io_type_supported,
	.submit_request	= vbdev_cas_submit_request,
	.get_io_channel	= vbdev_cas_get_io_channel,
	.dump_info_json = vbdev_cas_dump_config_json,
};

/* Start OCF cache, attach caching device */
static int
start_cache(struct vbdev_cas *vbdev)
{
	int rc;

	rc = ocf_mngt_cache_start(opencas_ctx, &vbdev->ocf_cache, &vbdev->cfg.cache);
	if (rc) {
		SPDK_ERRLOG("Failed to start cache instance\n");
		return rc;
	}
	vbdev->cache.id = ocf_cache_get_id(vbdev->ocf_cache);

	rc = ocf_mngt_cache_attach(vbdev->ocf_cache, &vbdev->cfg.device);
	if (rc) {
		SPDK_ERRLOG("Failed to attach cache device\n");
		return rc;
	}

	return 0;
}

/* Add core for existing OCF cache instance */
static int
add_core(struct vbdev_cas *vbdev)
{
	int rc;

	rc = ocf_mngt_cache_add_core(vbdev->ocf_cache, &vbdev->ocf_core, &vbdev->cfg.core);
	if (rc) {
		SPDK_ERRLOG("Failed to add core device to cache instance\n");
		return rc;
	}

	vbdev->core.id = ocf_core_get_id(vbdev->ocf_core);

	return 0;
}

/* Called on cache vbdev creation at every thread */
static int
io_device_create_cb(void *io_device, void *ctx_buf)
{
	struct vbdev_cas *vbdev = io_device;
	struct vbdev_cas_qcxt *qctx = ctx_buf, *tmp;
	int max_queue_id = 0;
	int rc;

	TAILQ_FOREACH(tmp, &vbdev->queues, tailq) {
		max_queue_id = tmp->queue_id > max_queue_id ? tmp->queue_id : max_queue_id;
	}

	/* Queue ids used in SPDK IO path start from 1 */
	qctx->queue_id = max_queue_id + 1;

	if (qctx->queue_id >= g_queues_count) {
		SPDK_ERRLOG("CAS queues size is too small, try to allocate more than %d\n",
			    qctx->queue_id);
		return -EINVAL;
	}

	TAILQ_INSERT_TAIL(&vbdev->queues, qctx, tailq);

	rc = ocf_cache_get_queue(vbdev->ocf_cache, qctx->queue_id, &qctx->queue);
	if (rc) {
		SPDK_ERRLOG("Could not get CAS queue #%d\n", qctx->queue_id);
		assert(false);
		return rc;
	}

	qctx->th         = spdk_get_thread();
	qctx->vbdev      = vbdev;
	qctx->cache_ch   = spdk_bdev_get_io_channel(vbdev->cache.desc);
	qctx->core_ch    = spdk_bdev_get_io_channel(vbdev->core.desc);

	ocf_queue_set_priv(qctx->queue, qctx);

	return rc;
}

static void
io_device_destroy_cb(void *io_device, void *ctx_buf)
{
	struct vbdev_cas_qcxt *qctx = ctx_buf;

	spdk_put_io_channel(qctx->cache_ch);
	spdk_put_io_channel(qctx->core_ch);
}

/* Start OCF cache and register vbdev_cas at bdev layer */
static int
register_vbdev(struct vbdev_cas *vbdev)
{
	int result;

	result = start_cache(vbdev);
	if (result) {
		SPDK_ERRLOG("Failed to start cache instance\n");
		return result;
	}

	result = add_core(vbdev);
	if (result) {
		SPDK_ERRLOG("Failed to add core to cache instance\n");
		return result;
	}

	/* Create exported spdk object */

	/* Copy properties of the base bdev */
	vbdev->exp_bdev.blocklen = vbdev->core.bdev->blocklen;
	vbdev->exp_bdev.write_cache = vbdev->core.bdev->write_cache;
	vbdev->exp_bdev.required_alignment = vbdev->core.bdev->required_alignment;

	vbdev->exp_bdev.name = vbdev->name;
	vbdev->exp_bdev.product_name = INTEL_OCF_PRODUCT_NAME;

	vbdev->exp_bdev.blockcnt = vbdev->core.bdev->blockcnt;
	vbdev->exp_bdev.ctxt = vbdev;
	vbdev->exp_bdev.fn_table = &cache_dev_fn_table;
	vbdev->exp_bdev.module = SPDK_GET_BDEV_MODULE(cache);

	/* Finally register vbdev in SPDK */
	spdk_io_device_register(vbdev, io_device_create_cb, io_device_destroy_cb,
				sizeof(struct vbdev_cas_qcxt), vbdev->name);
	result = spdk_bdev_register(&vbdev->exp_bdev);
	if (result) {
		SPDK_ERRLOG("Could not register exposed bdev\n");
		return result;
	}

	/* Open descriptor to ourselves. This will allow for async unregister later */
	result = spdk_bdev_open(&vbdev->exp_bdev, true, selfhotremove, vbdev, &vbdev->exp_bdev_desc);
	if (result) {
		SPDK_ERRLOG("Could not open self-descriptor\n");
		return result;
	}

	vbdev->state.started = true;

	return result;
}

/* Open base SPDK bdev for writing */
static int
open_base(struct vbdev_cas_base *base)
{
	int status;

	status = spdk_bdev_open(base->bdev, true, NULL, NULL, &base->desc);
	if (status) {
		SPDK_ERRLOG("Can't open device %s for writing\n", base->name);
		return status;
	}

	return status;
}

/* Claim base SPDK bdev
 * Must be called on the same thread as open_base */
static int
claim_base(struct vbdev_cas_base *base)
{
	int status;

	status = spdk_bdev_module_claim_bdev(base->bdev, base->desc,
					     SPDK_GET_BDEV_MODULE(cache));
	if (status) {
		spdk_bdev_close(base->desc);
		SPDK_ERRLOG("Can't claim bdev %s\n", base->name);
	}

	return status;
}

/* Open base bdevs, claim them and register main CAS vbdev */
static int
claim_vbdev(struct vbdev_cas *vbdev)
{
	int status;

	if (!vbdev->cache.attached || !vbdev->core.attached) {
		return -EPERM;
	}

	/* Open devices for writing */
	status = open_base(&vbdev->cache);
	if (status) {
		return status;
	}
	status = open_base(&vbdev->core);
	if (status) {
		return status;
	}

	/* Claim base bdevs for exclusive use */
	status = claim_base(&vbdev->cache);
	if (status) {
		return status;
	}
	status = claim_base(&vbdev->core);
	if (status) {
		return status;
	}

	status = register_vbdev(vbdev);
	if (status) {
		SPDK_ERRLOG("Error while create cache instance status=%d\n", status);
	}

	return status;
}

/* Init OCF configuration options
 * for core and cache devices
 * Also allocate context for them */
static int
init_vbdev_config(struct vbdev_cas *vbdev)
{
	vbdev->cfg.cache.id = 0;
	vbdev->cfg.cache.name = vbdev->name;
	vbdev->cfg.cache.name_size = strlen(vbdev->name) + 1;
	vbdev->cfg.cache.metadata_volatile = true;
	vbdev->cfg.cache.cache_line_size = ocf_cache_line_size_4;
	vbdev->cfg.cache.backfill.max_queue_size = 65536;
	vbdev->cfg.cache.backfill.queue_unblock_size = 60000;

	/* At this moment CAS queues count is static
	 * so we choose some value for it
	 * It has to be bigger than SPDK thread count */
	vbdev->cfg.cache.io_queues = g_queues_count;

	vbdev->cfg.device.cache_line_size = ocf_cache_line_size_4;
	vbdev->cfg.device.force = true;
	vbdev->cfg.device.min_free_ram = 2000;
	vbdev->cfg.device.perform_test = false;
	vbdev->cfg.device.discard_on_start = false;

	vbdev->cfg.core.data_obj_type = SPDK_OBJECT;

	vbdev->cfg.device.uuid.data = strdup(vbdev->cache.name);
	vbdev->cfg.device.uuid.size = strlen(vbdev->cache.name) + 1;
	vbdev->cfg.core.uuid.data = strdup(vbdev->core.name);
	vbdev->cfg.core.uuid.size = strlen(vbdev->core.name) + 1;

	return 0;
}

/* Allocate vbdev structure object and add it to the global list */
static int
init_vbdev(const char *vbdev_name,
	   const char *cache_mode_name,
	   const char *cache_name,
	   const char *core_name)
{
	int rc;
	struct vbdev_cas *vbdev;

	if (spdk_bdev_get_by_name(vbdev_name) || vbdev_cas_get_by_name(vbdev_name)) {
		SPDK_ERRLOG("Device with name \"%s\" already exists", vbdev_name);
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
	TAILQ_INIT(&vbdev->queues);

	if (cache_mode_name) {
		vbdev->cfg.cache.cache_mode
			= ocf_get_cache_mode(cache_mode_name);
	} else {
		SPDK_ERRLOG("No cache mode specified\n");
		rc = -EINVAL;
		goto error_free;
	}
	if (vbdev->cfg.cache.cache_mode < 0) {
		SPDK_ERRLOG("Incorrect cache mode \"%s\"\n", cache_mode_name);
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

	rc = init_vbdev_config(vbdev);
	if (rc) {
		goto error_free;
	}

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
vbdev_cas_init(void)
{
	const char *vbdev_name, *modename, *cache_name, *core_name;
	struct spdk_conf_section *sp;
	int status;

	status = opencas_init();
	if (status) {
		SPDK_ERRLOG("OCF ctx initialization failed with=%d\n", status);
		return status;
	}

	sp = spdk_conf_find_section(NULL, "CAS");
	if (sp == NULL) {
		return 0;
	}

	for (int i = 0; ; i++) {
		if (!spdk_conf_section_get_nval(sp, "CAS", i)) {
			break;
		}

		vbdev_name = spdk_conf_section_get_nmval(sp, "CAS", i, 0);
		if (!vbdev_name) {
			SPDK_ERRLOG("No vbdev name specified\n");
			continue;
		}

		modename = spdk_conf_section_get_nmval(sp, "CAS", i, 1);
		if (!modename) {
			SPDK_ERRLOG("No modename specified for CAS vbdev \"%s\"\n", vbdev_name);
			continue;
		}

		cache_name = spdk_conf_section_get_nmval(sp, "CAS", i, 2);
		if (!cache_name) {
			SPDK_ERRLOG("No cache device specified for CAS vbdev \"%s\"\n", vbdev_name);
			continue;
		}

		core_name = spdk_conf_section_get_nmval(sp, "CAS", i, 3);
		if (!core_name) {
			SPDK_ERRLOG("No core devices specified for CAS vbdev \"%s\"\n", vbdev_name);
			continue;
		}

		status = init_vbdev(vbdev_name, modename, cache_name, core_name);
		if (status) {
			SPDK_ERRLOG("Config initialization failed with code: %d\n", status);
		}
	}

	return status;
}

/* Called at application shutdown */
static void
vbdev_cas_fini_start(void)
{
	g_shutdown_started = true;
}

/* Called after application shutdown started
 * Release memory of allocated structures here */
static void
vbdev_cas_module_fini(void)
{
	struct vbdev_cas *vbdev;

	while ((vbdev = TAILQ_FIRST(&g_ocf_vbdev_head))) {
		TAILQ_REMOVE(&g_ocf_vbdev_head, vbdev, tailq);
		free_vbdev(vbdev);
	}

	opencas_cleanup();
}

/* Attach base bdevs
 * If they attached, start vbdev
 * otherwise wait for them to appear at examine */
static int
create_from_bdevs(struct vbdev_cas *vbdev,
		  struct spdk_bdev *cache_bdev, struct spdk_bdev *core_bdev)
{
	int rc = 0;

	if (cache_bdev) {
		if (vbdev->cache.attached) {
			rc = -1;
		} else {
			vbdev->cache.bdev = cache_bdev;
			vbdev->cache.attached = true;
		}
	}

	if (core_bdev) {
		if (vbdev->core.attached) {
			rc = -1;
		} else {
			vbdev->core.bdev = core_bdev;
			vbdev->core.attached = true;
		}
	}

	if (rc == 0 && vbdev->core.attached && vbdev->cache.attached) {
		rc = claim_vbdev(vbdev);
	}

	return rc;
}

/* Init and then start vbdev if all base devices are present */
int
vbdev_cas_construct(const char *vbdev_name,
		    const char *cache_mode_name,
		    const char *cache_name,
		    const char *core_name)
{
	int rc;
	struct spdk_bdev *cache_bdev = spdk_bdev_get_by_name(cache_name);
	struct spdk_bdev *core_bdev = spdk_bdev_get_by_name(core_name);
	struct vbdev_cas *vbdev;

	rc = init_vbdev(vbdev_name, cache_mode_name, cache_name, core_name);
	if (rc) {
		return rc;
	}

	vbdev = vbdev_cas_get_by_name(vbdev_name);
	if (vbdev == NULL) {
		return -EFAULT;
	}

	if (cache_bdev == NULL) {
		SPDK_NOTICELOG("Cache vbdev \"%s\" is waiting for cache device \"%s\" to connect",
			       vbdev->name, cache_name);
	}
	if (core_bdev == NULL) {
		SPDK_NOTICELOG("Cache vbdev \"%s\" is waiting for core device \"%s\" to connect",
			       vbdev->name, core_name);
	}

	return create_from_bdevs(vbdev, cache_bdev, core_bdev);
}

/* This called if new device is created in SPDK application
 * If that device named as one of base bdevs of cache_vbdev,
 * attach them
 * If last device attached here, vbdev starts here */
static void
vbdev_cas_examine(struct spdk_bdev *bdev)
{
	const char *bdev_name = spdk_bdev_get_name(bdev);
	struct vbdev_cas *vbdev;

	TAILQ_FOREACH(vbdev, &g_ocf_vbdev_head, tailq) {
		if (vbdev->state.doing_finish) {
			continue;
		}

		if (!strcmp(bdev_name, vbdev->cache.name)) {
			create_from_bdevs(vbdev, bdev, NULL);
			break;
		}
		if (!strcmp(bdev_name, vbdev->core.name)) {
			create_from_bdevs(vbdev, NULL, bdev);
			break;
		}
	}
	spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(cache));
}

/* Module-global function table
 * Does not relate to vbdev instances */
static struct spdk_bdev_module cache_if = {
	.name = "cache",
	.module_init = vbdev_cas_init,
	.fini_start = vbdev_cas_fini_start,
	.module_fini = vbdev_cas_module_fini,
	.config_text = NULL,
	.get_ctx_size = NULL,
	.examine_config = vbdev_cas_examine,
};
SPDK_BDEV_MODULE_REGISTER(&cache_if);

SPDK_LOG_REGISTER_COMPONENT("vbdev_cas", SPDK_TRACE_VBDEV_CACHE)
