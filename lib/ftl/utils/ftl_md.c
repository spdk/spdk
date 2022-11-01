/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/env.h"
#include "spdk/bdev_module.h"

#include "ftl_core.h"
#include "ftl_md.h"
#include "ftl_nv_cache_io.h"

struct ftl_md;
static void io_submit(struct ftl_md *md);
static void io_done(struct ftl_md *md);

static bool
has_mirror(struct ftl_md *md)
{
	if (md->region) {
		if (md->region->mirror_type != FTL_LAYOUT_REGION_TYPE_INVALID) {
			return md->mirror_enabled;
		}
	}

	return false;
}

static int
setup_mirror(struct ftl_md *md)
{
	if (!md->mirror) {
		md->mirror = calloc(1, sizeof(*md->mirror));
		if (!md->mirror) {
			return -ENOMEM;
		}
		md->mirror_enabled = true;
	}

	md->mirror->dev = md->dev;
	md->mirror->data_blocks = md->data_blocks;
	md->mirror->data = md->data;
	md->mirror->vss_data = md->vss_data;

	/* Set proper region in secondary object */
	assert(md->region->mirror_type != FTL_LAYOUT_REGION_TYPE_INVALID);
	md->mirror->region = &md->dev->layout.region[md->region->mirror_type];

	return 0;
}

uint64_t
ftl_md_xfer_blocks(struct spdk_ftl_dev *dev)
{
	return 4ULL * dev->xfer_size;
}

static uint64_t
xfer_size(struct ftl_md *md)
{
	return ftl_md_xfer_blocks(md->dev) * FTL_BLOCK_SIZE;
}

static void
ftl_md_create_heap(struct ftl_md *md, uint64_t vss_blksz)
{
	md->shm_fd = -1;
	md->vss_data = NULL;
	md->data = calloc(md->data_blocks, FTL_BLOCK_SIZE + vss_blksz);

	if (md->data && vss_blksz) {
		md->vss_data = ((char *)md->data) + md->data_blocks * FTL_BLOCK_SIZE;
	}
}

static void
ftl_md_destroy_heap(struct ftl_md *md)
{
	if (md->data) {
		free(md->data);
		md->data = NULL;
		md->vss_data = NULL;
	}
}

static int
ftl_wrapper_open(const char *name, int of, mode_t m)
{
	return open(name, of, m);
}

static void
ftl_md_setup_obj(struct ftl_md *md, int flags,
		 const char *name)
{
	char uuid_str[SPDK_UUID_STRING_LEN];
	const char *fmt;

	if (!(flags & FTL_MD_CREATE_SHM)) {
		assert(false);
		return;
	}

	/* TODO: temporary, define a proper hugetlbfs mountpoint */
	fmt = "/dev/hugepages/ftl_%s_%s";
	md->shm_mmap_flags = MAP_SHARED;
	md->shm_open = ftl_wrapper_open;
	md->shm_unlink = unlink;

	if (name == NULL ||
	    spdk_uuid_fmt_lower(uuid_str, SPDK_UUID_STRING_LEN, &md->dev->conf.uuid) ||
	    snprintf(md->name, sizeof(md->name) / sizeof(md->name[0]),
		     fmt, uuid_str, name) <= 0) {
		md->name[0] = 0;
	}
}

static void
ftl_md_invalidate_shm(struct ftl_md *md)
{
	if (md->dev->sb_shm && md->dev->sb_shm->shm_ready) {
		md->dev->init_retry = true;
		md->dev->sb_shm->shm_ready = false;
	}
}

static void
ftl_md_create_shm(struct ftl_md *md, uint64_t vss_blksz, int flags)
{
	struct stat shm_stat;
	size_t vss_blk_offs;
	void *shm_ptr;
	int open_flags = O_RDWR;
	mode_t open_mode = S_IRUSR | S_IWUSR;

	assert(md->shm_open && md->shm_unlink);
	md->data = NULL;
	md->vss_data = NULL;
	md->shm_sz = 0;

	/* Must have an object name */
	if (md->name[0] == 0) {
		assert(false);
		return;
	}

	/* If specified, unlink before create a new SHM object */
	if (flags & FTL_MD_CREATE_SHM_NEW) {
		if (md->shm_unlink(md->name) < 0 && errno != ENOENT) {
			ftl_md_invalidate_shm(md);
			return;
		}
		open_flags += O_CREAT | O_TRUNC;
	}

	/* Open existing or create a new SHM object, then query its props */
	md->shm_fd = md->shm_open(md->name, open_flags, open_mode);
	if (md->shm_fd < 0 || fstat(md->shm_fd, &shm_stat) < 0) {
		goto err_shm;
	}

	/* Verify open mode hasn't changed */
	if ((shm_stat.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) != open_mode) {
		goto err_shm;
	}

	/* Round up the SHM obj size to the nearest blk size (i.e. page size) */
	md->shm_sz = spdk_divide_round_up(md->data_blocks * FTL_BLOCK_SIZE, shm_stat.st_blksize);

	/* Add some blks for VSS metadata */
	vss_blk_offs = md->shm_sz;

	if (vss_blksz) {
		md->shm_sz += spdk_divide_round_up(md->data_blocks * vss_blksz,
						   shm_stat.st_blksize);
	}

	/* Total SHM obj size */
	md->shm_sz *= shm_stat.st_blksize;

	/* Set or check the object size - zero init`d in case of set (FTL_MD_CREATE_SHM_NEW) */
	if ((shm_stat.st_size == 0 && (ftruncate(md->shm_fd, md->shm_sz) < 0 ||
				       (flags & FTL_MD_CREATE_SHM_NEW) == 0))
	    || (shm_stat.st_size > 0 && (size_t)shm_stat.st_size != md->shm_sz)) {
		goto err_shm;
	}

	/* Create a virtual memory mapping for the object */
	shm_ptr = mmap(NULL, md->shm_sz, PROT_READ | PROT_WRITE, md->shm_mmap_flags,
		       md->shm_fd, 0);
	if (shm_ptr == MAP_FAILED) {
		goto err_shm;
	}

	md->data = shm_ptr;
	if (vss_blksz) {
		md->vss_data = ((char *)shm_ptr) + vss_blk_offs * shm_stat.st_blksize;
	}

	/* Lock the pages in memory (i.e. prevent the pages to be paged out) */
	if (mlock(md->data, md->shm_sz) < 0) {
		goto err_map;
	}

	if (spdk_mem_register(md->data, md->shm_sz)) {
		goto err_mlock;
	}
	md->mem_reg = true;

	return;

	/* Cleanup upon fault */
err_mlock:
	munlock(md->data, md->shm_sz);

err_map:
	munmap(md->data, md->shm_sz);
	md->data = NULL;
	md->vss_data = NULL;
	md->shm_sz = 0;

err_shm:
	if (md->shm_fd >= 0) {
		close(md->shm_fd);
		md->shm_unlink(md->name);
		md->shm_fd = -1;
	}
	ftl_md_invalidate_shm(md);
}

static void
ftl_md_destroy_shm(struct ftl_md *md, int flags)
{
	if (!md->data) {
		return;
	}

	assert(md->shm_sz > 0);
	if (md->mem_reg) {
		spdk_mem_unregister(md->data, md->shm_sz);
		md->mem_reg = false;
	}

	/* Unlock the pages in memory */
	munlock(md->data, md->shm_sz);

	/* Remove the virtual memory mapping for the object */
	munmap(md->data, md->shm_sz);

	/* Close SHM object fd */
	close(md->shm_fd);

	md->data = NULL;
	md->vss_data = NULL;

	/* If specified, keep the object in SHM */
	if (flags & FTL_MD_DESTROY_SHM_KEEP) {
		return;
	}

	/* Otherwise destroy/unlink the object */
	assert(md->name[0] != 0 && md->shm_unlink != NULL);
	md->shm_unlink(md->name);
}

struct ftl_md *ftl_md_create(struct spdk_ftl_dev *dev, uint64_t blocks,
			     uint64_t vss_blksz, const char *name, int flags,
			     const struct ftl_layout_region *region)
{
	struct ftl_md *md;

	md = calloc(1, sizeof(*md));
	if (!md) {
		return NULL;
	}
	md->dev = dev;
	md->data_blocks = blocks;
	md->mirror_enabled = true;

	if (flags != FTL_MD_CREATE_NO_MEM) {
		if (flags & FTL_MD_CREATE_SHM) {
			ftl_md_setup_obj(md, flags, name);
			ftl_md_create_shm(md, vss_blksz, flags);
		} else {
			assert((flags & FTL_MD_CREATE_HEAP) == FTL_MD_CREATE_HEAP);
			ftl_md_create_heap(md, vss_blksz);
		}

		if (!md->data) {
			free(md);
			return NULL;
		}
	}

	if (region) {
		size_t entry_vss_buf_size = vss_blksz * region->entry_size;

		if (entry_vss_buf_size) {
			md->entry_vss_dma_buf = spdk_malloc(entry_vss_buf_size, FTL_BLOCK_SIZE,
							    NULL, SPDK_ENV_LCORE_ID_ANY,
							    SPDK_MALLOC_DMA);
			if (!md->entry_vss_dma_buf) {
				goto err;
			}
		}

		if (ftl_md_set_region(md, region)) {
			goto err;
		}
	}

	return md;
err:
	ftl_md_destroy(md, ftl_md_destroy_region_flags(dev, region->type));
	return NULL;
}

int
ftl_md_unlink(struct spdk_ftl_dev *dev, const char *name, int flags)
{
	struct ftl_md md = { 0 };

	if (0 == (flags & FTL_MD_CREATE_SHM)) {
		/* Unlink can be called for shared memory only */
		return -EINVAL;
	}

	md.dev = dev;
	ftl_md_setup_obj(&md, flags, name);

	return md.shm_unlink(md.name);
}

void
ftl_md_destroy(struct ftl_md *md, int flags)
{
	if (!md) {
		return;
	}

	ftl_md_free_buf(md, flags);

	spdk_free(md->entry_vss_dma_buf);

	free(md->mirror);
	free(md);
}

void
ftl_md_free_buf(struct ftl_md *md, int flags)
{
	if (!md) {
		return;
	}

	if (md->shm_fd < 0) {
		assert(flags == 0);
		ftl_md_destroy_heap(md);
	} else {
		ftl_md_destroy_shm(md, flags);
	}
}

void *
ftl_md_get_buffer(struct ftl_md *md)
{
	return md->data;
}

uint64_t
ftl_md_get_buffer_size(struct ftl_md *md)
{
	return md->data_blocks * FTL_BLOCK_SIZE;
}

static void
ftl_md_vss_buf_init(union ftl_md_vss *buf, uint32_t count,
		    const union ftl_md_vss *vss_pattern)
{
	while (count) {
		count--;
		buf[count] = *vss_pattern;
	}
}

union ftl_md_vss *ftl_md_vss_buf_alloc(struct ftl_layout_region *region, uint32_t count)
{
	union ftl_md_vss *buf = spdk_zmalloc(count * FTL_MD_VSS_SZ, FTL_BLOCK_SIZE, NULL,
						     SPDK_ENV_LCORE_ID_ANY,
						     SPDK_MALLOC_DMA);

	if (!buf) {
		return NULL;
	}

	union ftl_md_vss vss_buf = {0};
	vss_buf.version.md_version = region->current.version;
	ftl_md_vss_buf_init(buf, count, &vss_buf);
	return buf;
}

union ftl_md_vss *ftl_md_get_vss_buffer(struct ftl_md *md)
{
	return md->vss_data;
}

static void
io_cleanup(struct ftl_md *md)
{
	spdk_dma_free(md->io.data);
	md->io.data = NULL;

	spdk_dma_free(md->io.md);
	md->io.md = NULL;
}

static void
exception(void *arg)
{
	struct ftl_md *md = arg;

	md->cb(md->dev, md, -EINVAL);
	io_cleanup(md);
}

static inline enum ftl_stats_type
get_bdev_io_ftl_stats_type(struct spdk_ftl_dev *dev, struct spdk_bdev_io *bdev_io) {
	struct spdk_bdev *nvc = spdk_bdev_desc_get_bdev(dev->nv_cache.bdev_desc);

	if (bdev_io->bdev == nvc)
	{
		return FTL_STATS_TYPE_MD_NV_CACHE;
	} else
	{
		return FTL_STATS_TYPE_MD_BASE;
	}
}

static void
audit_md_vss_version(struct ftl_md *md, uint64_t blocks)
{
#if defined(DEBUG)
	union ftl_md_vss *vss = md->io.md;
	while (blocks) {
		blocks--;
		assert(vss[blocks].version.md_version == md->region->current.version);
	}
#endif
}

static void
read_write_blocks_cb(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct ftl_md *md = arg;

	ftl_stats_bdev_io_completed(md->dev, get_bdev_io_ftl_stats_type(md->dev, bdev_io), bdev_io);

	if (spdk_unlikely(!success)) {
		if (md->io.op == FTL_MD_OP_RESTORE && has_mirror(md)) {
			md->io.status = -EAGAIN;
		} else {
			md->io.status = -EIO;
		}
	} else {
		uint64_t blocks = bdev_io->u.bdev.num_blocks;
		uint64_t size = blocks * FTL_BLOCK_SIZE;

		if (md->io.op == FTL_MD_OP_RESTORE) {
			memcpy(md->data + md->io.data_offset, md->io.data, size);

			if (md->vss_data) {
				uint64_t vss_offset = md->io.data_offset / FTL_BLOCK_SIZE;
				vss_offset *= FTL_MD_VSS_SZ;
				audit_md_vss_version(md, blocks);
				memcpy(md->vss_data + vss_offset, md->io.md, blocks * FTL_MD_VSS_SZ);
			}
		}

		md->io.address += blocks;
		md->io.remaining -= blocks;
		md->io.data_offset += size;
	}

	spdk_bdev_free_io(bdev_io);

	io_submit(md);
}

static inline int
read_blocks(struct spdk_ftl_dev *dev, struct spdk_bdev_desc *desc,
	    struct spdk_io_channel *ch,
	    void *buf, void *md_buf,
	    uint64_t offset_blocks, uint64_t num_blocks,
	    spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	if (desc == dev->nv_cache.bdev_desc) {
		return ftl_nv_cache_bdev_read_blocks_with_md(dev, desc, ch, buf, md_buf,
				offset_blocks, num_blocks,
				cb, cb_arg);
	} else if (md_buf) {
		return spdk_bdev_read_blocks_with_md(desc, ch, buf, md_buf,
						     offset_blocks, num_blocks,
						     cb, cb_arg);
	} else {
		return spdk_bdev_read_blocks(desc, ch, buf,
					     offset_blocks, num_blocks,
					     cb, cb_arg);
	}
}

static inline int
write_blocks(struct spdk_ftl_dev *dev, struct spdk_bdev_desc *desc,
	     struct spdk_io_channel *ch,
	     void *buf, void *md_buf,
	     uint64_t offset_blocks, uint64_t num_blocks,
	     spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	if (desc == dev->nv_cache.bdev_desc) {
		return ftl_nv_cache_bdev_write_blocks_with_md(dev, desc, ch, buf, md_buf,
				offset_blocks, num_blocks,
				cb, cb_arg);
	} else if (md_buf) {
		return spdk_bdev_write_blocks_with_md(desc, ch, buf, md_buf, offset_blocks,
						      num_blocks, cb, cb_arg);
	} else {
		return spdk_bdev_write_blocks(desc, ch, buf, offset_blocks, num_blocks, cb, cb_arg);
	}
}

static void
read_write_blocks(void *_md)
{
	struct ftl_md *md = _md;
	const struct ftl_layout_region *region = md->region;
	uint64_t blocks;
	int rc = 0;

	blocks = spdk_min(md->io.remaining, ftl_md_xfer_blocks(md->dev));

	switch (md->io.op) {
	case FTL_MD_OP_RESTORE:
		rc = read_blocks(md->dev, region->bdev_desc, region->ioch,
				 md->io.data, md->io.md,
				 md->io.address, blocks,
				 read_write_blocks_cb, md);
		break;
	case FTL_MD_OP_PERSIST:
	case FTL_MD_OP_CLEAR:
		rc = write_blocks(md->dev, region->bdev_desc, region->ioch,
				  md->io.data, md->io.md,
				  md->io.address, blocks,
				  read_write_blocks_cb, md);
		break;
	default:
		ftl_abort();
	}

	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(region->bdev_desc);
			md->io.bdev_io_wait.bdev = bdev;
			md->io.bdev_io_wait.cb_fn = read_write_blocks;
			md->io.bdev_io_wait.cb_arg = md;
			spdk_bdev_queue_io_wait(bdev, region->ioch, &md->io.bdev_io_wait);
		} else {
			ftl_abort();
		}
	}
}

static void
io_submit(struct ftl_md *md)
{
	if (!md->io.remaining || md->io.status) {
		io_done(md);
		return;
	}

	if (md->io.op == FTL_MD_OP_PERSIST) {
		uint64_t blocks = spdk_min(md->io.remaining, ftl_md_xfer_blocks(md->dev));

		memcpy(md->io.data, md->data + md->io.data_offset, FTL_BLOCK_SIZE * blocks);

		if (md->vss_data) {
			uint64_t vss_offset = md->io.data_offset / FTL_BLOCK_SIZE;
			vss_offset *= FTL_MD_VSS_SZ;
			assert(md->io.md);
			memcpy(md->io.md, md->vss_data + vss_offset, FTL_MD_VSS_SZ * blocks);
			audit_md_vss_version(md, blocks);
		}
	}
#if defined(DEBUG)
	if (md->io.md && md->io.op == FTL_MD_OP_CLEAR) {
		uint64_t blocks = spdk_min(md->io.remaining, ftl_md_xfer_blocks(md->dev));
		audit_md_vss_version(md, blocks);
	}
#endif

	read_write_blocks(md);
}

static int
io_can_start(struct ftl_md *md)
{
	assert(NULL == md->io.data);
	if (NULL != md->io.data) {
		/* Outgoing IO on metadata */
		return -EINVAL;
	}

	if (!md->region) {
		/* No device region to process data */
		return -EINVAL;
	}

	if (md->region->current.blocks > md->data_blocks) {
		/* No device region to process data */
		FTL_ERRLOG(md->dev, "Blocks number mismatch between metadata object and"
			   "device region\n");
		return -EINVAL;
	}

	return 0;
}

static int
io_prepare(struct ftl_md *md, enum ftl_md_ops op)
{
	const struct ftl_layout_region *region = md->region;
	uint64_t data_size, meta_size = 0;

	/* Allocates buffer for IO */
	data_size = xfer_size(md);
	md->io.data = spdk_zmalloc(data_size, FTL_BLOCK_SIZE, NULL,
				   SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!md->io.data) {
		return -ENOMEM;
	}

	if (md->vss_data || md->region->vss_blksz) {
		meta_size = ftl_md_xfer_blocks(md->dev) * FTL_MD_VSS_SZ;
		md->io.md = spdk_zmalloc(meta_size, FTL_BLOCK_SIZE, NULL,
					 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (!md->io.md) {
			spdk_dma_free(md->io.data);
			md->io.data = NULL;
			return -ENOMEM;
		}
	}

	md->io.address = region->current.offset;
	md->io.remaining = region->current.blocks;
	md->io.data_offset = 0;
	md->io.status = 0;
	md->io.op = op;

	return 0;
}

static int
io_init(struct ftl_md *md, enum ftl_md_ops op)
{
	if (io_can_start(md)) {
		return -EINVAL;
	}

	if (io_prepare(md, op)) {
		return -ENOMEM;
	}

	return 0;
}

static uint64_t
persist_entry_lba(struct ftl_md *md, uint64_t start_entry)
{
	return md->region->current.offset + start_entry * md->region->entry_size;
}

static void
persist_entry_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_md_io_entry_ctx *ctx = cb_arg;
	struct ftl_md *md = ctx->md;

	ftl_stats_bdev_io_completed(md->dev, get_bdev_io_ftl_stats_type(md->dev, bdev_io), bdev_io);

	spdk_bdev_free_io(bdev_io);

	assert(ctx->remaining > 0);
	ctx->remaining--;

	if (!success) {
		ctx->status = -EIO;
	}

	if (!ctx->remaining) {
		ctx->cb(ctx->status, ctx->cb_arg);
	}
}

static int
ftl_md_persist_entry_write_blocks(struct ftl_md_io_entry_ctx *ctx, struct ftl_md *md,
				  spdk_bdev_io_wait_cb retry_fn)
{
	int rc;

	rc = write_blocks(md->dev, md->region->bdev_desc, md->region->ioch,
			  ctx->buffer, ctx->vss_buffer,
			  persist_entry_lba(md, ctx->start_entry), md->region->entry_size,
			  persist_entry_cb, ctx);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(md->region->bdev_desc);
			ctx->bdev_io_wait.bdev = bdev;
			ctx->bdev_io_wait.cb_fn = retry_fn;
			ctx->bdev_io_wait.cb_arg = ctx;
			spdk_bdev_queue_io_wait(bdev, md->region->ioch, &ctx->bdev_io_wait);
		} else {
			ftl_abort();
		}
	}

	return rc;
}

static void
ftl_md_persist_entry_mirror(void *_ctx)
{
	struct ftl_md_io_entry_ctx *ctx = _ctx;

	ftl_md_persist_entry_write_blocks(ctx, ctx->md->mirror, ftl_md_persist_entry_mirror);
}

static void
ftl_md_persist_entry_primary(void *_ctx)
{
	struct ftl_md_io_entry_ctx *ctx = _ctx;
	struct ftl_md *md = ctx->md;
	int rc;

	rc = ftl_md_persist_entry_write_blocks(ctx, md, ftl_md_persist_entry_primary);

	if (!rc && has_mirror(md)) {
		assert(md->region->entry_size == md->mirror->region->entry_size);

		/* The MD object has mirror so execute persist on it too */
		ftl_md_persist_entry_mirror(ctx);
		ctx->remaining++;
	}
}

static void
_ftl_md_persist_entry(struct ftl_md_io_entry_ctx *ctx)
{
	ctx->status = 0;
	ctx->remaining = 1;

	/* First execute an IO to the primary region */
	ftl_md_persist_entry_primary(ctx);
}

void
ftl_md_persist_entry(struct ftl_md *md, uint64_t start_entry, void *buffer, void *vss_buffer,
		     ftl_md_io_entry_cb cb, void *cb_arg,
		     struct ftl_md_io_entry_ctx *ctx)
{
	if (spdk_unlikely(0 == md->region->entry_size)) {
		/* This MD has not been configured to support persist entry call */
		ftl_abort();
	}

	/* Initialize persist entry context */
	ctx->cb = cb;
	ctx->cb_arg = cb_arg;
	ctx->md = md;
	ctx->start_entry = start_entry;
	ctx->buffer = buffer;
	ctx->vss_buffer = vss_buffer ? : md->entry_vss_dma_buf;

	_ftl_md_persist_entry(ctx);
}

static void
read_entry_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_md_io_entry_ctx *ctx = cb_arg;
	struct ftl_md *md = ctx->md;

	ftl_stats_bdev_io_completed(md->dev, get_bdev_io_ftl_stats_type(md->dev, bdev_io), bdev_io);

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		if (has_mirror(md)) {
			if (setup_mirror(md)) {
				/* An error when setup the mirror */
				ctx->status = -EIO;
				goto finish_io;
			}

			/* First read from the mirror */
			ftl_md_read_entry(md->mirror, ctx->start_entry, ctx->buffer, ctx->vss_buffer,
					  ctx->cb, ctx->cb_arg,
					  ctx);
			return;
		} else {
			ctx->status = -EIO;
			goto finish_io;
		}
	}

finish_io:
	ctx->cb(ctx->status, ctx->cb_arg);
}

static void
ftl_md_read_entry_read_blocks(struct ftl_md_io_entry_ctx *ctx, struct ftl_md *md,
			      spdk_bdev_io_wait_cb retry_fn)
{
	int rc;

	rc = read_blocks(md->dev, md->region->bdev_desc, md->region->ioch,
			 ctx->buffer, ctx->vss_buffer,
			 persist_entry_lba(md, ctx->start_entry), md->region->entry_size,
			 read_entry_cb, ctx);

	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(md->region->bdev_desc);
			ctx->bdev_io_wait.bdev = bdev;
			ctx->bdev_io_wait.cb_fn = retry_fn;
			ctx->bdev_io_wait.cb_arg = ctx;
			spdk_bdev_queue_io_wait(bdev, md->region->ioch, &ctx->bdev_io_wait);
		} else {
			ftl_abort();
		}
	}
}

static void
_ftl_md_read_entry(void *_ctx)
{
	struct ftl_md_io_entry_ctx *ctx = _ctx;

	ftl_md_read_entry_read_blocks(ctx, ctx->md, _ftl_md_read_entry);
}

void
ftl_md_read_entry(struct ftl_md *md, uint64_t start_entry, void *buffer, void *vss_buffer,
		  ftl_md_io_entry_cb cb, void *cb_arg,
		  struct ftl_md_io_entry_ctx *ctx)
{
	if (spdk_unlikely(0 == md->region->entry_size)) {
		/* This MD has not been configured to support read entry call */
		ftl_abort();
	}

	ctx->cb = cb;
	ctx->cb_arg = cb_arg;
	ctx->md = md;
	ctx->start_entry = start_entry;
	ctx->buffer = buffer;
	ctx->vss_buffer = vss_buffer;

	_ftl_md_read_entry(ctx);
}

void
ftl_md_persist_entry_retry(struct ftl_md_io_entry_ctx *ctx)
{
	_ftl_md_persist_entry(ctx);
}

static void
persist_mirror_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_md *primary = md->owner.private;

	if (status) {
		/* We got an error, stop persist procedure immediately */
		primary->io.status = status;
		io_done(primary);
	} else {
		/* Now continue the persist procedure on the primary MD object */
		if (0 == io_init(primary, FTL_MD_OP_PERSIST)) {
			io_submit(primary);
		} else {
			spdk_thread_send_msg(spdk_get_thread(), exception, primary);
		}
	}
}

void
ftl_md_persist(struct ftl_md *md)
{
	if (has_mirror(md)) {
		if (setup_mirror(md)) {
			/* An error when setup the mirror */
			spdk_thread_send_msg(spdk_get_thread(), exception, md);
			return;
		}

		/* Set callback and context in mirror */
		md->mirror->cb = persist_mirror_cb;
		md->mirror->owner.private = md;

		/* First persist the mirror */
		ftl_md_persist(md->mirror);
		return;
	}

	if (0 == io_init(md, FTL_MD_OP_PERSIST)) {
		io_submit(md);
	} else {
		spdk_thread_send_msg(spdk_get_thread(), exception, md);
	}
}

static void
restore_mirror_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_md *primary = md->owner.private;

	if (status) {
		/* Cannot restore the object from the mirror too, mark error and fail */
		primary->io.status = -EIO;
		io_done(primary);
	} else {
		/*
		 * Restoring from the mirror successful. Synchronize mirror to the primary.
		 * Because we read MD content from the mirror, we can disable it, only the primary
		 * requires persisting.
		 */
		primary->io.status = 0;
		primary->mirror_enabled = false;
		io_cleanup(primary);
		ftl_md_persist(primary);
		primary->mirror_enabled = true;
	}
}

static void
restore_sync_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_md *primary = md->owner.private;

	if (status) {
		/* Cannot sync the object from the primary to the mirror, mark error and fail */
		primary->io.status = -EIO;
		io_done(primary);
	} else {
		primary->cb(dev, primary, primary->io.status);
		io_cleanup(primary);
	}
}

static int
restore_done(struct ftl_md *md)
{
	if (-EAGAIN == md->io.status) {
		/* Failed to read MD from primary region, try it from mirror.
		 * At the moment read the mirror entirely, (TODO) in the
		 * feature we can restore from primary and mirror region
		 * with finer granularity.
		 */

		if (has_mirror(md)) {
			if (setup_mirror(md)) {
				/* An error when setup the mirror */
				return -EIO;
			}

			/* Set callback and context in mirror */
			md->mirror->cb = restore_mirror_cb;
			md->mirror->owner.private = md;

			/* First persist the mirror */
			ftl_md_restore(md->mirror);
			return -EAGAIN;
		} else {
			return -EIO;
		}
	} else if (0 == md->io.status && false == md->dev->sb->clean) {
		if (has_mirror(md)) {
			/* There was a dirty shutdown, synchronize primary to mirror */

			/* Set callback and context in the mirror */
			md->mirror->cb = restore_sync_cb;
			md->mirror->owner.private = md;

			/* First persist the mirror */
			ftl_md_persist(md->mirror);
			return -EAGAIN;
		}
	}

	return md->io.status;
}

static void
io_done(struct ftl_md *md)
{
	int status;

	if (md->io.op == FTL_MD_OP_RESTORE) {
		status = restore_done(md);
	} else {
		status = md->io.status;
	}

	if (status != -EAGAIN) {
		md->cb(md->dev, md, status);
		io_cleanup(md);
	}
}

void
ftl_md_restore(struct ftl_md *md)
{
	if (0 == io_init(md, FTL_MD_OP_RESTORE)) {
		io_submit(md);
	} else {
		spdk_thread_send_msg(spdk_get_thread(), exception, md);
	}
}

static int
pattern_prepare(struct ftl_md *md,
		int data_pattern, union ftl_md_vss *vss_pattern)
{
	void *data = md->io.data;
	uint64_t data_size = xfer_size(md);

	memset(data, data_pattern, data_size);

	if (md->io.md) {
		if (vss_pattern) {
			/* store the VSS pattern... */
			ftl_md_vss_buf_init(md->io.md, ftl_md_xfer_blocks(md->dev), vss_pattern);
		} else {
			/* ...or default init VSS to 0 */
			union ftl_md_vss vss = {0};

			vss.version.md_version = md->region->current.version;
			ftl_md_vss_buf_init(md->io.md, ftl_md_xfer_blocks(md->dev), &vss);
		}
	}

	return 0;
}

static void
clear_mirror_cb(struct spdk_ftl_dev *dev, struct ftl_md *secondary, int status)
{
	struct ftl_md *primary = secondary->owner.private;

	if (status) {
		/* We got an error, stop persist procedure immediately */
		primary->io.status = status;
		io_done(primary);
	} else {
		/* Now continue the persist procedure on the primary MD object */
		if (0 == io_init(primary, FTL_MD_OP_CLEAR) &&
		    0 == pattern_prepare(primary, *(int *)secondary->io.data,
					 secondary->io.md)) {
			io_submit(primary);
		} else {
			spdk_thread_send_msg(spdk_get_thread(), exception, primary);
		}
	}
}

void
ftl_md_clear(struct ftl_md *md, int data_pattern, union ftl_md_vss *vss_pattern)
{
	if (has_mirror(md)) {
		if (setup_mirror(md)) {
			/* An error when setup the mirror */
			spdk_thread_send_msg(spdk_get_thread(), exception, md);
			return;
		}

		/* Set callback and context in mirror */
		md->mirror->cb = clear_mirror_cb;
		md->mirror->owner.private = md;

		/* First persist the mirror */
		ftl_md_clear(md->mirror, data_pattern, vss_pattern);
		return;
	}

	if (0 == io_init(md, FTL_MD_OP_CLEAR) && 0 == pattern_prepare(md, data_pattern, vss_pattern)) {
		io_submit(md);
	} else {
		spdk_thread_send_msg(spdk_get_thread(), exception, md);
	}
}

const struct ftl_layout_region *
ftl_md_get_region(struct ftl_md *md)
{
	return md->region;
}

int
ftl_md_set_region(struct ftl_md *md,
		  const struct ftl_layout_region *region)
{
	assert(region->current.blocks <= md->data_blocks);
	md->region = region;

	if (md->vss_data) {
		union ftl_md_vss vss = {0};
		vss.version.md_version = region->current.version;
		ftl_md_vss_buf_init(md->vss_data, md->data_blocks, &vss);
		if (region->entry_size) {
			assert(md->entry_vss_dma_buf);
			ftl_md_vss_buf_init(md->entry_vss_dma_buf, region->entry_size, &vss);
		}
	}

	if (has_mirror(md)) {
		return setup_mirror(md);
	}

	return 0;
}

int
ftl_md_create_region_flags(struct spdk_ftl_dev *dev, int region_type)
{
	int flags = FTL_MD_CREATE_SHM;

	switch (region_type) {
	case FTL_LAYOUT_REGION_TYPE_SB:
		if (dev->conf.mode & SPDK_FTL_MODE_CREATE) {
			flags |= FTL_MD_CREATE_SHM_NEW;
		}
		break;

	case FTL_LAYOUT_REGION_TYPE_BAND_MD:
	case FTL_LAYOUT_REGION_TYPE_NVC_MD:
		if (!ftl_fast_startup(dev)) {
			flags |= FTL_MD_CREATE_SHM_NEW;
		}
		break;
	case FTL_LAYOUT_REGION_TYPE_VALID_MAP:
	case FTL_LAYOUT_REGION_TYPE_TRIM_MD:
		if (!ftl_fast_startup(dev) && !ftl_fast_recovery(dev)) {
			flags |= FTL_MD_CREATE_SHM_NEW;
		}
		break;
	default:
		return FTL_MD_CREATE_HEAP;
	}

	return flags;
}

int
ftl_md_destroy_region_flags(struct spdk_ftl_dev *dev, int region_type)
{
	switch (region_type) {
	case FTL_LAYOUT_REGION_TYPE_SB:
	case FTL_LAYOUT_REGION_TYPE_BAND_MD:
	case FTL_LAYOUT_REGION_TYPE_VALID_MAP:
	case FTL_LAYOUT_REGION_TYPE_NVC_MD:
	case FTL_LAYOUT_REGION_TYPE_TRIM_MD:
		if (dev->conf.fast_shutdown) {
			return FTL_MD_DESTROY_SHM_KEEP;
		}
		break;

	default:
		break;
	}
	return 0;
}

int
ftl_md_create_shm_flags(struct spdk_ftl_dev *dev)
{
	int flags = FTL_MD_CREATE_SHM;

	if (!ftl_fast_startup(dev) && !ftl_fast_recovery(dev)) {
		flags |= FTL_MD_CREATE_SHM_NEW;
	}
	return flags;
}

int
ftl_md_destroy_shm_flags(struct spdk_ftl_dev *dev)
{
	return (dev->conf.fast_shutdown) ? FTL_MD_DESTROY_SHM_KEEP : 0;
}
