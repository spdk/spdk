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
#include "spdk/string.h"
#include "spdk/io_channel.h"
#include "spdk/util.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

SPDK_DECLARE_BDEV_MODULE(split);

/* Base block device split context */
struct split_base {
	struct spdk_bdev	*bdev;
	struct spdk_bdev_desc	*desc;
	uint32_t		ref;
};

/* Context for each split virtual bdev */
struct split_disk {
	struct spdk_bdev	disk;
	struct split_base	*base;
	uint64_t		offset_blocks;
	uint64_t		offset_bytes;
	TAILQ_ENTRY(split_disk)	tailq;
};

static TAILQ_HEAD(, split_disk) g_split_disks = TAILQ_HEAD_INITIALIZER(g_split_disks);

struct split_channel {
	struct split_disk	*disk;
	struct spdk_io_channel	*base_ch;
};

static void
vbdev_split_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *split_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	spdk_bdev_io_complete(split_io, status);
	spdk_bdev_free_io(bdev_io);
}

static void
vbdev_split_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct split_channel *split_ch = spdk_io_channel_get_ctx(ch);
	struct split_disk *split_disk = split_ch->disk;
	struct spdk_io_channel *base_ch = split_ch->base_ch;
	struct spdk_bdev_desc *base_desc = split_disk->base->desc;
	uint64_t offset;
	int rc = 0;

	/* Modify the I/O to adjust for the offset within the base bdev. */
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		offset = bdev_io->u.read.offset + split_disk->offset_bytes;
		rc = spdk_bdev_readv(base_desc, base_ch, bdev_io->u.read.iovs,
				     bdev_io->u.read.iovcnt, offset,
				     bdev_io->u.read.len, vbdev_split_complete_io,
				     bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		offset = bdev_io->u.write.offset + split_disk->offset_bytes;
		rc = spdk_bdev_writev(base_desc, base_ch, bdev_io->u.write.iovs,
				      bdev_io->u.write.iovcnt, offset,
				      bdev_io->u.write.len, vbdev_split_complete_io,
				      bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		offset = bdev_io->u.unmap.offset + split_disk->offset_bytes;
		rc = spdk_bdev_unmap(base_desc, base_ch, offset, bdev_io->u.unmap.len,
				     vbdev_split_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		offset = bdev_io->u.flush.offset + split_disk->offset_bytes;
		rc = spdk_bdev_flush(base_desc, base_ch, offset, bdev_io->u.flush.len,
				     vbdev_split_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		rc = spdk_bdev_reset(split_disk->base->desc, base_ch,
				     vbdev_split_complete_io, bdev_io);
		break;
	default:
		SPDK_ERRLOG("split: unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (rc != 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
vbdev_split_base_get_ref(struct split_base *split_base, struct split_disk *split_disk)
{
	__sync_fetch_and_add(&split_base->ref, 1);
	split_disk->base = split_base;
}

static void
vbdev_split_base_free(struct split_base *split_base)
{
	assert(split_base->bdev);
	assert(split_base->desc);
	spdk_bdev_module_release_bdev(split_base->bdev);
	spdk_bdev_close(split_base->desc);
	free(split_base);
}

static void
vbdev_split_base_put_ref(struct split_base *split_base)
{
	if (__sync_sub_and_fetch(&split_base->ref, 1) == 0) {
		vbdev_split_base_free(split_base);
	}
}

static void
vbdev_split_free(struct split_disk *split_disk)
{
	struct split_base *split_base;

	if (!split_disk) {
		return;
	}

	split_base = split_disk->base;

	spdk_io_device_unregister(&split_disk->base, NULL);
	TAILQ_REMOVE(&g_split_disks, split_disk, tailq);
	free(split_disk->disk.name);
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

static void
vbdev_split_base_bdev_hotremove_cb(void *remove_ctx)
{
	struct spdk_bdev *base_bdev = remove_ctx;
	struct split_disk *split_disk, *tmp;

	TAILQ_FOREACH_SAFE(split_disk, &g_split_disks, tailq, tmp) {
		if (split_disk->base->bdev == base_bdev) {
			spdk_bdev_unregister(&split_disk->disk);
		}
	}
}

static bool
vbdev_split_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct split_disk *split_disk = ctx;

	return split_disk->base->bdev->fn_table->io_type_supported(split_disk->base->bdev, io_type);
}

static struct spdk_io_channel *
vbdev_split_get_io_channel(void *ctx)
{
	struct split_disk *split_disk = ctx;

	return spdk_get_io_channel(&split_disk->base);
}

static int
vbdev_split_dump_config_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct split_disk *split_disk = ctx;

	spdk_json_write_name(w, "split");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "base_bdev");
	spdk_json_write_string(w, spdk_bdev_get_name(split_disk->base->bdev));
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
split_channel_create_cb(void *io_device, void *ctx_buf)
{
	struct split_disk *disk = SPDK_CONTAINEROF(io_device, struct split_disk, base);
	struct split_channel *ch = ctx_buf;

	ch->disk = disk;
	ch->base_ch = spdk_bdev_get_io_channel(disk->base->desc);
	if (ch->base_ch == NULL) {
		return -1;
	}

	return 0;
}

static void
split_channel_destroy_cb(void *io_device, void *ctx_buf)
{
	struct split_channel *ch = ctx_buf;

	spdk_put_io_channel(ch->base_ch);
}

static int
vbdev_split_create(struct spdk_bdev *base_bdev, uint64_t split_count, uint64_t split_size_mb)
{
	uint64_t split_size_bytes, split_size_blocks, offset_bytes, offset_blocks;
	uint64_t max_split_count;
	uint64_t mb = 1024 * 1024;
	uint64_t i;
	int rc;
	struct split_base *split_base;

	if (split_size_mb) {
		if (((split_size_mb * mb) % base_bdev->blocklen) != 0) {
			SPDK_ERRLOG("Split size %" PRIu64 " MB is not possible with block size "
				    "%" PRIu32 "\n",
				    split_size_mb, base_bdev->blocklen);
			return -1;
		}
		split_size_blocks = (split_size_mb * mb) / base_bdev->blocklen;
		SPDK_DEBUGLOG(SPDK_TRACE_VBDEV_SPLIT, "Split size %" PRIu64 " MB specified by user\n",
			      split_size_mb);
	} else {
		split_size_blocks = base_bdev->blockcnt / split_count;
		SPDK_DEBUGLOG(SPDK_TRACE_VBDEV_SPLIT, "Split size not specified by user\n");
	}

	split_size_bytes = split_size_blocks * base_bdev->blocklen;

	max_split_count = base_bdev->blockcnt / split_size_blocks;
	if (split_count > max_split_count) {
		SPDK_WARNLOG("Split count %" PRIu64 " is greater than maximum possible split count "
			     "%" PRIu64 " - clamping\n", split_count, max_split_count);
		split_count = max_split_count;
	}

	SPDK_DEBUGLOG(SPDK_TRACE_VBDEV_SPLIT, "base_bdev: %s split_count: %" PRIu64
		      " split_size_bytes: %" PRIu64 "\n",
		      spdk_bdev_get_name(base_bdev), split_count, split_size_bytes);

	split_base = calloc(1, sizeof(*split_base));
	if (!split_base) {
		SPDK_ERRLOG("Cannot alloc memory for split base pointer\n");
		return -1;
	}
	split_base->bdev = base_bdev;
	split_base->ref = 0;

	rc = spdk_bdev_open(base_bdev, false, vbdev_split_base_bdev_hotremove_cb, base_bdev,
			    &split_base->desc);
	if (rc) {
		SPDK_ERRLOG("could not open bdev %s\n", spdk_bdev_get_name(base_bdev));
		free(split_base);
		return -1;
	}

	rc = spdk_bdev_module_claim_bdev(base_bdev, split_base->desc, SPDK_GET_BDEV_MODULE(split));
	if (rc) {
		SPDK_ERRLOG("could not claim bdev %s\n", spdk_bdev_get_name(base_bdev));
		spdk_bdev_close(split_base->desc);
		free(split_base);
		return -1;
	}

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

		/* Append partition number to the base bdev's name, e.g. Malloc0 -> Malloc0p0 */
		d->disk.name = spdk_sprintf_alloc("%sp%" PRIu64, spdk_bdev_get_name(base_bdev), i);
		if (!d->disk.name) {
			free(d);
			rc = -ENOMEM;
			goto cleanup;
		}
		d->disk.product_name = "Split Disk";
		d->offset_bytes = offset_bytes;
		d->offset_blocks = offset_blocks;
		d->disk.blockcnt = split_size_blocks;
		d->disk.ctxt = d;
		d->disk.fn_table = &vbdev_split_fn_table;
		d->disk.module = SPDK_GET_BDEV_MODULE(split);

		SPDK_DEBUGLOG(SPDK_TRACE_VBDEV_SPLIT, "Split vbdev %s: base bdev: %s offset_bytes: "
			      "%" PRIu64 " offset_blocks: %" PRIu64 "\n",
			      d->disk.name, spdk_bdev_get_name(base_bdev), d->offset_bytes, d->offset_blocks);

		vbdev_split_base_get_ref(split_base, d);

		spdk_io_device_register(&d->base, split_channel_create_cb, split_channel_destroy_cb,
					sizeof(struct split_channel));

		spdk_vbdev_register(&d->disk, &base_bdev, 1);

		TAILQ_INSERT_TAIL(&g_split_disks, d, tailq);

		offset_bytes += split_size_bytes;
		offset_blocks += split_size_blocks;
	}

	rc = 0;

cleanup:
	if (split_base->ref == 0) {
		/* If no split_disk instances were created, free the resources */
		vbdev_split_base_free(split_base);
	}

	return rc;
}

static int
vbdev_split_init(void)
{
	return 0;
}

static void
vbdev_split_examine(struct spdk_bdev *bdev)
{
	struct spdk_conf_section *sp;
	const char *base_bdev_name;
	const char *split_count_str;
	const char *split_size_str;
	int i, split_count, split_size;

	sp = spdk_conf_find_section(NULL, "Split");
	if (sp == NULL) {
		spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(split));
		return;
	}

	for (i = 0; ; i++) {
		if (!spdk_conf_section_get_nval(sp, "Split", i)) {
			break;
		}

		base_bdev_name = spdk_conf_section_get_nmval(sp, "Split", i, 0);
		if (!base_bdev_name) {
			SPDK_ERRLOG("Split configuration missing bdev name\n");
			break;
		}

		if (strcmp(base_bdev_name, bdev->name) != 0) {
			continue;
		}

		split_count_str = spdk_conf_section_get_nmval(sp, "Split", i, 1);
		if (!split_count_str) {
			SPDK_ERRLOG("Split configuration missing split count\n");
			break;
		}

		split_count = atoi(split_count_str);
		if (split_count < 1) {
			SPDK_ERRLOG("Invalid Split count %d\n", split_count);
			break;
		}

		/* Optional split size in MB */
		split_size = 0;
		split_size_str = spdk_conf_section_get_nmval(sp, "Split", i, 2);
		if (split_size_str) {
			split_size = atoi(split_size_str);
			if (split_size <= 0) {
				SPDK_ERRLOG("Invalid Split size %d\n", split_size);
				break;
			}
		}

		if (vbdev_split_create(bdev, split_count, split_size)) {
			SPDK_ERRLOG("could not split bdev %s\n", bdev->name);
			break;
		}
	}

	spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(split));
}

static void
vbdev_split_fini(void)
{
	struct split_disk *split_disk, *tmp;

	TAILQ_FOREACH_SAFE(split_disk, &g_split_disks, tailq, tmp) {
		vbdev_split_free(split_disk);
	}
}

static int
vbdev_split_get_ctx_size(void)
{
	return 0;
}

SPDK_BDEV_MODULE_REGISTER(split, vbdev_split_init, vbdev_split_fini, NULL,
			  vbdev_split_get_ctx_size, vbdev_split_examine)
SPDK_LOG_REGISTER_TRACE_FLAG("vbdev_split", SPDK_TRACE_VBDEV_SPLIT)
