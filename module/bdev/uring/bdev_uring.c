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

#include "bdev_uring.h"

#include "spdk/stdinc.h"

#include "spdk/barrier.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/fd.h"
#include "spdk/likely.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/util.h"
#include "spdk/string.h"

#include "spdk/log.h"
#include "spdk_internal/uring.h"

struct bdev_uring_io_channel {
	struct bdev_uring_group_channel		*group_ch;
};

struct bdev_uring_group_channel {
	uint64_t				io_inflight;
	uint64_t				io_pending;
	struct spdk_poller			*poller;
	struct io_uring				uring;
};

struct bdev_uring_task {
	uint64_t			len;
	struct bdev_uring_io_channel	*ch;
	TAILQ_ENTRY(bdev_uring_task)	link;
};

struct bdev_uring {
	struct spdk_bdev	bdev;
	char			*filename;
	int			fd;
	TAILQ_ENTRY(bdev_uring)  link;
};

static int bdev_uring_init(void);
static void bdev_uring_fini(void);
static void uring_free_bdev(struct bdev_uring *uring);
static TAILQ_HEAD(, bdev_uring) g_uring_bdev_head = TAILQ_HEAD_INITIALIZER(g_uring_bdev_head);

#define SPDK_URING_QUEUE_DEPTH 512
#define MAX_EVENTS_PER_POLL 32

static int
bdev_uring_get_ctx_size(void)
{
	return sizeof(struct bdev_uring_task);
}

static struct spdk_bdev_module uring_if = {
	.name		= "uring",
	.module_init	= bdev_uring_init,
	.module_fini	= bdev_uring_fini,
	.get_ctx_size	= bdev_uring_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(uring, &uring_if)

static int
bdev_uring_open(struct bdev_uring *bdev)
{
	int fd;

	fd = open(bdev->filename, O_RDWR | O_DIRECT | O_NOATIME);
	if (fd < 0) {
		/* Try without O_DIRECT for non-disk files */
		fd = open(bdev->filename, O_RDWR | O_NOATIME);
		if (fd < 0) {
			SPDK_ERRLOG("open() failed (file:%s), errno %d: %s\n",
				    bdev->filename, errno, spdk_strerror(errno));
			bdev->fd = -1;
			return -1;
		}
	}

	bdev->fd = fd;

	return 0;
}

static int
bdev_uring_close(struct bdev_uring *bdev)
{
	int rc;

	if (bdev->fd == -1) {
		return 0;
	}

	rc = close(bdev->fd);
	if (rc < 0) {
		SPDK_ERRLOG("close() failed (fd=%d), errno %d: %s\n",
			    bdev->fd, errno, spdk_strerror(errno));
		return -1;
	}

	bdev->fd = -1;

	return 0;
}

static int64_t
bdev_uring_readv(struct bdev_uring *uring, struct spdk_io_channel *ch,
		 struct bdev_uring_task *uring_task,
		 struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct bdev_uring_io_channel *uring_ch = spdk_io_channel_get_ctx(ch);
	struct bdev_uring_group_channel *group_ch = uring_ch->group_ch;
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(&group_ch->uring);
	io_uring_prep_readv(sqe, uring->fd, iov, iovcnt, offset);
	io_uring_sqe_set_data(sqe, uring_task);
	uring_task->len = nbytes;
	uring_task->ch = uring_ch;

	SPDK_DEBUGLOG(uring, "read %d iovs size %lu to off: %#lx\n",
		      iovcnt, nbytes, offset);

	group_ch->io_pending++;
	return nbytes;
}

static int64_t
bdev_uring_writev(struct bdev_uring *uring, struct spdk_io_channel *ch,
		  struct bdev_uring_task *uring_task,
		  struct iovec *iov, int iovcnt, size_t nbytes, uint64_t offset)
{
	struct bdev_uring_io_channel *uring_ch = spdk_io_channel_get_ctx(ch);
	struct bdev_uring_group_channel *group_ch = uring_ch->group_ch;
	struct io_uring_sqe *sqe;

	sqe = io_uring_get_sqe(&group_ch->uring);
	io_uring_prep_writev(sqe, uring->fd, iov, iovcnt, offset);
	io_uring_sqe_set_data(sqe, uring_task);
	uring_task->len = nbytes;
	uring_task->ch = uring_ch;

	SPDK_DEBUGLOG(uring, "write %d iovs size %lu from off: %#lx\n",
		      iovcnt, nbytes, offset);

	group_ch->io_pending++;
	return nbytes;
}

static int
bdev_uring_destruct(void *ctx)
{
	struct bdev_uring *uring = ctx;
	int rc = 0;

	TAILQ_REMOVE(&g_uring_bdev_head, uring, link);
	rc = bdev_uring_close(uring);
	if (rc < 0) {
		SPDK_ERRLOG("bdev_uring_close() failed\n");
	}
	spdk_io_device_unregister(uring, NULL);
	uring_free_bdev(uring);
	return rc;
}

static int
bdev_uring_reap(struct io_uring *ring, int max)
{
	int i, count, ret;
	struct io_uring_cqe *cqe;
	struct bdev_uring_task *uring_task;
	enum spdk_bdev_io_status status;

	count = 0;
	for (i = 0; i < max; i++) {
		ret = io_uring_peek_cqe(ring, &cqe);
		if (ret != 0) {
			return ret;
		}

		if (cqe == NULL) {
			return count;
		}

		uring_task = (struct bdev_uring_task *)cqe->user_data;
		if (cqe->res != (signed)uring_task->len) {
			status = SPDK_BDEV_IO_STATUS_FAILED;
		} else {
			status = SPDK_BDEV_IO_STATUS_SUCCESS;
		}

		uring_task->ch->group_ch->io_inflight--;
		io_uring_cqe_seen(ring, cqe);
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(uring_task), status);
		count++;
	}

	return count;
}

static int
bdev_uring_group_poll(void *arg)
{
	struct bdev_uring_group_channel *group_ch = arg;
	int to_complete, to_submit;
	int count, ret;

	to_submit = group_ch->io_pending;

	if (to_submit > 0) {
		/* If there are I/O to submit, use io_uring_submit here.
		 * It will automatically call spdk_io_uring_enter appropriately. */
		ret = io_uring_submit(&group_ch->uring);
		if (ret < 0) {
			return SPDK_POLLER_BUSY;
		}

		group_ch->io_pending = 0;
		group_ch->io_inflight += to_submit;
	}

	to_complete = group_ch->io_inflight;
	count = 0;
	if (to_complete > 0) {
		count = bdev_uring_reap(&group_ch->uring, to_complete);
	}

	if (count + to_submit > 0) {
		return SPDK_POLLER_BUSY;
	} else {
		return SPDK_POLLER_IDLE;
	}
}

static void bdev_uring_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
				  bool success)
{
	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		bdev_uring_readv((struct bdev_uring *)bdev_io->bdev->ctxt,
				 ch,
				 (struct bdev_uring_task *)bdev_io->driver_ctx,
				 bdev_io->u.bdev.iovs,
				 bdev_io->u.bdev.iovcnt,
				 bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
				 bdev_io->u.bdev.offset_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		bdev_uring_writev((struct bdev_uring *)bdev_io->bdev->ctxt,
				  ch,
				  (struct bdev_uring_task *)bdev_io->driver_ctx,
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

static int _bdev_uring_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	/* Read and write operations must be performed on buffers aligned to
	 * bdev->required_alignment. If user specified unaligned buffers,
	 * get the aligned buffer from the pool by calling spdk_bdev_io_get_buf. */
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		spdk_bdev_io_get_buf(bdev_io, bdev_uring_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return 0;
	default:
		return -1;
	}
}

static void bdev_uring_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	if (_bdev_uring_submit_request(ch, bdev_io) < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
bdev_uring_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;
	default:
		return false;
	}
}

static int
bdev_uring_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_uring_io_channel *ch = ctx_buf;

	ch->group_ch = spdk_io_channel_get_ctx(spdk_get_io_channel(&uring_if));

	return 0;
}

static void
bdev_uring_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_uring_io_channel *ch = ctx_buf;

	spdk_put_io_channel(spdk_io_channel_from_ctx(ch->group_ch));
}

static struct spdk_io_channel *
bdev_uring_get_io_channel(void *ctx)
{
	struct bdev_uring *uring = ctx;

	return spdk_get_io_channel(uring);
}

static int
bdev_uring_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct bdev_uring *uring = ctx;

	spdk_json_write_named_object_begin(w, "uring");

	spdk_json_write_named_string(w, "filename", uring->filename);

	spdk_json_write_object_end(w);

	return 0;
}

static void
bdev_uring_write_json_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct bdev_uring *uring = bdev->ctxt;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_uring_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_uint32(w, "block_size", bdev->blocklen);
	spdk_json_write_named_string(w, "filename", uring->filename);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table uring_fn_table = {
	.destruct		= bdev_uring_destruct,
	.submit_request		= bdev_uring_submit_request,
	.io_type_supported	= bdev_uring_io_type_supported,
	.get_io_channel		= bdev_uring_get_io_channel,
	.dump_info_json		= bdev_uring_dump_info_json,
	.write_config_json	= bdev_uring_write_json_config,
};

static void uring_free_bdev(struct bdev_uring *uring)
{
	if (uring == NULL) {
		return;
	}
	free(uring->filename);
	free(uring->bdev.name);
	free(uring);
}

static int
bdev_uring_group_create_cb(void *io_device, void *ctx_buf)
{
	struct bdev_uring_group_channel *ch = ctx_buf;

	/* Do not use IORING_SETUP_IOPOLL until the Linux kernel can support not only
	 * local devices but also devices attached from remote target */
	if (io_uring_queue_init(SPDK_URING_QUEUE_DEPTH, &ch->uring, 0) < 0) {
		SPDK_ERRLOG("uring I/O context setup failure\n");
		return -1;
	}

	ch->poller = SPDK_POLLER_REGISTER(bdev_uring_group_poll, ch, 0);
	return 0;
}

static void
bdev_uring_group_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_uring_group_channel *ch = ctx_buf;

	io_uring_queue_exit(&ch->uring);

	spdk_poller_unregister(&ch->poller);
}

struct spdk_bdev *
create_uring_bdev(const char *name, const char *filename, uint32_t block_size)
{
	struct bdev_uring *uring;
	uint32_t detected_block_size;
	uint64_t bdev_size;
	int rc;

	uring = calloc(1, sizeof(*uring));
	if (!uring) {
		SPDK_ERRLOG("Unable to allocate enough memory for uring backend\n");
		return NULL;
	}

	uring->filename = strdup(filename);
	if (!uring->filename) {
		goto error_return;
	}

	if (bdev_uring_open(uring)) {
		SPDK_ERRLOG("Unable to open file %s. fd: %d errno: %d\n", filename, uring->fd, errno);
		goto error_return;
	}

	bdev_size = spdk_fd_get_size(uring->fd);

	uring->bdev.name = strdup(name);
	if (!uring->bdev.name) {
		goto error_return;
	}
	uring->bdev.product_name = "URING bdev";
	uring->bdev.module = &uring_if;

	uring->bdev.write_cache = 1;

	detected_block_size = spdk_fd_get_blocklen(uring->fd);
	if (block_size == 0) {
		/* User did not specify block size - use autodetected block size. */
		if (detected_block_size == 0) {
			SPDK_ERRLOG("Block size could not be auto-detected\n");
			goto error_return;
		}
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
	}

	if (block_size < 512) {
		SPDK_ERRLOG("Invalid block size %" PRIu32 " (must be at least 512).\n", block_size);
		goto error_return;
	}

	if (!spdk_u32_is_pow2(block_size)) {
		SPDK_ERRLOG("Invalid block size %" PRIu32 " (must be a power of 2.)\n", block_size);
		goto error_return;
	}

	uring->bdev.blocklen = block_size;
	uring->bdev.required_alignment = spdk_u32log2(block_size);

	if (bdev_size % uring->bdev.blocklen != 0) {
		SPDK_ERRLOG("Disk size %" PRIu64 " is not a multiple of block size %" PRIu32 "\n",
			    bdev_size, uring->bdev.blocklen);
		goto error_return;
	}

	uring->bdev.blockcnt = bdev_size / uring->bdev.blocklen;
	uring->bdev.ctxt = uring;

	uring->bdev.fn_table = &uring_fn_table;

	spdk_io_device_register(uring, bdev_uring_create_cb, bdev_uring_destroy_cb,
				sizeof(struct bdev_uring_io_channel),
				uring->bdev.name);
	rc = spdk_bdev_register(&uring->bdev);
	if (rc) {
		spdk_io_device_unregister(uring, NULL);
		goto error_return;
	}

	TAILQ_INSERT_TAIL(&g_uring_bdev_head, uring, link);
	return &uring->bdev;

error_return:
	bdev_uring_close(uring);
	uring_free_bdev(uring);
	return NULL;
}

struct delete_uring_bdev_ctx {
	spdk_delete_uring_complete cb_fn;
	void *cb_arg;
};

static void
uring_bdev_unregister_cb(void *arg, int bdeverrno)
{
	struct delete_uring_bdev_ctx *ctx = arg;

	ctx->cb_fn(ctx->cb_arg, bdeverrno);
	free(ctx);
}

void
delete_uring_bdev(struct spdk_bdev *bdev, spdk_delete_uring_complete cb_fn, void *cb_arg)
{
	struct delete_uring_bdev_ctx *ctx;

	if (!bdev || bdev->module != &uring_if) {
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
	spdk_bdev_unregister(bdev, uring_bdev_unregister_cb, ctx);
}

static int
bdev_uring_init(void)
{
	spdk_io_device_register(&uring_if, bdev_uring_group_create_cb, bdev_uring_group_destroy_cb,
				sizeof(struct bdev_uring_group_channel), "uring_module");

	return 0;
}

static void
bdev_uring_fini(void)
{
	spdk_io_device_unregister(&uring_if, NULL);
}

SPDK_LOG_REGISTER_COMPONENT(uring)
