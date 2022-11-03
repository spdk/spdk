/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/memory.h"
#include "spdk/barrier.h"
#include "spdk/vhost.h"
#include "vhost_internal.h"
#include "spdk/queue.h"


static struct spdk_cpuset g_vhost_core_mask;

static TAILQ_HEAD(, spdk_vhost_dev) g_vhost_devices = TAILQ_HEAD_INITIALIZER(
			g_vhost_devices);
static pthread_mutex_t g_vhost_mutex = PTHREAD_MUTEX_INITIALIZER;

static TAILQ_HEAD(, spdk_virtio_blk_transport) g_virtio_blk_transports = TAILQ_HEAD_INITIALIZER(
			g_virtio_blk_transports);

static spdk_vhost_fini_cb g_fini_cb;

struct spdk_vhost_dev *
spdk_vhost_dev_next(struct spdk_vhost_dev *vdev)
{
	if (vdev == NULL) {
		return TAILQ_FIRST(&g_vhost_devices);
	}

	return TAILQ_NEXT(vdev, tailq);
}

struct spdk_vhost_dev *
spdk_vhost_dev_find(const char *ctrlr_name)
{
	struct spdk_vhost_dev *vdev;

	TAILQ_FOREACH(vdev, &g_vhost_devices, tailq) {
		if (strcmp(vdev->name, ctrlr_name) == 0) {
			return vdev;
		}
	}

	return NULL;
}

static int
vhost_parse_core_mask(const char *mask, struct spdk_cpuset *cpumask)
{
	int rc;
	struct spdk_cpuset negative_vhost_mask;

	if (cpumask == NULL) {
		return -1;
	}

	if (mask == NULL) {
		spdk_cpuset_copy(cpumask, &g_vhost_core_mask);
		return 0;
	}

	rc = spdk_cpuset_parse(cpumask, mask);
	if (rc < 0) {
		SPDK_ERRLOG("invalid cpumask %s\n", mask);
		return -1;
	}

	spdk_cpuset_copy(&negative_vhost_mask, &g_vhost_core_mask);
	spdk_cpuset_negate(&negative_vhost_mask);
	spdk_cpuset_and(&negative_vhost_mask, cpumask);

	if (spdk_cpuset_count(&negative_vhost_mask) != 0) {
		SPDK_ERRLOG("one of selected cpu is outside of core mask(=%s)\n",
			    spdk_cpuset_fmt(&g_vhost_core_mask));
		return -1;
	}

	spdk_cpuset_and(cpumask, &g_vhost_core_mask);

	if (spdk_cpuset_count(cpumask) == 0) {
		SPDK_ERRLOG("no cpu is selected among core mask(=%s)\n",
			    spdk_cpuset_fmt(&g_vhost_core_mask));
		return -1;
	}

	return 0;
}

TAILQ_HEAD(, virtio_blk_transport_ops_list_element)
g_spdk_virtio_blk_transport_ops = TAILQ_HEAD_INITIALIZER(g_spdk_virtio_blk_transport_ops);

const struct spdk_virtio_blk_transport_ops *
virtio_blk_get_transport_ops(const char *transport_name)
{
	struct virtio_blk_transport_ops_list_element *ops;
	TAILQ_FOREACH(ops, &g_spdk_virtio_blk_transport_ops, link) {
		if (strcasecmp(transport_name, ops->ops.name) == 0) {
			return &ops->ops;
		}
	}
	return NULL;
}

int
vhost_dev_register(struct spdk_vhost_dev *vdev, const char *name, const char *mask_str,
		   const struct spdk_json_val *params,
		   const struct spdk_vhost_dev_backend *backend,
		   const struct spdk_vhost_user_dev_backend *user_backend)
{
	struct spdk_cpuset cpumask = {};
	int rc;

	assert(vdev);
	if (name == NULL) {
		SPDK_ERRLOG("Can't register controller with no name\n");
		return -EINVAL;
	}

	if (vhost_parse_core_mask(mask_str, &cpumask) != 0) {
		SPDK_ERRLOG("cpumask %s is invalid (core mask is 0x%s)\n",
			    mask_str, spdk_cpuset_fmt(&g_vhost_core_mask));
		return -EINVAL;
	}

	spdk_vhost_lock();
	if (spdk_vhost_dev_find(name)) {
		SPDK_ERRLOG("vhost controller %s already exists.\n", name);
		spdk_vhost_unlock();
		return -EEXIST;
	}

	vdev->name = strdup(name);
	if (vdev->name == NULL) {
		spdk_vhost_unlock();
		return -EIO;
	}

	vdev->backend = backend;
	if (vdev->backend->type == VHOST_BACKEND_SCSI) {
		rc = vhost_user_dev_register(vdev, name, &cpumask, user_backend);
	} else {
		rc = virtio_blk_construct_ctrlr(vdev, name, &cpumask, params, user_backend);
	}
	if (rc != 0) {
		free(vdev->name);
		spdk_vhost_unlock();
		return rc;
	}

	TAILQ_INSERT_TAIL(&g_vhost_devices, vdev, tailq);
	spdk_vhost_unlock();

	SPDK_INFOLOG(vhost, "Controller %s: new controller added\n", vdev->name);
	return 0;
}

int
vhost_dev_unregister(struct spdk_vhost_dev *vdev)
{
	int rc;

	if (vdev->backend->type == VHOST_BACKEND_SCSI) {
		rc = vhost_user_dev_unregister(vdev);
	} else {
		rc = virtio_blk_destroy_ctrlr(vdev);
	}
	if (rc != 0) {
		return rc;
	}

	SPDK_INFOLOG(vhost, "Controller %s: removed\n", vdev->name);

	free(vdev->name);

	spdk_vhost_lock();
	TAILQ_REMOVE(&g_vhost_devices, vdev, tailq);
	if (TAILQ_EMPTY(&g_vhost_devices) && g_fini_cb != NULL) {
		g_fini_cb();
	}
	spdk_vhost_unlock();

	return 0;
}

const char *
spdk_vhost_dev_get_name(struct spdk_vhost_dev *vdev)
{
	assert(vdev != NULL);
	return vdev->name;
}

const struct spdk_cpuset *
spdk_vhost_dev_get_cpumask(struct spdk_vhost_dev *vdev)
{
	assert(vdev != NULL);
	return spdk_thread_get_cpumask(vdev->thread);
}

void
vhost_dump_info_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	assert(vdev->backend->dump_info_json != NULL);
	vdev->backend->dump_info_json(vdev, w);
}

int
spdk_vhost_dev_remove(struct spdk_vhost_dev *vdev)
{
	return vdev->backend->remove_device(vdev);
}

int
spdk_vhost_set_coalescing(struct spdk_vhost_dev *vdev, uint32_t delay_base_us,
			  uint32_t iops_threshold)
{
	assert(vdev->backend->set_coalescing != NULL);
	return vdev->backend->set_coalescing(vdev, delay_base_us, iops_threshold);
}

void
spdk_vhost_get_coalescing(struct spdk_vhost_dev *vdev, uint32_t *delay_base_us,
			  uint32_t *iops_threshold)
{
	assert(vdev->backend->get_coalescing != NULL);
	vdev->backend->get_coalescing(vdev, delay_base_us, iops_threshold);
}

void
spdk_vhost_lock(void)
{
	pthread_mutex_lock(&g_vhost_mutex);
}

int
spdk_vhost_trylock(void)
{
	return -pthread_mutex_trylock(&g_vhost_mutex);
}

void
spdk_vhost_unlock(void)
{
	pthread_mutex_unlock(&g_vhost_mutex);
}

void
spdk_vhost_scsi_init(spdk_vhost_init_cb init_cb)
{
	uint32_t i;
	int ret = 0;

	ret = vhost_user_init();
	if (ret != 0) {
		init_cb(ret);
		return;
	}

	spdk_cpuset_zero(&g_vhost_core_mask);
	SPDK_ENV_FOREACH_CORE(i) {
		spdk_cpuset_set_cpu(&g_vhost_core_mask, i, true);
	}
	init_cb(ret);
}

static void
vhost_fini(void)
{
	struct spdk_vhost_dev *vdev, *tmp;

	if (spdk_vhost_dev_next(NULL) == NULL) {
		g_fini_cb();
		return;
	}

	vdev = spdk_vhost_dev_next(NULL);
	while (vdev != NULL) {
		tmp = spdk_vhost_dev_next(vdev);
		spdk_vhost_dev_remove(vdev);
		/* don't care if it fails, there's nothing we can do for now */
		vdev = tmp;
	}

	/* g_fini_cb will get called when last device is unregistered. */
}

void
spdk_vhost_blk_init(spdk_vhost_init_cb init_cb)
{
	uint32_t i;
	int ret = 0;

	ret = virtio_blk_transport_create("vhost_user_blk", NULL);
	if (ret != 0) {
		goto out;
	}

	spdk_cpuset_zero(&g_vhost_core_mask);
	SPDK_ENV_FOREACH_CORE(i) {
		spdk_cpuset_set_cpu(&g_vhost_core_mask, i, true);
	}
out:
	init_cb(ret);
}

void
spdk_vhost_scsi_fini(spdk_vhost_fini_cb fini_cb)
{
	g_fini_cb = fini_cb;

	vhost_user_fini(vhost_fini);
}

static void
virtio_blk_transports_destroy(void)
{
	struct spdk_virtio_blk_transport *transport = TAILQ_FIRST(&g_virtio_blk_transports);

	if (transport == NULL) {
		g_fini_cb();
		return;
	}
	TAILQ_REMOVE(&g_virtio_blk_transports, transport, tailq);
	virtio_blk_transport_destroy(transport, virtio_blk_transports_destroy);
}

void
spdk_vhost_blk_fini(spdk_vhost_fini_cb fini_cb)
{
	g_fini_cb = fini_cb;

	virtio_blk_transports_destroy();
}

static void
vhost_user_config_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	uint32_t delay_base_us;
	uint32_t iops_threshold;

	vdev->backend->write_config_json(vdev, w);

	spdk_vhost_get_coalescing(vdev, &delay_base_us, &iops_threshold);
	if (delay_base_us) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "vhost_controller_set_coalescing");

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "ctrlr", vdev->name);
		spdk_json_write_named_uint32(w, "delay_base_us", delay_base_us);
		spdk_json_write_named_uint32(w, "iops_threshold", iops_threshold);
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}
}

void
spdk_vhost_scsi_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_vhost_dev *vdev;

	spdk_json_write_array_begin(w);

	spdk_vhost_lock();
	for (vdev = spdk_vhost_dev_next(NULL); vdev != NULL;
	     vdev = spdk_vhost_dev_next(vdev)) {
		if (vdev->backend->type == VHOST_BACKEND_SCSI) {
			vhost_user_config_json(vdev, w);
		}
	}
	spdk_vhost_unlock();

	spdk_json_write_array_end(w);
}

static void
vhost_blk_dump_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_virtio_blk_transport *transport;

	/* Write vhost transports */
	TAILQ_FOREACH(transport, &g_virtio_blk_transports, tailq) {
		/* Since vhost_user_blk is always added on SPDK startup,
		 * do not emit virtio_blk_create_transport RPC. */
		if (strcasecmp(transport->ops->name, "vhost_user_blk") != 0) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "method", "virtio_blk_create_transport");
			spdk_json_write_named_object_begin(w, "params");
			transport->ops->dump_opts(transport, w);
			spdk_json_write_object_end(w);
			spdk_json_write_object_end(w);
		}
	}
}

void
spdk_vhost_blk_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_vhost_dev *vdev;

	spdk_json_write_array_begin(w);

	spdk_vhost_lock();
	for (vdev = spdk_vhost_dev_next(NULL); vdev != NULL;
	     vdev = spdk_vhost_dev_next(vdev)) {
		if (vdev->backend->type == VHOST_BACKEND_BLK) {
			vhost_user_config_json(vdev, w);
		}
	}
	spdk_vhost_unlock();

	vhost_blk_dump_config_json(w);

	spdk_json_write_array_end(w);
}

void
virtio_blk_transport_register(const struct spdk_virtio_blk_transport_ops *ops)
{
	struct virtio_blk_transport_ops_list_element *new_ops;

	if (virtio_blk_get_transport_ops(ops->name) != NULL) {
		SPDK_ERRLOG("Double registering virtio blk transport type %s.\n", ops->name);
		assert(false);
		return;
	}

	new_ops = calloc(1, sizeof(*new_ops));
	if (new_ops == NULL) {
		SPDK_ERRLOG("Unable to allocate memory to register new transport type %s.\n", ops->name);
		assert(false);
		return;
	}

	new_ops->ops = *ops;

	TAILQ_INSERT_TAIL(&g_spdk_virtio_blk_transport_ops, new_ops, link);
}

int
virtio_blk_transport_create(const char *transport_name,
			    const struct spdk_json_val *params)
{
	const struct spdk_virtio_blk_transport_ops *ops = NULL;
	struct spdk_virtio_blk_transport *transport;

	TAILQ_FOREACH(transport, &g_virtio_blk_transports, tailq) {
		if (strcasecmp(transport->ops->name, transport_name) == 0) {
			return -EEXIST;
		}
	}

	ops = virtio_blk_get_transport_ops(transport_name);
	if (!ops) {
		SPDK_ERRLOG("Transport type '%s' unavailable.\n", transport_name);
		return -ENOENT;
	}

	transport = ops->create(params);
	if (!transport) {
		SPDK_ERRLOG("Unable to create new transport of type %s\n", transport_name);
		return -EPERM;
	}

	transport->ops = ops;
	TAILQ_INSERT_TAIL(&g_virtio_blk_transports, transport, tailq);
	return 0;
}

struct spdk_virtio_blk_transport *
virtio_blk_transport_get_first(void)
{
	return TAILQ_FIRST(&g_virtio_blk_transports);
}

struct spdk_virtio_blk_transport *
virtio_blk_transport_get_next(struct spdk_virtio_blk_transport *transport)
{
	return TAILQ_NEXT(transport, tailq);
}

void
virtio_blk_transport_dump_opts(struct spdk_virtio_blk_transport *transport,
			       struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "name", transport->ops->name);

	if (transport->ops->dump_opts) {
		transport->ops->dump_opts(transport, w);
	}

	spdk_json_write_object_end(w);
}

struct spdk_virtio_blk_transport *
virtio_blk_tgt_get_transport(const char *transport_name)
{
	struct spdk_virtio_blk_transport *transport;

	TAILQ_FOREACH(transport, &g_virtio_blk_transports, tailq) {
		if (strcasecmp(transport->ops->name, transport_name) == 0) {
			return transport;
		}
	}
	return NULL;
}

int
virtio_blk_transport_destroy(struct spdk_virtio_blk_transport *transport,
			     spdk_vhost_fini_cb cb_fn)
{
	return transport->ops->destroy(transport, cb_fn);
}

SPDK_LOG_REGISTER_COMPONENT(vhost)
SPDK_LOG_REGISTER_COMPONENT(vhost_ring)
