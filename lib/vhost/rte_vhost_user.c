/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 Mellanox Technologies LTD. All rights reserved.
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
#include <rte_version.h>

#include "spdk_internal/vhost_user.h"

/* Path to folder where character device will be created. Can be set by user. */
static char g_vhost_user_dev_dirname[PATH_MAX] = "";

static struct spdk_thread *g_vhost_user_init_thread;

/**
 * DPDK calls our callbacks synchronously but the work those callbacks
 * perform needs to be async. Luckily, all DPDK callbacks are called on
 * a DPDK-internal pthread, so we'll just wait on a semaphore in there.
 */
static sem_t g_dpdk_sem;

/** Return code for the current DPDK callback */
static int g_dpdk_response;

struct vhost_session_fn_ctx {
	/** Device pointer obtained before enqueueing the event */
	struct spdk_vhost_dev *vdev;

	/** ID of the session to send event to. */
	uint32_t vsession_id;

	/** User provided function to be executed on session's thread. */
	spdk_vhost_session_fn cb_fn;

	/**
	 * User provided function to be called on the init thread
	 * after iterating through all sessions.
	 */
	spdk_vhost_dev_fn cpl_fn;

	/** Custom user context */
	void *user_ctx;
};

static int vhost_user_wait_for_session_stop(struct spdk_vhost_session *vsession,
		unsigned timeout_sec, const char *errmsg);

static void
__attribute__((constructor))
_vhost_user_sem_init(void)
{
	if (sem_init(&g_dpdk_sem, 0, 0) != 0) {
		SPDK_ERRLOG("Failed to initialize semaphore for rte_vhost pthread.\n");
		abort();
	}
}

static void
__attribute__((destructor))
_vhost_user_sem_destroy(void)
{
	sem_destroy(&g_dpdk_sem);
}

void *
vhost_gpa_to_vva(struct spdk_vhost_session *vsession, uint64_t addr, uint64_t len)
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
vhost_log_req_desc(struct spdk_vhost_session *vsession, struct spdk_vhost_virtqueue *virtqueue,
		   uint16_t req_id)
{
	struct vring_desc *desc, *desc_table;
	uint32_t desc_table_size;
	int rc;

	if (spdk_likely(!vhost_dev_has_feature(vsession, VHOST_F_LOG_ALL))) {
		return;
	}

	rc = vhost_vq_get_desc(vsession, virtqueue, req_id, &desc, &desc_table, &desc_table_size);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Can't log used ring descriptors!\n");
		return;
	}

	do {
		if (vhost_vring_desc_is_wr(desc)) {
			/* To be honest, only pages really touched should be logged, but
			 * doing so would require tracking those changes in each backed.
			 * Also backend most likely will touch all/most of those pages so
			 * for lets assume we touched all pages passed to as writeable buffers. */
			rte_vhost_log_write(vsession->vid, desc->addr, desc->len);
		}
		vhost_vring_desc_get_next(&desc, desc_table, desc_table_size);
	} while (desc);
}

static void
vhost_log_used_vring_elem(struct spdk_vhost_session *vsession,
			  struct spdk_vhost_virtqueue *virtqueue,
			  uint16_t idx)
{
	uint64_t offset, len;

	if (spdk_likely(!vhost_dev_has_feature(vsession, VHOST_F_LOG_ALL))) {
		return;
	}

	if (spdk_unlikely(virtqueue->packed.packed_ring)) {
		offset = idx * sizeof(struct vring_packed_desc);
		len = sizeof(struct vring_packed_desc);
	} else {
		offset = offsetof(struct vring_used, ring[idx]);
		len = sizeof(virtqueue->vring.used->ring[idx]);
	}

	rte_vhost_log_used_vring(vsession->vid, virtqueue->vring_idx, offset, len);
}

static void
vhost_log_used_vring_idx(struct spdk_vhost_session *vsession,
			 struct spdk_vhost_virtqueue *virtqueue)
{
	uint64_t offset, len;
	uint16_t vq_idx;

	if (spdk_likely(!vhost_dev_has_feature(vsession, VHOST_F_LOG_ALL))) {
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
vhost_vq_avail_ring_get(struct spdk_vhost_virtqueue *virtqueue, uint16_t *reqs,
			uint16_t reqs_len)
{
	struct rte_vhost_vring *vring = &virtqueue->vring;
	struct vring_avail *avail = vring->avail;
	uint16_t size_mask = vring->size - 1;
	uint16_t last_idx = virtqueue->last_avail_idx, avail_idx = avail->idx;
	uint16_t count, i;
	int rc;
	uint64_t u64_value;

	spdk_smp_rmb();

	if (virtqueue->vsession && spdk_unlikely(virtqueue->vsession->interrupt_mode)) {
		/* Read to clear vring's kickfd */
		rc = read(vring->kickfd, &u64_value, sizeof(u64_value));
		if (rc < 0) {
			SPDK_ERRLOG("failed to acknowledge kickfd: %s.\n", spdk_strerror(errno));
			return -errno;
		}
	}

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
	/* Check whether there are unprocessed reqs in vq, then kick vq manually */
	if (virtqueue->vsession && spdk_unlikely(virtqueue->vsession->interrupt_mode)) {
		/* If avail_idx is larger than virtqueue's last_avail_idx, then there is unprocessed reqs.
		 * avail_idx should get updated here from memory, in case of race condition with guest.
		 */
		avail_idx = * (volatile uint16_t *) &avail->idx;
		if (avail_idx > virtqueue->last_avail_idx) {
			/* Write to notify vring's kickfd */
			rc = write(vring->kickfd, &u64_value, sizeof(u64_value));
			if (rc < 0) {
				SPDK_ERRLOG("failed to kick vring: %s.\n", spdk_strerror(errno));
				return -errno;
			}
		}
	}

	for (i = 0; i < count; i++) {
		reqs[i] = vring->avail->ring[(last_idx + i) & size_mask];
	}

	SPDK_DEBUGLOG(vhost_ring,
		      "AVAIL: last_idx=%"PRIu16" avail_idx=%"PRIu16" count=%"PRIu16"\n",
		      last_idx, avail_idx, count);

	return count;
}

static bool
vhost_vring_desc_is_indirect(struct vring_desc *cur_desc)
{
	return !!(cur_desc->flags & VRING_DESC_F_INDIRECT);
}

static bool
vhost_vring_packed_desc_is_indirect(struct vring_packed_desc *cur_desc)
{
	return (cur_desc->flags & VRING_DESC_F_INDIRECT) != 0;
}

static bool
vhost_inflight_packed_desc_is_indirect(spdk_vhost_inflight_desc *cur_desc)
{
	return (cur_desc->flags & VRING_DESC_F_INDIRECT) != 0;
}

int
vhost_vq_get_desc(struct spdk_vhost_session *vsession, struct spdk_vhost_virtqueue *virtqueue,
		  uint16_t req_idx, struct vring_desc **desc, struct vring_desc **desc_table,
		  uint32_t *desc_table_size)
{
	if (spdk_unlikely(req_idx >= virtqueue->vring.size)) {
		return -1;
	}

	*desc = &virtqueue->vring.desc[req_idx];

	if (vhost_vring_desc_is_indirect(*desc)) {
		*desc_table_size = (*desc)->len / sizeof(**desc);
		*desc_table = vhost_gpa_to_vva(vsession, (*desc)->addr,
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

static bool
vhost_packed_desc_indirect_to_desc_table(struct spdk_vhost_session *vsession,
		uint64_t addr, uint32_t len,
		struct vring_packed_desc **desc_table,
		uint32_t *desc_table_size)
{
	*desc_table_size = len / sizeof(struct vring_packed_desc);

	*desc_table = vhost_gpa_to_vva(vsession, addr, len);
	if (spdk_unlikely(*desc_table == NULL)) {
		return false;
	}

	return true;
}

int
vhost_vq_get_desc_packed(struct spdk_vhost_session *vsession,
			 struct spdk_vhost_virtqueue *virtqueue,
			 uint16_t req_idx, struct vring_packed_desc **desc,
			 struct vring_packed_desc **desc_table, uint32_t *desc_table_size)
{
	*desc =  &virtqueue->vring.desc_packed[req_idx];

	/* In packed ring when the desc is non-indirect we get next desc
	 * by judging (desc->flag & VRING_DESC_F_NEXT) != 0. When the desc
	 * is indirect we get next desc by idx and desc_table_size. It's
	 * different from split ring.
	 */
	if (vhost_vring_packed_desc_is_indirect(*desc)) {
		if (!vhost_packed_desc_indirect_to_desc_table(vsession, (*desc)->addr, (*desc)->len,
				desc_table, desc_table_size)) {
			return -1;
		}

		*desc = *desc_table;
	} else {
		*desc_table = NULL;
		*desc_table_size  = 0;
	}

	return 0;
}

int
vhost_inflight_queue_get_desc(struct spdk_vhost_session *vsession,
			      spdk_vhost_inflight_desc *desc_array,
			      uint16_t req_idx, spdk_vhost_inflight_desc **desc,
			      struct vring_packed_desc  **desc_table, uint32_t *desc_table_size)
{
	*desc = &desc_array[req_idx];

	if (vhost_inflight_packed_desc_is_indirect(*desc)) {
		if (!vhost_packed_desc_indirect_to_desc_table(vsession, (*desc)->addr, (*desc)->len,
				desc_table, desc_table_size)) {
			return -1;
		}

		/* This desc is the inflight desc not the packed desc.
		 * When set the F_INDIRECT the table entry should be the packed desc
		 * so set the inflight desc NULL.
		 */
		*desc = NULL;
	} else {
		/* When not set the F_INDIRECT means there is no packed desc table */
		*desc_table = NULL;
		*desc_table_size = 0;
	}

	return 0;
}

int
vhost_vq_used_signal(struct spdk_vhost_session *vsession,
		     struct spdk_vhost_virtqueue *virtqueue)
{
	if (virtqueue->used_req_cnt == 0) {
		return 0;
	}

	SPDK_DEBUGLOG(vhost_ring,
		      "Queue %td - USED RING: sending IRQ: last used %"PRIu16"\n",
		      virtqueue - vsession->virtqueue, virtqueue->last_used_idx);

	if (rte_vhost_vring_call(vsession->vid, virtqueue->vring_idx) == 0) {
		/* interrupt signalled */
		virtqueue->req_cnt += virtqueue->used_req_cnt;
		virtqueue->used_req_cnt = 0;
		return 1;
	} else {
		/* interrupt not signalled */
		return 0;
	}
}

static void
session_vq_io_stats_update(struct spdk_vhost_session *vsession,
			   struct spdk_vhost_virtqueue *virtqueue, uint64_t now)
{
	uint32_t irq_delay_base = vsession->coalescing_delay_time_base;
	uint32_t io_threshold = vsession->coalescing_io_rate_threshold;
	int32_t irq_delay;
	uint32_t req_cnt;

	req_cnt = virtqueue->req_cnt + virtqueue->used_req_cnt;
	if (req_cnt <= io_threshold) {
		return;
	}

	irq_delay = (irq_delay_base * (req_cnt - io_threshold)) / io_threshold;
	virtqueue->irq_delay_time = (uint32_t) spdk_max(0, irq_delay);

	virtqueue->req_cnt = 0;
	virtqueue->next_event_time = now;
}

static void
check_session_vq_io_stats(struct spdk_vhost_session *vsession,
			  struct spdk_vhost_virtqueue *virtqueue, uint64_t now)
{
	if (now < vsession->next_stats_check_time) {
		return;
	}

	vsession->next_stats_check_time = now + vsession->stats_check_interval;
	session_vq_io_stats_update(vsession, virtqueue, now);
}

static inline bool
vhost_vq_event_is_suppressed(struct spdk_vhost_virtqueue *vq)
{
	if (spdk_unlikely(vq->packed.packed_ring)) {
		if (vq->vring.driver_event->flags & VRING_PACKED_EVENT_FLAG_DISABLE) {
			return true;
		}
	} else {
		if (vq->vring.avail->flags & VRING_AVAIL_F_NO_INTERRUPT) {
			return true;
		}
	}

	return false;
}

void
vhost_session_vq_used_signal(struct spdk_vhost_virtqueue *virtqueue)
{
	struct spdk_vhost_session *vsession = virtqueue->vsession;
	uint64_t now;

	if (vsession->coalescing_delay_time_base == 0) {
		if (virtqueue->vring.desc == NULL) {
			return;
		}

		if (vhost_vq_event_is_suppressed(virtqueue)) {
			return;
		}

		vhost_vq_used_signal(vsession, virtqueue);
	} else {
		now = spdk_get_ticks();
		check_session_vq_io_stats(vsession, virtqueue, now);

		/* No need for event right now */
		if (now < virtqueue->next_event_time) {
			return;
		}

		if (vhost_vq_event_is_suppressed(virtqueue)) {
			return;
		}

		if (!vhost_vq_used_signal(vsession, virtqueue)) {
			return;
		}

		/* Syscall is quite long so update time */
		now = spdk_get_ticks();
		virtqueue->next_event_time = now + virtqueue->irq_delay_time;
	}
}

/*
 * Enqueue id and len to used ring.
 */
void
vhost_vq_used_ring_enqueue(struct spdk_vhost_session *vsession,
			   struct spdk_vhost_virtqueue *virtqueue,
			   uint16_t id, uint32_t len)
{
	struct rte_vhost_vring *vring = &virtqueue->vring;
	struct vring_used *used = vring->used;
	uint16_t last_idx = virtqueue->last_used_idx & (vring->size - 1);
	uint16_t vq_idx = virtqueue->vring_idx;

	SPDK_DEBUGLOG(vhost_ring,
		      "Queue %td - USED RING: last_idx=%"PRIu16" req id=%"PRIu16" len=%"PRIu32"\n",
		      virtqueue - vsession->virtqueue, virtqueue->last_used_idx, id, len);

	vhost_log_req_desc(vsession, virtqueue, id);

	virtqueue->last_used_idx++;
	used->ring[last_idx].id = id;
	used->ring[last_idx].len = len;

	/* Ensure the used ring is updated before we log it or increment used->idx. */
	spdk_smp_wmb();

	rte_vhost_set_last_inflight_io_split(vsession->vid, vq_idx, id);

	vhost_log_used_vring_elem(vsession, virtqueue, last_idx);
	* (volatile uint16_t *) &used->idx = virtqueue->last_used_idx;
	vhost_log_used_vring_idx(vsession, virtqueue);

	rte_vhost_clr_inflight_desc_split(vsession->vid, vq_idx, virtqueue->last_used_idx, id);

	virtqueue->used_req_cnt++;

	if (vsession->interrupt_mode) {
		if (virtqueue->vring.desc == NULL || vhost_vq_event_is_suppressed(virtqueue)) {
			return;
		}

		vhost_vq_used_signal(vsession, virtqueue);
	}
}

void
vhost_vq_packed_ring_enqueue(struct spdk_vhost_session *vsession,
			     struct spdk_vhost_virtqueue *virtqueue,
			     uint16_t num_descs, uint16_t buffer_id,
			     uint32_t length, uint16_t inflight_head)
{
	struct vring_packed_desc *desc = &virtqueue->vring.desc_packed[virtqueue->last_used_idx];
	bool used, avail;

	SPDK_DEBUGLOG(vhost_ring,
		      "Queue %td - RING: buffer_id=%"PRIu16"\n",
		      virtqueue - vsession->virtqueue, buffer_id);

	/* When the descriptor is used, two flags in descriptor
	 * avail flag and used flag are set to equal
	 * and used flag value == used_wrap_counter.
	 */
	used = !!(desc->flags & VRING_DESC_F_USED);
	avail = !!(desc->flags & VRING_DESC_F_AVAIL);
	if (spdk_unlikely(used == virtqueue->packed.used_phase && used == avail)) {
		SPDK_ERRLOG("descriptor has been used before\n");
		return;
	}

	/* In used desc addr is unused and len specifies the buffer length
	 * that has been written to by the device.
	 */
	desc->addr = 0;
	desc->len = length;

	/* This bit specifies whether any data has been written by the device */
	if (length != 0) {
		desc->flags |= VRING_DESC_F_WRITE;
	}

	/* Buffer ID is included in the last descriptor in the list.
	 * The driver needs to keep track of the size of the list corresponding
	 * to each buffer ID.
	 */
	desc->id = buffer_id;

	/* A device MUST NOT make the descriptor used before buffer_id is
	 * written to the descriptor.
	 */
	spdk_smp_wmb();

	rte_vhost_set_last_inflight_io_packed(vsession->vid, virtqueue->vring_idx, inflight_head);
	/* To mark a desc as used, the device sets the F_USED bit in flags to match
	 * the internal Device ring wrap counter. It also sets the F_AVAIL bit to
	 * match the same value.
	 */
	if (virtqueue->packed.used_phase) {
		desc->flags |= VRING_DESC_F_AVAIL_USED;
	} else {
		desc->flags &= ~VRING_DESC_F_AVAIL_USED;
	}
	rte_vhost_clr_inflight_desc_packed(vsession->vid, virtqueue->vring_idx, inflight_head);

	vhost_log_used_vring_elem(vsession, virtqueue, virtqueue->last_used_idx);
	virtqueue->last_used_idx += num_descs;
	if (virtqueue->last_used_idx >= virtqueue->vring.size) {
		virtqueue->last_used_idx -= virtqueue->vring.size;
		virtqueue->packed.used_phase = !virtqueue->packed.used_phase;
	}

	virtqueue->used_req_cnt++;
}

bool
vhost_vq_packed_ring_is_avail(struct spdk_vhost_virtqueue *virtqueue)
{
	uint16_t flags = virtqueue->vring.desc_packed[virtqueue->last_avail_idx].flags;

	/* To mark a desc as available, the driver sets the F_AVAIL bit in flags
	 * to match the internal avail wrap counter. It also sets the F_USED bit to
	 * match the inverse value but it's not mandatory.
	 */
	return (!!(flags & VRING_DESC_F_AVAIL) == virtqueue->packed.avail_phase);
}

bool
vhost_vring_packed_desc_is_wr(struct vring_packed_desc *cur_desc)
{
	return (cur_desc->flags & VRING_DESC_F_WRITE) != 0;
}

bool
vhost_vring_inflight_desc_is_wr(spdk_vhost_inflight_desc *cur_desc)
{
	return (cur_desc->flags & VRING_DESC_F_WRITE) != 0;
}

int
vhost_vring_packed_desc_get_next(struct vring_packed_desc **desc, uint16_t *req_idx,
				 struct spdk_vhost_virtqueue *vq,
				 struct vring_packed_desc *desc_table,
				 uint32_t desc_table_size)
{
	if (desc_table != NULL) {
		/* When the desc_table isn't NULL means it's indirect and we get the next
		 * desc by req_idx and desc_table_size. The return value is NULL means
		 * we reach the last desc of this request.
		 */
		(*req_idx)++;
		if (*req_idx < desc_table_size) {
			*desc = &desc_table[*req_idx];
		} else {
			*desc = NULL;
		}
	} else {
		/* When the desc_table is NULL means it's non-indirect and we get the next
		 * desc by req_idx and F_NEXT in flags. The return value is NULL means
		 * we reach the last desc of this request. When return new desc
		 * we update the req_idx too.
		 */
		if (((*desc)->flags & VRING_DESC_F_NEXT) == 0) {
			*desc = NULL;
			return 0;
		}

		*req_idx = (*req_idx + 1) % vq->vring.size;
		*desc = &vq->vring.desc_packed[*req_idx];
	}

	return 0;
}

static int
vhost_vring_desc_payload_to_iov(struct spdk_vhost_session *vsession, struct iovec *iov,
				uint16_t *iov_index, uintptr_t payload, uint64_t remaining)
{
	uintptr_t vva;
	uint64_t len;

	do {
		if (*iov_index >= SPDK_VHOST_IOVS_MAX) {
			SPDK_ERRLOG("SPDK_VHOST_IOVS_MAX(%d) reached\n", SPDK_VHOST_IOVS_MAX);
			return -1;
		}
		len = remaining;
		vva = (uintptr_t)rte_vhost_va_from_guest_pa(vsession->mem, payload, &len);
		if (vva == 0 || len == 0) {
			SPDK_ERRLOG("gpa_to_vva(%p) == NULL\n", (void *)payload);
			return -1;
		}
		iov[*iov_index].iov_base = (void *)vva;
		iov[*iov_index].iov_len = len;
		remaining -= len;
		payload += len;
		(*iov_index)++;
	} while (remaining);

	return 0;
}

int
vhost_vring_packed_desc_to_iov(struct spdk_vhost_session *vsession, struct iovec *iov,
			       uint16_t *iov_index, const struct vring_packed_desc *desc)
{
	return vhost_vring_desc_payload_to_iov(vsession, iov, iov_index,
					       desc->addr, desc->len);
}

int
vhost_vring_inflight_desc_to_iov(struct spdk_vhost_session *vsession, struct iovec *iov,
				 uint16_t *iov_index, const spdk_vhost_inflight_desc *desc)
{
	return vhost_vring_desc_payload_to_iov(vsession, iov, iov_index,
					       desc->addr, desc->len);
}

/* 1, Traverse the desc chain to get the buffer_id and return buffer_id as task_idx.
 * 2, Update the vq->last_avail_idx to point next available desc chain.
 * 3, Update the avail_wrap_counter if last_avail_idx overturn.
 */
uint16_t
vhost_vring_packed_desc_get_buffer_id(struct spdk_vhost_virtqueue *vq, uint16_t req_idx,
				      uint16_t *num_descs)
{
	struct vring_packed_desc *desc;
	uint16_t desc_head = req_idx;

	*num_descs = 1;

	desc =  &vq->vring.desc_packed[req_idx];
	if (!vhost_vring_packed_desc_is_indirect(desc)) {
		while ((desc->flags & VRING_DESC_F_NEXT) != 0) {
			req_idx = (req_idx + 1) % vq->vring.size;
			desc = &vq->vring.desc_packed[req_idx];
			(*num_descs)++;
		}
	}

	/* Queue Size doesn't have to be a power of 2
	 * Device maintains last_avail_idx so we can make sure
	 * the value is valid(0 ~ vring.size - 1)
	 */
	vq->last_avail_idx = (req_idx + 1) % vq->vring.size;
	if (vq->last_avail_idx < desc_head) {
		vq->packed.avail_phase = !vq->packed.avail_phase;
	}

	return desc->id;
}

int
vhost_vring_desc_get_next(struct vring_desc **desc,
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

int
vhost_vring_desc_to_iov(struct spdk_vhost_session *vsession, struct iovec *iov,
			uint16_t *iov_index, const struct vring_desc *desc)
{
	return vhost_vring_desc_payload_to_iov(vsession, iov, iov_index,
					       desc->addr, desc->len);
}

static inline void
vhost_session_mem_region_calc(uint64_t *previous_start, uint64_t *start, uint64_t *end,
			      uint64_t *len, struct rte_vhost_mem_region *region)
{
	*start = FLOOR_2MB(region->mmap_addr);
	*end = CEIL_2MB(region->mmap_addr + region->mmap_size);
	if (*start == *previous_start) {
		*start += (size_t) VALUE_2MB;
	}
	*previous_start = *start;
	*len = *end - *start;
}

void
vhost_session_mem_register(struct rte_vhost_memory *mem)
{
	uint64_t start, end, len;
	uint32_t i;
	uint64_t previous_start = UINT64_MAX;


	for (i = 0; i < mem->nregions; i++) {
		vhost_session_mem_region_calc(&previous_start, &start, &end, &len, &mem->regions[i]);
		SPDK_INFOLOG(vhost, "Registering VM memory for vtophys translation - 0x%jx len:0x%jx\n",
			     start, len);

		if (spdk_mem_register((void *)start, len) != 0) {
			SPDK_WARNLOG("Failed to register memory region %"PRIu32". Future vtophys translation might fail.\n",
				     i);
			continue;
		}
	}
}

void
vhost_session_mem_unregister(struct rte_vhost_memory *mem)
{
	uint64_t start, end, len;
	uint32_t i;
	uint64_t previous_start = UINT64_MAX;

	for (i = 0; i < mem->nregions; i++) {
		vhost_session_mem_region_calc(&previous_start, &start, &end, &len, &mem->regions[i]);
		if (spdk_vtophys((void *) start, NULL) == SPDK_VTOPHYS_ERROR) {
			continue; /* region has not been registered */
		}

		if (spdk_mem_unregister((void *)start, len) != 0) {
			assert(false);
		}
	}
}

static bool
vhost_memory_changed(struct rte_vhost_memory *new,
		     struct rte_vhost_memory *old)
{
	uint32_t i;

	if (new->nregions != old->nregions) {
		return true;
	}

	for (i = 0; i < new->nregions; ++i) {
		struct rte_vhost_mem_region *new_r = &new->regions[i];
		struct rte_vhost_mem_region *old_r = &old->regions[i];

		if (new_r->guest_phys_addr != old_r->guest_phys_addr) {
			return true;
		}
		if (new_r->size != old_r->size) {
			return true;
		}
		if (new_r->guest_user_addr != old_r->guest_user_addr) {
			return true;
		}
		if (new_r->mmap_addr != old_r->mmap_addr) {
			return true;
		}
		if (new_r->fd != old_r->fd) {
			return true;
		}
	}

	return false;
}

static int
vhost_register_memtable_if_required(struct spdk_vhost_session *vsession, int vid)
{
	struct rte_vhost_memory *new_mem;

	if (vhost_get_mem_table(vid, &new_mem) != 0) {
		SPDK_ERRLOG("vhost device %d: Failed to get guest memory table\n", vid);
		return -1;
	}

	if (vsession->mem == NULL) {
		SPDK_INFOLOG(vhost, "Start to set memtable\n");
		vsession->mem = new_mem;
		vhost_session_mem_register(vsession->mem);
		return 0;
	}

	if (vhost_memory_changed(new_mem, vsession->mem)) {
		SPDK_INFOLOG(vhost, "Memtable is changed\n");
		vhost_session_mem_unregister(vsession->mem);
		free(vsession->mem);

		vsession->mem = new_mem;
		vhost_session_mem_register(vsession->mem);
		return 0;

	}

	SPDK_INFOLOG(vhost, "Memtable is unchanged\n");
	free(new_mem);
	return 0;
}

static int
_stop_session(struct spdk_vhost_session *vsession)
{
	struct spdk_vhost_virtqueue *q;
	int rc;
	uint16_t i;

	rc = vhost_user_wait_for_session_stop(vsession, 3, "stop session");
	if (rc != 0) {
		SPDK_ERRLOG("Couldn't stop device with vid %d.\n", vsession->vid);
		return rc;
	}

	for (i = 0; i < vsession->max_queues; i++) {
		q = &vsession->virtqueue[i];

		/* vring.desc and vring.desc_packed are in a union struct
		 * so q->vring.desc can replace q->vring.desc_packed.
		 */
		if (q->vring.desc == NULL) {
			continue;
		}

		/* Packed virtqueues support up to 2^15 entries each
		 * so left one bit can be used as wrap counter.
		 */
		if (q->packed.packed_ring) {
			q->last_avail_idx = q->last_avail_idx |
					    ((uint16_t)q->packed.avail_phase << 15);
			q->last_used_idx = q->last_used_idx |
					   ((uint16_t)q->packed.used_phase << 15);
		}

		rte_vhost_set_vring_base(vsession->vid, i, q->last_avail_idx, q->last_used_idx);
		q->vring.desc = NULL;
	}
	vsession->max_queues = 0;

	return 0;
}

static int
new_connection(int vid)
{
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_user_dev *user_dev;
	struct spdk_vhost_session *vsession;
	size_t dev_dirname_len;
	char ifname[PATH_MAX];
	char *ctrlr_name;

	if (rte_vhost_get_ifname(vid, ifname, PATH_MAX) < 0) {
		SPDK_ERRLOG("Couldn't get a valid ifname for device with vid %d\n", vid);
		return -1;
	}

	ctrlr_name = &ifname[0];
	dev_dirname_len = strlen(g_vhost_user_dev_dirname);
	if (strncmp(ctrlr_name, g_vhost_user_dev_dirname, dev_dirname_len) == 0) {
		ctrlr_name += dev_dirname_len;
	}

	spdk_vhost_lock();
	vdev = spdk_vhost_dev_find(ctrlr_name);
	if (vdev == NULL) {
		SPDK_ERRLOG("Couldn't find device with vid %d to create connection for.\n", vid);
		spdk_vhost_unlock();
		return -1;
	}
	spdk_vhost_unlock();

	user_dev = to_user_dev(vdev);
	pthread_mutex_lock(&user_dev->lock);
	if (user_dev->registered == false) {
		SPDK_ERRLOG("Device %s is unregistered\n", ctrlr_name);
		pthread_mutex_unlock(&user_dev->lock);
		return -1;
	}

	/* We expect sessions inside user_dev->vsessions to be sorted in ascending
	 * order in regard of vsession->id. For now we always set id = vsessions_num++
	 * and append each session to the very end of the vsessions list.
	 * This is required for vhost_user_dev_foreach_session() to work.
	 */
	if (user_dev->vsessions_num == UINT_MAX) {
		pthread_mutex_unlock(&user_dev->lock);
		assert(false);
		return -EINVAL;
	}

	if (posix_memalign((void **)&vsession, SPDK_CACHE_LINE_SIZE, sizeof(*vsession) +
			   user_dev->user_backend->session_ctx_size)) {
		SPDK_ERRLOG("vsession alloc failed\n");
		pthread_mutex_unlock(&user_dev->lock);
		return -1;
	}
	memset(vsession, 0, sizeof(*vsession) + user_dev->user_backend->session_ctx_size);

	vsession->vdev = vdev;
	vsession->vid = vid;
	vsession->id = user_dev->vsessions_num++;
	vsession->name = spdk_sprintf_alloc("%ss%u", vdev->name, vsession->vid);
	if (vsession->name == NULL) {
		SPDK_ERRLOG("vsession alloc failed\n");
		free(vsession);
		pthread_mutex_unlock(&user_dev->lock);
		return -1;
	}
	vsession->started = false;
	vsession->next_stats_check_time = 0;
	vsession->stats_check_interval = SPDK_VHOST_STATS_CHECK_INTERVAL_MS *
					 spdk_get_ticks_hz() / 1000UL;
	TAILQ_INSERT_TAIL(&user_dev->vsessions, vsession, tailq);
	vhost_session_install_rte_compat_hooks(vsession);
	pthread_mutex_unlock(&user_dev->lock);

	return 0;
}

static void
vhost_user_session_start(void *arg1)
{
	struct spdk_vhost_session *vsession = arg1;
	struct spdk_vhost_dev *vdev = vsession->vdev;
	struct spdk_vhost_user_dev *user_dev = to_user_dev(vsession->vdev);
	const struct spdk_vhost_user_dev_backend *backend;
	int rc;

	pthread_mutex_lock(&user_dev->lock);
	backend = user_dev->user_backend;
	rc = backend->start_session(vdev, vsession, NULL);
	if (rc == 0) {
		vsession->started = true;
	}
	pthread_mutex_unlock(&user_dev->lock);
}

static int
set_device_vq_callfd(struct spdk_vhost_session *vsession, uint16_t qid)
{
	struct spdk_vhost_virtqueue *q;

	if (qid >= SPDK_VHOST_MAX_VQUEUES) {
		return -EINVAL;
	}

	q = &vsession->virtqueue[qid];
	/* vq isn't enabled yet */
	if (q->vring_idx != qid) {
		return 0;
	}

	/* vring.desc and vring.desc_packed are in a union struct
	 * so q->vring.desc can replace q->vring.desc_packed.
	 */
	if (q->vring.desc == NULL || q->vring.size == 0) {
		return 0;
	}

	/*
	 * Not sure right now but this look like some kind of QEMU bug and guest IO
	 * might be frozed without kicking all queues after live-migration. This look like
	 * the previous vhost instance failed to effectively deliver all interrupts before
	 * the GET_VRING_BASE message. This shouldn't harm guest since spurious interrupts
	 * should be ignored by guest virtio driver.
	 *
	 * Tested on QEMU 2.10.91 and 2.11.50.
	 *
	 * Make sure a successful call of
	 * `rte_vhost_vring_call` will happen
	 * after starting the device.
	 */
	q->used_req_cnt += 1;

	return 0;
}

static int
enable_device_vq(struct spdk_vhost_session *vsession, uint16_t qid)
{
	struct spdk_vhost_virtqueue *q;
	bool packed_ring;
	const struct spdk_vhost_user_dev_backend *backend;
	int rc;

	if (qid >= SPDK_VHOST_MAX_VQUEUES) {
		return -EINVAL;
	}

	q = &vsession->virtqueue[qid];
	memset(q, 0, sizeof(*q));
	packed_ring = ((vsession->negotiated_features & (1ULL << VIRTIO_F_RING_PACKED)) != 0);

	q->vsession = vsession;
	q->vring_idx = -1;
	if (rte_vhost_get_vhost_vring(vsession->vid, qid, &q->vring)) {
		return 0;
	}
	q->vring_idx = qid;
	rte_vhost_get_vhost_ring_inflight(vsession->vid, qid, &q->vring_inflight);

	/* vring.desc and vring.desc_packed are in a union struct
	 * so q->vring.desc can replace q->vring.desc_packed.
	 */
	if (q->vring.desc == NULL || q->vring.size == 0) {
		return 0;
	}

	if (rte_vhost_get_vring_base(vsession->vid, qid, &q->last_avail_idx, &q->last_used_idx)) {
		q->vring.desc = NULL;
		return 0;
	}

	backend = to_user_dev(vsession->vdev)->user_backend;
	rc = backend->alloc_vq_tasks(vsession, qid);
	if (rc) {
		return rc;
	}

	if (packed_ring) {
		/* Use the inflight mem to restore the last_avail_idx and last_used_idx.
		 * When the vring format is packed, there is no used_idx in the
		 * used ring, so VM can't resend the used_idx to VHOST when reconnect.
		 * QEMU version 5.2.0 supports the packed inflight before that it only
		 * supports split ring inflight because it doesn't send negotiated features
		 * before get inflight fd. Users can use RPC to enable this function.
		 */
		if (spdk_unlikely(vsession->vdev->packed_ring_recovery)) {
			rte_vhost_get_vring_base_from_inflight(vsession->vid, qid,
							       &q->last_avail_idx,
							       &q->last_used_idx);
		}

		/* Packed virtqueues support up to 2^15 entries each
		 * so left one bit can be used as wrap counter.
		 */
		q->packed.avail_phase = q->last_avail_idx >> 15;
		q->last_avail_idx = q->last_avail_idx & 0x7FFF;
		q->packed.used_phase = q->last_used_idx >> 15;
		q->last_used_idx = q->last_used_idx & 0x7FFF;

		if (!vsession->interrupt_mode) {
			/* Disable I/O submission notifications, we'll be polling. */
			q->vring.device_event->flags = VRING_PACKED_EVENT_FLAG_DISABLE;
		}
	} else {
		if (!vsession->interrupt_mode) {
			/* Disable I/O submission notifications, we'll be polling. */
			q->vring.used->flags = VRING_USED_F_NO_NOTIFY;
		}
	}

	q->packed.packed_ring = packed_ring;
	vsession->max_queues = spdk_max(vsession->max_queues, qid + 1);

	return 0;
}

static int
start_device(int vid)
{
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_session *vsession;
	struct spdk_vhost_user_dev *user_dev;
	int rc = 0;

	vsession = vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Couldn't find session with vid %d.\n", vid);
		return -1;
	}
	vdev = vsession->vdev;
	user_dev = to_user_dev(vdev);

	pthread_mutex_lock(&user_dev->lock);
	if (vsession->started) {
		/* already started, nothing to do */
		goto out;
	}

	if (!vsession->mem) {
		rc = -1;
		SPDK_ERRLOG("Session %s doesn't set memory table yet\n", vsession->name);
		goto out;
	}

	vhost_user_session_set_coalescing(vdev, vsession, NULL);
	spdk_thread_send_msg(vdev->thread, vhost_user_session_start, vsession);

out:
	pthread_mutex_unlock(&user_dev->lock);
	return rc;
}

static void
stop_device(int vid)
{
	struct spdk_vhost_session *vsession;
	struct spdk_vhost_user_dev *user_dev;

	vsession = vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Couldn't find session with vid %d.\n", vid);
		return;
	}
	user_dev = to_user_dev(vsession->vdev);

	pthread_mutex_lock(&user_dev->lock);
	if (!vsession->started) {
		pthread_mutex_unlock(&user_dev->lock);
		/* already stopped, nothing to do */
		return;
	}

	_stop_session(vsession);
	pthread_mutex_unlock(&user_dev->lock);
}

static void
destroy_connection(int vid)
{
	struct spdk_vhost_session *vsession;
	struct spdk_vhost_user_dev *user_dev;

	vsession = vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Couldn't find session with vid %d.\n", vid);
		return;
	}
	user_dev = to_user_dev(vsession->vdev);

	pthread_mutex_lock(&user_dev->lock);
	if (vsession->started) {
		if (_stop_session(vsession) != 0) {
			pthread_mutex_unlock(&user_dev->lock);
			return;
		}
	}

	if (vsession->mem) {
		vhost_session_mem_unregister(vsession->mem);
		free(vsession->mem);
	}

	TAILQ_REMOVE(&to_user_dev(vsession->vdev)->vsessions, vsession, tailq);
	free(vsession->name);
	free(vsession);
	pthread_mutex_unlock(&user_dev->lock);
}

#if RTE_VERSION >= RTE_VERSION_NUM(21, 11, 0, 0)
static const struct rte_vhost_device_ops g_spdk_vhost_ops = {
#else
static const struct vhost_device_ops g_spdk_vhost_ops = {
#endif
	.new_device =  start_device,
	.destroy_device = stop_device,
	.new_connection = new_connection,
	.destroy_connection = destroy_connection,
};

static struct spdk_vhost_session *
vhost_session_find_by_id(struct spdk_vhost_dev *vdev, unsigned id)
{
	struct spdk_vhost_session *vsession;

	TAILQ_FOREACH(vsession, &to_user_dev(vdev)->vsessions, tailq) {
		if (vsession->id == id) {
			return vsession;
		}
	}

	return NULL;
}

struct spdk_vhost_session *
vhost_session_find_by_vid(int vid)
{
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_session *vsession;
	struct spdk_vhost_user_dev *user_dev;

	spdk_vhost_lock();
	for (vdev = spdk_vhost_dev_next(NULL); vdev != NULL;
	     vdev = spdk_vhost_dev_next(vdev)) {
		user_dev = to_user_dev(vdev);

		pthread_mutex_lock(&user_dev->lock);
		TAILQ_FOREACH(vsession, &user_dev->vsessions, tailq) {
			if (vsession->vid == vid) {
				pthread_mutex_unlock(&user_dev->lock);
				spdk_vhost_unlock();
				return vsession;
			}
		}
		pthread_mutex_unlock(&user_dev->lock);
	}
	spdk_vhost_unlock();

	return NULL;
}

static void
wait_for_semaphore(int timeout_sec, const char *errmsg)
{
	struct timespec timeout;
	int rc;

	clock_gettime(CLOCK_REALTIME, &timeout);
	timeout.tv_sec += timeout_sec;
	rc = sem_timedwait(&g_dpdk_sem, &timeout);
	if (rc != 0) {
		SPDK_ERRLOG("Timeout waiting for event: %s.\n", errmsg);
		sem_wait(&g_dpdk_sem);
	}
}

void
vhost_user_session_stop_done(struct spdk_vhost_session *vsession, int response)
{
	if (response == 0) {
		vsession->started = false;
	}

	g_dpdk_response = response;
	sem_post(&g_dpdk_sem);
}

static void
vhost_user_session_stop_event(void *arg1)
{
	struct vhost_session_fn_ctx *ctx = arg1;
	struct spdk_vhost_dev *vdev = ctx->vdev;
	struct spdk_vhost_user_dev *user_dev = to_user_dev(vdev);
	struct spdk_vhost_session *vsession;

	if (pthread_mutex_trylock(&user_dev->lock) != 0) {
		spdk_thread_send_msg(spdk_get_thread(), vhost_user_session_stop_event, arg1);
		return;
	}

	vsession = vhost_session_find_by_id(vdev, ctx->vsession_id);
	user_dev->user_backend->stop_session(vdev, vsession, NULL);
	pthread_mutex_unlock(&user_dev->lock);
}

static int
vhost_user_wait_for_session_stop(struct spdk_vhost_session *vsession,
				 unsigned timeout_sec, const char *errmsg)
{
	struct vhost_session_fn_ctx ev_ctx = {0};
	struct spdk_vhost_dev *vdev = vsession->vdev;
	struct spdk_vhost_user_dev *user_dev = to_user_dev(vdev);

	ev_ctx.vdev = vdev;
	ev_ctx.vsession_id = vsession->id;

	spdk_thread_send_msg(vdev->thread, vhost_user_session_stop_event, &ev_ctx);

	pthread_mutex_unlock(&user_dev->lock);
	wait_for_semaphore(timeout_sec, errmsg);
	pthread_mutex_lock(&user_dev->lock);

	return g_dpdk_response;
}

static void
foreach_session_finish_cb(void *arg1)
{
	struct vhost_session_fn_ctx *ev_ctx = arg1;
	struct spdk_vhost_dev *vdev = ev_ctx->vdev;
	struct spdk_vhost_user_dev *user_dev = to_user_dev(vdev);

	if (pthread_mutex_trylock(&user_dev->lock) != 0) {
		spdk_thread_send_msg(spdk_get_thread(),
				     foreach_session_finish_cb, arg1);
		return;
	}

	assert(user_dev->pending_async_op_num > 0);
	user_dev->pending_async_op_num--;
	if (ev_ctx->cpl_fn != NULL) {
		ev_ctx->cpl_fn(vdev, ev_ctx->user_ctx);
	}

	pthread_mutex_unlock(&user_dev->lock);
	free(ev_ctx);
}

static void
foreach_session(void *arg1)
{
	struct vhost_session_fn_ctx *ev_ctx = arg1;
	struct spdk_vhost_dev *vdev = ev_ctx->vdev;
	struct spdk_vhost_user_dev *user_dev = to_user_dev(vdev);
	struct spdk_vhost_session *vsession;
	int rc;

	if (pthread_mutex_trylock(&user_dev->lock) != 0) {
		spdk_thread_send_msg(spdk_get_thread(), foreach_session, arg1);
		return;
	}

	TAILQ_FOREACH(vsession, &user_dev->vsessions, tailq) {
		rc = ev_ctx->cb_fn(vdev, vsession, ev_ctx->user_ctx);
		if (rc < 0) {
			goto out;
		}
	}

out:
	pthread_mutex_unlock(&user_dev->lock);
	spdk_thread_send_msg(g_vhost_user_init_thread, foreach_session_finish_cb, arg1);
}

void
vhost_user_dev_foreach_session(struct spdk_vhost_dev *vdev,
			       spdk_vhost_session_fn fn,
			       spdk_vhost_dev_fn cpl_fn,
			       void *arg)
{
	struct vhost_session_fn_ctx *ev_ctx;
	struct spdk_vhost_user_dev *user_dev = to_user_dev(vdev);

	ev_ctx = calloc(1, sizeof(*ev_ctx));
	if (ev_ctx == NULL) {
		SPDK_ERRLOG("Failed to alloc vhost event.\n");
		assert(false);
		return;
	}

	ev_ctx->vdev = vdev;
	ev_ctx->cb_fn = fn;
	ev_ctx->cpl_fn = cpl_fn;
	ev_ctx->user_ctx = arg;

	pthread_mutex_lock(&user_dev->lock);
	assert(user_dev->pending_async_op_num < UINT32_MAX);
	user_dev->pending_async_op_num++;
	pthread_mutex_unlock(&user_dev->lock);

	spdk_thread_send_msg(vdev->thread, foreach_session, ev_ctx);
}

void
vhost_user_session_set_interrupt_mode(struct spdk_vhost_session *vsession, bool interrupt_mode)
{
	uint16_t i;
	bool packed_ring;
	int rc = 0;

	packed_ring = ((vsession->negotiated_features & (1ULL << VIRTIO_F_RING_PACKED)) != 0);

	for (i = 0; i < vsession->max_queues; i++) {
		struct spdk_vhost_virtqueue *q = &vsession->virtqueue[i];
		uint64_t num_events = 1;

		/* vring.desc and vring.desc_packed are in a union struct
		 * so q->vring.desc can replace q->vring.desc_packed.
		 */
		if (q->vring.desc == NULL || q->vring.size == 0) {
			continue;
		}

		if (interrupt_mode) {
			/* Enable I/O submission notifications, we'll be interrupting. */
			if (packed_ring) {
				* (volatile uint16_t *) &q->vring.device_event->flags = VRING_PACKED_EVENT_FLAG_ENABLE;
			} else {
				* (volatile uint16_t *) &q->vring.used->flags = 0;
			}

			/* In case of race condition, always kick vring when switch to intr */
			rc = write(q->vring.kickfd, &num_events, sizeof(num_events));
			if (rc < 0) {
				SPDK_ERRLOG("failed to kick vring: %s.\n", spdk_strerror(errno));
			}

			vsession->interrupt_mode = true;
		} else {
			/* Disable I/O submission notifications, we'll be polling. */
			if (packed_ring) {
				* (volatile uint16_t *) &q->vring.device_event->flags = VRING_PACKED_EVENT_FLAG_DISABLE;
			} else {
				* (volatile uint16_t *) &q->vring.used->flags = VRING_USED_F_NO_NOTIFY;
			}

			vsession->interrupt_mode = false;
		}
	}
}


static int
extern_vhost_pre_msg_handler(int vid, void *_msg)
{
	struct vhost_user_msg *msg = _msg;
	struct spdk_vhost_session *vsession;
	struct spdk_vhost_user_dev *user_dev;

	vsession = vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Received a message to uninitialized session (vid %d).\n", vid);
		assert(false);
		return RTE_VHOST_MSG_RESULT_ERR;
	}
	user_dev = to_user_dev(vsession->vdev);

	switch (msg->request) {
	case VHOST_USER_GET_VRING_BASE:
		pthread_mutex_lock(&user_dev->lock);
		if (vsession->started) {
			pthread_mutex_unlock(&user_dev->lock);
			/* `stop_device` is running in synchronous, it
			 * will hold this lock again before exiting.
			 */
			g_spdk_vhost_ops.destroy_device(vid);
		}
		pthread_mutex_unlock(&user_dev->lock);
		break;
	case VHOST_USER_GET_CONFIG: {
		int rc = 0;

		pthread_mutex_lock(&user_dev->lock);
		if (vsession->vdev->backend->vhost_get_config) {
			rc = vsession->vdev->backend->vhost_get_config(vsession->vdev,
					msg->payload.cfg.region, msg->payload.cfg.size);
			if (rc != 0) {
				msg->size = 0;
			}
		}
		pthread_mutex_unlock(&user_dev->lock);

		return RTE_VHOST_MSG_RESULT_REPLY;
	}
	case VHOST_USER_SET_CONFIG: {
		int rc = 0;

		pthread_mutex_lock(&user_dev->lock);
		if (vsession->vdev->backend->vhost_set_config) {
			rc = vsession->vdev->backend->vhost_set_config(vsession->vdev,
					msg->payload.cfg.region, msg->payload.cfg.offset,
					msg->payload.cfg.size, msg->payload.cfg.flags);
		}
		pthread_mutex_unlock(&user_dev->lock);

		return rc == 0 ? RTE_VHOST_MSG_RESULT_OK : RTE_VHOST_MSG_RESULT_ERR;
	}
	default:
		break;
	}

	return RTE_VHOST_MSG_RESULT_NOT_HANDLED;
}

static int
extern_vhost_post_msg_handler(int vid, void *_msg)
{
	struct vhost_user_msg *msg = _msg;
	struct spdk_vhost_session *vsession;
	struct spdk_vhost_user_dev *user_dev;
	uint16_t qid;
	int rc;

	vsession = vhost_session_find_by_vid(vid);
	if (vsession == NULL) {
		SPDK_ERRLOG("Received a message to uninitialized session (vid %d).\n", vid);
		assert(false);
		return RTE_VHOST_MSG_RESULT_ERR;
	}
	user_dev = to_user_dev(vsession->vdev);

	if (msg->request == VHOST_USER_SET_MEM_TABLE) {
		vhost_register_memtable_if_required(vsession, vid);
	}

	switch (msg->request) {
	case VHOST_USER_SET_FEATURES:
		rc = vhost_get_negotiated_features(vid, &vsession->negotiated_features);
		if (rc) {
			SPDK_ERRLOG("vhost device %d: Failed to get negotiated driver features\n", vid);
			return RTE_VHOST_MSG_RESULT_ERR;
		}
		break;
	case VHOST_USER_SET_VRING_CALL:
		qid = (uint16_t)msg->payload.u64;
		rc = set_device_vq_callfd(vsession, qid);
		if (rc) {
			return RTE_VHOST_MSG_RESULT_ERR;
		}
		break;
	case VHOST_USER_SET_VRING_KICK:
		qid = (uint16_t)msg->payload.u64;
		rc = enable_device_vq(vsession, qid);
		if (rc) {
			return RTE_VHOST_MSG_RESULT_ERR;
		}

		/* vhost-user spec tells us to start polling a queue after receiving
		 * its SET_VRING_KICK message. Let's do it!
		 */
		pthread_mutex_lock(&user_dev->lock);
		if (!vsession->started) {
			pthread_mutex_unlock(&user_dev->lock);
			g_spdk_vhost_ops.new_device(vid);
			return RTE_VHOST_MSG_RESULT_NOT_HANDLED;
		}
		pthread_mutex_unlock(&user_dev->lock);
		break;
	default:
		break;
	}

	return RTE_VHOST_MSG_RESULT_NOT_HANDLED;
}

struct rte_vhost_user_extern_ops g_spdk_extern_vhost_ops = {
	.pre_msg_handle = extern_vhost_pre_msg_handler,
	.post_msg_handle = extern_vhost_post_msg_handler,
};

void
vhost_session_install_rte_compat_hooks(struct spdk_vhost_session *vsession)
{
	int rc;

	rc = rte_vhost_extern_callback_register(vsession->vid, &g_spdk_extern_vhost_ops, NULL);
	if (rc != 0) {
		SPDK_ERRLOG("rte_vhost_extern_callback_register() failed for vid = %d\n",
			    vsession->vid);
		return;
	}
}

int
vhost_register_unix_socket(const char *path, const char *ctrl_name,
			   uint64_t virtio_features, uint64_t disabled_features, uint64_t protocol_features)
{
	struct stat file_stat;
	uint64_t features = 0;

	/* Register vhost driver to handle vhost messages. */
	if (stat(path, &file_stat) != -1) {
		if (!S_ISSOCK(file_stat.st_mode)) {
			SPDK_ERRLOG("Cannot create a domain socket at path \"%s\": "
				    "The file already exists and is not a socket.\n",
				    path);
			return -EIO;
		} else if (unlink(path) != 0) {
			SPDK_ERRLOG("Cannot create a domain socket at path \"%s\": "
				    "The socket already exists and failed to unlink.\n",
				    path);
			return -EIO;
		}
	}

#if RTE_VERSION < RTE_VERSION_NUM(20, 8, 0, 0)
	if (rte_vhost_driver_register(path, 0) != 0) {
#else
	if (rte_vhost_driver_register(path, RTE_VHOST_USER_ASYNC_COPY) != 0) {
#endif
		SPDK_ERRLOG("Could not register controller %s with vhost library\n", ctrl_name);
		SPDK_ERRLOG("Check if domain socket %s already exists\n", path);
		return -EIO;
	}
	if (rte_vhost_driver_set_features(path, virtio_features) ||
	    rte_vhost_driver_disable_features(path, disabled_features)) {
		SPDK_ERRLOG("Couldn't set vhost features for controller %s\n", ctrl_name);

		rte_vhost_driver_unregister(path);
		return -EIO;
	}

	if (rte_vhost_driver_callback_register(path, &g_spdk_vhost_ops) != 0) {
		rte_vhost_driver_unregister(path);
		SPDK_ERRLOG("Couldn't register callbacks for controller %s\n", ctrl_name);
		return -EIO;
	}

	rte_vhost_driver_get_protocol_features(path, &features);
	features |= protocol_features;
	rte_vhost_driver_set_protocol_features(path, features);

	if (rte_vhost_driver_start(path) != 0) {
		SPDK_ERRLOG("Failed to start vhost driver for controller %s (%d): %s\n",
			    ctrl_name, errno, spdk_strerror(errno));
		rte_vhost_driver_unregister(path);
		return -EIO;
	}

	return 0;
}

int
vhost_get_mem_table(int vid, struct rte_vhost_memory **mem)
{
	return rte_vhost_get_mem_table(vid, mem);
}

int
vhost_driver_unregister(const char *path)
{
	return rte_vhost_driver_unregister(path);
}

int
vhost_get_negotiated_features(int vid, uint64_t *negotiated_features)
{
	return rte_vhost_get_negotiated_features(vid, negotiated_features);
}

int
vhost_user_dev_set_coalescing(struct spdk_vhost_user_dev *user_dev, uint32_t delay_base_us,
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

	user_dev->coalescing_delay_us = delay_base_us;
	user_dev->coalescing_iops_threshold = iops_threshold;
	return 0;
}

int
vhost_user_session_set_coalescing(struct spdk_vhost_dev *vdev,
				  struct spdk_vhost_session *vsession, void *ctx)
{
	vsession->coalescing_delay_time_base =
		to_user_dev(vdev)->coalescing_delay_us * spdk_get_ticks_hz() / 1000000ULL;
	vsession->coalescing_io_rate_threshold =
		to_user_dev(vdev)->coalescing_iops_threshold * SPDK_VHOST_STATS_CHECK_INTERVAL_MS / 1000U;
	return 0;
}

int
vhost_user_set_coalescing(struct spdk_vhost_dev *vdev, uint32_t delay_base_us,
			  uint32_t iops_threshold)
{
	int rc;

	rc = vhost_user_dev_set_coalescing(to_user_dev(vdev), delay_base_us, iops_threshold);
	if (rc != 0) {
		return rc;
	}

	vhost_user_dev_foreach_session(vdev, vhost_user_session_set_coalescing, NULL, NULL);

	return 0;
}

void
vhost_user_get_coalescing(struct spdk_vhost_dev *vdev, uint32_t *delay_base_us,
			  uint32_t *iops_threshold)
{
	struct spdk_vhost_user_dev *user_dev = to_user_dev(vdev);

	if (delay_base_us) {
		*delay_base_us = user_dev->coalescing_delay_us;
	}

	if (iops_threshold) {
		*iops_threshold = user_dev->coalescing_iops_threshold;
	}
}

int
spdk_vhost_set_socket_path(const char *basename)
{
	int ret;

	if (basename && strlen(basename) > 0) {
		ret = snprintf(g_vhost_user_dev_dirname, sizeof(g_vhost_user_dev_dirname) - 2, "%s", basename);
		if (ret <= 0) {
			return -EINVAL;
		}
		if ((size_t)ret >= sizeof(g_vhost_user_dev_dirname) - 2) {
			SPDK_ERRLOG("Char dev dir path length %d is too long\n", ret);
			return -EINVAL;
		}

		if (g_vhost_user_dev_dirname[ret - 1] != '/') {
			g_vhost_user_dev_dirname[ret] = '/';
			g_vhost_user_dev_dirname[ret + 1]  = '\0';
		}
	}

	return 0;
}

static void
vhost_dev_thread_exit(void *arg1)
{
	spdk_thread_exit(spdk_get_thread());
}

static bool g_vhost_user_started = false;

int
vhost_user_dev_register(struct spdk_vhost_dev *vdev, const char *name, struct spdk_cpuset *cpumask,
			const struct spdk_vhost_user_dev_backend *user_backend)
{
	char path[PATH_MAX];
	struct spdk_vhost_user_dev *user_dev;

	if (snprintf(path, sizeof(path), "%s%s", g_vhost_user_dev_dirname, name) >= (int)sizeof(path)) {
		SPDK_ERRLOG("Resulting socket path for controller %s is too long: %s%s\n",
			    name, g_vhost_user_dev_dirname, name);
		return -EINVAL;
	}

	vdev->path = strdup(path);
	if (vdev->path == NULL) {
		return -EIO;
	}

	user_dev = calloc(1, sizeof(*user_dev));
	if (user_dev == NULL) {
		free(vdev->path);
		return -ENOMEM;
	}
	vdev->ctxt = user_dev;

	vdev->thread = spdk_thread_create(vdev->name, cpumask);
	if (vdev->thread == NULL) {
		free(user_dev);
		free(vdev->path);
		SPDK_ERRLOG("Failed to create thread for vhost controller %s.\n", name);
		return -EIO;
	}

	user_dev->user_backend = user_backend;
	user_dev->vdev = vdev;
	user_dev->registered = true;
	TAILQ_INIT(&user_dev->vsessions);
	pthread_mutex_init(&user_dev->lock, NULL);

	vhost_user_dev_set_coalescing(user_dev, SPDK_VHOST_COALESCING_DELAY_BASE_US,
				      SPDK_VHOST_VQ_IOPS_COALESCING_THRESHOLD);

	if (vhost_register_unix_socket(path, name, vdev->virtio_features, vdev->disabled_features,
				       vdev->protocol_features)) {
		spdk_thread_send_msg(vdev->thread, vhost_dev_thread_exit, NULL);
		pthread_mutex_destroy(&user_dev->lock);
		free(user_dev);
		free(vdev->path);
		return -EIO;
	}

	return 0;
}

int
vhost_user_dev_unregister(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_user_dev *user_dev = to_user_dev(vdev);
	struct spdk_vhost_session *vsession, *tmp_vsession;

	pthread_mutex_lock(&user_dev->lock);
	if (user_dev->pending_async_op_num) {
		pthread_mutex_unlock(&user_dev->lock);
		return -EBUSY;
	}

	/* This is the case that uses RPC call `vhost_delete_controller` while VM is connected */
	if (!TAILQ_EMPTY(&user_dev->vsessions) && g_vhost_user_started) {
		SPDK_ERRLOG("Controller %s has still valid connection.\n", vdev->name);
		pthread_mutex_unlock(&user_dev->lock);
		return -EBUSY;
	}

	/* This is the case that quits the subsystem while VM is connected, the VM
	 * should be stopped by the shutdown thread.
	 */
	if (!g_vhost_user_started) {
		TAILQ_FOREACH_SAFE(vsession, &user_dev->vsessions, tailq, tmp_vsession) {
			assert(vsession->started == false);
			TAILQ_REMOVE(&user_dev->vsessions, vsession, tailq);
			if (vsession->mem) {
				vhost_session_mem_unregister(vsession->mem);
				free(vsession->mem);
			}
			free(vsession->name);
			free(vsession);
		}
	}

	user_dev->registered = false;
	pthread_mutex_unlock(&user_dev->lock);

	/* There are no valid connections now, and it's not an error if the domain
	 * socket was already removed by shutdown thread.
	 */
	vhost_driver_unregister(vdev->path);

	spdk_thread_send_msg(vdev->thread, vhost_dev_thread_exit, NULL);
	pthread_mutex_destroy(&user_dev->lock);

	free(user_dev);
	free(vdev->path);

	return 0;
}

int
vhost_user_init(void)
{
	size_t len;

	if (g_vhost_user_started) {
		return 0;
	}

	if (g_vhost_user_dev_dirname[0] == '\0') {
		if (getcwd(g_vhost_user_dev_dirname, sizeof(g_vhost_user_dev_dirname) - 1) == NULL) {
			SPDK_ERRLOG("getcwd failed (%d): %s\n", errno, spdk_strerror(errno));
			return -1;
		}

		len = strlen(g_vhost_user_dev_dirname);
		if (g_vhost_user_dev_dirname[len - 1] != '/') {
			g_vhost_user_dev_dirname[len] = '/';
			g_vhost_user_dev_dirname[len + 1] = '\0';
		}
	}

	g_vhost_user_started = true;

	g_vhost_user_init_thread = spdk_get_thread();
	assert(g_vhost_user_init_thread != NULL);

	return 0;
}

static void
vhost_user_session_shutdown_on_init(void *vhost_cb)
{
	spdk_vhost_fini_cb fn = vhost_cb;

	fn();
}

static void *
vhost_user_session_shutdown(void *vhost_cb)
{
	struct spdk_vhost_dev *vdev = NULL;
	struct spdk_vhost_session *vsession;
	struct spdk_vhost_user_dev *user_dev;

	for (vdev = spdk_vhost_dev_next(NULL); vdev != NULL;
	     vdev = spdk_vhost_dev_next(vdev)) {
		user_dev = to_user_dev(vdev);
		pthread_mutex_lock(&user_dev->lock);
		TAILQ_FOREACH(vsession, &user_dev->vsessions, tailq) {
			if (vsession->started) {
				_stop_session(vsession);
			}
		}
		pthread_mutex_unlock(&user_dev->lock);
		vhost_driver_unregister(vdev->path);
	}

	SPDK_INFOLOG(vhost, "Exiting\n");
	spdk_thread_send_msg(g_vhost_user_init_thread, vhost_user_session_shutdown_on_init, vhost_cb);
	return NULL;
}

void
vhost_user_fini(spdk_vhost_fini_cb vhost_cb)
{
	pthread_t tid;
	int rc;

	if (!g_vhost_user_started) {
		vhost_cb();
		return;
	}

	g_vhost_user_started = false;

	/* rte_vhost API for removing sockets is not asynchronous. Since it may call SPDK
	 * ops for stopping a device or removing a connection, we need to call it from
	 * a separate thread to avoid deadlock.
	 */
	rc = pthread_create(&tid, NULL, &vhost_user_session_shutdown, vhost_cb);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to start session shutdown thread (%d): %s\n", rc, spdk_strerror(rc));
		abort();
	}
	pthread_detach(tid);
}
