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

#include "bdev_aio.h"

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/fd.h"
#include "spdk/io_channel.h"
#include "spdk/json.h"
#include "spdk/util.h"
#include "spdk/string.h"

#include "spdk_internal/log.h"

static int bdev_aio_initialize(void);
static void aio_free_disk(struct file_disk *fdisk);
static void bdev_aio_get_spdk_running_config(FILE *fp);
static TAILQ_HEAD(, file_disk) g_aio_disk_head;

#define SPDK_AIO_QUEUE_DEPTH 128

static int
bdev_aio_get_ctx_size(void)
{
	return sizeof(struct bdev_aio_task);
}

SPDK_BDEV_MODULE_REGISTER(aio, bdev_aio_initialize, NULL, bdev_aio_get_spdk_running_config,
			  bdev_aio_get_ctx_size, NULL)

static int
bdev_aio_open(struct file_disk *disk)
{
	int fd;

	fd = open(disk->filename, O_RDWR | O_DIRECT);
	if (fd < 0) {
		/* Try without O_DIRECT for non-disk files */
		fd = open(disk->filename, O_RDWR);
		if (fd < 0) {
			SPDK_ERRLOG("open() failed (file:%s), errno %d: %s\n",
				    disk->filename, errno, spdk_strerror(errno));
			disk->fd = -1;
			return -1;
		}
	}

	disk->fd = fd;

	return 0;
}

static int
bdev_aio_close(struct file_disk *disk)
{
	int rc;

	if (disk->fd == -1) {
		return 0;
	}

	rc = close(disk->fd);
	if (rc < 0) {
		SPDK_ERRLOG("close() failed (fd=%d), errno %d: %s\n",
			    disk->fd, errno, spdk_strerror(errno));
		return -1;
	}

	disk->fd = -1;

	return 0;
}

static int64_t
bdev_aio_readv(struct file_disk *fdisk, struct spdk_io_channel *ch,
	       struct bdev_aio_task *aio_task,
	       struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct iocb *iocb = &aio_task->iocb;
	struct bdev_aio_io_channel *aio_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	io_prep_preadv(iocb, fdisk->fd, iov, iovcnt, offset);
	iocb->data = aio_task;
	aio_task->len = nbytes;

	SPDK_DEBUGLOG(SPDK_LOG_AIO, "read %d iovs size %lu to off: %#lx\n",
		      iovcnt, nbytes, offset);

	rc = io_submit(aio_ch->io_ctx, 1, &iocb);
	if (rc < 0) {
		if (rc == EAGAIN) {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_NOMEM);
		} else {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_FAILED);
		}
		SPDK_ERRLOG("%s: io_submit returned %d\n", __func__, rc);
		return -1;
	}
	aio_ch->io_inflight++;
	return nbytes;
}

static int64_t
bdev_aio_writev(struct file_disk *fdisk, struct spdk_io_channel *ch,
		struct bdev_aio_task *aio_task,
		struct iovec *iov, int iovcnt, size_t len, uint64_t offset)
{
	struct iocb *iocb = &aio_task->iocb;
	struct bdev_aio_io_channel *aio_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	io_prep_pwritev(iocb, fdisk->fd, iov, iovcnt, offset);
	iocb->data = aio_task;
	aio_task->len = len;

	SPDK_DEBUGLOG(SPDK_LOG_AIO, "write %d iovs size %lu from off: %#lx\n",
		      iovcnt, len, offset);

	rc = io_submit(aio_ch->io_ctx, 1, &iocb);
	if (rc < 0) {
		if (rc == EAGAIN) {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_NOMEM);
		} else {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_FAILED);
		}
		SPDK_ERRLOG("%s: io_submit returned %d\n", __func__, rc);
		return -1;
	}
	aio_ch->io_inflight++;
	return len;
}

static void
bdev_aio_flush(struct file_disk *fdisk, struct bdev_aio_task *aio_task,
	       uint64_t offset, uint64_t nbytes)
{
	int rc = fsync(fdisk->fd);

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task),
			      rc == 0 ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED);
}

static int
bdev_aio_destruct(void *ctx)
{
	struct file_disk *fdisk = ctx;
	int rc = 0;

	TAILQ_REMOVE(&g_aio_disk_head, fdisk, link);
	rc = bdev_aio_close(fdisk);
	if (rc < 0) {
		SPDK_ERRLOG("bdev_aio_close() failed\n");
	}
	aio_free_disk(fdisk);
	return rc;
}

static int
bdev_aio_initialize_io_channel(struct bdev_aio_io_channel *ch)
{
	if (io_setup(SPDK_AIO_QUEUE_DEPTH, &ch->io_ctx) < 0) {
		SPDK_ERRLOG("async I/O context setup failure\n");
		return -1;
	}

	return 0;
}

static void
bdev_aio_poll(void *arg)
{
	struct bdev_aio_io_channel *ch = arg;
	int nr, i;
	enum spdk_bdev_io_status status;
	struct bdev_aio_task *aio_task;
	struct timespec timeout;
	struct io_event events[SPDK_AIO_QUEUE_DEPTH];

	timeout.tv_sec = 0;
	timeout.tv_nsec = 0;

	nr = io_getevents(ch->io_ctx, 1, SPDK_AIO_QUEUE_DEPTH,
			  events, &timeout);

	if (nr < 0) {
		SPDK_ERRLOG("%s: io_getevents returned %d\n", __func__, nr);
		return;
	}

	for (i = 0; i < nr; i++) {
		aio_task = events[i].data;
		if (events[i].res != aio_task->len) {
			status = SPDK_BDEV_IO_STATUS_FAILED;
		} else {
			status = SPDK_BDEV_IO_STATUS_SUCCESS;
		}

		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), status);
		ch->io_inflight--;
	}
}

static void
_bdev_aio_get_io_inflight(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct bdev_aio_io_channel *aio_ch = spdk_io_channel_get_ctx(ch);

	if (aio_ch->io_inflight) {
		spdk_for_each_channel_continue(i, -1);
		return;
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
bdev_aio_reset_retry_timer(void *arg);

static void
_bdev_aio_get_io_inflight_done(struct spdk_io_channel_iter *i, int status)
{
	struct file_disk *fdisk = spdk_io_channel_iter_get_ctx(i);

	if (status == -1) {
		fdisk->reset_retry_timer = spdk_poller_register(bdev_aio_reset_retry_timer, fdisk, 500);
		return;
	}

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(fdisk->reset_task), SPDK_BDEV_IO_STATUS_SUCCESS);
}

static void
bdev_aio_reset_retry_timer(void *arg)
{
	struct file_disk *fdisk = arg;

	if (fdisk->reset_retry_timer) {
		spdk_poller_unregister(&fdisk->reset_retry_timer);
	}

	spdk_for_each_channel(fdisk,
			      _bdev_aio_get_io_inflight,
			      fdisk,
			      _bdev_aio_get_io_inflight_done);
}

static void
bdev_aio_reset(struct file_disk *fdisk, struct bdev_aio_task *aio_task)
{
	fdisk->reset_task = aio_task;

	bdev_aio_reset_retry_timer(fdisk);
}

static void bdev_aio_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	bdev_aio_readv((struct file_disk *)bdev_io->bdev->ctxt,
		       ch,
		       (struct bdev_aio_task *)bdev_io->driver_ctx,
		       bdev_io->u.bdev.iovs,
		       bdev_io->u.bdev.iovcnt,
		       bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
		       bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen);
}

static int _bdev_aio_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_aio_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return 0;

	case SPDK_BDEV_IO_TYPE_WRITE:
		bdev_aio_writev((struct file_disk *)bdev_io->bdev->ctxt,
				ch,
				(struct bdev_aio_task *)bdev_io->driver_ctx,
				bdev_io->u.bdev.iovs,
				bdev_io->u.bdev.iovcnt,
				bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
				bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen);
		return 0;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		bdev_aio_flush((struct file_disk *)bdev_io->bdev->ctxt,
			       (struct bdev_aio_task *)bdev_io->driver_ctx,
			       bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen,
			       bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return 0;

	case SPDK_BDEV_IO_TYPE_RESET:
		bdev_aio_reset((struct file_disk *)bdev_io->bdev->ctxt,
			       (struct bdev_aio_task *)bdev_io->driver_ctx);
		return 0;
	default:
		return -1;
	}
}

static void bdev_aio_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	if (_bdev_aio_submit_request(ch, bdev_io) < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
bdev_aio_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
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
bdev_aio_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_aio_io_channel *ch = ctx_buf;

	if (bdev_aio_initialize_io_channel(ch) != 0) {
		return -1;
	}

	ch->poller = spdk_poller_register(bdev_aio_poll, ch, 0);
	return 0;
}

static void
bdev_aio_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_aio_io_channel *io_channel = ctx_buf;

	io_destroy(io_channel->io_ctx);
	spdk_poller_unregister(&io_channel->poller);
}

static struct spdk_io_channel *
bdev_aio_get_io_channel(void *ctx)
{
	struct file_disk *fdisk = ctx;

	return spdk_get_io_channel(fdisk);
}


static int
bdev_aio_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct file_disk *fdisk = ctx;

	spdk_json_write_name(w, "aio");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "filename");
	spdk_json_write_string(w, fdisk->filename);

	spdk_json_write_object_end(w);

	return 0;
}

static const struct spdk_bdev_fn_table aio_fn_table = {
	.destruct		= bdev_aio_destruct,
	.submit_request		= bdev_aio_submit_request,
	.io_type_supported	= bdev_aio_io_type_supported,
	.get_io_channel		= bdev_aio_get_io_channel,
	.dump_info_json		= bdev_aio_dump_info_json,
};

static void aio_free_disk(struct file_disk *fdisk)
{
	if (fdisk == NULL) {
		return;
	}
	free(fdisk->filename);
	free(fdisk->disk.name);
	free(fdisk);
}

struct spdk_bdev *
create_aio_disk(const char *name, const char *filename, uint32_t block_size)
{
	struct file_disk *fdisk;
	uint32_t detected_block_size;
	uint64_t disk_size;
	int rc;

	fdisk = calloc(1, sizeof(*fdisk));
	if (!fdisk) {
		SPDK_ERRLOG("Unable to allocate enough memory for aio backend\n");
		return NULL;
	}

	fdisk->filename = strdup(filename);
	if (!fdisk->filename) {
		goto error_return;
	}

	if (bdev_aio_open(fdisk)) {
		SPDK_ERRLOG("Unable to open file %s. fd: %d errno: %d\n", filename, fdisk->fd, errno);
		goto error_return;
	}

	disk_size = spdk_fd_get_size(fdisk->fd);

	fdisk->disk.name = strdup(name);
	if (!fdisk->disk.name) {
		goto error_return;
	}
	fdisk->disk.product_name = "AIO disk";
	fdisk->disk.module = SPDK_GET_BDEV_MODULE(aio);

	fdisk->disk.need_aligned_buffer = 1;
	fdisk->disk.write_cache = 1;

	detected_block_size = spdk_fd_get_blocklen(fdisk->fd);
	if (block_size == 0) {
		/* User did not specify block size - use autodetected block size. */
		if (detected_block_size == 0) {
			SPDK_ERRLOG("Block size could not be auto-detected\n");
			goto error_return;
		}
		fdisk->block_size_override = false;
		block_size = detected_block_size;
	} else {
		if (block_size < detected_block_size) {
			SPDK_ERRLOG("Specified block size %" PRIu32 " is smaller than "
				    "auto-detected block size %" PRIu32 "\n",
				    block_size, detected_block_size);
			goto error_return;
		} else if (detected_block_size != 0 && block_size != detected_block_size) {
			SPDK_WARNLOG("Specified block size %" PRIu32 " does not match "
				     "auto-detected block size %" PRIu32 "\n",
				     block_size, detected_block_size);
		}
		fdisk->block_size_override = true;
	}

	if (block_size < 512) {
		SPDK_ERRLOG("Invalid block size %" PRIu32 " (must be at least 512).\n", block_size);
		goto error_return;
	}

	if (!spdk_u32_is_pow2(block_size)) {
		SPDK_ERRLOG("Invalid block size %" PRIu32 " (must be a power of 2.)\n", block_size);
		goto error_return;
	}

	fdisk->disk.blocklen = block_size;

	if (disk_size % fdisk->disk.blocklen != 0) {
		SPDK_ERRLOG("Disk size %" PRIu64 " is not a multiple of block size %" PRIu32 "\n",
			    disk_size, fdisk->disk.blocklen);
		goto error_return;
	}

	fdisk->disk.blockcnt = disk_size / fdisk->disk.blocklen;
	fdisk->disk.ctxt = fdisk;

	fdisk->disk.fn_table = &aio_fn_table;

	spdk_io_device_register(fdisk, bdev_aio_create_cb, bdev_aio_destroy_cb,
				sizeof(struct bdev_aio_io_channel));
	rc = spdk_bdev_register(&fdisk->disk);
	if (rc) {
		spdk_io_device_unregister(fdisk, NULL);
		goto error_return;
	}

	TAILQ_INSERT_TAIL(&g_aio_disk_head, fdisk, link);
	return &fdisk->disk;

error_return:
	bdev_aio_close(fdisk);
	aio_free_disk(fdisk);
	return NULL;
}

static int
bdev_aio_initialize(void)
{
	size_t i;
	struct spdk_conf_section *sp;
	struct spdk_bdev *bdev;

	TAILQ_INIT(&g_aio_disk_head);
	sp = spdk_conf_find_section(NULL, "AIO");
	if (!sp) {
		return 0;
	}

	i = 0;
	while (true) {
		const char *file;
		const char *name;
		const char *block_size_str;
		uint32_t block_size = 0;

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

		block_size_str = spdk_conf_section_get_nmval(sp, "AIO", i, 2);
		if (block_size_str) {
			block_size = atoi(block_size_str);
		}

		bdev = create_aio_disk(name, file, block_size);
		if (!bdev) {
			SPDK_ERRLOG("Unable to create AIO bdev from file %s\n", file);
			i++;
			continue;
		}

		i++;
	}

	return 0;
}

static void
bdev_aio_get_spdk_running_config(FILE *fp)
{
	char 	*file;
	char 	*name;
	uint32_t block_size;
	struct 	 file_disk *fdisk;

	fprintf(fp,
		"\n"
		"# Users must change this section to match the /dev/sdX devices to be\n"
		"# exported as iSCSI LUNs. The devices are accessed using Linux AIO.\n"
		"# The format is:\n"
		"# AIO <file name> <bdev name> [<block size>]\n"
		"# The file name is the backing device\n"
		"# The bdev name can be referenced from elsewhere in the configuration file.\n"
		"# Block size may be omitted to automatically detect the block size of a disk.\n"
		"[AIO]\n");

	TAILQ_FOREACH(fdisk, &g_aio_disk_head, link) {
		file = fdisk->filename;
		name = fdisk->disk.name;
		block_size = fdisk->disk.blocklen;
		fprintf(fp, "  AIO %s %s ", file, name);
		if (fdisk->block_size_override) {
			fprintf(fp, "%d", block_size);
		}
		fprintf(fp, "\n");
	}
	fprintf(fp, "\n");
}

SPDK_LOG_REGISTER_COMPONENT("aio", SPDK_LOG_AIO)
