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

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

#include <getopt.h>
#include <sys/param.h>

#include <linux/virtio_scsi.h>

#include <virtio_ethdev.h>
#include <virtio_user/virtio_user_dev.h>

#include "spdk/scsi_spec.h"

static int bdev_virtio_initialize(void);
static void bdev_virtio_finish(void);

struct virtio_scsi_disk {
	struct spdk_bdev	bdev;
	struct virtio_hw	*hw;
	uint64_t		num_blocks;
	uint32_t		block_size;
};

struct virtio_scsi_io_ctx {
	struct virtio_req 		vreq;
	struct virtio_scsi_cmd_req 	req;
	struct virtio_scsi_cmd_resp 	resp;
};

struct bdev_virtio_io_channel {
	struct virtio_hw	*hw;
	struct spdk_bdev_poller	*poller;
};

static int
bdev_virtio_get_ctx_size(void)
{
	return sizeof(struct virtio_scsi_io_ctx);
}

SPDK_BDEV_MODULE_REGISTER(virtio_scsi, bdev_virtio_initialize, bdev_virtio_finish,
			  NULL, bdev_virtio_get_ctx_size, NULL)

static void
bdev_virtio_rw(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct virtio_req *vreq;
	struct virtio_scsi_cmd_req *req;
	struct virtio_scsi_cmd_resp *resp;
	struct virtio_scsi_disk *disk = (struct virtio_scsi_disk *)bdev_io->bdev;
	struct virtio_scsi_io_ctx *io_ctx = (struct virtio_scsi_io_ctx *)bdev_io->driver_ctx;
	bool is_read = (bdev_io->type == SPDK_BDEV_IO_TYPE_READ);

	vreq = &io_ctx->vreq;
	req = &io_ctx->req;
	resp = &io_ctx->resp;

	vreq->iov_req.iov_base = (void *)req;
	vreq->iov_req.iov_len = sizeof(*req);

	vreq->iov_resp.iov_base = (void *)resp;
	vreq->iov_resp.iov_len = sizeof(*resp);

	vreq->is_write = !is_read;

	memset(req, 0, sizeof(*req));
	req->lun[0] = 1;
	req->lun[1] = 0;

	if (is_read) {
		vreq->iov = bdev_io->u.read.iovs;
		vreq->iovcnt = bdev_io->u.read.iovcnt;
		req->cdb[0] = SPDK_SBC_READ_10;
		to_be32(&req->cdb[2], bdev_io->u.read.offset / disk->block_size);
		to_be16(&req->cdb[7], bdev_io->u.read.len / disk->block_size);
	} else {
		vreq->iov = bdev_io->u.write.iovs;
		vreq->iovcnt = bdev_io->u.write.iovcnt;
		req->cdb[0] = SPDK_SBC_WRITE_10;
		to_be32(&req->cdb[2], bdev_io->u.write.offset / disk->block_size);
		to_be16(&req->cdb[7], bdev_io->u.write.len / disk->block_size);
	}

	virtio_xmit_pkts(disk->hw->vqs[2], vreq);
}

static int _bdev_virtio_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_virtio_rw);
		return 0;
	case SPDK_BDEV_IO_TYPE_WRITE:
		bdev_virtio_rw(ch, bdev_io);
		return 0;
	case SPDK_BDEV_IO_TYPE_RESET:
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
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

	return spdk_get_io_channel(&disk->hw);
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

	cnt = virtio_recv_pkts(ch->hw->vqs[2], req, SPDK_COUNTOF(req));
	for (i = 0; i < cnt; ++i) {
		bdev_virtio_io_cpl(req[i]);
	}
}

static int
bdev_virtio_create_cb(void *io_device, void *ctx_buf)
{
	struct virtio_hw **hw = io_device;
	struct bdev_virtio_io_channel *ch = ctx_buf;

	ch->hw = *hw;
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

static int
process_scan_inquiry(struct virtio_hw *hw, struct virtio_req *vreq)
{
	struct iovec *iov = vreq->iov;
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;
	struct virtio_scsi_cmd_resp *resp = vreq->iov_resp.iov_base;
	uint8_t lun_id;

	if (resp->response != VIRTIO_SCSI_S_OK || resp->status != SPDK_SCSI_STATUS_GOOD) {
		return -1;
	}

	lun_id = req->lun[1];
	/* reuse vreq for next request */
	memset(req, 0, sizeof(*req));
	req->lun[0] = 1;
	req->lun[1] = lun_id;

	req->cdb[0] = SPDK_SPC_SERVICE_ACTION_IN_16;
	req->cdb[1] = SPDK_SBC_SAI_READ_CAPACITY_16;

	iov[0].iov_len = 32;
	to_be32(&req->cdb[10], iov[0].iov_len);

	virtio_xmit_pkts(hw->vqs[2], vreq);
	return 0;
}

static int
process_read_cap(struct virtio_hw *hw, struct virtio_req *vreq)
{
	struct virtio_scsi_disk *disk;
	struct spdk_bdev *bdev;
	struct virtio_scsi_cmd_req *req = vreq->iov_req.iov_base;
	struct virtio_scsi_cmd_resp *resp = vreq->iov_resp.iov_base;

	if (resp->response != VIRTIO_SCSI_S_OK || resp->status != SPDK_SCSI_STATUS_GOOD) {
		SPDK_ERRLOG("read capacity failed for target %"PRIu8".\n", req->lun[1]);
		return -1;
	}

	disk = calloc(1, sizeof(*disk));
	if (disk == NULL) {
		SPDK_ERRLOG("could not allocate disk\n");
		return -1;
	}

	disk->num_blocks = from_be64((uint64_t *)(vreq->iov[0].iov_base)) + 1;
	disk->block_size = from_be32((uint32_t *)(vreq->iov[0].iov_base + 8));

	disk->hw = hw;

	bdev = &disk->bdev;
	bdev->name = spdk_sprintf_alloc("Virtio0");
	bdev->product_name = "Virtio SCSI Disk";
	bdev->write_cache = 0;
	bdev->blocklen = disk->block_size;
	bdev->blockcnt = disk->num_blocks;

	bdev->ctxt = disk;
	bdev->fn_table = &virtio_fn_table;
	bdev->module = SPDK_GET_BDEV_MODULE(virtio_scsi);

	spdk_io_device_register(&disk->hw, bdev_virtio_create_cb, bdev_virtio_destroy_cb,
				sizeof(struct bdev_virtio_io_channel));
	spdk_bdev_register(bdev);

	return 0;
}

static void
scan_target(struct virtio_hw *hw, uint8_t target)
{
	struct iovec _iov;
	struct iovec *iov;
	struct virtio_req _vreq;
	struct virtio_req *vreq;
	struct virtio_scsi_cmd_req *req;
	struct virtio_scsi_cmd_resp *resp;
	struct spdk_scsi_cdb_inquiry *cdb;
	uint16_t cnt;
	struct virtio_req *complete;

	iov = &_iov;
	vreq = &_vreq;
	vreq->iov = iov;
	vreq->iovcnt = 1;
	vreq->is_write = 0;

	req = spdk_dma_zmalloc(sizeof(*req), 64, NULL);
	resp = spdk_dma_malloc(sizeof(*resp), 64, NULL);

	vreq->iov_req.iov_base = (void *)req;
	vreq->iov_req.iov_len = sizeof(*req);

	vreq->iov_resp.iov_base = (void *)resp;
	vreq->iov_resp.iov_len = sizeof(*resp);

	iov[0].iov_base = spdk_dma_malloc(4096, 64, NULL);
	iov[0].iov_len = 255;

	req->lun[0] = 1;
	req->lun[1] = target;

	cdb = (struct spdk_scsi_cdb_inquiry *)req->cdb;
	cdb->opcode = SPDK_SPC_INQUIRY;
	cdb->alloc_len[1] = 255;

	virtio_xmit_pkts(hw->vqs[2], vreq);

	do {
		cnt = virtio_recv_pkts(hw->vqs[2], &complete, 1);
	} while (cnt == 0);

	if (process_scan_inquiry(hw, vreq) != 0) {
		return;
	}

	do {
		cnt = virtio_recv_pkts(hw->vqs[2], &complete, 1);
	} while (cnt == 0);

	process_read_cap(hw, vreq);
}

static int
bdev_virtio_initialize(void)
{
	struct spdk_conf_section *sp = spdk_conf_find_section(NULL, "Virtio");
	struct virtio_hw *hw = NULL;
	char *type, *path;
	uint32_t i;

	if (sp == NULL) {
		return 0;
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
			hw = virtio_user_dev_init(path, 1, 512);
		} else if (!strcmp("Pci", type)) {
			hw = get_pci_virtio_hw();
		} else {
			SPDK_ERRLOG("Invalid type %s specified for index %d\n", type, i);
			continue;
		}
	}

	if (hw == NULL) {
		return 0;
	}

	eth_virtio_dev_init(hw, 3);
	virtio_dev_start(hw);

	for (i = 0; i < 64; i++) {
		scan_target(hw, i);
	}

	return 0;
}

static void bdev_virtio_finish(void)
{
}

SPDK_LOG_REGISTER_TRACE_FLAG("virtio", SPDK_TRACE_VIRTIO)
