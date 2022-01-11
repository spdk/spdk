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

bool g_packed_ring_recovery = false;

static struct spdk_cpuset g_vhost_core_mask;

/* Thread performing all vhost management operations */
struct spdk_thread *g_vhost_init_thread = NULL;

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

static TAILQ_HEAD(, spdk_vhost_dev) g_vhost_devices = TAILQ_HEAD_INITIALIZER(
			g_vhost_devices);
static pthread_mutex_t g_vhost_mutex = PTHREAD_MUTEX_INITIALIZER;

void *vhost_gpa_to_vva(struct spdk_vhost_session *vsession, uint64_t addr, uint64_t len)
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
			/* To be honest, only pages realy touched should be logged, but
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

	virtqueue->req_cnt += virtqueue->used_req_cnt;
	virtqueue->used_req_cnt = 0;

	SPDK_DEBUGLOG(vhost_ring,
		      "Queue %td - USED RING: sending IRQ: last used %"PRIu16"\n",
		      virtqueue - vsession->virtqueue, virtqueue->last_used_idx);

	if (rte_vhost_vring_call(vsession->vid, virtqueue->vring_idx) == 0) {
		/* interrupt signalled */
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

void
vhost_session_used_signal(struct spdk_vhost_session *vsession)
{
	struct spdk_vhost_virtqueue *virtqueue;
	uint16_t q_idx;

	for (q_idx = 0; q_idx < vsession->max_queues; q_idx++) {
		virtqueue = &vsession->virtqueue[q_idx];
		vhost_session_vq_used_signal(virtqueue);
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

static struct spdk_vhost_session *
vhost_session_find_by_id(struct spdk_vhost_dev *vdev, unsigned id)
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
vhost_session_find_by_vid(int vid)
{
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_session *vsession;

	for (vdev = spdk_vhost_dev_next(NULL); vdev != NULL;
	     vdev = spdk_vhost_dev_next(vdev)) {
		TAILQ_FOREACH(vsession, &vdev->vsessions, tailq) {
			if (vsession->vid == vid) {
				return vsession;
			}
		}
	}

	return NULL;
}

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

static void
vhost_session_cb_done(int rc)
{
	g_dpdk_response = rc;
	sem_post(&g_dpdk_sem);
}

void
vhost_session_start_done(struct spdk_vhost_session *vsession, int response)
{
	if (response == 0) {
		vsession->started = true;

		assert(vsession->vdev->active_session_num < UINT32_MAX);
		vsession->vdev->active_session_num++;
	}

	vhost_session_cb_done(response);
}

void
vhost_session_stop_done(struct spdk_vhost_session *vsession, int response)
{
	if (response == 0) {
		vsession->started = false;

		assert(vsession->vdev->active_session_num > 0);
		vsession->vdev->active_session_num--;
	}

	vhost_session_cb_done(response);
}

static void
vhost_event_cb(void *arg1)
{
	struct vhost_session_fn_ctx *ctx = arg1;
	struct spdk_vhost_session *vsession;

	if (spdk_vhost_trylock() != 0) {
		spdk_thread_send_msg(spdk_get_thread(), vhost_event_cb, arg1);
		return;
	}

	vsession = vhost_session_find_by_id(ctx->vdev, ctx->vsession_id);
	ctx->cb_fn(ctx->vdev, vsession, NULL);
	spdk_vhost_unlock();
}

int
vhost_session_send_event(struct spdk_vhost_session *vsession,
			 spdk_vhost_session_fn cb_fn, unsigned timeout_sec,
			 const char *errmsg)
{
	struct vhost_session_fn_ctx ev_ctx = {0};
	struct spdk_vhost_dev *vdev = vsession->vdev;

	ev_ctx.vdev = vdev;
	ev_ctx.vsession_id = vsession->id;
	ev_ctx.cb_fn = cb_fn;

	spdk_thread_send_msg(vdev->thread, vhost_event_cb, &ev_ctx);

	spdk_vhost_unlock();
	wait_for_semaphore(timeout_sec, errmsg);
	spdk_vhost_lock();

	return g_dpdk_response;
}

static void
foreach_session_finish_cb(void *arg1)
{
	struct vhost_session_fn_ctx *ev_ctx = arg1;
	struct spdk_vhost_dev *vdev = ev_ctx->vdev;

	if (spdk_vhost_trylock() != 0) {
		spdk_thread_send_msg(spdk_get_thread(),
				     foreach_session_finish_cb, arg1);
		return;
	}

	assert(vdev->pending_async_op_num > 0);
	vdev->pending_async_op_num--;
	if (ev_ctx->cpl_fn != NULL) {
		ev_ctx->cpl_fn(vdev, ev_ctx->user_ctx);
	}

	spdk_vhost_unlock();
	free(ev_ctx);
}

static void
foreach_session(void *arg1)
{
	struct vhost_session_fn_ctx *ev_ctx = arg1;
	struct spdk_vhost_session *vsession;
	struct spdk_vhost_dev *vdev = ev_ctx->vdev;
	int rc;

	if (spdk_vhost_trylock() != 0) {
		spdk_thread_send_msg(spdk_get_thread(), foreach_session, arg1);
		return;
	}

	TAILQ_FOREACH(vsession, &vdev->vsessions, tailq) {
		if (vsession->initialized) {
			rc = ev_ctx->cb_fn(vdev, vsession, ev_ctx->user_ctx);
			if (rc < 0) {
				goto out;
			}
		}
	}

out:
	spdk_vhost_unlock();

	spdk_thread_send_msg(g_vhost_init_thread, foreach_session_finish_cb, arg1);
}

void
vhost_dev_foreach_session(struct spdk_vhost_dev *vdev,
			  spdk_vhost_session_fn fn,
			  spdk_vhost_dev_fn cpl_fn,
			  void *arg)
{
	struct vhost_session_fn_ctx *ev_ctx;

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

	assert(vdev->pending_async_op_num < UINT32_MAX);
	vdev->pending_async_op_num++;

	spdk_thread_send_msg(vdev->thread, foreach_session, ev_ctx);
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
	if (vdev->pending_async_op_num) {
		return -EBUSY;
	}

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

	g_vhost_init_thread = spdk_get_thread();
	assert(g_vhost_init_thread != NULL);

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
	assert(spdk_get_thread() == g_vhost_init_thread);

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
