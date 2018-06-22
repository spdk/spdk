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

struct spdk_vhost_dev_fn_ctx {
	/** Pointer to the target obtained before enqueuing the event */
	struct spdk_vhost_tgt *vtgt;

	/** ID of the vtgt to send event to. */
	unsigned vtgt_id;

	/** Pointer to the device obtained before enqueuing the event */
	struct spdk_vhost_dev *vdev;

	/** ID of the device to send event to. */
	unsigned vdev_id;

	/** User callback function to be executed on given lcore. */
	spdk_vhost_dev_fn cb_fn;

	bool foreach;
};

static void spdk_vhost_tgt_foreach_vdev_nolock(struct spdk_vhost_tgt *vtgt,
		spdk_vhost_dev_fn fn, void *arg);

const struct rte_vhost2_tgt_ops g_spdk_vhost_ops;

static TAILQ_HEAD(, spdk_vhost_tgt) g_spdk_vhost_tgts = TAILQ_HEAD_INITIALIZER(
			g_spdk_vhost_tgts);
static pthread_mutex_t g_spdk_vhost_mutex = PTHREAD_MUTEX_INITIALIZER;

void *
spdk_vhost_gpa_to_vva(struct spdk_vhost_dev *vdev,
		struct spdk_vhost_virtqueue *vq,
		uint64_t addr, uint32_t len)
{
	void *vva;
	uint32_t newlen;

	newlen = len;
	vva = rte_vhost2_iova_to_vva(vdev->rte_vdev, vq->rte_vq, addr,
			&newlen, VHOST_ACCESS_RW);
	if (newlen != len) {
		return NULL;
	}

	return vva;

}

static void
spdk_vhost_log_req_desc(struct spdk_vhost_dev *vdev, struct spdk_vhost_virtqueue *virtqueue,
			uint16_t req_id)
{
	/* FIXME */
}

static void
spdk_vhost_log_used_vring_elem(struct spdk_vhost_dev *vdev, struct spdk_vhost_virtqueue *virtqueue,
			       uint16_t idx)
{
	/* FIXME */
}

static void
spdk_vhost_log_used_vring_idx(struct spdk_vhost_dev *vdev, struct spdk_vhost_virtqueue *virtqueue)
{
	/* FIXME */
}

/*
 * Get available requests from avail ring.
 */
uint16_t
spdk_vhost_vq_avail_ring_get(struct spdk_vhost_virtqueue *virtqueue, uint16_t *reqs,
			     uint16_t reqs_len)
{
	struct rte_vhost2_vq *rte_vq = virtqueue->rte_vq;
	struct vring_avail *avail = rte_vq->vring.avail;
	uint16_t size_mask = rte_vq->vring.num - 1;
	uint16_t last_idx = rte_vq->last_avail_idx, avail_idx = avail->idx;
	uint16_t count, i;

	count = avail_idx - last_idx;
	if (spdk_likely(count == 0)) {
		return 0;
	}

	if (spdk_unlikely(count > rte_vq->vring.num)) {
		/* TODO: the queue is unrecoverably broken and should be marked so.
		 * For now we will fail silently and report there are no new avail entries.
		 */
		return 0;
	}

	count = spdk_min(count, reqs_len);
	rte_vq->last_avail_idx += count;
	for (i = 0; i < count; i++) {
		reqs[i] = avail->ring[(last_idx + i) & size_mask];
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
	if (spdk_unlikely(req_idx >= virtqueue->rte_vq->vring.num)) {
		return -1;
	}

	*desc = &virtqueue->rte_vq->vring.desc[req_idx];

	if (spdk_vhost_vring_desc_is_indirect(*desc)) {
		assert(spdk_vhost_dev_has_feature(vdev, VIRTIO_RING_F_INDIRECT_DESC));
		*desc_table_size = (*desc)->len / sizeof(**desc);
		*desc_table = spdk_vhost_gpa_to_vva(vdev, virtqueue, (*desc)->addr,
						    sizeof(**desc) * *desc_table_size);
		*desc = *desc_table;
		if (*desc == NULL) {
			return -1;
		}

		return 0;
	}

	*desc_table = virtqueue->rte_vq->vring.desc;
	*desc_table_size = virtqueue->rte_vq->vring.num;

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
		      virtqueue - vdev->virtqueue, virtqueue->rte_vq->last_used_idx);

	rte_vhost2_dev_call(vdev->rte_vdev, virtqueue->rte_vq);
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
	for (q_idx = 0; q_idx < SPDK_VHOST_MAX_VQUEUES; q_idx++) {
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
		for (q_idx = 0; q_idx < SPDK_VHOST_MAX_VQUEUES; q_idx++) {
			virtqueue = &vdev->virtqueue[q_idx];

			if (virtqueue->rte_vq == NULL ||
			    (virtqueue->rte_vq->vring.avail->flags & VRING_AVAIL_F_NO_INTERRUPT)) {
				continue;
			}

			spdk_vhost_vq_used_signal(vdev, virtqueue);
		}
	} else {
		now = spdk_get_ticks();
		check_dev_io_stats(vdev, now);

		for (q_idx = 0; q_idx < SPDK_VHOST_MAX_VQUEUES; q_idx++) {
			virtqueue = &vdev->virtqueue[q_idx];

			/* No need for event right now */
			if (now < virtqueue->next_event_time ||
			    (virtqueue->rte_vq->vring.avail->flags & VRING_AVAIL_F_NO_INTERRUPT)) {
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

static void
_set_coalescing_cb(struct spdk_vhost_tgt *vtgt, struct spdk_vhost_dev *vdev, void *arg)
{
	if (vtgt == NULL || vdev == NULL) {
		return;
	}

	vdev->coalescing_delay_time_base = vdev->vtgt->coalescing_delay_time_base;
	vdev->coalescing_io_rate_threshold = vdev->vtgt->coalescing_io_rate_threshold;
}

int
spdk_vhost_set_coalescing(struct spdk_vhost_tgt *vtgt, uint32_t delay_base_us,
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

	vtgt->coalescing_delay_time_base = delay_time_base;
	vtgt->coalescing_io_rate_threshold = io_rate;

	vtgt->coalescing_delay_us = delay_base_us;
	vtgt->coalescing_iops_threshold = iops_threshold;

	spdk_vhost_tgt_foreach_vdev_nolock(vtgt, _set_coalescing_cb, NULL);
	return 0;
}

void
spdk_vhost_get_coalescing(struct spdk_vhost_tgt *vtgt, uint32_t *delay_base_us,
			  uint32_t *iops_threshold)
{
	if (delay_base_us) {
		*delay_base_us = vtgt->coalescing_delay_us;
	}

	if (iops_threshold) {
		*iops_threshold = vtgt->coalescing_iops_threshold;
	}
}

/*
 * Enqueue id and len to used ring.
 */
void
spdk_vhost_vq_used_ring_enqueue(struct spdk_vhost_dev *vdev, struct spdk_vhost_virtqueue *virtqueue,
				uint16_t id, uint32_t len)
{
	struct rte_vhost2_vq *rte_vq = virtqueue->rte_vq;
	struct vring_used *used = rte_vq->vring.used;
	uint16_t last_idx = rte_vq->last_used_idx & (rte_vq->vring.num - 1);

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_RING,
		      "Queue %td - USED RING: last_idx=%"PRIu16" req id=%"PRIu16" len=%"PRIu32"\n",
		      virtqueue - vdev->virtqueue, rte_vq->last_used_idx, id, len);

	spdk_vhost_log_req_desc(vdev, virtqueue, id);

	rte_vq->last_used_idx++;
	used->ring[last_idx].id = id;
	used->ring[last_idx].len = len;

	/* Ensure the used ring is updated before we log it or increment used->idx. */
	spdk_smp_wmb();

	spdk_vhost_log_used_vring_elem(vdev, virtqueue, last_idx);
	* (volatile uint16_t *) &used->idx = rte_vq->last_used_idx;
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
spdk_vhost_vring_desc_to_iov(struct spdk_vhost_dev *vdev, struct spdk_vhost_virtqueue *vq, struct iovec *iov,
			     uint16_t *iov_index, const struct vring_desc *desc)
{
	uint32_t len = desc->len;
	uint32_t remaining = len;
	uint64_t payload = desc->addr;
	void *vva;

	while (remaining) {
 		if (*iov_index >= SPDK_VHOST_IOVS_MAX) {
 			SPDK_ERRLOG("SPDK_VHOST_IOVS_MAX(%d) reached\n", SPDK_VHOST_IOVS_MAX);
 			return -1;
  		}

		vva = rte_vhost2_iova_to_vva(vdev->rte_vdev, vq->rte_vq,
				payload, &len, VHOST_ACCESS_RW);
		if (vva == 0) {
			SPDK_ERRLOG("iova_to_vva(%p) == NULL\n", (void *)payload);
			return -1;
		}

		iov[*iov_index].iov_base = vva;
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
spdk_vhost_dev_find_by_id(struct spdk_vhost_tgt *vtgt, unsigned id)
{
	struct spdk_vhost_dev *vdev;

	TAILQ_FOREACH(vdev, &vtgt->vdevs, tailq) {
		if (vdev->id == id) {
			return vdev;
		}
	}

	return NULL;
}

static struct spdk_vhost_dev *
spdk_vhost_find_rte_dev(struct rte_vhost2_dev *rte_vdev)
{
	struct spdk_vhost_tgt *vtgt;
	struct spdk_vhost_dev *vdev;

	TAILQ_FOREACH(vtgt, &g_spdk_vhost_tgts, tailq) {
		TAILQ_FOREACH(vdev, &vtgt->vdevs, tailq) {
			if (vdev->rte_vdev == rte_vdev) {
				return vdev;
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
spdk_vhost_dev_mem_register(struct spdk_vhost_dev *vdev)
{
	struct rte_vhost2_mem_region *region;
	uint32_t i;

	for (i = 0; i < vdev->mem->nregions; i++) {
		uint64_t start, end, len;
		region = &vdev->mem->regions[i];
		start = FLOOR_2MB(region->host_user_addr);
		end = CEIL_2MB(region->host_user_addr + region->size);
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
spdk_vhost_dev_mem_unregister(struct spdk_vhost_dev *vdev)
{
	struct rte_vhost2_mem_region *region;
	uint32_t i;

	if (vdev->mem == NULL) {
		return;
	}

	for (i = 0; i < vdev->mem->nregions; i++) {
		uint64_t start, end, len;
		region = &vdev->mem->regions[i];
		start = FLOOR_2MB(region->host_user_addr);
		end = CEIL_2MB(region->host_user_addr + region->size);
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

int
spdk_vhost_tgt_register(struct spdk_vhost_tgt *vtgt, const char *name,
			const char *mask_str,
			const struct spdk_vhost_tgt_backend *backend,
			uint64_t features)
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

	if (rte_vhost2_tgt_register("vhost-user", path, 0, NULL, &g_spdk_vhost_ops,
		features) != 0) {
		SPDK_ERRLOG("Could not register controller %s with vhost library\n", name);
		SPDK_ERRLOG("Check if domain socket %s already exists\n", path);
		rc = -EIO;
		goto out;
	}

	vtgt->name = strdup(name);
	vtgt->path = strdup(path);
	assert(vtgt->name);
	assert(vtgt->path);
	vtgt->id = ctrlr_num++;
	vtgt->cpumask = cpumask;
	vtgt->backend = backend;

	spdk_vhost_set_coalescing(vtgt, SPDK_VHOST_COALESCING_DELAY_BASE_US,
				  SPDK_VHOST_VQ_IOPS_COALESCING_THRESHOLD);

	TAILQ_INIT(&vtgt->vdevs);
	TAILQ_INSERT_TAIL(&g_spdk_vhost_tgts, vtgt, tailq);

	SPDK_INFOLOG(SPDK_LOG_VHOST, "Controller %s: new controller added\n", vtgt->name);
	return 0;

out:
	spdk_cpuset_free(cpumask);
	return rc;
}

static void
_tgt_unregister_cb(void *arg)
{
	struct spdk_vhost_tgt *vtgt = arg;

	free(vtgt->name);
	free(vtgt->path);
	spdk_cpuset_free(vtgt->cpumask);
	TAILQ_REMOVE(&g_spdk_vhost_tgts, vtgt, tailq);

	SPDK_INFOLOG(SPDK_LOG_VHOST, "Controller %s: removed\n", vtgt->name);

	if (vtgt->unregister_cpl_fn) {
		vtgt->unregister_cpl_fn(vtgt);
	}
}

int
spdk_vhost_tgt_unregister(struct spdk_vhost_tgt *vtgt,
		void (*cpl_fn)(struct spdk_vhost_tgt *vtgt))
{
	if (!vtgt->force_removal && !TAILQ_EMPTY(&vtgt->vdevs)) {
		SPDK_ERRLOG("Target %s has still valid connections.\n", vtgt->name);
		return -ENODEV;
	}

	if (rte_vhost2_tgt_unregister("vhost_user", vtgt->path,
			_tgt_unregister_cb, vtgt) != 0) {
		SPDK_ERRLOG("Could not unregister controller %s with vhost library\n"
			    "Check if domain socket %s still exists\n",
			    vtgt->name, vtgt->path);
		return -EIO;
	}

	return 0;
}

static struct spdk_vhost_dev *
spdk_vhost_dev_next(struct spdk_vhost_tgt *vtgt, unsigned i)
{
	struct spdk_vhost_dev *vdev;

	if (vtgt == NULL) {
		return NULL;
	}

	TAILQ_FOREACH(vdev, &vtgt->vdevs, tailq) {
		if (vdev->id > i) {
			return vdev;
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
spdk_vhost_dev_set_op_cpl_fn(struct spdk_vhost_dev *vdev,
		void (*op_cpl_cb)(struct spdk_vhost_dev *vdev, void *arg),
		void *op_cpl_ctx)
{
	assert(vdev->op_cpl_cb == NULL);
	vdev->op_cpl_cb = op_cpl_cb;
	vdev->op_cpl_ctx = op_cpl_ctx;
}

void
spdk_vhost_dev_backend_event_done(struct spdk_vhost_dev *vdev, int rc)
{
	if (vdev->op_cpl_cb) {
		vdev->op_cpl_cb(vdev, vdev->op_cpl_ctx);
		vdev->op_cpl_cb = NULL;
	}
	rte_vhost2_dev_op_complete(vdev->rte_vdev, rc);
}

static void spdk_vhost_tgt_foreach_vdev_continue(struct spdk_vhost_tgt *vtgt,
		struct spdk_vhost_dev *vdev, spdk_vhost_dev_fn fn, void *arg);

static void
spdk_vhost_event_async_fn(void *arg1, void *arg2)
{
	struct spdk_vhost_dev_fn_ctx *ctx = arg1;
	struct spdk_vhost_tgt *vtgt;
	struct spdk_vhost_dev *vdev;
	struct spdk_event *ev;

	if (pthread_mutex_trylock(&g_spdk_vhost_mutex) != 0) {
		ev = spdk_event_allocate(spdk_env_get_current_core(),
					 spdk_vhost_event_async_fn, arg1, arg2);
		spdk_event_call(ev);
		return;
	}

	/* Check if our target didn't go down. */
	vtgt = spdk_vhost_tgt_find_by_id(ctx->vtgt_id);
	if (vtgt == ctx->vtgt) {
		/* Check if our device didn't go down. */
		vdev = spdk_vhost_dev_find_by_id(vtgt, ctx->vdev_id);
		if (vdev == ctx->vdev) {
			ctx->cb_fn(ctx->vtgt, ctx->vdev, arg2);
		} else {
			/* ctx->vdev is probably a dangling pointer at this
			 * point. It must have been removed in the meantime,
			 * so we just skip it.
			 */
		}
	} else {
		/* ctx->vtgt is probably a dangling pointer at this point.
		 * It must have been removed in the meantime.
		 */
		vtgt = NULL;
		vdev = NULL;
	}

	if (ctx->foreach) {
		vdev = spdk_vhost_dev_next(vtgt, ctx->vdev_id);
		spdk_vhost_tgt_foreach_vdev_continue(vtgt, vdev, ctx->cb_fn, arg2);
	} else if (vdev == NULL) {
		ctx->cb_fn(NULL, NULL, arg2);;
	}

	pthread_mutex_unlock(&g_spdk_vhost_mutex);

	free(ctx);
}

static int
spdk_vhost_event_async_send(struct spdk_vhost_dev *vdev,
		spdk_vhost_dev_fn cb_fn, void *arg,
		bool foreach)
{
	struct spdk_vhost_dev_fn_ctx *ev_ctx;
	struct spdk_event *ev;

	ev_ctx = calloc(1, sizeof(*ev_ctx));
	if (ev_ctx == NULL) {
		SPDK_ERRLOG("Failed to alloc vhost event.\n");
		return -ENOMEM;
	}

	ev_ctx->vtgt = vdev->vtgt;
	ev_ctx->vtgt_id = vdev->vtgt->id;
	ev_ctx->vdev = vdev;
	ev_ctx->vdev_id = vdev->id;
	ev_ctx->cb_fn = cb_fn;
	ev_ctx->foreach = foreach;

	ev = spdk_event_allocate(vdev->lcore, spdk_vhost_event_async_fn,
				 ev_ctx, arg);
	spdk_event_call(ev);

	return 0;
}

static void
device_init(struct rte_vhost2_dev *rte_vdev)
{
	struct spdk_vhost_dev *vdev;
	int i, rc = -1;

	pthread_mutex_lock(&g_spdk_vhost_mutex);

	vdev = spdk_vhost_find_rte_dev(rte_vdev);
	if (vdev == NULL) {
		SPDK_ERRLOG("Device %p doesn't exist.\n", rte_vdev);
		assert(false);
		goto out;
	}

	for (i = 0; i < SPDK_VHOST_MAX_VQUEUES; i++) {
		assert(vdev->virtqueue[i].rte_vq == NULL);
	}

	spdk_vhost_dev_mem_unregister(vdev);
	free(vdev->mem);
	vdev->mem = malloc(sizeof(*vdev->mem) +
		sizeof(struct rte_vhost2_mem_region) * rte_vdev->mem->nregions);
	vdev->mem->nregions = rte_vdev->mem->nregions;
	memcpy(vdev->mem->regions, rte_vdev->mem->regions,
	       sizeof(struct rte_vhost2_mem_region) * rte_vdev->mem->nregions);
	spdk_vhost_dev_mem_register(vdev);
	rc = 0;
out:
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	rte_vhost2_dev_op_complete(rte_vdev, rc);
}

static int
get_config(struct rte_vhost2_dev *rte_vdev, uint8_t *config, uint32_t len)
{
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_tgt *vtgt;
	int rc = -1;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vdev = spdk_vhost_find_rte_dev(rte_vdev);
	if (vdev == NULL) {
		SPDK_ERRLOG("Device %p doesn't exist.\n", rte_vdev);
		assert(false);
		goto out;
	}

	vtgt = vdev->vtgt;
	assert(vtgt->backend->vhost_get_config);
	rc = vtgt->backend->vhost_get_config(vtgt, config, len);

out:
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	return rc;
}

static int
set_config(struct rte_vhost2_dev *rte_vdev, uint8_t *config, uint32_t offset,
	   uint32_t size, uint32_t flags)
{
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_tgt *vtgt;
	int rc = -1;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vdev = spdk_vhost_find_rte_dev(rte_vdev);
	if (vdev == NULL) {
		SPDK_ERRLOG("Device %p doesn't exist.\n", rte_vdev);
		assert(false);
		goto out;
	}

	vtgt = vdev->vtgt;
	assert(vtgt->backend->vhost_set_config);
	rc = vtgt->backend->vhost_set_config(vtgt, config, offset, size, flags);

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

void
spdk_vhost_dump_info_json(struct spdk_vhost_tgt *vtgt, struct spdk_json_write_ctx *w)
{
	assert(vtgt->backend->dump_info_json != NULL);
	vtgt->backend->dump_info_json(vtgt, w);
}

int
spdk_vhost_tgt_remove(struct spdk_vhost_tgt *vtgt)
{
	return vtgt->backend->remove_device(vtgt);
}

static void
_device_create_cb_cpl(struct spdk_vhost_dev *vdev, void *ctx)
{
	struct spdk_vhost_tgt *vtgt = vdev->vtgt;

	TAILQ_INSERT_TAIL(&vtgt->vdevs, vdev, tailq);
}

static void
_device_create_cb(void *arg1, void *arg2)
{
	static unsigned ctrlr_num = 0;
	struct spdk_vhost_tgt *vtgt = arg1;
	struct rte_vhost2_dev *rte_vdev = arg2;
	struct spdk_vhost_dev *vdev;
	int rc = -1;

	/* rte_vhost2 assures the vtgt can't be removed while device
	 * creation/destruction or any other op is pending.
	 */

	pthread_mutex_lock(&g_spdk_vhost_mutex);

	/* We expect devices inside vtgt->vdevs to be sorted in ascending
	 * order in regard of vdev->id. For now we always set vdev->id = ctrlr_num++
	 * and append each vdev to the very end of vtgt->vdevs list.
	 * This is required for foreach vhost events to work.
	 */
	if (ctrlr_num == UINT_MAX) {
		assert(false);
		rc = -EINVAL;
		goto err;
	}

	vdev = spdk_vhost_find_rte_dev(rte_vdev);
	if (vdev) {
		SPDK_ERRLOG("%s: device %p already connected.\n", vtgt->name, vdev);
		assert(false);
		goto err;
	}

	vdev = spdk_dma_zmalloc(sizeof(struct spdk_vhost_dev) + vtgt->backend->dev_ctx_size,
				SPDK_CACHE_LINE_SIZE, NULL);
	if (vdev == NULL) {
		SPDK_ERRLOG("vdev calloc failed.\n");
		goto err;
	}

	vdev->rte_vdev = rte_vdev;
	vdev->vtgt = vtgt;
	vdev->id = ctrlr_num++;
	vdev->name = spdk_sprintf_alloc("%s_d%d", vtgt->name, vdev->id);
	if (vdev->name == NULL) {
		SPDK_ERRLOG("vdev name alloc failed.\n");
		spdk_dma_free(vdev);
		goto err;
	}
	vdev->lcore = spdk_env_get_current_core();
	vdev->coalescing_delay_time_base = vtgt->coalescing_delay_time_base;
	vdev->coalescing_io_rate_threshold = vtgt->coalescing_io_rate_threshold;
	vdev->next_stats_check_time = 0;
	vdev->stats_check_interval = SPDK_VHOST_DEV_STATS_CHECK_INTERVAL_MS * spdk_get_ticks_hz() /
				     1000UL;

	spdk_vhost_dev_set_op_cpl_fn(vdev, _device_create_cb_cpl, NULL);
	vtgt->backend->device_create(vdev);
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	return;
err:
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	rte_vhost2_dev_op_complete(rte_vdev, rc);
}

static void
device_create(struct rte_vhost2_dev *rte_vdev, const char *trtype,
	       const char *trid)
{
	struct spdk_vhost_tgt *vtgt;
	struct spdk_event *ev;
	uint32_t lcore;
	int rc = -1;

	pthread_mutex_lock(&g_spdk_vhost_mutex);

	if (strcmp(trtype, "vhost-user") != 0) {
		SPDK_ERRLOG("%s: unsupported vhost transport %s\n", trid, trtype);
		assert(false);
		rc = -EINVAL;
		goto err;
	}

	vtgt = spdk_vhost_tgt_find(trid);
	if (vtgt == NULL) {
		SPDK_ERRLOG("Couldn't find target `%s` to start device for.\n",
			    trid);
		assert(false);
		rc = -ENODEV;
		goto err;
	}

	lcore = spdk_vhost_allocate_reactor(vtgt->cpumask);
	ev = spdk_event_allocate(lcore, _device_create_cb, vtgt, rte_vdev);
	spdk_event_call(ev);

	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	return;

err:
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
	rte_vhost2_dev_op_complete(rte_vdev, rc);
}

static void
_destroy_device_cb_cpl_deferred(void *arg1, void *arg2)
{
	struct spdk_vhost_dev *vdev = arg1;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	TAILQ_REMOVE(&vdev->vtgt->vdevs, vdev, tailq);
	spdk_vhost_free_reactor(vdev->lcore);
	spdk_vhost_dev_mem_unregister(vdev);
	pthread_mutex_unlock(&g_spdk_vhost_mutex);

	free(vdev->mem);
	free(vdev->name);
	spdk_dma_free(vdev);
}

static void
_destroy_device_cb_cpl(struct spdk_vhost_dev *vdev, void *ctx)
{
	struct spdk_event *ev;

	/* We might be under the great vhost lock, so defer
	 * actual device removal to keep the code fairly simple.
	 * The device and target can't be removed while an
	 * rte_vhost2 op is pending, so we don't need any extra
	 * security checks here.
	 */
	ev = spdk_event_allocate(vdev->lcore, _destroy_device_cb_cpl_deferred,
				 vdev, NULL);
	spdk_event_call(ev);
}

static void
_device_destroy_cb(struct spdk_vhost_tgt *vtgt, struct spdk_vhost_dev *vdev, void *arg)
{
	if (vdev == NULL) {
		return;
	}

	spdk_vhost_dev_set_op_cpl_fn(vdev, _destroy_device_cb_cpl, NULL);
	vtgt->backend->device_destroy(vdev);
}

static void
destroy_device(struct rte_vhost2_dev *rte_vdev)
{
	struct spdk_vhost_dev *vdev;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vdev = spdk_vhost_find_rte_dev(rte_vdev);
	if (vdev == NULL) {
		SPDK_ERRLOG("Couldn't find device %p to destroy.\n", rte_vdev);
		assert(false);
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		rte_vhost2_dev_op_complete(rte_vdev, -ENODEV);
		return;
	}

	spdk_vhost_event_async_send(vdev, _device_destroy_cb, NULL, false);
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
}

static void
_device_features_changed_cb_cpl(struct spdk_vhost_dev *vdev, void *ctx)
{
	vdev->negotiated_features = (uint64_t)(uintptr_t) ctx;
}

static void
_device_features_changed_cb(struct spdk_vhost_tgt *vtgt, struct spdk_vhost_dev *vdev, void *arg)
{
	uint64_t features = (uint64_t)(uintptr_t)arg;

	spdk_vhost_dev_set_op_cpl_fn(vdev, _device_features_changed_cb_cpl, arg);
	vtgt->backend->device_features_changed(vdev, features);
}

static void
device_features_changed(struct rte_vhost2_dev *rte_vdev, uint64_t features)
{
	struct spdk_vhost_dev *vdev;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vdev = spdk_vhost_find_rte_dev(rte_vdev);
	if (vdev == NULL) {
		SPDK_ERRLOG("Couldn't find device %p to destroy.\n", rte_vdev);
		assert(false);
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		rte_vhost2_dev_op_complete(rte_vdev, -ENODEV);
		return;
	}

	spdk_vhost_event_async_send(vdev, _device_features_changed_cb,
			(void *)(uintptr_t)features, false);
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
}

static void
_queue_start_cb(struct spdk_vhost_tgt *vtgt, struct spdk_vhost_dev *vdev, void *arg)
{
	struct rte_vhost2_vq *rte_vq = arg;
	struct spdk_vhost_virtqueue *vq;

	assert(rte_vq->idx < SPDK_VHOST_MAX_VQUEUES);
	vq = &vdev->virtqueue[rte_vq->idx];
	vq->rte_vq = rte_vq;

	spdk_vhost_dev_set_op_cpl_fn(vdev, NULL, NULL);
	vtgt->backend->start_queue(vdev, vq);
}

static void
queue_start(struct rte_vhost2_dev *rte_vdev, struct rte_vhost2_vq *rte_vq)
{
	struct spdk_vhost_dev *vdev;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vdev = spdk_vhost_find_rte_dev(rte_vdev);
	if (vdev == NULL) {
		SPDK_ERRLOG("Couldn't find device %p to destroy.\n", rte_vdev);
		assert(false);
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		rte_vhost2_dev_op_complete(rte_vdev, -ENODEV);
		return;
	}

	spdk_vhost_event_async_send(vdev, _queue_start_cb, rte_vq, false);
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
}

static void
_queue_stop_cb_cpl(struct spdk_vhost_dev *vdev, void *ctx)
{
	struct spdk_vhost_virtqueue *vq = ctx;

	vq->rte_vq = NULL;
}

static void
_queue_stop_cb(struct spdk_vhost_tgt *vtgt, struct spdk_vhost_dev *vdev, void *arg)
{
	struct rte_vhost2_vq *rte_vq = arg;
	struct spdk_vhost_virtqueue *vq;

	assert(rte_vq->idx < SPDK_VHOST_MAX_VQUEUES);
	vq = &vdev->virtqueue[rte_vq->idx];

	spdk_vhost_dev_set_op_cpl_fn(vdev, _queue_stop_cb_cpl, vq);
	vtgt->backend->stop_queue(vdev, vq);
}

static void
queue_stop(struct rte_vhost2_dev *rte_vdev, struct rte_vhost2_vq *rte_vq)
{
	struct spdk_vhost_dev *vdev;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vdev = spdk_vhost_find_rte_dev(rte_vdev);
	if (vdev == NULL) {
		SPDK_ERRLOG("Couldn't find device %p to destroy.\n", rte_vdev);
		assert(false);
		pthread_mutex_unlock(&g_spdk_vhost_mutex);
		rte_vhost2_dev_op_complete(rte_vdev, -ENODEV);
		return;
	}

	spdk_vhost_event_async_send(vdev, _queue_stop_cb, rte_vq, false);
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
}

void
spdk_vhost_call_external_event(const char *vtgt_name, spdk_vhost_event_fn fn, void *arg)
{
	struct spdk_vhost_tgt *vtgt;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	vtgt = spdk_vhost_tgt_find(vtgt_name);

	fn(vtgt, arg);
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
}

static void
spdk_vhost_tgt_foreach_vdev_continue(struct spdk_vhost_tgt *vtgt,
				     struct spdk_vhost_dev *vdev,
				     spdk_vhost_dev_fn fn, void *arg)
{
	if (vtgt == NULL || vdev == NULL) {
		/* the device we were supposed to iterate through now has
		 * disappeared (has been removed after enqueuing this event)
		 * and there are no other devices to iterate through.
		 */
		fn(vtgt, NULL, arg);
		return;
	}

	while (vdev->lcore == -1) {
		fn(vtgt, vdev, arg);
		vdev = spdk_vhost_dev_next(vtgt, vdev->id);
		if (vdev == NULL) {
			fn(vtgt, NULL, arg);
			return;
		}
	}

	spdk_vhost_event_async_send(vdev, fn, arg, true);
}

static void
spdk_vhost_tgt_foreach_vdev_nolock(struct spdk_vhost_tgt *vtgt,
				   spdk_vhost_dev_fn fn, void *arg)
{
	struct spdk_vhost_dev *vdev;

	vdev = TAILQ_FIRST(&vtgt->vdevs);
	spdk_vhost_tgt_foreach_vdev_continue(vtgt, vdev, fn, arg);
}

void
spdk_vhost_tgt_foreach_vdev(struct spdk_vhost_tgt *vtgt,
			    spdk_vhost_dev_fn fn, void *arg)
{
	pthread_mutex_lock(&g_spdk_vhost_mutex);
	spdk_vhost_tgt_foreach_vdev_nolock(vtgt, fn, arg);
	pthread_mutex_unlock(&g_spdk_vhost_mutex);
}

void
spdk_vhost_call_external_event_foreach(spdk_vhost_event_fn fn, void *arg)
{
	struct spdk_vhost_tgt *vtgt, *vtgt_next;

	pthread_mutex_lock(&g_spdk_vhost_mutex);
	TAILQ_FOREACH_SAFE(vtgt, &g_spdk_vhost_tgts, tailq, vtgt_next) {
		fn(vtgt, arg);
	}
	fn(NULL, arg);
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

	return 0;
}

static int
_spdk_vhost_fini_remove_vtgt_cb(struct spdk_vhost_tgt *vtgt, void *arg)
{
	spdk_vhost_fini_cb fini_cb = arg;

	if (vtgt != NULL) {
		vtgt->force_removal = true;
		spdk_vhost_tgt_remove(vtgt);
		return 0;
	}

	/* All targets are removed now. */
	free(g_num_ctrlrs);
	fini_cb();
	return 0;
}

void
spdk_vhost_fini(spdk_vhost_fini_cb fini_cb)
{
	spdk_vhost_call_external_event_foreach(_spdk_vhost_fini_remove_vtgt_cb, fini_cb);
}

struct spdk_vhost_write_config_json_ctx {
	struct spdk_json_write_ctx *w;
	struct spdk_event *done_ev;
};

static int
spdk_vhost_config_json_cb(struct spdk_vhost_tgt *vtgt, void *arg)
{
	struct spdk_vhost_write_config_json_ctx *ctx = arg;
	uint32_t delay_base_us;
	uint32_t iops_threshold;

	if (vtgt == NULL) {
		spdk_json_write_array_end(ctx->w);
		spdk_event_call(ctx->done_ev);
		free(ctx);
		return 0;
	}

	vtgt->backend->write_config_json(vtgt, ctx->w);

	spdk_vhost_get_coalescing(vtgt, &delay_base_us, &iops_threshold);
	if (delay_base_us) {
		spdk_json_write_object_begin(ctx->w);
		spdk_json_write_named_string(ctx->w, "method", "set_vhost_controller_coalescing");

		spdk_json_write_named_object_begin(ctx->w, "params");
		spdk_json_write_named_string(ctx->w, "ctrlr", vtgt->name);
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

const struct rte_vhost2_tgt_ops g_spdk_vhost_ops = {
	.device_create = device_create,
	.device_init =  device_init,
	.device_features_changed = device_features_changed,
	.queue_start = queue_start,
	.queue_stop = queue_stop,
	.get_config = get_config,
	.set_config = set_config,
	.device_destroy = destroy_device,
};

SPDK_LOG_REGISTER_COMPONENT("vhost", SPDK_LOG_VHOST)
SPDK_LOG_REGISTER_COMPONENT("vhost_ring", SPDK_LOG_VHOST_RING)
