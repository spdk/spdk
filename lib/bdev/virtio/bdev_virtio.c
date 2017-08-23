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

static int
bdev_virtio_get_ctx_size(void)
{
	return 0;
}

SPDK_BDEV_MODULE_REGISTER(virtio_scsi, bdev_virtio_initialize, bdev_virtio_finish,
			  NULL, bdev_virtio_get_ctx_size, NULL)

static void
bdev_virtio_rw(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct iovec iov[128];
	struct virtio_req vreq;
	struct virtio_scsi_cmd_req *req;
	struct virtio_scsi_cmd_resp *resp;
	uint16_t cnt;
	struct virtio_req *complete;
	struct virtio_scsi_disk *disk = (struct virtio_scsi_disk *)bdev_io->bdev;
	bool is_read = (bdev_io->type == SPDK_BDEV_IO_TYPE_READ);

	vreq.iov = iov;

	req = spdk_dma_malloc(4096, 64, NULL);
	resp = spdk_dma_malloc(4096, 64, NULL);

	iov[0].iov_base = (void *)req;
	iov[0].iov_len = sizeof(*req);

	if (is_read) {
		iov[1].iov_base = (void *)resp;
		iov[1].iov_len = sizeof(struct virtio_scsi_cmd_resp);
		memcpy(&iov[2], bdev_io->u.read.iovs, sizeof(struct iovec) * bdev_io->u.read.iovcnt);
		vreq.iovcnt = 2 + bdev_io->u.read.iovcnt;
		vreq.start_write = 1;
	} else {
		memcpy(&iov[1], bdev_io->u.write.iovs, sizeof(struct iovec) * bdev_io->u.write.iovcnt);
		iov[1 + bdev_io->u.write.iovcnt].iov_base = (void *)resp;
		iov[1 + bdev_io->u.write.iovcnt].iov_len = sizeof(struct virtio_scsi_cmd_resp);
		vreq.iovcnt = 2 + bdev_io->u.write.iovcnt;
		vreq.start_write = vreq.iovcnt - 1;
	}

	memset(req, 0, sizeof(*req));
	req->lun[0] = 1;
	req->lun[1] = 0;

	if (is_read) {
		req->cdb[0] = SPDK_SBC_READ_10;
		to_be32(&req->cdb[2], bdev_io->u.read.offset / disk->block_size);
		to_be16(&req->cdb[7], bdev_io->u.read.len / disk->block_size);
	} else {
		req->cdb[0] = SPDK_SBC_WRITE_10;
		to_be32(&req->cdb[2], bdev_io->u.write.offset / disk->block_size);
		to_be16(&req->cdb[7], bdev_io->u.write.len / disk->block_size);
	}

	virtio_xmit_pkts(disk->hw->tx_queues[2], &vreq);

	do {
		cnt = virtio_recv_pkts(disk->hw->tx_queues[2], &complete, 1);
	} while (cnt == 0);

	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	spdk_dma_free(req);
	spdk_dma_free(resp);
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

static int
bdev_virtio_create_cb(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
bdev_virtio_destroy_cb(void *io_device, void *ctx_buf)
{
}

static void
scan_target(struct virtio_hw *hw, uint8_t target)
{
	struct iovec iov[3];
	struct virtio_req vreq;
	struct virtio_scsi_cmd_req *req;
	struct virtio_scsi_cmd_resp *resp;
	struct spdk_scsi_cdb_inquiry *cdb;
	uint16_t cnt;
	struct virtio_req *complete;
	struct virtio_scsi_disk *disk;
	struct spdk_bdev *bdev;

	vreq.iov = iov;
	vreq.iovcnt = 3;
	vreq.start_write = 1;

	iov[0].iov_base = spdk_dma_malloc(4096, 64, NULL);
	iov[1].iov_base = spdk_dma_malloc(4096, 64, NULL);
	iov[2].iov_base = spdk_dma_malloc(4096, 64, NULL);

	req = iov[0].iov_base;
	resp = iov[1].iov_base;

	memset(req, 0, sizeof(*req));
	req->lun[0] = 1;
	req->lun[1] = target;
	iov[0].iov_len = sizeof(*req);

	cdb = (struct spdk_scsi_cdb_inquiry *)req->cdb;
	cdb->opcode = SPDK_SPC_INQUIRY;
	cdb->alloc_len[1] = 255;

	iov[1].iov_len = sizeof(struct virtio_scsi_cmd_resp);
	iov[2].iov_len = 255;

	virtio_xmit_pkts(hw->tx_queues[2], &vreq);

	do {
		cnt = virtio_recv_pkts(hw->tx_queues[2], &complete, 1);
	} while (cnt == 0);

	if (resp->response != VIRTIO_SCSI_S_OK || resp->status != SPDK_SCSI_STATUS_GOOD) {
		return;
	}

	memset(req, 0, sizeof(*req));
	req->lun[0] = 1;
	req->lun[1] = target;
	iov[0].iov_len = sizeof(*req);

	req->cdb[0] = SPDK_SPC_SERVICE_ACTION_IN_16;
	req->cdb[1] = SPDK_SBC_SAI_READ_CAPACITY_16;

	iov[1].iov_len = sizeof(struct virtio_scsi_cmd_resp);
	iov[2].iov_len = 32;
	to_be32(&req->cdb[10], iov[2].iov_len);

	virtio_xmit_pkts(hw->tx_queues[2], &vreq);

	do {
		cnt = virtio_recv_pkts(hw->tx_queues[2], &complete, 1);
	} while (cnt == 0);

	disk = calloc(1, sizeof(*disk));
	if (disk == NULL) {
		SPDK_ERRLOG("could not allocate disk\n");
		return;
	}

	disk->num_blocks = from_be64((uint64_t *)(iov[2].iov_base)) + 1;
	disk->block_size = from_be32((uint32_t *)(iov[2].iov_base + 8));

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

	spdk_io_device_register(&disk->hw, bdev_virtio_create_cb, bdev_virtio_destroy_cb, 0);
	spdk_bdev_register(bdev);
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
