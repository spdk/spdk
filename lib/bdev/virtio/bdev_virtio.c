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
#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/scsi_spec.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/json.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

#include <linux/virtio_scsi.h>

#include "rte_virtio/virtio.h"
#include "bdev_virtio.h"

#define BDEV_VIRTIO_MAX_TARGET 64
#define BDEV_VIRTIO_SCAN_PAYLOAD_SIZE 256
#define CTRLQ_POLL_PERIOD_US (1000 * 5)
#define CTRLQ_RING_SIZE 16
#define SCAN_REQUEST_RETRIES 5

#define VIRTIO_SCSI_CONTROLQ	0
#define VIRTIO_SCSI_EVENTQ	1
#define VIRTIO_SCSI_REQUESTQ	2

static int bdev_virtio_initialize(void);
static void bdev_virtio_finish(void);

struct virtio_scsi_io_ctx {
	struct virtio_req 		vreq;
	union {
		struct virtio_scsi_cmd_req req;
		struct virtio_scsi_ctrl_tmf_req tmf_req;
	};
	union {
		struct virtio_scsi_cmd_resp resp;
		struct virtio_scsi_ctrl_tmf_resp tmf_resp;
	};
};

struct virtio_scsi_scan_info {
	uint64_t			num_blocks;
	uint32_t			block_size;
	uint8_t				target;
	bool				unmap_supported;
};

struct virtio_scsi_scan_base {
	struct virtio_dev		*vdev;

	/** Virtqueue used for the scan I/O. */
	struct virtqueue		*vq;

	virtio_create_device_cb		cb_fn;
	void				*cb_arg;

	/* Currently queried target */
	unsigned			target;

	/* Disks to be registered after the scan finishes */
	TAILQ_HEAD(, virtio_scsi_disk) found_disks;

	/** Remaining attempts for sending the current request. */
	unsigned                        retries;

	struct virtio_scsi_io_ctx	io_ctx;
	struct iovec			iov;
	uint8_t				payload[BDEV_VIRTIO_SCAN_PAYLOAD_SIZE];

	/** Scan results for the current target. */
	struct virtio_scsi_scan_info	info;
};

struct virtio_scsi_disk {
	struct spdk_bdev		bdev;
	struct virtio_dev		*vdev;
	struct virtio_scsi_scan_info	info;
	TAILQ_ENTRY(virtio_scsi_disk)	link;
};

struct bdev_virtio_io_channel {
	struct virtio_dev	*vdev;

	/** Virtqueue exclusively assigned to this channel. */
	struct virtqueue	*vq;
};

static int scan_target(struct virtio_scsi_scan_base *base);

static int
bdev_virtio_get_ctx_size(void)
{
	return sizeof(struct virtio_scsi_io_ctx);
}

SPDK_BDEV_MODULE_REGISTER(virtio_scsi, bdev_virtio_initialize, bdev_virtio_finish,
			  NULL, bdev_virtio_get_ctx_size, NULL)

SPDK_BDEV_MODULE_ASYNC_INIT(virtio_scsi)
SPDK_BDEV_MODULE_ASYNC_FINI(virtio_scsi);

static struct virtio_req *
bdev_virtio_init_io_vreq(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_req *vreq;
	struct virtio_scsi_cmd_req *req;
	struct virtio_scsi_cmd_resp *resp;
	struct virtio_scsi_disk *disk = (struct virtio_scsi_disk *)bdev_io->bdev;
	struct virtio_scsi_io_ctx *io_ctx = (struct virtio_scsi_io_ctx *)bdev_io->driver_ctx;

	vreq = &io_ctx->vreq;
	req = &io_ctx->req;
	resp = &io_ctx->resp;

	vreq->iov_req.iov_base = req;
	vreq->iov_req.iov_len = sizeof(*req);

	vreq->iov_resp.iov_base = resp;
	vreq->iov_resp.iov_len = sizeof(*resp);

	vreq->is_write = bdev_io->type != SPDK_BDEV_IO_TYPE_READ;

	memset(req, 0, sizeof(*req));
	req->lun[0] = 1;
	req->lun[1] = disk->info.target;

	return vreq;
}

static struct virtio_req *
bdev_virtio_init_tmf_vreq(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_req *vreq;
	struct virtio_scsi_ctrl_tmf_req *tmf_req;
	struct virtio_scsi_ctrl_tmf_resp *tmf_resp;
	struct virtio_scsi_disk *disk = SPDK_CONTAINEROF(bdev_io->bdev, struct virtio_scsi_disk, bdev);
	struct virtio_scsi_io_ctx *io_ctx = (struct virtio_scsi_io_ctx *)bdev_io->driver_ctx;

	vreq = &io_ctx->vreq;
	tmf_req = &io_ctx->tmf_req;
	tmf_resp = &io_ctx->tmf_resp;

	vreq->iov = NULL;
	vreq->iov_req.iov_base = tmf_req;
	vreq->iov_req.iov_len = sizeof(*tmf_req);
	vreq->iov_resp.iov_base = tmf_resp;
	vreq->iov_resp.iov_len = sizeof(*tmf_resp);
	vreq->iovcnt = 0;
	vreq->is_write = false;

	memset(tmf_req, 0, sizeof(*tmf_req));
	tmf_req->lun[0] = 1;
	tmf_req->lun[1] = disk->info.target;

	return vreq;
}

static void
bdev_virtio_send_io(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct bdev_virtio_io_channel *virtio_channel = spdk_io_channel_get_ctx(ch);
	struct virtio_scsi_io_ctx *io_ctx = (struct virtio_scsi_io_ctx *)bdev_io->driver_ctx;
	struct virtio_req *vreq = &io_ctx->vreq;
	int rc;

	rc = virtio_xmit_pkt(virtio_channel->vq, vreq);
	if (spdk_likely(rc == 0)) {
		return;
	} else if (rc == -ENOMEM) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
	} else {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
bdev_virtio_rw(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_scsi_disk *disk = SPDK_CONTAINEROF(bdev_io->bdev, struct virtio_scsi_disk, bdev);
	struct virtio_req *vreq = bdev_virtio_init_io_vreq(ch, bdev_io);
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;

	vreq->iov = bdev_io->u.bdev.iovs;
	vreq->iovcnt = bdev_io->u.bdev.iovcnt;

	if (disk->info.num_blocks > (1ULL << 32)) {
		req->cdb[0] = vreq->is_write ? SPDK_SBC_WRITE_16 : SPDK_SBC_READ_16;
		to_be64(&req->cdb[2], bdev_io->u.bdev.offset_blocks);
		to_be32(&req->cdb[10], bdev_io->u.bdev.num_blocks);
	} else {
		req->cdb[0] = vreq->is_write ? SPDK_SBC_WRITE_10 : SPDK_SBC_READ_10;
		to_be32(&req->cdb[2], bdev_io->u.bdev.offset_blocks);
		to_be16(&req->cdb[7], bdev_io->u.bdev.num_blocks);
	}

	bdev_virtio_send_io(ch, bdev_io);
}

static void
bdev_virtio_reset(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct bdev_virtio_io_channel *virtio_ch = spdk_io_channel_get_ctx(ch);
	struct virtio_req *vreq = bdev_virtio_init_tmf_vreq(ch, bdev_io);
	struct virtio_scsi_ctrl_tmf_req *tmf_req = vreq->iov_req.iov_base;
	struct spdk_ring *ctrlq_send_ring = virtio_ch->vdev->vqs[VIRTIO_SCSI_CONTROLQ]->poller_ctx;
	size_t enqueued_count;

	tmf_req->type = VIRTIO_SCSI_T_TMF;
	tmf_req->subtype = VIRTIO_SCSI_T_TMF_LOGICAL_UNIT_RESET;

	enqueued_count = spdk_ring_enqueue(ctrlq_send_ring, (void **)&vreq, 1);
	if (spdk_likely(enqueued_count == 1)) {
		return;
	} else {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
	}
}

static void
bdev_virtio_unmap(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_req *vreq = bdev_virtio_init_io_vreq(ch, bdev_io);
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;
	struct spdk_scsi_unmap_bdesc *desc, *first_desc;
	uint8_t *buf;
	uint64_t offset_blocks, num_blocks;
	uint16_t cmd_len;

	vreq->iov = bdev_io->u.bdev.iovs;
	vreq->iovcnt = bdev_io->u.bdev.iovcnt;
	buf = vreq->iov->iov_base;

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

static int _bdev_virtio_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_scsi_disk *disk = SPDK_CONTAINEROF(bdev_io->bdev, struct virtio_scsi_disk, bdev);

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_virtio_rw,
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

	return spdk_get_io_channel(disk->vdev);
}

static int
bdev_virtio_destruct(void *ctx)
{
	return 0;
}

static int
bdev_virtio_dump_json_config(void *ctx, struct spdk_json_write_ctx *w)
{
	struct virtio_scsi_disk *disk = ctx;

	virtio_dev_dump_json_config(disk->vdev, w);
	return 0;
}

static const struct spdk_bdev_fn_table virtio_fn_table = {
	.destruct		= bdev_virtio_destruct,
	.submit_request		= bdev_virtio_submit_request,
	.io_type_supported	= bdev_virtio_io_type_supported,
	.get_io_channel		= bdev_virtio_get_io_channel,
	.dump_config_json	= bdev_virtio_dump_json_config,
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
bdev_virtio_io_cpl(struct virtio_req *req)
{
	struct virtio_scsi_io_ctx *io_ctx = SPDK_CONTAINEROF(req, struct virtio_scsi_io_ctx, vreq);
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(io_ctx);
	int sk, asc, ascq;

	get_scsi_status(&io_ctx->resp, &sk, &asc, &ascq);
	spdk_bdev_io_complete_scsi_status(bdev_io, io_ctx->resp.status, sk, asc, ascq);
}

static void
bdev_virtio_poll(void *arg)
{
	struct bdev_virtio_io_channel *ch = arg;
	struct virtio_req *req[32];
	uint16_t i, cnt;

	cnt = virtio_recv_pkts(ch->vq, req, SPDK_COUNTOF(req));
	for (i = 0; i < cnt; ++i) {
		bdev_virtio_io_cpl(req[i]);
	}
}

static void
bdev_virtio_tmf_cpl_cb(void *ctx)
{
	struct virtio_req *req = ctx;
	struct virtio_scsi_io_ctx *io_ctx = SPDK_CONTAINEROF(req, struct virtio_scsi_io_ctx, vreq);
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(io_ctx);

	if (io_ctx->tmf_resp.response == VIRTIO_SCSI_S_OK) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
bdev_virtio_tmf_cpl(struct virtio_req *req)
{
	struct virtio_scsi_io_ctx *io_ctx = SPDK_CONTAINEROF(req, struct virtio_scsi_io_ctx, vreq);
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(io_ctx);

	spdk_thread_send_msg(spdk_bdev_io_get_thread(bdev_io), bdev_virtio_tmf_cpl_cb, req);
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
bdev_virtio_tmf_abort(struct virtio_req *req, int status)
{
	struct virtio_scsi_io_ctx *io_ctx = SPDK_CONTAINEROF(req, struct virtio_scsi_io_ctx, vreq);
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(io_ctx);
	spdk_thread_fn fn;

	if (status == -ENOMEM) {
		fn = bdev_virtio_tmf_abort_nomem_cb;
	} else {
		fn = bdev_virtio_tmf_abort_ioerr_cb;
	}

	spdk_thread_send_msg(spdk_bdev_io_get_thread(bdev_io), fn, bdev_io);
}

static void
bdev_virtio_ctrlq_poll(void *arg)
{
	struct virtio_dev *vdev = arg;
	struct virtqueue *ctrlq = vdev->vqs[VIRTIO_SCSI_CONTROLQ];
	struct spdk_ring *send_ring = ctrlq->poller_ctx;
	struct virtio_req *req[16];
	uint16_t i, cnt;
	int rc;

	cnt = spdk_ring_dequeue(send_ring, (void **)req, SPDK_COUNTOF(req));
	for (i = 0; i < cnt; ++i) {
		rc = virtio_xmit_pkt(ctrlq, req[i]);
		if (rc != 0) {
			bdev_virtio_tmf_abort(req[i], rc);
		}
	}

	cnt = virtio_recv_pkts(ctrlq, req, SPDK_COUNTOF(req));
	for (i = 0; i < cnt; ++i) {
		bdev_virtio_tmf_cpl(req[i]);
	}
}

static int
bdev_virtio_create_cb(void *io_device, void *ctx_buf)
{
	struct virtio_dev *vdev = io_device;
	struct bdev_virtio_io_channel *ch = ctx_buf;
	struct virtqueue *vq;
	int32_t queue_idx;

	queue_idx = virtio_dev_find_and_acquire_queue(vdev, VIRTIO_SCSI_REQUESTQ);
	if (queue_idx < 0) {
		SPDK_ERRLOG("Couldn't get an unused queue for the io_channel.\n");
		return -1;
	}

	vq = vdev->vqs[queue_idx];

	ch->vdev = vdev;
	ch->vq = vq;

	vq->poller = spdk_poller_register(bdev_virtio_poll, ch, 0);

	return 0;
}

static void
bdev_virtio_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_virtio_io_channel *io_channel = ctx_buf;
	struct virtio_dev *vdev = io_channel->vdev;
	struct virtqueue *vq = io_channel->vq;

	spdk_poller_unregister(&vq->poller);
	virtio_dev_release_queue(vdev, vq->vq_queue_index);
}

static void
scan_target_abort(struct virtio_scsi_scan_base *base, int error)
{
	struct virtio_scsi_disk *disk;

	while ((disk = TAILQ_FIRST(&base->found_disks))) {
		TAILQ_REMOVE(&base->found_disks, disk, link);
		free(disk);
	}

	TAILQ_REMOVE(&g_virtio_driver.init_ctrlrs, base->vdev, tailq);
	virtio_dev_reset(base->vdev);
	virtio_dev_free(base->vdev);


	if (base->cb_fn) {
		base->cb_fn(base->cb_arg, error, NULL, 0);
	}

	spdk_dma_free(base);
}

static void
scan_target_finish(struct virtio_scsi_scan_base *base)
{
	size_t bdevs_cnt = 0;
	struct spdk_bdev *bdevs[BDEV_VIRTIO_MAX_TARGET];
	struct virtio_scsi_disk *disk;
	struct virtqueue *ctrlq;
	struct spdk_ring *ctrlq_ring;
	int rc;

	base->target++;
	if (base->target < BDEV_VIRTIO_MAX_TARGET) {
		rc = scan_target(base);
		if (rc != 0) {
			assert(false);
		}
		return;
	}

	spdk_poller_unregister(&base->vq->poller);
	base->vq->poller_ctx = NULL;
	virtio_dev_release_queue(base->vdev, base->vq->vq_queue_index);

	ctrlq_ring = spdk_ring_create(SPDK_RING_TYPE_MP_SC, CTRLQ_RING_SIZE,
				      SPDK_ENV_SOCKET_ID_ANY);
	if (ctrlq_ring == NULL) {
		SPDK_ERRLOG("Failed to allocate send ring for the controlq.\n");
		scan_target_abort(base, -ENOMEM);
		return;
	}

	rc = virtio_dev_acquire_queue(base->vdev, VIRTIO_SCSI_CONTROLQ);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to acquire the controlq.\n");
		assert(false);
		spdk_ring_free(ctrlq_ring);
		scan_target_abort(base, rc);
		return;
	}

	ctrlq = base->vdev->vqs[VIRTIO_SCSI_CONTROLQ];
	ctrlq->poller_ctx = ctrlq_ring;
	ctrlq->poller = spdk_poller_register(bdev_virtio_ctrlq_poll, base->vdev, CTRLQ_POLL_PERIOD_US);

	spdk_io_device_register(base->vdev, bdev_virtio_create_cb, bdev_virtio_destroy_cb,
				sizeof(struct bdev_virtio_io_channel));

	while ((disk = TAILQ_FIRST(&base->found_disks))) {
		TAILQ_REMOVE(&base->found_disks, disk, link);
		rc = spdk_bdev_register(&disk->bdev);
		if (rc) {
			spdk_io_device_unregister(base->vdev, NULL);
			SPDK_ERRLOG("Failed to register bdev name=%s\n", disk->bdev.name);
			spdk_ring_free(ctrlq_ring);
			scan_target_abort(base, rc);
			return;
		}
		bdevs[bdevs_cnt] = &disk->bdev;
		bdevs_cnt++;
	}

	TAILQ_REMOVE(&g_virtio_driver.init_ctrlrs, base->vdev, tailq);
	TAILQ_INSERT_TAIL(&g_virtio_driver.attached_ctrlrs, base->vdev, tailq);

	if (base->cb_fn) {
		base->cb_fn(base->cb_arg, 0, bdevs, bdevs_cnt);
	}

	spdk_dma_free(base);
}

static void
send_inquiry_vpd(struct virtio_scsi_scan_base *base, uint8_t target_id, struct virtio_req *vreq,
		 uint8_t page_code)
{
	struct iovec *iov = vreq->iov;
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;
	struct spdk_scsi_cdb_inquiry *inquiry_cdb = (struct spdk_scsi_cdb_inquiry *)req->cdb;
	int rc;

	memset(req, 0, sizeof(*req));
	req->lun[0] = 1;
	req->lun[1] = target_id;

	iov[0].iov_len = BDEV_VIRTIO_SCAN_PAYLOAD_SIZE;
	inquiry_cdb->opcode = SPDK_SPC_INQUIRY;
	inquiry_cdb->evpd = 1;
	inquiry_cdb->page_code = page_code;
	to_be16(inquiry_cdb->alloc_len, iov[0].iov_len);

	rc = virtio_xmit_pkt(base->vq, vreq);
	if (rc != 0) {
		assert(false);
	}
}

static void
send_read_cap_10(struct virtio_scsi_scan_base *base, uint8_t target_id, struct virtio_req *vreq)
{
	struct iovec *iov = vreq->iov;
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;
	int rc;

	memset(req, 0, sizeof(*req));
	req->lun[0] = 1;
	req->lun[1] = target_id;

	iov[0].iov_len = 8;
	req->cdb[0] = SPDK_SBC_READ_CAPACITY_10;

	rc = virtio_xmit_pkt(base->vq, vreq);
	if (rc != 0) {
		assert(false);
	}
}

static void
send_read_cap_16(struct virtio_scsi_scan_base *base, uint8_t target_id, struct virtio_req *vreq)
{
	struct iovec *iov = vreq->iov;
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;
	int rc;

	memset(req, 0, sizeof(*req));
	req->lun[0] = 1;
	req->lun[1] = target_id;

	iov[0].iov_len = 32;
	req->cdb[0] = SPDK_SPC_SERVICE_ACTION_IN_16;
	req->cdb[1] = SPDK_SBC_SAI_READ_CAPACITY_16;
	to_be32(&req->cdb[10], iov[0].iov_len);

	rc = virtio_xmit_pkt(base->vq, vreq);
	if (rc != 0) {
		assert(false);
	}
}

static int
process_scan_inquiry_standard(struct virtio_scsi_scan_base *base, struct virtio_req *vreq)
{
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;
	struct virtio_scsi_cmd_resp *resp = vreq->iov_resp.iov_base;
	struct spdk_scsi_cdb_inquiry_data *inquiry_data = vreq->iov[0].iov_base;
	uint8_t target_id;

	if (resp->response != VIRTIO_SCSI_S_OK || resp->status != SPDK_SCSI_STATUS_GOOD) {
		return -1;
	}

	if (inquiry_data->peripheral_device_type != SPDK_SPC_PERIPHERAL_DEVICE_TYPE_DISK ||
	    inquiry_data->peripheral_qualifier != SPDK_SPC_PERIPHERAL_QUALIFIER_CONNECTED) {
		SPDK_WARNLOG("Unsupported peripheral device type 0x%02x (qualifier 0x%02x)\n",
			     inquiry_data->peripheral_device_type,
			     inquiry_data->peripheral_qualifier);
		return -1;
	}

	target_id = req->lun[1];
	send_inquiry_vpd(base, target_id, vreq, SPDK_SPC_VPD_SUPPORTED_VPD_PAGES);
	return 0;
}

static int
process_scan_inquiry_vpd_supported_vpd_pages(struct virtio_scsi_scan_base *base,
		struct virtio_req *vreq)
{
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;
	struct virtio_scsi_cmd_resp *resp = vreq->iov_resp.iov_base;
	uint8_t target_id;
	bool block_provisioning_page_supported = false;

	if (resp->response == VIRTIO_SCSI_S_OK && resp->status == SPDK_SCSI_STATUS_GOOD) {
		const uint8_t *vpd_data = vreq->iov[0].iov_base;
		const uint8_t *supported_vpd_pages = vpd_data + 4;
		uint16_t page_length;
		uint16_t num_supported_pages;
		uint16_t i;

		page_length = from_be16(vpd_data + 2);
		num_supported_pages = spdk_min(page_length, vreq->iov[0].iov_len - 4);

		for (i = 0; i < num_supported_pages; i++) {
			if (supported_vpd_pages[i] == SPDK_SPC_VPD_BLOCK_THIN_PROVISION) {
				block_provisioning_page_supported = true;
				break;
			}
		}
	}

	target_id = req->lun[1];
	if (block_provisioning_page_supported) {
		send_inquiry_vpd(base, target_id, vreq, SPDK_SPC_VPD_BLOCK_THIN_PROVISION);
	} else {
		send_read_cap_10(base, target_id, vreq);
	}
	return 0;
}

static int
process_scan_inquiry_vpd_block_thin_provision(struct virtio_scsi_scan_base *base,
		struct virtio_req *vreq)
{
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;
	struct virtio_scsi_cmd_resp *resp = vreq->iov_resp.iov_base;
	uint8_t target_id;

	base->info.unmap_supported = false;

	if (resp->response == VIRTIO_SCSI_S_OK && resp->status == SPDK_SCSI_STATUS_GOOD) {
		uint8_t *vpd_data = vreq->iov[0].iov_base;

		base->info.unmap_supported = !!(vpd_data[5] & SPDK_SCSI_UNMAP_LBPU);
	}

	SPDK_INFOLOG(SPDK_TRACE_VIRTIO, "Target %u: unmap supported = %d\n",
		     base->info.target, (int)base->info.unmap_supported);

	target_id = req->lun[1];
	send_read_cap_10(base, target_id, vreq);
	return 0;
}

static int
process_scan_inquiry(struct virtio_scsi_scan_base *base, struct virtio_req *vreq)
{
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;
	struct spdk_scsi_cdb_inquiry *inquiry_cdb = (struct spdk_scsi_cdb_inquiry *)req->cdb;

	if ((inquiry_cdb->evpd & 1) == 0) {
		return process_scan_inquiry_standard(base, vreq);
	}

	switch (inquiry_cdb->page_code) {
	case SPDK_SPC_VPD_SUPPORTED_VPD_PAGES:
		return process_scan_inquiry_vpd_supported_vpd_pages(base, vreq);
	case SPDK_SPC_VPD_BLOCK_THIN_PROVISION:
		return process_scan_inquiry_vpd_block_thin_provision(base, vreq);
	default:
		SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO, "Unexpected VPD page 0x%02x\n", inquiry_cdb->page_code);
		return -1;
	}
}

static int
alloc_virtio_disk(struct virtio_scsi_scan_base *base)
{
	struct virtio_scsi_disk *disk;
	struct spdk_bdev *bdev;

	disk = calloc(1, sizeof(*disk));
	if (disk == NULL) {
		SPDK_ERRLOG("could not allocate disk\n");
		return -1;
	}

	disk->vdev = base->vdev;
	disk->info = base->info;

	bdev = &disk->bdev;
	bdev->name = spdk_sprintf_alloc("%st%"PRIu8, base->vdev->name, disk->info.target);
	if (bdev->name == NULL) {
		SPDK_ERRLOG("Couldn't alloc memory for the bdev name.\n");
		free(disk);
		return -1;
	}

	bdev->product_name = "Virtio SCSI Disk";
	bdev->write_cache = 0;
	bdev->blocklen = disk->info.block_size;
	bdev->blockcnt = disk->info.num_blocks;

	bdev->ctxt = disk;
	bdev->fn_table = &virtio_fn_table;
	bdev->module = SPDK_GET_BDEV_MODULE(virtio_scsi);

	TAILQ_INSERT_TAIL(&base->found_disks, disk, link);
	scan_target_finish(base);
	return 0;
}

static int
process_read_cap_10(struct virtio_scsi_scan_base *base, struct virtio_req *vreq)
{
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;
	struct virtio_scsi_cmd_resp *resp = vreq->iov_resp.iov_base;
	uint64_t max_block;
	uint32_t block_size;
	uint8_t target_id = req->lun[1];

	if (resp->response != VIRTIO_SCSI_S_OK || resp->status != SPDK_SCSI_STATUS_GOOD) {
		SPDK_ERRLOG("READ CAPACITY (10) failed for target %"PRIu8".\n", target_id);
		return -1;
	}

	block_size = from_be32((uint8_t *)vreq->iov[0].iov_base + 4);
	max_block = from_be32(vreq->iov[0].iov_base);

	if (max_block == 0xffffffff) {
		send_read_cap_16(base, target_id, vreq);
		return 0;
	}

	base->info.num_blocks = (uint64_t)max_block + 1;
	base->info.block_size = block_size;

	return alloc_virtio_disk(base);
}

static int
process_read_cap_16(struct virtio_scsi_scan_base *base, struct virtio_req *vreq)
{
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;
	struct virtio_scsi_cmd_resp *resp = vreq->iov_resp.iov_base;
	uint8_t target_id = req->lun[1];

	if (resp->response != VIRTIO_SCSI_S_OK || resp->status != SPDK_SCSI_STATUS_GOOD) {
		SPDK_ERRLOG("READ CAPACITY (16) failed for target %"PRIu8".\n", target_id);
		return -1;
	}

	base->info.num_blocks = from_be64((uint64_t *)(vreq->iov[0].iov_base)) + 1;
	base->info.block_size = from_be32((uint32_t *)(vreq->iov[0].iov_base + 8));
	return alloc_virtio_disk(base);
}

static void
process_scan_resp(struct virtio_scsi_scan_base *base, struct virtio_req *vreq)
{
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;
	struct virtio_scsi_cmd_resp *resp = vreq->iov_resp.iov_base;
	int rc, sk, asc, ascq;
	uint8_t target_id;

	if (vreq->iov_req.iov_len < sizeof(struct virtio_scsi_cmd_req) ||
	    vreq->iov_resp.iov_len < sizeof(struct virtio_scsi_cmd_resp)) {
		SPDK_ERRLOG("Received target scan message with invalid length.\n");
		scan_target_finish(base);
		return;
	}

	get_scsi_status(resp, &sk, &asc, &ascq);
	target_id = req->lun[1];

	if (resp->response == VIRTIO_SCSI_S_OK &&
	    resp->status == SPDK_SCSI_STATUS_CHECK_CONDITION &&
	    sk != SPDK_SCSI_SENSE_ILLEGAL_REQUEST) {
		assert(base->retries > 0);
		base->retries--;
		if (base->retries == 0) {
			SPDK_NOTICELOG("Target %"PRIu8" is present, but unavailable.\n", target_id);
			SPDK_TRACEDUMP(SPDK_TRACE_VIRTIO, "CDB", req->cdb, sizeof(req->cdb));
			SPDK_TRACEDUMP(SPDK_TRACE_VIRTIO, "SENSE DATA", resp->sense, sizeof(resp->sense));
			scan_target_finish(base);
			return;
		}

		/* resend the same request */
		rc = virtio_xmit_pkt(base->vq, vreq);
		if (rc != 0) {
			assert(false);
		}
		return;
	}

	base->retries = SCAN_REQUEST_RETRIES;

	switch (req->cdb[0]) {
	case SPDK_SPC_INQUIRY:
		rc = process_scan_inquiry(base, vreq);
		break;
	case SPDK_SBC_READ_CAPACITY_10:
		rc = process_read_cap_10(base, vreq);
		break;
	case SPDK_SPC_SERVICE_ACTION_IN_16:
		rc = process_read_cap_16(base, vreq);
		break;
	default:
		SPDK_ERRLOG("Received invalid target scan message: cdb[0] = %"PRIu8".\n", req->cdb[0]);
		rc = -1;
		break;
	}

	if (rc < 0) {
		scan_target_finish(base);
	}
}

static void
bdev_scan_poll(void *arg)
{
	struct virtio_scsi_scan_base *base = arg;
	struct virtio_req *req;
	uint16_t cnt;

	cnt = virtio_recv_pkts(base->vq, &req, 1);
	if (cnt > 0) {
		process_scan_resp(base, req);
	}
}

static int
scan_target(struct virtio_scsi_scan_base *base)
{
	struct iovec *iov;
	struct virtio_req *vreq;
	struct virtio_scsi_cmd_req *req;
	struct virtio_scsi_cmd_resp *resp;
	struct spdk_scsi_cdb_inquiry *cdb;

	memset(&base->info, 0, sizeof(base->info));
	base->info.target = base->target;

	vreq = &base->io_ctx.vreq;
	req = &base->io_ctx.req;
	resp = &base->io_ctx.resp;
	iov = &base->iov;

	vreq->iov = iov;
	vreq->iovcnt = 1;
	vreq->is_write = 0;

	vreq->iov_req.iov_base = (void *)req;
	vreq->iov_req.iov_len = sizeof(*req);

	vreq->iov_resp.iov_base = (void *)resp;
	vreq->iov_resp.iov_len = sizeof(*resp);

	iov[0].iov_base = (void *)&base->payload;
	iov[0].iov_len = BDEV_VIRTIO_SCAN_PAYLOAD_SIZE;

	req->lun[0] = 1;
	req->lun[1] = base->info.target;

	cdb = (struct spdk_scsi_cdb_inquiry *)req->cdb;
	cdb->opcode = SPDK_SPC_INQUIRY;
	to_be16(cdb->alloc_len, BDEV_VIRTIO_SCAN_PAYLOAD_SIZE);

	base->retries = SCAN_REQUEST_RETRIES;
	return virtio_xmit_pkt(base->vq, vreq);
}

static int
bdev_virtio_process_config(void)
{
	struct spdk_conf_section *sp;
	struct virtio_dev *vdev = NULL;
	char *default_name = NULL;
	char *path, *name;
	unsigned vdev_num;
	int num_queues;
	bool enable_pci;
	int rc = 0;

	for (sp = spdk_conf_first_section(NULL); sp != NULL; sp = spdk_conf_next_section(sp)) {
		if (!spdk_conf_section_match_prefix(sp, "VirtioUser")) {
			continue;
		}

		if (sscanf(spdk_conf_section_get_name(sp), "VirtioUser%u", &vdev_num) != 1) {
			SPDK_ERRLOG("Section '%s' has non-numeric suffix.\n",
				    spdk_conf_section_get_name(sp));
			rc = -1;
			goto out;
		}

		path = spdk_conf_section_get_val(sp, "Path");
		if (path == NULL) {
			SPDK_ERRLOG("VirtioUser%u: missing Path\n", vdev_num);
			rc = -1;
			goto out;
		}

		num_queues = spdk_conf_section_get_intval(sp, "Queues");
		if (num_queues < 1) {
			num_queues = 1;
		}

		name = spdk_conf_section_get_val(sp, "Name");
		if (name == NULL) {
			default_name = spdk_sprintf_alloc("VirtioScsi%u", vdev_num);
			name = default_name;
		}

		vdev = virtio_user_dev_init(name, path, num_queues, 512, SPDK_VIRTIO_SCSI_QUEUE_NUM_FIXED);

		free(default_name);
		default_name = NULL;

		if (vdev == NULL) {
			rc = -1;
			goto out;
		}
	}

	sp = spdk_conf_find_section(NULL, "VirtioPci");
	if (sp == NULL) {
		return 0;
	}

	enable_pci = spdk_conf_section_get_boolval(sp, "Enable", false);
	if (enable_pci) {
		rc = virtio_enumerate_pci();
	}

out:
	return rc;
}

static void
bdev_virtio_scsi_free(struct virtio_dev *vdev)
{
	struct virtqueue *vq;

	if (virtio_dev_queue_is_acquired(vdev, VIRTIO_SCSI_REQUESTQ)) {
		vq = vdev->vqs[VIRTIO_SCSI_REQUESTQ];
		spdk_poller_unregister(&vq->poller);
		spdk_dma_free(vq->poller_ctx);
		vq->poller_ctx = NULL;
		virtio_dev_release_queue(vdev, VIRTIO_SCSI_REQUESTQ);
	}

	virtio_dev_reset(vdev);
	virtio_dev_free(vdev);
}

static int
bdev_virtio_scsi_scan(struct virtio_dev *vdev, virtio_create_device_cb cb_fn, void *cb_arg)
{
	struct virtio_scsi_scan_base *base = spdk_dma_zmalloc(sizeof(struct virtio_scsi_scan_base), 64,
					     NULL);
	struct virtqueue *vq;
	int rc;

	if (base == NULL) {
		SPDK_ERRLOG("couldn't allocate memory for scsi target scan.\n");
		return -ENOMEM;
	}

	base->cb_fn = cb_fn;
	base->cb_arg = cb_arg;

	rc = virtio_dev_init(vdev, VIRTIO_SCSI_DEV_SUPPORTED_FEATURES);
	if (rc != 0) {
		spdk_dma_free(base);
		return rc;
	}

	rc = virtio_dev_start(vdev);
	if (rc != 0) {
		spdk_dma_free(base);
		return rc;
	}

	base->vdev = vdev;
	TAILQ_INIT(&base->found_disks);

	rc = virtio_dev_acquire_queue(vdev, VIRTIO_SCSI_REQUESTQ);
	if (rc != 0) {
		SPDK_ERRLOG("Couldn't acquire requestq for the target scan.\n");
		spdk_dma_free(base);
		return rc;
	}

	vq = vdev->vqs[VIRTIO_SCSI_REQUESTQ];
	base->vq = vq;
	vq->poller_ctx = base;
	vq->poller = spdk_poller_register(bdev_scan_poll, base, 0);
	rc = scan_target(base);
	if (rc) {
		SPDK_ERRLOG("Failed to start target scan.\n");
	}

	return rc;
}

static void
bdev_virtio_initial_scan_complete(void *ctx __attribute__((unused)),
				  int result  __attribute__((unused)),
				  struct spdk_bdev **bdevs __attribute__((unused)), size_t bdevs_cnt __attribute__((unused)))
{
	if (TAILQ_EMPTY(&g_virtio_driver.init_ctrlrs)) {
		spdk_bdev_module_init_done(SPDK_GET_BDEV_MODULE(virtio_scsi));
	}
}

static int
bdev_virtio_initialize(void)
{
	struct virtio_dev *vdev, *next_vdev;
	int rc;

	rc = bdev_virtio_process_config();
	if (rc != 0) {
		goto out;
	}

	if (TAILQ_EMPTY(&g_virtio_driver.init_ctrlrs)) {
		spdk_bdev_module_init_done(SPDK_GET_BDEV_MODULE(virtio_scsi));
		return 0;
	}

	/* Initialize all created devices and scan available targets */
	TAILQ_FOREACH(vdev, &g_virtio_driver.init_ctrlrs, tailq) {
		rc = bdev_virtio_scsi_scan(vdev, bdev_virtio_initial_scan_complete, NULL);
		if (rc != 0) {
			goto out;
		}
	}

	return 0;

out:
	/* Remove any created devices */
	TAILQ_FOREACH_SAFE(vdev, &g_virtio_driver.init_ctrlrs, tailq, next_vdev) {
		TAILQ_REMOVE(&g_virtio_driver.init_ctrlrs, vdev, tailq);
		bdev_virtio_scsi_free(vdev);
	}

	spdk_bdev_module_init_done(SPDK_GET_BDEV_MODULE(virtio_scsi));
	return rc;
}

static void
virtio_scsi_dev_unregister_cb(void *io_device)
{
	struct virtio_dev *vdev = io_device;
	struct virtqueue *vq;
	struct spdk_ring *send_ring;
	struct spdk_thread *thread;
	bool finish_module;

	thread = virtio_dev_queue_get_thread(vdev, VIRTIO_SCSI_CONTROLQ);
	if (thread != spdk_get_thread()) {
		spdk_thread_send_msg(thread, virtio_scsi_dev_unregister_cb, io_device);
		return;
	}

	if (virtio_dev_queue_is_acquired(vdev, VIRTIO_SCSI_CONTROLQ)) {
		vq = vdev->vqs[VIRTIO_SCSI_CONTROLQ];
		spdk_poller_unregister(&vq->poller);
		send_ring = vq->poller_ctx;
		/* bdevs built on top of this vdev mustn't be destroyed with outstanding I/O. */
		assert(spdk_ring_count(send_ring) == 0);
		spdk_ring_free(send_ring);
		vq->poller_ctx = NULL;
		virtio_dev_release_queue(vdev, VIRTIO_SCSI_CONTROLQ);
	}

	virtio_dev_reset(vdev);
	virtio_dev_free(vdev);

	TAILQ_REMOVE(&g_virtio_driver.attached_ctrlrs, vdev, tailq);
	finish_module = TAILQ_EMPTY(&g_virtio_driver.attached_ctrlrs);

	if (finish_module) {
		spdk_bdev_module_finish_done();
	}
}

static void
bdev_virtio_finish(void)
{
	struct virtio_dev *vdev, *next;

	if (TAILQ_EMPTY(&g_virtio_driver.attached_ctrlrs)) {
		spdk_bdev_module_finish_done();
		return;
	}

	TAILQ_FOREACH_SAFE(vdev, &g_virtio_driver.attached_ctrlrs, tailq, next) {
		spdk_io_device_unregister(vdev, virtio_scsi_dev_unregister_cb);
	}
}

int
create_virtio_user_scsi_device(const char *base_name, const char *path, unsigned num_queues,
			       unsigned queue_size, virtio_create_device_cb cb_fn, void *cb_arg)
{
	struct virtio_dev *vdev;
	int rc;

	vdev = virtio_user_dev_init(base_name, path, num_queues, queue_size,
				    SPDK_VIRTIO_SCSI_QUEUE_NUM_FIXED);
	if (!vdev) {
		SPDK_ERRLOG("Failed to create virito device %s: %s\n", base_name, path);
		return -EINVAL;
	}

	rc = bdev_virtio_scsi_scan(vdev, cb_fn, cb_arg);
	if (rc) {
		TAILQ_REMOVE(&g_virtio_driver.init_ctrlrs, vdev, tailq);
		bdev_virtio_scsi_free(vdev);
	}

	return rc;
}


SPDK_LOG_REGISTER_TRACE_FLAG("virtio", SPDK_TRACE_VIRTIO)
