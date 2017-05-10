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

/*
 * This is a simple example of a virtual block device that takes a single
 * bdev and slices it into multiple smaller bdevs.
 */

#include "spdk/stdinc.h"

#include "spdk/rpc.h"
#include "spdk/conf.h"
#include "spdk/endian.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

/* Base block device split context */
struct split_base {
	struct spdk_bdev	*base_bdev;
	uint32_t		ref;
};

/* Context for each split virtual bdev */
struct split_disk {
	struct spdk_bdev	disk;
	struct spdk_bdev	*base_bdev;
	struct split_base	*split_base;
	uint64_t		offset_blocks;
	uint64_t		offset_bytes;
	TAILQ_ENTRY(split_disk)	tailq;
};

static TAILQ_HEAD(, split_disk) g_split_disks = TAILQ_HEAD_INITIALIZER(g_split_disks);

static void
split_read(struct split_disk *split_disk, struct spdk_bdev_io *bdev_io)
{
	bdev_io->u.read.offset += split_disk->offset_bytes;
}

static void
split_write(struct split_disk *split_disk, struct spdk_bdev_io *bdev_io)
{
	bdev_io->u.write.offset += split_disk->offset_bytes;
}

static void
split_unmap(struct split_disk *split_disk, struct spdk_bdev_io *bdev_io)
{
	uint16_t i;
	uint64_t lba;

	for (i = 0; i < bdev_io->u.unmap.bdesc_count; i++) {
		lba = from_be64(&bdev_io->u.unmap.unmap_bdesc[i].lba);
		lba += split_disk->offset_blocks;
		to_be64(&bdev_io->u.unmap.unmap_bdesc[i].lba, lba);
	}
}

static void
split_flush(struct split_disk *split_disk, struct spdk_bdev_io *bdev_io)
{
	bdev_io->u.flush.offset += split_disk->offset_bytes;
}

static void
split_reset(struct split_disk *split_disk, struct spdk_bdev_io *bdev_io)
{
	/*
	 * No offset to modify for reset - pass the I/O through unmodified.
	 *
	 * However, we do need to increment the generation count for the split bdev,
	 * since the spdk_bdev_io_complete() path that normally updates it will not execute
	 * after we resubmit the I/O to the base_bdev.
	 */
	if (bdev_io->u.reset.type == SPDK_BDEV_RESET_HARD) {
		split_disk->disk.gencnt++;
	}
}

static void
vbdev_split_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct split_disk *split_disk = bdev_io->bdev->ctxt;

	/* Modify the I/O to adjust for the offset within the base bdev. */
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		split_read(split_disk, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		split_write(split_disk, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		split_unmap(split_disk, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		split_flush(split_disk, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		split_reset(split_disk, bdev_io);
		break;
	default:
		SPDK_ERRLOG("split: unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	/* Submit the modified I/O to the underlying bdev. */
	spdk_bdev_io_resubmit(bdev_io, split_disk->base_bdev);
}

static void
vbdev_split_base_get_ref(struct split_base *split_base, struct split_disk *split_disk)
{
	__sync_fetch_and_add(&split_base->ref, 1);
	split_disk->split_base = split_base;
}

static void
vbdev_split_base_put_ref(struct split_base *split_base)
{
	if (__sync_sub_and_fetch(&split_base->ref, 1) == 0) {
		spdk_bdev_unclaim(split_base->base_bdev);
		free(split_base);
	}
}

static void
vbdev_split_free(struct split_disk *split_disk)
{
	struct split_base *split_base;

	if (!split_disk) {
		return;
	}

	split_base = split_disk->split_base;

	TAILQ_REMOVE(&g_split_disks, split_disk, tailq);
	free(split_disk);

	if (split_base) {
		vbdev_split_base_put_ref(split_base);
	}
}

static int
vbdev_split_destruct(void *ctx)
{
	struct split_disk *split_disk = ctx;

	vbdev_split_free(split_disk);
	return 0;
}

static bool
vbdev_split_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct split_disk *split_disk = ctx;

	return split_disk->base_bdev->fn_table->io_type_supported(split_disk->base_bdev, io_type);
}

static struct spdk_io_channel *
vbdev_split_get_io_channel(void *ctx, uint32_t priority)
{
	struct split_disk *split_disk = ctx;

	return split_disk->base_bdev->fn_table->get_io_channel(split_disk->base_bdev, priority);
}

static int
vbdev_split_dump_config_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct split_disk *split_disk = ctx;

	spdk_json_write_name(w, "split");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "base_bdev");
	spdk_json_write_string(w, spdk_bdev_get_name(split_disk->base_bdev));
	spdk_json_write_name(w, "offset_blocks");
	spdk_json_write_uint64(w, split_disk->offset_blocks);

	spdk_json_write_object_end(w);

	return 0;
}

static struct spdk_bdev_fn_table vbdev_split_fn_table = {
	.destruct		= vbdev_split_destruct,
	.io_type_supported	= vbdev_split_io_type_supported,
	.submit_request		= vbdev_split_submit_request,
	.get_io_channel		= vbdev_split_get_io_channel,
	.dump_config_json	= vbdev_split_dump_config_json,
};

static int
vbdev_split_create(struct spdk_bdev *base_bdev, uint64_t split_count, uint64_t split_size_mb)
{
	uint64_t split_size_bytes, split_size_blocks, offset_bytes, offset_blocks;
	uint64_t max_split_count;
	uint64_t mb = 1024 * 1024;
	uint64_t i;
	int rc;
	struct split_base *split_base;

	if (!spdk_bdev_claim(base_bdev, NULL, NULL)) {
		SPDK_ERRLOG("Split bdev %s is already claimed\n", spdk_bdev_get_name(base_bdev));
		return -1;
	}

	if (split_size_mb) {
		if (((split_size_mb * mb) % base_bdev->blocklen) != 0) {
			SPDK_ERRLOG("Split size %" PRIu64 " MB is not possible with block size "
				    "%" PRIu32 "\n",
				    split_size_mb, base_bdev->blocklen);
			return -1;
		}
		split_size_blocks = (split_size_mb * mb) / base_bdev->blocklen;
		SPDK_TRACELOG(SPDK_TRACE_VBDEV_SPLIT, "Split size %" PRIu64 " MB specified by user\n",
			      split_size_mb);
	} else {
		split_size_blocks = base_bdev->blockcnt / split_count;
		SPDK_TRACELOG(SPDK_TRACE_VBDEV_SPLIT, "Split size not specified by user\n");
	}

	split_size_bytes = split_size_blocks * base_bdev->blocklen;

	max_split_count = base_bdev->blockcnt / split_size_blocks;
	if (split_count > max_split_count) {
		SPDK_WARNLOG("Split count %" PRIu64 " is greater than maximum possible split count "
			     "%" PRIu64 " - clamping\n", split_count, max_split_count);
		split_count = max_split_count;
	}

	SPDK_TRACELOG(SPDK_TRACE_VBDEV_SPLIT, "base_bdev: %s split_count: %" PRIu64
		      " split_size_bytes: %" PRIu64 "\n",
		      spdk_bdev_get_name(base_bdev), split_count, split_size_bytes);

	split_base = calloc(1, sizeof(*split_base));
	if (!split_base) {
		SPDK_ERRLOG("Cannot alloc memory for split base pointer\n");
		return -1;
	}
	split_base->base_bdev = base_bdev;
	split_base->ref = 0;

	offset_bytes = 0;
	offset_blocks = 0;
	for (i = 0; i < split_count; i++) {
		struct split_disk *d;

		d = calloc(1, sizeof(*d));
		if (!d) {
			SPDK_ERRLOG("Memory allocation failure\n");
			rc = -1;
			goto cleanup;
		}

		/* Copy properties of the base bdev */
		d->disk.blocklen = base_bdev->blocklen;
		d->disk.write_cache = base_bdev->write_cache;
		d->disk.need_aligned_buffer = base_bdev->need_aligned_buffer;
		d->disk.max_unmap_bdesc_count = base_bdev->max_unmap_bdesc_count;

		/* Append partition number to the base bdev's name, e.g. Malloc0 -> Malloc0p0 */
		snprintf(d->disk.name, sizeof(d->disk.name), "%sp%" PRIu64, spdk_bdev_get_name(base_bdev), i);
		snprintf(d->disk.product_name, sizeof(d->disk.product_name), "Split Disk");
		d->base_bdev = base_bdev;
		d->offset_bytes = offset_bytes;
		d->offset_blocks = offset_blocks;
		d->disk.blockcnt = split_size_blocks;
		d->disk.ctxt = d;
		d->disk.fn_table = &vbdev_split_fn_table;

		SPDK_TRACELOG(SPDK_TRACE_VBDEV_SPLIT, "Split vbdev %s: base bdev: %s offset_bytes: "
			      "%" PRIu64 " offset_blocks: %" PRIu64 "\n",
			      d->disk.name, spdk_bdev_get_name(base_bdev), d->offset_bytes, d->offset_blocks);

		vbdev_split_base_get_ref(split_base, d);

		spdk_bdev_register(&d->disk);

		TAILQ_INSERT_TAIL(&g_split_disks, d, tailq);

		offset_bytes += split_size_bytes;
		offset_blocks += split_size_blocks;
	}

	rc = 0;

cleanup:
	if (split_base->ref == 0) {
		/* If no split_disk instances were created, free the base context */
		free(split_base);
	}

	return rc;
}

static int
vbdev_split_init(void)
{
	struct spdk_conf_section *sp;
	const char *base_bdev_name;
	const char *split_count_str;
	const char *split_size_str;
	int i, split_count, split_size;
	struct spdk_bdev *base_bdev;

	sp = spdk_conf_find_section(NULL, "Split");
	if (sp == NULL) {
		return 0;
	}

	for (i = 0; ; i++) {
		if (!spdk_conf_section_get_nval(sp, "Split", i)) {
			break;
		}

		base_bdev_name = spdk_conf_section_get_nmval(sp, "Split", i, 0);
		if (!base_bdev_name) {
			SPDK_ERRLOG("Split configuration missing blockdev name\n");
			return -1;
		}

		base_bdev = spdk_bdev_get_by_name(base_bdev_name);
		if (!base_bdev) {
			SPDK_ERRLOG("Could not find Split bdev %s\n", base_bdev_name);
			return -1;
		}

		split_count_str = spdk_conf_section_get_nmval(sp, "Split", i, 1);
		if (!split_count_str) {
			SPDK_ERRLOG("Split configuration missing split count\n");
			return -1;
		}

		split_count = atoi(split_count_str);
		if (split_count < 1) {
			SPDK_ERRLOG("Invalid Split count %d\n", split_count);
			return -1;
		}

		/* Optional split size in MB */
		split_size = 0;
		split_size_str = spdk_conf_section_get_nmval(sp, "Split", i, 2);
		if (split_size_str) {
			split_size = atoi(split_size_str);
			if (split_size <= 0) {
				SPDK_ERRLOG("Invalid Split size %d\n", split_size);
				return -1;
			}
		}

		if (vbdev_split_create(base_bdev, split_count, split_size)) {
			return -1;
		}
	}

	return 0;
}

static void
vbdev_split_fini(void)
{
	struct split_disk *split_disk, *tmp;

	TAILQ_FOREACH_SAFE(split_disk, &g_split_disks, tailq, tmp) {
		vbdev_split_free(split_disk);
	}
}

SPDK_VBDEV_MODULE_REGISTER(vbdev_split_init, vbdev_split_fini, NULL, NULL)
SPDK_LOG_REGISTER_TRACE_FLAG("vbdev_split", SPDK_TRACE_VBDEV_SPLIT)
