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

#include "spdk/env.h"
#include "spdk/scsi.h"
#include "spdk/conf.h"
#include "spdk/event.h"

#include "spdk/vhost.h"
#include "vhost_internal.h"
#include "vhost_scsi.h"
#include "task.h"

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

#define CONTROLQ_POLL_PERIOD_US (1000 * 5)

#define VIRTIO_SCSI_CONTROLQ   0
#define VIRTIO_SCSI_EVENTQ   1
#define VIRTIO_SCSI_REQUESTQ   2

struct spdk_vhost_scsi_dev {
	struct spdk_vhost_dev vdev;

	struct spdk_scsi_dev *scsi_dev[SPDK_VHOST_SCSI_CTRLR_MAX_DEVS];
	struct spdk_poller *requestq_poller;
	struct spdk_poller *controlq_poller;
} __rte_cache_aligned;

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

static void task_submit(struct spdk_vhost_task *task);
static int process_request(struct spdk_vhost_task *task);
static void invalid_request(struct spdk_vhost_task *task);

static void
submit_completion(struct spdk_vhost_task *task)
{
	struct iovec *iovs = NULL;
	int result;

	spdk_vhost_vq_used_ring_enqueue(&task->svdev->vdev, task->vq, task->req_idx,
					task->scsi.data_transferred);
	SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI, "Finished task (%p) req_idx=%d\n", task, task->req_idx);

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
mgmt_task_submit(struct spdk_vhost_task *task, enum spdk_scsi_task_func func)
{
	task->tmf_resp->response = VIRTIO_SCSI_S_OK;
	task->scsi.cb_event = spdk_event_allocate(rte_lcore_id(),
			      process_mgmt_task_completion,
			      task, NULL);
	spdk_scsi_dev_queue_mgmt_task(task->scsi_dev, &task->scsi, func);
}

static void
invalid_request(struct spdk_vhost_task *task)
{
	spdk_vhost_vq_used_ring_enqueue(&task->svdev->vdev, task->vq, task->req_idx, 0);
	spdk_vhost_task_put(task);

	SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI, "Invalid request (status=%" PRIu8")\n",
		      task->resp ? task->resp->response : -1);
}

static struct spdk_scsi_dev *
get_scsi_dev(struct spdk_vhost_scsi_dev *svdev, const __u8 *lun)
{
	SPDK_TRACEDUMP(SPDK_TRACE_VHOST_SCSI_DATA, "LUN", lun, 8);
	/* First byte must be 1 and second is target */
	if (lun[0] != 1 || lun[1] >= SPDK_VHOST_SCSI_CTRLR_MAX_DEVS)
		return NULL;

	return svdev->scsi_dev[lun[1]];
}

static struct spdk_scsi_lun *
get_scsi_lun(struct spdk_scsi_dev *scsi_dev, const __u8 *lun)
{
	uint16_t lun_id = (((uint16_t)lun[2] << 8) | lun[3]) & 0x3FFF;

	/* For now only one LUN per controller is allowed so no need to search LUN IDs */
	return spdk_scsi_dev_get_lun(scsi_dev, lun_id);
}

static void
process_ctrl_request(struct spdk_vhost_scsi_dev *svdev, struct rte_vhost_vring *controlq,
		     uint16_t req_idx)
{
	struct spdk_vhost_task *task;

	struct vring_desc *desc;
	struct virtio_scsi_ctrl_tmf_req *ctrl_req;
	struct virtio_scsi_ctrl_an_resp *an_resp;

	desc = spdk_vhost_vq_get_desc(controlq, req_idx);
	ctrl_req = spdk_vhost_gpa_to_vva(&svdev->vdev, desc->addr);

	SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI_DATA,
		      "Processing controlq descriptor: desc %d/%p, desc_addr %p, len %d, flags %d, last_used_idx %d; kickfd %d; size %d\n",
		      req_idx, desc, (void *)desc->addr, desc->len, desc->flags, controlq->last_used_idx,
		      controlq->kickfd, controlq->size);
	SPDK_TRACEDUMP(SPDK_TRACE_VHOST_SCSI_DATA, "Request desriptor", (uint8_t *)ctrl_req,
		       desc->len);

	task = spdk_vhost_task_get(svdev);
	task->vq = controlq;
	task->svdev = svdev;
	task->req_idx = req_idx;
	task->scsi_dev = get_scsi_dev(task->svdev, ctrl_req->lun);

	/* Process the TMF request */
	switch (ctrl_req->type) {
	case VIRTIO_SCSI_T_TMF:
		/* Get the response buffer */
		assert(spdk_vhost_vring_desc_has_next(desc));
		desc = spdk_vhost_vring_desc_get_next(controlq->desc, desc);
		task->tmf_resp = spdk_vhost_gpa_to_vva(&svdev->vdev, desc->addr);

		/* Check if we are processing a valid request */
		if (task->scsi_dev == NULL) {
			task->tmf_resp->response = VIRTIO_SCSI_S_BAD_TARGET;
			break;
		}

		switch (ctrl_req->subtype) {
		case VIRTIO_SCSI_T_TMF_LOGICAL_UNIT_RESET:
			/* Handle LUN reset */
			SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI_DATA, "LUN reset\n");
			task->scsi.lun = get_scsi_lun(task->scsi_dev, ctrl_req->lun);

			mgmt_task_submit(task, SPDK_SCSI_TASK_FUNC_LUN_RESET);
			return;
		default:
			task->tmf_resp->response = VIRTIO_SCSI_S_ABORTED;
			/* Unsupported command */
			SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI_DATA, "Unsupported TMF command %x\n", ctrl_req->subtype);
			break;
		}
		break;
	case VIRTIO_SCSI_T_AN_QUERY:
	case VIRTIO_SCSI_T_AN_SUBSCRIBE: {
		desc = spdk_vhost_vring_desc_get_next(controlq->desc, desc);
		an_resp = spdk_vhost_gpa_to_vva(&svdev->vdev, desc->addr);
		an_resp->response = VIRTIO_SCSI_S_ABORTED;
		break;
	}
	default:
		SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI_DATA, "Unsupported control command %x\n", ctrl_req->type);
		break;
	}

	spdk_vhost_vq_used_ring_enqueue(&svdev->vdev, controlq, req_idx, 0);
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
	struct iovec *iovs = task->scsi.iovs;
	uint16_t iovcnt = 0, iovcnt_max = task->scsi.iovcnt;
	uint32_t len = 0;

	assert(iovcnt_max == 1 || iovcnt_max == VHOST_SCSI_IOVS_LEN);

	/* Sanity check. First descriptor must be readable and must have next one. */
	if (unlikely(spdk_vhost_vring_desc_is_wr(desc) || !spdk_vhost_vring_desc_has_next(desc))) {
		SPDK_WARNLOG("Invalid first (request) descriptor.\n");
		task->resp = NULL;
		goto abort_task;
	}

	*req = spdk_vhost_gpa_to_vva(vdev, desc->addr);

	desc = spdk_vhost_vring_desc_get_next(vq->desc, desc);
	task->scsi.dxfer_dir = spdk_vhost_vring_desc_is_wr(desc) ? SPDK_SCSI_DIR_FROM_DEV :
			       SPDK_SCSI_DIR_TO_DEV;

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
		if (iovcnt_max != VHOST_SCSI_IOVS_LEN && spdk_vhost_vring_desc_has_next(desc)) {
			iovs = spdk_vhost_iovec_alloc();
			if (iovs == NULL) {
				return 1;
			}

			iovcnt_max = VHOST_SCSI_IOVS_LEN;
		}

		/* All remaining descriptors are data. */
		while (iovcnt < iovcnt_max) {
			iovs[iovcnt].iov_base = spdk_vhost_gpa_to_vva(vdev, desc->addr);
			iovs[iovcnt].iov_len = desc->len;
			len += desc->len;
			iovcnt++;

			if (!spdk_vhost_vring_desc_has_next(desc))
				break;

			desc = spdk_vhost_vring_desc_get_next(vq->desc, desc);
			if (unlikely(!spdk_vhost_vring_desc_is_wr(desc))) {
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

		if (iovcnt_max != VHOST_SCSI_IOVS_LEN && spdk_vhost_vring_desc_has_next(desc)) {
			/* If next descriptor is not for response, allocate iovs. */
			if (!spdk_vhost_vring_desc_is_wr(spdk_vhost_vring_desc_get_next(vq->desc, desc))) {
				iovs = spdk_vhost_iovec_alloc();

				if (iovs == NULL) {
					return 1;
				}

				iovcnt_max = VHOST_SCSI_IOVS_LEN;
			}
		}

		/* Process descriptors up to response. */
		while (!spdk_vhost_vring_desc_is_wr(desc) && iovcnt < iovcnt_max) {
			iovs[iovcnt].iov_base = spdk_vhost_gpa_to_vva(vdev, desc->addr);
			iovs[iovcnt].iov_len = desc->len;
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

	task->scsi_dev = get_scsi_dev(task->svdev, req->lun);
	if (unlikely(task->scsi_dev == NULL)) {
		task->resp->response = VIRTIO_SCSI_S_BAD_TARGET;
		return -1;
	}

	task->scsi.lun = get_scsi_lun(task->scsi_dev, req->lun);
	task->scsi.cdb = req->cdb;
	task->scsi.target_port = spdk_scsi_dev_find_port_by_id(task->scsi_dev, 0);
	SPDK_TRACEDUMP(SPDK_TRACE_VHOST_SCSI_DATA, "request CDB", req->cdb, VIRTIO_SCSI_CDB_SIZE);
	return 0;
}

static void
process_controlq(struct spdk_vhost_scsi_dev *vdev, struct rte_vhost_vring *vq)
{
	uint16_t reqs[32];
	uint16_t reqs_cnt, i;

	reqs_cnt = spdk_vhost_vq_avail_ring_get(vq, reqs, RTE_DIM(reqs));
	for (i = 0; i < reqs_cnt; i++) {
		process_ctrl_request(vdev, vq, reqs[i]);
	}
}

static void
process_requestq(struct spdk_vhost_scsi_dev *svdev, struct rte_vhost_vring *vq)
{
	uint16_t reqs[32];
	uint16_t reqs_cnt, i;
	struct spdk_vhost_task *task;
	int result;

	reqs_cnt = spdk_vhost_vq_avail_ring_get(vq, reqs, RTE_DIM(reqs));
	assert(reqs_cnt <= 32);

	for (i = 0; i < reqs_cnt; i++) {
		task = spdk_vhost_task_get(svdev);

		SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI, "====== Starting processing request idx %"PRIu16"======\n",
			      reqs[i]);
		task->vq = vq;
		task->svdev = svdev;
		task->req_idx = reqs[i];
		result = process_request(task);
		if (likely(result == 0)) {
			task_submit(task);
			SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI, "====== Task %p req_idx %d submitted ======\n", task,
				      task->req_idx);
		} else if (result > 0) {
			spdk_vhost_enqueue_task(task);
			SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI, "====== Task %p req_idx %d deferred ======\n", task, task->req_idx);
		} else {
			invalid_request(task);
			SPDK_TRACELOG(SPDK_TRACE_VHOST_SCSI, "====== Task %p req_idx %d failed ======\n", task, task->req_idx);
		}
	}
}

static void
vdev_controlq_worker(void *arg)
{
	struct spdk_vhost_scsi_dev *svdev = arg;

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
	struct spdk_vhost_scsi_dev *svdev = arg1;
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
	spdk_poller_register(&svdev->controlq_poller, vdev_controlq_worker, svdev, vdev->lcore,
			     CONTROLQ_POLL_PERIOD_US);
	sem_post((sem_t *)arg2);
}

static void
remove_vdev_cb(void *arg1, void *arg2)
{
	struct spdk_vhost_scsi_dev *svdev = arg1;
	uint32_t i;

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; i++) {
		if (svdev->scsi_dev[i] == NULL) {
			continue;
		}
		spdk_scsi_dev_free_io_channels(svdev->scsi_dev[i]);
	}

	SPDK_NOTICELOG("Stopping poller for vhost controller %s\n", svdev->vdev.name);
	spdk_vhost_dev_mem_unregister(&svdev->vdev);

	sem_post((sem_t *)arg2);
}

int
spdk_vhost_scsi_dev_construct(const char *name, uint64_t cpumask)
{
	struct spdk_vhost_scsi_dev *svdev;
	struct spdk_vhost_dev *vdev;
	int rc;

	if (name == NULL) {
		SPDK_ERRLOG("Can't add controller with no name\n");
		return -EINVAL;
	}

	if ((cpumask & spdk_app_get_core_mask()) != cpumask) {
		SPDK_ERRLOG("cpumask 0x%jx not a subset of app mask 0x%jx\n",
			    cpumask, spdk_app_get_core_mask());
		return -EINVAL;
	}

	svdev = spdk_dma_zmalloc(sizeof(*svdev), SPDK_CACHE_LINE_SIZE, NULL);
	if (svdev == NULL) {
		SPDK_ERRLOG("Couldn't allocate memory for vhost dev\n");
		return -ENOMEM;
	}

	vdev = &svdev->vdev;
	vdev->name =  strdup(name);
	vdev->cpumask = cpumask;
	vdev->lcore = -1;

	vdev->type = SPDK_VHOST_DEV_T_SCSI;

	rc = spdk_vhost_dev_register(vdev, &spdk_vhost_scsi_device_backend);
	if (rc < 0) {
		free(vdev->name);
		spdk_dma_free(svdev);
	}

	return rc;
}

int
spdk_vhost_scsi_dev_remove(struct spdk_vhost_scsi_dev *svdev)
{
	struct spdk_vhost_dev *vdev;
	int i;

	vdev = &svdev->vdev;
	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; ++i) {
		if (svdev->scsi_dev[i]) {
			SPDK_ERRLOG("Trying to remove non-empty controller: %s.\n", vdev->name);
			return -EBUSY;
		}
	}

	if (spdk_vhost_dev_unregister(vdev) != 0) {
		SPDK_ERRLOG("Could not unregister scsi controller %s with vhost library\n", vdev->name);
		return -EIO;
	}

	SPDK_NOTICELOG("Controller %s: removed\n", vdev->name);

	/*
	 * since spdk_vhost_scsi_vdev must not be in use,
	 * it should be already *destructed* (spdk_vhost_dev_destruct)
	 */
	free(vdev->name);
	spdk_dma_free(svdev);

	return 0;
}

struct spdk_scsi_dev *
spdk_vhost_scsi_dev_get_dev(struct spdk_vhost_scsi_dev *svdev, uint8_t num)
{
	assert(svdev != NULL);
	assert(num < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS);
	return svdev->scsi_dev[num];
}

int
spdk_vhost_scsi_dev_add_dev(const char *ctrlr_name, unsigned scsi_dev_num, const char *lun_name)
{
	struct spdk_vhost_scsi_dev *svdev;
	struct spdk_vhost_dev *vdev;
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

	svdev = (struct spdk_vhost_scsi_dev *) spdk_vhost_dev_find(ctrlr_name);
	if (svdev == NULL) {
		SPDK_ERRLOG("Controller %s is not defined\n", ctrlr_name);
		return -ENODEV;
	}

	vdev = &svdev->vdev;

	if (vdev->lcore != -1) {
		SPDK_ERRLOG("Controller %s is in use and hotplug is not supported\n", ctrlr_name);
		return -ENODEV;
	}

	if (svdev->scsi_dev[scsi_dev_num] != NULL) {
		SPDK_ERRLOG("Controller %s dev %u already occupied\n", ctrlr_name, scsi_dev_num);
		return -EEXIST;
	}

	/*
	 * At this stage only one LUN per device
	 */
	snprintf(dev_name, sizeof(dev_name), "Dev %u", scsi_dev_num);
	lun_id_list[0] = 0;
	lun_names_list[0] = (char *)lun_name;

	svdev->scsi_dev[scsi_dev_num] = spdk_scsi_dev_construct(dev_name, lun_names_list, lun_id_list, 1,
					SPDK_SPC_PROTOCOL_IDENTIFIER_SAS);

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
spdk_vhost_scsi_dev_remove_dev(struct spdk_vhost_scsi_dev *svdev, unsigned scsi_dev_num)
{
	struct spdk_vhost_dev *vdev = &svdev->vdev;

	if (vdev->lcore != -1) {
		SPDK_ERRLOG("Controller %s is in use and hotremove is not supported\n", vdev->name);
		return -EBUSY;
	}

	if (svdev->scsi_dev[scsi_dev_num] == NULL) {
		SPDK_ERRLOG("Controller %s dev %u is not occupied\n", vdev->name, scsi_dev_num);
		return -ENODEV;
	}

	spdk_scsi_dev_destruct(svdev->scsi_dev[scsi_dev_num]);
	svdev->scsi_dev[scsi_dev_num] = NULL;

	SPDK_NOTICELOG("Controller %s: removed device 'Dev %u'\n",
		       vdev->name, scsi_dev_num);
	return 0;
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
	struct spdk_vhost_dev *vdev = NULL;
	struct spdk_event *event;
	sem_t added;

	vdev = spdk_vhost_dev_load(vid);
	if (vdev == NULL) {
		return -1;
	}

	event = vhost_sem_event_alloc(vdev->lcore, add_vdev_cb, vdev, &added);
	spdk_event_call(event);
	if (vhost_sem_timedwait(&added, 1))
		rte_panic("Failed to register new device '%s'\n", vdev->name);
	return 0;
}

static void
destroy_device(int vid)
{
	struct spdk_vhost_scsi_dev *svdev;
	struct spdk_vhost_dev *vdev;
	struct spdk_event *event;
	sem_t done_sem;
	uint32_t i;

	vdev = spdk_vhost_dev_find_by_vid(vid);
	if (vdev == NULL) {
		rte_panic("Couldn't find device with vid %d to stop.\n", vid);
	}
	svdev = (struct spdk_vhost_scsi_dev *) vdev;

	event = vhost_sem_event_alloc(vdev->lcore, vdev_event_done_cb, NULL, &done_sem);
	spdk_poller_unregister(&svdev->requestq_poller, event);
	if (vhost_sem_timedwait(&done_sem, 1))
		rte_panic("%s: failed to unregister request queue poller.\n", vdev->name);

	event = vhost_sem_event_alloc(vdev->lcore, vdev_event_done_cb, NULL, &done_sem);
	spdk_poller_unregister(&svdev->controlq_poller, event);
	if (vhost_sem_timedwait(&done_sem, 1))
		rte_panic("%s: failed to unregister control queue poller.\n", vdev->name);

	/* Wait for all tasks to finish */
	for (i = 1000; i && vdev->task_cnt > 0; i--) {
		usleep(1000);
	}

	if (vdev->task_cnt > 0) {
		rte_panic("%s: pending tasks did not finish in 1s.\n", vdev->name);
	}

	event = vhost_sem_event_alloc(vdev->lcore, remove_vdev_cb, svdev, &done_sem);
	spdk_event_call(event);
	if (vhost_sem_timedwait(&done_sem, 1))
		rte_panic("%s: failed to unregister poller.\n", vdev->name);

	spdk_vhost_dev_unload(vdev);
}

SPDK_LOG_REGISTER_TRACE_FLAG("vhost", SPDK_TRACE_VHOST_SCSI)
SPDK_LOG_REGISTER_TRACE_FLAG("vhost_queue", SPDK_TRACE_VHOST_SCSI_DATA)
SPDK_LOG_REGISTER_TRACE_FLAG("vhost_data", SPDK_TRACE_VHOST_SCSI_DATA)
