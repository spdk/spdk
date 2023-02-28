/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/*
 * virtio-blk over vfio-user transport
 */
#include <linux/virtio_blk.h>

#include "spdk/env.h"
#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/stdinc.h"
#include "spdk/assert.h"
#include "spdk/barrier.h"
#include "spdk/thread.h"
#include "spdk/memory.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk/pci_ids.h"

#include "vfu_virtio_internal.h"

#define VIRTIO_BLK_SUPPORTED_FEATURES	((1ULL << VIRTIO_BLK_F_SIZE_MAX) | \
					 (1ULL << VIRTIO_BLK_F_SEG_MAX) | \
					 (1ULL << VIRTIO_BLK_F_TOPOLOGY) | \
					 (1ULL << VIRTIO_BLK_F_BLK_SIZE) | \
					 (1ULL << VIRTIO_BLK_F_MQ))

struct virtio_blk_endpoint {
	struct vfu_virtio_endpoint	virtio;

	/* virtio_blk specific configurations */
	struct spdk_thread		*init_thread;
	struct spdk_bdev		*bdev;
	struct spdk_bdev_desc		*bdev_desc;
	struct spdk_io_channel		*io_channel;
	struct virtio_blk_config	blk_cfg;

	/* virtio_blk ring process poller */
	struct spdk_poller		*ring_poller;
};

struct virtio_blk_req {
	volatile uint8_t *status;
	struct virtio_blk_endpoint *endpoint;
	/* KEEP req at last */
	struct vfu_virtio_req req;
};

static inline struct virtio_blk_endpoint *
to_blk_endpoint(struct vfu_virtio_endpoint *virtio_endpoint)
{
	return SPDK_CONTAINEROF(virtio_endpoint, struct virtio_blk_endpoint, virtio);
}

static inline struct virtio_blk_req *
to_blk_request(struct vfu_virtio_req *request)
{
	return SPDK_CONTAINEROF(request, struct virtio_blk_req, req);
}

static int
vfu_virtio_blk_vring_poll(void *ctx)
{
	struct virtio_blk_endpoint *blk_endpoint = ctx;
	struct vfu_virtio_dev *dev = blk_endpoint->virtio.dev;
	struct vfu_virtio_vq *vq;
	uint32_t i, count = 0;

	if (spdk_unlikely(!virtio_dev_is_started(dev))) {
		return SPDK_POLLER_IDLE;
	}

	if (spdk_unlikely(blk_endpoint->virtio.quiesce_in_progress)) {
		return SPDK_POLLER_IDLE;
	}

	for (i = 0; i < dev->num_queues; i++) {
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

static int
virtio_blk_start(struct vfu_virtio_endpoint *virtio_endpoint)
{
	struct virtio_blk_endpoint *blk_endpoint = to_blk_endpoint(virtio_endpoint);

	if (blk_endpoint->ring_poller) {
		return 0;
	}

	SPDK_DEBUGLOG(vfu_virtio_blk, "starting %s\n", virtio_endpoint->dev->name);
	blk_endpoint->io_channel = spdk_bdev_get_io_channel(blk_endpoint->bdev_desc);
	blk_endpoint->ring_poller = SPDK_POLLER_REGISTER(vfu_virtio_blk_vring_poll, blk_endpoint, 0);

	return 0;
}

static void
_virtio_blk_stop_msg(void *ctx)
{
	struct virtio_blk_endpoint *blk_endpoint = ctx;

	spdk_poller_unregister(&blk_endpoint->ring_poller);
	spdk_put_io_channel(blk_endpoint->io_channel);
	blk_endpoint->io_channel = NULL;

	SPDK_DEBUGLOG(vfu_virtio_blk, "%s is stopped\n",
		      spdk_vfu_get_endpoint_id(blk_endpoint->virtio.endpoint));
}

static int
virtio_blk_stop(struct vfu_virtio_endpoint *virtio_endpoint)
{
	struct virtio_blk_endpoint *blk_endpoint = to_blk_endpoint(virtio_endpoint);

	if (!blk_endpoint->io_channel) {
		return 0;
	}

	SPDK_DEBUGLOG(vfu_virtio_blk, "%s stopping\n", spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint));
	spdk_thread_send_msg(virtio_endpoint->thread, _virtio_blk_stop_msg, blk_endpoint);
	return 0;
}

static void
virtio_blk_req_finish(struct virtio_blk_req *blk_req, uint8_t status)
{
	struct vfu_virtio_req *req = &blk_req->req;

	if (spdk_likely(blk_req->status)) {
		*blk_req->status = status;
		blk_req->status = NULL;
	}

	vfu_virtio_finish_req(req);
}

static void
blk_request_complete_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct virtio_blk_req *blk_req = cb_arg;

	SPDK_DEBUGLOG(vfu_virtio_blk, "IO done status %u\n", success);

	spdk_bdev_free_io(bdev_io);
	virtio_blk_req_finish(blk_req, success ? VIRTIO_BLK_S_OK : VIRTIO_BLK_S_IOERR);
}

static int
virtio_blk_process_req(struct vfu_virtio_endpoint *virtio_endpoint, struct vfu_virtio_vq *vq,
		       struct vfu_virtio_req *req)
{
	struct virtio_blk_endpoint *blk_endpoint = to_blk_endpoint(virtio_endpoint);
	struct virtio_blk_req *blk_req = to_blk_request(req);
	const struct virtio_blk_outhdr *hdr;
	struct virtio_blk_discard_write_zeroes *desc;
	struct iovec *iov;
	uint16_t iovcnt;
	uint64_t flush_bytes;
	uint32_t type;
	uint32_t payload_len;
	int ret;

	blk_req->endpoint = blk_endpoint;

	iov = &req->iovs[0];
	if (spdk_unlikely(iov->iov_len != sizeof(*hdr))) {
		SPDK_ERRLOG("Invalid virtio_blk header length %lu\n", iov->iov_len);
		virtio_blk_req_finish(blk_req, VIRTIO_BLK_S_UNSUPP);
		return -EINVAL;
	}
	hdr = iov->iov_base;

	iov = &req->iovs[req->iovcnt - 1];
	if (spdk_unlikely(iov->iov_len != 1)) {
		SPDK_ERRLOG("Invalid virtio_blk response length %lu\n", iov->iov_len);
		virtio_blk_req_finish(blk_req, VIRTIO_BLK_S_UNSUPP);
		return -EINVAL;
	}
	blk_req->status = iov->iov_base;

	payload_len = req->payload_size;
	payload_len -= sizeof(*hdr) + 1;
	iovcnt = req->iovcnt - 2;

	type = hdr->type;
	/* Legacy type isn't supported */
	type &= ~VIRTIO_BLK_T_BARRIER;

	SPDK_DEBUGLOG(vfu_virtio_blk, "%s: type %u, iovcnt %u, payload_len %u\n",
		      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
		      type, iovcnt, payload_len);

	if (spdk_unlikely(blk_endpoint->bdev_desc == NULL)) {
		SPDK_ERRLOG("Bdev has been removed\n");
		virtio_blk_req_finish(blk_req, VIRTIO_BLK_S_IOERR);
		return 0;
	}

	switch (type) {
	case VIRTIO_BLK_T_IN:
	case VIRTIO_BLK_T_OUT:
		if (spdk_unlikely(payload_len == 0 || (payload_len & (512 - 1)) != 0)) {
			SPDK_ERRLOG("Invalid payload length %u\n", payload_len);
			virtio_blk_req_finish(blk_req, VIRTIO_BLK_S_UNSUPP);
			return -EINVAL;
		}
		if (type == VIRTIO_BLK_T_IN) {
			req->used_len = payload_len + 1;
			ret = spdk_bdev_readv(blk_endpoint->bdev_desc, blk_endpoint->io_channel,
					      &req->iovs[1], iovcnt, hdr->sector * 512,
					      payload_len, blk_request_complete_cb, blk_req);
		} else {
			req->used_len = 1;
			ret = spdk_bdev_writev(blk_endpoint->bdev_desc, blk_endpoint->io_channel,
					       &req->iovs[1], iovcnt, hdr->sector * 512,
					       payload_len, blk_request_complete_cb, blk_req);
		}
		if (ret) {
			SPDK_ERRLOG("R/W error\n");
			virtio_blk_req_finish(blk_req, VIRTIO_BLK_S_IOERR);
			return ret;
		}
		break;
	case VIRTIO_BLK_T_DISCARD:
		desc = req->iovs[1].iov_base;
		if (payload_len != sizeof(*desc)) {
			SPDK_NOTICELOG("Invalid discard payload size: %u\n", payload_len);
			virtio_blk_req_finish(blk_req, VIRTIO_BLK_S_IOERR);
			return -EINVAL;
		}

		if (desc->flags & VIRTIO_BLK_WRITE_ZEROES_FLAG_UNMAP) {
			SPDK_ERRLOG("UNMAP flag is only used for WRITE ZEROES command\n");
			virtio_blk_req_finish(blk_req, VIRTIO_BLK_S_UNSUPP);
			return -EINVAL;
		}

		ret = spdk_bdev_unmap(blk_endpoint->bdev_desc, blk_endpoint->io_channel,
				      desc->sector * 512, desc->num_sectors * 512,
				      blk_request_complete_cb, blk_req);
		if (ret) {
			SPDK_ERRLOG("UNMAP error\n");
			virtio_blk_req_finish(blk_req, VIRTIO_BLK_S_IOERR);
			return ret;
		}
		break;
	case VIRTIO_BLK_T_WRITE_ZEROES:
		desc = req->iovs[1].iov_base;
		if (payload_len != sizeof(*desc)) {
			SPDK_NOTICELOG("Invalid write zeroes payload size: %u\n", payload_len);
			virtio_blk_req_finish(blk_req, VIRTIO_BLK_S_IOERR);
			return -1;
		}

		/* Unmap this range, SPDK doesn't support it, kernel will enable this flag by default
		 * without checking unmap feature is negotiated or not, the flag isn't mandatory, so
		 * just print a warning.
		 */
		if (desc->flags & VIRTIO_BLK_WRITE_ZEROES_FLAG_UNMAP) {
			SPDK_WARNLOG("Ignore the unmap flag for WRITE ZEROES from %"PRIx64", len %"PRIx64"\n",
				     (uint64_t)desc->sector * 512, (uint64_t)desc->num_sectors * 512);
		}

		ret = spdk_bdev_write_zeroes(blk_endpoint->bdev_desc, blk_endpoint->io_channel,
					     desc->sector * 512, desc->num_sectors * 512,
					     blk_request_complete_cb, blk_req);
		if (ret) {
			SPDK_ERRLOG("WRITE ZEROES error\n");
			virtio_blk_req_finish(blk_req, VIRTIO_BLK_S_IOERR);
			return ret;
		}
		break;
	case VIRTIO_BLK_T_FLUSH:
		flush_bytes = spdk_bdev_get_num_blocks(blk_endpoint->bdev) * spdk_bdev_get_block_size(
				      blk_endpoint->bdev);
		if (hdr->sector != 0) {
			SPDK_NOTICELOG("sector must be zero for flush command\n");
			virtio_blk_req_finish(blk_req, VIRTIO_BLK_S_IOERR);
			return -EINVAL;
		}
		ret = spdk_bdev_flush(blk_endpoint->bdev_desc, blk_endpoint->io_channel,
				      0, flush_bytes,
				      blk_request_complete_cb, blk_req);
		if (ret) {
			SPDK_ERRLOG("FLUSH error\n");
			virtio_blk_req_finish(blk_req, VIRTIO_BLK_S_IOERR);
			return ret;
		}
		break;
	case VIRTIO_BLK_T_GET_ID:
		if (!iovcnt || !payload_len) {
			virtio_blk_req_finish(blk_req, VIRTIO_BLK_S_UNSUPP);
			return -EINVAL;
		}
		req->used_len = spdk_min((size_t)VIRTIO_BLK_ID_BYTES, req->iovs[1].iov_len);
		spdk_strcpy_pad(req->iovs[1].iov_base, spdk_bdev_get_name(blk_endpoint->bdev),
				req->used_len, ' ');
		virtio_blk_req_finish(blk_req, VIRTIO_BLK_S_OK);
		break;
	default:
		virtio_blk_req_finish(blk_req, VIRTIO_BLK_S_UNSUPP);
		return -ENOTSUP;
	}

	return 0;
}

static void
virtio_blk_update_config(struct virtio_blk_config *blk_cfg, struct spdk_bdev *bdev,
			 uint16_t num_queues)
{
	memset(blk_cfg, 0, sizeof(*blk_cfg));

	if (!bdev) {
		return;
	}

	blk_cfg->blk_size = spdk_bdev_get_block_size(bdev);
	blk_cfg->capacity = (blk_cfg->blk_size * spdk_bdev_get_num_blocks(bdev)) / 512;
	/* minimum I/O size in blocks */
	blk_cfg->min_io_size = 1;
	blk_cfg->num_queues = num_queues;

	if (spdk_bdev_get_buf_align(bdev) > 1) {
		blk_cfg->size_max = SPDK_BDEV_LARGE_BUF_MAX_SIZE;
		blk_cfg->seg_max = spdk_min(VIRTIO_DEV_MAX_IOVS - 2 - 1, SPDK_BDEV_IO_NUM_CHILD_IOV - 2 - 1);
	} else {
		blk_cfg->size_max = 131072;
		/*  -2 for REQ and RESP and -1 for region boundary splitting */
		blk_cfg->seg_max = VIRTIO_DEV_MAX_IOVS - 2 - 1;
	}

	if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
		/* 16MiB, expressed in 512 Bytes */
		blk_cfg->max_discard_sectors = 32768;
		blk_cfg->max_discard_seg = 1;
		blk_cfg->discard_sector_alignment = blk_cfg->blk_size / 512;
	}
	if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES)) {
		blk_cfg->max_write_zeroes_sectors = 32768;
		blk_cfg->max_write_zeroes_seg = 1;
	}
}

static void
_vfu_virtio_blk_bdev_close(void *arg1)
{
	struct spdk_bdev_desc *bdev_desc = arg1;

	spdk_bdev_close(bdev_desc);
}

static void
bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
	      void *event_ctx)
{
	struct virtio_blk_endpoint *blk_endpoint = event_ctx;

	SPDK_DEBUGLOG(vfu_virtio_blk, "Bdev event: type %d, name %s\n", type, bdev->name);

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		SPDK_NOTICELOG("bdev name (%s) received event(SPDK_BDEV_EVENT_REMOVE)\n", bdev->name);
		virtio_blk_update_config(&blk_endpoint->blk_cfg, NULL, 0);

		if (blk_endpoint->io_channel) {
			spdk_thread_send_msg(blk_endpoint->virtio.thread, _virtio_blk_stop_msg, blk_endpoint);
		}

		if (blk_endpoint->bdev_desc) {
			spdk_thread_send_msg(blk_endpoint->init_thread, _vfu_virtio_blk_bdev_close,
					     blk_endpoint->bdev_desc);
			blk_endpoint->bdev_desc = NULL;
		}
		break;
	case SPDK_BDEV_EVENT_RESIZE:
		SPDK_NOTICELOG("bdev name (%s) received event(SPDK_BDEV_EVENT_RESIZE)\n", bdev->name);
		virtio_blk_update_config(&blk_endpoint->blk_cfg, blk_endpoint->bdev,
					 blk_endpoint->virtio.num_queues);
		vfu_virtio_notify_config(&blk_endpoint->virtio);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

static uint64_t
virtio_blk_get_supported_features(struct vfu_virtio_endpoint *virtio_endpoint)
{
	struct virtio_blk_endpoint *blk_endpoint = to_blk_endpoint(virtio_endpoint);
	uint64_t features;
	struct spdk_bdev *bdev;

	features = VIRTIO_BLK_SUPPORTED_FEATURES | VIRTIO_HOST_SUPPORTED_FEATURES;

	if (!virtio_endpoint->packed_ring) {
		features &= ~(1ULL << VIRTIO_F_RING_PACKED);
	}
	bdev = blk_endpoint->bdev;

	if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
		features |= (1ULL << VIRTIO_BLK_F_DISCARD);
	}

	if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES)) {
		features |= (1ULL << VIRTIO_BLK_F_WRITE_ZEROES);
	}

	if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_FLUSH)) {
		features |= (1ULL << VIRTIO_BLK_F_FLUSH);
	}

	return features;
}

static int
virtio_blk_get_device_specific_config(struct vfu_virtio_endpoint *virtio_endpoint, char *buf,
				      uint64_t offset, uint64_t count)
{
	struct virtio_blk_endpoint *blk_endpoint = to_blk_endpoint(virtio_endpoint);
	uint8_t *blk_cfg;
	uint64_t len;

	if (offset >= sizeof(struct virtio_blk_config)) {
		return -EINVAL;
	}
	len = spdk_min(sizeof(struct virtio_blk_config) - offset, count);

	blk_cfg = (uint8_t *)&blk_endpoint->blk_cfg;
	memcpy(buf, blk_cfg + offset, len);

	return 0;
}

static struct vfu_virtio_req *
virtio_blk_alloc_req(struct vfu_virtio_endpoint *virtio_endpoint, struct vfu_virtio_vq *vq)
{
	struct virtio_blk_req *blk_req;

	blk_req = calloc(1, sizeof(*blk_req) + dma_sg_size() * (VIRTIO_DEV_MAX_IOVS + 1));
	if (!blk_req) {
		return NULL;
	}

	return &blk_req->req;
}

static void
virtio_blk_free_req(struct vfu_virtio_endpoint *virtio_endpoint, struct vfu_virtio_vq *vq,
		    struct vfu_virtio_req *req)
{
	struct virtio_blk_req *blk_req = to_blk_request(req);

	free(blk_req);
}

struct vfu_virtio_ops virtio_blk_ops = {
	.get_device_features = virtio_blk_get_supported_features,
	.alloc_req = virtio_blk_alloc_req,
	.free_req = virtio_blk_free_req,
	.exec_request = virtio_blk_process_req,
	.get_config = virtio_blk_get_device_specific_config,
	.start_device = virtio_blk_start,
	.stop_device = virtio_blk_stop,
};

int
vfu_virtio_blk_add_bdev(const char *name, const char *bdev_name,
			uint16_t num_queues, uint16_t qsize, bool packed_ring)
{
	struct spdk_vfu_endpoint *endpoint;
	struct vfu_virtio_endpoint *virtio_endpoint;
	struct virtio_blk_endpoint *blk_endpoint;
	int ret;

	endpoint = spdk_vfu_get_endpoint_by_name(name);
	if (!endpoint) {
		SPDK_ERRLOG("Endpoint %s doesn't exist\n", name);
		return -ENOENT;
	}

	virtio_endpoint = spdk_vfu_get_endpoint_private(endpoint);
	blk_endpoint = to_blk_endpoint(virtio_endpoint);

	if (blk_endpoint->bdev_desc) {
		SPDK_ERRLOG("%s: block device already exists\n", spdk_vfu_get_endpoint_id(endpoint));
		return -EEXIST;
	}

	if (num_queues && (num_queues <= VIRTIO_DEV_MAX_VQS)) {
		blk_endpoint->virtio.num_queues = num_queues;
	}
	if (qsize && (qsize <= VIRTIO_VQ_MAX_SIZE)) {
		blk_endpoint->virtio.qsize = qsize;
	}
	blk_endpoint->virtio.packed_ring = packed_ring;

	SPDK_DEBUGLOG(vfu_virtio_blk, "%s: add block device %s, num_queues %u, qsize %u, packed ring %s\n",
		      spdk_vfu_get_endpoint_id(endpoint),
		      bdev_name, blk_endpoint->virtio.num_queues, blk_endpoint->virtio.qsize,
		      packed_ring ? "enabled" : "disabled");

	ret = spdk_bdev_open_ext(bdev_name, true, bdev_event_cb, blk_endpoint,
				 &blk_endpoint->bdev_desc);
	if (ret != 0) {
		SPDK_ERRLOG("%s could not open bdev '%s', error=%d\n",
			    name, bdev_name, ret);
		return ret;
	}
	blk_endpoint->bdev = spdk_bdev_desc_get_bdev(blk_endpoint->bdev_desc);
	virtio_blk_update_config(&blk_endpoint->blk_cfg, blk_endpoint->bdev,
				 blk_endpoint->virtio.num_queues);
	blk_endpoint->init_thread = spdk_get_thread();

	return 0;
}

static int
vfu_virtio_blk_endpoint_destruct(struct spdk_vfu_endpoint *endpoint)
{
	struct vfu_virtio_endpoint *virtio_endpoint = spdk_vfu_get_endpoint_private(endpoint);
	struct virtio_blk_endpoint *blk_endpoint = to_blk_endpoint(virtio_endpoint);

	if (blk_endpoint->bdev_desc) {
		spdk_thread_send_msg(blk_endpoint->init_thread, _vfu_virtio_blk_bdev_close,
				     blk_endpoint->bdev_desc);
		blk_endpoint->bdev_desc = NULL;
	}

	vfu_virtio_endpoint_destruct(&blk_endpoint->virtio);
	free(blk_endpoint);

	return 0;
}

static void *
vfu_virtio_blk_endpoint_init(struct spdk_vfu_endpoint *endpoint,
			     char *basename, const char *endpoint_name)
{
	struct virtio_blk_endpoint *blk_endpoint;
	int ret;

	blk_endpoint = calloc(1, sizeof(*blk_endpoint));
	if (!blk_endpoint) {
		return NULL;
	}

	ret = vfu_virtio_endpoint_setup(&blk_endpoint->virtio, endpoint, basename, endpoint_name,
					&virtio_blk_ops);
	if (ret) {
		SPDK_ERRLOG("Error to setup endpoint %s\n", endpoint_name);
		free(blk_endpoint);
		return NULL;
	}

	return (void *)&blk_endpoint->virtio;
}

static int
vfu_virtio_blk_get_device_info(struct spdk_vfu_endpoint *endpoint,
			       struct spdk_vfu_pci_device *device_info)
{
	struct vfu_virtio_endpoint *virtio_endpoint = spdk_vfu_get_endpoint_private(endpoint);
	struct virtio_blk_endpoint *blk_endpoint = to_blk_endpoint(virtio_endpoint);

	vfu_virtio_get_device_info(&blk_endpoint->virtio, device_info);
	/* Fill Device ID */
	device_info->id.did = PCI_DEVICE_ID_VIRTIO_BLK_MODERN;

	return 0;
}

struct spdk_vfu_endpoint_ops vfu_virtio_blk_ops = {
	.name = "virtio_blk",
	.init = vfu_virtio_blk_endpoint_init,
	.get_device_info = vfu_virtio_blk_get_device_info,
	.get_vendor_capability = vfu_virtio_get_vendor_capability,
	.post_memory_add = vfu_virtio_post_memory_add,
	.pre_memory_remove = vfu_virtio_pre_memory_remove,
	.reset_device = vfu_virtio_pci_reset_cb,
	.quiesce_device = vfu_virtio_quiesce_cb,
	.destruct = vfu_virtio_blk_endpoint_destruct,
	.attach_device = vfu_virtio_attach_device,
	.detach_device = vfu_virtio_detach_device,
};

static void
__attribute__((constructor)) _vfu_virtio_blk_pci_model_register(void)
{
	spdk_vfu_register_endpoint_ops(&vfu_virtio_blk_ops);
}

SPDK_LOG_REGISTER_COMPONENT(vfu_virtio_blk)
