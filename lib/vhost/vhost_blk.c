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
#include "spdk/io_channel.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/vhost.h"

#include "vhost_internal.h"

struct spdk_vhost_blk_task {
	struct spdk_bdev_io *bdev_io;
	struct spdk_vhost_blk_dev *bvdev;
	struct rte_vhost_vring *vq;

	volatile uint8_t *status;

	uint16_t req_idx;

	uint32_t length;
	uint16_t iovcnt;
	struct iovec iovs[SPDK_VHOST_IOVS_MAX];
};

struct spdk_vhost_blk_dev {
	struct spdk_vhost_dev vdev;
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *bdev_desc;
	struct spdk_io_channel *bdev_io_channel;
	struct spdk_poller *requestq_poller;
	struct spdk_ring *tasks_pool;
	bool readonly;
};

static void
spdk_vhost_blk_get_tasks(struct spdk_vhost_blk_dev *bvdev, struct spdk_vhost_blk_task **tasks,
			 size_t count)
{
	size_t res_count;

	bvdev->vdev.task_cnt += count;
	res_count = spdk_ring_dequeue(bvdev->tasks_pool, (void **)tasks, count);

	/* Allocated task count in init function is equal queue depth so dequeue must not fail. */
	assert(res_count == count);

	for (res_count = 0; res_count < count; res_count++) {
		SPDK_DEBUGLOG(SPDK_TRACE_VHOST_BLK_TASK, "GET task %p\n", tasks[res_count]);
	}
}

static void
spdk_vhost_blk_put_tasks(struct spdk_vhost_blk_dev *bvdev, struct spdk_vhost_blk_task **tasks,
			 size_t count)
{
	size_t res_count;

	for (res_count = 0; res_count < count; res_count++) {
		SPDK_DEBUGLOG(SPDK_TRACE_VHOST_BLK_TASK, "PUT task %p\n", tasks[res_count]);
	}

	res_count = spdk_ring_enqueue(bvdev->tasks_pool, (void **)tasks, count);

	/* Allocated task count in init function is equal queue depth so enqueue must not fail. */
	assert(res_count == count);
	bvdev->vdev.task_cnt -= count;
}

static void
invalid_blk_request(struct spdk_vhost_blk_task *task, uint8_t status)
{
	if (task->status) {
		*task->status = status;
	}

	spdk_vhost_vq_used_ring_enqueue(&task->bvdev->vdev, task->vq, task->req_idx, 0);
	spdk_vhost_blk_put_tasks(task->bvdev, &task, 1);
	SPDK_DEBUGLOG(SPDK_TRACE_VHOST_BLK_DATA, "Invalid request (status=%" PRIu8")\n", status);
}

/*
 * Process task's descriptor chain and setup data related fields.
 * Return
 *   total size of suplied buffers
 *
 *   FIXME: Make this function return to rd_cnt and wr_cnt
 */
static int
blk_iovs_setup(struct spdk_vhost_dev *vdev, struct rte_vhost_vring *vq, uint16_t req_idx,
	       struct iovec *iovs, uint16_t *iovs_cnt, uint32_t *length)
{
	struct vring_desc *desc = spdk_vhost_vq_get_desc(vq, req_idx);
	uint16_t out_cnt = 0, cnt = 0;
	uint32_t len = 0;

	while (1) {
		/*
		 * Maximum cnt reached?
		 * Should not happen if request is well formatted, otherwise this is a BUG.
		 */
		if (spdk_unlikely(cnt == *iovs_cnt)) {
			SPDK_DEBUGLOG(SPDK_TRACE_VHOST_BLK, "Max IOVs in request reached (req_idx = %"PRIu16").\n",
				      req_idx);
			return -1;
		}

		if (spdk_unlikely(spdk_vhost_vring_desc_to_iov(vdev, iovs, &cnt, desc))) {
			SPDK_DEBUGLOG(SPDK_TRACE_VHOST_BLK, "Invalid descriptor %" PRIu16" (req_idx = %"PRIu16").\n",
				      req_idx, cnt);
			return -1;
		}

		len += desc->len;

		out_cnt += spdk_vhost_vring_desc_is_wr(desc);

		if (spdk_vhost_vring_desc_has_next(desc)) {
			desc = spdk_vhost_vring_desc_get_next(vq->desc, desc);
		} else {
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
					task->length);
	SPDK_DEBUGLOG(SPDK_TRACE_VHOST_BLK, "Finished task (%p) req_idx=%d\n status: %s\n", task,
		      task->req_idx, success ? "OK" : "FAIL");
	spdk_vhost_blk_put_tasks(task->bvdev, &task, 1);
}

static void
blk_request_complete_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_vhost_blk_task *task = cb_arg;

	spdk_bdev_free_io(bdev_io);
	blk_request_finish(success, task);
}

static int
process_blk_request(struct spdk_vhost_blk_task *task, struct spdk_vhost_blk_dev *bvdev,
		    struct rte_vhost_vring *vq,
		    uint16_t req_idx)
{
	const struct virtio_blk_outhdr *req;
	struct iovec *iov;
	uint32_t type;
	int rc;

	assert(task->bvdev == bvdev);
	task->req_idx = req_idx;
	task->vq = vq;
	task->iovcnt = SPDK_COUNTOF(task->iovs);
	task->status = NULL;

	if (blk_iovs_setup(&bvdev->vdev, vq, req_idx, task->iovs, &task->iovcnt, &task->length)) {
		SPDK_DEBUGLOG(SPDK_TRACE_VHOST_BLK, "Invalid request (req_idx = %"PRIu16").\n", req_idx);
		/* Only READ and WRITE are supported for now. */
		invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	iov = &task->iovs[0];
	if (spdk_unlikely(iov->iov_len != sizeof(*req))) {
		SPDK_DEBUGLOG(SPDK_TRACE_VHOST_BLK,
			      "First descriptor size is %zu but expected %zu (req_idx = %"PRIu16").\n",
			      iov->iov_len, sizeof(*req), req_idx);
		invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	req = iov->iov_base;

	iov = &task->iovs[task->iovcnt - 1];
	if (spdk_unlikely(iov->iov_len != 1)) {
		SPDK_DEBUGLOG(SPDK_TRACE_VHOST_BLK,
			      "Last descriptor size is %zu but expected %d (req_idx = %"PRIu16").\n",
			      iov->iov_len, 1, req_idx);
		invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	task->status = iov->iov_base;
	task->length -= sizeof(*req) + 1;
	task->iovcnt -= 2;

	type = req->type;
#ifdef VIRTIO_BLK_T_BARRIER
	/* Don't care about barier for now (as QEMU's virtio-blk do). */
	type &= ~VIRTIO_BLK_T_BARRIER;
#endif

	switch (type) {
	case VIRTIO_BLK_T_IN:
	case VIRTIO_BLK_T_OUT:
		if (spdk_unlikely((task->length & (512 - 1)) != 0)) {
			SPDK_ERRLOG("%s - passed IO buffer is not multiple of 512b (req_idx = %"PRIu16").\n",
				    type ? "WRITE" : "READ", req_idx);
			invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
			return -1;
		}

		if (type == VIRTIO_BLK_T_IN) {
			rc = spdk_bdev_readv(bvdev->bdev_desc, bvdev->bdev_io_channel,
					     &task->iovs[1], task->iovcnt, req->sector * 512,
					     task->length, blk_request_complete_cb, task);
		} else if (!bvdev->readonly) {
			rc = spdk_bdev_writev(bvdev->bdev_desc, bvdev->bdev_io_channel,
					      &task->iovs[1], task->iovcnt, req->sector * 512,
					      task->length, blk_request_complete_cb, task);
		} else {
			SPDK_DEBUGLOG(SPDK_TRACE_VHOST_BLK, "Device is in read-only mode!\n");
			rc = -1;
		}

		if (rc) {
			invalid_blk_request(task, VIRTIO_BLK_S_IOERR);
			return -1;
		}
		break;
	case VIRTIO_BLK_T_GET_ID:
		if (!task->iovcnt || !task->length) {
			invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
			return -1;
		}
		task->length = spdk_min((size_t)VIRTIO_BLK_ID_BYTES, task->iovs[1].iov_len);
		spdk_strcpy_pad(task->iovs[1].iov_base, spdk_bdev_get_product_name(bvdev->bdev), task->length, ' ');
		blk_request_finish(true, task);
		break;
	default:
		SPDK_DEBUGLOG(SPDK_TRACE_VHOST_BLK, "Not supported request type '%"PRIu32"'.\n", type);
		invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	return 0;
}

static void
process_vq(struct spdk_vhost_blk_dev *bvdev, struct rte_vhost_vring *vq)
{
	struct spdk_vhost_blk_task *tasks[32] = {0};
	int rc;
	uint16_t reqs[32];
	uint16_t reqs_cnt, i;

	reqs_cnt = spdk_vhost_vq_avail_ring_get(vq, reqs, SPDK_COUNTOF(reqs));
	if (!reqs_cnt) {
		return;
	}

	spdk_vhost_blk_get_tasks(bvdev, tasks, reqs_cnt);
	for (i = 0; i < reqs_cnt; i++) {
		SPDK_DEBUGLOG(SPDK_TRACE_VHOST_BLK, "====== Starting processing request idx %"PRIu16"======\n",
			      reqs[i]);
		rc = process_blk_request(tasks[i], bvdev, vq, reqs[i]);
		if (rc == 0) {
			SPDK_DEBUGLOG(SPDK_TRACE_VHOST_BLK, "====== Task %p req_idx %d submitted ======\n", tasks[i],
				      reqs[i]);
		} else {
			SPDK_DEBUGLOG(SPDK_TRACE_VHOST_BLK, "====== Task %p req_idx %d failed ======\n", tasks[i], reqs[i]);
		}
	}
}

static void
vdev_worker(void *arg)
{
	struct spdk_vhost_blk_dev *bvdev = arg;
	uint16_t q_idx;

	for (q_idx = 0; q_idx < bvdev->vdev.num_queues; q_idx++) {
		process_vq(bvdev, &bvdev->vdev.virtqueue[q_idx]);
	}
}

static void
no_bdev_process_vq(struct spdk_vhost_blk_dev *bvdev, struct rte_vhost_vring *vq)
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
		SPDK_DEBUGLOG(SPDK_TRACE_VHOST_BLK_DATA, "Aborting request %" PRIu16"\n", req_idx);
	}

	spdk_vhost_vq_used_ring_enqueue(&bvdev->vdev, vq, req_idx, 0);
}

static void
no_bdev_vdev_worker(void *arg)
{
	struct spdk_vhost_blk_dev *bvdev = arg;
	uint16_t q_idx;

	for (q_idx = 0; q_idx < bvdev->vdev.num_queues; q_idx++) {
		no_bdev_process_vq(bvdev, &bvdev->vdev.virtqueue[q_idx]);
	}
}

static void
add_vdev_cb(void *arg)
{
	struct spdk_vhost_blk_dev *bvdev = arg;
	struct spdk_vhost_dev *vdev = &bvdev->vdev;

	spdk_vhost_dev_mem_register(&bvdev->vdev);

	if (bvdev->bdev) {
		bvdev->bdev_io_channel = spdk_bdev_get_io_channel(bvdev->bdev_desc);
		if (!bvdev->bdev_io_channel) {
			SPDK_ERRLOG("Controller %s: IO channel allocation failed\n", vdev->name);
			abort();
		}
	}

	spdk_poller_register(&bvdev->requestq_poller, bvdev->bdev ? vdev_worker : no_bdev_vdev_worker,
			     bvdev, vdev->lcore, 0);
	SPDK_NOTICELOG("Started poller for vhost controller %s on lcore %d\n", vdev->name, vdev->lcore);
}

static void
remove_vdev_cb(void *arg)
{
	struct spdk_vhost_blk_dev *bvdev = arg;

	SPDK_NOTICELOG("Stopping poller for vhost controller %s\n", bvdev->vdev.name);

	if (bvdev->bdev_io_channel) {
		spdk_put_io_channel(bvdev->bdev_io_channel);
		bvdev->bdev_io_channel = NULL;
	}

	spdk_vhost_dev_mem_unregister(&bvdev->vdev);
}

static struct spdk_vhost_blk_dev *
to_blk_dev(struct spdk_vhost_dev *vdev)
{
	if (vdev == NULL) {
		return NULL;
	}

	if (vdev->type != SPDK_VHOST_DEV_T_BLK) {
		SPDK_ERRLOG("Controller %s: expected block controller (%d) but got %d\n",
			    vdev->name, SPDK_VHOST_DEV_T_BLK, vdev->type);
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

static void
bdev_remove_cb(void *remove_ctx)
{
	struct spdk_vhost_blk_dev *bvdev = remove_ctx;

	if (bvdev->vdev.lcore != -1 && (uint32_t)bvdev->vdev.lcore != spdk_env_get_current_core()) {
		/* Call self on proper core. */
		spdk_vhost_timed_event_send(bvdev->vdev.lcore, bdev_remove_cb, bvdev, 1, "vhost blk hot remove");
		return;
	}

	SPDK_WARNLOG("Controller %s: Hot-removing bdev - all further requests will fail.\n",
		     bvdev->vdev.name);
	if (bvdev->requestq_poller) {
		spdk_poller_unregister(&bvdev->requestq_poller, NULL);
		spdk_poller_register(&bvdev->requestq_poller, no_bdev_vdev_worker, bvdev, bvdev->vdev.lcore, 0);
	}

	bvdev->bdev = NULL;
}


static void
free_task_pool(struct spdk_vhost_blk_dev *bvdev)
{
	struct spdk_vhost_task *task;

	if (!bvdev->tasks_pool) {
		return;
	}

	while (spdk_ring_dequeue(bvdev->tasks_pool, (void **)&task, 1) == 1) {
		spdk_dma_free(task);
	}

	spdk_ring_free(bvdev->tasks_pool);
	bvdev->tasks_pool = NULL;
}

static int
alloc_task_pool(struct spdk_vhost_blk_dev *bvdev)
{
	struct spdk_vhost_blk_task *task;
	uint32_t task_cnt = 0;
	uint32_t ring_size, socket_id;
	uint16_t i;
	int rc;

	for (i = 0; i < bvdev->vdev.num_queues; i++) {
		/*
		 * FIXME:
		 * this is too big because we need only size/2 from each queue but for now
		 * lets leave it as is to be sure we are not mistaken.
		 *
		 * Limit the pool size to 1024 * num_queues. This should be enough as QEMU have the
		 * same hard limit for queue size.
		 */
		task_cnt += spdk_min(bvdev->vdev.virtqueue[i].size, 1024);
	}

	ring_size = spdk_align32pow2(task_cnt + 1);
	socket_id = spdk_env_get_socket_id(bvdev->vdev.lcore);

	bvdev->tasks_pool = spdk_ring_create(SPDK_RING_TYPE_SP_SC, ring_size, socket_id);
	if (bvdev->tasks_pool == NULL) {
		SPDK_ERRLOG("Controller %s: Failed to init vhost blk task pool\n", bvdev->vdev.name);
		return -1;
	}

	for (i = 0; i < task_cnt; ++i) {
		task = spdk_dma_malloc_socket(sizeof(*task), SPDK_CACHE_LINE_SIZE, NULL, socket_id);
		if (task == NULL) {
			SPDK_ERRLOG("Controller %s: Failed to allocate task\n", bvdev->vdev.name);
			free_task_pool(bvdev);
			return -1;
		}

		task->bvdev = bvdev;

		rc = spdk_ring_enqueue(bvdev->tasks_pool, (void **)&task, 1);
		if (rc != 1) {
			SPDK_ERRLOG("Controller %s: Failed to enqueue %"PRIu32" vhost blk tasks\n", bvdev->vdev.name,
				    task_cnt);
			free_task_pool(bvdev);
			return -1;
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
new_device(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_blk_dev *bvdev;
	int rc = -1;

	bvdev = to_blk_dev(vdev);
	if (bvdev == NULL) {
		SPDK_ERRLOG("Trying to start non-blk controller as a blk one.\n");
		return -1;
	} else if (bvdev == NULL) {
		SPDK_ERRLOG("Trying to start non-blk controller as blk one.\n");
		return -1;
	}

	rc = alloc_task_pool(bvdev);
	if (rc != 0) {
		SPDK_ERRLOG("%s: failed to alloc task pool.\n", bvdev->vdev.name);
		return -1;
	}

	spdk_vhost_timed_event_send(bvdev->vdev.lcore, add_vdev_cb, bvdev, 1, "add blk vdev");

	return 0;
}

static int
destroy_device(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_blk_dev *bvdev;
	struct spdk_vhost_timed_event event = {0};
	uint32_t i;

	bvdev = to_blk_dev(vdev);
	if (bvdev == NULL) {
		SPDK_ERRLOG("Trying to stop non-blk controller as a blk one.\n");
		return -1;
	}

	spdk_vhost_timed_event_init(&event, vdev->lcore, NULL, NULL, 1);
	spdk_poller_unregister(&bvdev->requestq_poller, event.spdk_event);
	spdk_vhost_timed_event_wait(&event, "unregister poller");

	/* Wait for all tasks to finish */
	for (i = 1000; i && vdev->task_cnt > 0; i--) {
		usleep(1000);
	}

	if (vdev->task_cnt > 0) {
		SPDK_ERRLOG("%s: pending tasks did not finish in 1s.\n", vdev->name);
		abort();
	}

	spdk_vhost_timed_event_send(vdev->lcore, remove_vdev_cb, bvdev, 1, "remove vdev");

	free_task_pool(bvdev);
	return 0;
}

static void
spdk_vhost_blk_dump_config_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct spdk_bdev *bdev = spdk_vhost_blk_get_dev(vdev);
	struct spdk_vhost_blk_dev *bvdev = to_blk_dev(vdev);

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
	.new_device =  new_device,
	.destroy_device = destroy_device,
	.dump_config_json = spdk_vhost_blk_dump_config_json,
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
	struct spdk_vhost_blk_dev *bvdev;
	struct spdk_bdev *bdev;
	int ret;

	bdev = spdk_bdev_get_by_name(dev_name);
	if (bdev == NULL) {
		SPDK_ERRLOG("Controller %s: bdev '%s' not found\n",
			    name, dev_name);
		return -1;
	}

	bvdev = spdk_dma_zmalloc(sizeof(*bvdev), SPDK_CACHE_LINE_SIZE, NULL);
	if (bvdev == NULL) {
		return -1;
	}

	ret = spdk_bdev_open(bdev, true, bdev_remove_cb, bvdev, &bvdev->bdev_desc);
	if (ret != 0) {
		SPDK_ERRLOG("Controller %s: could not open bdev '%s', error=%d\n",
			    name, dev_name, ret);
		goto err;
	}

	bvdev->bdev = bdev;
	bvdev->readonly = readonly;
	ret = spdk_vhost_dev_construct(&bvdev->vdev, name, cpumask, SPDK_VHOST_DEV_T_BLK,
				       &vhost_blk_device_backend);
	if (ret != 0) {
		spdk_bdev_close(bvdev->bdev_desc);
		goto err;
	}

	if (readonly && rte_vhost_driver_enable_features(bvdev->vdev.path, (1ULL << VIRTIO_BLK_F_RO))) {
		SPDK_ERRLOG("Controller %s: failed to set as a readonly\n", name);
		spdk_bdev_close(bvdev->bdev_desc);

		if (spdk_vhost_dev_remove(&bvdev->vdev) != 0) {
			SPDK_ERRLOG("Controller %s: failed to remove controller\n", name);
		}

		goto err;
	}

	SPDK_NOTICELOG("Controller %s: using bdev '%s'\n",
		       name, dev_name);

	return 0;

err:
	spdk_dma_free(bvdev);
	return -1;
}

int
spdk_vhost_blk_destroy(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_blk_dev *bvdev = to_blk_dev(vdev);

	if (!bvdev) {
		return -EINVAL;
	}

	spdk_bdev_close(bvdev->bdev_desc);
	bvdev->bdev = NULL;

	SPDK_NOTICELOG("Controller %s: removed device\n", vdev->name);

	if (spdk_vhost_dev_remove(&bvdev->vdev)) {
		return -EIO;
	}

	spdk_dma_free(bvdev);
	return 0;
}

SPDK_LOG_REGISTER_TRACE_FLAG("vhost_blk", SPDK_TRACE_VHOST_BLK)
SPDK_LOG_REGISTER_TRACE_FLAG("vhost_blk_task", SPDK_TRACE_VHOST_BLK_TASK)
SPDK_LOG_REGISTER_TRACE_FLAG("vhost_blk_data", SPDK_TRACE_VHOST_BLK_DATA)
