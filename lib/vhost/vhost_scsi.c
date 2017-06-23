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

#undef container_of
#define container_of(ptr, type, member) ({ \
		typeof(((type *)0)->member) *__mptr = (ptr); \
		(type *)((char *)__mptr - offsetof(type, member)); })

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
 * - LUN params change
 * - T10 PI
 */
#define SPDK_VHOST_SCSI_DISABLED_FEATURES	((1ULL << VHOST_F_LOG_ALL) | \
						(1ULL << VIRTIO_SCSI_F_T10_PI ))

#define MGMT_POLL_PERIOD_US (1000 * 5)

#define VIRTIO_SCSI_CONTROLQ   0
#define VIRTIO_SCSI_EVENTQ   1
#define VIRTIO_SCSI_REQUESTQ   2

/* Allocated iovec buffer len */
#define VHOST_SCSI_IOVS_LEN		128

struct spdk_scsi_dev_w {
	/** Actual scsi device currently used */
	struct spdk_scsi_dev *scsi_dev;

	/**
	 * Scsi device that is being removed.
	 * We can't just directly delete it, since there might be
	 * some remaining tasks still using this device.
	 */
	struct spdk_scsi_dev *scsi_removed_dev;
	int task_cnt;
};

struct spdk_vhost_scsi_dev {
	struct spdk_vhost_dev vdev;

	struct spdk_scsi_dev_w dev[SPDK_VHOST_SCSI_CTRLR_MAX_DEVS];
	struct spdk_poller *requestq_poller;
	struct spdk_poller *mgmt_poller;
	struct spdk_poller *shutdown_poller;

	struct spdk_ring *eventq_ring;
} __rte_cache_aligned;

struct spdk_vhost_task {
	struct spdk_scsi_task	scsi;
	struct iovec iovs[VHOST_SCSI_IOVS_LEN];

	union {
		struct virtio_scsi_cmd_resp *resp;
		struct virtio_scsi_ctrl_tmf_resp *tmf_resp;
	};

	struct spdk_vhost_scsi_dev *svdev;
	struct spdk_scsi_dev_w *dev;

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

static struct spdk_ring *g_task_pool;

static void spdk_vhost_scsi_lun_hotremove(struct spdk_scsi_lun *lun, void *arg);

static int
spdk_vhost_scsi_dev_w_init(struct spdk_vhost_scsi_dev *svdev, unsigned scsi_dev_num,
			   char *lun_names_list[], int *lun_id_list, int num_luns)
{
	struct spdk_scsi_dev_w *dev;
	struct spdk_scsi_lun *lun;
	char dev_name[SPDK_SCSI_DEV_MAX_NAME];

	dev = &svdev->dev[scsi_dev_num];
	if (dev->scsi_dev != NULL) {
		SPDK_ERRLOG("Controller %s dev %u already occupied\n", svdev->vdev.name, scsi_dev_num);
		return -1;
	}

	snprintf(dev_name, sizeof(dev_name), "Dev %u", scsi_dev_num);
	dev->scsi_dev = spdk_scsi_dev_construct(dev_name, lun_names_list, lun_id_list, 1,
						SPDK_SPC_PROTOCOL_IDENTIFIER_SAS);
	dev->task_cnt = 0;

	if (dev->scsi_dev == NULL) {
		SPDK_ERRLOG("Couldn't create spdk SCSI device '%s' using lun device '%s' in controller: %s\n",
			    dev_name, lun_names_list[0], svdev->vdev.name);
		return -1;
	}

	lun = spdk_scsi_dev_get_lun(dev->scsi_dev, lun_id_list[0]);
	spdk_scsi_lun_set_hotremove_cb(lun, spdk_vhost_scsi_lun_hotremove, svdev);

	spdk_scsi_dev_add_port(dev->scsi_dev, 0, "vhost");

	SPDK_NOTICELOG("Controller %s: defined device '%s' using lun '%s'\n",
		       svdev->vdev.name, dev_name, lun_names_list[0]);

	return 0;
}

static void
spdk_vhost_scsi_dev_w_remove_cb(void *arg1, void *arg2)
{
	struct spdk_vhost_scsi_dev *svdev = arg1;
	struct spdk_scsi_dev_w *dev = arg2;

	/* FIXME: ids */
	if (dev->scsi_dev == NULL) {
		SPDK_ERRLOG("Controller %s dev %u is not occupied\n", svdev->vdev.name, 42);
		return;
	}

	dev->scsi_removed_dev = dev->scsi_dev;
	dev->scsi_dev = NULL;

	SPDK_NOTICELOG("Controller %s: removed device 'Dev %u'\n",
		       svdev->vdev.name, 42);

}

static void eventq_enqueue(struct spdk_vhost_scsi_dev *svdev, struct spdk_scsi_dev *dev,
			   struct spdk_scsi_lun *lun, uint32_t event, uint32_t reason);

static int
spdk_vhost_scsi_dev_w_remove(struct spdk_vhost_scsi_dev *svdev, unsigned scsi_dev_num)
{
	struct spdk_scsi_dev_w *dev = &svdev->dev[scsi_dev_num];
	struct spdk_scsi_dev *scsi_dev = dev->scsi_dev;
	struct spdk_event *event;

	if (svdev->vdev.lcore == -1) {
		/* controller is not in use, remove dev and exit */
		spdk_vhost_scsi_dev_w_remove_cb(svdev, dev);
		return 0;
	}

	if ((svdev->vdev.negotiated_features & (1ULL << VIRTIO_SCSI_F_HOTPLUG)) == 0) {
		SPDK_WARNLOG("Controller %s: hotremove is not supported\n", svdev->vdev.name);
		return -1;
	}

	event = spdk_event_allocate(svdev->vdev.lcore, spdk_vhost_scsi_dev_w_remove_cb, svdev, dev);
	spdk_event_call(event);

	eventq_enqueue(svdev, scsi_dev, NULL, VIRTIO_SCSI_T_TRANSPORT_RESET, VIRTIO_SCSI_EVT_RESET_REMOVED);

	return 0;
}

static void
spdk_vhost_task_put(struct spdk_vhost_task *task)
{
	spdk_scsi_task_put(&task->scsi);
}

static void
spdk_vhost_task_free_cb(struct spdk_scsi_task *scsi_task)
{
	struct spdk_vhost_task *task = container_of(scsi_task, struct spdk_vhost_task, scsi);

	if (spdk_likely(task->dev)) {
		--task->dev->task_cnt;
	}
	spdk_ring_enqueue(g_task_pool, (void **) &task, 1);
}

static void
spdk_vhost_get_tasks(struct spdk_vhost_scsi_dev *svdev, struct spdk_vhost_task **tasks,
		     size_t count)
{
	size_t res_count;

	res_count = spdk_ring_dequeue(g_task_pool, (void **)tasks, count);
	if (res_count != count) {
		SPDK_ERRLOG("%s: couldn't get %zu tasks from task_pool\n", svdev->vdev.name, count);
		/* FIXME: we should never run out of tasks, but what if we do? */
		abort();
	}
}

static void
process_eventq(struct spdk_vhost_scsi_dev *svdev)
{
	struct rte_vhost_vring *vq;
	struct vring_desc *desc;
	struct virtio_scsi_event *ev, *desc_ev;
	uint32_t req_size;
	uint16_t req;

	vq = &svdev->vdev.virtqueue[VIRTIO_SCSI_EVENTQ];

	while (spdk_ring_dequeue(svdev->eventq_ring, (void **)&ev, 1) == 1) {
		if (spdk_vhost_vq_avail_ring_get(vq, &req, 1) != 1) {
			SPDK_ERRLOG("Controller %s: Failed to send virtio event (no avail ring entries?).\n",
				    svdev->vdev.name);
			spdk_dma_free(ev);
			break;
		}

		desc =  spdk_vhost_vq_get_desc(vq, req);
		desc_ev = spdk_vhost_gpa_to_vva(&svdev->vdev, desc->addr);

		if (desc->len >= sizeof(*desc_ev) && desc_ev != NULL) {
			req_size = sizeof(*desc_ev);
			memcpy(desc_ev, ev, sizeof(*desc_ev));
		} else {
			SPDK_ERRLOG("Controller %s: Invalid eventq descriptor.\n", svdev->vdev.name);
			req_size = 0;
		}

		spdk_vhost_vq_used_ring_enqueue(&svdev->vdev, vq, req, req_size);
		spdk_dma_free(ev);
	}
}

static void
process_dev_status(struct spdk_vhost_scsi_dev *svdev)
{
	struct spdk_scsi_dev_w *dev;
	int i;

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; ++i) {
		dev = &svdev->dev[i];

		if (dev->scsi_removed_dev && dev->task_cnt == 0) {
			spdk_scsi_dev_destruct(dev->scsi_removed_dev);
			dev->scsi_removed_dev = NULL;
		}
	}
}

static void
eventq_enqueue(struct spdk_vhost_scsi_dev *svdev, struct spdk_scsi_dev *dev,
	       struct spdk_scsi_lun *lun, uint32_t event, uint32_t reason)
{
	struct virtio_scsi_event *ev;
	int dev_id;
	int lun_id;

	if (dev == NULL) {
		SPDK_ERRLOG("%s: eventq device cannot be NULL.\n", svdev->vdev.name);
		return;
	}

	for (dev_id = 0; dev_id < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; dev_id++) {
		if (svdev->dev[dev_id].scsi_dev == dev) {
			break;
		}
	}

	if (dev_id == SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
		SPDK_ERRLOG("Dev %s is not a part of vhost scsi controller '%s'.\n", spdk_scsi_dev_get_name(dev),
			    svdev->vdev.name);
		return;
	}

	/* some events may apply to the entire target via ids set to 0 */
	lun_id = (lun == NULL ? 0 : spdk_scsi_lun_get_id(lun));

	ev = spdk_dma_zmalloc(sizeof(*ev), __alignof__(uint32_t), NULL);
	assert(ev);

	ev->event = event;
	ev->lun[0] = 1;
	ev->lun[1] = dev_id;
	ev->lun[2] = lun_id >> 8; /* relies on linux kernel implementation */
	ev->lun[3] = lun_id & 0xFF;
	ev->reason = reason;

	if (spdk_ring_enqueue(svdev->eventq_ring, (void **)&ev, 1) != 1) {
		SPDK_ERRLOG("Controller %s: Failed to inform guest about LUN #%d removal (no room in ring?).\n",
			    svdev->vdev.name, lun_id);
		spdk_dma_free(ev);
		return;
	}
}

static void
submit_completion(struct spdk_vhost_task *task)
{
	spdk_vhost_vq_used_ring_enqueue(&task->svdev->vdev, task->vq, task->req_idx,
					task->scsi.data_transferred);
	SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI, "Finished task (%p) req_idx=%d\n", task, task->req_idx);

	spdk_vhost_task_put(task);
}

static void
spdk_vhost_task_mgmt_cpl(struct spdk_scsi_task *scsi_task)
{
	struct spdk_vhost_task *task = container_of(scsi_task, struct spdk_vhost_task, scsi);

	submit_completion(task);
}

static void
spdk_vhost_task_cpl(struct spdk_scsi_task *scsi_task)
{
	struct spdk_vhost_task *task = container_of(scsi_task, struct spdk_vhost_task, scsi);

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
	   will be invoked when the SCSI command is completed.  See spdk_vhost_task_cpl()
	   for what SPDK vhost-scsi does when the task is completed.
	 */

	task->resp->response = VIRTIO_SCSI_S_OK;
	spdk_scsi_dev_queue_task(task->dev->scsi_dev, &task->scsi);
}

static void
mgmt_task_submit(struct spdk_vhost_task *task, enum spdk_scsi_task_func func)
{
	task->tmf_resp->response = VIRTIO_SCSI_S_OK;
	spdk_scsi_dev_queue_mgmt_task(task->dev->scsi_dev, &task->scsi, func);
}

static void
invalid_request(struct spdk_vhost_task *task)
{
	/*
	 *  Flush eventq so that guest is instantly
	 *  notified about any hotremoved luns.
	 *  This might prevent him from sending more
	 *  invalid requests and trying to reset
	 *  the device.
	 */
	process_eventq(task->svdev);
	spdk_vhost_vq_used_ring_enqueue(&task->svdev->vdev, task->vq, task->req_idx, 0);
	spdk_vhost_task_put(task);

	SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI, "Invalid request (status=%" PRIu8")\n",
		      task->resp ? task->resp->response : -1);
}

static int
parse_virtio_lun(struct spdk_vhost_task *task, const __u8 *lun)
{
	struct spdk_scsi_dev_w *dev;
	uint16_t lun_id = (((uint16_t)lun[2] << 8) | lun[3]) & 0x3FFF;

	SPDK_TRACEDUMP(SPDK_TRACE_VHOST_SCSI_QUEUE, "LUN", lun, 8);

	/* First byte must be 1 and second is target */
	if (lun[0] != 1 || lun[1] >= SPDK_VHOST_SCSI_CTRLR_MAX_DEVS)
		return -1;

	dev = &task->svdev->dev[lun[1]];
	if (dev->scsi_dev == NULL) {
		return -1;
	}

	task->dev = dev;
	++dev->task_cnt;

	task->scsi.lun = spdk_scsi_dev_get_lun(dev->scsi_dev, lun_id);
	return 0;
}

static void
process_ctrl_request(struct spdk_vhost_task *task)
{
	struct vring_desc *desc;
	struct virtio_scsi_ctrl_tmf_req *ctrl_req;
	struct virtio_scsi_ctrl_an_resp *an_resp;
	int lun_rc;

	spdk_scsi_task_construct(&task->scsi, spdk_vhost_task_mgmt_cpl, spdk_vhost_task_free_cb, NULL);
	desc = spdk_vhost_vq_get_desc(task->vq, task->req_idx);
	ctrl_req = spdk_vhost_gpa_to_vva(&task->svdev->vdev, desc->addr);

	SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI_QUEUE,
		      "Processing controlq descriptor: desc %d/%p, desc_addr %p, len %d, flags %d, last_used_idx %d; kickfd %d; size %d\n",
		      task->req_idx, desc, (void *)desc->addr, desc->len, desc->flags, task->vq->last_used_idx,
		      task->vq->kickfd, task->vq->size);
	SPDK_TRACEDUMP(SPDK_TRACE_VHOST_SCSI_QUEUE, "Request desriptor", (uint8_t *)ctrl_req,
		       desc->len);

	lun_rc = parse_virtio_lun(task, ctrl_req->lun);

	/* Process the TMF request */
	switch (ctrl_req->type) {
	case VIRTIO_SCSI_T_TMF:
		/* Get the response buffer */
		assert(spdk_vhost_vring_desc_has_next(desc));
		desc = spdk_vhost_vring_desc_get_next(task->vq->desc, desc);
		task->tmf_resp = spdk_vhost_gpa_to_vva(&task->svdev->vdev, desc->addr);

		/* Check if we are processing a valid request */
		if (lun_rc != 0) {
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
	struct spdk_vhost_dev *vdev = &task->svdev->vdev;
	struct vring_desc *desc =  spdk_vhost_vq_get_desc(task->vq, task->req_idx);
	struct iovec *iovs = task->iovs;
	uint16_t iovcnt = 0, iovcnt_max = VHOST_SCSI_IOVS_LEN;
	uint32_t len = 0;

	/* Sanity check. First descriptor must be readable and must have next one. */
	if (spdk_unlikely(spdk_vhost_vring_desc_is_wr(desc) || !spdk_vhost_vring_desc_has_next(desc))) {
		SPDK_WARNLOG("Invalid first (request) descriptor.\n");
		task->resp = NULL;
		goto abort_task;
	}

	spdk_scsi_task_construct(&task->scsi, spdk_vhost_task_cpl, spdk_vhost_task_free_cb, NULL);
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
process_request(struct spdk_vhost_task *task)
{
	struct virtio_scsi_cmd_req *req;
	int result;

	result = task_data_setup(task, &req);
	if (result) {
		return result;
	}

	result = parse_virtio_lun(task, req->lun);
	if (spdk_unlikely(result != 0)) {
		task->resp->response = VIRTIO_SCSI_S_BAD_TARGET;
		return -1;
	}

	task->scsi.cdb = req->cdb;
	task->scsi.target_port = spdk_scsi_dev_find_port_by_id(task->dev->scsi_dev, 0);
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
	struct spdk_vhost_task *tasks[32];
	struct spdk_vhost_task *task;
	uint16_t reqs[32];
	uint16_t reqs_cnt, i;

	reqs_cnt = spdk_vhost_vq_avail_ring_get(vq, reqs, SPDK_COUNTOF(reqs));

	spdk_vhost_get_tasks(svdev, tasks, reqs_cnt);

	for (i = 0; i < reqs_cnt; i++) {
		task = tasks[i];

		task->vq = vq;
		task->svdev = svdev;
		task->req_idx = reqs[i];

		process_ctrl_request(task);
	}
}

static void
process_requestq(struct spdk_vhost_scsi_dev *svdev, struct rte_vhost_vring *vq)
{
	struct spdk_vhost_task *tasks[32];
	struct spdk_vhost_task *task;
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

		task->vq = vq;
		task->svdev = svdev;
		task->req_idx = reqs[i];
		result = process_request(task);
		if (likely(result == 0)) {
			task_submit(task);
			SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI, "====== Task %p req_idx %d submitted ======\n", task,
				      task->req_idx);
		} else if (result > 0) {
			spdk_vhost_task_cpl(&task->scsi);
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

	process_dev_status(svdev);
	process_eventq(svdev);
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
		if (svdev->dev[i].scsi_dev == NULL) {
			continue;
		}
		spdk_scsi_dev_allocate_io_channels(svdev->dev[i].scsi_dev);
	}
	SPDK_NOTICELOG("Started poller for vhost controller %s on lcore %d\n", vdev->name, vdev->lcore);

	svdev->eventq_ring = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 16,
					      spdk_env_get_socket_id(svdev->vdev.lcore));
	assert(svdev->eventq_ring != NULL);

	spdk_vhost_dev_mem_register(vdev);

	spdk_poller_register(&svdev->requestq_poller, vdev_worker, svdev, vdev->lcore, 0);
	spdk_poller_register(&svdev->mgmt_poller, vdev_mgmt_worker, svdev, vdev->lcore,
			     MGMT_POLL_PERIOD_US);
}

static void
delete_vdev(struct spdk_vhost_scsi_dev *svdev)
{
	void *ev;

	spdk_vhost_dev_mem_unregister(&svdev->vdev);

	/* Cleanup not sent events */
	while (spdk_ring_dequeue(svdev->eventq_ring, &ev, 1) == 1) {
		spdk_dma_free(ev);
	}

	spdk_ring_free(svdev->eventq_ring);
	svdev->eventq_ring = NULL;

	spdk_vhost_dev_unload(&svdev->vdev);
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

int
spdk_vhost_scsi_dev_construct(const char *name, uint64_t cpumask)
{
	struct spdk_vhost_scsi_dev *svdev = spdk_dma_zmalloc(sizeof(struct spdk_vhost_scsi_dev),
					    SPDK_CACHE_LINE_SIZE, NULL);
	int ret;

	if (svdev == NULL) {
		return -ENOMEM;
	}

	ret = spdk_vhost_dev_construct(&svdev->vdev, name, cpumask, SPDK_VHOST_DEV_T_SCSI,
				       &spdk_vhost_scsi_device_backend);

	if (ret) {
		spdk_dma_free(svdev);
	}

	return ret;
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
		if (svdev->dev[i].scsi_dev) {
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

	return svdev ? svdev->dev[num].scsi_dev : NULL;
}

static void
spdk_vhost_scsi_lun_hotremove(struct spdk_scsi_lun *lun, void *arg)
{
	struct spdk_vhost_scsi_dev *svdev = arg;

	assert(lun != NULL);
	assert(svdev != NULL);
	if ((svdev->vdev.negotiated_features & (1ULL << VIRTIO_SCSI_F_HOTPLUG)) == 0) {
		SPDK_WARNLOG("Controller %s: hotremove is not supported\n", svdev->vdev.name);
		return;
	}

	eventq_enqueue(svdev, spdk_scsi_lun_get_dev(lun), lun, VIRTIO_SCSI_T_TRANSPORT_RESET,
		       VIRTIO_SCSI_EVT_RESET_REMOVED);
}

int
spdk_vhost_scsi_dev_add_dev(const char *ctrlr_name, unsigned scsi_dev_num, const char *lun_name)
{
	struct spdk_vhost_scsi_dev *svdev;
	struct spdk_vhost_dev *vdev;
	char dev_name[SPDK_SCSI_DEV_MAX_NAME];
	int lun_id_list[1];
	char *lun_names_list[1];
	int rc;

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

	vdev = spdk_vhost_dev_find(ctrlr_name);
	if (vdev == NULL) {
		SPDK_ERRLOG("Controller %s is not defined.\n", ctrlr_name);
		return -ENODEV;
	}

	svdev = to_scsi_dev(vdev);
	if (svdev == NULL) {
		return -EINVAL;
	}

	if (vdev->lcore != -1) {
		SPDK_ERRLOG("Controller %s is in use and hotplug is not supported\n", ctrlr_name);
		return -ENODEV;
	}

	if (svdev->dev[scsi_dev_num].scsi_dev != NULL) {
		SPDK_ERRLOG("Controller %s dev %u already occupied\n", ctrlr_name, scsi_dev_num);
		return -EEXIST;
	}

	/*
	 * At this stage only one LUN per device
	 */
	snprintf(dev_name, sizeof(dev_name), "Dev %u", scsi_dev_num);
	lun_id_list[0] = 0;
	lun_names_list[0] = (char *)lun_name;

	rc = spdk_vhost_scsi_dev_w_init(svdev, scsi_dev_num, lun_names_list, lun_id_list, 1);
	return rc;
}

int
spdk_vhost_scsi_dev_remove_dev(struct spdk_vhost_dev *vdev, unsigned scsi_dev_num)
{
	struct spdk_vhost_scsi_dev *svdev;
	int rc;

	assert(vdev != NULL);
	svdev = to_scsi_dev(vdev);
	if (svdev == NULL) {
		return -ENODEV;
	}

	rc = spdk_vhost_scsi_dev_w_remove(svdev, scsi_dev_num);
	return rc;
}

int
spdk_vhost_scsi_controller_construct(void)
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

		if (spdk_vhost_scsi_dev_construct(name, cpumask) < 0) {
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

			if (spdk_vhost_scsi_dev_add_dev(name, dev_num, lun_name) < 0) {
				return -1;
			}
		}

		sp = spdk_conf_next_section(sp);

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

	/* FIXME: sync with shutdown poller */
	svdev = to_scsi_dev(spdk_vhost_dev_load(vid));
	if (svdev == NULL) {
		return -1;
	}

	spdk_vhost_timed_event_send(svdev->vdev.lcore, add_vdev_cb, svdev, 1, "add scsi vdev");
	return 0;
}

static void
shutdown_worker(void *arg)
{
	struct spdk_vhost_scsi_dev *svdev = arg;
	struct spdk_scsi_dev_w *dev;
	struct spdk_scsi_dev *scsi_dev;
	int i;
	bool done = false;

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; ++i) {
		dev = &svdev->dev[i];
		scsi_dev = (dev->scsi_dev != NULL ? dev->scsi_dev : dev->scsi_removed_dev);
		if (scsi_dev) {
			done = false;

			if (dev->task_cnt != 0) {
				continue;
			}

			/* FIXME: devices that were hot-removed have leaked io channels */
			spdk_scsi_dev_free_io_channels(scsi_dev);
			spdk_scsi_dev_destruct(scsi_dev);
			dev->scsi_dev = dev->scsi_removed_dev = NULL;
		}
	}

	if (done) {
		delete_vdev(svdev);
		spdk_poller_unregister(&svdev->shutdown_poller, NULL);
	}
}

static void
destroy_device(int vid)
{
	struct spdk_vhost_scsi_dev *svdev;
	struct spdk_vhost_dev *vdev;
	struct spdk_vhost_timed_event event = {0};

	vdev = spdk_vhost_dev_find_by_vid(vid);
	if (vdev == NULL) {
		rte_panic("Couldn't find device with vid %d to stop.\n", vid);
	}
	svdev = to_scsi_dev(vdev);
	assert(svdev);

	SPDK_NOTICELOG("Stopping poller for vhost controller %s\n", svdev->vdev.name);

	spdk_vhost_timed_event_init(&event, vdev->lcore, NULL, NULL, 1);
	spdk_poller_unregister(&svdev->mgmt_poller, event.spdk_event);
	spdk_vhost_timed_event_wait(&event, "unregister management poller");

	spdk_vhost_timed_event_init(&event, vdev->lcore, NULL, NULL, 1);
	spdk_poller_unregister(&svdev->requestq_poller, event.spdk_event);
	spdk_vhost_timed_event_wait(&event, "unregister request queue poller");

	spdk_poller_register(&svdev->shutdown_poller, shutdown_worker, svdev, vdev->lcore, 0);
}

int
spdk_vhost_init(void)
{
	struct spdk_vhost_dev *task;
	int rc, i;

	g_task_pool = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 16384, SOCKET_ID_ANY);
	if (g_task_pool == NULL) {
		SPDK_ERRLOG("Failed to init vhost scsi task pool\n");
		return -1;
	}

	for (i = 0; i < 16384 - 1; ++i) {
		task = spdk_dma_zmalloc(sizeof(*task), SPDK_CACHE_LINE_SIZE, NULL);
		rc = spdk_ring_enqueue(g_task_pool, (void **)&task, 1);
		if (rc != 1) {
			SPDK_ERRLOG("Failed to alloc vhost scsi tasks\n");
			return -1;
		}
	}

	return 0;
}

int
spdk_vhost_fini(void)
{
	struct spdk_vhost_dev *task;

	while (spdk_ring_dequeue(g_task_pool, (void **)&task, 1) == 1) {
		spdk_dma_free(task);
	}

	spdk_ring_free(g_task_pool);

	return 0;
}

SPDK_LOG_REGISTER_TRACE_FLAG("vhost_scsi", SPDK_TRACE_VHOST_SCSI)
SPDK_LOG_REGISTER_TRACE_FLAG("vhost_scsi_queue", SPDK_TRACE_VHOST_SCSI_QUEUE)
SPDK_LOG_REGISTER_TRACE_FLAG("vhost_scsi_data", SPDK_TRACE_VHOST_SCSI_DATA)
