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
 * OCSSD vbdev mimics nvme bdev and verified on QEMU NVMe which Matias maintains.
 */

#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk/bdev_module.h"
#include "spdk_internal/log.h"
#include "vbdev_ocssd.h"

static int vbdev_ocssd_init(void);
static void vbdev_ocssd_examine(struct spdk_bdev *bdev);

static int
vbdev_ocssd_get_ctx_size(void)
{
	return sizeof(struct nvme_bdev_io);
}

static struct spdk_bdev_module ocssd_if = {
	.name = "ocssd",
	.module_init = vbdev_ocssd_init,
	.examine = vbdev_ocssd_examine,
	.get_ctx_size = vbdev_ocssd_get_ctx_size,

};
SPDK_BDEV_MODULE_REGISTER(&ocssd_if)

/* Base block device ocssd context */
struct ocssd_base {
	struct spdk_bdev_part_base	part_base;
	struct spdk_ocssd		ocssd;

	/* This channel is used for XXX. */
	struct spdk_io_channel		*ch;
};

/* Context for each ocssd virtual bdev */
struct ocssd_disk {
	struct spdk_bdev_part	part;
	uint32_t		index;
};

struct ocssd_channel {
	struct spdk_bdev_part_channel	part_ch;
};

static SPDK_BDEV_PART_TAILQ g_ocssd_disks = TAILQ_HEAD_INITIALIZER(g_ocssd_disks);

static bool g_ocssd_disabled;

static void
spdk_ocssd_base_free(struct spdk_bdev_part_base *base)
{
	struct ocssd_base *ocssd_base = SPDK_CONTAINEROF(base, struct ocssd_base, part_base);

	spdk_dma_free(ocssd_base->ocssd.buf);
	free(ocssd_base);
}

static void
spdk_ocssd_base_bdev_hotremove_cb(void *_base_bdev)
{
	spdk_bdev_part_base_hotremove(_base_bdev, &g_ocssd_disks);
}

static int vbdev_ocssd_destruct(void *ctx);
static void vbdev_ocssd_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io);
static int vbdev_ocssd_dump_info_json(void *ctx, struct spdk_json_write_ctx *w);
static void ocssd_admin_cb(void *arg, const struct spdk_nvme_cpl *cpl);

static struct spdk_bdev_fn_table vbdev_ocssd_fn_table = {
	.destruct		= vbdev_ocssd_destruct,
	.submit_request		= vbdev_ocssd_submit_request,
	.dump_info_json		= vbdev_ocssd_dump_info_json,
};

struct cb_arg {
	bool done;
	struct spdk_nvme_cpl cpl;
} cb;

static void
ocssd_admin_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct cb_arg *cb = arg;

	cb->cpl = *cpl;
	cb->done = true;
}

static struct ocssd_base *
spdk_ocssd_base_bdev_init(struct spdk_bdev *bdev)
{
	struct ocssd_base *ocssd_base;
	int rc;

	struct nvme_bdev *nbdev = bdev->ctxt;
	const struct spdk_nvme_ns_data *nsdata;
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_cmd cmd;

	struct spdk_ocssd *ocssd;
	struct spdk_ocssd_geometry_data *geo;
	struct spdk_ocssd_chunk_information *tbl;
	size_t tbl_sz;

	if (strcmp(bdev->product_name, "NVMe disk")) {
		SPDK_ERRLOG("product name is not NVMe disk but %s\n", bdev->product_name);
		return NULL;
	}

	nsdata = spdk_nvme_ns_get_data(nbdev->ns);
	if (nsdata->vendor_specific[0] != 0x01) {
		SPDK_ERRLOG("NVMe disk is not OCSSD\n");
		return NULL;
	}

	geo = spdk_dma_zmalloc(4096, 4096, NULL);
	if (geo == NULL) {
		SPDK_ERRLOG("cannot alloc geo\n");
		return NULL;
	}

	ctrlr = nbdev->nvme_ctrlr->ctrlr;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = SPDK_OCSSD_OPC_GEOMETRY;
	cmd.nsid = 1;

	cb.done = false;
	rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, geo, 4096, ocssd_admin_cb, &cb);
	if (rc) {
		SPDK_ERRLOG("cannot submit geometry\n");
		goto error_geo_free;
	}

	while (!cb.done) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	if (spdk_nvme_cpl_is_error(&cb.cpl)) {
		SPDK_ERRLOG("geometry failed\n");
		goto error_geo_free;
	}

	if (geo->mjr != 2 || geo->mnr != 0) {
		SPDK_ERRLOG("OCSSD version is not 2.0 but %d.%d\n", geo->mjr, geo->mnr);
		goto error_geo_free;
	}

	SPDK_NOTICELOG("geo %d.%d %d/%d/%d/%d/%d/%d %" PRIu64 "\n", geo->mjr, geo->mnr,
			geo->ws_min, geo->ws_opt,
			geo->clba, geo->num_chk, geo->num_pu, geo->num_grp,
			bdev->blockcnt);

	tbl_sz = geo->num_grp * geo->num_pu * geo->num_chk * sizeof(*tbl);
	tbl_sz = (tbl_sz + 4095) & ~4095;
	tbl = spdk_dma_zmalloc(tbl_sz, 4096, NULL);

	if (!tbl) {
		SPDK_ERRLOG("cannot alloc tbl\n");
		goto error_geo_free;
	}

	cb.done = false;
	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_OCSSD_LOG_CHUNK_INFO, 1, tbl, tbl_sz, 0,
			ocssd_admin_cb, &cb);
	if (rc) {
		SPDK_ERRLOG("cannot submit chk info\n");
		goto error_tbl_free;
	}

	while (!cb.done) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	if (spdk_nvme_cpl_is_error(&cb.cpl)) {
		SPDK_ERRLOG("chk info failed\n");
		goto error_tbl_free;
	}

#if 1
	for (uint32_t i = 0; i < geo->num_grp * geo->num_pu * geo->num_chk; i += geo->num_chk) {
		SPDK_NOTICELOG("chk=%d %x/%x/%d/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "\n", i,
				*((uint8_t *) &tbl[i].cs), *((uint8_t *) &tbl[i].ct), tbl[i].wli,
				tbl[i].slba, tbl[i].cnlb, tbl[i].wp);
	}
#endif

	ocssd_base = calloc(1, sizeof(*ocssd_base));
	if (!ocssd_base) {
		SPDK_ERRLOG("Cannot alloc memory for ocssd_base pointer\n");
		goto error_tbl_free;
	}

	rc = spdk_bdev_part_base_construct(&ocssd_base->part_base, bdev,
					   spdk_ocssd_base_bdev_hotremove_cb,
					   &ocssd_if, &vbdev_ocssd_fn_table,
					   &g_ocssd_disks, spdk_ocssd_base_free,
					   sizeof(struct ocssd_channel), NULL, NULL);
	if (rc) {
		SPDK_ERRLOG("cannot construct ocssd_base");
		goto error_free;
	}

	ocssd = &ocssd_base->ocssd;
	ocssd->geo = geo;
	ocssd->tbl = tbl;
	ocssd->buf_size = SPDK_OCSSD_BUFFER_SIZE;
	ocssd->buf = spdk_dma_malloc(SPDK_OCSSD_BUFFER_SIZE, 4096, NULL);
	if (!ocssd->buf) {
		SPDK_ERRLOG("cannot alloc buf\n");
		goto error_free;
	}
	ocssd->sector_size = bdev->blocklen;
	ocssd->total_sectors = bdev->blockcnt;
	ocssd->nbdev = nbdev;
	ocssd->ctrlr = ctrlr;

	return ocssd_base;

error_free:
	free(ocssd_base);

error_tbl_free:
	spdk_dma_free(tbl);

error_geo_free:
	spdk_dma_free(geo);
	return NULL;
}

static int
vbdev_ocssd_destruct(void *ctx)
{
	struct ocssd_disk *ocssd_disk = ctx;

	return spdk_bdev_part_free(&ocssd_disk->part);
}

static void vbdev_ocssd_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

static void
vbdev_ocssd_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *part_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	spdk_bdev_io_complete(part_io, status);
	spdk_bdev_free_io(bdev_io);
}

//++bdev_nvme.c
static void ocssd_vreset_done(void *ref, const struct spdk_nvme_cpl *cpl);
static void ocssd_io_done(void *ref, const struct spdk_nvme_cpl *cpl);
static void bdev_nvme_queued_reset_sgl(void *ref, uint32_t sgl_offset);
static int bdev_nvme_queued_next_sge(void *ref, void **address, uint32_t *length);

static void
ocssd_vreset_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *nbio = (struct nvme_bdev_io *) ref;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(nbio);

	struct spdk_bdev_part *part = bdev_io->bdev->ctxt;
	struct spdk_ocssd *ocssd = (struct spdk_ocssd *) (part->base + 1);
	struct spdk_ocssd_geometry_data *geo = ocssd->geo;
	struct spdk_ocssd_chunk_information *tbl = ocssd->tbl;
	uint64_t chk = bdev_io->u.bdev.offset_blocks / geo->clba;


	if (spdk_nvme_cpl_is_error(cpl)) {
		tbl[chk].cs.offline = 1;
		SPDK_ERRLOG("vector reset error chk=%" PRIu64 "\n", chk);
	} else {
		tbl[chk].cs.open = 1;
		tbl[chk].wp = 0;
	}

	spdk_bdev_io_complete_nvme_status(bdev_io, cpl->status.sct, cpl->status.sc);
}

static void
ocssd_io_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *nbio = (struct nvme_bdev_io *) ref;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(nbio);
	uint64_t slba = bdev_io->u.bdev.offset_blocks;
	uint64_t nlb = bdev_io->u.bdev.num_blocks;

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_ERRLOG("io error slba=%" PRIu64 "\n", bdev_io->u.bdev.offset_blocks);
	}
	spdk_bdev_io_complete_nvme_status(bdev_io, cpl->status.sct, cpl->status.sc);

	if (bdev_io->type == SPDK_BDEV_IO_TYPE_READ) {
		uint64_t *p2l = nbio->md;
		for (uint64_t i = 0; i < nlb; i += 2) {
			if ((p2l[i] != slba + i) || (p2l[i + 1] != ~(slba + i))) {
				SPDK_ERRLOG("read %" PRIx64 ",%" PRIx64 " expect %" PRIx64 ",%" PRIx64 "\n",
						p2l[i], p2l[i+1], slba + i, ~(slba + i));
				break;
			}
		}
	}
	spdk_dma_free(nbio->md);
}

static void
bdev_nvme_queued_reset_sgl(void *ref, uint32_t sgl_offset)
{
	struct nvme_bdev_io *bio = ref;
	struct iovec *iov;

	bio->iov_offset = sgl_offset;
	for (bio->iovpos = 0; bio->iovpos < bio->iovcnt; bio->iovpos++) {
		iov = &bio->iovs[bio->iovpos];
		if (bio->iov_offset < iov->iov_len) {
			break;
		}

		bio->iov_offset -= iov->iov_len;
	}
}

static int
bdev_nvme_queued_next_sge(void *ref, void **address, uint32_t *length)
{
	struct nvme_bdev_io *bio = ref;
	struct iovec *iov;

	assert(bio->iovpos < bio->iovcnt);

	iov = &bio->iovs[bio->iovpos];

	*address = iov->iov_base;
	*length = iov->iov_len;
	if (bio->iov_offset) {
		assert(bio->iov_offset <= iov->iov_len);
		*address += bio->iov_offset;
		*length -= bio->iov_offset;
	}

	bio->iov_offset += *length;
	if (bio->iov_offset == iov->iov_len) {
		bio->iovpos++;
		bio->iov_offset = 0;
	}

	return 0;
}
//--bdev_nvme.c

// spdk_bdev_part_submit_request()
static void
vbdev_ocssd_submit_request(struct spdk_io_channel *_ch, struct spdk_bdev_io *bdev_io)
{
	struct ocssd_channel *lch = spdk_io_channel_get_ctx(_ch);
	struct spdk_bdev_part_channel *ch = &lch->part_ch;
	struct spdk_bdev_part *part = ch->part;
	struct spdk_io_channel *base_ch = ch->base_ch;

	struct spdk_ocssd *ocssd = (struct spdk_ocssd *) (part->base + 1);
	struct spdk_ocssd_geometry_data *geo = ocssd->geo;
	struct spdk_bdev_desc *base_desc = part->base->desc;

	// as like nvme ns bdev
	struct spdk_bdev_channel *bdev_ch = spdk_io_channel_get_ctx(base_ch);
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(bdev_ch->channel);
	struct nvme_bdev_io *nbio = (struct nvme_bdev_io *) bdev_io->driver_ctx;
	struct spdk_nvme_cmd cmd = {0};
	uint64_t slba = bdev_io->u.bdev.offset_blocks;
	uint32_t nlb = bdev_io->u.bdev.num_blocks;
	size_t md_len = ocssd->nbdev->ns->md_size * nlb;
	void *md = NULL;
	int rc = 0;

	if ((bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) ||
			(bdev_io->type == SPDK_BDEV_IO_TYPE_READ)) {
		md = spdk_dma_zmalloc(md_len, 4096, NULL);
		if (!md) {
			rc = -ENOMEM;
			goto error_no_mem;
		}

		if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
			uint64_t *p2l = md;
			for (uint64_t i = 0; i < nlb; i += 2) {
				p2l[i] = slba + i;
				p2l[i+1] = ~(slba + i);
			}
		}
	}

	nbio->iovs = bdev_io->u.bdev.iovs;
	nbio->iovcnt = bdev_io->u.bdev.iovcnt;
	nbio->iovpos = 0;
	nbio->iov_offset = 0;
	nbio->md = md;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_UNMAP:
		if ((slba % geo->clba != 0) || (nlb != geo->clba)) {
			rc = -EINVAL;
			break;
		}

		cmd.opc = SPDK_OCSSD_OPC_VECTOR_RESET;
		cmd.nsid = spdk_nvme_ns_get_id(ocssd->nbdev->ns);
		*((uint64_t *) &cmd.cdw10) = slba;
		rc = spdk_nvme_ctrlr_cmd_io_raw(ocssd->ctrlr, nvme_ch->qpair, &cmd, NULL, 0,
				ocssd_vreset_done, nbio);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		if ((slba % geo->ws_opt != 0) || (nlb != geo->ws_opt)) {
			rc = -EINVAL;
			break;
		}
		rc = spdk_nvme_ns_cmd_writev_with_md(ocssd->nbdev->ns, nvme_ch->qpair, slba, nlb,
				ocssd_io_done, nbio, 0, bdev_nvme_queued_reset_sgl,
				bdev_nvme_queued_next_sge, md);
		break;
	case SPDK_BDEV_IO_TYPE_READ:
		rc = spdk_nvme_ns_cmd_readv_with_md(ocssd->nbdev->ns, nvme_ch->qpair,
				slba, nlb, ocssd_io_done, nbio, 0,
				bdev_nvme_queued_reset_sgl, bdev_nvme_queued_next_sge, md);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		rc = spdk_bdev_reset(base_desc, base_ch,
				vbdev_ocssd_complete_io, bdev_io);
		break;
	default:
		SPDK_ERRLOG("ocssd: unknown I/O type %d\n", bdev_io->type);
		rc = -EINVAL;
		break;
	}

error_no_mem:
	if (rc != 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static int
vbdev_ocssd_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	return 0;
}

static int
vbdev_ocssd_create_bdev(struct ocssd_base *ocssd_base)
{
	struct ocssd_disk *d;
	struct spdk_ocssd *ocssd;
	char *name;
	struct spdk_bdev *base_bdev;
	int rc;

	ocssd = &ocssd_base->ocssd;

	d = calloc(1, sizeof(*d));
	if (!d) {
		SPDK_ERRLOG("Memory allocation failure\n");
		return -1;
	}

	base_bdev = ocssd_base->part_base.bdev;
	name = spdk_sprintf_alloc("%soc", spdk_bdev_get_name(base_bdev));
	if (!name) {
		SPDK_ERRLOG("name allocation failure\n");
		free(d);
		return -1;
	}

	rc = spdk_bdev_part_construct(&d->part, &ocssd_base->part_base, name,
				      0, ocssd->total_sectors, "OCSSD disk");
	if (rc) {
		SPDK_ERRLOG("could not construct bdev part\n");
		/* spdk_bdev_part_construct will free name on failure */
		free(d);
		return -1;
	}

	return 0;
}

static int
vbdev_ocssd_identify(struct spdk_bdev *bdev)
{
	struct ocssd_base *ocssd_base;
	int rc;

	ocssd_base = spdk_ocssd_base_bdev_init(bdev);
	if (!ocssd_base) {
		SPDK_ERRLOG("Cannot allocated ocssd_base\n");
		return -1;
	}

	rc = vbdev_ocssd_create_bdev(ocssd_base);
	if (rc < 0) {
		SPDK_DEBUGLOG(SPDK_LOG_VBDEV_OCSSD, "Failed to create dev=%s for ocssd\n",
			      spdk_bdev_get_name(bdev));
	}

	/*
	 * Notify the generic bdev layer that the actions related to the original examine
	 *  callback are now completed.
	 */
	spdk_bdev_module_examine_done(&ocssd_if);

	if (ocssd_base->part_base.ref == 0) {
		/* If no ocssd_disk instances were created, free the base context */
		spdk_bdev_part_base_free(&ocssd_base->part_base);
	} else {
		SPDK_NOTICELOG("ocssd bdev created\n");
	}

	return 0;
}

static int
vbdev_ocssd_init(void)
{
	struct spdk_conf_section *sp = spdk_conf_find_section(NULL, "ocssd");

	if (sp && spdk_conf_section_get_boolval(sp, "Disable", false)) {
		/* Disable ocssd probe */
		g_ocssd_disabled = true;
	}

	return 0;
}

static void
vbdev_ocssd_examine(struct spdk_bdev *bdev)
{
	int rc;

	if (g_ocssd_disabled) {
		spdk_bdev_module_examine_done(&ocssd_if);
		return;
	}

	rc = vbdev_ocssd_identify(bdev);
	if (rc) {
		spdk_bdev_module_examine_done(&ocssd_if);
		SPDK_ERRLOG("Failed to identify bdev %s\n", spdk_bdev_get_name(bdev));
	}
}

SPDK_LOG_REGISTER_COMPONENT("vbdev_ocssd", SPDK_LOG_VBDEV_OCSSD)
