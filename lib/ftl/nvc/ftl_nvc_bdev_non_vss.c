/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#include "ftl_nvc_dev.h"
#include "ftl_core.h"
#include "ftl_layout.h"
#include "ftl_nv_cache.h"
#include "mngt/ftl_mngt.h"
#include "ftl_nvc_bdev_common.h"

static void write_io(struct ftl_io *io);
static void p2l_log_cb(struct ftl_io *io);

static int
init(struct spdk_ftl_dev *dev)
{
	int rc;

	rc = ftl_p2l_log_init(dev);
	if (rc) {
		return 0;
	}

	return 0;
}

static void
deinit(struct spdk_ftl_dev *dev)
{
	ftl_p2l_log_deinit(dev);
}

static bool
is_bdev_compatible(struct spdk_ftl_dev *dev, struct spdk_bdev *bdev)
{
	if (spdk_bdev_get_md_size(bdev) != 0) {
		/* Bdev's metadata is invalid size */
		return false;
	}

	return true;
}

static void
on_chunk_open(struct spdk_ftl_dev *dev, struct ftl_nv_cache_chunk *chunk)
{
	assert(NULL == chunk->p2l_log);
	chunk->p2l_log = ftl_p2l_log_acquire(dev, chunk->md->seq_id, p2l_log_cb);
	chunk->md->p2l_log_type = ftl_p2l_log_type(chunk->p2l_log);
}

static void
on_chunk_closed(struct spdk_ftl_dev *dev, struct ftl_nv_cache_chunk *chunk)
{
	assert(chunk->p2l_log);
	ftl_p2l_log_release(dev, chunk->p2l_log);
	chunk->p2l_log = NULL;
}

static void
p2l_log_cb(struct ftl_io *io)
{
	ftl_nv_cache_write_complete(io, true);
}

static void
write_io_cb(struct spdk_bdev_io *bdev_io, bool success, void *ctx)
{
	struct ftl_io *io = ctx;

	ftl_stats_bdev_io_completed(io->dev, FTL_STATS_TYPE_USER, bdev_io);
	spdk_bdev_free_io(bdev_io);

	if (spdk_likely(success)) {
		struct ftl_p2l_log *log = io->nv_cache_chunk->p2l_log;
		ftl_p2l_log_io(log, io);
	} else {
		ftl_nv_cache_write_complete(io, false);
	}
}

static void
write_io_retry(void *ctx)
{
	struct ftl_io *io = ctx;

	write_io(io);
}

static void
write_io(struct ftl_io *io)
{
	struct spdk_ftl_dev *dev = io->dev;
	struct ftl_nv_cache *nv_cache = &dev->nv_cache;
	int rc;

	rc = spdk_bdev_writev_blocks(nv_cache->bdev_desc, nv_cache->cache_ioch,
				     io->iov, io->iov_cnt,
				     ftl_addr_to_nvc_offset(dev, io->addr), io->num_blocks,
				     write_io_cb, io);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(nv_cache->bdev_desc);
			io->bdev_io_wait.bdev = bdev;
			io->bdev_io_wait.cb_fn = write_io_retry;
			io->bdev_io_wait.cb_arg = io;
			spdk_bdev_queue_io_wait(bdev, nv_cache->cache_ioch, &io->bdev_io_wait);
		} else {
			ftl_abort();
		}
	}
}

static void
process(struct spdk_ftl_dev *dev)
{
	ftl_p2l_log_flush(dev);
}

struct recovery_chunk_ctx {
	struct ftl_nv_cache_chunk *chunk;
};

static void
recovery_chunk_recover_p2l_map_cb(void *cb_arg, int status)
{
	struct ftl_mngt_process *mngt = cb_arg;

	if (status) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

static int
recovery_chunk_recover_p2l_map_read_cb(struct spdk_ftl_dev *dev, void *cb_arg,
				       uint64_t lba, ftl_addr addr, uint64_t seq_id)
{
	struct ftl_mngt_process *mngt = cb_arg;
	struct recovery_chunk_ctx *ctx = ftl_mngt_get_process_ctx(mngt);
	struct ftl_nv_cache_chunk *chunk = ctx->chunk;

	ftl_nv_cache_chunk_set_addr(chunk, lba, addr);

	/* TODO We could stop scanning when getting all LBA within the chunk */
	return 0;
}


static void
recovery_chunk_recover_p2l_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct recovery_chunk_ctx *ctx = ftl_mngt_get_process_ctx(mngt);
	struct ftl_nv_cache_chunk *chunk = ctx->chunk;
	int rc;

	rc = ftl_p2l_log_read(dev, chunk->md->p2l_log_type, chunk->md->seq_id,
			      recovery_chunk_recover_p2l_map_cb, mngt,
			      recovery_chunk_recover_p2l_map_read_cb);

	if (rc) {
		ftl_mngt_fail_step(mngt);
	}
}

static int
recovery_chunk_init(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
		    void *init_ctx)
{
	struct recovery_chunk_ctx *ctx = ftl_mngt_get_process_ctx(mngt);

	ctx->chunk = init_ctx;
	return 0;
}

static const struct ftl_mngt_process_desc desc_chunk_recovery = {
	.name = "Recover open chunk",
	.ctx_size = sizeof(struct recovery_chunk_ctx),
	.init_handler = recovery_chunk_init,
	.steps = {
		{
			.name = "Recover chunk P2L map",
			.action = recovery_chunk_recover_p2l_map,
		},
		{}
	}
};

static void
recover_open_chunk(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt,
		   struct ftl_nv_cache_chunk *chunk)
{
	ftl_mngt_call_process(mngt, &desc_chunk_recovery, chunk);
}

static int
setup_layout(struct spdk_ftl_dev *dev)
{
	const struct ftl_md_layout_ops *md_ops = &dev->nv_cache.nvc_type->ops.md_layout_ops;
	const uint64_t blocks = ftl_p2l_log_get_md_blocks_required(dev, 1, ftl_get_num_blocks_in_band(dev));
	enum ftl_layout_region_type region_type;

	for (region_type = FTL_LAYOUT_REGION_TYPE_P2L_LOG_IO_MIN;
	     region_type <= FTL_LAYOUT_REGION_TYPE_P2L_LOG_IO_MAX;
	     region_type++) {
		if (md_ops->region_create(dev, region_type, FTL_P2L_LOG_VERSION_CURRENT, blocks)) {
			return -1;
		}

		if (md_ops->region_open(dev, region_type, FTL_P2L_LOG_VERSION_CURRENT,
					FTL_BLOCK_SIZE, blocks,
					&dev->layout.region[region_type])) {
			return -1;
		}
	}

	return 0;
}

struct ftl_nv_cache_device_type nvc_bdev_non_vss = {
	.name = "bdev-non-vss",
	.features = {
	},
	.ops = {
		.init = init,
		.deinit = deinit,
		.on_chunk_open = on_chunk_open,
		.on_chunk_closed = on_chunk_closed,
		.is_bdev_compatible = is_bdev_compatible,
		.is_chunk_active = ftl_nvc_bdev_common_is_chunk_active,
		.setup_layout = setup_layout,
		.md_layout_ops = {
			.region_create = ftl_nvc_bdev_common_region_create,
			.region_open = ftl_nvc_bdev_common_region_open,
		},
		.process = process,
		.write = write_io,
		.recover_open_chunk = recover_open_chunk
	}
};
FTL_NV_CACHE_DEVICE_TYPE_REGISTER(nvc_bdev_non_vss)
