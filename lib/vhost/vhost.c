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

static uint32_t *g_num_ctrlrs;

/* Path to folder where character device will be created. Can be set by user. */
static char dev_dirname[PATH_MAX] = "";

struct spdk_vhost_dev_event_ctx {
	/** Pointer to the target obtained before enqueuing the event */
	struct spdk_vhost_tgt *vtgt;

	/** ID of the vtgt to send event to. */
	unsigned vtgt_id;

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
static int get_config(int vid, uint8_t *config, uint32_t len);
static int set_config(int vid, uint8_t *config, uint32_t offset,
		      uint32_t size, uint32_t flags);

const struct vhost_device_ops g_spdk_vhost_ops = {
	.new_device =  start_device,
	.destroy_device = stop_device,
	.get_config = get_config,
	.set_config = set_config,
	.new_connection = new_connection,
	.destroy_connection = destroy_connection,
	.vhost_nvme_admin_passthrough = spdk_vhost_nvme_admin_passthrough,
	.vhost_nvme_set_cq_call = spdk_vhost_nvme_set_cq_call,
	.vhost_nvme_get_cap = spdk_vhost_nvme_get_cap,
};

static TAILQ_HEAD(, spdk_vhost_tgt) g_spdk_vhost_tgts = TAILQ_HEAD_INITIALIZER(
			g_spdk_vhost_tgts);
static pthread_mutex_t g_spdk_vhost_mutex = PTHREAD_MUTEX_INITIALIZER;

void *spdk_vhost_gpa_to_vva(struct spdk_vhost_dev *vdev, uint64_t addr)
{
	return (void *)rte_vhost_gpa_to_vva(vdev->mem, addr);
}

static void
spdk_vhost_log_req_desc(struct spdk_vhost_dev *vdev, struct spdk_vhost_virtqueue *virtqueue,
			uint16_t req_id)
{
	struct vring_desc *desc, *desc_table;
	uint32_t desc_table_size;
	int rc;

	if (spdk_likely(!spdk_vhost_dev_has_feature(vdev, VHOST_F_LOG_ALL))) {
		return;
	}

	rc = spdk_vhost_vq_get_desc(vdev, virtqueue, req_id, &desc, &desc_table, &desc_table_size);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Can't log used ring descriptors!\n");
		return;
	}

	do {
		if (spdk_vhost_vring_desc_is_wr(desc)) {
			/* To be honest, only pages realy touched should be logged, but
			 * doing so would require tracking those changes in each backed.
			 * Also backend most likely will touch all/most of those pages so
			 * for lets assume we touched all pages passed to as writeable buffers. */

			rte_vhost_log_write(vdev->vtgt->vid, desc->addr, desc->len);
		}
		spdk_vhost_vring_desc_get_next(&desc, desc_table, desc_table_size);
	} while (desc);
}

static void
spdk_vhost_log_used_vring_elem(struct spdk_vhost_dev *vdev, struct spdk_vhost_virtqueue *virtqueue,
			       uint16_t idx)
{
	uint64_t offset, len;
	uint16_t vq_idx;

	if (spdk_likely(!spdk_vhost_dev_has_feature(vdev, VHOST_F_LOG_ALL))) {
		return;
	}

	offset = offsetof(struct vring_used, ring[idx]);
	len = sizeof(virtqueue->vring.used->ring[idx]);
	vq_idx = virtqueue - vdev->virtqueue;

	rte_vhost_log_used_vring(vdev->vtgt->vid, vq_idx, offset, len);
}

static void
spdk_vhost_log_used_vring_idx(struct spdk_vhost_dev *vdev, struct spdk_vhost_virtqueue *virtqueue)
{
	uint64_t offset, len;
	uint16_t vq_idx;

	if (spdk_likely(!spdk_vhost_dev_has_feature(vdev, VHOST_F_LOG_ALL))) {
		return;
	}

	offset = offsetof(struct vring_used, idx);
	len = sizeof(virtqueue->vring.used->idx);
	vq_idx = virtqueue - vdev->virtqueue;

	rte_vhost_log_used_vring(vdev->vtgt->vid, vq_idx, offset, len);
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
	uint16_t count, i;

	count = avail_idx - last_idx;
	if (spdk_likely(count == 0)) {
		return 0;
	}

	if (spdk_unlikely(count > vring->size)) {
		/* TODO: the queue is unrecoverably broken and should be marked so.
		 * For now we will fail silently and report there are no new avail entries.
		 */
		return 0;
	}

	count = spdk_min(count, reqs_len);
	vring->last_avail_idx += count;
	for (i = 0; i < count; i++) {
		reqs[i] = vring->avail->ring[(last_idx + i) & size_mask];
	}

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_RING,
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
		if (*desc == NULL) {
			return -1;
		}

		return 0;
	}

	*desc_table = virtqueue->vring.desc;
	*desc_table_size = virtqueue->vring.size;

	return 0;
}

int
spdk_vhost_vq_used_signal(struct spdk_vhost_dev *vdev, struct spdk_vhost_virtqueue *virtqueue)
{
	if (virtqueue->used_req_cnt == 0) {
		return 0;
	}

	virtqueue->req_cnt += virtqueue->used_req_cnt;
	virtqueue->used_req_cnt = 0;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_RING,
		      "Queue %td - USED RING: sending IRQ: last used %"PRIu16"\n",
		      virtqueue - vdev->virtqueue, virtqueue->vring.last_used_idx);

	eventfd_write(virtqueue->vring.callfd, (eventfd_t)1);
	return 1;
}


static void
check_dev_io_stats(struct spdk_vhost_dev *vdev, uint64_t now)
{
	struct spdk_vhost_virtqueue *virtqueue;
	uint32_t irq_delay_base = vdev->coalescing_delay_time_base;
	uint32_t io_threshold = vdev->coalescing_io_rate_threshold;
	uint32_t irq_delay, req_cnt;
	uint16_t q_idx;

	if (now < vdev->next_stats_check_time) {
		return;
	}

	vdev->next_stats_check_time = now + vdev->stats_check_interval;
	for (q_idx = 0; q_idx < vdev->num_queues; q_idx++) {
		virtqueue = &vdev->virtqueue[q_idx];

		req_cnt = virtqueue->req_cnt + virtqueue->used_req_cnt;
		if (req_cnt <= io_threshold) {
			continue;
		}

		irq_delay = (irq_delay_base * (req_cnt - io_threshold)) / io_threshold;
		virtqueue->irq_delay_time = (uint32_t) spdk_min(0, irq_delay);

		virtqueue->req_cnt = 0;
		virtqueue->next_event_time = now;
	}
}

void
spdk_vhost_dev_used_signal(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_virtqueue *virtqueue;
	uint64_t now;
	uint16_t q_idx;

	if (vdev->coalescing_delay_time_base == 0) {
		for (q_idx = 0; q_idx < vdev->num_queues; q_idx++) {
			virtqueue = &vdev->virtqueue[q_idx];

			if (virtqueue->vring.avail->flags & VRING_AVAIL_F_NO_INTERRUPT) {
				continue;
			}

			spdk_vhost_vq_used_signal(vdev, virtqueue);
		}
	} else {
		now = spdk_get_ticks();
		check_dev_io_stats(vdev, now);

		for (q_idx = 0; q_idx < vdev->num_queues; q_idx++) {
			virtqueue = &vdev->virtqueue[q_idx];

			/* No need for event right now */
			if (now < virtqueue->next_event_time ||
			    (virtqueue->vring.avail->flags & VRING_AVAIL_F_NO_INTERRUPT)) {
				continue;
			}

			if (!spdk_vhost_vq_used_signal(vdev, virtqueue)) {
				continue;
			}

			/* Syscall is quite long so update time */
			now = spdk_get_ticks();
			virtqueue->next_event_time = now + virtqueue->irq_delay_time;
		}
	}
}

int
spdk_vhost_set_coalescing(struct spdk_vhost_tgt *vtgt, uint32_t delay_base_us,
			  uint32_t iops_threshold)
{
	struct spdk_vhost_dev *vdev;
	uint64_t delay_time_base = delay_base_us * spdk_get_ticks_hz() / 1000000ULL;
	uint32_t io_rate = iops_threshold * SPDK_VHOST_DEV_STATS_CHECK_INTERVAL_MS / 1000;

	if (delay_time_base >= UINT32_MAX) {
		SPDK_ERRLOG("Delay time of %"PRIu32" is to big\n", delay_base_us);
		return -EINVAL;
	} else if (io_rate == 0) {
		SPDK_ERRLOG("IOPS rate of %"PRIu32" is too low. Min is %u\n", io_rate,
			    1000U / SPDK_VHOST_DEV_STATS_CHECK_INTERVAL_MS);
		return -EINVAL;
	}

	vtgt->coalescing_delay_time_base = delay_time_base;
	vtgt->coalescing_io_rate_threshold = io_rate;

	vdev = vtgt->vdev;
	if (vdev) {
		vdev->coalescing_delay_time_base = vtgt->coalescing_delay_time_base;
		vdev->coalescing_io_rate_threshold = vtgt->coalescing_io_rate_threshold;
	}
	return 0;
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

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_RING,
		      "Queue %td - USED RING: last_idx=%"PRIu16" req id=%"PRIu16" len=%"PRIu32"\n",
		      virtqueue - vdev->virtqueue, vring->last_used_idx, id, len);

	spdk_vhost_log_req_desc(vdev, virtqueue, id);

	vring->last_used_idx++;
	used->ring[last_idx].id = id;
	used->ring[last_idx].len = len;

	/* Ensure the used ring is updated before we log it or increment used->idx. */
	spdk_smp_wmb();

	spdk_vhost_log_used_vring_elem(vdev, virtqueue, last_idx);
	* (volatile uint16_t *) &used->idx = vring->last_used_idx;
	spdk_vhost_log_used_vring_idx(vdev, virtqueue);

	/* Ensure all our used ring changes are visible to the guest at the time
	 * of interrupt.
	 * TODO: this is currently an sfence on x86. For other architectures we
	 * will most likely need an smp_mb(), but smp_mb() is an overkill for x86.
	 */
	spdk_wmb();

	virtqueue->used_req_cnt++;
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
	uint32_t to_boundary;
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
		to_boundary = 0x200000 - _2MB_OFFSET(payload);
		if (spdk_likely(remaining <= to_boundary)) {
			len = remaining;
		} else {
			/*
			 * Descriptor crosses a 2MB hugepage boundary.  vhost memory regions are allocated
			 *  from hugepage memory, so this means this descriptor may be described by
			 *  discontiguous vhost memory regions.  Do not blindly split on the 2MB boundary,
			 *  only split it if the two sides of the boundary do not map to the same vhost
			 *  memory region.  This helps ensure we do not exceed the max number of IOVs
			 *  defined by SPDK_VHOST_IOVS_MAX.
			 */
			len = to_boundary;
			while (len < remaining) {
				if (vva + len != (uintptr_t)spdk_vhost_gpa_to_vva(vdev, payload + len)) {
					break;
				}
				len += spdk_min(remaining - len, 0x200000);
			}
		}
		iov[*iov_index].iov_base = (void *)vva;
		iov[*iov_index].iov_len = len;
		remaining -= len;
		payload += len;
		(*iov_index)++;
	}

	return 0;
}

static struct spdk_vhost_tgt *
spdk_vhost_tgt_find_by_id(unsigned id)
{
	struct spdk_vhost_tgt *vtgt;

	TAILQ_FOREACH(vtgt, &g_spdk_vhost_tgts, tailq) {
		if (vtgt->id == id) {
			return vtgt;
		}
	}

	return NULL;
}

static struct spdk_vhost_dev *
spdk_vhost_dev_find_by_vid(int vid)
{
	struct spdk_vhost_tgt *vtgt;

	TAILQ_FOREACH(vtgt, &g_spdk_vhost_tgts, tailq) {
		if (vtgt->vdev && vtgt->vid == vid) {
			return vtgt->vdev;
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
		SPDK_INFOLOG(SPDK_LOG_VHOST, "Registering VM memory for vtophys translation - 0x%jx len:0x%jx\n",
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

struct spdk_vhost_tgt *
spdk_vhost_tgt_find(const char *vtgt_name)
{
	struct spdk_vhost_tgt *vtgt;
	size_t dev_dirname_len = strlen(dev_dirname);

	if (strncmp(vtgt_name, dev_dirname, dev_dirname_len) == 0) {
		vtgt_name += dev_dirname_len;
	}

	TAILQ_FOREACH(vtgt, &g_spdk_vhost_tgts, tailq) {
		if (strcmp(vtgt->name, vtgt_name) == 0) {
			return vtgt;
		}
	}

	return NULL;
}

static int
spdk_vhost_parse_core_mask(const char *mask, struct spdk_cpuset *cpumask)
{
	int rc;

	if (cpumask == NULL) {
		return -1;
	}

	if (mask == NULL) {
		spdk_cpuset_copy(cpumask, spdk_app_get_core_mask());
		return 0;
	}

	rc = spdk_app_parse_core_mask(mask, cpumask);
	if (rc < 0) {
		SPDK_ERRLOG("invalid cpumask %s\n", mask);
		return -1;
	}

	if (spdk_cpuset_count(cpumask) == 0) {
		SPDK_ERRLOG("no cpu is selected among reactor mask(=%s)\n",
			    spdk_cpuset_fmt(spdk_app_get_core_mask()));
		return -1;
	}

	return 0;
}

static void *
_start_rte_driver(void *arg)
{
	char *path = arg;

	if (rte_vhost_driver_start(path) != 0) {
		return NULL;
	}

	return path;
}

int
spdk_vhost_tgt_register(struct spdk_vhost_tgt *vtgt, const char *name, const char *mask_str,
			const struct spdk_vhost_tgt_backend *backend)
{
	static unsigned ctrlr_num;
	char path[PATH_MAX];
	struct stat file_stat;
	struct spdk_cpuset *cpumask;
	int rc;

	assert(vtgt);

	/* We expect targets inside g_spdk_vhost_tgts to be sorted in ascending
	 * order in regard of vtgt->id. For now we always set vtgt->id = ctrlr_num++
	 * and append each vtgt to the very end of g_spdk_vhost_tgts list.
	 * This is required for foreach vhost events to work.
	 */
	if (ctrlr_num == UINT_MAX) {
		assert(false);
		return -EINVAL;
	}

	if (name == NULL) {
		SPDK_ERRLOG("Can't register controller with no name\n");
		return -EINVAL;
	}

	cpumask = spdk_cpuset_alloc();
	if (!cpumask) {
		SPDK_ERRLOG("spdk_cpuset_alloc failed\n");
		return -ENOMEM;
	}

	if (spdk_vhost_parse_core_mask(mask_str, cpumask) != 0) {
		SPDK_ERRLOG("cpumask %s is invalid (app mask is 0x%s)\n",
			    mask_str, spdk_cpuset_fmt(spdk_app_get_core_mask()));
		rc = -EINVAL;
		goto out;
	}

	if (spdk_vhost_tgt_find(name)) {
		SPDK_ERRLOG("vhost controller %s already exists.\n", name);
		rc = -EEXIST;
		goto out;
	}

	if (snprintf(path, sizeof(path), "%s%s", dev_dirname, name) >= (int)sizeof(path)) {
		SPDK_ERRLOG("Resulting socket path for controller %s is too long: %s%s\n", name, dev_dirname,
			    name);
		rc = -EINVAL;
		goto out;
	}

	/* Register vhost driver to handle vhost messages. */
	if (stat(path, &file_stat) != -1) {
		if (!S_ISSOCK(file_stat.st_mode)) {
			SPDK_ERRLOG("Cannot create a domain socket at path \"%s\": "
				    "The file already exists and is not a socket.\n",
				    path);
			rc = -EIO;
			goto out;
		} else if (unlink(path) != 0) {
			SPDK_ERRLOG("Cannot create a domain socket at path \"%s\": "
				    "The socket already exists and failed to unlink.\n",
				    path);
			rc = -EIO;
			goto out;
		}
	}

	if (rte_vhost_driver_register(path, 0) != 0) {
		SPDK_ERRLOG("Could not register controller %s with vhost library\n", name);
		SPDK_ERRLOG("Check if domain socket %s already exists\n", path);
		rc = -EIO;
		goto out;
	}
	if (rte_vhost_driver_set_features(path, backend->virtio_features) ||
	    rte_vhost_driver_disable_features(path, backend->disabled_features)) {
		SPDK_ERRLOG("Couldn't set vhost features for controller %s\n", name);

		rte_vhost_driver_unregister(path);
		rc = -EIO;
		goto out;
	}

	if (rte_vhost_driver_callback_register(path, &g_spdk_vhost_ops) != 0) {
		rte_vhost_driver_unregister(path);
		SPDK_ERRLOG("Couldn't register callbacks for controller %s\n", name);
		rc = -EIO;
		goto out;
	}

	/* The following might start a POSIX thread that polls for incoming
	 * socket connections and calls backend->start/stop_device. These backend
	 * callbacks are also protected by the global SPDK vhost mutex, so we're
	 * safe with not initializing the vdev just yet.
	 */
	if (spdk_call_unaffinitized(_start_rte_driver, path) == NULL) {
		SPDK_ERRLOG("Failed to start vhost driver for controller %s (%d): %s\n",
			    name, errno, spdk_strerror(errno));
		rte_vhost_driver_unregister(path);
		rc = -EIO;
		goto out;
	}

	vtgt->name = strdup(name);
	vtgt->path = strdup(path);
	vtgt->id = ctrlr_num++;
	vtgt->vid = -1;
	vtgt->lcore = -1;
	vtgt->cpumask = cpumask;
	vtgt->registered = true;
	vtgt->backend = backend;

	spdk_vhost_set_coalescing(vtgt, SPDK_VHOST_COALESCING_DELAY_BASE_US,
				  SPDK_VHOST_VQ_IOPS_COALESCING_THRESHOLD);

	TAILQ_INSERT_TAIL(&g_spdk_vhost_tgts, vtgt, tailq);

	SPDK_INFOLOG(SPDK_LOG_VHOST, "Controller %s: new controller added\n", vtgt->name);
	return 0;

out:
	spdk_cpuset_free(cpumask);
	return rc;
}

int
spdk_vhost_tgt_unregister(struct spdk_vhost_tgt *vtgt)
{
	if (vtgt->vdev) {
		SPDK_ERRLOG("Controller %s has still valid connection.\n", vtgt->name);
		return -ENODEV;
	}

	if (vtgt->registered && rte_vhost_driver_unregister(vtgt->path) != 0) {
		SPDK_ERRLOG("Could not unregister controller %s with vhost library\n"
			    "Check if domain socket %s still exists\n",
			    vtgt->name, vtgt->path);
		return -EIO;
	}

	SPDK_INFOLOG(SPDK_LOG_VHOST, "Controller %s: removed\n", vtgt->name);

	free(vtgt->name);
	free(vtgt->path);
	spdk_cpuset_free(vtgt->cpumask);
	TAILQ_REMOVE(&g_spdk_vhost_tgts, vtgt, tailq);
	return 0;
}

static struct spdk_vhost_tgt *
spdk_vhost_tgt_next(unsigned i)
{
	struct spdk_vhost_tgt *vtgt;

	TAILQ_FOREACH(vtgt, &g_spdk_vhost_tgts, tailq) {
		if (vtgt->id > i) {
			return vtgt;
		}
	}

	return NULL;
}

const char *
spdk_vhost_tgt_get_name(struct spdk_vhost_tgt *vtgt)
{
	assert(vtgt != NULL);
	return vtgt->name;
}

const struct spdk_cpuset *
spdk_vhost_tgt_get_cpumask(struct spdk_vhost_tgt *vtgt)
{
	assert(vtgt != NULL);
	return vtgt->cpumask;
}

static uint32_t
spdk_vhost_allocate_reactor(struct spdk_cpuset *cpumask)
{
	uint32_t i, selected_core;
	uint32_t min_ctrlrs;

	min_ctrlrs = INT_MAX;
	selected_core = spdk_env_get_first_core();

	SPDK_ENV_FOREACH_CORE(i) {
		if (!spdk_cpuset_get_cpu(cpumask, i)) {
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

	ctx->cb_fn(ctx->vtgt, ctx);
}

static void
spdk_vhost_event_async_fn(void *arg1, void *arg2)
{
	struct spdk_vhost_dev_event_ctx *ctx = arg1;
	struct spdk_vhost_tgt *vtgt;
	struct spdk_event *ev;

	if (pthread_mutex_trylock(&g_spdk_vhost_mutex) != 0) {
		ev = spdk_event_allocate(spdk_env_get_current_core(), spdk_vhost_event_async_fn, arg1, arg2);
		spdk_event_call(ev);
		return;
	}

	vtgt = spdk_vhost_tgt_find_by_id(ctx->vtgt_id);
	if (vtgt != ctx->vtgt) {
		/* vtgt has been changed after enqueuing this event */
		vtgt = NULL;
	}

	ctx->cb_fn(vtgt, arg2);
	pthread_mutex_unlock(&g_spdk_vhost_mutex);

	free(ctx);
}

static void spdk_vhost_external_event_foreach_continue(struct spdk_vhost_tgt *vtgt,
		spdk_vhost_event_fn fn, void *arg);

static void
spdk_vhost_event_async_foreach_fn(void *arg1, void *arg2)
{
	struct spdk_vhost_dev_event_ctx *ctx = arg1;
	struct spdk_vhost_tgt *vtgt;
	struct spdk_event *ev;

	if (pthread_mutex_trylock(&g_spdk_vhost_mutex) != 0) {
		ev = spdk_event_allocate(spdk_env_get_current_core(),
					 spdk_vhost_event_async_foreach_fn, arg1, arg2);
		spdk_event_call(ev);
		return;
	}

	vtgt = spdk_vhost_tgt_find_by_id(ctx->vtgt_id);
	if (vtgt == ctx->vtgt) {
		ctx->cb_fn(vtgt, arg2);
	} else {
		/* ctx->vtgt is probably a dangling pointer at this point.
		 * It must have been removed in the meantime, so we just skip
		 * it in our foreach chain. */
	}

	vtgt = spdk_vhost_tgt_next(ctx->vtgt_id);
	spdk_vhost_external_event_foreach_continue(vtgt, ctx->cb_fn, arg2);
	pthread_mutex_unlock(&g_spdk_vhost_mutex);

	free(ctx);
}

static int
spdk_vhost_event_send(struct spdk_vhost_tgt *vtgt, spdk_vhost_event_fn cb_fn,
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

	ev_ctx.vtgt = vtgt;
	ev_ctx.cb_fn = cb_fn;
	ev = spdk_event_allocate(vtgt->lcore, spdk_vhost_event_cb, &ev_ctx, NULL);
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
spdk_vhost_event_async_send(struct spdk_vhost_tgt *vtgt, spdk_vhost_event_fn cb_fn, void *arg,
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

	ev_ctx->vtgt = vtgt;
	ev_ctx->vtgt_id = vtgt->id;
	ev_ctx->cb_fn = cb_fn;

	fn = foreach ? spdk_vhost_event_async_foreach_fn : spdk_vhost_event_async_fn;
	ev = spdk_event_allocate(ev_ctx->vtgt->lcore, fn, ev_ctx, arg);
	assert(ev);
	spdk_event_call(ev);

	return 0;
}

static void
stop_device(int vid)
{
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_tgt *vtgt;
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

	vtgt = vdev->vtgt;
	if (vtgt->lcore == -1) {
		SPDK_ERRLOG("Controller %s is not loaded.\n", vtgt->name);
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		return;
	}

	rc = spdk_vhost_event_send(vtgt, vtgt->backend->stop_device, 3, "stop device");
	if (rc != 0) {
		SPDK_ERRLOG("Couldn't stop device with vid %d.\n", vid);
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		return;
	}

	for (i = 0; i < vdev->num_queues; i++) {
		q = &vdev->virtqueue[i].vring;
		rte_vhost_set_vhost_vring_last_idx(vdev->vtgt->vid, i, q->last_avail_idx, q->last_used_idx);
	}

	free(vdev->mem);
	spdk_vhost_free_reactor(vtgt->lcore);
	vtgt->lcore = -1;
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
}

static int
start_device(int vid)
{
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_tgt *vtgt;
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

	vtgt = vdev->vtgt;
	if (vtgt->lcore != -1) {
		SPDK_ERRLOG("Controller %s already loaded.\n", vtgt->name);
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

		if (vdev->virtqueue[i].vring.size == 0) {
			SPDK_ERRLOG("vhost device %d: Queue %"PRIu16" has size 0.\n", vid, i);
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

	vdev->coalescing_delay_time_base = vtgt->coalescing_delay_time_base;
	vdev->coalescing_io_rate_threshold = vtgt->coalescing_io_rate_threshold;
	vdev->next_stats_check_time = 0;
	vdev->stats_check_interval = SPDK_VHOST_DEV_STATS_CHECK_INTERVAL_MS * spdk_get_ticks_hz() /
				     1000UL;

	/*
	 * Not sure right now but this look like some kind of QEMU bug and guest IO
	 * might be frozed without kicking all queues after live-migration. This look like
	 * the previous vhost instance failed to effectively deliver all interrupts before
	 * the GET_VRING_BASE message. This shouldn't harm guest since spurious interrupts
	 * should be ignored by guest virtio driver.
	 *
	 * Tested on QEMU 2.10.91 and 2.11.50.
	 */
	for (i = 0; i < num_queues; i++) {
		if (vdev->virtqueue[i].vring.callfd != -1) {
			eventfd_write(vdev->virtqueue[i].vring.callfd, (eventfd_t)1);
		}
	}

	vtgt->lcore = spdk_vhost_allocate_reactor(vtgt->cpumask);
	rc = spdk_vhost_event_send(vtgt, vtgt->backend->start_device, 3, "start device");
	if (rc != 0) {
		free(vdev->mem);
		spdk_vhost_free_reactor(vtgt->lcore);
		vtgt->lcore = -1;
	}

out:
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	return rc;
}

static int
get_config(int vid, uint8_t *config, uint32_t len)
{
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_tgt *vtgt;
	int rc = -1;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vdev = spdk_vhost_dev_find_by_vid(vid);
	if (vdev == NULL) {
		SPDK_ERRLOG("Controller with vid %d doesn't exist.\n", vid);
		goto out;
	}

	vtgt = vdev->vtgt;
	if (vtgt->backend->vhost_get_config) {
		rc = vtgt->backend->vhost_get_config(vtgt, config, len);
	}

out:
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	return rc;
}

static int
set_config(int vid, uint8_t *config, uint32_t offset, uint32_t size, uint32_t flags)
{
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_tgt *vtgt;
	int rc = -1;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vdev = spdk_vhost_dev_find_by_vid(vid);
	if (vdev == NULL) {
		SPDK_ERRLOG("Controller with vid %d doesn't exist.\n", vid);
		goto out;
	}

	vtgt = vdev->vtgt;
	if (vtgt->backend->vhost_set_config) {
		rc = vtgt->backend->vhost_set_config(vtgt, config, offset, size, flags);
	}

out:
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	return rc;
}

int
spdk_vhost_set_socket_path(const char *basename)
{
	int ret;

	if (basename && strlen(basename) > 0) {
		ret = snprintf(dev_dirname, sizeof(dev_dirname) - 2, "%s", basename);
		if ((size_t)ret >= sizeof(dev_dirname) - 2) {
			SPDK_ERRLOG("Char dev dir path length %d is too long\n", ret);
			return -EINVAL;
		}

		if (dev_dirname[ret - 1] != '/') {
			dev_dirname[ret] = '/';
			dev_dirname[ret + 1]  = '\0';
		}
	}

	return 0;
}

static void *
session_shutdown(void *arg)
{
	struct spdk_vhost_tgt *vtgt = NULL;

	TAILQ_FOREACH(vtgt, &g_spdk_vhost_tgts, tailq) {
		rte_vhost_driver_unregister(vtgt->path);
		vtgt->registered = false;
	}

	SPDK_INFOLOG(SPDK_LOG_VHOST, "Exiting\n");
	spdk_event_call((struct spdk_event *)arg);
	return NULL;
}

void
spdk_vhost_dump_config_json(struct spdk_vhost_tgt *vtgt,
			    struct spdk_json_write_ctx *w)
{
	assert(vtgt->backend->dump_config_json != NULL);
	vtgt->backend->dump_config_json(vtgt, w);
}

int
spdk_vhost_tgt_remove(struct spdk_vhost_tgt *vtgt)
{
	return vtgt->backend->remove_device(vtgt);
}

static int
new_connection(int vid)
{
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_tgt *vtgt;
	char ifname[PATH_MAX];
	int rc = -1;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	if (rte_vhost_get_ifname(vid, ifname, PATH_MAX) < 0) {
		SPDK_ERRLOG("Couldn't get a valid ifname for device with vid %d\n", vid);
		goto err;
	}

	vtgt = spdk_vhost_tgt_find(ifname);
	if (vtgt == NULL) {
		SPDK_ERRLOG("Couldn't find device with vid %d to create connection for.\n", vid);
		goto err;
	}

	/* since pollers are not running it safe not to use spdk_event here */
	if (vtgt->vdev) {
		SPDK_ERRLOG("Device with vid %d is already connected.\n", vid);
		goto err;
	}

	vdev = spdk_dma_zmalloc(sizeof(struct spdk_vhost_dev),
				SPDK_CACHE_LINE_SIZE, NULL);
	if (vdev == NULL) {
		SPDK_ERRLOG("vdev calloc failed.\n");
		goto err;
	}

	vdev->vtgt = vtgt;
	vtgt->vid = vid;
	vtgt->vdev = vdev;

	rc = 0;
err:
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	return rc;
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
	vdev->vtgt->vid = -1;
	vdev->vtgt->vdev = NULL;
	spdk_dma_free(vdev);
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
}

void
spdk_vhost_call_external_event(const char *vtgt_name, spdk_vhost_event_fn fn, void *arg)
{
	struct spdk_vhost_tgt *vtgt;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vtgt = spdk_vhost_tgt_find(vtgt_name);

	if (vtgt == NULL) {
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		fn(NULL, arg);
		return;
	}

	if (vtgt->lcore == -1) {
		fn(vtgt, arg);
	} else {
		spdk_vhost_event_async_send(vtgt, fn, arg, false);
	}

	pthread_mutex_unlock(&g_spdk_vhost_mutex);
}

static void
spdk_vhost_external_event_foreach_continue(struct spdk_vhost_tgt *vtgt,
		spdk_vhost_event_fn fn, void *arg)
{
	if (vtgt == NULL) {
		fn(NULL, arg);
		return;
	}

	while (vtgt->lcore == -1) {
		fn(vtgt, arg);
		vtgt = spdk_vhost_tgt_next(vtgt->id);
		if (vtgt == NULL) {
			fn(NULL, arg);
			return;
		}
	}

	spdk_vhost_event_async_send(vtgt, fn, arg, true);
}

void
spdk_vhost_call_external_event_foreach(spdk_vhost_event_fn fn, void *arg)
{
	struct spdk_vhost_tgt *vtgt;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vtgt = TAILQ_FIRST(&g_spdk_vhost_tgts);
	spdk_vhost_external_event_foreach_continue(vtgt, fn, arg);
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

int
spdk_vhost_init(void)
{
	uint32_t last_core;
	int ret;

	last_core = spdk_env_get_last_core();
	g_num_ctrlrs = calloc(last_core + 1, sizeof(uint32_t));
	if (!g_num_ctrlrs) {
		SPDK_ERRLOG("Could not allocate array size=%u for g_num_ctrlrs\n",
			    last_core + 1);
		return -1;
	}

	ret = spdk_vhost_scsi_controller_construct();
	if (ret != 0) {
		SPDK_ERRLOG("Cannot construct vhost controllers\n");
		return -1;
	}

	ret = spdk_vhost_blk_controller_construct();
	if (ret != 0) {
		SPDK_ERRLOG("Cannot construct vhost block controllers\n");
		return -1;
	}

	ret = spdk_vhost_nvme_controller_construct();
	if (ret != 0) {
		SPDK_ERRLOG("Cannot construct vhost NVMe controllers\n");
		return -1;
	}

	return 0;
}

static int
_spdk_vhost_fini_remove_vtgt_cb(struct spdk_vhost_tgt *vtgt, void *arg)
{
	spdk_vhost_fini_cb fini_cb = arg;

	if (vtgt != NULL) {
		spdk_vhost_tgt_remove(vtgt);
		return 0;
	}

	/* All targets are removed now. */
	free(g_num_ctrlrs);
	fini_cb();
	return 0;
}

static void
_spdk_vhost_fini(void *arg1, void *arg2)
{
	spdk_vhost_fini_cb fini_cb = arg1;

	spdk_vhost_call_external_event_foreach(_spdk_vhost_fini_remove_vtgt_cb, fini_cb);
}

void
spdk_vhost_fini(spdk_vhost_fini_cb fini_cb)
{
	pthread_t tid;
	int rc;
	struct spdk_event *fini_ev;

	fini_ev = spdk_event_allocate(spdk_env_get_current_core(), _spdk_vhost_fini, fini_cb, NULL);

	/* rte_vhost API for removing sockets is not asynchronous. Since it may call SPDK
	 * ops for stopping a device or removing a connection, we need to call it from
	 * a separate thread to avoid deadlock.
	 */
	rc = pthread_create(&tid, NULL, &session_shutdown, fini_ev);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to start session shutdown thread (%d): %s\n", rc, spdk_strerror(rc));
		abort();
	}
	pthread_detach(tid);
}

SPDK_LOG_REGISTER_COMPONENT("vhost", SPDK_LOG_VHOST)
SPDK_LOG_REGISTER_COMPONENT("vhost_ring", SPDK_LOG_VHOST_RING)
