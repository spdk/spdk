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

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/conf.h"
#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/string.h"
#include "spdk/endian.h"
#include "spdk/stdinc.h"
#include "spdk/util.h"
#include "spdk/endian.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

#include <getopt.h>
#include <sys/param.h>

SPDK_DECLARE_BDEV_MODULE(qcow);
SPDK_BDEV_MODULE_ASYNC_INIT(qcow)

struct qcow_header_l1_entry {
	uint64_t	reserved1	: 9;
	uint64_t	l2_offset	: 47;
	uint64_t	reserved2	: 7;
	uint64_t	cow_required	: 1;
};
SPDK_STATIC_ASSERT(sizeof(struct qcow_header_l1_entry) == 8, "incorrect L1 entry size");

struct qcow_header_l2_entry {
	uint64_t	desc		: 62;
	uint64_t	compressed	: 1;
	uint64_t	cow_required	: 1;
};
SPDK_STATIC_ASSERT(sizeof(struct qcow_header_l2_entry) == 8, "incorrect L2 entry size");

struct qcow_header_standard_desc {
	uint64_t	read_zeroes	: 1;
	uint64_t	reserved1	: 8;
	uint64_t	cluster_offset	: 47; // must be aligned to cluster boundary
	uint64_t	reserved2	: 6;
};

/**
 * struct qcow_header_compressed_desc {
 * 	uint64_t	cluster_offset	: (63 - (cluster_bits - 8)); // unaligned
 * 	uint64_t	cluster_size	: fill to 64 bits;
 * }
 */

struct qcow_header_essentials {
	uint8_t		magic[4];
	uint32_t	version;
	uint64_t	backing_file_offset;
	uint32_t	backing_file_size;
	uint32_t	cluster_bits;
	uint64_t	size;

	uint32_t	crypt_method;
	uint32_t	l1_size;
	uint64_t	l1_table_offset;
};

struct spdk_qcow_disk {
	struct spdk_bdev	*bdev;
	struct spdk_bdev_desc	*desc;
	struct spdk_io_channel	*ch;
	struct qcow_header_essentials header;
};

static struct spdk_qcow_disk *g_qcow;

static void
bdev_qcow_read(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	/* FIXME */
}

static int
_bdev_qcow_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_qcow_read,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return 0;
	case SPDK_BDEV_IO_TYPE_WRITE:
		/* FIXME
		 * Don't allow any writes until READs are confirmed
		 * to work correctly, so that no data gets corrupted.
		 */
		return -1;
	case SPDK_BDEV_IO_TYPE_RESET:
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	default:
		return -1;
	}
	return 0;
}

static void
bdev_qcow_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	if (_bdev_qcow_submit_request(ch, bdev_io) < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
bdev_qcow_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
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
bdev_qcow_get_io_channel(void *ctx)
{
	struct spdk_qcow_disk *qcow = ctx;

	return spdk_get_io_channel(qcow);
}

static int
bdev_qcow_destruct(void *ctx)
{
	return 0;
}

static const struct spdk_bdev_fn_table qcow_fn_table = {
	.destruct		= bdev_qcow_destruct,
	.submit_request		= bdev_qcow_submit_request,
	.io_type_supported	= bdev_qcow_io_type_supported,
	.get_io_channel		= bdev_qcow_get_io_channel,
};

static int
bdev_qcow_create_cb(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
bdev_qcow_destroy_cb(void *io_device, void *ctx_buf)
{
}

static int
create_qcow_bdev(struct spdk_qcow_disk *qcow, uint64_t num_blocks, uint32_t block_size)
{
	struct spdk_bdev *bdev;

	bdev = calloc(1, sizeof(*bdev));
	assert(bdev);

	bdev->name = spdk_sprintf_alloc("QCOW");
	bdev->product_name = "QEMU Copy On Write (QCOW) Disk";
	bdev->blocklen = block_size;
	bdev->blockcnt = num_blocks;
	bdev->ctxt = qcow;
	bdev->fn_table = &qcow_fn_table;
	bdev->module = SPDK_GET_BDEV_MODULE(qcow);

	spdk_io_device_register(qcow, bdev_qcow_create_cb, bdev_qcow_destroy_cb, 0);
	spdk_bdev_register(bdev);
	return 0;
}

static void
qcow_header_read_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_qcow_disk *qcow = cb_arg;
	uint8_t *data;
	uint64_t num_blocks;
	uint32_t block_size;
	int rc;

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		SPDK_ERRLOG("header read failed\n");
		goto out;
	}

	assert(bdev_io->u.bdev.iovs[0].iov_len >= 32);
	data = bdev_io->u.bdev.iovs[0].iov_base;

	spdk_trace_dump(stderr, "first qcow page", data, bdev_io->u.bdev.iovs[0].iov_len);

	memcpy(qcow->header.magic, &data[0], 4);
	qcow->header.version = from_be32(&data[4]);
	qcow->header.backing_file_offset = from_be64(&data[8]);
	qcow->header.backing_file_offset = from_be32(&data[16]);
	qcow->header.cluster_bits = from_be32(&data[20]);
	qcow->header.size = from_be64(&data[24]);

	qcow->header.crypt_method = from_be32(&data[32]);
	qcow->header.l1_size = from_be32(&data[36]);
	qcow->header.l1_table_offset = from_be32(&data[40]);

	if (memcmp(qcow->header.magic, "QFI\xfb", 4) != 0) {
		SPDK_ERRLOG("not a QCOW image\n");
		goto out;
	}

	if (qcow->header.version < 2) {
		SPDK_ERRLOG("unsupported QCOW image version %"PRIu32"\n", qcow->header.version);
		goto out;
	}

	if (qcow->header.cluster_bits < 9 || qcow->header.cluster_bits > 24) {
		SPDK_ERRLOG("cluster size must be in range <512, (1 << 24)> , got (1ULL << %"PRIu32")\n",
			    qcow->header.cluster_bits);
		goto out;
	}

	block_size = 1ULL << qcow->header.cluster_bits;
	num_blocks = qcow->header.size / block_size;

	rc = create_qcow_bdev(qcow, num_blocks, block_size);
	if (rc != 0) {
		assert(false);
	}

out:
	spdk_bdev_module_init_done(SPDK_GET_BDEV_MODULE(qcow));
}

static void
init_qcow_disk(struct spdk_bdev *bdev)
{
	struct spdk_qcow_disk *qcow;
	int rc __attribute__((unused));

	qcow = g_qcow = calloc(1, sizeof(*g_qcow));
	qcow->bdev = bdev;

	rc = spdk_bdev_open(bdev, true, NULL, NULL, &qcow->desc);
	assert(rc == 0);

	qcow->ch = spdk_bdev_get_io_channel(qcow->desc);
	assert(qcow->ch);

	rc = spdk_bdev_module_claim_bdev(bdev, qcow->desc, SPDK_GET_BDEV_MODULE(qcow));
	assert(rc == 0);

	rc = spdk_bdev_read_blocks(qcow->desc, qcow->ch, NULL, 0, 1, qcow_header_read_cb, qcow);
	assert(rc == 0);
}

static int
bdev_qcow_initialize(void)
{
	struct spdk_conf_section *sp = spdk_conf_find_section(NULL, "QCOW");
	struct spdk_bdev *bdev;
	char *bdev_name;

	if (sp == NULL) {
		goto out;
	}

	bdev_name = spdk_conf_section_get_nmval(sp, "Bdev", 0, 0);
	if (bdev_name == NULL) {
		SPDK_ERRLOG("null name\n");
		goto out;
	}

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (bdev == NULL) {
		SPDK_ERRLOG("no bdev named %s\n", bdev_name);
		goto out;
	}

	init_qcow_disk(bdev);
	return 0;

out:
	spdk_bdev_module_init_done(SPDK_GET_BDEV_MODULE(qcow));
	return 0;
}

static void bdev_qcow_finish(void)
{
}

static int
bdev_qcow_get_ctx_size(void)
{
	return 0;
}

SPDK_BDEV_MODULE_REGISTER(qcow, bdev_qcow_initialize, bdev_qcow_finish,
			  NULL, bdev_qcow_get_ctx_size, NULL)

SPDK_LOG_REGISTER_TRACE_FLAG("qcow", SPDK_TRACE_QCOW)
