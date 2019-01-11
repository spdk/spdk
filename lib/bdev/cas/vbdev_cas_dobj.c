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
#include "dobj.h"
#include "ctx.h"
#include "vbdev_cas.h"

static int
opencas_dobj_open(ocf_data_obj_t obj)
{
	struct vbdev_cas_base *base = vbdev_cas_get_base_by_name(ocf_data_obj_get_uuid(obj)->data);

	if (base == NULL) {
		assert(false);
		return -EINVAL;
	}

	ocf_data_obj_set_priv(obj, base);

	return 0;
}

static void
opencas_dobj_close(ocf_data_obj_t obj)
{
}

static uint64_t
opencas_dobj_get_length(ocf_data_obj_t obj)
{
	struct vbdev_cas_base *base = ocf_data_obj_get_priv(obj);
	uint64_t len;

	len = base->bdev->blocklen * base->bdev->blockcnt;

	return len;
}

static int
opencas_dobj_io_set_data(struct ocf_io *io, ctx_data_t *data,
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
opencas_dobj_io_get_data(struct ocf_io *io)
{
	return ocf_get_io_ctx(io)->data;
}

static void
opencas_dobj_io_get(struct ocf_io *io)
{
	struct ocf_io_ctx *io_ctx = ocf_get_io_ctx(io);

	atomic_inc(&io_ctx->ref);
}

static void
opencas_dobj_io_put(struct ocf_io *io)
{
	struct ocf_io_ctx *io_ctx = ocf_get_io_ctx(io);
	int value;

	value = env_atomic_sub_return(1, &io_ctx->ref);
	if (value) {
		return;
	}

	ocf_data_obj_del_io(io);
}

static const struct ocf_io_ops opencas_dobj_io_ops = {
	.set_data = opencas_dobj_io_set_data,
	.get_data = opencas_dobj_io_get_data,
	.get = opencas_dobj_io_get,
	.put = opencas_dobj_io_put,
};

static struct ocf_io *
opencas_dobj_new_io(ocf_data_obj_t obj)
{
	struct ocf_io *io;
	struct ocf_io_ctx *io_ctx;

	io = ocf_data_obj_new_io(obj);
	if (io == NULL) {
		return NULL;
	}

	io->ops = &opencas_dobj_io_ops;

	io_ctx = ocf_get_io_ctx(io);
	atomic_set(&io_ctx->rq_cnt, 0);
	atomic_set(&io_ctx->ref, 1);
	io_ctx->error = 0;

	return io;
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
opencas_dobj_submit_io_cb(struct spdk_bdev_io *bdev_io, bool success, void *opaque)
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

	if (env_atomic_sub_return(1, &io_ctx->rq_cnt)) {
		return;
	}

	if (io_ctx->offset) {
		switch (bdev_io->type) {
		case SPDK_BDEV_IO_TYPE_READ:
		case SPDK_BDEV_IO_TYPE_WRITE:
			env_free(bdev_io->u.bdev.iovs);
			break;
		default:
			assert(false);
			break;
		}
	}
	if (io_ctx->error) {
		SPDK_ERRLOG("ERROR: %d\n", io_ctx->error);
	}

	io->end(io, io_ctx->error);

	opencas_dobj_io_put(io);
	if (bdev_io) {
		spdk_bdev_free_io(bdev_io);
	}
}

static int
prepare_submit(struct ocf_io *io)
{
	struct ocf_io_ctx *io_ctx = ocf_get_io_ctx(io);
	struct vbdev_cas_qcxt *qctx;
	struct vbdev_cas_base *base;
	ocf_queue_t q;
	int rc = 0;

	if (atomic_read(&io_ctx->rq_cnt) == 0) {
		/* increase the ref counts */
		atomic_inc(&io_ctx->rq_cnt);
		opencas_dobj_io_get(io);

		base = ocf_data_obj_get_priv(io->obj);

		if (io->io_queue == 0) {
			/* TODO: handle 0 queue id
			 * In current implementation it is not used
			 * but if we are going to use ocf_purge then queue id will be 0 */
			assert(false);
			return -EINVAL;
		}

		rc = ocf_cache_get_queue(base->parent->ocf_cache, io->io_queue, &q);
		if (rc) {
			SPDK_ERRLOG("Could not get queue #%d\n", io->io_queue);
			assert(false);
			return rc;
		}

		qctx = ocf_queue_get_priv(q);
		if (base->is_cache) {
			io_ctx->ch = qctx->cache_ch;
		} else {
			io_ctx->ch = qctx->core_ch;
		}
	}
	return rc;
}

static void
opencas_dobj_submit_flush(struct ocf_io *io)
{
}

static void
opencas_dobj_submit_io(struct ocf_io *io)
{
	struct vbdev_cas_base *base = ocf_data_obj_get_priv(io->obj);
	struct ocf_io_ctx *io_ctx = ocf_get_io_ctx(io);
	struct iovec *iovs;
	int iovcnt, status = 0, i, offset;
	uint64_t addr, len;

	if (io->flags == OCF_WRITE_FLUSH) {
		opencas_dobj_submit_flush(io);
		return;
	}

	prepare_submit(io);

	/* IO fields */
	addr = io->addr;
	len = io->bytes;
	offset = io_ctx->offset;

	if (offset) {
		i = get_starting_vec(io_ctx->data->iovs, io_ctx->data->iovcnt, &offset);

		if (i < 0) {
			SPDK_ERRLOG("offset bigger than data size");
			opencas_dobj_submit_io_cb(NULL, false, io);
			return;
		}

		iovcnt = io_ctx->data->iovcnt - i;

		iovs = env_malloc(sizeof(*iovs) * iovcnt, ENV_MEM_NOIO);

		if (!iovs) {
			SPDK_ERRLOG("allocation failed");
			opencas_dobj_submit_io_cb(NULL, false, io);
			return;
		}

		initialize_cpy_vector(iovs, io_ctx->data->iovcnt, &io_ctx->data->iovs[i],
				      io_ctx->data->iovcnt - i, offset, len);
	} else {
		iovs = io_ctx->data->iovs;
		iovcnt = io_ctx->data->iovcnt;
	}

	if (io->dir == OCF_READ) {
		status = spdk_bdev_readv(base->desc, io_ctx->ch,
					 iovs, iovcnt, addr, len, opencas_dobj_submit_io_cb, io);
	} else if (io->dir == OCF_WRITE) {
		status = spdk_bdev_writev(base->desc, io_ctx->ch,
					  iovs, iovcnt, addr, len, opencas_dobj_submit_io_cb, io);
	}

	if (status) {
		/* Since callback is not called, we need to do it manually to free io structures */
		SPDK_ERRLOG("submission failed with status=%d", status);
		opencas_dobj_submit_io_cb(NULL, false, io);
	}
}

static void
opencas_dobj_submit_discard(struct ocf_io *io)
{
	/* TODO [unmap support] */
	io->end(io, 0);
}

static void
opencas_dobj_submit_metadata(struct ocf_io *io)
{
	/* Implement with persistent metadata support */
}

static unsigned int
opencas_dobj_get_max_io_size(ocf_data_obj_t obj)
{
	return 256;
}

static struct ocf_data_obj_properties opencas_dobj_props = {
	.name = "SPDK block device",
	.io_context_size = sizeof(struct ocf_io_ctx),
	.caps = {
		.atomic_writes = 0 /* to enable need to have ops->submit_metadata */
	},
	.ops = {
		.new_io = opencas_dobj_new_io,
		.open = opencas_dobj_open,
		.close = opencas_dobj_close,
		.get_length = opencas_dobj_get_length,
		.submit_io = opencas_dobj_submit_io,
		.submit_discard = opencas_dobj_submit_discard,
		.submit_flush = opencas_dobj_submit_flush,
		.get_max_io_size = opencas_dobj_get_max_io_size,
		.submit_metadata = opencas_dobj_submit_metadata,
	}
};

int
opencas_dobj_init(void)
{
	return ocf_ctx_register_data_obj_type(opencas_ctx, SPDK_OBJECT, &opencas_dobj_props);
}

void
opencas_dobj_cleanup(void)
{
	ocf_ctx_unregister_data_obj_type(opencas_ctx, SPDK_OBJECT);
}

SPDK_LOG_REGISTER_COMPONENT("vbdev_cas_dobj", SPDK_TRACE_VBDEV_CACHE_DOBJ)
