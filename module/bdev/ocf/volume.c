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

#include <ocf/ocf.h>

#include "spdk/bdev_module.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/log.h"

#include "data.h"
#include "volume.h"
#include "ctx.h"
#include "vbdev_ocf.h"

static int
vbdev_ocf_volume_open(ocf_volume_t volume, void *opts)
{
	struct vbdev_ocf_base **priv = ocf_volume_get_priv(volume);
	struct vbdev_ocf_base *base;

	if (opts) {
		base = opts;
	} else {
		base = vbdev_ocf_get_base_by_name(ocf_volume_get_uuid(volume)->data);
		if (base == NULL) {
			return -ENODEV;
		}
	}

	*priv = base;

	return 0;
}

static void
vbdev_ocf_volume_close(ocf_volume_t volume)
{
}

static uint64_t
vbdev_ocf_volume_get_length(ocf_volume_t volume)
{
	struct vbdev_ocf_base *base = *((struct vbdev_ocf_base **)ocf_volume_get_priv(volume));
	uint64_t len;

	len = base->bdev->blocklen * base->bdev->blockcnt;

	return len;
}

static int
vbdev_ocf_volume_io_set_data(struct ocf_io *io, ctx_data_t *data,
			     uint32_t offset)
{
	struct ocf_io_ctx *io_ctx = ocf_get_io_ctx(io);

	io_ctx->offset = offset;
	io_ctx->data = data;

	if (io_ctx->data && offset >= io_ctx->data->size) {
		return -ENOBUFS;
	}

	return 0;
}

static ctx_data_t *
vbdev_ocf_volume_io_get_data(struct ocf_io *io)
{
	return ocf_get_io_ctx(io)->data;
}

static void
vbdev_ocf_volume_io_get(struct ocf_io *io)
{
	struct ocf_io_ctx *io_ctx = ocf_get_io_ctx(io);

	io_ctx->ref++;
}

static void
vbdev_ocf_volume_io_put(struct ocf_io *io)
{
	struct ocf_io_ctx *io_ctx = ocf_get_io_ctx(io);

	if (--io_ctx->ref) {
		return;
	}
}

static int
get_starting_vec(struct iovec *iovs, int iovcnt, int *offset)
{
	int i;
	size_t off;

	off = *offset;

	for (i = 0; i < iovcnt; i++) {
		if (off < iovs[i].iov_len) {
			*offset = off;
			return i;
		}
		off -= iovs[i].iov_len;
	}

	return -1;
}

static void
initialize_cpy_vector(struct iovec *cpy_vec, int cpy_vec_len, struct iovec *orig_vec,
		      int orig_vec_len,
		      size_t offset, size_t bytes)
{
	void *curr_base;
	int len, i;

	i = 0;

	while (bytes > 0) {
		curr_base = orig_vec[i].iov_base + offset;
		len = MIN(bytes, orig_vec[i].iov_len - offset);

		cpy_vec[i].iov_base = curr_base;
		cpy_vec[i].iov_len = len;

		bytes -= len;
		offset = 0;
		i++;
	}
}

static void
vbdev_ocf_volume_submit_io_cb(struct spdk_bdev_io *bdev_io, bool success, void *opaque)
{
	struct ocf_io *io;
	struct ocf_io_ctx *io_ctx;

	assert(opaque);

	io = opaque;
	io_ctx = ocf_get_io_ctx(io);
	assert(io_ctx != NULL);

	if (!success) {
		io_ctx->error |= 1;
	}

	if (io_ctx->iovs_allocated && bdev_io != NULL) {
		env_free(bdev_io->u.bdev.iovs);
	}

	if (io_ctx->error) {
		SPDK_DEBUGLOG(vbdev_ocf_volume,
			      "base returned error on io submission: %d\n", io_ctx->error);
	}

	if (io->io_queue == NULL && io_ctx->ch != NULL) {
		spdk_put_io_channel(io_ctx->ch);
	}

	vbdev_ocf_volume_io_put(io);
	if (bdev_io) {
		spdk_bdev_free_io(bdev_io);
	}

	if (--io_ctx->rq_cnt == 0) {
		io->end(io, io_ctx->error);
	}
}

static int
prepare_submit(struct ocf_io *io)
{
	struct ocf_io_ctx *io_ctx = ocf_get_io_ctx(io);
	struct vbdev_ocf_qctx *qctx;
	struct vbdev_ocf_base *base;
	ocf_queue_t q = io->io_queue;
	ocf_cache_t cache;
	struct vbdev_ocf_cache_ctx *cctx;
	int rc = 0;

	io_ctx->rq_cnt++;
	if (io_ctx->rq_cnt != 1) {
		return 0;
	}

	vbdev_ocf_volume_io_get(io);
	base = *((struct vbdev_ocf_base **)ocf_volume_get_priv(ocf_io_get_volume(io)));

	if (io->io_queue == NULL) {
		/* In case IO is initiated by OCF, queue is unknown
		 * so we have to get io channel ourselves */
		io_ctx->ch = spdk_bdev_get_io_channel(base->desc);
		if (io_ctx->ch == NULL) {
			return -EPERM;
		}
		return 0;
	}

	cache = ocf_queue_get_cache(q);
	cctx = ocf_cache_get_priv(cache);
	if (cctx == NULL) {
		return -EFAULT;
	}

	if (q == cctx->cleaner_queue || q == cctx->mngt_queue) {
		io_ctx->ch = base->management_channel;
		return 0;
	}

	qctx = ocf_queue_get_priv(q);
	if (qctx == NULL) {
		return -EFAULT;
	}

	if (base->is_cache) {
		io_ctx->ch = qctx->cache_ch;
	} else {
		io_ctx->ch = qctx->core_ch;
	}

	return rc;
}

static void
vbdev_ocf_volume_submit_flush(struct ocf_io *io)
{
	struct vbdev_ocf_base *base =
		*((struct vbdev_ocf_base **)
		  ocf_volume_get_priv(ocf_io_get_volume(io)));
	struct ocf_io_ctx *io_ctx = ocf_get_io_ctx(io);
	int status;

	status = prepare_submit(io);
	if (status) {
		SPDK_ERRLOG("Preparing io failed with status=%d\n", status);
		vbdev_ocf_volume_submit_io_cb(NULL, false, io);
		return;
	}

	status = spdk_bdev_flush(
			 base->desc, io_ctx->ch,
			 io->addr, io->bytes,
			 vbdev_ocf_volume_submit_io_cb, io);
	if (status) {
		/* Since callback is not called, we need to do it manually to free io structures */
		SPDK_ERRLOG("Submission failed with status=%d\n", status);
		vbdev_ocf_volume_submit_io_cb(NULL, false, io);
	}
}

static void
vbdev_ocf_volume_submit_io(struct ocf_io *io)
{
	struct vbdev_ocf_base *base =
		*((struct vbdev_ocf_base **)
		  ocf_volume_get_priv(ocf_io_get_volume(io)));
	struct ocf_io_ctx *io_ctx = ocf_get_io_ctx(io);
	struct iovec *iovs;
	int iovcnt, status = 0, i, offset;
	uint64_t addr, len;

	if (io->flags == OCF_WRITE_FLUSH) {
		vbdev_ocf_volume_submit_flush(io);
		return;
	}

	status = prepare_submit(io);
	if (status) {
		SPDK_ERRLOG("Preparing io failed with status=%d\n", status);
		vbdev_ocf_volume_submit_io_cb(NULL, false, io);
		return;
	}

	/* IO fields */
	addr = io->addr;
	len = io->bytes;
	offset = io_ctx->offset;

	if (len < io_ctx->data->size) {
		if (io_ctx->data->iovcnt == 1) {
			if (io->dir == OCF_READ) {
				status = spdk_bdev_read(base->desc, io_ctx->ch,
							io_ctx->data->iovs[0].iov_base + offset, addr, len,
							vbdev_ocf_volume_submit_io_cb, io);
			} else if (io->dir == OCF_WRITE) {
				status = spdk_bdev_write(base->desc, io_ctx->ch,
							 io_ctx->data->iovs[0].iov_base + offset, addr, len,
							 vbdev_ocf_volume_submit_io_cb, io);
			}
			goto end;
		} else {
			i = get_starting_vec(io_ctx->data->iovs, io_ctx->data->iovcnt, &offset);

			if (i < 0) {
				SPDK_ERRLOG("offset bigger than data size\n");
				vbdev_ocf_volume_submit_io_cb(NULL, false, io);
				return;
			}

			iovcnt = io_ctx->data->iovcnt - i;

			io_ctx->iovs_allocated = true;
			iovs = env_malloc(sizeof(*iovs) * iovcnt, ENV_MEM_NOIO);

			if (!iovs) {
				SPDK_ERRLOG("allocation failed\n");
				vbdev_ocf_volume_submit_io_cb(NULL, false, io);
				return;
			}

			initialize_cpy_vector(iovs, io_ctx->data->iovcnt, &io_ctx->data->iovs[i],
					      iovcnt, offset, len);
		}
	} else {
		iovs = io_ctx->data->iovs;
		iovcnt = io_ctx->data->iovcnt;
	}

	if (io->dir == OCF_READ) {
		status = spdk_bdev_readv(base->desc, io_ctx->ch,
					 iovs, iovcnt, addr, len, vbdev_ocf_volume_submit_io_cb, io);
	} else if (io->dir == OCF_WRITE) {
		status = spdk_bdev_writev(base->desc, io_ctx->ch,
					  iovs, iovcnt, addr, len, vbdev_ocf_volume_submit_io_cb, io);
	}

end:
	if (status) {
		/* TODO [ENOMEM]: implement ENOMEM handling when submitting IO to base device */

		/* Since callback is not called, we need to do it manually to free io structures */
		SPDK_ERRLOG("submission failed with status=%d\n", status);
		vbdev_ocf_volume_submit_io_cb(NULL, false, io);
	}
}

static void
vbdev_ocf_volume_submit_discard(struct ocf_io *io)
{
	struct vbdev_ocf_base *base =
		*((struct vbdev_ocf_base **)
		  ocf_volume_get_priv(ocf_io_get_volume(io)));
	struct ocf_io_ctx *io_ctx = ocf_get_io_ctx(io);
	int status = 0;

	status = prepare_submit(io);
	if (status) {
		SPDK_ERRLOG("Preparing io failed with status=%d\n", status);
		vbdev_ocf_volume_submit_io_cb(NULL, false, io);
		return;
	}

	status = spdk_bdev_unmap(
			 base->desc, io_ctx->ch,
			 io->addr, io->bytes,
			 vbdev_ocf_volume_submit_io_cb, io);
	if (status) {
		/* Since callback is not called, we need to do it manually to free io structures */
		SPDK_ERRLOG("Submission failed with status=%d\n", status);
		vbdev_ocf_volume_submit_io_cb(NULL, false, io);
	}
}

static void
vbdev_ocf_volume_submit_metadata(struct ocf_io *io)
{
	/* Implement with persistent metadata support */
}

static unsigned int
vbdev_ocf_volume_get_max_io_size(ocf_volume_t volume)
{
	return 131072;
}

static struct ocf_volume_properties vbdev_volume_props = {
	.name = "SPDK_block_device",
	.io_priv_size = sizeof(struct ocf_io_ctx),
	.volume_priv_size = sizeof(struct vbdev_ocf_base *),
	.caps = {
		.atomic_writes = 0 /* to enable need to have ops->submit_metadata */
	},
	.ops = {
		.open = vbdev_ocf_volume_open,
		.close = vbdev_ocf_volume_close,
		.get_length = vbdev_ocf_volume_get_length,
		.submit_io = vbdev_ocf_volume_submit_io,
		.submit_discard = vbdev_ocf_volume_submit_discard,
		.submit_flush = vbdev_ocf_volume_submit_flush,
		.get_max_io_size = vbdev_ocf_volume_get_max_io_size,
		.submit_metadata = vbdev_ocf_volume_submit_metadata,
	},
	.io_ops = {
		.set_data = vbdev_ocf_volume_io_set_data,
		.get_data = vbdev_ocf_volume_io_get_data,
	},
};

int
vbdev_ocf_volume_init(void)
{
	return ocf_ctx_register_volume_type(vbdev_ocf_ctx, SPDK_OBJECT, &vbdev_volume_props);
}

void
vbdev_ocf_volume_cleanup(void)
{
	ocf_ctx_unregister_volume_type(vbdev_ocf_ctx, SPDK_OBJECT);
}

SPDK_LOG_REGISTER_COMPONENT(vbdev_ocf_volume)
