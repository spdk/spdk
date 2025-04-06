/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2025 Huawei Technologies
 *   All rights reserved.
 */

#include "vbdev_ocf_cache.h"
#include "ctx.h"
#include "utils.h"

struct vbdev_ocf_caches_head g_vbdev_ocf_caches = STAILQ_HEAD_INITIALIZER(g_vbdev_ocf_caches);

int
vbdev_ocf_cache_create(const char *cache_name, struct vbdev_ocf_cache **out)
{
	struct vbdev_ocf_cache *cache;
	int rc = 0;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF cache '%s': allocating vbdev_ocf_cache and adding it to cache list\n",
		      cache_name);

	cache = calloc(1, sizeof(struct vbdev_ocf_cache));
	if (!cache) {
		SPDK_ERRLOG("OCF cache '%s': failed to allocate memory for vbdev_ocf_cache\n",
			    cache_name);
		return -ENOMEM;
	}

	cache->name = strdup(cache_name);
	if (!cache->name) {
		SPDK_ERRLOG("OCF cache '%s': failed to allocate memory for vbdev_ocf_cache name\n",
			    cache_name);
		free(cache);
		return -ENOMEM;
	}

	STAILQ_INIT(&cache->cores);
	STAILQ_INSERT_TAIL(&g_vbdev_ocf_caches, cache, link);

	*out = cache;

	return rc;
}

void
vbdev_ocf_cache_destroy(struct vbdev_ocf_cache *cache)
{
	SPDK_DEBUGLOG(vbdev_ocf, "OCF cache '%s': deallocating vbdev_ocf_cache and removing it from cache list\n",
		      cache->name);

	STAILQ_REMOVE(&g_vbdev_ocf_caches, cache, vbdev_ocf_cache, link);
	free(cache->name);
	free(cache);
}

int
vbdev_ocf_cache_set_config(struct vbdev_ocf_cache *cache, const char *cache_mode,
			   const uint8_t cache_line_size)
{
	struct ocf_mngt_cache_config *ocf_cache_cfg = &cache->ocf_cache_cfg;
	struct ocf_mngt_cache_attach_config *ocf_cache_att_cfg = &cache->ocf_cache_att_cfg;
	int rc = 0;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF cache '%s': setting OCF config\n", cache->name);

	ocf_mngt_cache_config_set_default(ocf_cache_cfg);
	ocf_mngt_cache_attach_config_set_default(ocf_cache_att_cfg);

	strncpy(ocf_cache_cfg->name, cache->name, OCF_CACHE_NAME_SIZE);
	if (cache_mode) {
		ocf_cache_cfg->cache_mode = ocf_get_cache_mode(cache_mode);
	}
	if (cache_line_size) {
		ocf_cache_cfg->cache_line_size = cache_line_size * KiB;
		ocf_cache_att_cfg->cache_line_size = cache_line_size * KiB;
	}
	ocf_cache_cfg->locked = true;
	ocf_cache_att_cfg->open_cores = false;
	ocf_cache_att_cfg->discard_on_start = false; // needed ?
	ocf_cache_att_cfg->device.perform_test = false; // needed ?
	// TODO: add load option
	ocf_cache_att_cfg->force = true;

	return rc; // rm?
}

// vbdev_ocf_cache_detach() instead of this ?
static void
vbdev_ocf_cache_hotremove(struct spdk_bdev *bdev, void *event_ctx)
{
	struct vbdev_ocf_cache *cache = event_ctx;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF cache '%s': hot removal of base bdev '%s'\n",
		      cache->name, bdev->name);

	assert(bdev == cache->base.bdev);

	if (vbdev_ocf_cache_is_running(cache)) {
		// OCF cache flush
		// OCF cache detach
		// (to comply with SPDK hotremove support)
	}

	vbdev_ocf_cache_base_detach(cache);
}

static void
_vbdev_ocf_cache_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	struct vbdev_ocf_cache *cache = event_ctx;

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		vbdev_ocf_cache_hotremove(bdev, event_ctx);
		break;
	default:
		SPDK_NOTICELOG("OCF cache '%s': unsupported bdev event type: %d\n", cache->name, type);
	}
}

int
vbdev_ocf_cache_base_attach(struct vbdev_ocf_cache *cache, const char *bdev_name)
{
	int rc = 0;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF cache '%s': attaching base bdev '%s'\n",
		      cache->name, bdev_name);

	if ((rc = spdk_bdev_open_ext(bdev_name, true, _vbdev_ocf_cache_event_cb, cache, &cache->base.desc))) {
		return rc;
	}

	if ((rc = spdk_bdev_module_claim_bdev_desc(cache->base.desc,
						   SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE,
						   NULL, &ocf_if))) {
		SPDK_ERRLOG("OCF cache '%s': failed to claim base bdev '%s'\n", cache->name, bdev_name);
		spdk_bdev_close(cache->base.desc);
		return rc;
	}

	cache->base.mngt_ch = spdk_bdev_get_io_channel(cache->base.desc);
	if (!cache->base.mngt_ch) {
		SPDK_ERRLOG("OCF cache '%s': failed to get IO channel for base bdev '%s'\n",
			    cache->name, bdev_name);
		spdk_bdev_close(cache->base.desc);
		return -ENOMEM;
	}

	cache->base.bdev = spdk_bdev_desc_get_bdev(cache->base.desc);
	cache->base.thread = spdk_get_thread();
	cache->base.is_cache = true;
	cache->base.attached = true;

	// why not ? what is it then ?!?
	//assert(__bdev_to_io_dev(cache->base.bdev) == cache->base.bdev->ctxt);

	return rc;
}

void
vbdev_ocf_cache_base_detach(struct vbdev_ocf_cache *cache)
{
	struct vbdev_ocf_base *base = &cache->base;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF cache '%s': detaching base bdev '%s'\n",
		      cache->name, base->bdev->name);

	vbdev_ocf_base_detach(base);
}

int
vbdev_ocf_cache_add_incomplete(struct vbdev_ocf_cache *cache, const char *bdev_name)
{
	struct vbdev_ocf_cache_init_params *init_params;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF cache '%s': allocating init params\n", cache->name);

	init_params = calloc(1, sizeof(struct vbdev_ocf_cache_init_params));
	if (!init_params) {
		SPDK_ERRLOG("OCF cache '%s': failed to allocate memory for init params\n",
			    cache->name);
		return -ENOMEM;
	}

	init_params->bdev_name = strdup(bdev_name);
	if (!init_params->bdev_name) {
		SPDK_ERRLOG("OCF cache '%s': failed to allocate memory for base bdev name\n",
			    cache->name);
		free(init_params);
		return -ENOMEM;
	}

	cache->init_params = init_params;

	return 0;
}

void
vbdev_ocf_cache_remove_incomplete(struct vbdev_ocf_cache *cache)
{
	SPDK_DEBUGLOG(vbdev_ocf, "OCF cache '%s': deallocating init params\n", cache->name);

	free(cache->init_params->bdev_name);
	free(cache->init_params);
	/* It's important to set init_params back to NULL.
	 * This is an indicator that cache is not incomplete any more. */
	cache->init_params = NULL;
}

static void
_cache_mngt_queue_stop(void *ctx)
{
	struct vbdev_ocf_cache_mngt_queue_ctx *mngt_q_ctx = ctx;

	spdk_poller_unregister(&mngt_q_ctx->poller);
	free(mngt_q_ctx);
}

static void
vbdev_ocf_cache_mngt_queue_stop(ocf_queue_t queue)
{
	struct vbdev_ocf_cache_mngt_queue_ctx *mngt_q_ctx = ocf_queue_get_priv(queue);
	
	SPDK_DEBUGLOG(vbdev_ocf, "OCF cache '%s': destroying OCF management queue\n",
		      mngt_q_ctx->cache->name);

	if (mngt_q_ctx->thread && mngt_q_ctx->thread != spdk_get_thread()) {
		spdk_thread_send_msg(mngt_q_ctx->thread, _cache_mngt_queue_stop, mngt_q_ctx);
	} else {
		_cache_mngt_queue_stop(mngt_q_ctx);
	}
}

static void
vbdev_ocf_cache_mngt_queue_kick(ocf_queue_t queue)
{
}

const struct ocf_queue_ops cache_mngt_queue_ops = {
	.kick_sync = NULL,
	.kick = vbdev_ocf_cache_mngt_queue_kick,
	.stop = vbdev_ocf_cache_mngt_queue_stop,
};

int
vbdev_ocf_cache_mngt_queue_create(struct vbdev_ocf_cache *cache)
{
	struct vbdev_ocf_cache_mngt_queue_ctx *mngt_q_ctx;
	int rc = 0;

	SPDK_DEBUGLOG(vbdev_ocf, "OCF cache '%s': creating OCF management queue\n", cache->name);

	mngt_q_ctx = calloc(1, sizeof(struct vbdev_ocf_cache_mngt_queue_ctx));
	if (!mngt_q_ctx) {
		SPDK_ERRLOG("OCF cache '%s': failed to allocate memory for management queue context\n",
			    cache->name);
		return -ENOMEM;
	}

	if ((rc = vbdev_ocf_queue_create_mngt(cache->ocf_cache, &cache->ocf_cache_mngt_q, &cache_mngt_queue_ops))) {
		SPDK_ERRLOG("OCF cache '%s': failed to create OCF management queue\n", cache->name);
		free(mngt_q_ctx);
		return rc;
	}
	ocf_queue_set_priv(cache->ocf_cache_mngt_q, mngt_q_ctx);

	mngt_q_ctx->poller = SPDK_POLLER_REGISTER(vbdev_ocf_queue_poller, cache->ocf_cache_mngt_q, 1000);
	if (!mngt_q_ctx->poller) {
		SPDK_ERRLOG("OCF cache '%s': failed to create management queue poller\n", cache->name);
		vbdev_ocf_queue_put(cache->ocf_cache_mngt_q);
		return -ENOMEM;
	}

	mngt_q_ctx->cache = cache; // keep? (only for DEBUGLOG)
	mngt_q_ctx->thread = spdk_get_thread();

	return rc;
}

struct vbdev_ocf_cache *
vbdev_ocf_cache_get_by_name(const char *cache_name)
{
	struct vbdev_ocf_cache *cache;

	vbdev_ocf_foreach_cache(cache) {
		if (strcmp(cache_name, cache->name)) {
			continue;
		}
		return cache;
	}
	return NULL;
}

bool
vbdev_ocf_cache_is_running(struct vbdev_ocf_cache *cache)
{
	ocf_cache_t ocf_cache = cache->ocf_cache;

	if (ocf_cache && ocf_cache_is_running(ocf_cache)) {
		return true;
	}
	return false;
}

bool
vbdev_ocf_cache_is_incomplete(struct vbdev_ocf_cache *cache)
{
	return !!cache->init_params;
}
