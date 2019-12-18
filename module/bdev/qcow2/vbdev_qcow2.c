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
 * This driver tries to construct a QCOW2 bdev from a base bdev and
 * exposes a virtual block device if there is QCOW2 format contained in the oringinal bdev.
 */

#include "qcow2.h"

#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk/bdev_module.h"
#include "spdk_internal/log.h"

static int vbdev_qcow2_init(void);
static void vbdev_qcow2_examine(struct spdk_bdev *bdev);
static int vbdev_qcow2_get_ctx_size(void);

static struct spdk_bdev_module qcow2_if = {
	.name = "qcow2",
	.module_init = vbdev_qcow2_init,
	.get_ctx_size = vbdev_qcow2_get_ctx_size,
	.examine_disk = vbdev_qcow2_examine,

};
SPDK_BDEV_MODULE_REGISTER(qcow2, &qcow2_if)

/* Base block device qcow2 context */
struct qcow2_base {
	struct spdk_qcow2		qcow2;

	struct spdk_bdev_part_base	*base;

	SPDK_BDEV_PART_TAILQ		parts;

	/* This channel is only used for reading the Qcow2 header info. */
	struct spdk_io_channel		*ch;
};

/* Context for qcow2 virtual bdev */
struct qcow2_disk {
	struct spdk_bdev_part	part;
};

struct qcow2_channel {
	struct spdk_bdev_part_channel	part_ch;
};

struct qcow2_io {
	struct spdk_io_channel *ch;
	struct spdk_bdev_io *bdev_io;

	/* for bdev_io_wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;
};

static bool g_qcow2_disabled;

static void
spdk_qcow2_base_free(void *ctx)
{
	struct qcow2_base *qcow2_base = ctx;

	spdk_free(qcow2_base->qcow2.buf);
	free(qcow2_base);
}

static void
spdk_qcow2_base_bdev_hotremove_cb(void *_part_base)
{
	struct spdk_bdev_part_base *part_base = _part_base;
	struct qcow2_base *qcow2_base = spdk_bdev_part_base_get_ctx(part_base);

	spdk_bdev_part_base_hotremove(part_base, &qcow2_base->parts);
}

static int vbdev_qcow2_destruct(void *ctx);
static void vbdev_qcow2_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io);
static int vbdev_qcow2_dump_info_json(void *ctx, struct spdk_json_write_ctx *w);

static struct spdk_bdev_fn_table vbdev_qcow2_fn_table = {
	.destruct		= vbdev_qcow2_destruct,
	.submit_request		= vbdev_qcow2_submit_request,
	.dump_info_json		= vbdev_qcow2_dump_info_json,
};

static struct qcow2_base *
spdk_qcow2_base_bdev_init(struct spdk_bdev *bdev)
{
	struct qcow2_base *qcow2_base;
	struct spdk_qcow2 *qcow2;

	qcow2_base = calloc(1, sizeof(*qcow2_base));
	if (!qcow2_base) {
		SPDK_ERRLOG("Cannot alloc memory for qcow2_base pointer\n");
		return NULL;
	}

	TAILQ_INIT(&qcow2_base->parts);
	qcow2_base->base = spdk_bdev_part_base_construct(bdev,
			   spdk_qcow2_base_bdev_hotremove_cb,
			   &qcow2_if, &vbdev_qcow2_fn_table,
			   &qcow2_base->parts, spdk_qcow2_base_free, qcow2_base,
			   sizeof(struct qcow2_channel), NULL, NULL);
	if (!qcow2_base->base) {
		free(qcow2_base);
		SPDK_ERRLOG("cannot construct qcow2_base");
		return NULL;
	}

	qcow2 = &qcow2_base->qcow2;
	qcow2->parse_phase = SPDK_QCOW2_PARSE_PHASE_QCOW_HEADER;
	qcow2->buf_size = spdk_max(SPDK_QCOW2_BUFFER_SIZE, bdev->blocklen);
	qcow2->buf = spdk_zmalloc(qcow2->buf_size, spdk_bdev_get_buf_align(bdev), NULL,
				  SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (!qcow2->buf) {
		SPDK_ERRLOG("Cannot alloc buf\n");
		spdk_bdev_part_base_free(qcow2_base->base);
		return NULL;
	}

	qcow2->sector_size = bdev->blocklen;
	qcow2->total_sectors = bdev->blockcnt;
	qcow2->lba_start = 0;
	qcow2->lba_end = qcow2->total_sectors - 1;

	return qcow2_base;
}

static int
vbdev_qcow2_destruct(void *ctx)
{
	struct qcow2_disk *qcow2_disk = ctx;

	return spdk_bdev_part_free(&qcow2_disk->part);
}

static void
_vbdev_qcow2_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io);

static void
vbdev_qcow2_resubmit_request(void *arg)
{
	struct qcow2_io *io = (struct qcow2_io *)arg;

	_vbdev_qcow2_submit_request(io->ch, io->bdev_io);
}

static void
vbdev_qcow2_queue_io(struct qcow2_io *io)
{
	int rc;

	io->bdev_io_wait.bdev = io->bdev_io->bdev;
	io->bdev_io_wait.cb_fn = vbdev_qcow2_resubmit_request;
	io->bdev_io_wait.cb_arg = io;

	rc = spdk_bdev_queue_io_wait(io->bdev_io->bdev,
				     io->ch, &io->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("Queue io failed in vbdev_qcow2_queue_io, rc=%d.\n", rc);
		spdk_bdev_io_complete(io->bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
vbdev_qcow2_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	_vbdev_qcow2_submit_request(ch, bdev_io);
}

static void
_vbdev_qcow2_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct qcow2_channel *ch = spdk_io_channel_get_ctx(_ch);
	struct qcow2_io *io = (struct qcow2_io *)bdev_io->driver_ctx;
	int rc;

	rc = spdk_bdev_part_submit_request(&ch->part_ch, bdev_io);
	if (rc) {
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(SPDK_LOG_VBDEV_GPT, "qcow2: no memory, queue io\n");
			io->ch = _ch;
			io->bdev_io = bdev_io;
			vbdev_qcow2_queue_io(io);
		} else {
			SPDK_ERRLOG("qcow2: error on bdev_io submission, rc=%d.\n", rc);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

static void
vbdev_qcow2_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, vbdev_qcow2_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	default:
		_vbdev_qcow2_submit_request(_ch, bdev_io);
		break;
	}
}

int
vbdev_qcow2_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct qcow2_disk *qcow2_disk = SPDK_CONTAINEROF(ctx, struct qcow2_disk, part);
	struct spdk_bdev_part_base *base_bdev = spdk_bdev_part_get_base(&qcow2_disk->part);
	struct spdk_bdev *part_base_bdev = spdk_bdev_part_base_get_bdev(base_bdev);

	spdk_json_write_named_object_begin(w, "qcow2");
	spdk_json_write_named_string(w, "base_bdev", spdk_bdev_get_name(part_base_bdev));
	spdk_json_write_object_end(w);

	return 0;
}

static int
vbdev_qcow2_create_bdev(struct qcow2_base *qcow2_base)
{
	struct qcow2_disk *q_disk;
	char *name;
	struct spdk_bdev *base_bdev;
	int rc;

	q_disk = calloc(1, sizeof(*q_disk));
	if (!q_disk) {
		SPDK_ERRLOG("Memory allocation failure\n");
		return -1;
	}

	/* index start at 1 instead of 0 to match the existing style */
	base_bdev = spdk_bdev_part_base_get_bdev(qcow2_base->base);
	name = spdk_sprintf_alloc("%s-qcow2", spdk_bdev_get_name(base_bdev));
	if (!name) {
		SPDK_ERRLOG("name allocation failure\n");
		free(q_disk);
		return -1;
	}

	/* We can re-use this function, but we always parse offset: 0 since the offset is useless */
	rc = spdk_bdev_part_construct(&q_disk->part, qcow2_base->base, name,
				      0, 0 /* should be revised */, "QCOW2 Disk");
	free(name);
	if (rc) {
		SPDK_ERRLOG("could not construct qcow2 bdev\n");
		/* spdk_bdev_part_construct will free name on failure */
		free(q_disk);
		return -1;
	}

	return 0;
}

static void
spdk_qcow2_bdev_complete(struct spdk_bdev_io *bdev_io, bool status, void *arg)
{
	struct qcow2_base *qcow2_base = (struct qcow2_base *)arg;
	struct spdk_bdev *bdev = spdk_bdev_part_base_get_bdev(qcow2_base->base);
	int rc;

	spdk_bdev_free_io(bdev_io);

	if (status != SPDK_BDEV_IO_STATUS_SUCCESS) {
		SPDK_ERRLOG("Gpt: bdev=%s io error status=%d\n",
			    spdk_bdev_get_name(bdev), status);
		goto end;
	}

	rc = spdk_qcow2_parse_header(&qcow2_base->qcow2);
	if (rc) {
		SPDK_DEBUGLOG(SPDK_LOG_VBDEV_QCOW2, "Failed to parse header\n");
		goto end;
	}

	rc = spdk_qcow2_parse_mapping_table(&qcow2_base->qcow2);
	if (rc) {
		SPDK_DEBUGLOG(SPDK_LOG_VBDEV_QCOW2, "Failed to get mapping table info of QCOW2\n");
		goto end;
	}

	rc = vbdev_qcow2_create_bdev(qcow2_base);
	if (rc) {
		SPDK_DEBUGLOG(SPDK_LOG_VBDEV_QCOW2, "Failed to create qcow2 on bdev=%s by qcow2 tale\n",
			      spdk_bdev_get_name(bdev));
	}

end:
	spdk_put_io_channel(qcow2_base->ch);
	qcow2_base->ch = NULL;
	/*
	 * Notify the generic bdev layer that the actions related to the original examine
	 *  callback are now completed.
	 */
	spdk_bdev_module_examine_done(&qcow2_if);

	/*
	 * vbdev_qcow2_create_bdev returns 0 upon success.
	 */
	if (!rc) {
		/* If no qcow2_disk instances were created, free the base context */
		spdk_bdev_part_base_free(qcow2_base->base);
	}
}

static int
vbdev_qcow2_read_qcow2(struct spdk_bdev *bdev)
{
	struct qcow2_base *qcow2_base;
	struct spdk_bdev_desc *base_desc;
	int rc;

	qcow2_base = spdk_qcow2_base_bdev_init(bdev);
	if (!qcow2_base) {
		SPDK_ERRLOG("Cannot allocated qcow2_base\n");
		return -1;
	}

	base_desc = spdk_bdev_part_base_get_desc(qcow2_base->base);
	qcow2_base->ch = spdk_bdev_get_io_channel(base_desc);
	if (qcow2_base->ch == NULL) {
		SPDK_ERRLOG("Failed to get an io_channel.\n");
		spdk_bdev_part_base_free(qcow2_base->base);
		return -1;
	}

	rc = spdk_bdev_read(base_desc, qcow2_base->ch, qcow2_base->qcow2.buf, 0,
			    qcow2_base->qcow2.buf_size, spdk_qcow2_bdev_complete, qcow2_base);
	if (rc < 0) {
		spdk_put_io_channel(qcow2_base->ch);
		spdk_bdev_part_base_free(qcow2_base->base);
		SPDK_ERRLOG("Failed to send bdev_io command\n");
		return -1;
	}

	return 0;
}

static int
vbdev_qcow2_init(void)
{
	struct spdk_conf_section *sp = spdk_conf_find_section(NULL, "Qcow2");

	if (sp && spdk_conf_section_get_boolval(sp, "Disable", false)) {
		/* Disable Gpt probe */
		g_qcow2_disabled = true;
	}

	return 0;
}

static int
vbdev_qcow2_get_ctx_size(void)
{
	return sizeof(struct qcow2_io);
}

static void
vbdev_qcow2_examine(struct spdk_bdev *bdev)
{
	int rc;

	if (g_qcow2_disabled) {
		spdk_bdev_module_examine_done(&qcow2_if);
		return;
	}

	if (spdk_bdev_get_block_size(bdev) % 512 != 0) {
		SPDK_ERRLOG("QCOW2 module does not support block size %" PRIu32 " for bdev %s\n",
			    spdk_bdev_get_block_size(bdev), spdk_bdev_get_name(bdev));
		spdk_bdev_module_examine_done(&qcow2_if);
		return;
	}

	rc = vbdev_qcow2_read_qcow2(bdev);
	if (rc) {
		spdk_bdev_module_examine_done(&qcow2_if);
		SPDK_ERRLOG("Failed to read info from bdev %s\n", spdk_bdev_get_name(bdev));
	}
}

SPDK_LOG_REGISTER_COMPONENT("vbdev_qcow2", SPDK_LOG_VBDEV_QCOW2)
