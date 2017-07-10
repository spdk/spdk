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
#include "vhost_iommu.h"

#define VHOST_BLK_IOVS_MAX 128

struct spdk_vhost_blk_task {
	struct spdk_bdev_io *bdev_io;
	struct spdk_vhost_blk_dev *bvdev;
	volatile uint8_t *status;

	uint16_t req_idx;

	uint32_t length;
	uint16_t iovcnt;
	struct iovec iovs[VHOST_BLK_IOVS_MAX];
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
		SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK_TASK, "GET task %p\n", tasks[res_count]);
	}
}

static void
spdk_vhost_blk_put_tasks(struct spdk_vhost_blk_dev *bvdev, struct spdk_vhost_blk_task **tasks,
			 size_t count)
{
	size_t res_count;

	for (res_count = 0; res_count < count; res_count++) {
		SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK_TASK, "PUT task %p\n", tasks[res_count]);
	}

	res_count = spdk_ring_enqueue(bvdev->tasks_pool, (void **)tasks, count);

	/* Allocated task count in init function is equal queue depth so enqueue must not fail. */
	assert(res_count == count);
	bvdev->vdev.task_cnt -= count;
}

static void
invalid_blk_request(struct spdk_vhost_blk_task *task, uint8_t status)
{
	*task->status = status;
	spdk_vhost_vq_used_ring_enqueue(&task->bvdev->vdev, &task->bvdev->vdev.virtqueue[0], task->req_idx,
					0);
	spdk_vhost_blk_put_tasks(task->bvdev, &task, 1);
	SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK_DATA, "Invalid request (status=%" PRIu8")\n", status);
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
			SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK, "Max IOVs in request reached (req_idx = %"PRIu16").\n",
				      req_idx);
			return -1;
		}

		if (spdk_unlikely(spdk_vhost_vring_desc_to_iov(vdev, &iovs[cnt], desc))) {
			SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK, "Invalid descriptor %" PRIu16" (req_idx = %"PRIu16").\n",
				      req_idx, cnt);
			return -1;
		}

		len += iovs[cnt].iov_len;
		cnt++;

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
	spdk_vhost_vq_used_ring_enqueue(&task->bvdev->vdev, &task->bvdev->vdev.virtqueue[0], task->req_idx,
					task->length);
	SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK, "Finished task (%p) req_idx=%d\n status: %s\n", task,
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
		    uint16_t req_idx)
{
	struct rte_vhost_vring *vq = &bvdev->vdev.virtqueue[0];
	const struct virtio_blk_outhdr *req;
	struct iovec *iov;
	uint32_t type;
	int rc;

	assert(task->bvdev == bvdev);
	task->req_idx = req_idx;
	task->iovcnt = SPDK_COUNTOF(task->iovs);

	if (blk_iovs_setup(&bvdev->vdev, vq, req_idx, task->iovs, &task->iovcnt, &task->length)) {
		SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK, "Invalid request (req_idx = %"PRIu16").\n", req_idx);
		/* Only READ and WRITE are supported for now. */
		invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	iov = &task->iovs[0];
	if (spdk_unlikely(iov->iov_len != sizeof(*req))) {
		SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK,
			      "First descriptor size is %zu but expected %zu (req_idx = %"PRIu16").\n",
			      iov->iov_len, sizeof(*req), req_idx);
		invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	req = iov->iov_base;

	iov = &task->iovs[task->iovcnt - 1];
	if (spdk_unlikely(iov->iov_len != 1)) {
		SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK,
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
			rc = spdk_bdev_readv(bvdev->bdev, bvdev->bdev_io_channel,
					     &task->iovs[1], task->iovcnt, req->sector * 512,
					     task->length, blk_request_complete_cb, task);
		} else if (!bvdev->readonly) {
			rc = spdk_bdev_writev(bvdev->bdev, bvdev->bdev_io_channel,
					      &task->iovs[1], task->iovcnt, req->sector * 512,
					      task->length, blk_request_complete_cb, task);
		} else {
			SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK, "Device is in read-only mode!\n");
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
		SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK, "Not supported request type '%"PRIu32"'.\n", type);
		invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	return 0;
}

static void
vdev_worker(void *arg)
{
	struct spdk_vhost_blk_dev *bvdev = arg;
	struct rte_vhost_vring *vq = &bvdev->vdev.virtqueue[0];
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
		SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK, "====== Starting processing request idx %"PRIu16"======\n",
			      reqs[i]);
		rc = process_blk_request(tasks[i], bvdev, reqs[i]);
		if (rc == 0) {
			SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK, "====== Task %p req_idx %d submitted ======\n", tasks[i], reqs[i]);
		} else {
			SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK, "====== Task %p req_idx %d failed ======\n", tasks[i], reqs[i]);
		}
	}
}

static void
no_bdev_vdev_worker(void *arg)
{
	struct spdk_vhost_blk_dev *bvdev = arg;
	struct rte_vhost_vring *vq = &bvdev->vdev.virtqueue[0];
	struct iovec iovs[VHOST_BLK_IOVS_MAX];
	uint32_t length;
	uint16_t iovcnt, req_idx;

	if (spdk_vhost_vq_avail_ring_get(vq, &req_idx, 1) != 1) {
		return;
	}

	iovcnt = SPDK_COUNTOF(iovs);
	if (blk_iovs_setup(&bvdev->vdev, vq, req_idx, iovs, &iovcnt, &length) && iovcnt >= 2) {
		*(volatile uint8_t *)iovs[iovcnt - 1].iov_base = VIRTIO_BLK_S_IOERR;
		SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK_DATA, "Aborting request %" PRIu16"\n", req_idx);
	}

	spdk_vhost_vq_used_ring_enqueue(&bvdev->vdev, vq, req_idx, 0);
}

static void
add_vdev_cb(void *arg)
{
	struct spdk_vhost_blk_dev *bvdev = arg;
	struct spdk_vhost_dev *vdev = &bvdev->vdev;
	struct spdk_vhost_blk_task *task;
	size_t rc;
	uint32_t i;

	spdk_vhost_dev_mem_register(&bvdev->vdev);

	if (bvdev->bdev) {
		bvdev->bdev_io_channel = spdk_bdev_get_io_channel(bvdev->bdev_desc);
		if (!bvdev->bdev_io_channel) {
			SPDK_ERRLOG("Controller %s: IO channel allocation failed\n", vdev->name);
			abort();
		}
	}

	bvdev->tasks_pool = spdk_ring_create(SPDK_RING_TYPE_SP_SC, vdev->virtqueue[0].size * 2,
					     spdk_env_get_socket_id(vdev->lcore));

	for (i = 0; i < vdev->virtqueue[0].size; i++) {
		task = spdk_dma_zmalloc(sizeof(*task), SPDK_CACHE_LINE_SIZE, NULL);
		if (task == NULL) {
			// TODO: add a mechanism to report failure so we can handle this properly
			SPDK_ERRLOG("task allocation failed\n");
			abort();
		}
		task->bvdev = bvdev;

		rc = spdk_ring_enqueue(bvdev->tasks_pool, (void **)&task, 1);
		if (rc != 1) {
			assert(false);
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
	struct spdk_vhost_blk_task *task;

	SPDK_NOTICELOG("Stopping poller for vhost controller %s\n", bvdev->vdev.name);

	assert(rte_ring_count((struct rte_ring *)bvdev->tasks_pool) == bvdev->vdev.virtqueue[0].size);

	if (bvdev->bdev_io_channel) {
		spdk_put_io_channel(bvdev->bdev_io_channel);
		bvdev->bdev_io_channel = NULL;
	}

	while (spdk_ring_dequeue(bvdev->tasks_pool, (void **)&task, 1) == 1) {
		spdk_dma_free(task);
	}

	spdk_ring_free(bvdev->tasks_pool);
	bvdev->tasks_pool = NULL;

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

bool
spdk_vhost_blk_get_readonly(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_blk_dev *bvdev = to_blk_dev(vdev);

	assert(bvdev != NULL);
	return bvdev->readonly;
}

static void
bdev_remove_cb(void *remove_ctx)
{
	SPDK_ERRLOG("Hot-removing bdev's not supported yet.\n");
	abort();
}

/*
 * A new device is added to a data core. First the device is added to the main linked list
 * and then allocated to a specific data core.
 *
 */
static int
new_device(int vid)
{
	struct spdk_vhost_dev *vdev;

	vdev = spdk_vhost_dev_load(vid);
	if (vdev == NULL) {
		return -1;
	}

	if (vdev->num_queues != 1) {
		SPDK_ERRLOG("Controller %s virtio-block device must have exactly one queue but got %d.\n",
			    vdev->name, vdev->num_queues);
		vdev->vid = -1;
		return -1;
	}

	spdk_vhost_timed_event_send(vdev->lcore, add_vdev_cb, vdev, 1, "add blk vdev");
	return 0;
}

static void
destroy_device(int vid)
{
	struct spdk_vhost_blk_dev *bvdev;
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_timed_event event = {0};
	uint32_t i;

	vdev = spdk_vhost_dev_find_by_vid(vid);
	bvdev = to_blk_dev(vdev);
	if (bvdev == NULL) {
		SPDK_ERRLOG("Couldn't find device with vid %d to stop.\n", vid);
		abort();
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
	spdk_vhost_dev_unload(vdev);
}

static const struct spdk_vhost_dev_backend vhost_blk_device_backend = {
	.virtio_features = (1ULL << VHOST_F_LOG_ALL) | (1ULL << VHOST_USER_F_PROTOCOL_FEATURES) |
	(1ULL << VIRTIO_F_VERSION_1) | (1ULL << VIRTIO_F_NOTIFY_ON_EMPTY) |
	(1ULL << VIRTIO_BLK_F_SIZE_MAX) | (1ULL << VIRTIO_BLK_F_SEG_MAX) |
	(1ULL << VIRTIO_BLK_F_GEOMETRY) | (1ULL << VIRTIO_BLK_F_RO) |
	(1ULL << VIRTIO_BLK_F_BLK_SIZE) | (1ULL << VIRTIO_BLK_F_TOPOLOGY) |
	(1ULL << VIRTIO_BLK_F_BARRIER)  | (1ULL << VIRTIO_BLK_F_SCSI) |
	(1ULL << VIRTIO_BLK_F_FLUSH)    | (1ULL << VIRTIO_BLK_F_CONFIG_WCE),
	.disabled_features = (1ULL << VHOST_F_LOG_ALL) | (1ULL << VIRTIO_BLK_F_GEOMETRY) |
	(1ULL << VIRTIO_BLK_F_RO) | (1ULL << VIRTIO_BLK_F_FLUSH) | (1ULL << VIRTIO_BLK_F_CONFIG_WCE) |
	(1ULL << VIRTIO_BLK_F_BARRIER) | (1ULL << VIRTIO_BLK_F_SCSI),
	.ops = {
		.new_device =  new_device,
		.destroy_device = destroy_device,
	}
};

int
spdk_vhost_blk_controller_construct(void)
{
	struct spdk_conf_section *sp;
	unsigned ctrlr_num;
	char *bdev_name;
	char *cpumask_str;
	char *name;
	uint64_t cpumask;
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

		cpumask_str = spdk_conf_section_get_val(sp, "Cpumask");
		readonly = spdk_conf_section_get_boolval(sp, "ReadOnly", false);
		if (cpumask_str == NULL) {
			cpumask = spdk_app_get_core_mask();
		} else if (spdk_vhost_parse_core_mask(cpumask_str, &cpumask)) {
			SPDK_ERRLOG("%s: Error parsing cpumask '%s' while creating controller\n", name, cpumask_str);
			return -1;
		}

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
spdk_vhost_blk_construct(const char *name, uint64_t cpumask, const char *dev_name, bool readonly)
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
