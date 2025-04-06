/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   Copyright (C) 2025 Huawei Technologies
 *   All rights reserved.
 */

#include <ocf/ocf.h>

#include "spdk/bdev_module.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/log.h"

#include "vbdev_ocf_core.h"
#include "data.h"
#include "volume.h"
#include "ctx.h"

static int
vbdev_ocf_volume_open(ocf_volume_t volume, void *opts)
{
	struct vbdev_ocf_base **priv = ocf_volume_get_priv(volume);

	// refactor (like ocf_core_volume in ocf_core.c ?)

	assert(opts);

	*priv = opts;

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
get_starting_vec(struct iovec *iovs, int iovcnt, uint64_t *offset)
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

static unsigned int
vbdev_ocf_volume_get_max_io_size(ocf_volume_t volume)
{
	return 131072;
}

static void
vbdev_forward_io_cb(struct spdk_bdev_io *bdev_io, bool success, void *opaque)
{
	ocf_forward_token_t token = (ocf_forward_token_t) opaque;

	assert(token);

	spdk_bdev_free_io(bdev_io);

	ocf_forward_end(token, success ? 0 : -OCF_ERR_IO);
}

static void
vbdev_forward_io_free_iovs_cb(struct spdk_bdev_io *bdev_io, bool success, void *opaque)
{
	env_free(bdev_io->u.bdev.iovs);
	vbdev_forward_io_cb(bdev_io, success, opaque);
}

static struct spdk_io_channel *
vbdev_forward_get_channel(ocf_volume_t volume, ocf_forward_token_t token)
{
	struct vbdev_ocf_base *base =
		*((struct vbdev_ocf_base **)
		  ocf_volume_get_priv(volume));
	ocf_queue_t queue = ocf_forward_get_io_queue(token);
	struct vbdev_ocf_core_io_channel_ctx *ch_ctx;

	if (unlikely(ocf_queue_is_mngt(queue))) {
		return base->mngt_ch;
	}

	ch_ctx = ocf_queue_get_priv(queue);
	if (unlikely(ch_ctx == NULL)) {
		return NULL;
	}

	return (base->is_cache) ? ch_ctx->cache_ch : ch_ctx->core_ch;
}

static void
vbdev_forward_io(ocf_volume_t volume, ocf_forward_token_t token,
		 int dir, uint64_t addr, uint64_t bytes,
		 uint64_t offset)
{
	struct vbdev_ocf_base *base =
		*((struct vbdev_ocf_base **)
		  ocf_volume_get_priv(volume));
	struct vbdev_ocf_data *data = ocf_forward_get_data(token);
	struct spdk_io_channel *ch;
	spdk_bdev_io_completion_cb cb = vbdev_forward_io_cb;
	bool iovs_allocated = false;
	int iovcnt, skip, status = -1;
	struct iovec *iovs;

	ch = vbdev_forward_get_channel(volume, token);
	if (unlikely(ch == NULL)) {
		ocf_forward_end(token, -EFAULT);
		return;
	}

	if (bytes == data->size) {
		iovs = data->iovs;
		iovcnt = data->iovcnt;
	} else {
		skip = get_starting_vec(data->iovs, data->iovcnt, &offset);
		if (skip < 0) {
			SPDK_ERRLOG("Offset bigger than data size\n");
			ocf_forward_end(token, -OCF_ERR_IO);
			return;
		}

		iovcnt = data->iovcnt - skip;

		iovs_allocated = true;
		cb = vbdev_forward_io_free_iovs_cb;
		iovs = env_malloc(sizeof(*iovs) * iovcnt, ENV_MEM_NOIO);

		if (!iovs) {
			SPDK_ERRLOG("Allocation failed\n");
			ocf_forward_end(token, -OCF_ERR_NO_MEM);
			return;
		}

		initialize_cpy_vector(iovs, data->iovcnt, &data->iovs[skip],
				      iovcnt, offset, bytes);
	}

	if (dir == OCF_READ) {
		status = spdk_bdev_readv(base->desc, ch, iovs, iovcnt,
					 addr, bytes, cb, (void *) token);
	} else if (dir == OCF_WRITE) {
		status = spdk_bdev_writev(base->desc, ch, iovs, iovcnt,
					  addr, bytes, cb, (void *) token);
	}

	if (unlikely(status)) {
		SPDK_ERRLOG("Submission failed with status=%d\n", status);
		/* Since callback is not called, we need to do it manually to free iovs */
		if (iovs_allocated) {
			env_free(iovs);
		}
		ocf_forward_end(token, (status == -ENOMEM) ? -OCF_ERR_NO_MEM : -OCF_ERR_IO);
	}
}

static void
vbdev_forward_flush(ocf_volume_t volume, ocf_forward_token_t token)
{
	struct vbdev_ocf_base *base =
		*((struct vbdev_ocf_base **)
		  ocf_volume_get_priv(volume));
	struct spdk_io_channel *ch;
	uint64_t bytes = base->bdev->blockcnt * base->bdev->blocklen;
	int status;

	// workaround (?); check for better solution
	/* If base device doesn't support flush just warn about it and exit. */
	if (unlikely(!spdk_bdev_io_type_supported(base->bdev, SPDK_BDEV_IO_TYPE_FLUSH))) {
		SPDK_WARNLOG("Base bdev '%s': attempt to flush device that doesn't support it\n", base->bdev->name);
		ocf_forward_end(token, 0);
		return;
	}

	ch = vbdev_forward_get_channel(volume, token);
	if (unlikely(ch == NULL)) {
		ocf_forward_end(token, -EFAULT);
		return;
	}

	status = spdk_bdev_flush(
			 base->desc, ch, 0, bytes,
			 vbdev_forward_io_cb, (void *)token);
	if (unlikely(status)) {
		SPDK_ERRLOG("Submission failed with status=%d\n", status);
		ocf_forward_end(token, (status == -ENOMEM) ? -OCF_ERR_NO_MEM : -OCF_ERR_IO);
	}
}

static void
vbdev_forward_discard(ocf_volume_t volume, ocf_forward_token_t token,
		      uint64_t addr, uint64_t bytes)
{
	struct vbdev_ocf_base *base =
		*((struct vbdev_ocf_base **)
		  ocf_volume_get_priv(volume));
	struct spdk_io_channel *ch;
	int status = 0;

	ch = vbdev_forward_get_channel(volume, token);
	if (unlikely(ch == NULL)) {
		ocf_forward_end(token, -EFAULT);
		return;
	}

	status = spdk_bdev_unmap(
			 base->desc, ch, addr, bytes,
			 vbdev_forward_io_cb, (void *)token);
	if (unlikely(status)) {
		SPDK_ERRLOG("Submission failed with status=%d\n", status);
		ocf_forward_end(token, (status == -ENOMEM) ? -OCF_ERR_NO_MEM : -OCF_ERR_IO);
	}
}

struct vbdev_forward_io_simple_ctx {
	ocf_forward_token_t token;
	struct spdk_io_channel *ch;
};

static void
vbdev_forward_io_simple_cb(struct spdk_bdev_io *bdev_io, bool success, void *opaque)
{
	struct vbdev_forward_io_simple_ctx *ctx = opaque;
	ocf_forward_token_t token = ctx->token;

	assert(token);

	spdk_bdev_free_io(bdev_io);
	spdk_put_io_channel(ctx->ch);
	env_free(ctx);

	ocf_forward_end(token, success ? 0 : -OCF_ERR_IO);
}

static void
vbdev_forward_io_simple(ocf_volume_t volume, ocf_forward_token_t token,
			int dir, uint64_t addr, uint64_t bytes)
{
	struct vbdev_ocf_base *base =
		*((struct vbdev_ocf_base **)
		  ocf_volume_get_priv(volume));
	struct vbdev_ocf_data *data = ocf_forward_get_data(token);
	struct vbdev_forward_io_simple_ctx *ctx;
	int status = -1;

	ctx = env_malloc(sizeof(*ctx), ENV_MEM_NOIO);
	if (unlikely(!ctx)) {
		ocf_forward_end(token, -OCF_ERR_NO_MEM);
		return;
	}

	/* Forward IO simple is used in context where queue is not available
	 * so we have to get io channel ourselves */
	ctx->ch = spdk_bdev_get_io_channel(base->desc);
	if (unlikely(ctx->ch == NULL)) {
		env_free(ctx);
		ocf_forward_end(token, -EFAULT);
		return;
	}

	ctx->token = token;

	if (dir == OCF_READ) {
		status = spdk_bdev_readv(base->desc, ctx->ch, data->iovs,
					 data->iovcnt, addr, bytes,
					 vbdev_forward_io_simple_cb, ctx);
	} else if (dir == OCF_WRITE) {
		status = spdk_bdev_writev(base->desc, ctx->ch, data->iovs,
					  data->iovcnt, addr, bytes,
					  vbdev_forward_io_simple_cb, ctx);
	}

	if (unlikely(status)) {
		SPDK_ERRLOG("Submission failed with status=%d\n", status);
		spdk_put_io_channel(ctx->ch);
		env_free(ctx);
		ocf_forward_end(token, (status == -ENOMEM) ? -OCF_ERR_NO_MEM : -OCF_ERR_IO);
	}
}

static struct ocf_volume_properties vbdev_volume_props = {
	.name = "SPDK_block_device",
	.volume_priv_size = sizeof(struct vbdev_ocf_base *),
	.caps = {
		.atomic_writes = 0 /* to enable need to have ops->submit_metadata */
	},
	.ops = {
		.open = vbdev_ocf_volume_open,
		.close = vbdev_ocf_volume_close,
		.get_length = vbdev_ocf_volume_get_length,
		.get_max_io_size = vbdev_ocf_volume_get_max_io_size,
		.forward_io = vbdev_forward_io,
		.forward_flush = vbdev_forward_flush,
		.forward_discard = vbdev_forward_discard,
		.forward_io_simple = vbdev_forward_io_simple,
	},
};

static void
_base_detach(void *ctx)
{
	struct vbdev_ocf_base *base = ctx;

	spdk_put_io_channel(base->mngt_ch);
	spdk_bdev_close(base->desc);
}

void
vbdev_ocf_base_detach(struct vbdev_ocf_base *base)
{
	if (base->thread && base->thread != spdk_get_thread()) {
		spdk_thread_send_msg(base->thread, _base_detach, base);
	} else {
		_base_detach(base);
	}
	base->attached = false;
}

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
