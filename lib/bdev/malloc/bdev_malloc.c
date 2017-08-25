/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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

#include "spdk/stdinc.h"

#include "bdev_malloc.h"
#include "spdk/bdev.h"
#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/copy_engine.h"
#include "spdk/io_channel.h"
#include "spdk/string.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

struct malloc_disk {
	struct spdk_bdev	disk;
	void 			*malloc_buf;
	struct malloc_disk	*next;
};

struct malloc_task {
	int				num_outstanding;
	enum spdk_bdev_io_status	status;
};

static struct malloc_task *
__malloc_task_from_copy_task(struct spdk_copy_task *ct)
{
	return (struct malloc_task *)((uintptr_t)ct - sizeof(struct malloc_task));
}

static struct spdk_copy_task *
__copy_task_from_malloc_task(struct malloc_task *mt)
{
	return (struct spdk_copy_task *)((uintptr_t)mt + sizeof(struct malloc_task));
}

static void
malloc_done(void *ref, int status)
{
	struct malloc_task *task = __malloc_task_from_copy_task(ref);

	if (status != 0) {
		task->status = SPDK_BDEV_IO_STATUS_FAILED;
	}

	if (--task->num_outstanding == 0) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task), task->status);
	}
}

static struct malloc_disk *g_malloc_disk_head = NULL;

int malloc_disk_count = 0;

static int bdev_malloc_initialize(void);
static void bdev_malloc_finish(void);
static void bdev_malloc_get_spdk_running_config(FILE *fp);

static int
bdev_malloc_get_ctx_size(void)
{
	return sizeof(struct malloc_task) + spdk_copy_task_size();
}

SPDK_BDEV_MODULE_REGISTER(malloc, bdev_malloc_initialize, bdev_malloc_finish,
			  bdev_malloc_get_spdk_running_config, bdev_malloc_get_ctx_size, NULL)

static void
bdev_malloc_delete_from_list(struct malloc_disk *malloc_disk)
{
	struct malloc_disk *prev = NULL;
	struct malloc_disk *node = g_malloc_disk_head;

	if (malloc_disk == NULL)
		return;

	while (node != NULL) {
		if (node == malloc_disk) {
			if (prev != NULL) {
				prev->next = malloc_disk->next;
			} else {
				g_malloc_disk_head = malloc_disk->next;
			}
			break;
		}
		prev = node;
		node = node->next;
	}
}

static void
malloc_disk_free(struct malloc_disk *malloc_disk)
{
	if (!malloc_disk) {
		return;
	}

	free(malloc_disk->disk.name);
	spdk_dma_free(malloc_disk->malloc_buf);
	spdk_dma_free(malloc_disk);
}

static int
bdev_malloc_destruct(void *ctx)
{
	struct malloc_disk *malloc_disk = ctx;
	bdev_malloc_delete_from_list(malloc_disk);
	malloc_disk_free(malloc_disk);
	return 0;
}

static int
bdev_malloc_check_iov_len(struct iovec *iovs, int iovcnt, size_t nbytes)
{
	int i;

	for (i = 0; i < iovcnt; i++) {
		if (nbytes < iovs[i].iov_len)
			return 0;

		nbytes -= iovs[i].iov_len;
	}

	return nbytes != 0;
}

static void
bdev_malloc_readv(struct malloc_disk *mdisk, struct spdk_io_channel *ch,
		  struct malloc_task *task,
		  struct iovec *iov, int iovcnt, size_t len, uint64_t offset)
{
	int64_t res = 0;
	void *src = mdisk->malloc_buf + offset;
	int i;

	if (bdev_malloc_check_iov_len(iov, iovcnt, len)) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task),
				      SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	SPDK_DEBUGLOG(SPDK_TRACE_BDEV_MALLOC, "read %lu bytes from offset %#lx\n",
		      len, offset);

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	task->num_outstanding = iovcnt;

	for (i = 0; i < iovcnt; i++) {
		res = spdk_copy_submit(__copy_task_from_malloc_task(task),
				       ch, iov[i].iov_base,
				       src, iov[i].iov_len, malloc_done);

		if (res != (int64_t)iov[i].iov_len) {
			malloc_done(__copy_task_from_malloc_task(task), -1);
		}

		src += iov[i].iov_len;
		len -= iov[i].iov_len;
	}
}

static void
bdev_malloc_writev(struct malloc_disk *mdisk, struct spdk_io_channel *ch,
		   struct malloc_task *task,
		   struct iovec *iov, int iovcnt, size_t len, uint64_t offset)
{
	int64_t res = 0;
	void *dst = mdisk->malloc_buf + offset;
	int i;

	if (bdev_malloc_check_iov_len(iov, iovcnt, len)) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task),
				      SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	SPDK_DEBUGLOG(SPDK_TRACE_BDEV_MALLOC, "wrote %lu bytes to offset %#lx\n",
		      len, offset);

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	task->num_outstanding = iovcnt;

	for (i = 0; i < iovcnt; i++) {
		res = spdk_copy_submit(__copy_task_from_malloc_task(task),
				       ch, dst, iov[i].iov_base,
				       iov[i].iov_len, malloc_done);

		if (res != (int64_t)iov[i].iov_len) {
			malloc_done(__copy_task_from_malloc_task(task), -1);
		}

		dst += iov[i].iov_len;
		len -= iov[i].iov_len;
	}
}

static int
bdev_malloc_unmap(struct malloc_disk *mdisk,
		  struct spdk_io_channel *ch,
		  struct malloc_task *task,
		  uint64_t offset,
		  uint64_t byte_count)
{
	uint64_t lba;
	uint32_t block_count;

	lba = offset / mdisk->disk.blocklen;
	block_count = byte_count / mdisk->disk.blocklen;

	if (lba >= mdisk->disk.blockcnt || block_count > mdisk->disk.blockcnt - lba) {
		return -1;
	}

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	task->num_outstanding = 1;

	return spdk_copy_submit_fill(__copy_task_from_malloc_task(task), ch,
				     mdisk->malloc_buf + offset, 0, byte_count, malloc_done);
}

static int64_t
bdev_malloc_flush(struct malloc_disk *mdisk, struct malloc_task *task,
		  uint64_t offset, uint64_t nbytes)
{
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task), SPDK_BDEV_IO_STATUS_SUCCESS);

	return 0;
}

static int
bdev_malloc_reset(struct malloc_disk *mdisk, struct malloc_task *task)
{
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task), SPDK_BDEV_IO_STATUS_SUCCESS);

	return 0;
}

static int _bdev_malloc_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		if (bdev_io->u.read.iovs[0].iov_base == NULL) {
			assert(bdev_io->u.read.iovcnt == 1);
			bdev_io->u.read.iovs[0].iov_base =
				((struct malloc_disk *)bdev_io->bdev->ctxt)->malloc_buf +
				bdev_io->u.read.offset;
			bdev_io->u.read.iovs[0].iov_len = bdev_io->u.read.len;
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_io->driver_ctx),
					      SPDK_BDEV_IO_STATUS_SUCCESS);
			return 0;
		}

		bdev_malloc_readv((struct malloc_disk *)bdev_io->bdev->ctxt,
				  ch,
				  (struct malloc_task *)bdev_io->driver_ctx,
				  bdev_io->u.read.iovs,
				  bdev_io->u.read.iovcnt,
				  bdev_io->u.read.len,
				  bdev_io->u.read.offset);
		return 0;

	case SPDK_BDEV_IO_TYPE_WRITE:
		bdev_malloc_writev((struct malloc_disk *)bdev_io->bdev->ctxt,
				   ch,
				   (struct malloc_task *)bdev_io->driver_ctx,
				   bdev_io->u.write.iovs,
				   bdev_io->u.write.iovcnt,
				   bdev_io->u.write.len,
				   bdev_io->u.write.offset);
		return 0;

	case SPDK_BDEV_IO_TYPE_RESET:
		return bdev_malloc_reset((struct malloc_disk *)bdev_io->bdev->ctxt,
					 (struct malloc_task *)bdev_io->driver_ctx);

	case SPDK_BDEV_IO_TYPE_FLUSH:
		return bdev_malloc_flush((struct malloc_disk *)bdev_io->bdev->ctxt,
					 (struct malloc_task *)bdev_io->driver_ctx,
					 bdev_io->u.flush.offset,
					 bdev_io->u.flush.len);

	case SPDK_BDEV_IO_TYPE_UNMAP:
		return bdev_malloc_unmap((struct malloc_disk *)bdev_io->bdev->ctxt,
					 ch,
					 (struct malloc_task *)bdev_io->driver_ctx,
					 bdev_io->u.unmap.offset,
					 bdev_io->u.unmap.len);

	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		/* bdev_malloc_unmap is implemented with a call to mem_cpy_fill which zeroes out all of the requested bytes. */
		return bdev_malloc_unmap((struct malloc_disk *)bdev_io->bdev->ctxt,
					 ch,
					 (struct malloc_task *)bdev_io->driver_ctx,
					 bdev_io->u.write.offset,
					 bdev_io->u.write.len);

	default:
		return -1;
	}
	return 0;
}

static void bdev_malloc_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	if (_bdev_malloc_submit_request(ch, bdev_io) < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
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
		return true;

	default:
		return false;
	}
}

static struct spdk_io_channel *
bdev_malloc_get_io_channel(void *ctx)
{
	return spdk_copy_engine_get_io_channel();
}

static const struct spdk_bdev_fn_table malloc_fn_table = {
	.destruct		= bdev_malloc_destruct,
	.submit_request		= bdev_malloc_submit_request,
	.io_type_supported	= bdev_malloc_io_type_supported,
	.get_io_channel		= bdev_malloc_get_io_channel,
};

struct spdk_bdev *create_malloc_disk(uint64_t num_blocks, uint32_t block_size)
{
	struct malloc_disk	*mdisk;

	if (block_size % 512 != 0) {
		SPDK_ERRLOG("Block size %u is not a multiple of 512.\n", block_size);
		return NULL;
	}

	if (num_blocks == 0) {
		SPDK_ERRLOG("Disk must be more than 0 blocks\n");
		return NULL;
	}

	mdisk = spdk_dma_zmalloc(sizeof(*mdisk), 0, NULL);
	if (!mdisk) {
		perror("mdisk");
		return NULL;
	}

	/*
	 * Allocate the large backend memory buffer from pinned memory.
	 *
	 * TODO: need to pass a hint so we know which socket to allocate
	 *  from on multi-socket systems.
	 */
	mdisk->malloc_buf = spdk_dma_zmalloc(num_blocks * block_size, 2 * 1024 * 1024, NULL);
	if (!mdisk->malloc_buf) {
		SPDK_ERRLOG("spdk_dma_zmalloc failed\n");
		malloc_disk_free(mdisk);
		return NULL;
	}

	mdisk->disk.name = spdk_sprintf_alloc("Malloc%d", malloc_disk_count);
	if (!mdisk->disk.name) {
		malloc_disk_free(mdisk);
		return NULL;
	}
	mdisk->disk.product_name = "Malloc disk";
	malloc_disk_count++;

	mdisk->disk.write_cache = 1;
	mdisk->disk.blocklen = block_size;
	mdisk->disk.blockcnt = num_blocks;

	mdisk->disk.ctxt = mdisk;
	mdisk->disk.fn_table = &malloc_fn_table;
	mdisk->disk.module = SPDK_GET_BDEV_MODULE(malloc);

	spdk_bdev_register(&mdisk->disk);

	mdisk->next = g_malloc_disk_head;
	g_malloc_disk_head = mdisk;

	return &mdisk->disk;
}

static void free_malloc_disk(struct malloc_disk *mdisk)
{
	free(mdisk->disk.name);
	spdk_dma_free(mdisk->malloc_buf);
	spdk_dma_free(mdisk);
}

static int bdev_malloc_initialize(void)
{
	struct spdk_conf_section *sp = spdk_conf_find_section(NULL, "Malloc");
	int NumberOfLuns, LunSizeInMB, BlockSize, i, rc = 0;
	uint64_t size;
	struct spdk_bdev *bdev;

	if (sp != NULL) {
		NumberOfLuns = spdk_conf_section_get_intval(sp, "NumberOfLuns");
		LunSizeInMB = spdk_conf_section_get_intval(sp, "LunSizeInMB");
		BlockSize = spdk_conf_section_get_intval(sp, "BlockSize");
		if ((NumberOfLuns < 1) || (LunSizeInMB < 1)) {
			SPDK_ERRLOG("Malloc section present, but no devices specified\n");
			rc = EINVAL;
			goto end;
		}
		if (BlockSize < 1) {
			/* Default is 512 bytes */
			BlockSize = 512;
		}
		size = (uint64_t)LunSizeInMB * 1024 * 1024;
		for (i = 0; i < NumberOfLuns; i++) {
			bdev = create_malloc_disk(size / BlockSize, BlockSize);
			if (bdev == NULL) {
				SPDK_ERRLOG("Could not create malloc disk\n");
				rc = EINVAL;
				goto end;
			}
		}
	}

end:
	return rc;
}

static void bdev_malloc_finish(void)
{
	struct malloc_disk *mdisk;

	while (g_malloc_disk_head != NULL) {
		mdisk = g_malloc_disk_head;
		g_malloc_disk_head = mdisk->next;
		free_malloc_disk(mdisk);
	}
}

static void
bdev_malloc_get_spdk_running_config(FILE *fp)
{
	int num_malloc_luns = 0;
	uint64_t malloc_lun_size = 0;

	/* count number of malloc LUNs, get LUN size */
	struct malloc_disk *mdisk = g_malloc_disk_head;
	while (mdisk != NULL) {
		if (0 == malloc_lun_size) {
			/* assume all malloc luns the same size */
			malloc_lun_size = mdisk->disk.blocklen * mdisk->disk.blockcnt;
			malloc_lun_size /= (1024 * 1024);
		}
		num_malloc_luns++;
		mdisk = mdisk->next;
	}

	if (num_malloc_luns > 0) {
		fprintf(fp,
			"\n"
			"# Users may change this section to create a different number or size of\n"
			"# malloc LUNs.\n"
			"# This will generate %d LUNs with a malloc-allocated backend. Each LUN \n"
			"# will be %" PRIu64 "MB in size and these will be named Malloc0 through Malloc%d.\n"
			"# Not all LUNs defined here are necessarily used below.\n"
			"[Malloc]\n"
			"  NumberOfLuns %d\n"
			"  LunSizeInMB %" PRIu64 "\n",
			num_malloc_luns, malloc_lun_size,
			num_malloc_luns - 1, num_malloc_luns,
			malloc_lun_size);
	}
}

SPDK_LOG_REGISTER_TRACE_FLAG("bdev_malloc", SPDK_TRACE_BDEV_MALLOC)
