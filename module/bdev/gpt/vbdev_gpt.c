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

#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk/bdev_module.h"
#include "spdk/log.h"

static int vbdev_gpt_init(void);
static void vbdev_gpt_examine(struct spdk_bdev *bdev);
static int vbdev_gpt_get_ctx_size(void);

static struct spdk_bdev_module gpt_if = {
	.name = "gpt",
	.module_init = vbdev_gpt_init,
	.get_ctx_size = vbdev_gpt_get_ctx_size,
	.examine_disk = vbdev_gpt_examine,

};
SPDK_BDEV_MODULE_REGISTER(gpt, &gpt_if)

/* Base block device gpt context */
struct gpt_base {
	struct spdk_gpt			gpt;
	struct spdk_bdev_part_base	*part_base;
	SPDK_BDEV_PART_TAILQ		parts;

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

struct gpt_io {
	struct spdk_io_channel *ch;
	struct spdk_bdev_io *bdev_io;

	/* for bdev_io_wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;
};

static void
gpt_base_free(void *ctx)
{
	struct gpt_base *gpt_base = ctx;

	spdk_free(gpt_base->gpt.buf);
	free(gpt_base);
}

static void
gpt_base_bdev_hotremove_cb(void *_part_base)
{
	struct spdk_bdev_part_base *part_base = _part_base;
	struct gpt_base *gpt_base = spdk_bdev_part_base_get_ctx(part_base);

	spdk_bdev_part_base_hotremove(part_base, &gpt_base->parts);
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
gpt_base_bdev_init(struct spdk_bdev *bdev)
{
	struct gpt_base *gpt_base;
	struct spdk_gpt *gpt;

	gpt_base = calloc(1, sizeof(*gpt_base));
	if (!gpt_base) {
		SPDK_ERRLOG("Cannot alloc memory for gpt_base pointer\n");
		return NULL;
	}

	TAILQ_INIT(&gpt_base->parts);
	gpt_base->part_base = spdk_bdev_part_base_construct(bdev,
			      gpt_base_bdev_hotremove_cb,
			      &gpt_if, &vbdev_gpt_fn_table,
			      &gpt_base->parts, gpt_base_free, gpt_base,
			      sizeof(struct gpt_channel), NULL, NULL);
	if (!gpt_base->part_base) {
		free(gpt_base);
		SPDK_ERRLOG("cannot construct gpt_base");
		return NULL;
	}

	gpt = &gpt_base->gpt;
	gpt->parse_phase = SPDK_GPT_PARSE_PHASE_PRIMARY;
	gpt->buf_size = spdk_max(SPDK_GPT_BUFFER_SIZE, bdev->blocklen);
	gpt->buf = spdk_zmalloc(gpt->buf_size, spdk_bdev_get_buf_align(bdev), NULL,
				SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!gpt->buf) {
		SPDK_ERRLOG("Cannot alloc buf\n");
		spdk_bdev_part_base_free(gpt_base->part_base);
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

	return spdk_bdev_part_free(&gpt_disk->part);
}

static void
_vbdev_gpt_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io);

static void
vbdev_gpt_resubmit_request(void *arg)
{
	struct gpt_io *io = (struct gpt_io *)arg;

	_vbdev_gpt_submit_request(io->ch, io->bdev_io);
}

static void
vbdev_gpt_queue_io(struct gpt_io *io)
{
	struct gpt_channel *ch = spdk_io_channel_get_ctx(io->ch);
	int rc;

	io->bdev_io_wait.bdev = io->bdev_io->bdev;
	io->bdev_io_wait.cb_fn = vbdev_gpt_resubmit_request;
	io->bdev_io_wait.cb_arg = io;

	rc = spdk_bdev_queue_io_wait(io->bdev_io->bdev,
				     ch->part_ch.base_ch, &io->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("Queue io failed in vbdev_gpt_queue_io, rc=%d.\n", rc);
		spdk_bdev_io_complete(io->bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
vbdev_gpt_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	_vbdev_gpt_submit_request(ch, bdev_io);
}

static void
_vbdev_gpt_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct gpt_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct gpt_io *io = (struct gpt_io *)bdev_io->driver_ctx;
	int rc;

	rc = spdk_bdev_part_submit_request(&ch->part_ch, bdev_io);
	if (rc) {
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(vbdev_gpt, "gpt: no memory, queue io\n");
			io->ch = _ch;
			io->bdev_io = bdev_io;
			vbdev_gpt_queue_io(io);
		} else {
			SPDK_ERRLOG("gpt: error on bdev_io submission, rc=%d.\n", rc);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

static void
vbdev_gpt_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, vbdev_gpt_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	default:
		_vbdev_gpt_submit_request(_ch, bdev_io);
		break;
	}
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
	struct gpt_disk *gpt_disk = SPDK_CONTAINEROF(ctx, struct gpt_disk, part);
	struct spdk_bdev_part_base *base_bdev = spdk_bdev_part_get_base(&gpt_disk->part);
	struct gpt_base *gpt_base = spdk_bdev_part_base_get_ctx(base_bdev);
	struct spdk_bdev *part_base_bdev = spdk_bdev_part_base_get_bdev(base_bdev);
	struct spdk_gpt *gpt = &gpt_base->gpt;
	struct spdk_gpt_partition_entry *gpt_entry = &gpt->partitions[gpt_disk->partition_index];
	uint64_t offset_blocks = spdk_bdev_part_get_offset_blocks(&gpt_disk->part);

	spdk_json_write_named_object_begin(w, "gpt");

	spdk_json_write_named_string(w, "base_bdev", spdk_bdev_get_name(part_base_bdev));

	spdk_json_write_named_uint64(w, "offset_blocks", offset_blocks);

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
	uint32_t num_partitions;
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
	num_partitions = 0;

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
		base_bdev = spdk_bdev_part_base_get_bdev(gpt_base->part_base);
		name = spdk_sprintf_alloc("%sp%" PRIu64, spdk_bdev_get_name(base_bdev), i + 1);
		if (!name) {
			SPDK_ERRLOG("name allocation failure\n");
			free(d);
			return -1;
		}

		rc = spdk_bdev_part_construct(&d->part, gpt_base->part_base, name,
					      lba_start, lba_end - lba_start, "GPT Disk");
		free(name);
		if (rc) {
			SPDK_ERRLOG("could not construct bdev part\n");
			/* spdk_bdev_part_construct will free name on failure */
			free(d);
			return -1;
		}
		num_partitions++;
		d->partition_index = i;
	}

	return num_partitions;
}

static void
gpt_read_secondary_table_complete(struct spdk_bdev_io *bdev_io, bool status, void *arg)
{
	struct gpt_base *gpt_base = (struct gpt_base *)arg;
	struct spdk_bdev *bdev = spdk_bdev_part_base_get_bdev(gpt_base->part_base);
	int rc, num_partitions = 0;

	spdk_bdev_free_io(bdev_io);
	spdk_put_io_channel(gpt_base->ch);
	gpt_base->ch = NULL;

	if (status != SPDK_BDEV_IO_STATUS_SUCCESS) {
		SPDK_ERRLOG("Gpt: bdev=%s io error status=%d\n",
			    spdk_bdev_get_name(bdev), status);
		goto end;
	}

	rc = gpt_parse_partition_table(&gpt_base->gpt);
	if (rc) {
		SPDK_DEBUGLOG(vbdev_gpt, "Failed to parse secondary partition table\n");
		goto end;
	}

	SPDK_WARNLOG("Gpt: bdev=%s primary partition table broken, use the secondary\n",
		     spdk_bdev_get_name(bdev));

	num_partitions = vbdev_gpt_create_bdevs(gpt_base);
	if (num_partitions < 0) {
		SPDK_DEBUGLOG(vbdev_gpt, "Failed to split dev=%s by gpt table\n",
			      spdk_bdev_get_name(bdev));
	}

end:
	spdk_bdev_module_examine_done(&gpt_if);
	if (num_partitions <= 0) {
		/* If no gpt_disk instances were created, free the base context */
		spdk_bdev_part_base_free(gpt_base->part_base);
	}
}

static int
vbdev_gpt_read_secondary_table(struct gpt_base *gpt_base)
{
	struct spdk_gpt *gpt;
	struct spdk_bdev_desc *part_base_desc;
	uint64_t secondary_offset;

	gpt = &gpt_base->gpt;
	gpt->parse_phase = SPDK_GPT_PARSE_PHASE_SECONDARY;
	gpt->header = NULL;
	gpt->partitions = NULL;

	part_base_desc = spdk_bdev_part_base_get_desc(gpt_base->part_base);

	secondary_offset = gpt->total_sectors * gpt->sector_size - gpt->buf_size;
	return spdk_bdev_read(part_base_desc, gpt_base->ch, gpt_base->gpt.buf, secondary_offset,
			      gpt_base->gpt.buf_size, gpt_read_secondary_table_complete,
			      gpt_base);
}

static void
gpt_bdev_complete(struct spdk_bdev_io *bdev_io, bool status, void *arg)
{
	struct gpt_base *gpt_base = (struct gpt_base *)arg;
	struct spdk_bdev *bdev = spdk_bdev_part_base_get_bdev(gpt_base->part_base);
	int rc, num_partitions = 0;

	spdk_bdev_free_io(bdev_io);

	if (status != SPDK_BDEV_IO_STATUS_SUCCESS) {
		SPDK_ERRLOG("Gpt: bdev=%s io error status=%d\n",
			    spdk_bdev_get_name(bdev), status);
		goto end;
	}

	rc = gpt_parse_mbr(&gpt_base->gpt);
	if (rc) {
		SPDK_DEBUGLOG(vbdev_gpt, "Failed to parse mbr\n");
		goto end;
	}

	rc = gpt_parse_partition_table(&gpt_base->gpt);
	if (rc) {
		SPDK_DEBUGLOG(vbdev_gpt, "Failed to parse primary partition table\n");
		rc = vbdev_gpt_read_secondary_table(gpt_base);
		if (rc) {
			SPDK_ERRLOG("Failed to read secondary table\n");
			goto end;
		}
		return;
	}

	num_partitions = vbdev_gpt_create_bdevs(gpt_base);
	if (num_partitions < 0) {
		SPDK_DEBUGLOG(vbdev_gpt, "Failed to split dev=%s by gpt table\n",
			      spdk_bdev_get_name(bdev));
	}

end:
	spdk_put_io_channel(gpt_base->ch);
	gpt_base->ch = NULL;
	/*
	 * Notify the generic bdev layer that the actions related to the original examine
	 *  callback are now completed.
	 */
	spdk_bdev_module_examine_done(&gpt_if);

	/*
	 * vbdev_gpt_create_bdevs returns the number of bdevs created upon success.
	 * We can branch on this value.
	 */
	if (num_partitions <= 0) {
		/* If no gpt_disk instances were created, free the base context */
		spdk_bdev_part_base_free(gpt_base->part_base);
	}
}

static int
vbdev_gpt_read_gpt(struct spdk_bdev *bdev)
{
	struct gpt_base *gpt_base;
	struct spdk_bdev_desc *part_base_desc;
	int rc;

	gpt_base = gpt_base_bdev_init(bdev);
	if (!gpt_base) {
		SPDK_ERRLOG("Cannot allocated gpt_base\n");
		return -1;
	}

	part_base_desc = spdk_bdev_part_base_get_desc(gpt_base->part_base);
	gpt_base->ch = spdk_bdev_get_io_channel(part_base_desc);
	if (gpt_base->ch == NULL) {
		SPDK_ERRLOG("Failed to get an io_channel.\n");
		spdk_bdev_part_base_free(gpt_base->part_base);
		return -1;
	}

	rc = spdk_bdev_read(part_base_desc, gpt_base->ch, gpt_base->gpt.buf, 0,
			    gpt_base->gpt.buf_size, gpt_bdev_complete, gpt_base);
	if (rc < 0) {
		spdk_put_io_channel(gpt_base->ch);
		spdk_bdev_part_base_free(gpt_base->part_base);
		SPDK_ERRLOG("Failed to send bdev_io command\n");
		return -1;
	}

	return 0;
}

static int
vbdev_gpt_init(void)
{
	return 0;
}

static int
vbdev_gpt_get_ctx_size(void)
{
	return sizeof(struct gpt_io);
}

static void
vbdev_gpt_examine(struct spdk_bdev *bdev)
{
	int rc;

	/* A bdev with fewer than 2 blocks cannot have a GPT. Block 0 has
	 * the MBR and block 1 has the GPT header.
	 */
	if (spdk_bdev_get_num_blocks(bdev) < 2) {
		spdk_bdev_module_examine_done(&gpt_if);
		return;
	}

	if (spdk_bdev_get_block_size(bdev) % 512 != 0) {
		SPDK_DEBUGLOG(vbdev_gpt,
			      "GPT module does not support block size %" PRIu32 " for bdev %s\n",
			      spdk_bdev_get_block_size(bdev), spdk_bdev_get_name(bdev));
		spdk_bdev_module_examine_done(&gpt_if);
		return;
	}

	rc = vbdev_gpt_read_gpt(bdev);
	if (rc) {
		spdk_bdev_module_examine_done(&gpt_if);
		SPDK_ERRLOG("Failed to read info from bdev %s\n", spdk_bdev_get_name(bdev));
	}
}

SPDK_LOG_REGISTER_COMPONENT(vbdev_gpt)
