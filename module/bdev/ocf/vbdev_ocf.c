/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2025 Huawei Technologies
 *   All rights reserved.
 */

#include <ocf/ocf.h>

#include "spdk/bdev_module.h"
#include "spdk/string.h" // rm ?

#include "vbdev_ocf.h"
#include "vbdev_ocf_cache.h"
#include "vbdev_ocf_core.h"
#include "ctx.h"
#include "data.h"
#include "volume.h"

/* This namespace UUID was generated using uuid_generate() method. */
#define BDEV_OCF_NAMESPACE_UUID "f92b7f49-f6c0-44c8-bd23-3205e8c3b6ad"

static int vbdev_ocf_module_init(void);
static void vbdev_ocf_module_fini_start(void);
static void vbdev_ocf_module_fini(void);
static int vbdev_ocf_module_get_ctx_size(void);

struct spdk_bdev_module ocf_if = {
	.name = "OCF",
	.module_init = vbdev_ocf_module_init,
	.fini_start = vbdev_ocf_module_fini_start,
	.module_fini = vbdev_ocf_module_fini,
	.get_ctx_size = vbdev_ocf_module_get_ctx_size,
	.examine_config = NULL, // todo
	.examine_disk = NULL, // todo ?
	.async_fini_start = true,
};

SPDK_BDEV_MODULE_REGISTER(ocf, &ocf_if)

static int vbdev_ocf_fn_destruct(void *ctx);
static void vbdev_ocf_fn_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);
static bool vbdev_ocf_fn_io_type_supported(void *ctx, enum spdk_bdev_io_type);
static struct spdk_io_channel *vbdev_ocf_fn_get_io_channel(void *ctx);
static int vbdev_ocf_fn_dump_info_json(void *ctx, struct spdk_json_write_ctx *w);
static void vbdev_ocf_fn_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w);

struct spdk_bdev_fn_table vbdev_ocf_fn_table = {
	.destruct = vbdev_ocf_fn_destruct,
	.submit_request = vbdev_ocf_fn_submit_request,
	.io_type_supported = vbdev_ocf_fn_io_type_supported,
	.get_io_channel = vbdev_ocf_fn_get_io_channel,
	.dump_info_json = vbdev_ocf_fn_dump_info_json,
	.write_config_json = vbdev_ocf_fn_write_config_json,
	.dump_device_stat_json = NULL, // todo ?
	.reset_device_stat = NULL, // todo ?
};

// rm
#define __bdev_to_io_dev(bdev)          (((char *)bdev) + 1)

static bool
vbdev_ocf_device_exists(const char *name) {
	struct vbdev_ocf_cache *cache;
	struct vbdev_ocf_core *core;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF: looking for '%s' in existing device names\n", name);

	vbdev_ocf_foreach_core_incomplete(core) {
		if (!strcmp(name, core->name)) {
			return true;
		}
	}
	vbdev_ocf_foreach_cache(cache) {
		if (!strcmp(name, cache->name)) {
			return true;
		}
		vbdev_ocf_foreach_core_in_cache(core, cache) {
			if (!strcmp(name, core->name)) {
				return true;
			}
		}
	}

	return false;
}

static int
vbdev_ocf_module_init(void)
{
	int rc = 0;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF: starting module\n");

	if ((rc = vbdev_ocf_ctx_init())) {
		SPDK_ERRLOG("OCF: failed to initialize context: %d\n", rc);
		return rc;
	}

	if ((rc = vbdev_ocf_volume_init())) {
		vbdev_ocf_ctx_cleanup();
		SPDK_ERRLOG("OCF: failed to register volume: %d\n", rc);
		return rc;
	}

	return rc;
}

static void
_cache_stop_module_fini_stop_cb(ocf_cache_t ocf_cache, void *cb_arg, int error)
{
	struct vbdev_ocf_cache *cache = cb_arg;
	struct vbdev_ocf_core *core;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF cache '%s': finishing stop of OCF cache\n", cache->name);

	ocf_mngt_cache_unlock(ocf_cache);

	if (error) {
		SPDK_ERRLOG("OCF cache '%s': failed to stop OCF cache (OCF error: %d)\n",
			    cache->name, error);
		return;
	}

	vbdev_ocf_foreach_core_in_cache(core, cache) {
		/* It's important to set ocf_core back to NULL before unregistering.
		 * This is an indicator for destruct that OCF cache isn't there any more. */
		core->ocf_core = NULL;
	}

	vbdev_ocf_cache_base_detach(cache);

	if (cache == STAILQ_LAST(&g_vbdev_ocf_caches, vbdev_ocf_cache, link)) {
		spdk_bdev_module_fini_start_done();
	}
}

static void
_cache_stop_module_fini_flush_cb(ocf_cache_t ocf_cache, void *cb_arg, int error)
{
	struct vbdev_ocf_cache *cache = cb_arg;

	if (error) {
		SPDK_ERRLOG("OCF cache '%s': failed to flush OCF cache (OCF error: %d)\n",
			    cache->name, error);
		ocf_mngt_cache_unlock(ocf_cache);
		return;
	}

	ocf_mngt_cache_stop(ocf_cache, _cache_stop_module_fini_stop_cb, cache);
}

static void
_cache_stop_module_fini_lock_cb(ocf_cache_t ocf_cache, void *lock_arg, int lock_err)
{
	struct vbdev_ocf_cache *cache = lock_arg;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF cache '%s': initiating stop of OCF cache\n", cache->name);

	if (lock_err) {
		SPDK_ERRLOG("OCF cache '%s': failed to acquire OCF cache lock (OCF error: %d)\n",
			    cache->name, lock_err);
		return;
	}

	if (ocf_mngt_cache_is_dirty(ocf_cache)) {
		ocf_mngt_cache_flush(ocf_cache, _cache_stop_module_fini_flush_cb, cache);
	} else {
		ocf_mngt_cache_stop(ocf_cache, _cache_stop_module_fini_stop_cb, cache);
	}
}

static void
vbdev_ocf_module_fini_start(void)
{
	struct vbdev_ocf_cache *cache;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF: initiating module stop\n");

	if (STAILQ_EMPTY(&g_vbdev_ocf_caches)) {
		spdk_bdev_module_fini_start_done();
		return;
	}

	/* Stop all OCF caches before unregistering all bdevs. */
	vbdev_ocf_foreach_cache(cache) {
		ocf_mngt_cache_lock(cache->ocf_cache, _cache_stop_module_fini_lock_cb, cache);
	}
}

static void
vbdev_ocf_module_fini(void)
{
	struct vbdev_ocf_cache *cache;
	struct vbdev_ocf_core *core;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF: finishing module stop\n");

	while ((core = STAILQ_FIRST(&g_vbdev_ocf_incomplete_cores))) {
		vbdev_ocf_core_remove_incomplete(core);
		vbdev_ocf_core_destroy(core);
	}
	while ((cache = STAILQ_FIRST(&g_vbdev_ocf_caches))) {
		if (vbdev_ocf_cache_is_incomplete(cache)) {
			/* If cache is incomplete it won't be started yet and won't have
			 * any cores attached, thus just removing init params here. */
			vbdev_ocf_cache_remove_incomplete(cache);
		} else {
			while ((core = STAILQ_FIRST(&cache->cores))) {
				vbdev_ocf_core_remove_from_cache(core);
				vbdev_ocf_core_destroy(core);
			}
		}
		vbdev_ocf_cache_destroy(cache);
	}

	vbdev_ocf_volume_cleanup();
	vbdev_ocf_ctx_cleanup();
}

static int
vbdev_ocf_module_get_ctx_size(void)
{
	return sizeof(struct vbdev_ocf_data);
}

static void
_io_device_unregister_cb(void *io_device)
{
	struct vbdev_ocf_core *core = io_device;

	vbdev_ocf_core_base_detach(core);

	/* This one finally calls the callback from spdk_bdev_unregister_by_name(). */
	spdk_bdev_destruct_done(&core->ocf_vbdev, 0);
}

// remove this wrapper ?
static void
vbdev_ocf_destruct_done(struct vbdev_ocf_core *core)
{
	SPDK_DEBUGLOG(vbdev_ocf, "OCF vbdev '%s': finishing destruct\n", core->ocf_vbdev.name);

	spdk_io_device_unregister(core, _io_device_unregister_cb);
}

static void
_core_remove_destruct_remove_cb(void *cb_arg, int error)
{
	struct vbdev_ocf_core *core = cb_arg;
	struct vbdev_ocf_cache *cache = vbdev_ocf_core_get_cache(core);

	SPDK_DEBUGLOG(vbdev_ocf, "OCF vbdev '%s': finishing remove of OCF core\n",
		      core->ocf_vbdev.name);

	ocf_mngt_cache_unlock(cache->ocf_cache);

	if (error) {
		SPDK_ERRLOG("OCF vbdev '%s': failed to remove OCF core device (OCF error: %d)\n",
			      core->ocf_vbdev.name, error);
		spdk_bdev_destruct_done(&core->ocf_vbdev, error);
		return;
	}

	vbdev_ocf_core_remove_from_cache(core);

	vbdev_ocf_destruct_done(core);
}

static void
_core_remove_destruct_flush_cb(ocf_core_t ocf_core, void *cb_arg, int error)
{
	struct vbdev_ocf_core *core = cb_arg;

	assert(ocf_core == core->ocf_core);

	if (error) { // WARN only ? (may be hotremoved)
		SPDK_ERRLOG("OCF vbdev '%s': failed to flush OCF core device (OCF error: %d)\n",
			      core->ocf_vbdev.name, error);
		ocf_mngt_cache_unlock(vbdev_ocf_core_get_cache(core)->ocf_cache);
		spdk_bdev_destruct_done(&core->ocf_vbdev, error);
		return;
	}

	ocf_mngt_cache_remove_core(ocf_core, _core_remove_destruct_remove_cb, core);
}

static void
_core_remove_destruct_lock_cb(ocf_cache_t ocf_cache, void *lock_arg, int lock_err)
{
	struct vbdev_ocf_core *core = lock_arg;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF vbdev '%s': initiating remove of OCF core\n",
		      core->ocf_vbdev.name);

	if (lock_err) {
		SPDK_ERRLOG("OCF vbdev '%s': failed to acquire OCF cache lock (OCF error: %d)\n",
			      core->ocf_vbdev.name, lock_err);
		spdk_bdev_destruct_done(&core->ocf_vbdev, lock_err);
		return;
	}

	if (ocf_mngt_core_is_dirty(core->ocf_core)) {
		ocf_mngt_core_flush(core->ocf_core, _core_remove_destruct_flush_cb, core);
	} else {
		ocf_mngt_cache_remove_core(core->ocf_core, _core_remove_destruct_remove_cb, core);
	}
}

/* This is called internally by SPDK during vbdev_ocf_core_unregister(). */
static int
vbdev_ocf_fn_destruct(void *ctx)
{
	struct vbdev_ocf_core *core = ctx;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF vbdev '%s': initiating destruct\n", core->ocf_vbdev.name);

	if (vbdev_ocf_core_cache_is_started(core)) {
		ocf_mngt_cache_lock(vbdev_ocf_core_get_cache(core)->ocf_cache,
				    _core_remove_destruct_lock_cb, core);
	} else {
		vbdev_ocf_destruct_done(core);
	}

	/* Return one to indicate async destruct. */
	return 1;
}

static void
_vbdev_ocf_submit_io_cb(ocf_io_t io, void *priv1, void *priv2, int error)
{
	struct spdk_bdev_io *bdev_io = priv1;

	// rm ?
	SPDK_DEBUGLOG(vbdev_ocf, "OCF vbdev '%s': finishing submit of IO request\n",
		      bdev_io->bdev->name);

	ocf_io_put(io);

	if (error == -OCF_ERR_NO_MEM) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
	} else if (error) {
		SPDK_ERRLOG("OCF vbdev '%s': failed to complete OCF IO\n", bdev_io->bdev->name);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	} else {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	}
}

typedef void (*submit_io_to_ocf_fn)(ocf_io_t io);

static void
vbdev_ocf_submit_io(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, uint64_t offset,
		    uint32_t len, uint32_t dir, uint64_t flags, submit_io_to_ocf_fn submit_io_fn)
{
	struct vbdev_ocf_core *core = bdev_io->bdev->ctxt;
	struct vbdev_ocf_data *data = (struct vbdev_ocf_data *)bdev_io->driver_ctx;
	struct vbdev_ocf_core_io_channel_ctx *ch_ctx = spdk_io_channel_get_ctx(ch);
	ocf_io_t io = NULL;

	// impossible to be true ?
	if (!core->ocf_core) {
		SPDK_ERRLOG("OCF vbdev '%s': failed to submit IO - no OCF core device\n",
			    bdev_io->bdev->name);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	
	io = ocf_volume_new_io(ocf_core_get_front_volume(core->ocf_core), ch_ctx->queue,
			       offset, len, dir, 0, flags);
	if (!io) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		return;
	}

	data->iovs = bdev_io->u.bdev.iovs;
	data->iovcnt = bdev_io->u.bdev.iovcnt;
	data->size = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;

	ocf_io_set_data(io, data, 0);
	ocf_io_set_cmpl(io, bdev_io, NULL, _vbdev_ocf_submit_io_cb);
	submit_io_fn(io);
}

static void
_io_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	uint64_t offset = bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen;
	uint32_t len = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;

	if (!success) {
		SPDK_ERRLOG("OCF vbdev '%s': failed to allocate IO buffer - size of the "
			    "buffer to allocate might be greater than the permitted maximum\n",
			    bdev_io->bdev->name);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	vbdev_ocf_submit_io(ch, bdev_io, offset, len, OCF_READ, 0, ocf_core_submit_io);
}

static void
vbdev_ocf_fn_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	uint64_t offset = bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen;
	uint32_t len = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;

	// rm ?
	SPDK_DEBUGLOG(vbdev_ocf, "OCF vbdev '%s': initiating submit of IO request\n",
		      bdev_io->bdev->name);

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		// align buffer for write as well ? (comment in old vbdev_ocf.c)
		// from doc: This function *must* be called from the thread issuing bdev_io.
		spdk_bdev_io_get_buf(bdev_io, _io_read_get_buf_cb, len);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		vbdev_ocf_submit_io(ch, bdev_io, offset, len, OCF_WRITE, 0, ocf_core_submit_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		vbdev_ocf_submit_io(ch, bdev_io, offset, len, OCF_WRITE, 0, ocf_core_submit_discard);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		vbdev_ocf_submit_io(ch, bdev_io, 0, 0, OCF_WRITE, OCF_WRITE_FLUSH, ocf_core_submit_flush);
		break;
	default:
		SPDK_ERRLOG("OCF vbdev '%s': unsupported IO type: %s\n", bdev_io->bdev->name,
			    spdk_bdev_get_io_type_name(bdev_io->type));
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
vbdev_ocf_fn_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct vbdev_ocf_core *core = ctx;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF vbdev '%s': checking if IO type '%s' is supported\n",
		      core->ocf_vbdev.name, spdk_bdev_get_io_type_name(io_type));

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		return spdk_bdev_io_type_supported(core->base.bdev, io_type);
	default:
		return false;
	}
}

static struct spdk_io_channel *
vbdev_ocf_fn_get_io_channel(void *ctx) // ctx == ocf_vbdev.ctxt
{
	struct vbdev_ocf_core *core = ctx;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF vbdev '%s': got request for IO channel\n", core->ocf_vbdev.name);

	return spdk_get_io_channel(core);
}

static int
vbdev_ocf_fn_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	return 0;
}

static void
vbdev_ocf_fn_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
}

static void
_cache_stop_err_cb(ocf_cache_t ocf_cache, void *cb_arg, int error)
{
	struct vbdev_ocf_cache *cache = cb_arg;

	ocf_mngt_cache_unlock(ocf_cache);

	if (error) {
		SPDK_ERRLOG("OCF cache '%s': failed to stop OCF cache (OCF error: %d)\n",
			    cache->name, error);
		return;
	}
}

static void
vbdev_ocf_cache_start_rollback(struct vbdev_ocf_cache *cache)
{
	vbdev_ocf_queue_put(cache->ocf_cache_mngt_q);
	ocf_mngt_cache_stop(cache->ocf_cache, _cache_stop_err_cb, cache);
	vbdev_ocf_cache_base_detach(cache);
	vbdev_ocf_cache_destroy(cache);
}

static void
_cache_start_rpc_cb(ocf_cache_t ocf_cache, void *cb_arg, int error)
{
	struct vbdev_ocf_cache_start_ctx *cache_start_ctx = cb_arg;
	struct vbdev_ocf_cache *cache = cache_start_ctx->cache;
	int rc = 0; // rm ?

	SPDK_DEBUGLOG(vbdev_ocf, "OCF cache '%s': finishing start\n", cache->name);

	ocf_volume_destroy(cache->ocf_cache_att_cfg.device.volume);

	if (error) {
		SPDK_ERRLOG("OCF cache '%s': failed to attach OCF cache device\n",
			    cache->name);
		// here or in ocf_mngt_cache_start() ?
		if (error == -OCF_ERR_NO_MEM) {
			uint64_t mem_needed, volume_size;

			volume_size = cache->base.bdev->blockcnt * cache->base.bdev->blocklen;
			mem_needed = ocf_mngt_get_ram_needed(ocf_cache, volume_size);
			SPDK_ERRLOG("Not enough memory. Try to increase hugepage memory size or cache line size.\n");
			SPDK_NOTICELOG("Needed memory to start cache in this configuration "
				       "(device size: %"PRIu64", cache line size: %"PRIu64"): %"PRIu64"\n",
				       volume_size, cache->ocf_cache_cfg.cache_line_size, mem_needed);
		}
		vbdev_ocf_cache_start_rollback(cache);
		cache_start_ctx->rpc_cb_fn(NULL, cache_start_ctx->rpc_cb_arg, error);
		free(cache_start_ctx);
		return;
	}

	SPDK_NOTICELOG("OCF cache '%s': started\n", cache->name);

	// check for cores in g_vbdev_ocf_incomplete_cores
	// check if (cache_block_size > core_block_size)
	
	ocf_mngt_cache_unlock(ocf_cache);
	cache_start_ctx->rpc_cb_fn(cache, cache_start_ctx->rpc_cb_arg, rc);
	free(cache_start_ctx);
}

/* RPC entry point. */
void
vbdev_ocf_cache_start(const char *cache_name, const char *bdev_name, const char *cache_mode,
		      const uint8_t cache_line_size, vbdev_ocf_cache_start_cb rpc_cb_fn, void *rpc_cb_arg)
{
	struct vbdev_ocf_cache *cache;
	struct vbdev_ocf_cache_start_ctx *cache_start_ctx;
	struct ocf_volume_uuid volume_uuid;
	int rc = 0;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF cache '%s': initiating start\n", cache_name);

	if (vbdev_ocf_device_exists(cache_name)) {
		SPDK_ERRLOG("OCF: device '%s' already exists\n", cache_name);
		rc = -EEXIST;
		goto err_create;
	}

	/* Allocate memory for cache struct and put it on the global cache list. */
	if ((rc = vbdev_ocf_cache_create(cache_name, &cache))) {
		SPDK_ERRLOG("OCF cache '%s': failed to create cache\n", cache_name);
		goto err_create;
	}

	/* Create OCF configs for cache and attach. */
	if ((rc = vbdev_ocf_cache_set_config(cache, cache_mode, cache_line_size))) {
		SPDK_ERRLOG("OCF cache '%s': failed to create OCF config\n", cache_name);
		goto err_base;
	}

	/* Check if base device for this cache is already present. */
	if ((rc = vbdev_ocf_cache_base_attach(cache, bdev_name))) {
		if (rc == -ENODEV) {
			SPDK_NOTICELOG("OCF cache '%s': start deferred - waiting for base bdev '%s'\n",
				       cache_name, bdev_name);

			/* If not, just save cache init params for use in examine and exit. */
			if ((rc = vbdev_ocf_cache_add_incomplete(cache, bdev_name))) {
				SPDK_ERRLOG("OCF cache '%s': failed to save init params - removing cache\n",
					    cache_name);
				goto err_base;
			}
			rpc_cb_fn(cache, rpc_cb_arg, -ENODEV);
			return;
		}
		SPDK_ERRLOG("OCF cache '%s': failed to open base bdev '%s'\n", cache_name, bdev_name);
		goto err_base;
	}

	/* Start OCF cache. */

	if ((rc = ocf_uuid_set_str(&volume_uuid, cache->name))) { // set spdk uuid ?
		SPDK_ERRLOG("OCF cache '%s': failed to set OCF volume uuid\n", cache_name);
		goto err_volume;
	}

	// move elsewhere
	cache->ocf_cache_att_cfg.device.volume_params = &cache->base; // for ocf_volume_open() in ocf_mngt_cache_attach()

	// do it right after set_config() and then ocf_volume_destroy() in all callbacks ?
	if ((rc = ocf_ctx_volume_create(vbdev_ocf_ctx, &cache->ocf_cache_att_cfg.device.volume,
					&volume_uuid, SPDK_OBJECT))) {
		SPDK_ERRLOG("OCF cache '%s': failed to create OCF volume\n", cache_name);
		goto err_volume;
	}

	// maybe this as well for incomplete cache ?
	// but then how to check for incomplition? is_cache_detached() ?
	// in examine only attach (both in start_rpc and attach_rpc) ?
	if ((rc = ocf_mngt_cache_start(vbdev_ocf_ctx, &cache->ocf_cache, &cache->ocf_cache_cfg, cache))) {
		SPDK_ERRLOG("OCF cache '%s': failed to start OCF cache\n", cache_name);
		goto err_start;
	}

	if ((rc = vbdev_ocf_cache_mngt_queue_create(cache))) {
		SPDK_ERRLOG("OCF cache '%s': failed to create management queue\n", cache_name);
		goto err_queue;
	}

	cache_start_ctx = calloc(1, sizeof(struct vbdev_ocf_cache_start_ctx));
	if (!cache_start_ctx) {
		SPDK_ERRLOG("OCF cache '%s': failed to allocate memory for cache start context\n",
			    cache_name);
		rc = -ENOMEM;
		goto err_alloc;
	}
	cache_start_ctx->cache = cache;
	cache_start_ctx->rpc_cb_fn = rpc_cb_fn;
	cache_start_ctx->rpc_cb_arg = rpc_cb_arg;

	ocf_mngt_cache_attach(cache->ocf_cache, &cache->ocf_cache_att_cfg,
			      _cache_start_rpc_cb, cache_start_ctx);

	return;

err_alloc:
	vbdev_ocf_queue_put(cache->ocf_cache_mngt_q);
err_queue:
	ocf_mngt_cache_stop(cache->ocf_cache, _cache_stop_err_cb, cache);
err_start:
	ocf_volume_destroy(cache->ocf_cache_att_cfg.device.volume);
err_volume:
	vbdev_ocf_cache_base_detach(cache);
err_base:
	vbdev_ocf_cache_destroy(cache);
err_create:
	rpc_cb_fn(NULL, rpc_cb_arg, rc);
}

static void
_core_unregister_cache_stop_cb(void *cb_arg, int error)
{
	struct vbdev_ocf_core *core = cb_arg;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': finishing unregister of OCF vbdev\n",
		      core->name);

	if (error) {
		SPDK_ERRLOG("OCF core '%s': failed to unregister OCF vbdev during cache stop: %s\n",
			    core->name, spdk_strerror(-error));
	}

	// destroy core despite the error ?
	vbdev_ocf_core_destroy(core);
}

static void
_cache_stop_rpc_stop_cb(ocf_cache_t ocf_cache, void *cb_arg, int error)
{
	struct vbdev_ocf_cache_stop_ctx *cache_stop_ctx = cb_arg;
	struct vbdev_ocf_cache *cache = cache_stop_ctx->cache;
	struct vbdev_ocf_core *core;
	int rc = 0;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF cache '%s': finishing stop of OCF cache\n", cache->name);
	SPDK_DEBUGLOG(vbdev_ocf, "OCF cache '%s': finishing stop\n", cache->name);

	ocf_mngt_cache_unlock(ocf_cache);

	if (error) {
		SPDK_ERRLOG("OCF cache '%s': failed to stop OCF cache\n",
			    cache->name);
		cache_stop_ctx->rpc_cb_fn(cache_stop_ctx->rpc_cb_arg, error);
		free(cache_stop_ctx);
		return;
	}

	SPDK_NOTICELOG("OCF cache '%s': stopped\n", cache->name);

	vbdev_ocf_foreach_core_in_cache(core, cache) {
		/* It's important to set ocf_core back to NULL before unregistering.
		 * This is an indicator for destruct that OCF cache isn't there any more. */
		core->ocf_core = NULL;

		if ((rc = vbdev_ocf_core_unregister(core, _core_unregister_cache_stop_cb, core))) {
			SPDK_ERRLOG("OCF core '%s': failed to start unregistering OCF vbdev during cache stop: %s\n",
				    core->name, spdk_strerror(-rc));
			// destroy core despite the error ?
			vbdev_ocf_core_destroy(core);
		}
	}

	vbdev_ocf_cache_base_detach(cache);
	vbdev_ocf_cache_destroy(cache);

	cache_stop_ctx->rpc_cb_fn(cache_stop_ctx->rpc_cb_arg, rc);
	free(cache_stop_ctx);
}

static void
_cache_stop_rpc_flush_cb(ocf_cache_t ocf_cache, void *cb_arg, int error)
{
	struct vbdev_ocf_cache_stop_ctx *cache_stop_ctx = cb_arg;
	struct vbdev_ocf_cache *cache = cache_stop_ctx->cache;

	if (error) {
		SPDK_ERRLOG("OCF cache '%s': failed to flush OCF cache\n",
			    cache->name);
		ocf_mngt_cache_unlock(ocf_cache);
		cache_stop_ctx->rpc_cb_fn(cache_stop_ctx->rpc_cb_arg, error);
		free(cache_stop_ctx);
		return;
	}

	ocf_mngt_cache_stop(ocf_cache, _cache_stop_rpc_stop_cb, cache_stop_ctx);
}

static void
_cache_stop_rpc_lock_cb(ocf_cache_t ocf_cache, void *lock_arg, int lock_err)
{
	struct vbdev_ocf_cache_stop_ctx *cache_stop_ctx = lock_arg;
	struct vbdev_ocf_cache *cache = cache_stop_ctx->cache;

	assert(ocf_cache == cache->ocf_cache);

	SPDK_DEBUGLOG(vbdev_ocf, "OCF cache '%s': initiating stop of OCF cache\n", cache->name);

	if (lock_err) {
		SPDK_ERRLOG("OCF cache '%s': failed to acquire OCF cache lock\n",
			    cache->name);
		cache_stop_ctx->rpc_cb_fn(cache_stop_ctx->rpc_cb_arg, lock_err);
		free(cache_stop_ctx);
		return;
	}

	if (ocf_mngt_cache_is_dirty(ocf_cache)) {
		ocf_mngt_cache_flush(ocf_cache, _cache_stop_rpc_flush_cb, cache_stop_ctx);
	} else {
		ocf_mngt_cache_stop(ocf_cache, _cache_stop_rpc_stop_cb, cache_stop_ctx);
	}
}

/* RPC entry point. */
void
vbdev_ocf_cache_stop(const char *cache_name, vbdev_ocf_cache_stop_cb rpc_cb_fn, void *rpc_cb_arg)
{
	struct vbdev_ocf_cache *cache;
	struct vbdev_ocf_cache_stop_ctx *cache_stop_ctx;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF cache '%s': initiating stop\n", cache_name);

	cache = vbdev_ocf_cache_get_by_name(cache_name);
	if (!cache) {
		SPDK_ERRLOG("OCF cache '%s': device not found\n", cache_name);
		rpc_cb_fn(rpc_cb_arg, -ENODEV);
		return;
	}

	/* If cache was not started yet due to lack of base device, just free its structs and exit. */
	if (vbdev_ocf_cache_is_incomplete(cache)) {
		SPDK_DEBUGLOG(vbdev_ocf, "OCF cache '%s': removing as incomplete\n", cache->name);
		assert(!cache->base.attached);
		vbdev_ocf_cache_remove_incomplete(cache);
		vbdev_ocf_cache_destroy(cache);
		rpc_cb_fn(rpc_cb_arg, 0);
		return;
	}

	// TODO: send hotremove to each core opener first - check how spdk_bdev_unregister does that
	// take from bdev_unregister_unsafe()
	// or stop cache after unregister ?

	cache_stop_ctx = calloc(1, sizeof(struct vbdev_ocf_cache_stop_ctx));
	if (!cache_stop_ctx) {
		SPDK_ERRLOG("OCF cache '%s': failed to allocate memory for cache stop context\n",
			    cache_name);
		rpc_cb_fn(rpc_cb_arg, -ENOMEM);
		return;
	}
	cache_stop_ctx->cache = cache;
	cache_stop_ctx->rpc_cb_fn = rpc_cb_fn;
	cache_stop_ctx->rpc_cb_arg = rpc_cb_arg;

	ocf_mngt_cache_lock(cache->ocf_cache, _cache_stop_rpc_lock_cb, cache_stop_ctx);
}

static void
_core_remove_err_cb(void *cb_arg, int error)
{
	struct vbdev_ocf_core *core = cb_arg;
	struct vbdev_ocf_cache *cache = vbdev_ocf_core_get_cache(core);

	ocf_mngt_cache_unlock(cache->ocf_cache);

	if (error) {
		SPDK_ERRLOG("OCF core '%s': failed to remove OCF core device (OCF error: %d)\n",
			    core->name, error);
		return;
	}

	vbdev_ocf_core_remove_from_cache(core);
}

static void
vbdev_ocf_core_add_rollback(struct vbdev_ocf_core *core)
{
	ocf_mngt_cache_remove_core(core->ocf_core, _core_remove_err_cb, core);
	vbdev_ocf_core_base_detach(core);
	vbdev_ocf_core_destroy(core);
}

static void
_core_add_rpc_cb(ocf_cache_t ocf_cache, ocf_core_t ocf_core, void *cb_arg, int error)
{
	struct vbdev_ocf_core_add_ctx *core_add_ctx = cb_arg;
	struct vbdev_ocf_cache *cache = core_add_ctx->cache;
	struct vbdev_ocf_core *core = core_add_ctx->core;
	int rc = 0;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': finishing add of OCF core\n", core->name);
	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': finishing add\n", core->name);

	if (error) {
		SPDK_ERRLOG("OCF core '%s': failed to add core to OCF cache '%s'\n",
			    core->name, cache->name);
		ocf_mngt_cache_unlock(ocf_cache);
		vbdev_ocf_core_base_detach(core);
		vbdev_ocf_core_destroy(core);
		core_add_ctx->rpc_cb_fn(NULL, core_add_ctx->rpc_cb_arg, error);
		free(core_add_ctx);
		return;
	}

	vbdev_ocf_core_add_to_cache(core, cache);
	core->ocf_core = ocf_core;

	if ((rc = vbdev_ocf_core_register(core))) {
		SPDK_ERRLOG("OCF core '%s': failed to register vbdev\n", core->name);
		vbdev_ocf_core_add_rollback(core);
		core_add_ctx->rpc_cb_fn(NULL, core_add_ctx->rpc_cb_arg, rc);
		free(core_add_ctx);
		return;
	}

	SPDK_NOTICELOG("OCF core '%s': added to cache '%s'\n", core->name, cache->name);

	ocf_mngt_cache_unlock(ocf_cache);
	core_add_ctx->rpc_cb_fn(core, core_add_ctx->rpc_cb_arg, rc);
	free(core_add_ctx);
}

static void
_vbdev_ocf_core_add_rpc_lock_cb(ocf_cache_t ocf_cache, void *lock_arg, int lock_err)
{
	struct vbdev_ocf_core_add_ctx *core_add_ctx = lock_arg;
	struct vbdev_ocf_cache *cache = core_add_ctx->cache;
	struct vbdev_ocf_core *core = core_add_ctx->core;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': initiating add of OCF core\n", core->name);

	if (lock_err) {
		SPDK_ERRLOG("OCF core '%s': failed to acquire OCF cache lock\n",
			    core->name);
		vbdev_ocf_core_base_detach(core);
		vbdev_ocf_core_destroy(core);
		core_add_ctx->rpc_cb_fn(NULL, core_add_ctx->rpc_cb_arg, lock_err);
		free(core_add_ctx);
		return;
	}

	ocf_mngt_cache_add_core(cache->ocf_cache, &core->ocf_core_cfg, _core_add_rpc_cb, core_add_ctx);
}

/* RPC entry point. */
void
vbdev_ocf_core_add(const char *core_name, const char *bdev_name, const char *cache_name,
		   vbdev_ocf_core_add_cb rpc_cb_fn, void *rpc_cb_arg)
{
	struct vbdev_ocf_cache *cache;
	struct vbdev_ocf_core *core;
	struct vbdev_ocf_core_add_ctx *core_add_ctx;
	int rc = 0;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': initiating add\n", core_name);

	if (vbdev_ocf_device_exists(core_name)) {
		SPDK_ERRLOG("OCF: device '%s' already exists\n", core_name);
		rc = -EEXIST;
		goto err_create;
	}

	/* Allocate memory for core struct. */
	if ((rc = vbdev_ocf_core_create(core_name, &core))) {
		SPDK_ERRLOG("OCF core '%s': failed to create core\n", core_name);
		goto err_create;
	}

	/* Create OCF core config. */
	if ((rc = vbdev_ocf_core_set_config(core))) {
		SPDK_ERRLOG("OCF core '%s': failed to create OCF config\n", core_name);
		goto err_base;
	}

	/* First, check if base device for this core is already present. */
	if ((rc = vbdev_ocf_core_base_attach(core, bdev_name))) {
		if (rc == -ENODEV) {
			SPDK_NOTICELOG("OCF core '%s': add deferred - waiting for base bdev '%s'\n",
				       core_name, bdev_name);
			/* If not, just save core init params for use in examine,
			 * put core on the temporary incomplete core list and exit. */
			if ((rc = vbdev_ocf_core_add_incomplete(core, bdev_name, cache_name))) {
				SPDK_ERRLOG("OCF core '%s': failed to save init params - removing core\n",
					    core_name);
				goto err_base;
			}
			rpc_cb_fn(core, rpc_cb_arg, -ENODEV);
			return;
		}
		SPDK_ERRLOG("OCF core '%s': failed to open base bdev '%s'\n", core_name, bdev_name);
		goto err_base;
	}

	// move elsewhere
	core->ocf_core_cfg.volume_params = &core->base; // for ocf_volume_open() in ocf_mngt_cache_add_core()

	/* Second, check if OCF cache for this core is already present and started. */
	cache = vbdev_ocf_cache_get_by_name(cache_name);
	if (!cache || vbdev_ocf_cache_is_incomplete(cache)) {
		SPDK_NOTICELOG("OCF core '%s': add deferred - waiting for OCF cache '%s'\n",
			       core_name, cache_name);

		/* If not, just save core init params for use in examine,
		 * put core on the temporary incomplete core list and exit. */
		if ((rc = vbdev_ocf_core_add_incomplete(core, bdev_name, cache_name))) {
			SPDK_ERRLOG("OCF core '%s': failed to save init params - removing core\n",
				    core_name);
			goto err_alloc;
		}
		rpc_cb_fn(core, rpc_cb_arg, -ENODEV);
		return;
	}

	// check if (cache_block_size > core_block_size)

	core_add_ctx = calloc(1, sizeof(struct vbdev_ocf_core_add_ctx));
	if (!core_add_ctx) {
		SPDK_ERRLOG("OCF core '%s': failed to allocate memory for vbdev_ocf_core_add_ctx\n",
			    core_name);
		rc = -ENOMEM;
		goto err_alloc;
	}
	core_add_ctx->core = core;
	core_add_ctx->cache = cache;
	core_add_ctx->rpc_cb_fn = rpc_cb_fn;
	core_add_ctx->rpc_cb_arg = rpc_cb_arg;

	ocf_mngt_cache_lock(cache->ocf_cache, _vbdev_ocf_core_add_rpc_lock_cb, core_add_ctx);

	return;

err_alloc:
	vbdev_ocf_core_base_detach(core);
err_base:
	vbdev_ocf_core_destroy(core);
err_create:
	rpc_cb_fn(NULL, rpc_cb_arg, rc);
}

static void
_core_unregister_core_rm_cb(void *cb_arg, int error)
{
	struct vbdev_ocf_core_remove_ctx *core_rm_ctx = cb_arg;
	struct vbdev_ocf_core *core = core_rm_ctx->core;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': finishing unregister of OCF vbdev\n",
		      core->name);
	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': finishing removal\n", core->name);

	if (error) {
		SPDK_ERRLOG("OCF core '%s': failed to unregister OCF vbdev during core removal\n",
			    core->name);
	} else {
		SPDK_NOTICELOG("OCF core '%s': removed from cache\n", core->name);
		vbdev_ocf_core_destroy(core);
	}

	core_rm_ctx->rpc_cb_fn(core_rm_ctx->rpc_cb_arg, error);
	free(core_rm_ctx);
}

/* RPC entry point. */
void
vbdev_ocf_core_remove(const char *core_name, vbdev_ocf_core_remove_cb rpc_cb_fn, void *rpc_cb_arg)
{
	struct vbdev_ocf_core *core;
	struct vbdev_ocf_core_remove_ctx *core_rm_ctx;
	int rc;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': initiating removal\n", core_name);

	core = vbdev_ocf_core_get_by_name(core_name);
	if (!core) {
		SPDK_ERRLOG("OCF core '%s': device not found\n", core_name);
		rpc_cb_fn(rpc_cb_arg, -ENODEV);
		return;
	}

	/* If core was not added yet due to lack of base or cache device,
	 * just free its structs (and detach its base if exists) and exit. */
	if (vbdev_ocf_core_is_incomplete(core)) {
		SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': removing as incomplete\n", core->name);
		if (core->base.attached) {
			vbdev_ocf_core_base_detach(core);
		}
		vbdev_ocf_core_remove_incomplete(core);
		vbdev_ocf_core_destroy(core);
		rpc_cb_fn(rpc_cb_arg, 0);
		return;
	}

	core_rm_ctx = calloc(1, sizeof(struct vbdev_ocf_core_remove_ctx));
	if (!core_rm_ctx) {
		SPDK_ERRLOG("OCF core '%s': failed to allocate memory for core remove context\n", core_name);
		rpc_cb_fn(rpc_cb_arg, -ENOMEM);
		return;
	}
	core_rm_ctx->core = core;
	core_rm_ctx->rpc_cb_fn = rpc_cb_fn;
	core_rm_ctx->rpc_cb_arg = rpc_cb_arg;

	if ((rc = vbdev_ocf_core_unregister(core, _core_unregister_core_rm_cb, core_rm_ctx))) {
		SPDK_ERRLOG("OCF core '%s': failed to start unregistering OCF vbdev during core removal\n",
			    core->name);
		rpc_cb_fn(rpc_cb_arg, rc);
		free(core_rm_ctx);
	}
}

static void
_write_cache_info_begin(struct spdk_json_write_ctx *w, struct vbdev_ocf_cache *cache)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "type", "OCF_cache");
	spdk_json_write_named_string(w, "name", cache->name);
	spdk_json_write_named_string(w, "base_bdev_name",
				     cache->base.bdev ? spdk_bdev_get_name(cache->base.bdev) : "" );
	spdk_json_write_named_uint16(w, "cores_count", cache->cores_count);
}

static void
_write_cache_info_end(struct spdk_json_write_ctx *w, struct vbdev_ocf_cache *cache)
{
	spdk_json_write_object_end(w);
}

static void
_write_core_info(struct spdk_json_write_ctx *w, struct vbdev_ocf_core *core)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "type", "OCF_core");
	spdk_json_write_named_string(w, "name", core->name);
	spdk_json_write_named_string(w, "base_bdev_name",
				     core->base.bdev ? spdk_bdev_get_name(core->base.bdev) : "" );
	spdk_json_write_named_string(w, "cache_name", vbdev_ocf_core_get_cache(core)->name);
	spdk_json_write_object_end(w);
}

/* RPC entry point. */
void
vbdev_ocf_get_bdevs(const char *name, vbdev_ocf_get_bdevs_cb rpc_cb_fn, void *rpc_cb_arg1, void *rpc_cb_arg2)
{
	struct spdk_json_write_ctx *w = rpc_cb_arg1;
	struct vbdev_ocf_cache *cache;
	struct vbdev_ocf_core *core;
	bool found = false;

	if (name) {
		vbdev_ocf_foreach_cache(cache) {
			vbdev_ocf_foreach_core_in_cache(core, cache) {
				if (strcmp(name, core->name)) {
					continue;
				}
				found = true;

				// dump_info_json() instead?
				_write_core_info(w, core);
				break;
			}
			if (found) {
				break;
			}
			if (strcmp(name, cache->name)) {
				continue;
			}

			_write_cache_info_begin(w, cache);
			_write_cache_info_end(w, cache);
			break;
		}
	} else {
		vbdev_ocf_foreach_cache(cache) {
			_write_cache_info_begin(w, cache);
			spdk_json_write_named_array_begin(w, "cores");
			vbdev_ocf_foreach_core_in_cache(core, cache) {
				_write_core_info(w, core);
			}
			spdk_json_write_array_end(w);
			_write_cache_info_end(w, cache);
		}
	}

	rpc_cb_fn(rpc_cb_arg1, rpc_cb_arg2);
}

SPDK_LOG_REGISTER_COMPONENT(vbdev_ocf)
