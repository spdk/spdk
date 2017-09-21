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

#include "spdk/conf.h"
#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk/util.h"
#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

#include "bdev_pmem.h"
#include "libpmemblk.h"

struct pmem_disk {
	struct spdk_bdev	disk;
	PMEMblkpool *pool;
	TAILQ_ENTRY(pmem_disk) tailq;
};

TAILQ_HEAD(, pmem_disk) g_pmem_disks = TAILQ_HEAD_INITIALIZER(g_pmem_disks);

static int pmem_disk_count = 0;

static int bdev_pmem_initialize(void);
static void bdev_pmem_finish(void);

static int
bdev_pmem_get_ctx_size(void)
{
	return 0;
}

SPDK_BDEV_MODULE_REGISTER(pmem, bdev_pmem_initialize, bdev_pmem_finish,
			  NULL, bdev_pmem_get_ctx_size, NULL)


static int
bdev_pmem_destruct(void *ctx)
{
	struct pmem_disk *pdisk = ctx;

	TAILQ_REMOVE(&g_pmem_disks, pdisk, tailq);
	free(pdisk->disk.name);
	pmemblk_close(pdisk->pool);
	free(pdisk);

	return 0;
}

static int
bdev_pmem_check_iov_len(struct iovec *iovs, int iovcnt, size_t num_blocks, uint32_t block_size)
{
	size_t nbytes = num_blocks * block_size;
	int i;

	for (i = 0; i < iovcnt; i++) {
		if (spdk_unlikely(iovs[i].iov_base == NULL && iovs[i].iov_len != 0)) {
			return -1;
		}

		if (nbytes <= iovs[i].iov_len) {
			return 0;
		}

		if (spdk_unlikely(iovs[i].iov_len % block_size != 0)) {
			return -1;
		}

		nbytes -= iovs[i].iov_len;
	}

	return -1;
}

static int
bdev_pmem_readv(struct pmem_disk *pdisk, struct spdk_io_channel *ch, struct iovec *iov, int iovcnt,
		uint64_t offset_blocks, size_t num_blocks, uint32_t block_size)
{
	size_t nbytes, offset, len;
	int rc;
	size_t offset_len = offset_blocks;

	rc = bdev_pmem_check_iov_len(iov, iovcnt, num_blocks, block_size);
	if (rc) {
		return SPDK_BDEV_IO_STATUS_FAILED;
	}

	SPDK_DEBUGLOG(SPDK_TRACE_BDEV_PMEM, "read %lu bytes from offset %#lx\n",
		      num_blocks, offset_blocks);

	for (nbytes = num_blocks * block_size; nbytes > 0; iov++) {
		len = spdk_min(iov->iov_len, nbytes);
		nbytes -= len;

		offset = 0;
		while (offset != len) {
			rc = pmemblk_read(pdisk->pool, iov->iov_base + offset, offset_len);
			if (rc != 0) {
				SPDK_ERRLOG("pmemblk read failed: %d (%s)\n", errno, pmemblk_errormsg());
				return SPDK_BDEV_IO_STATUS_FAILED;
			}

			offset += block_size;
			offset_len++;
		}
	}

	assert(num_blocks == offset_len - offset_blocks);
	return SPDK_BDEV_IO_STATUS_SUCCESS;
}

static int
bdev_pmem_writev(struct pmem_disk *pdisk, struct spdk_io_channel *ch, struct iovec *iov, int iovcnt,
		 uint64_t offset_blocks, size_t num_blocks, uint32_t block_size)
{
	size_t nbytes, offset, len;
	int rc;
	size_t offset_len = offset_blocks;

	if (bdev_pmem_check_iov_len(iov, iovcnt, num_blocks, block_size)) {
		return SPDK_BDEV_IO_STATUS_FAILED;
	}

	SPDK_DEBUGLOG(SPDK_TRACE_BDEV_PMEM, "write %lu bytes to offset %#lx\n",
		      num_blocks, offset_blocks);

	for (nbytes = num_blocks * block_size; nbytes > 0; iov++) {
		len = spdk_min(iov->iov_len, nbytes);
		nbytes -= len;

		offset = 0;
		while (offset != len) {
			rc = pmemblk_write(pdisk->pool, iov->iov_base + offset, offset_len);
			if (rc != 0) {
				SPDK_ERRLOG("pmemblk write failed: %d (%s)\n", errno, pmemblk_errormsg());
				return SPDK_BDEV_IO_STATUS_FAILED;
			}

			offset += block_size;
			offset_len++;
		}
	}

	assert(num_blocks == offset_len - offset_blocks);
	return SPDK_BDEV_IO_STATUS_SUCCESS;
}

static int
bdev_pmem_write_zeros(struct pmem_disk *pdisk, struct spdk_io_channel *ch, uint64_t offset_blocks,
		      uint64_t num_blocks, uint32_t block_size)
{
	uint64_t last_block = offset_blocks + num_blocks;
	int rc;

	for (; offset_blocks != last_block; offset_blocks++) {
		rc = pmemblk_set_zero(pdisk->pool, offset_blocks);
		if (rc != 0) {
			SPDK_ERRLOG("pmemblk_set_zero failed: %d (%s)\n", errno, pmemblk_errormsg());
			return SPDK_BDEV_IO_STATUS_FAILED;
		}
	}

	return SPDK_BDEV_IO_STATUS_SUCCESS;
}

static int
bdev_pmem_flush(struct pmem_disk *pdisk, uint64_t offset_blocks, uint64_t num_blocks,
		uint32_t block_size)
{
	return SPDK_BDEV_IO_STATUS_SUCCESS;
}

static int
bdev_pmem_reset(struct pmem_disk *pdisk)
{
	return SPDK_BDEV_IO_STATUS_SUCCESS;
}


static void
bdev_pmem_submit_request(struct spdk_io_channel *channel, struct spdk_bdev_io *bdev_io)
{
	struct pmem_disk *pdisk = bdev_io->bdev->ctxt;
	enum spdk_bdev_io_status status;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		status = bdev_pmem_readv(pdisk,
					 channel,
					 bdev_io->u.read.iovs,
					 bdev_io->u.read.iovcnt,
					 bdev_io->u.read.offset_blocks,
					 bdev_io->u.read.num_blocks,
					 bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		status = bdev_pmem_writev(pdisk,
					  channel,
					  bdev_io->u.write.iovs,
					  bdev_io->u.write.iovcnt,
					  bdev_io->u.write.offset_blocks,
					  bdev_io->u.write.num_blocks,
					  bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		status = bdev_pmem_flush(pdisk,
					 bdev_io->u.flush.offset_blocks,
					 bdev_io->u.flush.num_blocks,
					 bdev_io->bdev->blocklen);
		break;

	case SPDK_BDEV_IO_TYPE_RESET:
		status = bdev_pmem_reset(pdisk);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		status = bdev_pmem_write_zeros(pdisk,
					       channel,
					       bdev_io->u.unmap.offset_blocks,
					       bdev_io->u.unmap.num_blocks,
					       bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		status = bdev_pmem_write_zeros(pdisk,
					       channel,
					       bdev_io->u.write.offset_blocks,
					       bdev_io->u.write.num_blocks,
					       bdev_io->bdev->blocklen);
		break;

	default:
		status = SPDK_BDEV_IO_STATUS_FAILED;
	}
	spdk_bdev_io_complete(bdev_io, status);
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
	return spdk_get_io_channel(&g_pmem_disks);
}

static int
bdev_pmem_create_cb(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
bdev_pmem_destroy_cb(void *io_device, void *ctx_buf)
{
}

static const struct spdk_bdev_fn_table pmem_fn_table = {
	.destruct		= bdev_pmem_destruct,
	.submit_request		= bdev_pmem_submit_request,
	.io_type_supported	= bdev_pmem_io_type_supported,
	.get_io_channel		= bdev_pmem_get_io_channel,
};

struct spdk_bdev *
create_pmem_disk(const char *pmem_file)
{
	uint64_t num_blocks;
	uint32_t block_size;
	struct pmem_disk *pdisk;

	if (pmemblk_check(pmem_file, 0) != 1) {
		SPDK_ERRLOG("Pool '%s' check failed: %s\n", pmem_file, pmemblk_errormsg());
		errno = EIO;
		return NULL;
	}

	pdisk = calloc(1, sizeof(*pdisk));
	if (!pdisk) {
		errno = ENOMEM;
		return NULL;
	}

	pdisk->pool = pmemblk_open(pmem_file, 0);
	if (!pdisk->pool) {
		SPDK_ERRLOG("Opening pmem pool '%s' failed: %d\n", pmem_file, errno);
		free(pdisk);
		return NULL;
	}

	block_size = pmemblk_bsize(pdisk->pool);
	num_blocks = pmemblk_nblock(pdisk->pool);

	if (block_size == 0) {
		SPDK_ERRLOG("Block size must be more than 0 bytes\n");
		pmemblk_close(pdisk->pool);
		free(pdisk);
		errno = EINVAL;
		return NULL;
	}

	if (num_blocks == 0) {
		SPDK_ERRLOG("Disk must be more than 0 blocks\n");
		pmemblk_close(pdisk->pool);
		free(pdisk);
		errno = EINVAL;
		return NULL;
	}


	pdisk->disk.name = spdk_sprintf_alloc("pmem%d", pmem_disk_count);

	if (!pdisk->disk.name) {
		pmemblk_close(pdisk->pool);
		free(pdisk);
		errno = ENOMEM;
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

	TAILQ_INSERT_TAIL(&g_pmem_disks, pdisk, tailq);


	return &pdisk->disk;
}

static int
bdev_pmem_initialize(void)
{
	const char *err = pmemblk_check_version(PMEMBLK_MAJOR_VERSION, PMEMBLK_MINOR_VERSION);

	if (err != NULL) {
		SPDK_ERRLOG("Invalid libpmemblk version (expected %d.%d): %s\n", PMEMBLK_MAJOR_VERSION,
			    PMEMBLK_MINOR_VERSION, err);
		return -1;
	}

	spdk_io_device_register(&g_pmem_disks, bdev_pmem_create_cb, bdev_pmem_destroy_cb, 0);

	return 0;

}

static void
bdev_pmem_finish(void)
{
	struct pmem_disk *pdisk, *tmp;

	TAILQ_FOREACH_SAFE(pdisk, &g_pmem_disks, tailq, tmp) {
		bdev_pmem_destruct(pdisk);
	}

	spdk_io_device_unregister(&g_pmem_disks, NULL);
}

SPDK_LOG_REGISTER_TRACE_FLAG("bdev_pmem", SPDK_TRACE_BDEV_PMEM)
