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

#include "spdk/barrier.h"
#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/env.h"
#include "spdk/fd.h"
#include "spdk/likely.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/util.h"
#include "spdk/string.h"

#include "spdk/log.h"

#include <sys/eventfd.h>
#include <libaio.h>

struct bdev_aio_io_channel {
	uint64_t				io_inflight;
	io_context_t				io_ctx;
	struct bdev_aio_group_channel		*group_ch;
	TAILQ_ENTRY(bdev_aio_io_channel)	link;
};

struct bdev_aio_group_channel {
	/* eventfd for io completion notification in interrupt mode.
	 * Negative value like '-1' indicates it is invalid or unused.
	 */
	int					efd;
	struct spdk_interrupt			*intr;
	struct spdk_poller			*poller;
	TAILQ_HEAD(, bdev_aio_io_channel)	io_ch_head;
};

struct bdev_aio_task {
	struct iocb			iocb;
	uint64_t			len;
	struct bdev_aio_io_channel	*ch;
};

struct file_disk {
	struct bdev_aio_task	*reset_task;
	struct spdk_poller	*reset_retry_timer;
	struct spdk_bdev	disk;
	char			*filename;
	int			fd;
	TAILQ_ENTRY(file_disk)  link;
	bool			block_size_override;
};

/* For user space reaping of completions */
struct spdk_aio_ring {
	uint32_t id;
	uint32_t size;
	uint32_t head;
	uint32_t tail;

	uint32_t version;
	uint32_t compat_features;
	uint32_t incompat_features;
	uint32_t header_length;
};

#define SPDK_AIO_RING_VERSION	0xa10a10a1

static int bdev_aio_initialize(void);
static void bdev_aio_fini(void);
static void aio_free_disk(struct file_disk *fdisk);
static TAILQ_HEAD(, file_disk) g_aio_disk_head = TAILQ_HEAD_INITIALIZER(g_aio_disk_head);

#define SPDK_AIO_QUEUE_DEPTH 128
#define MAX_EVENTS_PER_POLL 32

static int
bdev_aio_get_ctx_size(void)
{
	return sizeof(struct bdev_aio_task);
}

static struct spdk_bdev_module aio_if = {
	.name		= "aio",
	.module_init	= bdev_aio_initialize,
	.module_fini	= bdev_aio_fini,
	.get_ctx_size	= bdev_aio_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(aio, &aio_if)

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

static void
bdev_aio_readv(struct file_disk *fdisk, struct spdk_io_channel *ch,
	       struct bdev_aio_task *aio_task,
	       struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct iocb *iocb = &aio_task->iocb;
	struct bdev_aio_io_channel *aio_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	io_prep_preadv(iocb, fdisk->fd, iov, iovcnt, offset);
	if (aio_ch->group_ch->efd >= 0) {
		io_set_eventfd(iocb, aio_ch->group_ch->efd);
	}
	iocb->data = aio_task;
	aio_task->len = nbytes;
	aio_task->ch = aio_ch;

	SPDK_DEBUGLOG(aio, "read %d iovs size %lu to off: %#lx\n",
		      iovcnt, nbytes, offset);

	rc = io_submit(aio_ch->io_ctx, 1, &iocb);
	if (spdk_unlikely(rc < 0)) {
		if (rc == -EAGAIN) {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_NOMEM);
		} else {
			spdk_bdev_io_complete_aio_status(spdk_bdev_io_from_ctx(aio_task), rc);
			SPDK_ERRLOG("%s: io_submit returned %d\n", __func__, rc);
		}
	} else {
		aio_ch->io_inflight++;
	}
}

static void
bdev_aio_writev(struct file_disk *fdisk, struct spdk_io_channel *ch,
		struct bdev_aio_task *aio_task,
		struct iovec *iov, int iovcnt, size_t len, uint64_t offset)
{
	struct iocb *iocb = &aio_task->iocb;
	struct bdev_aio_io_channel *aio_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	io_prep_pwritev(iocb, fdisk->fd, iov, iovcnt, offset);
	if (aio_ch->group_ch->efd >= 0) {
		io_set_eventfd(iocb, aio_ch->group_ch->efd);
	}
	iocb->data = aio_task;
	aio_task->len = len;
	aio_task->ch = aio_ch;

	SPDK_DEBUGLOG(aio, "write %d iovs size %lu from off: %#lx\n",
		      iovcnt, len, offset);

	rc = io_submit(aio_ch->io_ctx, 1, &iocb);
	if (spdk_unlikely(rc < 0)) {
		if (rc == -EAGAIN) {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_NOMEM);
		} else {
			spdk_bdev_io_complete_aio_status(spdk_bdev_io_from_ctx(aio_task), rc);
			SPDK_ERRLOG("%s: io_submit returned %d\n", __func__, rc);
		}
	} else {
		aio_ch->io_inflight++;
	}
}

static void
bdev_aio_flush(struct file_disk *fdisk, struct bdev_aio_task *aio_task)
{
	int rc = fsync(fdisk->fd);

	if (rc == 0) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		spdk_bdev_io_complete_aio_status(spdk_bdev_io_from_ctx(aio_task), -errno);
	}
}

static void
bdev_aio_destruct_cb(void *io_device)
{
	struct file_disk *fdisk = io_device;
	int rc = 0;

	TAILQ_REMOVE(&g_aio_disk_head, fdisk, link);
	rc = bdev_aio_close(fdisk);
	if (rc < 0) {
		SPDK_ERRLOG("bdev_aio_close() failed\n");
	}

	aio_free_disk(fdisk);
}

static int
bdev_aio_destruct(void *ctx)
{
	struct file_disk *fdisk = ctx;

	spdk_io_device_unregister(fdisk, bdev_aio_destruct_cb);

	return 0;
}

static int
bdev_user_io_getevents(io_context_t io_ctx, unsigned int max, struct io_event *uevents)
{
	uint32_t head, tail, count;
	struct spdk_aio_ring *ring;
	struct timespec timeout;
	struct io_event *kevents;

	ring = (struct spdk_aio_ring *)io_ctx;

	if (spdk_unlikely(ring->version != SPDK_AIO_RING_VERSION || ring->incompat_features != 0)) {
		timeout.tv_sec = 0;
		timeout.tv_nsec = 0;

		return io_getevents(io_ctx, 0, max, uevents, &timeout);
	}

	/* Read the current state out of the ring */
	head = ring->head;
	tail = ring->tail;

	/* This memory barrier is required to prevent the loads above
	 * from being re-ordered with stores to the events array
	 * potentially occurring on other threads. */
	spdk_smp_rmb();

	/* Calculate how many items are in the circular ring */
	count = tail - head;
	if (tail < head) {
		count += ring->size;
	}

	/* Reduce the count to the limit provided by the user */
	count = spdk_min(max, count);

	/* Grab the memory location of the event array */
	kevents = (struct io_event *)((uintptr_t)ring + ring->header_length);

	/* Copy the events out of the ring. */
	if ((head + count) <= ring->size) {
		/* Only one copy is required */
		memcpy(uevents, &kevents[head], count * sizeof(struct io_event));
	} else {
		uint32_t first_part = ring->size - head;
		/* Two copies are required */
		memcpy(uevents, &kevents[head], first_part * sizeof(struct io_event));
		memcpy(&uevents[first_part], &kevents[0], (count - first_part) * sizeof(struct io_event));
	}

	/* Update the head pointer. On x86, stores will not be reordered with older loads,
	 * so the copies out of the event array will always be complete prior to this
	 * update becoming visible. On other architectures this is not guaranteed, so
	 * add a barrier. */
#if defined(__i386__) || defined(__x86_64__)
	spdk_compiler_barrier();
#else
	spdk_smp_mb();
#endif
	ring->head = (head + count) % ring->size;

	return count;
}

static int
bdev_aio_io_channel_poll(struct bdev_aio_io_channel *io_ch)
{
	int nr, i = 0;
	struct bdev_aio_task *aio_task;
	struct io_event events[SPDK_AIO_QUEUE_DEPTH];
	uint64_t io_result;

	nr = bdev_user_io_getevents(io_ch->io_ctx, SPDK_AIO_QUEUE_DEPTH, events);

	if (nr < 0) {
		return 0;
	}

#define MAX_AIO_ERRNO 256
	for (i = 0; i < nr; i++) {
		aio_task = events[i].data;
		aio_task->ch->io_inflight--;
		io_result = events[i].res;
		if (io_result == aio_task->len) {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_SUCCESS);
		} else if (io_result < MAX_AIO_ERRNO) {
			/* Linux AIO will return its errno to io_event.res */
			int aio_errno = io_result;

			spdk_bdev_io_complete_aio_status(spdk_bdev_io_from_ctx(aio_task), -aio_errno);
		} else {
			SPDK_ERRLOG("failed to complete aio: requested len is %lu, but completed len is %lu.\n",
				    aio_task->len, io_result);
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(aio_task), SPDK_BDEV_IO_STATUS_FAILED);
		}
	}

	return nr;
}

static int
bdev_aio_group_poll(void *arg)
{
	struct bdev_aio_group_channel *group_ch = arg;
	struct bdev_aio_io_channel *io_ch;
	int nr = 0;

	TAILQ_FOREACH(io_ch, &group_ch->io_ch_head, link) {
		nr += bdev_aio_io_channel_poll(io_ch);
	}

	return nr > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static int
bdev_aio_group_interrupt(void *arg)
{
	struct bdev_aio_group_channel *group_ch = arg;
	int rc;
	uint64_t num_events;

	assert(group_ch->efd >= 0);

	/* if completed IO number is larger than SPDK_AIO_QUEUE_DEPTH,
	 * io_getevent should be called again to ensure all completed IO are processed.
	 */
	rc = read(group_ch->efd, &num_events, sizeof(num_events));
	if (rc < 0) {
		SPDK_ERRLOG("failed to acknowledge aio group: %s.\n", spdk_strerror(errno));
		return -errno;
	}

	if (num_events > SPDK_AIO_QUEUE_DEPTH) {
		num_events -= SPDK_AIO_QUEUE_DEPTH;
		rc = write(group_ch->efd, &num_events, sizeof(num_events));
		if (rc < 0) {
			SPDK_ERRLOG("failed to notify aio group: %s.\n", spdk_strerror(errno));
		}
	}

	return bdev_aio_group_poll(group_ch);
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

static int bdev_aio_reset_retry_timer(void *arg);

static void
_bdev_aio_get_io_inflight_done(struct spdk_io_channel_iter *i, int status)
{
	struct file_disk *fdisk = spdk_io_channel_iter_get_ctx(i);

	if (status == -1) {
		fdisk->reset_retry_timer = SPDK_POLLER_REGISTER(bdev_aio_reset_retry_timer, fdisk, 500);
		return;
	}

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(fdisk->reset_task), SPDK_BDEV_IO_STATUS_SUCCESS);
}

static int
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

	return SPDK_POLLER_BUSY;
}

static void
bdev_aio_reset(struct file_disk *fdisk, struct bdev_aio_task *aio_task)
{
	fdisk->reset_task = aio_task;

	bdev_aio_reset_retry_timer(fdisk);
}

static void
bdev_aio_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		    bool success)
{
	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		bdev_aio_readv((struct file_disk *)bdev_io->bdev->ctxt,
			       ch,
			       (struct bdev_aio_task *)bdev_io->driver_ctx,
			       bdev_io->u.bdev.iovs,
			       bdev_io->u.bdev.iovcnt,
			       bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
			       bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		bdev_aio_writev((struct file_disk *)bdev_io->bdev->ctxt,
				ch,
				(struct bdev_aio_task *)bdev_io->driver_ctx,
				bdev_io->u.bdev.iovs,
				bdev_io->u.bdev.iovcnt,
				bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
				bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen);
		break;
	default:
		SPDK_ERRLOG("Wrong io type\n");
		break;
	}
}

static int _bdev_aio_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	/* Read and write operations must be performed on buffers aligned to
	 * bdev->required_alignment. If user specified unaligned buffers,
	 * get the aligned buffer from the pool by calling spdk_bdev_io_get_buf. */
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		spdk_bdev_io_get_buf(bdev_io, bdev_aio_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return 0;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		bdev_aio_flush((struct file_disk *)bdev_io->bdev->ctxt,
			       (struct bdev_aio_task *)bdev_io->driver_ctx);
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

	if (io_setup(SPDK_AIO_QUEUE_DEPTH, &ch->io_ctx) < 0) {
		SPDK_ERRLOG("async I/O context setup failure\n");
		return -1;
	}

	ch->group_ch = spdk_io_channel_get_ctx(spdk_get_io_channel(&aio_if));
	TAILQ_INSERT_TAIL(&ch->group_ch->io_ch_head, ch, link);

	return 0;
}

static void
bdev_aio_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_aio_io_channel *ch = ctx_buf;

	io_destroy(ch->io_ctx);

	assert(ch->group_ch);
	TAILQ_REMOVE(&ch->group_ch->io_ch_head, ch, link);

	spdk_put_io_channel(spdk_io_channel_from_ctx(ch->group_ch));
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

	spdk_json_write_named_object_begin(w, "aio");

	spdk_json_write_named_string(w, "filename", fdisk->filename);

	spdk_json_write_object_end(w);

	return 0;
}

static void
bdev_aio_write_json_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct file_disk *fdisk = bdev->ctxt;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_aio_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	if (fdisk->block_size_override) {
		spdk_json_write_named_uint32(w, "block_size", bdev->blocklen);
	}
	spdk_json_write_named_string(w, "filename", fdisk->filename);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table aio_fn_table = {
	.destruct		= bdev_aio_destruct,
	.submit_request		= bdev_aio_submit_request,
	.io_type_supported	= bdev_aio_io_type_supported,
	.get_io_channel		= bdev_aio_get_io_channel,
	.dump_info_json		= bdev_aio_dump_info_json,
	.write_config_json	= bdev_aio_write_json_config,
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

static int
bdev_aio_register_interrupt(struct bdev_aio_group_channel *ch)
{
	int efd;

	efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (efd < 0) {
		return -1;
	}

	ch->intr = SPDK_INTERRUPT_REGISTER(efd, bdev_aio_group_interrupt, ch);
	if (ch->intr == NULL) {
		close(efd);
		return -1;
	}
	ch->efd = efd;

	return 0;
}

static void
bdev_aio_unregister_interrupt(struct bdev_aio_group_channel *ch)
{
	spdk_interrupt_unregister(&ch->intr);
	close(ch->efd);
	ch->efd = -1;
}

static void
bdev_aio_poller_set_interrupt_mode(struct spdk_poller *poller, void *cb_arg, bool interrupt_mode)
{
	return;
}

static int
bdev_aio_group_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_aio_group_channel *ch = ctx_buf;
	int rc;

	TAILQ_INIT(&ch->io_ch_head);
	/* Initialize ch->efd to be invalid and unused. */
	ch->efd = -1;
	if (spdk_interrupt_mode_is_enabled()) {
		rc = bdev_aio_register_interrupt(ch);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to prepare intr resource to bdev_aio\n");
			return rc;
		}
	}

	ch->poller = SPDK_POLLER_REGISTER(bdev_aio_group_poll, ch, 0);
	spdk_poller_register_interrupt(ch->poller, bdev_aio_poller_set_interrupt_mode, NULL);

	return 0;
}

static void
bdev_aio_group_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_aio_group_channel *ch = ctx_buf;

	if (!TAILQ_EMPTY(&ch->io_ch_head)) {
		SPDK_ERRLOG("Group channel of bdev aio has uncleared io channel\n");
	}

	spdk_poller_unregister(&ch->poller);
	if (spdk_interrupt_mode_is_enabled()) {
		bdev_aio_unregister_interrupt(ch);
	}
}

int
create_aio_bdev(const char *name, const char *filename, uint32_t block_size)
{
	struct file_disk *fdisk;
	uint32_t detected_block_size;
	uint64_t disk_size;
	int rc;

	fdisk = calloc(1, sizeof(*fdisk));
	if (!fdisk) {
		SPDK_ERRLOG("Unable to allocate enough memory for aio backend\n");
		return -ENOMEM;
	}

	fdisk->filename = strdup(filename);
	if (!fdisk->filename) {
		rc = -ENOMEM;
		goto error_return;
	}

	if (bdev_aio_open(fdisk)) {
		SPDK_ERRLOG("Unable to open file %s. fd: %d errno: %d\n", filename, fdisk->fd, errno);
		rc = -errno;
		goto error_return;
	}

	disk_size = spdk_fd_get_size(fdisk->fd);

	fdisk->disk.name = strdup(name);
	if (!fdisk->disk.name) {
		rc = -ENOMEM;
		goto error_return;
	}
	fdisk->disk.product_name = "AIO disk";
	fdisk->disk.module = &aio_if;

	fdisk->disk.write_cache = 1;

	detected_block_size = spdk_fd_get_blocklen(fdisk->fd);
	if (block_size == 0) {
		/* User did not specify block size - use autodetected block size. */
		if (detected_block_size == 0) {
			SPDK_ERRLOG("Block size could not be auto-detected\n");
			rc = -EINVAL;
			goto error_return;
		}
		fdisk->block_size_override = false;
		block_size = detected_block_size;
	} else {
		if (block_size < detected_block_size) {
			SPDK_ERRLOG("Specified block size %" PRIu32 " is smaller than "
				    "auto-detected block size %" PRIu32 "\n",
				    block_size, detected_block_size);
			rc = -EINVAL;
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
		rc = -EINVAL;
		goto error_return;
	}

	if (!spdk_u32_is_pow2(block_size)) {
		SPDK_ERRLOG("Invalid block size %" PRIu32 " (must be a power of 2.)\n", block_size);
		rc = -EINVAL;
		goto error_return;
	}

	fdisk->disk.blocklen = block_size;
	if (fdisk->block_size_override && detected_block_size) {
		fdisk->disk.required_alignment = spdk_u32log2(detected_block_size);
	} else {
		fdisk->disk.required_alignment = spdk_u32log2(block_size);
	}

	if (disk_size % fdisk->disk.blocklen != 0) {
		SPDK_ERRLOG("Disk size %" PRIu64 " is not a multiple of block size %" PRIu32 "\n",
			    disk_size, fdisk->disk.blocklen);
		rc = -EINVAL;
		goto error_return;
	}

	fdisk->disk.blockcnt = disk_size / fdisk->disk.blocklen;
	fdisk->disk.ctxt = fdisk;

	fdisk->disk.fn_table = &aio_fn_table;

	spdk_io_device_register(fdisk, bdev_aio_create_cb, bdev_aio_destroy_cb,
				sizeof(struct bdev_aio_io_channel),
				fdisk->disk.name);
	rc = spdk_bdev_register(&fdisk->disk);
	if (rc) {
		spdk_io_device_unregister(fdisk, NULL);
		goto error_return;
	}

	TAILQ_INSERT_TAIL(&g_aio_disk_head, fdisk, link);
	return 0;

error_return:
	bdev_aio_close(fdisk);
	aio_free_disk(fdisk);
	return rc;
}

int
bdev_aio_rescan(struct spdk_bdev *bdev)
{
	struct file_disk *fdisk;
	uint64_t disk_size, blockcnt;
	int rc;

	if (!bdev || bdev->module != &aio_if) {
		return -ENODEV;
	}

	fdisk = SPDK_CONTAINEROF(bdev, struct file_disk, disk);
	disk_size = spdk_fd_get_size(fdisk->fd);
	blockcnt = disk_size / fdisk->disk.blocklen;

	if (fdisk->disk.blockcnt != blockcnt) {
		SPDK_NOTICELOG("AIO device is resized: bdev name %s, old block count %" PRIu64 ", new block count %"
			       PRIu64 "\n",
			       fdisk->filename,
			       fdisk->disk.blockcnt,
			       blockcnt);
		rc = spdk_bdev_notify_blockcnt_change(&fdisk->disk, blockcnt);
		if (rc != 0) {
			SPDK_ERRLOG("Could not change num blocks for aio bdev: name %s, errno: %d.\n",
				    fdisk->filename, rc);
			return rc;
		}
	}

	return 0;
}

struct delete_aio_bdev_ctx {
	delete_aio_bdev_complete cb_fn;
	void *cb_arg;
};

static void
aio_bdev_unregister_cb(void *arg, int bdeverrno)
{
	struct delete_aio_bdev_ctx *ctx = arg;

	ctx->cb_fn(ctx->cb_arg, bdeverrno);
	free(ctx);
}

void
bdev_aio_delete(struct spdk_bdev *bdev, delete_aio_bdev_complete cb_fn, void *cb_arg)
{
	struct delete_aio_bdev_ctx *ctx;

	if (!bdev || bdev->module != &aio_if) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	spdk_bdev_unregister(bdev, aio_bdev_unregister_cb, ctx);
}

static int
bdev_aio_initialize(void)
{
	spdk_io_device_register(&aio_if, bdev_aio_group_create_cb, bdev_aio_group_destroy_cb,
				sizeof(struct bdev_aio_group_channel), "aio_module");

	return 0;
}

static void
bdev_aio_fini(void)
{
	spdk_io_device_unregister(&aio_if, NULL);
}

SPDK_LOG_REGISTER_COMPONENT(aio)
