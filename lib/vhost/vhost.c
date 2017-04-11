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

#include <linux/virtio_ring.h>
#include <linux/virtio_scsi.h>

#include <rte_vhost.h>

#include "spdk_internal/log.h"
#include "spdk/env.h"
#include "spdk/scsi.h"
#include "spdk/conf.h"
#include "spdk/event.h"
#include "spdk/scsi_spec.h"
#include "spdk/likely.h"

#include "spdk/vhost.h"
#include "task.h"
#include "vhost_iommu.h"

static uint32_t g_num_ctrlrs[RTE_MAX_LCORE];

#define CONTROLQ_POLL_PERIOD_US (1000 * 5)

#define VIRTIO_SCSI_CONTROLQ   0
#define VIRTIO_SCSI_EVENTQ   1
#define VIRTIO_SCSI_REQUESTQ   2

/* Path to folder where character device will be created. Can be set by user. */
static char dev_dirname[PATH_MAX] = "";

#define SPDK_CACHE_LINE_SIZE RTE_CACHE_LINE_SIZE

#define MAX_VHOST_DEVICE	1024

#ifndef VIRTIO_F_VERSION_1
#define VIRTIO_F_VERSION_1 32
#endif

#define VHOST_USER_F_PROTOCOL_FEATURES	30

/* Features supported by SPDK VHOST lib. */
#define SPDK_VHOST_SCSI_FEATURES	((1ULL << VIRTIO_F_VERSION_1) | \
					(1ULL << VHOST_F_LOG_ALL) | \
					(1ULL << VHOST_USER_F_PROTOCOL_FEATURES) | \
					(1ULL << VIRTIO_F_NOTIFY_ON_EMPTY) | \
					(1ULL << VIRTIO_SCSI_F_INOUT) | \
					(1ULL << VIRTIO_SCSI_F_HOTPLUG) | \
					(1ULL << VIRTIO_SCSI_F_CHANGE ) | \
					(1ULL << VIRTIO_SCSI_F_T10_PI ))

/* Features that are specified in VIRTIO SCSI but currently not supported:
 * - Live migration not supported yet
 * - Hotplug/hotremove
 * - LUN params change
 * - T10 PI
 */
#define SPDK_VHOST_SCSI_DISABLED_FEATURES	((1ULL << VHOST_F_LOG_ALL) | \
						(1ULL << VIRTIO_SCSI_F_HOTPLUG) | \
						(1ULL << VIRTIO_SCSI_F_CHANGE ) | \
						(1ULL << VIRTIO_SCSI_F_T10_PI ))

struct spdk_vhost_dev {
	struct rte_vhost_memory *mem;
	int vid;
	uint16_t num_queues;
	uint64_t negotiated_features;
	struct rte_vhost_vring virtqueue[0] __attribute((aligned(SPDK_CACHE_LINE_SIZE)));
};

static void
spdk_vhost_dev_free(struct spdk_vhost_dev *dev)
{
	free(dev->mem);
	spdk_free(dev);
}

static void
spdk_vhost_dev_destruct(struct spdk_vhost_dev *dev)
{
	struct rte_vhost_vring *q;
	uint16_t i;

	for (i = 0; i < dev->num_queues; i++) {
		q = &dev->virtqueue[i];
		rte_vhost_set_vhost_vring_last_idx(dev->vid, i, q->last_avail_idx, q->last_used_idx);
	}

	spdk_vhost_dev_free(dev);
}

static struct spdk_vhost_dev *
spdk_vhost_dev_create(int vid)
{
	uint16_t num_queues = rte_vhost_get_vring_num(vid);
	size_t size = sizeof(struct spdk_vhost_dev) + num_queues * sizeof(struct rte_vhost_vring);
	struct spdk_vhost_dev *dev = spdk_zmalloc(size, SPDK_CACHE_LINE_SIZE, NULL);
	uint16_t i;

	if (dev == NULL) {
		SPDK_ERRLOG("vhost device %d: Failed to allocate new vhost device with %"PRIu16" queues\n", vid,
			    num_queues);
		return NULL;
	}

	for (i = 0; i < num_queues; i++) {
		if (rte_vhost_get_vhost_vring(vid, i, &dev->virtqueue[i])) {
			SPDK_ERRLOG("vhost device %d: Failed to get information of queue %"PRIu16"\n", vid, i);
			goto err;
		}

		/* Disable notifications. */
		if (rte_vhost_enable_guest_notification(vid, i, 0) != 0) {
			SPDK_ERRLOG("vhost device %d: Failed to disable guest notification on queue %"PRIu16"\n", vid, i);
			goto err;
		}

	}

	dev->vid = vid;
	dev->num_queues = num_queues;

	if (rte_vhost_get_negotiated_features(vid, &dev->negotiated_features) != 0) {
		SPDK_ERRLOG("vhost device %d: Failed to get negotiated driver features\n", vid);
		goto err;
	}

	if (rte_vhost_get_mem_table(vid, &dev->mem) != 0) {
		SPDK_ERRLOG("vhost device %d: Failed to get guest memory table\n", vid);
		goto err;
	}
	return dev;

err:
	spdk_vhost_dev_free(dev);
	return NULL;
}

static uint64_t
gpa_to_vva(struct spdk_vhost_dev *vdev, uint64_t addr)
{
	return rte_vhost_gpa_to_vva(vdev->mem, addr);
}

struct spdk_vhost_scsi_ctrlr {
	char *name;
	struct spdk_vhost_dev *dev;

	/**< TODO make this an array of spdk_scsi_devs.  The vhost scsi
	 *   request will tell us which scsi_dev to use.
	 */
	struct spdk_scsi_dev *scsi_dev[SPDK_VHOST_SCSI_CTRLR_MAX_DEVS];

	int task_cnt;

	struct spdk_poller *requestq_poller;
	struct spdk_poller *controlq_poller;

	int32_t lcore;

	uint64_t cpumask;
} __rte_cache_aligned;

/* This maps from the integer index passed by DPDK to the our controller representation. */
/* MAX_VHOST_DEVICE from DPDK. */
static struct spdk_vhost_scsi_ctrlr *dpdk_vid_mapping[MAX_VHOST_DEVICE];

/*
 * Get available requests from avail ring.
 */
static uint16_t
vq_avail_ring_get(struct rte_vhost_vring *vq, uint16_t *reqs, uint16_t reqs_len)
{
	struct vring_avail *avail = vq->avail;
	uint16_t size_mask = vq->size - 1;
	uint16_t last_idx = vq->last_avail_idx, avail_idx = avail->idx;
	uint16_t count = RTE_MIN((avail_idx - last_idx) & size_mask, reqs_len);
	uint16_t i;

	if (spdk_likely(count == 0)) {
		return 0;
	}

	vq->last_avail_idx += count;
	for (i = 0; i < count; i++) {
		reqs[i] = vq->avail->ring[(last_idx + i) & size_mask];
	}

	SPDK_TRACELOG(SPDK_TRACE_VHOST_RING,
		      "AVAIL: last_idx=%"PRIu16" avail_idx=%"PRIu16" count=%"PRIu16"\n",
		      last_idx, avail_idx, count);

	return count;
}

static bool
vq_should_notify(struct spdk_vhost_dev *dev, struct rte_vhost_vring *vq)
{
	if ((dev->negotiated_features & (1ULL << VIRTIO_F_NOTIFY_ON_EMPTY)) &&
	    spdk_unlikely(vq->avail->idx == vq->last_avail_idx)) {
		return 1;
	}

	return !(vq->avail->flags & VRING_AVAIL_F_NO_INTERRUPT);
}

/*
 * Enqueue id and len to used ring.
 */
static void
vq_used_ring_enqueue(struct spdk_vhost_dev *dev, struct rte_vhost_vring *vq, uint16_t id,
		     uint32_t len)
{
	struct vring_used *used = vq->used;
	uint16_t size_mask = vq->size - 1;
	uint16_t last_idx = vq->last_used_idx;

	SPDK_TRACELOG(SPDK_TRACE_VHOST_RING, "USED: last_idx=%"PRIu16" req id=%"PRIu16" len=%"PRIu32"\n",
		      last_idx, id, len);

	vq->last_used_idx++;
	last_idx &= size_mask;

	used->ring[last_idx].id = id;
	used->ring[last_idx].len = len;

	rte_compiler_barrier();

	vq->used->idx = vq->last_used_idx;
	if (vq_should_notify(dev, vq)) {
		eventfd_write(vq->callfd, (eventfd_t)1);
	}
}

static bool
vring_desc_has_next(struct vring_desc *cur_desc)
{
	return !!(cur_desc->flags & VRING_DESC_F_NEXT);
}

static struct vring_desc *
vring_desc_get_next(struct vring_desc *vq_desc, struct vring_desc *cur_desc)
{
	assert(vring_desc_has_next(cur_desc));
	return &vq_desc[cur_desc->next];
}

static bool
vring_desc_is_wr(struct vring_desc *cur_desc)
{
	return !!(cur_desc->flags & VRING_DESC_F_WRITE);
}

static void task_submit(struct spdk_vhost_task *task);
static int process_request(struct spdk_vhost_task *task);
static void invalid_request(struct spdk_vhost_task *task);

static void
submit_completion(struct spdk_vhost_task *task)
{
	struct iovec *iovs = NULL;
	int result;

	vq_used_ring_enqueue(task->vdev->dev, task->vq, task->req_idx, task->scsi.data_transferred);
	SPDK_TRACELOG(SPDK_TRACE_VHOST, "Finished task (%p) req_idx=%d\n", task, task->req_idx);

	if (task->scsi.iovs != &task->scsi.iov) {
		iovs = task->scsi.iovs;
		task->scsi.iovs = &task->scsi.iov;
		task->scsi.iovcnt = 1;
	}

	spdk_vhost_task_put(task);

	if (!iovs) {
		return;
	}

	while (1) {
		task = spdk_vhost_dequeue_task();
		if (!task) {
			spdk_vhost_iovec_free(iovs);
			break;
		}

		/* Set iovs so underlying functions will not try to alloc IOV */
		task->scsi.iovs = iovs;
		task->scsi.iovcnt = VHOST_SCSI_IOVS_LEN;

		result = process_request(task);
		if (result == 0) {
			task_submit(task);
			break;
		} else {
			task->scsi.iovs = &task->scsi.iov;
			task->scsi.iovcnt = 1;
			invalid_request(task);
		}
	}
}

static void
process_mgmt_task_completion(void *arg1, void *arg2)
{
	struct spdk_vhost_task *task = arg1;

	submit_completion(task);
}

static void
process_task_completion(void *arg1, void *arg2)
{
	struct spdk_vhost_task *task = arg1;

	/* The SCSI task has completed.  Do final processing and then post
	   notification to the virtqueue's "used" ring.
	 */
	task->resp->status = task->scsi.status;

	if (task->scsi.status != SPDK_SCSI_STATUS_GOOD) {
		memcpy(task->resp->sense, task->scsi.sense_data, task->scsi.sense_data_len);
		task->resp->sense_len = task->scsi.sense_data_len;
	}
	task->resp->resid = task->scsi.transfer_len - task->scsi.data_transferred;

	submit_completion(task);
}

static void
task_submit(struct spdk_vhost_task *task)
{
	/* The task is ready to be submitted.  First create the callback event that
	   will be invoked when the SCSI command is completed.  See process_task_completion()
	   for what SPDK vhost-scsi does when the task is completed.
	 */

	task->resp->response = VIRTIO_SCSI_S_OK;
	task->scsi.cb_event = spdk_event_allocate(rte_lcore_id(),
			      process_task_completion,
			      task, NULL);
	spdk_scsi_dev_queue_task(task->scsi_dev, &task->scsi);
}

static void
mgmt_task_submit(struct spdk_vhost_task *task)
{
	task->tmf_resp->response = VIRTIO_SCSI_S_OK;
	task->scsi.cb_event = spdk_event_allocate(rte_lcore_id(),
			      process_mgmt_task_completion,
			      task, NULL);
	spdk_scsi_dev_queue_mgmt_task(task->scsi_dev, &task->scsi);
}

static void
invalid_request(struct spdk_vhost_task *task)
{
	vq_used_ring_enqueue(task->vdev->dev, task->vq, task->req_idx, 0);
	spdk_vhost_task_put(task);

	SPDK_TRACELOG(SPDK_TRACE_VHOST, "Invalid request (status=%" PRIu8")\n",
		      task->resp ? task->resp->response : -1);
}

static struct spdk_scsi_dev *
get_scsi_dev(struct spdk_vhost_scsi_ctrlr *vdev, const __u8 *lun)
{
	SPDK_TRACEDUMP(SPDK_TRACE_VHOST_QUEUE, "LUN", lun, 8);
	/* First byte must be 1 and second is target */
	if (lun[0] != 1 || lun[1] >= SPDK_VHOST_SCSI_CTRLR_MAX_DEVS)
		return NULL;

	return vdev->scsi_dev[lun[1]];
}

static struct spdk_scsi_lun *
get_scsi_lun(struct spdk_scsi_dev *scsi_dev, const __u8 *lun)
{
	uint16_t lun_id = (((uint16_t)lun[2] << 8) | lun[3]) & 0x3FFF;

	/* For now only one LUN per controller is allowed so no need to search LUN IDs */
	if (likely(scsi_dev != NULL)) {
		return spdk_scsi_dev_get_lun(scsi_dev, lun_id);
	}

	return NULL;
}

void
spdk_vhost_scsi_ctrlr_task_ref(struct spdk_vhost_scsi_ctrlr *vdev)
{
	assert(vdev->task_cnt < INT_MAX);
	vdev->task_cnt++;
}

void
spdk_vhost_scsi_ctrlr_task_unref(struct spdk_vhost_scsi_ctrlr *vdev)
{
	assert(vdev->task_cnt > 0);
	vdev->task_cnt--;
}

static void
process_ctrl_request(struct spdk_vhost_scsi_ctrlr *vdev, struct rte_vhost_vring *controlq,
		     uint16_t req_idx)
{
	struct spdk_vhost_task *task;

	struct vring_desc *desc;
	struct virtio_scsi_ctrl_tmf_req *ctrl_req;
	struct virtio_scsi_ctrl_an_resp *an_resp;

	desc = &controlq->desc[req_idx];
	ctrl_req = (void *)gpa_to_vva(vdev->dev, desc->addr);

	SPDK_TRACELOG(SPDK_TRACE_VHOST_QUEUE,
		      "Processing controlq descriptor: desc %d/%p, desc_addr %p, len %d, flags %d, last_used_idx %d; kickfd %d; size %d\n",
		      req_idx, desc, (void *)desc->addr, desc->len, desc->flags, controlq->last_used_idx,
		      controlq->kickfd, controlq->size);
	SPDK_TRACEDUMP(SPDK_TRACE_VHOST_QUEUE, "Request desriptor", (uint8_t *)ctrl_req,
		       desc->len);

	task = spdk_vhost_task_get(vdev);
	task->vq = controlq;
	task->vdev = vdev;
	task->req_idx = req_idx;
	task->scsi_dev = get_scsi_dev(task->vdev, ctrl_req->lun);

	/* Process the TMF request */
	switch (ctrl_req->type) {
	case VIRTIO_SCSI_T_TMF:
		/* Get the response buffer */
		assert(vring_desc_has_next(desc));
		desc = vring_desc_get_next(controlq->desc, desc);
		task->tmf_resp = (void *)gpa_to_vva(vdev->dev, desc->addr);

		/* Check if we are processing a valid request */
		if (task->scsi_dev == NULL) {
			task->tmf_resp->response = VIRTIO_SCSI_S_BAD_TARGET;
			break;
		}

		switch (ctrl_req->subtype) {
		case VIRTIO_SCSI_T_TMF_LOGICAL_UNIT_RESET:
			/* Handle LUN reset */
			SPDK_TRACELOG(SPDK_TRACE_VHOST_QUEUE, "LUN reset\n");
			task->scsi.type = SPDK_SCSI_TASK_TYPE_MANAGE;
			task->scsi.function = SPDK_SCSI_TASK_FUNC_LUN_RESET;
			task->scsi.lun = get_scsi_lun(task->scsi_dev, ctrl_req->lun);

			mgmt_task_submit(task);
			return;
		default:
			task->tmf_resp->response = VIRTIO_SCSI_S_ABORTED;
			/* Unsupported command */
			SPDK_TRACELOG(SPDK_TRACE_VHOST_QUEUE, "Unsupported TMF command %x\n", ctrl_req->subtype);
			break;
		}
		break;
	case VIRTIO_SCSI_T_AN_QUERY:
	case VIRTIO_SCSI_T_AN_SUBSCRIBE: {
		desc = vring_desc_get_next(controlq->desc, desc);
		an_resp = (void *)gpa_to_vva(vdev->dev, desc->addr);
		an_resp->response = VIRTIO_SCSI_S_ABORTED;
		break;
	}
	default:
		SPDK_TRACELOG(SPDK_TRACE_VHOST_QUEUE, "Unsupported control command %x\n", ctrl_req->type);
		break;
	}

	vq_used_ring_enqueue(vdev->dev, controlq, req_idx, 0);
	spdk_vhost_task_put(task);
}

/*
 * Process task's descriptor chain and setup data related fields.
 * Return
 *   -1 if request is invalid and must be aborted,
 *    0 if all data are set,
 *    1 if it was not possible to allocate IO vector for this task.
 */
static int
task_data_setup(struct spdk_vhost_task *task,
		struct virtio_scsi_cmd_req **req)
{
	struct rte_vhost_vring *vq = task->vq;
	struct spdk_vhost_dev *dev = task->vdev->dev;
	struct vring_desc *desc =  &task->vq->desc[task->req_idx];
	struct iovec *iovs = task->scsi.iovs;
	uint16_t iovcnt = 0, iovcnt_max = task->scsi.iovcnt;
	uint32_t len = 0;

	assert(iovcnt_max == 1 || iovcnt_max == VHOST_SCSI_IOVS_LEN);

	/* Sanity check. First descriptor must be readable and must have next one. */
	if (unlikely(vring_desc_is_wr(desc) || !vring_desc_has_next(desc))) {
		SPDK_WARNLOG("Invalid first (request) descriptor.\n");
		task->resp = NULL;
		goto abort_task;
	}

	*req = (void *)gpa_to_vva(dev, desc->addr);

	desc = vring_desc_get_next(vq->desc, desc);
	task->scsi.dxfer_dir = vring_desc_is_wr(desc) ? SPDK_SCSI_DIR_FROM_DEV : SPDK_SCSI_DIR_TO_DEV;

	if (task->scsi.dxfer_dir == SPDK_SCSI_DIR_FROM_DEV) {
		/*
		 * FROM_DEV (READ): [RD_req][WR_resp][WR_buf0]...[WR_bufN]
		 */
		task->resp = (void *)gpa_to_vva(dev, desc->addr);
		if (!vring_desc_has_next(desc)) {
			/*
			 * TEST UNIT READY command and some others might not contain any payload and this is not an error.
			 */
			SPDK_TRACELOG(SPDK_TRACE_VHOST_DATA,
				      "No payload descriptors for FROM DEV command req_idx=%"PRIu16".\n", task->req_idx);
			SPDK_TRACEDUMP(SPDK_TRACE_VHOST_DATA, "CDB=", (*req)->cdb, VIRTIO_SCSI_CDB_SIZE);
			task->scsi.iovcnt = 1;
			task->scsi.iovs[0].iov_len = 0;
			task->scsi.length = 0;
			task->scsi.transfer_len = 0;
			return 0;
		}

		desc = vring_desc_get_next(vq->desc, desc);
		if (iovcnt_max != VHOST_SCSI_IOVS_LEN && vring_desc_has_next(desc)) {
			iovs = spdk_vhost_iovec_alloc();
			if (iovs == NULL) {
				return 1;
			}

			iovcnt_max = VHOST_SCSI_IOVS_LEN;
		}

		/* All remaining descriptors are data. */
		while (iovcnt < iovcnt_max) {
			iovs[iovcnt].iov_base = (void *)gpa_to_vva(dev, desc->addr);
			iovs[iovcnt].iov_len = desc->len;
			len += desc->len;
			iovcnt++;

			if (!vring_desc_has_next(desc))
				break;

			desc = vring_desc_get_next(vq->desc, desc);
			if (unlikely(!vring_desc_is_wr(desc))) {
				SPDK_WARNLOG("FROM DEV cmd: descriptor nr %" PRIu16" in payload chain is read only.\n", iovcnt);
				task->resp = NULL;
				goto abort_task;
			}
		}
	} else {
		SPDK_TRACELOG(SPDK_TRACE_VHOST_DATA, "TO DEV");
		/*
		 * TO_DEV (WRITE):[RD_req][RD_buf0]...[RD_bufN][WR_resp]
		 * No need to check descriptor WR flag as this is done while setting scsi.dxfer_dir.
		 */

		if (iovcnt_max != VHOST_SCSI_IOVS_LEN && vring_desc_has_next(desc)) {
			/* If next descriptor is not for response, allocate iovs. */
			if (!vring_desc_is_wr(vring_desc_get_next(vq->desc, desc))) {
				iovs = spdk_vhost_iovec_alloc();

				if (iovs == NULL) {
					return 1;
				}

				iovcnt_max = VHOST_SCSI_IOVS_LEN;
			}
		}

		/* Process descriptors up to response. */
		while (!vring_desc_is_wr(desc) && iovcnt < iovcnt_max) {
			iovs[iovcnt].iov_base = (void *)gpa_to_vva(dev, desc->addr);
			iovs[iovcnt].iov_len = desc->len;
			len += desc->len;
			iovcnt++;

			if (!vring_desc_has_next(desc)) {
				SPDK_WARNLOG("TO_DEV cmd: no response descriptor.\n");
				task->resp = NULL;
				goto abort_task;
			}

			desc = vring_desc_get_next(vq->desc, desc);
		}

		task->resp = (void *)gpa_to_vva(dev, desc->addr);
		if (vring_desc_has_next(desc)) {
			SPDK_WARNLOG("TO_DEV cmd: ignoring unexpected descriptors after response descriptor.\n");
		}
	}

	if (iovcnt_max > 1 && iovcnt == iovcnt_max) {
		SPDK_WARNLOG("Too many IO vectors in chain!\n");
		goto abort_task;
	}

	task->scsi.iovs = iovs;
	task->scsi.iovcnt = iovcnt;
	task->scsi.length = len;
	task->scsi.transfer_len = len;
	return 0;

abort_task:
	if (iovs != task->scsi.iovs) {
		spdk_vhost_iovec_free(iovs);
	}

	if (task->resp) {
		task->resp->response = VIRTIO_SCSI_S_ABORTED;
	}

	return -1;
}

static int
process_request(struct spdk_vhost_task *task)
{
	struct virtio_scsi_cmd_req *req;
	int result;

	result = task_data_setup(task, &req);
	if (result) {
		return result;
	}

	task->scsi_dev = get_scsi_dev(task->vdev, req->lun);
	if (unlikely(task->scsi_dev == NULL)) {
		task->resp->response = VIRTIO_SCSI_S_BAD_TARGET;
		return -1;
	}

	task->scsi.lun = get_scsi_lun(task->scsi_dev, req->lun);
	task->scsi.cdb = req->cdb;
	task->scsi.target_port = spdk_scsi_dev_find_port_by_id(task->scsi_dev, 0);
	SPDK_TRACEDUMP(SPDK_TRACE_VHOST_DATA, "request CDB", req->cdb, VIRTIO_SCSI_CDB_SIZE);
	return 0;
}

static void
process_controlq(struct spdk_vhost_scsi_ctrlr *vdev, struct rte_vhost_vring *vq)
{
	uint16_t reqs[32];
	uint16_t reqs_cnt, i;

	reqs_cnt = vq_avail_ring_get(vq, reqs, RTE_DIM(reqs));
	for (i = 0; i < reqs_cnt; i++) {
		process_ctrl_request(vdev, vq, reqs[i]);
	}
}

static void
process_requestq(struct spdk_vhost_scsi_ctrlr *vdev, struct rte_vhost_vring *vq)
{
	uint16_t reqs[32];
	uint16_t reqs_cnt, i;
	struct spdk_vhost_task *task;
	int result;

	reqs_cnt = vq_avail_ring_get(vq, reqs, RTE_DIM(reqs));
	assert(reqs_cnt <= 32);

	for (i = 0; i < reqs_cnt; i++) {
		task = spdk_vhost_task_get(vdev);

		SPDK_TRACELOG(SPDK_TRACE_VHOST, "====== Starting processing request idx %"PRIu16"======\n",
			      reqs[i]);
		task->vq = vq;
		task->vdev = vdev;
		task->req_idx = reqs[i];
		result = process_request(task);
		if (likely(result == 0)) {
			task_submit(task);
			SPDK_TRACELOG(SPDK_TRACE_VHOST, "====== Task %p req_idx %d submitted ======\n", task,
				      task->req_idx);
		} else if (result > 0) {
			spdk_vhost_enqueue_task(task);
			SPDK_TRACELOG(SPDK_TRACE_VHOST, "====== Task %p req_idx %d deferred ======\n", task, task->req_idx);
		} else {
			invalid_request(task);
			SPDK_TRACELOG(SPDK_TRACE_VHOST, "====== Task %p req_idx %d failed ======\n", task, task->req_idx);
		}
	}
}

static void
vdev_controlq_worker(void *arg)
{
	struct spdk_vhost_scsi_ctrlr *vdev = arg;

	process_controlq(vdev, &vdev->dev->virtqueue[VIRTIO_SCSI_CONTROLQ]);
}

static void
vdev_worker(void *arg)
{
	struct spdk_vhost_scsi_ctrlr *vdev = arg;
	uint32_t q_idx;

	for (q_idx = VIRTIO_SCSI_REQUESTQ; q_idx < vdev->dev->num_queues; q_idx++) {
		process_requestq(vdev, &vdev->dev->virtqueue[q_idx]);
	}
}

#define SHIFT_2MB	21
#define SIZE_2MB	(1ULL << SHIFT_2MB)
#define FLOOR_2MB(x)	(((uintptr_t)x) / SIZE_2MB) << SHIFT_2MB
#define CEIL_2MB(x)	((((uintptr_t)x) + SIZE_2MB - 1) / SIZE_2MB) << SHIFT_2MB

static void
vdev_event_done_cb(void *arg1, void *arg2)
{
	sem_post((sem_t *)arg2);
}

static struct spdk_event *
vhost_sem_event_alloc(uint32_t core, spdk_event_fn fn, void *arg1, sem_t *sem)
{
	if (sem_init(sem, 0, 0) < 0)
		rte_panic("Failed to initialize semaphore.");

	return spdk_event_allocate(core, fn, arg1, sem);
}

static int
vhost_sem_timedwait(sem_t *sem, unsigned sec)
{
	struct timespec timeout;
	int rc;

	clock_gettime(CLOCK_REALTIME, &timeout);
	timeout.tv_sec += sec;

	rc = sem_timedwait(sem, &timeout);
	sem_destroy(sem);

	return rc;
}

static void
add_vdev_cb(void *arg1, void *arg2)
{
	struct spdk_vhost_scsi_ctrlr *vdev = arg1;
	struct rte_vhost_mem_region *region;
	uint32_t i;

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; i++) {
		if (vdev->scsi_dev[i] == NULL) {
			continue;
		}
		spdk_scsi_dev_allocate_io_channels(vdev->scsi_dev[i]);
	}
	SPDK_NOTICELOG("Started poller for vhost controller %s on lcore %d\n", vdev->name, vdev->lcore);

	for (i = 0; i < vdev->dev->mem->nregions; i++) {
		uint64_t start, end, len;
		region = &vdev->dev->mem->regions[i];
		start = FLOOR_2MB(region->mmap_addr);
		end = CEIL_2MB(region->mmap_addr + region->mmap_size);
		len = end - start;
		SPDK_NOTICELOG("Registering VM memory for vtophys translation - 0x%jx len:0x%jx\n",
			       start, len);
		spdk_mem_register((void *)start, len);
		spdk_iommu_mem_register(region->host_user_addr, region->size);

	}

	spdk_poller_register(&vdev->requestq_poller, vdev_worker, vdev, vdev->lcore, 0);
	spdk_poller_register(&vdev->controlq_poller, vdev_controlq_worker, vdev, vdev->lcore,
			     CONTROLQ_POLL_PERIOD_US);
	sem_post((sem_t *)arg2);
}

static void
remove_vdev_cb(void *arg1, void *arg2)
{
	struct spdk_vhost_scsi_ctrlr *vdev = arg1;
	struct rte_vhost_mem_region *region;
	uint32_t i;

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; i++) {
		if (vdev->scsi_dev[i] == NULL) {
			continue;
		}
		spdk_scsi_dev_free_io_channels(vdev->scsi_dev[i]);
	}

	SPDK_NOTICELOG("Stopping poller for vhost controller %s\n", vdev->name);
	for (i = 0; i < vdev->dev->mem->nregions; i++) {
		uint64_t start, end, len;
		region = &vdev->dev->mem->regions[i];
		start = FLOOR_2MB(region->mmap_addr);
		end = CEIL_2MB(region->mmap_addr + region->mmap_size);
		len = end - start;
		spdk_iommu_mem_unregister(region->host_user_addr, region->size);
		spdk_mem_unregister((void *)start, len);
	}

	sem_post((sem_t *)arg2);
}

static void
destroy_device(int vid)
{
	struct spdk_vhost_scsi_ctrlr *vdev;
	struct spdk_event *event;
	sem_t done_sem;
	uint32_t i;

	assert(vid < MAX_VHOST_DEVICE);
	vdev = dpdk_vid_mapping[vid];

	event = vhost_sem_event_alloc(vdev->lcore, vdev_event_done_cb, NULL, &done_sem);
	spdk_poller_unregister(&vdev->requestq_poller, event);
	if (vhost_sem_timedwait(&done_sem, 1))
		rte_panic("%s: failed to unregister request queue poller.\n", vdev->name);

	event = vhost_sem_event_alloc(vdev->lcore, vdev_event_done_cb, NULL, &done_sem);
	spdk_poller_unregister(&vdev->controlq_poller, event);
	if (vhost_sem_timedwait(&done_sem, 1))
		rte_panic("%s: failed to unregister control queue poller.\n", vdev->name);

	/* Wait for all tasks to finish */
	for (i = 1000; i && vdev->task_cnt > 0; i--) {
		usleep(1000);
	}

	if (vdev->task_cnt > 0) {
		rte_panic("%s: pending tasks did not finish in 1s.\n", vdev->name);
	}

	event = vhost_sem_event_alloc(vdev->lcore, remove_vdev_cb, vdev, &done_sem);
	spdk_event_call(event);
	if (vhost_sem_timedwait(&done_sem, 1))
		rte_panic("%s: failed to unregister poller.\n", vdev->name);

	g_num_ctrlrs[vdev->lcore]--;
	vdev->lcore = -1;

	spdk_vhost_dev_destruct(vdev->dev);
	vdev->dev = NULL;
	dpdk_vid_mapping[vid] = NULL;
}

#define LUN_DEV_NAME_SIZE 8
#define MAX_SCSI_CTRLRS 15

static struct spdk_vhost_scsi_ctrlr *spdk_vhost_ctrlrs[MAX_SCSI_CTRLRS];

static struct spdk_vhost_scsi_ctrlr *
spdk_vhost_scsi_ctrlr_find(const char *ctrlr_name)
{
	unsigned i;
	size_t dev_dirname_len = strlen(dev_dirname);

	if (strncmp(ctrlr_name, dev_dirname, dev_dirname_len) == 0) {
		ctrlr_name += dev_dirname_len;
	}

	for (i = 0; i < MAX_SCSI_CTRLRS; i++) {
		if (spdk_vhost_ctrlrs[i] == NULL) {
			continue;
		}

		if (strcmp(spdk_vhost_ctrlrs[i]->name, ctrlr_name) == 0) {
			return spdk_vhost_ctrlrs[i];
		}
	}

	return NULL;
}


static int new_device(int vid);
static void destroy_device(int vid);
/*
 * These callback allow devices to be added to the data core when configuration
 * has been fully complete.
 */
static const struct vhost_device_ops spdk_vhost_scsi_device_ops = {
	.new_device =  new_device,
	.destroy_device = destroy_device,
};

int
spdk_vhost_scsi_ctrlr_construct(const char *name, uint64_t cpumask)
{
	struct spdk_vhost_scsi_ctrlr *vdev;
	unsigned ctrlr_num;
	char path[PATH_MAX];
	struct stat file_stat;

	if (name == NULL) {
		SPDK_ERRLOG("Can't add controller with no name\n");
		return -EINVAL;
	}

	if ((cpumask & spdk_app_get_core_mask()) != cpumask) {
		SPDK_ERRLOG("cpumask 0x%jx not a subset of app mask 0x%jx\n",
			    cpumask, spdk_app_get_core_mask());
		return -EINVAL;
	}

	if (spdk_vhost_scsi_ctrlr_find(name)) {
		SPDK_ERRLOG("vhost scsi controller %s already exists.\n", name);
		return -EEXIST;
	}

	for (ctrlr_num = 0; ctrlr_num < MAX_SCSI_CTRLRS; ctrlr_num++) {
		if (spdk_vhost_ctrlrs[ctrlr_num] == NULL) {
			break;
		}
	}

	if (ctrlr_num == MAX_SCSI_CTRLRS) {
		SPDK_ERRLOG("Max scsi controllers reached (%d).\n", MAX_SCSI_CTRLRS);
		return -ENOSPC;
	}

	if (snprintf(path, sizeof(path), "%s%s", dev_dirname, name) >= (int)sizeof(path)) {
		SPDK_ERRLOG("Resulting socket path for controller %s is too long: %s%s\n", name, dev_dirname, name);
		return -EINVAL;
	}

	/* Register vhost driver to handle vhost messages. */
	if (stat(path, &file_stat) != -1) {
		if (!S_ISSOCK(file_stat.st_mode)) {
			SPDK_ERRLOG("Cannot remove %s: not a socket.\n", path);
			return -EINVAL;
		} else if (unlink(path) != 0) {
			rte_exit(EXIT_FAILURE, "Cannot remove %s.\n", path);
		}
	}

	if (rte_vhost_driver_register(path, 0) != 0) {
		SPDK_ERRLOG("Could not register controller %s with vhost library\n", name);
		SPDK_ERRLOG("Check if domain socket %s already exists\n", path);
		return -EIO;
	}
	if (rte_vhost_driver_set_features(path, SPDK_VHOST_SCSI_FEATURES) ||
	    rte_vhost_driver_disable_features(path, SPDK_VHOST_SCSI_DISABLED_FEATURES)) {
		SPDK_ERRLOG("Couldn't set vhost features for controller %s\n", name);
		return -EINVAL;
	}

	if (rte_vhost_driver_callback_register(path, &spdk_vhost_scsi_device_ops) != 0) {
		SPDK_ERRLOG("Couldn't register callbacks for controller %s\n", name);
		return -ENOENT;
	}

	vdev = spdk_zmalloc(sizeof(*vdev), RTE_CACHE_LINE_SIZE, NULL);
	if (vdev == NULL) {
		SPDK_ERRLOG("Couldn't allocate memory for vhost dev\n");
		return -ENOMEM;
	}

	vdev->name =  strdup(name);
	vdev->cpumask = cpumask;
	vdev->lcore = -1;

	if (rte_vhost_driver_start(path) != 0) {
		SPDK_ERRLOG("Failed to start vhost driver for controller %s (%d): %s", name, errno,
			    strerror(errno));
		free(vdev->name);
		spdk_free(vdev);
		return -EIO;
	}

	spdk_vhost_ctrlrs[ctrlr_num] = vdev;
	SPDK_NOTICELOG("Controller %s: new controller added\n", name);
	return 0;
}

int
spdk_vhost_parse_core_mask(const char *mask, uint64_t *cpumask)
{
	char *end;

	if (mask == NULL || cpumask == NULL) {
		return -1;
	}

	errno = 0;
	*cpumask = strtoull(mask, &end, 16);

	if (*end != '\0' || errno || !*cpumask ||
	    ((*cpumask & spdk_app_get_core_mask()) != *cpumask)) {

		SPDK_ERRLOG("cpumask %s not a subset of app mask 0x%jx\n",
			    mask, spdk_app_get_core_mask());
		return -1;
	}

	return 0;
}

struct spdk_scsi_dev *
spdk_vhost_scsi_ctrlr_get_dev(struct spdk_vhost_scsi_ctrlr *ctrlr, uint8_t num)
{
	assert(ctrlr != NULL);
	assert(num < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS);
	return ctrlr->scsi_dev[num];
}

int
spdk_vhost_scsi_ctrlr_add_dev(const char *ctrlr_name, unsigned scsi_dev_num, const char *lun_name)
{
	struct spdk_vhost_scsi_ctrlr *vdev;
	char dev_name[SPDK_SCSI_DEV_MAX_NAME];
	int lun_id_list[1];
	char *lun_names_list[1];

	if (ctrlr_name == NULL) {
		SPDK_ERRLOG("No controller name\n");
		return -EINVAL;
	}

	if (scsi_dev_num >= SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
		SPDK_ERRLOG("Controller %d device number too big (max %d)\n", scsi_dev_num,
			    SPDK_VHOST_SCSI_CTRLR_MAX_DEVS);
		return -EINVAL;
	}

	if (lun_name == NULL) {
		SPDK_ERRLOG("No lun name specified \n");
		return -EINVAL;
	} else if (strlen(lun_name) >= SPDK_SCSI_DEV_MAX_NAME) {
		SPDK_ERRLOG("LUN name '%s' too long (max %d).\n", lun_name, SPDK_SCSI_DEV_MAX_NAME - 1);
		return -1;
	}

	vdev = spdk_vhost_scsi_ctrlr_find(ctrlr_name);
	if (vdev == NULL) {
		SPDK_ERRLOG("Controller %s is not defined\n", ctrlr_name);
		return -ENODEV;
	}

	if (vdev->lcore != -1) {
		SPDK_ERRLOG("Controller %s is in use and hotplug is not supported\n", ctrlr_name);
		return -ENODEV;
	}

	if (vdev->scsi_dev[scsi_dev_num] != NULL) {
		SPDK_ERRLOG("Controller %s dev %u already occupied\n", ctrlr_name, scsi_dev_num);
		return -EEXIST;
	}

	/*
	 * At this stage only one LUN per device
	 */
	snprintf(dev_name, sizeof(dev_name), "Dev %u", scsi_dev_num);
	lun_id_list[0] = 0;
	lun_names_list[0] = (char *)lun_name;

	vdev->scsi_dev[scsi_dev_num] = spdk_scsi_dev_construct(dev_name, lun_names_list, lun_id_list, 1);
	if (vdev->scsi_dev[scsi_dev_num] == NULL) {
		SPDK_ERRLOG("Couldn't create spdk SCSI device '%s' using lun device '%s' in controller: %s\n",
			    dev_name, lun_name, vdev->name);
		return -EINVAL;
	}

	spdk_scsi_dev_add_port(vdev->scsi_dev[scsi_dev_num], 0, "vhost");
	SPDK_NOTICELOG("Controller %s: defined device '%s' using lun '%s'\n",
		       vdev->name, dev_name, lun_name);
	return 0;
}

struct spdk_vhost_scsi_ctrlr *
spdk_vhost_scsi_ctrlr_next(struct spdk_vhost_scsi_ctrlr *prev)
{
	int i = 0;

	if (prev != NULL) {
		for (; i < MAX_SCSI_CTRLRS; i++) {
			if (spdk_vhost_ctrlrs[i] == prev) {
				break;
			}
		}

		i++;
	}

	for (; i < MAX_SCSI_CTRLRS; i++) {
		if (spdk_vhost_ctrlrs[i] == NULL) {
			continue;
		}

		return spdk_vhost_ctrlrs[i];
	}

	return NULL;
}

const char *
spdk_vhost_scsi_ctrlr_get_name(struct spdk_vhost_scsi_ctrlr *ctrlr)
{
	assert(ctrlr != NULL);
	return ctrlr->name;
}

uint64_t
spdk_vhost_scsi_ctrlr_get_cpumask(struct spdk_vhost_scsi_ctrlr *ctrlr)
{
	assert(ctrlr != NULL);
	return ctrlr->cpumask;
}

static int spdk_vhost_scsi_controller_construct(void)
{
	struct spdk_conf_section *sp = spdk_conf_first_section(NULL);
	int i, dev_num;
	unsigned ctrlr_num = 0;
	char *lun_name, *dev_num_str;
	char *cpumask_str;
	char *name;
	uint64_t cpumask;

	while (sp != NULL) {
		if (!spdk_conf_section_match_prefix(sp, "VhostScsi")) {
			sp = spdk_conf_next_section(sp);
			continue;
		}

		if (sscanf(spdk_conf_section_get_name(sp), "VhostScsi%u", &ctrlr_num) != 1) {
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

		if (spdk_vhost_scsi_ctrlr_construct(name, cpumask) < 0) {
			return -1;
		}

		for (i = 0; spdk_conf_section_get_nval(sp, "Dev", i) != NULL; i++) {
			dev_num_str = spdk_conf_section_get_nmval(sp, "Dev", i, 0);
			if (dev_num_str == NULL) {
				SPDK_ERRLOG("%s: Invalid or missing Dev number\n", name);
				return -1;
			}

			dev_num = (int)strtol(dev_num_str, NULL, 10);
			lun_name = spdk_conf_section_get_nmval(sp, "Dev", i, 1);
			if (lun_name == NULL) {
				SPDK_ERRLOG("%s: Invalid or missing LUN name for dev %d\n", name, dev_num);
				return -1;
			} else if (spdk_conf_section_get_nmval(sp, "Dev", i, 2)) {
				SPDK_ERRLOG("%s: Only one LUN per vhost SCSI device supported\n", name);
				return -1;
			}

			if (spdk_vhost_scsi_ctrlr_add_dev(name, dev_num, lun_name) < 0) {
				return -1;
			}
		}

		sp = spdk_conf_next_section(sp);

	}

	return 0;
}

static uint32_t
spdk_vhost_scsi_allocate_reactor(uint64_t cpumask)
{
	uint32_t i, selected_core;
	uint32_t min_ctrlrs;

	cpumask &= spdk_app_get_core_mask();

	if (cpumask == 0) {
		return 0;
	}

	min_ctrlrs = INT_MAX;
	selected_core = 0;

	for (i = 0; i < RTE_MAX_LCORE && i < 64; i++) {
		if (!((1ULL << i) & cpumask)) {
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

/*
 * A new device is added to a data core. First the device is added to the main linked list
 * and then allocated to a specific data core.
 */
static int
new_device(int vid)
{
	struct spdk_vhost_scsi_ctrlr *vdev = NULL;
	struct spdk_event *event;

	char ifname[PATH_MAX];
	sem_t added;

	assert(vid < MAX_VHOST_DEVICE);

	if (rte_vhost_get_ifname(vid, ifname, PATH_MAX) < 0) {
		SPDK_ERRLOG("Couldn't get a valid ifname for device %d\n", vid);
		return -1;
	}

	vdev = spdk_vhost_scsi_ctrlr_find(ifname);
	if (vdev == NULL) {
		SPDK_ERRLOG("Controller %s not found.\n", ifname);
		return -1;
	}

	if (vdev->lcore != -1) {
		SPDK_ERRLOG("Controller %s already connected.\n", ifname);
		return -1;
	}

	assert(vdev->dev == NULL);
	vdev->dev = spdk_vhost_dev_create(vid);
	if (vdev->dev == NULL) {
		return -1;
	}

	dpdk_vid_mapping[vid] = vdev;
	vdev->lcore = spdk_vhost_scsi_allocate_reactor(vdev->cpumask);

	event = vhost_sem_event_alloc(vdev->lcore, add_vdev_cb, vdev, &added);
	spdk_event_call(event);
	if (vhost_sem_timedwait(&added, 1))
		rte_panic("Failed to register new device '%s'\n", vdev->name);
	return 0;
}

void
spdk_vhost_startup(void *arg1, void *arg2)
{
	int ret;
	const char *basename = arg1;

	if (basename && strlen(basename) > 0) {
		ret = snprintf(dev_dirname, sizeof(dev_dirname) - 2, "%s", basename);
		if ((size_t)ret >= sizeof(dev_dirname) - 2) {
			rte_exit(EXIT_FAILURE, "Char dev dir path length %d is too long\n", ret);
		}

		if (dev_dirname[ret - 1] != '/') {
			dev_dirname[ret] = '/';
			dev_dirname[ret + 1]  = '\0';
		}
	}

	ret = spdk_vhost_scsi_controller_construct();
	if (ret != 0)
		rte_exit(EXIT_FAILURE, "Cannot construct vhost controllers\n");
}

static void *
session_shutdown(void *arg)
{
	struct spdk_vhost_scsi_ctrlr *vdev = NULL;
	int i;

	for (i = 0; i < MAX_SCSI_CTRLRS; i++) {
		vdev = spdk_vhost_ctrlrs[i];
		if (vdev == NULL) {
			continue;
		}
		rte_vhost_driver_unregister(vdev->name);
	}

	SPDK_NOTICELOG("Exiting\n");
	spdk_app_stop(0);
	return NULL;
}

/*
 * When we receive a INT signal. Execute shutdown in separate thread to avoid deadlock.
 */
void
spdk_vhost_shutdown_cb(void)
{
	pthread_t tid;
	if (pthread_create(&tid, NULL, &session_shutdown, NULL) < 0)
		rte_panic("Failed to start session shutdown thread (%d): %s", errno, strerror(errno));
	pthread_detach(tid);
}

SPDK_LOG_REGISTER_TRACE_FLAG("vhost", SPDK_TRACE_VHOST)
SPDK_LOG_REGISTER_TRACE_FLAG("vhost_ring", SPDK_TRACE_VHOST_RING)
SPDK_LOG_REGISTER_TRACE_FLAG("vhost_queue", SPDK_TRACE_VHOST_QUEUE)
SPDK_LOG_REGISTER_TRACE_FLAG("vhost_data", SPDK_TRACE_VHOST_DATA)
