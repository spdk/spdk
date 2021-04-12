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
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/thread.h"

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
	spdk_bdev_remove_cb_t		remove_cb;
	struct spdk_thread		*thread;
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

const char *
spdk_bdev_part_base_get_bdev_name(struct spdk_bdev_part_base *part_base)
{
	return part_base->bdev->name;
}

static void
bdev_part_base_free(void *ctx)
{
	struct spdk_bdev_desc *desc = ctx;

	spdk_bdev_close(desc);
}

void
spdk_bdev_part_base_free(struct spdk_bdev_part_base *base)
{
	if (base->desc) {
		/* Close the underlying bdev on its same opened thread. */
		if (base->thread && base->thread != spdk_get_thread()) {
			spdk_thread_send_msg(base->thread, bdev_part_base_free, base->desc);
		} else {
			spdk_bdev_close(base->desc);
		}
	}

	if (base->base_free_fn != NULL) {
		base->base_free_fn(base->ctx);
	}

	free(base);
}

static void
bdev_part_free_cb(void *io_device)
{
	struct spdk_bdev_part *part = io_device;
	struct spdk_bdev_part_base *base;

	assert(part);
	assert(part->internal.base);

	base = part->internal.base;

	TAILQ_REMOVE(base->tailq, part, tailq);

	if (--base->ref == 0) {
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
	spdk_io_device_unregister(part, bdev_part_free_cb);

	/* Return 1 to indicate that this is an asynchronous operation that isn't complete
	 * until spdk_bdev_destruct_done is called */
	return 1;
}

void
spdk_bdev_part_base_hotremove(struct spdk_bdev_part_base *part_base, struct bdev_part_tailq *tailq)
{
	struct spdk_bdev_part *part, *tmp;

	TAILQ_FOREACH_SAFE(part, tailq, tailq, tmp) {
		if (part->internal.base == part_base) {
			spdk_bdev_unregister(&part->internal.bdev, NULL, NULL);
		}
	}
}

static bool
bdev_part_io_type_supported(void *_part, enum spdk_bdev_io_type io_type)
{
	struct spdk_bdev_part *part = _part;

	/* We can't decode/modify passthrough NVMe commands, so don't report
	 *  that a partition supports these io types, even if the underlying
	 *  bdev does.
	 */
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_NVME_ADMIN:
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		return false;
	default:
		break;
	}

	return part->internal.base->bdev->fn_table->io_type_supported(part->internal.base->bdev->ctxt,
			io_type);
}

static struct spdk_io_channel *
bdev_part_get_io_channel(void *_part)
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

static int
bdev_part_remap_dif(struct spdk_bdev_io *bdev_io, uint32_t offset,
		    uint32_t remapped_offset)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct spdk_dif_ctx dif_ctx;
	struct spdk_dif_error err_blk = {};
	int rc;

	if (spdk_likely(!(bdev->dif_check_flags & SPDK_DIF_FLAGS_REFTAG_CHECK))) {
		return 0;
	}

	rc = spdk_dif_ctx_init(&dif_ctx,
			       bdev->blocklen, bdev->md_len, bdev->md_interleave,
			       bdev->dif_is_head_of_md, bdev->dif_type, bdev->dif_check_flags,
			       offset, 0, 0, 0, 0);
	if (rc != 0) {
		SPDK_ERRLOG("Initialization of DIF context failed\n");
		return rc;
	}

	spdk_dif_ctx_set_remapped_init_ref_tag(&dif_ctx, remapped_offset);

	if (bdev->md_interleave) {
		rc = spdk_dif_remap_ref_tag(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					    bdev_io->u.bdev.num_blocks, &dif_ctx, &err_blk);
	} else {
		struct iovec md_iov = {
			.iov_base	= bdev_io->u.bdev.md_buf,
			.iov_len	= bdev_io->u.bdev.num_blocks * bdev->md_len,
		};

		rc = spdk_dix_remap_ref_tag(&md_iov, bdev_io->u.bdev.num_blocks, &dif_ctx, &err_blk);
	}

	if (rc != 0) {
		SPDK_ERRLOG("Remapping reference tag failed. type=%d, offset=%" PRIu32 "\n",
			    err_blk.err_type, err_blk.err_offset);
	}

	return rc;
}

static void
bdev_part_complete_read_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *part_io = cb_arg;
	uint32_t offset, remapped_offset;
	int rc, status;

	offset = bdev_io->u.bdev.offset_blocks;
	remapped_offset = part_io->u.bdev.offset_blocks;

	if (success) {
		rc = bdev_part_remap_dif(bdev_io, offset, remapped_offset);
		if (rc != 0) {
			success = false;
		}
	}

	status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	spdk_bdev_io_complete(part_io, status);
	spdk_bdev_free_io(bdev_io);
}

static void
bdev_part_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *part_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	spdk_bdev_io_complete(part_io, status);
	spdk_bdev_free_io(bdev_io);
}

static void
bdev_part_complete_zcopy_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *part_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	spdk_bdev_io_set_buf(part_io, bdev_io->u.bdev.iovs[0].iov_base, bdev_io->u.bdev.iovs[0].iov_len);
	spdk_bdev_io_complete(part_io, status);
	spdk_bdev_free_io(bdev_io);
}

int
spdk_bdev_part_submit_request(struct spdk_bdev_part_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev_part *part = ch->part;
	struct spdk_io_channel *base_ch = ch->base_ch;
	struct spdk_bdev_desc *base_desc = part->internal.base->desc;
	uint64_t offset, remapped_offset;
	int rc = 0;

	offset = bdev_io->u.bdev.offset_blocks;
	remapped_offset = offset + part->internal.offset_blocks;

	/* Modify the I/O to adjust for the offset within the base bdev. */
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		if (bdev_io->u.bdev.md_buf == NULL) {
			rc = spdk_bdev_readv_blocks(base_desc, base_ch, bdev_io->u.bdev.iovs,
						    bdev_io->u.bdev.iovcnt, remapped_offset,
						    bdev_io->u.bdev.num_blocks,
						    bdev_part_complete_read_io, bdev_io);
		} else {
			rc = spdk_bdev_readv_blocks_with_md(base_desc, base_ch,
							    bdev_io->u.bdev.iovs,
							    bdev_io->u.bdev.iovcnt,
							    bdev_io->u.bdev.md_buf, remapped_offset,
							    bdev_io->u.bdev.num_blocks,
							    bdev_part_complete_read_io, bdev_io);
		}
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = bdev_part_remap_dif(bdev_io, offset, remapped_offset);
		if (rc != 0) {
			return SPDK_BDEV_IO_STATUS_FAILED;
		}

		if (bdev_io->u.bdev.md_buf == NULL) {
			rc = spdk_bdev_writev_blocks(base_desc, base_ch, bdev_io->u.bdev.iovs,
						     bdev_io->u.bdev.iovcnt, remapped_offset,
						     bdev_io->u.bdev.num_blocks,
						     bdev_part_complete_io, bdev_io);
		} else {
			rc = spdk_bdev_writev_blocks_with_md(base_desc, base_ch,
							     bdev_io->u.bdev.iovs,
							     bdev_io->u.bdev.iovcnt,
							     bdev_io->u.bdev.md_buf, remapped_offset,
							     bdev_io->u.bdev.num_blocks,
							     bdev_part_complete_io, bdev_io);
		}
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		rc = spdk_bdev_write_zeroes_blocks(base_desc, base_ch, remapped_offset,
						   bdev_io->u.bdev.num_blocks, bdev_part_complete_io,
						   bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = spdk_bdev_unmap_blocks(base_desc, base_ch, remapped_offset,
					    bdev_io->u.bdev.num_blocks, bdev_part_complete_io,
					    bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush_blocks(base_desc, base_ch, remapped_offset,
					    bdev_io->u.bdev.num_blocks, bdev_part_complete_io,
					    bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		rc = spdk_bdev_reset(base_desc, base_ch,
				     bdev_part_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		rc = spdk_bdev_zcopy_start(base_desc, base_ch, NULL, 0, remapped_offset,
					   bdev_io->u.bdev.num_blocks, bdev_io->u.bdev.zcopy.populate,
					   bdev_part_complete_zcopy_io, bdev_io);
		break;
	default:
		SPDK_ERRLOG("unknown I/O type %d\n", bdev_io->type);
		return SPDK_BDEV_IO_STATUS_FAILED;
	}

	return rc;
}

static int
bdev_part_channel_create_cb(void *io_device, void *ctx_buf)
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
bdev_part_channel_destroy_cb(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_part *part = (struct spdk_bdev_part *)io_device;
	struct spdk_bdev_part_channel *ch = ctx_buf;

	if (part->internal.base->ch_destroy_cb) {
		part->internal.base->ch_destroy_cb(io_device, ctx_buf);
	}
	spdk_put_io_channel(ch->base_ch);
}

static void
bdev_part_base_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			void *event_ctx)
{
	struct spdk_bdev_part_base *base = event_ctx;

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		base->remove_cb(base);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

int
spdk_bdev_part_base_construct_ext(const char *bdev_name,
				  spdk_bdev_remove_cb_t remove_cb, struct spdk_bdev_module *module,
				  struct spdk_bdev_fn_table *fn_table, struct bdev_part_tailq *tailq,
				  spdk_bdev_part_base_free_fn free_fn, void *ctx,
				  uint32_t channel_size, spdk_io_channel_create_cb ch_create_cb,
				  spdk_io_channel_destroy_cb ch_destroy_cb,
				  struct spdk_bdev_part_base **_base)
{
	int rc;
	struct spdk_bdev_part_base *base;

	if (_base == NULL) {
		return -EINVAL;
	}

	base = calloc(1, sizeof(*base));
	if (!base) {
		SPDK_ERRLOG("Memory allocation failure\n");
		return -ENOMEM;
	}
	fn_table->get_io_channel = bdev_part_get_io_channel;
	fn_table->io_type_supported = bdev_part_io_type_supported;

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
	base->remove_cb = remove_cb;

	rc = spdk_bdev_open_ext(bdev_name, false, bdev_part_base_event_cb, base, &base->desc);
	if (rc) {
		if (rc == -ENODEV) {
			free(base);
		} else {
			SPDK_ERRLOG("could not open bdev %s: %s\n", bdev_name, spdk_strerror(-rc));
			spdk_bdev_part_base_free(base);
		}
		return rc;
	}

	base->bdev = spdk_bdev_desc_get_bdev(base->desc);

	/* Save the thread where the base device is opened */
	base->thread = spdk_get_thread();

	*_base = base;

	return 0;
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
	part->internal.bdev.required_alignment = base->bdev->required_alignment;
	part->internal.bdev.ctxt = part;
	part->internal.bdev.module = base->module;
	part->internal.bdev.fn_table = base->fn_table;

	part->internal.bdev.md_interleave = base->bdev->md_interleave;
	part->internal.bdev.md_len = base->bdev->md_len;
	part->internal.bdev.dif_type = base->bdev->dif_type;
	part->internal.bdev.dif_is_head_of_md = base->bdev->dif_is_head_of_md;
	part->internal.bdev.dif_check_flags = base->bdev->dif_check_flags;

	part->internal.bdev.name = strdup(name);
	if (part->internal.bdev.name == NULL) {
		SPDK_ERRLOG("Failed to allocate name for new part of bdev %s\n", spdk_bdev_get_name(base->bdev));
		return -1;
	}

	part->internal.bdev.product_name = strdup(product_name);
	if (part->internal.bdev.product_name == NULL) {
		free(part->internal.bdev.name);
		SPDK_ERRLOG("Failed to allocate product name for new part of bdev %s\n",
			    spdk_bdev_get_name(base->bdev));
		return -1;
	}

	base->ref++;
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

	spdk_io_device_register(part, bdev_part_channel_create_cb,
				bdev_part_channel_destroy_cb,
				base->channel_size,
				name);

	spdk_bdev_register(&part->internal.bdev);
	TAILQ_INSERT_TAIL(base->tailq, part, tailq);

	return 0;
}
