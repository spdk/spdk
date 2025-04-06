/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2025 Huawei Technologies
 *   All rights reserved.
 */

#include "spdk/string.h" // rm ?

#include "vbdev_ocf_core.h"
#include "vbdev_ocf_cache.h"
#include "ctx.h"

struct vbdev_ocf_incomplete_cores_head g_vbdev_ocf_incomplete_cores =
		STAILQ_HEAD_INITIALIZER(g_vbdev_ocf_incomplete_cores);

int
vbdev_ocf_core_create(const char *core_name, struct vbdev_ocf_core **out)
{
	struct vbdev_ocf_core *core;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': allocating vbdev_ocf_core and adding it to core list\n",
		      core_name);

	core = calloc(1, sizeof(struct vbdev_ocf_core));
	if (!core) {
		SPDK_ERRLOG("OCF core '%s': failed to allocate memory for vbdev_ocf_core\n",
			    core_name);
		return -ENOMEM;
	}

	core->name = strdup(core_name);
	if (!core->name) {
		SPDK_ERRLOG("OCF core '%s': failed to allocate memory for vbdev_ocf_core name\n",
			    core_name);
		free(core);
		return -ENOMEM;
	}

	*out = core;

	return 0;
}

void
vbdev_ocf_core_destroy(struct vbdev_ocf_core *core)
{
	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': deallocating vbdev_ocf_core\n",
		      core->name);

	free(core->name);
	free(core);
}

int
vbdev_ocf_core_set_config(struct vbdev_ocf_core *core)
{
	struct ocf_mngt_core_config *ocf_core_cfg = &core->ocf_core_cfg;
	int rc = 0;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': setting OCF config\n", core->name);

	ocf_mngt_core_config_set_default(ocf_core_cfg);

	strncpy(ocf_core_cfg->name, core->name, OCF_CORE_NAME_SIZE);
	if ((rc = ocf_uuid_set_str(&ocf_core_cfg->uuid, core->name))) { // set spdk uuid ?
		return rc;
	}
	ocf_core_cfg->volume_type = SPDK_OBJECT;

	return rc;
}

static void
_core_unregister_core_hotrm_cb(void *cb_arg, int error)
{
	struct vbdev_ocf_core *core = cb_arg;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': finishing unregister of OCF vbdev\n",
		      core->name);
	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': finishing hot removal of base bdev\n",
		      core->name);

	if (error) {
		SPDK_ERRLOG("OCF core '%s': failed to unregister OCF vbdev during hot removal: %s\n",
			    core->name, spdk_strerror(-error));
	} else {
		/* Do not destroy core struct in hotremove; it will be needed in examine.
		 * Just indicate that it's not connected to cache anymore. */
		core->ocf_core = NULL;
	}
}

static void
vbdev_ocf_core_hotremove(struct spdk_bdev *bdev, void *event_ctx)
{
	struct vbdev_ocf_core *core = event_ctx;
	int rc;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': initiating hot removal of base bdev '%s'\n",
		      core->name, bdev->name);

	assert(bdev == core->base.bdev);

	if (vbdev_ocf_core_is_incomplete(core)) {
		SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': hot removing as incomplete\n", core->name);
		vbdev_ocf_core_base_detach(core);
		return;
	}

	if ((rc = vbdev_ocf_core_unregister(core, _core_unregister_core_hotrm_cb, core))) {
		SPDK_ERRLOG("OCF core '%s': failed to start unregistering OCF vbdev during core hot removal: %s\n",
			    core->name, spdk_strerror(-rc));
		// detach base despite the error ?
		vbdev_ocf_core_base_detach(core);
		return;
	}
}

static void
_vbdev_ocf_core_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	struct vbdev_ocf_core *core = event_ctx;

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		vbdev_ocf_core_hotremove(bdev, event_ctx);
		break;
	default:
		SPDK_NOTICELOG("OCF core '%s': unsupported bdev event type: %d\n", core->name, type);
	}
}

int
vbdev_ocf_core_base_attach(struct vbdev_ocf_core *core, const char *bdev_name)
{
	int rc = 0;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': attaching base bdev '%s'\n",
		      core->name, bdev_name);

	if ((rc = spdk_bdev_open_ext(bdev_name, true, _vbdev_ocf_core_event_cb, core, &core->base.desc))) {
		return rc;
	}

	if ((rc = spdk_bdev_module_claim_bdev_desc(core->base.desc,
						   SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE,
						   NULL, &ocf_if))) {
		SPDK_ERRLOG("OCF core '%s': failed to claim base bdev '%s'\n", core->name, bdev_name);
		spdk_bdev_close(core->base.desc);
		return rc;
	}

	core->base.mngt_ch = spdk_bdev_get_io_channel(core->base.desc);
	if (!core->base.mngt_ch) {
		SPDK_ERRLOG("OCF core '%s': failed to get IO channel for base bdev '%s'\n",
			    core->name, bdev_name);
		spdk_bdev_close(core->base.desc);
		return -ENOMEM;
	}

	core->base.bdev = spdk_bdev_desc_get_bdev(core->base.desc);
	core->base.thread = spdk_get_thread();
	core->base.is_cache = false;
	core->base.attached = true;

	// why not ? what is it then ?!?
	//assert(__bdev_to_io_dev(core->base.bdev) == core->base.bdev->ctxt);

	return rc;
}

void
vbdev_ocf_core_base_detach(struct vbdev_ocf_core *core)
{
	struct vbdev_ocf_base *base = &core->base;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': detaching base bdev '%s'\n",
		      core->name, base->bdev->name);

	vbdev_ocf_base_detach(base);
}

int
vbdev_ocf_core_add_incomplete(struct vbdev_ocf_core *core, const char *bdev_name,
			      const char *cache_name)
{
	struct vbdev_ocf_core_init_params *init_params;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': allocating init params and adding core to incomplete list\n",
		      core->name);

	init_params = calloc(1, sizeof(struct vbdev_ocf_core_init_params));
	if (!init_params) {
		SPDK_ERRLOG("OCF core '%s': failed to allocate memory for init params\n",
			    core->name);
		return -ENOMEM;
	}

	init_params->bdev_name = strdup(bdev_name);
	if (!init_params->bdev_name) {
		SPDK_ERRLOG("OCF core '%s': failed to allocate memory for base bdev name\n",
			    core->name);
		free(init_params);
		return -ENOMEM;
	}

	init_params->cache_name = strdup(cache_name);
	if (!init_params->cache_name) {
		SPDK_ERRLOG("OCF core '%s': failed to allocate memory for cache bdev name\n",
			    core->name);
		free(init_params->bdev_name);
		free(init_params);
		return -ENOMEM;
	}

	core->init_params = init_params;
	STAILQ_INSERT_TAIL(&g_vbdev_ocf_incomplete_cores, core, link);

	return 0;
}

void
vbdev_ocf_core_remove_incomplete(struct vbdev_ocf_core *core)
{
	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': deallocating init params and removing core from incomplete list\n",
		      core->name);

	free(core->init_params->bdev_name);
	free(core->init_params->cache_name);
	free(core->init_params);
	/* It's important to set init_params back to NULL.
	 * This is an indicator that core is not incomplete any more. */
	core->init_params = NULL;
	STAILQ_REMOVE(&g_vbdev_ocf_incomplete_cores, core, vbdev_ocf_core, link);
}

void
vbdev_ocf_core_add_to_cache(struct vbdev_ocf_core *core, struct vbdev_ocf_cache *cache)
{
	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': adding core to list in cache\n", core->name);

	core->cache = cache;
	cache->cores_count++;
	STAILQ_INSERT_TAIL(&cache->cores, core, link);
}

void
vbdev_ocf_core_remove_from_cache(struct vbdev_ocf_core *core)
{
	struct vbdev_ocf_cache *cache;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': removing core from list in cache\n", core->name);

	cache = vbdev_ocf_core_get_cache(core);
	cache->cores_count--;
	STAILQ_REMOVE(&cache->cores, core, vbdev_ocf_core, link);
}

static void
_core_io_queue_stop(void *ctx)
{
	struct vbdev_ocf_core_io_channel_ctx *ch_ctx = ctx;

	spdk_poller_unregister(&ch_ctx->poller);
	spdk_put_io_channel(ch_ctx->cache_ch);
	spdk_put_io_channel(ch_ctx->core_ch);
	free(ch_ctx);
}

static void
vbdev_ocf_core_io_queue_stop(ocf_queue_t queue)
{
	struct vbdev_ocf_core_io_channel_ctx *ch_ctx = ocf_queue_get_priv(queue);
	
	SPDK_DEBUGLOG(vbdev_ocf, "OCF vbdev '%s': deallocating external IO channel context\n",
		      ch_ctx->core->ocf_vbdev.name);

	if (ch_ctx->thread && ch_ctx->thread != spdk_get_thread()) {
		spdk_thread_send_msg(ch_ctx->thread, _core_io_queue_stop, ch_ctx);
	} else {
		_core_io_queue_stop(ch_ctx);
	}
}

static void
vbdev_ocf_core_io_queue_kick(ocf_queue_t queue)
{
}

const struct ocf_queue_ops core_io_queue_ops = {
	.kick_sync = NULL,
	.kick = vbdev_ocf_core_io_queue_kick,
	.stop = vbdev_ocf_core_io_queue_stop,
};

static int
_vbdev_ocf_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct vbdev_ocf_core *core = io_device;
	struct vbdev_ocf_cache *cache = vbdev_ocf_core_get_cache(core);
	struct vbdev_ocf_core_io_channel_ctx *ch_destroy_ctx = ctx_buf;
	struct vbdev_ocf_core_io_channel_ctx *ch_ctx;
	int rc = 0;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF vbdev '%s': creating IO channel and allocating external context\n",
		      core->ocf_vbdev.name);

	/* Do not use provided buffer for IO channel context, as it will be freed
	 * when this channel is destroyed. Instead allocate our own and keep it
	 * in queue priv. It will be needed later, after the channel was closed,
	 * as it may be referenced by backfill. */
	ch_ctx = calloc(1, sizeof(struct vbdev_ocf_core_io_channel_ctx));
	if (!ch_ctx) {
		SPDK_ERRLOG("OCF vbdev '%s': failed to allocate memory for IO channel context\n",
			    core->ocf_vbdev.name);
		return -ENOMEM;
	}

	if ((rc = vbdev_ocf_queue_create(cache->ocf_cache, &ch_ctx->queue, &core_io_queue_ops))) {
		SPDK_ERRLOG("OCF vbdev '%s': failed to create OCF queue\n", core->ocf_vbdev.name);
		free(ch_ctx);
		return rc;
	}
	ocf_queue_set_priv(ch_ctx->queue, ch_ctx);

	ch_ctx->cache_ch = spdk_bdev_get_io_channel(cache->base.desc);
	if (!ch_ctx->cache_ch) {
		SPDK_ERRLOG("OCF vbdev '%s': failed to create IO channel for base bdev '%s'\n",
			    core->ocf_vbdev.name, cache->base.bdev->name);
		vbdev_ocf_queue_put(ch_ctx->queue);
		return -ENOMEM;
	}

	ch_ctx->core_ch = spdk_bdev_get_io_channel(core->base.desc);
	if (!ch_ctx->core_ch) {
		SPDK_ERRLOG("OCF vbdev '%s': failed to create IO channel for base bdev '%s'\n",
			    core->ocf_vbdev.name, core->base.bdev->name);
		spdk_put_io_channel(ch_ctx->cache_ch);
		vbdev_ocf_queue_put(ch_ctx->queue);
		return -ENOMEM;
	}

	ch_ctx->poller = SPDK_POLLER_REGISTER(vbdev_ocf_queue_poller, ch_ctx->queue, 0);
	if (!ch_ctx->poller) {
		SPDK_ERRLOG("OCF vbdev '%s': failed to create IO queue poller\n", core->ocf_vbdev.name);
		spdk_put_io_channel(ch_ctx->core_ch);
		spdk_put_io_channel(ch_ctx->cache_ch);
		vbdev_ocf_queue_put(ch_ctx->queue);
		return -ENOMEM;
	}

	ch_ctx->core = core; // keep? (only for DEBUGLOG)
	ch_ctx->thread = spdk_get_thread();

	/* Save queue pointer in buffer provided by the IO channel callback.
	 * Only this will be needed in channel destroy callback to decrement
	 * the refcount. The rest is freed in queue stop callback. */
	ch_destroy_ctx->queue = ch_ctx->queue;

	return rc;
}

static void
_vbdev_ocf_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct vbdev_ocf_core *core = io_device;
	struct vbdev_ocf_core_io_channel_ctx *ch_destroy_ctx = ctx_buf;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF vbdev '%s': destroying IO channel\n", core->ocf_vbdev.name);

	vbdev_ocf_queue_put(ch_destroy_ctx->queue);
}

int
vbdev_ocf_core_register(struct vbdev_ocf_core *core)
{
	int rc = 0;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': registering OCF vbdev in SPDK bdev layer\n", core->name);

	core->ocf_vbdev.ctxt = core;
	core->ocf_vbdev.name = core->name;
	core->ocf_vbdev.product_name = "OCF_disk";
	core->ocf_vbdev.write_cache = core->base.bdev->write_cache;
	core->ocf_vbdev.blocklen = core->base.bdev->blocklen;
	core->ocf_vbdev.blockcnt = core->base.bdev->blockcnt;
	// ?
	//core->ocf_vbdev.required_alignment = core->base.bdev->required_alignment;
	//core->ocf_vbdev.optimal_io_boundary = core->base.bdev->optimal_io_boundary;
	// generate UUID based on namespace UUID + base bdev UUID (take from old module?)
	core->ocf_vbdev.fn_table = &vbdev_ocf_fn_table;
	core->ocf_vbdev.module = &ocf_if;

	spdk_io_device_register(core, _vbdev_ocf_ch_create_cb, _vbdev_ocf_ch_destroy_cb,
				sizeof(struct vbdev_ocf_core_io_channel_ctx), core->name);
	SPDK_DEBUGLOG(vbdev_ocf, "OCF vbdev '%s': io_device created at 0x%p\n", core->ocf_vbdev.name, core);

	if ((rc = spdk_bdev_register(&core->ocf_vbdev))) { // needs to be called from SPDK app thread
		SPDK_ERRLOG("OCF vbdev '%s': failed to register SPDK bdev\n", core->ocf_vbdev.name);
		spdk_io_device_unregister(core, NULL);
		return rc;
	}

	return rc;
}

int
vbdev_ocf_core_unregister(struct vbdev_ocf_core *core, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	SPDK_DEBUGLOG(vbdev_ocf, "OCF core '%s': initiating unregister of OCF vbdev\n", core->name);

	return spdk_bdev_unregister_by_name(core->ocf_vbdev.name, &ocf_if, cb_fn, cb_arg);
}

struct vbdev_ocf_cache *
vbdev_ocf_core_get_cache(struct vbdev_ocf_core *core)
{
	return core->cache;
}

struct vbdev_ocf_core *
vbdev_ocf_core_get_by_name(const char *core_name)
{
	struct vbdev_ocf_cache *cache;
	struct vbdev_ocf_core *core;

	vbdev_ocf_foreach_core_incomplete(core) {
		if (strcmp(core_name, core->name)) {
			continue;
		}
		return core;
	}
	vbdev_ocf_foreach_cache(cache) {
		vbdev_ocf_foreach_core_in_cache(core, cache) {
			if (strcmp(core_name, core->name)) {
				continue;
			}
			return core;
		}
	}
	return NULL;
}

bool
vbdev_ocf_core_cache_is_started(struct vbdev_ocf_core *core)
{
	ocf_core_t ocf_core = core->ocf_core;
	ocf_cache_t ocf_cache;

	if (ocf_core) {
		ocf_cache = ocf_core_get_cache(ocf_core);
		if (ocf_cache_is_running(ocf_cache) || ocf_cache_is_detached(ocf_cache)) { // smth else ?
			return true;
		}
	}
	return false;
}

bool
vbdev_ocf_core_is_incomplete(struct vbdev_ocf_core *core)
{
	return !!core->init_params;
}
