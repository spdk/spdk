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

/* Vhost-user-scsi support protocol features */
#define SPDK_VHOST_SCSI_PROTOCOL_FEATURES	(1ULL << VHOST_USER_PROTOCOL_F_INFLIGHT_SHMFD)

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
	int ref;
	bool registered;
	struct spdk_vhost_dev vdev;
	struct spdk_scsi_dev_vhost_state scsi_dev_state[SPDK_VHOST_SCSI_CTRLR_MAX_DEVS];
};

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

static int vhost_scsi_start(struct spdk_vhost_session *vsession);
static int vhost_scsi_stop(struct spdk_vhost_session *vsession);
static void vhost_scsi_dump_info_json(struct spdk_vhost_dev *vdev,
				      struct spdk_json_write_ctx *w);
static void vhost_scsi_write_config_json(struct spdk_vhost_dev *vdev,
		struct spdk_json_write_ctx *w);
static int vhost_scsi_dev_remove(struct spdk_vhost_dev *vdev);
static int vhost_scsi_dev_param_changed(struct spdk_vhost_dev *vdev,
					unsigned scsi_tgt_num);

static const struct spdk_vhost_dev_backend spdk_vhost_scsi_device_backend = {
	.session_ctx_size = sizeof(struct spdk_vhost_scsi_session) - sizeof(struct spdk_vhost_session),
	.start_session =  vhost_scsi_start,
	.stop_session = vhost_scsi_stop,
	.dump_info_json = vhost_scsi_dump_info_json,
	.write_config_json = vhost_scsi_write_config_json,
	.remove_device = vhost_scsi_dev_remove,
};

static inline void
scsi_task_init(struct spdk_vhost_scsi_task *task)
{
	memset(&task->scsi, 0, sizeof(task->scsi));
	/* Tmf_resp pointer and resp pointer are in a union.
	 * Here means task->tmf_resp = task->resp = NULL.
	 */
	task->resp = NULL;
	task->used = true;
	task->used_len = 0;
}

static void
vhost_scsi_task_put(struct spdk_vhost_scsi_task *task)
{
	spdk_scsi_task_put(&task->scsi);
}

static void
vhost_scsi_task_free_cb(struct spdk_scsi_task *scsi_task)
{
	struct spdk_vhost_scsi_task *task = SPDK_CONTAINEROF(scsi_task, struct spdk_vhost_scsi_task, scsi);
	struct spdk_vhost_session *vsession = &task->svsession->vsession;

	assert(vsession->task_cnt > 0);
	vsession->task_cnt--;
	task->used = false;
}

static void
remove_scsi_tgt(struct spdk_vhost_scsi_dev *svdev,
		unsigned scsi_tgt_num)
{
	struct spdk_scsi_dev_vhost_state *state;
	struct spdk_scsi_dev *dev;

	state = &svdev->scsi_dev_state[scsi_tgt_num];
	dev = state->dev;
	state->dev = NULL;
	assert(state->status == VHOST_SCSI_DEV_REMOVING);
	state->status = VHOST_SCSI_DEV_EMPTY;
	spdk_scsi_dev_destruct(dev, NULL, NULL);
	if (state->remove_cb) {
		state->remove_cb(&svdev->vdev, state->remove_ctx);
		state->remove_cb = NULL;
	}
	SPDK_INFOLOG(vhost, "%s: removed target 'Target %u'\n",
		     svdev->vdev.name, scsi_tgt_num);

	if (--svdev->ref == 0 && svdev->registered == false) {
		free(svdev);
	}
}

static void
vhost_scsi_dev_process_removed_cpl_cb(struct spdk_vhost_dev *vdev, void *ctx)
{
	unsigned scsi_tgt_num = (unsigned)(uintptr_t)ctx;
	struct spdk_vhost_scsi_dev *svdev = SPDK_CONTAINEROF(vdev,
					    struct spdk_vhost_scsi_dev, vdev);

	/* all sessions have already detached the device */
	if (svdev->scsi_dev_state[scsi_tgt_num].status != VHOST_SCSI_DEV_REMOVING) {
		/* device was already removed in the meantime */
		return;
	}

	remove_scsi_tgt(svdev, scsi_tgt_num);
}

static int
vhost_scsi_session_process_removed(struct spdk_vhost_dev *vdev,
				   struct spdk_vhost_session *vsession, void *ctx)
{
	unsigned scsi_tgt_num = (unsigned)(uintptr_t)ctx;
	struct spdk_vhost_scsi_session *svsession = (struct spdk_vhost_scsi_session *)vsession;
	struct spdk_scsi_dev_session_state *state = &svsession->scsi_dev_state[scsi_tgt_num];

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

		if (dev && state->status == VHOST_SCSI_DEV_REMOVING &&
		    !spdk_scsi_dev_has_pending_tasks(dev, NULL)) {
			/* detach the device from this session */
			spdk_scsi_dev_free_io_channels(dev);
			state->dev = NULL;
			state->status = VHOST_SCSI_DEV_REMOVED;
			/* try to detach it globally */
			spdk_vhost_lock();
			vhost_dev_foreach_session(&svsession->svdev->vdev,
						  vhost_scsi_session_process_removed,
						  vhost_scsi_dev_process_removed_cpl_cb,
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
	struct spdk_vhost_virtqueue *vq;
	struct vring_desc *desc, *desc_table;
	struct virtio_scsi_event *desc_ev;
	uint32_t desc_table_size, req_size = 0;
	uint16_t req;
	int rc;

	assert(scsi_dev_num < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS);
	vq = &vsession->virtqueue[VIRTIO_SCSI_EVENTQ];

	if (vq->vring.desc == NULL || vhost_vq_avail_ring_get(vq, &req, 1) != 1) {
		SPDK_ERRLOG("%s: failed to send virtio event (no avail ring entries?).\n",
			    vsession->name);
		return;
	}

	rc = vhost_vq_get_desc(vsession, vq, req, &desc, &desc_table, &desc_table_size);
	if (rc != 0 || desc->len < sizeof(*desc_ev)) {
		SPDK_ERRLOG("%s: invalid eventq descriptor at index %"PRIu16".\n",
			    vsession->name, req);
		goto out;
	}

	desc_ev = vhost_gpa_to_vva(vsession, desc->addr, sizeof(*desc_ev));
	if (desc_ev == NULL) {
		SPDK_ERRLOG("%s: eventq descriptor at index %"PRIu16" points "
			    "to unmapped guest memory address %p.\n",
			    vsession->name, req, (void *)(uintptr_t)desc->addr);
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
	vhost_vq_used_ring_enqueue(vsession, vq, req, req_size);
}

static void
submit_completion(struct spdk_vhost_scsi_task *task)
{
	struct spdk_vhost_session *vsession = &task->svsession->vsession;

	vhost_vq_used_ring_enqueue(vsession, task->vq, task->req_idx,
				   task->used_len);
	SPDK_DEBUGLOG(vhost_scsi, "Finished task (%p) req_idx=%d\n", task, task->req_idx);

	vhost_scsi_task_put(task);
}

static void
vhost_scsi_task_mgmt_cpl(struct spdk_scsi_task *scsi_task)
{
	struct spdk_vhost_scsi_task *task = SPDK_CONTAINEROF(scsi_task, struct spdk_vhost_scsi_task, scsi);

	submit_completion(task);
}

static void
vhost_scsi_task_cpl(struct spdk_scsi_task *scsi_task)
{
	struct spdk_vhost_scsi_task *task = SPDK_CONTAINEROF(scsi_task, struct spdk_vhost_scsi_task, scsi);

	/* The SCSI task has completed.  Do final processing and then post
	   notification to the virtqueue's "used" ring.
	 */
	task->resp->status = task->scsi.status;

	if (task->scsi.status != SPDK_SCSI_STATUS_GOOD) {
		memcpy(task->resp->sense, task->scsi.sense_data, task->scsi.sense_data_len);
		task->resp->sense_len = task->scsi.sense_data_len;
		SPDK_DEBUGLOG(vhost_scsi, "Task (%p) req_idx=%d failed - status=%u\n", task, task->req_idx,
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

	vhost_vq_used_ring_enqueue(vsession, task->vq, task->req_idx,
				   task->used_len);
	vhost_scsi_task_put(task);

	SPDK_DEBUGLOG(vhost_scsi, "Invalid request (status=%" PRIu8")\n",
		      task->resp ? task->resp->response : -1);
}

static int
vhost_scsi_task_init_target(struct spdk_vhost_scsi_task *task, const __u8 *lun)
{
	struct spdk_vhost_scsi_session *svsession = task->svsession;
	struct spdk_scsi_dev_session_state *state;
	uint16_t lun_id = (((uint16_t)lun[2] << 8) | lun[3]) & 0x3FFF;

	SPDK_LOGDUMP(vhost_scsi_queue, "LUN", lun, 8);

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
	struct vring_desc *desc, *desc_table;
	struct virtio_scsi_ctrl_tmf_req *ctrl_req;
	struct virtio_scsi_ctrl_an_resp *an_resp;
	uint32_t desc_table_size, used_len = 0;
	int rc;

	spdk_scsi_task_construct(&task->scsi, vhost_scsi_task_mgmt_cpl, vhost_scsi_task_free_cb);
	rc = vhost_vq_get_desc(vsession, task->vq, task->req_idx, &desc, &desc_table,
			       &desc_table_size);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("%s: invalid controlq descriptor at index %d.\n",
			    vsession->name, task->req_idx);
		goto out;
	}

	ctrl_req = vhost_gpa_to_vva(vsession, desc->addr, sizeof(*ctrl_req));
	if (ctrl_req == NULL) {
		SPDK_ERRLOG("%s: invalid task management request at index %d.\n",
			    vsession->name, task->req_idx);
		goto out;
	}

	SPDK_DEBUGLOG(vhost_scsi_queue,
		      "Processing controlq descriptor: desc %d/%p, desc_addr %p, len %d, flags %d, last_used_idx %d; kickfd %d; size %d\n",
		      task->req_idx, desc, (void *)desc->addr, desc->len, desc->flags, task->vq->last_used_idx,
		      task->vq->vring.kickfd, task->vq->vring.size);
	SPDK_LOGDUMP(vhost_scsi_queue, "Request descriptor", (uint8_t *)ctrl_req, desc->len);

	vhost_scsi_task_init_target(task, ctrl_req->lun);

	vhost_vring_desc_get_next(&desc, desc_table, desc_table_size);
	if (spdk_unlikely(desc == NULL)) {
		SPDK_ERRLOG("%s: no response descriptor for controlq request %d.\n",
			    vsession->name, task->req_idx);
		goto out;
	}

	/* Process the TMF request */
	switch (ctrl_req->type) {
	case VIRTIO_SCSI_T_TMF:
		task->tmf_resp = vhost_gpa_to_vva(vsession, desc->addr, sizeof(*task->tmf_resp));
		if (spdk_unlikely(desc->len < sizeof(struct virtio_scsi_ctrl_tmf_resp) || task->tmf_resp == NULL)) {
			SPDK_ERRLOG("%s: TMF response descriptor at index %d points to invalid guest memory region\n",
				    vsession->name, task->req_idx);
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
			SPDK_DEBUGLOG(vhost_scsi_queue, "%s: LUN reset\n", vsession->name);

			mgmt_task_submit(task, SPDK_SCSI_TASK_FUNC_LUN_RESET);
			return;
		default:
			task->tmf_resp->response = VIRTIO_SCSI_S_ABORTED;
			/* Unsupported command */
			SPDK_DEBUGLOG(vhost_scsi_queue, "%s: unsupported TMF command %x\n",
				      vsession->name, ctrl_req->subtype);
			break;
		}
		break;
	case VIRTIO_SCSI_T_AN_QUERY:
	case VIRTIO_SCSI_T_AN_SUBSCRIBE: {
		an_resp = vhost_gpa_to_vva(vsession, desc->addr, sizeof(*an_resp));
		if (spdk_unlikely(desc->len < sizeof(struct virtio_scsi_ctrl_an_resp) || an_resp == NULL)) {
			SPDK_WARNLOG("%s: asynchronous response descriptor points to invalid guest memory region\n",
				     vsession->name);
			goto out;
		}

		an_resp->response = VIRTIO_SCSI_S_ABORTED;
		break;
	}
	default:
		SPDK_DEBUGLOG(vhost_scsi_queue, "%s: Unsupported control command %x\n",
			      vsession->name, ctrl_req->type);
		break;
	}

	used_len = sizeof(struct virtio_scsi_ctrl_tmf_resp);
out:
	vhost_vq_used_ring_enqueue(vsession, task->vq, task->req_idx, used_len);
	vhost_scsi_task_put(task);
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
	struct vring_desc *desc, *desc_table;
	struct iovec *iovs = task->iovs;
	uint16_t iovcnt = 0;
	uint32_t desc_table_len, len = 0;
	int rc;

	spdk_scsi_task_construct(&task->scsi, vhost_scsi_task_cpl, vhost_scsi_task_free_cb);

	rc = vhost_vq_get_desc(vsession, task->vq, task->req_idx, &desc, &desc_table, &desc_table_len);
	/* First descriptor must be readable */
	if (spdk_unlikely(rc != 0  || vhost_vring_desc_is_wr(desc) ||
			  desc->len < sizeof(struct virtio_scsi_cmd_req))) {
		SPDK_WARNLOG("%s: invalid first request descriptor at index %"PRIu16".\n",
			     vsession->name, task->req_idx);
		goto invalid_task;
	}

	*req = vhost_gpa_to_vva(vsession, desc->addr, sizeof(**req));
	if (spdk_unlikely(*req == NULL)) {
		SPDK_WARNLOG("%s: request descriptor at index %d points to invalid guest memory region\n",
			     vsession->name, task->req_idx);
		goto invalid_task;
	}

	/* Each request must have at least 2 descriptors (e.g. request and response) */
	vhost_vring_desc_get_next(&desc, desc_table, desc_table_len);
	if (desc == NULL) {
		SPDK_WARNLOG("%s: descriptor chain at index %d contains neither payload nor response buffer.\n",
			     vsession->name, task->req_idx);
		goto invalid_task;
	}
	task->scsi.dxfer_dir = vhost_vring_desc_is_wr(desc) ? SPDK_SCSI_DIR_FROM_DEV :
			       SPDK_SCSI_DIR_TO_DEV;
	task->scsi.iovs = iovs;

	if (task->scsi.dxfer_dir == SPDK_SCSI_DIR_FROM_DEV) {
		/*
		 * FROM_DEV (READ): [RD_req][WR_resp][WR_buf0]...[WR_bufN]
		 */
		task->resp = vhost_gpa_to_vva(vsession, desc->addr, sizeof(*task->resp));
		if (spdk_unlikely(desc->len < sizeof(struct virtio_scsi_cmd_resp) || task->resp == NULL)) {
			SPDK_WARNLOG("%s: response descriptor at index %d points to invalid guest memory region\n",
				     vsession->name, task->req_idx);
			goto invalid_task;
		}
		rc = vhost_vring_desc_get_next(&desc, desc_table, desc_table_len);
		if (spdk_unlikely(rc != 0)) {
			SPDK_WARNLOG("%s: invalid descriptor chain at request index %d (descriptor id overflow?).\n",
				     vsession->name, task->req_idx);
			goto invalid_task;
		}

		if (desc == NULL) {
			/*
			 * TEST UNIT READY command and some others might not contain any payload and this is not an error.
			 */
			SPDK_DEBUGLOG(vhost_scsi_data,
				      "No payload descriptors for FROM DEV command req_idx=%"PRIu16".\n", task->req_idx);
			SPDK_LOGDUMP(vhost_scsi_data, "CDB=", (*req)->cdb, VIRTIO_SCSI_CDB_SIZE);
			task->used_len = sizeof(struct virtio_scsi_cmd_resp);
			task->scsi.iovcnt = 1;
			task->scsi.iovs[0].iov_len = 0;
			task->scsi.length = 0;
			task->scsi.transfer_len = 0;
			return 0;
		}

		/* All remaining descriptors are data. */
		while (desc) {
			if (spdk_unlikely(!vhost_vring_desc_is_wr(desc))) {
				SPDK_WARNLOG("%s: FROM DEV cmd: descriptor nr %" PRIu16" in payload chain is read only.\n",
					     vsession->name, iovcnt);
				goto invalid_task;
			}

			if (spdk_unlikely(vhost_vring_desc_to_iov(vsession, iovs, &iovcnt, desc))) {
				goto invalid_task;
			}
			len += desc->len;

			rc = vhost_vring_desc_get_next(&desc, desc_table, desc_table_len);
			if (spdk_unlikely(rc != 0)) {
				SPDK_WARNLOG("%s: invalid payload in descriptor chain starting at index %d.\n",
					     vsession->name, task->req_idx);
				goto invalid_task;
			}
		}

		task->used_len = sizeof(struct virtio_scsi_cmd_resp) + len;
	} else {
		SPDK_DEBUGLOG(vhost_scsi_data, "TO DEV");
		/*
		 * TO_DEV (WRITE):[RD_req][RD_buf0]...[RD_bufN][WR_resp]
		 * No need to check descriptor WR flag as this is done while setting scsi.dxfer_dir.
		 */

		/* Process descriptors up to response. */
		while (!vhost_vring_desc_is_wr(desc)) {
			if (spdk_unlikely(vhost_vring_desc_to_iov(vsession, iovs, &iovcnt, desc))) {
				goto invalid_task;
			}
			len += desc->len;

			vhost_vring_desc_get_next(&desc, desc_table, desc_table_len);
			if (spdk_unlikely(desc == NULL)) {
				SPDK_WARNLOG("%s: TO_DEV cmd: no response descriptor.\n", vsession->name);
				goto invalid_task;
			}
		}

		task->resp = vhost_gpa_to_vva(vsession, desc->addr, sizeof(*task->resp));
		if (spdk_unlikely(desc->len < sizeof(struct virtio_scsi_cmd_resp) || task->resp == NULL)) {
			SPDK_WARNLOG("%s: response descriptor at index %d points to invalid guest memory region\n",
				     vsession->name, task->req_idx);
			goto invalid_task;
		}

		task->used_len = sizeof(struct virtio_scsi_cmd_resp);
	}

	task->scsi.iovcnt = iovcnt;
	task->scsi.length = len;
	task->scsi.transfer_len = len;
	return 0;

invalid_task:
	SPDK_DEBUGLOG(vhost_scsi_data, "%s: Invalid task at index %"PRIu16".\n",
		      vsession->name, task->req_idx);
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

	result = vhost_scsi_task_init_target(task, req->lun);
	if (spdk_unlikely(result != 0)) {
		task->resp->response = VIRTIO_SCSI_S_BAD_TARGET;
		return -1;
	}

	task->scsi.cdb = req->cdb;
	SPDK_LOGDUMP(vhost_scsi_data, "request CDB", req->cdb, VIRTIO_SCSI_CDB_SIZE);

	if (spdk_unlikely(task->scsi.lun == NULL)) {
		spdk_scsi_task_process_null_lun(&task->scsi);
		task->resp->response = VIRTIO_SCSI_S_OK;
		return 1;
	}

	return 0;
}

static void
process_scsi_task(struct spdk_vhost_session *vsession,
		  struct spdk_vhost_virtqueue *vq,
		  uint16_t req_idx)
{
	struct spdk_vhost_scsi_task *task;
	int result;

	task = &((struct spdk_vhost_scsi_task *)vq->tasks)[req_idx];
	if (spdk_unlikely(task->used)) {
		SPDK_ERRLOG("%s: request with idx '%"PRIu16"' is already pending.\n",
			    vsession->name, req_idx);
		vhost_vq_used_ring_enqueue(vsession, vq, req_idx, 0);
		return;
	}

	vsession->task_cnt++;
	scsi_task_init(task);

	if (spdk_unlikely(vq->vring_idx == VIRTIO_SCSI_CONTROLQ)) {
		process_ctrl_request(task);
	} else {
		result = process_request(task);
		if (likely(result == 0)) {
			task_submit(task);
			SPDK_DEBUGLOG(vhost_scsi, "====== Task %p req_idx %d submitted ======\n", task,
				      task->req_idx);
		} else if (result > 0) {
			vhost_scsi_task_cpl(&task->scsi);
			SPDK_DEBUGLOG(vhost_scsi, "====== Task %p req_idx %d finished early ======\n", task,
				      task->req_idx);
		} else {
			invalid_request(task);
			SPDK_DEBUGLOG(vhost_scsi, "====== Task %p req_idx %d failed ======\n", task,
				      task->req_idx);
		}
	}
}

static void
submit_inflight_desc(struct spdk_vhost_scsi_session *svsession,
		     struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_session *vsession = &svsession->vsession;
	spdk_vhost_resubmit_info *resubmit = vq->vring_inflight.resubmit_inflight;
	spdk_vhost_resubmit_desc *resubmit_list;
	uint16_t req_idx;

	if (spdk_likely(resubmit == NULL || resubmit->resubmit_list == NULL)) {
		return;
	}

	resubmit_list = resubmit->resubmit_list;
	while (resubmit->resubmit_num-- > 0) {
		req_idx = resubmit_list[resubmit->resubmit_num].index;
		SPDK_DEBUGLOG(vhost_scsi, "====== Start processing request idx %"PRIu16"======\n",
			      req_idx);

		if (spdk_unlikely(req_idx >= vq->vring.size)) {
			SPDK_ERRLOG("%s: request idx '%"PRIu16"' exceeds virtqueue size (%"PRIu16").\n",
				    vsession->name, req_idx, vq->vring.size);
			vhost_vq_used_ring_enqueue(vsession, vq, req_idx, 0);
			continue;
		}

		process_scsi_task(vsession, vq, req_idx);
	}
	/* reset the submit_num to 0 to avoid underflow. */
	resubmit->resubmit_num = 0;

	free(resubmit_list);
	resubmit->resubmit_list = NULL;
}

static void
process_vq(struct spdk_vhost_scsi_session *svsession, struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_session *vsession = &svsession->vsession;
	uint16_t reqs[32];
	uint16_t reqs_cnt, i;

	submit_inflight_desc(svsession, vq);

	reqs_cnt = vhost_vq_avail_ring_get(vq, reqs, SPDK_COUNTOF(reqs));
	assert(reqs_cnt <= 32);

	for (i = 0; i < reqs_cnt; i++) {
		SPDK_DEBUGLOG(vhost_scsi, "====== Starting processing request idx %"PRIu16"======\n",
			      reqs[i]);

		if (spdk_unlikely(reqs[i] >= vq->vring.size)) {
			SPDK_ERRLOG("%s: request idx '%"PRIu16"' exceeds virtqueue size (%"PRIu16").\n",
				    vsession->name, reqs[i], vq->vring.size);
			vhost_vq_used_ring_enqueue(vsession, vq, reqs[i], 0);
			continue;
		}

		rte_vhost_set_inflight_desc_split(vsession->vid, vq->vring_idx, reqs[i]);

		process_scsi_task(vsession, vq, reqs[i]);
	}
}

static int
vdev_mgmt_worker(void *arg)
{
	struct spdk_vhost_scsi_session *svsession = arg;
	struct spdk_vhost_session *vsession = &svsession->vsession;

	process_removed_devs(svsession);

	if (vsession->virtqueue[VIRTIO_SCSI_EVENTQ].vring.desc) {
		vhost_vq_used_signal(vsession, &vsession->virtqueue[VIRTIO_SCSI_EVENTQ]);
	}

	if (vsession->virtqueue[VIRTIO_SCSI_CONTROLQ].vring.desc) {
		process_vq(svsession, &vsession->virtqueue[VIRTIO_SCSI_CONTROLQ]);
		vhost_vq_used_signal(vsession, &vsession->virtqueue[VIRTIO_SCSI_CONTROLQ]);
	}

	return SPDK_POLLER_BUSY;
}

static int
vdev_worker(void *arg)
{
	struct spdk_vhost_scsi_session *svsession = arg;
	struct spdk_vhost_session *vsession = &svsession->vsession;
	uint32_t q_idx;

	for (q_idx = VIRTIO_SCSI_REQUESTQ; q_idx < vsession->max_queues; q_idx++) {
		process_vq(svsession, &vsession->virtqueue[q_idx]);
	}

	vhost_session_used_signal(vsession);

	return SPDK_POLLER_BUSY;
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
	assert(vsession->vdev->backend == &spdk_vhost_scsi_device_backend);
	return (struct spdk_vhost_scsi_session *)vsession;
}

int
spdk_vhost_scsi_dev_construct(const char *name, const char *cpumask)
{
	struct spdk_vhost_scsi_dev *svdev = calloc(1, sizeof(*svdev));
	int rc;

	if (svdev == NULL) {
		return -ENOMEM;
	}

	svdev->vdev.virtio_features = SPDK_VHOST_SCSI_FEATURES;
	svdev->vdev.disabled_features = SPDK_VHOST_SCSI_DISABLED_FEATURES;
	svdev->vdev.protocol_features = SPDK_VHOST_SCSI_PROTOCOL_FEATURES;

	spdk_vhost_lock();
	rc = vhost_dev_register(&svdev->vdev, name, cpumask,
				&spdk_vhost_scsi_device_backend);

	if (rc) {
		free(svdev);
		spdk_vhost_unlock();
		return rc;
	}

	svdev->registered = true;

	spdk_vhost_unlock();
	return rc;
}

static int
vhost_scsi_dev_remove(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_scsi_dev *svdev = to_scsi_dev(vdev);
	int rc, i;

	assert(svdev != NULL);
	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; ++i) {
		if (svdev->scsi_dev_state[i].dev) {
			if (vdev->registered) {
				SPDK_ERRLOG("%s: SCSI target %d is still present.\n", vdev->name, i);
				return -EBUSY;
			}

			rc = spdk_vhost_scsi_dev_remove_tgt(vdev, i, NULL, NULL);
			if (rc != 0) {
				SPDK_ERRLOG("%s: failed to force-remove target %d\n", vdev->name, i);
				return rc;
			}
		}
	}

	rc = vhost_dev_unregister(vdev);
	if (rc != 0) {
		return rc;
	}
	svdev->registered = false;

	if (svdev->ref == 0) {
		free(svdev);
	}

	return 0;
}

struct spdk_scsi_dev *
spdk_vhost_scsi_dev_get_tgt(struct spdk_vhost_dev *vdev, uint8_t num)
{
	struct spdk_vhost_scsi_dev *svdev;

	assert(num < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS);
	svdev = to_scsi_dev(vdev);
	assert(svdev != NULL);
	if (svdev->scsi_dev_state[num].status != VHOST_SCSI_DEV_PRESENT) {
		return NULL;
	}

	assert(svdev->scsi_dev_state[num].dev != NULL);
	return svdev->scsi_dev_state[num].dev;
}

static unsigned
get_scsi_dev_num(const struct spdk_vhost_scsi_dev *svdev,
		 const struct spdk_scsi_lun *lun)
{
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

	return scsi_dev_num;
}

static void
vhost_scsi_lun_resize(const struct spdk_scsi_lun *lun, void *arg)
{
	struct spdk_vhost_scsi_dev *svdev = arg;
	unsigned scsi_dev_num;

	scsi_dev_num = get_scsi_dev_num(svdev, lun);
	if (scsi_dev_num == SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
		/* The entire device has been already removed. */
		return;
	}

	vhost_scsi_dev_param_changed(&svdev->vdev, scsi_dev_num);
}

static void
vhost_scsi_lun_hotremove(const struct spdk_scsi_lun *lun, void *arg)
{
	struct spdk_vhost_scsi_dev *svdev = arg;
	unsigned scsi_dev_num;

	scsi_dev_num = get_scsi_dev_num(svdev, lun);
	if (scsi_dev_num == SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
		/* The entire device has been already removed. */
		return;
	}

	/* remove entire device */
	spdk_vhost_scsi_dev_remove_tgt(&svdev->vdev, scsi_dev_num, NULL, NULL);
}

static void
vhost_scsi_dev_add_tgt_cpl_cb(struct spdk_vhost_dev *vdev, void *ctx)
{
	unsigned scsi_tgt_num = (unsigned)(uintptr_t)ctx;
	struct spdk_vhost_scsi_dev *svdev = SPDK_CONTAINEROF(vdev,
					    struct spdk_vhost_scsi_dev, vdev);
	struct spdk_scsi_dev_vhost_state *vhost_sdev;

	vhost_sdev = &svdev->scsi_dev_state[scsi_tgt_num];

	/* All sessions have added the target */
	assert(vhost_sdev->status == VHOST_SCSI_DEV_ADDING);
	vhost_sdev->status = VHOST_SCSI_DEV_PRESENT;
	svdev->ref++;
}

static int
vhost_scsi_session_add_tgt(struct spdk_vhost_dev *vdev,
			   struct spdk_vhost_session *vsession, void *ctx)
{
	unsigned scsi_tgt_num = (unsigned)(uintptr_t)ctx;
	struct spdk_vhost_scsi_session *svsession = (struct spdk_vhost_scsi_session *)vsession;
	struct spdk_scsi_dev_session_state *session_sdev = &svsession->scsi_dev_state[scsi_tgt_num];
	struct spdk_scsi_dev_vhost_state *vhost_sdev;
	int rc;

	if (!vsession->started || session_sdev->dev != NULL) {
		/* Nothing to do. */
		return 0;
	}

	vhost_sdev = &svsession->svdev->scsi_dev_state[scsi_tgt_num];
	session_sdev->dev = vhost_sdev->dev;
	session_sdev->status = VHOST_SCSI_DEV_PRESENT;

	rc = spdk_scsi_dev_allocate_io_channels(svsession->scsi_dev_state[scsi_tgt_num].dev);
	if (rc != 0) {
		SPDK_ERRLOG("%s: Couldn't allocate io channnel for SCSI target %u.\n",
			    vsession->name, scsi_tgt_num);

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

	if (vhost_dev_has_feature(vsession, VIRTIO_SCSI_F_HOTPLUG)) {
		eventq_enqueue(svsession, scsi_tgt_num,
			       VIRTIO_SCSI_T_TRANSPORT_RESET, VIRTIO_SCSI_EVT_RESET_RESCAN);
	} else {
		SPDK_NOTICELOG("%s: driver does not support hotplug. "
			       "Please restart it or perform a rescan.\n",
			       vsession->name);
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
	if (!svdev) {
		SPDK_ERRLOG("Before adding a SCSI target, there should be a SCSI device.");
		return -EINVAL;
	}

	if (scsi_tgt_num < 0) {
		for (scsi_tgt_num = 0; scsi_tgt_num < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; scsi_tgt_num++) {
			if (svdev->scsi_dev_state[scsi_tgt_num].dev == NULL) {
				break;
			}
		}

		if (scsi_tgt_num == SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
			SPDK_ERRLOG("%s: all SCSI target slots are already in use.\n", vdev->name);
			return -ENOSPC;
		}
	} else {
		if (scsi_tgt_num >= SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
			SPDK_ERRLOG("%s: SCSI target number is too big (got %d, max %d)\n",
				    vdev->name, scsi_tgt_num, SPDK_VHOST_SCSI_CTRLR_MAX_DEVS);
			return -EINVAL;
		}
	}

	if (bdev_name == NULL) {
		SPDK_ERRLOG("No lun name specified\n");
		return -EINVAL;
	}

	state = &svdev->scsi_dev_state[scsi_tgt_num];
	if (state->dev != NULL) {
		SPDK_ERRLOG("%s: SCSI target %u already occupied\n", vdev->name, scsi_tgt_num);
		return -EEXIST;
	}

	/*
	 * At this stage only one LUN per target
	 */
	snprintf(target_name, sizeof(target_name), "Target %u", scsi_tgt_num);
	lun_id_list[0] = 0;
	bdev_names_list[0] = (char *)bdev_name;

	state->status = VHOST_SCSI_DEV_ADDING;
	state->dev = spdk_scsi_dev_construct_ext(target_name, bdev_names_list, lun_id_list, 1,
			SPDK_SPC_PROTOCOL_IDENTIFIER_SAS,
			vhost_scsi_lun_resize, svdev,
			vhost_scsi_lun_hotremove, svdev);

	if (state->dev == NULL) {
		state->status = VHOST_SCSI_DEV_EMPTY;
		SPDK_ERRLOG("%s: couldn't create SCSI target %u using bdev '%s'\n",
			    vdev->name, scsi_tgt_num, bdev_name);
		return -EINVAL;
	}
	spdk_scsi_dev_add_port(state->dev, 0, "vhost");

	SPDK_INFOLOG(vhost, "%s: added SCSI target %u using bdev '%s'\n",
		     vdev->name, scsi_tgt_num, bdev_name);

	vhost_dev_foreach_session(vdev, vhost_scsi_session_add_tgt,
				  vhost_scsi_dev_add_tgt_cpl_cb,
				  (void *)(uintptr_t)scsi_tgt_num);
	return scsi_tgt_num;
}

struct scsi_tgt_hotplug_ctx {
	unsigned scsi_tgt_num;
	bool async_fini;
};

static void
vhost_scsi_dev_remove_tgt_cpl_cb(struct spdk_vhost_dev *vdev, void *_ctx)
{
	struct scsi_tgt_hotplug_ctx *ctx = _ctx;
	struct spdk_vhost_scsi_dev *svdev = SPDK_CONTAINEROF(vdev,
					    struct spdk_vhost_scsi_dev, vdev);

	if (!ctx->async_fini) {
		/* there aren't any active sessions, so remove the dev and exit */
		remove_scsi_tgt(svdev, ctx->scsi_tgt_num);
	}

	free(ctx);
}

static int
vhost_scsi_session_remove_tgt(struct spdk_vhost_dev *vdev,
			      struct spdk_vhost_session *vsession, void *_ctx)
{
	struct scsi_tgt_hotplug_ctx *ctx = _ctx;
	unsigned scsi_tgt_num = ctx->scsi_tgt_num;
	struct spdk_vhost_scsi_session *svsession = (struct spdk_vhost_scsi_session *)vsession;
	struct spdk_scsi_dev_session_state *state = &svsession->scsi_dev_state[scsi_tgt_num];

	if (!vsession->started || state->dev == NULL) {
		/* Nothing to do */
		return 0;
	}

	/* Mark the target for removal */
	assert(state->status == VHOST_SCSI_DEV_PRESENT);
	state->status = VHOST_SCSI_DEV_REMOVING;

	/* Send a hotremove virtio event */
	if (vhost_dev_has_feature(vsession, VIRTIO_SCSI_F_HOTPLUG)) {
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
		SPDK_ERRLOG("%s: invalid SCSI target number %d\n", vdev->name, scsi_tgt_num);
		return -EINVAL;
	}

	svdev = to_scsi_dev(vdev);
	if (!svdev) {
		SPDK_ERRLOG("An invalid SCSI device that removing from a SCSI target.");
		return -EINVAL;
	}

	scsi_dev_state = &svdev->scsi_dev_state[scsi_tgt_num];

	if (scsi_dev_state->status != VHOST_SCSI_DEV_PRESENT) {
		return -EBUSY;
	}

	if (scsi_dev_state->dev == NULL || scsi_dev_state->status == VHOST_SCSI_DEV_ADDING) {
		SPDK_ERRLOG("%s: SCSI target %u is not occupied\n", vdev->name, scsi_tgt_num);
		return -ENODEV;
	}

	assert(scsi_dev_state->status != VHOST_SCSI_DEV_EMPTY);
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

	vhost_dev_foreach_session(vdev, vhost_scsi_session_remove_tgt,
				  vhost_scsi_dev_remove_tgt_cpl_cb, ctx);
	return 0;
}

static int
vhost_scsi_session_param_changed(struct spdk_vhost_dev *vdev,
				 struct spdk_vhost_session *vsession, void *ctx)
{
	unsigned scsi_tgt_num = (unsigned)(uintptr_t)ctx;
	struct spdk_vhost_scsi_session *svsession = (struct spdk_vhost_scsi_session *)vsession;
	struct spdk_scsi_dev_session_state *state = &svsession->scsi_dev_state[scsi_tgt_num];

	if (!vsession->started || state->dev == NULL) {
		/* Nothing to do */
		return 0;
	}

	/* Send a parameter change virtio event */
	if (vhost_dev_has_feature(vsession, VIRTIO_SCSI_F_CHANGE)) {
		/*
		 * virtio 1.0 spec says:
		 * By sending this event, the device signals a change in the configuration
		 * parameters of a logical unit, for example the capacity or cache mode.
		 * event is set to VIRTIO_SCSI_T_PARAM_CHANGE. lun addresses a logical unit
		 * in the SCSI host. The same event SHOULD also be reported as a unit
		 * attention condition. reason contains the additional sense code and
		 * additional sense code qualifier, respectively in bits 0…7 and 8…15.
		 * Note: For example, a change in * capacity will be reported as asc
		 * 0x2a, ascq 0x09 (CAPACITY DATA HAS CHANGED).
		 */
		eventq_enqueue(svsession, scsi_tgt_num, VIRTIO_SCSI_T_PARAM_CHANGE, 0x2a | (0x09 << 8));
	}

	return 0;
}

static int
vhost_scsi_dev_param_changed(struct spdk_vhost_dev *vdev, unsigned scsi_tgt_num)
{
	struct spdk_vhost_scsi_dev *svdev;
	struct spdk_scsi_dev_vhost_state *scsi_dev_state;

	if (scsi_tgt_num >= SPDK_VHOST_SCSI_CTRLR_MAX_DEVS) {
		SPDK_ERRLOG("%s: invalid SCSI target number %d\n", vdev->name, scsi_tgt_num);
		return -EINVAL;
	}

	svdev = to_scsi_dev(vdev);
	if (!svdev) {
		SPDK_ERRLOG("An invalid SCSI device that removing from a SCSI target.");
		return -EINVAL;
	}

	scsi_dev_state = &svdev->scsi_dev_state[scsi_tgt_num];

	if (scsi_dev_state->status != VHOST_SCSI_DEV_PRESENT) {
		return -EBUSY;
	}

	if (scsi_dev_state->dev == NULL || scsi_dev_state->status == VHOST_SCSI_DEV_ADDING) {
		SPDK_ERRLOG("%s: SCSI target %u is not occupied\n", vdev->name, scsi_tgt_num);
		return -ENODEV;
	}

	assert(scsi_dev_state->status != VHOST_SCSI_DEV_EMPTY);

	vhost_dev_foreach_session(vdev, vhost_scsi_session_param_changed,
				  NULL, (void *)(uintptr_t)scsi_tgt_num);
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

		spdk_free(vq->tasks);
		vq->tasks = NULL;
	}
}

static int
alloc_task_pool(struct spdk_vhost_scsi_session *svsession)
{
	struct spdk_vhost_session *vsession = &svsession->vsession;
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
			SPDK_ERRLOG("%s: virtuque %"PRIu16" is too big. (size = %"PRIu32", max = %"PRIu32")\n",
				    vsession->name, i, task_cnt, SPDK_VHOST_MAX_VQ_SIZE);
			free_task_pool(svsession);
			return -1;
		}
		vq->tasks = spdk_zmalloc(sizeof(struct spdk_vhost_scsi_task) * task_cnt,
					 SPDK_CACHE_LINE_SIZE, NULL,
					 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (vq->tasks == NULL) {
			SPDK_ERRLOG("%s: failed to allocate %"PRIu32" tasks for virtqueue %"PRIu16"\n",
				    vsession->name, task_cnt, i);
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
vhost_scsi_start_cb(struct spdk_vhost_dev *vdev,
		    struct spdk_vhost_session *vsession, void *unused)
{
	struct spdk_vhost_scsi_session *svsession = to_scsi_session(vsession);
	struct spdk_vhost_scsi_dev *svdev = svsession->svdev;
	struct spdk_scsi_dev_vhost_state *state;
	uint32_t i;
	int rc;

	/* validate all I/O queues are in a contiguous index range */
	for (i = VIRTIO_SCSI_REQUESTQ; i < vsession->max_queues; i++) {
		if (vsession->virtqueue[i].vring.desc == NULL) {
			SPDK_ERRLOG("%s: queue %"PRIu32" is empty\n", vsession->name, i);
			rc = -1;
			goto out;
		}
	}

	rc = alloc_task_pool(svsession);
	if (rc != 0) {
		SPDK_ERRLOG("%s: failed to alloc task pool.\n", vsession->name);
		goto out;
	}

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; i++) {
		state = &svdev->scsi_dev_state[i];
		if (state->dev == NULL || state->status == VHOST_SCSI_DEV_REMOVING) {
			continue;
		}

		assert(svsession->scsi_dev_state[i].status == VHOST_SCSI_DEV_EMPTY);
		svsession->scsi_dev_state[i].dev = state->dev;
		svsession->scsi_dev_state[i].status = VHOST_SCSI_DEV_PRESENT;
		rc = spdk_scsi_dev_allocate_io_channels(state->dev);
		if (rc != 0) {
			SPDK_ERRLOG("%s: failed to alloc io_channel for SCSI target %"PRIu32"\n",
				    vsession->name, i);
			/* unset the SCSI target so that all I/O to it will be rejected */
			svsession->scsi_dev_state[i].dev = NULL;
			/* set EMPTY state so that we won't reply with SCSI hotremove
			 * sense codes - the device hasn't ever been added.
			 */
			svsession->scsi_dev_state[i].status = VHOST_SCSI_DEV_EMPTY;
			continue;
		}
	}
	SPDK_INFOLOG(vhost, "%s: started poller on lcore %d\n",
		     vsession->name, spdk_env_get_current_core());

	svsession->requestq_poller = SPDK_POLLER_REGISTER(vdev_worker, svsession, 0);
	svsession->mgmt_poller = SPDK_POLLER_REGISTER(vdev_mgmt_worker, svsession,
				 MGMT_POLL_PERIOD_US);
out:
	vhost_session_start_done(vsession, rc);
	return rc;
}

static int
vhost_scsi_start(struct spdk_vhost_session *vsession)
{
	struct spdk_vhost_scsi_session *svsession = to_scsi_session(vsession);
	struct spdk_vhost_scsi_dev *svdev;

	svdev = to_scsi_dev(vsession->vdev);
	assert(svdev != NULL);
	svsession->svdev = svdev;

	return vhost_session_send_event(vsession, vhost_scsi_start_cb,
					3, "start session");
}

static int
destroy_session_poller_cb(void *arg)
{
	struct spdk_vhost_scsi_session *svsession = arg;
	struct spdk_vhost_session *vsession = &svsession->vsession;
	struct spdk_scsi_dev_session_state *state;
	uint32_t i;

	if (vsession->task_cnt > 0) {
		return SPDK_POLLER_BUSY;
	}

	if (spdk_vhost_trylock() != 0) {
		return SPDK_POLLER_BUSY;
	}

	for (i = 0; i < vsession->max_queues; i++) {
		vhost_vq_used_signal(vsession, &vsession->virtqueue[i]);
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
			vhost_dev_foreach_session(vsession->vdev,
						  vhost_scsi_session_process_removed,
						  vhost_scsi_dev_process_removed_cpl_cb,
						  (void *)(uintptr_t)i);
		}
	}

	SPDK_INFOLOG(vhost, "%s: stopping poller on lcore %d\n",
		     vsession->name, spdk_env_get_current_core());

	free_task_pool(svsession);

	spdk_poller_unregister(&svsession->stop_poller);
	vhost_session_stop_done(vsession, 0);

	spdk_vhost_unlock();
	return SPDK_POLLER_BUSY;
}

static int
vhost_scsi_stop_cb(struct spdk_vhost_dev *vdev,
		   struct spdk_vhost_session *vsession, void *unused)
{
	struct spdk_vhost_scsi_session *svsession = to_scsi_session(vsession);

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
	svsession->stop_poller = SPDK_POLLER_REGISTER(destroy_session_poller_cb,
				 svsession, 1000);

	return 0;
}

static int
vhost_scsi_stop(struct spdk_vhost_session *vsession)
{
	return vhost_session_send_event(vsession, vhost_scsi_stop_cb,
					3, "stop session");
}

static void
vhost_scsi_dump_info_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
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
vhost_scsi_write_config_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct spdk_scsi_dev *scsi_dev;
	struct spdk_scsi_lun *lun;
	uint32_t i;

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "vhost_create_scsi_controller");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "ctrlr", vdev->name);
	spdk_json_write_named_string(w, "cpumask",
				     spdk_cpuset_fmt(spdk_thread_get_cpumask(vdev->thread)));
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);

	for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; i++) {
		scsi_dev = spdk_vhost_scsi_dev_get_tgt(vdev, i);
		if (scsi_dev == NULL) {
			continue;
		}

		lun = spdk_scsi_dev_get_lun(scsi_dev, 0);
		assert(lun != NULL);

		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "vhost_scsi_controller_add_target");

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "ctrlr", vdev->name);
		spdk_json_write_named_uint32(w, "scsi_target_num", i);

		spdk_json_write_named_string(w, "bdev_name", spdk_scsi_lun_get_bdev_name(lun));
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}
}

SPDK_LOG_REGISTER_COMPONENT(vhost_scsi)
SPDK_LOG_REGISTER_COMPONENT(vhost_scsi_queue)
SPDK_LOG_REGISTER_COMPONENT(vhost_scsi_data)
