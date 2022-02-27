/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/string.h"
#include "spdk/likely.h"

#include "spdk/bdev_module.h"
#include "spdk/log.h"

#include "bdev_null.h"

struct null_bdev {
	struct spdk_bdev	bdev;
	TAILQ_ENTRY(null_bdev)	tailq;
};

struct null_io_channel {
	struct spdk_poller		*poller;
	TAILQ_HEAD(, spdk_bdev_io)	io;
};

static TAILQ_HEAD(, null_bdev) g_null_bdev_head = TAILQ_HEAD_INITIALIZER(g_null_bdev_head);
static void *g_null_read_buf;

static int bdev_null_initialize(void);
static void bdev_null_finish(void);

static struct spdk_bdev_module null_if = {
	.name = "null",
	.module_init = bdev_null_initialize,
	.module_fini = bdev_null_finish,
	.async_fini = true,
};

SPDK_BDEV_MODULE_REGISTER(null, &null_if)

static int
bdev_null_destruct(void *ctx)
{
	struct null_bdev *bdev = ctx;

	TAILQ_REMOVE(&g_null_bdev_head, bdev, tailq);
	free(bdev->bdev.name);
	free(bdev);

	return 0;
}

static bool
bdev_null_abort_io(struct null_io_channel *ch, struct spdk_bdev_io *bio_to_abort)
{
	struct spdk_bdev_io *bdev_io;

	TAILQ_FOREACH(bdev_io, &ch->io, module_link) {
		if (bdev_io == bio_to_abort) {
			TAILQ_REMOVE(&ch->io, bio_to_abort, module_link);
			spdk_bdev_io_complete(bio_to_abort, SPDK_BDEV_IO_STATUS_ABORTED);
			return true;
		}
	}

	return false;
}

static void
bdev_null_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct null_io_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct spdk_dif_ctx dif_ctx;
	struct spdk_dif_error err_blk;
	int rc;

	if (SPDK_DIF_DISABLE != bdev->dif_type &&
	    (SPDK_BDEV_IO_TYPE_READ == bdev_io->type ||
	     SPDK_BDEV_IO_TYPE_WRITE == bdev_io->type)) {
		rc = spdk_dif_ctx_init(&dif_ctx,
				       bdev->blocklen,
				       bdev->md_len,
				       bdev->md_interleave,
				       bdev->dif_is_head_of_md,
				       bdev->dif_type,
				       bdev->dif_check_flags,
				       bdev_io->u.bdev.offset_blocks & 0xFFFFFFFF,
				       0xFFFF, 0, 0, 0);
		if (0 != rc) {
			SPDK_ERRLOG("Failed to initialize DIF context, error %d\n", rc);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		if (bdev_io->u.bdev.iovs[0].iov_base == NULL) {
			assert(bdev_io->u.bdev.iovcnt == 1);
			if (spdk_likely(bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen <=
					SPDK_BDEV_LARGE_BUF_MAX_SIZE)) {
				bdev_io->u.bdev.iovs[0].iov_base = g_null_read_buf;
				bdev_io->u.bdev.iovs[0].iov_len = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;
			} else {
				SPDK_ERRLOG("Overflow occurred. Read I/O size %" PRIu64 " was larger than permitted %d\n",
					    bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
					    SPDK_BDEV_LARGE_BUF_MAX_SIZE);
				spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		}
		if (SPDK_DIF_DISABLE != bdev->dif_type) {
			rc = spdk_dif_generate(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					       bdev_io->u.bdev.num_blocks, &dif_ctx);
			if (0 != rc) {
				SPDK_ERRLOG("IO DIF generation failed: lba %" PRIu64 ", num_block %" PRIu64 "\n",
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks);
				spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		}
		TAILQ_INSERT_TAIL(&ch->io, bdev_io, module_link);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		if (SPDK_DIF_DISABLE != bdev->dif_type) {
			rc = spdk_dif_verify(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					     bdev_io->u.bdev.num_blocks, &dif_ctx, &err_blk);
			if (0 != rc) {
				SPDK_ERRLOG("IO DIF verification failed: lba %" PRIu64 ", num_blocks %" PRIu64 ", "
					    "err_type %u, expected %u, actual %u, err_offset %u\n",
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    err_blk.err_type,
					    err_blk.expected,
					    err_blk.actual,
					    err_blk.err_offset);
				spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		}
		TAILQ_INSERT_TAIL(&ch->io, bdev_io, module_link);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
		TAILQ_INSERT_TAIL(&ch->io, bdev_io, module_link);
		break;
	case SPDK_BDEV_IO_TYPE_ABORT:
		if (bdev_null_abort_io(ch, bdev_io->u.abort.bio_to_abort)) {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		} else {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	default:
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

static bool
bdev_null_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_ABORT:
		return true;
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	default:
		return false;
	}
}

static struct spdk_io_channel *
bdev_null_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_null_bdev_head);
}

static void
bdev_null_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	char uuid_str[SPDK_UUID_STRING_LEN];

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_null_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_uint64(w, "num_blocks", bdev->blockcnt);
	spdk_json_write_named_uint32(w, "block_size", bdev->blocklen);
	spdk_json_write_named_uint32(w, "md_size", bdev->md_len);
	spdk_json_write_named_uint32(w, "dif_type", bdev->dif_type);
	spdk_json_write_named_bool(w, "dif_is_head_of_md", bdev->dif_is_head_of_md);
	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &bdev->uuid);
	spdk_json_write_named_string(w, "uuid", uuid_str);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table null_fn_table = {
	.destruct		= bdev_null_destruct,
	.submit_request		= bdev_null_submit_request,
	.io_type_supported	= bdev_null_io_type_supported,
	.get_io_channel		= bdev_null_get_io_channel,
	.write_config_json	= bdev_null_write_config_json,
};

int
bdev_null_create(struct spdk_bdev **bdev, const struct spdk_null_bdev_opts *opts)
{
	struct null_bdev *null_disk;
	uint32_t data_block_size;
	int rc;

	if (!opts) {
		SPDK_ERRLOG("No options provided for Null bdev.\n");
		return -EINVAL;
	}

	if (opts->md_interleave) {
		if (opts->block_size < opts->md_size) {
			SPDK_ERRLOG("Interleaved metadata size can not be greater than block size.\n");
			return -EINVAL;
		}
		data_block_size = opts->block_size - opts->md_size;
	} else {
		if (opts->md_size != 0) {
			SPDK_ERRLOG("Metadata in separate buffer is not supported\n");
			return -ENOTSUP;
		}
		data_block_size = opts->block_size;
	}

	if (data_block_size % 512 != 0) {
		SPDK_ERRLOG("Data block size %u is not a multiple of 512.\n", opts->block_size);
		return -EINVAL;
	}

	if (opts->num_blocks == 0) {
		SPDK_ERRLOG("Disk must be more than 0 blocks\n");
		return -EINVAL;
	}

	null_disk = calloc(1, sizeof(*null_disk));
	if (!null_disk) {
		SPDK_ERRLOG("could not allocate null_bdev\n");
		return -ENOMEM;
	}

	null_disk->bdev.name = strdup(opts->name);
	if (!null_disk->bdev.name) {
		free(null_disk);
		return -ENOMEM;
	}
	null_disk->bdev.product_name = "Null disk";

	null_disk->bdev.write_cache = 0;
	null_disk->bdev.blocklen = opts->block_size;
	null_disk->bdev.blockcnt = opts->num_blocks;
	null_disk->bdev.md_len = opts->md_size;
	null_disk->bdev.md_interleave = opts->md_interleave;
	null_disk->bdev.dif_type = opts->dif_type;
	null_disk->bdev.dif_is_head_of_md = opts->dif_is_head_of_md;
	/* Current block device layer API does not propagate
	 * any DIF related information from user. So, we can
	 * not generate or verify Application Tag.
	 */
	switch (opts->dif_type) {
	case SPDK_DIF_TYPE1:
	case SPDK_DIF_TYPE2:
		null_disk->bdev.dif_check_flags = SPDK_DIF_FLAGS_GUARD_CHECK |
						  SPDK_DIF_FLAGS_REFTAG_CHECK;
		break;
	case SPDK_DIF_TYPE3:
		null_disk->bdev.dif_check_flags = SPDK_DIF_FLAGS_GUARD_CHECK;
		break;
	case SPDK_DIF_DISABLE:
		break;
	}
	if (opts->uuid) {
		null_disk->bdev.uuid = *opts->uuid;
	} else {
		spdk_uuid_generate(&null_disk->bdev.uuid);
	}

	null_disk->bdev.ctxt = null_disk;
	null_disk->bdev.fn_table = &null_fn_table;
	null_disk->bdev.module = &null_if;

	rc = spdk_bdev_register(&null_disk->bdev);
	if (rc) {
		free(null_disk->bdev.name);
		free(null_disk);
		return rc;
	}

	*bdev = &(null_disk->bdev);

	TAILQ_INSERT_TAIL(&g_null_bdev_head, null_disk, tailq);

	return rc;
}

void
bdev_null_delete(struct spdk_bdev *bdev, spdk_delete_null_complete cb_fn, void *cb_arg)
{
	if (!bdev || bdev->module != &null_if) {
		cb_fn(cb_arg, -ENODEV);
		return;
	}

	spdk_bdev_unregister(bdev, cb_fn, cb_arg);
}

static int
null_io_poll(void *arg)
{
	struct null_io_channel		*ch = arg;
	TAILQ_HEAD(, spdk_bdev_io)	io;
	struct spdk_bdev_io		*bdev_io;

	TAILQ_INIT(&io);
	TAILQ_SWAP(&ch->io, &io, spdk_bdev_io, module_link);

	if (TAILQ_EMPTY(&io)) {
		return SPDK_POLLER_IDLE;
	}

	while (!TAILQ_EMPTY(&io)) {
		bdev_io = TAILQ_FIRST(&io);
		TAILQ_REMOVE(&io, bdev_io, module_link);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	}

	return SPDK_POLLER_BUSY;
}

static int
null_bdev_create_cb(void *io_device, void *ctx_buf)
{
	struct null_io_channel *ch = ctx_buf;

	TAILQ_INIT(&ch->io);
	ch->poller = SPDK_POLLER_REGISTER(null_io_poll, ch, 0);

	return 0;
}

static void
null_bdev_destroy_cb(void *io_device, void *ctx_buf)
{
	struct null_io_channel *ch = ctx_buf;

	spdk_poller_unregister(&ch->poller);
}

static int
bdev_null_initialize(void)
{
	/*
	 * This will be used if upper layer expects us to allocate the read buffer.
	 *  Instead of using a real rbuf from the bdev pool, just always point to
	 *  this same zeroed buffer.
	 */
	g_null_read_buf = spdk_zmalloc(SPDK_BDEV_LARGE_BUF_MAX_SIZE, 0, NULL,
				       SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (g_null_read_buf == NULL) {
		return -1;
	}

	/*
	 * We need to pick some unique address as our "io device" - so just use the
	 *  address of the global tailq.
	 */
	spdk_io_device_register(&g_null_bdev_head, null_bdev_create_cb, null_bdev_destroy_cb,
				sizeof(struct null_io_channel), "null_bdev");

	return 0;
}

int
bdev_null_resize(struct spdk_bdev *bdev, const uint64_t new_size_in_mb)
{
	uint64_t current_size_in_mb;
	uint64_t new_size_in_byte;
	int rc;

	if (bdev->module != &null_if) {
		return -EINVAL;
	}

	current_size_in_mb = bdev->blocklen * bdev->blockcnt / (1024 * 1024);
	if (new_size_in_mb < current_size_in_mb) {
		SPDK_ERRLOG("The new bdev size must not be smaller than current bdev size.\n");
		return -EINVAL;
	}

	new_size_in_byte = new_size_in_mb * 1024 * 1024;

	rc = spdk_bdev_notify_blockcnt_change(bdev, new_size_in_byte / bdev->blocklen);
	if (rc != 0) {
		SPDK_ERRLOG("failed to notify block cnt change.\n");
		return rc;
	}

	return 0;
}

static void
_bdev_null_finish_cb(void *arg)
{
	spdk_free(g_null_read_buf);
	spdk_bdev_module_fini_done();
}

static void
bdev_null_finish(void)
{
	if (g_null_read_buf == NULL) {
		spdk_bdev_module_fini_done();
		return;
	}
	spdk_io_device_unregister(&g_null_bdev_head, _bdev_null_finish_cb);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_null)
