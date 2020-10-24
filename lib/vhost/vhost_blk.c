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

#include <linux/virtio_blk.h>

#include "spdk/env.h"
#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/vhost.h"

#include "vhost_internal.h"
#include <rte_version.h>

/* Minimal set of features supported by every SPDK VHOST-BLK device */
#define SPDK_VHOST_BLK_FEATURES_BASE (SPDK_VHOST_FEATURES | \
		(1ULL << VIRTIO_BLK_F_SIZE_MAX) | (1ULL << VIRTIO_BLK_F_SEG_MAX) | \
		(1ULL << VIRTIO_BLK_F_GEOMETRY) | (1ULL << VIRTIO_BLK_F_BLK_SIZE) | \
		(1ULL << VIRTIO_BLK_F_TOPOLOGY) | (1ULL << VIRTIO_BLK_F_BARRIER)  | \
		(1ULL << VIRTIO_BLK_F_SCSI)     | (1ULL << VIRTIO_BLK_F_CONFIG_WCE) | \
		(1ULL << VIRTIO_BLK_F_MQ))

/* Not supported features */
#define SPDK_VHOST_BLK_DISABLED_FEATURES (SPDK_VHOST_DISABLED_FEATURES | \
		(1ULL << VIRTIO_BLK_F_GEOMETRY) | (1ULL << VIRTIO_BLK_F_CONFIG_WCE) | \
		(1ULL << VIRTIO_BLK_F_BARRIER)  | (1ULL << VIRTIO_BLK_F_SCSI))

/* Vhost-blk support protocol features */
#define SPDK_VHOST_BLK_PROTOCOL_FEATURES ((1ULL << VHOST_USER_PROTOCOL_F_CONFIG) | \
		(1ULL << VHOST_USER_PROTOCOL_F_INFLIGHT_SHMFD))

struct spdk_vhost_blk_task {
	struct spdk_bdev_io *bdev_io;
	struct spdk_vhost_blk_session *bvsession;
	struct spdk_vhost_virtqueue *vq;

	volatile uint8_t *status;

	uint16_t req_idx;
	uint16_t num_descs;
	uint16_t buffer_id;

	/* for io wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;

	/* If set, the task is currently used for I/O processing. */
	bool used;

	/** Number of bytes that were written. */
	uint32_t used_len;
	uint16_t iovcnt;
	struct iovec iovs[SPDK_VHOST_IOVS_MAX];
};

struct spdk_vhost_blk_dev {
	struct spdk_vhost_dev vdev;
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *bdev_desc;
	/* dummy_io_channel is used to hold a bdev reference */
	struct spdk_io_channel *dummy_io_channel;
	bool readonly;
};

struct spdk_vhost_blk_session {
	/* The parent session must be the very first field in this struct */
	struct spdk_vhost_session vsession;
	struct spdk_vhost_blk_dev *bvdev;
	struct spdk_poller *requestq_poller;
	struct spdk_io_channel *io_channel;
	struct spdk_poller *stop_poller;
};

/* forward declaration */
static const struct spdk_vhost_dev_backend vhost_blk_device_backend;

static int
process_blk_request(struct spdk_vhost_blk_task *task,
		    struct spdk_vhost_blk_session *bvsession,
		    struct spdk_vhost_virtqueue *vq);

static struct spdk_vhost_blk_session *
to_blk_session(struct spdk_vhost_session *vsession)
{
	assert(vsession->vdev->backend == &vhost_blk_device_backend);
	return (struct spdk_vhost_blk_session *)vsession;
}

static void
blk_task_finish(struct spdk_vhost_blk_task *task)
{
	assert(task->bvsession->vsession.task_cnt > 0);
	task->bvsession->vsession.task_cnt--;
	task->used = false;
}

static void
blk_task_init(struct spdk_vhost_blk_task *task)
{
	task->used = true;
	task->iovcnt = SPDK_COUNTOF(task->iovs);
	task->status = NULL;
	task->used_len = 0;
}

static void
blk_task_enqueue(struct spdk_vhost_blk_task *task)
{
	if (task->vq->packed.packed_ring) {
		vhost_vq_packed_ring_enqueue(&task->bvsession->vsession, task->vq,
					     task->num_descs,
					     task->buffer_id, task->used_len);
	} else {
		vhost_vq_used_ring_enqueue(&task->bvsession->vsession, task->vq,
					   task->req_idx, task->used_len);
	}
}

static void
invalid_blk_request(struct spdk_vhost_blk_task *task, uint8_t status)
{
	if (task->status) {
		*task->status = status;
	}

	blk_task_enqueue(task);
	blk_task_finish(task);
	SPDK_DEBUGLOG(vhost_blk_data, "Invalid request (status=%" PRIu8")\n", status);
}

/*
 * Process task's descriptor chain and setup data related fields.
 * Return
 *   total size of suplied buffers
 *
 *   FIXME: Make this function return to rd_cnt and wr_cnt
 */
static int
blk_iovs_split_queue_setup(struct spdk_vhost_blk_session *bvsession,
			   struct spdk_vhost_virtqueue *vq,
			   uint16_t req_idx, struct iovec *iovs, uint16_t *iovs_cnt, uint32_t *length)
{
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	struct spdk_vhost_dev *vdev = vsession->vdev;
	struct vring_desc *desc, *desc_table;
	uint16_t out_cnt = 0, cnt = 0;
	uint32_t desc_table_size, len = 0;
	uint32_t desc_handled_cnt;
	int rc;

	rc = vhost_vq_get_desc(vsession, vq, req_idx, &desc, &desc_table, &desc_table_size);
	if (rc != 0) {
		SPDK_ERRLOG("%s: invalid descriptor at index %"PRIu16".\n", vdev->name, req_idx);
		return -1;
	}

	desc_handled_cnt = 0;
	while (1) {
		/*
		 * Maximum cnt reached?
		 * Should not happen if request is well formatted, otherwise this is a BUG.
		 */
		if (spdk_unlikely(cnt == *iovs_cnt)) {
			SPDK_DEBUGLOG(vhost_blk, "%s: max IOVs in request reached (req_idx = %"PRIu16").\n",
				      vsession->name, req_idx);
			return -1;
		}

		if (spdk_unlikely(vhost_vring_desc_to_iov(vsession, iovs, &cnt, desc))) {
			SPDK_DEBUGLOG(vhost_blk, "%s: invalid descriptor %" PRIu16" (req_idx = %"PRIu16").\n",
				      vsession->name, req_idx, cnt);
			return -1;
		}

		len += desc->len;

		out_cnt += vhost_vring_desc_is_wr(desc);

		rc = vhost_vring_desc_get_next(&desc, desc_table, desc_table_size);
		if (rc != 0) {
			SPDK_ERRLOG("%s: descriptor chain at index %"PRIu16" terminated unexpectedly.\n",
				    vsession->name, req_idx);
			return -1;
		} else if (desc == NULL) {
			break;
		}

		desc_handled_cnt++;
		if (spdk_unlikely(desc_handled_cnt > desc_table_size)) {
			/* Break a cycle and report an error, if any. */
			SPDK_ERRLOG("%s: found a cycle in the descriptor chain: desc_table_size = %d, desc_handled_cnt = %d.\n",
				    vsession->name, desc_table_size, desc_handled_cnt);
			return -1;
		}
	}

	/*
	 * There must be least two descriptors.
	 * First contain request so it must be readable.
	 * Last descriptor contain buffer for response so it must be writable.
	 */
	if (spdk_unlikely(out_cnt == 0 || cnt < 2)) {
		return -1;
	}

	*length = len;
	*iovs_cnt = cnt;
	return 0;
}

static int
blk_iovs_packed_queue_setup(struct spdk_vhost_blk_session *bvsession,
			    struct spdk_vhost_virtqueue *vq,
			    uint16_t req_idx, struct iovec *iovs, uint16_t *iovs_cnt, uint32_t *length)
{
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	struct spdk_vhost_dev *vdev = vsession->vdev;
	struct vring_packed_desc *desc = NULL, *desc_table;
	uint16_t out_cnt = 0, cnt = 0;
	uint32_t desc_table_size, len = 0;
	int rc = 0;

	rc = vhost_vq_get_desc_packed(vsession, vq, req_idx, &desc,
				      &desc_table, &desc_table_size);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("%s: Invalid descriptor at index %"PRIu16".\n", vdev->name, req_idx);
		return rc;
	}

	if (desc_table != NULL) {
		req_idx = 0;
	}

	while (1) {
		/*
		 * Maximum cnt reached?
		 * Should not happen if request is well formatted, otherwise this is a BUG.
		 */
		if (spdk_unlikely(cnt == *iovs_cnt)) {
			SPDK_ERRLOG("%s: max IOVs in request reached (req_idx = %"PRIu16").\n",
				    vsession->name, req_idx);
			return -EINVAL;
		}

		if (spdk_unlikely(vhost_vring_packed_desc_to_iov(vsession, iovs, &cnt, desc))) {
			SPDK_ERRLOG("%s: invalid descriptor %" PRIu16" (req_idx = %"PRIu16").\n",
				    vsession->name, req_idx, cnt);
			return -EINVAL;
		}

		len += desc->len;
		out_cnt += vhost_vring_packed_desc_is_wr(desc);

		/* desc is NULL means we reach the last desc of this request */
		vhost_vring_packed_desc_get_next(&desc, &req_idx, vq, desc_table, desc_table_size);
		if (desc == NULL) {
			break;
		}
	}

	/*
	 * There must be least two descriptors.
	 * First contain request so it must be readable.
	 * Last descriptor contain buffer for response so it must be writable.
	 */
	if (spdk_unlikely(out_cnt == 0 || cnt < 2)) {
		return -EINVAL;
	}

	*length = len;
	*iovs_cnt = cnt;

	return 0;
}

static void
blk_request_finish(bool success, struct spdk_vhost_blk_task *task)
{
	*task->status = success ? VIRTIO_BLK_S_OK : VIRTIO_BLK_S_IOERR;

	blk_task_enqueue(task);

	SPDK_DEBUGLOG(vhost_blk, "Finished task (%p) req_idx=%d\n status: %s\n", task,
		      task->req_idx, success ? "OK" : "FAIL");
	blk_task_finish(task);
}

static void
blk_request_complete_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_vhost_blk_task *task = cb_arg;

	spdk_bdev_free_io(bdev_io);
	blk_request_finish(success, task);
}

static void
blk_request_resubmit(void *arg)
{
	struct spdk_vhost_blk_task *task = (struct spdk_vhost_blk_task *)arg;
	int rc = 0;

	blk_task_init(task);

	rc = process_blk_request(task, task->bvsession, task->vq);
	if (rc == 0) {
		SPDK_DEBUGLOG(vhost_blk, "====== Task %p resubmitted ======\n", task);
	} else {
		SPDK_DEBUGLOG(vhost_blk, "====== Task %p failed ======\n", task);
	}
}

static inline void
blk_request_queue_io(struct spdk_vhost_blk_task *task)
{
	int rc;
	struct spdk_vhost_blk_session *bvsession = task->bvsession;
	struct spdk_bdev *bdev = bvsession->bvdev->bdev;

	task->bdev_io_wait.bdev = bdev;
	task->bdev_io_wait.cb_fn = blk_request_resubmit;
	task->bdev_io_wait.cb_arg = task;

	rc = spdk_bdev_queue_io_wait(bdev, bvsession->io_channel, &task->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("%s: failed to queue I/O, rc=%d\n", bvsession->vsession.name, rc);
		invalid_blk_request(task, VIRTIO_BLK_S_IOERR);
	}
}

static int
process_blk_request(struct spdk_vhost_blk_task *task,
		    struct spdk_vhost_blk_session *bvsession,
		    struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_blk_dev *bvdev = bvsession->bvdev;
	const struct virtio_blk_outhdr *req;
	struct virtio_blk_discard_write_zeroes *desc;
	struct iovec *iov;
	uint32_t type;
	uint32_t payload_len;
	uint64_t flush_bytes;
	int rc;

	if (vq->packed.packed_ring) {
		rc = blk_iovs_packed_queue_setup(bvsession, vq, task->req_idx, task->iovs, &task->iovcnt,
						 &payload_len);
	} else {
		rc = blk_iovs_split_queue_setup(bvsession, vq, task->req_idx, task->iovs, &task->iovcnt,
						&payload_len);
	}

	if (rc) {
		SPDK_DEBUGLOG(vhost_blk, "Invalid request (req_idx = %"PRIu16").\n", task->req_idx);
		/* Only READ and WRITE are supported for now. */
		invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	iov = &task->iovs[0];
	if (spdk_unlikely(iov->iov_len != sizeof(*req))) {
		SPDK_DEBUGLOG(vhost_blk,
			      "First descriptor size is %zu but expected %zu (req_idx = %"PRIu16").\n",
			      iov->iov_len, sizeof(*req), task->req_idx);
		invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	req = iov->iov_base;

	iov = &task->iovs[task->iovcnt - 1];
	if (spdk_unlikely(iov->iov_len != 1)) {
		SPDK_DEBUGLOG(vhost_blk,
			      "Last descriptor size is %zu but expected %d (req_idx = %"PRIu16").\n",
			      iov->iov_len, 1, task->req_idx);
		invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	task->status = iov->iov_base;
	payload_len -= sizeof(*req) + sizeof(*task->status);
	task->iovcnt -= 2;

	type = req->type;
#ifdef VIRTIO_BLK_T_BARRIER
	/* Don't care about barier for now (as QEMU's virtio-blk do). */
	type &= ~VIRTIO_BLK_T_BARRIER;
#endif

	switch (type) {
	case VIRTIO_BLK_T_IN:
	case VIRTIO_BLK_T_OUT:
		if (spdk_unlikely(payload_len == 0 || (payload_len & (512 - 1)) != 0)) {
			SPDK_ERRLOG("%s - passed IO buffer is not multiple of 512b (req_idx = %"PRIu16").\n",
				    type ? "WRITE" : "READ", task->req_idx);
			invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
			return -1;
		}

		if (type == VIRTIO_BLK_T_IN) {
			task->used_len = payload_len + sizeof(*task->status);
			rc = spdk_bdev_readv(bvdev->bdev_desc, bvsession->io_channel,
					     &task->iovs[1], task->iovcnt, req->sector * 512,
					     payload_len, blk_request_complete_cb, task);
		} else if (!bvdev->readonly) {
			task->used_len = sizeof(*task->status);
			rc = spdk_bdev_writev(bvdev->bdev_desc, bvsession->io_channel,
					      &task->iovs[1], task->iovcnt, req->sector * 512,
					      payload_len, blk_request_complete_cb, task);
		} else {
			SPDK_DEBUGLOG(vhost_blk, "Device is in read-only mode!\n");
			rc = -1;
		}

		if (rc) {
			if (rc == -ENOMEM) {
				SPDK_DEBUGLOG(vhost_blk, "No memory, start to queue io.\n");
				blk_request_queue_io(task);
			} else {
				invalid_blk_request(task, VIRTIO_BLK_S_IOERR);
				return -1;
			}
		}
		break;
	case VIRTIO_BLK_T_DISCARD:
		desc = task->iovs[1].iov_base;
		if (payload_len != sizeof(*desc)) {
			SPDK_NOTICELOG("Invalid discard payload size: %u\n", payload_len);
			invalid_blk_request(task, VIRTIO_BLK_S_IOERR);
			return -1;
		}

		rc = spdk_bdev_unmap(bvdev->bdev_desc, bvsession->io_channel,
				     desc->sector * 512, desc->num_sectors * 512,
				     blk_request_complete_cb, task);
		if (rc) {
			if (rc == -ENOMEM) {
				SPDK_DEBUGLOG(vhost_blk, "No memory, start to queue io.\n");
				blk_request_queue_io(task);
			} else {
				invalid_blk_request(task, VIRTIO_BLK_S_IOERR);
				return -1;
			}
		}
		break;
	case VIRTIO_BLK_T_WRITE_ZEROES:
		desc = task->iovs[1].iov_base;
		if (payload_len != sizeof(*desc)) {
			SPDK_NOTICELOG("Invalid write zeroes payload size: %u\n", payload_len);
			invalid_blk_request(task, VIRTIO_BLK_S_IOERR);
			return -1;
		}

		/* Zeroed and Unmap the range, SPDK doen't support it. */
		if (desc->flags & VIRTIO_BLK_WRITE_ZEROES_FLAG_UNMAP) {
			SPDK_NOTICELOG("Can't support Write Zeroes with Unmap flag\n");
			invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
			return -1;
		}

		rc = spdk_bdev_write_zeroes(bvdev->bdev_desc, bvsession->io_channel,
					    desc->sector * 512, desc->num_sectors * 512,
					    blk_request_complete_cb, task);
		if (rc) {
			if (rc == -ENOMEM) {
				SPDK_DEBUGLOG(vhost_blk, "No memory, start to queue io.\n");
				blk_request_queue_io(task);
			} else {
				invalid_blk_request(task, VIRTIO_BLK_S_IOERR);
				return -1;
			}
		}
		break;
	case VIRTIO_BLK_T_FLUSH:
		flush_bytes = spdk_bdev_get_num_blocks(bvdev->bdev) * spdk_bdev_get_block_size(bvdev->bdev);
		if (req->sector != 0) {
			SPDK_NOTICELOG("sector must be zero for flush command\n");
			invalid_blk_request(task, VIRTIO_BLK_S_IOERR);
			return -1;
		}
		rc = spdk_bdev_flush(bvdev->bdev_desc, bvsession->io_channel,
				     0, flush_bytes,
				     blk_request_complete_cb, task);
		if (rc) {
			if (rc == -ENOMEM) {
				SPDK_DEBUGLOG(vhost_blk, "No memory, start to queue io.\n");
				blk_request_queue_io(task);
			} else {
				invalid_blk_request(task, VIRTIO_BLK_S_IOERR);
				return -1;
			}
		}
		break;
	case VIRTIO_BLK_T_GET_ID:
		if (!task->iovcnt || !payload_len) {
			invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
			return -1;
		}
		task->used_len = spdk_min((size_t)VIRTIO_BLK_ID_BYTES, task->iovs[1].iov_len);
		spdk_strcpy_pad(task->iovs[1].iov_base, spdk_bdev_get_product_name(bvdev->bdev),
				task->used_len, ' ');
		blk_request_finish(true, task);
		break;
	default:
		SPDK_DEBUGLOG(vhost_blk, "Not supported request type '%"PRIu32"'.\n", type);
		invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	return 0;
}

static void
process_blk_task(struct spdk_vhost_virtqueue *vq, uint16_t req_idx)
{
	struct spdk_vhost_blk_task *task;
	uint16_t task_idx = req_idx, num_descs;

	if (vq->packed.packed_ring) {
		/* Packed ring used the buffer_id as the task_idx to get task struct.
		 * In kernel driver, it uses the vq->free_head to set the buffer_id so the value
		 * must be in the range of 0 ~ vring.size. The free_head value must be unique
		 * in the outstanding requests.
		 * We can't use the req_idx as the task_idx because the desc can be reused in
		 * the next phase even when it's not completed in the previous phase. For example,
		 * At phase 0, last_used_idx was 2 and desc0 was not completed.Then after moving
		 * phase 1, last_avail_idx is updated to 1. In this case, req_idx can not be used
		 * as task_idx because we will know task[0]->used is true at phase 1.
		 * The split queue is quite different, the desc would insert into the free list when
		 * device completes the request, the driver gets the desc from the free list which
		 * ensures the req_idx is unique in the outstanding requests.
		 */
		task_idx = vhost_vring_packed_desc_get_buffer_id(vq, req_idx, &num_descs);
	}

	task = &((struct spdk_vhost_blk_task *)vq->tasks)[task_idx];
	if (spdk_unlikely(task->used)) {
		SPDK_ERRLOG("%s: request with idx '%"PRIu16"' is already pending.\n",
			    task->bvsession->vsession.name, task_idx);
		task->used_len = 0;
		blk_task_enqueue(task);
		return;
	}

	if (vq->packed.packed_ring) {
		task->req_idx = req_idx;
		task->num_descs = num_descs;
		task->buffer_id = task_idx;
	}

	task->bvsession->vsession.task_cnt++;

	blk_task_init(task);

	if (process_blk_request(task, task->bvsession, vq) == 0) {
		SPDK_DEBUGLOG(vhost_blk, "====== Task %p req_idx %d submitted ======\n", task,
			      task_idx);
	} else {
		SPDK_ERRLOG("====== Task %p req_idx %d failed ======\n", task, task_idx);
	}
}

static void
submit_inflight_desc(struct spdk_vhost_blk_session *bvsession,
		     struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	spdk_vhost_resubmit_info *resubmit = vq->vring_inflight.resubmit_inflight;
	spdk_vhost_resubmit_desc *resubmit_list;
	uint16_t req_idx;

	if (spdk_likely(resubmit == NULL || resubmit->resubmit_list == NULL)) {
		return;
	}

	resubmit_list = resubmit->resubmit_list;
	while (resubmit->resubmit_num-- > 0) {
		req_idx = resubmit_list[resubmit->resubmit_num].index;
		SPDK_DEBUGLOG(vhost_blk, "====== Start processing request idx %"PRIu16"======\n",
			      req_idx);

		if (spdk_unlikely(req_idx >= vq->vring.size)) {
			SPDK_ERRLOG("%s: request idx '%"PRIu16"' exceeds virtqueue size (%"PRIu16").\n",
				    vsession->name, req_idx, vq->vring.size);
			vhost_vq_used_ring_enqueue(vsession, vq, req_idx, 0);
			continue;
		}

		process_blk_task(vq, req_idx);
	}

	free(resubmit_list);
	resubmit->resubmit_list = NULL;
}

static void
process_vq(struct spdk_vhost_blk_session *bvsession, struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	uint16_t reqs[SPDK_VHOST_VQ_MAX_SUBMISSIONS];
	uint16_t reqs_cnt, i;

	submit_inflight_desc(bvsession, vq);

	reqs_cnt = vhost_vq_avail_ring_get(vq, reqs, SPDK_COUNTOF(reqs));
	if (!reqs_cnt) {
		return;
	}

	for (i = 0; i < reqs_cnt; i++) {
		SPDK_DEBUGLOG(vhost_blk, "====== Starting processing request idx %"PRIu16"======\n",
			      reqs[i]);

		if (spdk_unlikely(reqs[i] >= vq->vring.size)) {
			SPDK_ERRLOG("%s: request idx '%"PRIu16"' exceeds virtqueue size (%"PRIu16").\n",
				    vsession->name, reqs[i], vq->vring.size);
			vhost_vq_used_ring_enqueue(vsession, vq, reqs[i], 0);
			continue;
		}

		rte_vhost_set_inflight_desc_split(vsession->vid, vq->vring_idx, reqs[i]);

		process_blk_task(vq, reqs[i]);
	}
}

static void
process_packed_vq(struct spdk_vhost_blk_session *bvsession, struct spdk_vhost_virtqueue *vq)
{
	uint16_t i = 0;

	while (i++ < SPDK_VHOST_VQ_MAX_SUBMISSIONS &&
	       vhost_vq_packed_ring_is_avail(vq)) {
		SPDK_DEBUGLOG(vhost_blk, "====== Starting processing request idx %"PRIu16"======\n",
			      vq->last_avail_idx);

		process_blk_task(vq, vq->last_avail_idx);
	}
}

static int
_vdev_vq_worker(struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_session *vsession = vq->vsession;
	struct spdk_vhost_blk_session *bvsession = to_blk_session(vsession);
	bool packed_ring;

	packed_ring = vq->packed.packed_ring;
	if (packed_ring) {
		process_packed_vq(bvsession, vq);
	} else {
		process_vq(bvsession, vq);
	}

	vhost_session_vq_used_signal(vq);

	return SPDK_POLLER_BUSY;

}

static int
vdev_vq_worker(void *arg)
{
	struct spdk_vhost_virtqueue *vq = arg;

	return _vdev_vq_worker(vq);
}

static int
vdev_worker(void *arg)
{
	struct spdk_vhost_blk_session *bvsession = arg;
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	uint16_t q_idx;

	for (q_idx = 0; q_idx < vsession->max_queues; q_idx++) {
		_vdev_vq_worker(&vsession->virtqueue[q_idx]);
	}

	return SPDK_POLLER_BUSY;
}

static void
no_bdev_process_vq(struct spdk_vhost_blk_session *bvsession, struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	struct iovec iovs[SPDK_VHOST_IOVS_MAX];
	uint32_t length;
	uint16_t iovcnt, req_idx;

	if (vhost_vq_avail_ring_get(vq, &req_idx, 1) != 1) {
		return;
	}

	iovcnt = SPDK_COUNTOF(iovs);
	if (blk_iovs_split_queue_setup(bvsession, vq, req_idx, iovs, &iovcnt, &length) == 0) {
		*(volatile uint8_t *)iovs[iovcnt - 1].iov_base = VIRTIO_BLK_S_IOERR;
		SPDK_DEBUGLOG(vhost_blk_data, "Aborting request %" PRIu16"\n", req_idx);
	}

	vhost_vq_used_ring_enqueue(vsession, vq, req_idx, 0);
}

static void
no_bdev_process_packed_vq(struct spdk_vhost_blk_session *bvsession, struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	struct spdk_vhost_blk_task *task;
	uint32_t length;
	uint16_t req_idx = vq->last_avail_idx;
	uint16_t task_idx, num_descs;

	if (!vhost_vq_packed_ring_is_avail(vq)) {
		return;
	}

	task_idx = vhost_vring_packed_desc_get_buffer_id(vq, req_idx, &num_descs);
	task = &((struct spdk_vhost_blk_task *)vq->tasks)[task_idx];
	if (spdk_unlikely(task->used)) {
		SPDK_ERRLOG("%s: request with idx '%"PRIu16"' is already pending.\n",
			    vsession->name, req_idx);
		vhost_vq_packed_ring_enqueue(vsession, vq, num_descs,
					     task->buffer_id, task->used_len);
		return;
	}

	task->req_idx = req_idx;
	task->num_descs = num_descs;
	task->buffer_id = task_idx;
	blk_task_init(task);

	if (blk_iovs_packed_queue_setup(bvsession, vq, task->req_idx, task->iovs, &task->iovcnt,
					&length)) {
		*(volatile uint8_t *)(task->iovs[task->iovcnt - 1].iov_base) = VIRTIO_BLK_S_IOERR;
		SPDK_DEBUGLOG(vhost_blk_data, "Aborting request %" PRIu16"\n", req_idx);
	}

	task->used = false;
	vhost_vq_packed_ring_enqueue(vsession, vq, num_descs,
				     task->buffer_id, task->used_len);
}

static int
_no_bdev_vdev_vq_worker(struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_session *vsession = vq->vsession;
	struct spdk_vhost_blk_session *bvsession = to_blk_session(vsession);
	bool packed_ring;

	packed_ring = vq->packed.packed_ring;
	if (packed_ring) {
		no_bdev_process_packed_vq(bvsession, vq);
	} else {
		no_bdev_process_vq(bvsession, vq);
	}

	vhost_session_vq_used_signal(vq);

	if (vsession->task_cnt == 0 && bvsession->io_channel) {
		spdk_put_io_channel(bvsession->io_channel);
		bvsession->io_channel = NULL;
	}

	return SPDK_POLLER_BUSY;
}

static int
no_bdev_vdev_vq_worker(void *arg)
{
	struct spdk_vhost_virtqueue *vq = arg;

	return _no_bdev_vdev_vq_worker(vq);
}

static int
no_bdev_vdev_worker(void *arg)
{
	struct spdk_vhost_blk_session *bvsession = arg;
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	uint16_t q_idx;

	for (q_idx = 0; q_idx < vsession->max_queues; q_idx++) {
		_no_bdev_vdev_vq_worker(&vsession->virtqueue[q_idx]);
	}

	return SPDK_POLLER_BUSY;
}

static void
vhost_blk_session_unregister_interrupts(struct spdk_vhost_blk_session *bvsession)
{
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	struct spdk_vhost_virtqueue *vq;
	int i;

	SPDK_DEBUGLOG(vhost_blk, "unregister virtqueues interrupt\n");
	for (i = 0; i < vsession->max_queues; i++) {
		vq = &vsession->virtqueue[i];
		if (vq->intr == NULL) {
			break;
		}

		SPDK_DEBUGLOG(vhost_blk, "unregister vq[%d]'s kickfd is %d\n",
			      i, vq->vring.kickfd);
		spdk_interrupt_unregister(&vq->intr);
	}
}

static int
vhost_blk_session_register_interrupts(struct spdk_vhost_blk_session *bvsession,
				      spdk_interrupt_fn fn)
{
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	struct spdk_vhost_virtqueue *vq = NULL;
	int i;

	SPDK_DEBUGLOG(vhost_blk, "Register virtqueues interrupt\n");
	for (i = 0; i < vsession->max_queues; i++) {
		vq = &vsession->virtqueue[i];
		SPDK_DEBUGLOG(vhost_blk, "Register vq[%d]'s kickfd is %d\n",
			      i, vq->vring.kickfd);

		vq->intr = SPDK_INTERRUPT_REGISTER(vq->vring.kickfd, fn, vq);
		if (vq->intr == NULL) {
			SPDK_ERRLOG("Fail to register req notifier handler.\n");
			goto err;
		}
	}

	return 0;

err:
	vhost_blk_session_unregister_interrupts(bvsession);

	return -1;
}

static struct spdk_vhost_blk_dev *
to_blk_dev(struct spdk_vhost_dev *vdev)
{
	if (vdev == NULL) {
		return NULL;
	}

	if (vdev->backend != &vhost_blk_device_backend) {
		SPDK_ERRLOG("%s: not a vhost-blk device\n", vdev->name);
		return NULL;
	}

	return SPDK_CONTAINEROF(vdev, struct spdk_vhost_blk_dev, vdev);
}

static int
vhost_session_bdev_resize_cb(struct spdk_vhost_dev *vdev,
			     struct spdk_vhost_session *vsession,
			     void *ctx)
{
#if RTE_VERSION >= RTE_VERSION_NUM(20, 02, 0, 0)
	SPDK_NOTICELOG("bdev send slave msg to vid(%d)\n", vsession->vid);
	rte_vhost_slave_config_change(vsession->vid, false);
#else
	SPDK_NOTICELOG("bdev does not support resize until DPDK submodule version >= 20.02\n");
#endif

	return 0;
}

static void
blk_resize_cb(void *resize_ctx)
{
	struct spdk_vhost_blk_dev *bvdev = resize_ctx;

	spdk_vhost_lock();
	vhost_dev_foreach_session(&bvdev->vdev, vhost_session_bdev_resize_cb,
				  NULL, NULL);
	spdk_vhost_unlock();
}

static void
vhost_dev_bdev_remove_cpl_cb(struct spdk_vhost_dev *vdev, void *ctx)
{

	/* All sessions have been notified, time to close the bdev */
	struct spdk_vhost_blk_dev *bvdev = to_blk_dev(vdev);

	assert(bvdev != NULL);
	spdk_put_io_channel(bvdev->dummy_io_channel);
	spdk_bdev_close(bvdev->bdev_desc);
	bvdev->bdev_desc = NULL;
	bvdev->bdev = NULL;
}

static int
vhost_session_bdev_remove_cb(struct spdk_vhost_dev *vdev,
			     struct spdk_vhost_session *vsession,
			     void *ctx)
{
	struct spdk_vhost_blk_session *bvsession;
	int rc;

	bvsession = (struct spdk_vhost_blk_session *)vsession;
	if (bvsession->requestq_poller) {
		spdk_poller_unregister(&bvsession->requestq_poller);
		bvsession->requestq_poller = SPDK_POLLER_REGISTER(no_bdev_vdev_worker, bvsession, 0);
	}

	if (vsession->virtqueue[0].intr) {
		vhost_blk_session_unregister_interrupts(bvsession);
		rc = vhost_blk_session_register_interrupts(bvsession, no_bdev_vdev_vq_worker);
		if (rc) {
			SPDK_ERRLOG("%s: Interrupt register failed\n", vsession->name);
			return -1;
		}

	}

	return 0;
}

static void
bdev_remove_cb(void *remove_ctx)
{
	struct spdk_vhost_blk_dev *bvdev = remove_ctx;

	SPDK_WARNLOG("%s: hot-removing bdev - all further requests will fail.\n",
		     bvdev->vdev.name);

	spdk_vhost_lock();
	vhost_dev_foreach_session(&bvdev->vdev, vhost_session_bdev_remove_cb,
				  vhost_dev_bdev_remove_cpl_cb, NULL);
	spdk_vhost_unlock();
}

static void
bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
	      void *event_ctx)
{
	SPDK_DEBUGLOG(vhost_blk, "Bdev event: type %d, name %s\n",
		      type,
		      bdev->name);

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		SPDK_NOTICELOG("bdev name (%s) received event(SPDK_BDEV_EVENT_REMOVE)\n", bdev->name);
		bdev_remove_cb(event_ctx);
		break;
	case SPDK_BDEV_EVENT_RESIZE:
		SPDK_NOTICELOG("bdev name (%s) received event(SPDK_BDEV_EVENT_RESIZE)\n", bdev->name);
		blk_resize_cb(event_ctx);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

static void
free_task_pool(struct spdk_vhost_blk_session *bvsession)
{
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	struct spdk_vhost_virtqueue *vq;
	uint16_t i;

	for (i = 0; i < vsession->max_queues; i++) {
		vq = &vsession->virtqueue[i];
		if (vq->tasks == NULL) {
			continue;
		}

		spdk_free(vq->tasks);
		vq->tasks = NULL;
	}
}

static int
alloc_task_pool(struct spdk_vhost_blk_session *bvsession)
{
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	struct spdk_vhost_virtqueue *vq;
	struct spdk_vhost_blk_task *task;
	uint32_t task_cnt;
	uint16_t i;
	uint32_t j;

	for (i = 0; i < vsession->max_queues; i++) {
		vq = &vsession->virtqueue[i];
		if (vq->vring.desc == NULL) {
			continue;
		}

		task_cnt = vq->vring.size;
		if (task_cnt > SPDK_VHOST_MAX_VQ_SIZE) {
			/* sanity check */
			SPDK_ERRLOG("%s: virtuque %"PRIu16" is too big. (size = %"PRIu32", max = %"PRIu32")\n",
				    vsession->name, i, task_cnt, SPDK_VHOST_MAX_VQ_SIZE);
			free_task_pool(bvsession);
			return -1;
		}
		vq->tasks = spdk_zmalloc(sizeof(struct spdk_vhost_blk_task) * task_cnt,
					 SPDK_CACHE_LINE_SIZE, NULL,
					 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (vq->tasks == NULL) {
			SPDK_ERRLOG("%s: failed to allocate %"PRIu32" tasks for virtqueue %"PRIu16"\n",
				    vsession->name, task_cnt, i);
			free_task_pool(bvsession);
			return -1;
		}

		for (j = 0; j < task_cnt; j++) {
			task = &((struct spdk_vhost_blk_task *)vq->tasks)[j];
			task->bvsession = bvsession;
			task->req_idx = j;
			task->vq = vq;
		}
	}

	return 0;
}

static int
vhost_blk_start_cb(struct spdk_vhost_dev *vdev,
		   struct spdk_vhost_session *vsession, void *unused)
{
	struct spdk_vhost_blk_session *bvsession = to_blk_session(vsession);
	struct spdk_vhost_blk_dev *bvdev;
	int i, rc = 0;

	bvdev = to_blk_dev(vdev);
	assert(bvdev != NULL);
	bvsession->bvdev = bvdev;

	/* validate all I/O queues are in a contiguous index range */
	for (i = 0; i < vsession->max_queues; i++) {
		/* vring.desc and vring.desc_packed are in a union struct
		 * so q->vring.desc can replace q->vring.desc_packed.
		 */
		if (vsession->virtqueue[i].vring.desc == NULL) {
			SPDK_ERRLOG("%s: queue %"PRIu32" is empty\n", vsession->name, i);
			rc = -1;
			goto out;
		}
	}

	rc = alloc_task_pool(bvsession);
	if (rc != 0) {
		SPDK_ERRLOG("%s: failed to alloc task pool.\n", vsession->name);
		goto out;
	}

	if (bvdev->bdev) {
		bvsession->io_channel = spdk_bdev_get_io_channel(bvdev->bdev_desc);
		if (!bvsession->io_channel) {
			free_task_pool(bvsession);
			SPDK_ERRLOG("%s: I/O channel allocation failed\n", vsession->name);
			rc = -1;
			goto out;
		}
	}

	if (spdk_interrupt_mode_is_enabled()) {
		rc = vhost_blk_session_register_interrupts(bvsession,
				bvdev->bdev ? vdev_vq_worker : no_bdev_vdev_vq_worker);
		if (rc) {
			SPDK_ERRLOG("%s: Interrupt register failed\n", vsession->name);
			goto out;
		}
		SPDK_INFOLOG(vhost, "%s: started interrupt source on lcore %d\n",
			     vsession->name, spdk_env_get_current_core());
	} else {
		bvsession->requestq_poller = SPDK_POLLER_REGISTER(bvdev->bdev ? vdev_worker : no_bdev_vdev_worker,
					     bvsession, 0);
		SPDK_INFOLOG(vhost, "%s: started poller on lcore %d\n",
			     vsession->name, spdk_env_get_current_core());
	}

out:
	vhost_session_start_done(vsession, rc);
	return rc;
}

static int
vhost_blk_start(struct spdk_vhost_session *vsession)
{
	return vhost_session_send_event(vsession, vhost_blk_start_cb,
					3, "start session");
}

static int
destroy_session_poller_cb(void *arg)
{
	struct spdk_vhost_blk_session *bvsession = arg;
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	int i;

	if (vsession->task_cnt > 0) {
		return SPDK_POLLER_BUSY;
	}

	if (spdk_vhost_trylock() != 0) {
		return SPDK_POLLER_BUSY;
	}

	for (i = 0; i < vsession->max_queues; i++) {
		vsession->virtqueue[i].next_event_time = 0;
		vhost_vq_used_signal(vsession, &vsession->virtqueue[i]);
	}

	SPDK_INFOLOG(vhost, "%s: stopping poller on lcore %d\n",
		     vsession->name, spdk_env_get_current_core());

	if (bvsession->io_channel) {
		spdk_put_io_channel(bvsession->io_channel);
		bvsession->io_channel = NULL;
	}

	free_task_pool(bvsession);
	spdk_poller_unregister(&bvsession->stop_poller);
	vhost_session_stop_done(vsession, 0);

	spdk_vhost_unlock();
	return SPDK_POLLER_BUSY;
}

static int
vhost_blk_stop_cb(struct spdk_vhost_dev *vdev,
		  struct spdk_vhost_session *vsession, void *unused)
{
	struct spdk_vhost_blk_session *bvsession = to_blk_session(vsession);

	spdk_poller_unregister(&bvsession->requestq_poller);

	if (vsession->virtqueue[0].intr) {
		vhost_blk_session_unregister_interrupts(bvsession);
	}

	bvsession->stop_poller = SPDK_POLLER_REGISTER(destroy_session_poller_cb,
				 bvsession, 1000);
	return 0;
}

static int
vhost_blk_stop(struct spdk_vhost_session *vsession)
{
	return vhost_session_send_event(vsession, vhost_blk_stop_cb,
					3, "stop session");
}

static void
vhost_blk_dump_info_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct spdk_vhost_blk_dev *bvdev;

	bvdev = to_blk_dev(vdev);
	assert(bvdev != NULL);

	spdk_json_write_named_object_begin(w, "block");

	spdk_json_write_named_bool(w, "readonly", bvdev->readonly);

	spdk_json_write_name(w, "bdev");
	if (bvdev->bdev) {
		spdk_json_write_string(w, spdk_bdev_get_name(bvdev->bdev));
	} else {
		spdk_json_write_null(w);
	}

	spdk_json_write_object_end(w);
}

static void
vhost_blk_write_config_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct spdk_vhost_blk_dev *bvdev;

	bvdev = to_blk_dev(vdev);
	assert(bvdev != NULL);

	if (!bvdev->bdev) {
		return;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "vhost_create_blk_controller");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "ctrlr", vdev->name);
	spdk_json_write_named_string(w, "dev_name", spdk_bdev_get_name(bvdev->bdev));
	spdk_json_write_named_string(w, "cpumask",
				     spdk_cpuset_fmt(spdk_thread_get_cpumask(vdev->thread)));
	spdk_json_write_named_bool(w, "readonly", bvdev->readonly);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static int vhost_blk_destroy(struct spdk_vhost_dev *dev);

static int
vhost_blk_get_config(struct spdk_vhost_dev *vdev, uint8_t *config,
		     uint32_t len)
{
	struct virtio_blk_config blkcfg;
	struct spdk_vhost_blk_dev *bvdev;
	struct spdk_bdev *bdev;
	uint32_t blk_size;
	uint64_t blkcnt;

	memset(&blkcfg, 0, sizeof(blkcfg));
	bvdev = to_blk_dev(vdev);
	assert(bvdev != NULL);
	bdev = bvdev->bdev;
	if (bdev == NULL) {
		/* We can't just return -1 here as this GET_CONFIG message might
		 * be caused by a QEMU VM reboot. Returning -1 will indicate an
		 * error to QEMU, who might then decide to terminate itself.
		 * We don't want that. A simple reboot shouldn't break the system.
		 *
		 * Presenting a block device with block size 0 and block count 0
		 * doesn't cause any problems on QEMU side and the virtio-pci
		 * device is even still available inside the VM, but there will
		 * be no block device created for it - the kernel drivers will
		 * silently reject it.
		 */
		blk_size = 0;
		blkcnt = 0;
	} else {
		blk_size = spdk_bdev_get_block_size(bdev);
		blkcnt = spdk_bdev_get_num_blocks(bdev);
		if (spdk_bdev_get_buf_align(bdev) > 1) {
			blkcfg.size_max = SPDK_BDEV_LARGE_BUF_MAX_SIZE;
			blkcfg.seg_max = spdk_min(SPDK_VHOST_IOVS_MAX - 2 - 1, BDEV_IO_NUM_CHILD_IOV - 2 - 1);
		} else {
			blkcfg.size_max = 131072;
			/*  -2 for REQ and RESP and -1 for region boundary splitting */
			blkcfg.seg_max = SPDK_VHOST_IOVS_MAX - 2 - 1;
		}
	}

	blkcfg.blk_size = blk_size;
	/* minimum I/O size in blocks */
	blkcfg.min_io_size = 1;
	/* expressed in 512 Bytes sectors */
	blkcfg.capacity = (blkcnt * blk_size) / 512;
	/* QEMU can overwrite this value when started */
	blkcfg.num_queues = SPDK_VHOST_MAX_VQUEUES;

	if (bdev && spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
		/* 16MiB, expressed in 512 Bytes */
		blkcfg.max_discard_sectors = 32768;
		blkcfg.max_discard_seg = 1;
		blkcfg.discard_sector_alignment = blk_size / 512;
	}
	if (bdev && spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES)) {
		blkcfg.max_write_zeroes_sectors = 32768;
		blkcfg.max_write_zeroes_seg = 1;
	}

	memcpy(config, &blkcfg, spdk_min(len, sizeof(blkcfg)));

	return 0;
}

static const struct spdk_vhost_dev_backend vhost_blk_device_backend = {
	.session_ctx_size = sizeof(struct spdk_vhost_blk_session) - sizeof(struct spdk_vhost_session),
	.start_session =  vhost_blk_start,
	.stop_session = vhost_blk_stop,
	.vhost_get_config = vhost_blk_get_config,
	.dump_info_json = vhost_blk_dump_info_json,
	.write_config_json = vhost_blk_write_config_json,
	.remove_device = vhost_blk_destroy,
};

int
spdk_vhost_blk_construct(const char *name, const char *cpumask, const char *dev_name,
			 bool readonly, bool packed_ring)
{
	struct spdk_vhost_blk_dev *bvdev = NULL;
	struct spdk_vhost_dev *vdev;
	struct spdk_bdev *bdev;
	int ret = 0;

	spdk_vhost_lock();

	bvdev = calloc(1, sizeof(*bvdev));
	if (bvdev == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	ret = spdk_bdev_open_ext(dev_name, true, bdev_event_cb, bvdev, &bvdev->bdev_desc);
	if (ret != 0) {
		SPDK_ERRLOG("%s: could not open bdev '%s', error=%d\n",
			    name, dev_name, ret);
		goto out;
	}
	bdev = spdk_bdev_desc_get_bdev(bvdev->bdev_desc);

	vdev = &bvdev->vdev;
	vdev->virtio_features = SPDK_VHOST_BLK_FEATURES_BASE;
	vdev->disabled_features = SPDK_VHOST_BLK_DISABLED_FEATURES;
	vdev->protocol_features = SPDK_VHOST_BLK_PROTOCOL_FEATURES;

	vdev->virtio_features |= (uint64_t)packed_ring << VIRTIO_F_RING_PACKED;

	if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
		vdev->virtio_features |= (1ULL << VIRTIO_BLK_F_DISCARD);
	}
	if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES)) {
		vdev->virtio_features |= (1ULL << VIRTIO_BLK_F_WRITE_ZEROES);
	}
	if (readonly) {
		vdev->virtio_features |= (1ULL << VIRTIO_BLK_F_RO);
	}
	if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_FLUSH)) {
		vdev->virtio_features |= (1ULL << VIRTIO_BLK_F_FLUSH);
	}

	/*
	 * When starting qemu with vhost-user-blk multiqueue, the vhost device will
	 * be started/stopped many times, related to the queues num, as the
	 * vhost-user backend doesn't know the exact number of queues used for this
	 * device. The target have to stop and start the device once got a valid
	 * IO queue.
	 * When stoping and starting the vhost device, the backend bdev io device
	 * will be deleted and created repeatedly.
	 * Hold a bdev reference so that in the struct spdk_vhost_blk_dev, so that
	 * the io device will not be deleted.
	 */
	bvdev->dummy_io_channel = spdk_bdev_get_io_channel(bvdev->bdev_desc);

	bvdev->bdev = bdev;
	bvdev->readonly = readonly;
	ret = vhost_dev_register(vdev, name, cpumask, &vhost_blk_device_backend);
	if (ret != 0) {
		spdk_put_io_channel(bvdev->dummy_io_channel);
		spdk_bdev_close(bvdev->bdev_desc);
		goto out;
	}

	SPDK_INFOLOG(vhost, "%s: using bdev '%s'\n", name, dev_name);
out:
	if (ret != 0 && bvdev) {
		free(bvdev);
	}
	spdk_vhost_unlock();
	return ret;
}

static int
vhost_blk_destroy(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_blk_dev *bvdev = to_blk_dev(vdev);
	int rc;

	assert(bvdev != NULL);

	rc = vhost_dev_unregister(&bvdev->vdev);
	if (rc != 0) {
		return rc;
	}

	/* if the bdev is removed, don't need call spdk_put_io_channel. */
	if (bvdev->bdev) {
		spdk_put_io_channel(bvdev->dummy_io_channel);
	}

	if (bvdev->bdev_desc) {
		spdk_bdev_close(bvdev->bdev_desc);
		bvdev->bdev_desc = NULL;
	}
	bvdev->bdev = NULL;

	free(bvdev);
	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(vhost_blk)
SPDK_LOG_REGISTER_COMPONENT(vhost_blk_data)
