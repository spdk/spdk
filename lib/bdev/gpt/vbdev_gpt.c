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
 * This driver reads a GPT partition table from a bdev and exposes a virtual block device for
 * each partition.
 */

#include "gpt.h"

#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

SPDK_DECLARE_BDEV_MODULE(gpt);

/* Base block device gpt context */
struct gpt_base {
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	struct spdk_gpt gpt;
	struct spdk_io_channel *ch;
	uint32_t ref;
};

/* Context for each gpt virtual bdev */
struct gpt_disk {
	struct spdk_bdev	disk;
	uint32_t		partition_index;
	struct gpt_base		*base;
	uint64_t		offset_blocks;
	TAILQ_ENTRY(gpt_disk)	tailq;
};

static TAILQ_HEAD(, gpt_disk) g_gpt_disks = TAILQ_HEAD_INITIALIZER(g_gpt_disks);
static TAILQ_HEAD(, spdk_bdev) g_bdevs = TAILQ_HEAD_INITIALIZER(g_bdevs);

static bool g_gpt_disabled;

static void
spdk_gpt_base_free(struct gpt_base *gpt_base)
{
	assert(gpt_base->ch == NULL);
	assert(gpt_base->bdev);
	assert(gpt_base->desc);
	if (gpt_base->bdev->claim_module == SPDK_GET_BDEV_MODULE(gpt)) {
		spdk_bdev_module_release_bdev(gpt_base->bdev);
	}
	spdk_bdev_close(gpt_base->desc);
	spdk_dma_free(gpt_base->gpt.buf);
	free(gpt_base);
}

static void
spdk_gpt_base_bdev_hotremove_cb(void *remove_ctx)
{
	struct spdk_bdev *base_bdev = remove_ctx;
	struct gpt_disk *gpt_disk, *tmp;

	TAILQ_FOREACH_SAFE(gpt_disk, &g_gpt_disks, tailq, tmp) {
		if (gpt_disk->base->bdev == base_bdev) {
			spdk_bdev_unregister(&gpt_disk->disk);
		}
	}
}

static struct gpt_base *
spdk_gpt_base_bdev_init(struct spdk_bdev *bdev)
{
	struct gpt_base *gpt_base;
	struct spdk_gpt *gpt;
	int rc;

	gpt_base = calloc(1, sizeof(*gpt_base));
	if (!gpt_base) {
		SPDK_ERRLOG("Cannot alloc memory for gpt_base pointer\n");
		return NULL;
	}

	gpt_base->bdev = bdev;
	gpt_base->ref = 0;

	gpt = &gpt_base->gpt;
	gpt->buf = spdk_dma_zmalloc(SPDK_GPT_BUFFER_SIZE, 0x1000, NULL);
	if (!gpt->buf) {
		SPDK_ERRLOG("Cannot alloc buf\n");
		free(gpt_base);
		return NULL;
	}

	gpt->sector_size = bdev->blocklen;
	gpt->total_sectors = bdev->blockcnt;
	gpt->lba_start = 0;
	gpt->lba_end = gpt->total_sectors - 1;

	rc = spdk_bdev_open(gpt_base->bdev, false, spdk_gpt_base_bdev_hotremove_cb, bdev,
			    &gpt_base->desc);
	if (rc != 0) {
		SPDK_ERRLOG("Could not open bdev %s, error=%d\n",
			    spdk_bdev_get_name(gpt_base->bdev), rc);
		spdk_dma_free(gpt->buf);
		free(gpt_base);
		return NULL;
	}

	gpt_base->ch = spdk_bdev_get_io_channel(gpt_base->desc);
	if (!gpt_base->ch) {
		SPDK_ERRLOG("Cannot allocate ch\n");
		spdk_bdev_close(gpt_base->desc);
		spdk_dma_free(gpt->buf);
		free(gpt_base);
		return NULL;
	}

	return gpt_base;

}

static void
gpt_read(struct gpt_disk *gpt_disk, struct spdk_bdev_io *bdev_io)
{
	bdev_io->u.read.offset_blocks += gpt_disk->offset_blocks;
}

static void
gpt_write(struct gpt_disk *gpt_disk, struct spdk_bdev_io *bdev_io)
{
	bdev_io->u.write.offset_blocks += gpt_disk->offset_blocks;
}

static void
gpt_unmap(struct gpt_disk *gpt_disk, struct spdk_bdev_io *bdev_io)
{
	bdev_io->u.unmap.offset_blocks += gpt_disk->offset_blocks;
}

static void
gpt_flush(struct gpt_disk *gpt_disk, struct spdk_bdev_io *bdev_io)
{
	bdev_io->u.flush.offset_blocks += gpt_disk->offset_blocks;
}


static void
_vbdev_gpt_complete_reset(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *gpt_io = cb_arg;
	struct spdk_io_channel *base_ch = *(struct spdk_io_channel **)gpt_io->driver_ctx;

	spdk_put_io_channel(base_ch);
	spdk_bdev_io_complete(gpt_io, success);
	spdk_bdev_free_io(bdev_io);
}

static void
vbdev_gpt_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct gpt_disk *gpt_disk = bdev_io->bdev->ctxt;
	struct spdk_io_channel *base_ch;

	/* Modify the I/O to adjust for the offset within the base bdev. */
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		gpt_read(gpt_disk, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		gpt_write(gpt_disk, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		gpt_unmap(gpt_disk, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		gpt_flush(gpt_disk, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		base_ch = spdk_get_io_channel(gpt_disk->base->bdev);
		*(struct spdk_io_channel **)bdev_io->driver_ctx = base_ch;
		spdk_bdev_reset(gpt_disk->base->desc, base_ch,
				_vbdev_gpt_complete_reset, bdev_io);
		return;
	default:
		SPDK_ERRLOG("gpt: unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	/* Submit the modified I/O to the underlying bdev. */
	spdk_bdev_io_resubmit(bdev_io, gpt_disk->base->desc);
}

static void
vbdev_gpt_base_get_ref(struct gpt_base *gpt_base,
		       struct gpt_disk *gpt_disk)
{
	__sync_fetch_and_add(&gpt_base->ref, 1);
	gpt_disk->base = gpt_base;
}

static void
vbdev_gpt_base_put_ref(struct gpt_base *gpt_base)
{
	if (__sync_sub_and_fetch(&gpt_base->ref, 1) == 0) {
		spdk_gpt_base_free(gpt_base);
	}
}

static void
vbdev_gpt_free(struct gpt_disk *gpt_disk)
{
	struct gpt_base *gpt_base;

	if (!gpt_disk) {
		return;
	}

	gpt_base = gpt_disk->base;

	TAILQ_REMOVE(&g_gpt_disks, gpt_disk, tailq);
	free(gpt_disk->disk.name);
	free(gpt_disk);

	assert(gpt_base != NULL);
	vbdev_gpt_base_put_ref(gpt_base);
}

static int
vbdev_gpt_destruct(void *ctx)
{
	struct gpt_disk *gpt_disk = ctx;

	vbdev_gpt_free(gpt_disk);
	return 0;
}

static bool
vbdev_gpt_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct gpt_disk *gpt_disk = ctx;

	return gpt_disk->base->bdev->fn_table->io_type_supported(gpt_disk->base->bdev,
			io_type);
}

static struct spdk_io_channel *
vbdev_gpt_get_io_channel(void *ctx)
{
	struct gpt_disk *gpt_disk = ctx;

	return gpt_disk->base->bdev->fn_table->get_io_channel(gpt_disk->base->bdev);
}

static void
write_guid(struct spdk_json_write_ctx *w, const struct spdk_gpt_guid *guid)
{
	spdk_json_write_string_fmt(w, "%08x-%04x-%04x-%04x-%04x%08x",
				   from_le32(&guid->raw[0]),
				   from_le16(&guid->raw[4]),
				   from_le16(&guid->raw[6]),
				   from_be16(&guid->raw[8]),
				   from_be16(&guid->raw[10]),
				   from_be32(&guid->raw[12]));
}

static void
write_string_utf16le(struct spdk_json_write_ctx *w, const uint16_t *str, size_t max_len)
{
	size_t len;
	const uint16_t *p;

	for (len = 0, p = str; len < max_len && *p; p++) {
		len++;
	}

	spdk_json_write_string_utf16le_raw(w, str, len);
}

static int
vbdev_gpt_dump_config_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct gpt_disk *gpt_disk = ctx;
	struct spdk_gpt *gpt = &gpt_disk->base->gpt;
	struct spdk_gpt_partition_entry *gpt_entry = &gpt->partitions[gpt_disk->partition_index];

	spdk_json_write_name(w, "gpt");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "base_bdev");
	spdk_json_write_string(w, spdk_bdev_get_name(gpt_disk->base->bdev));

	spdk_json_write_name(w, "offset_blocks");
	spdk_json_write_uint64(w, gpt_disk->offset_blocks);

	spdk_json_write_name(w, "partition_type_guid");
	write_guid(w, &gpt_entry->part_type_guid);

	spdk_json_write_name(w, "unique_partition_guid");
	write_guid(w, &gpt_entry->unique_partition_guid);

	spdk_json_write_name(w, "partition_name");
	write_string_utf16le(w, gpt_entry->partition_name, SPDK_COUNTOF(gpt_entry->partition_name));

	spdk_json_write_object_end(w);

	return 0;
}

static struct spdk_bdev_fn_table vbdev_gpt_fn_table = {
	.destruct		= vbdev_gpt_destruct,
	.io_type_supported	= vbdev_gpt_io_type_supported,
	.submit_request		= vbdev_gpt_submit_request,
	.get_io_channel		= vbdev_gpt_get_io_channel,
	.dump_config_json	= vbdev_gpt_dump_config_json,
};

static int
vbdev_gpt_create_bdevs(struct gpt_base *gpt_base)
{
	uint32_t num_partition_entries;
	uint64_t i, head_lba_start, head_lba_end;
	struct spdk_gpt_partition_entry *p;
	struct gpt_disk *d;
	struct spdk_bdev *base_bdev = gpt_base->bdev;
	struct spdk_gpt *gpt;

	gpt = &gpt_base->gpt;
	num_partition_entries = from_le32(&gpt->header->num_partition_entries);
	head_lba_start = from_le64(&gpt->header->first_usable_lba);
	head_lba_end = from_le64(&gpt->header->last_usable_lba);

	for (i = 0; i < num_partition_entries; i++) {
		p = &gpt->partitions[i];
		uint64_t lba_start = from_le64(&p->starting_lba);
		uint64_t lba_end = from_le64(&p->ending_lba);

		if (!SPDK_GPT_GUID_EQUAL(&gpt->partitions[i].part_type_guid,
					 &SPDK_GPT_PART_TYPE_GUID) ||
		    lba_start == 0) {
			continue;
		}
		if (lba_start < head_lba_start || lba_end > head_lba_end) {
			continue;
		}

		d = calloc(1, sizeof(*d));
		if (!d) {
			SPDK_ERRLOG("Memory allocation failure\n");
			return -1;
		}

		/* Copy properties of the base bdev */
		d->disk.blocklen = base_bdev->blocklen;
		d->disk.write_cache = base_bdev->write_cache;
		d->disk.need_aligned_buffer = base_bdev->need_aligned_buffer;

		/* index start at 1 instead of 0 to match the existing style */
		d->disk.name = spdk_sprintf_alloc("%sp%" PRIu64, spdk_bdev_get_name(base_bdev), i + 1);
		if (!d->disk.name) {
			free(d);
			SPDK_ERRLOG("Failed to allocate disk name\n");
			return -1;
		}

		d->partition_index = i;
		d->disk.product_name = "GPT Disk";
		d->offset_blocks = lba_start;
		d->disk.blockcnt = lba_end - lba_start;
		d->disk.ctxt = d;
		d->disk.fn_table = &vbdev_gpt_fn_table;
		d->disk.module = SPDK_GET_BDEV_MODULE(gpt);

		SPDK_DEBUGLOG(SPDK_TRACE_VBDEV_GPT, "gpt vbdev %s: base bdev: %s"
			      " offset_blocks: %" PRIu64 "\n",
			      d->disk.name, spdk_bdev_get_name(base_bdev), d->offset_blocks);

		vbdev_gpt_base_get_ref(gpt_base, d);

		spdk_vbdev_register(&d->disk, &base_bdev, 1);

		TAILQ_INSERT_TAIL(&g_gpt_disks, d, tailq);
	}

	return 0;
}

static void
spdk_gpt_bdev_complete(struct spdk_bdev_io *bdev_io, bool status, void *arg)
{
	struct gpt_base *gpt_base = (struct gpt_base *)arg;
	struct spdk_bdev *bdev = gpt_base->bdev;
	int rc;

	/* free the ch and also close the bdev_desc */
	spdk_put_io_channel(gpt_base->ch);
	gpt_base->ch = NULL;
	spdk_bdev_free_io(bdev_io);

	if (status != SPDK_BDEV_IO_STATUS_SUCCESS) {
		SPDK_ERRLOG("Gpt: bdev=%s io error status=%d\n",
			    spdk_bdev_get_name(bdev), status);
		goto end;
	}

	rc = spdk_gpt_parse(&gpt_base->gpt);
	if (rc) {
		SPDK_DEBUGLOG(SPDK_TRACE_VBDEV_GPT, "Failed to parse gpt\n");
		goto end;
	}

	rc = spdk_bdev_module_claim_bdev(bdev, gpt_base->desc, SPDK_GET_BDEV_MODULE(gpt));
	if (rc) {
		SPDK_ERRLOG("could not claim bdev %s\n", spdk_bdev_get_name(bdev));
		goto end;
	}

	rc = vbdev_gpt_create_bdevs(gpt_base);
	if (rc < 0) {
		SPDK_DEBUGLOG(SPDK_TRACE_VBDEV_GPT, "Failed to split dev=%s by gpt table\n",
			      spdk_bdev_get_name(bdev));
	}

end:
	/*
	 * Notify the generic bdev layer that the actions related to the original examine
	 *  callback are now completed.
	 */
	spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(gpt));

	if (gpt_base->ref == 0) {
		/* If no gpt_disk instances were created, free the base context */
		spdk_gpt_base_free(gpt_base);
	}
}

static int
vbdev_gpt_read_gpt(struct spdk_bdev *bdev)
{
	struct gpt_base *gpt_base;
	int rc;

	gpt_base = spdk_gpt_base_bdev_init(bdev);
	if (!gpt_base) {
		SPDK_ERRLOG("Cannot allocated gpt_base\n");
		return -1;
	}

	rc = spdk_bdev_read(gpt_base->desc, gpt_base->ch, gpt_base->gpt.buf, 0, SPDK_GPT_BUFFER_SIZE,
			    spdk_gpt_bdev_complete, gpt_base);
	if (rc < 0) {
		spdk_gpt_base_free(gpt_base);
		SPDK_ERRLOG("Failed to send bdev_io command\n");
		return -1;
	}

	return 0;
}

static int
vbdev_gpt_init(void)
{
	struct spdk_conf_section *sp = spdk_conf_find_section(NULL, "Gpt");

	if (sp && spdk_conf_section_get_boolval(sp, "Disable", false)) {
		/* Disable Gpt probe */
		g_gpt_disabled = true;
	}

	return 0;
}

static void
vbdev_gpt_fini(void)
{
	struct gpt_disk *gpt_disk, *tmp;

	TAILQ_FOREACH_SAFE(gpt_disk, &g_gpt_disks, tailq, tmp) {
		vbdev_gpt_free(gpt_disk);
	}
}

static void
vbdev_gpt_examine(struct spdk_bdev *bdev)
{
	int rc;

	if (g_gpt_disabled) {
		spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(gpt));
		return;
	}

	rc = vbdev_gpt_read_gpt(bdev);
	if (rc) {
		spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(gpt));
		SPDK_ERRLOG("Failed to read info from bdev %s\n", spdk_bdev_get_name(bdev));
	}
}

static int
vbdev_gpt_get_ctx_size(void)
{
	/*
	 * Note: this context is only used for RESET operations, since it is the only
	 *  I/O type that does not just resubmit to the base bdev.
	 */
	return sizeof(struct spdk_io_channel *);
}

SPDK_BDEV_MODULE_REGISTER(gpt, vbdev_gpt_init, vbdev_gpt_fini, NULL,
			  vbdev_gpt_get_ctx_size, vbdev_gpt_examine)
SPDK_LOG_REGISTER_TRACE_FLAG("vbdev_gpt", SPDK_TRACE_VBDEV_GPT)
