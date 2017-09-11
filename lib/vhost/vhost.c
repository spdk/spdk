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
#include "spdk/barrier.h"

#include "spdk/vhost.h"
#include "vhost_internal.h"

static uint32_t g_num_ctrlrs[RTE_MAX_LCORE];

/* Path to folder where character device will be created. Can be set by user. */
static char dev_dirname[PATH_MAX] = "";

#define MAX_VHOST_DEVICES	64

struct spdk_vhost_dev_event_ctx {
	/** Pointer to the controller obtained before enqueuing the event */
	struct spdk_vhost_dev *vdev;

	/** Index of the ctrlr to send event to. */
	unsigned vdev_id;

	/** User callback function to be executed on given lcore. */
	spdk_vhost_event_fn cb_fn;

	/** Semaphore used to signal that event is done. */
	sem_t sem;

	/** Response to be written by enqueued event. */
	int response;
};

static int new_connection(int vid);
static int start_device(int vid);
static void stop_device(int vid);
static void destroy_connection(int vid);

const struct vhost_device_ops g_spdk_vhost_ops = {
	.new_device =  start_device,
	.destroy_device = stop_device,
	.new_connection = new_connection,
	.destroy_connection = destroy_connection,
};

static struct spdk_vhost_dev *g_spdk_vhost_devices[MAX_VHOST_DEVICES];
static pthread_mutex_t g_spdk_vhost_mutex = PTHREAD_MUTEX_INITIALIZER;

void *spdk_vhost_gpa_to_vva(struct spdk_vhost_dev *vdev, uint64_t addr)
{
	return (void *)rte_vhost_gpa_to_vva(vdev->mem, addr);
}

/*
 * Get available requests from avail ring.
 */
uint16_t
spdk_vhost_vq_avail_ring_get(struct spdk_vhost_virtqueue *virtqueue, uint16_t *reqs,
			     uint16_t reqs_len)
{
	struct rte_vhost_vring *vring = &virtqueue->vring;
	struct vring_avail *avail = vring->avail;
	uint16_t size_mask = vring->size - 1;
	uint16_t last_idx = vring->last_avail_idx, avail_idx = avail->idx;
	uint16_t count = RTE_MIN((avail_idx - last_idx) & size_mask, reqs_len);
	uint16_t i;

	if (spdk_likely(count == 0)) {
		return 0;
	}

	vring->last_avail_idx += count;
	for (i = 0; i < count; i++) {
		reqs[i] = vring->avail->ring[(last_idx + i) & size_mask];
	}

	SPDK_DEBUGLOG(SPDK_TRACE_VHOST_RING,
		      "AVAIL: last_idx=%"PRIu16" avail_idx=%"PRIu16" count=%"PRIu16"\n",
		      last_idx, avail_idx, count);

	return count;
}

static bool
spdk_vhost_vring_desc_is_indirect(struct vring_desc *cur_desc)
{
	return !!(cur_desc->flags & VRING_DESC_F_INDIRECT);
}

int
spdk_vhost_vq_get_desc(struct spdk_vhost_dev *vdev, struct spdk_vhost_virtqueue *virtqueue,
		       uint16_t req_idx, struct vring_desc **desc, struct vring_desc **desc_table,
		       uint32_t *desc_table_size)
{
	if (spdk_unlikely(req_idx >= virtqueue->vring.size)) {
		return -1;
	}

	*desc = &virtqueue->vring.desc[req_idx];

	if (spdk_vhost_vring_desc_is_indirect(*desc)) {
		assert(spdk_vhost_dev_has_feature(vdev, VIRTIO_RING_F_INDIRECT_DESC));
		*desc_table = spdk_vhost_gpa_to_vva(vdev, (*desc)->addr);
		*desc_table_size = (*desc)->len / sizeof(**desc);
		*desc = *desc_table;
		return 0;
	}

	*desc_table = virtqueue->vring.desc;
	*desc_table_size = virtqueue->vring.size;

	return 0;
}

int
spdk_vhost_vq_used_signal(struct spdk_vhost_dev *vdev, struct spdk_vhost_virtqueue *virtqueue)
{
	if (virtqueue->used_enqueued == false) {
		return 0;
	}

	SPDK_DEBUGLOG(SPDK_TRACE_VHOST_RING,
		      "Queue %td - USED RING: sending IRQ: last used %"PRIu16"\n",
		      virtqueue - vdev->virtqueue, virtqueue->vring.last_used_idx);

	eventfd_write(virtqueue->vring.callfd, (eventfd_t)1);

	virtqueue->used_enqueued = 0;
	return 1;
}


static void
check_dev_io_stats(struct spdk_vhost_dev *vdev, uint64_t now)
{
	struct spdk_vhost_virtqueue *virtqueue;
	uint64_t io_treshold = vdev->coalescing_io_threshold;
	uint16_t q_idx;

	if (vdev->event_coalescing_time == 0 || now < vdev->next_event_coalescing_check_time) {
		return;
	}

	vdev->next_event_coalescing_check_time = now + vdev->check_io_stats_interval;
	for (q_idx = 0; q_idx < vdev->num_queues; q_idx++) {
		virtqueue = &vdev->virtqueue[q_idx];

//		if (virtqueue->coalescing_enabled != (virtqueue->io_cnt > io_treshold)) {
//			SPDK_ERRLOG("%s coleasting for queue %"PRIu16"\n", (virtqueue->io_cnt > io_treshold) ? "Enabling" : "Disabling", q_idx);
//		}

		virtqueue->coalescing_enabled = virtqueue->io_cnt > io_treshold;
		virtqueue->io_cnt = 0;
		virtqueue->next_event_time = now;
	}
}

void
spdk_vhost_dev_used_signal(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_virtqueue *virtqueue;
	uint64_t now;
	uint16_t q_idx;

	now = spdk_get_ticks();
	check_dev_io_stats(vdev, now);

	for (q_idx = 0; q_idx < vdev->num_queues; q_idx++) {
		virtqueue = &vdev->virtqueue[q_idx];
		if (virtqueue->coalescing_enabled == false) {
			spdk_vhost_vq_used_signal(vdev, virtqueue);
			continue;
		}

		/* No need for event right now */
		if (now < virtqueue->next_event_time) {
			continue;
		}

		if (spdk_vhost_vq_used_signal(vdev, virtqueue)) {
			/* Syscall is quite long so refresh the time now */
			now = spdk_get_ticks();
		}

		virtqueue->next_event_time = now + vdev->event_coalescing_time;
	}
}

/*
 * Enqueue id and len to used ring.
 */
void
spdk_vhost_vq_used_ring_enqueue(struct spdk_vhost_dev *vdev, struct spdk_vhost_virtqueue *virtqueue,
				uint16_t id, uint32_t len)
{
	struct rte_vhost_vring *vring = &virtqueue->vring;
	struct vring_used *used = vring->used;
	uint16_t last_idx = vring->last_used_idx & (vring->size - 1);

	SPDK_DEBUGLOG(SPDK_TRACE_VHOST_RING,
		      "Queue %td - USED RING: last_idx=%"PRIu16" req id=%"PRIu16" len=%"PRIu32"\n",
		      virtqueue - vdev->virtqueue, vring->last_used_idx, id, len);

	vring->last_used_idx++;
	used->ring[last_idx].id = id;
	used->ring[last_idx].len = len;

	spdk_wmb();
	* (volatile uint16_t *) &used->idx = vring->last_used_idx;

	virtqueue->io_cnt++;
	virtqueue->used_enqueued = true;

	/* We need to signal every last_used_idx overflow. */
	if (vring->last_used_idx == 0 ||
	    (spdk_vhost_dev_has_feature(vdev, VIRTIO_F_NOTIFY_ON_EMPTY) &&
	     spdk_unlikely(vring->avail->idx == vring->last_avail_idx))) {
		spdk_vhost_vq_used_signal(vdev, virtqueue);
	}
}

int
spdk_vhost_vring_desc_get_next(struct vring_desc **desc,
			       struct vring_desc *desc_table, uint32_t desc_table_size)
{
	struct vring_desc *old_desc = *desc;
	uint16_t next_idx;

	if ((old_desc->flags & VRING_DESC_F_NEXT) == 0) {
		*desc = NULL;
		return 0;
	}

	next_idx = old_desc->next;
	if (spdk_unlikely(next_idx >= desc_table_size)) {
		*desc = NULL;
		return -1;
	}

	*desc = &desc_table[next_idx];
	return 0;
}

bool
spdk_vhost_vring_desc_is_wr(struct vring_desc *cur_desc)
{
	return !!(cur_desc->flags & VRING_DESC_F_WRITE);
}

#define _2MB_OFFSET(ptr)	((ptr) & (0x200000 - 1))

int
spdk_vhost_vring_desc_to_iov(struct spdk_vhost_dev *vdev, struct iovec *iov,
			     uint16_t *iov_index, const struct vring_desc *desc)
{
	uint32_t remaining = desc->len;
	uint32_t len;
	uintptr_t payload = desc->addr;
	uintptr_t vva;

	while (remaining) {
		if (*iov_index >= SPDK_VHOST_IOVS_MAX) {
			SPDK_ERRLOG("SPDK_VHOST_IOVS_MAX(%d) reached\n", SPDK_VHOST_IOVS_MAX);
			return -1;
		}
		vva = (uintptr_t)spdk_vhost_gpa_to_vva(vdev, payload);
		if (vva == 0) {
			SPDK_ERRLOG("gpa_to_vva(%p) == NULL\n", (void *)payload);
			return -1;
		}
		len = spdk_min(remaining, 0x200000 - _2MB_OFFSET(payload));
		iov[*iov_index].iov_base = (void *)vva;
		iov[*iov_index].iov_len = len;
		remaining -= len;
		payload += len;
		(*iov_index)++;
	}

	return 0;
}

bool
spdk_vhost_dev_has_feature(struct spdk_vhost_dev *vdev, unsigned feature_id)
{
	return vdev->negotiated_features & (1ULL << feature_id);
}

static struct spdk_vhost_dev *
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

		if (spdk_mem_register((void *)start, len) != 0) {
			SPDK_WARNLOG("Failed to register memory region %"PRIu32". Future vtophys translation might fail.\n",
				     i);
			continue;
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

		if (spdk_vtophys((void *) start) == SPDK_VTOPHYS_ERROR) {
			continue; /* region has not been registered */
		}

		if (spdk_mem_unregister((void *)start, len) != 0) {
			assert(false);
		}
	}
}

static void
spdk_vhost_free_reactor(uint32_t lcore)
{
	g_num_ctrlrs[lcore]--;
}

static int
spdk_vhost_dev_find_id(const char *ctrlr_name)
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
			return i;
		}
	}

	return -1;
}

struct spdk_vhost_dev *
spdk_vhost_dev_find(const char *ctrlr_name)
{
	int id;

	id = spdk_vhost_dev_find_id(ctrlr_name);
	if (id == -1) {
		return NULL;
	}

	return g_spdk_vhost_devices[id];
}

static int
spdk_vhost_parse_core_mask(const char *mask, uint64_t *cpumask)
{
	char *end;

	if (mask == NULL) {
		*cpumask = spdk_app_get_core_mask();
		return 0;
	}

	errno = 0;
	*cpumask = strtoull(mask, &end, 16);

	if (*end != '\0' || errno || !*cpumask ||
	    ((*cpumask & spdk_app_get_core_mask()) != *cpumask)) {
		return -1;
	}

	return 0;
}

int
spdk_vhost_dev_construct(struct spdk_vhost_dev *vdev, const char *name, const char *mask_str,
			 enum spdk_vhost_dev_type type, const struct spdk_vhost_dev_backend *backend)
{
	unsigned ctrlr_num;
	char path[PATH_MAX];
	struct stat file_stat;
	char buf[64];
	uint64_t cpumask;

	assert(vdev);

	if (name == NULL) {
		SPDK_ERRLOG("Can't register controller with no name\n");
		return -EINVAL;
	}

	if (spdk_vhost_parse_core_mask(mask_str, &cpumask) != 0) {
		SPDK_ERRLOG("cpumask %s not a subset of app mask 0x%jx\n",
			    mask_str, spdk_app_get_core_mask());
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

	if (rte_vhost_driver_callback_register(path, &g_spdk_vhost_ops) != 0) {
		rte_vhost_driver_unregister(path);
		SPDK_ERRLOG("Couldn't register callbacks for controller %s\n", name);
		return -EIO;
	}

	vdev->name = strdup(name);
	vdev->path = strdup(path);
	vdev->vid = -1;
	vdev->lcore = -1;
	vdev->cpumask = cpumask;
	vdev->type = type;
	vdev->backend = backend;
	vdev->event_coalescing_time = SPDK_VHOST_DEFFAULT_IRQ_INTERVAL_US * spdk_get_ticks_hz() /
				      1000000ULL;
	vdev->next_event_coalescing_check_time = 0;
	vdev->check_io_stats_interval = SPDK_VHOST_DEV_STATS_CHECK_INTERVAL_MS * spdk_get_ticks_hz() /
					1000UL;
	vdev->coalescing_io_threshold = SPDK_VHOST_VQ_COALESCING_THRESHOLD;

	g_spdk_vhost_devices[ctrlr_num] = vdev;

	if (rte_vhost_driver_start(path) != 0) {
		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_ERRLOG("Failed to start vhost driver for controller %s (%d): %s\n", name, errno,
			    buf);
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

	if (vdev->vid != -1) {
		SPDK_ERRLOG("Controller %s has still valid connection.\n", vdev->name);
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
	free(vdev->path);
	g_spdk_vhost_devices[ctrlr_num] = NULL;
	return 0;
}

static int
spdk_vhost_dev_next(int i)
{
	for (i++; i < MAX_VHOST_DEVICES; i++) {
		if (g_spdk_vhost_devices[i] == NULL) {
			continue;
		}

		return i;
	}

	return -1;
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
spdk_vhost_dev_backend_event_done(void *event_ctx, int response)
{
	struct spdk_vhost_dev_event_ctx *ctx = event_ctx;

	ctx->response = response;
	sem_post(&ctx->sem);
}

static void
spdk_vhost_event_cb(void *arg1, void *arg2)
{
	struct spdk_vhost_dev_event_ctx *ctx = arg1;

	ctx->cb_fn(ctx->vdev, ctx);
}

static void
spdk_vhost_event_async_fn(void *arg1, void *arg2)
{
	struct spdk_vhost_dev_event_ctx *ctx = arg1;
	struct spdk_vhost_dev *vdev;
	struct spdk_event *ev;

	if (pthread_mutex_trylock(&g_spdk_vhost_mutex) != 0) {
		ev = spdk_event_allocate(spdk_env_get_current_core(), spdk_vhost_event_async_fn, arg1, arg2);
		spdk_event_call(ev);
		return;
	}

	vdev = g_spdk_vhost_devices[ctx->vdev_id];
	if (vdev != ctx->vdev) {
		/* vdev has been changed after enqueuing this event */
		vdev = NULL;
	}

	ctx->cb_fn(vdev, arg2);
	pthread_mutex_unlock(&g_spdk_vhost_mutex);

	free(ctx);
}

static void spdk_vhost_external_event_foreach_continue(int vdev_id,
		spdk_vhost_event_fn fn, void *arg);

static void
spdk_vhost_event_async_foreach_fn(void *arg1, void *arg2)
{
	struct spdk_vhost_dev_event_ctx *ctx = arg1;
	struct spdk_vhost_dev *vdev;
	struct spdk_event *ev;

	if (pthread_mutex_trylock(&g_spdk_vhost_mutex) != 0) {
		ev = spdk_event_allocate(spdk_env_get_current_core(),
					 spdk_vhost_event_async_foreach_fn, arg1, arg2);
		spdk_event_call(ev);
		return;
	}

	vdev = g_spdk_vhost_devices[ctx->vdev_id];
	if (vdev == ctx->vdev) {
		ctx->cb_fn(vdev, arg2);
	}

	spdk_vhost_external_event_foreach_continue(ctx->vdev_id, ctx->cb_fn, arg2);
	pthread_mutex_unlock(&g_spdk_vhost_mutex);

	free(ctx);
}

static int
spdk_vhost_event_send(struct spdk_vhost_dev *vdev, spdk_vhost_event_fn cb_fn,
		      unsigned timeout_sec, const char *errmsg)
{
	struct spdk_vhost_dev_event_ctx ev_ctx = {0};
	struct spdk_event *ev;
	struct timespec timeout;
	int rc;

	rc = sem_init(&ev_ctx.sem, 0, 0);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to initialize semaphore for vhost timed event\n");
		return -errno;
	}

	ev_ctx.vdev = vdev;
	ev_ctx.cb_fn = cb_fn;

	ev = spdk_event_allocate(vdev->lcore, spdk_vhost_event_cb, &ev_ctx, NULL);
	assert(ev);
	spdk_event_call(ev);

	clock_gettime(CLOCK_REALTIME, &timeout);
	timeout.tv_sec += timeout_sec;

	rc = sem_timedwait(&ev_ctx.sem, &timeout);
	if (rc != 0) {
		SPDK_ERRLOG("Timeout waiting for event: %s.\n", errmsg);
		sem_wait(&ev_ctx.sem);
	}

	sem_destroy(&ev_ctx.sem);
	return ev_ctx.response;
}

static int
spdk_vhost_event_async_send(unsigned vdev_id, spdk_vhost_event_fn cb_fn, void *arg,
			    bool foreach)
{
	struct spdk_vhost_dev_event_ctx *ev_ctx;
	struct spdk_event *ev;
	spdk_event_fn fn;

	ev_ctx = calloc(1, sizeof(*ev_ctx));
	if (ev_ctx == NULL) {
		SPDK_ERRLOG("Failed to alloc vhost event.\n");
		return -ENOMEM;
	}

	ev_ctx->vdev = g_spdk_vhost_devices[vdev_id];
	ev_ctx->vdev_id = vdev_id;
	ev_ctx->cb_fn = cb_fn;

	fn = foreach ? spdk_vhost_event_async_foreach_fn : spdk_vhost_event_async_fn;
	ev = spdk_event_allocate(ev_ctx->vdev->lcore, fn, ev_ctx, arg);
	assert(ev);
	spdk_event_call(ev);

	return 0;
}

static void
stop_device(int vid)
{
	struct spdk_vhost_dev *vdev;
	struct rte_vhost_vring *q;
	int rc;
	uint16_t i;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vdev = spdk_vhost_dev_find_by_vid(vid);
	if (vdev == NULL) {
		SPDK_ERRLOG("Couldn't find device with vid %d to stop.\n", vid);
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		return;
	}

	if (vdev->lcore == -1) {
		SPDK_ERRLOG("Controller %s is not loaded.\n", vdev->name);
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		return;
	}

	rc = spdk_vhost_event_send(vdev, vdev->backend->stop_device, 3, "stop device");
	if (rc != 0) {
		SPDK_ERRLOG("Couldn't stop device with vid %d.\n", vid);
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		return;
	}

	for (i = 0; i < vdev->num_queues; i++) {
		q = &vdev->virtqueue[i].vring;
		rte_vhost_set_vhost_vring_last_idx(vdev->vid, i, q->last_avail_idx, q->last_used_idx);
	}

	free(vdev->mem);
	spdk_vhost_free_reactor(vdev->lcore);
	vdev->lcore = -1;
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
}

static int
start_device(int vid)
{
	struct spdk_vhost_dev *vdev;
	int rc = -1;
	uint16_t num_queues;
	uint16_t i;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	num_queues = rte_vhost_get_vring_num(vid);

	vdev = spdk_vhost_dev_find_by_vid(vid);
	if (vdev == NULL) {
		SPDK_ERRLOG("Controller with vid %d doesn't exist.\n", vid);
		goto out;
	}

	if (vdev->lcore != -1) {
		SPDK_ERRLOG("Controller %s already loaded.\n", vdev->name);
		goto out;
	}

	if (num_queues > SPDK_VHOST_MAX_VQUEUES) {
		SPDK_ERRLOG("vhost device %d: Too many queues (%"PRIu16"). Max %"PRIu16"\n", vid, num_queues,
			    SPDK_VHOST_MAX_VQUEUES);
		goto out;
	}

	memset(vdev->virtqueue, 0, sizeof(vdev->virtqueue));
	for (i = 0; i < num_queues; i++) {
		if (rte_vhost_get_vhost_vring(vid, i, &vdev->virtqueue[i].vring)) {
			SPDK_ERRLOG("vhost device %d: Failed to get information of queue %"PRIu16"\n", vid, i);
			goto out;
		}

		/* Disable notifications. */
		if (rte_vhost_enable_guest_notification(vid, i, 0) != 0) {
			SPDK_ERRLOG("vhost device %d: Failed to disable guest notification on queue %"PRIu16"\n", vid, i);
			goto out;
		}

	}

	vdev->num_queues = num_queues;

	if (rte_vhost_get_negotiated_features(vid, &vdev->negotiated_features) != 0) {
		SPDK_ERRLOG("vhost device %d: Failed to get negotiated driver features\n", vid);
		goto out;
	}

	if (rte_vhost_get_mem_table(vid, &vdev->mem) != 0) {
		SPDK_ERRLOG("vhost device %d: Failed to get guest memory table\n", vid);
		goto out;
	}

	vdev->lcore = spdk_vhost_allocate_reactor(vdev->cpumask);
	rc = spdk_vhost_event_send(vdev, vdev->backend->start_device, 3, "start device");
	if (rc != 0) {
		free(vdev->mem);
		spdk_vhost_free_reactor(vdev->lcore);
		vdev->lcore = -1;
	}

out:
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	return rc;
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

static void
session_app_stop(void *arg1, void *arg2)
{
	spdk_app_stop(0);
}

static void *
session_shutdown(void *arg)
{
	struct spdk_vhost_dev *vdev = NULL;
	char path[PATH_MAX];
	int i;

	for (i = 0; i < MAX_VHOST_DEVICES; i++) {
		vdev = g_spdk_vhost_devices[i];
		if (vdev == NULL) {
			continue;
		}

		if (snprintf(path, sizeof(path), "%s%s", dev_dirname, vdev->name) >= (int)sizeof(path)) {
			SPDK_ERRLOG("Resulting socket path for controller %s is too long: %s%s\n", vdev->name, dev_dirname,
				    vdev->name);
			assert(false);
			continue;
		}

		rte_vhost_driver_unregister(path);
	}

	SPDK_NOTICELOG("Exiting\n");
	spdk_event_call((struct spdk_event *)arg);
	return NULL;
}

/*
 * When we receive a INT signal. Execute shutdown in separate thread to avoid deadlock.
 */
void
spdk_vhost_shutdown_cb(void)
{
	pthread_t tid;
	char buf[64];
	struct spdk_event *vhost_app_stop;

	vhost_app_stop = spdk_event_allocate(spdk_env_get_current_core(), session_app_stop, NULL, NULL);

	if (pthread_create(&tid, NULL, &session_shutdown, vhost_app_stop) < 0) {
		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_ERRLOG("Failed to start session shutdown thread (%d): %s\n", errno, buf);
		abort();
	}
	pthread_detach(tid);
}

void
spdk_vhost_dump_config_json(struct spdk_vhost_dev *vdev,
			    struct spdk_json_write_ctx *w)
{
	assert(vdev->backend->dump_config_json != NULL);
	vdev->backend->dump_config_json(vdev, w);
}

int
spdk_remove_vhost_controller(struct spdk_vhost_dev *vdev)
{
	return vdev->backend->vhost_remove_controller(vdev);
}

static int
new_connection(int vid)
{
	struct spdk_vhost_dev *vdev;
	char ifname[PATH_MAX];

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	if (rte_vhost_get_ifname(vid, ifname, PATH_MAX) < 0) {
		SPDK_ERRLOG("Couldn't get a valid ifname for device with vid %d\n", vid);
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		return -1;
	}

	vdev = spdk_vhost_dev_find(ifname);
	if (vdev == NULL) {
		SPDK_ERRLOG("Couldn't find device with vid %d to create connection for.\n", vid);
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		return -1;
	}

	/* since pollers are not running it safe not to use spdk_event here */
	if (vdev->vid != -1) {
		SPDK_ERRLOG("Device with vid %d is already connected.\n", vid);
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		return -1;
	}

	vdev->vid = vid;
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	return 0;
}

static void
destroy_connection(int vid)
{
	struct spdk_vhost_dev *vdev;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vdev = spdk_vhost_dev_find_by_vid(vid);
	if (vdev == NULL) {
		SPDK_ERRLOG("Couldn't find device with vid %d to destroy connection for.\n", vid);
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		return;
	}

	/* since pollers are not running it safe not to use spdk_event here */
	vdev->vid = -1;
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
}

void
spdk_vhost_call_external_event(const char *ctrlr_name, spdk_vhost_event_fn fn, void *arg)
{
	struct spdk_vhost_dev *vdev;
	int vdev_id;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vdev_id = spdk_vhost_dev_find_id(ctrlr_name);

	if (vdev_id == -1) {
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		fn(NULL, arg);
		return;
	}

	vdev = g_spdk_vhost_devices[vdev_id];
	if (vdev->lcore == -1) {
		fn(vdev, arg);
	} else {
		spdk_vhost_event_async_send(vdev_id, fn, arg, false);
	}

	pthread_mutex_unlock(&g_spdk_vhost_mutex);
}

static void
spdk_vhost_external_event_foreach_continue(int vdev_id, spdk_vhost_event_fn fn, void *arg)
{
	struct spdk_vhost_dev *vdev;

	vdev_id = spdk_vhost_dev_next(vdev_id);
	if (vdev_id == -1) {
		fn(NULL, arg);
		return;
	}

	vdev = g_spdk_vhost_devices[vdev_id];
	while (vdev->lcore == -1) {
		fn(vdev, arg);
		vdev_id = spdk_vhost_dev_next(vdev_id);
		if (vdev_id == -1) {
			fn(NULL, arg);
			return;
		}
		vdev = g_spdk_vhost_devices[vdev_id];
	}

	spdk_vhost_event_async_send(vdev_id, fn, arg, true);
}

void
spdk_vhost_call_external_event_foreach(spdk_vhost_event_fn fn, void *arg)
{
	pthread_mutex_lock(&g_spdk_vhost_mutex);
	spdk_vhost_external_event_foreach_continue(-1, fn, arg);
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
}

void
spdk_vhost_lock(void)
{
	pthread_mutex_lock(&g_spdk_vhost_mutex);
}

void
spdk_vhost_unlock(void)
{
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
}

SPDK_LOG_REGISTER_TRACE_FLAG("vhost_ring", SPDK_TRACE_VHOST_RING)
