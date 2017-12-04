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
#include "spdk/fd.h"
#include "spdk/io_channel.h"
#include "spdk/json.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"
#include "spdk_internal/bdev.h"

#include "iscsi/iscsi.h"
#include "iscsi/scsi-lowlevel.h"

struct iscsi_lun;

const char *g_initiator = "iqn.2017-11.spdk:iscsi_init";
struct iscsi_context *g_context;
struct iscsi_url *g_url;

static int bdev_iscsi_initialize(void);
static TAILQ_HEAD(, iscsi_lun) g_iscsi_lun_head;

struct bdev_iscsi_task {
	bool	tmp;
};

struct iscsi_target_node {
};

struct iscsi_lun {
	struct spdk_bdev	disk;
	TAILQ_ENTRY(iscsi_lun)	link;

};

struct bdev_iscsi_io_channel {
	struct spdk_poller	*poller;
};

static int
bdev_iscsi_get_ctx_size(void)
{
	return sizeof(struct bdev_iscsi_task);
}

static void bdev_iscsi_finish(void)
{
	iscsi_logout_sync(g_context);
	iscsi_disconnect(g_context);
	iscsi_destroy_url(g_url);
}

SPDK_BDEV_MODULE_REGISTER(iscsi, bdev_iscsi_initialize, bdev_iscsi_finish, NULL, bdev_iscsi_get_ctx_size, NULL)

static int64_t
bdev_iscsi_readv(struct iscsi_lun *fdisk, struct spdk_io_channel *ch,
	       struct bdev_iscsi_task *iscsi_task,
	       struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t lba)
{
	struct scsi_task *task;

	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI_INIT, "read %d iovs size %lu to lba: %#lx\n",
		      iovcnt, nbytes, lba);

	task = iscsi_read16_iov_sync(g_context, 0, lba, nbytes, 512, 0, 0, 0, 0, 0, (struct scsi_iovec *)iov, iovcnt);

	scsi_free_scsi_task(task);
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(iscsi_task), SPDK_BDEV_IO_STATUS_SUCCESS);

	return nbytes;
}

static int64_t
bdev_iscsi_writev(struct iscsi_lun *fdisk, struct spdk_io_channel *ch,
		struct bdev_iscsi_task *iscsi_task,
		struct iovec *iov, int iovcnt, size_t len, uint64_t lba)
{
	struct scsi_task *task;

	SPDK_DEBUGLOG(SPDK_TRACE_ISCSI_INIT, "write %d iovs size %lu to lba: %#lx\n",
		      iovcnt, len, lba);

	task = iscsi_write16_iov_sync(g_context, 0, lba, NULL, len, 512, 0, 0, 0, 0, 0, (struct scsi_iovec *)iov, iovcnt);

	scsi_free_scsi_task(task);
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(iscsi_task), SPDK_BDEV_IO_STATUS_SUCCESS);

	return len;
}

static void
bdev_iscsi_flush(struct iscsi_lun *fdisk, struct bdev_iscsi_task *iscsi_task,
	       uint64_t offset, uint64_t nbytes)
{
	int rc = 0;

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(iscsi_task),
			      rc == 0 ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);
}

static int
bdev_iscsi_destruct(void *ctx)
{
	struct iscsi_lun *lun = ctx;
	int rc = 0;

	TAILQ_REMOVE(&g_iscsi_lun_head, lun, link);
	return rc;
}

static int
bdev_iscsi_initialize_io_channel(struct bdev_iscsi_io_channel *ch)
{
	return 0;
}

static void
bdev_iscsi_poll(void *arg)
{
}

static void
bdev_iscsi_reset(struct iscsi_lun *fdisk, struct bdev_iscsi_task *iscsi_task)
{
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(iscsi_task), SPDK_BDEV_IO_STATUS_SUCCESS);
}

static void bdev_iscsi_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	bdev_iscsi_readv((struct iscsi_lun *)bdev_io->bdev->ctxt,
		       ch,
		       (struct bdev_iscsi_task *)bdev_io->driver_ctx,
		       bdev_io->u.bdev.iovs,
		       bdev_io->u.bdev.iovcnt,
		       bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
		       bdev_io->u.bdev.offset_blocks);
}

static int _bdev_iscsi_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_iscsi_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return 0;

	case SPDK_BDEV_IO_TYPE_WRITE:
		bdev_iscsi_writev((struct iscsi_lun *)bdev_io->bdev->ctxt,
				ch,
				(struct bdev_iscsi_task *)bdev_io->driver_ctx,
				bdev_io->u.bdev.iovs,
				bdev_io->u.bdev.iovcnt,
				bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
				bdev_io->u.bdev.offset_blocks);
		return 0;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		bdev_iscsi_flush((struct iscsi_lun *)bdev_io->bdev->ctxt,
			       (struct bdev_iscsi_task *)bdev_io->driver_ctx,
			       bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen,
			       bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return 0;

	case SPDK_BDEV_IO_TYPE_RESET:
		bdev_iscsi_reset((struct iscsi_lun *)bdev_io->bdev->ctxt,
			       (struct bdev_iscsi_task *)bdev_io->driver_ctx);
		return 0;
	default:
		return -1;
	}
}

static void bdev_iscsi_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	if (_bdev_iscsi_submit_request(ch, bdev_io) < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
bdev_iscsi_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
		return true;

	default:
		return false;
	}
}

static int
bdev_iscsi_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_iscsi_io_channel *ch = ctx_buf;

	if (bdev_iscsi_initialize_io_channel(ch) != 0) {
		return -1;
	}

	ch->poller = spdk_poller_register(bdev_iscsi_poll, ch, 0);
	return 0;
}

static void
bdev_iscsi_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_iscsi_io_channel *io_channel = ctx_buf;

	spdk_poller_unregister(&io_channel->poller);
}

static struct spdk_io_channel *
bdev_iscsi_get_io_channel(void *ctx)
{
	struct iscsi_lun *fdisk = ctx;

	return spdk_get_io_channel(&fdisk->link);
}


static int
bdev_iscsi_dump_config_json(void *ctx, struct spdk_json_write_ctx *w)
{
	spdk_json_write_name(w, "iscsi");
	spdk_json_write_object_begin(w);
	spdk_json_write_object_end(w);

	return 0;
}

static const struct spdk_bdev_fn_table iscsi_fn_table = {
	.destruct		= bdev_iscsi_destruct,
	.submit_request		= bdev_iscsi_submit_request,
	.io_type_supported	= bdev_iscsi_io_type_supported,
	.get_io_channel		= bdev_iscsi_get_io_channel,
	.dump_config_json	= bdev_iscsi_dump_config_json,
};

static void iscsi_free_disk(struct iscsi_lun *fdisk)
{
	if (fdisk == NULL)
		return;
	free(fdisk->disk.name);
	free(fdisk);
}

static struct spdk_bdev *
create_iscsi_disk(const char *name, uint64_t num_blocks, uint32_t block_size)
{
	struct iscsi_lun *fdisk;
	int rc;

	fdisk = calloc(sizeof(*fdisk), 1);
	if (!fdisk) {
		SPDK_ERRLOG("Unable to allocate enough memory for iscsi backend\n");
		return NULL;
	}

	fdisk->disk.name = strdup(name);
	if (!fdisk->disk.name) {
		goto error_return;
	}
	fdisk->disk.product_name = "iSCSI LUN";
	fdisk->disk.module = SPDK_GET_BDEV_MODULE(iscsi);
	fdisk->disk.blocklen = block_size;
	fdisk->disk.blockcnt = num_blocks;
	fdisk->disk.ctxt = fdisk;

	fdisk->disk.fn_table = &iscsi_fn_table;

	spdk_io_device_register(&fdisk->link, bdev_iscsi_create_cb, bdev_iscsi_destroy_cb,
				sizeof(struct bdev_iscsi_io_channel));
	rc = spdk_bdev_register(&fdisk->disk);
	if (rc) {
		spdk_io_device_unregister(&fdisk->link, NULL);
		goto error_return;
	}

	TAILQ_INSERT_TAIL(&g_iscsi_lun_head, fdisk, link);
	return &fdisk->disk;

error_return:
	iscsi_free_disk(fdisk);
	return NULL;
}

static int
bdev_iscsi_initialize(void)
{
	int rc;
	struct spdk_bdev *bdev;
	struct scsi_task *task;
	struct scsi_readcapacity16 *readcap16;

	g_context = iscsi_create_context(g_initiator);
	if (g_context == NULL) {
		SPDK_ERRLOG("could not create iscsi context\n");
		return -1;
	}

	g_url = iscsi_parse_full_url(g_context, "iscsi://127.0.0.1/iqn.2016-06.io.spdk:disk1/0");
	if (g_url == NULL) {
		SPDK_ERRLOG("could not parse URL\n");
		return -1;
	}

	iscsi_set_session_type(g_context, ISCSI_SESSION_NORMAL);
	iscsi_set_header_digest(g_context, ISCSI_HEADER_DIGEST_NONE);
	rc = iscsi_full_connect_sync(g_context, g_url->portal, g_url->lun);
	if (rc != 0) {
		SPDK_ERRLOG("could not login\n");
		return -1;
	}

	TAILQ_INIT(&g_iscsi_lun_head);

	task = iscsi_readcapacity16_sync(g_context, 0);
	readcap16 = scsi_datain_unmarshall(task);
	
	bdev = create_iscsi_disk("iSCSI0", readcap16->returned_lba + 1, readcap16->block_length);
	if (!bdev) {
		SPDK_ERRLOG("Unable to create iscsi bdev\n");
	}

	scsi_free_scsi_task(task);

	return 0;
}

SPDK_LOG_REGISTER_TRACE_FLAG("iscsi_init", SPDK_TRACE_ISCSI_INIT)
