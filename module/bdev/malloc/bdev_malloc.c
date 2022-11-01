/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "bdev_malloc.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/accel.h"
#include "spdk/string.h"

#include "spdk/log.h"

struct malloc_disk {
	struct spdk_bdev		disk;
	void				*malloc_buf;
	void				*malloc_md_buf;
	TAILQ_ENTRY(malloc_disk)	link;
};

struct malloc_task {
	int				num_outstanding;
	enum spdk_bdev_io_status	status;
	TAILQ_ENTRY(malloc_task)	tailq;
};

struct malloc_channel {
	struct spdk_io_channel		*accel_channel;
	struct spdk_poller		*completion_poller;
	TAILQ_HEAD(, malloc_task)	completed_tasks;
};

static int
malloc_verify_pi(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct spdk_dif_ctx dif_ctx;
	struct spdk_dif_error err_blk;
	int rc;

	rc = spdk_dif_ctx_init(&dif_ctx,
			       bdev->blocklen,
			       bdev->md_len,
			       bdev->md_interleave,
			       bdev->dif_is_head_of_md,
			       bdev->dif_type,
			       bdev->dif_check_flags,
			       bdev_io->u.bdev.offset_blocks & 0xFFFFFFFF,
			       0xFFFF, 0, 0, 0);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to initialize DIF/DIX context\n");
		return rc;
	}

	if (spdk_bdev_is_md_interleaved(bdev)) {
		rc = spdk_dif_verify(bdev_io->u.bdev.iovs,
				     bdev_io->u.bdev.iovcnt,
				     bdev_io->u.bdev.num_blocks,
				     &dif_ctx,
				     &err_blk);
	} else {
		struct iovec md_iov = {
			.iov_base	= bdev_io->u.bdev.md_buf,
			.iov_len	= bdev_io->u.bdev.num_blocks * bdev->md_len,
		};

		rc = spdk_dix_verify(bdev_io->u.bdev.iovs,
				     bdev_io->u.bdev.iovcnt,
				     &md_iov,
				     bdev_io->u.bdev.num_blocks,
				     &dif_ctx,
				     &err_blk);
	}

	if (rc != 0) {
		SPDK_ERRLOG("DIF/DIX verify failed: lba %" PRIu64 ", num_blocks %" PRIu64 ", "
			    "err_type %u, expected %u, actual %u, err_offset %u\n",
			    bdev_io->u.bdev.offset_blocks,
			    bdev_io->u.bdev.num_blocks,
			    err_blk.err_type,
			    err_blk.expected,
			    err_blk.actual,
			    err_blk.err_offset);
	}

	return rc;
}

static void
malloc_done(void *ref, int status)
{
	struct malloc_task *task = (struct malloc_task *)ref;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(task);
	int rc;

	if (status != 0) {
		if (status == -ENOMEM) {
			task->status = SPDK_BDEV_IO_STATUS_NOMEM;
		} else {
			task->status = SPDK_BDEV_IO_STATUS_FAILED;
		}
	}

	if (--task->num_outstanding != 0) {
		return;
	}

	if (bdev_io->bdev->dif_type != SPDK_DIF_DISABLE &&
	    bdev_io->type == SPDK_BDEV_IO_TYPE_READ &&
	    task->status == SPDK_BDEV_IO_STATUS_SUCCESS) {
		rc = malloc_verify_pi(bdev_io);
		if (rc != 0) {
			task->status = SPDK_BDEV_IO_STATUS_FAILED;
		}
	}

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task), task->status);
}

static void
malloc_complete_task(struct malloc_task *task, struct malloc_channel *mch,
		     enum spdk_bdev_io_status status)
{
	task->status = status;
	TAILQ_INSERT_TAIL(&mch->completed_tasks, task, tailq);
}

static TAILQ_HEAD(, malloc_disk) g_malloc_disks = TAILQ_HEAD_INITIALIZER(g_malloc_disks);

int malloc_disk_count = 0;

static int bdev_malloc_initialize(void);
static void bdev_malloc_deinitialize(void);

static int
bdev_malloc_get_ctx_size(void)
{
	return sizeof(struct malloc_task);
}

static struct spdk_bdev_module malloc_if = {
	.name = "malloc",
	.module_init = bdev_malloc_initialize,
	.module_fini = bdev_malloc_deinitialize,
	.get_ctx_size = bdev_malloc_get_ctx_size,

};

SPDK_BDEV_MODULE_REGISTER(malloc, &malloc_if)

static void
malloc_disk_free(struct malloc_disk *malloc_disk)
{
	if (!malloc_disk) {
		return;
	}

	free(malloc_disk->disk.name);
	spdk_free(malloc_disk->malloc_buf);
	spdk_free(malloc_disk->malloc_md_buf);
	free(malloc_disk);
}

static int
bdev_malloc_destruct(void *ctx)
{
	struct malloc_disk *malloc_disk = ctx;

	TAILQ_REMOVE(&g_malloc_disks, malloc_disk, link);
	malloc_disk_free(malloc_disk);
	return 0;
}

static int
bdev_malloc_check_iov_len(struct iovec *iovs, int iovcnt, size_t nbytes)
{
	int i;

	for (i = 0; i < iovcnt; i++) {
		if (nbytes < iovs[i].iov_len) {
			return 0;
		}

		nbytes -= iovs[i].iov_len;
	}

	return nbytes != 0;
}

static void
bdev_malloc_readv(struct malloc_disk *mdisk, struct spdk_io_channel *ch,
		  struct malloc_task *task,
		  struct iovec *iov, int iovcnt, size_t len, uint64_t offset,
		  void *md_buf, size_t md_len, uint64_t md_offset)
{
	int64_t res = 0;
	void *src;
	void *md_src;
	int i;

	if (bdev_malloc_check_iov_len(iov, iovcnt, len)) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task),
				      SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	task->num_outstanding = 0;

	SPDK_DEBUGLOG(bdev_malloc, "read %zu bytes from offset %#" PRIx64 ", iovcnt=%d\n",
		      len, offset, iovcnt);

	src = mdisk->malloc_buf + offset;

	for (i = 0; i < iovcnt; i++) {
		task->num_outstanding++;
		res = spdk_accel_submit_copy(ch, iov[i].iov_base,
					     src, iov[i].iov_len, 0, malloc_done, task);

		if (res != 0) {
			malloc_done(task, res);
			break;
		}

		src += iov[i].iov_len;
		len -= iov[i].iov_len;
	}

	if (md_buf == NULL) {
		return;
	}

	SPDK_DEBUGLOG(bdev_malloc, "read metadata %zu bytes from offset%#" PRIx64 "\n",
		      md_len, md_offset);

	md_src = mdisk->malloc_md_buf + md_offset;

	task->num_outstanding++;
	res = spdk_accel_submit_copy(ch, md_buf, md_src, md_len, 0, malloc_done, task);

	if (res != 0) {
		malloc_done(task, res);
	}
}

static void
bdev_malloc_writev(struct malloc_disk *mdisk, struct spdk_io_channel *ch,
		   struct malloc_task *task,
		   struct iovec *iov, int iovcnt, size_t len, uint64_t offset,
		   void *md_buf, size_t md_len, uint64_t md_offset)
{

	int64_t res = 0;
	void *dst;
	void *md_dst;
	int i;

	if (bdev_malloc_check_iov_len(iov, iovcnt, len)) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task),
				      SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	SPDK_DEBUGLOG(bdev_malloc, "wrote %zu bytes to offset %#" PRIx64 ", iovcnt=%d\n",
		      len, offset, iovcnt);

	dst = mdisk->malloc_buf + offset;

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	task->num_outstanding = 0;

	for (i = 0; i < iovcnt; i++) {
		task->num_outstanding++;
		res = spdk_accel_submit_copy(ch, dst, iov[i].iov_base,
					     iov[i].iov_len, 0, malloc_done, task);

		if (res != 0) {
			malloc_done(task, res);
			break;
		}

		dst += iov[i].iov_len;
	}

	if (md_buf == NULL) {
		return;
	}
	SPDK_DEBUGLOG(bdev_malloc, "wrote metadata %zu bytes to offset %#" PRIx64 "\n",
		      md_len, md_offset);

	md_dst = mdisk->malloc_md_buf + md_offset;

	task->num_outstanding++;
	res = spdk_accel_submit_copy(ch, md_dst, md_buf, md_len, 0, malloc_done, task);

	if (res != 0) {
		malloc_done(task, res);
	}

}

static int
bdev_malloc_unmap(struct malloc_disk *mdisk,
		  struct spdk_io_channel *ch,
		  struct malloc_task *task,
		  uint64_t offset,
		  uint64_t byte_count)
{
	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	task->num_outstanding = 1;

	return spdk_accel_submit_fill(ch, mdisk->malloc_buf + offset, 0,
				      byte_count, 0, malloc_done, task);
}

static void
bdev_malloc_copy(struct malloc_disk *mdisk, struct spdk_io_channel *ch,
		 struct malloc_task *task,
		 uint64_t dst_offset, uint64_t src_offset, size_t len)
{
	int64_t res = 0;
	void *dst = mdisk->malloc_buf + dst_offset;
	void *src = mdisk->malloc_buf + src_offset;

	SPDK_DEBUGLOG(bdev_malloc, "Copy %zu bytes from offset %#" PRIx64 " to offset %#" PRIx64 "\n",
		      len, src_offset, dst_offset);

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	task->num_outstanding = 1;

	res = spdk_accel_submit_copy(ch, dst, src, len, 0, malloc_done, task);
	if (res != 0) {
		malloc_done(task, res);
	}
}

static int
_bdev_malloc_submit_request(struct malloc_channel *mch, struct spdk_bdev_io *bdev_io)
{
	uint32_t block_size = bdev_io->bdev->blocklen;
	uint32_t md_size = bdev_io->bdev->md_len;
	int rc;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		if (bdev_io->u.bdev.iovs[0].iov_base == NULL) {
			assert(bdev_io->u.bdev.iovcnt == 1);
			bdev_io->u.bdev.iovs[0].iov_base =
				((struct malloc_disk *)bdev_io->bdev->ctxt)->malloc_buf +
				bdev_io->u.bdev.offset_blocks * block_size;
			bdev_io->u.bdev.iovs[0].iov_len = bdev_io->u.bdev.num_blocks * block_size;
			malloc_complete_task((struct malloc_task *)bdev_io->driver_ctx, mch,
					     SPDK_BDEV_IO_STATUS_SUCCESS);
			return 0;
		}

		bdev_malloc_readv((struct malloc_disk *)bdev_io->bdev->ctxt,
				  mch->accel_channel,
				  (struct malloc_task *)bdev_io->driver_ctx,
				  bdev_io->u.bdev.iovs,
				  bdev_io->u.bdev.iovcnt,
				  bdev_io->u.bdev.num_blocks * block_size,
				  bdev_io->u.bdev.offset_blocks * block_size,
				  bdev_io->u.bdev.md_buf,
				  bdev_io->u.bdev.num_blocks * md_size,
				  bdev_io->u.bdev.offset_blocks * md_size);
		return 0;

	case SPDK_BDEV_IO_TYPE_WRITE:
		if (bdev_io->bdev->dif_type != SPDK_DIF_DISABLE) {
			rc = malloc_verify_pi(bdev_io);
			if (rc != 0) {
				malloc_complete_task((struct malloc_task *)bdev_io->driver_ctx, mch,
						     SPDK_BDEV_IO_STATUS_FAILED);
				return 0;
			}
		}

		bdev_malloc_writev((struct malloc_disk *)bdev_io->bdev->ctxt,
				   mch->accel_channel,
				   (struct malloc_task *)bdev_io->driver_ctx,
				   bdev_io->u.bdev.iovs,
				   bdev_io->u.bdev.iovcnt,
				   bdev_io->u.bdev.num_blocks * block_size,
				   bdev_io->u.bdev.offset_blocks * block_size,
				   bdev_io->u.bdev.md_buf,
				   bdev_io->u.bdev.num_blocks * md_size,
				   bdev_io->u.bdev.offset_blocks * md_size);
		return 0;

	case SPDK_BDEV_IO_TYPE_RESET:
		malloc_complete_task((struct malloc_task *)bdev_io->driver_ctx, mch,
				     SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;

	case SPDK_BDEV_IO_TYPE_FLUSH:
		malloc_complete_task((struct malloc_task *)bdev_io->driver_ctx, mch,
				     SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;

	case SPDK_BDEV_IO_TYPE_UNMAP:
		return bdev_malloc_unmap((struct malloc_disk *)bdev_io->bdev->ctxt,
					 mch->accel_channel,
					 (struct malloc_task *)bdev_io->driver_ctx,
					 bdev_io->u.bdev.offset_blocks * block_size,
					 bdev_io->u.bdev.num_blocks * block_size);

	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		/* bdev_malloc_unmap is implemented with a call to mem_cpy_fill which zeroes out all of the requested bytes. */
		return bdev_malloc_unmap((struct malloc_disk *)bdev_io->bdev->ctxt,
					 mch->accel_channel,
					 (struct malloc_task *)bdev_io->driver_ctx,
					 bdev_io->u.bdev.offset_blocks * block_size,
					 bdev_io->u.bdev.num_blocks * block_size);

	case SPDK_BDEV_IO_TYPE_ZCOPY:
		if (bdev_io->u.bdev.zcopy.start) {
			void *buf;
			size_t len;

			buf = ((struct malloc_disk *)bdev_io->bdev->ctxt)->malloc_buf +
			      bdev_io->u.bdev.offset_blocks * block_size;
			len = bdev_io->u.bdev.num_blocks * block_size;
			spdk_bdev_io_set_buf(bdev_io, buf, len);

		}
		malloc_complete_task((struct malloc_task *)bdev_io->driver_ctx, mch,
				     SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;
	case SPDK_BDEV_IO_TYPE_ABORT:
		malloc_complete_task((struct malloc_task *)bdev_io->driver_ctx, mch,
				     SPDK_BDEV_IO_STATUS_FAILED);
		return 0;
	case SPDK_BDEV_IO_TYPE_COPY:
		bdev_malloc_copy((struct malloc_disk *)bdev_io->bdev->ctxt,
				 mch->accel_channel,
				 (struct malloc_task *)bdev_io->driver_ctx,
				 bdev_io->u.bdev.offset_blocks * block_size,
				 bdev_io->u.bdev.copy.src_offset_blocks * block_size,
				 bdev_io->u.bdev.num_blocks * block_size);
		return 0;

	default:
		return -1;
	}
	return 0;
}

static void
bdev_malloc_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct malloc_channel *mch = spdk_io_channel_get_ctx(ch);

	if (_bdev_malloc_submit_request(mch, bdev_io) != 0) {
		malloc_complete_task((struct malloc_task *)bdev_io->driver_ctx, mch,
				     SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
bdev_malloc_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_ZCOPY:
	case SPDK_BDEV_IO_TYPE_ABORT:
	case SPDK_BDEV_IO_TYPE_COPY:
		return true;

	default:
		return false;
	}
}

static struct spdk_io_channel *
bdev_malloc_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_malloc_disks);
}

static void
bdev_malloc_write_json_config(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	char uuid_str[SPDK_UUID_STRING_LEN];

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_malloc_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_uint64(w, "num_blocks", bdev->blockcnt);
	spdk_json_write_named_uint32(w, "block_size", bdev->blocklen);
	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &bdev->uuid);
	spdk_json_write_named_string(w, "uuid", uuid_str);
	spdk_json_write_named_uint32(w, "optimal_io_boundary", bdev->optimal_io_boundary);

	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table malloc_fn_table = {
	.destruct		= bdev_malloc_destruct,
	.submit_request		= bdev_malloc_submit_request,
	.io_type_supported	= bdev_malloc_io_type_supported,
	.get_io_channel		= bdev_malloc_get_io_channel,
	.write_config_json	= bdev_malloc_write_json_config,
};

static int
malloc_disk_setup_pi(struct malloc_disk *mdisk)
{
	struct spdk_bdev *bdev = &mdisk->disk;
	struct spdk_dif_ctx dif_ctx;
	struct iovec iov, md_iov;
	int rc;

	rc = spdk_dif_ctx_init(&dif_ctx,
			       bdev->blocklen,
			       bdev->md_len,
			       bdev->md_interleave,
			       bdev->dif_is_head_of_md,
			       bdev->dif_type,
			       bdev->dif_check_flags,
			       0,	/* configure the whole buffers */
			       0, 0, 0, 0);
	if (rc != 0) {
		SPDK_ERRLOG("Initialization of DIF/DIX context failed\n");
		return rc;
	}

	iov.iov_base = mdisk->malloc_buf;
	iov.iov_len = bdev->blockcnt * bdev->blocklen;

	if (mdisk->disk.md_interleave) {
		rc = spdk_dif_generate(&iov, 1, bdev->blockcnt, &dif_ctx);
	} else {
		md_iov.iov_base = mdisk->malloc_md_buf;
		md_iov.iov_len = bdev->blockcnt * bdev->md_len;

		rc = spdk_dix_generate(&iov, 1, &md_iov, bdev->blockcnt, &dif_ctx);
	}

	if (rc != 0) {
		SPDK_ERRLOG("Formatting by DIF/DIX failed\n");
	}

	return rc;
}

int
create_malloc_disk(struct spdk_bdev **bdev, const struct malloc_bdev_opts *opts)
{
	struct malloc_disk *mdisk;
	uint32_t block_size;
	int rc;

	assert(opts != NULL);

	if (opts->num_blocks == 0) {
		SPDK_ERRLOG("Disk num_blocks must be greater than 0");
		return -EINVAL;
	}

	if (opts->block_size % 512) {
		SPDK_ERRLOG("Data block size must be 512 bytes aligned\n");
		return -EINVAL;
	}

	switch (opts->md_size) {
	case 0:
	case 8:
	case 16:
	case 32:
	case 64:
	case 128:
		break;
	default:
		SPDK_ERRLOG("metadata size %u is not supported\n", opts->md_size);
		return -EINVAL;
	}

	if (opts->md_interleave) {
		block_size = opts->block_size + opts->md_size;
	} else {
		block_size = opts->block_size;
	}

	if (opts->dif_type < SPDK_DIF_DISABLE || opts->dif_type > SPDK_DIF_TYPE3) {
		SPDK_ERRLOG("DIF type is invalid\n");
		return -EINVAL;
	}

	if (opts->dif_type != SPDK_DIF_DISABLE && opts->md_size == 0) {
		SPDK_ERRLOG("Metadata size should not be zero if DIF is enabled\n");
		return -EINVAL;
	}

	mdisk = calloc(1, sizeof(*mdisk));
	if (!mdisk) {
		SPDK_ERRLOG("mdisk calloc() failed\n");
		return -ENOMEM;
	}

	/*
	 * Allocate the large backend memory buffer from pinned memory.
	 *
	 * TODO: need to pass a hint so we know which socket to allocate
	 *  from on multi-socket systems.
	 */
	mdisk->malloc_buf = spdk_zmalloc(opts->num_blocks * block_size, 2 * 1024 * 1024, NULL,
					 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!mdisk->malloc_buf) {
		SPDK_ERRLOG("malloc_buf spdk_zmalloc() failed\n");
		malloc_disk_free(mdisk);
		return -ENOMEM;
	}

	if (!opts->md_interleave && opts->md_size != 0) {
		mdisk->malloc_md_buf = spdk_zmalloc(opts->num_blocks * opts->md_size, 2 * 1024 * 1024, NULL,
						    SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (!mdisk->malloc_md_buf) {
			SPDK_ERRLOG("malloc_md_buf spdk_zmalloc() failed\n");
			malloc_disk_free(mdisk);
			return -ENOMEM;
		}
	}

	if (opts->name) {
		mdisk->disk.name = strdup(opts->name);
	} else {
		/* Auto-generate a name */
		mdisk->disk.name = spdk_sprintf_alloc("Malloc%d", malloc_disk_count);
		malloc_disk_count++;
	}
	if (!mdisk->disk.name) {
		malloc_disk_free(mdisk);
		return -ENOMEM;
	}
	mdisk->disk.product_name = "Malloc disk";

	mdisk->disk.write_cache = 1;
	mdisk->disk.blocklen = block_size;
	mdisk->disk.blockcnt = opts->num_blocks;
	mdisk->disk.md_len = opts->md_size;
	mdisk->disk.md_interleave = opts->md_interleave;
	mdisk->disk.dif_type = opts->dif_type;
	mdisk->disk.dif_is_head_of_md = opts->dif_is_head_of_md;
	/* Current block device layer API does not propagate
	 * any DIF related information from user. So, we can
	 * not generate or verify Application Tag.
	 */
	switch (opts->dif_type) {
	case SPDK_DIF_TYPE1:
	case SPDK_DIF_TYPE2:
		mdisk->disk.dif_check_flags = SPDK_DIF_FLAGS_GUARD_CHECK |
					      SPDK_DIF_FLAGS_REFTAG_CHECK;
		break;
	case SPDK_DIF_TYPE3:
		mdisk->disk.dif_check_flags = SPDK_DIF_FLAGS_GUARD_CHECK;
		break;
	case SPDK_DIF_DISABLE:
		break;
	}

	if (opts->dif_type != SPDK_DIF_DISABLE) {
		rc = malloc_disk_setup_pi(mdisk);
		if (rc) {
			SPDK_ERRLOG("Failed to set up protection information.\n");
			malloc_disk_free(mdisk);
			return rc;
		}
	}

	if (opts->optimal_io_boundary) {
		mdisk->disk.optimal_io_boundary = opts->optimal_io_boundary;
		mdisk->disk.split_on_optimal_io_boundary = true;
	}
	if (!spdk_mem_all_zero(&opts->uuid, sizeof(opts->uuid))) {
		spdk_uuid_copy(&mdisk->disk.uuid, &opts->uuid);
	} else {
		spdk_uuid_generate(&mdisk->disk.uuid);
	}

	mdisk->disk.max_copy = 0;
	mdisk->disk.ctxt = mdisk;
	mdisk->disk.fn_table = &malloc_fn_table;
	mdisk->disk.module = &malloc_if;

	rc = spdk_bdev_register(&mdisk->disk);
	if (rc) {
		malloc_disk_free(mdisk);
		return rc;
	}

	*bdev = &(mdisk->disk);

	TAILQ_INSERT_TAIL(&g_malloc_disks, mdisk, link);

	return rc;
}

void
delete_malloc_disk(const char *name, spdk_delete_malloc_complete cb_fn, void *cb_arg)
{
	int rc;

	rc = spdk_bdev_unregister_by_name(name, &malloc_if, cb_fn, cb_arg);
	if (rc != 0) {
		cb_fn(cb_arg, rc);
	}
}

static int
malloc_completion_poller(void *ctx)
{
	struct malloc_channel *ch = ctx;
	struct malloc_task *task;
	TAILQ_HEAD(, malloc_task) completed_tasks;
	uint32_t num_completions = 0;

	TAILQ_INIT(&completed_tasks);
	TAILQ_SWAP(&completed_tasks, &ch->completed_tasks, malloc_task, tailq);

	while (!TAILQ_EMPTY(&completed_tasks)) {
		task = TAILQ_FIRST(&completed_tasks);
		TAILQ_REMOVE(&completed_tasks, task, tailq);
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task), task->status);
		num_completions++;
	}

	return num_completions > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static int
malloc_create_channel_cb(void *io_device, void *ctx)
{
	struct malloc_channel *ch = ctx;

	ch->accel_channel = spdk_accel_get_io_channel();
	if (!ch->accel_channel) {
		SPDK_ERRLOG("Failed to get accel framework's IO channel\n");
		return -ENOMEM;
	}

	ch->completion_poller = SPDK_POLLER_REGISTER(malloc_completion_poller, ch, 0);
	if (!ch->completion_poller) {
		SPDK_ERRLOG("Failed to register malloc completion poller\n");
		spdk_put_io_channel(ch->accel_channel);
		return -ENOMEM;
	}

	TAILQ_INIT(&ch->completed_tasks);

	return 0;
}

static void
malloc_destroy_channel_cb(void *io_device, void *ctx)
{
	struct malloc_channel *ch = ctx;

	assert(TAILQ_EMPTY(&ch->completed_tasks));

	spdk_put_io_channel(ch->accel_channel);
	spdk_poller_unregister(&ch->completion_poller);
}

static int
bdev_malloc_initialize(void)
{
	/* This needs to be reset for each reinitialization of submodules.
	 * Otherwise after enough devices or reinitializations the value gets too high.
	 * TODO: Make malloc bdev name mandatory and remove this counter. */
	malloc_disk_count = 0;

	spdk_io_device_register(&g_malloc_disks, malloc_create_channel_cb,
				malloc_destroy_channel_cb, sizeof(struct malloc_channel),
				"bdev_malloc");

	return 0;
}

static void
bdev_malloc_deinitialize(void)
{
	spdk_io_device_unregister(&g_malloc_disks, NULL);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_malloc)
