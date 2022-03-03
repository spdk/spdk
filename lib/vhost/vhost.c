/*-
 *   BSD LICENSE
 *
 *   Copyright(c) Intel Corporation. All rights reserved.
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

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/memory.h"
#include "spdk/barrier.h"
#include "spdk/vhost.h"
#include "vhost_internal.h"

static struct spdk_cpuset g_vhost_core_mask;

static TAILQ_HEAD(, spdk_vhost_dev) g_vhost_devices = TAILQ_HEAD_INITIALIZER(
			g_vhost_devices);
static pthread_mutex_t g_vhost_mutex = PTHREAD_MUTEX_INITIALIZER;

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

int
vhost_dev_register(struct spdk_vhost_dev *vdev, const char *name, const char *mask_str,
		   const struct spdk_vhost_dev_backend *backend)
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

	if (spdk_vhost_dev_find(name)) {
		SPDK_ERRLOG("vhost controller %s already exists.\n", name);
		return -EEXIST;
	}

	vdev->name = strdup(name);
	if (vdev->name == NULL) {
		return -EIO;
	}

	rc = vhost_user_dev_register(vdev, name, &cpumask, backend);
	if (rc != 0) {
		free(vdev->name);
		return rc;
	}

	TAILQ_INSERT_TAIL(&g_vhost_devices, vdev, tailq);

	SPDK_INFOLOG(vhost, "Controller %s: new controller added\n", vdev->name);
	return 0;
}

int
vhost_dev_unregister(struct spdk_vhost_dev *vdev)
{
	int rc;

	rc = vhost_user_dev_unregister(vdev);
	if (rc != 0) {
		return rc;
	}

	SPDK_INFOLOG(vhost, "Controller %s: removed\n", vdev->name);

	free(vdev->name);
	TAILQ_REMOVE(&g_vhost_devices, vdev, tailq);
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
spdk_vhost_init(spdk_vhost_init_cb init_cb)
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

static spdk_vhost_fini_cb g_fini_cb;

static void
vhost_fini(void *arg1)
{
	struct spdk_vhost_dev *vdev, *tmp;

	spdk_vhost_lock();
	vdev = spdk_vhost_dev_next(NULL);
	while (vdev != NULL) {
		tmp = spdk_vhost_dev_next(vdev);
		spdk_vhost_dev_remove(vdev);
		/* don't care if it fails, there's nothing we can do for now */
		vdev = tmp;
	}
	spdk_vhost_unlock();

	g_fini_cb();
}

void
spdk_vhost_fini(spdk_vhost_fini_cb fini_cb)
{
	g_fini_cb = fini_cb;

	vhost_user_fini(vhost_fini);
}

void
spdk_vhost_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_vhost_dev *vdev;
	uint32_t delay_base_us;
	uint32_t iops_threshold;

	spdk_json_write_array_begin(w);

	spdk_vhost_lock();
	for (vdev = spdk_vhost_dev_next(NULL); vdev != NULL;
	     vdev = spdk_vhost_dev_next(vdev)) {
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
	spdk_vhost_unlock();

	spdk_json_write_array_end(w);
}

SPDK_LOG_REGISTER_COMPONENT(vhost)
SPDK_LOG_REGISTER_COMPONENT(vhost_ring)
