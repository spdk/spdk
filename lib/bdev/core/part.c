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

/*
 * Common code for partition-like virtual bdevs.
 */

#include "spdk/bdev.h"
#include "spdk/log.h"
#include "spdk/string.h"

#include "spdk/bdev_module.h"

struct spdk_bdev_part_base {
	struct spdk_bdev		*bdev;
	struct spdk_bdev_desc		*desc;
	uint32_t			ref;
	uint32_t			channel_size;
	spdk_bdev_part_base_free_fn	base_free_fn;
	void				*ctx;
	bool				claimed;
	struct spdk_bdev_module		*module;
	struct spdk_bdev_fn_table	*fn_table;
	struct bdev_part_tailq		*tailq;
	spdk_io_channel_create_cb	ch_create_cb;
	spdk_io_channel_destroy_cb	ch_destroy_cb;
};

struct spdk_bdev *
spdk_bdev_part_base_get_bdev(struct spdk_bdev_part_base *part_base)
{
	return part_base->bdev;
}

struct spdk_bdev_desc *
spdk_bdev_part_base_get_desc(struct spdk_bdev_part_base *part_base)
{
	return part_base->desc;
}

struct bdev_part_tailq *
spdk_bdev_part_base_get_tailq(struct spdk_bdev_part_base *part_base)
{
	return part_base->tailq;
}

void *
spdk_bdev_part_base_get_ctx(struct spdk_bdev_part_base *part_base)
{
	return part_base->ctx;
}

void
spdk_bdev_part_base_free(struct spdk_bdev_part_base *base)
{
	if (base->desc) {
		spdk_bdev_close(base->desc);
		base->desc = NULL;
	}

	if (base->base_free_fn != NULL) {
		base->base_free_fn(base->ctx);
	}

	free(base);
}

static void
spdk_bdev_part_free_cb(void *io_device)
{
	struct spdk_bdev_part *part = io_device;
	struct spdk_bdev_part_base *base;

	assert(part);
	assert(part->internal.base);

	base = part->internal.base;

	TAILQ_REMOVE(base->tailq, part, tailq);

	if (__sync_sub_and_fetch(&base->ref, 1) == 0) {
		spdk_bdev_module_release_bdev(base->bdev);
		spdk_bdev_part_base_free(base);
	}

	spdk_bdev_destruct_done(&part->internal.bdev, 0);
	free(part->internal.bdev.name);
	free(part->internal.bdev.product_name);
	free(part);
}

int
spdk_bdev_part_free(struct spdk_bdev_part *part)
{
	spdk_io_device_unregister(part, spdk_bdev_part_free_cb);

	/* Return 1 to indicate that this is an asynchronous operation that isn't complete
	 * until spdk_bdev_destruct_done is called */
	return 1;
}

void
spdk_bdev_part_base_hotremove(struct spdk_bdev *base_bdev, struct bdev_part_tailq *tailq)
{
	struct spdk_bdev_part *part, *tmp;

	TAILQ_FOREACH_SAFE(part, tailq, tailq, tmp) {
		if (part->internal.base->bdev == base_bdev) {
			spdk_bdev_unregister(&part->internal.bdev, NULL, NULL);
		}
	}
}

static bool
spdk_bdev_part_io_type_supported(void *_part, enum spdk_bdev_io_type io_type)
{
	struct spdk_bdev_part *part = _part;

	return part->internal.base->bdev->fn_table->io_type_supported(part->internal.base->bdev->ctxt,
			io_type);
}

static struct spdk_io_channel *
spdk_bdev_part_get_io_channel(void *_part)
{
	struct spdk_bdev_part *part = _part;

	return spdk_get_io_channel(part);
}

struct spdk_bdev *
spdk_bdev_part_get_bdev(struct spdk_bdev_part *part)
{
	return &part->internal.bdev;
}

struct spdk_bdev_part_base *
spdk_bdev_part_get_base(struct spdk_bdev_part *part)
{
	return part->internal.base;
}

struct spdk_bdev *
spdk_bdev_part_get_base_bdev(struct spdk_bdev_part *part)
{
	return part->internal.base->bdev;
}

uint64_t
spdk_bdev_part_get_offset_blocks(struct spdk_bdev_part *part)
{
	return part->internal.offset_blocks;
}

static void
spdk_bdev_part_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *part_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	spdk_bdev_io_complete(part_io, status);
	spdk_bdev_free_io(bdev_io);
}

int
spdk_bdev_part_submit_request(struct spdk_bdev_part_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_part *part = ch->part;
	struct spdk_io_channel *base_ch = ch->base_ch;
	struct spdk_bdev_desc *base_desc = part->internal.base->desc;
	uint64_t offset;
	int rc = 0;

	/* Modify the I/O to adjust for the offset within the base bdev. */
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		offset = bdev_io->u.bdev.offset_blocks + part->internal.offset_blocks;
		rc = spdk_bdev_readv_blocks(base_desc, base_ch, bdev_io->u.bdev.iovs,
					    bdev_io->u.bdev.iovcnt, offset,
					    bdev_io->u.bdev.num_blocks, spdk_bdev_part_complete_io,
					    bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		offset = bdev_io->u.bdev.offset_blocks + part->internal.offset_blocks;
		rc = spdk_bdev_writev_blocks(base_desc, base_ch, bdev_io->u.bdev.iovs,
					     bdev_io->u.bdev.iovcnt, offset,
					     bdev_io->u.bdev.num_blocks, spdk_bdev_part_complete_io,
					     bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		offset = bdev_io->u.bdev.offset_blocks + part->internal.offset_blocks;
		rc = spdk_bdev_write_zeroes_blocks(base_desc, base_ch, offset, bdev_io->u.bdev.num_blocks,
						   spdk_bdev_part_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		offset = bdev_io->u.bdev.offset_blocks + part->internal.offset_blocks;
		rc = spdk_bdev_unmap_blocks(base_desc, base_ch, offset, bdev_io->u.bdev.num_blocks,
					    spdk_bdev_part_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		offset = bdev_io->u.bdev.offset_blocks + part->internal.offset_blocks;
		rc = spdk_bdev_flush_blocks(base_desc, base_ch, offset, bdev_io->u.bdev.num_blocks,
					    spdk_bdev_part_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		rc = spdk_bdev_reset(base_desc, base_ch,
				     spdk_bdev_part_complete_io, bdev_io);
		break;
	default:
		SPDK_ERRLOG("split: unknown I/O type %d\n", bdev_io->type);
		return SPDK_BDEV_IO_STATUS_FAILED;
	}

	return rc;
}

static int
spdk_bdev_part_channel_create_cb(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_part *part = (struct spdk_bdev_part *)io_device;
	struct spdk_bdev_part_channel *ch = ctx_buf;

	ch->part = part;
	ch->base_ch = spdk_bdev_get_io_channel(part->internal.base->desc);
	if (ch->base_ch == NULL) {
		return -1;
	}

	if (part->internal.base->ch_create_cb) {
		return part->internal.base->ch_create_cb(io_device, ctx_buf);
	} else {
		return 0;
	}
}

static void
spdk_bdev_part_channel_destroy_cb(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_part *part = (struct spdk_bdev_part *)io_device;
	struct spdk_bdev_part_channel *ch = ctx_buf;

	if (part->internal.base->ch_destroy_cb) {
		part->internal.base->ch_destroy_cb(io_device, ctx_buf);
	}
	spdk_put_io_channel(ch->base_ch);
}

struct spdk_bdev_part_base *
	spdk_bdev_part_base_construct(struct spdk_bdev *bdev,
			      spdk_bdev_remove_cb_t remove_cb, struct spdk_bdev_module *module,
			      struct spdk_bdev_fn_table *fn_table, struct bdev_part_tailq *tailq,
			      spdk_bdev_part_base_free_fn free_fn, void *ctx,
			      uint32_t channel_size, spdk_io_channel_create_cb ch_create_cb,
			      spdk_io_channel_destroy_cb ch_destroy_cb)
{
	int rc;
	struct spdk_bdev_part_base *base;

	base = calloc(1, sizeof(*base));
	if (!base) {
		SPDK_ERRLOG("Memory allocation failure\n");
		return NULL;
	}
	fn_table->get_io_channel = spdk_bdev_part_get_io_channel;
	fn_table->io_type_supported = spdk_bdev_part_io_type_supported;

	base->bdev = bdev;
	base->desc = NULL;
	base->ref = 0;
	base->module = module;
	base->fn_table = fn_table;
	base->tailq = tailq;
	base->base_free_fn = free_fn;
	base->ctx = ctx;
	base->claimed = false;
	base->channel_size = channel_size;
	base->ch_create_cb = ch_create_cb;
	base->ch_destroy_cb = ch_destroy_cb;

	rc = spdk_bdev_open(bdev, false, remove_cb, bdev, &base->desc);
	if (rc) {
		spdk_bdev_part_base_free(base);
		SPDK_ERRLOG("could not open bdev %s\n", spdk_bdev_get_name(bdev));
		return NULL;
	}

	return base;
}

int
spdk_bdev_part_construct(struct spdk_bdev_part *part, struct spdk_bdev_part_base *base,
			 char *name, uint64_t offset_blocks, uint64_t num_blocks,
			 char *product_name)
{
	part->internal.bdev.blocklen = base->bdev->blocklen;
	part->internal.bdev.blockcnt = num_blocks;
	part->internal.offset_blocks = offset_blocks;

	part->internal.bdev.write_cache = base->bdev->write_cache;
	part->internal.bdev.need_aligned_buffer = base->bdev->need_aligned_buffer;
	part->internal.bdev.ctxt = part;
	part->internal.bdev.module = base->module;
	part->internal.bdev.fn_table = base->fn_table;

	part->internal.bdev.name = strdup(name);
	part->internal.bdev.product_name = strdup(product_name);

	if (part->internal.bdev.name == NULL) {
		SPDK_ERRLOG("Failed to allocate name for new part of bdev %s\n", spdk_bdev_get_name(base->bdev));
		return -1;
	} else if (part->internal.bdev.product_name == NULL) {
		free(part->internal.bdev.name);
		SPDK_ERRLOG("Failed to allocate product name for new part of bdev %s\n",
			    spdk_bdev_get_name(base->bdev));
		return -1;
	}

	__sync_fetch_and_add(&base->ref, 1);
	part->internal.base = base;

	if (!base->claimed) {
		int rc;

		rc = spdk_bdev_module_claim_bdev(base->bdev, base->desc, base->module);
		if (rc) {
			SPDK_ERRLOG("could not claim bdev %s\n", spdk_bdev_get_name(base->bdev));
			free(part->internal.bdev.name);
			free(part->internal.bdev.product_name);
			return -1;
		}
		base->claimed = true;
	}

	spdk_io_device_register(part, spdk_bdev_part_channel_create_cb,
				spdk_bdev_part_channel_destroy_cb,
				base->channel_size,
				name);

	spdk_vbdev_register(&part->internal.bdev, &base->bdev, 1);
	TAILQ_INSERT_TAIL(base->tailq, part, tailq);

	return 0;
}
