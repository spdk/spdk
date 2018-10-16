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
#include "spdk/conf.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/vhost.h"

#include "vhost_internal.h"

struct spdk_vhost_blk_task {
	struct spdk_bdev_io *bdev_io;
	struct spdk_vhost_blk_dev *bvdev;
	struct spdk_vhost_virtqueue *vq;

	volatile uint8_t *status;

	uint16_t req_idx;

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
	struct spdk_io_channel *bdev_io_channel;
	struct spdk_poller *requestq_poller;
	struct spdk_vhost_dev_destroy_ctx destroy_ctx;
	bool readonly;
};

/* forward declaration */
static const struct spdk_vhost_dev_backend vhost_blk_device_backend;

static int
process_blk_request(struct spdk_vhost_blk_task *task, struct spdk_vhost_blk_dev *bvdev,
		    struct spdk_vhost_virtqueue *vq);

static void
blk_task_finish(struct spdk_vhost_blk_task *task)
{
	assert(task->bvdev->vdev.task_cnt > 0);
	task->bvdev->vdev.task_cnt--;
	task->used = false;
}

static void
invalid_blk_request(struct spdk_vhost_blk_task *task, uint8_t status)
{
	if (task->status) {
		*task->status = status;
	}

	spdk_vhost_vq_used_ring_enqueue(&task->bvdev->vdev, task->vq, task->req_idx,
					task->used_len);
	blk_task_finish(task);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_BLK_DATA, "Invalid request (status=%" PRIu8")\n", status);
}

/*
 * Process task's descriptor chain and setup data related fields.
 * Return
 *   total size of suplied buffers
 *
 *   FIXME: Make this function return to rd_cnt and wr_cnt
 */
static int
blk_iovs_setup(struct spdk_vhost_dev *vdev, struct spdk_vhost_virtqueue *vq, uint16_t req_idx,
	       struct iovec *iovs, uint16_t *iovs_cnt, uint32_t *length)
{
	struct vring_desc *desc, *desc_table;
	uint16_t out_cnt = 0, cnt = 0;
	uint32_t desc_table_size, len = 0;
	int rc;

	rc = spdk_vhost_vq_get_desc(vdev, vq, req_idx, &desc, &desc_table, &desc_table_size);
	if (rc != 0) {
		SPDK_ERRLOG("%s: Invalid descriptor at index %"PRIu16".\n", vdev->name, req_idx);
		return -1;
	}

	while (1) {
		/*
		 * Maximum cnt reached?
		 * Should not happen if request is well formatted, otherwise this is a BUG.
		 */
		if (spdk_unlikely(cnt == *iovs_cnt)) {
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_BLK, "Max IOVs in request reached (req_idx = %"PRIu16").\n",
				      req_idx);
			return -1;
		}

		if (spdk_unlikely(spdk_vhost_vring_desc_to_iov(vdev, iovs, &cnt, desc))) {
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_BLK, "Invalid descriptor %" PRIu16" (req_idx = %"PRIu16").\n",
				      req_idx, cnt);
			return -1;
		}

		len += desc->len;

		out_cnt += spdk_vhost_vring_desc_is_wr(desc);

		rc = spdk_vhost_vring_desc_get_next(&desc, desc_table, desc_table_size);
		if (rc != 0) {
			SPDK_ERRLOG("%s: Descriptor chain at index %"PRIu16" terminated unexpectedly.\n",
				    vdev->name, req_idx);
			return -1;
		} else if (desc == NULL) {
			break;
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

static void
blk_request_finish(bool success, struct spdk_vhost_blk_task *task)
{
	*task->status = success ? VIRTIO_BLK_S_OK : VIRTIO_BLK_S_IOERR;
	spdk_vhost_vq_used_ring_enqueue(&task->bvdev->vdev, task->vq, task->req_idx,
					task->used_len);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_BLK, "Finished task (%p) req_idx=%d\n status: %s\n", task,
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

	rc = process_blk_request(task, task->bvdev, task->vq);
	if (rc == 0) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_BLK, "====== Task %p resubmitted ======\n", task);
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_BLK, "====== Task %p failed ======\n", task);
	}
}

static inline void
blk_request_queue_io(struct spdk_vhost_blk_task *task)
{
	int rc;
	struct spdk_vhost_blk_dev *bvdev = task->bvdev;
	struct spdk_bdev *bdev = bvdev->bdev;

	task->bdev_io_wait.bdev = bdev;
	task->bdev_io_wait.cb_fn = blk_request_resubmit;
	task->bdev_io_wait.cb_arg = task;

	rc = spdk_bdev_queue_io_wait(bdev, bvdev->bdev_io_channel, &task->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("Queue io failed in vhost_blk, rc=%d\n", rc);
		invalid_blk_request(task, VIRTIO_BLK_S_IOERR);
	}
}

static int
process_blk_request(struct spdk_vhost_blk_task *task, struct spdk_vhost_blk_dev *bvdev,
		    struct spdk_vhost_virtqueue *vq)
{
	const struct virtio_blk_outhdr *req;
	struct iovec *iov;
	uint32_t type;
	uint32_t payload_len;
	int rc;

	if (blk_iovs_setup(&bvdev->vdev, vq, task->req_idx, task->iovs, &task->iovcnt, &payload_len)) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_BLK, "Invalid request (req_idx = %"PRIu16").\n", task->req_idx);
		/* Only READ and WRITE are supported for now. */
		invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	iov = &task->iovs[0];
	if (spdk_unlikely(iov->iov_len != sizeof(*req))) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_BLK,
			      "First descriptor size is %zu but expected %zu (req_idx = %"PRIu16").\n",
			      iov->iov_len, sizeof(*req), task->req_idx);
		invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	req = iov->iov_base;

	iov = &task->iovs[task->iovcnt - 1];
	if (spdk_unlikely(iov->iov_len != 1)) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_BLK,
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
		if (spdk_unlikely((payload_len & (512 - 1)) != 0)) {
			SPDK_ERRLOG("%s - passed IO buffer is not multiple of 512b (req_idx = %"PRIu16").\n",
				    type ? "WRITE" : "READ", task->req_idx);
			invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
			return -1;
		}

		if (type == VIRTIO_BLK_T_IN) {
			task->used_len = payload_len + sizeof(*task->status);
			rc = spdk_bdev_readv(bvdev->bdev_desc, bvdev->bdev_io_channel,
					     &task->iovs[1], task->iovcnt, req->sector * 512,
					     payload_len, blk_request_complete_cb, task);
		} else if (!bvdev->readonly) {
			task->used_len = sizeof(*task->status);
			rc = spdk_bdev_writev(bvdev->bdev_desc, bvdev->bdev_io_channel,
					      &task->iovs[1], task->iovcnt, req->sector * 512,
					      payload_len, blk_request_complete_cb, task);
		} else {
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_BLK, "Device is in read-only mode!\n");
			rc = -1;
		}

		if (rc) {
			if (rc == -ENOMEM) {
				SPDK_DEBUGLOG(SPDK_LOG_VHOST_BLK, "No memory, start to queue io.\n");
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
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_BLK, "Not supported request type '%"PRIu32"'.\n", type);
		invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	return 0;
}

static void
process_vq(struct spdk_vhost_blk_dev *bvdev, struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_blk_task *task;
	int rc;
	uint16_t reqs[32];
	uint16_t reqs_cnt, i;

	reqs_cnt = spdk_vhost_vq_avail_ring_get(vq, reqs, SPDK_COUNTOF(reqs));
	if (!reqs_cnt) {
		return;
	}

	for (i = 0; i < reqs_cnt; i++) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_BLK, "====== Starting processing request idx %"PRIu16"======\n",
			      reqs[i]);

		if (spdk_unlikely(reqs[i] >= vq->vring.size)) {
			SPDK_ERRLOG("%s: request idx '%"PRIu16"' exceeds virtqueue size (%"PRIu16").\n",
				    bvdev->vdev.name, reqs[i], vq->vring.size);
			spdk_vhost_vq_used_ring_enqueue(&bvdev->vdev, vq, reqs[i], 0);
			continue;
		}

		task = &((struct spdk_vhost_blk_task *)vq->tasks)[reqs[i]];
		if (spdk_unlikely(task->used)) {
			SPDK_ERRLOG("%s: request with idx '%"PRIu16"' is already pending.\n",
				    bvdev->vdev.name, reqs[i]);
			spdk_vhost_vq_used_ring_enqueue(&bvdev->vdev, vq, reqs[i], 0);
			continue;
		}

		bvdev->vdev.task_cnt++;

		task->used = true;
		task->iovcnt = SPDK_COUNTOF(task->iovs);
		task->status = NULL;
		task->used_len = 0;

		rc = process_blk_request(task, bvdev, vq);
		if (rc == 0) {
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_BLK, "====== Task %p req_idx %d submitted ======\n", task,
				      reqs[i]);
		} else {
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_BLK, "====== Task %p req_idx %d failed ======\n", task, reqs[i]);
		}
	}
}

static int
vdev_worker(void *arg)
{
	struct spdk_vhost_blk_dev *bvdev = arg;
	uint16_t q_idx;

	for (q_idx = 0; q_idx < bvdev->vdev.max_queues; q_idx++) {
		process_vq(bvdev, &bvdev->vdev.virtqueue[q_idx]);
	}

	spdk_vhost_dev_used_signal(&bvdev->vdev);

	return -1;
}

static void
no_bdev_process_vq(struct spdk_vhost_blk_dev *bvdev, struct spdk_vhost_virtqueue *vq)
{
	struct iovec iovs[SPDK_VHOST_IOVS_MAX];
	uint32_t length;
	uint16_t iovcnt, req_idx;

	if (spdk_vhost_vq_avail_ring_get(vq, &req_idx, 1) != 1) {
		return;
	}

	iovcnt = SPDK_COUNTOF(iovs);
	if (blk_iovs_setup(&bvdev->vdev, vq, req_idx, iovs, &iovcnt, &length) == 0) {
		*(volatile uint8_t *)iovs[iovcnt - 1].iov_base = VIRTIO_BLK_S_IOERR;
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_BLK_DATA, "Aborting request %" PRIu16"\n", req_idx);
	}

	spdk_vhost_vq_used_ring_enqueue(&bvdev->vdev, vq, req_idx, 0);
}

static int
no_bdev_vdev_worker(void *arg)
{
	struct spdk_vhost_blk_dev *bvdev = arg;
	uint16_t q_idx;

	for (q_idx = 0; q_idx < bvdev->vdev.max_queues; q_idx++) {
		no_bdev_process_vq(bvdev, &bvdev->vdev.virtqueue[q_idx]);
	}

	spdk_vhost_dev_used_signal(&bvdev->vdev);

	if (bvdev->vdev.task_cnt == 0 && bvdev->bdev_io_channel) {
		spdk_put_io_channel(bvdev->bdev_io_channel);
		bvdev->bdev_io_channel = NULL;
	}

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

struct spdk_bdev *
spdk_vhost_blk_get_dev(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_blk_dev *bvdev = to_blk_dev(vdev);

	assert(bvdev != NULL);
	return bvdev->bdev;
}

static int
_bdev_remove_cb(struct spdk_vhost_dev *vdev, void *arg)
{
	struct spdk_vhost_blk_dev *bvdev = arg;

	SPDK_WARNLOG("Controller %s: Hot-removing bdev - all further requests will fail.\n",
		     bvdev->vdev.name);
	if (bvdev->requestq_poller) {
		spdk_poller_unregister(&bvdev->requestq_poller);
		bvdev->requestq_poller = spdk_poller_register(no_bdev_vdev_worker, bvdev, 0);
	}

	spdk_bdev_close(bvdev->bdev_desc);
	bvdev->bdev_desc = NULL;
	bvdev->bdev = NULL;
	return 0;
}

static void
bdev_remove_cb(void *remove_ctx)
{
	struct spdk_vhost_blk_dev *bvdev = remove_ctx;

	spdk_vhost_call_external_event(bvdev->vdev.name, _bdev_remove_cb, bvdev);
}

static void
free_task_pool(struct spdk_vhost_blk_dev *bvdev)
{
	struct spdk_vhost_virtqueue *vq;
	uint16_t i;

	for (i = 0; i < bvdev->vdev.max_queues; i++) {
		vq = &bvdev->vdev.virtqueue[i];
		if (vq->tasks == NULL) {
			continue;
		}

		spdk_dma_free(vq->tasks);
		vq->tasks = NULL;
	}
}

static int
alloc_task_pool(struct spdk_vhost_blk_dev *bvdev)
{
	struct spdk_vhost_virtqueue *vq;
	struct spdk_vhost_blk_task *task;
	uint32_t task_cnt;
	uint16_t i;
	uint32_t j;

	for (i = 0; i < bvdev->vdev.max_queues; i++) {
		vq = &bvdev->vdev.virtqueue[i];
		if (vq->vring.desc == NULL) {
			continue;
		}

		task_cnt = vq->vring.size;
		if (task_cnt > SPDK_VHOST_MAX_VQ_SIZE) {
			/* sanity check */
			SPDK_ERRLOG("Controller %s: virtuque %"PRIu16" is too big. (size = %"PRIu32", max = %"PRIu32")\n",
				    bvdev->vdev.name, i, task_cnt, SPDK_VHOST_MAX_VQ_SIZE);
			free_task_pool(bvdev);
			return -1;
		}
		vq->tasks = spdk_dma_zmalloc(sizeof(struct spdk_vhost_blk_task) * task_cnt,
					     SPDK_CACHE_LINE_SIZE, NULL);
		if (vq->tasks == NULL) {
			SPDK_ERRLOG("Controller %s: failed to allocate %"PRIu32" tasks for virtqueue %"PRIu16"\n",
				    bvdev->vdev.name, task_cnt, i);
			free_task_pool(bvdev);
			return -1;
		}

		for (j = 0; j < task_cnt; j++) {
			task = &((struct spdk_vhost_blk_task *)vq->tasks)[j];
			task->bvdev = bvdev;
			task->req_idx = j;
			task->vq = vq;
		}
	}

	return 0;
}

/*
 * A new device is added to a data core. First the device is added to the main linked list
 * and then allocated to a specific data core.
 *
 */
static int
spdk_vhost_blk_start(struct spdk_vhost_dev *vdev, void *event_ctx)
{
	struct spdk_vhost_blk_dev *bvdev;
	int i, rc = 0;

	bvdev = to_blk_dev(vdev);
	if (bvdev == NULL) {
		SPDK_ERRLOG("Trying to start non-blk controller as a blk one.\n");
		rc = -1;
		goto out;
	}

	/* validate all I/O queues are in a contiguous index range */
	for (i = 0; i < vdev->max_queues; i++) {
		if (vdev->virtqueue[i].vring.desc == NULL) {
			SPDK_ERRLOG("%s: queue %"PRIu32" is empty\n", vdev->name, i);
			rc = -1;
			goto out;
		}
	}

	rc = alloc_task_pool(bvdev);
	if (rc != 0) {
		SPDK_ERRLOG("%s: failed to alloc task pool.\n", bvdev->vdev.name);
		goto out;
	}

	if (bvdev->bdev) {
		bvdev->bdev_io_channel = spdk_bdev_get_io_channel(bvdev->bdev_desc);
		if (!bvdev->bdev_io_channel) {
			free_task_pool(bvdev);
			SPDK_ERRLOG("Controller %s: IO channel allocation failed\n", vdev->name);
			rc = -1;
			goto out;
		}
	}

	bvdev->requestq_poller = spdk_poller_register(bvdev->bdev ? vdev_worker : no_bdev_vdev_worker,
				 bvdev, 0);
	SPDK_INFOLOG(SPDK_LOG_VHOST, "Started poller for vhost controller %s on lcore %d\n",
		     vdev->name, vdev->lcore);
out:
	spdk_vhost_dev_backend_event_done(event_ctx, rc);
	return rc;
}

static int
destroy_device_poller_cb(void *arg)
{
	struct spdk_vhost_blk_dev *bvdev = arg;
	int i;

	if (bvdev->vdev.task_cnt > 0) {
		return -1;
	}

	for (i = 0; i < bvdev->vdev.max_queues; i++) {
		bvdev->vdev.virtqueue[i].next_event_time = 0;
		spdk_vhost_vq_used_signal(&bvdev->vdev, &bvdev->vdev.virtqueue[i]);
	}

	SPDK_INFOLOG(SPDK_LOG_VHOST, "Stopping poller for vhost controller %s\n", bvdev->vdev.name);

	if (bvdev->bdev_io_channel) {
		spdk_put_io_channel(bvdev->bdev_io_channel);
		bvdev->bdev_io_channel = NULL;
	}

	free_task_pool(bvdev);
	spdk_poller_unregister(&bvdev->destroy_ctx.poller);
	spdk_vhost_dev_backend_event_done(bvdev->destroy_ctx.event_ctx, 0);

	return -1;
}

static int
spdk_vhost_blk_stop(struct spdk_vhost_dev *vdev, void *event_ctx)
{
	struct spdk_vhost_blk_dev *bvdev;

	bvdev = to_blk_dev(vdev);
	if (bvdev == NULL) {
		SPDK_ERRLOG("Trying to stop non-blk controller as a blk one.\n");
		goto err;
	}

	bvdev->destroy_ctx.event_ctx = event_ctx;
	spdk_poller_unregister(&bvdev->requestq_poller);
	bvdev->destroy_ctx.poller = spdk_poller_register(destroy_device_poller_cb,
				    bvdev, 1000);
	return 0;

err:
	spdk_vhost_dev_backend_event_done(event_ctx, -1);
	return -1;
}

static void
spdk_vhost_blk_dump_info_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct spdk_bdev *bdev = spdk_vhost_blk_get_dev(vdev);
	struct spdk_vhost_blk_dev *bvdev;

	bvdev = to_blk_dev(vdev);
	if (bvdev == NULL) {
		return;
	}

	assert(bvdev != NULL);
	spdk_json_write_name(w, "block");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "readonly");
	spdk_json_write_bool(w, bvdev->readonly);

	spdk_json_write_name(w, "bdev");
	if (bdev) {
		spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	} else {
		spdk_json_write_null(w);
	}

	spdk_json_write_object_end(w);
}

static void
spdk_vhost_blk_write_config_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct spdk_vhost_blk_dev *bvdev;

	bvdev = to_blk_dev(vdev);
	if (bvdev == NULL) {
		return;
	}

	if (!bvdev->bdev) {
		return;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "construct_vhost_blk_controller");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "ctrlr", vdev->name);
	spdk_json_write_named_string(w, "dev_name", spdk_bdev_get_name(bvdev->bdev));
	spdk_json_write_named_string(w, "cpumask", spdk_cpuset_fmt(vdev->cpumask));
	spdk_json_write_named_bool(w, "readonly", bvdev->readonly);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static int spdk_vhost_blk_destroy(struct spdk_vhost_dev *dev);

static int
spdk_vhost_blk_get_config(struct spdk_vhost_dev *vdev, uint8_t *config,
			  uint32_t len)
{
	struct virtio_blk_config *blkcfg = (struct virtio_blk_config *)config;
	struct spdk_vhost_blk_dev *bvdev;
	struct spdk_bdev *bdev;
	uint32_t blk_size;
	uint64_t blkcnt;

	bvdev = to_blk_dev(vdev);
	if (bvdev == NULL) {
		SPDK_ERRLOG("Trying to get virito_blk configuration failed\n");
		return -1;
	}

	if (len < sizeof(*blkcfg)) {
		return -1;
	}

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
	}

	memset(blkcfg, 0, sizeof(*blkcfg));
	blkcfg->blk_size = blk_size;
	/* minimum I/O size in blocks */
	blkcfg->min_io_size = 1;
	/* expressed in 512 Bytes sectors */
	blkcfg->capacity = (blkcnt * blk_size) / 512;
	blkcfg->size_max = 131072;
	/*  -2 for REQ and RESP and -1 for region boundary splitting */
	blkcfg->seg_max = SPDK_VHOST_IOVS_MAX - 2 - 1;
	/* QEMU can overwrite this value when started */
	blkcfg->num_queues = SPDK_VHOST_MAX_VQUEUES;

	return 0;
}

static const struct spdk_vhost_dev_backend vhost_blk_device_backend = {
	.virtio_features = SPDK_VHOST_FEATURES |
	(1ULL << VIRTIO_BLK_F_SIZE_MAX) | (1ULL << VIRTIO_BLK_F_SEG_MAX) |
	(1ULL << VIRTIO_BLK_F_GEOMETRY) | (1ULL << VIRTIO_BLK_F_RO) |
	(1ULL << VIRTIO_BLK_F_BLK_SIZE) | (1ULL << VIRTIO_BLK_F_TOPOLOGY) |
	(1ULL << VIRTIO_BLK_F_BARRIER)  | (1ULL << VIRTIO_BLK_F_SCSI) |
	(1ULL << VIRTIO_BLK_F_FLUSH)    | (1ULL << VIRTIO_BLK_F_CONFIG_WCE) |
	(1ULL << VIRTIO_BLK_F_MQ),
	.disabled_features = SPDK_VHOST_DISABLED_FEATURES | (1ULL << VIRTIO_BLK_F_GEOMETRY) |
	(1ULL << VIRTIO_BLK_F_RO) | (1ULL << VIRTIO_BLK_F_FLUSH) | (1ULL << VIRTIO_BLK_F_CONFIG_WCE) |
	(1ULL << VIRTIO_BLK_F_BARRIER) | (1ULL << VIRTIO_BLK_F_SCSI),
	.start_device =  spdk_vhost_blk_start,
	.stop_device = spdk_vhost_blk_stop,
	.vhost_get_config = spdk_vhost_blk_get_config,
	.dump_info_json = spdk_vhost_blk_dump_info_json,
	.write_config_json = spdk_vhost_blk_write_config_json,
	.remove_device = spdk_vhost_blk_destroy,
};

int
spdk_vhost_blk_controller_construct(void)
{
	struct spdk_conf_section *sp;
	unsigned ctrlr_num;
	char *bdev_name;
	char *cpumask;
	char *name;
	bool readonly;

	for (sp = spdk_conf_first_section(NULL); sp != NULL; sp = spdk_conf_next_section(sp)) {
		if (!spdk_conf_section_match_prefix(sp, "VhostBlk")) {
			continue;
		}

		if (sscanf(spdk_conf_section_get_name(sp), "VhostBlk%u", &ctrlr_num) != 1) {
			SPDK_ERRLOG("Section '%s' has non-numeric suffix.\n",
				    spdk_conf_section_get_name(sp));
			return -1;
		}

		name = spdk_conf_section_get_val(sp, "Name");
		if (name == NULL) {
			SPDK_ERRLOG("VhostBlk%u: missing Name\n", ctrlr_num);
			return -1;
		}

		cpumask = spdk_conf_section_get_val(sp, "Cpumask");
		readonly = spdk_conf_section_get_boolval(sp, "ReadOnly", false);

		bdev_name = spdk_conf_section_get_val(sp, "Dev");
		if (bdev_name == NULL) {
			continue;
		}

		if (spdk_vhost_blk_construct(name, cpumask, bdev_name, readonly) < 0) {
			return -1;
		}
	}

	return 0;
}

int
spdk_vhost_blk_construct(const char *name, const char *cpumask, const char *dev_name, bool readonly)
{
	struct spdk_vhost_blk_dev *bvdev = NULL;
	struct spdk_bdev *bdev;
	int ret = 0;

	spdk_vhost_lock();
	bdev = spdk_bdev_get_by_name(dev_name);
	if (bdev == NULL) {
		SPDK_ERRLOG("Controller %s: bdev '%s' not found\n",
			    name, dev_name);
		ret = -ENODEV;
		goto out;
	}

	bvdev = spdk_dma_zmalloc(sizeof(*bvdev), SPDK_CACHE_LINE_SIZE, NULL);
	if (bvdev == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	ret = spdk_bdev_open(bdev, true, bdev_remove_cb, bvdev, &bvdev->bdev_desc);
	if (ret != 0) {
		SPDK_ERRLOG("Controller %s: could not open bdev '%s', error=%d\n",
			    name, dev_name, ret);
		goto out;
	}

	bvdev->bdev = bdev;
	bvdev->readonly = readonly;
	ret = spdk_vhost_dev_register(&bvdev->vdev, name, cpumask, &vhost_blk_device_backend);
	if (ret != 0) {
		spdk_bdev_close(bvdev->bdev_desc);
		goto out;
	}

	if (readonly && rte_vhost_driver_enable_features(bvdev->vdev.path, (1ULL << VIRTIO_BLK_F_RO))) {
		SPDK_ERRLOG("Controller %s: failed to set as a readonly\n", name);
		spdk_bdev_close(bvdev->bdev_desc);

		if (spdk_vhost_dev_unregister(&bvdev->vdev) != 0) {
			SPDK_ERRLOG("Controller %s: failed to remove controller\n", name);
		}

		ret = -1;
		goto out;
	}

	SPDK_INFOLOG(SPDK_LOG_VHOST, "Controller %s: using bdev '%s'\n", name, dev_name);
out:
	if (ret != 0 && bvdev) {
		spdk_dma_free(bvdev);
	}
	spdk_vhost_unlock();
	return ret;
}

static int
spdk_vhost_blk_destroy(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_blk_dev *bvdev = to_blk_dev(vdev);
	int rc;

	if (!bvdev) {
		return -EINVAL;
	}

	rc = spdk_vhost_dev_unregister(&bvdev->vdev);
	if (rc != 0) {
		return rc;
	}

	if (bvdev->bdev_desc) {
		spdk_bdev_close(bvdev->bdev_desc);
		bvdev->bdev_desc = NULL;
	}
	bvdev->bdev = NULL;

	spdk_dma_free(bvdev);
	return 0;
}

SPDK_LOG_REGISTER_COMPONENT("vhost_blk", SPDK_LOG_VHOST_BLK)
SPDK_LOG_REGISTER_COMPONENT("vhost_blk_data", SPDK_LOG_VHOST_BLK_DATA)
