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

#include "blockdev_aio.h"

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/fd.h"
#include "spdk/io_channel.h"

#include "spdk_internal/log.h"

static int blockdev_aio_initialize(void);
static void aio_free_disk(struct file_disk *fdisk);

static int
blockdev_aio_get_ctx_size(void)
{
	return sizeof(struct blockdev_aio_task);
}

SPDK_BDEV_MODULE_REGISTER(blockdev_aio_initialize, NULL, NULL, blockdev_aio_get_ctx_size)

static int
blockdev_aio_open(struct file_disk *disk)
{
	int fd;

	fd = open(disk->file, O_RDWR | O_DIRECT);
	if (fd < 0) {
		perror("open");
		disk->fd = -1;
		return -1;
	}

	disk->fd = fd;

	return 0;
}

static int
blockdev_aio_close(struct file_disk *disk)
{
	int rc;

	if (disk->fd == -1) {
		return 0;
	}

	rc = close(disk->fd);
	if (rc < 0) {
		perror("close");
		return -1;
	}

	disk->fd = -1;

	return 0;
}

static int64_t
blockdev_aio_readv(struct file_disk *fdisk, struct spdk_io_channel *ch,
		   struct blockdev_aio_task *aio_task,
		   struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct iocb *iocb = &aio_task->iocb;
	struct blockdev_aio_io_channel *aio_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	io_prep_preadv(iocb, fdisk->fd, iov, iovcnt, offset);
	iocb->data = aio_task;
	aio_task->len = nbytes;

	SPDK_TRACELOG(SPDK_TRACE_AIO, "read %d iovs size %lu to off: %#lx\n",
		      iovcnt, nbytes, offset);

	rc = io_submit(aio_ch->io_ctx, 1, &iocb);
	if (rc < 0) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_FAILED);
		SPDK_ERRLOG("%s: io_submit returned %d\n", __func__, rc);
		return -1;
	}

	return nbytes;
}

static int64_t
blockdev_aio_writev(struct file_disk *fdisk, struct spdk_io_channel *ch,
		    struct blockdev_aio_task *aio_task,
		    struct iovec *iov, int iovcnt, size_t len, uint64_t offset)
{
	struct iocb *iocb = &aio_task->iocb;
	struct blockdev_aio_io_channel *aio_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	io_prep_pwritev(iocb, fdisk->fd, iov, iovcnt, offset);
	iocb->data = aio_task;
	aio_task->len = len;

	SPDK_TRACELOG(SPDK_TRACE_AIO, "write %d iovs size %lu from off: %#lx\n",
		      iovcnt, len, offset);

	rc = io_submit(aio_ch->io_ctx, 1, &iocb);
	if (rc < 0) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_FAILED);
		SPDK_ERRLOG("%s: io_submit returned %d\n", __func__, rc);
		return -1;
	}

	return len;
}

static void
blockdev_aio_flush(struct file_disk *fdisk, struct blockdev_aio_task *aio_task,
		   uint64_t offset, uint64_t nbytes)
{
	int rc = fsync(fdisk->fd);

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task),
			      rc == 0 ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);
}

static int
blockdev_aio_destruct(void *ctx)
{
	struct file_disk *fdisk = ctx;
	int rc = 0;

	rc = blockdev_aio_close(fdisk);
	if (rc < 0) {
		SPDK_ERRLOG("blockdev_aio_close() failed\n");
	}
	aio_free_disk(fdisk);
	return rc;
}

static int
blockdev_aio_initialize_io_channel(struct blockdev_aio_io_channel *ch)
{
	ch->queue_depth = 128;

	if (io_setup(ch->queue_depth, &ch->io_ctx) < 0) {
		SPDK_ERRLOG("async I/O context setup failure\n");
		return -1;
	}

	ch->events = calloc(sizeof(struct io_event), ch->queue_depth);
	if (!ch->events) {
		io_destroy(ch->io_ctx);
		return -1;
	}

	return 0;
}

static void
blockdev_aio_poll(void *arg)
{
	struct blockdev_aio_io_channel *ch = arg;
	int nr, i;
	enum spdk_bdev_io_status status;
	struct blockdev_aio_task *aio_task;
	struct timespec timeout;

	timeout.tv_sec = 0;
	timeout.tv_nsec = 0;

	nr = io_getevents(ch->io_ctx, 1, ch->queue_depth,
			  ch->events, &timeout);

	if (nr < 0) {
		SPDK_ERRLOG("%s: io_getevents returned %d\n", __func__, nr);
		return;
	}

	for (i = 0; i < nr; i++) {
		aio_task = ch->events[i].data;
		if (ch->events[i].res != aio_task->len) {
			status = SPDK_BDEV_IO_STATUS_FAILED;
		} else {
			status = SPDK_BDEV_IO_STATUS_SUCCESS;
		}

		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), status);
	}
}

static void
blockdev_aio_reset(struct file_disk *fdisk, struct blockdev_aio_task *aio_task)
{
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_SUCCESS);
}

static void blockdev_aio_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	blockdev_aio_readv((struct file_disk *)bdev_io->bdev->ctxt,
			   ch,
			   (struct blockdev_aio_task *)bdev_io->driver_ctx,
			   bdev_io->u.read.iovs,
			   bdev_io->u.read.iovcnt,
			   bdev_io->u.read.len,
			   bdev_io->u.read.offset);
}

static int _blockdev_aio_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, blockdev_aio_get_buf_cb);
		return 0;

	case SPDK_BDEV_IO_TYPE_WRITE:
		blockdev_aio_writev((struct file_disk *)bdev_io->bdev->ctxt,
				    ch,
				    (struct blockdev_aio_task *)bdev_io->driver_ctx,
				    bdev_io->u.write.iovs,
				    bdev_io->u.write.iovcnt,
				    bdev_io->u.write.len,
				    bdev_io->u.write.offset);
		return 0;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		blockdev_aio_flush((struct file_disk *)bdev_io->bdev->ctxt,
				   (struct blockdev_aio_task *)bdev_io->driver_ctx,
				   bdev_io->u.flush.offset,
				   bdev_io->u.flush.length);
		return 0;

	case SPDK_BDEV_IO_TYPE_RESET:
		blockdev_aio_reset((struct file_disk *)bdev_io->bdev->ctxt,
				   (struct blockdev_aio_task *)bdev_io->driver_ctx);
		return 0;
	default:
		return -1;
	}
}

static void blockdev_aio_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	if (_blockdev_aio_submit_request(ch, bdev_io) < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
blockdev_aio_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
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
blockdev_aio_create_cb(void *io_device, uint32_t priority, void *ctx_buf, void *unique_ctx)
{
	struct blockdev_aio_io_channel *ch = ctx_buf;

	if (blockdev_aio_initialize_io_channel(ch) != 0) {
		return -1;
	}

	spdk_poller_register(&ch->poller, blockdev_aio_poll, ch,
			     spdk_env_get_current_core(), 0);
	return 0;
}

static void
blockdev_aio_destroy_cb(void *io_device, void *ctx_buf)
{
	struct blockdev_aio_io_channel *io_channel = ctx_buf;

	io_destroy(io_channel->io_ctx);
	free(io_channel->events);
	spdk_poller_unregister(&io_channel->poller, NULL);
}

static struct spdk_io_channel *
blockdev_aio_get_io_channel(void *ctx, uint32_t priority)
{
	struct file_disk *fdisk = ctx;

	return spdk_get_io_channel(&fdisk->fd, priority, false, NULL);
}

static const struct spdk_bdev_fn_table aio_fn_table = {
	.destruct		= blockdev_aio_destruct,
	.submit_request		= blockdev_aio_submit_request,
	.io_type_supported	= blockdev_aio_io_type_supported,
	.get_io_channel		= blockdev_aio_get_io_channel,
};

static void aio_free_disk(struct file_disk *fdisk)
{
	if (fdisk == NULL)
		return;
	free(fdisk);
}

struct spdk_bdev *
create_aio_disk(const char *name, const char *fname)
{
	struct file_disk *fdisk;

	fdisk = calloc(sizeof(*fdisk), 1);
	if (!fdisk) {
		SPDK_ERRLOG("Unable to allocate enough memory for aio backend\n");
		return NULL;
	}

	fdisk->file = fname;
	if (blockdev_aio_open(fdisk)) {
		SPDK_ERRLOG("Unable to open file %s. fd: %d errno: %d\n", fname, fdisk->fd, errno);
		goto error_return;
	}

	fdisk->size = spdk_fd_get_size(fdisk->fd);

	TAILQ_INIT(&fdisk->sync_completion_list);
	snprintf(fdisk->disk.name, SPDK_BDEV_MAX_NAME_LENGTH, "%s", name);
	snprintf(fdisk->disk.product_name, SPDK_BDEV_MAX_PRODUCT_NAME_LENGTH, "AIO disk");

	fdisk->disk.need_aligned_buffer = 1;
	fdisk->disk.write_cache = 1;
	fdisk->disk.blocklen = spdk_fd_get_blocklen(fdisk->fd);
	fdisk->disk.blockcnt = fdisk->size / fdisk->disk.blocklen;
	fdisk->disk.ctxt = fdisk;

	fdisk->disk.fn_table = &aio_fn_table;

	spdk_io_device_register(&fdisk->fd, blockdev_aio_create_cb, blockdev_aio_destroy_cb,
				sizeof(struct blockdev_aio_io_channel));
	spdk_bdev_register(&fdisk->disk);
	return &fdisk->disk;

error_return:
	blockdev_aio_close(fdisk);
	aio_free_disk(fdisk);
	return NULL;
}

static int blockdev_aio_initialize(void)
{
	size_t i;
	struct spdk_conf_section *sp;
	struct spdk_bdev *bdev;

	sp = spdk_conf_find_section(NULL, "AIO");
	if (!sp) {
		return 0;
	}

	i = 0;
	while (true) {
		const char *file;
		const char *name;

		file = spdk_conf_section_get_nmval(sp, "AIO", i, 0);
		if (!file) {
			break;
		}

		name = spdk_conf_section_get_nmval(sp, "AIO", i, 1);
		if (!name) {
			SPDK_ERRLOG("No name provided for AIO disk with file %s\n", file);
			i++;
			continue;
		}

		bdev = create_aio_disk(name, file);
		if (!bdev) {
			SPDK_ERRLOG("Unable to create AIO bdev from file %s\n", file);
			i++;
			continue;
		}

		i++;
	}

	return 0;
}

SPDK_LOG_REGISTER_TRACE_FLAG("aio", SPDK_TRACE_AIO)
