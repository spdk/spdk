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

#include <stdio.h>
#include <errno.h>
#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>

#include "blockdev_malloc.h"
#include "spdk/bdev.h"
#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/log.h"
#include "spdk/copy_engine.h"
#include "spdk/io_channel.h"

#include "bdev_module.h"

#define MALLOC_MAX_UNMAP_BDESC	1

struct malloc_disk {
	struct spdk_bdev	disk;	/* this must be the first element */
	void 			*malloc_buf;
	struct malloc_disk	*next;
};

static void
malloc_done(void *ref, int status)
{
	struct copy_task *cp_task = (struct copy_task *)ref;
	enum spdk_bdev_io_status bdev_status;

	if (status != 0) {
		bdev_status = SPDK_BDEV_IO_STATUS_FAILED;
	} else {
		bdev_status = SPDK_BDEV_IO_STATUS_SUCCESS;
	}
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(cp_task), bdev_status);
}

static struct malloc_disk *g_malloc_disk_head = NULL;

int malloc_disk_count = 0;

static int blockdev_malloc_initialize(void);
static void blockdev_malloc_finish(void);
static void blockdev_malloc_get_spdk_running_config(FILE *fp);

static int
blockdev_malloc_get_ctx_size(void)
{
	return spdk_copy_module_get_max_ctx_size();
}

SPDK_BDEV_MODULE_REGISTER(blockdev_malloc_initialize, blockdev_malloc_finish,
			  blockdev_malloc_get_spdk_running_config, blockdev_malloc_get_ctx_size)

static void
blockdev_malloc_delete_from_list(struct malloc_disk *malloc_disk)
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

static int
blockdev_malloc_destruct(struct spdk_bdev *bdev)
{
	struct malloc_disk *malloc_disk = (struct malloc_disk *)bdev;
	blockdev_malloc_delete_from_list(malloc_disk);
	rte_free(malloc_disk->malloc_buf);
	rte_free(malloc_disk);
	return 0;
}

static int64_t
blockdev_malloc_read(struct malloc_disk *mdisk, struct spdk_io_channel *ch,
		     struct copy_task *copy_req,
		     void *buf, uint64_t nbytes, off_t offset)
{
	SPDK_TRACELOG(SPDK_TRACE_MALLOC, "read %lu bytes from offset %#lx to %p\n",
		      nbytes, offset, buf);

	return spdk_copy_submit(copy_req, ch, buf, mdisk->malloc_buf + offset,
				nbytes, malloc_done);
}

static int64_t
blockdev_malloc_writev(struct malloc_disk *mdisk, struct spdk_io_channel *ch,
		       struct copy_task *copy_req,
		       struct iovec *iov, int iovcnt, size_t len, off_t offset)
{
	if ((iovcnt != 1) || (iov->iov_len != len))
		return -1;

	SPDK_TRACELOG(SPDK_TRACE_MALLOC, "wrote %lu bytes to offset %#lx from %p\n",
		      iov->iov_len, offset, iov->iov_base);

	return spdk_copy_submit(copy_req, ch, mdisk->malloc_buf + offset,
				iov->iov_base, len, malloc_done);
}

static int
blockdev_malloc_unmap(struct malloc_disk *mdisk,
		      struct spdk_io_channel *ch,
		      struct copy_task *copy_req,
		      struct spdk_scsi_unmap_bdesc *unmap_d,
		      uint16_t bdesc_count)
{
	uint64_t lba, offset, byte_count;
	uint32_t block_count;

	assert(bdesc_count <= MALLOC_MAX_UNMAP_BDESC);

	/*
	 * For now, only support a single unmap descriptor per command. The copy engine API does not
	 * support batch submission of operations.
	 */
	assert(bdesc_count == 1);

	lba = from_be64(&unmap_d[0].lba);
	offset = lba * mdisk->disk.blocklen;
	block_count = from_be32(&unmap_d[0].block_count);
	byte_count = (uint64_t)block_count * mdisk->disk.blocklen;

	if (lba >= mdisk->disk.blockcnt || block_count > mdisk->disk.blockcnt - lba) {
		return -1;
	}

	return spdk_copy_submit_fill(copy_req, ch, mdisk->malloc_buf + offset, 0, byte_count, malloc_done);
}

static int64_t
blockdev_malloc_flush(struct malloc_disk *mdisk, struct copy_task *copy_req,
		      uint64_t offset, uint64_t nbytes)
{
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(copy_req), SPDK_BDEV_IO_STATUS_SUCCESS);

	return 0;
}

static int
blockdev_malloc_reset(struct malloc_disk *mdisk, struct copy_task *copy_req)
{
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(copy_req), SPDK_BDEV_IO_STATUS_SUCCESS);

	return 0;
}

static int _blockdev_malloc_submit_request(struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		if (bdev_io->u.read.buf == NULL) {
			bdev_io->u.read.buf = ((struct malloc_disk *)bdev_io->ctx)->malloc_buf +
					      bdev_io->u.read.offset;
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_io->driver_ctx),
					      SPDK_BDEV_IO_STATUS_SUCCESS);
			return 0;
		}

		return blockdev_malloc_read((struct malloc_disk *)bdev_io->ctx,
					    bdev_io->ch,
					    (struct copy_task *)bdev_io->driver_ctx,
					    bdev_io->u.read.buf,
					    bdev_io->u.read.nbytes,
					    bdev_io->u.read.offset);

	case SPDK_BDEV_IO_TYPE_WRITE:
		return blockdev_malloc_writev((struct malloc_disk *)bdev_io->ctx,
					      bdev_io->ch,
					      (struct copy_task *)bdev_io->driver_ctx,
					      bdev_io->u.write.iovs,
					      bdev_io->u.write.iovcnt,
					      bdev_io->u.write.len,
					      bdev_io->u.write.offset);

	case SPDK_BDEV_IO_TYPE_RESET:
		return blockdev_malloc_reset((struct malloc_disk *)bdev_io->ctx,
					     (struct copy_task *)bdev_io->driver_ctx);

	case SPDK_BDEV_IO_TYPE_FLUSH:
		return blockdev_malloc_flush((struct malloc_disk *)bdev_io->ctx,
					     (struct copy_task *)bdev_io->driver_ctx,
					     bdev_io->u.flush.offset,
					     bdev_io->u.flush.length);

	case SPDK_BDEV_IO_TYPE_UNMAP:
		return blockdev_malloc_unmap((struct malloc_disk *)bdev_io->ctx,
					     bdev_io->ch,
					     (struct copy_task *)bdev_io->driver_ctx,
					     bdev_io->u.unmap.unmap_bdesc,
					     bdev_io->u.unmap.bdesc_count);
	default:
		return -1;
	}
	return 0;
}

static void blockdev_malloc_submit_request(struct spdk_bdev_io *bdev_io)
{
	if (_blockdev_malloc_submit_request(bdev_io) < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
blockdev_malloc_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		return true;

	default:
		return false;
	}
}

static struct spdk_io_channel *
blockdev_malloc_get_io_channel(struct spdk_bdev *bdev, uint32_t priority)
{
	return spdk_copy_engine_get_io_channel(priority);
}

static const struct spdk_bdev_fn_table malloc_fn_table = {
	.destruct		= blockdev_malloc_destruct,
	.submit_request		= blockdev_malloc_submit_request,
	.io_type_supported	= blockdev_malloc_io_type_supported,
	.get_io_channel		= blockdev_malloc_get_io_channel,
};

struct malloc_disk *create_malloc_disk(uint64_t num_blocks, uint32_t block_size)
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

	mdisk = rte_malloc(NULL, sizeof(*mdisk), 0);
	if (!mdisk) {
		perror("mdisk");
		return NULL;
	}

	memset(mdisk, 0, sizeof(*mdisk));

	/*
	 * Allocate the large backend memory buffer using rte_malloc(),
	 *  so that we guarantee it is allocated from hugepage memory.
	 *
	 * TODO: need to pass a hint so we know which socket to allocate
	 *  from on multi-socket systems.
	 */
	mdisk->malloc_buf = rte_zmalloc(NULL, num_blocks * block_size, 2 * 1024 * 1024);
	if (!mdisk->malloc_buf) {
		SPDK_ERRLOG("rte_zmalloc failed\n");
		rte_free(mdisk);
		return NULL;
	}

	snprintf(mdisk->disk.name, SPDK_BDEV_MAX_NAME_LENGTH, "Malloc%d", malloc_disk_count);
	snprintf(mdisk->disk.product_name, SPDK_BDEV_MAX_PRODUCT_NAME_LENGTH, "Malloc disk");
	malloc_disk_count++;

	mdisk->disk.write_cache = 1;
	mdisk->disk.blocklen = block_size;
	mdisk->disk.blockcnt = num_blocks;
	mdisk->disk.thin_provisioning = 1;
	mdisk->disk.max_unmap_bdesc_count = MALLOC_MAX_UNMAP_BDESC;

	mdisk->disk.ctxt = mdisk;
	mdisk->disk.fn_table = &malloc_fn_table;

	spdk_bdev_register(&mdisk->disk);

	mdisk->next = g_malloc_disk_head;
	g_malloc_disk_head = mdisk;

	return mdisk;
}

static void free_malloc_disk(struct malloc_disk *mdisk)
{
	rte_free(mdisk->malloc_buf);
	rte_free(mdisk);
}

static int blockdev_malloc_initialize(void)
{
	struct spdk_conf_section *sp = spdk_conf_find_section(NULL, "Malloc");
	int NumberOfLuns, LunSizeInMB, BlockSize, i;
	uint64_t size;
	struct malloc_disk *mdisk;

	if (sp != NULL) {
		NumberOfLuns = spdk_conf_section_get_intval(sp, "NumberOfLuns");
		LunSizeInMB = spdk_conf_section_get_intval(sp, "LunSizeInMB");
		BlockSize = spdk_conf_section_get_intval(sp, "BlockSize");
		if ((NumberOfLuns < 1) || (LunSizeInMB < 1)) {
			SPDK_ERRLOG("Malloc section present, but no devices specified\n");
			return EINVAL;
		}
		if (BlockSize < 1) {
			/* Default is 512 bytes */
			BlockSize = 512;
		}
		size = (uint64_t)LunSizeInMB * 1024 * 1024;
		for (i = 0; i < NumberOfLuns; i++) {
			mdisk = create_malloc_disk(size / BlockSize, BlockSize);
			if (mdisk == NULL) {
				SPDK_ERRLOG("Could not create malloc disk\n");
				return EINVAL;
			}
		}
	}
	return 0;
}

static void blockdev_malloc_finish(void)
{
	struct malloc_disk *mdisk;

	while (g_malloc_disk_head != NULL) {
		mdisk = g_malloc_disk_head;
		g_malloc_disk_head = mdisk->next;
		free_malloc_disk(mdisk);
	}
}

static void
blockdev_malloc_get_spdk_running_config(FILE *fp)
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

SPDK_LOG_REGISTER_TRACE_FLAG("malloc", SPDK_TRACE_MALLOC)
