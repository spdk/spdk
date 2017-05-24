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
#include "spdk/conf.h"
#include "spdk/likely.h"
#include "spdk/bdev.h"
#include "spdk/io_channel.h"
#include "spdk_internal/bdev.h"
#include "spdk/vhost.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "vhost_internal.h"
#include "vhost_iommu.h"

#ifndef VIRTIO_BLK_T_FLUSH_OUT
/* TODO: check if this is needed */
#define VIRTIO_BLK_T_FLUSH_OUT 5
#endif

#define VIRTIO_F(f) (1ULL << (VIRTIO_F_ ## f))
#define VIRTIO_BLK_F(f) (1ULL << (VIRTIO_BLK_F_ ## f))

#define VHOST_BLK_IOVS_MAX 128

struct spdk_vhost_blk_task {
	struct spdk_bdev_io *bdev_io;
	struct spdk_vhost_blk_dev *vdev;
	volatile uint8_t *status;

	uint16_t req_idx;

	uint32_t length;
	uint16_t iovcnt;
	struct iovec iovs[VHOST_BLK_IOVS_MAX];
};

struct spdk_vhost_blk_dev {
	struct spdk_vhost_dev dev;

	struct spdk_bdev *bdev;
	struct spdk_io_channel *bdev_io_channel;

	struct spdk_poller *requestq_poller;

	struct spdk_ring *tasks_pool;
};

static void
spdk_vhost_blk_get_tasks(struct spdk_vhost_blk_dev *vdev, struct spdk_vhost_blk_task **tasks,
			 size_t count)
{
	size_t res_count = spdk_ring_dequeue(vdev->tasks_pool, (void **)tasks, count);

	/* Allocated task count in init function is equal queue depth so dequeue must not fail. */
	assert(res_count != count);
}

static void
spdk_vhost_blk_put_tasks(struct spdk_vhost_blk_dev *vdev, struct spdk_vhost_blk_task **tasks,
			 size_t count)
{
	size_t result = spdk_ring_enqueue(vdev->tasks_pool, (void **)tasks, count);

	if (unlikely(result != count)) {
		SPDK_ERRLOG("Controller %s: failed to put task\n", vdev->dev.name);
		abort();
	}
}

static void
invalid_blk_request(struct spdk_vhost_blk_task *task, uint8_t status)
{
	*task->status = status;
	spdk_vhost_vq_used_ring_enqueue(&task->vdev->dev, &task->vdev->dev.virtqueue[0], task->req_idx, 0);
	spdk_vhost_blk_put_tasks(task->vdev, &task, 1);
	SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK, "Invalid request (status=%" PRIu8")\n", status);
}

static bool
spdk_vhost_desc_to_iov(struct spdk_vhost_dev *vdev, struct iovec *iov,
		       const struct vring_desc *desc)
{
	iov->iov_base =  spdk_vhost_gpa_to_vva(vdev, desc->addr);
	iov->iov_len = desc->len;
	return !iov->iov_base;
}

/*
 * Process task's descriptor chain and setup data related fields.
 * Return
 *   total size of suplied buffers
 *
 *   FIXME: Make this function return to rd_cnt and wr_cnt
 */
static int
blk_iovs_setup(struct spdk_vhost_dev *dev, struct rte_vhost_vring *vq, uint16_t req_idx,
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

		if (spdk_unlikely(spdk_vhost_desc_to_iov(dev, &iovs[cnt], desc))) {
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
	spdk_vhost_vq_used_ring_enqueue(&task->vdev->dev, &task->vdev->dev.virtqueue[0], task->req_idx,
					task->length);
	SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK, "Finished task (%p) req_idx=%d\n status: %s\n", task,
		      task->req_idx, success ? "OK" : "FAIL");
	spdk_vhost_blk_put_tasks(task->vdev, &task, 1);
}

static void
blk_request_complete_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_vhost_blk_task *task = cb_arg;
	spdk_bdev_free_io(bdev_io);

	blk_request_finish(success, task);
}

static void
process_blk_request(struct spdk_vhost_blk_task *task, struct spdk_vhost_blk_dev *vdev,
		    uint16_t req_idx)
{
	struct rte_vhost_vring *vq = &vdev->dev.virtqueue[0];
	const struct virtio_blk_outhdr *req;
	struct spdk_bdev_io *bdev_io;
	struct iovec *iov;
	uint64_t offset;
	uint32_t type;

	SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK, "====== Starting processing request idx %"PRIu16"======\n",
		      req_idx);

	assert(task->vdev == vdev);
	task->req_idx = req_idx;
	task->iovcnt = SPDK_COUNTOF(task->iovs);

	if (blk_iovs_setup(&vdev->dev, vq, req_idx, task->iovs, &task->iovcnt, &task->length)) {
		SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK, "Invalid request (req_idx = %"PRIu16").\n", req_idx);
		/* Only READ and WRITE are supported for now. */
		goto err;
	}

	iov = &task->iovs[0];
	if (spdk_unlikely(iov->iov_len != sizeof(*req))) {
		SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK,
			      "First descriptor size is %zu but expected %zu (req_idx = %"PRIu16").\n",
			      iov->iov_len, sizeof(*req), req_idx);
		goto err;
	}

	req = iov->iov_base;

	iov = &task->iovs[task->iovcnt - 1];
	if (spdk_unlikely(iov->iov_len != 1)) {
		SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK,
			      "Last descriptor size is %zu but expected %d (req_idx = %"PRIu16").\n",
			      iov->iov_len, 1, req_idx);
		goto err;
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
		offset = req->sector * 512;
		if ((task->length & (512 - 1)) != 0) {
			SPDK_ERRLOG("%s - passed IO buffer is not multiple of 512b (req_idx = %"PRIu16").\n",
				    type ? "WRITE" : "READ", req_idx);
			goto err;
		}

		if (type == VIRTIO_BLK_T_IN) {
			bdev_io = spdk_bdev_readv(vdev->bdev, vdev->bdev_io_channel,
						  &task->iovs[1], task->iovcnt, offset,
						  task->length, blk_request_complete_cb, task);
		} else {

			bdev_io = spdk_bdev_writev(vdev->bdev, vdev->bdev_io_channel,
						   &task->iovs[1], task->iovcnt, offset,
						   task->length, blk_request_complete_cb, task);
		}

		if (!bdev_io) {
			goto err;
		}
		break;
	case VIRTIO_BLK_T_GET_ID:
		if (!task->iovcnt || !task->length) {
			goto err;
		}
		task->length = spdk_min((size_t)VIRTIO_BLK_ID_BYTES, task->iovs[1].iov_len);
		spdk_strcpy_pad(task->iovs[1].iov_base, vdev->bdev->product_name, task->length, ' ');
		blk_request_finish(true, task);
		goto out;
	default:
		SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK, "Not supported request type '%"PRIu32"'.\n", type);
		goto err;
		goto err;
	}

out:
	SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK, "====== Task %p req_idx %d submitted ======\n", task,
		      req_idx);
	return;
err:
	invalid_blk_request(task, VIRTIO_BLK_S_UNSUPP);
	SPDK_TRACELOG(SPDK_TRACE_VHOST_BLK, "====== Task %p req_idx %d failed ======\n", task, req_idx);
}

static void
vdev_worker(void *arg)
{
	struct spdk_vhost_blk_dev *vdev = arg;
	struct rte_vhost_vring *vq = &vdev->dev.virtqueue[0];
	struct spdk_vhost_blk_task *tasks[32] = {0};

	uint16_t reqs[32];
	uint16_t reqs_cnt, i;

	reqs_cnt = spdk_vhost_vq_avail_ring_get(vq, reqs, RTE_DIM(reqs));
	assert(reqs_cnt <= 32);

	/* TODO: remove if() and replace by assert(). This might not fail */
	spdk_vhost_blk_get_tasks(vdev, tasks, reqs_cnt);

	for (i = 0; i < reqs_cnt; i++) {
		process_blk_request(tasks[i], vdev, reqs[i]);

	}
}

static void
add_vdev_cb(void *arg)
{
	struct spdk_vhost_blk_dev *vdev = arg;
	struct spdk_vhost_dev *dev = &vdev->dev;
	struct spdk_vhost_blk_task *task;
	size_t rc;
	uint32_t i;

	spdk_vhost_dev_mem_register(&vdev->dev);

	vdev->bdev_io_channel = spdk_bdev_get_io_channel(vdev->bdev);
	if (!vdev->bdev_io_channel) {
		SPDK_ERRLOG("Controller %s: IO channel allocation failed\n", dev->name);
		abort();
	}

	vdev->tasks_pool = spdk_ring_create(SPDK_RING_TYPE_SP_SC, dev->virtqueue[0].size * 2,
					    spdk_env_get_socket_id(dev->lcore));

	for (i = 0; i < dev->virtqueue[0].size; i++) {
		task = spdk_dma_zmalloc(sizeof(*task), SPDK_CACHE_LINE_SIZE, NULL);
		task->vdev = vdev;

		rc = spdk_ring_enqueue(vdev->tasks_pool, (void **)&task, 1);

		assert(rc == 1);
	}

	spdk_poller_register(&vdev->requestq_poller, vdev_worker, vdev, dev->lcore, 0);

	SPDK_NOTICELOG("Started poller for vhost controller %s on lcore %d\n", dev->name, dev->lcore);
}

static void
remove_vdev_cb(void *arg)
{
	struct spdk_vhost_blk_dev *vdev = arg;
	struct spdk_vhost_dev *dev = &vdev->dev;
	struct spdk_vhost_blk_task *task;

	spdk_put_io_channel(vdev->bdev_io_channel);
	vdev->bdev_io_channel = NULL;

	SPDK_NOTICELOG("Stopping poller for vhost controller %s\n", vdev->dev.name);

	assert(rte_ring_count((struct rte_ring *)vdev->tasks_pool) == vdev->dev.virtqueue[0].size);

	while (spdk_ring_dequeue(vdev->tasks_pool, (void **)&task, 1) == 1) {
		spdk_dma_free(task);
	}

	spdk_ring_free(vdev->tasks_pool);
	vdev->tasks_pool = NULL;

	spdk_vhost_dev_mem_unregister(dev);
}

static struct spdk_vhost_blk_dev *
to_blk_dev(struct spdk_vhost_dev *ctrlr)
{
	if (ctrlr == NULL) {
		return NULL;
	}

	if (ctrlr->type != SPDK_VHOST_DEV_T_BLK) {
		SPDK_ERRLOG("Controller %s: expected block controller (%d) but got %d\n",
			    ctrlr->name, SPDK_VHOST_DEV_T_BLK, ctrlr->type);
		return NULL;
	}

	return (struct spdk_vhost_blk_dev *)ctrlr;
}

struct spdk_bdev *
spdk_vhost_blk_get_dev(struct spdk_vhost_dev *ctrlr)
{
	struct spdk_vhost_blk_dev *vdev = to_blk_dev(ctrlr);
	assert(vdev != NULL);
	return vdev->bdev;
}

static void
bdev_remove_cb(void *remove_ctx)
{
	(void) remove_ctx;
	SPDK_ERRLOG("Hot-removing bdev's not supported yet.\n");
	abort();
}

int
spdk_vhost_blk_add_dev(const char *ctrlr_name, const char *bdev_name)
{
	struct spdk_vhost_dev *dev;
	struct spdk_vhost_blk_dev *vdev;

	if (ctrlr_name == NULL) {
		SPDK_ERRLOG("No controller name\n");
		return -EINVAL;
	}

	if (bdev_name == NULL) {
		SPDK_ERRLOG("No bdev name specified \n");
		return -EINVAL;
	}

	dev = spdk_vhost_dev_find(ctrlr_name);
	if (dev == NULL) {
		SPDK_ERRLOG("Controller %s is not defined\n", ctrlr_name);
		return -ENODEV;
	}

	if (dev->lcore != -1) {
		SPDK_ERRLOG("Controller %s is in use and hotplug is not supported\n", ctrlr_name);
		return -ENODEV;
	}

	vdev = to_blk_dev(dev);
	if (!vdev) {
		SPDK_ERRLOG("Controller %s is not block controller\n", ctrlr_name);
		return -ENODEV;
	}

	if (vdev->bdev != NULL) {
		SPDK_ERRLOG("Controller %s bdev already assigned ('%s')\n", ctrlr_name,
			    spdk_bdev_get_name(vdev->bdev));
		return -EEXIST;
	}

	vdev->bdev = spdk_bdev_get_by_name(bdev_name);
	if (vdev->bdev == NULL) {
		SPDK_ERRLOG("Controller %s: bdev '%s' not found\n",
			    vdev->dev.name, bdev_name);
		return -EINVAL;
	} else if (spdk_bdev_claim(vdev->bdev, bdev_remove_cb, vdev) == false) {
		SPDK_ERRLOG("Controller %s: failed to claim bdev '%s'\n",
			    vdev->dev.name, bdev_name);
		vdev->bdev = NULL;
		return -EINVAL;
	}

	SPDK_NOTICELOG("Controller %s: using bdev '%s'\n",
		       vdev->dev.name, bdev_name);
	return 0;
}

int
spdk_vhost_blk_remove_dev(struct spdk_vhost_dev *dev)
{
	struct spdk_vhost_blk_dev *vdev = to_blk_dev(dev);;

	if (!vdev) {
		return -EINVAL;
	}

	if (dev->lcore != -1) {
		SPDK_ERRLOG("Controller %s is in use and hotremove is not supported\n", dev->name);
		return -EBUSY;
	}

	if (vdev->bdev == NULL) {
		SPDK_ERRLOG("Controller %s dev is not occupied\n", dev->name);
		return -ENODEV;
	}


	spdk_bdev_unclaim(vdev->bdev);
	vdev->bdev = NULL;

	SPDK_NOTICELOG("Controller %s: removed device\n", dev->name);
	return 0;
}

/*
 * A new device is added to a data core. First the device is added to the main linked list
 * and then allocated to a specific data core.
 *
 */
static int
new_device(int vid)
{
	struct spdk_vhost_dev *vdev = NULL;

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

	if (vdev->num_queues != 1) {
		SPDK_ERRLOG("Controller %s virtio-block device must have exactly one queue but got %d.\n",
			    vdev->name, vdev->num_queues);
		vdev->vid = -1;
		return -1;
	}

	spdk_vhost_timed_event_send(vdev->lcore, add_vdev_cb, vdev, 1, "add vdev");
	return 0;
}

static void
destroy_device(int vid)
{
	struct spdk_vhost_blk_dev *vdev;
	struct spdk_vhost_dev *dev;
	struct spdk_vhost_timed_event event = {0};
	uint32_t i;

	dev = spdk_vhost_dev_find_by_vid(vid);
	vdev = to_blk_dev(dev);
	if (vdev == NULL) {
		rte_panic("Couldn't find device with vid %d to stop.\n", vid);
	}

	spdk_vhost_timed_event_init(&event, dev->lcore, NULL, NULL, 1);
	spdk_poller_unregister(&vdev->requestq_poller, event.spdk_event);
	spdk_vhost_timed_event_wait(&event, "unregister poller");

	/* Wait for all tasks to finish */
	for (i = 1000; i && dev->task_cnt > 0; i--) {
		usleep(1000);
	}

	if (dev->task_cnt > 0) {
		rte_panic("%s: pending tasks did not finish in 1s.\n", dev->name);
	}

	spdk_vhost_timed_event_send(dev->lcore, remove_vdev_cb, vdev, 1, "remove vdev");
	spdk_vhost_dev_unload(dev);
}

const struct spdk_vhost_dev_backend vhost_blk_device_backend = {
	.virtio_features = (1ULL << VHOST_F_LOG_ALL) | (1ULL << VHOST_USER_F_PROTOCOL_FEATURES) |
	VIRTIO_F(VERSION_1)    | VIRTIO_F(NOTIFY_ON_EMPTY) |
	VIRTIO_BLK_F(SIZE_MAX) | VIRTIO_BLK_F(SEG_MAX) |
	VIRTIO_BLK_F(GEOMETRY) | VIRTIO_BLK_F(RO) |
	VIRTIO_BLK_F(BLK_SIZE) | VIRTIO_BLK_F(TOPOLOGY) |
	VIRTIO_BLK_F(BARRIER)  | VIRTIO_BLK_F(SCSI) |
	VIRTIO_BLK_F(FLUSH)    | VIRTIO_BLK_F(CONFIG_WCE),
	.disabled_features = (1ULL << VHOST_F_LOG_ALL) | VIRTIO_BLK_F(GEOMETRY)  |
	VIRTIO_BLK_F(RO) | VIRTIO_BLK_F(FLUSH) |
	VIRTIO_BLK_F(TOPOLOGY) | VIRTIO_BLK_F(CONFIG_WCE) |
	VIRTIO_BLK_F(BARRIER) | VIRTIO_BLK_F(SCSI),
	.ops = {
		.new_device =  new_device,
		.destroy_device = destroy_device,
	}
};

int
spdk_vhost_blk_controller_construct(void)
{
	struct spdk_conf_section *sp = spdk_conf_first_section(NULL);
	unsigned ctrlr_num = 0;
	char *bdev_name;
	char *cpumask_str;
	char *name;
	uint64_t cpumask;

	while (sp != NULL) {
		if (!spdk_conf_section_match_prefix(sp, "VhostBlk")) {
			sp = spdk_conf_next_section(sp);
			continue;
		}

		if (sscanf(spdk_conf_section_get_name(sp), "VhostBlk%u", &ctrlr_num) != 1) {
			SPDK_ERRLOG("Section '%s' has non-numeric suffix.\n",
				    spdk_conf_section_get_name(sp));
			return -1;
		}

		name =  spdk_conf_section_get_val(sp, "Name");
		cpumask_str = spdk_conf_section_get_val(sp, "Cpumask");
		if (cpumask_str == NULL) {
			cpumask = spdk_app_get_core_mask();
		} else if (spdk_vhost_parse_core_mask(cpumask_str, &cpumask)) {
			SPDK_ERRLOG("%s: Error parsing cpumask '%s' while creating controller\n", name, cpumask_str);
			return -1;
		}

		if (spdk_vhost_blk_construct(name, cpumask) < 0) {
			return -1;
		}

		bdev_name = spdk_conf_section_get_val(sp, "Dev");
		if (bdev_name == NULL) {
			SPDK_ERRLOG("%s: Invalid or missing BDEV name for dev\n", name);
			return -1;
		}

		if (spdk_vhost_blk_add_dev(name, bdev_name) < 0) {
			return -1;
		}

		sp = spdk_conf_next_section(sp);
	}

	return 0;
}

int
spdk_vhost_blk_construct(const char *name, uint64_t cpumask)
{
	struct spdk_vhost_blk_dev *vdev;
	int rc;

	if (name == NULL) {
		SPDK_ERRLOG("Can't add controller with no name\n");
		return -EINVAL;
	} else if (spdk_vhost_dev_find(name)) {
		SPDK_ERRLOG("Controller %s already exists.\n", name);
		return -EEXIST;
	}

	if ((cpumask & spdk_app_get_core_mask()) != cpumask) {
		SPDK_ERRLOG("cpumask 0x%jx not a subset of app mask 0x%jx\n",
			    cpumask, spdk_app_get_core_mask());
		return -EINVAL;
	}

	vdev = spdk_dma_zmalloc(sizeof(*vdev), SPDK_CACHE_LINE_SIZE, NULL);
	if (vdev == NULL) {
		SPDK_ERRLOG("Couldn't allocate memory for vhost dev\n");
		return -ENOMEM;
	}

	/* FIXME: move this to vhost.c */
	vdev->dev.name =  strdup(name);
	vdev->dev.cpumask = cpumask;
	vdev->dev.vid = -1;
	vdev->dev.lcore = -1;
	vdev->dev.type = SPDK_VHOST_DEV_T_BLK;

	rc = spdk_vhost_dev_register(&vdev->dev, &vhost_blk_device_backend);
	if (rc < 0) {
		free(vdev->dev.name);
		spdk_dma_free(vdev);
	}

	return rc;
}

int
spdk_vhost_blk_destroy(struct spdk_vhost_dev *dev)
{
	struct spdk_vhost_blk_dev *vdev = to_blk_dev(dev);

	if (!vdev) {
		return -EINVAL;
	}

	if (vdev->dev.lcore != -1) {
		SPDK_ERRLOG("Controller %s is in use.\n", vdev->dev.name);
		return -EBUSY;
	}

	if (vdev->bdev) {
		SPDK_ERRLOG("Trying to remove non-empty controller: %s.\n", vdev->dev.name);
		return -EBUSY;
	}

	if (spdk_vhost_dev_unregister(&vdev->dev) != 0) {
		SPDK_ERRLOG("Could not unregister controller %s\n", vdev->dev.name);
		return -EIO;
	}

	SPDK_NOTICELOG("Controller %s: removed\n", vdev->dev.name);

	/* FIXME: move this to vhost.c */
	free(vdev->dev.name);

	spdk_dma_free(vdev);
	return 0;
}


// TODO: refactor this
SPDK_LOG_REGISTER_TRACE_FLAG("vhost_blk", SPDK_TRACE_VHOST_BLK)
SPDK_LOG_REGISTER_TRACE_FLAG("vhost_blk_data", SPDK_TRACE_VHOST_BLK_DATA)
