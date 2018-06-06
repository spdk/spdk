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
	/** Pointer to the controller obtained before enqueuing the event */
	struct spdk_vhost_dev *vdev;

	/** ID of the vdev to send event to. */
	unsigned vdev_id;

	/** User callback function to be executed on given lcore. */
	spdk_vhost_event_fn cb_fn;

	/** Semaphore used to signal that event is done. */
	sem_t sem;

	/** Response to be written by enqueued event. */
	int response;
};


#if 1
/* It is defined in "vhost.h" */
#define VHOST_ACCESS_RW 0x3
#endif

//FIXIT: by now only one device is supported
struct spdk_vhost_dev *g_vdev;


static void device_create(struct rte_vhost2_dev *vdev);
static void device_init(struct rte_vhost2_dev *vdev);
static void device_features_changed(struct rte_vhost2_dev *vdev,
		uint64_t features);
static void queue_start(struct rte_vhost2_dev *vdev, struct rte_vhost2_vq *vq);
static void queue_stop(struct rte_vhost2_dev *vdev, struct rte_vhost2_vq *vq);
static void device_destroy(struct rte_vhost2_dev *vdev);
static void custom_msg(struct rte_vhost2_dev *vdev, struct rte_vhost2_vq *vq,
		const char *id, void *ctx);
static void queue_kick(struct rte_vhost2_dev *vdev, struct rte_vhost2_vq *vq);
static int get_config(struct rte_vhost2_dev *vdev, uint8_t *config,
		uint32_t len);
static int set_config(struct rte_vhost2_dev *vdev, uint8_t *config,
		uint32_t offset, uint32_t len,
		enum rte_vhost2_set_config_type type);

const struct rte_vhost2_tgt_ops g_spdk_vhost_ops = {
	.device_create = device_create,
	.device_init = device_init,
	.device_features_changed = device_features_changed,
	.queue_start = queue_start,
	.queue_stop = queue_stop,
	.device_destroy = device_destroy,
	.custom_msg = custom_msg,
	.queue_kick = queue_kick,
	.get_config = get_config,
	.set_config = set_config,
};

static TAILQ_HEAD(, spdk_vhost_dev) g_spdk_vhost_devices = TAILQ_HEAD_INITIALIZER(
			g_spdk_vhost_devices);
static pthread_mutex_t g_spdk_vhost_mutex = PTHREAD_MUTEX_INITIALIZER;


static void device_features_changed(struct rte_vhost2_dev *vdev,
		uint64_t features) {
	printf("[] device_features_changed\n");
	rte_vhost2_dev_op_complete(vdev, 0);
}

static void custom_msg(struct rte_vhost2_dev *vdev, struct rte_vhost2_vq *vq,
		const char *id, void *ctx) {
	printf("[] custom_msg\n");
	rte_vhost2_dev_op_complete(vdev, 0);
}

static void queue_kick(struct rte_vhost2_dev *vdev, struct rte_vhost2_vq *vq) {
	printf("[] queue_kick\n");
	rte_vhost2_dev_op_complete(vdev, 0);
}



void *spdk_vhost_gpa_to_vva(struct spdk_vhost_dev *vdev, uint64_t addr, uint64_t len)
{
	void *vva;
	uint64_t newlen;

	newlen = len;

	printf("() spdk_vhost_gpa_to_vva()\n");

#if 0
	vva = (void *)rte_vhost2_iova_to_vva(vdev->, struct rte_vhost2_vq *vq,
			       uint64_t iova, uint32_t *len, uint8_t perm)

			       rte_vhost_va_from_guest_pa(vdev->mem, addr, &newlen);
#endif
	if (newlen != len) {
		return NULL;
	}

	return vva;

}

static void
spdk_vhost_log_req_desc(struct spdk_vhost_dev *vdev, struct spdk_vhost_virtqueue *virtqueue,
			uint16_t req_id)
{
	struct vring_desc *desc, *desc_table;
	uint32_t desc_table_size;
	int rc;

	return;

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

			//rte_vhost2_log_write(vdev->vid, desc->addr, desc->len);

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
	len = sizeof(virtqueue->vring.vring.used->ring[idx]);
	vq_idx = virtqueue - vdev->virtqueue;

	//rte_vhost_log_used_vring(vdev->vid, vq_idx, offset, len);
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
	len = sizeof(virtqueue->vring.vring.used->idx);
	vq_idx = virtqueue - vdev->virtqueue;

	//rte_vhost_log_used_vring(vdev->vid, vq_idx, offset, len);
}

/*
 * Get available requests from avail ring.
 */
uint16_t
spdk_vhost_vq_avail_ring_get(struct spdk_vhost_virtqueue *virtqueue, uint16_t *reqs,
			     uint16_t reqs_len)
{
	struct rte_vhost2_vq *vring = &virtqueue->vring;
	struct vring_avail *avail = vring->vring.avail;
	uint16_t size_mask = vring->vring.num - 1;
	uint16_t last_idx = vring->last_avail_idx, avail_idx = avail->idx;
	uint16_t count, i;

	count = avail_idx - last_idx;
	if (spdk_likely(count == 0)) {
		return 0;
	}

	if (spdk_unlikely(count > vring->vring.num)) {
		/* TODO: the queue is unrecoverably broken and should be marked so.
		 * For now we will fail silently and report there are no new avail entries.
		 */
		return 0;
	}

	count = spdk_min(count, reqs_len);
	vring->last_avail_idx += count;
	for (i = 0; i < count; i++) {
		reqs[i] = vring->vring.avail->ring[(last_idx + i) & size_mask];
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
	if (spdk_unlikely(req_idx >= virtqueue->vring.vring.num)) {
		return -1;
	}

	*desc = &virtqueue->vring.vring.desc[req_idx];

	if (spdk_vhost_vring_desc_is_indirect(*desc)) {
		assert(spdk_vhost_dev_has_feature(vdev, VIRTIO_RING_F_INDIRECT_DESC));
		*desc_table_size = (*desc)->len / sizeof(**desc);
		*desc_table = spdk_vhost_gpa_to_vva(vdev, (*desc)->addr,
						    sizeof(**desc) * *desc_table_size);
		*desc = *desc_table;
		if (*desc == NULL) {
			return -1;
		}

		return 0;
	}

	*desc_table = virtqueue->vring.vring.desc;
	*desc_table_size = virtqueue->vring.vring.num;

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

	rte_vhost2_dev_call(vdev->dev, &virtqueue->vring);
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
	for (q_idx = 0; q_idx < vdev->max_queues; q_idx++) {
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
		for (q_idx = 0; q_idx < vdev->max_queues; q_idx++) {
			virtqueue = &vdev->virtqueue[q_idx];

			if (virtqueue->vring.vring.desc == NULL ||
			    (virtqueue->vring.vring.avail->flags & VRING_AVAIL_F_NO_INTERRUPT)) {
				continue;
			}

			spdk_vhost_vq_used_signal(vdev, virtqueue);
		}
	} else {
		now = spdk_get_ticks();
		check_dev_io_stats(vdev, now);

		for (q_idx = 0; q_idx < vdev->max_queues; q_idx++) {
			virtqueue = &vdev->virtqueue[q_idx];

			/* No need for event right now */
			if (now < virtqueue->next_event_time ||
			    (virtqueue->vring.vring.avail->flags & VRING_AVAIL_F_NO_INTERRUPT)) {
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
spdk_vhost_set_coalescing(struct spdk_vhost_dev *vdev, uint32_t delay_base_us,
			  uint32_t iops_threshold)
{
	uint64_t delay_time_base = delay_base_us * spdk_get_ticks_hz() / 1000000ULL;
	uint32_t io_rate = iops_threshold * SPDK_VHOST_DEV_STATS_CHECK_INTERVAL_MS / 1000U;

	if (delay_time_base >= UINT32_MAX) {
		SPDK_ERRLOG("Delay time of %"PRIu32" is to big\n", delay_base_us);
		return -EINVAL;
	} else if (io_rate == 0) {
		SPDK_ERRLOG("IOPS rate of %"PRIu32" is too low. Min is %u\n", io_rate,
			    1000U / SPDK_VHOST_DEV_STATS_CHECK_INTERVAL_MS);
		return -EINVAL;
	}

	vdev->coalescing_delay_time_base = delay_time_base;
	vdev->coalescing_io_rate_threshold = io_rate;

	vdev->coalescing_delay_us = delay_base_us;
	vdev->coalescing_iops_threshold = iops_threshold;
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

/*
 * Enqueue id and len to used ring.
 */
void
spdk_vhost_vq_used_ring_enqueue(struct spdk_vhost_dev *vdev, struct spdk_vhost_virtqueue *virtqueue,
				uint16_t id, uint32_t len)
{
	struct rte_vhost2_vq *vring = &virtqueue->vring;
	struct vring_used *used = vring->vring.used;
	uint16_t last_idx = vring->last_used_idx & (vring->vring.num - 1);

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
	//FIXIT: Need to set vq!!!!
	struct rte_vhost2_vq *vq = NULL;


	uint32_t remaining = desc->len;
	uint32_t to_boundary;
	uint32_t len;
	uintptr_t payload = desc->addr;
	uintptr_t vva;

	printf("[] spdk_vhost_vring_desc_to_iov()\n");

	while (remaining) {
		if (*iov_index >= SPDK_VHOST_IOVS_MAX) {
			SPDK_ERRLOG("SPDK_VHOST_IOVS_MAX(%d) reached\n", SPDK_VHOST_IOVS_MAX);
			return -1;
		}
		//vva = (uintptr_t)rte_vhost2_gpa_to_vva(vdev->dev->mem, payload);

		vva = (uintptr_t)rte_vhost2_iova_to_vva(vdev->dev, vq,
				       payload, &remaining, VHOST_ACCESS_RW);

		if (vva == 0) {
			SPDK_ERRLOG("gpa_to_vva(%p) == NULL\n", (void *)payload);
			return -1;
		}

#if 1
		/* TODO: Use remaining here!!! */
		len = remaining;
#else
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
				if (vva + len != (uintptr_t)rte_vhost2_gpa_to_vva(vdev->dev->mem, payload + len)) {
					break;
				}
				len += spdk_min(remaining - len, 0x200000);
			}
		}
#endif
		iov[*iov_index].iov_base = (void *)vva;
		iov[*iov_index].iov_len = len;
		remaining -= len;
		payload += len;
		(*iov_index)++;
	}

	return 0;
}

static struct spdk_vhost_dev *
spdk_vhost_dev_find_by_id(unsigned id)
{
	struct spdk_vhost_dev *vdev;

	TAILQ_FOREACH(vdev, &g_spdk_vhost_devices, tailq) {
		if (vdev->id == id) {
			return vdev;
		}
	}

	return NULL;
}

#if 0
static struct spdk_vhost_dev *
spdk_vhost_dev_find_by_vid(int vid)
{
	struct spdk_vhost_dev *vdev;

	TAILQ_FOREACH(vdev, &g_spdk_vhost_devices, tailq) {
		if (vdev->vid == vid) {
			return vdev;
		}
	}

	return NULL;
}
#endif

static struct spdk_vhost_dev *
spdk_vhost_dev_find_by_dev(struct rte_vhost2_dev *vdev)
{
	struct spdk_vhost_dev *_vdev;

	TAILQ_FOREACH(_vdev, &g_spdk_vhost_devices, tailq) {
		if (_vdev->dev == vdev) {
			return _vdev;
		}
	}

	return NULL;
}

#define SHIFT_2MB	21
#define SIZE_2MB	(1ULL << SHIFT_2MB)
#define FLOOR_2MB(x)	(((uintptr_t)x) / SIZE_2MB) << SHIFT_2MB
#define CEIL_2MB(x)	((((uintptr_t)x) + SIZE_2MB - 1) / SIZE_2MB) << SHIFT_2MB

static void
spdk_vhost_dev_mem_register(struct spdk_vhost_dev *vdev)
{
	struct rte_vhost2_mem_region *region;
	uint32_t i;
	printf("[] %s\n", __FUNCTION__);
#if 0
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
#endif
}

static void
spdk_vhost_dev_mem_unregister(struct spdk_vhost_dev *vdev)
{
	struct rte_vhost2_mem_region *region;
	uint32_t i;
	printf("[] %s\n", __FUNCTION__);
#if 0
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
#endif
}

static void
spdk_vhost_free_reactor(uint32_t lcore)
{
	printf("[] %s\n", __FUNCTION__);
	g_num_ctrlrs[lcore]--;
}

struct spdk_vhost_dev *
spdk_vhost_dev_find(const char *ctrlr_name)
{
	struct spdk_vhost_dev *vdev;
	size_t dev_dirname_len = strlen(dev_dirname);

	printf("[] %s\n", __FUNCTION__);

	if (strncmp(ctrlr_name, dev_dirname, dev_dirname_len) == 0) {
		ctrlr_name += dev_dirname_len;
	}

	TAILQ_FOREACH(vdev, &g_spdk_vhost_devices, tailq) {
		if (strcmp(vdev->name, ctrlr_name) == 0) {
			printf("[] %s DONE\n", __FUNCTION__);
			return vdev;
		}
	}

	printf("[] %s FAILED\n", __FUNCTION__);

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

int
spdk_vhost_dev_register(struct spdk_vhost_dev *vdev, const char *name, const char *mask_str,
			const struct spdk_vhost_dev_backend *backend)
{
	static unsigned ctrlr_num;
	char path[PATH_MAX];
	struct stat file_stat;
	struct spdk_cpuset *cpumask;
	int rc;

	assert(vdev);

	printf("[] spdk_vhost_dev_register()\n");

	/* We expect devices inside g_spdk_vhost_devices to be sorted in ascending
	 * order in regard of vdev->id. For now we always set vdev->id = ctrlr_num++
	 * and append each vdev to the very end of g_spdk_vhost_devices list.
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

	if (rte_vhost2_tgt_register("vhost-user", path, 0, NULL, &g_spdk_vhost_ops,
			backend->virtio_features) != 0) {
		SPDK_ERRLOG("Could not register controller %s with vhost library\n", name);
		SPDK_ERRLOG("Check if domain socket %s already exists\n", path);
		rc = -EIO;
		goto out;
	}

	vdev->name = strdup(name);
	vdev->path = strdup(path);
	vdev->id = ctrlr_num++;
	vdev->vid = -1;
	vdev->lcore = -1;
	vdev->cpumask = cpumask;
	vdev->registered = true;
	vdev->backend = backend;

	spdk_vhost_set_coalescing(vdev, SPDK_VHOST_COALESCING_DELAY_BASE_US,
				  SPDK_VHOST_VQ_IOPS_COALESCING_THRESHOLD);
	vdev->next_stats_check_time = 0;
	vdev->stats_check_interval = SPDK_VHOST_DEV_STATS_CHECK_INTERVAL_MS * spdk_get_ticks_hz() /
				     1000UL;

	TAILQ_INSERT_TAIL(&g_spdk_vhost_devices, vdev, tailq);

	SPDK_INFOLOG(SPDK_LOG_VHOST, "Controller %s: new controller added\n", vdev->name);

	printf("[] %s [DONE]\n", __FUNCTION__);

	//FIXIT!
	g_vdev = vdev;

	return 0;

out:
	spdk_cpuset_free(cpumask);
	return rc;
}

int
spdk_vhost_dev_unregister(struct spdk_vhost_dev *vdev)
{
	if (vdev->vid != -1) {
		SPDK_ERRLOG("Controller %s has still valid connection.\n", vdev->name);
		return -ENODEV;
	}

	/* TODO: handle finish callback */
	if (vdev->registered &&
			rte_vhost2_tgt_unregister("vhost-user", vdev->path, NULL, NULL) != 0) {
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

static struct spdk_vhost_dev *
spdk_vhost_dev_next(unsigned i)
{
	struct spdk_vhost_dev *vdev;

	TAILQ_FOREACH(vdev, &g_spdk_vhost_devices, tailq) {
		if (vdev->id > i) {
			return vdev;
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

	vdev = spdk_vhost_dev_find_by_id(ctx->vdev_id);
	if (vdev != ctx->vdev) {
		/* vdev has been changed after enqueuing this event */
		vdev = NULL;
	}

	ctx->cb_fn(vdev, arg2);
	pthread_mutex_unlock(&g_spdk_vhost_mutex);

	free(ctx);
}

static void spdk_vhost_external_event_foreach_continue(struct spdk_vhost_dev *vdev,
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

	vdev = spdk_vhost_dev_find_by_id(ctx->vdev_id);
	if (vdev == ctx->vdev) {
		ctx->cb_fn(vdev, arg2);
	} else {
		/* ctx->vdev is probably a dangling pointer at this point.
		 * It must have been removed in the meantime, so we just skip
		 * it in our foreach chain. */
	}

	vdev = spdk_vhost_dev_next(ctx->vdev_id);
	spdk_vhost_external_event_foreach_continue(vdev, ctx->cb_fn, arg2);
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
spdk_vhost_event_async_send(struct spdk_vhost_dev *vdev, spdk_vhost_event_fn cb_fn, void *arg,
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

	ev_ctx->vdev = vdev;
	ev_ctx->vdev_id = vdev->id;
	ev_ctx->cb_fn = cb_fn;

	fn = foreach ? spdk_vhost_event_async_foreach_fn : spdk_vhost_event_async_fn;
	ev = spdk_event_allocate(ev_ctx->vdev->lcore, fn, ev_ctx, arg);
	assert(ev);
	spdk_event_call(ev);

	return 0;
}

static void queue_stop(struct rte_vhost2_dev *vdev, struct rte_vhost2_vq *vq) {
	printf("[] Queue stop\n");
	rte_vhost2_dev_op_complete(vdev, 0);
}

#if 0
static void
stop_device(int vid)
{
	struct spdk_vhost_dev *vdev;
	struct rte_vhost2_vq *q;
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

	for (i = 0; i < vdev->max_queues; i++) {
		q = &vdev->virtqueue[i].vring;
		if (q->vring.desc == NULL) {
			continue;
		}
		rte_vhost2_set_vhost_vring_last_idx(vdev->vid, i, q->last_avail_idx, q->last_used_idx);
	}

	spdk_vhost_dev_mem_unregister(vdev);
	free(vdev->dev->mem);
	spdk_vhost_free_reactor(vdev->lcore);
	vdev->lcore = -1;
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
}
#endif

static void queue_start(struct rte_vhost2_dev *vdev, struct rte_vhost2_vq *vq) {

	printf("[] Queue start %d\n", vq->idx);

	int rc = 0;
	struct spdk_vhost_dev *_vdev;

	if (vq->vring.desc == NULL ||
	    vq->vring.num == 0) {
		return;
	}

	//FIXIT: need to sync this
	pthread_mutex_lock(&g_spdk_vhost_mutex);

	_vdev = spdk_vhost_dev_find_by_dev(vdev);
	//FIXIT: use queue pointers instead copy array
	memcpy(&_vdev->virtqueue[vq->idx].vring, vq, sizeof(struct rte_vhost2_vq));

	//FIXIT: This is a workaround!!!
	if (_vdev->max_queues <= vq->idx)
		_vdev->max_queues = vq->idx + 1;

#if 0
	/* FIXIT: Pass queue to the backend */
	rc = spdk_vhost_event_send(_vdev, _vdev->backend->start_queue, 3, "start queue");
	if (rc != 0) {
		//FIXIT: error handling
	}
#else
	_vdev->backend->start_queue(_vdev, vq);
#endif
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	rte_vhost2_dev_op_complete(vdev, rc);
}

#if 0
static int
start_device(int vid)
{
	struct spdk_vhost_dev *vdev;
	int rc = -1;
	uint16_t i;

	pthread_mutex_lock(&g_spdk_vhost_mutex);

	vdev = spdk_vhost_dev_find_by_vid(vid);
	if (vdev == NULL) {
		SPDK_ERRLOG("Controller with vid %d doesn't exist.\n", vid);
		goto out;
	}

	if (vdev->lcore != -1) {
		SPDK_ERRLOG("Controller %s already loaded.\n", vdev->name);
		goto out;
	}

	vdev->max_queues = 0;
	memset(vdev->virtqueue, 0, sizeof(vdev->virtqueue));
	for (i = 0; i < SPDK_VHOST_MAX_VQUEUES; i++) {
		if (rte_vhost2_get_vhost_vring(vid, i, &vdev->virtqueue[i].vring)) {
			continue;
		}

		if (vdev->virtqueue[i].vring.vring.desc == NULL ||
		    vdev->virtqueue[i].vring.vring.num == 0) {
			continue;
		}

		/* Disable notifications. */
		if (rte_vhost2_enable_guest_notification(vid, i, 0) != 0) {
			SPDK_ERRLOG("vhost device %d: Failed to disable guest notification on queue %"PRIu16"\n", vid, i);
			goto out;
		}

		vdev->max_queues = i + 1;
	}

	if (rte_vhost2_get_negotiated_features(vid, &vdev->negotiated_features) != 0) {
		SPDK_ERRLOG("vhost device %d: Failed to get negotiated driver features\n", vid);
		goto out;
	}

	if (rte_vhost2_get_mem_table(vid, &vdev->dev->mem) != 0) {
		SPDK_ERRLOG("vhost device %d: Failed to get guest memory table\n", vid);
		goto out;
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
	for (i = 0; i < vdev->max_queues; i++) {
		rte_vhost2_dev_call((struct rte_vhost2_dev *)NULL, &vdev->virtqueue[i].vring);
	}

	vdev->lcore = spdk_vhost_allocate_reactor(vdev->cpumask);
	spdk_vhost_dev_mem_register(vdev);
	rc = spdk_vhost_event_send(vdev, vdev->backend->start_device, 3, "start device");
	if (rc != 0) {
		spdk_vhost_dev_mem_unregister(vdev);
		free(vdev->dev->mem);
		spdk_vhost_free_reactor(vdev->lcore);
		vdev->lcore = -1;
	}

out:
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	return rc;
}
#endif

static int get_config(struct rte_vhost2_dev *vdev, uint8_t *config,
		uint32_t len) {
	int rc = -1;
	struct spdk_vhost_dev *_vdev;

	printf("[] Get config\n");

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	_vdev = spdk_vhost_dev_find_by_dev(vdev);
	if (_vdev == NULL) {
		/* FIXIT: error message! */
		SPDK_ERRLOG("Controller doesn't exist.\n");
		goto out;
	}

	if (_vdev->backend->vhost_get_config) {
		rc = _vdev->backend->vhost_get_config(_vdev, config, len);
	}

out:
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	rte_vhost2_dev_op_complete(vdev, rc);
	return rc;
}

static int set_config(struct rte_vhost2_dev *vdev, uint8_t *config,
		uint32_t offset, uint32_t len,
		enum rte_vhost2_set_config_type type) {
	int rc = -1;
	struct spdk_vhost_dev *_vdev;

	printf("[] Set config\n");

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	_vdev = spdk_vhost_dev_find_by_dev(vdev);
	if (_vdev == NULL) {
		SPDK_ERRLOG("Controller doesn't exist.\n");
		goto out;
	}

	if (_vdev->backend->vhost_set_config) {
		/* Size and flags */
		//rc = _vdev->backend->vhost_set_config(_vdev, config, offset, size, flags);
	}

out:
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	rte_vhost2_dev_op_complete(vdev, rc);
	return rc;
}

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
		rte_vhost2_tgt_unregister("vhost-user", vdev->path, NULL, NULL);
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
	return vdev->backend->remove_device(vdev);
}


static void device_create(struct rte_vhost2_dev *vdev) {
	struct spdk_vhost_dev *_vdev;
	int rc = 0;

	printf("[] Device create\n");

	//TODO: fixit (we're expecting only one device for now)
	g_vdev->dev = vdev;
	//

	pthread_mutex_lock(&g_spdk_vhost_mutex);

	_vdev = spdk_vhost_dev_find_by_dev(vdev);
	if (_vdev == NULL) {
		SPDK_ERRLOG("Couldn't find device to create connection for.\n");
		rc = -1;
		goto end;
	}

	_vdev->max_queues = 0;
	memset(_vdev->virtqueue, 0, sizeof(_vdev->virtqueue));

end:
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	rte_vhost2_dev_op_complete(vdev, rc);
}

static void device_init(struct rte_vhost2_dev *vdev) {

	struct spdk_vhost_dev *_vdev;
	int i;
	int rc;

	printf("[] Device init\n");

	/* NOTE: this call can be made multiple times! */

	/* Start device */
	_vdev = spdk_vhost_dev_find_by_dev(vdev);

#if 0
	if (rte_vhost2_get_mem_table(vid, &vdev->dev->mem) != 0) {
		SPDK_ERRLOG("vhost device %d: Failed to get guest memory table\n", vid);
		goto out;
	}
#endif

#if 0
	for (i = 0; i < SPDK_VHOST_MAX_VQUEUES; i++) {

		if (rte_vhost2_get_vhost_vring(vid, i, &vdev->virtqueue[i].vring)) {
			continue;
		}

		if (vdev->virtqueue[i].vring.vring.desc == NULL ||
		    vdev->virtqueue[i].vring.vring.num == 0) {
			continue;
		}

		/* Disable notifications. */
		if (rte_vhost2_enable_guest_notification(vid, i, 0) != 0) {
			SPDK_ERRLOG("vhost device %d: Failed to disable guest notification on queue %"PRIu16"\n", vid, i);
			goto out;
		}

		vdev->max_queues = i + 1;
	}
#endif
	_vdev->lcore = spdk_vhost_allocate_reactor(_vdev->cpumask);
	spdk_vhost_dev_mem_register(_vdev);

	rc = spdk_vhost_event_send(_vdev, _vdev->backend->start_device, 3, "start device");
	if (rc != 0) {
		spdk_vhost_dev_mem_unregister(_vdev);
		free(_vdev->dev->mem);
		spdk_vhost_free_reactor(_vdev->lcore);
		_vdev->lcore = -1;
	}

	printf(".  Device init Ok\n");

	rte_vhost2_dev_op_complete(vdev, 0);
}

static void device_destroy(struct rte_vhost2_dev *vdev) {
	struct spdk_vhost_dev *_vdev;

	pthread_mutex_lock(&g_spdk_vhost_mutex);

	_vdev = spdk_vhost_dev_find_by_dev(vdev);
	if (_vdev == NULL) {
		SPDK_ERRLOG("Couldn't find device to destroy connection for.\n");
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		return;
	}

	/* since pollers are not running it safe not to use spdk_event here */
	//_vdev->vid = -1;

	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	rte_vhost2_dev_op_complete(vdev, 0);
}

void
spdk_vhost_call_external_event(const char *ctrlr_name, spdk_vhost_event_fn fn, void *arg)
{
	struct spdk_vhost_dev *vdev;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vdev = spdk_vhost_dev_find(ctrlr_name);

	if (vdev == NULL) {
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		fn(NULL, arg);
		return;
	}

	if (vdev->lcore == -1) {
		fn(vdev, arg);
	} else {
		spdk_vhost_event_async_send(vdev, fn, arg, false);
	}

	pthread_mutex_unlock(&g_spdk_vhost_mutex);
}

static void
spdk_vhost_external_event_foreach_continue(struct spdk_vhost_dev *vdev,
		spdk_vhost_event_fn fn, void *arg)
{
	if (vdev == NULL) {
		fn(NULL, arg);
		return;
	}

	while (vdev->lcore == -1) {
		fn(vdev, arg);
		vdev = spdk_vhost_dev_next(vdev->id);
		if (vdev == NULL) {
			fn(NULL, arg);
			return;
		}
	}

	spdk_vhost_event_async_send(vdev, fn, arg, true);
}

void
spdk_vhost_call_external_event_foreach(spdk_vhost_event_fn fn, void *arg)
{
	struct spdk_vhost_dev *vdev;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vdev = TAILQ_FIRST(&g_spdk_vhost_devices);
	spdk_vhost_external_event_foreach_continue(vdev, fn, arg);
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
_spdk_vhost_fini_remove_vdev_cb(struct spdk_vhost_dev *vdev, void *arg)
{
	spdk_vhost_fini_cb fini_cb = arg;

	if (vdev != NULL) {
		spdk_vhost_dev_remove(vdev);
		return 0;
	}

	/* All devices are removed now. */
	free(g_num_ctrlrs);
	fini_cb();
	return 0;
}

static void
_spdk_vhost_fini(void *arg1, void *arg2)
{
	spdk_vhost_fini_cb fini_cb = arg1;

	spdk_vhost_call_external_event_foreach(_spdk_vhost_fini_remove_vdev_cb, fini_cb);
}

void
spdk_vhost_fini(spdk_vhost_fini_cb fini_cb)
{
	pthread_t tid;
	int rc;
	struct spdk_event *fini_ev;

	fini_ev = spdk_event_allocate(spdk_env_get_current_core(), _spdk_vhost_fini, fini_cb, NULL);

	//FIXIT: Update comments to the new rte_vhost2 library!
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

struct spdk_vhost_write_config_json_ctx {
	struct spdk_json_write_ctx *w;
	struct spdk_event *done_ev;
};

static int
spdk_vhost_config_json_cb(struct spdk_vhost_dev *vdev, void *arg)
{
	struct spdk_vhost_write_config_json_ctx *ctx = arg;
	uint32_t delay_base_us;
	uint32_t iops_threshold;

	if (vdev == NULL) {
		spdk_json_write_array_end(ctx->w);
		spdk_event_call(ctx->done_ev);
		free(ctx);
		return 0;
	}

	vdev->backend->write_config_json(vdev, ctx->w);

	spdk_vhost_get_coalescing(vdev, &delay_base_us, &iops_threshold);
	if (delay_base_us) {
		spdk_json_write_object_begin(ctx->w);
		spdk_json_write_named_string(ctx->w, "method", "set_vhost_controller_coalescing");

		spdk_json_write_named_object_begin(ctx->w, "params");
		spdk_json_write_named_string(ctx->w, "ctrlr", vdev->name);
		spdk_json_write_named_uint32(ctx->w, "delay_base_us", delay_base_us);
		spdk_json_write_named_uint32(ctx->w, "iops_threshold", iops_threshold);
		spdk_json_write_object_end(ctx->w);

		spdk_json_write_object_end(ctx->w);
	}

	return 0;
}

void
spdk_vhost_config_json(struct spdk_json_write_ctx *w, struct spdk_event *done_ev)
{
	struct spdk_vhost_write_config_json_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_event_call(done_ev);
		return;
	}

	ctx->w = w;
	ctx->done_ev = done_ev;

	spdk_json_write_array_begin(w);

	spdk_vhost_call_external_event_foreach(spdk_vhost_config_json_cb, ctx);
}

SPDK_LOG_REGISTER_COMPONENT("vhost", SPDK_LOG_VHOST)
SPDK_LOG_REGISTER_COMPONENT("vhost_ring", SPDK_LOG_VHOST_RING)
