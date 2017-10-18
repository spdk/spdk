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

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

#include <linux/virtio_scsi.h>

#include <virtio_dev.h>
#include <virtio_user/virtio_user_dev.h>

#define BDEV_VIRTIO_MAX_TARGET 64
#define BDEV_VIRTIO_SCAN_PAYLOAD_SIZE 256

#define VIRTIO_SCSI_CONTROLQ	0
#define VIRTIO_SCSI_EVENTQ	1
#define VIRTIO_SCSI_REQUESTQ	2

static int bdev_virtio_initialize(void);
static void bdev_virtio_finish(void);

struct virtio_scsi_io_ctx {
	struct virtio_req 		vreq;
	struct virtio_scsi_cmd_req 	req;
	struct virtio_scsi_cmd_resp 	resp;
};

struct virtio_scsi_scan_base {
	struct virtio_dev		*vdev;

	/** Virtqueue used for the scan I/O. */
	struct virtqueue		*vq;

	/* Currently queried target */
	unsigned			target;

	/* Disks to be registered after the scan finishes */
	TAILQ_HEAD(, virtio_scsi_disk) found_disks;

	struct virtio_scsi_io_ctx	io_ctx;
	struct iovec			iov;
	uint8_t				payload[BDEV_VIRTIO_SCAN_PAYLOAD_SIZE];
};

struct virtio_scsi_disk {
	struct spdk_bdev	bdev;
	struct virtio_dev	*vdev;
	uint64_t		num_blocks;
	uint32_t		block_size;
	uint8_t			target;
	TAILQ_ENTRY(virtio_scsi_disk) link;
};

struct bdev_virtio_io_channel {
	struct virtio_dev	*vdev;

	/** Virtqueue exclusively assigned to this channel. */
	struct virtqueue	*vq;
};

static void scan_target(struct virtio_scsi_scan_base *base);

static int
bdev_virtio_get_ctx_size(void)
{
	return sizeof(struct virtio_scsi_io_ctx);
}

SPDK_BDEV_MODULE_REGISTER(virtio_scsi, bdev_virtio_initialize, bdev_virtio_finish,
			  NULL, bdev_virtio_get_ctx_size, NULL)

SPDK_BDEV_MODULE_ASYNC_INIT(virtio_scsi)

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
	req->lun[1] = disk->target;

	return vreq;
}

static void
bdev_virtio_rw(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_scsi_disk *disk = SPDK_CONTAINEROF(bdev_io->bdev, struct virtio_scsi_disk, bdev);
	struct virtio_req *vreq = bdev_virtio_init_io_vreq(ch, bdev_io);
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;
	struct bdev_virtio_io_channel *virtio_channel = spdk_io_channel_get_ctx(ch);

	vreq->iov = bdev_io->u.bdev.iovs;
	vreq->iovcnt = bdev_io->u.bdev.iovcnt;

	if (disk->num_blocks > (1ULL << 32)) {
		req->cdb[0] = vreq->is_write ? SPDK_SBC_WRITE_16 : SPDK_SBC_READ_16;
		to_be64(&req->cdb[2], bdev_io->u.bdev.offset_blocks);
		to_be32(&req->cdb[10], bdev_io->u.bdev.num_blocks);
	} else {
		req->cdb[0] = vreq->is_write ? SPDK_SBC_WRITE_10 : SPDK_SBC_READ_10;
		to_be32(&req->cdb[2], bdev_io->u.bdev.offset_blocks);
		to_be16(&req->cdb[7], bdev_io->u.bdev.num_blocks);
	}

	virtio_xmit_pkts(virtio_channel->vq, vreq);
}

static void
bdev_virtio_unmap(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_req *vreq = bdev_virtio_init_io_vreq(ch, bdev_io);
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;
	struct bdev_virtio_io_channel *virtio_channel = spdk_io_channel_get_ctx(ch);
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

	virtio_xmit_pkts(virtio_channel->vq, vreq);
}

static int _bdev_virtio_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_virtio_rw,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return 0;
	case SPDK_BDEV_IO_TYPE_WRITE:
		bdev_virtio_rw(ch, bdev_io);
		return 0;
	case SPDK_BDEV_IO_TYPE_RESET:
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;
	case SPDK_BDEV_IO_TYPE_UNMAP: {
		uint64_t buf_len = 8 /* header size */ +
				   (bdev_io->u.bdev.num_blocks + UINT32_MAX - 1) /
				   UINT32_MAX * sizeof(struct spdk_scsi_unmap_bdesc);

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
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		return true;

	default:
		return false;
	}
}

static struct spdk_io_channel *
bdev_virtio_get_io_channel(void *ctx)
{
	struct virtio_scsi_disk *disk = ctx;

	return spdk_get_io_channel(&disk->vdev);
}

static int
bdev_virtio_destruct(void *ctx)
{
	return 0;
}

static const struct spdk_bdev_fn_table virtio_fn_table = {
	.destruct		= bdev_virtio_destruct,
	.submit_request		= bdev_virtio_submit_request,
	.io_type_supported	= bdev_virtio_io_type_supported,
	.get_io_channel		= bdev_virtio_get_io_channel,
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

static int
bdev_virtio_create_cb(void *io_device, void *ctx_buf)
{
	struct virtio_dev **vdev_ptr = io_device;
	struct virtio_dev *vdev = *vdev_ptr;
	struct bdev_virtio_io_channel *ch = ctx_buf;
	struct virtqueue *vq;
	int32_t queue_idx;

	queue_idx = virtio_dev_find_and_acquire_queue(vdev, VIRTIO_SCSI_REQUESTQ);
	if (queue_idx < 0) {
		SPDK_ERRLOG("Couldn't get an unused queue for the io_channel.\n");
		return queue_idx;
	}

	vq = vdev->vqs[queue_idx];

	ch->vdev = vdev;
	ch->vq = vq;

	spdk_bdev_poller_start(&vq->poller, bdev_virtio_poll, ch,
			       vq->owner_lcore, 0);

	return 0;
}

static void
bdev_virtio_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_virtio_io_channel *io_channel = ctx_buf;
	struct virtio_dev *vdev = io_channel->vdev;
	struct virtqueue *vq = io_channel->vq;

	spdk_bdev_poller_stop(&vq->poller);
	virtio_dev_release_queue(vdev, vq->vq_queue_index);
}

static void
scan_target_finish(struct virtio_scsi_scan_base *base)
{
	struct virtio_scsi_disk *disk;

	base->target++;
	if (base->target < BDEV_VIRTIO_MAX_TARGET) {
		scan_target(base);
		return;
	}

	spdk_bdev_poller_stop(&base->vq->poller);
	virtio_dev_release_queue(base->vdev, base->vq->vq_queue_index);

	while ((disk = TAILQ_FIRST(&base->found_disks))) {
		TAILQ_REMOVE(&base->found_disks, disk, link);
		spdk_io_device_register(&disk->vdev, bdev_virtio_create_cb, bdev_virtio_destroy_cb,
					sizeof(struct bdev_virtio_io_channel));
		spdk_bdev_register(&disk->bdev);
	}

	TAILQ_REMOVE(&g_virtio_driver.init_ctrlrs, base->vdev, tailq);
	TAILQ_INSERT_TAIL(&g_virtio_driver.attached_ctrlrs, base->vdev, tailq);

	spdk_dma_free(base);

	if (TAILQ_EMPTY(&g_virtio_driver.init_ctrlrs)) {
		spdk_bdev_module_init_done(SPDK_GET_BDEV_MODULE(virtio_scsi));
	}
}

static void
send_read_cap_10(struct virtio_scsi_scan_base *base, uint8_t target_id, struct virtio_req *vreq)
{
	struct iovec *iov = vreq->iov;
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;

	memset(req, 0, sizeof(*req));
	req->lun[0] = 1;
	req->lun[1] = target_id;

	iov[0].iov_len = 8;
	req->cdb[0] = SPDK_SBC_READ_CAPACITY_10;

	virtio_xmit_pkts(base->vq, vreq);
}

static void
send_read_cap_16(struct virtio_scsi_scan_base *base, uint8_t target_id, struct virtio_req *vreq)
{
	struct iovec *iov = vreq->iov;
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;

	memset(req, 0, sizeof(*req));
	req->lun[0] = 1;
	req->lun[1] = target_id;

	iov[0].iov_len = 32;
	req->cdb[0] = SPDK_SPC_SERVICE_ACTION_IN_16;
	req->cdb[1] = SPDK_SBC_SAI_READ_CAPACITY_16;
	to_be32(&req->cdb[10], iov[0].iov_len);

	virtio_xmit_pkts(base->vq, vreq);
}

static int
process_scan_inquiry(struct virtio_scsi_scan_base *base, struct virtio_req *vreq)
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
	send_read_cap_10(base, target_id, vreq);
	return 0;
}

static int
alloc_virtio_disk(struct virtio_scsi_scan_base *base, uint8_t target_id, uint64_t num_blocks,
		  uint32_t block_size)
{
	struct virtio_scsi_disk *disk;
	struct spdk_bdev *bdev;

	disk = calloc(1, sizeof(*disk));
	if (disk == NULL) {
		SPDK_ERRLOG("could not allocate disk\n");
		return -1;
	}

	disk->vdev = base->vdev;
	disk->num_blocks = num_blocks;
	disk->block_size = block_size;
	disk->target = target_id;

	bdev = &disk->bdev;
	bdev->name = spdk_sprintf_alloc("VirtioScsi%"PRIu32"t%"PRIu8, disk->vdev->id, target_id);
	if (bdev->name == NULL) {
		SPDK_ERRLOG("Couldn't alloc memory for the bdev name.\n");
		free(disk);
		return -1;
	}

	bdev->product_name = "Virtio SCSI Disk";
	bdev->write_cache = 0;
	bdev->blocklen = disk->block_size;
	bdev->blockcnt = disk->num_blocks;

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

	return alloc_virtio_disk(base, target_id, max_block + 1, block_size);
}

static int
process_read_cap_16(struct virtio_scsi_scan_base *base, struct virtio_req *vreq)
{
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;
	struct virtio_scsi_cmd_resp *resp = vreq->iov_resp.iov_base;
	uint64_t num_blocks;
	uint32_t block_size;
	uint8_t target_id = req->lun[1];

	if (resp->response != VIRTIO_SCSI_S_OK || resp->status != SPDK_SCSI_STATUS_GOOD) {
		SPDK_ERRLOG("READ CAPACITY (16) failed for target %"PRIu8".\n", target_id);
		return -1;
	}

	num_blocks = from_be64((uint64_t *)(vreq->iov[0].iov_base)) + 1;
	block_size = from_be32((uint32_t *)(vreq->iov[0].iov_base + 8));
	return alloc_virtio_disk(base, target_id, num_blocks, block_size);
}

static void
process_scan_resp(struct virtio_scsi_scan_base *base, struct virtio_req *vreq)
{
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;
	int rc;

	if (vreq->iov_req.iov_len < sizeof(struct virtio_scsi_cmd_req) ||
	    vreq->iov_resp.iov_len < sizeof(struct virtio_scsi_cmd_resp)) {
		SPDK_ERRLOG("Received target scan message with invalid length.\n");
		scan_target_finish(base);
		return;
	}

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

static void
scan_target(struct virtio_scsi_scan_base *base)
{
	struct iovec *iov;
	struct virtio_req *vreq;
	struct virtio_scsi_cmd_req *req;
	struct virtio_scsi_cmd_resp *resp;
	struct spdk_scsi_cdb_inquiry *cdb;

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
	req->lun[1] = base->target;

	cdb = (struct spdk_scsi_cdb_inquiry *)req->cdb;
	cdb->opcode = SPDK_SPC_INQUIRY;
	cdb->alloc_len[1] = 255;

	virtio_xmit_pkts(base->vq, vreq);
}

static int
bdev_virtio_process_config(void)
{
	struct spdk_conf_section *sp;
	struct virtio_dev *vdev = NULL;
	char *path;
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

		vdev = virtio_user_dev_init(path, num_queues, 512);
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
		rc = vtpci_enumerate_pci();
	}

out:
	return rc;
}


static int
bdev_virtio_initialize(void)
{
	struct virtio_scsi_scan_base *base;
	struct virtio_dev *vdev, *next_vdev;
	struct virtqueue *vq;
	int rc = 0;

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
		base = spdk_dma_zmalloc(sizeof(*base), 64, NULL);
		if (base == NULL) {
			SPDK_ERRLOG("couldn't allocate memory for scsi target scan.\n");
			rc = -1;
			goto out;
		}

		rc = virtio_dev_init(vdev, VIRTIO_SCSI_DEV_SUPPORTED_FEATURES);
		if (rc != 0) {
			spdk_dma_free(base);
			goto out;
		}

		rc = virtio_dev_start(vdev);
		if (rc != 0) {
			spdk_dma_free(base);
			goto out;
		}

		base->vdev = vdev;
		TAILQ_INIT(&base->found_disks);

		rc = virtio_dev_acquire_queue(vdev, VIRTIO_SCSI_REQUESTQ);
		if (rc != 0) {
			SPDK_ERRLOG("Couldn't acquire requestq for the target scan.\n");
			spdk_dma_free(base);
			goto out;
		}

		vq = vdev->vqs[VIRTIO_SCSI_REQUESTQ];
		base->vq = vq;
		vq->poller_ctx = base;
		spdk_bdev_poller_start(&vq->poller, bdev_scan_poll, base,
				       vq->owner_lcore, 0);

		scan_target(base);
	}

	return 0;

out:
	/* Remove any created devices */
	TAILQ_FOREACH_SAFE(vdev, &g_virtio_driver.init_ctrlrs, tailq, next_vdev) {
		TAILQ_REMOVE(&g_virtio_driver.init_ctrlrs, vdev, tailq);
		if (virtio_dev_queue_is_acquired(vdev, VIRTIO_SCSI_REQUESTQ)) {
			vq = vdev->vqs[VIRTIO_SCSI_REQUESTQ];
			spdk_bdev_poller_stop(&vq->poller);
			spdk_dma_free(vq->poller_ctx);
			vq->poller_ctx = NULL;
			virtio_dev_release_queue(vdev, VIRTIO_SCSI_REQUESTQ);
		}
		/* since scan pollers couldn't do a single tick yet.
		 * it's safe just to free the vdev now.
		 */
		virtio_dev_free(vdev);
	}

	spdk_bdev_module_init_done(SPDK_GET_BDEV_MODULE(virtio_scsi));
	return rc;
}

static void bdev_virtio_finish(void)
{
	struct virtio_dev *vdev, *next;

	TAILQ_FOREACH_SAFE(vdev, &g_virtio_driver.attached_ctrlrs, tailq, next) {
		virtio_dev_free(vdev);
	}
}

SPDK_LOG_REGISTER_TRACE_FLAG("virtio", SPDK_TRACE_VIRTIO)
