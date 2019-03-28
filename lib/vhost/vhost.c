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

#include "spdk_internal/memory.h"

static uint32_t *g_num_ctrlrs;

/* Path to folder where character device will be created. Can be set by user. */
static char dev_dirname[PATH_MAX] = "";

struct spdk_vhost_session_fn_ctx {
	/** Device pointer obtained before enqueuing the event */
	struct spdk_vhost_dev *vdev;

	/** ID of the session to send event to. */
	uint32_t vsession_id;

	/** User callback function to be executed on given lcore. */
	spdk_vhost_session_fn cb_fn;

	/** Semaphore used to signal that event is done. */
	sem_t sem;

	/** Response to be written by enqueued event. */
	int response;
};

static int new_connection(int vid);
static int start_device(int vid);
static void stop_device(int vid);
static void destroy_connection(int vid);

#ifdef SPDK_CONFIG_VHOST_INTERNAL_LIB
static int get_config(int vid, uint8_t *config, uint32_t len);
static int set_config(int vid, uint8_t *config, uint32_t offset,
		      uint32_t size, uint32_t flags);
#endif

const struct vhost_device_ops g_spdk_vhost_ops = {
	.new_device =  start_device,
	.destroy_device = stop_device,
	.new_connection = new_connection,
	.destroy_connection = destroy_connection,
#ifdef SPDK_CONFIG_VHOST_INTERNAL_LIB
	.get_config = get_config,
	.set_config = set_config,
	.vhost_nvme_admin_passthrough = spdk_vhost_nvme_admin_passthrough,
	.vhost_nvme_set_cq_call = spdk_vhost_nvme_set_cq_call,
	.vhost_nvme_get_cap = spdk_vhost_nvme_get_cap,
	.vhost_nvme_set_bar_mr = spdk_vhost_nvme_set_bar_mr,
#endif
};

static TAILQ_HEAD(, spdk_vhost_dev) g_spdk_vhost_devices = TAILQ_HEAD_INITIALIZER(
			g_spdk_vhost_devices);
static pthread_mutex_t g_spdk_vhost_mutex = PTHREAD_MUTEX_INITIALIZER;

void *spdk_vhost_gpa_to_vva(struct spdk_vhost_session *vsession, uint64_t addr, uint64_t len)
{
	void *vva;
	uint64_t newlen;

	newlen = len;
	vva = (void *)rte_vhost_va_from_guest_pa(vsession->mem, addr, &newlen);
	if (newlen != len) {
		return NULL;
	}

	return vva;

}

static void
spdk_vhost_log_req_desc(struct spdk_vhost_session *vsession, struct spdk_vhost_virtqueue *virtqueue,
			uint16_t req_id)
{
	struct vring_desc *desc, *desc_table;
	uint32_t desc_table_size;
	int rc;

	if (spdk_likely(!spdk_vhost_dev_has_feature(vsession, VHOST_F_LOG_ALL))) {
		return;
	}

	rc = spdk_vhost_vq_get_desc(vsession, virtqueue, req_id, &desc, &desc_table, &desc_table_size);
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
			rte_vhost_log_write(vsession->vid, desc->addr, desc->len);
		}
		spdk_vhost_vring_desc_get_next(&desc, desc_table, desc_table_size);
	} while (desc);
}

static void
spdk_vhost_log_used_vring_elem(struct spdk_vhost_session *vsession,
			       struct spdk_vhost_virtqueue *virtqueue,
			       uint16_t idx)
{
	uint64_t offset, len;
	uint16_t vq_idx;

	if (spdk_likely(!spdk_vhost_dev_has_feature(vsession, VHOST_F_LOG_ALL))) {
		return;
	}

	offset = offsetof(struct vring_used, ring[idx]);
	len = sizeof(virtqueue->vring.used->ring[idx]);
	vq_idx = virtqueue - vsession->virtqueue;

	rte_vhost_log_used_vring(vsession->vid, vq_idx, offset, len);
}

static void
spdk_vhost_log_used_vring_idx(struct spdk_vhost_session *vsession,
			      struct spdk_vhost_virtqueue *virtqueue)
{
	uint64_t offset, len;
	uint16_t vq_idx;

	if (spdk_likely(!spdk_vhost_dev_has_feature(vsession, VHOST_F_LOG_ALL))) {
		return;
	}

	offset = offsetof(struct vring_used, idx);
	len = sizeof(virtqueue->vring.used->idx);
	vq_idx = virtqueue - vsession->virtqueue;

	rte_vhost_log_used_vring(vsession->vid, vq_idx, offset, len);
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
	uint16_t last_idx = virtqueue->last_avail_idx, avail_idx = avail->idx;
	uint16_t count, i;
	VhostInflightInfo *inflight = vring->inflight;
	uint16_t idx;

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
	virtqueue->last_avail_idx += count;
	for (i = 0; i < count; i++) {
		idx = vring->avail->ring[(last_idx + i) & size_mask];
		reqs[i] = idx;
		if (inflight) {
			inflight->desc[idx].inflight = 1;
		}
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
spdk_vhost_vq_get_desc(struct spdk_vhost_session *vsession, struct spdk_vhost_virtqueue *virtqueue,
		       uint16_t req_idx, struct vring_desc **desc, struct vring_desc **desc_table,
		       uint32_t *desc_table_size)
{
	if (spdk_unlikely(req_idx >= virtqueue->vring.size)) {
		return -1;
	}

	*desc = &virtqueue->vring.desc[req_idx];

	if (spdk_vhost_vring_desc_is_indirect(*desc)) {
		*desc_table_size = (*desc)->len / sizeof(**desc);
		*desc_table = spdk_vhost_gpa_to_vva(vsession, (*desc)->addr,
						    sizeof(**desc) * *desc_table_size);
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
spdk_vhost_vq_used_signal(struct spdk_vhost_session *vsession,
			  struct spdk_vhost_virtqueue *virtqueue)
{
	if (virtqueue->used_req_cnt == 0) {
		return 0;
	}

	virtqueue->req_cnt += virtqueue->used_req_cnt;
	virtqueue->used_req_cnt = 0;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_RING,
		      "Queue %td - USED RING: sending IRQ: last used %"PRIu16"\n",
		      virtqueue - vsession->virtqueue, virtqueue->last_used_idx);

	eventfd_write(virtqueue->vring.callfd, (eventfd_t)1);
	return 1;
}


static void
check_session_io_stats(struct spdk_vhost_session *vsession, uint64_t now)
{
	struct spdk_vhost_virtqueue *virtqueue;
	uint32_t irq_delay_base = vsession->coalescing_delay_time_base;
	uint32_t io_threshold = vsession->coalescing_io_rate_threshold;
	int32_t irq_delay;
	uint32_t req_cnt;
	uint16_t q_idx;

	if (now < vsession->next_stats_check_time) {
		return;
	}

	vsession->next_stats_check_time = now + vsession->stats_check_interval;
	for (q_idx = 0; q_idx < vsession->max_queues; q_idx++) {
		virtqueue = &vsession->virtqueue[q_idx];

		req_cnt = virtqueue->req_cnt + virtqueue->used_req_cnt;
		if (req_cnt <= io_threshold) {
			continue;
		}

		irq_delay = (irq_delay_base * (req_cnt - io_threshold)) / io_threshold;
		virtqueue->irq_delay_time = (uint32_t) spdk_max(0, irq_delay);

		virtqueue->req_cnt = 0;
		virtqueue->next_event_time = now;
	}
}

void
spdk_vhost_session_used_signal(struct spdk_vhost_session *vsession)
{
	struct spdk_vhost_virtqueue *virtqueue;
	uint64_t now;
	uint16_t q_idx;

	if (vsession->coalescing_delay_time_base == 0) {
		for (q_idx = 0; q_idx < vsession->max_queues; q_idx++) {
			virtqueue = &vsession->virtqueue[q_idx];

			if (virtqueue->vring.desc == NULL ||
			    (virtqueue->vring.avail->flags & VRING_AVAIL_F_NO_INTERRUPT)) {
				continue;
			}

			spdk_vhost_vq_used_signal(vsession, virtqueue);
		}
	} else {
		now = spdk_get_ticks();
		check_session_io_stats(vsession, now);

		for (q_idx = 0; q_idx < vsession->max_queues; q_idx++) {
			virtqueue = &vsession->virtqueue[q_idx];

			/* No need for event right now */
			if (now < virtqueue->next_event_time ||
			    (virtqueue->vring.avail->flags & VRING_AVAIL_F_NO_INTERRUPT)) {
				continue;
			}

			if (!spdk_vhost_vq_used_signal(vsession, virtqueue)) {
				continue;
			}

			/* Syscall is quite long so update time */
			now = spdk_get_ticks();
			virtqueue->next_event_time = now + virtqueue->irq_delay_time;
		}
	}
}

static int
spdk_vhost_session_set_coalescing(struct spdk_vhost_dev *vdev,
				  struct spdk_vhost_session *vsession, void *ctx)
{
	if (vdev == NULL || vsession == NULL) {
		/* nothing to do */
		return 0;
	}

	vsession->coalescing_delay_time_base =
		vdev->coalescing_delay_us * spdk_get_ticks_hz() / 1000000ULL;
	vsession->coalescing_io_rate_threshold =
		vdev->coalescing_iops_threshold * SPDK_VHOST_STATS_CHECK_INTERVAL_MS / 1000U;
	return 0;
}

int
spdk_vhost_set_coalescing(struct spdk_vhost_dev *vdev, uint32_t delay_base_us,
			  uint32_t iops_threshold)
{
	uint64_t delay_time_base = delay_base_us * spdk_get_ticks_hz() / 1000000ULL;
	uint32_t io_rate = iops_threshold * SPDK_VHOST_STATS_CHECK_INTERVAL_MS / 1000U;

	if (delay_time_base >= UINT32_MAX) {
		SPDK_ERRLOG("Delay time of %"PRIu32" is to big\n", delay_base_us);
		return -EINVAL;
	} else if (io_rate == 0) {
		SPDK_ERRLOG("IOPS rate of %"PRIu32" is too low. Min is %u\n", io_rate,
			    1000U / SPDK_VHOST_STATS_CHECK_INTERVAL_MS);
		return -EINVAL;
	}

	vdev->coalescing_delay_us = delay_base_us;
	vdev->coalescing_iops_threshold = iops_threshold;

	spdk_vhost_dev_foreach_session(vdev, spdk_vhost_session_set_coalescing, NULL);
	return 0;
}

void
spdk_vhost_get_coalescing(struct spdk_vhost_dev *vdev, uint32_t *delay_base_us,
			  uint32_t *iops_threshold)
{
	if (delay_base_us) {
		*delay_base_us = vdev->coalescing_delay_us;
	}

	if (iops_threshold) {
		*iops_threshold = vdev->coalescing_iops_threshold;
	}
}

static void
spdk_inflight_clear_begin(VhostInflightInfo *inflight, uint16_t idx)
{
	if (unlikely(!inflight)) {
		return;
	}

	inflight->last_inflight_io = idx;
}

static void
spdk_inflight_clear_end(VhostInflightInfo *inflight,
			struct spdk_vhost_virtqueue *virtqueue,
			uint16_t idx)
{
	if (unlikely(!inflight)) {
		return;
	}

	spdk_compiler_barrier();
	inflight->desc[idx].inflight = 0;
	spdk_compiler_barrier();
	inflight->used_idx = virtqueue->last_used_idx;
}

/*
 * Enqueue id and len to used ring.
 */
void
spdk_vhost_vq_used_ring_enqueue(struct spdk_vhost_session *vsession,
				struct spdk_vhost_virtqueue *virtqueue,
				uint16_t id, uint32_t len)
{
	struct rte_vhost_vring *vring = &virtqueue->vring;
	struct vring_used *used = vring->used;
	uint16_t last_idx = virtqueue->last_used_idx & (vring->size - 1);
	VhostInflightInfo *inflight = vring->inflight;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_RING,
		      "Queue %td - USED RING: last_idx=%"PRIu16" req id=%"PRIu16" len=%"PRIu32"\n",
		      virtqueue - vsession->virtqueue, virtqueue->last_used_idx, id, len);

	spdk_vhost_log_req_desc(vsession, virtqueue, id);

	virtqueue->last_used_idx++;
	used->ring[last_idx].id = id;
	used->ring[last_idx].len = len;

	/* Ensure the used ring is updated before we log it or increment used->idx. */
	spdk_smp_wmb();

	spdk_inflight_clear_begin(inflight, id);

	spdk_vhost_log_used_vring_elem(vsession, virtqueue, last_idx);
	* (volatile uint16_t *) &used->idx = virtqueue->last_used_idx;
	spdk_vhost_log_used_vring_idx(vsession, virtqueue);

	spdk_inflight_clear_end(inflight, virtqueue, id);

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

int
spdk_vhost_vring_desc_to_iov(struct spdk_vhost_session *vsession, struct iovec *iov,
			     uint16_t *iov_index, const struct vring_desc *desc)
{
	uint32_t remaining = desc->len;
	uint32_t to_boundary;
	uint32_t len;
	uintptr_t payload = desc->addr;
	uintptr_t vva;

	do {
		if (*iov_index >= SPDK_VHOST_IOVS_MAX) {
			SPDK_ERRLOG("SPDK_VHOST_IOVS_MAX(%d) reached\n", SPDK_VHOST_IOVS_MAX);
			return -1;
		}
		vva = (uintptr_t)rte_vhost_gpa_to_vva(vsession->mem, payload);
		if (vva == 0) {
			SPDK_ERRLOG("gpa_to_vva(%p) == NULL\n", (void *)payload);
			return -1;
		}
		to_boundary = VALUE_2MB - _2MB_OFFSET(payload);
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
				if (vva + len != (uintptr_t)rte_vhost_gpa_to_vva(vsession->mem, payload + len)) {
					break;
				}
				len += spdk_min(remaining - len, VALUE_2MB);
			}
		}
		iov[*iov_index].iov_base = (void *)vva;
		iov[*iov_index].iov_len = len;
		remaining -= len;
		payload += len;
		(*iov_index)++;
	} while (remaining);

	return 0;
}

static struct spdk_vhost_session *
spdk_vhost_session_find_by_id(struct spdk_vhost_dev *vdev, unsigned id)
{
	struct spdk_vhost_session *vsession;

	TAILQ_FOREACH(vsession, &vdev->vsessions, tailq) {
		if (vsession->id == id) {
			return vsession;
		}
	}

	return NULL;
}

struct spdk_vhost_session *
spdk_vhost_session_find_by_vid(int vid)
{
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_session *vsession;

	TAILQ_FOREACH(vdev, &g_spdk_vhost_devices, tailq) {
		TAILQ_FOREACH(vsession, &vdev->vsessions, tailq) {
			if (vsession->vid == vid) {
				return vsession;
			}
		}
	}

	return NULL;
}

#define SHIFT_2MB	21
#define SIZE_2MB	(1ULL << SHIFT_2MB)
#define FLOOR_2MB(x)	(((uintptr_t)x) / SIZE_2MB) << SHIFT_2MB
#define CEIL_2MB(x)	((((uintptr_t)x) + SIZE_2MB - 1) / SIZE_2MB) << SHIFT_2MB

static void
spdk_vhost_session_mem_register(struct spdk_vhost_session *vsession)
{
	struct rte_vhost_mem_region *region;
	uint32_t i;

	for (i = 0; i < vsession->mem->nregions; i++) {
		uint64_t start, end, len;
		region = &vsession->mem->regions[i];
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

static void
spdk_vhost_session_mem_unregister(struct spdk_vhost_session *vsession)
{
	struct rte_vhost_mem_region *region;
	uint32_t i;

	for (i = 0; i < vsession->mem->nregions; i++) {
		uint64_t start, end, len;
		region = &vsession->mem->regions[i];
		start = FLOOR_2MB(region->mmap_addr);
		end = CEIL_2MB(region->mmap_addr + region->mmap_size);
		len = end - start;

		if (spdk_vtophys((void *) start, NULL) == SPDK_VTOPHYS_ERROR) {
			continue; /* region has not been registered */
		}

		if (spdk_mem_unregister((void *)start, len) != 0) {
			assert(false);
		}
	}

}

void
spdk_vhost_free_reactor(uint32_t lcore)
{
	g_num_ctrlrs[lcore]--;
}

struct spdk_vhost_dev *
spdk_vhost_dev_next(struct spdk_vhost_dev *vdev)
{
	if (vdev == NULL) {
		return TAILQ_FIRST(&g_spdk_vhost_devices);
	}

	return TAILQ_NEXT(vdev, tailq);
}

struct spdk_vhost_dev *
spdk_vhost_dev_find(const char *ctrlr_name)
{
	struct spdk_vhost_dev *vdev;
	size_t dev_dirname_len = strlen(dev_dirname);

	if (strncmp(ctrlr_name, dev_dirname, dev_dirname_len) == 0) {
		ctrlr_name += dev_dirname_len;
	}

	TAILQ_FOREACH(vdev, &g_spdk_vhost_devices, tailq) {
		if (strcmp(vdev->name, ctrlr_name) == 0) {
			return vdev;
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
spdk_vhost_dev_register(struct spdk_vhost_dev *vdev, const char *name, const char *mask_str,
			const struct spdk_vhost_dev_backend *backend)
{
	char path[PATH_MAX];
	struct stat file_stat;
	struct spdk_cpuset *cpumask;
	int rc;

	assert(vdev);
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

	if (spdk_vhost_dev_find(name)) {
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

	vdev->name = strdup(name);
	vdev->path = strdup(path);
	if (vdev->name == NULL || vdev->path == NULL) {
		free(vdev->name);
		free(vdev->path);
		rte_vhost_driver_unregister(path);
		rc = -EIO;
		goto out;
	}

	vdev->cpumask = cpumask;
	vdev->registered = true;
	vdev->backend = backend;
	TAILQ_INIT(&vdev->vsessions);
	TAILQ_INSERT_TAIL(&g_spdk_vhost_devices, vdev, tailq);

	spdk_vhost_set_coalescing(vdev, SPDK_VHOST_COALESCING_DELAY_BASE_US,
				  SPDK_VHOST_VQ_IOPS_COALESCING_THRESHOLD);

	spdk_vhost_dev_install_rte_compat_hooks(vdev);

	/* The following might start a POSIX thread that polls for incoming
	 * socket connections and calls backend->start/stop_device. These backend
	 * callbacks are also protected by the global SPDK vhost mutex, so we're
	 * safe with not initializing the vdev just yet.
	 */
	if (spdk_call_unaffinitized(_start_rte_driver, path) == NULL) {
		SPDK_ERRLOG("Failed to start vhost driver for controller %s (%d): %s\n",
			    name, errno, spdk_strerror(errno));
		rte_vhost_driver_unregister(path);
		TAILQ_REMOVE(&g_spdk_vhost_devices, vdev, tailq);
		free(vdev->name);
		free(vdev->path);
		rc = -EIO;
		goto out;
	}

	SPDK_INFOLOG(SPDK_LOG_VHOST, "Controller %s: new controller added\n", vdev->name);
	return 0;

out:
	spdk_cpuset_free(cpumask);
	return rc;
}

int
spdk_vhost_dev_unregister(struct spdk_vhost_dev *vdev)
{
	if (!TAILQ_EMPTY(&vdev->vsessions)) {
		SPDK_ERRLOG("Controller %s has still valid connection.\n", vdev->name);
		return -EBUSY;
	}

	if (vdev->registered && rte_vhost_driver_unregister(vdev->path) != 0) {
		SPDK_ERRLOG("Could not unregister controller %s with vhost library\n"
			    "Check if domain socket %s still exists\n",
			    vdev->name, vdev->path);
		return -EIO;
	}

	SPDK_INFOLOG(SPDK_LOG_VHOST, "Controller %s: removed\n", vdev->name);

	free(vdev->name);
	free(vdev->path);
	spdk_cpuset_free(vdev->cpumask);
	TAILQ_REMOVE(&g_spdk_vhost_devices, vdev, tailq);
	return 0;
}

static struct spdk_vhost_session *
spdk_vhost_session_next(struct spdk_vhost_dev *vdev, unsigned prev_id)
{
	struct spdk_vhost_session *vsession;

	TAILQ_FOREACH(vsession, &vdev->vsessions, tailq) {
		if (vsession->id > prev_id) {
			return vsession;
		}
	}

	return NULL;
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
	return vdev->cpumask;
}

uint32_t
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

static void
complete_session_event(struct spdk_vhost_session *vsession, int response)
{
	struct spdk_vhost_session_fn_ctx *ctx = vsession->event_ctx;

	ctx->response = response;
	sem_post(&ctx->sem);
}

void
spdk_vhost_session_start_done(struct spdk_vhost_session *vsession, int response)
{
	if (response == 0) {
		vsession->lcore = spdk_env_get_current_core();
		assert(vsession->vdev->active_session_num < UINT32_MAX);
		vsession->vdev->active_session_num++;
	}
	complete_session_event(vsession, response);
}

void
spdk_vhost_session_stop_done(struct spdk_vhost_session *vsession, int response)
{
	if (response == 0) {
		vsession->lcore = -1;
		assert(vsession->vdev->active_session_num > 0);
		vsession->vdev->active_session_num--;
	}
	complete_session_event(vsession, response);
}

static void
spdk_vhost_event_cb(void *arg1, void *arg2)
{
	struct spdk_vhost_session_fn_ctx *ctx = arg1;
	struct spdk_vhost_session *vsession;
	struct spdk_event *ev;

	if (pthread_mutex_trylock(&g_spdk_vhost_mutex) != 0) {
		ev = spdk_event_allocate(spdk_env_get_current_core(),
					 spdk_vhost_event_cb, arg1, arg2);
		spdk_event_call(ev);
		return;
	}

	vsession = spdk_vhost_session_find_by_id(ctx->vdev, ctx->vsession_id);
	ctx->cb_fn(ctx->vdev, vsession, NULL);
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
}

static void spdk_vhost_external_event_foreach_continue(struct spdk_vhost_dev *vdev,
		struct spdk_vhost_session *vsession,
		spdk_vhost_session_fn fn, void *arg);

static void
spdk_vhost_event_async_foreach_fn(void *arg1, void *arg2)
{
	struct spdk_vhost_session_fn_ctx *ctx = arg1;
	struct spdk_vhost_session *vsession = NULL;
	struct spdk_vhost_dev *vdev = ctx->vdev;
	struct spdk_event *ev;
	int rc;

	if (pthread_mutex_trylock(&g_spdk_vhost_mutex) != 0) {
		ev = spdk_event_allocate(spdk_env_get_current_core(),
					 spdk_vhost_event_async_foreach_fn, arg1, arg2);
		spdk_event_call(ev);
		return;
	}

	vsession = spdk_vhost_session_find_by_id(vdev, ctx->vsession_id);
	if (vsession == NULL) {
		/* The session must have been removed in the meantime, so we
		 * just skip it in our foreach chain
		 */
		goto out_unlock_continue;
	}

	if (vsession->lcore >= 0 &&
	    (uint32_t)vsession->lcore != spdk_env_get_current_core()) {
		/* if session has been relocated to other core, it is no longer thread-safe
		 * to access its contents here. Even though we're running under the global
		 * vhost mutex, the session itself (and its pollers) are not. We need to chase
		 * the session thread as many times as necessary.
		 */
		ev = spdk_event_allocate(vsession->lcore,
					 spdk_vhost_event_async_foreach_fn, arg1, arg2);
		spdk_event_call(ev);
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		return;
	}

	rc = ctx->cb_fn(vdev, vsession, arg2);
	if (rc < 0) {
		goto out_unlock;
	}

out_unlock_continue:
	vsession = spdk_vhost_session_next(vdev, ctx->vsession_id);
	spdk_vhost_external_event_foreach_continue(vdev, vsession, ctx->cb_fn, arg2);
out_unlock:
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	free(ctx);
}

int
spdk_vhost_session_send_event(int32_t lcore, struct spdk_vhost_session *vsession,
			      spdk_vhost_session_fn cb_fn, unsigned timeout_sec,
			      const char *errmsg)
{
	struct spdk_vhost_session_fn_ctx ev_ctx = {0};
	struct spdk_event *ev;
	struct timespec timeout;
	int rc;

	rc = sem_init(&ev_ctx.sem, 0, 0);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to initialize semaphore for vhost timed event\n");
		return -errno;
	}

	ev_ctx.vdev = vsession->vdev;
	ev_ctx.vsession_id = vsession->id;
	ev_ctx.cb_fn = cb_fn;

	vsession->event_ctx = &ev_ctx;
	ev = spdk_event_allocate(lcore, spdk_vhost_event_cb, &ev_ctx, NULL);
	assert(ev);
	spdk_event_call(ev);
	pthread_mutex_unlock(&g_spdk_vhost_mutex);

	clock_gettime(CLOCK_REALTIME, &timeout);
	timeout.tv_sec += timeout_sec;

	rc = sem_timedwait(&ev_ctx.sem, &timeout);
	if (rc != 0) {
		SPDK_ERRLOG("Timeout waiting for event: %s.\n", errmsg);
		sem_wait(&ev_ctx.sem);
	}

	sem_destroy(&ev_ctx.sem);
	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vsession->event_ctx = NULL;
	return ev_ctx.response;
}

static int
spdk_vhost_event_async_send_foreach_continue(struct spdk_vhost_session *vsession,
		spdk_vhost_session_fn cb_fn, void *arg)
{
	struct spdk_vhost_dev *vdev = vsession->vdev;
	struct spdk_vhost_session_fn_ctx *ev_ctx;
	struct spdk_event *ev;

	ev_ctx = calloc(1, sizeof(*ev_ctx));
	if (ev_ctx == NULL) {
		SPDK_ERRLOG("Failed to alloc vhost event.\n");
		assert(false);
		return -ENOMEM;
	}

	ev_ctx->vdev = vdev;
	ev_ctx->vsession_id = vsession->id;
	ev_ctx->cb_fn = cb_fn;

	ev = spdk_event_allocate(vsession->lcore,
				 spdk_vhost_event_async_foreach_fn, ev_ctx, arg);
	assert(ev);
	spdk_event_call(ev);

	return 0;
}

static void
_stop_session(struct spdk_vhost_session *vsession)
{
	struct spdk_vhost_dev *vdev = vsession->vdev;
	struct spdk_vhost_virtqueue *q;
	int rc;
	uint16_t i;

	rc = vdev->backend->stop_session(vsession);
	if (rc != 0) {
		SPDK_ERRLOG("Couldn't stop device with vid %d.\n", vsession->vid);
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		return;
	}

	for (i = 0; i < vsession->max_queues; i++) {
		q = &vsession->virtqueue[i];
		if (q->vring.desc == NULL) {
			continue;
		}
		rte_vhost_set_vring_base(vsession->vid, i, q->last_avail_idx, q->last_used_idx);
	}

	spdk_vhost_session_mem_unregister(vsession);
	free(vsession->mem);
}

static void
stop_device(int vid)
{
	struct spdk_vhost_session *vsession;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vsession = spdk_vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Couldn't find session with vid %d.\n", vid);
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		return;
	}

	if (vsession->lcore == -1) {
		/* already stopped, nothing to do */
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		return;
	}

	_stop_session(vsession);
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
}

static int
start_device(int vid)
{
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_session *vsession;
	int rc = -1;
	uint16_t i;

	pthread_mutex_lock(&g_spdk_vhost_mutex);

	vsession = spdk_vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Couldn't find session with vid %d.\n", vid);
		goto out;
	}

	vdev = vsession->vdev;
	if (vsession->lcore != -1) {
		/* already started, nothing to do */
		rc = 0;
		goto out;
	}

	vsession->max_queues = 0;
	memset(vsession->virtqueue, 0, sizeof(vsession->virtqueue));
	for (i = 0; i < SPDK_VHOST_MAX_VQUEUES; i++) {
		struct spdk_vhost_virtqueue *q = &vsession->virtqueue[i];

		if (rte_vhost_get_vhost_vring(vid, i, &q->vring)) {
			continue;
		}

		if (q->vring.desc == NULL || q->vring.size == 0) {
			continue;
		}

		if (rte_vhost_get_vring_base(vsession->vid, i, &q->last_avail_idx, &q->last_used_idx)) {
			q->vring.desc = NULL;
			continue;
		}

		/* Disable notifications. */
		if (rte_vhost_enable_guest_notification(vid, i, 0) != 0) {
			SPDK_ERRLOG("vhost device %d: Failed to disable guest notification on queue %"PRIu16"\n", vid, i);
			goto out;
		}

		vsession->max_queues = i + 1;
	}

	if (rte_vhost_get_negotiated_features(vid, &vsession->negotiated_features) != 0) {
		SPDK_ERRLOG("vhost device %d: Failed to get negotiated driver features\n", vid);
		goto out;
	}

	if (rte_vhost_get_mem_table(vid, &vsession->mem) != 0) {
		SPDK_ERRLOG("vhost device %d: Failed to get guest memory table\n", vid);
		goto out;
	}

	for (i = 0; i < vsession->mem->nregions; i++) {
		uint64_t mmap_size = vsession->mem->regions[i].mmap_size;

		if (mmap_size & MASK_2MB) {
			SPDK_ERRLOG("vhost device %d: Guest mmaped memory size %" PRIx64
				    " is not a 2MB multiple\n", vid, mmap_size);
			free(vsession->mem);
			goto out;
		}
	}

	/*
	 * Not sure right now but this look like some kind of QEMU bug and guest IO
	 * might be frozed without kicking all queues after live-migration. This look like
	 * the previous vhost instance failed to effectively deliver all interrupts before
	 * the GET_VRING_BASE message. This shouldn't harm guest since spurious interrupts
	 * should be ignored by guest virtio driver.
	 *
	 * Tested on QEMU 2.10.91 and 2.11.50.
	 */
	for (i = 0; i < vsession->max_queues; i++) {
		if (vsession->virtqueue[i].vring.callfd != -1) {
			eventfd_write(vsession->virtqueue[i].vring.callfd, (eventfd_t)1);
		}
	}

	spdk_vhost_session_set_coalescing(vdev, vsession, NULL);
	spdk_vhost_session_mem_register(vsession);
	rc = vdev->backend->start_session(vsession);
	if (rc != 0) {
		spdk_vhost_session_mem_unregister(vsession);
		free(vsession->mem);
		goto out;
	}

out:
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	return rc;
}

#ifdef SPDK_CONFIG_VHOST_INTERNAL_LIB
static int
get_config(int vid, uint8_t *config, uint32_t len)
{
	struct spdk_vhost_session *vsession;
	struct spdk_vhost_dev *vdev;
	int rc = -1;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vsession = spdk_vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Couldn't find session with vid %d.\n", vid);
		goto out;
	}

	vdev = vsession->vdev;
	if (vdev->backend->vhost_get_config) {
		rc = vdev->backend->vhost_get_config(vdev, config, len);
	}

out:
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	return rc;
}

static int
set_config(int vid, uint8_t *config, uint32_t offset, uint32_t size, uint32_t flags)
{
	struct spdk_vhost_session *vsession;
	struct spdk_vhost_dev *vdev;
	int rc = -1;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vsession = spdk_vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Couldn't find session with vid %d.\n", vid);
		goto out;
	}

	vdev = vsession->vdev;
	if (vdev->backend->vhost_set_config) {
		rc = vdev->backend->vhost_set_config(vdev, config, offset, size, flags);
	}

out:
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	return rc;
}
#endif

int
spdk_vhost_set_socket_path(const char *basename)
{
	int ret;

	if (basename && strlen(basename) > 0) {
		ret = snprintf(dev_dirname, sizeof(dev_dirname) - 2, "%s", basename);
		if (ret <= 0) {
			return -EINVAL;
		}
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
	struct spdk_vhost_dev *vdev = NULL;

	TAILQ_FOREACH(vdev, &g_spdk_vhost_devices, tailq) {
		rte_vhost_driver_unregister(vdev->path);
		vdev->registered = false;
	}

	SPDK_INFOLOG(SPDK_LOG_VHOST, "Exiting\n");
	spdk_event_call((struct spdk_event *)arg);
	return NULL;
}

void
spdk_vhost_dump_info_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	assert(vdev->backend->dump_info_json != NULL);
	vdev->backend->dump_info_json(vdev, w);
}

int
spdk_vhost_dev_remove(struct spdk_vhost_dev *vdev)
{
	if (vdev->pending_async_op_num) {
		return -EBUSY;
	}

	return vdev->backend->remove_device(vdev);
}

static int
new_connection(int vid)
{
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_session *vsession;
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

	/* We expect sessions inside vdev->vsessions to be sorted in ascending
	 * order in regard of vsession->id. For now we always set id = vsessions_cnt++
	 * and append each session to the very end of the vsessions list.
	 * This is required for spdk_vhost_dev_foreach_session() to work.
	 */
	if (vdev->vsessions_num == UINT_MAX) {
		assert(false);
		return -EINVAL;
	}

	vsession = spdk_dma_zmalloc(sizeof(struct spdk_vhost_session) +
				    vdev->backend->session_ctx_size,
				    SPDK_CACHE_LINE_SIZE, NULL);
	if (vsession == NULL) {
		SPDK_ERRLOG("spdk_dma_zmalloc failed\n");
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		return -1;
	}

	vsession->vdev = vdev;
	vsession->id = vdev->vsessions_num++;
	vsession->vid = vid;
	vsession->lcore = -1;
	vsession->next_stats_check_time = 0;
	vsession->stats_check_interval = SPDK_VHOST_STATS_CHECK_INTERVAL_MS *
					 spdk_get_ticks_hz() / 1000UL;
	TAILQ_INSERT_TAIL(&vdev->vsessions, vsession, tailq);

	spdk_vhost_session_install_rte_compat_hooks(vsession);
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	return 0;
}

static void
destroy_connection(int vid)
{
	struct spdk_vhost_session *vsession;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vsession = spdk_vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Couldn't find session with vid %d.\n", vid);
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		return;
	}

	if (vsession->lcore != -1) {
		_stop_session(vsession);
	}

	TAILQ_REMOVE(&vsession->vdev->vsessions, vsession, tailq);
	spdk_dma_free(vsession);
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
}

static void
spdk_vhost_external_event_foreach_continue(struct spdk_vhost_dev *vdev,
		struct spdk_vhost_session *vsession,
		spdk_vhost_session_fn fn, void *arg)
{
	int rc;

	if (vsession == NULL) {
		goto out_finish_foreach;
	}

	while (vsession->lcore == -1) {
		rc = fn(vdev, vsession, arg);
		if (rc < 0) {
			return;
		}
		vsession = spdk_vhost_session_next(vdev, vsession->id);
		if (vsession == NULL) {
			goto out_finish_foreach;
		}
	}

	spdk_vhost_event_async_send_foreach_continue(vsession, fn, arg);
	return;

out_finish_foreach:
	/* there are no more sessions to iterate through, so call the
	 * fn one last time with vsession == NULL
	 */
	assert(vdev->pending_async_op_num > 0);
	vdev->pending_async_op_num--;
	fn(vdev, NULL, arg);
}

void
spdk_vhost_dev_foreach_session(struct spdk_vhost_dev *vdev,
			       spdk_vhost_session_fn fn, void *arg)
{
	struct spdk_vhost_session *vsession = TAILQ_FIRST(&vdev->vsessions);

	assert(vdev->pending_async_op_num < UINT32_MAX);
	vdev->pending_async_op_num++;
	spdk_vhost_external_event_foreach_continue(vdev, vsession, fn, arg);
}

void
spdk_vhost_lock(void)
{
	pthread_mutex_lock(&g_spdk_vhost_mutex);
}

int
spdk_vhost_trylock(void)
{
	return -pthread_mutex_trylock(&g_spdk_vhost_mutex);
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
	size_t len;
	int ret;

	if (dev_dirname[0] == '\0') {
		if (getcwd(dev_dirname, sizeof(dev_dirname) - 1) == NULL) {
			SPDK_ERRLOG("getcwd failed (%d): %s\n", errno, spdk_strerror(errno));
			return -1;
		}

		len = strlen(dev_dirname);
		if (dev_dirname[len - 1] != '/') {
			dev_dirname[len] = '/';
			dev_dirname[len + 1] = '\0';
		}
	}

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

#ifdef SPDK_CONFIG_VHOST_INTERNAL_LIB
	ret = spdk_vhost_nvme_controller_construct();
	if (ret != 0) {
		SPDK_ERRLOG("Cannot construct vhost NVMe controllers\n");
		return -1;
	}
#endif

	return 0;
}

static void
_spdk_vhost_fini(void *arg1, void *arg2)
{
	spdk_vhost_fini_cb fini_cb = arg1;
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

	/* All devices are removed now. */
	free(g_num_ctrlrs);
	fini_cb();
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

void
spdk_vhost_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_vhost_dev *vdev;
	uint32_t delay_base_us;
	uint32_t iops_threshold;

	spdk_json_write_array_begin(w);

	spdk_vhost_lock();
	vdev = spdk_vhost_dev_next(NULL);
	while (vdev != NULL) {
		vdev->backend->write_config_json(vdev, w);

		spdk_vhost_get_coalescing(vdev, &delay_base_us, &iops_threshold);
		if (delay_base_us) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "method", "set_vhost_controller_coalescing");

			spdk_json_write_named_object_begin(w, "params");
			spdk_json_write_named_string(w, "ctrlr", vdev->name);
			spdk_json_write_named_uint32(w, "delay_base_us", delay_base_us);
			spdk_json_write_named_uint32(w, "iops_threshold", iops_threshold);
			spdk_json_write_object_end(w);

			spdk_json_write_object_end(w);
		}
		vdev = spdk_vhost_dev_next(vdev);
	}
	spdk_vhost_unlock();

	spdk_json_write_array_end(w);
}

SPDK_LOG_REGISTER_COMPONENT("vhost", SPDK_LOG_VHOST)
SPDK_LOG_REGISTER_COMPONENT("vhost_ring", SPDK_LOG_VHOST_RING)
