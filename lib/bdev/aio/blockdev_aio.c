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

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include "spdk/bdev.h"
#include "spdk/conf.h"
#include "spdk/file.h"
#include "spdk/log.h"

static int g_blockdev_count = 0;

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

	io_destroy(disk->io_ctx);

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
blockdev_aio_read(struct file_disk *fdisk, struct blockdev_aio_task *aio_task,
		  void *buf, uint64_t nbytes, off_t offset)
{
	struct iocb *iocb = &aio_task->iocb;
	int rc;

	iocb->aio_fildes = fdisk->fd;
	iocb->aio_reqprio = 0;
	iocb->aio_lio_opcode = IO_CMD_PREAD;
	iocb->u.c.buf = buf;
	iocb->u.c.nbytes = nbytes;
	iocb->u.c.offset = offset;
	iocb->data = aio_task;
	aio_task->len = nbytes;

	SPDK_TRACELOG(SPDK_TRACE_AIO, "read from %p of size %lu to off: %#lx\n",
		      buf, nbytes, offset);

	rc = io_submit(fdisk->io_ctx, 1, &iocb);
	if (rc < 0) {
		SPDK_ERRLOG("%s: io_submit returned %d\n", __func__, rc);
		return -1;
	}

	return nbytes;
}

static int64_t
blockdev_aio_writev(struct file_disk *fdisk, struct blockdev_aio_task *aio_task,
		    struct iovec *iov, int iovcnt, size_t len, off_t offset)
{
	struct iocb *iocb = &aio_task->iocb;
	int rc;

	iocb->aio_fildes = fdisk->fd;
	iocb->aio_lio_opcode = IO_CMD_PWRITEV;
	iocb->aio_reqprio = 0;
	iocb->u.v.vec = iov;
	iocb->u.v.nr = iovcnt;
	iocb->u.v.offset = offset;
	iocb->data = aio_task;
	aio_task->len = len;

	SPDK_TRACELOG(SPDK_TRACE_AIO, "write %d iovs size %lu from off: %#lx\n",
		      iovcnt, len, offset);

	rc = io_submit(fdisk->io_ctx, 1, &iocb);
	if (rc < 0) {
		SPDK_ERRLOG("%s: io_submit returned %d\n", __func__, rc);
		return -1;
	}

	return len;
}

static int64_t
blockdev_aio_flush(struct file_disk *fdisk, struct blockdev_aio_task *aio_task,
		   uint64_t offset, uint64_t nbytes)
{
	int rc = fsync(fdisk->fd);

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task),
			      rc == 0 ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);

	return rc;
}

static int
blockdev_aio_destruct(struct spdk_bdev *bdev)
{
	struct file_disk *fdisk = (struct file_disk *)bdev;
	int rc = 0;

	rc = blockdev_aio_close(fdisk);
	if (rc < 0) {
		SPDK_ERRLOG("blockdev_aio_close() failed\n");
	}
	aio_free_disk(fdisk);
	return rc;
}

static int
blockdev_aio_check_io(struct spdk_bdev *bdev)
{
	int nr, i;
	enum spdk_bdev_io_status status;
	struct blockdev_aio_task *aio_task;
	struct file_disk *fdisk = (struct file_disk *)bdev;
	struct timespec timeout;

	timeout.tv_sec = 0;
	timeout.tv_nsec = 0;

	nr = io_getevents(fdisk->io_ctx, 1, fdisk->queue_depth,
			  fdisk->events, &timeout);

	if (nr < 0) {
		SPDK_ERRLOG("%s: io_getevents returned %d\n", __func__, nr);
		return -1;
	}

	for (i = 0; i < nr; i++) {
		aio_task = fdisk->events[i].data;
		if (fdisk->events[i].res != aio_task->len) {
			status = SPDK_BDEV_IO_STATUS_FAILED;
		} else {
			status = SPDK_BDEV_IO_STATUS_SUCCESS;
		}

		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), status);
	}

	return 0;
}

static int
blockdev_aio_reset(struct file_disk *fdisk, struct blockdev_aio_task *aio_task)
{
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_SUCCESS);

	return 0;
}

static void blockdev_aio_get_rbuf_cb(struct spdk_bdev_io *bdev_io)
{
	int ret = 0;

	ret = blockdev_aio_read((struct file_disk *)bdev_io->ctx,
				(struct blockdev_aio_task *)bdev_io->driver_ctx,
				bdev_io->u.read.buf,
				bdev_io->u.read.nbytes,
				bdev_io->u.read.offset);

	if (ret < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static int _blockdev_aio_submit_request(struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_rbuf(bdev_io, blockdev_aio_get_rbuf_cb);
		return 0;

	case SPDK_BDEV_IO_TYPE_WRITE:
		return blockdev_aio_writev((struct file_disk *)bdev_io->ctx,
					   (struct blockdev_aio_task *)bdev_io->driver_ctx,
					   bdev_io->u.write.iovs,
					   bdev_io->u.write.iovcnt,
					   bdev_io->u.write.len,
					   bdev_io->u.write.offset);
	case SPDK_BDEV_IO_TYPE_FLUSH:
		return blockdev_aio_flush((struct file_disk *)bdev_io->ctx,
					  (struct blockdev_aio_task *)bdev_io->driver_ctx,
					  bdev_io->u.flush.offset,
					  bdev_io->u.flush.length);

	case SPDK_BDEV_IO_TYPE_RESET:
		return blockdev_aio_reset((struct file_disk *)bdev_io->ctx,
					  (struct blockdev_aio_task *)bdev_io->driver_ctx);
	default:
		return -1;
	}
	return 0;
}

static void blockdev_aio_submit_request(struct spdk_bdev_io *bdev_io)
{
	if (_blockdev_aio_submit_request(bdev_io) < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void blockdev_aio_free_request(struct spdk_bdev_io *bdev_io)
{
}

static bool
blockdev_aio_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type)
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

static struct spdk_bdev_fn_table aio_fn_table = {
	.destruct		= blockdev_aio_destruct,
	.check_io		= blockdev_aio_check_io,
	.submit_request		= blockdev_aio_submit_request,
	.free_request		= blockdev_aio_free_request,
	.io_type_supported	= blockdev_aio_io_type_supported,
};

static void aio_free_disk(struct file_disk *fdisk)
{
	if (fdisk == NULL)
		return;
	if (fdisk->events != NULL)
		free(fdisk->events);
	free(fdisk);
}

struct file_disk *
create_aio_disk(char *fname)
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

	fdisk->size = spdk_file_get_size(fdisk->fd);
	fdisk->queue_depth = 128; // TODO: where do we get the queue depth from.

	TAILQ_INIT(&fdisk->sync_completion_list);
	snprintf(fdisk->disk.name, SPDK_BDEV_MAX_NAME_LENGTH, "AIO%d",
		 g_blockdev_count);
	snprintf(fdisk->disk.product_name, SPDK_BDEV_MAX_PRODUCT_NAME_LENGTH, "iSCSI AIO disk");

	fdisk->disk.need_aligned_buffer = 1;
	fdisk->disk.write_cache = 1;
	fdisk->disk.blocklen = spdk_dev_get_blocklen(fdisk->fd);
	fdisk->disk.blockcnt = fdisk->size / fdisk->disk.blocklen;
	fdisk->disk.ctxt = fdisk;

	fdisk->disk.fn_table = &aio_fn_table;
	if (io_setup(fdisk->queue_depth, &fdisk->io_ctx) < 0) {
		SPDK_ERRLOG("async I/O context setup failure\n");
		goto error_return;
	}

	fdisk->events = calloc(sizeof(struct io_event), fdisk->queue_depth);
	if (!fdisk->events) {
		SPDK_ERRLOG("unable to allocate async events\n");
		goto error_return;
	}

	g_blockdev_count++;

	spdk_bdev_register(&fdisk->disk);
	return fdisk;

error_return:
	blockdev_aio_close(fdisk);
	aio_free_disk(fdisk);
	return NULL;
}

static int blockdev_aio_initialize(void)
{
	struct file_disk *fdisk;
	int i;
	const char *val = NULL;
	char *file;
	struct spdk_conf_section *sp = spdk_conf_find_section(NULL, "AIO");
	bool skip_missing = false;

	if (sp != NULL) {
		val = spdk_conf_section_get_val(sp, "SkipMissingFiles");
	}
	if (val != NULL && !strcmp(val, "Yes")) {
		skip_missing = true;
	}

	if (sp != NULL) {
		for (i = 0; ; i++) {
			val = spdk_conf_section_get_nval(sp, "AIO", i);
			if (val == NULL)
				break;
			file = spdk_conf_section_get_nmval(sp, "AIO", i, 0);
			if (file == NULL) {
				SPDK_ERRLOG("AIO%d: format error\n", i);
				return -1;
			}

			fdisk = create_aio_disk(file);

			if (fdisk == NULL && !skip_missing) {
				return -1;
			}
		}
	}
	return 0;
}

SPDK_LOG_REGISTER_TRACE_FLAG("aio", SPDK_TRACE_AIO)
