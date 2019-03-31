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
#include "spdk/thread.h"
#include "spdk/scsi.h"
#include "spdk/scsi_spec.h"
#include "spdk/conf.h"
#include "spdk/event.h"
#include "spdk/util.h"
#include "spdk/likely.h"

#include "spdk/vhost.h"
#include "vhost_internal.h"

/* Features supported by SPDK VHOST lib. */
#define SPDK_VHOST_SCSI_FEATURES	(SPDK_VHOST_FEATURES | \
					(1ULL << VIRTIO_SCSI_F_INOUT) | \
					(1ULL << VIRTIO_SCSI_F_HOTPLUG) | \
					(1ULL << VIRTIO_SCSI_F_CHANGE ) | \
					(1ULL << VIRTIO_SCSI_F_T10_PI ))

/* Features that are specified in VIRTIO SCSI but currently not supported:
 * - Live migration not supported yet
 * - T10 PI
 */
#define SPDK_VHOST_SCSI_DISABLED_FEATURES	(SPDK_VHOST_DISABLED_FEATURES | \
						(1ULL << VIRTIO_SCSI_F_T10_PI ))

#define MGMT_POLL_PERIOD_US (1000 * 5)

#define VIRTIO_SCSI_CONTROLQ   0
#define VIRTIO_SCSI_EVENTQ   1
#define VIRTIO_SCSI_REQUESTQ   2

enum spdk_scsi_dev_vhost_status {
	/* Target ID is empty. */
	VHOST_SCSI_DEV_EMPTY,

	/* Target is still being added. */
	VHOST_SCSI_DEV_ADDING,

	/* Target ID occupied. */
	VHOST_SCSI_DEV_PRESENT,

	/* Target ID is occupied but removal is in progress. */
	VHOST_SCSI_DEV_REMOVING,

	/* In session - device (SCSI target) seen but removed. */
	VHOST_SCSI_DEV_REMOVED,
};

/** Context for a SCSI target in a vhost device */
struct spdk_scsi_dev_vhost_state {
	struct spdk_scsi_dev *dev;
	enum spdk_scsi_dev_vhost_status status;
	spdk_vhost_event_fn remove_cb;
	void *remove_ctx;
};

struct spdk_vhost_scsi_dev {
	struct spdk_vhost_dev vdev;
	struct spdk_scsi_dev_vhost_state scsi_dev_state[SPDK_VHOST_SCSI_CTRLR_MAX_DEVS];

	/* The CPU chosen to poll I/O of all active vhost sessions */
	int32_t lcore;
} __rte_cache_aligned;

/** Context for a SCSI target in a vhost session */
struct spdk_scsi_dev_session_state {
	struct spdk_scsi_dev *dev;
	enum spdk_scsi_dev_vhost_status status;
};

struct spdk_vhost_scsi_session {
	struct spdk_vhost_session vsession;

	struct spdk_vhost_scsi_dev *svdev;
	/** Local copy of the device state */
	struct spdk_scsi_dev_session_state scsi_dev_state[SPDK_VHOST_SCSI_CTRLR_MAX_DEVS];
	struct spdk_poller *requestq_poller;
	struct spdk_poller *mgmt_poller;
	struct spdk_poller *stop_poller;
};

struct spdk_vhost_scsi_task {
	struct spdk_scsi_task	scsi;
	struct iovec iovs[SPDK_VHOST_IOVS_MAX];

	union {
		struct virtio_scsi_cmd_resp *resp;
		struct virtio_scsi_ctrl_tmf_resp *tmf_resp;
	};

	struct spdk_vhost_scsi_session *svsession;
	struct spdk_scsi_dev *scsi_dev;

	/** Number of bytes that were written. */
	uint32_t used_len;

	int req_idx;

	/* If set, the task is currently used for I/O processing. */
	bool used;

	struct spdk_vhost_virtqueue *vq;
};

static int spdk_vhost_scsi_start(struct spdk_vhost_session *vsession);
static int spdk_vhost_scsi_stop(struct spdk_vhost_session *vsession);
static void spdk_vhost_scsi_dump_info_json(struct spdk_vhost_dev *vdev,
		struct spdk_json_write_ctx *w);
static void spdk_vhost_scsi_write_config_json(struct spdk_vhost_dev *vdev,
		struct spdk_json_write_ctx *w);
static int spdk_vhost_scsi_dev_remove(struct spdk_vhost_dev *vdev);

const struct spdk_vhost_dev_backend spdk_vhost_scsi_device_backend = {
	.virtio_features = SPDK_VHOST_SCSI_FEATURES,
	.disabled_features = SPDK_VHOST_SCSI_DISABLED_FEATURES,
	.session_ctx_size = sizeof(struct spdk_vhost_scsi_session) - sizeof(struct spdk_vhost_session),
	.start_session =  spdk_vhost_scsi_start,
	.stop_session = spdk_vhost_scsi_stop,
	.dump_info_json = spdk_vhost_scsi_dump_info_json,
	.write_config_json = spdk_vhost_scsi_write_config_json,
	.remove_device = spdk_vhost_scsi_dev_remove,
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
	struct spdk_vhost_session *vsession = &task->svsession->vsession;

	assert(vsession->task_cnt > 0);
	vsession->task_cnt--;
	task->used = false;
}

static int
remove_scsi_tgt(struct spdk_vhost_scsi_dev *svdev,
		unsigned scsi_tgt_num)
{
	struct spdk_scsi_dev_vhost_state *state;
	struct spdk_scsi_dev *dev;

	state = &svdev->scsi_dev_state[scsi_tgt_num];
	if (state->dev == NULL) {
		/* we've been already removed in the meantime */
		return 0;
	}

	dev = state->dev;
	state->dev = NULL;
	assert(state->status == VHOST_SCSI_DEV_REMOVING);
	state->status = VHOST_SCSI_DEV_EMPTY;
	spdk_scsi_dev_destruct(dev);
	if (state->remove_cb) {
		state->remove_cb(&svdev->vdev, state->remove_ctx);
		state->remove_cb = NULL;
	}
	SPDK_INFOLOG(SPDK_LOG_VHOST, "%s: removed target 'Target %u'\n",
		     svdev->vdev.name, scsi_tgt_num);
	return 0;
}

static int
spdk_vhost_scsi_session_process_removed(struct spdk_vhost_dev *vdev,
					struct spdk_vhost_session *vsession, void *ctx)
{
	unsigned scsi_tgt_num = (unsigned)(uintptr_t)ctx;
	struct spdk_vhost_scsi_session *svsession;
	struct spdk_scsi_dev_session_state *state;

	if (vsession == NULL) {
		/* all sessions have already detached the device */
		struct spdk_vhost_scsi_dev *svdev = SPDK_CONTAINEROF(vdev,
						    struct spdk_vhost_scsi_dev, vdev);

		return remove_scsi_tgt(svdev, scsi_tgt_num);
	}

	svsession = (struct spdk_vhost_scsi_session *)vsession;
	state = &svsession->scsi_dev_state[scsi_tgt_num];

	if (state->dev != NULL) {
		/* there's still a session that references this device,
		 * so abort our foreach chain here. We'll be called
		 * again from this session's management poller after it
		 * is removed in there
		 */
		return -1;
	}

	return 0;
}

static void
process_removed_devs(struct spdk_vhost_scsi_session *svsession)
{
	struct spdk_scsi_dev *dev;
	struct spdk_scsi_dev_session_state *state;
	int i;

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; ++i) {
		state = &svsession->scsi_dev_state[i];
		dev = state->dev;

		if (dev && state->status == VHOST_SCSI_DEV_REMOVING && !spdk_scsi_dev_has_pending_tasks(dev)) {
			/* detach the device from this session */
			spdk_scsi_dev_free_io_channels(dev);
			state->dev = NULL;
			state->status = VHOST_SCSI_DEV_REMOVED;
			/* try to detach it globally */
			spdk_vhost_lock();
			spdk_vhost_dev_foreach_session(&svsession->svdev->vdev,
						       spdk_vhost_scsi_session_process_removed,
						       (void *)(uintptr_t)i);
			spdk_vhost_unlock();
		}
	}
}

static void
eventq_enqueue(struct spdk_vhost_scsi_session *svsession, unsigned scsi_dev_num,
	       uint32_t event, uint32_t reason)
{
	struct spdk_vhost_session *vsession = &svsession->vsession;
	struct spdk_vhost_dev *vdev = vsession->vdev;
	struct spdk_vhost_virtqueue *vq;
	struct vring_desc *desc, *desc_table;
	struct virtio_scsi_event *desc_ev;
	uint32_t desc_table_size, req_size = 0;
	uint16_t req;
	int rc;

	assert(scsi_dev_num < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS);
	vq = &vsession->virtqueue[VIRTIO_SCSI_EVENTQ];

	if (spdk_vhost_vq_avail_ring_get(vq, &req, 1) != 1) {
		SPDK_ERRLOG("Controller %s: Failed to send virtio event (no avail ring entries?).\n",
			    vdev->name);
		return;
	}

	rc = spdk_vhost_vq_get_desc(vsession, vq, req, &desc, &desc_table, &desc_table_size);
	if (rc != 0 || desc->len < sizeof(*desc_ev)) {
		SPDK_ERRLOG("Controller %s: Invalid eventq descriptor at index %"PRIu16".\n",
			    vdev->name, req);
		goto out;
	}

	desc_ev = spdk_vhost_gpa_to_vva(vsession, desc->addr, sizeof(*desc_ev));
	if (desc_ev == NULL) {
		SPDK_ERRLOG("Controller %s: Eventq descriptor at index %"PRIu16" points to unmapped guest memory address %p.\n",
			    vdev->name, req, (void *)(uintptr_t)desc->addr);
		goto out;
	}

	desc_ev->event = event;
	desc_ev->lun[0] = 1;
	desc_ev->lun[1] = scsi_dev_num;
	/* virtio LUN id 0 can refer either to the entire device
	 * or actual LUN 0 (the only supported by vhost for now)
	 */
	desc_ev->lun[2] = 0 >> 8;
	desc_ev->lun[3] = 0 & 0xFF;
	/* virtio doesn't specify any strict format for LUN id (bytes 2 and 3)
	 * current implementation relies on linux kernel sources
	 */
	memset(&desc_ev->lun[4], 0, 4);
	desc_ev->reason = reason;
	req_size = sizeof(*desc_ev);

out:
	spdk_vhost_vq_used_ring_enqueue(vsession, vq, req, req_size);
}

static void
submit_completion(struct spdk_vhost_scsi_task *task)
{
	struct spdk_vhost_session *vsession = &task->svsession->vsession;

	spdk_vhost_vq_used_ring_enqueue(vsession, task->vq, task->req_idx,
					task->used_len);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_SCSI, "Finished task (%p) req_idx=%d\n", task, task->req_idx);

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
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_SCSI, "Task (%p) req_idx=%d failed - status=%u\n", task, task->req_idx,
			      task->scsi.status);
	}
	assert(task->scsi.transfer_len == task->scsi.length);
	task->resp->resid = task->scsi.length - task->scsi.data_transferred;

	submit_completion(task);
}

static void
task_submit(struct spdk_vhost_scsi_task *task)
{
	task->resp->response = VIRTIO_SCSI_S_OK;
	spdk_scsi_dev_queue_task(task->scsi_dev, &task->scsi);
}

static void
mgmt_task_submit(struct spdk_vhost_scsi_task *task, enum spdk_scsi_task_func func)
{
	task->tmf_resp->response = VIRTIO_SCSI_S_OK;
	task->scsi.function = func;
	spdk_scsi_dev_queue_mgmt_task(task->scsi_dev, &task->scsi);
}

static void
invalid_request(struct spdk_vhost_scsi_task *task)
{
	struct spdk_vhost_session *vsession = &task->svsession->vsession;

	spdk_vhost_vq_used_ring_enqueue(vsession, task->vq, task->req_idx,
					task->used_len);
	spdk_vhost_scsi_task_put(task);

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_SCSI, "Invalid request (status=%" PRIu8")\n",
		      task->resp ? task->resp->response : -1);
}

static int
spdk_vhost_scsi_task_init_target(struct spdk_vhost_scsi_task *task, const __u8 *lun)
{
	struct spdk_vhost_scsi_session *svsession = task->svsession;
	struct spdk_scsi_dev_session_state *state;
	uint16_t lun_id = (((uint16_t)lun[2] << 8) | lun[3]) & 0x3FFF;

	SPDK_LOGDUMP(SPDK_LOG_VHOST_SCSI_QUEUE, "LUN", lun, 8);

	/* First byte must be 1 and second is target */
	if (lun[0] != 1 || lun[1] >= SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
		return -1;
	}

	state = &svsession->scsi_dev_state[lun[1]];
	task->scsi_dev = state->dev;
	if (state->dev == NULL || state->status != VHOST_SCSI_DEV_PRESENT) {
		/* If dev has been hotdetached, return 0 to allow sending
		 * additional hotremove event via sense codes.
		 */
		return state->status != VHOST_SCSI_DEV_EMPTY ? 0 : -1;
	}

	task->scsi.target_port = spdk_scsi_dev_find_port_by_id(task->scsi_dev, 0);
	task->scsi.lun = spdk_scsi_dev_get_lun(state->dev, lun_id);
	return 0;
}

static void
process_ctrl_request(struct spdk_vhost_scsi_task *task)
{
	struct spdk_vhost_session *vsession = &task->svsession->vsession;
	struct spdk_vhost_dev *vdev = vsession->vdev;
	struct vring_desc *desc, *desc_table;
	struct virtio_scsi_ctrl_tmf_req *ctrl_req;
	struct virtio_scsi_ctrl_an_resp *an_resp;
	uint32_t desc_table_size, used_len = 0;
	int rc;

	spdk_scsi_task_construct(&task->scsi, spdk_vhost_scsi_task_mgmt_cpl, spdk_vhost_scsi_task_free_cb);
	rc = spdk_vhost_vq_get_desc(vsession, task->vq, task->req_idx, &desc, &desc_table,
				    &desc_table_size);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("%s: Invalid controlq descriptor at index %d.\n",
			    vdev->name, task->req_idx);
		goto out;
	}

	ctrl_req = spdk_vhost_gpa_to_vva(vsession, desc->addr, sizeof(*ctrl_req));
	if (ctrl_req == NULL) {
		SPDK_ERRLOG("%s: Invalid task management request at index %d.\n",
			    vdev->name, task->req_idx);
		goto out;
	}

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_SCSI_QUEUE,
		      "Processing controlq descriptor: desc %d/%p, desc_addr %p, len %d, flags %d, last_used_idx %d; kickfd %d; size %d\n",
		      task->req_idx, desc, (void *)desc->addr, desc->len, desc->flags, task->vq->last_used_idx,
		      task->vq->vring.kickfd, task->vq->vring.size);
	SPDK_LOGDUMP(SPDK_LOG_VHOST_SCSI_QUEUE, "Request descriptor", (uint8_t *)ctrl_req, desc->len);

	spdk_vhost_scsi_task_init_target(task, ctrl_req->lun);

	spdk_vhost_vring_desc_get_next(&desc, desc_table, desc_table_size);
	if (spdk_unlikely(desc == NULL)) {
		SPDK_ERRLOG("%s: No response descriptor for controlq request %d.\n",
			    vdev->name, task->req_idx);
		goto out;
	}

	/* Process the TMF request */
	switch (ctrl_req->type) {
	case VIRTIO_SCSI_T_TMF:
		task->tmf_resp = spdk_vhost_gpa_to_vva(vsession, desc->addr, sizeof(*task->tmf_resp));
		if (spdk_unlikely(desc->len < sizeof(struct virtio_scsi_ctrl_tmf_resp) || task->tmf_resp == NULL)) {
			SPDK_ERRLOG("%s: TMF response descriptor at index %d points to invalid guest memory region\n",
				    vdev->name, task->req_idx);
			goto out;
		}

		/* Check if we are processing a valid request */
		if (task->scsi_dev == NULL) {
			task->tmf_resp->response = VIRTIO_SCSI_S_BAD_TARGET;
			break;
		}

		switch (ctrl_req->subtype) {
		case VIRTIO_SCSI_T_TMF_LOGICAL_UNIT_RESET:
			/* Handle LUN reset */
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_SCSI_QUEUE, "LUN reset\n");

			mgmt_task_submit(task, SPDK_SCSI_TASK_FUNC_LUN_RESET);
			return;
		default:
			task->tmf_resp->response = VIRTIO_SCSI_S_ABORTED;
			/* Unsupported command */
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_SCSI_QUEUE, "Unsupported TMF command %x\n", ctrl_req->subtype);
			break;
		}
		break;
	case VIRTIO_SCSI_T_AN_QUERY:
	case VIRTIO_SCSI_T_AN_SUBSCRIBE: {
		an_resp = spdk_vhost_gpa_to_vva(vsession, desc->addr, sizeof(*an_resp));
		if (spdk_unlikely(desc->len < sizeof(struct virtio_scsi_ctrl_an_resp) || an_resp == NULL)) {
			SPDK_WARNLOG("%s: Asynchronous response descriptor points to invalid guest memory region\n",
				     vdev->name);
			goto out;
		}

		an_resp->response = VIRTIO_SCSI_S_ABORTED;
		break;
	}
	default:
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_SCSI_QUEUE, "Unsupported control command %x\n", ctrl_req->type);
		break;
	}

	used_len = sizeof(struct virtio_scsi_ctrl_tmf_resp);
out:
	spdk_vhost_vq_used_ring_enqueue(vsession, task->vq, task->req_idx, used_len);
	spdk_vhost_scsi_task_put(task);
}

/*
 * Process task's descriptor chain and setup data related fields.
 * Return
 *   -1 if request is invalid and must be aborted,
 *    0 if all data are set.
 */
static int
task_data_setup(struct spdk_vhost_scsi_task *task,
		struct virtio_scsi_cmd_req **req)
{
	struct spdk_vhost_session *vsession = &task->svsession->vsession;
	struct spdk_vhost_dev *vdev = vsession->vdev;
	struct vring_desc *desc, *desc_table;
	struct iovec *iovs = task->iovs;
	uint16_t iovcnt = 0;
	uint32_t desc_table_len, len = 0;
	int rc;

	spdk_scsi_task_construct(&task->scsi, spdk_vhost_scsi_task_cpl, spdk_vhost_scsi_task_free_cb);

	rc = spdk_vhost_vq_get_desc(vsession, task->vq, task->req_idx, &desc, &desc_table, &desc_table_len);
	/* First descriptor must be readable */
	if (spdk_unlikely(rc != 0  || spdk_vhost_vring_desc_is_wr(desc) ||
			  desc->len < sizeof(struct virtio_scsi_cmd_req))) {
		SPDK_WARNLOG("%s: invalid first (request) descriptor at index %"PRIu16".\n",
			     vdev->name, task->req_idx);
		goto invalid_task;
	}

	*req = spdk_vhost_gpa_to_vva(vsession, desc->addr, sizeof(**req));
	if (spdk_unlikely(*req == NULL)) {
		SPDK_WARNLOG("%s: Request descriptor at index %d points to invalid guest memory region\n",
			     vdev->name, task->req_idx);
		goto invalid_task;
	}

	/* Each request must have at least 2 descriptors (e.g. request and response) */
	spdk_vhost_vring_desc_get_next(&desc, desc_table, desc_table_len);
	if (desc == NULL) {
		SPDK_WARNLOG("%s: Descriptor chain at index %d contains neither payload nor response buffer.\n",
			     vdev->name, task->req_idx);
		goto invalid_task;
	}
	task->scsi.dxfer_dir = spdk_vhost_vring_desc_is_wr(desc) ? SPDK_SCSI_DIR_FROM_DEV :
			       SPDK_SCSI_DIR_TO_DEV;
	task->scsi.iovs = iovs;

	if (task->scsi.dxfer_dir == SPDK_SCSI_DIR_FROM_DEV) {
		/*
		 * FROM_DEV (READ): [RD_req][WR_resp][WR_buf0]...[WR_bufN]
		 */
		task->resp = spdk_vhost_gpa_to_vva(vsession, desc->addr, sizeof(*task->resp));
		if (spdk_unlikely(desc->len < sizeof(struct virtio_scsi_cmd_resp) || task->resp == NULL)) {
			SPDK_WARNLOG("%s: Response descriptor at index %d points to invalid guest memory region\n",
				     vdev->name, task->req_idx);
			goto invalid_task;
		}
		rc = spdk_vhost_vring_desc_get_next(&desc, desc_table, desc_table_len);
		if (spdk_unlikely(rc != 0)) {
			SPDK_WARNLOG("%s: invalid descriptor chain at request index %d (descriptor id overflow?).\n",
				     vdev->name, task->req_idx);
			goto invalid_task;
		}

		if (desc == NULL) {
			/*
			 * TEST UNIT READY command and some others might not contain any payload and this is not an error.
			 */
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_SCSI_DATA,
				      "No payload descriptors for FROM DEV command req_idx=%"PRIu16".\n", task->req_idx);
			SPDK_LOGDUMP(SPDK_LOG_VHOST_SCSI_DATA, "CDB=", (*req)->cdb, VIRTIO_SCSI_CDB_SIZE);
			task->used_len = sizeof(struct virtio_scsi_cmd_resp);
			task->scsi.iovcnt = 1;
			task->scsi.iovs[0].iov_len = 0;
			task->scsi.length = 0;
			task->scsi.transfer_len = 0;
			return 0;
		}

		/* All remaining descriptors are data. */
		while (desc) {
			if (spdk_unlikely(!spdk_vhost_vring_desc_is_wr(desc))) {
				SPDK_WARNLOG("FROM DEV cmd: descriptor nr %" PRIu16" in payload chain is read only.\n", iovcnt);
				goto invalid_task;
			}

			if (spdk_unlikely(spdk_vhost_vring_desc_to_iov(vsession, iovs, &iovcnt, desc))) {
				goto invalid_task;
			}
			len += desc->len;

			rc = spdk_vhost_vring_desc_get_next(&desc, desc_table, desc_table_len);
			if (spdk_unlikely(rc != 0)) {
				SPDK_WARNLOG("%s: invalid payload in descriptor chain starting at index %d.\n",
					     vdev->name, task->req_idx);
				goto invalid_task;
			}
		}

		task->used_len = sizeof(struct virtio_scsi_cmd_resp) + len;
	} else {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_SCSI_DATA, "TO DEV");
		/*
		 * TO_DEV (WRITE):[RD_req][RD_buf0]...[RD_bufN][WR_resp]
		 * No need to check descriptor WR flag as this is done while setting scsi.dxfer_dir.
		 */

		/* Process descriptors up to response. */
		while (!spdk_vhost_vring_desc_is_wr(desc)) {
			if (spdk_unlikely(spdk_vhost_vring_desc_to_iov(vsession, iovs, &iovcnt, desc))) {
				goto invalid_task;
			}
			len += desc->len;

			spdk_vhost_vring_desc_get_next(&desc, desc_table, desc_table_len);
			if (spdk_unlikely(desc == NULL)) {
				SPDK_WARNLOG("TO_DEV cmd: no response descriptor.\n");
				goto invalid_task;
			}
		}

		task->resp = spdk_vhost_gpa_to_vva(vsession, desc->addr, sizeof(*task->resp));
		if (spdk_unlikely(desc->len < sizeof(struct virtio_scsi_cmd_resp) || task->resp == NULL)) {
			SPDK_WARNLOG("%s: Response descriptor at index %d points to invalid guest memory region\n",
				     vdev->name, task->req_idx);
			goto invalid_task;
		}

		task->used_len = sizeof(struct virtio_scsi_cmd_resp);
	}

	task->scsi.iovcnt = iovcnt;
	task->scsi.length = len;
	task->scsi.transfer_len = len;
	return 0;

invalid_task:
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_SCSI_DATA, "%s: Invalid task at index %"PRIu16".\n",
		      vdev->name, task->req_idx);
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
	SPDK_LOGDUMP(SPDK_LOG_VHOST_SCSI_DATA, "request CDB", req->cdb, VIRTIO_SCSI_CDB_SIZE);

	if (spdk_unlikely(task->scsi.lun == NULL)) {
		spdk_scsi_task_process_null_lun(&task->scsi);
		task->resp->response = VIRTIO_SCSI_S_OK;
		return 1;
	}

	return 0;
}

static void
process_controlq(struct spdk_vhost_scsi_session *svsession, struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_scsi_dev *svdev = svsession->svdev;
	struct spdk_vhost_session *vsession = &svsession->vsession;
	struct spdk_vhost_scsi_task *task;
	uint16_t reqs[32];
	uint16_t reqs_cnt, i;

	reqs_cnt = spdk_vhost_vq_avail_ring_get(vq, reqs, SPDK_COUNTOF(reqs));
	for (i = 0; i < reqs_cnt; i++) {
		if (spdk_unlikely(reqs[i] >= vq->vring.size)) {
			SPDK_ERRLOG("%s: invalid entry in avail ring. Buffer '%"PRIu16"' exceeds virtqueue size (%"PRIu16")\n",
				    svdev->vdev.name, reqs[i], vq->vring.size);
			spdk_vhost_vq_used_ring_enqueue(vsession, vq, reqs[i], 0);
			continue;
		}

		task = &((struct spdk_vhost_scsi_task *)vq->tasks)[reqs[i]];
		if (spdk_unlikely(task->used)) {
			SPDK_ERRLOG("%s: invalid entry in avail ring. Buffer '%"PRIu16"' is still in use!\n",
				    svdev->vdev.name, reqs[i]);
			spdk_vhost_vq_used_ring_enqueue(vsession, vq, reqs[i], 0);
			continue;
		}

		vsession->task_cnt++;
		memset(&task->scsi, 0, sizeof(task->scsi));
		task->tmf_resp = NULL;
		task->used = true;
		process_ctrl_request(task);
	}
}

static void
process_requestq(struct spdk_vhost_scsi_session *svsession, struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_session *vsession = &svsession->vsession;
	struct spdk_vhost_dev *vdev = vsession->vdev;
	struct spdk_vhost_scsi_task *task;
	uint16_t reqs[32];
	uint16_t reqs_cnt, i;
	int result;

	reqs_cnt = spdk_vhost_vq_avail_ring_get(vq, reqs, SPDK_COUNTOF(reqs));
	assert(reqs_cnt <= 32);

	for (i = 0; i < reqs_cnt; i++) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_SCSI, "====== Starting processing request idx %"PRIu16"======\n",
			      reqs[i]);

		if (spdk_unlikely(reqs[i] >= vq->vring.size)) {
			SPDK_ERRLOG("%s: request idx '%"PRIu16"' exceeds virtqueue size (%"PRIu16").\n",
				    vdev->name, reqs[i], vq->vring.size);
			spdk_vhost_vq_used_ring_enqueue(vsession, vq, reqs[i], 0);
			continue;
		}

		task = &((struct spdk_vhost_scsi_task *)vq->tasks)[reqs[i]];
		if (spdk_unlikely(task->used)) {
			SPDK_ERRLOG("%s: request with idx '%"PRIu16"' is already pending.\n",
				    vdev->name, reqs[i]);
			spdk_vhost_vq_used_ring_enqueue(vsession, vq, reqs[i], 0);
			continue;
		}

		vsession->task_cnt++;
		memset(&task->scsi, 0, sizeof(task->scsi));
		task->resp = NULL;
		task->used = true;
		task->used_len = 0;
		result = process_request(task);
		if (likely(result == 0)) {
			task_submit(task);
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_SCSI, "====== Task %p req_idx %d submitted ======\n", task,
				      task->req_idx);
		} else if (result > 0) {
			spdk_vhost_scsi_task_cpl(&task->scsi);
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_SCSI, "====== Task %p req_idx %d finished early ======\n", task,
				      task->req_idx);
		} else {
			invalid_request(task);
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_SCSI, "====== Task %p req_idx %d failed ======\n", task,
				      task->req_idx);
		}
	}
}

static int
vdev_mgmt_worker(void *arg)
{
	struct spdk_vhost_scsi_session *svsession = arg;
	struct spdk_vhost_session *vsession = &svsession->vsession;

	process_removed_devs(svsession);
	spdk_vhost_vq_used_signal(vsession, &vsession->virtqueue[VIRTIO_SCSI_EVENTQ]);

	process_controlq(svsession, &vsession->virtqueue[VIRTIO_SCSI_CONTROLQ]);
	spdk_vhost_vq_used_signal(vsession, &vsession->virtqueue[VIRTIO_SCSI_CONTROLQ]);

	return -1;
}

static int
vdev_worker(void *arg)
{
	struct spdk_vhost_scsi_session *svsession = arg;
	struct spdk_vhost_session *vsession = &svsession->vsession;
	uint32_t q_idx;

	for (q_idx = VIRTIO_SCSI_REQUESTQ; q_idx < vsession->max_queues; q_idx++) {
		process_requestq(svsession, &vsession->virtqueue[q_idx]);
	}

	spdk_vhost_session_used_signal(vsession);

	return -1;
}

static struct spdk_vhost_scsi_dev *
to_scsi_dev(struct spdk_vhost_dev *ctrlr)
{
	if (ctrlr == NULL) {
		return NULL;
	}

	if (ctrlr->backend != &spdk_vhost_scsi_device_backend) {
		SPDK_ERRLOG("%s: not a vhost-scsi device.\n", ctrlr->name);
		return NULL;
	}

	return SPDK_CONTAINEROF(ctrlr, struct spdk_vhost_scsi_dev, vdev);
}

static struct spdk_vhost_scsi_session *
to_scsi_session(struct spdk_vhost_session *vsession)
{
	if (vsession == NULL) {
		return NULL;
	}

	if (vsession->vdev->backend != &spdk_vhost_scsi_device_backend) {
		SPDK_ERRLOG("%s: not a vhost-scsi device.\n", vsession->vdev->name);
		return NULL;
	}

	return (struct spdk_vhost_scsi_session *)vsession;
}

int
spdk_vhost_scsi_dev_construct(const char *name, const char *cpumask)
{
	struct spdk_vhost_scsi_dev *svdev = spdk_dma_zmalloc(sizeof(struct spdk_vhost_scsi_dev),
					    SPDK_CACHE_LINE_SIZE, NULL);
	int rc;

	if (svdev == NULL) {
		return -ENOMEM;
	}

	spdk_vhost_lock();
	rc = spdk_vhost_dev_register(&svdev->vdev, name, cpumask,
				     &spdk_vhost_scsi_device_backend);

	if (rc) {
		spdk_dma_free(svdev);
	}

	spdk_vhost_unlock();
	return rc;
}

static int
spdk_vhost_scsi_dev_remove(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_scsi_dev *svdev = to_scsi_dev(vdev);
	int rc, i;

	if (svdev == NULL) {
		return -EINVAL;
	}

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; ++i) {
		if (svdev->scsi_dev_state[i].dev) {
			if (vdev->registered) {
				SPDK_ERRLOG("Trying to remove non-empty controller: %s.\n", vdev->name);
				return -EBUSY;
			}

			rc = spdk_vhost_scsi_dev_remove_tgt(vdev, i, NULL, NULL);
			if (rc != 0) {
				SPDK_ERRLOG("%s: failed to force-remove target %d\n", vdev->name, i);
				return rc;
			}
		}
	}

	rc = spdk_vhost_dev_unregister(vdev);
	if (rc != 0) {
		return rc;
	}

	spdk_dma_free(svdev);
	return 0;
}

struct spdk_scsi_dev *
spdk_vhost_scsi_dev_get_tgt(struct spdk_vhost_dev *vdev, uint8_t num)
{
	struct spdk_vhost_scsi_dev *svdev;

	assert(num < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS);
	svdev = to_scsi_dev(vdev);
	if (svdev == NULL || svdev->scsi_dev_state[num].status != VHOST_SCSI_DEV_PRESENT) {
		return NULL;
	}

	assert(svdev->scsi_dev_state[num].dev != NULL);
	return svdev->scsi_dev_state[num].dev;
}

static void
spdk_vhost_scsi_lun_hotremove(const struct spdk_scsi_lun *lun, void *arg)
{
	struct spdk_vhost_scsi_dev *svdev = arg;
	const struct spdk_scsi_dev *scsi_dev;
	unsigned scsi_dev_num;

	assert(lun != NULL);
	assert(svdev != NULL);
	scsi_dev = spdk_scsi_lun_get_dev(lun);
	for (scsi_dev_num = 0; scsi_dev_num < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; scsi_dev_num++) {
		if (svdev->scsi_dev_state[scsi_dev_num].dev == scsi_dev) {
			break;
		}
	}

	if (scsi_dev_num == SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
		/* The entire device has been already removed. */
		return;
	}

	/* remove entire device */
	spdk_vhost_scsi_dev_remove_tgt(&svdev->vdev, scsi_dev_num, NULL, NULL);
}

static int
spdk_vhost_scsi_session_add_tgt(struct spdk_vhost_dev *vdev,
				struct spdk_vhost_session *vsession, void *ctx)
{
	unsigned scsi_tgt_num = (unsigned)(uintptr_t)ctx;
	struct spdk_vhost_scsi_session *svsession;
	struct spdk_scsi_dev_vhost_state *vhost_sdev;
	struct spdk_scsi_dev_session_state *session_sdev;
	int rc;

	if (vsession == NULL) {
		struct spdk_vhost_scsi_dev *svdev = SPDK_CONTAINEROF(vdev,
						    struct spdk_vhost_scsi_dev, vdev);
		vhost_sdev = &svdev->scsi_dev_state[scsi_tgt_num];

		/* All sessions have added the target */
		assert(vhost_sdev->status == VHOST_SCSI_DEV_ADDING);
		vhost_sdev->status = VHOST_SCSI_DEV_PRESENT;
		return 0;
	}

	if (vsession->lcore == -1) {
		/* Nothing to do. */
		return 0;
	}

	svsession = (struct spdk_vhost_scsi_session *)vsession;
	vhost_sdev = &svsession->svdev->scsi_dev_state[scsi_tgt_num];
	session_sdev = &svsession->scsi_dev_state[scsi_tgt_num];

	session_sdev->dev = vhost_sdev->dev;
	session_sdev->status = VHOST_SCSI_DEV_PRESENT;

	rc = spdk_scsi_dev_allocate_io_channels(svsession->scsi_dev_state[scsi_tgt_num].dev);
	if (rc != 0) {
		SPDK_ERRLOG("Couldn't allocate io channnel for SCSI target %u in device %s\n",
			    scsi_tgt_num, vdev->name);

		/* unset the SCSI target so that all I/O to it will be rejected */
		session_sdev->dev = NULL;
		/* Set status to EMPTY so that we won't reply with SCSI hotremove
		 * sense codes - the device hasn't ever been added.
		 */
		session_sdev->status = VHOST_SCSI_DEV_EMPTY;

		/* Return with no error. We'll continue allocating io_channels for
		 * other sessions on this device in hopes they succeed. The sessions
		 * that failed to allocate io_channels simply won't be able to
		 * detect the SCSI target, nor do any I/O to it.
		 */
		return 0;
	}

	if (spdk_vhost_dev_has_feature(vsession, VIRTIO_SCSI_F_HOTPLUG)) {
		eventq_enqueue(svsession, scsi_tgt_num,
			       VIRTIO_SCSI_T_TRANSPORT_RESET, VIRTIO_SCSI_EVT_RESET_RESCAN);
	} else {
		SPDK_NOTICELOG("Device %s does not support hotplug. "
			       "Please restart the driver or perform a rescan.\n",
			       vdev->name);
	}

	return 0;
}

int
spdk_vhost_scsi_dev_add_tgt(struct spdk_vhost_dev *vdev, int scsi_tgt_num,
			    const char *bdev_name)
{
	struct spdk_vhost_scsi_dev *svdev;
	struct spdk_scsi_dev_vhost_state *state;
	char target_name[SPDK_SCSI_DEV_MAX_NAME];
	int lun_id_list[1];
	const char *bdev_names_list[1];

	svdev = to_scsi_dev(vdev);
	if (svdev == NULL) {
		return -EINVAL;
	}

	if (scsi_tgt_num < 0) {
		for (scsi_tgt_num = 0; scsi_tgt_num < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; scsi_tgt_num++) {
			if (svdev->scsi_dev_state[scsi_tgt_num].dev == NULL) {
				break;
			}
		}

		if (scsi_tgt_num == SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
			SPDK_ERRLOG("Controller %s - all targets already in use.\n", vdev->name);
			return -ENOSPC;
		}
	} else {
		if (scsi_tgt_num >= SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
			SPDK_ERRLOG("Controller %s target %d number too big (max %d)\n", vdev->name, scsi_tgt_num,
				    SPDK_VHOST_SCSI_CTRLR_MAX_DEVS);
			return -EINVAL;
		}
	}

	if (bdev_name == NULL) {
		SPDK_ERRLOG("No lun name specified\n");
		return -EINVAL;
	}

	state = &svdev->scsi_dev_state[scsi_tgt_num];
	if (state->dev != NULL) {
		SPDK_ERRLOG("Controller %s target %u already occupied\n", vdev->name, scsi_tgt_num);
		return -EEXIST;
	}

	/*
	 * At this stage only one LUN per target
	 */
	snprintf(target_name, sizeof(target_name), "Target %u", scsi_tgt_num);
	lun_id_list[0] = 0;
	bdev_names_list[0] = (char *)bdev_name;

	state->status = VHOST_SCSI_DEV_ADDING;
	state->dev = spdk_scsi_dev_construct(target_name, bdev_names_list, lun_id_list, 1,
					     SPDK_SPC_PROTOCOL_IDENTIFIER_SAS,
					     spdk_vhost_scsi_lun_hotremove, svdev);

	if (state->dev == NULL) {
		state->status = VHOST_SCSI_DEV_EMPTY;
		SPDK_ERRLOG("Couldn't create spdk SCSI target '%s' using bdev '%s' in controller: %s\n",
			    target_name, bdev_name, vdev->name);
		return -EINVAL;
	}
	spdk_scsi_dev_add_port(state->dev, 0, "vhost");

	SPDK_INFOLOG(SPDK_LOG_VHOST, "Controller %s: defined target '%s' using bdev '%s'\n",
		     vdev->name, target_name, bdev_name);

	spdk_vhost_dev_foreach_session(vdev, spdk_vhost_scsi_session_add_tgt,
				       (void *)(uintptr_t)scsi_tgt_num);
	return scsi_tgt_num;
}

struct scsi_tgt_hotplug_ctx {
	unsigned scsi_tgt_num;
	bool async_fini;
};

static int
spdk_vhost_scsi_session_remove_tgt(struct spdk_vhost_dev *vdev,
				   struct spdk_vhost_session *vsession, void *_ctx)
{
	struct scsi_tgt_hotplug_ctx *ctx = _ctx;
	unsigned scsi_tgt_num = ctx->scsi_tgt_num;
	struct spdk_vhost_scsi_session *svsession;
	struct spdk_scsi_dev_session_state *state;
	int rc = 0;

	if (vsession == NULL) {
		struct spdk_vhost_scsi_dev *svdev = SPDK_CONTAINEROF(vdev,
						    struct spdk_vhost_scsi_dev, vdev);

		if (!ctx->async_fini) {
			/* there aren't any active sessions, so remove the dev and exit */
			rc = remove_scsi_tgt(svdev, scsi_tgt_num);
		}

		free(ctx);
		return rc;
	}

	svsession = (struct spdk_vhost_scsi_session *)vsession;
	state = &svsession->scsi_dev_state[scsi_tgt_num];

	if (vsession->lcore == -1 || state->dev == NULL) {
		/* Nothing to do */
		return 0;
	}

	/* Mark the target for removal */
	assert(state->status == VHOST_SCSI_DEV_PRESENT);
	state->status = VHOST_SCSI_DEV_REMOVING;

	/* Send a hotremove Virtio event */
	if (spdk_vhost_dev_has_feature(vsession, VIRTIO_SCSI_F_HOTPLUG)) {
		eventq_enqueue(svsession, scsi_tgt_num,
			       VIRTIO_SCSI_T_TRANSPORT_RESET, VIRTIO_SCSI_EVT_RESET_REMOVED);
	}

	/* Wait for the session's management poller to remove the target after
	 * all its pending I/O has finished.
	 */
	ctx->async_fini = true;
	return 0;
}

int
spdk_vhost_scsi_dev_remove_tgt(struct spdk_vhost_dev *vdev, unsigned scsi_tgt_num,
			       spdk_vhost_event_fn cb_fn, void *cb_arg)
{
	struct spdk_vhost_scsi_dev *svdev;
	struct spdk_scsi_dev_vhost_state *scsi_dev_state;
	struct scsi_tgt_hotplug_ctx *ctx;

	if (scsi_tgt_num >= SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
		SPDK_ERRLOG("%s: invalid target number %d\n", vdev->name, scsi_tgt_num);
		return -EINVAL;
	}

	svdev = to_scsi_dev(vdev);
	if (svdev == NULL) {
		return -ENODEV;
	}

	scsi_dev_state = &svdev->scsi_dev_state[scsi_tgt_num];
	if (scsi_dev_state->dev == NULL || scsi_dev_state->status == VHOST_SCSI_DEV_ADDING) {
		SPDK_ERRLOG("Controller %s target %u is not occupied\n", vdev->name, scsi_tgt_num);
		return -ENODEV;
	}

	assert(scsi_dev_state->status != VHOST_SCSI_DEV_EMPTY);
	if (scsi_dev_state->status != VHOST_SCSI_DEV_PRESENT) {
		SPDK_WARNLOG("%s: 'Target %u' has been already marked to hotremove.\n",
			     vdev->name, scsi_tgt_num);
		return -EBUSY;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("calloc failed\n");
		return -ENOMEM;
	}

	ctx->scsi_tgt_num = scsi_tgt_num;
	ctx->async_fini = false;

	scsi_dev_state->remove_cb = cb_fn;
	scsi_dev_state->remove_ctx = cb_arg;
	scsi_dev_state->status = VHOST_SCSI_DEV_REMOVING;

	spdk_vhost_dev_foreach_session(vdev, spdk_vhost_scsi_session_remove_tgt, ctx);
	return 0;
}

int
spdk_vhost_scsi_controller_construct(void)
{
	struct spdk_conf_section *sp = spdk_conf_first_section(NULL);
	struct spdk_vhost_dev *vdev;
	int i, dev_num;
	unsigned ctrlr_num = 0;
	char *bdev_name, *tgt_num_str;
	char *cpumask;
	char *name;
	char *tgt = NULL;

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
		cpumask = spdk_conf_section_get_val(sp, "Cpumask");

		if (spdk_vhost_scsi_dev_construct(name, cpumask) < 0) {
			return -1;
		}

		vdev = spdk_vhost_dev_find(name);
		assert(vdev);

		for (i = 0; ; i++) {

			tgt = spdk_conf_section_get_nval(sp, "Target", i);
			if (tgt == NULL) {
				break;
			}

			tgt_num_str = spdk_conf_section_get_nmval(sp, "Target", i, 0);
			if (tgt_num_str == NULL) {
				SPDK_ERRLOG("%s: Invalid or missing target number\n", name);
				return -1;
			}

			dev_num = (int)strtol(tgt_num_str, NULL, 10);
			bdev_name = spdk_conf_section_get_nmval(sp, "Target", i, 1);
			if (bdev_name == NULL) {
				SPDK_ERRLOG("%s: Invalid or missing bdev name for target %d\n", name, dev_num);
				return -1;
			} else if (spdk_conf_section_get_nmval(sp, "Target", i, 2)) {
				SPDK_ERRLOG("%s: Only one LUN per vhost SCSI device supported\n", name);
				return -1;
			}

			if (spdk_vhost_scsi_dev_add_tgt(vdev, dev_num, bdev_name) < 0) {
				return -1;
			}
		}

		sp = spdk_conf_next_section(sp);
	}

	return 0;
}

static void
free_task_pool(struct spdk_vhost_scsi_session *svsession)
{
	struct spdk_vhost_session *vsession = &svsession->vsession;
	struct spdk_vhost_virtqueue *vq;
	uint16_t i;

	for (i = 0; i < vsession->max_queues; i++) {
		vq = &vsession->virtqueue[i];
		if (vq->tasks == NULL) {
			continue;
		}

		spdk_dma_free(vq->tasks);
		vq->tasks = NULL;
	}
}

static int
alloc_task_pool(struct spdk_vhost_scsi_session *svsession)
{
	struct spdk_vhost_session *vsession = &svsession->vsession;
	struct spdk_vhost_scsi_dev *svdev = svsession->svdev;
	struct spdk_vhost_virtqueue *vq;
	struct spdk_vhost_scsi_task *task;
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
			SPDK_ERRLOG("Controller %s: virtuque %"PRIu16" is too big. (size = %"PRIu32", max = %"PRIu32")\n",
				    svdev->vdev.name, i, task_cnt, SPDK_VHOST_MAX_VQ_SIZE);
			free_task_pool(svsession);
			return -1;
		}
		vq->tasks = spdk_zmalloc(sizeof(struct spdk_vhost_scsi_task) * task_cnt,
					 SPDK_CACHE_LINE_SIZE, NULL,
					 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (vq->tasks == NULL) {
			SPDK_ERRLOG("Controller %s: failed to allocate %"PRIu32" tasks for virtqueue %"PRIu16"\n",
				    svdev->vdev.name, task_cnt, i);
			free_task_pool(svsession);
			return -1;
		}

		for (j = 0; j < task_cnt; j++) {
			task = &((struct spdk_vhost_scsi_task *)vq->tasks)[j];
			task->svsession = svsession;
			task->vq = vq;
			task->req_idx = j;
		}
	}

	return 0;
}

static int
spdk_vhost_scsi_start_cb(struct spdk_vhost_dev *vdev,
			 struct spdk_vhost_session *vsession, void *unused)
{
	struct spdk_vhost_scsi_dev *svdev;
	struct spdk_vhost_scsi_session *svsession;
	struct spdk_scsi_dev_vhost_state *state;
	uint32_t i;
	int rc;

	svsession = to_scsi_session(vsession);
	assert(svsession != NULL);
	svdev = svsession->svdev;

	/* validate all I/O queues are in a contiguous index range */
	for (i = VIRTIO_SCSI_REQUESTQ; i < vsession->max_queues; i++) {
		if (vsession->virtqueue[i].vring.desc == NULL) {
			SPDK_ERRLOG("%s: queue %"PRIu32" is empty\n", vsession->vdev->name, i);
			rc = -1;
			goto out;
		}
	}

	rc = alloc_task_pool(svsession);
	if (rc != 0) {
		SPDK_ERRLOG("%s: failed to alloc task pool.\n", vdev->name);
		goto out;
	}

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; i++) {
		state = &svdev->scsi_dev_state[i];
		if (state->dev == NULL || state->status == VHOST_SCSI_DEV_REMOVING) {
			continue;
		}

		assert(state->dev != NULL);
		assert(svsession->scsi_dev_state[i].status == VHOST_SCSI_DEV_EMPTY);
		svsession->scsi_dev_state[i].dev = state->dev;
		svsession->scsi_dev_state[i].status = state->status;
		rc = spdk_scsi_dev_allocate_io_channels(state->dev);
		if (rc != 0) {
			SPDK_ERRLOG("%s: failed to alloc io_channel for SCSI target %"PRIu32"\n", vdev->name, i);
			/* unset the SCSI target so that all I/O to it will be rejected */
			svsession->scsi_dev_state[i].dev = NULL;
			/* set EMPTY state so that we won't reply with SCSI hotremove
			 * sense codes - the device hasn't ever been added.
			 */
			svsession->scsi_dev_state[i].status = VHOST_SCSI_DEV_EMPTY;
			continue;
		}
	}
	SPDK_INFOLOG(SPDK_LOG_VHOST, "Started poller for vhost controller %s on lcore %d\n",
		     vdev->name, spdk_env_get_current_core());

	svsession->requestq_poller = spdk_poller_register(vdev_worker, svsession, 0);
	if (vsession->virtqueue[VIRTIO_SCSI_CONTROLQ].vring.desc &&
	    vsession->virtqueue[VIRTIO_SCSI_EVENTQ].vring.desc) {
		svsession->mgmt_poller = spdk_poller_register(vdev_mgmt_worker, svsession,
					 MGMT_POLL_PERIOD_US);
	}
out:
	spdk_vhost_session_start_done(vsession, rc);
	return rc;
}

static int
spdk_vhost_scsi_start(struct spdk_vhost_session *vsession)
{
	struct spdk_vhost_scsi_session *svsession;
	struct spdk_vhost_scsi_dev *svdev;
	int rc;

	svsession = to_scsi_session(vsession);
	if (svsession == NULL) {
		SPDK_ERRLOG("Trying to start non-scsi session as a scsi one.\n");
		return -1;
	}

	svdev = to_scsi_dev(vsession->vdev);
	assert(svdev != NULL);
	svsession->svdev = svdev;

	if (svdev->vdev.active_session_num == 0) {
		svdev->lcore = spdk_vhost_allocate_reactor(svdev->vdev.cpumask);
	}

	rc = spdk_vhost_session_send_event(svdev->lcore, vsession, spdk_vhost_scsi_start_cb,
					   3, "start session");
	if (rc != 0) {
		if (svdev->vdev.active_session_num == 0) {
			spdk_vhost_free_reactor(svdev->lcore);
			svdev->lcore = -1;
		}
	}

	return rc;
}

static int
destroy_session_poller_cb(void *arg)
{
	struct spdk_vhost_scsi_session *svsession = arg;
	struct spdk_vhost_session *vsession = &svsession->vsession;
	struct spdk_scsi_dev_session_state *state;
	uint32_t i;

	if (vsession->task_cnt > 0) {
		return -1;
	}

	if (spdk_vhost_trylock() != 0) {
		return -1;
	}

	for (i = 0; i < vsession->max_queues; i++) {
		spdk_vhost_vq_used_signal(vsession, &vsession->virtqueue[i]);
	}

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; i++) {
		enum spdk_scsi_dev_vhost_status prev_status;

		state = &svsession->scsi_dev_state[i];
		/* clear the REMOVED status so that we won't send hotremove events anymore */
		prev_status = state->status;
		state->status = VHOST_SCSI_DEV_EMPTY;
		if (state->dev == NULL) {
			continue;
		}

		spdk_scsi_dev_free_io_channels(state->dev);

		state->dev = NULL;

		if (prev_status == VHOST_SCSI_DEV_REMOVING) {
			/* try to detach it globally */
			spdk_vhost_dev_foreach_session(vsession->vdev,
						       spdk_vhost_scsi_session_process_removed,
						       (void *)(uintptr_t)i);
		}
	}

	SPDK_INFOLOG(SPDK_LOG_VHOST, "Stopping poller for vhost controller %s\n",
		     svsession->svdev->vdev.name);

	free_task_pool(svsession);

	spdk_poller_unregister(&svsession->stop_poller);
	spdk_vhost_session_stop_done(vsession, 0);

	spdk_vhost_unlock();
	return -1;
}

static int
spdk_vhost_scsi_stop_cb(struct spdk_vhost_dev *vdev,
			struct spdk_vhost_session *vsession, void *unused)
{
	struct spdk_vhost_scsi_session *svsession;

	svsession = to_scsi_session(vsession);
	assert(svsession != NULL);

	/* Stop receiving new I/O requests */
	spdk_poller_unregister(&svsession->requestq_poller);

	/* Stop receiving controlq requests, also stop processing the
	 * asynchronous hotremove events. All the remaining events
	 * will be finalized by the stop_poller below.
	 */
	spdk_poller_unregister(&svsession->mgmt_poller);

	/* Wait for all pending I/Os to complete, then process all the
	 * remaining hotremove events one last time.
	 */
	svsession->stop_poller = spdk_poller_register(destroy_session_poller_cb,
				 svsession, 1000);

	return 0;
}

static int
spdk_vhost_scsi_stop(struct spdk_vhost_session *vsession)
{
	struct spdk_vhost_scsi_session *svsession;
	int rc;

	svsession = to_scsi_session(vsession);
	if (svsession == NULL) {
		SPDK_ERRLOG("Trying to stop non-scsi session as a scsi one.\n");
		return -1;
	}
	rc = spdk_vhost_session_send_event(vsession->lcore, vsession,
					   spdk_vhost_scsi_stop_cb, 3, "stop session");
	if (rc != 0) {
		return rc;
	}

	if (vsession->vdev->active_session_num == 0) {
		spdk_vhost_free_reactor(svsession->svdev->lcore);
		svsession->svdev->lcore = -1;
	}
	return 0;
}

static void
spdk_vhost_scsi_dump_info_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct spdk_scsi_dev *sdev;
	struct spdk_scsi_lun *lun;
	uint32_t dev_idx;
	uint32_t lun_idx;

	assert(vdev != NULL);
	spdk_json_write_named_array_begin(w, "scsi");
	for (dev_idx = 0; dev_idx < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; dev_idx++) {
		sdev = spdk_vhost_scsi_dev_get_tgt(vdev, dev_idx);
		if (!sdev) {
			continue;
		}

		spdk_json_write_object_begin(w);

		spdk_json_write_named_uint32(w, "scsi_dev_num", dev_idx);

		spdk_json_write_named_uint32(w, "id", spdk_scsi_dev_get_id(sdev));

		spdk_json_write_named_string(w, "target_name", spdk_scsi_dev_get_name(sdev));

		spdk_json_write_named_array_begin(w, "luns");

		for (lun_idx = 0; lun_idx < SPDK_SCSI_DEV_MAX_LUN; lun_idx++) {
			lun = spdk_scsi_dev_get_lun(sdev, lun_idx);
			if (!lun) {
				continue;
			}

			spdk_json_write_object_begin(w);

			spdk_json_write_named_int32(w, "id", spdk_scsi_lun_get_id(lun));

			spdk_json_write_named_string(w, "bdev_name", spdk_scsi_lun_get_bdev_name(lun));

			spdk_json_write_object_end(w);
		}

		spdk_json_write_array_end(w);
		spdk_json_write_object_end(w);
	}

	spdk_json_write_array_end(w);
}

static void
spdk_vhost_scsi_write_config_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct spdk_scsi_dev *scsi_dev;
	struct spdk_scsi_lun *lun;
	uint32_t i;

	if (to_scsi_dev(vdev) == NULL) {
		return;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "construct_vhost_scsi_controller");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "ctrlr", vdev->name);
	spdk_json_write_named_string(w, "cpumask", spdk_cpuset_fmt(vdev->cpumask));
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; i++) {
		scsi_dev = spdk_vhost_scsi_dev_get_tgt(vdev, i);
		if (scsi_dev == NULL) {
			continue;
		}

		lun = spdk_scsi_dev_get_lun(scsi_dev, 0);

		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "add_vhost_scsi_lun");

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "ctrlr", vdev->name);
		spdk_json_write_named_uint32(w, "scsi_target_num", i);

		spdk_json_write_named_string(w, "bdev_name", spdk_scsi_lun_get_bdev_name(lun));
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}
}

SPDK_LOG_REGISTER_COMPONENT("vhost_scsi", SPDK_LOG_VHOST_SCSI)
SPDK_LOG_REGISTER_COMPONENT("vhost_scsi_queue", SPDK_LOG_VHOST_SCSI_QUEUE)
SPDK_LOG_REGISTER_COMPONENT("vhost_scsi_data", SPDK_LOG_VHOST_SCSI_DATA)
