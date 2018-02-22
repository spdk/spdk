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
	struct spdk_bdev_part_base	part_base;
	struct spdk_gpt			gpt;

	/* This channel is only used for reading the partition table. */
	struct spdk_io_channel		*ch;
};

/* Context for each gpt virtual bdev */
struct gpt_disk {
	struct spdk_bdev_part	part;
	uint32_t		partition_index;
};

struct gpt_channel {
	struct spdk_bdev_part_channel	part_ch;
};

static SPDK_BDEV_PART_TAILQ g_gpt_disks = TAILQ_HEAD_INITIALIZER(g_gpt_disks);

static bool g_gpt_disabled;

static void
spdk_gpt_base_free(struct spdk_bdev_part_base *base)
{
	struct gpt_base *gpt_base = SPDK_CONTAINEROF(base, struct gpt_base, part_base);

	spdk_dma_free(gpt_base->gpt.buf);
	free(gpt_base);
}

static void
spdk_gpt_base_bdev_hotremove_cb(void *_base_bdev)
{
	spdk_bdev_part_base_hotremove(_base_bdev, &g_gpt_disks);
}

static int vbdev_gpt_destruct(void *ctx);
static void vbdev_gpt_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io);
static int vbdev_gpt_dump_info_json(void *ctx, struct spdk_json_write_ctx *w);

static struct spdk_bdev_fn_table vbdev_gpt_fn_table = {
	.destruct		= vbdev_gpt_destruct,
	.submit_request		= vbdev_gpt_submit_request,
	.dump_info_json		= vbdev_gpt_dump_info_json,
};

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

	rc = spdk_bdev_part_base_construct(&gpt_base->part_base, bdev,
					   spdk_gpt_base_bdev_hotremove_cb,
					   SPDK_GET_BDEV_MODULE(gpt), &vbdev_gpt_fn_table,
					   &g_gpt_disks, spdk_gpt_base_free,
					   sizeof(struct gpt_channel), NULL, NULL);
	if (rc) {
		SPDK_ERRLOG("cannot construct gpt_base");
		return NULL;
	}

	gpt = &gpt_base->gpt;
	gpt->buf = spdk_dma_zmalloc(SPDK_GPT_BUFFER_SIZE, 0x1000, NULL);
	if (!gpt->buf) {
		SPDK_ERRLOG("Cannot alloc buf\n");
		spdk_bdev_part_base_free(&gpt_base->part_base);
		return NULL;
	}

	gpt->sector_size = bdev->blocklen;
	gpt->total_sectors = bdev->blockcnt;
	gpt->lba_start = 0;
	gpt->lba_end = gpt->total_sectors - 1;

	return gpt_base;
}

static int
vbdev_gpt_destruct(void *ctx)
{
	struct gpt_disk *gpt_disk = ctx;

	spdk_bdev_part_free(&gpt_disk->part);
	return 0;
}

static void
vbdev_gpt_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct gpt_channel *ch = spdk_io_channel_get_ctx(_ch);

	spdk_bdev_part_submit_request(&ch->part_ch, bdev_io);
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
vbdev_gpt_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct gpt_disk *gpt_disk = ctx;
	struct gpt_base *gpt_base = (struct gpt_base *)gpt_disk->part.base;
	struct spdk_gpt *gpt = &gpt_base->gpt;
	struct spdk_gpt_partition_entry *gpt_entry = &gpt->partitions[gpt_disk->partition_index];

	spdk_json_write_name(w, "gpt");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "base_bdev");
	spdk_json_write_string(w, spdk_bdev_get_name(gpt_disk->part.base->bdev));

	spdk_json_write_name(w, "offset_blocks");
	spdk_json_write_uint64(w, gpt_disk->part.offset_blocks);

	spdk_json_write_name(w, "partition_type_guid");
	write_guid(w, &gpt_entry->part_type_guid);

	spdk_json_write_name(w, "unique_partition_guid");
	write_guid(w, &gpt_entry->unique_partition_guid);

	spdk_json_write_name(w, "partition_name");
	write_string_utf16le(w, gpt_entry->partition_name, SPDK_COUNTOF(gpt_entry->partition_name));

	spdk_json_write_object_end(w);

	return 0;
}

static int
vbdev_gpt_create_bdevs(struct gpt_base *gpt_base)
{
	uint32_t num_partition_entries;
	uint64_t i, head_lba_start, head_lba_end;
	struct spdk_gpt_partition_entry *p;
	struct gpt_disk *d;
	struct spdk_gpt *gpt;
	char *name;
	struct spdk_bdev *base_bdev;
	int rc;

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

		/* index start at 1 instead of 0 to match the existing style */
		base_bdev = gpt_base->part_base.bdev;
		name = spdk_sprintf_alloc("%sp%" PRIu64, spdk_bdev_get_name(base_bdev), i + 1);
		if (!name) {
			SPDK_ERRLOG("name allocation failure\n");
			free(d);
			return -1;
		}

		rc = spdk_bdev_part_construct(&d->part, &gpt_base->part_base, name,
					      lba_start, lba_end - lba_start, "GPT Disk");
		if (rc) {
			SPDK_ERRLOG("could not construct bdev part\n");
			/* spdk_bdev_part_construct will free name on failure */
			free(d);
			return -1;
		}

		d->partition_index = i;
	}

	return 0;
}

static void
spdk_gpt_bdev_complete(struct spdk_bdev_io *bdev_io, bool status, void *arg)
{
	struct gpt_base *gpt_base = (struct gpt_base *)arg;
	struct spdk_bdev *bdev = gpt_base->part_base.bdev;
	int rc;

	spdk_bdev_free_io(bdev_io);
	spdk_put_io_channel(gpt_base->ch);
	gpt_base->ch = NULL;

	if (status != SPDK_BDEV_IO_STATUS_SUCCESS) {
		SPDK_ERRLOG("Gpt: bdev=%s io error status=%d\n",
			    spdk_bdev_get_name(bdev), status);
		goto end;
	}

	rc = spdk_gpt_parse(&gpt_base->gpt);
	if (rc) {
		SPDK_DEBUGLOG(SPDK_LOG_VBDEV_GPT, "Failed to parse gpt\n");
		goto end;
	}

	rc = vbdev_gpt_create_bdevs(gpt_base);
	if (rc < 0) {
		SPDK_DEBUGLOG(SPDK_LOG_VBDEV_GPT, "Failed to split dev=%s by gpt table\n",
			      spdk_bdev_get_name(bdev));
	}

end:
	/*
	 * Notify the generic bdev layer that the actions related to the original examine
	 *  callback are now completed.
	 */
	spdk_bdev_module_examine_done(SPDK_GET_BDEV_MODULE(gpt));

	if (gpt_base->part_base.ref == 0) {
		/* If no gpt_disk instances were created, free the base context */
		spdk_bdev_part_base_free(&gpt_base->part_base);
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

	gpt_base->ch = spdk_bdev_get_io_channel(gpt_base->part_base.desc);
	if (gpt_base->ch == NULL) {
		SPDK_ERRLOG("Failed to get an io_channel.\n");
		spdk_bdev_part_base_free(&gpt_base->part_base);
		return -1;
	}

	rc = spdk_bdev_read(gpt_base->part_base.desc, gpt_base->ch, gpt_base->gpt.buf, 0,
			    SPDK_GPT_BUFFER_SIZE, spdk_gpt_bdev_complete, gpt_base);
	if (rc < 0) {
		spdk_put_io_channel(gpt_base->ch);
		spdk_bdev_part_base_free(&gpt_base->part_base);
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

SPDK_BDEV_MODULE_REGISTER(gpt, vbdev_gpt_init, NULL, NULL,
			  NULL, vbdev_gpt_examine)
SPDK_LOG_REGISTER_COMPONENT("vbdev_gpt", SPDK_LOG_VBDEV_GPT)
