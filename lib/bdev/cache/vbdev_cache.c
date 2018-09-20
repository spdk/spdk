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
#include "vbdev_cache.h"
#include "stats.h"

#include "spdk/bdev_module.h"
#include "spdk/conf.h"
#include "spdk/io_channel.h"
#include "spdk/string.h"
#include "spdk_internal/log.h"

#define INTEL_OCF_PRODUCT_NAME "Intel(R) CAS"

#define SPDK_GET_BDEV_MODULE(name) &name ## _if

static struct spdk_bdev_module cache_if;

static uint32_t opencas_refcnt = 0;

static TAILQ_HEAD(, vbdev_cache) g_ocf_vbdev_head
	= TAILQ_HEAD_INITIALIZER(g_ocf_vbdev_head);

static bool g_shutdown_started = false;

/* Free allocated strings and structure itself
 * Used at shutdown only */
static int
free_vbdev(struct vbdev_cache *vbdev)
{
	if (vbdev) {
		free(vbdev->name);
		free(vbdev->cache.name);
		free(vbdev->core.name);
		free(vbdev);
		return 0;
	} else {
		return -EFAULT;
	}
}

/* Stop OCF cache object
 * vbdev_cache is not operational after this */
static int
stop_vbdev(struct vbdev_cache *vbdev)
{
	if (NULL == vbdev) {
		return -EFAULT;
	}

	if (NULL == vbdev->ocf_cache) {
		return -EFAULT;
	}

	if (!ocf_cache_is_running(vbdev->ocf_cache)) {
		return -EINVAL;
	}

	if (ocf_mngt_cache_stop(vbdev->ocf_cache)) {
		SPDK_ERRLOG("Could not stop cache for \"%s\"\n", vbdev->name);
		return -EPERM;
	}

	return 0;
}

/* Release SPDK and OCF objects associated with base */
static int
remove_base(struct vbdev_cache_base *base)
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
	spdk_bdev_module_release_bdev(base->bdev);
	if (!g_shutdown_started && base->desc) {
		spdk_bdev_close(base->desc);
	}

	base->attached = false;
	return rc;
}

static void
io_device_unregister_cb(void *ctx)
{
	struct vbdev_cache *vbdev = ctx;

	stop_vbdev(vbdev);
	remove_base(&vbdev->core);
	remove_base(&vbdev->cache);

	SPDK_DEBUGLOG(SPDK_TRACE_VBDEV_CACHE,
		      "Succesfully unregistered cache io device \"%s\"\n", vbdev->name);
}

/* Unregister io_device, release base devices
 * This function is called during spdk_bdev_unregister */
static int
vbdev_cache_destruct(void *opaque)
{
	struct vbdev_cache *vbdev = opaque;

	if (vbdev == NULL) {
		return -EFAULT;
	}
	if (vbdev->state.doing_finish) {
		return 0;
	}

	vbdev->state.doing_finish = true;
	spdk_io_device_unregister(vbdev, io_device_unregister_cb);

	return 0;
}

/* If vbdev is online, return its object */
struct vbdev_cache *
vbdev_cache_get_by_name(const char *name)
{
	struct vbdev_cache *vbdev;
	TAILQ_FOREACH(vbdev, &g_ocf_vbdev_head, tailq) {
		if (!vbdev || vbdev->name == NULL || vbdev->state.doing_finish) {
			continue;
		}
		if (!strcmp(vbdev->name, name)) {
			return vbdev;
		}
	}
	return NULL;
}

/* Called from OCF when SPDK_IO is completed  */
static void
opencas_io_submit_cb(struct ocf_io *io, int error)
{
	struct spdk_bdev_io *bdev_io = io->priv1;
	struct bdev_ocf_data *data = io->priv2;

	if (0 == error) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
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
		dir = bdev_io->type == SPDK_BDEV_IO_TYPE_READ ? OCF_READ : OCF_WRITE;
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
io_handle(struct spdk_bdev_io *bdev_io)
{
	struct vbdev_cache *vbdev = bdev_io->bdev->ctxt;
	struct ocf_io *io;
	struct bdev_ocf_data *data;
	int err;

	io = ocf_new_io(vbdev->ocf_core);
	if (!io) {
		err = -ENOMEM;
		goto err_alloc_io;
	}

	data = opencas_data_from_spdk_io(bdev_io);
	err = ocf_io_set_data(io, data, 0);
	if (err) {
		goto err_submit_io;
	}

	ocf_io_set_queue(io, 0);
	ocf_io_set_cmpl(io, bdev_io, data, opencas_io_submit_cb);

	err = io_submit_to_ocf(bdev_io, io);
	if (err) {
		goto err_submit_io;
	}

	return;

err_submit_io:
	ocf_io_put(io);
	opencas_data_free(data);
err_alloc_io:
	opencas_io_submit_cb(io, err);
}

static void
buf_alloc_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	io_handle(bdev_io);
}

/* Called from bdev layer when an io to Cache vbdev is submitted */
static void
vbdev_cache_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		if (bdev_io->u.bdev.iovcnt > 0 && NULL == bdev_io->u.bdev.iovs[0].iov_base) {
			spdk_bdev_io_get_buf(bdev_io, buf_alloc_cb,
					     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		} else {
			io_handle(bdev_io);
		}
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		io_handle(bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	default:
		SPDK_ERRLOG("Unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
}

/* Called from bdev layer */
static bool
vbdev_cache_io_type_supported(void *opaque, enum spdk_bdev_io_type io_type)
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
vbdev_cache_get_io_channel(void *opaque)
{
	struct vbdev_cache *bdev = opaque;
	return spdk_get_io_channel(bdev);
}

static int
vbdev_cache_dump_config_json(void *opaque, struct spdk_json_write_ctx *w)
{
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
	.destruct = vbdev_cache_destruct,
	.io_type_supported = vbdev_cache_io_type_supported,
	.submit_request	= vbdev_cache_submit_request,
	.get_io_channel	= vbdev_cache_get_io_channel,
	.dump_info_json = vbdev_cache_dump_config_json,
};

/* Start OCF cache, attach caching device */
static int
start_cache(struct vbdev_cache *dev)
{
	void *data_ptr = &dev->cache;

	if (ocf_mngt_cache_start(opencas_ctx, &dev->ocf_cache, &dev->cfg.cache)) {
		SPDK_ERRLOG("Failed to start cache instance\n");
		return -EPERM;
	}
	dev->cache.id = ocf_cache_get_id(dev->ocf_cache);

	dev->cfg.device.uuid.data = &data_ptr;
	dev->cfg.device.uuid.size = sizeof(void *);

	if (ocf_mngt_cache_attach(dev->ocf_cache, &dev->cfg.device)) {
		SPDK_ERRLOG("Failed to attach cache device\n");
		return -EPERM;
	}

	return 0;
}

/* Add core for existing OCF cache instance */
static int
add_core(struct vbdev_cache *dev)
{
	void *data_ptr = &dev->core;

	dev->cfg.core.uuid.data = &data_ptr;
	dev->cfg.core.uuid.size = sizeof(void *);

	if (ocf_mngt_cache_add_core(dev->ocf_cache, &dev->ocf_core, &dev->cfg.core)) {
		SPDK_ERRLOG("Failed to add core device to cache instance\n");
		return -EPERM;
	}

	dev->core.id = ocf_core_get_id(dev->ocf_core);

	return 0;
}

/* Called on cache vbdev creation at every thread */
static int
io_device_create_cb(void *io_device, void *ctx_buf)
{
	struct vbdev_cache *vbdev = io_device;
	vbdev->cache.base_channel = spdk_bdev_get_io_channel(vbdev->cache.desc);
	vbdev->core.base_channel = spdk_bdev_get_io_channel(vbdev->core.desc);
	return 0;
}

static void
io_device_destroy_cb(void *io_device, void *ctx_buf)
{
	struct vbdev_cache *vbdev = io_device;
	spdk_put_io_channel(vbdev->cache.base_channel);
	spdk_put_io_channel(vbdev->core.base_channel);
}

/* Start OCF cache and register vbdev_cache at bdev layer */
static int
register_vbdev(struct vbdev_cache *dev)
{
	int result = 0;

	if ((result = start_cache(dev))) {
		SPDK_ERRLOG("Failed to start cache instance\n");
		return result;
	}

	if ((result = add_core(dev))) {
		SPDK_ERRLOG("Failed to add core to cache instance\n");
		return result;
	}

	/*
	 * Below we will create exported spdk object
	 */

	spdk_io_device_register(dev, io_device_create_cb, io_device_destroy_cb,
				0, dev->name);

	/* Copy properties of the base bdev */
	dev->exp_bdev.blocklen = dev->core.bdev->blocklen;
	dev->exp_bdev.write_cache = dev->core.bdev->write_cache;
	dev->exp_bdev.required_alignment = dev->core.bdev->required_alignment;

	dev->exp_bdev.name = dev->name;
	dev->exp_bdev.product_name = INTEL_OCF_PRODUCT_NAME;

	dev->exp_bdev.blockcnt = dev->core.bdev->blockcnt;
	dev->exp_bdev.ctxt = dev;
	dev->exp_bdev.fn_table = &cache_dev_fn_table;
	dev->exp_bdev.module = SPDK_GET_BDEV_MODULE(cache);

	/* Finally register cache volume in SPDK */
	spdk_vbdev_register(&dev->exp_bdev, NULL, 2);

	return result;
}

/* Claim base bdevs and register main cache vbdev */
static int
claim_vbdev(struct vbdev_cache *vbdev)
{
	int status = 0;

	if (!vbdev->cache.attached || !vbdev->core.attached) {
		return -EPERM;
	}

	/* Claim the cache and core devices for exclusive use */
	status |= spdk_bdev_module_claim_bdev(vbdev->cache.bdev, vbdev->cache.desc,
					      SPDK_GET_BDEV_MODULE(cache));
	if (status) {
		SPDK_ERRLOG("Can't claim bdev %s\n", spdk_bdev_get_name(vbdev->cache.bdev));
	}
	status |= spdk_bdev_module_claim_bdev(vbdev->core.bdev, vbdev->core.desc,
					      SPDK_GET_BDEV_MODULE(cache));
	if (status) {
		SPDK_ERRLOG("Can't claim bdev %s\n", spdk_bdev_get_name(vbdev->core.bdev));
	}

	/* Close underlying devices in case we can't claim them */
	if (status) {
		spdk_bdev_close(vbdev->cache.desc);
		spdk_bdev_close(vbdev->core.desc);
		return -EBUSY;
	}

	status = register_vbdev(vbdev);

	if (status) {
		SPDK_ERRLOG("Error while create cache instance status=%d\n", status);
	}
	return status;
}

static void
init_vbdev_config(struct vbdev_cache *vbdev)
{
	vbdev->cfg.cache.id = 0;
	vbdev->cfg.cache.name = vbdev->name;
	vbdev->cfg.cache.name_size = strlen(vbdev->name) + 1;
	vbdev->cfg.cache.metadata_volatile = true;
	vbdev->cfg.cache.cache_line_size = ocf_cache_line_size_4;
	vbdev->cfg.cache.backfill.max_queue_size = 65536;
	vbdev->cfg.cache.backfill.queue_unblock_size = 60000;
	vbdev->cfg.cache.io_queues = 1;

	vbdev->cfg.device.cache_line_size = ocf_cache_line_size_4;
	vbdev->cfg.device.force = true;
	vbdev->cfg.device.min_free_ram = 2000;
	vbdev->cfg.device.perform_test = false;
	vbdev->cfg.device.discard_on_start = false;

	vbdev->cfg.core.data_obj_type = SPDK_OBJECT;
}

/* Allocate vbdev structure object and add it to the global list */
static struct vbdev_cache *
init_vbdev(const char *vbdev_name,
	   const char *cache_mode_name,
	   const char *cache_name,
	   const char *core_name)
{
	struct vbdev_cache *vbdev;

	if (spdk_bdev_get_by_name(vbdev_name)) {
		SPDK_ERRLOG("Device with name \"%s\" already exists", vbdev_name);
		return NULL;
	}

	vbdev = calloc(1, sizeof(*vbdev));
	if (!vbdev) {
		SPDK_ERRLOG("Can't allocate memory for cache context\n");
		return NULL;
	}
	vbdev->cache.parent = vbdev;
	vbdev->core.parent = vbdev;

	if (cache_mode_name) {
		vbdev->cfg.cache.cache_mode
			= ocf_get_cache_mode(cache_mode_name);
	} else {
		SPDK_ERRLOG("No cache mode specified\n");
		goto error_free;
	}
	if (-1 == vbdev->cfg.cache.cache_mode) {
		SPDK_ERRLOG("Incorrect cache mode \"%s\"\n", cache_mode_name);
		goto error_free;
	}

	vbdev->name = strdup(vbdev_name);
	if (!vbdev->name) {
		SPDK_ERRLOG("Memory allocation error\n");
		goto error_free;
	}

	vbdev->cache.name = strdup(cache_name);
	if (!vbdev->cache.name) {
		SPDK_ERRLOG("Memory allocation error\n");
		goto error_free;
	}

	vbdev->core.name = strdup(core_name);
	if (!vbdev->core.name) {
		SPDK_ERRLOG("Memory allocation error\n");
		goto error_free;
	}

	init_vbdev_config(vbdev);

	TAILQ_INSERT_TAIL(&g_ocf_vbdev_head, vbdev, tailq);
	return vbdev;
error_free:
	free_vbdev(vbdev);
	return NULL;
}

/* Read configuration file at start the of SPDK application
 * This adds vbdevs to global list if some mentioned in config */
static int
vbdev_cache_init(void)
{
	const char *vbdev_name,
	      *modename,
	      *cache_name,
	      *core_name;
	struct spdk_conf_section *sp;
	int status = 0;

	status = opencas_init();
	if (0 != status) {
		goto end;
	}

	sp = spdk_conf_find_section(NULL, "Cache");
	if (sp == NULL) {
		goto end;
	}

	for (int i = 0; ; i++) {
		if (!spdk_conf_section_get_nval(sp, "CAS", i)) {
			break;
		}

		vbdev_name = spdk_conf_section_get_nmval(sp, "CAS", i, 0);
		if (NULL == vbdev_name) {
			SPDK_ERRLOG("No volume name specified\n");
			continue;
		}

		modename = spdk_conf_section_get_nmval(sp, "CAS", i, 1);
		if (NULL == modename) {
			SPDK_ERRLOG("No modename specified for Cache volume \"%s\"\n", vbdev_name);
			continue;
		}

		cache_name = spdk_conf_section_get_nmval(sp, "CAS", i, 2);
		if (NULL == cache_name) {
			SPDK_ERRLOG("No cache device specified for Cache volume \"%s\"\n", vbdev_name);
			continue;
		}

		core_name = spdk_conf_section_get_nmval(sp, "CAS", i, 3);
		if (NULL == core_name) {
			SPDK_ERRLOG("No core devices specified for Cache volume \"%s\"\n", vbdev_name);
			continue;
		}

		if (NULL == init_vbdev(vbdev_name, modename, cache_name, core_name)) {
			status |= -1;
		}
	}

end:
	SPDK_DEBUGLOG(SPDK_TRACE_VBDEV_CACHE, "Finished cache initialization status=%d\n", status);
	return status;
}

/* Called at application shutdown */
static void
vbdev_cache_fini_start(void)
{
	g_shutdown_started = true;
}

/* Called after application shutdown started
 * Release memory of allocated structures here */
static void
vbdev_cache_module_fini(void)
{
	struct vbdev_cache *vbdev;

	while ((vbdev = TAILQ_FIRST(&g_ocf_vbdev_head))) {
		TAILQ_REMOVE(&g_ocf_vbdev_head, vbdev, tailq);
		free_vbdev(vbdev);
		vbdev = NULL;
	}

	opencas_cleanup();
}

/* Open base SPDK bdev */
static int
attach_base(struct vbdev_cache_base *base, struct spdk_bdev *bdev)
{
	int status = 0;

	status = spdk_bdev_open(bdev, true, NULL, NULL, &base->desc);

	if (status) {
		SPDK_ERRLOG("Can't open device %s for writing\n", spdk_bdev_get_name(bdev));
		return status;
	}

	base->bdev = bdev;
	base->attached = true;
	return status;
}

/* Attach base bdevs
 * If they attached, start vbdev
 * otherwise wait for them to appear at examine */
static int
create_from_bdevs(struct vbdev_cache *vbdev,
		  struct spdk_bdev *cache_bdev, struct spdk_bdev *core_bdev)
{
	int rc = 0;

	if (cache_bdev) {
		rc |= attach_base(&vbdev->cache, cache_bdev);
	}
	if (core_bdev) {
		rc |= attach_base(&vbdev->core, core_bdev);
	}
	if (rc == 0) {
		rc = claim_vbdev(vbdev);
	}

	return rc;
}

/* Init and then start vbdev if all base devices are present */
int
vbdev_cache_construct(const char *vbdev_name,
		      const char *cache_mode_name,
		      const char *cache_name,
		      const char *core_name)
{
	struct spdk_bdev *cache_bdev = spdk_bdev_get_by_name(cache_name);
	struct spdk_bdev *core_bdev = spdk_bdev_get_by_name(core_name);
	struct vbdev_cache *vbdev =
		init_vbdev(vbdev_name, cache_mode_name, cache_name, core_name);

	if (vbdev == NULL) {
		return -EPERM;
	}

	if (cache_bdev == NULL) {
		SPDK_NOTICELOG("Cache vbdev \"%s\" is waiting for cache device to connect", vbdev->name);
	}
	if (core_bdev == NULL) {
		SPDK_NOTICELOG("Cache vbdev \"%s\" is waiting for core device to connect", vbdev->name);
	}

	return create_from_bdevs(vbdev, cache_bdev, core_bdev);
}

/* This called if new device is created in SPDK application
 * If that device named as one of base bdevs of cache_vbdev,
 * attach them
 * If last device attached here, vbdev starts here */
static void
vbdev_cache_examine(struct spdk_bdev *bdev)
{
	const char *bdev_name = spdk_bdev_get_name(bdev);
	struct vbdev_cache *vbdev, *tmp;

	TAILQ_FOREACH_SAFE(vbdev, &g_ocf_vbdev_head, tailq, tmp) {
		if (!vbdev || vbdev->state.doing_finish) {
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
	.module_init = vbdev_cache_init,
	.fini_start = vbdev_cache_fini_start,
	.module_fini = vbdev_cache_module_fini,
	.config_text = NULL,
	.get_ctx_size = NULL,
	.examine_config = vbdev_cache_examine,
};
SPDK_BDEV_MODULE_REGISTER(&cache_if);

SPDK_LOG_REGISTER_COMPONENT("vbdev_cache", SPDK_TRACE_VBDEV_CACHE)
