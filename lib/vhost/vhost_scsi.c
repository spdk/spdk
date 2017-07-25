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

#include <linux/virtio_scsi.h>

#include "spdk/env.h"
#include "spdk/scsi.h"
#include "spdk/scsi_spec.h"
#include "spdk/conf.h"
#include "spdk/event.h"
#include "spdk/util.h"
#include "spdk/likely.h"

#include "spdk/vhost.h"
#include "vhost_internal.h"

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
 * - T10 PI
 */
#define SPDK_VHOST_SCSI_DISABLED_FEATURES	((1ULL << VHOST_F_LOG_ALL) | \
						(1ULL << VIRTIO_SCSI_F_T10_PI ))

#define MGMT_POLL_PERIOD_US (1000 * 5)

#define VIRTIO_SCSI_CONTROLQ   0
#define VIRTIO_SCSI_EVENTQ   1
#define VIRTIO_SCSI_REQUESTQ   2

/* Allocated iovec buffer len */
#define SPDK_VHOST_SCSI_IOVS_LEN 128

struct spdk_scsi_dev_vhost_state {
	bool removed;
	void (*remove_cb)(void *, void *);
	void *remove_ctx;
};

struct spdk_vhost_scsi_dev {
	struct spdk_vhost_dev vdev;
	struct spdk_scsi_dev *scsi_dev[SPDK_VHOST_SCSI_CTRLR_MAX_DEVS];
	struct spdk_scsi_dev_vhost_state scsi_dev_state[SPDK_VHOST_SCSI_CTRLR_MAX_DEVS];

	struct spdk_ring *task_pool;
	struct spdk_poller *requestq_poller;
	struct spdk_poller *mgmt_poller;
} __rte_cache_aligned;

struct spdk_vhost_scsi_task {
	struct spdk_scsi_task	scsi;
	struct iovec iovs[SPDK_VHOST_SCSI_IOVS_LEN];

	union {
		struct virtio_scsi_cmd_resp *resp;
		struct virtio_scsi_ctrl_tmf_resp *tmf_resp;
	};

	struct spdk_vhost_scsi_dev *svdev;
	struct spdk_scsi_dev *scsi_dev;

	int req_idx;

	struct rte_vhost_vring *vq;
};

static int new_device(int vid);
static void destroy_device(int vid);

const struct spdk_vhost_dev_backend spdk_vhost_scsi_device_backend = {
	.virtio_features = SPDK_VHOST_SCSI_FEATURES,
	.disabled_features = SPDK_VHOST_SCSI_DISABLED_FEATURES,
	.ops = {
		.new_device =  new_device,
		.destroy_device = destroy_device,
	}
};

static void
spdk_vhost_scsi_task_put(struct spdk_vhost_scsi_task *task)
{
	spdk_scsi_task_put(&task->scsi);
}

static void
spdk_vhost_scsi_task_free_cb(struct spdk_scsi_task *scsi_task)
{
	struct spdk_vhost_scsi_task *task = SPDK_CONTAINEROF(scsi_task, struct spdk_vhost_scsi_task, scsi);

	assert(task->svdev->vdev.task_cnt > 0);
	task->svdev->vdev.task_cnt--;
	spdk_ring_enqueue(task->svdev->task_pool, (void **) &task, 1);
}

static void
spdk_vhost_get_tasks(struct spdk_vhost_scsi_dev *svdev, struct spdk_vhost_scsi_task **tasks,
		     size_t count)
{
	size_t res_count;

	res_count = spdk_ring_dequeue(svdev->task_pool, (void **)tasks, count);
	if (res_count != count) {
		SPDK_ERRLOG("%s: couldn't get %zu tasks from task_pool\n", svdev->vdev.name, count);
		/* FIXME: we should never run out of tasks, but what if we do? */
		abort();
	}

	assert(svdev->vdev.task_cnt <= INT_MAX - (int) res_count);
	svdev->vdev.task_cnt += res_count;
}

static void
process_removed_devs(struct spdk_vhost_scsi_dev *svdev)
{
	struct spdk_scsi_dev *dev;
	struct spdk_scsi_dev_vhost_state *state;
	int i;

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; ++i) {
		dev = svdev->scsi_dev[i];
		state = &svdev->scsi_dev_state[i];

		if (dev && state->removed && !spdk_scsi_dev_has_pending_tasks(dev)) {
			spdk_scsi_dev_free_io_channels(dev);
			spdk_scsi_dev_destruct(dev);
			svdev->scsi_dev[i] = NULL;
			if (state->remove_cb) {
				state->remove_cb(&svdev->vdev, state->remove_ctx);
			}
			SPDK_NOTICELOG("%s: hotremoved device 'Dev %u'.\n", svdev->vdev.name, i);
		}
	}
}

static void
eventq_enqueue(struct spdk_vhost_scsi_dev *svdev, unsigned scsi_dev_num, uint32_t event,
	       uint32_t reason)
{
	struct rte_vhost_vring *vq;
	struct vring_desc *desc;
	struct virtio_scsi_event *desc_ev;
	uint32_t req_size;
	uint16_t req;

	assert(scsi_dev_num < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS);

	vq = &svdev->vdev.virtqueue[VIRTIO_SCSI_EVENTQ];

	if (spdk_vhost_vq_avail_ring_get(vq, &req, 1) != 1) {
		SPDK_ERRLOG("Controller %s: Failed to send virtio event (no avail ring entries?).\n",
			    svdev->vdev.name);
		return;
	}

	desc =  spdk_vhost_vq_get_desc(vq, req);
	desc_ev = spdk_vhost_gpa_to_vva(&svdev->vdev, desc->addr);

	if (desc->len < sizeof(*desc_ev) || desc_ev != NULL) {
		SPDK_ERRLOG("Controller %s: Invalid eventq descriptor.\n", svdev->vdev.name);
		req_size = 0;
	} else {
		desc_ev->event = event;
		desc_ev->lun[0] = 1;
		desc_ev->lun[1] = scsi_dev_num;
		/* virtio LUN id 0 can refer either to the entire device
		 * or actual LUN 0 (the only supported by vhost for now)
		 */
		desc_ev->lun[2] = 0 >> 8;
		desc_ev->lun[3] = 0 & 0xFF;
		/* virtio doesn't specify any strict format for LUN id (bytes 2 and 3)
		 * current implementation relies on linux kernel sources */
		memset(&desc_ev->lun[4], 0, 4);
		desc_ev->reason = reason;
	}

	spdk_vhost_vq_used_ring_enqueue(&svdev->vdev, vq, req, req_size);
}

static void
submit_completion(struct spdk_vhost_scsi_task *task)
{
	spdk_vhost_vq_used_ring_enqueue(&task->svdev->vdev, task->vq, task->req_idx,
					task->scsi.data_transferred);
	SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI, "Finished task (%p) req_idx=%d\n", task, task->req_idx);

	spdk_vhost_scsi_task_put(task);
}

static void
spdk_vhost_scsi_task_mgmt_cpl(struct spdk_scsi_task *scsi_task)
{
	struct spdk_vhost_scsi_task *task = SPDK_CONTAINEROF(scsi_task, struct spdk_vhost_scsi_task, scsi);

	submit_completion(task);
}

static void
spdk_vhost_scsi_task_cpl(struct spdk_scsi_task *scsi_task)
{
	struct spdk_vhost_scsi_task *task = SPDK_CONTAINEROF(scsi_task, struct spdk_vhost_scsi_task, scsi);

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
task_submit(struct spdk_vhost_scsi_task *task)
{
	/* The task is ready to be submitted.  First create the callback event that
	   will be invoked when the SCSI command is completed.  See spdk_vhost_scsi_task_cpl()
	   for what SPDK vhost-scsi does when the task is completed.
	 */

	task->resp->response = VIRTIO_SCSI_S_OK;
	spdk_scsi_dev_queue_task(task->scsi_dev, &task->scsi);
}

static void
mgmt_task_submit(struct spdk_vhost_scsi_task *task, enum spdk_scsi_task_func func)
{
	task->tmf_resp->response = VIRTIO_SCSI_S_OK;
	spdk_scsi_dev_queue_mgmt_task(task->scsi_dev, &task->scsi, func);
}

static void
invalid_request(struct spdk_vhost_scsi_task *task)
{
	spdk_vhost_vq_used_ring_enqueue(&task->svdev->vdev, task->vq, task->req_idx, 0);
	spdk_vhost_scsi_task_put(task);

	SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI, "Invalid request (status=%" PRIu8")\n",
		      task->resp ? task->resp->response : -1);
}

static int
spdk_vhost_scsi_task_init_target(struct spdk_vhost_scsi_task *task, const __u8 *lun)
{
	struct spdk_scsi_dev *dev;
	uint16_t lun_id = (((uint16_t)lun[2] << 8) | lun[3]) & 0x3FFF;

	SPDK_TRACEDUMP(SPDK_TRACE_VHOST_SCSI_QUEUE, "LUN", lun, 8);

	/* First byte must be 1 and second is target */
	if (lun[0] != 1 || lun[1] >= SPDK_VHOST_SCSI_CTRLR_MAX_DEVS)
		return -1;

	dev = task->svdev->scsi_dev[lun[1]];
	task->scsi_dev = dev;
	if (dev == NULL) {
		/* If dev has been hotremoved, return 0 to allow sending additional
		 * hotremove event via sense codes.
		 */
		return task->svdev->scsi_dev_state[lun[1]].removed ? 0 : -1;
	}

	task->scsi.target_port = spdk_scsi_dev_find_port_by_id(task->scsi_dev, 0);
	task->scsi.lun = spdk_scsi_dev_get_lun(dev, lun_id);
	return 0;
}

static void
process_ctrl_request(struct spdk_vhost_scsi_task *task)
{
	struct vring_desc *desc;
	struct virtio_scsi_ctrl_tmf_req *ctrl_req;
	struct virtio_scsi_ctrl_an_resp *an_resp;

	spdk_scsi_task_construct(&task->scsi, spdk_vhost_scsi_task_mgmt_cpl, spdk_vhost_scsi_task_free_cb,
				 NULL);
	desc = spdk_vhost_vq_get_desc(task->vq, task->req_idx);
	ctrl_req = spdk_vhost_gpa_to_vva(&task->svdev->vdev, desc->addr);

	SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI_QUEUE,
		      "Processing controlq descriptor: desc %d/%p, desc_addr %p, len %d, flags %d, last_used_idx %d; kickfd %d; size %d\n",
		      task->req_idx, desc, (void *)desc->addr, desc->len, desc->flags, task->vq->last_used_idx,
		      task->vq->kickfd, task->vq->size);
	SPDK_TRACEDUMP(SPDK_TRACE_VHOST_SCSI_QUEUE, "Request desriptor", (uint8_t *)ctrl_req,
		       desc->len);

	spdk_vhost_scsi_task_init_target(task, ctrl_req->lun);

	/* Process the TMF request */
	switch (ctrl_req->type) {
	case VIRTIO_SCSI_T_TMF:
		/* Get the response buffer */
		assert(spdk_vhost_vring_desc_has_next(desc));
		desc = spdk_vhost_vring_desc_get_next(task->vq->desc, desc);
		task->tmf_resp = spdk_vhost_gpa_to_vva(&task->svdev->vdev, desc->addr);

		/* Check if we are processing a valid request */
		if (task->scsi_dev == NULL) {
			task->tmf_resp->response = VIRTIO_SCSI_S_BAD_TARGET;
			break;
		}

		switch (ctrl_req->subtype) {
		case VIRTIO_SCSI_T_TMF_LOGICAL_UNIT_RESET:
			/* Handle LUN reset */
			SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI_QUEUE, "LUN reset\n");

			mgmt_task_submit(task, SPDK_SCSI_TASK_FUNC_LUN_RESET);
			return;
		default:
			task->tmf_resp->response = VIRTIO_SCSI_S_ABORTED;
			/* Unsupported command */
			SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI_QUEUE, "Unsupported TMF command %x\n", ctrl_req->subtype);
			break;
		}
		break;
	case VIRTIO_SCSI_T_AN_QUERY:
	case VIRTIO_SCSI_T_AN_SUBSCRIBE: {
		desc = spdk_vhost_vring_desc_get_next(task->vq->desc, desc);
		an_resp = spdk_vhost_gpa_to_vva(&task->svdev->vdev, desc->addr);
		an_resp->response = VIRTIO_SCSI_S_ABORTED;
		break;
	}
	default:
		SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI_QUEUE, "Unsupported control command %x\n", ctrl_req->type);
		break;
	}

	spdk_vhost_vq_used_ring_enqueue(&task->svdev->vdev, task->vq, task->req_idx, 0);
	spdk_vhost_scsi_task_put(task);
}

/*
 * Process task's descriptor chain and setup data related fields.
 * Return
 *   -1 if request is invalid and must be aborted,
 *    0 if all data are set,
 *    1 if it was not possible to allocate IO vector for this task.
 */
static int
task_data_setup(struct spdk_vhost_scsi_task *task,
		struct virtio_scsi_cmd_req **req)
{
	struct rte_vhost_vring *vq = task->vq;
	struct spdk_vhost_dev *vdev = &task->svdev->vdev;
	struct vring_desc *desc =  spdk_vhost_vq_get_desc(task->vq, task->req_idx);
	struct iovec *iovs = task->iovs;
	uint16_t iovcnt = 0, iovcnt_max = SPDK_VHOST_SCSI_IOVS_LEN;
	uint32_t len = 0;

	/* Sanity check. First descriptor must be readable and must have next one. */
	if (spdk_unlikely(spdk_vhost_vring_desc_is_wr(desc) || !spdk_vhost_vring_desc_has_next(desc))) {
		SPDK_WARNLOG("Invalid first (request) descriptor.\n");
		task->resp = NULL;
		goto abort_task;
	}

	spdk_scsi_task_construct(&task->scsi, spdk_vhost_scsi_task_cpl, spdk_vhost_scsi_task_free_cb, NULL);
	*req = spdk_vhost_gpa_to_vva(vdev, desc->addr);

	desc = spdk_vhost_vring_desc_get_next(vq->desc, desc);
	task->scsi.dxfer_dir = spdk_vhost_vring_desc_is_wr(desc) ? SPDK_SCSI_DIR_FROM_DEV :
			       SPDK_SCSI_DIR_TO_DEV;
	task->scsi.iovs = iovs;

	if (task->scsi.dxfer_dir == SPDK_SCSI_DIR_FROM_DEV) {
		/*
		 * FROM_DEV (READ): [RD_req][WR_resp][WR_buf0]...[WR_bufN]
		 */
		task->resp = spdk_vhost_gpa_to_vva(vdev, desc->addr);
		if (!spdk_vhost_vring_desc_has_next(desc)) {
			/*
			 * TEST UNIT READY command and some others might not contain any payload and this is not an error.
			 */
			SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI_DATA,
				      "No payload descriptors for FROM DEV command req_idx=%"PRIu16".\n", task->req_idx);
			SPDK_TRACEDUMP(SPDK_TRACE_VHOST_SCSI_DATA, "CDB=", (*req)->cdb, VIRTIO_SCSI_CDB_SIZE);
			task->scsi.iovcnt = 1;
			task->scsi.iovs[0].iov_len = 0;
			task->scsi.length = 0;
			task->scsi.transfer_len = 0;
			return 0;
		}

		desc = spdk_vhost_vring_desc_get_next(vq->desc, desc);

		/* All remaining descriptors are data. */
		while (iovcnt < iovcnt_max) {
			spdk_vhost_vring_desc_to_iov(vdev, &iovs[iovcnt], desc);
			len += desc->len;
			iovcnt++;

			if (!spdk_vhost_vring_desc_has_next(desc))
				break;

			desc = spdk_vhost_vring_desc_get_next(vq->desc, desc);
			if (spdk_unlikely(!spdk_vhost_vring_desc_is_wr(desc))) {
				SPDK_WARNLOG("FROM DEV cmd: descriptor nr %" PRIu16" in payload chain is read only.\n", iovcnt);
				task->resp = NULL;
				goto abort_task;
			}
		}
	} else {
		SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI_DATA, "TO DEV");
		/*
		 * TO_DEV (WRITE):[RD_req][RD_buf0]...[RD_bufN][WR_resp]
		 * No need to check descriptor WR flag as this is done while setting scsi.dxfer_dir.
		 */

		/* Process descriptors up to response. */
		while (!spdk_vhost_vring_desc_is_wr(desc) && iovcnt < iovcnt_max) {
			spdk_vhost_vring_desc_to_iov(vdev, &iovs[iovcnt], desc);
			len += desc->len;
			iovcnt++;

			if (!spdk_vhost_vring_desc_has_next(desc)) {
				SPDK_WARNLOG("TO_DEV cmd: no response descriptor.\n");
				task->resp = NULL;
				goto abort_task;
			}

			desc = spdk_vhost_vring_desc_get_next(vq->desc, desc);
		}

		task->resp = spdk_vhost_gpa_to_vva(vdev, desc->addr);
		if (spdk_vhost_vring_desc_has_next(desc)) {
			SPDK_WARNLOG("TO_DEV cmd: ignoring unexpected descriptors after response descriptor.\n");
		}
	}

	if (iovcnt == iovcnt_max) {
		SPDK_WARNLOG("Too many IO vectors in chain!\n");
		goto abort_task;
	}

	task->scsi.iovcnt = iovcnt;
	task->scsi.length = len;
	task->scsi.transfer_len = len;
	return 0;

abort_task:
	if (task->resp) {
		task->resp->response = VIRTIO_SCSI_S_ABORTED;
	}

	return -1;
}

static int
process_request(struct spdk_vhost_scsi_task *task)
{
	struct virtio_scsi_cmd_req *req;
	int result;

	result = task_data_setup(task, &req);
	if (result) {
		return result;
	}

	result = spdk_vhost_scsi_task_init_target(task, req->lun);
	if (spdk_unlikely(result != 0)) {
		task->resp->response = VIRTIO_SCSI_S_BAD_TARGET;
		return -1;
	}

	task->scsi.cdb = req->cdb;
	SPDK_TRACEDUMP(SPDK_TRACE_VHOST_SCSI_DATA, "request CDB", req->cdb, VIRTIO_SCSI_CDB_SIZE);

	if (spdk_unlikely(task->scsi.lun == NULL)) {
		spdk_scsi_task_process_null_lun(&task->scsi);
		task->resp->response = VIRTIO_SCSI_S_OK;
		return 1;
	}

	return 0;
}

static void
process_controlq(struct spdk_vhost_scsi_dev *svdev, struct rte_vhost_vring *vq)
{
	struct spdk_vhost_scsi_task *tasks[32];
	struct spdk_vhost_scsi_task *task;
	uint16_t reqs[32];
	uint16_t reqs_cnt, i;

	reqs_cnt = spdk_vhost_vq_avail_ring_get(vq, reqs, SPDK_COUNTOF(reqs));
	spdk_vhost_get_tasks(svdev, tasks, reqs_cnt);
	for (i = 0; i < reqs_cnt; i++) {
		task = tasks[i];
		memset(task, 0, sizeof(*task));
		task->vq = vq;
		task->svdev = svdev;
		task->req_idx = reqs[i];

		process_ctrl_request(task);
	}
}

static void
process_requestq(struct spdk_vhost_scsi_dev *svdev, struct rte_vhost_vring *vq)
{
	struct spdk_vhost_scsi_task *tasks[32];
	struct spdk_vhost_scsi_task *task;
	uint16_t reqs[32];
	uint16_t reqs_cnt, i;
	int result;

	reqs_cnt = spdk_vhost_vq_avail_ring_get(vq, reqs, SPDK_COUNTOF(reqs));
	assert(reqs_cnt <= 32);

	spdk_vhost_get_tasks(svdev, tasks, reqs_cnt);

	for (i = 0; i < reqs_cnt; i++) {
		SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI, "====== Starting processing request idx %"PRIu16"======\n",
			      reqs[i]);

		task = tasks[i];
		memset(task, 0, sizeof(*task));
		task->vq = vq;
		task->svdev = svdev;
		task->req_idx = reqs[i];
		result = process_request(task);
		if (likely(result == 0)) {
			task_submit(task);
			SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI, "====== Task %p req_idx %d submitted ======\n", task,
				      task->req_idx);
		} else if (result > 0) {
			spdk_vhost_scsi_task_cpl(&task->scsi);
			SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI, "====== Task %p req_idx %d finished early ======\n", task,
				      task->req_idx);
		} else {
			invalid_request(task);
			SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI, "====== Task %p req_idx %d failed ======\n", task,
				      task->req_idx);
		}
	}
}

static void
vdev_mgmt_worker(void *arg)
{
	struct spdk_vhost_scsi_dev *svdev = arg;

	process_removed_devs(svdev);
	process_controlq(svdev, &svdev->vdev.virtqueue[VIRTIO_SCSI_CONTROLQ]);
}

static void
vdev_worker(void *arg)
{
	struct spdk_vhost_scsi_dev *svdev = arg;
	uint32_t q_idx;

	for (q_idx = VIRTIO_SCSI_REQUESTQ; q_idx < svdev->vdev.num_queues; q_idx++) {
		process_requestq(svdev, &svdev->vdev.virtqueue[q_idx]);
	}
}

static void
add_vdev_cb(void *arg)
{
	struct spdk_vhost_scsi_dev *svdev = arg;
	struct spdk_vhost_dev *vdev = &svdev->vdev;
	uint32_t i;

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; i++) {
		if (svdev->scsi_dev[i] == NULL) {
			continue;
		}
		spdk_scsi_dev_allocate_io_channels(svdev->scsi_dev[i]);
	}
	SPDK_NOTICELOG("Started poller for vhost controller %s on lcore %d\n", vdev->name, vdev->lcore);

	spdk_vhost_dev_mem_register(vdev);

	spdk_poller_register(&svdev->requestq_poller, vdev_worker, svdev, vdev->lcore, 0);
	spdk_poller_register(&svdev->mgmt_poller, vdev_mgmt_worker, svdev, vdev->lcore,
			     MGMT_POLL_PERIOD_US);
}

static void
remove_vdev_cb(void *arg)
{
	struct spdk_vhost_scsi_dev *svdev = arg;
	uint32_t i;

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; i++) {
		if (svdev->scsi_dev[i] == NULL) {
			continue;
		}
		spdk_scsi_dev_free_io_channels(svdev->scsi_dev[i]);
	}

	SPDK_NOTICELOG("Stopping poller for vhost controller %s\n", svdev->vdev.name);
	spdk_vhost_dev_mem_unregister(&svdev->vdev);
}

static struct spdk_vhost_scsi_dev *
to_scsi_dev(struct spdk_vhost_dev *ctrlr)
{
	if (ctrlr == NULL) {
		return NULL;
	}

	if (ctrlr->type != SPDK_VHOST_DEV_T_SCSI) {
		SPDK_ERRLOG("Controller %s: expected SCSI controller (%d) but got %d\n",
			    ctrlr->name, SPDK_VHOST_DEV_T_SCSI, ctrlr->type);
		return NULL;
	}

	return (struct spdk_vhost_scsi_dev *)ctrlr;
}

struct spdk_vhost_dev *
	spdk_vhost_scsi_dev_construct(const char *name, uint64_t cpumask)
{
	struct spdk_vhost_scsi_dev *svdev = spdk_dma_zmalloc(sizeof(struct spdk_vhost_scsi_dev),
					    SPDK_CACHE_LINE_SIZE, NULL);
	int rc;

	if (svdev == NULL) {
		return NULL;
	}

	rc = spdk_vhost_dev_construct(&svdev->vdev, name, cpumask, SPDK_VHOST_DEV_T_SCSI,
				      &spdk_vhost_scsi_device_backend);

	if (rc) {
		spdk_dma_free(svdev);
		return NULL;
	}

	return &svdev->vdev;
}

int
spdk_vhost_scsi_dev_remove(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_scsi_dev *svdev = to_scsi_dev(vdev);
	int i;

	if (svdev == NULL) {
		return -EINVAL;
	}

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; ++i) {
		if (svdev->scsi_dev[i]) {
			SPDK_ERRLOG("Trying to remove non-empty controller: %s.\n", vdev->name);
			return -EBUSY;
		}
	}

	if (spdk_vhost_dev_remove(vdev) != 0) {
		return -EIO;
	}

	spdk_dma_free(svdev);
	return 0;
}

struct spdk_scsi_dev *
spdk_vhost_scsi_dev_get_dev(struct spdk_vhost_dev *vdev, uint8_t num)
{
	struct spdk_vhost_scsi_dev *svdev;

	assert(num < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS);
	svdev = to_scsi_dev(vdev);

	return svdev ? svdev->scsi_dev[num] : NULL;
}

static void
spdk_vhost_scsi_lun_hotremove(const struct spdk_scsi_lun *lun, void *arg)
{
	struct spdk_vhost_scsi_dev *svdev = arg;
	const struct spdk_scsi_dev *scsi_dev;
	unsigned scsi_dev_num;

	assert(lun != NULL);
	assert(svdev != NULL);
	if ((svdev->vdev.negotiated_features & (1ULL << VIRTIO_SCSI_F_HOTPLUG)) == 0) {
		SPDK_WARNLOG("Controller %s: hotremove is not supported\n", svdev->vdev.name);
		return;
	}

	scsi_dev = spdk_scsi_lun_get_dev(lun);
	for (scsi_dev_num = 0; scsi_dev_num < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; scsi_dev_num++) {
		if (svdev->scsi_dev[scsi_dev_num] == scsi_dev) {
			break;
		}
	}

	if (scsi_dev_num == SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
		SPDK_ERRLOG("Dev %s is not a part of vhost scsi controller '%s'.\n",
			    spdk_scsi_dev_get_name(scsi_dev),
			    svdev->vdev.name);
		return;
	}

	/* remove entire device */
	spdk_vhost_scsi_dev_remove_dev(&svdev->vdev, scsi_dev_num, NULL, NULL);
}

int
spdk_vhost_scsi_dev_add_dev(struct spdk_vhost_dev *vdev, unsigned scsi_dev_num,
			    const char *lun_name)
{
	struct spdk_vhost_scsi_dev *svdev;
	char dev_name[SPDK_SCSI_DEV_MAX_NAME];
	int lun_id_list[1];
	char *lun_names_list[1];

	svdev = to_scsi_dev(vdev);
	if (svdev == NULL) {
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

	svdev = to_scsi_dev(vdev);
	if (svdev == NULL) {
		return -EINVAL;
	}

	if (vdev->lcore != -1) {
		SPDK_ERRLOG("Controller %s is in use and hotplug is not supported\n", vdev->name);
		return -ENODEV;
	}

	if (svdev->scsi_dev[scsi_dev_num] != NULL) {
		SPDK_ERRLOG("Controller %s dev %u already occupied\n", vdev->name, scsi_dev_num);
		return -EEXIST;
	}

	/*
	 * At this stage only one LUN per device
	 */
	snprintf(dev_name, sizeof(dev_name), "Dev %u", scsi_dev_num);
	lun_id_list[0] = 0;
	lun_names_list[0] = (char *)lun_name;

	svdev->scsi_dev_state[scsi_dev_num].removed = false;
	svdev->scsi_dev[scsi_dev_num] = spdk_scsi_dev_construct(dev_name, lun_names_list, lun_id_list, 1,
					SPDK_SPC_PROTOCOL_IDENTIFIER_SAS, spdk_vhost_scsi_lun_hotremove, svdev);

	if (svdev->scsi_dev[scsi_dev_num] == NULL) {
		SPDK_ERRLOG("Couldn't create spdk SCSI device '%s' using lun device '%s' in controller: %s\n",
			    dev_name, lun_name, vdev->name);
		return -EINVAL;
	}
	spdk_scsi_dev_add_port(svdev->scsi_dev[scsi_dev_num], 0, "vhost");
	SPDK_NOTICELOG("Controller %s: defined device '%s' using lun '%s'\n",
		       vdev->name, dev_name, lun_name);
	return 0;
}

int
spdk_vhost_scsi_dev_remove_dev(struct spdk_vhost_dev *vdev, unsigned scsi_dev_num,
			       void (*cb_fn)(void *, void *), void *cb_arg)
{
	struct spdk_vhost_scsi_dev *svdev;
	struct spdk_scsi_dev *scsi_dev;
	struct spdk_scsi_dev_vhost_state *scsi_dev_state;

	if (scsi_dev_num >= SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
		SPDK_ERRLOG("%s: invalid device number %d\n", vdev->name, scsi_dev_num);
		return -EINVAL;
	}

	svdev = to_scsi_dev(vdev);
	if (svdev == NULL) {
		return -ENODEV;
	}

	scsi_dev = svdev->scsi_dev[scsi_dev_num];
	if (scsi_dev == NULL) {
		SPDK_ERRLOG("Controller %s dev %u is not occupied\n", vdev->name, scsi_dev_num);
		return -ENODEV;
	}

	if (svdev->vdev.lcore == -1) {
		/* controller is not in use, remove dev and exit */
		spdk_scsi_dev_destruct(scsi_dev);
		svdev->scsi_dev[scsi_dev_num] = NULL;
		cb_fn(vdev, cb_arg);
		SPDK_NOTICELOG("%s: removed device 'Dev %u'\n", vdev->name, scsi_dev_num);
		return 0;
	}

	if ((svdev->vdev.negotiated_features & (1ULL << VIRTIO_SCSI_F_HOTPLUG)) == 0) {
		SPDK_WARNLOG("Controller %s: hotremove is not supported\n", svdev->vdev.name);
		return -ENOTSUP;
	}

	scsi_dev_state = &svdev->scsi_dev_state[scsi_dev_num];
	if (scsi_dev_state->removed) {
		SPDK_WARNLOG("%s: 'Dev %u' has been already marked to hotremove.\n", svdev->vdev.name,
			     scsi_dev_num);
		return -EBUSY;
	}

	scsi_dev_state->remove_cb = cb_fn;
	scsi_dev_state->remove_ctx = cb_arg;
	scsi_dev_state->removed = true;
	eventq_enqueue(svdev, scsi_dev_num, VIRTIO_SCSI_T_TRANSPORT_RESET, VIRTIO_SCSI_EVT_RESET_REMOVED);

	SPDK_NOTICELOG("%s: 'Dev %u' marked for hotremove.\n", vdev->name, scsi_dev_num);
	return 1;
}

int
spdk_vhost_scsi_controller_construct(void)
{
	struct spdk_conf_section *sp = spdk_conf_first_section(NULL);
	struct spdk_vhost_dev *vdev;
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

		vdev = spdk_vhost_scsi_dev_construct(name, cpumask);
		if (vdev == NULL) {
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

			if (spdk_vhost_scsi_dev_add_dev(vdev, dev_num, lun_name) < 0) {
				return -1;
			}
		}

		sp = spdk_conf_next_section(sp);

	}

	return 0;
}

static void
free_task_pool(struct spdk_vhost_scsi_dev *svdev)
{
	struct spdk_vhost_task *task;

	if (!svdev->task_pool) {
		return;
	}

	while (spdk_ring_dequeue(svdev->task_pool, (void **)&task, 1) == 1) {
		spdk_dma_free(task);
	}

	spdk_ring_free(svdev->task_pool);
	svdev->task_pool = NULL;
}

static int
alloc_task_pool(struct spdk_vhost_scsi_dev *svdev)
{
	struct spdk_vhost_scsi_task *task;
	uint32_t task_cnt = 0;
	uint32_t ring_size;
	uint16_t i;
	int rc;

	for (i = 0; i < svdev->vdev.num_queues; i++) {
		/*
		 * FIXME:
		 * this is too big because we need only size/2 from each queue but for now
		 * lets leave it as is to be sure we are not mistaken.
		 *
		 * Limit the pool size to 1024 * num_queues. This should be enough as QEMU have the
		 * same hard limit for queue size.
		 */
		task_cnt += spdk_min(svdev->vdev.virtqueue[i].size, 1024);
	}

	ring_size = spdk_align32pow2(task_cnt + 1);
	svdev->task_pool = spdk_ring_create(SPDK_RING_TYPE_SP_SC, ring_size,
					    spdk_env_get_socket_id(svdev->vdev.lcore));
	if (svdev->task_pool == NULL) {
		SPDK_ERRLOG("Controller %s: Failed to init vhost scsi task pool\n", svdev->vdev.name);
		return -1;
	}

	for (i = 0; i < task_cnt; ++i) {
		task = spdk_dma_zmalloc(sizeof(*task), SPDK_CACHE_LINE_SIZE, NULL);
		if (task == NULL) {
			SPDK_ERRLOG("Controller %s: Failed to allocate task\n", svdev->vdev.name);
			free_task_pool(svdev);
			return -1;
		}

		rc = spdk_ring_enqueue(svdev->task_pool, (void **)&task, 1);
		if (rc != 1) {
			SPDK_ERRLOG("Controller %s: Failed to alloc %"PRIu32" vhost scsi tasks\n", svdev->vdev.name,
				    task_cnt);
			free_task_pool(svdev);
			return -1;
		}
	}

	return 0;
}

/*
 * A new device is added to a data core. First the device is added to the main linked list
 * and then allocated to a specific data core.
 */
static int
new_device(int vid)
{
	struct spdk_vhost_scsi_dev *svdev = NULL;

	svdev = to_scsi_dev(spdk_vhost_dev_load(vid));
	if (svdev == NULL) {
		return -1;
	}

	if (alloc_task_pool(svdev)) {
		spdk_vhost_dev_unload(&svdev->vdev);
		return -1;
	}

	spdk_vhost_timed_event_send(svdev->vdev.lcore, add_vdev_cb, svdev, 1, "add scsi vdev");
	return 0;
}

static void
destroy_device(int vid)
{
	struct spdk_vhost_scsi_dev *svdev;
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_timed_event event = {0};
	uint32_t i;

	vdev = spdk_vhost_dev_find_by_vid(vid);
	if (vdev == NULL) {
		rte_panic("Couldn't find device with vid %d to stop.\n", vid);
	}
	svdev = to_scsi_dev(vdev);
	assert(svdev);

	spdk_vhost_timed_event_init(&event, vdev->lcore, NULL, NULL, 1);
	spdk_poller_unregister(&svdev->requestq_poller, event.spdk_event);
	spdk_vhost_timed_event_wait(&event, "unregister request queue poller");

	spdk_vhost_timed_event_init(&event, vdev->lcore, NULL, NULL, 1);
	spdk_poller_unregister(&svdev->mgmt_poller, event.spdk_event);
	spdk_vhost_timed_event_wait(&event, "unregister management poller");

	/* Wait for all tasks to finish */
	for (i = 1000; i && vdev->task_cnt > 0; i--) {
		usleep(1000);
	}

	if (vdev->task_cnt > 0) {
		SPDK_ERRLOG("%s: pending tasks did not finish in 1s.\n", vdev->name);
	}

	spdk_vhost_timed_event_send(vdev->lcore, remove_vdev_cb, svdev, 1, "remove scsi vdev");

	free_task_pool(svdev);
	spdk_vhost_dev_unload(vdev);
}

int
spdk_vhost_init(void)
{
	return 0;
}

int
spdk_vhost_fini(void)
{
	return 0;
}

SPDK_LOG_REGISTER_TRACE_FLAG("vhost_scsi", SPDK_TRACE_VHOST_SCSI)
SPDK_LOG_REGISTER_TRACE_FLAG("vhost_scsi_queue", SPDK_TRACE_VHOST_SCSI_QUEUE)
SPDK_LOG_REGISTER_TRACE_FLAG("vhost_scsi_data", SPDK_TRACE_VHOST_SCSI_DATA)
