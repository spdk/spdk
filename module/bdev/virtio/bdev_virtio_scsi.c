/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
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

#include "spdk/bdev.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/scsi_spec.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/json.h"

#include "spdk/bdev_module.h"
#include "spdk/log.h"
#include "spdk_internal/virtio.h"
#include "spdk_internal/vhost_user.h"

#include <linux/virtio_scsi.h>
#include <linux/virtio_ids.h>

#include "bdev_virtio.h"

#define BDEV_VIRTIO_MAX_TARGET 64
#define BDEV_VIRTIO_SCAN_PAYLOAD_SIZE 256
#define MGMT_POLL_PERIOD_US (1000 * 5)
#define CTRLQ_RING_SIZE 16
#define SCAN_REQUEST_RETRIES 5

/* Number of non-request queues - eventq and controlq */
#define SPDK_VIRTIO_SCSI_QUEUE_NUM_FIXED 2

#define VIRTIO_SCSI_EVENTQ_BUFFER_COUNT 16

#define VIRTIO_SCSI_CONTROLQ	0
#define VIRTIO_SCSI_EVENTQ	1
#define VIRTIO_SCSI_REQUESTQ	2

static int bdev_virtio_initialize(void);
static void bdev_virtio_finish(void);

struct virtio_scsi_dev {
	/* Generic virtio device data. */
	struct virtio_dev		vdev;

	/** Detected SCSI LUNs */
	TAILQ_HEAD(, virtio_scsi_disk)	luns;

	/** Context for the SCSI target scan. */
	struct virtio_scsi_scan_base	*scan_ctx;

	/** Controlq poller. */
	struct spdk_poller		*mgmt_poller;

	/** Controlq messages to be sent. */
	struct spdk_ring		*ctrlq_ring;

	/** Buffers for the eventq. */
	struct virtio_scsi_eventq_io	*eventq_ios;

	/** Device marked for removal. */
	bool				removed;

	/** Callback to be called after vdev removal. */
	bdev_virtio_remove_cb		remove_cb;

	/** Context for the `remove_cb`. */
	void				*remove_ctx;

	TAILQ_ENTRY(virtio_scsi_dev) tailq;
};

struct virtio_scsi_io_ctx {
	struct iovec			iov_req;
	struct iovec			iov_resp;
	union {
		struct virtio_scsi_cmd_req req;
		struct virtio_scsi_ctrl_tmf_req tmf_req;
	};
	union {
		struct virtio_scsi_cmd_resp resp;
		struct virtio_scsi_ctrl_tmf_resp tmf_resp;
	};
};

struct virtio_scsi_eventq_io {
	struct iovec			iov;
	struct virtio_scsi_event	ev;
};

struct virtio_scsi_scan_info {
	uint64_t			num_blocks;
	uint32_t			block_size;
	uint8_t				target;
	bool				unmap_supported;
	TAILQ_ENTRY(virtio_scsi_scan_info) tailq;
};

struct virtio_scsi_scan_base {
	struct virtio_scsi_dev		*svdev;

	/** I/O channel used for the scan I/O. */
	struct bdev_virtio_io_channel	*channel;

	bdev_virtio_create_cb		cb_fn;
	void				*cb_arg;

	/** Scan all targets on the device. */
	bool				full_scan;

	/** Start a full rescan after receiving next scan I/O response. */
	bool				restart;

	/** Additional targets to be (re)scanned. */
	TAILQ_HEAD(, virtio_scsi_scan_info) scan_queue;

	/** Remaining attempts for sending the current request. */
	unsigned                        retries;

	/** If set, the last scan I/O needs to be resent */
	bool				needs_resend;

	struct virtio_scsi_io_ctx	io_ctx;
	struct iovec			iov;
	uint8_t				payload[BDEV_VIRTIO_SCAN_PAYLOAD_SIZE];

	/** Scan results for the current target. */
	struct virtio_scsi_scan_info	info;
};

struct virtio_scsi_disk {
	struct spdk_bdev		bdev;
	struct virtio_scsi_dev		*svdev;
	struct virtio_scsi_scan_info	info;

	/** Descriptor opened just to be notified of external bdev hotremove. */
	struct spdk_bdev_desc		*notify_desc;

	/** Disk marked for removal. */
	bool				removed;
	TAILQ_ENTRY(virtio_scsi_disk)	link;
};

struct bdev_virtio_io_channel {
	struct virtio_scsi_dev	*svdev;

	/** Virtqueue exclusively assigned to this channel. */
	struct virtqueue	*vq;

	/** Virtio response poller. */
	struct spdk_poller	*poller;
};

static TAILQ_HEAD(, virtio_scsi_dev) g_virtio_scsi_devs =
	TAILQ_HEAD_INITIALIZER(g_virtio_scsi_devs);

static pthread_mutex_t g_virtio_scsi_mutex = PTHREAD_MUTEX_INITIALIZER;

/** Module finish in progress */
static bool g_bdev_virtio_finish = false;

/* Features desired/implemented by this driver. */
#define VIRTIO_SCSI_DEV_SUPPORTED_FEATURES		\
	(1ULL << VIRTIO_SCSI_F_INOUT		|	\
	 1ULL << VIRTIO_SCSI_F_HOTPLUG		|	\
	 1ULL << VIRTIO_RING_F_EVENT_IDX	|	\
	 1ULL << VHOST_USER_F_PROTOCOL_FEATURES)

static void virtio_scsi_dev_unregister_cb(void *io_device);
static void virtio_scsi_dev_remove(struct virtio_scsi_dev *svdev,
				   bdev_virtio_remove_cb cb_fn, void *cb_arg);
static int bdev_virtio_scsi_ch_create_cb(void *io_device, void *ctx_buf);
static void bdev_virtio_scsi_ch_destroy_cb(void *io_device, void *ctx_buf);
static void process_scan_resp(struct virtio_scsi_scan_base *base);
static int bdev_virtio_mgmt_poll(void *arg);

static int
virtio_scsi_dev_send_eventq_io(struct virtqueue *vq, struct virtio_scsi_eventq_io *io)
{
	int rc;

	rc = virtqueue_req_start(vq, io, 1);
	if (rc != 0) {
		return -1;
	}

	virtqueue_req_add_iovs(vq, &io->iov, 1, SPDK_VIRTIO_DESC_WR);
	virtqueue_req_flush(vq);

	return 0;
}

static int
virtio_scsi_dev_init(struct virtio_scsi_dev *svdev, uint16_t max_queues)
{
	struct virtio_dev *vdev = &svdev->vdev;
	struct spdk_ring *ctrlq_ring;
	struct virtio_scsi_eventq_io *eventq_io;
	struct virtqueue *eventq;
	uint16_t i, num_events;
	int rc;

	rc = virtio_dev_reset(vdev, VIRTIO_SCSI_DEV_SUPPORTED_FEATURES);
	if (rc != 0) {
		return rc;
	}

	rc = virtio_dev_start(vdev, max_queues, SPDK_VIRTIO_SCSI_QUEUE_NUM_FIXED);
	if (rc != 0) {
		return rc;
	}

	ctrlq_ring = spdk_ring_create(SPDK_RING_TYPE_MP_SC, CTRLQ_RING_SIZE,
				      SPDK_ENV_SOCKET_ID_ANY);
	if (ctrlq_ring == NULL) {
		SPDK_ERRLOG("Failed to allocate send ring for the controlq.\n");
		return -1;
	}

	rc = virtio_dev_acquire_queue(vdev, VIRTIO_SCSI_CONTROLQ);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to acquire the controlq.\n");
		spdk_ring_free(ctrlq_ring);
		return -1;
	}

	rc = virtio_dev_acquire_queue(vdev, VIRTIO_SCSI_EVENTQ);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to acquire the eventq.\n");
		virtio_dev_release_queue(vdev, VIRTIO_SCSI_CONTROLQ);
		spdk_ring_free(ctrlq_ring);
		return -1;
	}

	eventq = vdev->vqs[VIRTIO_SCSI_EVENTQ];
	num_events = spdk_min(eventq->vq_nentries, VIRTIO_SCSI_EVENTQ_BUFFER_COUNT);
	svdev->eventq_ios = spdk_zmalloc(sizeof(*svdev->eventq_ios) * num_events,
					 0, NULL, SPDK_ENV_LCORE_ID_ANY,
					 SPDK_MALLOC_DMA);
	if (svdev->eventq_ios == NULL) {
		SPDK_ERRLOG("cannot allocate memory for %"PRIu16" eventq buffers\n",
			    num_events);
		virtio_dev_release_queue(vdev, VIRTIO_SCSI_EVENTQ);
		virtio_dev_release_queue(vdev, VIRTIO_SCSI_CONTROLQ);
		spdk_ring_free(ctrlq_ring);
		return -1;
	}

	for (i = 0; i < num_events; i++) {
		eventq_io = &svdev->eventq_ios[i];
		eventq_io->iov.iov_base = &eventq_io->ev;
		eventq_io->iov.iov_len = sizeof(eventq_io->ev);
		virtio_scsi_dev_send_eventq_io(eventq, eventq_io);
	}

	svdev->ctrlq_ring = ctrlq_ring;

	svdev->mgmt_poller = SPDK_POLLER_REGISTER(bdev_virtio_mgmt_poll, svdev,
			     MGMT_POLL_PERIOD_US);

	TAILQ_INIT(&svdev->luns);
	svdev->scan_ctx = NULL;
	svdev->removed = false;
	svdev->remove_cb = NULL;
	svdev->remove_ctx = NULL;

	spdk_io_device_register(svdev, bdev_virtio_scsi_ch_create_cb,
				bdev_virtio_scsi_ch_destroy_cb,
				sizeof(struct bdev_virtio_io_channel),
				svdev->vdev.name);

	pthread_mutex_lock(&g_virtio_scsi_mutex);
	TAILQ_INSERT_TAIL(&g_virtio_scsi_devs, svdev, tailq);
	pthread_mutex_unlock(&g_virtio_scsi_mutex);
	return 0;
}

static struct virtio_scsi_dev *
virtio_pci_scsi_dev_create(const char *name, struct virtio_pci_ctx *pci_ctx)
{
	static int pci_dev_counter = 0;
	struct virtio_scsi_dev *svdev;
	struct virtio_dev *vdev;
	char *default_name = NULL;
	uint32_t num_queues;
	int rc;

	svdev = calloc(1, sizeof(*svdev));
	if (svdev == NULL) {
		SPDK_ERRLOG("virtio device calloc failed\n");
		return NULL;
	}

	vdev = &svdev->vdev;
	if (name == NULL) {
		default_name = spdk_sprintf_alloc("VirtioScsi%"PRIu32, pci_dev_counter++);
		if (default_name == NULL) {
			free(vdev);
			return NULL;
		}
		name = default_name;
	}

	rc = virtio_pci_dev_init(vdev, name, pci_ctx);
	free(default_name);

	if (rc != 0) {
		free(svdev);
		return NULL;
	}

	rc = virtio_dev_read_dev_config(vdev, offsetof(struct virtio_scsi_config, num_queues),
					&num_queues, sizeof(num_queues));
	if (rc) {
		SPDK_ERRLOG("%s: config read failed: %s\n", vdev->name, spdk_strerror(-rc));
		goto fail;
	}

	rc = virtio_scsi_dev_init(svdev, num_queues);
	if (rc != 0) {
		goto fail;
	}

	return svdev;

fail:
	vdev->ctx = NULL;
	virtio_dev_destruct(vdev);
	free(svdev);
	return NULL;
}

static struct virtio_scsi_dev *
virtio_user_scsi_dev_create(const char *name, const char *path,
			    uint16_t num_queues, uint32_t queue_size)
{
	struct virtio_scsi_dev *svdev;
	struct virtio_dev *vdev;
	int rc;

	svdev = calloc(1, sizeof(*svdev));
	if (svdev == NULL) {
		SPDK_ERRLOG("calloc failed for virtio device %s: %s\n", name, path);
		return NULL;
	}

	vdev = &svdev->vdev;
	rc = virtio_user_dev_init(vdev, name, path, queue_size);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to create virito device %s: %s\n", name, path);
		free(svdev);
		return NULL;
	}

	rc = virtio_scsi_dev_init(svdev, num_queues);
	if (rc != 0) {
		virtio_dev_destruct(vdev);
		free(svdev);
		return NULL;
	}

	return svdev;
}

static struct virtio_scsi_disk *
virtio_scsi_dev_get_disk_by_id(struct virtio_scsi_dev *svdev, uint8_t target_id)
{
	struct virtio_scsi_disk *disk;

	TAILQ_FOREACH(disk, &svdev->luns, link) {
		if (disk->info.target == target_id) {
			return disk;
		}
	}

	return NULL;
}

static int virtio_scsi_dev_scan(struct virtio_scsi_dev *svdev,
				bdev_virtio_create_cb cb_fn, void *cb_arg);
static int send_scan_io(struct virtio_scsi_scan_base *base);
static void _virtio_scsi_dev_scan_tgt(struct virtio_scsi_scan_base *base, uint8_t target);
static int _virtio_scsi_dev_scan_next(struct virtio_scsi_scan_base *base, int rc);
static void _virtio_scsi_dev_scan_finish(struct virtio_scsi_scan_base *base, int errnum);
static int virtio_scsi_dev_scan_tgt(struct virtio_scsi_dev *svdev, uint8_t target);

static int
bdev_virtio_get_ctx_size(void)
{
	return sizeof(struct virtio_scsi_io_ctx);
}

static int
bdev_virtio_scsi_config_json(struct spdk_json_write_ctx *w)
{
	struct virtio_scsi_dev *svdev;

	pthread_mutex_lock(&g_virtio_scsi_mutex);
	TAILQ_FOREACH(svdev, &g_virtio_scsi_devs, tailq) {
		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "method", "bdev_virtio_attach_controller");

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "name", svdev->vdev.name);
		spdk_json_write_named_string(w, "dev_type", "scsi");

		/* Write transport specific parameters. */
		svdev->vdev.backend_ops->write_json_config(&svdev->vdev, w);

		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);

	}
	pthread_mutex_unlock(&g_virtio_scsi_mutex);

	return 0;
}


static struct spdk_bdev_module virtio_scsi_if = {
	.name = "virtio_scsi",
	.module_init = bdev_virtio_initialize,
	.module_fini = bdev_virtio_finish,
	.get_ctx_size = bdev_virtio_get_ctx_size,
	.config_json = bdev_virtio_scsi_config_json,
	.async_fini = true,
};

SPDK_BDEV_MODULE_REGISTER(virtio_scsi, &virtio_scsi_if)

static struct virtio_scsi_io_ctx *
bdev_virtio_init_io_vreq(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_scsi_cmd_req *req;
	struct virtio_scsi_cmd_resp *resp;
	struct virtio_scsi_disk *disk = (struct virtio_scsi_disk *)bdev_io->bdev;
	struct virtio_scsi_io_ctx *io_ctx = (struct virtio_scsi_io_ctx *)bdev_io->driver_ctx;

	req = &io_ctx->req;
	resp = &io_ctx->resp;

	io_ctx->iov_req.iov_base = req;
	io_ctx->iov_req.iov_len = sizeof(*req);

	io_ctx->iov_resp.iov_base = resp;
	io_ctx->iov_resp.iov_len = sizeof(*resp);

	memset(req, 0, sizeof(*req));
	req->lun[0] = 1;
	req->lun[1] = disk->info.target;

	return io_ctx;
}

static struct virtio_scsi_io_ctx *
bdev_virtio_init_tmf_vreq(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_scsi_ctrl_tmf_req *tmf_req;
	struct virtio_scsi_ctrl_tmf_resp *tmf_resp;
	struct virtio_scsi_disk *disk = SPDK_CONTAINEROF(bdev_io->bdev, struct virtio_scsi_disk, bdev);
	struct virtio_scsi_io_ctx *io_ctx = (struct virtio_scsi_io_ctx *)bdev_io->driver_ctx;

	tmf_req = &io_ctx->tmf_req;
	tmf_resp = &io_ctx->tmf_resp;

	io_ctx->iov_req.iov_base = tmf_req;
	io_ctx->iov_req.iov_len = sizeof(*tmf_req);
	io_ctx->iov_resp.iov_base = tmf_resp;
	io_ctx->iov_resp.iov_len = sizeof(*tmf_resp);

	memset(tmf_req, 0, sizeof(*tmf_req));
	tmf_req->lun[0] = 1;
	tmf_req->lun[1] = disk->info.target;

	return io_ctx;
}

static void
bdev_virtio_send_io(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct bdev_virtio_io_channel *virtio_channel = spdk_io_channel_get_ctx(ch);
	struct virtqueue *vq = virtio_channel->vq;
	struct virtio_scsi_io_ctx *io_ctx = (struct virtio_scsi_io_ctx *)bdev_io->driver_ctx;
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
	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		virtqueue_req_add_iovs(vq, &io_ctx->iov_resp, 1, SPDK_VIRTIO_DESC_WR);
		virtqueue_req_add_iovs(vq, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				       SPDK_VIRTIO_DESC_WR);
	} else {
		virtqueue_req_add_iovs(vq, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				       SPDK_VIRTIO_DESC_RO);
		virtqueue_req_add_iovs(vq, &io_ctx->iov_resp, 1, SPDK_VIRTIO_DESC_WR);
	}

	virtqueue_req_flush(vq);
}

static void
bdev_virtio_rw(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_scsi_disk *disk = SPDK_CONTAINEROF(bdev_io->bdev, struct virtio_scsi_disk, bdev);
	struct virtio_scsi_io_ctx *io_ctx = bdev_virtio_init_io_vreq(ch, bdev_io);
	struct virtio_scsi_cmd_req *req = &io_ctx->req;
	bool is_write = bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE;

	if (disk->info.num_blocks > (1ULL << 32)) {
		req->cdb[0] = is_write ? SPDK_SBC_WRITE_16 : SPDK_SBC_READ_16;
		to_be64(&req->cdb[2], bdev_io->u.bdev.offset_blocks);
		to_be32(&req->cdb[10], bdev_io->u.bdev.num_blocks);
	} else {
		req->cdb[0] = is_write ? SPDK_SBC_WRITE_10 : SPDK_SBC_READ_10;
		to_be32(&req->cdb[2], bdev_io->u.bdev.offset_blocks);
		to_be16(&req->cdb[7], bdev_io->u.bdev.num_blocks);
	}

	bdev_virtio_send_io(ch, bdev_io);
}

static void
bdev_virtio_reset(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct bdev_virtio_io_channel *virtio_ch = spdk_io_channel_get_ctx(ch);
	struct virtio_scsi_io_ctx *io_ctx = bdev_virtio_init_tmf_vreq(ch, bdev_io);
	struct virtio_scsi_ctrl_tmf_req *tmf_req = &io_ctx->tmf_req;
	struct virtio_scsi_dev *svdev = virtio_ch->svdev;
	size_t enqueued_count;

	tmf_req->type = VIRTIO_SCSI_T_TMF;
	tmf_req->subtype = VIRTIO_SCSI_T_TMF_LOGICAL_UNIT_RESET;

	enqueued_count = spdk_ring_enqueue(svdev->ctrlq_ring, (void **)&bdev_io, 1, NULL);
	if (spdk_likely(enqueued_count == 1)) {
		return;
	} else {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
	}
}

static void
bdev_virtio_unmap(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	struct virtio_scsi_io_ctx *io_ctx = bdev_virtio_init_io_vreq(ch, bdev_io);
	struct virtio_scsi_cmd_req *req = &io_ctx->req;
	struct spdk_scsi_unmap_bdesc *desc, *first_desc;
	uint8_t *buf;
	uint64_t offset_blocks, num_blocks;
	uint16_t cmd_len;

	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	buf = bdev_io->u.bdev.iovs[0].iov_base;

	offset_blocks = bdev_io->u.bdev.offset_blocks;
	num_blocks = bdev_io->u.bdev.num_blocks;

	/* (n-1) * 16-byte descriptors */
	first_desc = desc = (struct spdk_scsi_unmap_bdesc *)&buf[8];
	while (num_blocks > UINT32_MAX) {
		to_be64(&desc->lba, offset_blocks);
		to_be32(&desc->block_count, UINT32_MAX);
		memset(&desc->reserved, 0, sizeof(desc->reserved));
		offset_blocks += UINT32_MAX;
		num_blocks -= UINT32_MAX;
		desc++;
	}

	/* The last descriptor with block_count <= UINT32_MAX */
	to_be64(&desc->lba, offset_blocks);
	to_be32(&desc->block_count, num_blocks);
	memset(&desc->reserved, 0, sizeof(desc->reserved));

	/* 8-byte header + n * 16-byte block descriptor */
	cmd_len = 8 + (desc - first_desc + 1) *  sizeof(struct spdk_scsi_unmap_bdesc);

	req->cdb[0] = SPDK_SBC_UNMAP;
	to_be16(&req->cdb[7], cmd_len);

	/* 8-byte header */
	to_be16(&buf[0], cmd_len - 2); /* total length (excluding the length field) */
	to_be16(&buf[2], cmd_len - 8); /* length of block descriptors */
	memset(&buf[4], 0, 4); /* reserved */

	bdev_virtio_send_io(ch, bdev_io);
}

static void
bdev_virtio_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		       bool success)
{
	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	bdev_virtio_rw(ch, bdev_io);
}

static int _bdev_virtio_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_scsi_disk *disk = SPDK_CONTAINEROF(bdev_io->bdev, struct virtio_scsi_disk, bdev);

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_virtio_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return 0;
	case SPDK_BDEV_IO_TYPE_WRITE:
		bdev_virtio_rw(ch, bdev_io);
		return 0;
	case SPDK_BDEV_IO_TYPE_RESET:
		bdev_virtio_reset(ch, bdev_io);
		return 0;
	case SPDK_BDEV_IO_TYPE_UNMAP: {
		uint64_t buf_len = 8 /* header size */ +
				   (bdev_io->u.bdev.num_blocks + UINT32_MAX - 1) /
				   UINT32_MAX * sizeof(struct spdk_scsi_unmap_bdesc);

		if (!disk->info.unmap_supported) {
			return -1;
		}

		if (buf_len > SPDK_BDEV_LARGE_BUF_MAX_SIZE) {
			SPDK_ERRLOG("Trying to UNMAP too many blocks: %"PRIu64"\n",
				    bdev_io->u.bdev.num_blocks);
			return -1;
		}
		spdk_bdev_io_get_buf(bdev_io, bdev_virtio_unmap, buf_len);
		return 0;
	}
	case SPDK_BDEV_IO_TYPE_FLUSH:
	default:
		return -1;
	}
	return 0;
}

static void bdev_virtio_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	if (_bdev_virtio_submit_request(ch, bdev_io) < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
bdev_virtio_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct virtio_scsi_disk *disk = ctx;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
		return true;

	case SPDK_BDEV_IO_TYPE_UNMAP:
		return disk->info.unmap_supported;

	default:
		return false;
	}
}

static struct spdk_io_channel *
bdev_virtio_get_io_channel(void *ctx)
{
	struct virtio_scsi_disk *disk = ctx;

	return spdk_get_io_channel(disk->svdev);
}

static int
bdev_virtio_disk_destruct(void *ctx)
{
	struct virtio_scsi_disk *disk = ctx;
	struct virtio_scsi_dev *svdev = disk->svdev;

	TAILQ_REMOVE(&svdev->luns, disk, link);
	free(disk->bdev.name);
	free(disk);

	if (svdev->removed && TAILQ_EMPTY(&svdev->luns)) {
		spdk_io_device_unregister(svdev, virtio_scsi_dev_unregister_cb);
	}

	return 0;
}

static int
bdev_virtio_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct virtio_scsi_disk *disk = ctx;

	virtio_dev_dump_json_info(&disk->svdev->vdev, w);
	return 0;
}

static void
bdev_virtio_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/* SCSI targets and LUNS are discovered during scan process so nothing
	 * to save here.
	 */
}

static const struct spdk_bdev_fn_table virtio_fn_table = {
	.destruct		= bdev_virtio_disk_destruct,
	.submit_request		= bdev_virtio_submit_request,
	.io_type_supported	= bdev_virtio_io_type_supported,
	.get_io_channel		= bdev_virtio_get_io_channel,
	.dump_info_json		= bdev_virtio_dump_info_json,
	.write_config_json	= bdev_virtio_write_config_json,
};

static void
get_scsi_status(struct virtio_scsi_cmd_resp *resp, int *sk, int *asc, int *ascq)
{
	/* see spdk_scsi_task_build_sense_data() for sense data details */
	*sk = 0;
	*asc = 0;
	*ascq = 0;

	if (resp->sense_len < 3) {
		return;
	}

	*sk = resp->sense[2] & 0xf;

	if (resp->sense_len < 13) {
		return;
	}

	*asc = resp->sense[12];

	if (resp->sense_len < 14) {
		return;
	}

	*ascq = resp->sense[13];
}

static void
bdev_virtio_io_cpl(struct spdk_bdev_io *bdev_io)
{
	struct virtio_scsi_io_ctx *io_ctx = (struct virtio_scsi_io_ctx *)bdev_io->driver_ctx;
	int sk, asc, ascq;

	get_scsi_status(&io_ctx->resp, &sk, &asc, &ascq);
	spdk_bdev_io_complete_scsi_status(bdev_io, io_ctx->resp.status, sk, asc, ascq);
}

static int
bdev_virtio_poll(void *arg)
{
	struct bdev_virtio_io_channel *ch = arg;
	struct virtio_scsi_dev *svdev = ch->svdev;
	struct virtio_scsi_scan_base *scan_ctx = svdev->scan_ctx;
	void *io[32];
	uint32_t io_len[32];
	uint16_t i, cnt;
	int rc;

	cnt = virtio_recv_pkts(ch->vq, (void **)io, io_len, SPDK_COUNTOF(io));
	for (i = 0; i < cnt; ++i) {
		if (spdk_unlikely(scan_ctx && io[i] == &scan_ctx->io_ctx)) {
			if (svdev->removed) {
				_virtio_scsi_dev_scan_finish(scan_ctx, -EINTR);
				return SPDK_POLLER_BUSY;
			}

			if (scan_ctx->restart) {
				scan_ctx->restart = false;
				scan_ctx->full_scan = true;
				_virtio_scsi_dev_scan_tgt(scan_ctx, 0);
				continue;
			}

			process_scan_resp(scan_ctx);
			continue;
		}

		bdev_virtio_io_cpl(io[i]);
	}

	if (spdk_unlikely(scan_ctx && scan_ctx->needs_resend)) {
		if (svdev->removed) {
			_virtio_scsi_dev_scan_finish(scan_ctx, -EINTR);
			return SPDK_POLLER_BUSY;
		} else if (cnt == 0) {
			return SPDK_POLLER_IDLE;
		}

		rc = send_scan_io(scan_ctx);
		if (rc != 0) {
			assert(scan_ctx->retries > 0);
			scan_ctx->retries--;
			if (scan_ctx->retries == 0) {
				SPDK_ERRLOG("Target scan failed unrecoverably with rc = %d.\n", rc);
				_virtio_scsi_dev_scan_finish(scan_ctx, rc);
			}
		}
	}

	return cnt;
}

static void
bdev_virtio_tmf_cpl_cb(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;
	struct virtio_scsi_io_ctx *io_ctx = (struct virtio_scsi_io_ctx *)bdev_io->driver_ctx;

	if (io_ctx->tmf_resp.response == VIRTIO_SCSI_S_OK) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
bdev_virtio_tmf_cpl(struct spdk_bdev_io *bdev_io)
{
	spdk_thread_send_msg(spdk_bdev_io_get_thread(bdev_io), bdev_virtio_tmf_cpl_cb, bdev_io);
}

static void
bdev_virtio_eventq_io_cpl(struct virtio_scsi_dev *svdev, struct virtio_scsi_eventq_io *io)
{
	struct virtio_scsi_event *ev = &io->ev;
	struct virtio_scsi_disk *disk;

	if (ev->lun[0] != 1) {
		SPDK_WARNLOG("Received an event with invalid data layout.\n");
		goto out;
	}

	if (ev->event & VIRTIO_SCSI_T_EVENTS_MISSED) {
		ev->event &= ~VIRTIO_SCSI_T_EVENTS_MISSED;
		virtio_scsi_dev_scan(svdev, NULL, NULL);
	}

	switch (ev->event) {
	case VIRTIO_SCSI_T_NO_EVENT:
		break;
	case VIRTIO_SCSI_T_TRANSPORT_RESET:
		switch (ev->reason) {
		case VIRTIO_SCSI_EVT_RESET_RESCAN:
			virtio_scsi_dev_scan_tgt(svdev, ev->lun[1]);
			break;
		case VIRTIO_SCSI_EVT_RESET_REMOVED:
			disk = virtio_scsi_dev_get_disk_by_id(svdev, ev->lun[1]);
			if (disk != NULL) {
				spdk_bdev_unregister(&disk->bdev, NULL, NULL);
			}
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

out:
	virtio_scsi_dev_send_eventq_io(svdev->vdev.vqs[VIRTIO_SCSI_EVENTQ], io);
}

static void
bdev_virtio_tmf_abort_nomem_cb(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;

	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
}

static void
bdev_virtio_tmf_abort_ioerr_cb(void *ctx)
{
	struct spdk_bdev_io *bdev_io = ctx;

	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
}

static void
bdev_virtio_tmf_abort(struct spdk_bdev_io *bdev_io, int status)
{
	spdk_msg_fn fn;

	if (status == -ENOMEM) {
		fn = bdev_virtio_tmf_abort_nomem_cb;
	} else {
		fn = bdev_virtio_tmf_abort_ioerr_cb;
	}

	spdk_thread_send_msg(spdk_bdev_io_get_thread(bdev_io), fn, bdev_io);
}

static int
bdev_virtio_send_tmf_io(struct virtqueue *ctrlq, struct spdk_bdev_io *bdev_io)
{
	struct virtio_scsi_io_ctx *io_ctx = (struct virtio_scsi_io_ctx *)bdev_io->driver_ctx;
	int rc;

	rc = virtqueue_req_start(ctrlq, bdev_io, 2);
	if (rc != 0) {
		return rc;
	}

	virtqueue_req_add_iovs(ctrlq, &io_ctx->iov_req, 1, SPDK_VIRTIO_DESC_RO);
	virtqueue_req_add_iovs(ctrlq, &io_ctx->iov_resp, 1, SPDK_VIRTIO_DESC_WR);

	virtqueue_req_flush(ctrlq);
	return 0;
}

static int
bdev_virtio_mgmt_poll(void *arg)
{
	struct virtio_scsi_dev *svdev = arg;
	struct virtio_dev *vdev = &svdev->vdev;
	struct virtqueue *eventq = vdev->vqs[VIRTIO_SCSI_EVENTQ];
	struct virtqueue *ctrlq = vdev->vqs[VIRTIO_SCSI_CONTROLQ];
	struct spdk_ring *send_ring = svdev->ctrlq_ring;
	void *io[16];
	uint32_t io_len[16];
	uint16_t i, cnt;
	int rc;
	int total = 0;

	cnt = spdk_ring_dequeue(send_ring, io, SPDK_COUNTOF(io));
	total += cnt;
	for (i = 0; i < cnt; ++i) {
		rc = bdev_virtio_send_tmf_io(ctrlq, io[i]);
		if (rc != 0) {
			bdev_virtio_tmf_abort(io[i], rc);
		}
	}

	cnt = virtio_recv_pkts(ctrlq, io, io_len, SPDK_COUNTOF(io));
	total += cnt;
	for (i = 0; i < cnt; ++i) {
		bdev_virtio_tmf_cpl(io[i]);
	}

	cnt = virtio_recv_pkts(eventq, io, io_len, SPDK_COUNTOF(io));
	total += cnt;
	for (i = 0; i < cnt; ++i) {
		bdev_virtio_eventq_io_cpl(svdev, io[i]);
	}

	return total;
}

static int
bdev_virtio_scsi_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct virtio_scsi_dev *svdev = io_device;
	struct virtio_dev *vdev = &svdev->vdev;
	struct bdev_virtio_io_channel *ch = ctx_buf;
	struct virtqueue *vq;
	int32_t queue_idx;

	queue_idx = virtio_dev_find_and_acquire_queue(vdev, VIRTIO_SCSI_REQUESTQ);
	if (queue_idx < 0) {
		SPDK_ERRLOG("Couldn't get an unused queue for the io_channel.\n");
		return -1;
	}

	vq = vdev->vqs[queue_idx];

	ch->svdev = svdev;
	ch->vq = vq;

	ch->poller = SPDK_POLLER_REGISTER(bdev_virtio_poll, ch, 0);

	return 0;
}

static void
bdev_virtio_scsi_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_virtio_io_channel *ch = ctx_buf;
	struct virtio_scsi_dev *svdev = ch->svdev;
	struct virtio_dev *vdev = &svdev->vdev;
	struct virtqueue *vq = ch->vq;

	spdk_poller_unregister(&ch->poller);
	virtio_dev_release_queue(vdev, vq->vq_queue_index);
}

static void
_virtio_scsi_dev_scan_finish(struct virtio_scsi_scan_base *base, int errnum)
{
	struct virtio_scsi_dev *svdev = base->svdev;
	size_t bdevs_cnt;
	struct spdk_bdev *bdevs[BDEV_VIRTIO_MAX_TARGET];
	struct virtio_scsi_disk *disk;
	struct virtio_scsi_scan_info *tgt, *next_tgt;

	spdk_put_io_channel(spdk_io_channel_from_ctx(base->channel));
	base->svdev->scan_ctx = NULL;

	TAILQ_FOREACH_SAFE(tgt, &base->scan_queue, tailq, next_tgt) {
		TAILQ_REMOVE(&base->scan_queue, tgt, tailq);
		free(tgt);
	}

	if (base->cb_fn == NULL) {
		spdk_free(base);
		return;
	}

	bdevs_cnt = 0;
	if (errnum == 0) {
		TAILQ_FOREACH(disk, &svdev->luns, link) {
			bdevs[bdevs_cnt] = &disk->bdev;
			bdevs_cnt++;
		}
	}

	base->cb_fn(base->cb_arg, errnum, bdevs, bdevs_cnt);
	spdk_free(base);
}

static int
send_scan_io(struct virtio_scsi_scan_base *base)
{
	struct virtio_scsi_io_ctx *io_ctx = &base->io_ctx;
	struct virtio_scsi_cmd_req *req = &base->io_ctx.req;
	struct virtqueue *vq = base->channel->vq;
	int payload_iov_cnt = base->iov.iov_len > 0 ? 1 : 0;
	int rc;

	req->lun[0] = 1;
	req->lun[1] = base->info.target;

	rc = virtqueue_req_start(vq, io_ctx, 2 + payload_iov_cnt);
	if (rc != 0) {
		base->needs_resend = true;
		return -1;
	}

	virtqueue_req_add_iovs(vq, &io_ctx->iov_req, 1, SPDK_VIRTIO_DESC_RO);
	virtqueue_req_add_iovs(vq, &io_ctx->iov_resp, 1, SPDK_VIRTIO_DESC_WR);
	virtqueue_req_add_iovs(vq, &base->iov, payload_iov_cnt, SPDK_VIRTIO_DESC_WR);

	virtqueue_req_flush(vq);
	return 0;
}

static int
send_inquiry(struct virtio_scsi_scan_base *base)
{
	struct virtio_scsi_cmd_req *req = &base->io_ctx.req;
	struct spdk_scsi_cdb_inquiry *cdb;

	memset(req, 0, sizeof(*req));

	base->iov.iov_len = BDEV_VIRTIO_SCAN_PAYLOAD_SIZE;
	cdb = (struct spdk_scsi_cdb_inquiry *)req->cdb;
	cdb->opcode = SPDK_SPC_INQUIRY;
	to_be16(cdb->alloc_len, BDEV_VIRTIO_SCAN_PAYLOAD_SIZE);

	return send_scan_io(base);
}

static int
send_inquiry_vpd(struct virtio_scsi_scan_base *base, uint8_t page_code)
{
	struct virtio_scsi_cmd_req *req = &base->io_ctx.req;
	struct spdk_scsi_cdb_inquiry *inquiry_cdb = (struct spdk_scsi_cdb_inquiry *)req->cdb;

	memset(req, 0, sizeof(*req));

	base->iov.iov_len = BDEV_VIRTIO_SCAN_PAYLOAD_SIZE;
	inquiry_cdb->opcode = SPDK_SPC_INQUIRY;
	inquiry_cdb->evpd = 1;
	inquiry_cdb->page_code = page_code;
	to_be16(inquiry_cdb->alloc_len, base->iov.iov_len);

	return send_scan_io(base);
}

static int
send_read_cap_10(struct virtio_scsi_scan_base *base)
{
	struct virtio_scsi_cmd_req *req = &base->io_ctx.req;

	memset(req, 0, sizeof(*req));

	base->iov.iov_len = 8;
	req->cdb[0] = SPDK_SBC_READ_CAPACITY_10;

	return send_scan_io(base);
}

static int
send_read_cap_16(struct virtio_scsi_scan_base *base)
{
	struct virtio_scsi_cmd_req *req = &base->io_ctx.req;

	memset(req, 0, sizeof(*req));

	base->iov.iov_len = 32;
	req->cdb[0] = SPDK_SPC_SERVICE_ACTION_IN_16;
	req->cdb[1] = SPDK_SBC_SAI_READ_CAPACITY_16;
	to_be32(&req->cdb[10], base->iov.iov_len);

	return send_scan_io(base);
}

static int
send_test_unit_ready(struct virtio_scsi_scan_base *base)
{
	struct virtio_scsi_cmd_req *req = &base->io_ctx.req;

	memset(req, 0, sizeof(*req));
	req->cdb[0] = SPDK_SPC_TEST_UNIT_READY;
	base->iov.iov_len = 0;

	return send_scan_io(base);
}

static int
send_start_stop_unit(struct virtio_scsi_scan_base *base)
{
	struct virtio_scsi_cmd_req *req = &base->io_ctx.req;

	memset(req, 0, sizeof(*req));
	req->cdb[0] = SPDK_SBC_START_STOP_UNIT;
	req->cdb[4] = SPDK_SBC_START_STOP_UNIT_START_BIT;
	base->iov.iov_len = 0;

	return send_scan_io(base);
}

static int
process_scan_start_stop_unit(struct virtio_scsi_scan_base *base)
{
	struct virtio_scsi_cmd_resp *resp = &base->io_ctx.resp;

	if (resp->status == SPDK_SCSI_STATUS_GOOD) {
		return send_inquiry_vpd(base, SPDK_SPC_VPD_SUPPORTED_VPD_PAGES);
	}

	return -1;
}

static int
process_scan_test_unit_ready(struct virtio_scsi_scan_base *base)
{
	struct virtio_scsi_cmd_resp *resp = &base->io_ctx.resp;
	int sk, asc, ascq;

	get_scsi_status(resp, &sk, &asc, &ascq);

	/* check response, get VPD if spun up otherwise send SSU */
	if (resp->status == SPDK_SCSI_STATUS_GOOD) {
		return send_inquiry_vpd(base, SPDK_SPC_VPD_SUPPORTED_VPD_PAGES);
	} else if (resp->response == VIRTIO_SCSI_S_OK &&
		   resp->status == SPDK_SCSI_STATUS_CHECK_CONDITION &&
		   sk == SPDK_SCSI_SENSE_UNIT_ATTENTION &&
		   asc == SPDK_SCSI_ASC_LOGICAL_UNIT_NOT_READY) {
		return send_start_stop_unit(base);
	} else {
		return -1;
	}
}

static int
process_scan_inquiry_standard(struct virtio_scsi_scan_base *base)
{
	struct virtio_scsi_cmd_resp *resp = &base->io_ctx.resp;
	struct spdk_scsi_cdb_inquiry_data *inquiry_data =
		(struct spdk_scsi_cdb_inquiry_data *)base->payload;

	if (resp->status != SPDK_SCSI_STATUS_GOOD) {
		return -1;
	}

	/* check to make sure its a supported device */
	if (inquiry_data->peripheral_device_type != SPDK_SPC_PERIPHERAL_DEVICE_TYPE_DISK ||
	    inquiry_data->peripheral_qualifier != SPDK_SPC_PERIPHERAL_QUALIFIER_CONNECTED) {
		SPDK_WARNLOG("Unsupported peripheral device type 0x%02x (qualifier 0x%02x)\n",
			     inquiry_data->peripheral_device_type,
			     inquiry_data->peripheral_qualifier);
		return -1;
	}

	return send_test_unit_ready(base);
}

static int
process_scan_inquiry_vpd_supported_vpd_pages(struct virtio_scsi_scan_base *base)
{
	struct virtio_scsi_cmd_resp *resp = &base->io_ctx.resp;
	bool block_provisioning_page_supported = false;

	if (resp->status == SPDK_SCSI_STATUS_GOOD) {
		const uint8_t *vpd_data = base->payload;
		const uint8_t *supported_vpd_pages = vpd_data + 4;
		uint16_t page_length;
		uint16_t num_supported_pages;
		uint16_t i;

		page_length = from_be16(vpd_data + 2);
		num_supported_pages = spdk_min(page_length, base->iov.iov_len - 4);

		for (i = 0; i < num_supported_pages; i++) {
			if (supported_vpd_pages[i] == SPDK_SPC_VPD_BLOCK_THIN_PROVISION) {
				block_provisioning_page_supported = true;
				break;
			}
		}
	}

	if (block_provisioning_page_supported) {
		return send_inquiry_vpd(base, SPDK_SPC_VPD_BLOCK_THIN_PROVISION);
	} else {
		return send_read_cap_10(base);
	}
}

static int
process_scan_inquiry_vpd_block_thin_provision(struct virtio_scsi_scan_base *base)
{
	struct virtio_scsi_cmd_resp *resp = &base->io_ctx.resp;

	base->info.unmap_supported = false;

	if (resp->status == SPDK_SCSI_STATUS_GOOD) {
		uint8_t *vpd_data = base->payload;

		base->info.unmap_supported = !!(vpd_data[5] & SPDK_SCSI_UNMAP_LBPU);
	}

	SPDK_INFOLOG(virtio, "Target %u: unmap supported = %d\n",
		     base->info.target, (int)base->info.unmap_supported);

	return send_read_cap_10(base);
}

static int
process_scan_inquiry(struct virtio_scsi_scan_base *base)
{
	struct virtio_scsi_cmd_req *req = &base->io_ctx.req;
	struct spdk_scsi_cdb_inquiry *inquiry_cdb = (struct spdk_scsi_cdb_inquiry *)req->cdb;

	if ((inquiry_cdb->evpd & 1) == 0) {
		return process_scan_inquiry_standard(base);
	}

	switch (inquiry_cdb->page_code) {
	case SPDK_SPC_VPD_SUPPORTED_VPD_PAGES:
		return process_scan_inquiry_vpd_supported_vpd_pages(base);
	case SPDK_SPC_VPD_BLOCK_THIN_PROVISION:
		return process_scan_inquiry_vpd_block_thin_provision(base);
	default:
		SPDK_DEBUGLOG(virtio, "Unexpected VPD page 0x%02x\n", inquiry_cdb->page_code);
		return -1;
	}
}

static void
bdev_virtio_disk_notify_remove(struct virtio_scsi_disk *disk)
{
	disk->removed = true;
	spdk_bdev_close(disk->notify_desc);
}

static void
bdev_virtio_disk_notify_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
				 void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		bdev_virtio_disk_notify_remove(event_ctx);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

/* To be called only from the thread performing target scan */
static int
virtio_scsi_dev_add_tgt(struct virtio_scsi_dev *svdev, struct virtio_scsi_scan_info *info)
{
	struct virtio_scsi_disk *disk;
	struct spdk_bdev *bdev;
	int rc;

	TAILQ_FOREACH(disk, &svdev->luns, link) {
		if (disk->info.target == info->target) {
			/* Target is already attached and param change is not supported */
			return 0;
		}
	}

	if (info->block_size == 0 || info->num_blocks == 0) {
		SPDK_ERRLOG("%s: invalid target %u: bs=%"PRIu32" blocks=%"PRIu64"\n",
			    svdev->vdev.name, info->target, info->block_size, info->num_blocks);
		return -EINVAL;
	}

	disk = calloc(1, sizeof(*disk));
	if (disk == NULL) {
		SPDK_ERRLOG("could not allocate disk\n");
		return -ENOMEM;
	}

	disk->svdev = svdev;
	memcpy(&disk->info, info, sizeof(*info));

	bdev = &disk->bdev;
	bdev->name = spdk_sprintf_alloc("%st%"PRIu8, svdev->vdev.name, info->target);
	if (bdev->name == NULL) {
		SPDK_ERRLOG("Couldn't alloc memory for the bdev name.\n");
		free(disk);
		return -ENOMEM;
	}

	bdev->product_name = "Virtio SCSI Disk";
	bdev->write_cache = 0;
	bdev->blocklen = disk->info.block_size;
	bdev->blockcnt = disk->info.num_blocks;

	bdev->ctxt = disk;
	bdev->fn_table = &virtio_fn_table;
	bdev->module = &virtio_scsi_if;

	rc = spdk_bdev_register(&disk->bdev);
	if (rc) {
		SPDK_ERRLOG("Failed to register bdev name=%s\n", disk->bdev.name);
		free(bdev->name);
		free(disk);
		return rc;
	}

	rc = spdk_bdev_open_ext(bdev->name, false, bdev_virtio_disk_notify_event_cb,
				disk, &disk->notify_desc);
	if (rc) {
		assert(false);
	}

	TAILQ_INSERT_TAIL(&svdev->luns, disk, link);
	return 0;
}

static int
process_read_cap_10(struct virtio_scsi_scan_base *base)
{
	struct virtio_scsi_cmd_req *req = &base->io_ctx.req;
	struct virtio_scsi_cmd_resp *resp = &base->io_ctx.resp;
	uint64_t max_block;
	uint32_t block_size;
	uint8_t target_id = req->lun[1];
	int rc;

	if (resp->response != VIRTIO_SCSI_S_OK || resp->status != SPDK_SCSI_STATUS_GOOD) {
		SPDK_ERRLOG("READ CAPACITY (10) failed for target %"PRIu8".\n", target_id);
		return -1;
	}

	block_size = from_be32(base->payload + 4);
	max_block = from_be32(base->payload);

	if (max_block == 0xffffffff) {
		return send_read_cap_16(base);
	}

	base->info.num_blocks = (uint64_t)max_block + 1;
	base->info.block_size = block_size;

	rc = virtio_scsi_dev_add_tgt(base->svdev, &base->info);
	if (rc != 0) {
		return rc;
	}

	return _virtio_scsi_dev_scan_next(base, 0);
}

static int
process_read_cap_16(struct virtio_scsi_scan_base *base)
{
	struct virtio_scsi_cmd_req *req = &base->io_ctx.req;
	struct virtio_scsi_cmd_resp *resp = &base->io_ctx.resp;
	uint8_t target_id = req->lun[1];
	int rc;

	if (resp->response != VIRTIO_SCSI_S_OK || resp->status != SPDK_SCSI_STATUS_GOOD) {
		SPDK_ERRLOG("READ CAPACITY (16) failed for target %"PRIu8".\n", target_id);
		return -1;
	}

	base->info.num_blocks = from_be64(base->payload) + 1;
	base->info.block_size = from_be32(base->payload + 8);
	rc = virtio_scsi_dev_add_tgt(base->svdev, &base->info);
	if (rc != 0) {
		return rc;
	}

	return _virtio_scsi_dev_scan_next(base, 0);
}

static void
process_scan_resp(struct virtio_scsi_scan_base *base)
{
	struct virtio_scsi_cmd_req *req = &base->io_ctx.req;
	struct virtio_scsi_cmd_resp *resp = &base->io_ctx.resp;
	int rc, sk, asc, ascq;
	uint8_t target_id;

	if (base->io_ctx.iov_req.iov_len < sizeof(struct virtio_scsi_cmd_req) ||
	    base->io_ctx.iov_resp.iov_len < sizeof(struct virtio_scsi_cmd_resp)) {
		SPDK_ERRLOG("Received target scan message with invalid length.\n");
		_virtio_scsi_dev_scan_next(base, -EIO);
		return;
	}

	get_scsi_status(resp, &sk, &asc, &ascq);
	target_id = req->lun[1];

	if (resp->response == VIRTIO_SCSI_S_BAD_TARGET ||
	    resp->response == VIRTIO_SCSI_S_INCORRECT_LUN) {
		_virtio_scsi_dev_scan_next(base, -ENODEV);
		return;
	}

	if (resp->response != VIRTIO_SCSI_S_OK ||
	    (resp->status == SPDK_SCSI_STATUS_CHECK_CONDITION &&
	     sk != SPDK_SCSI_SENSE_ILLEGAL_REQUEST)) {
		assert(base->retries > 0);
		base->retries--;
		if (base->retries == 0) {
			SPDK_NOTICELOG("Target %"PRIu8" is present, but unavailable.\n", target_id);
			SPDK_LOGDUMP(virtio, "CDB", req->cdb, sizeof(req->cdb));
			SPDK_LOGDUMP(virtio, "SENSE DATA", resp->sense, sizeof(resp->sense));
			_virtio_scsi_dev_scan_next(base, -EBUSY);
			return;
		}

		/* resend the same request */
		rc = send_scan_io(base);
		if (rc != 0) {
			/* Let response poller do the resend */
		}
		return;
	}

	base->retries = SCAN_REQUEST_RETRIES;

	switch (req->cdb[0]) {
	case SPDK_SPC_INQUIRY:
		rc = process_scan_inquiry(base);
		break;
	case SPDK_SPC_TEST_UNIT_READY:
		rc = process_scan_test_unit_ready(base);
		break;
	case SPDK_SBC_START_STOP_UNIT:
		rc = process_scan_start_stop_unit(base);
		break;
	case SPDK_SBC_READ_CAPACITY_10:
		rc = process_read_cap_10(base);
		break;
	case SPDK_SPC_SERVICE_ACTION_IN_16:
		rc = process_read_cap_16(base);
		break;
	default:
		SPDK_ERRLOG("Received invalid target scan message: cdb[0] = %"PRIu8".\n", req->cdb[0]);
		rc = -1;
		break;
	}

	if (rc != 0) {
		if (base->needs_resend) {
			return; /* Let response poller do the resend */
		}

		_virtio_scsi_dev_scan_next(base, rc);
	}
}

static int
_virtio_scsi_dev_scan_next(struct virtio_scsi_scan_base *base, int rc)
{
	struct virtio_scsi_scan_info *next;
	struct virtio_scsi_disk *disk;
	uint8_t target_id;

	if (base->full_scan) {
		if (rc != 0) {
			disk = virtio_scsi_dev_get_disk_by_id(base->svdev,
							      base->info.target);
			if (disk != NULL) {
				spdk_bdev_unregister(&disk->bdev, NULL, NULL);
			}
		}

		target_id = base->info.target + 1;
		if (target_id < BDEV_VIRTIO_MAX_TARGET) {
			_virtio_scsi_dev_scan_tgt(base, target_id);
			return 0;
		}

		base->full_scan = false;
	}

	next = TAILQ_FIRST(&base->scan_queue);
	if (next == NULL) {
		_virtio_scsi_dev_scan_finish(base, 0);
		return 0;
	}

	TAILQ_REMOVE(&base->scan_queue, next, tailq);
	target_id = next->target;
	free(next);

	_virtio_scsi_dev_scan_tgt(base, target_id);
	return 0;
}

static int
_virtio_scsi_dev_scan_init(struct virtio_scsi_dev *svdev)
{
	struct virtio_scsi_scan_base *base;
	struct spdk_io_channel *io_ch;
	struct virtio_scsi_io_ctx *io_ctx;
	struct virtio_scsi_cmd_req *req;
	struct virtio_scsi_cmd_resp *resp;

	io_ch = spdk_get_io_channel(svdev);
	if (io_ch == NULL) {
		return -EBUSY;
	}

	base = spdk_zmalloc(sizeof(*base), 64, NULL,
			    SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (base == NULL) {
		SPDK_ERRLOG("couldn't allocate memory for scsi target scan.\n");
		return -ENOMEM;
	}

	base->svdev = svdev;

	base->channel = spdk_io_channel_get_ctx(io_ch);
	TAILQ_INIT(&base->scan_queue);
	svdev->scan_ctx = base;

	base->iov.iov_base = base->payload;
	io_ctx = &base->io_ctx;
	req = &io_ctx->req;
	resp = &io_ctx->resp;
	io_ctx->iov_req.iov_base = req;
	io_ctx->iov_req.iov_len = sizeof(*req);
	io_ctx->iov_resp.iov_base = resp;
	io_ctx->iov_resp.iov_len = sizeof(*resp);

	base->retries = SCAN_REQUEST_RETRIES;
	return 0;
}

static void
_virtio_scsi_dev_scan_tgt(struct virtio_scsi_scan_base *base, uint8_t target)
{
	int rc;

	memset(&base->info, 0, sizeof(base->info));
	base->info.target = target;

	rc = send_inquiry(base);
	if (rc) {
		/* Let response poller do the resend */
	}
}

static int
virtio_scsi_dev_scan(struct virtio_scsi_dev *svdev, bdev_virtio_create_cb cb_fn,
		     void *cb_arg)
{
	struct virtio_scsi_scan_base *base;
	struct virtio_scsi_scan_info *tgt, *next_tgt;
	int rc;

	if (svdev->scan_ctx) {
		if (svdev->scan_ctx->full_scan) {
			return -EEXIST;
		}

		/* We're about to start a full rescan, so there's no need
		 * to scan particular targets afterwards.
		 */
		TAILQ_FOREACH_SAFE(tgt, &svdev->scan_ctx->scan_queue, tailq, next_tgt) {
			TAILQ_REMOVE(&svdev->scan_ctx->scan_queue, tgt, tailq);
			free(tgt);
		}

		svdev->scan_ctx->cb_fn = cb_fn;
		svdev->scan_ctx->cb_arg = cb_arg;
		svdev->scan_ctx->restart = true;
		return 0;
	}

	rc = _virtio_scsi_dev_scan_init(svdev);
	if (rc != 0) {
		return rc;
	}

	base = svdev->scan_ctx;
	base->cb_fn = cb_fn;
	base->cb_arg = cb_arg;
	base->full_scan = true;

	_virtio_scsi_dev_scan_tgt(base, 0);
	return 0;
}

static int
virtio_scsi_dev_scan_tgt(struct virtio_scsi_dev *svdev, uint8_t target)
{
	struct virtio_scsi_scan_base *base;
	struct virtio_scsi_scan_info *info;
	int rc;

	base = svdev->scan_ctx;
	if (base) {
		info = calloc(1, sizeof(*info));
		if (info == NULL) {
			SPDK_ERRLOG("calloc failed\n");
			return -ENOMEM;
		}

		info->target = target;
		TAILQ_INSERT_TAIL(&base->scan_queue, info, tailq);
		return 0;
	}

	rc = _virtio_scsi_dev_scan_init(svdev);
	if (rc != 0) {
		return rc;
	}

	base = svdev->scan_ctx;
	base->full_scan = true;
	_virtio_scsi_dev_scan_tgt(base, target);
	return 0;
}

static int
bdev_virtio_initialize(void)
{
	return 0;
}

static void
_virtio_scsi_dev_unregister_cb(void *io_device)
{
	struct virtio_scsi_dev *svdev = io_device;
	struct virtio_dev *vdev = &svdev->vdev;
	bool finish_module;
	bdev_virtio_remove_cb remove_cb;
	void *remove_ctx;

	assert(spdk_ring_count(svdev->ctrlq_ring) == 0);
	spdk_ring_free(svdev->ctrlq_ring);
	spdk_poller_unregister(&svdev->mgmt_poller);

	virtio_dev_release_queue(vdev, VIRTIO_SCSI_EVENTQ);
	virtio_dev_release_queue(vdev, VIRTIO_SCSI_CONTROLQ);

	virtio_dev_stop(vdev);
	virtio_dev_destruct(vdev);

	pthread_mutex_lock(&g_virtio_scsi_mutex);
	TAILQ_REMOVE(&g_virtio_scsi_devs, svdev, tailq);
	pthread_mutex_unlock(&g_virtio_scsi_mutex);

	remove_cb = svdev->remove_cb;
	remove_ctx = svdev->remove_ctx;
	spdk_free(svdev->eventq_ios);
	free(svdev);

	if (remove_cb) {
		remove_cb(remove_ctx, 0);
	}

	finish_module = TAILQ_EMPTY(&g_virtio_scsi_devs);

	if (g_bdev_virtio_finish && finish_module) {
		spdk_bdev_module_fini_done();
	}
}

static void
virtio_scsi_dev_unregister_cb(void *io_device)
{
	struct virtio_scsi_dev *svdev = io_device;
	struct spdk_thread *thread;

	thread = virtio_dev_queue_get_thread(&svdev->vdev, VIRTIO_SCSI_CONTROLQ);
	spdk_thread_send_msg(thread, _virtio_scsi_dev_unregister_cb, io_device);
}

static void
virtio_scsi_dev_remove(struct virtio_scsi_dev *svdev,
		       bdev_virtio_remove_cb cb_fn, void *cb_arg)
{
	struct virtio_scsi_disk *disk, *disk_tmp;
	bool do_remove = true;

	if (svdev->removed) {
		if (cb_fn) {
			cb_fn(cb_arg, -EBUSY);
		}
		return;
	}

	svdev->remove_cb = cb_fn;
	svdev->remove_ctx = cb_arg;
	svdev->removed = true;

	if (svdev->scan_ctx) {
		/* The removal will continue after we receive a pending scan I/O. */
		return;
	}

	TAILQ_FOREACH_SAFE(disk, &svdev->luns, link, disk_tmp) {
		if (!disk->removed) {
			spdk_bdev_unregister(&disk->bdev, NULL, NULL);
		}
		do_remove = false;
	}

	if (do_remove) {
		spdk_io_device_unregister(svdev, virtio_scsi_dev_unregister_cb);
	}
}

static void
bdev_virtio_finish(void)
{
	struct virtio_scsi_dev *svdev, *next;

	g_bdev_virtio_finish = true;

	pthread_mutex_lock(&g_virtio_scsi_mutex);
	if (TAILQ_EMPTY(&g_virtio_scsi_devs)) {
		pthread_mutex_unlock(&g_virtio_scsi_mutex);
		spdk_bdev_module_fini_done();
		return;
	}

	/* Defer module finish until all controllers are removed. */
	TAILQ_FOREACH_SAFE(svdev, &g_virtio_scsi_devs, tailq, next) {
		virtio_scsi_dev_remove(svdev, NULL, NULL);
	}
	pthread_mutex_unlock(&g_virtio_scsi_mutex);
}

int
bdev_virtio_user_scsi_dev_create(const char *base_name, const char *path,
				 unsigned num_queues, unsigned queue_size,
				 bdev_virtio_create_cb cb_fn, void *cb_arg)
{
	struct virtio_scsi_dev *svdev;
	int rc;

	svdev = virtio_user_scsi_dev_create(base_name, path, num_queues, queue_size);
	if (svdev == NULL) {
		return -1;
	}

	rc = virtio_scsi_dev_scan(svdev, cb_fn, cb_arg);
	if (rc) {
		virtio_scsi_dev_remove(svdev, NULL, NULL);
	}

	return rc;
}

struct bdev_virtio_pci_dev_create_ctx {
	const char *name;
	bdev_virtio_create_cb cb_fn;
	void *cb_arg;
};

static int
bdev_virtio_pci_scsi_dev_create_cb(struct virtio_pci_ctx *pci_ctx, void *ctx)
{
	struct virtio_scsi_dev *svdev;
	struct bdev_virtio_pci_dev_create_ctx *create_ctx = ctx;
	int rc;

	svdev = virtio_pci_scsi_dev_create(create_ctx->name, pci_ctx);
	if (svdev == NULL) {
		return -1;
	}

	rc = virtio_scsi_dev_scan(svdev, create_ctx->cb_fn, create_ctx->cb_arg);
	if (rc) {
		svdev->vdev.ctx = NULL;
		virtio_scsi_dev_remove(svdev, NULL, NULL);
	}

	return rc;
}

int
bdev_virtio_pci_scsi_dev_create(const char *name, struct spdk_pci_addr *pci_addr,
				bdev_virtio_create_cb cb_fn, void *cb_arg)
{
	struct bdev_virtio_pci_dev_create_ctx create_ctx;

	create_ctx.name = name;
	create_ctx.cb_fn = cb_fn;
	create_ctx.cb_arg = cb_arg;

	return virtio_pci_dev_attach(bdev_virtio_pci_scsi_dev_create_cb, &create_ctx,
				     VIRTIO_ID_SCSI, pci_addr);
}

int
bdev_virtio_scsi_dev_remove(const char *name, bdev_virtio_remove_cb cb_fn, void *cb_arg)
{
	struct virtio_scsi_dev *svdev;

	pthread_mutex_lock(&g_virtio_scsi_mutex);
	TAILQ_FOREACH(svdev, &g_virtio_scsi_devs, tailq) {
		if (strcmp(svdev->vdev.name, name) == 0) {
			break;
		}
	}

	if (svdev == NULL) {
		pthread_mutex_unlock(&g_virtio_scsi_mutex);
		SPDK_ERRLOG("Cannot find Virtio-SCSI device named '%s'\n", name);
		return -ENODEV;
	}

	virtio_scsi_dev_remove(svdev, cb_fn, cb_arg);
	pthread_mutex_unlock(&g_virtio_scsi_mutex);

	return 0;
}

void
bdev_virtio_scsi_dev_list(struct spdk_json_write_ctx *w)
{
	struct virtio_scsi_dev *svdev;

	spdk_json_write_array_begin(w);

	pthread_mutex_lock(&g_virtio_scsi_mutex);
	TAILQ_FOREACH(svdev, &g_virtio_scsi_devs, tailq) {
		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "name", svdev->vdev.name);

		virtio_dev_dump_json_info(&svdev->vdev, w);

		spdk_json_write_object_end(w);
	}
	pthread_mutex_unlock(&g_virtio_scsi_mutex);

	spdk_json_write_array_end(w);
}

SPDK_LOG_REGISTER_COMPONENT(virtio)
