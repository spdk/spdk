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

#include "spdk/env.h"
#include "spdk/bdev_module.h"

#include "ftl_core.h"
#include "ftl_md.h"
#include "ftl_nv_cache_io.h"

struct ftl_md_impl;
static void io_submit(struct ftl_md_impl *md);
static void io_done(struct ftl_md_impl *md);

typedef int (*shm_open_t)(const char *, int, mode_t);
typedef int (*shm_unlink_t)(const char *);

enum ftl_md_ops {
	FTL_MD_OP_RESTORE,
	FTL_MD_OP_PERSIST,
	FTL_MD_OP_CLEAR,
};

struct ftl_md_impl {
	struct ftl_md base;

	/* Pointer to the FTL device */
	struct spdk_ftl_dev *dev;

	/* Region of device on which store/restore the metadata */
	const struct ftl_layout_region  *region;

	/* Pointer to data */
	void *data;

	/* Size of buffer in FTL block size unit */
	uint64_t data_blocks;

	/* Pointer to VSS metadata data */
	void *vss_data;

	/* Fields for doing IO */
	struct {
		/* Flag indicates an IO activity is in progress */
		void *data;
		void *meta;
		uint64_t address;
		uint64_t left;
		uint64_t data_offset;
		int status;
		enum ftl_md_ops op;
		struct spdk_bdev_io_wait_entry bdev_io_wait;
	} io;

	/* SHM object file descriptor or -1 if heap alloc */
	int shm_fd;

	/* Object name */
	char name[NAME_MAX + 1];

	/* mmap flags for the SHM object */
	int shm_mmap_flags;

	/* Total size of SHM object (data + md) */
	size_t shm_sz;

	/* open() for the SHM object */
	shm_open_t shm_open;

	/* unlink() for the SHM object */
	shm_unlink_t shm_unlink;

	/* Memory was registered to SPDK */
	bool mem_reg;

	/* Metadata primary object */
	struct ftl_md_impl *mirror;

	/* This flag is used by the primary to disable mirror temporarily */
	bool mirror_on;
};

static bool has_mirror(struct ftl_md_impl *md)
{
	if (md->region) {
		if (md->region->mirror_type != ftl_layout_region_type_invalid) {
			return md->mirror_on;
		}
	}

	return false;
}

static int setup_mirror(struct ftl_md_impl *md)
{
	if (!md->mirror) {
		md->mirror = calloc(1, sizeof(*md->mirror));
		if (!md->mirror) {
			return -ENOMEM;
		}
		md->mirror_on = true;
	}

	md->mirror->dev = md->dev;
	md->mirror->data_blocks = md->data_blocks;
	md->mirror->data = md->data;
	md->mirror->vss_data = md->vss_data;

	/* Set proper region in secondary object */
	assert(md->region->mirror_type != ftl_layout_region_type_invalid);
	md->mirror->region = &md->dev->layout.region[md->region->mirror_type];

	return 0;
}

uint64_t ftl_md_xfer_blocks(struct spdk_ftl_dev *dev)
{
	return 4ULL * dev->xfer_size;
}

static uint64_t xfer_size(struct ftl_md_impl *md)
{
	return ftl_md_xfer_blocks(md->dev) * FTL_BLOCK_SIZE;
}

static void ftl_md_create_heap(struct ftl_md_impl *md, uint64_t vss_blksz)
{
	md->shm_fd = -1;
	md->data = calloc(md->data_blocks, FTL_BLOCK_SIZE + vss_blksz);
	if (!md->data) {
		md->vss_data = NULL;
		return;
	}

	if (vss_blksz) {
		md->vss_data = ((char *)md->data) + md->data_blocks * FTL_BLOCK_SIZE;
	} else {
		md->vss_data = NULL;
	}
}

static void ftl_md_destroy_heap(struct ftl_md_impl *md)
{
	if (md->data) {
		free(md->data);
		md->data = NULL;
		md->vss_data = NULL;
	}
}

static int ftl_wrapper_open(const char *name, int of, mode_t m)
{
	return open(name, of, m);
}

static void ftl_md_setup_obj(struct ftl_md_impl *md, int flags,
			     const char *name)
{
	char uuid_str[SPDK_UUID_STRING_LEN];
	const char *fmt;

	if ((flags & (FTL_MD_CREATE_SHM | FTL_MD_CREATE_SHM_HUGE)) ==
	    (FTL_MD_CREATE_SHM | FTL_MD_CREATE_SHM_HUGE)) {
		/* TODO: temporary, define a proper hugetlbfs mountpoint */
		fmt = "/dev/hugepages/ftl_%s_%s";
		md->shm_mmap_flags = MAP_SHARED;
		md->shm_open = ftl_wrapper_open;
		md->shm_unlink = unlink;
	} else {
		fmt = "/ftl_%s_%s";
		md->shm_mmap_flags = MAP_SHARED;
		md->shm_open = shm_open;
		md->shm_unlink = shm_unlink;
	}

	if (name == NULL ||
	    spdk_uuid_fmt_lower(uuid_str, SPDK_UUID_STRING_LEN, &md->dev->uuid) ||
	    snprintf(md->name, sizeof(md->name) / sizeof(md->name[0]),
		     fmt, uuid_str, name) <= 0) {
		md->name[0] = 0;
	}
}

static void ftl_md_create_shm(struct ftl_md_impl *md, uint64_t vss_blksz, int flags)
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
		return;
	}

	/* If specified, unlink before create a new SHM object */
	if (flags & FTL_MD_CREATE_SHM_NEW) {
		if (md->shm_unlink(md->name) < 0 && errno != ENOENT) {
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

	if (flags & FTL_MD_CREATE_SHM_HUGE) {
		if (spdk_mem_register(md->data, md->shm_sz)) {
			goto err_mlock;
		}
		md->mem_reg = true;
	}

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
}

static void ftl_md_destroy_shm(struct ftl_md_impl *md)
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

	/* Otherwise destroy/unlink the object */
	assert(md->name[0] != 0 && md->shm_unlink != NULL);
	md->shm_unlink(md->name);
}

struct ftl_md *ftl_md_create(struct spdk_ftl_dev *dev, uint64_t blocks,
			     uint64_t vss_blksz, const char *name, int flags)
{
	struct ftl_md_impl *md = calloc(1, sizeof(*md));

	if (!md) {
		return NULL;
	}
	md->dev = dev;
	md->data_blocks = blocks;
	md->mirror_on = true;

	ftl_md_setup_obj(md, flags, name);

	if (0 == (flags & FTL_MD_CREATE_NO_MEM)) {
		if (flags & FTL_MD_CREATE_SHM) {
			ftl_md_create_shm(md, vss_blksz, flags);
		} else {
			assert(flags == 0);
			ftl_md_create_heap(md, vss_blksz);
		}

		if (!md->data) {
			free(md);
			return NULL;
		}
	}

	return &md->base;
}

int ftl_md_unlink(struct spdk_ftl_dev *dev, const char *name, int flags)
{
	struct ftl_md_impl md = { 0 };

	if (0 == (flags & FTL_MD_CREATE_SHM)) {
		/* Unlink can be called for shared memory only */
		return -EINVAL;
	}

	md.dev = dev;
	ftl_md_setup_obj(&md, flags, name);

	return md.shm_unlink(md.name);
}

void ftl_md_destroy(struct ftl_md *md)
{
	struct ftl_md_impl *impl;

	if (!md) {
		return;
	}
	impl = SPDK_CONTAINEROF(md, struct ftl_md_impl, base);

	ftl_md_free_buf(md);

	free(impl->mirror);
	free(impl);
}

void ftl_md_free_buf(struct ftl_md *md)
{
	struct ftl_md_impl *impl;

	if (!md) {
		return;
	}
	impl = SPDK_CONTAINEROF(md, struct ftl_md_impl, base);

	if (impl->shm_fd < 0) {
		ftl_md_destroy_heap(impl);
	} else {
		ftl_md_destroy_shm(impl);
	}
}

void *ftl_md_get_buffer(struct ftl_md *md)
{
	struct ftl_md_impl *impl = SPDK_CONTAINEROF(md, struct ftl_md_impl, base);

	return impl->data;
}

uint64_t ftl_md_get_buffer_size(struct ftl_md *md)
{
	struct ftl_md_impl *impl = SPDK_CONTAINEROF(md, struct ftl_md_impl, base);

	return impl->data_blocks * FTL_BLOCK_SIZE;
}

static void ftl_md_vss_buf_init(union ftl_md_vss *buf, uint32_t count, const union ftl_md_vss *vss_pattern)
{
	do {
		count--;
		buf[count] = *vss_pattern;
	} while (count);
}

union ftl_md_vss *ftl_md_vss_buf_alloc(struct ftl_layout_region *region, uint32_t count)
{
	union ftl_md_vss *buf = spdk_zmalloc(count * FTL_MD_VSS_SZ, FTL_BLOCK_SIZE, NULL, SPDK_ENV_LCORE_ID_ANY,
		SPDK_MALLOC_DMA);

	if (!buf)
		return NULL;

	union ftl_md_vss vss_buf = {0};
	vss_buf.version.md_version = region->current.version;
	ftl_md_vss_buf_init(buf, count, &vss_buf);
	return buf;
}

union ftl_md_vss *ftl_md_get_vss_buffer(struct ftl_md *md)
{
	struct ftl_md_impl *impl = SPDK_CONTAINEROF(md, struct ftl_md_impl, base);

	return impl->vss_data;
}

static void io_cleanup(struct ftl_md_impl *md)
{
	spdk_dma_free(md->io.data);
	md->io.data = NULL;

	spdk_dma_free(md->io.meta);
	md->io.meta = NULL;
}

static void exception(void *arg)
{
	struct ftl_md_impl *md = arg;

	md->base.cb(md->dev, &md->base, -EINVAL);
	io_cleanup(md);
}

static void audit_md_vss_version(struct ftl_md_impl *md, uint64_t blocks)
{
#if defined(DEBUG)
	union ftl_md_vss *vss = md->io.meta;
	do {
		blocks--;
		assert(vss[blocks].version.md_version == md->region->current.version);
	} while (blocks);
#endif
}

static void read_write_blocks_cb(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct ftl_md_impl *md = arg;

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
				memcpy(md->vss_data + vss_offset, md->io.meta, blocks * FTL_MD_VSS_SZ);
			}
		}

		md->io.address += blocks;
		md->io.left -= blocks;
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

static void read_write_blocks(void *_md)
{
	struct ftl_md_impl *md = _md;
	const struct ftl_layout_region *region = md->region;
	uint64_t blocks;
	int rc = 0;

	blocks = spdk_min(md->io.left, ftl_md_xfer_blocks(md->dev));

	switch (md->io.op) {
	case FTL_MD_OP_RESTORE:
		rc = read_blocks(md->dev, region->bdev_desc, region->ioch,
				 md->io.data, md->io.meta,
				 md->io.address, blocks,
				 read_write_blocks_cb, md);
		break;
	case FTL_MD_OP_PERSIST:
	case FTL_MD_OP_CLEAR:
		rc = write_blocks(md->dev, region->bdev_desc, region->ioch,
				  md->io.data, md->io.meta,
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

static void io_submit(struct ftl_md_impl *md)
{
	if (!md->io.left || md->io.status) {
		io_done(md);
		return;
	}

	if (md->io.op == FTL_MD_OP_PERSIST) {
		uint64_t blocks = spdk_min(md->io.left, ftl_md_xfer_blocks(md->dev));

		memcpy(md->io.data, md->data + md->io.data_offset, FTL_BLOCK_SIZE * blocks);

		if (md->vss_data) {
			uint64_t vss_offset = md->io.data_offset / FTL_BLOCK_SIZE;
			vss_offset *= FTL_MD_VSS_SZ;
			assert(md->io.meta);
			memcpy(md->io.meta, md->vss_data + vss_offset, FTL_MD_VSS_SZ * blocks);
			audit_md_vss_version(md, blocks);
		}
	}
#if defined(DEBUG)
	if (md->io.meta && md->io.op == FTL_MD_OP_CLEAR) {
		uint64_t blocks = spdk_min(md->io.left, ftl_md_xfer_blocks(md->dev));
		audit_md_vss_version(md, blocks);
	}
#endif

	read_write_blocks(md);
}

static int io_can_start(struct ftl_md_impl *md)
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

static int io_prepare(struct ftl_md_impl *md, enum ftl_md_ops op)
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
		md->io.meta = spdk_zmalloc(meta_size, FTL_BLOCK_SIZE, NULL,
					SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (!md->io.meta) {
			return -ENOMEM;
		}
	}

	md->io.address = region->current.offset;
	md->io.left = region->current.blocks;
	md->io.data_offset = 0;
	md->io.status = 0;
	md->io.op = op;

	return 0;
}

static int io_init(struct ftl_md_impl *impl, enum ftl_md_ops op)
{
	if (io_can_start(impl)) {
		return -1;
	}

	if (io_prepare(impl, op)) {
		return -1;
	}

	return 0;
}

static uint64_t persist_entry_lba(struct ftl_md_impl *impl, uint64_t start_entry)
{
	return impl->region->current.offset + start_entry * impl->region->entry_size;
}

static void persist_entry_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_md_io_entry_ctx *ctx = cb_arg;

	spdk_bdev_free_io(bdev_io);

	assert(ctx->remaining > 0);
	ctx->remaining--;

	if (!success) {
		ctx->status = -EIO;
	}

	if (!ctx->remaining) {
		if (ctx->vss_buffer) {
			spdk_free(ctx->vss_buffer);
		}
		ctx->cb(ctx->status, ctx->cb_arg);
	}
}

static int
ftl_md_persist_entry_write_blocks(struct ftl_md_io_entry_ctx *ctx, struct ftl_md_impl *md,
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
	struct ftl_md_impl *impl = ctx->md;
	int rc;

	rc = ftl_md_persist_entry_write_blocks(ctx, impl, ftl_md_persist_entry_primary);

	if (!rc && has_mirror(impl)) {
		assert(impl->region->entry_size == impl->mirror->region->entry_size);

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

void ftl_md_persist_entry(struct ftl_md *md, uint64_t start_entry, void *buffer, void *vss_buffer,
			  ftl_md_io_entry_cb cb, void *cb_arg,
			  struct ftl_md_io_entry_ctx *ctx)
{
	struct ftl_md_impl *impl = SPDK_CONTAINEROF(md, struct ftl_md_impl, base);

	if (spdk_unlikely(0 == impl->region->entry_size)) {
		/* This MD has not been configured to support persist entry call */
		ftl_abort();
	}

	/* Initialize persist entry context */
	ctx->cb = cb;
	ctx->cb_arg = cb_arg;
	ctx->md = impl;
	ctx->start_entry = start_entry;
	ctx->buffer = buffer;

	if (vss_buffer) {
		size_t vss_buf_size = impl->region->entry_size * impl->region->vss_blksz;
		void *vss_buf = spdk_zmalloc(vss_buf_size, FTL_BLOCK_SIZE, NULL,
					     SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (!vss_buf) {
			cb(-ENOMEM, cb_arg);
			return;
		}
		memcpy(vss_buf, vss_buffer, vss_buf_size);
		vss_buffer = vss_buf;
	}
	ctx->vss_buffer = vss_buffer;

	_ftl_md_persist_entry(ctx);
}

static void read_entry_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct ftl_md_io_entry_ctx *ctx = cb_arg;
	struct ftl_md_impl *md = ctx->md;

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		if (has_mirror(md)) {
				if (setup_mirror(md)) {
					/* An error when setup the mirror */
					ctx->status = -EIO;
					goto finish_io;
				}

				/* First read from the mirror */
				ftl_md_read_entry(&md->mirror->base, ctx->start_entry, ctx->buffer, ctx->vss_buffer,
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
ftl_md_read_entry_read_blocks(struct ftl_md_io_entry_ctx *ctx, struct ftl_md_impl *md,
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
	struct ftl_md_impl *impl = ctx->md;

	ftl_md_read_entry_read_blocks(ctx, impl, _ftl_md_read_entry);
}

void ftl_md_read_entry(struct ftl_md *md, uint64_t start_entry, void *buffer, void *vss_buffer,
		  ftl_md_io_entry_cb cb, void *cb_arg,
		  struct ftl_md_io_entry_ctx *ctx)
{
	struct ftl_md_impl *impl = SPDK_CONTAINEROF(md, struct ftl_md_impl, base);

	if (spdk_unlikely(0 == impl->region->entry_size)) {
		/* This MD has not been configured to support read entry call */
		ftl_abort();
	}

	ctx->cb = cb;
	ctx->cb_arg = cb_arg;
	ctx->md = impl;
	ctx->start_entry = start_entry;
	ctx->buffer = buffer;
	ctx->vss_buffer = vss_buffer;

	_ftl_md_read_entry(ctx);
}

void ftl_md_persist_entry_retry(struct ftl_md_io_entry_ctx *ctx)
{
	_ftl_md_persist_entry(ctx);
}

static void persist_mirror_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_md_impl *primary = md->owner.private;

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

void ftl_md_persist(struct ftl_md *md)
{
	struct ftl_md_impl *impl = SPDK_CONTAINEROF(md, struct ftl_md_impl, base);

	if (has_mirror(impl)) {
		if (setup_mirror(impl)) {
			/* An error when setup the mirror */
			spdk_thread_send_msg(spdk_get_thread(), exception, impl);
			return;
		}

		/* Set callback and context in mirror */
		impl->mirror->base.cb = persist_mirror_cb;
		impl->mirror->base.owner.private = impl;

		/* First persist the mirror */
		ftl_md_persist(&impl->mirror->base);
		return;
	}

	if (0 == io_init(impl, FTL_MD_OP_PERSIST)) {
		io_submit(impl);
	} else {
		spdk_thread_send_msg(spdk_get_thread(), exception, impl);
	}
}

static void restore_mirror_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_md_impl *primary = md->owner.private;

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
		primary->mirror_on = false;
		io_cleanup(primary);
		ftl_md_persist(&primary->base);
		primary->mirror_on = true;
	}
}

static void restore_sync_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_md_impl *primary = md->owner.private;

	if (status) {
		/* Cannot sync the object from the primary to the mirror, mark error and fail */
		primary->io.status = -EIO;
		io_done(primary);
	} else {
		primary->base.cb(dev, &primary->base, primary->io.status);
		io_cleanup(primary);
	}
}

static int restore_done(struct ftl_md_impl *md)
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
			md->mirror->base.cb = restore_mirror_cb;
			md->mirror->base.owner.private = md;

			/* First persist the mirror */
			ftl_md_restore(&md->mirror->base);
			return -EAGAIN;
		} else {
			return -EIO;
		}
	} else if (0 == md->io.status && false == md->dev->sb->clean) {
		if (has_mirror(md)) {
			/* There was a dirty shutdown, synchronize primary to mirror */

			/* Set callback and context in the mirror */
			md->mirror->base.cb = restore_sync_cb;
			md->mirror->base.owner.private = md;

			/* First persist the mirror */
			ftl_md_persist(&md->mirror->base);
			return -EAGAIN;
		}
	}

	return md->io.status;
}

static void io_done(struct ftl_md_impl *md)
{
	int status;

	if (md->io.op == FTL_MD_OP_RESTORE) {
		status = restore_done(md);
	} else {
		status = md->io.status;
	}

	if (status != -EAGAIN) {
		md->base.cb(md->dev, &md->base, status);
		io_cleanup(md);
	}
}

void ftl_md_restore(struct ftl_md *md)
{
	struct ftl_md_impl *impl = SPDK_CONTAINEROF(md, struct ftl_md_impl, base);

	if (0 == io_init(impl, FTL_MD_OP_RESTORE)) {
		io_submit(impl);
	} else {
		spdk_thread_send_msg(spdk_get_thread(), exception, impl);
	}
}

static int pattern_prepare(struct ftl_md_impl *md,
			   const void *pattern, uint64_t pattern_size, union ftl_md_vss *vss_pattern)
{
	uint64_t i;
	void *data = md->io.data;
	uint64_t data_size = xfer_size(md);

	/* Check if pattern size is aligned to IO transfer size */
	if ((pattern_size > data_size) || (data_size % pattern_size)) {
		return -1;
	}

	for (i = 0; i < data_size; i += pattern_size, data += pattern_size) {
		assert(data + pattern_size <= md->io.data + data_size);
		memcpy(data, pattern, pattern_size);
	}

	if (md->io.meta) {
		if (vss_pattern) {
			// store the VSS pattern...
			ftl_md_vss_buf_init(md->io.meta, ftl_md_xfer_blocks(md->dev), vss_pattern);
		} else {
			// ...or default init VSS to 0
			union ftl_md_vss vss = {0};
			vss.version.md_version = md->region->current.version;
			ftl_md_vss_buf_init(md->io.meta, ftl_md_xfer_blocks(md->dev), &vss);
		}
	}

	return 0;
}

static void clear_mirror_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_md_impl *secondary = SPDK_CONTAINEROF(md, struct ftl_md_impl, base);
	struct ftl_md_impl *primary = md->owner.private;

	if (status) {
		/* We got an error, stop persist procedure immediately */
		primary->io.status = status;
		io_done(primary);
	} else {
		struct ftl_md_impl *impl = SPDK_CONTAINEROF(md, struct ftl_md_impl, base);
		/* Now continue the persist procedure on the primary MD object */
		if (0 == io_init(primary, FTL_MD_OP_CLEAR) && 0 == pattern_prepare(primary, secondary->io.data,
				xfer_size(secondary), impl->io.meta)) {
			io_submit(primary);
		} else {
			spdk_thread_send_msg(spdk_get_thread(), exception, primary);
		}
	}
}

void ftl_md_clear(struct ftl_md *md, const void *pattern, uint64_t size,
			union ftl_md_vss *vss_pattern)
{
	struct ftl_md_impl *impl = SPDK_CONTAINEROF(md, struct ftl_md_impl, base);

	if (has_mirror(impl)) {
		if (setup_mirror(impl)) {
			/* An error when setup the mirror */
			spdk_thread_send_msg(spdk_get_thread(), exception, impl);
			return;
		}

		/* Set callback and context in mirror */
		impl->mirror->base.cb = clear_mirror_cb;
		impl->mirror->base.owner.private = impl;

		/* First persist the mirror */
		ftl_md_clear(&impl->mirror->base, pattern, size, vss_pattern);
		return;
	}

	if (0 == io_init(impl, FTL_MD_OP_CLEAR) && 0 == pattern_prepare(impl, pattern, size, vss_pattern)) {
		io_submit(impl);
	} else {
		spdk_thread_send_msg(spdk_get_thread(), exception, impl);
	}
}

const struct ftl_layout_region *ftl_md_get_region(struct ftl_md *md)
{
	struct ftl_md_impl *impl = SPDK_CONTAINEROF(md, struct ftl_md_impl, base);

	return impl->region;
}

int ftl_md_set_region(struct ftl_md *md,
		      const struct ftl_layout_region *region)
{
	struct ftl_md_impl *impl = SPDK_CONTAINEROF(md, struct ftl_md_impl, base);

	assert(region->current.blocks <= impl->data_blocks);
	impl->region = region;

	if (impl->vss_data) {
		union ftl_md_vss vss = {0};
		vss.version.md_version = region->current.version;
		ftl_md_vss_buf_init(impl->vss_data, impl->data_blocks, &vss);
	}

	if (has_mirror(impl)) {
		return setup_mirror(impl);
	}

	return 0;
}
