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
#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/string.h"
#include "spdk/endian.h"
#include "spdk/stdinc.h"
#include "spdk/util.h"
#include "spdk/scsi_spec.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

#include <getopt.h>
#include <sys/param.h>

#include <linux/virtio_scsi.h>

#include <virtio_dev.h>
#include <virtio_user/virtio_user_dev.h>

#include "spdk/scsi_spec.h"
#include "bdev_virtio.h"

#define BDEV_VIRTIO_MAX_TARGET 64
#define BDEV_VIRTIO_SCAN_PAYLOAD_SIZE 256

static int bdev_virtio_initialize(void);
static void bdev_virtio_finish(void);

struct virtio_scsi_io_ctx {
	struct virtio_req 		vreq;
	struct virtio_scsi_cmd_req 	req;
	struct virtio_scsi_cmd_resp 	resp;
};

struct virtio_scsi_scan_base {
	struct virtio_dev		*vdev;
	struct spdk_bdev_poller		*scan_poller;

	const char *prefix;
	bool in_module_init;

	virtio_scsi_add_bdev_cb cb_fn;
	void *cb_arg;

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
	struct spdk_bdev_poller	*poller;
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
bdev_virtio_init_vreq(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
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
	struct virtio_scsi_disk *disk = (struct virtio_scsi_disk *)bdev_io->bdev;
	struct virtio_req *vreq = bdev_virtio_init_vreq(ch, bdev_io);
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;

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

	virtio_xmit_pkts(disk->vdev->vqs[2], vreq);
}

static void
bdev_virtio_unmap(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_scsi_disk *disk = (struct virtio_scsi_disk *)bdev_io->bdev;
	struct virtio_req *vreq = bdev_virtio_init_vreq(ch, bdev_io);
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;
	struct spdk_scsi_unmap_bdesc *desc;
	uint8_t *buf;
	uint16_t cmd_len = 24; /* 8 byte header + 16 byte descriptor */

	vreq->iov = bdev_io->u.bdev.iovs;
	vreq->iovcnt = bdev_io->u.bdev.iovcnt;

	req->cdb[0] = SPDK_SBC_UNMAP;
	to_be16(&req->cdb[7], cmd_len);

	buf = vreq->iov->iov_base;

	/* header */
	to_be16(&buf[0], cmd_len - 2); /* total length (excluding the length field) */
	to_be16(&buf[2], cmd_len - 8); /* length of block descriptors */
	memset(&buf[4], 0, 4); /* reserved */

	/* single block descriptor */
	desc = (struct spdk_scsi_unmap_bdesc *)&buf[8];
	to_be64(&desc->lba, bdev_io->u.bdev.offset_blocks);
	to_be32(&desc->block_count, bdev_io->u.bdev.num_blocks);
	memset(&desc->reserved, 0, sizeof(desc->reserved));

	virtio_xmit_pkts(disk->vdev->vqs[2], vreq);
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
	case SPDK_BDEV_IO_TYPE_UNMAP:
		if (bdev_io->u.bdev.num_blocks > UINT32_MAX) {
			SPDK_ERRLOG("single UNMAP block count must be no bigger than 2^32 - 1\n");
			return -1;
		}
		/* Since we support only bdev_io->u.bdev.num_blocks <= UINT32_MAX
		 * allocate just 24 bytes (8 byte header + 16 byte descriptor).
		 * A single block descriptor can UNMAP at most UINT32_MAX blocks.
		 */
		spdk_bdev_io_get_buf(bdev_io, bdev_virtio_unmap, 24);
		return 0;
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

	cnt = virtio_recv_pkts(ch->vdev->vqs[2], req, SPDK_COUNTOF(req));
	for (i = 0; i < cnt; ++i) {
		bdev_virtio_io_cpl(req[i]);
	}
}

static int
bdev_virtio_create_cb(void *io_device, void *ctx_buf)
{
	struct virtio_dev **vdev = io_device;
	struct bdev_virtio_io_channel *ch = ctx_buf;

	ch->vdev = *vdev;
	spdk_bdev_poller_start(&ch->poller, bdev_virtio_poll, ch,
			       spdk_env_get_current_core(), 0);
	return 0;
}

static void
bdev_virtio_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_virtio_io_channel *io_channel = ctx_buf;

	spdk_bdev_poller_stop(&io_channel->poller);
}

static void
scan_target_finish(struct virtio_scsi_scan_base *base)
{
	struct virtio_scsi_disk *disk;
	struct spdk_bdev *bdevs[BDEV_VIRTIO_MAX_TARGET];
	size_t bdevs_cnt;

	base->target++;
	if (base->target < BDEV_VIRTIO_MAX_TARGET) {
		scan_target(base);
		return;
	}

	spdk_bdev_poller_stop(&base->scan_poller);

	bdevs_cnt = 0;

	while ((disk = TAILQ_FIRST(&base->found_disks))) {
		TAILQ_REMOVE(&base->found_disks, disk, link);
		spdk_io_device_register(&disk->vdev, bdev_virtio_create_cb, bdev_virtio_destroy_cb,
					sizeof(struct bdev_virtio_io_channel));
		spdk_bdev_register(&disk->bdev);

		bdevs[bdevs_cnt] = &disk->bdev;
		bdevs_cnt++;
	}

	if (base->cb_fn) {
		base->cb_fn(base->cb_arg, bdevs, bdevs_cnt);
	}


	TAILQ_REMOVE(&g_virtio_driver.init_ctrlrs, base->vdev, tailq);
	TAILQ_INSERT_TAIL(&g_virtio_driver.attached_ctrlrs, base->vdev, tailq);

	if (base->in_module_init && TAILQ_EMPTY(&g_virtio_driver.init_ctrlrs)) {
		spdk_bdev_module_init_done(SPDK_GET_BDEV_MODULE(virtio_scsi));
	}

	spdk_dma_free(base);
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

	virtio_xmit_pkts(base->vdev->vqs[2], vreq);
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

	virtio_xmit_pkts(base->vdev->vqs[2], vreq);
}

static int
process_scan_inquiry(struct virtio_scsi_scan_base *base, struct virtio_req *vreq)
{
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;
	struct virtio_scsi_cmd_resp *resp = vreq->iov_resp.iov_base;
	uint8_t target_id;

	if (resp->response != VIRTIO_SCSI_S_OK || resp->status != SPDK_SCSI_STATUS_GOOD) {
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
	if (base->prefix) {
		bdev->name = spdk_sprintf_alloc("%st%u", base->prefix, target_id);
	} else {
		bdev->name = spdk_sprintf_alloc("VirtioScsi%ut%u", disk->vdev->id, target_id);
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

	cnt = virtio_recv_pkts(base->vdev->vqs[2], &req, 1);
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

	virtio_xmit_pkts(base->vdev->vqs[2], vreq);
}

static int
bdev_virtio_init_dev(struct virtio_dev *vdev, const char *prefix, virtio_scsi_add_bdev_cb cb_fn,
		     void *cb_arg,
		     bool in_init)
{
	struct virtio_scsi_scan_base *base = spdk_dma_zmalloc(sizeof(*base), 64, NULL);
	int rc = 0;

	if (base == NULL) {
		SPDK_ERRLOG("couldn't allocate memory for scsi target scan.\n");
		return -ENOMEM;
	}

	rc = virtio_dev_init(vdev, VIRTIO_SCSI_DEV_SUPPORTED_FEATURES);
	if (rc != 0) {
		spdk_dma_free(base);
		return -1;
	}

	base->prefix = prefix;
	rc = virtio_dev_start(vdev);
	if (rc != 0) {
		spdk_dma_free(base);
		return -1;
	}

	base->vdev = vdev;
	base->cb_fn = cb_fn;
	base->cb_arg = cb_arg;
	base->in_module_init = in_init;
	TAILQ_INIT(&base->found_disks);

	spdk_bdev_poller_start(&base->scan_poller, bdev_scan_poll, base,
			       spdk_env_get_current_core(), 0);

	scan_target(base);
	return 0;
}

int
create_virtio_user_scsi_device(const char *path, const char *prefix, int queue_size,
			       virtio_scsi_add_bdev_cb done_cb, void *cb_arg)
{
	struct virtio_dev *vdev = virtio_user_dev_init(path, 512);
	int rc;

	if (!vdev) {
		return -EIO;
	}

	rc = bdev_virtio_init_dev(vdev, prefix, done_cb, cb_arg, false);
	if (rc) {
		virtio_dev_free(vdev);
	} else {
		TAILQ_INSERT_TAIL(&g_virtio_driver.init_ctrlrs, vdev, tailq);
	}

	return rc;
}

static int
bdev_virtio_initialize(void)
{
	struct spdk_conf_section *sp = spdk_conf_find_section(NULL, "Virtio");
	struct virtio_dev *vdev, *tmp_vdev;
	char *type, *path;
	uint32_t i;
	int rc = 0;
	bool scan_pci = false;

	if (sp == NULL) {
		goto out;
	}

	for (i = 0; spdk_conf_section_get_nval(sp, "Dev", i) != NULL; i++) {
		type = spdk_conf_section_get_nmval(sp, "Dev", i, 0);
		if (type == NULL) {
			SPDK_ERRLOG("No type specified for index %d\n", i);
			continue;
		}
		if (!strcmp("User", type)) {
			path = spdk_conf_section_get_nmval(sp, "Dev", i, 1);
			if (path == NULL) {
				SPDK_ERRLOG("No path specified for index %d\n", i);
				continue;
			}
			if (virtio_user_dev_init(path, 512) == NULL) {
				rc = -EIO;
				goto out;
			}
		} else if (!strcmp("Pci", type)) {
			scan_pci = true;
		} else {
			SPDK_ERRLOG("Invalid type %s specified for index %d\n", type, i);
			continue;
		}
	}

	if (scan_pci) {
		vtpci_enumerate_pci();
	}

	TAILQ_FOREACH_SAFE(vdev, &g_virtio_driver.init_ctrlrs, tailq, tmp_vdev) {
		rc = bdev_virtio_init_dev(vdev, NULL, NULL, NULL, true);
		if (rc) {
			goto out;
		}
	}

	return 0;

out:
	spdk_bdev_module_init_done(SPDK_GET_BDEV_MODULE(virtio_scsi));
	return rc;
}

static void bdev_virtio_finish(void)
{
}

SPDK_LOG_REGISTER_TRACE_FLAG("virtio", SPDK_TRACE_VIRTIO)
