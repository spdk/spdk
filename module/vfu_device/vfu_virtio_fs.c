/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES
 *   All rights reserved.
 */

/*
 * virtio-fs over vfio-user transport
 */
#include <linux/virtio_fs.h>

#include "spdk/stdinc.h"
#include "spdk/env.h"
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
#include "spdk/fuse_dispatcher.h"
#include "linux/fuse_kernel.h"

#include "vfu_virtio_internal.h"

#define VIRTIO_FS_SUPPORTED_FEATURES 0

struct virtio_fs_endpoint {
	struct vfu_virtio_endpoint virtio;

	/* virtio_fs specific configurations */
	struct spdk_fuse_dispatcher *fuse_disp;
	struct spdk_thread *init_thread;
	struct spdk_io_channel *io_channel;
	struct virtio_fs_config	fs_cfg;

	/* virtio_fs ring process poller */
	struct spdk_poller *ring_poller;
};

struct virtio_fs_req {
	volatile uint32_t *status;
	struct virtio_fs_endpoint *endpoint;
	/* KEEP req at last */
	struct vfu_virtio_req req;
};

static inline struct virtio_fs_endpoint *
to_fs_endpoint(struct vfu_virtio_endpoint *virtio_endpoint)
{
	return SPDK_CONTAINEROF(virtio_endpoint, struct virtio_fs_endpoint, virtio);
}

static inline struct virtio_fs_req *
to_fs_request(struct vfu_virtio_req *request)
{
	return SPDK_CONTAINEROF(request, struct virtio_fs_req, req);
}

static int
vfu_virtio_fs_vring_poll(void *ctx)
{
	struct virtio_fs_endpoint *fs_endpoint = ctx;
	struct vfu_virtio_dev *dev = fs_endpoint->virtio.dev;
	struct vfu_virtio_vq *vq;
	uint32_t i, count = 0;

	if (spdk_unlikely(!virtio_dev_is_started(dev))) {
		return SPDK_POLLER_IDLE;
	}

	if (spdk_unlikely(fs_endpoint->virtio.quiesce_in_progress)) {
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
			count += vfu_virtio_dev_process_packed_ring(dev, vq);
		} else {
			/* split vring */
			count += vfu_virtio_dev_process_split_ring(dev, vq);
		}
	}

	return count ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static int
virtio_fs_start(struct vfu_virtio_endpoint *virtio_endpoint)
{
	struct virtio_fs_endpoint *fs_endpoint = to_fs_endpoint(virtio_endpoint);

	if (fs_endpoint->ring_poller) {
		return 0;
	}

	SPDK_DEBUGLOG(vfu_virtio_fs, "%s: starting...\n",
		      spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint));
	fs_endpoint->io_channel = spdk_fuse_dispatcher_get_io_channel(fs_endpoint->fuse_disp);
	if (!fs_endpoint->io_channel) {
		SPDK_ERRLOG("%s: failed to get primary IO channel\n",
			    spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint));
		return -EINVAL;
	}

	fs_endpoint->ring_poller = SPDK_POLLER_REGISTER(vfu_virtio_fs_vring_poll, fs_endpoint, 0);
	return 0;
}

static void
_virtio_fs_stop_msg(void *ctx)
{
	struct virtio_fs_endpoint *fs_endpoint = ctx;

	spdk_poller_unregister(&fs_endpoint->ring_poller);
	spdk_put_io_channel(fs_endpoint->io_channel);

	fs_endpoint->io_channel = NULL;

	SPDK_DEBUGLOG(vfu_virtio_fs, "%s is stopped\n",
		      spdk_vfu_get_endpoint_id(fs_endpoint->virtio.endpoint));
}

static int
virtio_fs_stop(struct vfu_virtio_endpoint *virtio_endpoint)
{
	struct virtio_fs_endpoint *fs_endpoint = to_fs_endpoint(virtio_endpoint);

	if (!fs_endpoint->io_channel) {
		return 0;
	}

	SPDK_DEBUGLOG(vfu_virtio_fs, "%s stopping\n", spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint));
	spdk_thread_send_msg(virtio_endpoint->thread, _virtio_fs_stop_msg, fs_endpoint);
	return 0;
}

static void
virtio_fs_req_finish(struct virtio_fs_req *fs_req, uint32_t status)
{
	struct vfu_virtio_req *req = &fs_req->req;

	if (spdk_likely(fs_req->status)) {
		*fs_req->status = status;
		fs_req->status = NULL;
	}

	vfu_virtio_finish_req(req);
}

static void
virtio_fs_fuse_req_done(void *cb_arg, int error)
{
	struct virtio_fs_req *fs_req = cb_arg;
	virtio_fs_req_finish(fs_req, -error);
}

static int
virtio_fs_process_req(struct vfu_virtio_endpoint *virtio_endpoint, struct vfu_virtio_vq *vq,
		      struct vfu_virtio_req *req)
{
	struct virtio_fs_endpoint *fs_endpoint = to_fs_endpoint(virtio_endpoint);
	struct virtio_fs_req *fs_req = to_fs_request(req);
	struct iovec *iov;
	const struct fuse_in_header *in;
	uint32_t in_len;
	struct iovec *in_iov, *out_iov;
	int in_iovcnt, out_iovcnt;

	fs_req->endpoint = fs_endpoint;

	in_iov = &req->iovs[0];
	in_iovcnt = 0;

	if (spdk_unlikely(in_iov[0].iov_len < sizeof(*in))) {
		SPDK_ERRLOG("Invalid virtio_fs IN header length %lu\n", in_iov[0].iov_len);
		virtio_fs_req_finish(fs_req, ENOTSUP);
		return -EINVAL;
	}

	in = in_iov->iov_base;
	in_len = 0;
	while (true) {
		iov = &req->iovs[in_iovcnt];
		in_len += iov->iov_len;
		in_iovcnt++;
		if (in_len == in->len) {
			break;
		} else if (in_len > in->len) {
			SPDK_ERRLOG("Invalid IOV array: length of %d elements >= %" PRIu32"\n", in_len, in->len);
			virtio_fs_req_finish(fs_req, ENOTSUP);
			return -EINVAL;
		}
	}

	out_iov = &req->iovs[in_iovcnt];
	out_iovcnt = req->iovcnt - in_iovcnt;

	spdk_fuse_dispatcher_submit_request(fs_endpoint->fuse_disp, fs_endpoint->io_channel,
					    in_iov, in_iovcnt, out_iov, out_iovcnt,
					    virtio_fs_fuse_req_done, fs_req);
	return 0;
}


static uint64_t
virtio_fs_get_supported_features(struct vfu_virtio_endpoint *virtio_endpoint)
{
	uint64_t features;

	features = VIRTIO_FS_SUPPORTED_FEATURES | VIRTIO_HOST_SUPPORTED_FEATURES;

	if (!virtio_endpoint->packed_ring) {
		features &= ~(1ULL << VIRTIO_F_RING_PACKED);
	}

	return features;
}

static struct vfu_virtio_req *
virtio_fs_alloc_req(struct vfu_virtio_endpoint *virtio_endpoint, struct vfu_virtio_vq *vq)
{
	struct virtio_fs_req *fs_req;

	fs_req = calloc(1, sizeof(*fs_req) + dma_sg_size() * (VIRTIO_DEV_MAX_IOVS + 1));
	if (!fs_req) {
		return NULL;
	}

	return &fs_req->req;
}

static void
virtio_fs_free_req(struct vfu_virtio_endpoint *virtio_endpoint, struct vfu_virtio_vq *vq,
		   struct vfu_virtio_req *req)
{
	struct virtio_fs_req *fs_req = to_fs_request(req);

	free(fs_req);
}

static int
virtio_fs_get_device_specific_config(struct vfu_virtio_endpoint *virtio_endpoint, char *buf,
				     uint64_t offset, uint64_t count)
{
	struct virtio_fs_endpoint *fs_endpoint = to_fs_endpoint(virtio_endpoint);
	uint8_t *fs_cfg;
	uint64_t len;

	SPDK_DEBUGLOG(vfu_virtio_fs, "%s: getting %" PRIu64 " config bytes at offset %" PRIu64
		      " (total: %zu)\n", spdk_vfu_get_endpoint_id(virtio_endpoint->endpoint),
		      count, offset, sizeof(struct virtio_fs_config));

	if (offset >= sizeof(struct virtio_fs_config)) {
		SPDK_WARNLOG("Offset is beyond the config size\n");
		return -EINVAL;
	}

	len = spdk_min(sizeof(struct virtio_fs_config) - offset, count);

	fs_cfg = (uint8_t *)&fs_endpoint->fs_cfg;
	memcpy(buf, fs_cfg + offset, len);

	return 0;
}

static struct vfu_virtio_ops virtio_fs_ops = {
	.get_device_features = virtio_fs_get_supported_features,
	.alloc_req = virtio_fs_alloc_req,
	.free_req = virtio_fs_free_req,
	.exec_request = virtio_fs_process_req,
	.get_config = virtio_fs_get_device_specific_config,
	.start_device = virtio_fs_start,
	.stop_device = virtio_fs_stop,
};

static void _vfu_virtio_fs_fuse_disp_delete(void *cb_arg);

static void
_vfu_virtio_fs_fuse_dispatcher_delete_cpl(void *cb_arg, int error)
{
	struct spdk_fuse_dispatcher *fuse_disp = cb_arg;

	if (error) {
		SPDK_ERRLOG("%s: FUSE dispatcher deletion failed with %d. Retrying...\n",
			    spdk_fuse_dispatcher_get_fsdev_name(fuse_disp), error);
		spdk_thread_send_msg(spdk_get_thread(), _vfu_virtio_fs_fuse_disp_delete, fuse_disp);
	}

	SPDK_NOTICELOG("FUSE dispatcher deleted\n");
}

static void
_vfu_virtio_fs_fuse_disp_delete(void *cb_arg)
{
	struct spdk_fuse_dispatcher *fuse_disp = cb_arg;
	int res;

	SPDK_DEBUGLOG(vfu_virtio_fs, "%s: initiating FUSE dispatcher deletion...\n",
		      spdk_fuse_dispatcher_get_fsdev_name(fuse_disp));

	res = spdk_fuse_dispatcher_delete(fuse_disp, _vfu_virtio_fs_fuse_dispatcher_delete_cpl, fuse_disp);
	if (res) {
		SPDK_ERRLOG("%s: FUSE dispatcher deletion failed with %d. Retrying...\n",
			    spdk_fuse_dispatcher_get_fsdev_name(fuse_disp), res);
		spdk_thread_send_msg(spdk_get_thread(), _vfu_virtio_fs_fuse_disp_delete, fuse_disp);
	}
}

static void
fuse_disp_event_cb(enum spdk_fuse_dispatcher_event_type type, struct spdk_fuse_dispatcher *disp,
		   void *event_ctx)
{
	struct virtio_fs_endpoint *fs_endpoint = event_ctx;

	SPDK_DEBUGLOG(vfu_virtio_fs, "%s: FUSE dispatcher event#%d arrived\n",
		      spdk_fuse_dispatcher_get_fsdev_name(fs_endpoint->fuse_disp), type);

	switch (type) {
	case SPDK_FUSE_DISP_EVENT_FSDEV_REMOVE:
		SPDK_NOTICELOG("%s: received SPDK_FUSE_DISP_EVENT_FSDEV_REMOVE\n",
			       spdk_fuse_dispatcher_get_fsdev_name(fs_endpoint->fuse_disp));
		memset(&fs_endpoint->fs_cfg, 0, sizeof(fs_endpoint->fs_cfg));

		if (fs_endpoint->io_channel) {
			spdk_thread_send_msg(fs_endpoint->virtio.thread, _virtio_fs_stop_msg, fs_endpoint);
		}

		if (fs_endpoint->fuse_disp) {
			spdk_thread_send_msg(fs_endpoint->init_thread, _vfu_virtio_fs_fuse_disp_delete,
					     fs_endpoint->fuse_disp);
			fs_endpoint->fuse_disp = NULL;
		}
		break;
	default:
		SPDK_NOTICELOG("%s: unsupported event type %d\n",
			       spdk_fuse_dispatcher_get_fsdev_name(fs_endpoint->fuse_disp), type);
		break;
	}
}

struct vfu_virtio_fs_add_fsdev_ctx {
	struct spdk_vfu_endpoint *endpoint;
	vfu_virtio_fs_add_fsdev_cpl_cb cb;
	void *cb_arg;
};

static void
fuse_dispatcher_create_cpl(void *cb_arg, struct spdk_fuse_dispatcher *disp)
{
	struct vfu_virtio_fs_add_fsdev_ctx *ctx = cb_arg;
	struct spdk_vfu_endpoint *endpoint = ctx->endpoint;
	struct vfu_virtio_endpoint *virtio_endpoint;
	struct virtio_fs_endpoint *fs_endpoint;

	if (!disp) {
		SPDK_ERRLOG("%s: failed to create SPDK FUSE dispatcher\n",
			    spdk_vfu_get_endpoint_id(endpoint));
		ctx->cb(ctx->cb_arg, -EINVAL);
		free(ctx);
		return;
	}

	virtio_endpoint = spdk_vfu_get_endpoint_private(endpoint);
	fs_endpoint = to_fs_endpoint(virtio_endpoint);

	fs_endpoint->fuse_disp = disp;

	SPDK_DEBUGLOG(vfu_virtio_fs, "%s: FUSE dispatcher created successfully\n",
		      spdk_fuse_dispatcher_get_fsdev_name(disp));

	ctx->cb(ctx->cb_arg, 0);
	free(ctx);
}

int
vfu_virtio_fs_add_fsdev(const char *name, const char *fsdev_name, const char *tag,
			uint16_t num_queues, uint16_t qsize, bool packed_ring,
			vfu_virtio_fs_add_fsdev_cpl_cb cb, void *cb_arg)
{
	struct spdk_vfu_endpoint *endpoint;
	struct vfu_virtio_endpoint *virtio_endpoint;
	struct virtio_fs_endpoint *fs_endpoint;
	struct vfu_virtio_fs_add_fsdev_ctx *ctx;
	size_t tag_len;
	int ret;

	if (!name || !fsdev_name || !tag) {
		SPDK_ERRLOG("name, fsdev_name and tag are mandatory\n");
		return -EINVAL;
	}

	endpoint = spdk_vfu_get_endpoint_by_name(name);
	if (!endpoint) {
		SPDK_ERRLOG("Endpoint %s doesn't exist\n", name);
		return -ENOENT;
	}

	virtio_endpoint = spdk_vfu_get_endpoint_private(endpoint);
	fs_endpoint = to_fs_endpoint(virtio_endpoint);

	if (fs_endpoint->fuse_disp) {
		SPDK_ERRLOG("%s: FUSE dispatcher already exists\n", spdk_vfu_get_endpoint_id(endpoint));
		return -EEXIST;
	}

	tag_len = strlen(tag);
	if (tag_len > sizeof(fs_endpoint->fs_cfg.tag)) {
		SPDK_ERRLOG("%s: tag is too long (%s, %zu > %zu)\n", spdk_vfu_get_endpoint_id(endpoint), tag,
			    tag_len, sizeof(fs_endpoint->fs_cfg.tag));
		return -EINVAL;
	}

	if (num_queues && (num_queues <= VIRTIO_DEV_MAX_VQS)) {
		fs_endpoint->virtio.num_queues = num_queues;
	}
	if (qsize && (qsize <= VIRTIO_VQ_MAX_SIZE)) {
		fs_endpoint->virtio.qsize = qsize;
	}
	fs_endpoint->virtio.packed_ring = packed_ring;

	SPDK_DEBUGLOG(vfu_virtio_fs, "%s: add fsdev %s, tag=%s, num_queues %u, qsize %u, packed ring %s\n",
		      spdk_vfu_get_endpoint_id(endpoint), fsdev_name, tag, fs_endpoint->virtio.num_queues,
		      fs_endpoint->virtio.qsize, packed_ring ? "enabled" : "disabled");

	/* Update config */
	memset(&fs_endpoint->fs_cfg, 0, sizeof(fs_endpoint->fs_cfg));
	fs_endpoint->fs_cfg.num_request_queues = fs_endpoint->virtio.num_queues -
			1; /* excluding the hprio */
	memcpy(fs_endpoint->fs_cfg.tag, tag, tag_len);
	fs_endpoint->init_thread = spdk_get_thread();

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("Failed to allocate context\n");
		return -ENOMEM;
	}

	ctx->endpoint = endpoint;
	ctx->cb = cb;
	ctx->cb_arg = cb_arg;

	ret = spdk_fuse_dispatcher_create(fsdev_name, fuse_disp_event_cb, fs_endpoint,
					  fuse_dispatcher_create_cpl, ctx);
	if (ret) {
		SPDK_ERRLOG("Failed to create SPDK FUSE dispatcher for %s (err=%d)\n",
			    fsdev_name, ret);
		free(ctx);
		return ret;
	}

	return 0;
}

static void *
vfu_virtio_fs_endpoint_init(struct spdk_vfu_endpoint *endpoint,
			    char *basename, const char *endpoint_name)
{
	struct virtio_fs_endpoint *fs_endpoint;
	int ret;

	fs_endpoint = calloc(1, sizeof(*fs_endpoint));
	if (!fs_endpoint) {
		return NULL;
	}

	ret = vfu_virtio_endpoint_setup(&fs_endpoint->virtio, endpoint, basename, endpoint_name,
					&virtio_fs_ops);
	if (ret) {
		SPDK_ERRLOG("Error to setup endpoint %s\n", endpoint_name);
		free(fs_endpoint);
		return NULL;
	}

	return (void *)&fs_endpoint->virtio;
}

static int
vfu_virtio_fs_endpoint_destruct(struct spdk_vfu_endpoint *endpoint)
{
	struct vfu_virtio_endpoint *virtio_endpoint = spdk_vfu_get_endpoint_private(endpoint);
	struct virtio_fs_endpoint *fs_endpoint = to_fs_endpoint(virtio_endpoint);

	if (fs_endpoint->fuse_disp) {
		if (fs_endpoint->init_thread == spdk_get_thread()) {
			_vfu_virtio_fs_fuse_disp_delete(fs_endpoint->fuse_disp);
		} else {
			spdk_thread_send_msg(spdk_get_thread(), _vfu_virtio_fs_fuse_disp_delete, fs_endpoint->fuse_disp);
		}
		fs_endpoint->fuse_disp = NULL;
	}

	vfu_virtio_endpoint_destruct(&fs_endpoint->virtio);
	free(fs_endpoint);

	return 0;
}

static int
vfu_virtio_fs_get_device_info(struct spdk_vfu_endpoint *endpoint,
			      struct spdk_vfu_pci_device *device_info)
{
	struct vfu_virtio_endpoint *virtio_endpoint = spdk_vfu_get_endpoint_private(endpoint);
	struct virtio_fs_endpoint *fs_endpoint = to_fs_endpoint(virtio_endpoint);

	vfu_virtio_get_device_info(&fs_endpoint->virtio, device_info);
	/* Fill Device ID */
	device_info->id.did = PCI_DEVICE_ID_VIRTIO_FS;

	return 0;
}

static struct spdk_vfu_endpoint_ops vfu_virtio_fs_ops = {
	.name = "virtio_fs",
	.init = vfu_virtio_fs_endpoint_init,
	.get_device_info = vfu_virtio_fs_get_device_info,
	.get_vendor_capability = vfu_virtio_get_vendor_capability,
	.post_memory_add = vfu_virtio_post_memory_add,
	.pre_memory_remove = vfu_virtio_pre_memory_remove,
	.reset_device = vfu_virtio_pci_reset_cb,
	.quiesce_device = vfu_virtio_quiesce_cb,
	.destruct = vfu_virtio_fs_endpoint_destruct,
	.attach_device = vfu_virtio_attach_device,
	.detach_device = vfu_virtio_detach_device,
};

static void
__attribute__((constructor)) _vfu_virtio_fs_pci_model_register(void)
{
	spdk_vfu_register_endpoint_ops(&vfu_virtio_fs_ops);
}

SPDK_LOG_REGISTER_COMPONENT(vfu_virtio_fs)
SPDK_LOG_REGISTER_COMPONENT(vfu_virtio_fs_data)
