/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk/util.h"
#include "spdk/rpc.h"
#include "spdk/bdev_module.h"
#include "spdk/log.h"
#include "spdk/config.h"

#include "bdev_pmem.h"
#include "libpmemblk.h"

struct pmem_disk {
	struct spdk_bdev	disk;
	PMEMblkpool *pool;
	char pmem_file[NAME_MAX];
	TAILQ_ENTRY(pmem_disk) tailq;
};

static TAILQ_HEAD(, pmem_disk) g_pmem_disks = TAILQ_HEAD_INITIALIZER(g_pmem_disks);

static int bdev_pmem_initialize(void);
static void bdev_pmem_finish(void);

static struct spdk_bdev_module pmem_if = {
	.name = "pmem",
	.module_init = bdev_pmem_initialize,
	.module_fini = bdev_pmem_finish,
	.async_fini = true,

};

SPDK_BDEV_MODULE_REGISTER(pmem, &pmem_if)

typedef int(*spdk_bdev_pmem_io_request)(PMEMblkpool *pbp, void *buf, long long blockno);

static int
_bdev_pmem_submit_io_read(PMEMblkpool *pbp, void *buf, long long blockno)
{
	return pmemblk_read(pbp, buf, blockno);
}

static int
_bdev_pmem_submit_io_write(PMEMblkpool *pbp, void *buf, long long blockno)
{
	return pmemblk_write(pbp, buf, blockno);
}

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

static void
bdev_pmem_submit_io(struct spdk_bdev_io *bdev_io, struct pmem_disk *pdisk,
		    struct spdk_io_channel *ch,
		    struct iovec *iov, int iovcnt,
		    uint64_t offset_blocks, size_t num_blocks, uint32_t block_size,
		    spdk_bdev_pmem_io_request fn)
{
	int rc;
	size_t nbytes, offset, len;
	enum spdk_bdev_io_status status;

	rc = bdev_pmem_check_iov_len(iov, iovcnt, num_blocks, block_size);
	if (rc) {
		status = SPDK_BDEV_IO_STATUS_FAILED;
		goto end;
	}

	SPDK_DEBUGLOG(bdev_pmem, "io %lu bytes from offset %#lx\n",
		      num_blocks, offset_blocks);

	for (nbytes = num_blocks * block_size; nbytes > 0; iov++) {
		len = spdk_min(iov->iov_len, nbytes);
		nbytes -= len;

		offset = 0;
		while (offset != len) {
			rc = fn(pdisk->pool, iov->iov_base + offset, offset_blocks);
			if (rc != 0) {
				SPDK_ERRLOG("pmemblk io failed: %d (%s)\n", errno, pmemblk_errormsg());
				status = SPDK_BDEV_IO_STATUS_FAILED;
				goto end;
			}

			offset += block_size;
			offset_blocks++;
		}
	}

	assert(num_blocks == offset_blocks - bdev_io->u.bdev.offset_blocks);
	status = SPDK_BDEV_IO_STATUS_SUCCESS;
end:

	spdk_bdev_io_complete(bdev_io, status);
}

static void
bdev_pmem_write_zeros(struct spdk_bdev_io *bdev_io, struct pmem_disk *pdisk,
		      struct spdk_io_channel *ch, uint64_t offset_blocks,
		      uint64_t num_blocks, uint32_t block_size)
{
	int rc;
	enum spdk_bdev_io_status status = SPDK_BDEV_IO_STATUS_SUCCESS;

	while (num_blocks > 0) {
		rc = pmemblk_set_zero(pdisk->pool, offset_blocks);
		if (rc != 0) {
			SPDK_ERRLOG("pmemblk_set_zero failed: %d (%s)\n", errno, pmemblk_errormsg());
			status = SPDK_BDEV_IO_STATUS_FAILED;
			break;
		}
		offset_blocks++;
		num_blocks--;
	}
	spdk_bdev_io_complete(bdev_io, status);
}

static void
bdev_pmem_io_get_buf_cb(struct spdk_io_channel *channel, struct spdk_bdev_io *bdev_io,
			bool success)
{
	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	bdev_pmem_submit_io(bdev_io,
			    bdev_io->bdev->ctxt,
			    channel,
			    bdev_io->u.bdev.iovs,
			    bdev_io->u.bdev.iovcnt,
			    bdev_io->u.bdev.offset_blocks,
			    bdev_io->u.bdev.num_blocks,
			    bdev_io->bdev->blocklen,
			    _bdev_pmem_submit_io_read);
}

static void
bdev_pmem_submit_request(struct spdk_io_channel *channel, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_pmem_io_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		bdev_pmem_submit_io(bdev_io,
				    bdev_io->bdev->ctxt,
				    channel,
				    bdev_io->u.bdev.iovs,
				    bdev_io->u.bdev.iovcnt,
				    bdev_io->u.bdev.offset_blocks,
				    bdev_io->u.bdev.num_blocks,
				    bdev_io->bdev->blocklen,
				    _bdev_pmem_submit_io_write);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		bdev_pmem_write_zeros(bdev_io,
				      bdev_io->bdev->ctxt,
				      channel,
				      bdev_io->u.bdev.offset_blocks,
				      bdev_io->u.bdev.num_blocks,
				      bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
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
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_FLUSH:
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
bdev_pmem_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct pmem_disk *pdisk = ctx;

	spdk_json_write_named_object_begin(w, "pmem");
	spdk_json_write_named_string(w, "pmem_file", pdisk->pmem_file);
	spdk_json_write_object_end(w);

	return 0;
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

static void
bdev_pmem_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct pmem_disk *disk = bdev->ctxt;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_pmem_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_string(w, "pmem_file", disk->pmem_file);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table pmem_fn_table = {
	.destruct		= bdev_pmem_destruct,
	.submit_request		= bdev_pmem_submit_request,
	.io_type_supported	= bdev_pmem_io_type_supported,
	.get_io_channel		= bdev_pmem_get_io_channel,
	.dump_info_json		= bdev_pmem_dump_info_json,
	.write_config_json	= bdev_pmem_write_config_json,
};

int
create_pmem_disk(const char *pmem_file, const char *name, struct spdk_bdev **bdev)
{
	uint64_t num_blocks;
	uint32_t block_size;
	struct pmem_disk *pdisk;
	int rc;

	*bdev = NULL;

	if (name == NULL) {
		SPDK_ERRLOG("Missing name parameter for create_pmem_disk()\n");
		return -EINVAL;
	}

	if (pmemblk_check(pmem_file, 0) != 1) {
		SPDK_ERRLOG("Pool '%s' check failed: %s\n", pmem_file, pmemblk_errormsg());
		return -EIO;
	}

	pdisk = calloc(1, sizeof(*pdisk));
	if (!pdisk) {
		return -ENOMEM;
	}

	snprintf(pdisk->pmem_file, sizeof(pdisk->pmem_file), "%s", pmem_file);
	pdisk->pool = pmemblk_open(pmem_file, 0);
	if (!pdisk->pool) {
		SPDK_ERRLOG("Opening pmem pool '%s' failed: %d\n", pmem_file, errno);
		free(pdisk);
		return -errno;
	}

	block_size = pmemblk_bsize(pdisk->pool);
	num_blocks = pmemblk_nblock(pdisk->pool);

	if (block_size == 0) {
		SPDK_ERRLOG("Block size must be more than 0 bytes\n");
		pmemblk_close(pdisk->pool);
		free(pdisk);
		return -EINVAL;
	}

	if (num_blocks == 0) {
		SPDK_ERRLOG("Disk must be more than 0 blocks\n");
		pmemblk_close(pdisk->pool);
		free(pdisk);
		return -EINVAL;
	}

	pdisk->disk.name = strdup(name);
	if (!pdisk->disk.name) {
		pmemblk_close(pdisk->pool);
		free(pdisk);
		return -ENOMEM;
	}

	pdisk->disk.product_name = "pmemblk disk";
	pdisk->disk.write_cache = 0;
	pdisk->disk.blocklen = block_size;
	pdisk->disk.blockcnt = num_blocks;

	pdisk->disk.ctxt = pdisk;
	pdisk->disk.fn_table = &pmem_fn_table;
	pdisk->disk.module = &pmem_if;

	rc = spdk_bdev_register(&pdisk->disk);
	if (rc) {
		pmemblk_close(pdisk->pool);
		free(pdisk->disk.name);
		free(pdisk);
		return rc;
	}

	TAILQ_INSERT_TAIL(&g_pmem_disks, pdisk, tailq);

	*bdev = &pdisk->disk;

	return 0;
}

void
delete_pmem_disk(const char *name, spdk_delete_pmem_complete cb_fn, void *cb_arg)
{
	int rc;

	rc = spdk_bdev_unregister_by_name(name, &pmem_if, cb_fn, cb_arg);
	if (rc != 0) {
		cb_fn(cb_arg, rc);
	}
}

SPDK_LOG_DEPRECATION_REGISTER(bdev_pmem, "PMDK libpmemblk bdev_pmem integration", "SPDK 23.05", 0);

static int
bdev_pmem_initialize(void)
{
	const char *err = pmemblk_check_version(PMEMBLK_MAJOR_VERSION, PMEMBLK_MINOR_VERSION);

	SPDK_LOG_DEPRECATED(bdev_pmem);
	if (err != NULL) {
		SPDK_ERRLOG("Invalid libpmemblk version (expected %d.%d): %s\n", PMEMBLK_MAJOR_VERSION,
			    PMEMBLK_MINOR_VERSION, err);
		return -1;
	}

#ifdef SPDK_CONFIG_DEBUG
	setenv("PMEMBLK_LOG_LEVEL", "1", 1);
#endif
	spdk_io_device_register(&g_pmem_disks, bdev_pmem_create_cb, bdev_pmem_destroy_cb, 0, "pmem_bdev");

	return 0;

}

static void
bdev_pmem_finish_done(void *io_device)
{
	spdk_bdev_module_fini_done();
}

static void
bdev_pmem_finish(void)
{
	spdk_io_device_unregister(&g_pmem_disks, bdev_pmem_finish_done);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_pmem)
