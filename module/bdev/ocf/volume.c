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
#include "spdk/io_channel.h"
#include "spdk_internal/log.h"

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
		SPDK_DEBUGLOG(SPDK_TRACE_VBDEV_OCF_VOLUME,
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
	struct vbdev_ocf_qcxt *qctx;
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
	base = *((struct vbdev_ocf_base **)ocf_volume_get_priv(io->volume));

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
	struct vbdev_ocf_base *base = *((struct vbdev_ocf_base **)ocf_volume_get_priv(io->volume));
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

typedef int (*spdk_bdev_submit_scalar_io)(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		void *buf, uint64_t offset_blocks, uint64_t num_blocks,
		spdk_bdev_io_completion_cb cb, void *cb_arg);

typedef int (*spdk_bdev_submit_vector_io)(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		struct iovec *iov, int iovcnt,
		uint64_t offset, uint64_t nbytes,
		spdk_bdev_io_completion_cb cb, void *cb_arg);

static int
vbdev_ocf_volume_bytes_2_blocks(struct spdk_bdev *bdev, uint64_t bytes,
				uint64_t *num_blocks)
{
	*num_blocks = bytes / bdev->blocklen;

	return bytes % bdev->blocklen;
}

static int
vbdev_ocf_volume_calculate_addr_len(struct spdk_bdev *bdev, uint64_t addr_bytes,
				    uint64_t len_bytes, uint64_t *addr_blk,
				    uint64_t *len_blk)
{
	if (vbdev_ocf_volume_bytes_2_blocks(bdev, addr_bytes, addr_blk)) {
		SPDK_ERRLOG("Address are not aligned to block size\n");
		return -EINVAL;
	}

	if (vbdev_ocf_volume_bytes_2_blocks(bdev, len_bytes, len_blk)) {
		SPDK_ERRLOG("Length are not aligned to block size\n");
		return -EINVAL;
	}

	return 0;
}

static int
vbdev_ocf_volume_atomic_fill_md_buf(ocf_cache_t cache, uint64_t blk_addr,
				    uint64_t blk_len, struct ocf_atomic_metadata *metadata)
{
	uint32_t i;
	uint64_t addr = ATOMIC_BLOCK_LEN * blk_addr;

	for (i = 0; i < blk_len; i++, addr += ATOMIC_BLOCK_LEN, metadata++) {
		if (ocf_metadata_get_atomic_entry(cache, addr, metadata)) {
			return -EINVAL;
		}
	}

	return 0;
}

static void *
vbdev_ocf_volume_submit_prepare_md_buf(struct ocf_io *io, uint64_t blk_addr,
				       uint64_t blk_len)
{
	struct vbdev_ocf_base *base = *((struct vbdev_ocf_base **)ocf_volume_get_priv(io->volume));
	struct ocf_atomic_metadata *metadata;

	/*
	 * OCF need 8B meatadata per each 512B of data
	 */
	metadata = env_malloc(sizeof(*metadata) * blk_len, ENV_MEM_NOIO);
	if (!metadata) {
		SPDK_ERRLOG("allocation for metadata failed\n");
		vbdev_ocf_volume_submit_io_cb(NULL, false, io);
		return NULL;
	}
	/*
	 * Fill buffer with ocf metadata
	 */
	if (vbdev_ocf_volume_atomic_fill_md_buf(base->parent->ocf_cache, blk_addr,
						blk_len, (void *)metadata)) {
		SPDK_ERRLOG("can not fill metadata buffor\n");
		vbdev_ocf_volume_submit_io_cb(NULL, false, io);
		return NULL;
	}

	return (void *)metadata;
}

static void
vbdev_ocf_volume_atomic_submit_io_cb(struct spdk_bdev_io *bdev_io, bool success, void *opaque)
{
	struct ocf_io *io;
	struct ocf_io_ctx *io_ctx;

	assert(opaque);

	io = opaque;
	io_ctx = ocf_get_io_ctx(io);
	assert(io_ctx != NULL);

	if (io_ctx->md_buf) {
		env_free(io_ctx->md_buf);
	}

	vbdev_ocf_volume_submit_io_cb(bdev_io, success, opaque);
}

static void
vbdev_ocf_volume_submit_scalar_io(struct ocf_io *io, uint64_t blk_addr,
				  uint64_t blk_len, int offset, void *md_buf)
{
	struct vbdev_ocf_base *base = *((struct vbdev_ocf_base **)ocf_volume_get_priv(io->volume));
	struct ocf_io_ctx *io_ctx = ocf_get_io_ctx(io);
	spdk_bdev_submit_scalar_io submit_scalar_io_fn;
	int status;

	if (md_buf && io->dir) {
		status = spdk_bdev_write_blocks_with_md(base->desc, io_ctx->ch,
							io_ctx->data->iovs[0].iov_base + offset, md_buf,
							blk_addr, blk_len, vbdev_ocf_volume_atomic_submit_io_cb, io);
		goto end;
	}

	submit_scalar_io_fn = io->dir ? spdk_bdev_write_blocks : spdk_bdev_read_blocks;
	status = (submit_scalar_io_fn)(base->desc, io_ctx->ch,
				       io_ctx->data->iovs[0].iov_base + offset,
				       blk_addr, blk_len, vbdev_ocf_volume_submit_io_cb, io);
end:
	if (status) {
		SPDK_ERRLOG("Submission of scalar io failed with status=%d\n", status);
		vbdev_ocf_volume_atomic_submit_io_cb(NULL, false, io);
	}
}

static void
_vbdev_ocf_volume_submit_io(struct ocf_io *io, bool with_meta)
{
	struct vbdev_ocf_base *base = *((struct vbdev_ocf_base **)ocf_volume_get_priv(io->volume));
	struct ocf_io_ctx *io_ctx = ocf_get_io_ctx(io);
	struct iovec *iovs;
	int iovcnt, status = 0, i, offset;
	uint64_t blk_addr, blk_len;

	spdk_bdev_submit_vector_io submit_vector_io_fn;

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

	/* Calculate address and length on blocks */
	if (vbdev_ocf_volume_calculate_addr_len(base->bdev, io->addr, io->bytes,
						&blk_addr, &blk_len)) {
		vbdev_ocf_volume_submit_io_cb(NULL, false, io);
		return;
	}

	offset = io_ctx->offset;

	if (with_meta && io->dir) {
		io_ctx->md_buf = vbdev_ocf_volume_submit_prepare_md_buf(io, blk_addr, blk_len);
		if (!io_ctx->md_buf) {
			return;
		}
	} else {
		io_ctx->md_buf = NULL;
	}

	if (io_ctx->data->iovcnt == 1) {
		vbdev_ocf_volume_submit_scalar_io(io, blk_addr,
						  blk_len, offset, io_ctx->md_buf);
		return;
	}

	if (io->bytes < io_ctx->data->size) {
		i = get_starting_vec(io_ctx->data->iovs, io_ctx->data->iovcnt, &offset);

		if (i < 0) {
			SPDK_ERRLOG("offset bigger than data size\n");
			vbdev_ocf_volume_atomic_submit_io_cb(NULL, false, io);
			return;
		}

		iovcnt = io_ctx->data->iovcnt - i;

		io_ctx->iovs_allocated = true;
		iovs = env_malloc(sizeof(*iovs) * iovcnt, ENV_MEM_NOIO);
		if (!iovs) {
			SPDK_ERRLOG("allocation failed\n");
			vbdev_ocf_volume_atomic_submit_io_cb(NULL, false, io);
			return;
		}

		initialize_cpy_vector(iovs, io_ctx->data->iovcnt, &io_ctx->data->iovs[i],
				      iovcnt, offset, io->bytes);
	} else {
		iovs = io_ctx->data->iovs;
		iovcnt = io_ctx->data->iovcnt;
	}

	if (io_ctx->md_buf) {
		status = spdk_bdev_writev_blocks_with_md(base->desc, io_ctx->ch,
				iovs, iovcnt, io_ctx->md_buf, blk_addr, blk_len,
				vbdev_ocf_volume_atomic_submit_io_cb, io);
		goto end;
	}

	submit_vector_io_fn = io->dir ? spdk_bdev_writev_blocks : spdk_bdev_readv_blocks;
	status = (submit_vector_io_fn)(base->desc, io_ctx->ch,
				       iovs, iovcnt, blk_addr, blk_len,
				       vbdev_ocf_volume_submit_io_cb, io);

	if (status) {
		/* TODO [ENOMEM]: implement ENOMEM handling when submitting IO to base device */

		/* Since callback is not called, we need to do it manually to free io structures */
		SPDK_ERRLOG("submission failed with status=%d\n", status);
		vbdev_ocf_volume_atomic_submit_io_cb(NULL, false, io);
	}
}

static void
vbdev_ocf_volume_submit_io(struct ocf_io *io)
{
	_vbdev_ocf_volume_submit_io(io, false);
}

static void
vbdev_ocf_volume_submit_discard(struct ocf_io *io)
{
	struct vbdev_ocf_base *base = *((struct vbdev_ocf_base **)ocf_volume_get_priv(io->volume));
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

static unsigned int
vbdev_ocf_volume_get_max_io_size(ocf_volume_t volume)
{
	return 131072;
}

static struct ocf_volume_properties vbdev_volume_props = {
	.name = "SPDK block device",
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
	},
	.io_ops = {
		.set_data = vbdev_ocf_volume_io_set_data,
		.get_data = vbdev_ocf_volume_io_get_data,
	},
};

static void
vbdev_ocf_volume_atomic_submit_io(struct ocf_io *io)
{
	struct vbdev_ocf_base *base = *((struct vbdev_ocf_base **)ocf_volume_get_priv(io->volume));
	struct spdk_bdev *bdev = base->bdev;

	if (bdev->blocklen != ATOMIC_BLOCK_LEN ||
	    bdev->md_len != OCF_ATOMIC_METADATA_SIZE) {
		/*
		 * OCF require 512B data + 8B metadata format
		 */
		SPDK_ERRLOG("Invalid device format\n");
		vbdev_ocf_volume_submit_io_cb(NULL, false, io);
		return;
	}

	_vbdev_ocf_volume_submit_io(io, true);
}

static void
vbdev_ocf_volume_atomic_submit_cb(struct spdk_bdev_io *bdev_io, bool success, void *opaque)
{
	struct ocf_io *io;
	struct ocf_io_ctx *io_ctx;

	assert(opaque);

	io = opaque;
	io_ctx = ocf_get_io_ctx(io);
	assert(io_ctx != NULL);

	if (io_ctx->md_buf) {
		env_free(io_ctx->md_buf);
	}

	if (bdev_io->u.bdev.iovs[0].iov_base) {
		env_free(bdev_io->u.bdev.iovs[0].iov_base);
	}

	vbdev_ocf_volume_submit_io_cb(bdev_io, success, opaque);
}

/*
 * This is specific function in OCF, OCF by this function wants to zeroed
 * whole device in case when discard/unmap are not working correctly.
 */
static void
vbdev_ocf_volume_submit_write_zeroes(struct ocf_io *io)
{

	struct vbdev_ocf_base *base = *((struct vbdev_ocf_base **)ocf_volume_get_priv(io->volume));
	struct ocf_io_ctx *io_ctx = ocf_get_io_ctx(io);
	int status = 0;
	uint64_t blk_addr, blk_len;
	void *data_buf;

	status = prepare_submit(io);
	if (status) {
		SPDK_ERRLOG("Preparing io failed with status=%d\n", status);
		vbdev_ocf_volume_submit_io_cb(NULL, false, io);
		return;
	}

	/* Calculate address and length on blocks */
	if (vbdev_ocf_volume_calculate_addr_len(base->bdev, io->addr, io->bytes,
						&blk_addr, &blk_len)) {
		vbdev_ocf_volume_submit_io_cb(NULL, false, io);
		return;
	}

	data_buf = env_zalloc(io->bytes, ENV_MEM_NOIO);
	if (!data_buf) {
		SPDK_ERRLOG("allocation for data failed\n");
		vbdev_ocf_volume_atomic_submit_cb(NULL, false, io);
		return;
	}

	io_ctx->md_buf = env_zalloc(OCF_ATOMIC_METADATA_SIZE * blk_len, ENV_MEM_NOIO);
	if (!io_ctx->md_buf) {
		SPDK_ERRLOG("allocation for metadata failed\n");
		vbdev_ocf_volume_atomic_submit_cb(NULL, false, io);
		return;
	}

	status = spdk_bdev_write_blocks_with_md(base->desc, io_ctx->ch,
						data_buf, io_ctx->md_buf,
						blk_addr, blk_len, vbdev_ocf_volume_atomic_submit_cb, io);
	if (status) {
		/* Since callback is not called, we need to do it manually to free io structures */
		SPDK_ERRLOG("Submission failed with status=%d\n", status);
		vbdev_ocf_volume_submit_io_cb(NULL, false, io);
	}
}

/*
 * This is specific function in OCF, OCF send 4k buffer for normal
 * data and expects that adapter will fill this buffer by metadata
 * for sectors according to address and length in this IO.
 */
static void
vbdev_ocf_volume_atomic_submit_metadata(struct ocf_io *io)
{
	struct vbdev_ocf_base *base = *((struct vbdev_ocf_base **)ocf_volume_get_priv(io->volume));
	struct ocf_io_ctx *io_ctx = ocf_get_io_ctx(io);
	int status = 0;
	uint64_t blk_addr, blk_len;
	void *data_buf;

	status = prepare_submit(io);
	if (status) {
		SPDK_ERRLOG("Preparing io failed with status=%d\n", status);
		vbdev_ocf_volume_submit_io_cb(NULL, false, io);
		return;
	}

	/* Calculate address and length on blocks */
	if (vbdev_ocf_volume_calculate_addr_len(base->bdev, io->addr, io->bytes,
						&blk_addr, &blk_len)) {
		vbdev_ocf_volume_submit_io_cb(NULL, false, io);
		return;
	}

	data_buf = env_zalloc(io->bytes, ENV_MEM_NOIO);
	if (!data_buf) {
		SPDK_ERRLOG("allocation for data failed\n");
		vbdev_ocf_volume_submit_io_cb(NULL, false, io);
		return;
	}

	/*
	 * Pass data as metadata and place NULL as data -
	 * ocf doesn't need data in this case
	 */
	status = spdk_bdev_read_blocks_with_md(base->desc, io_ctx->ch, data_buf,
					       io_ctx->data->iovs[0].iov_base,
					       blk_addr, blk_len, vbdev_ocf_volume_atomic_submit_cb, io);
	if (status) {
		/* Since callback is not called, we need to do it manually to free io structures */
		SPDK_ERRLOG("Submission failed with status=%d\n", status);
		vbdev_ocf_volume_submit_io_cb(NULL, false, io);
	}

}

static struct ocf_volume_properties vbdev_atomic_volume_props = {
	.name = "SPDK atomic block device",
	.io_priv_size = sizeof(struct ocf_io_ctx),
	.volume_priv_size = sizeof(struct vbdev_ocf_base *),
	.caps = {
		.atomic_writes = 1
	},
	.ops = {
		.open = vbdev_ocf_volume_open,
		.close = vbdev_ocf_volume_close,
		.get_length = vbdev_ocf_volume_get_length,
		.submit_io = vbdev_ocf_volume_atomic_submit_io,
		.submit_discard = vbdev_ocf_volume_submit_discard,
		.submit_write_zeroes = vbdev_ocf_volume_submit_write_zeroes,
		.submit_metadata = vbdev_ocf_volume_atomic_submit_metadata,
		.submit_flush = vbdev_ocf_volume_submit_flush,
		.get_max_io_size = vbdev_ocf_volume_get_max_io_size,
	},
	.io_ops = {
		.set_data = vbdev_ocf_volume_io_set_data,
		.get_data = vbdev_ocf_volume_io_get_data,
	},
};

int
vbdev_ocf_volume_init(void)
{
	int result;

	result = ocf_ctx_register_volume_type(vbdev_ocf_ctx, SPDK_OBJECT, &vbdev_volume_props);
	if (result) {
		return result;
	}

	return ocf_ctx_register_volume_type(vbdev_ocf_ctx, SPDK_OBJECT_ATOMIC, &vbdev_atomic_volume_props);
}

void
vbdev_ocf_volume_cleanup(void)
{
	ocf_ctx_unregister_volume_type(vbdev_ocf_ctx, SPDK_OBJECT);
	ocf_ctx_unregister_volume_type(vbdev_ocf_ctx, SPDK_OBJECT_ATOMIC);
}

SPDK_LOG_REGISTER_COMPONENT("vbdev_ocf_volume", SPDK_TRACE_VBDEV_OCF_VOLUME)
