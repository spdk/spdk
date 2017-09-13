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

/*
static struct pmem_task *
__pmem_task_from_copy_task(struct spdk_copy_task *ct)
{
	return (struct pmem_task *)((uintptr_t)ct - sizeof(struct pmem_task));
}

static struct spdk_copy_task *
__copy_task_from_pmem_task(struct pmem_task *mt)
{
	return (struct spdk_copy_task *)((uintptr_t)mt + sizeof(struct pmem_task));
}
static void
malloc_done(void *ref, int status)
{
	struct pmem_task *task = __pmem_task_from_copy_task(ref);

	if (status != 0) {
		task->status = SPDK_BDEV_IO_STATUS_FAILED;
	}

	if (--task->num_outstanding == 0) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task), task->status);
	}
}
*/

static struct pmem_disk *g_pmem_disk_head = NULL;

int pmem_disk_count = 0;

static int bdev_pmem_initialize(void);
static void bdev_pmem_finish(void);
static void bdev_pmem_get_spdk_running_config(FILE *fp);

static int
bdev_pmem_get_ctx_size(void)
{
	return sizeof(struct pmem_task) + spdk_copy_task_size();
}

SPDK_BDEV_MODULE_REGISTER(pmem, bdev_pmem_initialize, bdev_pmem_finish,
			  bdev_pmem_get_spdk_running_config, bdev_pmem_get_ctx_size, NULL)

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

static int
bdev_pmem_destruct(void *ctx)
{
	struct pmem_disk *pdisk = ctx;

	if (!pdisk) {
		return;
	}

	bdev_pmem_delete_from_list(pdisk);

	pmemblk_close(pdisk->pool);
	free(pdisk->disk.name);
	spdk_dma_free(pdisk);

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
bdev_pmem_readv(struct pmem_disk *mdisk, struct spdk_io_channel *ch,
		struct pmem_task *task,
		struct iovec *iov, int iovcnt, size_t len, uint64_t offset)
{
//	int64_t res = 0;
//	void *src = mdisk->malloc_buf + offset;
	int i;

	if (bdev_pmem_check_iov_len(iov, iovcnt, len)) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task),
				      SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	SPDK_DEBUGLOG(SPDK_TRACE_BDEV_PMEM, "read %lu bytes from offset %#lx\n",
		      len, offset);

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	task->num_outstanding = iovcnt;

	for (i = 0; i < iovcnt; i++) {

//		pmemblk copy
		/*
		res = spdk_copy_submit(__copy_task_from_pmem_task(task),
				       ch, iov[i].iov_base,
				       src, iov[i].iov_len, malloc_done);

		if (res != (int64_t)iov[i].iov_len) {
			malloc_done(__copy_task_from_pmem_task(task), -1);
		}
		src += iov[i].iov_len;
		len -= iov[i].iov_len;
*/
	}
}

static void
bdev_pmem_writev(struct pmem_disk *mdisk, struct spdk_io_channel *ch,
		 struct pmem_task *task,
		 struct iovec *iov, int iovcnt, size_t len, uint64_t offset)
{
//	int64_t res = 0;
//	void *dst = mdisk->malloc_buf + offset;
	int i;

	if (bdev_pmem_check_iov_len(iov, iovcnt, len)) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task),
				      SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	SPDK_DEBUGLOG(SPDK_TRACE_BDEV_PMEM, "wrote %lu bytes to offset %#lx\n",
		      len, offset);

	task->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	task->num_outstanding = iovcnt;

	for (i = 0; i < iovcnt; i++) {
		/* pmemblk copy
		res = spdk_copy_submit(__copy_task_from_pmem_task(task),
				       ch, dst, iov[i].iov_base,
				       iov[i].iov_len, malloc_done);

		if (res != (int64_t)iov[i].iov_len) {
			malloc_done(__copy_task_from_pmem_task(task), -1);
		}
		dst += iov[i].iov_len;
		len -= iov[i].iov_len;
*/
	}
}

static int
bdev_pmem_unmap(struct pmem_disk *mdisk,
		struct spdk_io_channel *ch,
		struct pmem_task *task,
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
	/*
	return spdk_copy_submit_fill(__copy_task_from_pmem_task(task), ch,
				     mdisk->malloc_buf + offset, 0, byte_count, malloc_done);
	*/
	return 0;
}

static int64_t
bdev_pmem_flush(struct pmem_disk *mdisk, struct pmem_task *task,
		uint64_t offset, uint64_t nbytes)
{
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task), SPDK_BDEV_IO_STATUS_SUCCESS);

	return 0;
}

static int
bdev_pmem_reset(struct pmem_disk *mdisk, struct pmem_task *task)
{
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(task), SPDK_BDEV_IO_STATUS_SUCCESS);

	return 0;
}

static int _bdev_pmem_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	uint32_t block_size = bdev_io->bdev->blocklen;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		if (bdev_io->u.read.iovs[0].iov_base == NULL) {
			// zero copy path
/*
			assert(bdev_io->u.read.iovcnt == 1);
			bdev_io->u.read.iovs[0].iov_base =
				((struct pmem_disk *)bdev_io->bdev->ctxt)->malloc_buf +
				bdev_io->u.read.offset_blocks * block_size;
			bdev_io->u.read.iovs[0].iov_len = bdev_io->u.read.num_blocks * block_size;
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bdev_io->driver_ctx),
					      SPDK_BDEV_IO_STATUS_SUCCESS);
			return 0;
*/
		}

		bdev_pmem_readv((struct pmem_disk *)bdev_io->bdev->ctxt,
				ch,
				(struct pmem_task *)bdev_io->driver_ctx,
				bdev_io->u.read.iovs,
				bdev_io->u.read.iovcnt,
				bdev_io->u.read.num_blocks * block_size,
				bdev_io->u.read.offset_blocks * block_size);
		return 0;

	case SPDK_BDEV_IO_TYPE_WRITE:
		bdev_pmem_writev((struct pmem_disk *)bdev_io->bdev->ctxt,
				 ch,
				 (struct pmem_task *)bdev_io->driver_ctx,
				 bdev_io->u.write.iovs,
				 bdev_io->u.write.iovcnt,
				 bdev_io->u.write.num_blocks * block_size,
				 bdev_io->u.write.offset_blocks * block_size);
		return 0;

	case SPDK_BDEV_IO_TYPE_RESET:
		return bdev_pmem_reset((struct pmem_disk *)bdev_io->bdev->ctxt,
				       (struct pmem_task *)bdev_io->driver_ctx);

	case SPDK_BDEV_IO_TYPE_FLUSH:
		return bdev_pmem_flush((struct pmem_disk *)bdev_io->bdev->ctxt,
				       (struct pmem_task *)bdev_io->driver_ctx,
				       bdev_io->u.flush.offset_blocks * block_size,
				       bdev_io->u.flush.num_blocks * block_size);

	case SPDK_BDEV_IO_TYPE_UNMAP:
		return bdev_pmem_unmap((struct pmem_disk *)bdev_io->bdev->ctxt,
				       ch,
				       (struct pmem_task *)bdev_io->driver_ctx,
				       bdev_io->u.unmap.offset_blocks * block_size,
				       bdev_io->u.unmap.num_blocks * block_size);

	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		/* bdev_pmem_unmap is implemented with a call to mem_cpy_fill which zeroes out all of the requested bytes. */
		return bdev_pmem_unmap((struct pmem_disk *)bdev_io->bdev->ctxt,
				       ch,
				       (struct pmem_task *)bdev_io->driver_ctx,
				       bdev_io->u.write.offset_blocks * block_size,
				       bdev_io->u.write.num_blocks * block_size);

	default:
		return -1;
	}
	return 0;
}

static void bdev_pmem_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	if (_bdev_pmem_submit_request(ch, bdev_io) < 0) {
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

struct spdk_bdev *create_pmem_disk(char* pmem_file)
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
	rc = pmemblk_check(pmem_file, 0);
	*/
	
	pdisk->pool = pmemblk_open(pmem_file, 0);
	
	if (!pdisk->pool) {
		SPDK_ERRLOG("Opening pmem pool %s failed, error %d\n", pmem_file, errno);
		free(pdisk);
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
		free(pdisk);
		pmemblk_close(pdisk->pool);
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

static void free_pmem_disk(struct pmem_disk *mdisk)
{
	free(mdisk->disk.name);
//	spdk_dma_free(mdisk->malloc_buf);
	spdk_dma_free(mdisk);
}

static int bdev_pmem_initialize(void)
{
#ifdef NEVER
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
			bdev = create_pmem_disk(size / BlockSize, BlockSize);
			if (bdev == NULL) {
				SPDK_ERRLOG("Could not create malloc disk\n");
				rc = EINVAL;
				goto end;
			}
		}
	}

end:
	return rc;
#endif
	return 0;

}

static void bdev_pmem_finish(void)
{
	struct pmem_disk *mdisk;

	while (g_pmem_disk_head != NULL) {
		mdisk = g_pmem_disk_head;
		g_pmem_disk_head = mdisk->next;
		free_pmem_disk(mdisk);
	}
}

static void
bdev_pmem_get_spdk_running_config(FILE *fp)
{
	int num_malloc_luns = 0;
	uint64_t malloc_lun_size = 0;

	/* count number of malloc LUNs, get LUN size */
	struct pmem_disk *mdisk = g_pmem_disk_head;
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

SPDK_LOG_REGISTER_TRACE_FLAG("bdev_pmem", SPDK_TRACE_BDEV_PMEM)
