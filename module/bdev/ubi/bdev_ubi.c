/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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

#include "bdev_ubi.h"

#define PATH_LEN 1024
#define UBI_BLOCK_SIZE (4 * 1024 * 1024)

struct ubi_bdev {
	struct spdk_bdev	bdev;
	char base_disk[PATH_LEN];
	char diff_disk[PATH_LEN];
	// support disks upto 32TB = 4 * 8 * 1024^4
	uint8_t modified[1024 * 1024];
	int num_blocks;
	TAILQ_ENTRY(ubi_bdev)	tailq;
};

struct ubi_io_channel {
	struct spdk_poller		*poller;
	TAILQ_HEAD(, spdk_bdev_io)	io;
};

static TAILQ_HEAD(, ubi_bdev) g_ubi_bdev_head = TAILQ_HEAD_INITIALIZER(g_ubi_bdev_head);
static void *g_ubi_read_buf;

static int bdev_ubi_initialize(void);
static void bdev_ubi_finish(void);

static struct spdk_bdev_module ubi_if = {
	.name = "ubi",
	.module_init = bdev_ubi_initialize,
	.module_fini = bdev_ubi_finish,
	.async_fini = true,
};

SPDK_BDEV_MODULE_REGISTER(ubi, &ubi_if)

static int
bdev_ubi_destruct(void *ctx)
{
	struct ubi_bdev *bdev = ctx;

	TAILQ_REMOVE(&g_ubi_bdev_head, bdev, tailq);
	free(bdev->bdev.name);
	free(bdev);

	return 0;
}

static bool
bdev_ubi_abort_io(struct ubi_io_channel *ch, struct spdk_bdev_io *bio_to_abort)
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
bdev_ubi_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct ubi_io_channel *ch = spdk_io_channel_get_ctx(_ch);

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		if (bdev_io->u.bdev.iovs[0].iov_base == NULL) {
			assert(bdev_io->u.bdev.iovcnt == 1);
			if (spdk_likely(bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen <=
					SPDK_BDEV_LARGE_BUF_MAX_SIZE)) {
				bdev_io->u.bdev.iovs[0].iov_base = g_ubi_read_buf;
				bdev_io->u.bdev.iovs[0].iov_len = bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen;
			} else {
				SPDK_ERRLOG("Overflow occurred. Read I/O size %" PRIu64 " was larger than permitted %d\n",
					    bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
					    SPDK_BDEV_LARGE_BUF_MAX_SIZE);
				spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
				return;
			}
		}
		SPDK_WARNLOG("Reading from Ubi ...\n");
		TAILQ_INSERT_TAIL(&ch->io, bdev_io, module_link);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		SPDK_WARNLOG("Writing to Ubi ...\n");
		TAILQ_INSERT_TAIL(&ch->io, bdev_io, module_link);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_RESET:
		TAILQ_INSERT_TAIL(&ch->io, bdev_io, module_link);
		break;
	case SPDK_BDEV_IO_TYPE_ABORT:
		if (bdev_ubi_abort_io(ch, bdev_io->u.abort.bio_to_abort)) {
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
bdev_ubi_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
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
bdev_ubi_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(&g_ubi_bdev_head);
}

static void
bdev_ubi_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	char uuid_str[SPDK_UUID_STRING_LEN];

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_ubi_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_json_write_named_uint64(w, "num_blocks", bdev->blockcnt);
	spdk_json_write_named_uint32(w, "block_size", bdev->blocklen);
	spdk_json_write_named_uint32(w, "physical_block_size", bdev->phys_blocklen);
	spdk_json_write_named_uint32(w, "md_size", bdev->md_len);
	spdk_json_write_named_uint32(w, "dif_type", bdev->dif_type);
	spdk_json_write_named_bool(w, "dif_is_head_of_md", bdev->dif_is_head_of_md);
	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &bdev->uuid);
	spdk_json_write_named_string(w, "uuid", uuid_str);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_bdev_fn_table ubi_fn_table = {
	.destruct		= bdev_ubi_destruct,
	.submit_request		= bdev_ubi_submit_request,
	.io_type_supported	= bdev_ubi_io_type_supported,
	.get_io_channel		= bdev_ubi_get_io_channel,
	.write_config_json	= bdev_ubi_write_config_json,
};

int
bdev_ubi_create(struct spdk_bdev **bdev, const struct spdk_ubi_bdev_opts *opts)
{
	struct ubi_bdev *ubi_disk;
	uint32_t data_block_size;
	int rc;

	if (!opts) {
		SPDK_ERRLOG("No options provided for Ubi bdev.\n");
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

	ubi_disk = calloc(1, sizeof(*ubi_disk));
	if (!ubi_disk) {
		SPDK_ERRLOG("could not allocate ubi_bdev\n");
		return -ENOMEM;
	}

	ubi_disk->bdev.name = strdup(opts->name);
	if (!ubi_disk->bdev.name) {
		free(ubi_disk);
		return -ENOMEM;
	}
	ubi_disk->bdev.product_name = "Ubi disk";

	ubi_disk->bdev.write_cache = 0;
	ubi_disk->bdev.blocklen = opts->block_size;
	ubi_disk->bdev.phys_blocklen = opts->physical_block_size;
	ubi_disk->bdev.blockcnt = opts->num_blocks;
	ubi_disk->bdev.md_len = opts->md_size;
	ubi_disk->bdev.md_interleave = opts->md_interleave;
	ubi_disk->bdev.dif_type = opts->dif_type;
	ubi_disk->bdev.dif_is_head_of_md = opts->dif_is_head_of_md;
	/* Current block device layer API does not propagate
	 * any DIF related information from user. So, we can
	 * not generate or verify Application Tag.
	 */
	switch (opts->dif_type) {
	case SPDK_DIF_TYPE1:
	case SPDK_DIF_TYPE2:
		ubi_disk->bdev.dif_check_flags = SPDK_DIF_FLAGS_GUARD_CHECK |
						  SPDK_DIF_FLAGS_REFTAG_CHECK;
		break;
	case SPDK_DIF_TYPE3:
		ubi_disk->bdev.dif_check_flags = SPDK_DIF_FLAGS_GUARD_CHECK;
		break;
	case SPDK_DIF_DISABLE:
		break;
	}
	if (opts->uuid) {
		ubi_disk->bdev.uuid = *opts->uuid;
	}

	ubi_disk->bdev.ctxt = ubi_disk;
	ubi_disk->bdev.fn_table = &ubi_fn_table;
	ubi_disk->bdev.module = &ubi_if;

	strcpy(ubi_disk->base_disk, "/home/hadi/base.img");
	strcpy(ubi_disk->diff_disk, "/home/hadi/diff.img");
	memset(ubi_disk->modified, 0, sizeof(ubi_disk->modified));
	ubi_disk->num_blocks = 16;

	rc = spdk_bdev_register(&ubi_disk->bdev);
	if (rc) {
		free(ubi_disk->bdev.name);
		free(ubi_disk);
		return rc;
	}

	*bdev = &(ubi_disk->bdev);

	TAILQ_INSERT_TAIL(&g_ubi_bdev_head, ubi_disk, tailq);

	return rc;
}

void
bdev_ubi_delete(const char *bdev_name, spdk_delete_ubi_complete cb_fn, void *cb_arg)
{
	int rc;

	rc = spdk_bdev_unregister_by_name(bdev_name, &ubi_if, cb_fn, cb_arg);
	if (rc != 0) {
		cb_fn(cb_arg, rc);
	}
}

static int
ubi_io_poll(void *arg)
{
	struct ubi_io_channel		*ch = arg;
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
ubi_bdev_create_cb(void *io_device, void *ctx_buf)
{
	struct ubi_io_channel *ch = ctx_buf;

	TAILQ_INIT(&ch->io);
	ch->poller = SPDK_POLLER_REGISTER(ubi_io_poll, ch, 0);

	return 0;
}

static void
ubi_bdev_destroy_cb(void *io_device, void *ctx_buf)
{
	struct ubi_io_channel *ch = ctx_buf;

	spdk_poller_unregister(&ch->poller);
}

static int
bdev_ubi_initialize(void)
{
	/*
	 * This will be used if upper layer expects us to allocate the read buffer.
	 *  Instead of using a real rbuf from the bdev pool, just always point to
	 *  this same zeroed buffer.
	 */
	g_ubi_read_buf = spdk_zmalloc(SPDK_BDEV_LARGE_BUF_MAX_SIZE, 0, NULL,
				       SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (g_ubi_read_buf == NULL) {
		return -1;
	}

	/*
	 * We need to pick some unique address as our "io device" - so just use the
	 *  address of the global tailq.
	 */
	spdk_io_device_register(
		&g_ubi_bdev_head,
		ubi_bdev_create_cb,
		ubi_bdev_destroy_cb,
		sizeof(struct ubi_io_channel), "ubi_bdev");

	return 0;
}

static void
dummy_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
}

int
bdev_ubi_resize(const char *bdev_name, const uint64_t new_size_in_mb)
{
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	uint64_t current_size_in_mb;
	uint64_t new_size_in_byte;
	int rc = 0;

	rc = spdk_bdev_open_ext(bdev_name, false, dummy_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		SPDK_ERRLOG("failed to open bdev; %s.\n", bdev_name);
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);

	if (bdev->module != &ubi_if) {
		rc = -EINVAL;
		goto exit;
	}

	current_size_in_mb = bdev->blocklen * bdev->blockcnt / (1024 * 1024);
	if (new_size_in_mb < current_size_in_mb) {
		SPDK_ERRLOG("The new bdev size must not be smaller than current bdev size.\n");
		rc = -EINVAL;
		goto exit;
	}

	new_size_in_byte = new_size_in_mb * 1024 * 1024;

	rc = spdk_bdev_notify_blockcnt_change(bdev, new_size_in_byte / bdev->blocklen);
	if (rc != 0) {
		SPDK_ERRLOG("failed to notify block cnt change.\n");
	}

exit:
	spdk_bdev_close(desc);
	return rc;
}

static void
_bdev_ubi_finish_cb(void *arg)
{
	spdk_free(g_ubi_read_buf);
	spdk_bdev_module_fini_done();
}

static void
bdev_ubi_finish(void)
{
	if (g_ubi_read_buf == NULL) {
		spdk_bdev_module_fini_done();
		return;
	}
	spdk_io_device_unregister(&g_ubi_bdev_head, _bdev_ubi_finish_cb);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_ubi)
