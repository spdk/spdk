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
#include "spdk/util.h"
#include "spdk/queue.h"
#include "spdk/likely.h"
#include "spdk/endian.h"
#include "spdk/string.h"

#include "spdk/env.h"
#include "spdk/bdev.h"
#include "spdk/io_channel.h"

#include "spdk/conf.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"
#include "spdk_internal/assert.h"

#include <linux/virtio_scsi.h>
#include "virtio_dev.h"
#include "virtio_user/virtio_user_dev.h"

#include "spdk/scsi_spec.h"
#include "bdev_virtio.h"

#define VIRTIO_SCSI_CONTROLQ   0
#define VIRTIO_SCSI_EVENTQ   1
#define VIRTIO_SCSI_REQUESTQ   2

#define BDEV_VIRTIO_MAX_TARGET 64
#define BDEV_VIRTIO_SCAN_PAYLOAD_SIZE 256

#define VIRTIO_SCSI_CTRL_MAGIC          0xfeed0001
#define VIRTIO_SCSI_DEV_MAGIC           0xfeed0002
#define VIRTIO_SCSI_IO_CHANNEL_MAGIC    0xfeed0003


/* (struct virtio_scsi_dev *) bdev->ctxt */
struct virtio_scsi_dev {
	struct spdk_bdev bdev;
	unsigned io_in_queue_cnt;
	unsigned io_pending_cnt;

	/* SCSI target */
	uint8_t target;

	/* Virtio SCSI controller to which this device belong to */
	struct spdk_virtio_scsi_ctrlr *ctrlr;
	uint32_t magic;
};

/* (struct  virtio_scsi_io_channel_ctx *)spdk_io_channel_get_ctx(struct spdk_io_channel) */
struct  virtio_scsi_io_channel_ctx {
	struct virtqueue *vq;
	TAILQ_HEAD(, spdk_bdev_io) pending_io;
	struct spdk_bdev_poller	*poller;
	uint32_t magic;
};

/* (struct virtio_scsi_io_ctx *)bdev_io->driver_ctx */
struct virtio_scsi_io_ctx {
	struct virtio_req 		vreq;
	struct virtio_scsi_cmd_req 	req;
	struct virtio_scsi_cmd_resp 	resp;
};

/*
 * Virtio device to controll PCI/user link
 */
struct spdk_virtio_dev {
	struct virtio_dev *dev;
	char path[PATH_MAX + 1]; /* path for user devs */

	TAILQ_ENTRY(spdk_virtio_dev) vdev_link;

	/* None zero value -> queue is used */
	uint8_t is_vq_used[VIRTIO_MAX_VIRTQUEUES];
};

/*
 * Virtio SCSI device
 */
struct spdk_virtio_scsi_ctrlr {
	struct spdk_virtio_dev vdev;
	struct virtio_scsi_dev *scsi_devs[BDEV_VIRTIO_MAX_TARGET];

	/* Fake bdev to watch entire device. */
	struct virtio_scsi_dev *sdev;
	struct spdk_bdev_desc *bdev_desc;
	struct spdk_io_channel *io_channel;
	uint32_t magic;
};

static TAILQ_HEAD(, spdk_virtio_dev) g_spdk_vdevs = TAILQ_HEAD_INITIALIZER(g_spdk_vdevs);
static int g_virtio_ctrlr_count = 0;
static int g_virtio_scsi_bdev_count = 0;

static void bdev_virtio_scsi_io_cpl(struct virtio_req *req);

static int bdev_virtio_scsi_initialize(void);
static void bdev_virtio_scsi_finish(void);
static int bdev_virtio_scsi_get_ctx_size(void);
SPDK_BDEV_MODULE_REGISTER(virtio_scsi, bdev_virtio_scsi_initialize, bdev_virtio_scsi_finish,
			  NULL, bdev_virtio_scsi_get_ctx_size, NULL);

SPDK_BDEV_MODULE_ASYNC_INIT(virtio_scsi);

static struct virtio_dev *
spdk_virtio_dev_get_dev(struct spdk_virtio_dev *vdev)
{
	return vdev->dev;
}

static const char *
spdk_virtio_dev_get_addr(struct spdk_virtio_dev *vdev)
{
	return vdev->path;
}

static struct spdk_virtio_dev *
spdk_virtio_dev_find_by_path(const char *path)
{
	struct spdk_virtio_dev *vdev;

	TAILQ_FOREACH(vdev, &g_spdk_vdevs, vdev_link) {
		if (strcmp(vdev->path, path) == 0) {
			return vdev;
		}
	}

	return NULL;
}


static void
virtio_scsi_set_target(uint8_t *lun, uint8_t target_id)
{
	lun[0] = 1;
	lun[1] = target_id;
	/* One LUN supported */
	lun[2] = 0;
	lun[3] = 0;
}

static void
virtio_scsi_io_ctx_init(struct virtio_scsi_io_ctx *io_ctx, int is_write, int target_id, struct iovec *iovs, uint32_t iovcnt)
{
	struct virtio_req *vreq = &io_ctx->vreq;

	vreq->iov_req.iov_base = &io_ctx->req;
	vreq->iov_req.iov_len = sizeof(io_ctx->req);
	vreq->iov_resp.iov_base = &io_ctx->resp;
	vreq->iov_resp.iov_len = sizeof(io_ctx->resp);
	vreq->is_write = is_write;
	vreq->iov = iovs;
	vreq->iovcnt = iovcnt;

	memset(&io_ctx->req, 0, sizeof(io_ctx->req));
	virtio_scsi_set_target(io_ctx->req.lun, target_id);

	/*
	 * Set response code to error in case other side is not abble to
	 * process out request and complete it without reporting status.
	 */
	io_ctx->resp.response = VIRTIO_SCSI_S_FAILURE;
}

static void
virtio_scsi_io_xmit(struct virtio_scsi_io_channel_ctx *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_scsi_io_ctx *io = (struct virtio_scsi_io_ctx *)bdev_io->driver_ctx;
	int rc = virtio_xmit_pkts(ch->vq, &io->vreq);
	struct virtio_scsi_dev *sdev = bdev_io->bdev->ctxt;

	assert(ch->magic == VIRTIO_SCSI_IO_CHANNEL_MAGIC);

	assert(sdev->magic == VIRTIO_SCSI_DEV_MAGIC);
	if (spdk_likely(rc == 1)) {
		sdev->io_in_queue_cnt ++;
		return;
	} else if (spdk_likely(rc == -EAGAIN)) {
		TAILQ_INSERT_TAIL(&ch->pending_io, bdev_io, module_link);
		sdev->io_pending_cnt++;
	} else {
		sdev->io_in_queue_cnt ++;
		io->resp.response = VIRTIO_SCSI_S_TRANSPORT_FAILURE;
		bdev_virtio_scsi_io_cpl(&io->vreq);
	}

}

static int
bdev_virtio_scsi_inquiry(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_scsi_io_ctx *io_ctx = (struct virtio_scsi_io_ctx *)bdev_io->driver_ctx;
	struct virtio_scsi_cmd_req *req = &io_ctx->req;
	struct iovec *iovs = bdev_io->u.bdev.iovs;
	struct spdk_scsi_cdb_inquiry *cdb = (void *)req->cdb;

	if (bdev_io->u.bdev.iovcnt < 2 || !iovs ||
		iovs[0].iov_len < 1 || iovs[1].iov_len < 8) {
		return -EINVAL;
	}

	virtio_scsi_io_ctx_init(io_ctx, false, *(uint8_t *)iovs[0].iov_base, &bdev_io->u.bdev.iovs[1], 1);
	cdb->opcode = SPDK_SPC_INQUIRY;
	to_be16(cdb->alloc_len, bdev_io->u.bdev.iovs[1].iov_len);
	virtio_scsi_io_xmit(spdk_io_channel_get_ctx(ch), bdev_io);
	return 0;
}

/* XXX: Move to public API ? */
/*
 * iovs[0] - point to uint8_t target_id - the target lun, len must be 1
 * iovs[1] - point to INQUIRY buffer
 * iovcnt - 1 or 2
 */
static int
spdk_virtio_scsi_inquiry(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    struct iovec *iovs, size_t iovcnt, spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_get_io();

	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed duing INQUIRY\n");
		return -ENOMEM;
	}

	bdev_io->ch = spdk_io_channel_get_ctx(ch);
	bdev_io->type = VIRTIO_SCSI_IO_TYPE_INQUIRY;
	bdev_io->u.bdev.iovs = iovs;
	bdev_io->u.bdev.iovcnt = iovcnt;
	spdk_bdev_io_init(bdev_io, spdk_bdev_desc_get_bdev(desc), cb_arg, cb);
	spdk_bdev_io_submit(bdev_io);
	return 0;
}


static int
bdev_virtio_scsi_read_cap_10(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_scsi_io_ctx *io_ctx = (struct virtio_scsi_io_ctx *)bdev_io->driver_ctx;
	struct virtio_scsi_cmd_req *req = &io_ctx->req;
	struct iovec *iovs = bdev_io->u.bdev.iovs;

	if (bdev_io->u.bdev.iovcnt < 2 || !iovs ||
		iovs[0].iov_len < 1 || iovs[1].iov_len < 8) {
		return -EINVAL;
	}

	virtio_scsi_io_ctx_init(io_ctx, false, *(uint8_t *)iovs[0].iov_base, &bdev_io->u.bdev.iovs[1], 1);
	req->cdb[0] = SPDK_SBC_READ_CAPACITY_10;

	virtio_scsi_io_xmit(spdk_io_channel_get_ctx(ch), bdev_io);
	return 0;
}

/*
 * iovs[0] - pointer to uint8_t target_id - the target lun, len must be 1
 * iovs[1] - pointer to uint32_t [2] buffer (big endian)
 * iovcnt - 1 or 2
 */
static int
spdk_virtio_scsi_read_cap_10(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    struct iovec *iovs, size_t iovcnt, spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_get_io();

	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed duing READCAP 10\n");
		return -ENOMEM;
	}

	bdev_io->ch = spdk_io_channel_get_ctx(ch);;
	bdev_io->type = VIRTIO_SCSI_IO_TYPE_READ_CAP_10;
	bdev_io->u.bdev.iovs = iovs;
	bdev_io->u.bdev.iovcnt = iovcnt;
	spdk_bdev_io_init(bdev_io, spdk_bdev_desc_get_bdev(desc), cb_arg, cb);
	spdk_bdev_io_submit(bdev_io);
	return 0;
}

static int
bdev_virtio_scsi_read_cap_16(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_scsi_io_ctx *io_ctx = (struct virtio_scsi_io_ctx *)bdev_io->driver_ctx;
	struct virtio_scsi_cmd_req *req = &io_ctx->req;
	struct iovec *iovs = bdev_io->u.bdev.iovs;

	if (bdev_io->u.bdev.iovcnt < 2 || !iovs ||
		iovs[0].iov_len < 1 || iovs[1].iov_len < 32) {
		return -EINVAL;
	}

	virtio_scsi_io_ctx_init(io_ctx, false, *(uint8_t *)iovs[0].iov_base, &iovs[1], 1);
	req->cdb[0] = SPDK_SPC_SERVICE_ACTION_IN_16;
	req->cdb[1] = SPDK_SBC_SAI_READ_CAPACITY_16;
	to_be32(&req->cdb[10], iovs[1].iov_len);

	virtio_scsi_io_xmit(spdk_io_channel_get_ctx(ch), bdev_io);
	return 0;
}

/*
 * iovs[0] - pointer to uint8_t target_id - the target lun, len must be 1
 * iovs[1] - pointer to 32 byte length buffer - READ CAPACITY 16 data:
 * iovcnt - 1 or 2
 */
static int
spdk_virtio_scsi_read_cap_16(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		    struct iovec *iovs, size_t iovcnt, spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_get_io();

	if (!bdev_io) {
		SPDK_ERRLOG("bdev_io memory allocation failed duing READCAP 10\n");
		return -ENOMEM;
	}

	bdev_io->ch = spdk_io_channel_get_ctx(ch);
	bdev_io->type = VIRTIO_SCSI_IO_TYPE_READ_CAP_16;
	bdev_io->u.bdev.iovs = iovs;
	bdev_io->u.bdev.iovcnt = iovcnt;
	spdk_bdev_io_init(bdev_io, spdk_bdev_desc_get_bdev(desc), cb_arg, cb);
	spdk_bdev_io_submit(bdev_io);
	return 0;
}

static void
bdev_virtio_scsi_rw(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_scsi_dev *sdev = SPDK_CONTAINEROF(bdev_io->bdev, struct virtio_scsi_dev, bdev);
	struct virtio_scsi_io_ctx *io_ctx = (struct virtio_scsi_io_ctx *)bdev_io->driver_ctx;
	struct virtio_scsi_cmd_req *req = &io_ctx->req;
	bool is_write = (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE);

	assert(sdev->magic == VIRTIO_SCSI_DEV_MAGIC);

	virtio_scsi_io_ctx_init(io_ctx, is_write, sdev->target, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt);

	/* XXX: Is 'disk->num_blocks > (1ULL << 32)' good condition here? */
	if (spdk_bdev_get_num_blocks(&sdev->bdev) > (1ULL << 32)) {
		req->cdb[0] = is_write ? SPDK_SBC_WRITE_16 : SPDK_SBC_READ_16;
		to_be64(&req->cdb[2], bdev_io->u.bdev.offset_blocks);
		to_be32(&req->cdb[10], bdev_io->u.bdev.num_blocks);
	} else {
		req->cdb[0] = is_write ? SPDK_SBC_WRITE_10 : SPDK_SBC_READ_10;
		to_be32(&req->cdb[2], bdev_io->u.bdev.offset_blocks);
		to_be16(&req->cdb[7], bdev_io->u.bdev.num_blocks);
	}

	virtio_scsi_io_xmit(spdk_io_channel_get_ctx(ch), bdev_io);
}

static int
_bdev_virtio_scsi_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_virtio_scsi_rw);
		return 0;
	case SPDK_BDEV_IO_TYPE_WRITE:
		bdev_virtio_scsi_rw(ch, bdev_io);
		return 0;
	case SPDK_BDEV_IO_TYPE_RESET:
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;
	case VIRTIO_SCSI_IO_TYPE_INQUIRY:
		return bdev_virtio_scsi_inquiry(ch, bdev_io);
	case VIRTIO_SCSI_IO_TYPE_READ_CAP_10:
		return bdev_virtio_scsi_read_cap_10(ch, bdev_io);
	case VIRTIO_SCSI_IO_TYPE_READ_CAP_16:
		return bdev_virtio_scsi_read_cap_16(ch, bdev_io);
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	default:
		return -ENOTSUP;
	}

}

static void
bdev_virtio_scsi_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	if (_bdev_virtio_scsi_submit_request(ch, bdev_io) < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
bdev_virtio_scsi_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch ((int)io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;
	/* FIXME: Support is incoming soon */
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		return false;
	/* Virtio SCSI specific IO */
	case VIRTIO_SCSI_IO_TYPE_INQUIRY:
	case VIRTIO_SCSI_IO_TYPE_READ_CAP_10:
	case VIRTIO_SCSI_IO_TYPE_READ_CAP_16:
		return true;
	default:
		return false;
	}
}

static struct spdk_io_channel *
bdev_virtio_scsi_get_io_channel(void *bdev_ctxt)
{
	struct spdk_bdev *bdev = bdev_ctxt;
	struct virtio_scsi_dev *sdev = bdev->ctxt;
	struct spdk_io_channel *ch = spdk_get_io_channel(sdev->ctrlr);

	assert(sdev->magic == VIRTIO_SCSI_DEV_MAGIC);
	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO, "Allocated channel %p for bdev '%s'\n",
			ch, sdev->bdev.name);

	return ch;
}


static int
bdev_virtio_scsi_destruct(void *ctx)
{
	/* TODO: */
	return 0;
}

static const struct spdk_bdev_fn_table virtio_fn_table = {
	.destruct		= bdev_virtio_scsi_destruct,
	.submit_request		= bdev_virtio_scsi_submit_request,
	.io_type_supported	= bdev_virtio_scsi_io_type_supported,
	.get_io_channel		= bdev_virtio_scsi_get_io_channel,
};

static struct virtio_scsi_dev *
virtio_scsi_dev_alloc(struct spdk_virtio_scsi_ctrlr *ctrlr, uint8_t target_id, uint64_t num_blocks, uint32_t block_size)
{
	struct virtio_scsi_dev *sdev;
	struct spdk_bdev *bdev, *base_bdev;

	assert(ctrlr->magic == VIRTIO_SCSI_CTRL_MAGIC);
	/* This need to be converted to if() when event queue will be implemented */
	assert(target_id < BDEV_VIRTIO_MAX_TARGET);
	assert(ctrlr->scsi_devs[target_id] == NULL);

	sdev = calloc(1, sizeof(*sdev));
	if (ctrlr == NULL) {
		SPDK_ERRLOG("could not allocate disk\n");
		return NULL;
	}

	sdev->ctrlr = ctrlr;
	sdev->target = target_id;
	sdev->magic = VIRTIO_SCSI_DEV_MAGIC;

	bdev = &sdev->bdev;
	bdev->name = spdk_sprintf_alloc("VirtioScsi%d", g_virtio_scsi_bdev_count);
	g_virtio_scsi_bdev_count++;

	bdev->product_name = "Virtio SCSI Disk";
	bdev->write_cache = 0;
	bdev->blocklen = block_size;
	bdev->blockcnt = num_blocks;

	bdev->ctxt = sdev;
	bdev->fn_table = &virtio_fn_table;
	bdev->module = SPDK_GET_BDEV_MODULE(virtio_scsi);

	SPDK_ERRLOG("Added new \n");
	ctrlr->scsi_devs[target_id] = sdev;

	base_bdev = &ctrlr->sdev->bdev;
	spdk_vbdev_register(bdev, &base_bdev, 1);
	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO, "New scsi target allocated %"PRIu8" to bdev '%s'.\n",target_id, bdev->name);
	return sdev;
}

static int
spdk_virtio_scsi_dev_init(struct virtio_dev *vdev)
{
	if (virtio_init_device(vdev, VIRTIO_SCSI_DEV_SUPPORTED_FEATURES) != 0) {
		return -EIO;
	}

	if (virtio_dev_start(vdev) < 0) {
		return -EIO;
	}

	return 0;
}

/* Ctrlr bdev */
static int
_bdev_virtio_scsi_ctrlr_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch ((int)bdev_io->type) {
	case VIRTIO_SCSI_IO_TYPE_INQUIRY:
		return bdev_virtio_scsi_inquiry(ch, bdev_io);
	case VIRTIO_SCSI_IO_TYPE_READ_CAP_10:
		return bdev_virtio_scsi_read_cap_10(ch, bdev_io);
	default:
		return -1;
	}
}

static void
bdev_virtio_scsi_ctrlr_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	if(_bdev_virtio_scsi_ctrlr_submit_request(ch, bdev_io)) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
bdev_virtio_scsi_ctrlr_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch ((int)io_type) {
	case VIRTIO_SCSI_IO_TYPE_INQUIRY:
	/* TODO: SPDK_BDEV_IO_TYPE_RESET might be supported as bus reset */
		return true;
	default:
		return false;
	}
}

static void
bdev_virtio_scsi_ctrlr_destruct_cb(void *io_device)
{
	struct spdk_virtio_scsi_ctrlr *ctrlr = io_device;

	assert(ctrlr->magic == VIRTIO_SCSI_CTRL_MAGIC);
	assert(ctrlr->sdev->magic == VIRTIO_SCSI_DEV_MAGIC);

	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO, "'%s'  destruct finish\n", ctrlr->sdev->bdev.name);

	if (ctrlr->bdev_desc) {
		spdk_bdev_close(ctrlr->bdev_desc);
	}

	if (ctrlr->io_channel) {
		spdk_put_io_channel(ctrlr->io_channel);
	}

	free(ctrlr->sdev->bdev.name);
	/* XXX: debug */
	ctrlr->sdev->magic = 0;
	free(ctrlr->sdev);

	if (ctrlr->vdev.dev) {
		virtio_user_dev_uninit(ctrlr->vdev.dev);
	}

	TAILQ_REMOVE(&g_spdk_vdevs, &ctrlr->vdev, vdev_link);

	/* XXX: debug */
	ctrlr->magic = 0;
	free(ctrlr);
}

static int
bdev_virtio_scsi_ctrlr_destruct(void *bdev_ctxt)
{
	struct virtio_scsi_dev *sdev = bdev_ctxt;

	SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO, "'%s'  destruct start\n", sdev->bdev.name);
	spdk_io_device_unregister(sdev->ctrlr, bdev_virtio_scsi_ctrlr_destruct_cb);
	return 0;
}

static const struct spdk_bdev_fn_table virtio_ctrlr_fn_table = {
	.destruct		= bdev_virtio_scsi_ctrlr_destruct,
	.submit_request		= bdev_virtio_scsi_ctrlr_submit_request,
	.io_type_supported	= bdev_virtio_scsi_ctrlr_io_type_supported,
	.get_io_channel		= bdev_virtio_scsi_get_io_channel,
};

/* Hot remove */
static void
bdev_virtio_scsi_device_remove_cb(void *ctx)
{
	assert(false);
}

static void
get_scsi_status(struct virtio_scsi_cmd_resp *resp, int *sk, uint8_t *asc, uint8_t *ascq)
{
	/* see spdk_scsi_task_build_sense_data() for sense data details */
	if (resp->sense_len >= 3) {
		*sk = resp->sense[2] & 0xf;
	}

	if (resp->sense_len >= 13) {
		*asc = resp->sense[12];
	}

	if (resp->sense_len >= 14) {
		*ascq = resp->sense[13];
	}
}

static void
bdev_virtio_scsi_io_cpl(struct virtio_req *req)
{
	struct virtio_scsi_io_ctx *io_ctx = SPDK_CONTAINEROF(req, struct virtio_scsi_io_ctx, vreq);
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(io_ctx);
	struct virtio_scsi_dev *sdev = bdev_io->bdev->ctxt;
	int sk = 0;
	uint8_t asc = 0;
	uint8_t ascq = 0;
	enum spdk_scsi_status sc;

	assert(sdev->magic == VIRTIO_SCSI_DEV_MAGIC);
	assert(sdev->io_in_queue_cnt > 0);
	sdev->io_in_queue_cnt--;

	switch(io_ctx->resp.response) {
	case VIRTIO_SCSI_S_OK:
		sc = io_ctx->resp.status;
		get_scsi_status(&io_ctx->resp, &sk, &asc, &ascq);
		break;
	/* XXX: how to handle other VIRTIO_SCSI_S_* ? */
	default:
		sc = SPDK_SCSI_STATUS_CHECK_CONDITION;
	}

	spdk_bdev_io_complete_scsi_status(bdev_io, sc, sk, asc, ascq);
	spdk_bdev_free_io(bdev_io);
}

static void
bdev_virtio_scsi_poll(void *arg)
{
	struct spdk_bdev_io *bdev_io;
	struct virtio_scsi_dev *sdev;
	struct virtio_scsi_io_channel_ctx *ch = arg;
	struct virtio_req *req[32];
	uint16_t i, cnt;

	assert(ch->magic == VIRTIO_SCSI_IO_CHANNEL_MAGIC);

	cnt = virtio_recv_pkts(ch->vq, req, SPDK_COUNTOF(req));
	for (i = 0; i < cnt; ++i) {
		bdev_virtio_scsi_io_cpl(req[i]);
	}

	for (; cnt > 0; cnt--) {
		bdev_io = TAILQ_FIRST(&ch->pending_io);
		if (!bdev_io) {
			break;
		}

		TAILQ_REMOVE(&ch->pending_io, bdev_io, module_link);

		sdev = bdev_io->bdev->ctxt;
		assert(sdev->io_pending_cnt > 0);
		sdev->io_pending_cnt--;

		virtio_scsi_io_xmit(ch, bdev_io);
	}
}

/* IO channel allocation/deallocation */
static int
virtio_scsi_io_channel_create(void *io_device, void *ctx_buf)
{
	struct spdk_virtio_scsi_ctrlr *ctrlr = io_device;
	struct virtio_scsi_io_channel_ctx *ch = ctx_buf;
	struct virtio_dev *vdev = spdk_virtio_dev_get_dev(&ctrlr->vdev);
	uint32_t i;

	assert(ctrlr->magic == VIRTIO_SCSI_CTRL_MAGIC);

	for (i = VIRTIO_SCSI_REQUESTQ; i < vdev->max_queues; i++) {
		if (ctrlr->vdev.is_vq_used[i]) {
			continue;
		}

		ctrlr->vdev.is_vq_used[i] = 1;

		ch->vq = vdev->vqs[i];
		TAILQ_INIT(&ch->pending_io);
		ch->magic = VIRTIO_SCSI_IO_CHANNEL_MAGIC;
		spdk_bdev_poller_start(&ch->poller, bdev_virtio_scsi_poll, ch, spdk_env_get_current_core(), 0);

		return 0;
	}

	SPDK_ERRLOG("Controller '%s': no free virtual queue to allocate io channel\n",
			spdk_virtio_dev_get_addr(&ctrlr->vdev));
	return -1;
}

static void
virtio_scsi_io_channel_destroy_cb(void *io_device, void *ctx_buf)
{
	struct spdk_virtio_scsi_ctrlr *ctrlr = io_device;
	struct virtio_scsi_io_channel_ctx *ch = ctx_buf;
	struct virtio_dev *vdev = spdk_virtio_dev_get_dev(&ctrlr->vdev);
	uint32_t i;

	assert(ctrlr->magic == VIRTIO_SCSI_CTRL_MAGIC);

	assert(ch->magic == VIRTIO_SCSI_IO_CHANNEL_MAGIC);
	spdk_bdev_poller_stop(&ch->poller);
	assert(TAILQ_EMPTY(&ch->pending_io));

	for (i = VIRTIO_SCSI_REQUESTQ; i < vdev->max_queues; i++) {
		if (vdev->vqs[i] != ch->vq) {
			continue;
		}

		ctrlr->vdev.is_vq_used[i] = 0;
		return;
	}

	SPDK_ERRLOG("Controller '%s': can't find virtual queue to stop!\n",
			spdk_virtio_dev_get_addr(&ctrlr->vdev));
}

struct virtio_scsi_scan_ctx {
	spdk_event_fn cpl_fn;
	void *cpl_arg;

	/* Phase of scanning */
	uint8_t opcode;

	/* Currently queried target */
	uint8_t target;

	/* IO vector for requests */
	struct iovec iov[2];

	uint8_t payload[BDEV_VIRTIO_SCAN_PAYLOAD_SIZE];

	/* Disks to be registered after the scan finishes */
//	TAILQ_HEAD(, virtio_scsi_dev) found_disks;
};

static void
virtio_scsi_scan_ctx_setup_iov(struct virtio_scsi_scan_ctx *scan)
{
	scan->iov[0].iov_base = &scan->target;
	scan->iov[0].iov_len = sizeof(scan->target);
	scan->iov[1].iov_base = scan->payload;
	scan->iov[1].iov_len = sizeof(scan->payload);
}

static void
virito_scsi_scan_target_cpl(struct spdk_bdev_io *bdev_io, bool success,	void *cb_arg)
{
	struct virtio_scsi_scan_ctx *scan = cb_arg;
	struct virtio_scsi_dev *sdev = bdev_io->bdev->ctxt;
	struct virtio_scsi_dev *new_sdev = NULL;
	struct spdk_virtio_scsi_ctrlr *ctrlr = sdev->ctrlr;
	uint64_t num_blocks, block_size;

	assert(ctrlr->magic == VIRTIO_SCSI_CTRL_MAGIC);
	assert(sdev->magic == VIRTIO_SCSI_DEV_MAGIC);

	if (!success) {
		SPDK_DEBUGLOG(SPDK_TRACE_VIRTIO, "Command 0x%02x failed for target %"PRIu8".\n", scan->opcode, scan->target);
		/* TODO: Check against removed devs */
		goto next_target;
	}

	switch(scan->opcode) {
	case VIRTIO_SCSI_IO_TYPE_INQUIRY:
		/* XXX: Use INQUIRY response. */;
		scan->opcode = VIRTIO_SCSI_IO_TYPE_READ_CAP_10;
		virtio_scsi_scan_ctx_setup_iov(scan);
		if (spdk_virtio_scsi_read_cap_10(ctrlr->bdev_desc, ctrlr->io_channel, scan->iov, 2, virito_scsi_scan_target_cpl, scan) == 0) {
			return;
		}

		break;
	case VIRTIO_SCSI_IO_TYPE_READ_CAP_10:
		num_blocks = from_be32(&scan->payload[0]);
		block_size = from_be32(&scan->payload[4]);
		if (num_blocks == 0xffffffff) {
			scan->opcode = VIRTIO_SCSI_IO_TYPE_READ_CAP_16;
			virtio_scsi_scan_ctx_setup_iov(scan);
			if (spdk_virtio_scsi_read_cap_16(ctrlr->bdev_desc, ctrlr->io_channel, scan->iov, 2, virito_scsi_scan_target_cpl, scan) == 0) {
				return;
			}
		} else {
			new_sdev = virtio_scsi_dev_alloc(ctrlr, scan->target, num_blocks, block_size);
		}
		break;
	/* What do we need this for? */
	case VIRTIO_SCSI_IO_TYPE_READ_CAP_16:
		num_blocks = from_be64(&scan->payload[0]) + 1;
		block_size = from_be32(&scan->payload[8]);
		new_sdev = virtio_scsi_dev_alloc(ctrlr, scan->target, num_blocks, block_size);
		break;
	default:
		SPDK_UNREACHABLE();
	}

	if (scan->cpl_fn && new_sdev) {
		scan->cpl_fn(scan, &new_sdev->bdev);
	}

next_target:
	for(scan->target++; scan->target < BDEV_VIRTIO_MAX_TARGET; scan->target++) {
		scan->opcode = SPDK_SPC_INQUIRY;
		virtio_scsi_scan_ctx_setup_iov(scan);
		if (spdk_virtio_scsi_inquiry(ctrlr->bdev_desc, ctrlr->io_channel, scan->iov, 2, virito_scsi_scan_target_cpl, scan) == 0){
			return;
		}
	}

	/*
	spdk_bdev_close(ctrlr->bdev_desc);
	ctrlr->bdev_desc = NULL;

	spdk_put_io_channel(ctrlr->io_channel);
	ctrlr->io_channel = NULL;
	*/

/*	base_bdev = &ctrlr->sdev->bdev;

	while ((disk = TAILQ_FIRST(&scan->found_disks))) {
		TAILQ_REMOVE(&scan->found_disks, disk, link);
		spdk_vbdev_register(&disk->bdev, &base_bdev, 1);

		if (scan->cpl_fn) {
			scan->cpl_fn(scan, &disk->bdev);
		}
	}
*/
	if (scan->cpl_fn) {
		scan->cpl_fn(scan, NULL);
	}

	spdk_dma_free(scan);
	spdk_bdev_module_init_done(SPDK_GET_BDEV_MODULE(virtio_scsi));
}

static int
virito_scsi_rescan(struct spdk_virtio_scsi_ctrlr *ctrlr, spdk_event_fn cpl_fn, void *arg)
{
	struct virtio_scsi_scan_ctx *scan = spdk_dma_zmalloc(sizeof(struct virtio_scsi_scan_ctx), 0, NULL);

	if (!scan) {
		return -ENOMEM;
	}

	assert(ctrlr->magic == VIRTIO_SCSI_CTRL_MAGIC);

	assert(ctrlr != NULL);

	scan->cpl_fn = cpl_fn;
	scan->cpl_arg = arg;
	scan->target = 0;
	scan->opcode = VIRTIO_SCSI_IO_TYPE_INQUIRY;
//	TAILQ_INIT(&scan->found_disks);
	virtio_scsi_scan_ctx_setup_iov(scan);
	return spdk_virtio_scsi_inquiry(ctrlr->bdev_desc, ctrlr->io_channel, scan->iov, 2, virito_scsi_scan_target_cpl, scan);
}

int
spdk_virtio_user_scsi_connect(const char *path, uint32_t max_queue, uint32_t vq_size, spdk_event_fn done_cb, void *cb_ctx)
{
	struct spdk_virtio_scsi_ctrlr *ctrlr;
	struct virtio_scsi_dev *sdev;
	struct spdk_bdev *bdev;
	struct stat file_stat;
	int rc = 0;

	if (path == NULL) {
		SPDK_ERRLOG("Missing path to the socket\n");
		rc = -EINVAL;
		goto invalid;
	}

	if (stat(path, &file_stat) != 0) {
		SPDK_ERRLOG("Invalid socket path\n");
		rc = -EIO;
		goto invalid;
	} else if (!S_ISSOCK(file_stat.st_mode)) {
		SPDK_ERRLOG("Path %s: not a socket.\n", path);
		rc = -EIO;
		goto invalid;
	}

	if (spdk_virtio_dev_find_by_path(path) != NULL) {
		SPDK_ERRLOG("Path %s: controller already exist.\n", path);
		return -EEXIST;
	}

	ctrlr = calloc(1, sizeof(*ctrlr));
	if (ctrlr == NULL) {
		SPDK_ERRLOG("Path %s: failed to allocate controller.\n", path);
		rc = -ENOMEM;
		goto invalid;
	}



	ctrlr->vdev.dev = virtio_user_dev_init(path, max_queue, vq_size);
	if (!ctrlr->vdev.dev) {
		rc = -EIO;
		goto invalid;
	}

	strncpy(ctrlr->vdev.path, path, sizeof(ctrlr->vdev.path));

	// LOCK?
	rc = spdk_virtio_scsi_dev_init(ctrlr->vdev.dev);
	if (rc < 0) {
		goto invalid;
	}

	sdev = calloc(1, sizeof(struct virtio_scsi_dev));
	if (!sdev) {
		rc = -ENOMEM;
		goto invalid;
	}
	ctrlr->sdev = sdev;
	sdev->ctrlr = ctrlr;

	/*
	 * Create fake bdev so we can watch entire device
	 */
	bdev = &sdev->bdev;
	bdev->name = spdk_sprintf_alloc("VirtioScsiController%d", g_virtio_ctrlr_count);
	g_virtio_ctrlr_count++;

	bdev->product_name = "Virtio SCSI Device";
	bdev->write_cache = 0;
	bdev->blocklen = 1;
	bdev->blockcnt = 0;

	bdev->ctxt = sdev;
	bdev->fn_table = &virtio_ctrlr_fn_table;
	bdev->module = SPDK_GET_BDEV_MODULE(virtio_scsi);

	ctrlr->magic = VIRTIO_SCSI_CTRL_MAGIC;
	sdev->magic = VIRTIO_SCSI_DEV_MAGIC;


	spdk_io_device_register(ctrlr, virtio_scsi_io_channel_create, virtio_scsi_io_channel_destroy_cb, sizeof(struct virtio_scsi_io_channel_ctx));
	spdk_bdev_register(bdev);

	TAILQ_INSERT_TAIL(&g_spdk_vdevs, &ctrlr->vdev, vdev_link);

	/* Open can't fail */
	rc = spdk_bdev_open(bdev, true, bdev_virtio_scsi_device_remove_cb, ctrlr, &ctrlr->bdev_desc);
	assert(rc == 0);
	if (rc) {
		goto invalid_unregister;
	}

	/* Claim can't fail */
	rc = spdk_bdev_module_claim_bdev(bdev, ctrlr->bdev_desc, SPDK_GET_BDEV_MODULE(virtio_scsi));
	assert(rc == 0);
	if (rc) {
		goto invalid_close;
	}

	ctrlr->io_channel = spdk_get_io_channel(bdev);
	if (!ctrlr->io_channel) {
		SPDK_ERRLOG("Path %s: failed to allocate management io channel.\n", path);
		spdk_io_device_unregister(ctrlr, NULL);
		rc = -EIO;
		goto invalid_close;
	}


	/* Rescan can't fail */
	rc = virito_scsi_rescan(ctrlr, done_cb, cb_ctx);
	assert(rc == 0);
	if (rc != 0) {
		goto invalid_close;
	}
	// UNLOCK ?

	return 0;
invalid_close:
	spdk_bdev_close(ctrlr->bdev_desc);
invalid_unregister:
	spdk_bdev_unregister(bdev);
	TAILQ_REMOVE(&g_spdk_vdevs, &ctrlr->vdev, vdev_link);
invalid:
	if (ctrlr->vdev.dev) {
		virtio_user_dev_uninit(ctrlr->vdev.dev);
	}

	free(ctrlr);
	return rc;
}

static int
bdev_virtio_scsi_initialize(void)
{
	struct spdk_conf_section *sp = spdk_conf_find_section(NULL, "Virtio");
	char *type, *path;
	uint32_t max_queue = spdk_env_get_core_count();
	uint32_t i;
	int rc = 0;

	SPDK_ERRLOG("=== Initialize ===\n");
	if (sp == NULL) {
		spdk_bdev_module_init_done(SPDK_GET_BDEV_MODULE(virtio_scsi));
		goto done;
	}

	for (i = 0; spdk_conf_section_get_nval(sp, "Dev", i) != NULL; i++) {
		type = spdk_conf_section_get_nmval(sp, "Dev", i, 0);
		if (type == NULL) {
			SPDK_ERRLOG("No type specified for index %d\n", i);
			return -EINVAL;
		}
		if (!strcmp("User", type)) {
			path = spdk_conf_section_get_nmval(sp, "Dev", i, 1);
			if (path == NULL) {
				SPDK_ERRLOG("No path specified for index %d\n", i);
				return -EINVAL;
			}
			rc = spdk_virtio_user_scsi_connect(path, max_queue, 512, NULL, NULL);
			if (rc) {
				return rc;
			}
		} else if (!strcmp("Pci", type)) {
//			vdev = get_pci_virtio_hw();
			continue;
		} else {
			SPDK_ERRLOG("Invalid type %s specified for index %d\n", type, i);
			return -1;
		}
	}
done:
	SPDK_ERRLOG("=== Initialize done ===\n");
	return 0;
}

static void
bdev_virtio_scsi_finish(void)
{
	struct spdk_virtio_dev *dev;
	struct spdk_virtio_scsi_ctrlr *ctrlr;
	struct virtio_scsi_dev *sdev;
	size_t i;

	SPDK_ERRLOG("\n\n");
	TAILQ_FOREACH(dev, &g_spdk_vdevs, vdev_link) {
		ctrlr = SPDK_CONTAINEROF(dev, struct spdk_virtio_scsi_ctrlr, vdev);
		sdev = ctrlr->sdev;

		SPDK_ERRLOG("%s: IO queued: %u, IO pending: %u\n",
			   sdev->bdev.name, sdev->io_in_queue_cnt, sdev->io_pending_cnt);


		for (i = 0; i < SPDK_COUNTOF(ctrlr->scsi_devs); i++) {
			sdev = ctrlr->scsi_devs[i];
			if (!sdev) {
				continue;
			}

			SPDK_ERRLOG("%s: IO queued: %u, IO pending: %u\n",
				   sdev->bdev.name, sdev->io_in_queue_cnt, sdev->io_pending_cnt);
		}
	}
	SPDK_ERRLOG("\n\n");
	/* TODO: cleanup */
}

static int
bdev_virtio_scsi_get_ctx_size(void)
{
	return sizeof(struct virtio_scsi_io_ctx);
}

SPDK_LOG_REGISTER_TRACE_FLAG("virtio", SPDK_TRACE_VIRTIO);
