/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#include "ftl_nvc_dev.h"
#include "ftl_core.h"
#include "ftl_layout.h"
#include "utils/ftl_layout_tracker_bdev.h"
#include "mngt/ftl_mngt.h"
#include "ftl_nvc_bdev_common.h"

static bool
is_bdev_compatible(struct spdk_ftl_dev *dev, struct spdk_bdev *bdev)
{
	if (!spdk_bdev_is_md_separate(bdev)) {
		/* It doesn't support separate metadata buffer IO */
		return false;
	}

	if (spdk_bdev_get_md_size(bdev) != sizeof(union ftl_md_vss)) {
		/* Bdev's metadata is invalid size */
		return false;
	}

	if (spdk_bdev_get_dif_type(bdev) != SPDK_DIF_DISABLE) {
		/* Unsupported DIF type used by bdev */
		return false;
	}

	if (ftl_md_xfer_blocks(dev) * spdk_bdev_get_md_size(bdev) > FTL_ZERO_BUFFER_SIZE) {
		FTL_ERRLOG(dev, "Zero buffer too small for bdev %s metadata transfer\n",
			   spdk_bdev_get_name(bdev));
		return false;
	}

	return true;
}

static void
write_io_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_io *io = cb_arg;
	struct ftl_nv_cache *nv_cache = &io->dev->nv_cache;

	ftl_stats_bdev_io_completed(io->dev, FTL_STATS_TYPE_USER, bdev_io);

	spdk_bdev_free_io(bdev_io);

	ftl_mempool_put(nv_cache->md_pool, io->md);

	ftl_nv_cache_write_complete(io, success);
}

static void write_io(struct ftl_io *io);

static void
_nvc_vss_write(void *io)
{
	write_io(io);
}

static void
write_io(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;
	int rc;

	io->md = ftl_mempool_get(dev->nv_cache.md_pool);
	if (spdk_unlikely(!io->md)) {
		ftl_abort();
	}

	ftl_nv_cache_fill_md(io);

	rc = spdk_bdev_writev_blocks_with_md(nv_cache->bdev_desc, nv_cache->cache_ioch,
					     io->iov, io->iov_cnt, io->md,
					     ftl_addr_to_nvc_offset(dev, io->addr), io->num_blocks,
					     write_io_cb, io);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			struct spdk_bdev *bdev;

			ftl_mempool_put(nv_cache->md_pool, io->md);
			io->md = NULL;

			bdev = spdk_bdev_desc_get_bdev(nv_cache->bdev_desc);
			io->bdev_io_wait.bdev = bdev;
			io->bdev_io_wait.cb_fn = _nvc_vss_write;
			io->bdev_io_wait.cb_arg = io;
			spdk_bdev_queue_io_wait(bdev, nv_cache->cache_ioch, &io->bdev_io_wait);
		} else {
			ftl_abort();
		}
	}
}

struct nvc_recover_open_chunk_ctx {
	struct ftl_nv_cache_chunk *chunk;
	struct ftl_rq *rq;
	uint64_t addr;
	uint64_t to_read;
};

static void
nvc_recover_open_chunk_read_vss_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_mngt_process *mngt = cb_arg;
	struct spdk_ftl_dev *dev = ftl_mngt_get_dev(mngt);
	struct nvc_recover_open_chunk_ctx *ctx = ftl_mngt_get_process_ctx(mngt);
	struct ftl_nv_cache_chunk *chunk = ctx->chunk;
	struct ftl_rq *rq = ctx->rq;
	union ftl_md_vss *md;
	uint64_t cache_offset = bdev_io->u.bdev.offset_blocks;
	uint64_t blocks = bdev_io->u.bdev.num_blocks;
	ftl_addr addr = ftl_addr_from_nvc_offset(dev, cache_offset);

	spdk_bdev_free_io(bdev_io);
	if (!success) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	/* Rebuild P2L map */
	for (rq->iter.idx = 0; rq->iter.idx < blocks; rq->iter.idx++) {
		md = rq->entries[rq->iter.idx].io_md;
		if (md->nv_cache.seq_id != chunk->md->seq_id) {
			md->nv_cache.lba = FTL_LBA_INVALID;
			md->nv_cache.seq_id = 0;
		}

		ftl_nv_cache_chunk_set_addr(chunk, md->nv_cache.lba, addr + rq->iter.idx);
	}

	assert(ctx->to_read >= blocks);
	ctx->addr += blocks;
	ctx->to_read -= blocks;
	ftl_mngt_continue_step(mngt);

}

static void
nvc_recover_open_chunk_read_vss(struct spdk_ftl_dev *dev,
				struct ftl_mngt_process *mngt)
{
	struct nvc_recover_open_chunk_ctx *ctx = ftl_mngt_get_process_ctx(mngt);
	uint64_t blocks = spdk_min(ctx->rq->num_blocks, ctx->to_read);
	int rc;

	if (blocks) {
		rc = spdk_bdev_read_blocks_with_md(dev->nv_cache.bdev_desc, dev->nv_cache.cache_ioch,
						   ctx->rq->io_payload, ctx->rq->io_md, ctx->addr, blocks,
						   nvc_recover_open_chunk_read_vss_cb, mngt);
		if (rc) {
			ftl_mngt_fail_step(mngt);
			return;
		}
	} else {
		ftl_mngt_next_step(mngt);
	}
}

static int
nvc_recover_open_chunk_init_handler(struct spdk_ftl_dev *dev,
				    struct ftl_mngt_process *mngt, void *init_ctx)
{
	struct nvc_recover_open_chunk_ctx *ctx = ftl_mngt_get_process_ctx(mngt);

	ctx->chunk = init_ctx;
	ctx->rq = ftl_rq_new(dev, dev->nv_cache.md_size);
	if (NULL == ctx->rq) {
		return -ENOMEM;
	}

	ctx->addr = ctx->chunk->offset;
	ctx->to_read = chunk_tail_md_offset(&dev->nv_cache);

	return 0;
}

static void
nvc_recover_open_chunk_deinit_handler(struct spdk_ftl_dev *dev,
				      struct ftl_mngt_process *mngt)
{
	struct nvc_recover_open_chunk_ctx *ctx = ftl_mngt_get_process_ctx(mngt);

	ftl_rq_del(ctx->rq);
}

static const struct ftl_mngt_process_desc desc_recover_open_chunk = {
	.name = "Recover open chunk",
	.ctx_size = sizeof(struct nvc_recover_open_chunk_ctx),
	.init_handler = nvc_recover_open_chunk_init_handler,
	.deinit_handler = nvc_recover_open_chunk_deinit_handler,
	.steps = {
		{
			.name = "Chunk recovery, read vss",
			.action = nvc_recover_open_chunk_read_vss
		},
		{}
	}
};

static void
nvc_recover_open_chunk(struct spdk_ftl_dev *dev,
		       struct ftl_mngt_process *mngt,
		       struct ftl_nv_cache_chunk *chunk)
{
	ftl_mngt_call_process(mngt, &desc_recover_open_chunk, chunk);
}

struct ftl_nv_cache_device_type nvc_bdev_vss = {
	.name = "bdev",
	.features = {
	},
	.ops = {
		.is_bdev_compatible = is_bdev_compatible,
		.is_chunk_active = ftl_nvc_bdev_common_is_chunk_active,
		.md_layout_ops = {
			.region_create = ftl_nvc_bdev_common_region_create,
			.region_open = ftl_nvc_bdev_common_region_open,
		},
		.write = write_io,
		.recover_open_chunk = nvc_recover_open_chunk,
	}
};
FTL_NV_CACHE_DEVICE_TYPE_REGISTER(nvc_bdev_vss)
