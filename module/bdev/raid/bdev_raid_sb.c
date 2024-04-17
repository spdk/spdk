/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/bdev_module.h"
#include "spdk/crc32.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "bdev_raid.h"

struct raid_bdev_write_sb_ctx {
	struct raid_bdev *raid_bdev;
	int status;
	uint8_t submitted;
	uint8_t remaining;
	raid_bdev_write_sb_cb cb;
	void *cb_ctx;
	struct spdk_bdev_io_wait_entry wait_entry;
};

struct raid_bdev_read_sb_ctx {
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	raid_bdev_load_sb_cb cb;
	void *cb_ctx;
	void *buf;
	uint32_t buf_size;
};

int
raid_bdev_alloc_superblock(struct raid_bdev *raid_bdev, uint32_t block_size)
{
	struct raid_bdev_superblock *sb;

	assert(raid_bdev->sb == NULL);

	sb = spdk_dma_zmalloc(SPDK_ALIGN_CEIL(RAID_BDEV_SB_MAX_LENGTH, block_size), 0x1000, NULL);
	if (!sb) {
		SPDK_ERRLOG("Failed to allocate raid bdev sb buffer\n");
		return -ENOMEM;
	}

	raid_bdev->sb = sb;

	return 0;
}

void
raid_bdev_free_superblock(struct raid_bdev *raid_bdev)
{
	if (raid_bdev->sb_io_buf != NULL && raid_bdev->sb_io_buf != raid_bdev->sb) {
		assert(spdk_bdev_is_md_interleaved(&raid_bdev->bdev));
		spdk_dma_free(raid_bdev->sb_io_buf);
		raid_bdev->sb_io_buf = NULL;
	}
	spdk_dma_free(raid_bdev->sb);
	raid_bdev->sb = NULL;
}

void
raid_bdev_init_superblock(struct raid_bdev *raid_bdev)
{
	struct raid_bdev_superblock *sb = raid_bdev->sb;
	struct raid_base_bdev_info *base_info;
	struct raid_bdev_sb_base_bdev *sb_base_bdev;

	memcpy(&sb->signature, RAID_BDEV_SB_SIG, sizeof(sb->signature));
	sb->version.major = RAID_BDEV_SB_VERSION_MAJOR;
	sb->version.minor = RAID_BDEV_SB_VERSION_MINOR;
	spdk_uuid_copy(&sb->uuid, &raid_bdev->bdev.uuid);
	snprintf(sb->name, RAID_BDEV_SB_NAME_SIZE, "%s", raid_bdev->bdev.name);
	sb->raid_size = raid_bdev->bdev.blockcnt;
	sb->block_size = spdk_bdev_get_data_block_size(&raid_bdev->bdev);
	sb->level = raid_bdev->level;
	sb->strip_size = raid_bdev->strip_size;
	/* TODO: sb->state */
	sb->num_base_bdevs = sb->base_bdevs_size = raid_bdev->num_base_bdevs;
	sb->length = sizeof(*sb) + sizeof(*sb_base_bdev) * sb->base_bdevs_size;

	sb_base_bdev = &sb->base_bdevs[0];
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		spdk_uuid_copy(&sb_base_bdev->uuid, &base_info->uuid);
		sb_base_bdev->data_offset = base_info->data_offset;
		sb_base_bdev->data_size = base_info->data_size;
		sb_base_bdev->state = RAID_SB_BASE_BDEV_CONFIGURED;
		sb_base_bdev->slot = raid_bdev_base_bdev_slot(base_info);
		sb_base_bdev++;
	}
}

static int
raid_bdev_alloc_sb_io_buf(struct raid_bdev *raid_bdev)
{
	struct raid_bdev_superblock *sb = raid_bdev->sb;

	if (spdk_bdev_is_md_interleaved(&raid_bdev->bdev)) {
		raid_bdev->sb_io_buf_size = spdk_divide_round_up(sb->length,
					    sb->block_size) * raid_bdev->bdev.blocklen;
		raid_bdev->sb_io_buf = spdk_dma_zmalloc(raid_bdev->sb_io_buf_size, 0x1000, NULL);
		if (!raid_bdev->sb_io_buf) {
			SPDK_ERRLOG("Failed to allocate raid bdev sb io buffer\n");
			return -ENOMEM;
		}
	} else {
		raid_bdev->sb_io_buf_size = SPDK_ALIGN_CEIL(sb->length, raid_bdev->bdev.blocklen);
		raid_bdev->sb_io_buf = raid_bdev->sb;
	}

	return 0;
}

static void
raid_bdev_sb_update_crc(struct raid_bdev_superblock *sb)
{
	sb->crc = 0;
	sb->crc = spdk_crc32c_update(sb, sb->length, 0);
}

static bool
raid_bdev_sb_check_crc(struct raid_bdev_superblock *sb)
{
	uint32_t crc, prev = sb->crc;

	raid_bdev_sb_update_crc(sb);
	crc = sb->crc;
	sb->crc = prev;

	return crc == prev;
}

static int
raid_bdev_parse_superblock(struct raid_bdev_read_sb_ctx *ctx)
{
	struct raid_bdev_superblock *sb = ctx->buf;
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(ctx->desc);
	struct raid_bdev_sb_base_bdev *sb_base_bdev;
	uint8_t i;

	if (memcmp(sb->signature, RAID_BDEV_SB_SIG, sizeof(sb->signature))) {
		SPDK_DEBUGLOG(bdev_raid_sb, "invalid signature\n");
		return -EINVAL;
	}

	if (spdk_divide_round_up(sb->length, spdk_bdev_get_data_block_size(bdev)) >
	    spdk_divide_round_up(ctx->buf_size, bdev->blocklen)) {
		if (sb->length > RAID_BDEV_SB_MAX_LENGTH) {
			SPDK_WARNLOG("Incorrect superblock length on bdev %s\n",
				     spdk_bdev_get_name(bdev));
			return -EINVAL;
		}

		return -EAGAIN;
	}

	if (!raid_bdev_sb_check_crc(sb)) {
		SPDK_WARNLOG("Incorrect superblock crc on bdev %s\n", spdk_bdev_get_name(bdev));
		return -EINVAL;
	}

	if (sb->version.major != RAID_BDEV_SB_VERSION_MAJOR) {
		SPDK_ERRLOG("Not supported superblock major version %d on bdev %s\n",
			    sb->version.major, spdk_bdev_get_name(bdev));
		return -EINVAL;
	}

	if (sb->version.minor > RAID_BDEV_SB_VERSION_MINOR) {
		SPDK_WARNLOG("Superblock minor version %d on bdev %s is higher than the currently supported: %d\n",
			     sb->version.minor, spdk_bdev_get_name(bdev), RAID_BDEV_SB_VERSION_MINOR);
	}

	for (i = 0; i < sb->base_bdevs_size; i++) {
		sb_base_bdev = &sb->base_bdevs[i];
		if (sb_base_bdev->slot >= sb->num_base_bdevs) {
			SPDK_WARNLOG("Invalid superblock base bdev slot number %u on bdev %s\n",
				     sb_base_bdev->slot, spdk_bdev_get_name(bdev));
			return -EINVAL;
		}
	}

	return 0;
}

static void
raid_bdev_read_sb_ctx_free(struct raid_bdev_read_sb_ctx *ctx)
{
	spdk_dma_free(ctx->buf);

	free(ctx);
}

static void raid_bdev_read_sb_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

static int
raid_bdev_read_sb_remainder(struct raid_bdev_read_sb_ctx *ctx)
{
	struct raid_bdev_superblock *sb = ctx->buf;
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(ctx->desc);
	uint32_t buf_size_prev;
	void *buf;
	int rc;

	buf_size_prev = ctx->buf_size;
	ctx->buf_size = spdk_divide_round_up(spdk_min(sb->length, RAID_BDEV_SB_MAX_LENGTH),
					     spdk_bdev_get_data_block_size(bdev)) * bdev->blocklen;
	buf = spdk_dma_realloc(ctx->buf, ctx->buf_size, spdk_bdev_get_buf_align(bdev), NULL);
	if (buf == NULL) {
		SPDK_ERRLOG("Failed to reallocate buffer\n");
		return -ENOMEM;
	}
	ctx->buf = buf;

	rc = spdk_bdev_read(ctx->desc, ctx->ch, ctx->buf + buf_size_prev, buf_size_prev,
			    ctx->buf_size - buf_size_prev, raid_bdev_read_sb_cb, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to read bdev %s superblock remainder: %s\n",
			    spdk_bdev_get_name(bdev), spdk_strerror(-rc));
		return rc;
	}

	return 0;
}

static void
raid_bdev_read_sb_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct raid_bdev_read_sb_ctx *ctx = cb_arg;
	struct raid_bdev_superblock *sb = NULL;
	int status;

	if (spdk_bdev_is_md_interleaved(bdev_io->bdev) && ctx->buf_size > bdev->blocklen) {
		const uint32_t data_block_size = spdk_bdev_get_data_block_size(bdev);
		uint32_t i;

		for (i = 1; i < ctx->buf_size / bdev->blocklen; i++) {
			memmove(ctx->buf + (i * data_block_size),
				ctx->buf + (i * bdev->blocklen),
				data_block_size);
		}
	}

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		status = -EIO;
		goto out;
	}

	status = raid_bdev_parse_superblock(ctx);
	if (status == -EAGAIN) {
		status = raid_bdev_read_sb_remainder(ctx);
		if (status == 0) {
			return;
		}
	} else if (status != 0) {
		SPDK_DEBUGLOG(bdev_raid_sb, "failed to parse bdev %s superblock\n",
			      spdk_bdev_get_name(spdk_bdev_desc_get_bdev(ctx->desc)));
	} else {
		sb = ctx->buf;
	}
out:
	ctx->cb(sb, status, ctx->cb_ctx);

	raid_bdev_read_sb_ctx_free(ctx);
}

int
raid_bdev_load_base_bdev_superblock(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				    raid_bdev_load_sb_cb cb, void *cb_ctx)
{
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	struct raid_bdev_read_sb_ctx *ctx;
	int rc;

	assert(cb != NULL);

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	ctx->desc = desc;
	ctx->ch = ch;
	ctx->cb = cb;
	ctx->cb_ctx = cb_ctx;
	ctx->buf_size = spdk_divide_round_up(sizeof(struct raid_bdev_superblock),
					     spdk_bdev_get_data_block_size(bdev)) * bdev->blocklen;
	ctx->buf = spdk_dma_malloc(ctx->buf_size, spdk_bdev_get_buf_align(bdev), NULL);
	if (!ctx->buf) {
		rc = -ENOMEM;
		goto err;
	}

	rc = spdk_bdev_read(desc, ch, ctx->buf, 0, ctx->buf_size, raid_bdev_read_sb_cb, ctx);
	if (rc) {
		goto err;
	}

	return 0;
err:
	raid_bdev_read_sb_ctx_free(ctx);

	return rc;
}

static void
raid_bdev_write_sb_base_bdev_done(int status, struct raid_bdev_write_sb_ctx *ctx)
{
	if (status != 0) {
		ctx->status = status;
	}

	if (--ctx->remaining == 0) {
		ctx->cb(ctx->status, ctx->raid_bdev, ctx->cb_ctx);
		free(ctx);
	}
}

static void
raid_bdev_write_superblock_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_write_sb_ctx *ctx = cb_arg;
	int status = 0;

	if (!success) {
		SPDK_ERRLOG("Failed to save superblock on bdev %s\n", bdev_io->bdev->name);
		status = -EIO;
	}

	spdk_bdev_free_io(bdev_io);

	raid_bdev_write_sb_base_bdev_done(status, ctx);
}

static void
_raid_bdev_write_superblock(void *_ctx)
{
	struct raid_bdev_write_sb_ctx *ctx = _ctx;
	struct raid_bdev *raid_bdev = ctx->raid_bdev;
	struct raid_base_bdev_info *base_info;
	uint8_t i;
	int rc;

	for (i = ctx->submitted; i < raid_bdev->num_base_bdevs; i++) {
		base_info = &raid_bdev->base_bdev_info[i];

		if (!base_info->is_configured || base_info->remove_scheduled) {
			assert(ctx->remaining > 1);
			raid_bdev_write_sb_base_bdev_done(0, ctx);
			ctx->submitted++;
			continue;
		}

		rc = spdk_bdev_write(base_info->desc, base_info->app_thread_ch,
				     raid_bdev->sb_io_buf, 0, raid_bdev->sb_io_buf_size,
				     raid_bdev_write_superblock_cb, ctx);
		if (rc != 0) {
			struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(base_info->desc);

			if (rc == -ENOMEM) {
				ctx->wait_entry.bdev = bdev;
				ctx->wait_entry.cb_fn = _raid_bdev_write_superblock;
				ctx->wait_entry.cb_arg = ctx;
				spdk_bdev_queue_io_wait(bdev, base_info->app_thread_ch, &ctx->wait_entry);
				return;
			}

			assert(ctx->remaining > 1);
			raid_bdev_write_sb_base_bdev_done(rc, ctx);
		}

		ctx->submitted++;
	}

	raid_bdev_write_sb_base_bdev_done(0, ctx);
}

void
raid_bdev_write_superblock(struct raid_bdev *raid_bdev, raid_bdev_write_sb_cb cb, void *cb_ctx)
{
	struct raid_bdev_write_sb_ctx *ctx;
	struct raid_bdev_superblock *sb = raid_bdev->sb;
	int rc;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());
	assert(sb != NULL);
	assert(cb != NULL);

	if (raid_bdev->sb_io_buf == NULL) {
		rc = raid_bdev_alloc_sb_io_buf(raid_bdev);
		if (rc != 0) {
			goto err;
		}
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		rc = -ENOMEM;
		goto err;
	}

	ctx->raid_bdev = raid_bdev;
	ctx->remaining = raid_bdev->num_base_bdevs + 1;
	ctx->cb = cb;
	ctx->cb_ctx = cb_ctx;

	sb->seq_number++;
	raid_bdev_sb_update_crc(sb);

	if (spdk_bdev_is_md_interleaved(&raid_bdev->bdev)) {
		void *sb_buf = sb;
		uint32_t i;

		for (i = 0; i < raid_bdev->sb_io_buf_size / raid_bdev->bdev.blocklen; i++) {
			memcpy(raid_bdev->sb_io_buf + (i * raid_bdev->bdev.blocklen),
			       sb_buf + (i * sb->block_size), sb->block_size);
		}
	}

	_raid_bdev_write_superblock(ctx);
	return;
err:
	cb(rc, raid_bdev, cb_ctx);
}

SPDK_LOG_REGISTER_COMPONENT(bdev_raid_sb)
