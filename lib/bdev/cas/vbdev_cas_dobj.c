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

static env_allocator *opencas_dobj_io_allocator;

static int
opencas_dobj_open(ocf_data_obj_t obj)
{
	return 0;
}

static void
opencas_dobj_close(ocf_data_obj_t obj)
{
}

static uint64_t
opencas_dobj_get_length(ocf_data_obj_t obj)
{
	struct vbdev_cas_base *base = *(void **)ocf_data_obj_get_uuid(obj)->data;
	uint64_t len;

	len = base->bdev->blocklen * base->bdev->blockcnt;

	return len;
}

static int
opencas_dobj_io_set_data(struct ocf_io *io, ctx_data_t *data,
			 uint32_t offset)
{
	struct ocf_io_container *io_ctnr = ocf_io_to_bdev_io(io);

	io_ctnr->offset = offset;
	io_ctnr->data = data;

	if (io_ctnr->data && offset >= io_ctnr->data->size) {
		return -ENOBUFS;
	}

	return 0;
}

static ctx_data_t *
opencas_dobj_io_get_data(struct ocf_io *io)
{
	return ocf_io_to_bdev_io(io)->data;
}

static void
opencas_dobj_io_get(struct ocf_io *io)
{
	struct ocf_io_container *io_ctnr = ocf_io_to_bdev_io(io);

	atomic_inc(&io_ctnr->ref);
}

static void
opencas_dobj_io_put(struct ocf_io *io)
{
	struct ocf_io_container *io_ctnr = ocf_io_to_bdev_io(io);
	int value;

	value = env_atomic_sub_return(1, &io_ctnr->ref);
	if (value) {
		return;
	}

	env_allocator_del(opencas_dobj_io_allocator, io_ctnr);
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
	struct ocf_io_container *io;

	io = env_allocator_new(opencas_dobj_io_allocator);
	if (io == NULL) {
		return NULL;
	}

	atomic_set(&io->rq_cnt, 0);
	atomic_set(&io->ref, 1);
	io->error = 0;
	io->base.obj = obj;
	io->base.ops = &opencas_dobj_io_ops;

	return &io->base;
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
	struct ocf_io_container *io_ctnr;

	assert(opaque);

	io = opaque;
	io_ctnr = ocf_io_to_bdev_io(io);

	assert(io_ctnr);

	if (!success) {
		io_ctnr->error |= 1;
	}

	if (env_atomic_sub_return(1, &io_ctnr->rq_cnt)) {
		return;
	}

	if (io_ctnr->offset) {
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
	if (io_ctnr->error) {
		SPDK_ERRLOG("ERROR: %d\n", io_ctnr->error);
	}

	io_ctnr->base.end(&io_ctnr->base, io_ctnr->error);

	/* TODO [multichannel0]: need to remove this work-around defer to dedicated thread */
	spdk_put_io_channel(io_ctnr->ch); /* decreasing reference, because we created this channel */
	opencas_dobj_io_put(io);

	if (bdev_io) {
		spdk_bdev_free_io(bdev_io);
	}
}

static void
spdk_get_ref_io_channel(struct spdk_io_channel *ch)
{
	ch->ref++;
}

static void
prepare_submit(struct ocf_io *io)
{
	struct ocf_io_container *io_ctnr = ocf_io_to_bdev_io(io);
	if (atomic_read(&io_ctnr->rq_cnt) == 0) {
		/* increase the ref counts */
		atomic_inc(&io_ctnr->rq_cnt);
		opencas_dobj_io_get(io);

		/* TODO [multichannel0]: need to remove this work-around defer to dedicated thread */
		struct vbdev_cas_base *base = *(void **)ocf_data_obj_get_uuid(io->obj)->data;
		io_ctnr->ch = base->base_channel;
		spdk_get_ref_io_channel(io_ctnr->ch);
	}
}

static void
opencas_dobj_submit_flush(struct ocf_io *io)
{
}

static void
opencas_dobj_submit_io(struct ocf_io *io)
{
	struct vbdev_cas_base *base = *(void **)ocf_data_obj_get_uuid(io->obj)->data;
	struct ocf_io_container *io_ctnr = ocf_io_to_bdev_io(io);
	struct iovec *iovs;
	int iovcnt, status = 0, i, offset;
	uint64_t addr, len;

	if (io_ctnr->base.flags == OCF_WRITE_FLUSH) {
		opencas_dobj_submit_flush(io);
		return;
	}

	prepare_submit(io);

	/* IO fields */
	addr = io->addr;
	len = io->bytes;
	offset = io_ctnr->offset;

	if (offset) {
		i = get_starting_vec(io_ctnr->data->iovs, io_ctnr->data->iovcnt, &offset);

		if (i < 0) {
			SPDK_ERRLOG("offset bigger than data size");
			opencas_dobj_submit_io_cb(NULL, false, io);
			return;
		}

		iovcnt = io_ctnr->data->iovcnt - i;

		iovs = env_malloc(sizeof(*iovs) * iovcnt, ENV_MEM_NOIO);

		if (!iovs) {
			SPDK_ERRLOG("allocation failed");
			opencas_dobj_submit_io_cb(NULL, false, io);
			return;
		}

		initialize_cpy_vector(iovs, io_ctnr->data->iovcnt, &io_ctnr->data->iovs[i],
				      io_ctnr->data->iovcnt - i, offset, len);
	} else {
		iovs = io_ctnr->data->iovs;
		iovcnt = io_ctnr->data->iovcnt;
	}

	if (io->dir == OCF_READ) {
		status = spdk_bdev_readv(base->desc, io_ctnr->ch,
					 iovs, iovcnt, addr, len, opencas_dobj_submit_io_cb, io);
	} else if (io->dir == OCF_WRITE) {
		status = spdk_bdev_writev(base->desc, io_ctnr->ch,
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
	.io_context_size = 0,
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
	opencas_dobj_io_allocator = env_allocator_create(sizeof(struct ocf_io_container),
				    "opencas_spdk_io");
	return ocf_ctx_register_data_obj_type(opencas_ctx, SPDK_OBJECT, &opencas_dobj_props);
}

void
opencas_dobj_cleanup(void)
{
	env_allocator_destroy(opencas_dobj_io_allocator);
	opencas_dobj_io_allocator = NULL;
}

SPDK_LOG_REGISTER_COMPONENT("vbdev_cas_dobj", SPDK_TRACE_VBDEV_CACHE_DOBJ)
