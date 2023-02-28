/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/json.h"

#include "spdk_internal/assert.h"
#include "spdk/bdev_module.h"
#include "spdk/log.h"
#include "spdk_internal/virtio.h"
#include "spdk_internal/vhost_user.h"

#include <linux/virtio_blk.h>
#include <linux/virtio_ids.h>

#include "bdev_virtio.h"

struct virtio_blk_dev {
	struct virtio_dev		vdev;
	struct spdk_bdev		bdev;
	bool				readonly;
	bool				unmap;
};

struct virtio_blk_io_ctx {
	struct iovec				iov_req;
	struct iovec				iov_resp;
	struct iovec				iov_unmap;
	struct virtio_blk_outhdr		req;
	struct virtio_blk_discard_write_zeroes	unmap;
	uint8_t					resp;
};

struct bdev_virtio_blk_io_channel {
	struct virtio_dev		*vdev;

	/** Virtqueue exclusively assigned to this channel. */
	struct virtqueue		*vq;

	/** Virtio response poller. */
	struct spdk_poller		*poller;
};

/* Features desired/implemented by this driver. */
#define VIRTIO_BLK_DEV_SUPPORTED_FEATURES		\
	(1ULL << VIRTIO_BLK_F_SIZE_MAX		|	\
	 1ULL << VIRTIO_BLK_F_SEG_MAX		|	\
	 1ULL << VIRTIO_BLK_F_BLK_SIZE		|	\
	 1ULL << VIRTIO_BLK_F_TOPOLOGY		|	\
	 1ULL << VIRTIO_BLK_F_MQ		|	\
	 1ULL << VIRTIO_BLK_F_RO		|	\
	 1ULL << VIRTIO_BLK_F_DISCARD		|	\
	 1ULL << VIRTIO_RING_F_EVENT_IDX)

/* 10 sec for max poll period */
#define VIRTIO_BLK_HOTPLUG_POLL_PERIOD_MAX		10000000ULL
/* Default poll period is 100ms */
#define VIRTIO_BLK_HOTPLUG_POLL_PERIOD_DEFAULT		100000ULL

static struct spdk_poller *g_blk_hotplug_poller = NULL;
static int g_blk_hotplug_fd = -1;

static int bdev_virtio_initialize(void);
static int bdev_virtio_blk_get_ctx_size(void);

static struct spdk_bdev_module virtio_blk_if = {
	.name = "virtio_blk",
	.module_init = bdev_virtio_initialize,
	.get_ctx_size = bdev_virtio_blk_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(virtio_blk, &virtio_blk_if)

static int bdev_virtio_blk_ch_create_cb(void *io_device, void *ctx_buf);
static void bdev_virtio_blk_ch_destroy_cb(void *io_device, void *ctx_buf);

static struct virtio_blk_io_ctx *
bdev_virtio_blk_init_io_vreq(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_blk_outhdr *req;
	uint8_t *resp;
	struct virtio_blk_discard_write_zeroes *desc;

	struct virtio_blk_io_ctx *io_ctx = (struct virtio_blk_io_ctx *)bdev_io->driver_ctx;

	req = &io_ctx->req;
	resp = &io_ctx->resp;
	desc = &io_ctx->unmap;

	io_ctx->iov_req.iov_base = req;
	io_ctx->iov_req.iov_len = sizeof(*req);

	io_ctx->iov_resp.iov_base = resp;
	io_ctx->iov_resp.iov_len = sizeof(*resp);

	io_ctx->iov_unmap.iov_base = desc;
	io_ctx->iov_unmap.iov_len = sizeof(*desc);

	memset(req, 0, sizeof(*req));
	return io_ctx;
}

static void
bdev_virtio_blk_send_io(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct bdev_virtio_blk_io_channel *virtio_channel = spdk_io_channel_get_ctx(ch);
	struct virtqueue *vq = virtio_channel->vq;
	struct virtio_blk_io_ctx *io_ctx = (struct virtio_blk_io_ctx *)bdev_io->driver_ctx;
	int rc;

	rc = virtqueue_req_start(vq, bdev_io, bdev_io->u.bdev.iovcnt + 2);
	if (rc == -ENOMEM) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		return;
	} else if (rc != 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	virtqueue_req_add_iovs(vq, &io_ctx->iov_req, 1, SPDK_VIRTIO_DESC_RO);
	if (bdev_io->type == SPDK_BDEV_IO_TYPE_UNMAP) {
		virtqueue_req_add_iovs(vq, &io_ctx->iov_unmap, 1, SPDK_VIRTIO_DESC_RO);
	} else {
		virtqueue_req_add_iovs(vq, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				       bdev_io->type == SPDK_BDEV_IO_TYPE_READ ?
				       SPDK_VIRTIO_DESC_WR : SPDK_VIRTIO_DESC_RO);
	}
	virtqueue_req_add_iovs(vq, &io_ctx->iov_resp, 1, SPDK_VIRTIO_DESC_WR);

	virtqueue_req_flush(vq);
}

static void
bdev_virtio_command(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_blk_io_ctx *io_ctx = bdev_virtio_blk_init_io_vreq(ch, bdev_io);
	struct virtio_blk_outhdr *req = &io_ctx->req;
	struct virtio_blk_discard_write_zeroes *desc = &io_ctx->unmap;

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		req->type = VIRTIO_BLK_T_IN;
	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		req->type = VIRTIO_BLK_T_OUT;
	} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_UNMAP) {
		req->type = VIRTIO_BLK_T_DISCARD;
		desc->sector = bdev_io->u.bdev.offset_blocks *
			       spdk_bdev_get_block_size(bdev_io->bdev) / 512;
		desc->num_sectors = bdev_io->u.bdev.num_blocks *
				    spdk_bdev_get_block_size(bdev_io->bdev) / 512;
		desc->flags = 0;
	}

	req->sector = bdev_io->u.bdev.offset_blocks *
		      spdk_bdev_get_block_size(bdev_io->bdev) / 512;

	bdev_virtio_blk_send_io(ch, bdev_io);
}

static void
bdev_virtio_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		       bool success)
{
	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	bdev_virtio_command(ch, bdev_io);
}

static int
_bdev_virtio_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_blk_dev *bvdev = bdev_io->bdev->ctxt;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_virtio_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return 0;
	case SPDK_BDEV_IO_TYPE_WRITE:
		if (bvdev->readonly) {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		} else {
			bdev_virtio_command(ch, bdev_io);
		}
		return 0;
	case SPDK_BDEV_IO_TYPE_RESET:
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		if (bvdev->unmap) {
			bdev_virtio_command(ch, bdev_io);
		} else {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
		return 0;
	case SPDK_BDEV_IO_TYPE_FLUSH:
	default:
		return -1;
	}

	SPDK_UNREACHABLE();
}

static void
bdev_virtio_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	if (_bdev_virtio_submit_request(ch, bdev_io) < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
bdev_virtio_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct virtio_blk_dev *bvdev = ctx;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_RESET:
		return true;
	case SPDK_BDEV_IO_TYPE_WRITE:
		return !bvdev->readonly;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		return bvdev->unmap;
	case SPDK_BDEV_IO_TYPE_FLUSH:
	default:
		return false;
	}
}

static struct spdk_io_channel *
bdev_virtio_get_io_channel(void *ctx)
{
	struct virtio_blk_dev *bvdev = ctx;

	return spdk_get_io_channel(bvdev);
}

static void
virtio_blk_dev_unregister_cb(void *io_device)
{
	struct virtio_blk_dev *bvdev = io_device;
	struct virtio_dev *vdev = &bvdev->vdev;

	virtio_dev_stop(vdev);
	virtio_dev_destruct(vdev);
	spdk_bdev_destruct_done(&bvdev->bdev, 0);
	free(bvdev);
}

static int
bdev_virtio_disk_destruct(void *ctx)
{
	struct virtio_blk_dev *bvdev = ctx;

	spdk_io_device_unregister(bvdev, virtio_blk_dev_unregister_cb);
	return 1;
}

int
bdev_virtio_blk_dev_remove(const char *name, bdev_virtio_remove_cb cb_fn, void *cb_arg)
{
	return spdk_bdev_unregister_by_name(name, &virtio_blk_if, cb_fn, cb_arg);
}

static int
bdev_virtio_dump_json_config(void *ctx, struct spdk_json_write_ctx *w)
{
	struct virtio_blk_dev *bvdev = ctx;

	virtio_dev_dump_json_info(&bvdev->vdev, w);
	return 0;
}

static void
bdev_virtio_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct virtio_blk_dev *bvdev = bdev->ctxt;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_virtio_attach_controller");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bvdev->vdev.name);
	spdk_json_write_named_string(w, "dev_type", "blk");

	/* Write transport specific parameters. */
	bvdev->vdev.backend_ops->write_json_config(&bvdev->vdev, w);

	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table virtio_fn_table = {
	.destruct		= bdev_virtio_disk_destruct,
	.submit_request		= bdev_virtio_submit_request,
	.io_type_supported	= bdev_virtio_io_type_supported,
	.get_io_channel		= bdev_virtio_get_io_channel,
	.dump_info_json		= bdev_virtio_dump_json_config,
	.write_config_json	= bdev_virtio_write_config_json,
};

static void
bdev_virtio_io_cpl(struct spdk_bdev_io *bdev_io)
{
	struct virtio_blk_io_ctx *io_ctx = (struct virtio_blk_io_ctx *)bdev_io->driver_ctx;

	spdk_bdev_io_complete(bdev_io, io_ctx->resp == VIRTIO_BLK_S_OK ?
			      SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);
}

static int
bdev_virtio_poll(void *arg)
{
	struct bdev_virtio_blk_io_channel *ch = arg;
	void *io[32];
	uint32_t io_len[32];
	uint16_t i, cnt;

	cnt = virtio_recv_pkts(ch->vq, io, io_len, SPDK_COUNTOF(io));
	for (i = 0; i < cnt; ++i) {
		bdev_virtio_io_cpl(io[i]);
	}

	return cnt;
}

static int
bdev_virtio_blk_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct virtio_blk_dev *bvdev = io_device;
	struct virtio_dev *vdev = &bvdev->vdev;
	struct bdev_virtio_blk_io_channel *ch = ctx_buf;
	struct virtqueue *vq;
	int32_t queue_idx;

	queue_idx = virtio_dev_find_and_acquire_queue(vdev, 0);
	if (queue_idx < 0) {
		SPDK_ERRLOG("Couldn't get an unused queue for the io_channel.\n");
		return -1;
	}

	vq = vdev->vqs[queue_idx];

	ch->vdev = vdev;
	ch->vq = vq;

	ch->poller = SPDK_POLLER_REGISTER(bdev_virtio_poll, ch, 0);
	return 0;
}

static void
bdev_virtio_blk_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct virtio_blk_dev *bvdev = io_device;
	struct virtio_dev *vdev = &bvdev->vdev;
	struct bdev_virtio_blk_io_channel *ch = ctx_buf;
	struct virtqueue *vq = ch->vq;

	spdk_poller_unregister(&ch->poller);
	virtio_dev_release_queue(vdev, vq->vq_queue_index);
}

static int
virtio_blk_dev_init(struct virtio_blk_dev *bvdev, uint16_t max_queues)
{
	struct virtio_dev *vdev = &bvdev->vdev;
	struct spdk_bdev *bdev = &bvdev->bdev;
	uint64_t capacity, num_blocks;
	uint32_t block_size, size_max, seg_max;
	uint16_t host_max_queues;
	int rc;

	if (virtio_dev_has_feature(vdev, VIRTIO_BLK_F_BLK_SIZE)) {
		rc = virtio_dev_read_dev_config(vdev, offsetof(struct virtio_blk_config, blk_size),
						&block_size, sizeof(block_size));
		if (rc) {
			SPDK_ERRLOG("%s: config read failed: %s\n", vdev->name, spdk_strerror(-rc));
			return rc;
		}

		if (block_size == 0 || block_size % 512 != 0) {
			SPDK_ERRLOG("%s: invalid block size (%"PRIu32"). Must be "
				    "a multiple of 512.\n", vdev->name, block_size);
			return -EIO;
		}
	} else {
		block_size = 512;
	}

	rc = virtio_dev_read_dev_config(vdev, offsetof(struct virtio_blk_config, capacity),
					&capacity, sizeof(capacity));
	if (rc) {
		SPDK_ERRLOG("%s: config read failed: %s\n", vdev->name, spdk_strerror(-rc));
		return rc;
	}

	/* `capacity` is a number of 512-byte sectors. */
	num_blocks = capacity * 512 / block_size;
	if (num_blocks == 0) {
		SPDK_ERRLOG("%s: size too small (size: %"PRIu64", blocksize: %"PRIu32").\n",
			    vdev->name, capacity * 512, block_size);
		return -EIO;
	}

	if ((capacity * 512) % block_size != 0) {
		SPDK_WARNLOG("%s: size has been rounded down to the nearest block size boundary. "
			     "(block size: %"PRIu32", previous size: %"PRIu64", new size: %"PRIu64")\n",
			     vdev->name, block_size, capacity * 512, num_blocks * block_size);
	}

	if (virtio_dev_has_feature(vdev, VIRTIO_BLK_F_MQ)) {
		rc = virtio_dev_read_dev_config(vdev, offsetof(struct virtio_blk_config, num_queues),
						&host_max_queues, sizeof(host_max_queues));
		if (rc) {
			SPDK_ERRLOG("%s: config read failed: %s\n", vdev->name, spdk_strerror(-rc));
			return rc;
		}
	} else {
		host_max_queues = 1;
	}

	if (virtio_dev_has_feature(vdev, VIRTIO_BLK_F_SIZE_MAX)) {
		rc = virtio_dev_read_dev_config(vdev, offsetof(struct virtio_blk_config, size_max),
						&size_max, sizeof(size_max));
		if (rc) {
			SPDK_ERRLOG("%s: config read failed: %s\n", vdev->name, spdk_strerror(-rc));
			return rc;
		}

		if (spdk_unlikely(size_max < block_size)) {
			SPDK_WARNLOG("%s: minimum segment size is set to block size %u forcefully.\n",
				     vdev->name, block_size);
			size_max = block_size;
		}

		bdev->max_segment_size = size_max;
	}

	if (virtio_dev_has_feature(vdev, VIRTIO_BLK_F_SEG_MAX)) {
		rc = virtio_dev_read_dev_config(vdev, offsetof(struct virtio_blk_config, seg_max),
						&seg_max, sizeof(seg_max));
		if (rc) {
			SPDK_ERRLOG("%s: config read failed: %s\n", vdev->name, spdk_strerror(-rc));
			return rc;
		}

		if (spdk_unlikely(seg_max == 0)) {
			SPDK_ERRLOG("%s: virtio blk SEG_MAX can't be 0\n", vdev->name);
			return -EINVAL;
		}

		bdev->max_num_segments = seg_max;
	}

	if (virtio_dev_has_feature(vdev, VIRTIO_BLK_F_RO)) {
		bvdev->readonly = true;
	}

	if (virtio_dev_has_feature(vdev, VIRTIO_BLK_F_DISCARD)) {
		bvdev->unmap = true;
	}

	if (max_queues == 0) {
		SPDK_ERRLOG("%s: requested 0 request queues (%"PRIu16" available).\n",
			    vdev->name, host_max_queues);
		return -EINVAL;
	}

	if (max_queues > host_max_queues) {
		SPDK_WARNLOG("%s: requested %"PRIu16" request queues "
			     "but only %"PRIu16" available.\n",
			     vdev->name, max_queues, host_max_queues);
		max_queues = host_max_queues;
	}

	/* bdev is tied with the virtio device; we can reuse the name */
	bdev->name = vdev->name;
	rc = virtio_dev_start(vdev, max_queues, 0);
	if (rc != 0) {
		return rc;
	}

	bdev->product_name = "VirtioBlk Disk";
	bdev->write_cache = 0;
	bdev->blocklen = block_size;
	bdev->blockcnt = num_blocks;

	bdev->ctxt = bvdev;
	bdev->fn_table = &virtio_fn_table;
	bdev->module = &virtio_blk_if;

	spdk_io_device_register(bvdev, bdev_virtio_blk_ch_create_cb,
				bdev_virtio_blk_ch_destroy_cb,
				sizeof(struct bdev_virtio_blk_io_channel),
				vdev->name);

	rc = spdk_bdev_register(bdev);
	if (rc) {
		SPDK_ERRLOG("Failed to register bdev name=%s\n", bdev->name);
		spdk_io_device_unregister(bvdev, NULL);
		virtio_dev_stop(vdev);
		return rc;
	}

	return 0;
}

static struct virtio_blk_dev *
virtio_pci_blk_dev_create(const char *name, struct virtio_pci_ctx *pci_ctx)
{
	static int pci_dev_counter = 0;
	struct virtio_blk_dev *bvdev;
	struct virtio_dev *vdev;
	char *default_name = NULL;
	uint16_t num_queues;
	int rc;

	bvdev = calloc(1, sizeof(*bvdev));
	if (bvdev == NULL) {
		SPDK_ERRLOG("virtio device calloc failed\n");
		return NULL;
	}
	vdev = &bvdev->vdev;

	if (name == NULL) {
		default_name = spdk_sprintf_alloc("VirtioBlk%"PRIu32, pci_dev_counter++);
		if (default_name == NULL) {
			free(vdev);
			return NULL;
		}
		name = default_name;
	}

	rc = virtio_pci_dev_init(vdev, name, pci_ctx);
	free(default_name);

	if (rc != 0) {
		free(bvdev);
		return NULL;
	}

	rc = virtio_dev_reset(vdev, VIRTIO_BLK_DEV_SUPPORTED_FEATURES);
	if (rc != 0) {
		goto fail;
	}

	/* TODO: add a way to limit usable virtqueues */
	if (virtio_dev_has_feature(vdev, VIRTIO_BLK_F_MQ)) {
		rc = virtio_dev_read_dev_config(vdev, offsetof(struct virtio_blk_config, num_queues),
						&num_queues, sizeof(num_queues));
		if (rc) {
			SPDK_ERRLOG("%s: config read failed: %s\n", vdev->name, spdk_strerror(-rc));
			goto fail;
		}
	} else {
		num_queues = 1;
	}

	rc = virtio_blk_dev_init(bvdev, num_queues);
	if (rc != 0) {
		goto fail;
	}

	return bvdev;

fail:
	vdev->ctx = NULL;
	virtio_dev_destruct(vdev);
	free(bvdev);
	return NULL;
}

static struct virtio_blk_dev *
virtio_user_blk_dev_create(const char *name, const char *path,
			   uint16_t num_queues, uint32_t queue_size)
{
	struct virtio_blk_dev *bvdev;
	uint64_t feature_bits;
	int rc;

	bvdev = calloc(1, sizeof(*bvdev));
	if (bvdev == NULL) {
		SPDK_ERRLOG("calloc failed for virtio device %s: %s\n", name, path);
		return NULL;
	}

	rc = virtio_user_dev_init(&bvdev->vdev, name, path, queue_size);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to create virito device %s: %s\n", name, path);
		free(bvdev);
		return NULL;
	}

	feature_bits = VIRTIO_BLK_DEV_SUPPORTED_FEATURES;
	feature_bits |= (1ULL << VHOST_USER_F_PROTOCOL_FEATURES);
	rc = virtio_dev_reset(&bvdev->vdev, feature_bits);
	if (rc != 0) {
		virtio_dev_destruct(&bvdev->vdev);
		free(bvdev);
		return NULL;
	}

	rc = virtio_blk_dev_init(bvdev, num_queues);
	if (rc != 0) {
		virtio_dev_destruct(&bvdev->vdev);
		free(bvdev);
		return NULL;
	}

	return bvdev;
}

struct bdev_virtio_pci_dev_create_ctx {
	const char *name;
	struct virtio_blk_dev *ret;
};

static int
bdev_virtio_pci_blk_dev_create_cb(struct virtio_pci_ctx *pci_ctx, void *ctx)
{
	struct bdev_virtio_pci_dev_create_ctx *create_ctx = ctx;

	create_ctx->ret = virtio_pci_blk_dev_create(create_ctx->name, pci_ctx);
	if (create_ctx->ret == NULL) {
		return -1;
	}

	return 0;
}

struct spdk_bdev *
bdev_virtio_pci_blk_dev_create(const char *name, struct spdk_pci_addr *pci_addr)
{
	struct bdev_virtio_pci_dev_create_ctx create_ctx;

	create_ctx.name = name;
	create_ctx.ret = NULL;

	virtio_pci_dev_attach(bdev_virtio_pci_blk_dev_create_cb, &create_ctx,
			      VIRTIO_ID_BLOCK, pci_addr);

	if (create_ctx.ret == NULL) {
		return NULL;
	}

	return &create_ctx.ret->bdev;
}

static int
bdev_virtio_pci_blk_monitor(void *arg)
{
	const char *vdev_name;
	struct bdev_virtio_pci_dev_create_ctx create_ctx;

	while ((vdev_name = virtio_pci_dev_event_process(g_blk_hotplug_fd, VIRTIO_ID_BLOCK)) != NULL) {
		bdev_virtio_blk_dev_remove(vdev_name, NULL, NULL);
	}

	/* Enumerate virtio pci_blk device */
	memset(&create_ctx, 0, sizeof(create_ctx));
	virtio_pci_dev_enumerate(bdev_virtio_pci_blk_dev_create_cb, &create_ctx,
				 VIRTIO_ID_BLOCK);

	return SPDK_POLLER_BUSY;
}

int
bdev_virtio_pci_blk_set_hotplug(bool enabled, uint64_t period_us)
{
	if (enabled == true && !spdk_process_is_primary()) {
		return -EPERM;
	}

	if (g_blk_hotplug_poller) {
		close(g_blk_hotplug_fd);
		spdk_poller_unregister(&g_blk_hotplug_poller);
	}

	if (!enabled) {
		return 0;
	}

	g_blk_hotplug_fd = spdk_pci_event_listen();
	if (g_blk_hotplug_fd < 0) {
		return g_blk_hotplug_fd;
	}

	period_us = period_us ? period_us : VIRTIO_BLK_HOTPLUG_POLL_PERIOD_DEFAULT;
	period_us = spdk_min(period_us, VIRTIO_BLK_HOTPLUG_POLL_PERIOD_MAX);
	g_blk_hotplug_poller = spdk_poller_register(bdev_virtio_pci_blk_monitor, NULL, period_us);
	if (!g_blk_hotplug_poller) {
		close(g_blk_hotplug_fd);
		return -1;
	}

	return 0;
}

static int
bdev_virtio_initialize(void)
{
	return 0;
}

struct spdk_bdev *
bdev_virtio_user_blk_dev_create(const char *name, const char *path,
				unsigned num_queues, unsigned queue_size)
{
	struct virtio_blk_dev *bvdev;

	bvdev = virtio_user_blk_dev_create(name, path, num_queues, queue_size);
	if (bvdev == NULL) {
		return NULL;
	}

	return &bvdev->bdev;
}

struct spdk_bdev *
bdev_virtio_vfio_user_blk_dev_create(const char *name, const char *path)
{
	struct virtio_blk_dev *bvdev;
	uint16_t num_queues = 0;
	int rc;

	bvdev = calloc(1, sizeof(*bvdev));
	if (bvdev == NULL) {
		SPDK_ERRLOG("calloc failed for virtio device %s: %s\n", name, path);
		return NULL;
	}

	rc = virtio_vfio_user_dev_init(&bvdev->vdev, name, path);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to create %s as virtio device\n", path);
		free(bvdev);
		return NULL;
	}

	rc = virtio_dev_reset(&bvdev->vdev, VIRTIO_BLK_DEV_SUPPORTED_FEATURES);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to reset %s as virtio device\n", path);
		virtio_dev_destruct(&bvdev->vdev);
		free(bvdev);
		return NULL;
	}

	if (virtio_dev_has_feature(&bvdev->vdev, VIRTIO_BLK_F_MQ)) {
		rc = virtio_dev_read_dev_config(&bvdev->vdev, offsetof(struct virtio_blk_config, num_queues),
						&num_queues, sizeof(num_queues));
		if (rc) {
			SPDK_ERRLOG("%s: config read failed: %s\n", name, spdk_strerror(-rc));
			virtio_dev_destruct(&bvdev->vdev);
			free(bvdev);
			return NULL;
		}
	} else {
		num_queues = 1;
	}

	rc = virtio_blk_dev_init(bvdev, num_queues);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to initialize %s as virtio device\n", path);
		virtio_dev_destruct(&bvdev->vdev);
		free(bvdev);
		return NULL;
	}

	return &bvdev->bdev;
}

static int
bdev_virtio_blk_get_ctx_size(void)
{
	return sizeof(struct virtio_blk_io_ctx);
}

SPDK_LOG_REGISTER_COMPONENT(virtio_blk)
