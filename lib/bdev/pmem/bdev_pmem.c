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

#include "bdev_pmem.h"
#include "spdk/bdev.h"
#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/copy_engine.h"
#include "spdk/io_channel.h"
#include "spdk/string.h"
#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"
#include "libpmemblk.h"

struct pmem_disk {
	struct spdk_bdev	disk;
	PMEMblkpool *pool;
	struct pmem_disk	*next;
};

struct pmem_task {
	int				num_outstanding;
	enum spdk_bdev_io_status	status;
};

static struct pmem_disk *g_pmem_disk_head = NULL;

int pmem_disk_count = 0;

static int bdev_pmem_initialize(void);
static void bdev_pmem_finish(void);

static int
bdev_pmem_get_ctx_size(void)
{
	return sizeof(struct pmem_task) + spdk_copy_task_size();
}

SPDK_BDEV_MODULE_REGISTER(pmem, bdev_pmem_initialize, bdev_pmem_finish,
			  NULL, bdev_pmem_get_ctx_size, NULL)

static void
bdev_pmem_delete_from_list(struct pmem_disk *pmem_disk)
{
	struct pmem_disk *prev = NULL;
	struct pmem_disk *node = g_pmem_disk_head;

	if (pmem_disk == NULL)
		return;

	while (node != NULL) {
		if (node == pmem_disk) {
			if (prev != NULL) {
				prev->next = pmem_disk->next;
			} else {
				g_pmem_disk_head = pmem_disk->next;
			}
			break;
		}
		prev = node;
		node = node->next;
	}
}

static void free_pmem_disk(struct pmem_disk *pdisk)
{
	if (!pdisk) {
		return;
	}

	free(pdisk->disk.name);
	pmemblk_close(pdisk->pool);
	spdk_dma_free(pdisk);
}

static int
bdev_pmem_destruct(void *ctx)
{
	struct pmem_disk *pdisk = ctx;
	bdev_pmem_delete_from_list(pdisk);
	free_pmem_disk(pdisk);
	return 0;
}

static int
bdev_pmem_check_iov_len(struct iovec *iovs, int iovcnt, size_t nbytes)
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
bdev_pmem_readv(struct pmem_disk *pdisk, struct spdk_io_channel *ch,
		struct pmem_task *task,
		struct iovec *iov, int iovcnt, uint64_t offset_blocks, size_t num_blocks, uint32_t block_size)
{
	int i;
	size_t len;

	/* TODO: case with iovs[0].iov_base == NULL */

	if (bdev_pmem_check_iov_len(iov, iovcnt, num_blocks * block_size)) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task),
				      SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
	SPDK_DEBUGLOG(SPDK_TRACE_BDEV_PMEM, "read %lu bytes from offset %#lx\n",
		      num_blocks, offset_blocks);

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	task->num_outstanding = iovcnt;

	for (i = 0; i < iovcnt; i++) {
		len = iov[i].iov_len;

		assert((len % block_size) == 0);

		while (len > 0) {
			pmemblk_read(pdisk->pool, iov[i].iov_base, offset_blocks);
			len -= block_size;
			offset_blocks += 1;
			num_blocks -= 1;
		}
	}
	assert(num_blocks == 0);
}

static void
bdev_pmem_writev(struct pmem_disk *pdisk, struct spdk_io_channel *ch,
		 struct pmem_task *task,
		 struct iovec *iov, int iovcnt, uint64_t offset_blocks, size_t num_blocks, uint32_t block_size)
{

	int i;
	size_t len;

	if (bdev_pmem_check_iov_len(iov, iovcnt, num_blocks * block_size)) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task),
				      SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	SPDK_DEBUGLOG(SPDK_TRACE_BDEV_PMEM, "wrote %lu bytes to offset %#lx\n",
		      len, offset);


	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	task->num_outstanding = iovcnt;

	for (i = 0; i < iovcnt; i++) {
		len = iov[i].iov_len;

		assert((len % block_size) == 0);

		while (len > 0) {
			pmemblk_write(pdisk->pool, iov[i].iov_base, offset_blocks);
			len -= block_size;
			offset_blocks += 1;
			num_blocks -= 1;
		}
	}
	assert(num_blocks == 0);
}

static int
bdev_pmem_unmap(struct pmem_disk *pdisk, struct spdk_io_channel *ch,
		struct pmem_task *task,
		uint64_t offset_blocks,	uint64_t num_blocks, uint32_t block_size)
{
	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	task->num_outstanding = 1;

	while(num_blocks > 0) {
		pmemblk_set_zero(pdisk->pool, offset_blocks);
		offset_blocks +=1;
		num_blocks -=1;	
	}

	return 0;
}

static int64_t
bdev_pmem_flush(struct pmem_disk *pdisk, struct pmem_task *task,
		uint64_t offset_blocks, uint64_t num_blocks, uint32_t block_size)
{
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task), SPDK_BDEV_IO_STATUS_SUCCESS);

	return 0;
}

static int
bdev_pmem_reset(struct pmem_disk *pdisk, struct pmem_task *task)
{
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task), SPDK_BDEV_IO_STATUS_SUCCESS);

	return 0;
}


static void bdev_pmem_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		bdev_pmem_readv((struct pmem_disk *)bdev_io->bdev->ctxt,
				ch,
				(struct pmem_task *)bdev_io->driver_ctx,
				bdev_io->u.read.iovs,
				bdev_io->u.read.iovcnt,
				bdev_io->u.read.offset_blocks,
				bdev_io->u.read.num_blocks,
				bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		bdev_pmem_writev((struct pmem_disk *)bdev_io->bdev->ctxt,
				 ch,
				 (struct pmem_task *)bdev_io->driver_ctx,
				 bdev_io->u.write.iovs,
				 bdev_io->u.write.iovcnt,
				 bdev_io->u.write.offset_blocks,
				 bdev_io->u.write.num_blocks,
				 bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		bdev_pmem_flush((struct pmem_disk *)bdev_io->bdev->ctxt,
				(struct pmem_task *)bdev_io->driver_ctx,
				bdev_io->u.flush.offset_blocks,
				bdev_io->u.flush.num_blocks,
				bdev_io->bdev->blocklen);
		break;

	case SPDK_BDEV_IO_TYPE_RESET:
		bdev_pmem_reset((struct pmem_disk *)bdev_io->bdev->ctxt,
				(struct pmem_task *)bdev_io->driver_ctx);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		bdev_pmem_unmap((struct pmem_disk *)bdev_io->bdev->ctxt,
				ch,
				(struct pmem_task *)bdev_io->driver_ctx,
				bdev_io->u.unmap.offset_blocks,
				bdev_io->u.unmap.num_blocks,
				bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		bdev_pmem_unmap((struct pmem_disk *)bdev_io->bdev->ctxt,
				ch,
				(struct pmem_task *)bdev_io->driver_ctx,
				bdev_io->u.write.offset_blocks,
				bdev_io->u.write.num_blocks,
				bdev_io->bdev->blocklen);
		break;

	default:
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
bdev_pmem_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
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
bdev_pmem_get_io_channel(void *ctx)
{
	return spdk_copy_engine_get_io_channel();
}

static const struct spdk_bdev_fn_table pmem_fn_table = {
	.destruct		= bdev_pmem_destruct,
	.submit_request		= bdev_pmem_submit_request,
	.io_type_supported	= bdev_pmem_io_type_supported,
	.get_io_channel		= bdev_pmem_get_io_channel,
};

struct spdk_bdev *create_pmem_disk(char *pmem_file)
{
	uint64_t num_blocks;
	uint32_t block_size;
	struct pmem_disk *pdisk;

	pdisk = spdk_dma_zmalloc(sizeof(*pdisk), 0, NULL);

	if (!pdisk) {
		perror("pdisk");
		return NULL;
	}

	/* TODO: do we need extra consistency check?
	 * rc = pmemblk_check(pmem_file, 0);
	 */

	pdisk->pool = pmemblk_open(pmem_file, 0);

	if (!pdisk->pool) {
		SPDK_ERRLOG("Opening pmem pool %s failed, error %d\n", pmem_file, errno);
		spdk_dma_free(pdisk);
		return NULL;
	}

	block_size = pmemblk_bsize(pdisk->pool);
	num_blocks = pmemblk_nblock(pdisk->pool);

	SPDK_DEBUGLOG(SPDK_TRACE_BDEV_PMEM, "Pool %s opened with bsize %d and nblock %d\n",
		      pmem_file, block_size, num_blocks);

	if (num_blocks == 0) {
		SPDK_ERRLOG("Disk must be more than 0 blocks\n");
		return NULL;
	}

	pdisk->disk.name = spdk_sprintf_alloc("pmem%d", pmem_disk_count);

	if (!pdisk->disk.name) {
		pmemblk_close(pdisk->pool);
		spdk_dma_free(pdisk);
		return NULL;
	}

	pdisk->disk.product_name = "pmemblk disk";
	pmem_disk_count++;

	pdisk->disk.write_cache = 1;
	pdisk->disk.blocklen = block_size;
	pdisk->disk.blockcnt = num_blocks;

	pdisk->disk.ctxt = pdisk;
	pdisk->disk.fn_table = &pmem_fn_table;
	pdisk->disk.module = SPDK_GET_BDEV_MODULE(pmem);

	spdk_bdev_register(&pdisk->disk);

	pdisk->next = g_pmem_disk_head;
	g_pmem_disk_head = pdisk;


	return &pdisk->disk;
}

static int bdev_pmem_initialize(void)
{
	return 0;

}

static void bdev_pmem_finish(void)
{
	struct pmem_disk *pdisk;

	while (g_pmem_disk_head != NULL) {
		pdisk = g_pmem_disk_head;
		g_pmem_disk_head = pdisk->next;
		free_pmem_disk(pdisk);
	}
}

SPDK_LOG_REGISTER_TRACE_FLAG("bdev_pmem", SPDK_TRACE_BDEV_PMEM)
