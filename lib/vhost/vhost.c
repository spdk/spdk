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
#include "spdk/util.h"

#include "spdk/vhost.h"
#include "vhost_internal.h"
#include "task.h"
#include "vhost_iommu.h"

static uint32_t g_num_ctrlrs[RTE_MAX_LCORE];

/* Path to folder where character device will be created. Can be set by user. */
static char dev_dirname[PATH_MAX] = "";

#define MAX_VHOST_DEVICES	15

static struct spdk_vhost_dev *g_spdk_vhost_devices[MAX_VHOST_DEVICES];

void *spdk_vhost_gpa_to_vva(struct spdk_vhost_dev *vdev, uint64_t addr)
{
	return (void *)rte_vhost_gpa_to_vva(vdev->mem, addr);
}

/*
 * Get available requests from avail ring.
 */
uint16_t
spdk_vhost_vq_avail_ring_get(struct rte_vhost_vring *vq, uint16_t *reqs, uint16_t reqs_len)
{
	struct vring_avail *avail = vq->avail;
	uint16_t size_mask = vq->size - 1;
	uint16_t last_idx = vq->last_avail_idx, avail_idx = avail->idx;
	uint16_t count = RTE_MIN((avail_idx - last_idx) & size_mask, reqs_len);
	uint16_t i;

	if (spdk_likely(count == 0)) {
		return 0;
	}

	vq->last_avail_idx += count;
	for (i = 0; i < count; i++) {
		reqs[i] = vq->avail->ring[(last_idx + i) & size_mask];
	}

	SPDK_TRACELOG(SPDK_TRACE_VHOST_RING,
		      "AVAIL: last_idx=%"PRIu16" avail_idx=%"PRIu16" count=%"PRIu16"\n",
		      last_idx, avail_idx, count);

	return count;
}

bool
spdk_vhost_vq_should_notify(struct spdk_vhost_dev *vdev, struct rte_vhost_vring *vq)
{
	if ((vdev->negotiated_features & (1ULL << VIRTIO_F_NOTIFY_ON_EMPTY)) &&
	    spdk_unlikely(vq->avail->idx == vq->last_avail_idx)) {
		return 1;
	}

	return !(vq->avail->flags & VRING_AVAIL_F_NO_INTERRUPT);
}

struct vring_desc *
spdk_vhost_vq_get_desc(struct rte_vhost_vring *vq, uint16_t req_idx)
{
	assert(req_idx < vq->size);
	return &vq->desc[req_idx];
}

/*
 * Enqueue id and len to used ring.
 */
void
spdk_vhost_vq_used_ring_enqueue(struct spdk_vhost_dev *vdev, struct rte_vhost_vring *vq,
				uint16_t id,
				uint32_t len)
{
	struct vring_used *used = vq->used;
	uint16_t size_mask = vq->size - 1;
	uint16_t last_idx = vq->last_used_idx;

	SPDK_TRACELOG(SPDK_TRACE_VHOST_RING, "USED: last_idx=%"PRIu16" req id=%"PRIu16" len=%"PRIu32"\n",
		      last_idx, id, len);

	vq->last_used_idx++;
	last_idx &= size_mask;

	used->ring[last_idx].id = id;
	used->ring[last_idx].len = len;

	rte_compiler_barrier();

	vq->used->idx = vq->last_used_idx;
	if (spdk_vhost_vq_should_notify(vdev, vq)) {
		eventfd_write(vq->callfd, (eventfd_t)1);
	}
}

bool
spdk_vhost_vring_desc_has_next(struct vring_desc *cur_desc)
{
	return !!(cur_desc->flags & VRING_DESC_F_NEXT);
}

struct vring_desc *
spdk_vhost_vring_desc_get_next(struct vring_desc *vq_desc, struct vring_desc *cur_desc)
{
	assert(spdk_vhost_vring_desc_has_next(cur_desc));
	return &vq_desc[cur_desc->next];
}

bool
spdk_vhost_vring_desc_is_wr(struct vring_desc *cur_desc)
{
	return !!(cur_desc->flags & VRING_DESC_F_WRITE);
}

bool
spdk_vhost_vring_desc_to_iov(struct spdk_vhost_dev *vdev, struct iovec *iov,
			     const struct vring_desc *desc)
{
	iov->iov_base =  spdk_vhost_gpa_to_vva(vdev, desc->addr);
	iov->iov_len = desc->len;
	return !iov->iov_base;
}

struct spdk_vhost_dev *
spdk_vhost_dev_find_by_vid(int vid)
{
	unsigned i;
	struct spdk_vhost_dev *vdev;

	for (i = 0; i < MAX_VHOST_DEVICES; i++) {
		vdev = g_spdk_vhost_devices[i];
		if (vdev && vdev->vid == vid) {
			return vdev;
		}
	}

	return NULL;
}

#define SHIFT_2MB	21
#define SIZE_2MB	(1ULL << SHIFT_2MB)
#define FLOOR_2MB(x)	(((uintptr_t)x) / SIZE_2MB) << SHIFT_2MB
#define CEIL_2MB(x)	((((uintptr_t)x) + SIZE_2MB - 1) / SIZE_2MB) << SHIFT_2MB

void
spdk_vhost_dev_mem_register(struct spdk_vhost_dev *vdev)
{
	struct rte_vhost_mem_region *region;
	uint32_t i;

	for (i = 0; i < vdev->mem->nregions; i++) {
		uint64_t start, end, len;
		region = &vdev->mem->regions[i];
		start = FLOOR_2MB(region->mmap_addr);
		end = CEIL_2MB(region->mmap_addr + region->mmap_size);
		len = end - start;
		SPDK_NOTICELOG("Registering VM memory for vtophys translation - 0x%jx len:0x%jx\n",
			       start, len);
		spdk_mem_register((void *)start, len);
		if (spdk_iommu_mem_register(region->host_user_addr, region->size)) {
			abort();
		}
	}
}

void
spdk_vhost_dev_mem_unregister(struct spdk_vhost_dev *vdev)
{
	struct rte_vhost_mem_region *region;
	uint32_t i;

	for (i = 0; i < vdev->mem->nregions; i++) {
		uint64_t start, end, len;
		region = &vdev->mem->regions[i];
		start = FLOOR_2MB(region->mmap_addr);
		end = CEIL_2MB(region->mmap_addr + region->mmap_size);
		len = end - start;

		if (spdk_iommu_mem_unregister(region->host_user_addr, region->size)) {
			abort();
		}

		spdk_mem_unregister((void *)start, len);
	}
}

static void
spdk_vhost_free_reactor(uint32_t lcore)
{
	g_num_ctrlrs[lcore]--;
}

struct spdk_vhost_dev *
spdk_vhost_dev_find(const char *ctrlr_name)
{
	unsigned i;
	size_t dev_dirname_len = strlen(dev_dirname);

	if (strncmp(ctrlr_name, dev_dirname, dev_dirname_len) == 0) {
		ctrlr_name += dev_dirname_len;
	}

	for (i = 0; i < MAX_VHOST_DEVICES; i++) {
		if (g_spdk_vhost_devices[i] == NULL) {
			continue;
		}

		if (strcmp(g_spdk_vhost_devices[i]->name, ctrlr_name) == 0) {
			return g_spdk_vhost_devices[i];
		}
	}

	return NULL;
}

int
spdk_vhost_dev_construct(struct spdk_vhost_dev *vdev, const char *name, uint64_t cpumask,
			 enum spdk_vhost_dev_type type, const struct spdk_vhost_dev_backend *backend)
{
	unsigned ctrlr_num;
	char path[PATH_MAX];
	struct stat file_stat;

	assert(vdev);

	if (name == NULL) {
		SPDK_ERRLOG("Can't register controller with no name\n");
		return -EINVAL;
	}

	if ((cpumask & spdk_app_get_core_mask()) != cpumask) {
		SPDK_ERRLOG("cpumask 0x%jx not a subset of app mask 0x%jx\n",
			    cpumask, spdk_app_get_core_mask());
		return -EINVAL;
	}

	if (spdk_vhost_dev_find(name)) {
		SPDK_ERRLOG("vhost controller %s already exists.\n", name);
		return -EEXIST;
	}

	for (ctrlr_num = 0; ctrlr_num < MAX_VHOST_DEVICES; ctrlr_num++) {
		if (g_spdk_vhost_devices[ctrlr_num] == NULL) {
			break;
		}
	}

	if (ctrlr_num == MAX_VHOST_DEVICES) {
		SPDK_ERRLOG("Max controllers reached (%d).\n", MAX_VHOST_DEVICES);
		return -ENOSPC;
	}

	if (snprintf(path, sizeof(path), "%s%s", dev_dirname, name) >= (int)sizeof(path)) {
		SPDK_ERRLOG("Resulting socket path for controller %s is too long: %s%s\n", name, dev_dirname,
			    name);
		return -EINVAL;
	}

	/* Register vhost driver to handle vhost messages. */
	if (stat(path, &file_stat) != -1) {
		if (!S_ISSOCK(file_stat.st_mode)) {
			SPDK_ERRLOG("Cannot remove %s: not a socket.\n", path);
			return -EIO;
		} else if (unlink(path) != 0) {
			SPDK_ERRLOG("Cannot remove %s.\n", path);
			return -EIO;
		}
	}

	if (rte_vhost_driver_register(path, 0) != 0) {
		SPDK_ERRLOG("Could not register controller %s with vhost library\n", name);
		SPDK_ERRLOG("Check if domain socket %s already exists\n", path);
		return -EIO;
	}
	if (rte_vhost_driver_set_features(path, backend->virtio_features) ||
	    rte_vhost_driver_disable_features(path, backend->disabled_features)) {
		SPDK_ERRLOG("Couldn't set vhost features for controller %s\n", name);

		rte_vhost_driver_unregister(path);
		return -EIO;
	}

	if (rte_vhost_driver_callback_register(path, &backend->ops) != 0) {
		rte_vhost_driver_unregister(path);
		SPDK_ERRLOG("Couldn't register callbacks for controller %s\n", name);
		return -EIO;
	}

	vdev->name = strdup(name);
	vdev->vid = -1;
	vdev->lcore = -1;
	vdev->cpumask = cpumask;
	vdev->type = type;

	g_spdk_vhost_devices[ctrlr_num] = vdev;

	if (rte_vhost_driver_start(path) != 0) {
		SPDK_ERRLOG("Failed to start vhost driver for controller %s (%d): %s", name, errno,
			    strerror(errno));
		rte_vhost_driver_unregister(path);
		return -EIO;
	}

	SPDK_NOTICELOG("Controller %s: new controller added\n", vdev->name);
	return 0;
}

int
spdk_vhost_dev_remove(struct spdk_vhost_dev *vdev)
{
	unsigned ctrlr_num;
	char path[PATH_MAX];

	if (vdev->lcore != -1) {
		SPDK_ERRLOG("Controller %s is in use and hotplug is not supported\n", vdev->name);
		return -ENODEV;
	}

	for (ctrlr_num = 0; ctrlr_num < MAX_VHOST_DEVICES; ctrlr_num++) {
		if (g_spdk_vhost_devices[ctrlr_num] == vdev) {
			break;
		}
	}

	if (ctrlr_num == MAX_VHOST_DEVICES) {
		SPDK_ERRLOG("Trying to remove invalid controller: %s.\n", vdev->name);
		return -ENOSPC;
	}

	if (snprintf(path, sizeof(path), "%s%s", dev_dirname, vdev->name) >= (int)sizeof(path)) {
		SPDK_ERRLOG("Resulting socket path for controller %s is too long: %s%s\n", vdev->name, dev_dirname,
			    vdev->name);
		return -EINVAL;
	}

	if (rte_vhost_driver_unregister(path) != 0) {
		SPDK_ERRLOG("Could not unregister controller %s with vhost library\n"
			    "Check if domain socket %s still exists\n", vdev->name, path);
		return -EIO;
	}

	SPDK_NOTICELOG("Controller %s: removed\n", vdev->name);

	free(vdev->name);
	g_spdk_vhost_devices[ctrlr_num] = NULL;
	return 0;
}

int
spdk_vhost_parse_core_mask(const char *mask, uint64_t *cpumask)
{
	char *end;

	if (mask == NULL || cpumask == NULL) {
		return -1;
	}

	errno = 0;
	*cpumask = strtoull(mask, &end, 16);

	if (*end != '\0' || errno || !*cpumask ||
	    ((*cpumask & spdk_app_get_core_mask()) != *cpumask)) {

		SPDK_ERRLOG("cpumask %s not a subset of app mask 0x%jx\n",
			    mask, spdk_app_get_core_mask());
		return -1;
	}

	return 0;
}

struct spdk_vhost_dev *
spdk_vhost_dev_next(struct spdk_vhost_dev *prev)
{
	int i = 0;

	if (prev != NULL) {
		for (; i < MAX_VHOST_DEVICES; i++) {
			if (g_spdk_vhost_devices[i] == prev) {
				break;
			}
		}

		i++;
	}

	for (; i < MAX_VHOST_DEVICES; i++) {
		if (g_spdk_vhost_devices[i] == NULL) {
			continue;
		}

		return g_spdk_vhost_devices[i];
	}

	return NULL;
}

const char *
spdk_vhost_dev_get_name(struct spdk_vhost_dev *vdev)
{
	assert(vdev != NULL);
	return vdev->name;
}

uint64_t
spdk_vhost_dev_get_cpumask(struct spdk_vhost_dev *vdev)
{
	assert(vdev != NULL);
	return vdev->cpumask;
}

static uint32_t
spdk_vhost_allocate_reactor(uint64_t cpumask)
{
	uint32_t i, selected_core;
	uint32_t min_ctrlrs;

	cpumask &= spdk_app_get_core_mask();

	if (cpumask == 0) {
		return 0;
	}

	min_ctrlrs = INT_MAX;
	selected_core = 0;

	for (i = 0; i < RTE_MAX_LCORE && i < 64; i++) {
		if (!((1ULL << i) & cpumask)) {
			continue;
		}

		if (g_num_ctrlrs[i] < min_ctrlrs) {
			selected_core = i;
			min_ctrlrs = g_num_ctrlrs[i];
		}
	}

	g_num_ctrlrs[selected_core]++;
	return selected_core;
}

void
spdk_vhost_dev_unload(struct spdk_vhost_dev *vdev)
{
	struct rte_vhost_vring *q;
	uint16_t i;

	for (i = 0; i < vdev->num_queues; i++) {
		q = &vdev->virtqueue[i];
		rte_vhost_set_vhost_vring_last_idx(vdev->vid, i, q->last_avail_idx, q->last_used_idx);
	}

	free(vdev->mem);

	spdk_vhost_free_reactor(vdev->lcore);
	vdev->lcore = -1;
}

struct spdk_vhost_dev *
spdk_vhost_dev_load(int vid)
{
	struct spdk_vhost_dev *vdev;
	char ifname[PATH_MAX];

	uint16_t num_queues = rte_vhost_get_vring_num(vid);
	uint16_t i;

	if (rte_vhost_get_ifname(vid, ifname, PATH_MAX) < 0) {
		SPDK_ERRLOG("Couldn't get a valid ifname for device %d\n", vid);
		return NULL;
	}

	vdev = spdk_vhost_dev_find(ifname);
	if (vdev == NULL) {
		SPDK_ERRLOG("Controller %s not found.\n", ifname);
		return NULL;
	}

	if (vdev->lcore != -1) {
		SPDK_ERRLOG("Controller %s already connected.\n", ifname);
		return NULL;
	}

	if (num_queues > MAX_VHOST_VRINGS) {
		SPDK_ERRLOG("vhost device %d: Too many queues (%"PRIu16"). Max %"PRIu16"\n", vid, num_queues,
			    MAX_VHOST_VRINGS);
		return NULL;
	}

	for (i = 0; i < num_queues; i++) {
		if (rte_vhost_get_vhost_vring(vid, i, &vdev->virtqueue[i])) {
			SPDK_ERRLOG("vhost device %d: Failed to get information of queue %"PRIu16"\n", vid, i);
			return NULL;
		}

		/* Disable notifications. */
		if (rte_vhost_enable_guest_notification(vid, i, 0) != 0) {
			SPDK_ERRLOG("vhost device %d: Failed to disable guest notification on queue %"PRIu16"\n", vid, i);
			return NULL;
		}

	}

	vdev->vid = vid;
	vdev->num_queues = num_queues;

	if (rte_vhost_get_negotiated_features(vid, &vdev->negotiated_features) != 0) {
		SPDK_ERRLOG("vhost device %d: Failed to get negotiated driver features\n", vid);
		return NULL;
	}

	if (rte_vhost_get_mem_table(vid, &vdev->mem) != 0) {
		SPDK_ERRLOG("vhost device %d: Failed to get guest memory table\n", vid);
		return NULL;
	}

	vdev->lcore = spdk_vhost_allocate_reactor(vdev->cpumask);

	return vdev;
}

void
spdk_vhost_startup(void *arg1, void *arg2)
{
	int ret;
	const char *basename = arg1;

	if (basename && strlen(basename) > 0) {
		ret = snprintf(dev_dirname, sizeof(dev_dirname) - 2, "%s", basename);
		if ((size_t)ret >= sizeof(dev_dirname) - 2) {
			SPDK_ERRLOG("Char dev dir path length %d is too long\n", ret);
			abort();
		}

		if (dev_dirname[ret - 1] != '/') {
			dev_dirname[ret] = '/';
			dev_dirname[ret + 1]  = '\0';
		}
	}

	ret = spdk_vhost_scsi_controller_construct();
	if (ret != 0) {
		SPDK_ERRLOG("Cannot construct vhost controllers\n");
		abort();
	}

	ret = spdk_vhost_blk_controller_construct();
	if (ret != 0) {
		SPDK_ERRLOG("Cannot construct vhost block controllers\n");
		abort();
	}
}

static void *
session_shutdown(void *arg)
{
	struct spdk_vhost_dev *vdev = NULL;
	int i;

	for (i = 0; i < MAX_VHOST_DEVICES; i++) {
		vdev = g_spdk_vhost_devices[i];
		if (vdev == NULL) {
			continue;
		}
		rte_vhost_driver_unregister(vdev->name);
	}

	SPDK_NOTICELOG("Exiting\n");
	spdk_app_stop(0);
	return NULL;
}

/*
 * When we receive a INT signal. Execute shutdown in separate thread to avoid deadlock.
 */
void
spdk_vhost_shutdown_cb(void)
{
	pthread_t tid;
	if (pthread_create(&tid, NULL, &session_shutdown, NULL) < 0) {
		SPDK_ERRLOG("Failed to start session shutdown thread (%d): %s", errno, strerror(errno));
		abort();
	}
	pthread_detach(tid);
}

static void
vhost_timed_event_fn(void *arg1, void *arg2)
{
	struct spdk_vhost_timed_event *ev = arg1;

	if (ev->cb_fn) {
		ev->cb_fn(arg2);
	}

	sem_post(&ev->sem);
}

static void
vhost_timed_event_init(struct spdk_vhost_timed_event *ev, int32_t lcore,
		       spdk_vhost_timed_event_fn cb_fn, void *arg, unsigned timeout_sec)
{
	/* No way to free spdk event so don't allow to use it again without calling, waiting. */
	assert(ev->spdk_event == NULL);

	if (sem_init(&ev->sem, 0, 0) < 0)
		SPDK_ERRLOG("Failed to initialize semaphore for vhost timed event\n");

	ev->cb_fn = cb_fn;
	clock_gettime(CLOCK_REALTIME, &ev->timeout);
	ev->timeout.tv_sec += timeout_sec;
	ev->spdk_event = spdk_event_allocate(lcore, vhost_timed_event_fn, ev, arg);
}

void
spdk_vhost_timed_event_init(struct spdk_vhost_timed_event *ev, int32_t lcore,
			    spdk_vhost_timed_event_fn cb_fn, void *arg, unsigned timeout_sec)
{
	vhost_timed_event_init(ev, lcore, cb_fn, arg, timeout_sec);
}

void
spdk_vhost_timed_event_send(int32_t lcore, spdk_vhost_timed_event_fn cb_fn, void *arg,
			    unsigned timeout_sec, const char *errmsg)
{
	struct spdk_vhost_timed_event ev = {0};

	vhost_timed_event_init(&ev, lcore, cb_fn, arg, timeout_sec);
	spdk_event_call(ev.spdk_event);
	spdk_vhost_timed_event_wait(&ev, errmsg);
}

void
spdk_vhost_timed_event_wait(struct spdk_vhost_timed_event *ev, const char *errmsg)
{
	int rc;

	assert(ev->spdk_event != NULL);

	rc = sem_timedwait(&ev->sem, &ev->timeout);
	if (rc != 0) {
		SPDK_ERRLOG("Timout waiting for event: %s.\n", errmsg);
		abort();
	}

	ev->spdk_event = NULL;
	sem_destroy(&ev->sem);
}

SPDK_LOG_REGISTER_TRACE_FLAG("vhost_ring", SPDK_TRACE_VHOST_RING)
