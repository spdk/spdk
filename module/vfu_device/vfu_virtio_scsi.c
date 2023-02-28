/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * virtio-scsi over vfio-user transport
 */
#include <linux/virtio_scsi.h>

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/assert.h"
#include "spdk/barrier.h"
#include "spdk/thread.h"
#include "spdk/memory.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk/scsi.h"
#include "spdk/scsi_spec.h"
#include "spdk/pci_ids.h"

#include "vfu_virtio_internal.h"

#define VIRTIO_SCSI_SUPPORTED_FEATURES	((1ULL << VIRTIO_SCSI_F_INOUT) | \
					 (1ULL << VIRTIO_SCSI_F_HOTPLUG) | \
					 (1ULL << VIRTIO_SCSI_F_CHANGE))

#define VIRTIO_SCSI_CTRLR_MAX_TARGETS	(8)

struct virtio_scsi_target {
	struct spdk_scsi_dev		*dev;
};

struct virtio_scsi_endpoint {
	struct vfu_virtio_endpoint	virtio;

	struct virtio_scsi_config	scsi_cfg;
	/* virtio_scsi specific configurations */
	struct virtio_scsi_target	targets[VIRTIO_SCSI_CTRLR_MAX_TARGETS];
	/* virtio_scsi SCSI task and IO ring process poller */
	struct spdk_poller		*ring_poller;
};

struct virtio_scsi_req {
	struct spdk_scsi_task scsi;
	union {
		struct virtio_scsi_cmd_req *cmd_req;
		struct virtio_scsi_ctrl_tmf_req *tmf_req;
	};
	union {
		struct virtio_scsi_cmd_resp *cmd_resp;
		struct virtio_scsi_ctrl_tmf_resp *tmf_resp;
	};
	struct virtio_scsi_endpoint *endpoint;
	/* KEEP req at last */
	struct vfu_virtio_req req;
};

static inline struct virtio_scsi_endpoint *
to_scsi_endpoint(struct vfu_virtio_endpoint *virtio_endpoint)
{
	return SPDK_CONTAINEROF(virtio_endpoint, struct virtio_scsi_endpoint, virtio);
}

static inline struct virtio_scsi_req *
to_scsi_request(struct vfu_virtio_req *request)
{
	return SPDK_CONTAINEROF(request, struct virtio_scsi_req, req);
}

static void
virtio_scsi_req_finish(struct virtio_scsi_req *scsi_req)
{
	struct vfu_virtio_req *req = &scsi_req->req;

	vfu_virtio_finish_req(req);
}

static int
vfu_virtio_scsi_vring_poll(void *ctx)
{
	struct virtio_scsi_endpoint *scsi_endpoint = ctx;
	struct vfu_virtio_dev *dev = scsi_endpoint->virtio.dev;
	struct vfu_virtio_vq *vq;
	uint32_t i, count = 0;

	if (spdk_unlikely(!virtio_dev_is_started(dev))) {
		return SPDK_POLLER_IDLE;
	}

	if (spdk_unlikely(scsi_endpoint->virtio.quiesce_in_progress)) {
		return SPDK_POLLER_IDLE;
	}

	/* We don't process event queue here */
	for (i = 0; i < dev->num_queues; i++) {
		if (i == 1) {
			continue;
		}

		vq = &dev->vqs[i];
		if (!vq->enabled || vq->q_state != VFU_VQ_ACTIVE) {
			continue;
		}

		vfu_virtio_vq_flush_irq(dev, vq);

		if (vq->packed.packed_ring) {
			/* packed vring */
			count += vfu_virito_dev_process_packed_ring(dev, vq);
		} else {
			/* split vring */
			count += vfu_virito_dev_process_split_ring(dev, vq);
		}
	}

	return count ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static void
vfu_virtio_scsi_eventq_enqueue(struct virtio_scsi_endpoint *scsi_endpoint, uint8_t scsi_target_num,
			       uint32_t event, uint32_t reason)
{
	struct vfu_virtio_dev *dev = scsi_endpoint->virtio.dev;
	struct vfu_virtio_req *req = NULL;
	struct virtio_scsi_req *scsi_req;
	struct virtio_scsi_event *desc_ev;
	struct vfu_virtio_vq *vq;

	assert(dev != NULL);

	if (scsi_target_num >= VIRTIO_SCSI_CTRLR_MAX_TARGETS) {
		return;
	}

	if (spdk_unlikely(scsi_endpoint->virtio.quiesce_in_progress)) {
		return;
	}

	/* event queue */
	vq = &dev->vqs[1];
	if (!vq->enabled || vq->q_state != VFU_VQ_ACTIVE) {
		return;
	}

	if (vq->packed.packed_ring) {
		/* packed vring */
		req = virito_dev_packed_ring_get_next_avail_req(dev, vq);
	} else {
		/* split vring */
		req = virito_dev_split_ring_get_next_avail_req(dev, vq);
	}

	if (!req) {
		return;
	}
	scsi_req = to_scsi_request(req);
	scsi_req->endpoint = scsi_endpoint;
	/* add 1 for scsi event */
	scsi_endpoint->virtio.io_outstanding++;

	assert(req->iovcnt == 1);
	assert(req->iovs[0].iov_len == sizeof(struct virtio_scsi_event));
	desc_ev = req->iovs[0].iov_base;

	desc_ev->event = event;
	desc_ev->lun[0] = 1;
	desc_ev->lun[1] = scsi_target_num;
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

	req->used_len = sizeof(*desc_ev);

	SPDK_DEBUGLOG(vfu_virtio_scsi, "%s: SCSI Target Num %u, Desc %p, Event %u, Reason %u\n",
		      spdk_vfu_get_endpoint_name(scsi_endpoint->virtio.endpoint), scsi_target_num, desc_ev, event,
		      reason);

	virtio_scsi_req_finish(scsi_req);
	vfu_virtio_vq_flush_irq(dev, vq);
}

static int
virtio_scsi_start(struct vfu_virtio_endpoint *virtio_endpoint)
{
	struct virtio_scsi_endpoint *scsi_endpoint = to_scsi_endpoint(virtio_endpoint);
	struct virtio_scsi_target *scsi_target;
	uint8_t i;
	int ret;

	if (scsi_endpoint->ring_poller) {
		return 0;
	}

	SPDK_DEBUGLOG(vfu_virtio_scsi, "starting %s\n",
		      spdk_vfu_get_endpoint_name(scsi_endpoint->virtio.endpoint));

	for (i = 0; i < VIRTIO_SCSI_CTRLR_MAX_TARGETS; i++) {
		scsi_target = &scsi_endpoint->targets[i];
		if (scsi_target->dev) {
			ret = spdk_scsi_dev_allocate_io_channels(scsi_target->dev);
			if (ret) {
				SPDK_ERRLOG("%s: Couldn't allocate io channel for SCSI target %u.\n",
					    spdk_vfu_get_endpoint_name(scsi_endpoint->virtio.endpoint), i);
				continue;
			}
		}
	}

	scsi_endpoint->ring_poller = SPDK_POLLER_REGISTER(vfu_virtio_scsi_vring_poll, scsi_endpoint,
				     0);

	return 0;
}

static int
virtio_scsi_stop(struct vfu_virtio_endpoint *virtio_endpoint)
{
	struct virtio_scsi_endpoint *scsi_endpoint = to_scsi_endpoint(virtio_endpoint);
	struct virtio_scsi_target *scsi_target;
	uint8_t i;

	SPDK_DEBUGLOG(vfu_virtio_scsi, "stopping %s\n",
		      spdk_vfu_get_endpoint_name(scsi_endpoint->virtio.endpoint));

	spdk_poller_unregister(&scsi_endpoint->ring_poller);

	for (i = 0; i < VIRTIO_SCSI_CTRLR_MAX_TARGETS; i++) {
		scsi_target = &scsi_endpoint->targets[i];
		if (scsi_target->dev) {
			spdk_scsi_dev_free_io_channels(scsi_target->dev);
		}
	}

	return 0;
}

static void
virtio_scsi_task_cpl(struct spdk_scsi_task *scsi_task)
{
	struct virtio_scsi_req *scsi_req = SPDK_CONTAINEROF(scsi_task, struct virtio_scsi_req, scsi);

	scsi_req->cmd_resp->status = scsi_task->status;
	if (scsi_task->status != SPDK_SCSI_STATUS_GOOD) {
		scsi_req->cmd_resp->sense_len = scsi_task->sense_data_len;
		memcpy(scsi_req->cmd_resp->sense, scsi_task->sense_data, scsi_task->sense_data_len);
	}
	assert(scsi_task->transfer_len == scsi_task->length);
	scsi_req->cmd_resp->resid = scsi_task->length - scsi_task->data_transferred;

	virtio_scsi_req_finish(scsi_req);
	spdk_scsi_task_put(scsi_task);
}

static void
virtio_scsi_task_mgmt_cpl(struct spdk_scsi_task *scsi_task)
{
	struct virtio_scsi_req *scsi_req = SPDK_CONTAINEROF(scsi_task, struct virtio_scsi_req, scsi);

	virtio_scsi_req_finish(scsi_req);
	spdk_scsi_task_put(scsi_task);
}

static void
virtio_scsi_task_free_cb(struct spdk_scsi_task *scsi_task)
{

}

static struct virtio_scsi_target *
virtio_scsi_cmd_lun_setup(struct virtio_scsi_endpoint *scsi_endpoint,
			  struct virtio_scsi_req *scsi_req, __u8 *lun)
{
	struct virtio_scsi_target *scsi_target;
	uint16_t lun_id = (((uint16_t)lun[2] << 8) | lun[3]) & 0x3FFF;

	SPDK_LOGDUMP(vfu_virtio_scsi_data, "LUN", lun, 8);

	/* First byte must be 1 and second is target */
	if (lun[0] != 1 || lun[1] >= VIRTIO_SCSI_CTRLR_MAX_TARGETS) {
		SPDK_DEBUGLOG(vfu_virtio_scsi, "Invalid LUN %u:%u\n", lun[0], lun[1]);
		return NULL;
	}

	scsi_target = &scsi_endpoint->targets[lun[1]];
	if (!scsi_target->dev) {
		SPDK_DEBUGLOG(vfu_virtio_scsi, "SCSI Target num %u doesn't exist\n", lun[1]);
		return NULL;
	}

	scsi_req->scsi.target_port = spdk_scsi_dev_find_port_by_id(scsi_target->dev, 0);
	scsi_req->scsi.lun = spdk_scsi_dev_get_lun(scsi_target->dev, lun_id);
	if (scsi_req->scsi.lun == NULL) {
		SPDK_DEBUGLOG(vfu_virtio_scsi, "LUN %u:%u doesn't exist\n", lun[0], lun[1]);
		return NULL;
	}
	SPDK_DEBUGLOG(vfu_virtio_scsi, "Got valid SCSI Target num %u, bdev %s\n", lun[1],
		      spdk_scsi_lun_get_bdev_name(scsi_req->scsi.lun));

	return scsi_target;
}

static int
virtio_scsi_cmd_data_setup(struct virtio_scsi_req *scsi_req)
{
	struct iovec *iov;
	uint32_t iovcnt;
	uint32_t payload_len;

	iov = &scsi_req->req.iovs[0];
	iovcnt = scsi_req->req.iovcnt;
	payload_len = scsi_req->req.payload_size;

	if (spdk_unlikely(iov->iov_len < sizeof(struct virtio_scsi_cmd_req))) {
		SPDK_ERRLOG("Invalid virtio_scsi command header length");
		return -EINVAL;
	}
	if (spdk_unlikely(iovcnt < 2)) {
		SPDK_ERRLOG("Invalid iovcnt %u\n", iovcnt);
		return -EINVAL;
	}

	scsi_req->cmd_req = scsi_req->req.iovs[0].iov_base;
	payload_len -= scsi_req->req.iovs[0].iov_len;

	/*
	 * FROM_DEV (READ): [RO_req][WR_resp][WR_buf0]...[WR_bufN]
	 * TO_DEV  (WRITE): [RO_req][RO_buf0]...[RO_bufN][WR_resp]
	 */
	if (virtio_req_iov_is_wr(&scsi_req->req, 1)) {
		scsi_req->scsi.dxfer_dir = SPDK_SCSI_DIR_FROM_DEV;
	} else {
		scsi_req->scsi.dxfer_dir = SPDK_SCSI_DIR_TO_DEV;
	}

	if (scsi_req->scsi.dxfer_dir == SPDK_SCSI_DIR_FROM_DEV) {
		if (scsi_req->req.iovs[1].iov_len < sizeof(struct virtio_scsi_cmd_resp)) {
			SPDK_ERRLOG("DIR_FROM_DEV: Invalid virtio_scsi command resp length");
			return -EINVAL;
		}
		scsi_req->cmd_resp = scsi_req->req.iovs[1].iov_base;
		scsi_req->req.used_len = payload_len;
		scsi_req->scsi.iovs = &scsi_req->req.iovs[2];
	} else {
		if (scsi_req->req.iovs[iovcnt - 1].iov_len < sizeof(struct virtio_scsi_cmd_resp)) {
			SPDK_ERRLOG("DIR_TO_DEV: Invalid virtio_scsi command resp length");
			return -EINVAL;
		}
		scsi_req->req.used_len = sizeof(struct virtio_scsi_cmd_resp);
		scsi_req->cmd_resp = scsi_req->req.iovs[iovcnt - 1].iov_base;
		scsi_req->scsi.iovs = &scsi_req->req.iovs[1];
	}

	/* -2 for REQ and RESP */
	iovcnt -= 2;
	if (!iovcnt) {
		scsi_req->scsi.length = 0;
		scsi_req->scsi.transfer_len = 0;
		scsi_req->scsi.iovs[0].iov_len = 0;
	} else {
		assert(payload_len > sizeof(struct virtio_scsi_cmd_resp));
		payload_len -= sizeof(struct virtio_scsi_cmd_resp);
		scsi_req->scsi.length = payload_len;
		scsi_req->scsi.transfer_len = payload_len;
	}
	scsi_req->scsi.iovcnt = iovcnt;
	scsi_req->scsi.cdb = scsi_req->cmd_req->cdb;
	scsi_req->cmd_resp->response = VIRTIO_SCSI_S_OK;

	SPDK_LOGDUMP(vfu_virtio_scsi_data, "CDB=", scsi_req->cmd_req->cdb, VIRTIO_SCSI_CDB_SIZE);
	SPDK_DEBUGLOG(vfu_virtio_scsi, "%s, iovcnt %u, transfer_len %u, used len %u\n",
		      scsi_req->scsi.dxfer_dir == SPDK_SCSI_DIR_FROM_DEV ? "XFER_FROM_DEV" : "XFER_TO_DEV",
		      scsi_req->scsi.iovcnt, payload_len, scsi_req->req.used_len);

	return 0;
}

static int
virtio_scsi_tmf_cmd_req(struct virtio_scsi_endpoint *scsi_endpoint,
			struct virtio_scsi_req *scsi_req)
{
	uint32_t iovcnt;
	struct iovec *iov;
	struct virtio_scsi_ctrl_tmf_req *tmf_req;
	struct virtio_scsi_target *scsi_target;

	iov = &scsi_req->req.iovs[0];
	iovcnt = scsi_req->req.iovcnt;
	tmf_req = iov->iov_base;
	if (spdk_unlikely(iovcnt < 2)) {
		SPDK_ERRLOG("Invalid iovcnt %u\n", iovcnt);
		goto invalid;
	}

	memset(&scsi_req->scsi, 0, sizeof(struct spdk_scsi_task));
	spdk_scsi_task_construct(&scsi_req->scsi, virtio_scsi_task_mgmt_cpl, virtio_scsi_task_free_cb);

	switch (tmf_req->type) {
	case VIRTIO_SCSI_T_TMF:
		if (scsi_req->req.iovs[0].iov_len < sizeof(struct virtio_scsi_ctrl_tmf_req) ||
		    scsi_req->req.iovs[1].iov_len < sizeof(struct virtio_scsi_ctrl_tmf_resp)) {
			SPDK_ERRLOG("Invalid size of tmf_req or tmf_resp\n");
			goto invalid;
		}
		scsi_req->tmf_req = tmf_req;
		scsi_req->tmf_resp = scsi_req->req.iovs[1].iov_base;
		switch (tmf_req->subtype) {
		case VIRTIO_SCSI_T_TMF_LOGICAL_UNIT_RESET:
			scsi_target = virtio_scsi_cmd_lun_setup(scsi_endpoint, scsi_req, scsi_req->tmf_req->lun);
			if (!scsi_target) {
				scsi_req->tmf_resp->response = VIRTIO_SCSI_S_BAD_TARGET;
				break;
			}
			/* Management task submission */
			scsi_req->tmf_resp->response = VIRTIO_SCSI_S_OK;
			scsi_req->scsi.function = SPDK_SCSI_TASK_FUNC_LUN_RESET;
			spdk_scsi_dev_queue_mgmt_task(scsi_target->dev, &scsi_req->scsi);
			return 0;
			break;
		default:
			scsi_req->tmf_resp->response = VIRTIO_SCSI_S_FUNCTION_REJECTED;
			break;
		}
		break;

	case VIRTIO_SCSI_T_AN_QUERY:
	case VIRTIO_SCSI_T_AN_SUBSCRIBE:
		if (scsi_req->req.iovs[0].iov_len < sizeof(struct virtio_scsi_ctrl_an_req) ||
		    scsi_req->req.iovs[1].iov_len < sizeof(struct virtio_scsi_ctrl_an_resp)) {
			SPDK_ERRLOG("Invalid size of tmf_req or tmf_resp\n");
			goto invalid;
		}
		scsi_req->req.used_len = sizeof(struct virtio_scsi_ctrl_an_resp);
		/* Do nothing to response byte of virtio_scsi_ctrl_an_resp */
		goto invalid;
		break;
	default:
		break;
	}

invalid:
	/* invalid request */
	virtio_scsi_req_finish(scsi_req);
	return -1;
}

static int
virtio_scsi_cmd_req(struct virtio_scsi_endpoint *scsi_endpoint, struct virtio_scsi_req *scsi_req)
{
	int ret;
	struct virtio_scsi_target *scsi_target;

	memset(&scsi_req->scsi, 0, sizeof(struct spdk_scsi_task));
	spdk_scsi_task_construct(&scsi_req->scsi, virtio_scsi_task_cpl, virtio_scsi_task_free_cb);

	ret = virtio_scsi_cmd_data_setup(scsi_req);
	if (ret) {
		SPDK_ERRLOG("Error to setup SCSI command, ret %d\n", ret);
		goto invalid;
	}

	scsi_target = virtio_scsi_cmd_lun_setup(scsi_endpoint, scsi_req, scsi_req->cmd_req->lun);
	if (!scsi_target) {
		scsi_req->cmd_resp->response = VIRTIO_SCSI_S_BAD_TARGET;
		goto invalid;
	}

	spdk_scsi_dev_queue_task(scsi_target->dev, &scsi_req->scsi);
	return 0;

invalid:
	/* invalid request */
	virtio_scsi_req_finish(scsi_req);
	return ret;
}

static int
virtio_scsi_process_req(struct vfu_virtio_endpoint *virtio_endpoint, struct vfu_virtio_vq *vq,
			struct vfu_virtio_req *req)
{
	struct virtio_scsi_endpoint *scsi_endpoint = to_scsi_endpoint(virtio_endpoint);
	struct virtio_scsi_req *scsi_req = to_scsi_request(req);

	scsi_req->endpoint = scsi_endpoint;

	/* SCSI task management command */
	if (spdk_unlikely(vq->id == 0)) {
		return virtio_scsi_tmf_cmd_req(scsi_endpoint, scsi_req);
	}

	/* SCSI command */
	return virtio_scsi_cmd_req(scsi_endpoint, scsi_req);;
}

static void
virtio_scsi_update_config(struct virtio_scsi_endpoint *scsi_endpoint)
{
	struct virtio_scsi_config *scsi_cfg;

	if (!scsi_endpoint) {
		return;
	}

	scsi_cfg = &scsi_endpoint->scsi_cfg;

	scsi_cfg->num_queues = scsi_endpoint->virtio.num_queues;
	/*  -2 for REQ and RESP and -1 for region boundary splitting */
	scsi_cfg->seg_max = spdk_min(VIRTIO_DEV_MAX_IOVS - 2 - 1, SPDK_BDEV_IO_NUM_CHILD_IOV - 2 - 1);
	/* we can set `max_sectors` and `cmd_per_lun` based on bdevs */
	scsi_cfg->max_sectors = 131072;
	scsi_cfg->cmd_per_lun = scsi_endpoint->virtio.qsize;
	scsi_cfg->event_info_size = sizeof(struct virtio_scsi_event);
	scsi_cfg->sense_size = VIRTIO_SCSI_SENSE_DEFAULT_SIZE;
	scsi_cfg->cdb_size = VIRTIO_SCSI_CDB_DEFAULT_SIZE;
	scsi_cfg->max_channel = 0;
	scsi_cfg->max_target = VIRTIO_SCSI_CTRLR_MAX_TARGETS;
	scsi_cfg->max_lun = 16383;
}

static uint64_t
virtio_scsi_get_supported_features(struct vfu_virtio_endpoint *virtio_endpoint)
{
	uint64_t features;

	features = VIRTIO_SCSI_SUPPORTED_FEATURES | VIRTIO_HOST_SUPPORTED_FEATURES;

	if (!virtio_endpoint->packed_ring) {
		features &= ~(1ULL << VIRTIO_F_RING_PACKED);
	}

	return features;
}

static int
virtio_scsi_get_device_specific_config(struct vfu_virtio_endpoint *virtio_endpoint, char *buf,
				       uint64_t offset, uint64_t count)
{
	struct virtio_scsi_endpoint *scsi_endpoint = to_scsi_endpoint(virtio_endpoint);
	uint8_t *scsi_cfg;

	if ((offset + count) > sizeof(struct virtio_scsi_config)) {
		SPDK_ERRLOG("Invalid device specific configuration offset 0x%"PRIx64"\n", offset);
		return -EINVAL;
	}

	scsi_cfg = (uint8_t *)&scsi_endpoint->scsi_cfg;
	memcpy(buf, scsi_cfg + offset, count);

	return 0;
}

static int
virtio_scsi_set_device_specific_config(struct vfu_virtio_endpoint *virtio_endpoint, char *buf,
				       uint64_t offset, uint64_t count)
{
	struct virtio_scsi_endpoint *scsi_endpoint = to_scsi_endpoint(virtio_endpoint);
	uint32_t value;
	int ret = 0;

	if ((offset + count) > sizeof(struct virtio_scsi_config)) {
		SPDK_ERRLOG("Invalid device specific configuration offset 0x%"PRIx64"\n", offset);
		return -EINVAL;
	}

	switch (offset) {
	case offsetof(struct virtio_scsi_config, sense_size):
		value = *(uint32_t *)buf;
		if (scsi_endpoint->scsi_cfg.sense_size != value) {
			SPDK_ERRLOG("Sense data size set to %u\n", value);
			ret = -ENOTSUP;
		}
		break;
	case offsetof(struct virtio_scsi_config, cdb_size):
		value = *(uint32_t *)buf;
		if (scsi_endpoint->scsi_cfg.cdb_size != value) {
			SPDK_ERRLOG("CDB size set to %u\n", value);
			ret = -ENOTSUP;
		}
		break;
	default:
		SPDK_ERRLOG("Error offset %"PRIu64"\n", offset);
		ret = -EINVAL;
		break;
	}


	return ret;
}

static struct vfu_virtio_req *
virtio_scsi_alloc_req(struct vfu_virtio_endpoint *virtio_endpoint, struct vfu_virtio_vq *vq)
{
	struct virtio_scsi_req *scsi_req;

	scsi_req = calloc(1, sizeof(*scsi_req) + dma_sg_size() * (VIRTIO_DEV_MAX_IOVS + 1));
	if (!scsi_req) {
		return NULL;
	}

	return &scsi_req->req;
}

static void
virtio_scsi_free_req(struct vfu_virtio_endpoint *virtio_endpoint, struct vfu_virtio_vq *vq,
		     struct vfu_virtio_req *req)
{
	struct virtio_scsi_req *scsi_req = to_scsi_request(req);

	free(scsi_req);
}

struct vfu_virtio_ops virtio_scsi_ops = {
	.get_device_features = virtio_scsi_get_supported_features,
	.alloc_req = virtio_scsi_alloc_req,
	.free_req = virtio_scsi_free_req,
	.exec_request = virtio_scsi_process_req,
	.get_config = virtio_scsi_get_device_specific_config,
	.set_config = virtio_scsi_set_device_specific_config,
	.start_device = virtio_scsi_start,
	.stop_device = virtio_scsi_stop,
};

int
vfu_virtio_scsi_set_options(const char *name, uint16_t num_io_queues, uint16_t qsize,
			    bool packed_ring)
{
	struct spdk_vfu_endpoint *endpoint;
	uint32_t num_queues;
	struct vfu_virtio_endpoint *virtio_endpoint;
	struct virtio_scsi_endpoint *scsi_endpoint;

	num_queues = num_io_queues + 2;

	endpoint = spdk_vfu_get_endpoint_by_name(name);
	if (!endpoint) {
		SPDK_ERRLOG("Endpoint %s doesn't exist\n", name);
		return -ENOENT;
	}

	virtio_endpoint = spdk_vfu_get_endpoint_private(endpoint);
	scsi_endpoint = to_scsi_endpoint(virtio_endpoint);
	if (virtio_endpoint->dev) {
		SPDK_ERRLOG("Options are not allowed to change in runtime\n");
		return -EFAULT;
	}

	if ((num_queues > 2) && (num_queues <= VIRTIO_DEV_MAX_VQS)) {
		scsi_endpoint->virtio.num_queues = num_queues;
	} else {
		SPDK_NOTICELOG("Number of IO queue %u\n", VIRTIO_DEV_MAX_VQS - 2);
		scsi_endpoint->virtio.num_queues = VIRTIO_DEV_MAX_VQS;
	}

	if (qsize && qsize <= VIRTIO_VQ_MAX_SIZE) {
		scsi_endpoint->virtio.qsize = qsize;
	} else {
		SPDK_NOTICELOG("Use queue size %u\n", VIRTIO_VQ_DEFAULT_SIZE);
		scsi_endpoint->virtio.qsize = VIRTIO_VQ_DEFAULT_SIZE;
	}
	scsi_endpoint->virtio.packed_ring = packed_ring;

	SPDK_DEBUGLOG(vfu_virtio_scsi, "%s: num_queues %u, qsize %u, packed ring %s\n",
		      spdk_vfu_get_endpoint_id(endpoint),
		      scsi_endpoint->virtio.num_queues, scsi_endpoint->virtio.qsize,
		      packed_ring ? "enabled" : "disabled");

	virtio_scsi_update_config(scsi_endpoint);

	return 0;
}

struct virtio_scsi_event_ctx {
	struct virtio_scsi_endpoint *scsi_endpoint;
	struct virtio_scsi_target *scsi_target;
	uint8_t scsi_target_num;
};

static uint8_t
get_scsi_target_num_by_lun(struct virtio_scsi_endpoint *scsi_endpoint,
			   const struct spdk_scsi_lun *lun)
{
	const struct spdk_scsi_dev *scsi_dev;
	struct virtio_scsi_target *scsi_target;
	uint8_t i;

	scsi_dev = spdk_scsi_lun_get_dev(lun);
	for (i = 0; i < VIRTIO_SCSI_CTRLR_MAX_TARGETS; i++) {
		scsi_target = &scsi_endpoint->targets[i];
		if (scsi_target->dev == scsi_dev) {
			return i;
		}
	}

	return VIRTIO_SCSI_CTRLR_MAX_TARGETS;
}

static void
vfu_virtio_scsi_lun_resize_msg(void *ctx)
{
	struct virtio_scsi_event_ctx *resize_ctx = ctx;
	struct virtio_scsi_endpoint *scsi_endpoint = resize_ctx->scsi_endpoint;
	uint8_t scsi_target_num = resize_ctx->scsi_target_num;

	free(resize_ctx);

	if (virtio_guest_has_feature(scsi_endpoint->virtio.dev, VIRTIO_SCSI_F_CHANGE)) {
		vfu_virtio_scsi_eventq_enqueue(scsi_endpoint, scsi_target_num,
					       VIRTIO_SCSI_T_PARAM_CHANGE, 0x2a | (0x09 << 8));
	}
}

static void
vfu_virtio_scsi_lun_resize(const struct spdk_scsi_lun *lun, void *arg)
{
	struct virtio_scsi_endpoint *scsi_endpoint = arg;
	uint8_t scsi_target_num;
	struct virtio_scsi_event_ctx *ctx;

	scsi_target_num = get_scsi_target_num_by_lun(scsi_endpoint, lun);
	if (scsi_target_num == VIRTIO_SCSI_CTRLR_MAX_TARGETS) {
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("Error to allocate hotplug ctx\n");
		return;
	}
	ctx->scsi_endpoint = scsi_endpoint;
	ctx->scsi_target_num = scsi_target_num;

	spdk_thread_send_msg(scsi_endpoint->virtio.thread, vfu_virtio_scsi_lun_resize_msg, ctx);
}

static void
vfu_virtio_scsi_lun_hotremove_msg(void *ctx)
{
	struct virtio_scsi_event_ctx *hotplug = ctx;
	struct virtio_scsi_endpoint *scsi_endpoint = hotplug->scsi_endpoint;
	struct virtio_scsi_target *scsi_target = hotplug->scsi_target;
	struct spdk_scsi_dev *scsi_dev = scsi_target->dev;
	uint8_t scsi_target_num = hotplug->scsi_target_num;

	free(hotplug);

	if (!scsi_dev) {
		return;
	}
	scsi_target->dev = NULL;
	spdk_scsi_dev_free_io_channels(scsi_dev);
	spdk_scsi_dev_destruct(scsi_dev, NULL, NULL);

	assert(scsi_endpoint->virtio.dev);
	if (!virtio_dev_is_started(scsi_endpoint->virtio.dev)) {
		return;
	}

	if (virtio_guest_has_feature(scsi_endpoint->virtio.dev, VIRTIO_SCSI_F_HOTPLUG)) {
		SPDK_DEBUGLOG(vfu_virtio_scsi, "Target num %u, sending event\n", scsi_target_num);
		vfu_virtio_scsi_eventq_enqueue(scsi_endpoint, scsi_target_num,
					       VIRTIO_SCSI_T_TRANSPORT_RESET, VIRTIO_SCSI_EVT_RESET_REMOVED);
	}
}

static void
vfu_virtio_scsi_lun_hotremove(const struct spdk_scsi_lun *lun, void *arg)
{
	struct virtio_scsi_endpoint *scsi_endpoint = arg;
	struct virtio_scsi_target *scsi_target;
	struct virtio_scsi_event_ctx *ctx;
	uint8_t scsi_target_num;

	if (!scsi_endpoint->virtio.dev) {
		return;
	}

	scsi_target_num = get_scsi_target_num_by_lun(scsi_endpoint, lun);
	if (scsi_target_num == VIRTIO_SCSI_CTRLR_MAX_TARGETS) {
		return;
	}
	scsi_target = &scsi_endpoint->targets[scsi_target_num];
	if (!scsi_target->dev) {
		return;
	}

	SPDK_DEBUGLOG(vfu_virtio_scsi, "Removing bdev %s, Target num %u\n",
		      spdk_scsi_lun_get_bdev_name(lun), scsi_target_num);

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("Error to allocate hotplug ctx\n");
		return;
	}
	ctx->scsi_endpoint = scsi_endpoint;
	ctx->scsi_target = scsi_target;
	ctx->scsi_target_num = scsi_target_num;

	spdk_thread_send_msg(scsi_endpoint->virtio.thread, vfu_virtio_scsi_lun_hotremove_msg, ctx);
}

static void
vfu_virtio_scsi_lun_hotplug_msg(void *ctx)
{
	struct virtio_scsi_event_ctx *hotplug = ctx;
	struct virtio_scsi_endpoint *scsi_endpoint = hotplug->scsi_endpoint;
	struct virtio_scsi_target *scsi_target = hotplug->scsi_target;
	uint8_t scsi_target_num = hotplug->scsi_target_num;
	int ret;

	free(hotplug);

	assert(scsi_endpoint->virtio.dev);
	if (!virtio_dev_is_started(scsi_endpoint->virtio.dev)) {
		return;
	}

	ret = spdk_scsi_dev_allocate_io_channels(scsi_target->dev);
	if (ret) {
		SPDK_ERRLOG("%s: Couldn't allocate io channel for SCSI target %u.\n",
			    spdk_vfu_get_endpoint_name(scsi_endpoint->virtio.endpoint), scsi_target_num);
		return;
	}

	if (virtio_guest_has_feature(scsi_endpoint->virtio.dev, VIRTIO_SCSI_F_HOTPLUG)) {
		vfu_virtio_scsi_eventq_enqueue(scsi_endpoint, scsi_target_num,
					       VIRTIO_SCSI_T_TRANSPORT_RESET, VIRTIO_SCSI_EVT_RESET_RESCAN);
	}
}

int
vfu_virtio_scsi_add_target(const char *name, uint8_t scsi_target_num, const char *bdev_name)
{
	struct spdk_vfu_endpoint *endpoint;
	struct vfu_virtio_endpoint *virtio_endpoint;
	struct virtio_scsi_endpoint *scsi_endpoint;
	struct virtio_scsi_target *scsi_target;
	char target_name[SPDK_SCSI_DEV_MAX_NAME];
	int lun_id_list[1];
	const char *bdev_names_list[1];

	endpoint = spdk_vfu_get_endpoint_by_name(name);
	if (!endpoint) {
		SPDK_ERRLOG("Endpoint %s doesn't exist\n", name);
		return -ENOENT;
	}
	virtio_endpoint = spdk_vfu_get_endpoint_private(endpoint);
	scsi_endpoint = to_scsi_endpoint(virtio_endpoint);

	if (scsi_target_num >= VIRTIO_SCSI_CTRLR_MAX_TARGETS) {
		SPDK_ERRLOG("Invalid SCSI target number, maximum SCSI target number is %u\n",
			    VIRTIO_SCSI_CTRLR_MAX_TARGETS - 1);
		return -EINVAL;
	}
	scsi_target = &scsi_endpoint->targets[scsi_target_num];
	if (scsi_target->dev) {
		SPDK_ERRLOG("SCSI Target %u is already occupied\n", scsi_target_num);
		return -EEXIST;
	}

	snprintf(target_name, sizeof(target_name), "Target %u", scsi_target_num);
	lun_id_list[0] = 0;
	bdev_names_list[0] = (char *)bdev_name;

	scsi_target->dev = spdk_scsi_dev_construct_ext(target_name, bdev_names_list, lun_id_list, 1,
			   SPDK_SPC_PROTOCOL_IDENTIFIER_SAS,
			   vfu_virtio_scsi_lun_resize, scsi_endpoint,
			   vfu_virtio_scsi_lun_hotremove, scsi_endpoint);
	if (!scsi_target->dev) {
		SPDK_ERRLOG("%s: couldn't create SCSI target %u via bdev %s\n", name, scsi_target_num, bdev_name);
		return -EFAULT;
	}
	spdk_scsi_dev_add_port(scsi_target->dev, 0, "vfu-virtio-scsi");

	SPDK_NOTICELOG("%s: added SCSI target %u using bdev '%s'\n", name, scsi_target_num, bdev_name);
	virtio_scsi_update_config(scsi_endpoint);

	if (virtio_endpoint->dev) {
		struct virtio_scsi_event_ctx *ctx;

		ctx = calloc(1, sizeof(*ctx));
		if (!ctx) {
			SPDK_ERRLOG("Error to allocate hotplug ctx\n");
			/* This isn't fatal, just skip hotplug notification */
		} else {
			ctx->scsi_endpoint = scsi_endpoint;
			ctx->scsi_target = scsi_target;
			ctx->scsi_target_num = scsi_target_num;
			spdk_thread_send_msg(virtio_endpoint->thread, vfu_virtio_scsi_lun_hotplug_msg, ctx);
		}
	}

	return 0;
}

int
vfu_virtio_scsi_remove_target(const char *name, uint8_t scsi_target_num)
{
	struct spdk_vfu_endpoint *endpoint;
	struct vfu_virtio_endpoint *virtio_endpoint;
	struct virtio_scsi_endpoint *scsi_endpoint;
	struct virtio_scsi_target *scsi_target;

	endpoint = spdk_vfu_get_endpoint_by_name(name);
	if (!endpoint) {
		SPDK_ERRLOG("Endpoint %s doesn't exist\n", name);
		return -ENOENT;
	}
	virtio_endpoint = spdk_vfu_get_endpoint_private(endpoint);
	scsi_endpoint = to_scsi_endpoint(virtio_endpoint);

	if (scsi_target_num >= VIRTIO_SCSI_CTRLR_MAX_TARGETS) {
		SPDK_ERRLOG("Invalid SCSI target number, maximum SCSI target number is %u\n",
			    VIRTIO_SCSI_CTRLR_MAX_TARGETS - 1);
		return -EINVAL;
	}
	scsi_target = &scsi_endpoint->targets[scsi_target_num];
	if (!scsi_target->dev) {
		SPDK_ERRLOG("SCSI Target %u doesn't exist\n", scsi_target_num);
		return -ENOENT;
	}

	SPDK_NOTICELOG("%s: Remove SCSI target num %u\n", name, scsi_target_num);

	if (virtio_endpoint->dev) {
		struct virtio_scsi_event_ctx *ctx;

		ctx = calloc(1, sizeof(*ctx));
		if (!ctx) {
			SPDK_ERRLOG("Error to allocate hotplug ctx\n");
			/* This isn't fatal, just skip hotplug notification */
		} else {
			ctx->scsi_endpoint = scsi_endpoint;
			ctx->scsi_target = scsi_target;
			ctx->scsi_target_num = scsi_target_num;
			spdk_thread_send_msg(scsi_endpoint->virtio.thread, vfu_virtio_scsi_lun_hotremove_msg, ctx);
		}
	} else {
		spdk_scsi_dev_destruct(scsi_target->dev, NULL, NULL);
		scsi_target->dev = NULL;
	}

	return 0;
}

static int
vfu_virtio_scsi_endpoint_destruct(struct spdk_vfu_endpoint *endpoint)
{
	struct vfu_virtio_endpoint *virtio_endpoint = spdk_vfu_get_endpoint_private(endpoint);
	struct virtio_scsi_endpoint *scsi_endpoint = to_scsi_endpoint(virtio_endpoint);
	struct virtio_scsi_target *scsi_target;
	uint8_t i;

	for (i = 0; i < VIRTIO_SCSI_CTRLR_MAX_TARGETS; i++) {
		scsi_target = &scsi_endpoint->targets[i];
		if (scsi_target->dev) {
			spdk_scsi_dev_destruct(scsi_target->dev, NULL, NULL);
		}
	}

	vfu_virtio_endpoint_destruct(&scsi_endpoint->virtio);
	free(scsi_endpoint);

	return 0;
}

static void *
vfu_virtio_scsi_endpoint_init(struct spdk_vfu_endpoint *endpoint,
			      char *basename, const char *endpoint_name)
{
	struct virtio_scsi_endpoint *scsi_endpoint;
	int ret;

	scsi_endpoint = calloc(1, sizeof(*scsi_endpoint));
	if (!scsi_endpoint) {
		return NULL;
	}

	ret = vfu_virtio_endpoint_setup(&scsi_endpoint->virtio, endpoint, basename, endpoint_name,
					&virtio_scsi_ops);
	if (ret) {
		SPDK_ERRLOG("Error to setup endpoint %s\n", endpoint_name);
		free(scsi_endpoint);
		return NULL;
	}

	virtio_scsi_update_config(scsi_endpoint);
	return (void *)&scsi_endpoint->virtio;
}

static int
vfu_virtio_scsi_get_device_info(struct spdk_vfu_endpoint *endpoint,
				struct spdk_vfu_pci_device *device_info)
{
	struct vfu_virtio_endpoint *virtio_endpoint = spdk_vfu_get_endpoint_private(endpoint);
	struct virtio_scsi_endpoint *scsi_endpoint = to_scsi_endpoint(virtio_endpoint);

	vfu_virtio_get_device_info(&scsi_endpoint->virtio, device_info);
	/* Fill Device ID */
	device_info->id.did = PCI_DEVICE_ID_VIRTIO_SCSI_MODERN;

	return 0;
}

struct spdk_vfu_endpoint_ops vfu_virtio_scsi_ops = {
	.name = "virtio_scsi",
	.init = vfu_virtio_scsi_endpoint_init,
	.get_device_info = vfu_virtio_scsi_get_device_info,
	.get_vendor_capability = vfu_virtio_get_vendor_capability,
	.post_memory_add = vfu_virtio_post_memory_add,
	.pre_memory_remove = vfu_virtio_pre_memory_remove,
	.reset_device = vfu_virtio_pci_reset_cb,
	.quiesce_device = vfu_virtio_quiesce_cb,
	.destruct = vfu_virtio_scsi_endpoint_destruct,
	.attach_device = vfu_virtio_attach_device,
	.detach_device = vfu_virtio_detach_device,
};

static void
__attribute__((constructor)) _vfu_virtio_scsi_pci_model_register(void)
{
	spdk_vfu_register_endpoint_ops(&vfu_virtio_scsi_ops);
}

SPDK_LOG_REGISTER_COMPONENT(vfu_virtio_scsi)
SPDK_LOG_REGISTER_COMPONENT(vfu_virtio_scsi_data)
