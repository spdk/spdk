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

#include "spdk/stdinc.h"
#include "spdk/bdev_module.h"
#include "spdk/bdev_zone.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/nvme_ocssd.h"
#include "spdk/nvme_ocssd_spec.h"
#include "spdk_internal/log.h"
#include "spdk/nvme.h"
#include "common.h"
#include "bdev_ocssd.h"

struct bdev_ocssd_lba_offsets {
	uint32_t	grp;
	uint32_t	pu;
	uint32_t	chk;
	uint32_t	lbk;
};

struct bdev_ocssd_io {
	size_t		iov_pos;
	size_t		iov_off;
	uint64_t	lba[SPDK_NVME_OCSSD_MAX_LBAL_ENTRIES];
};

struct ocssd_bdev {
	struct nvme_bdev		nvme_bdev;
	struct spdk_ocssd_geometry_data	geometry;
	struct bdev_ocssd_lba_offsets	lba_offsets;
};

static struct spdk_bdev_nvme_opts g_opts = {
	.action_on_timeout = SPDK_BDEV_NVME_TIMEOUT_ACTION_NONE,
	.timeout_us = 0,
	.retry_count = 4,
	.nvme_adminq_poll_period_us = 10000ULL,
	.nvme_ioq_poll_period_us = 0,
	.io_queue_requests = 0,
};

void
spdk_bdev_ocssd_get_opts(struct spdk_bdev_nvme_opts *opts)
{
	*opts = g_opts;
}

static int
bdev_ocssd_library_init(void)
{
	return 0;
}

static void
bdev_ocssd_library_fini(void)
{
}

static int
bdev_ocssd_config_json(struct spdk_json_write_ctx *w)
{
	return 0;
}

static int
bdev_ocssd_get_ctx_size(void)
{
	return sizeof(struct bdev_ocssd_io);
}

static struct spdk_bdev_module ocssd_if = {
	.name = "ocssd",
	.module_init = bdev_ocssd_library_init,
	.module_fini = bdev_ocssd_library_fini,
	.config_json = bdev_ocssd_config_json,
	.get_ctx_size = bdev_ocssd_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(ocssd, &ocssd_if);

static void
bdev_ocssd_free_bdev(struct ocssd_bdev *ocssd_bdev)
{
	if (!ocssd_bdev) {
		return;
	}

	free(ocssd_bdev->nvme_bdev.disk.name);
	free(ocssd_bdev);
}

static int
bdev_ocssd_destruct(void *ctx)
{
	struct ocssd_bdev *ocssd_bdev = ctx;
	struct nvme_bdev *nvme_bdev = &ocssd_bdev->nvme_bdev;
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr = nvme_bdev->nvme_bdev_ctrlr;

	nvme_bdev_detach_bdev_from_ctrlr(nvme_bdev_ctrlr, nvme_bdev);
	bdev_ocssd_free_bdev(ocssd_bdev);

	return 0;
}

static uint64_t
bdev_ocssd_to_disk_lba(struct ocssd_bdev *ocssd_bdev, uint64_t lba)
{
	const struct spdk_ocssd_geometry_data *geo = &ocssd_bdev->geometry;
	const struct bdev_ocssd_lba_offsets *offsets = &ocssd_bdev->lba_offsets;
	uint64_t addr_shift, lbk, chk, pu, grp;

	/* To achieve best performance, we need to make sure that adjacent zones can be accessed
	 * in parallel.  We accomplish this by having the following addressing scheme:
	 *
	 * [            zone id              ][  zone offset  ] User's LBA
	 * [ chunk ][ group ][ parallel unit ][ logical block ] Open Channel's LBA
	 *
	 * which means that neighbouring zones are placed in a different group and parallel unit.
	 */
	lbk = lba % geo->clba;
	addr_shift = geo->clba;

	pu = (lba / addr_shift) % geo->num_pu;
	addr_shift *= geo->num_pu;

	grp = (lba / addr_shift) % geo->num_grp;
	addr_shift *= geo->num_grp;

	chk = (lba / addr_shift) % geo->num_chk;

	return (lbk << offsets->lbk) |
	       (chk << offsets->chk) |
	       (pu  << offsets->pu)  |
	       (grp << offsets->grp);
}

static void
bdev_ocssd_reset_sgl(void *cb_arg, uint32_t offset)
{
	struct spdk_bdev_io *bdev_io = cb_arg;
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;
	struct iovec *iov;

	ocdev_io->iov_pos = 0;
	ocdev_io->iov_off = 0;

	for (; ocdev_io->iov_pos < (size_t)bdev_io->u.bdev.iovcnt; ++ocdev_io->iov_pos) {
		iov = &bdev_io->u.bdev.iovs[ocdev_io->iov_pos];
		if (offset < iov->iov_len) {
			ocdev_io->iov_off = offset;
			return;
		}

		offset -= iov->iov_len;
	}

	assert(false && "Invalid offset length");
}

static int
bdev_ocssd_next_sge(void *cb_arg, void **address, uint32_t *length)
{
	struct spdk_bdev_io *bdev_io = cb_arg;
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;
	struct iovec *iov;

	assert(ocdev_io->iov_pos < (size_t)bdev_io->u.bdev.iovcnt);
	iov = &bdev_io->u.bdev.iovs[ocdev_io->iov_pos];

	*address = iov->iov_base;
	*length = iov->iov_len;

	if (ocdev_io->iov_off != 0) {
		assert(ocdev_io->iov_off < iov->iov_len);
		*address = (char *)*address + ocdev_io->iov_off;
		*length -= ocdev_io->iov_off;
	}

	assert(ocdev_io->iov_off + *length == iov->iov_len);
	ocdev_io->iov_off = 0;
	ocdev_io->iov_pos++;

	return 0;
}

static void
bdev_ocssd_read_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_bdev_io *bdev_io = ctx;

	spdk_bdev_io_complete_nvme_status(bdev_io, cpl->status.sct, cpl->status.sc);
}

static int
bdev_ocssd_read(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io)
{
	struct ocssd_bdev *ocssd_bdev = bdev_io->bdev->ctxt;
	struct nvme_bdev *nvme_bdev = &ocssd_bdev->nvme_bdev;
	struct nvme_io_channel *nvme_ioch = spdk_io_channel_get_ctx(ioch);
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;
	const size_t zone_size = nvme_bdev->disk.zone_size;
	uint64_t lba;

	if ((bdev_io->u.bdev.offset_blocks % zone_size) + bdev_io->u.bdev.num_blocks > zone_size) {
		SPDK_ERRLOG("Tried to cross zone boundary during read command\n");
		return -EINVAL;
	}

	ocdev_io->iov_pos = 0;
	ocdev_io->iov_off = 0;

	lba = bdev_ocssd_to_disk_lba(ocssd_bdev, bdev_io->u.bdev.offset_blocks);

	return spdk_nvme_ns_cmd_readv_with_md(nvme_bdev->ns, nvme_ioch->qpair, lba,
					      bdev_io->u.bdev.num_blocks, bdev_ocssd_read_cb,
					      bdev_io, 0, bdev_ocssd_reset_sgl,
					      bdev_ocssd_next_sge, bdev_io->u.bdev.md_buf, 0, 0);
}

static void
bdev_ocssd_write_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_bdev_io *bdev_io = ctx;

	spdk_bdev_io_complete_nvme_status(bdev_io, cpl->status.sct, cpl->status.sc);
}

static int
bdev_ocssd_write(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io)
{
	struct ocssd_bdev *ocssd_bdev = bdev_io->bdev->ctxt;
	struct nvme_bdev *nvme_bdev = &ocssd_bdev->nvme_bdev;
	struct nvme_io_channel *nvme_ioch = spdk_io_channel_get_ctx(ioch);
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;
	const size_t zone_size = nvme_bdev->disk.zone_size;
	uint64_t lba;

	if ((bdev_io->u.bdev.offset_blocks % zone_size) + bdev_io->u.bdev.num_blocks > zone_size) {
		SPDK_ERRLOG("Tried to cross zone boundary during write commnad\n");
		return -EINVAL;
	}

	ocdev_io->iov_pos = 0;
	ocdev_io->iov_off = 0;

	lba = bdev_ocssd_to_disk_lba(ocssd_bdev, bdev_io->u.bdev.offset_blocks);

	return spdk_nvme_ns_cmd_writev_with_md(nvme_bdev->ns, nvme_ioch->qpair, lba,
					       bdev_io->u.bdev.num_blocks, bdev_ocssd_write_cb,
					       bdev_io, 0, bdev_ocssd_reset_sgl,
					       bdev_ocssd_next_sge, bdev_io->u.bdev.md_buf, 0, 0);
}

static void
bdev_ocssd_io_get_buf_cb(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io, bool success)
{
	int rc;

	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		return;
	}

	rc = bdev_ocssd_read(ioch, bdev_io);
	if (spdk_likely(rc != 0)) {
		if (rc == -ENOMEM) {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		} else {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

static void
bdev_ocssd_reset_zone_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_bdev_io *bdev_io = ctx;

	spdk_bdev_io_complete_nvme_status(bdev_io, cpl->status.sct, cpl->status.sc);
}

static int
bdev_ocssd_reset_zone(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io,
		      uint64_t slba, size_t num_zones)
{
	struct ocssd_bdev *ocssd_bdev = bdev_io->bdev->ctxt;
	struct nvme_bdev *nvme_bdev = &ocssd_bdev->nvme_bdev;
	struct nvme_io_channel *nvme_ioch = spdk_io_channel_get_ctx(ioch);
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;
	size_t offset, zone_size = nvme_bdev->disk.zone_size;

	if (num_zones > SPDK_NVME_OCSSD_MAX_LBAL_ENTRIES) {
		SPDK_ERRLOG("Exceeded maximum number of zones per single reset: %d\n",
			    SPDK_NVME_OCSSD_MAX_LBAL_ENTRIES);
		return -EINVAL;
	}

	for (offset = 0; offset < num_zones; ++offset) {
		ocdev_io->lba[offset] = bdev_ocssd_to_disk_lba(ocssd_bdev,
					slba + offset * zone_size);
	}

	return spdk_nvme_ocssd_ns_cmd_vector_reset(nvme_bdev->ns, nvme_ioch->qpair, ocdev_io->lba,
			num_zones, NULL, bdev_ocssd_reset_zone_cb, bdev_io);
}

static int
bdev_ocssd_unmap(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io)
{
	struct nvme_bdev *bdev = bdev_io->bdev->ctxt;
	const size_t zone_size = bdev->disk.zone_size;
	uint64_t num_zones = bdev_io->u.bdev.num_blocks / zone_size;

	if (bdev_io->u.bdev.offset_blocks % zone_size != 0) {
		SPDK_ERRLOG("Unaligned zone address for unmap request: %"PRIu64"\n",
			    bdev_io->u.bdev.offset_blocks);
		return -EINVAL;
	}

	if (bdev_io->u.bdev.num_blocks % zone_size != 0) {
		SPDK_ERRLOG("Unaligned length for zone unmap request: %"PRIu64"\n",
			    bdev_io->u.bdev.num_blocks);
		return -EINVAL;
	}

	return bdev_ocssd_reset_zone(ioch, bdev_io, bdev_io->u.bdev.offset_blocks, num_zones);
}

static int
bdev_ocssd_zone_management(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->u.zdev.zone_action) {
	case SPDK_BDEV_ZONE_RESET:
		return bdev_ocssd_reset_zone(ioch, bdev_io, bdev_io->u.zdev.zone_id,
					     bdev_io->u.zdev.num_zones);
	default:
		return -EINVAL;
	}
}

static void
bdev_ocssd_submit_request(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io)
{
	int rc = 0;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_ocssd_io_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;

	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = bdev_ocssd_write(ioch, bdev_io);
		break;

	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = bdev_ocssd_unmap(ioch, bdev_io);
		break;

	case SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT:
		rc = bdev_ocssd_zone_management(ioch, bdev_io);
		break;

	default:
		rc = -EINVAL;
		break;
	}

	if (spdk_unlikely(rc != 0)) {
		if (rc == -ENOMEM) {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		} else {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

static bool
bdev_ocssd_io_type_supported(void *ctx, enum spdk_bdev_io_type type)
{
	switch (type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT:
		return true;

	default:
		return false;
	}
}

static struct spdk_io_channel *
bdev_ocssd_get_io_channel(void *ctx)
{
	struct ocssd_bdev *ocssd_bdev = ctx;

	return spdk_get_io_channel(ocssd_bdev->nvme_bdev.nvme_bdev_ctrlr->ctrlr);
}

static struct spdk_bdev_fn_table ocssdlib_fn_table = {
	.destruct		= bdev_ocssd_destruct,
	.submit_request		= bdev_ocssd_submit_request,
	.io_type_supported	= bdev_ocssd_io_type_supported,
	.get_io_channel		= bdev_ocssd_get_io_channel,
};

struct bdev_ocssd_create_ctx {
	struct ocssd_bdev		*ocssd_bdev;
	spdk_bdev_ocssd_create_cb	cb_fn;
	void				*cb_arg;
};

static void
bdev_ocssd_geometry_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct bdev_ocssd_create_ctx *create_ctx = ctx;
	struct ocssd_bdev *ocssd_bdev = create_ctx->ocssd_bdev;
	const struct spdk_ocssd_geometry_data *geometry = &ocssd_bdev->geometry;
	struct nvme_bdev *nvme_bdev = &ocssd_bdev->nvme_bdev;
	const char *bdev_name = NULL;
	int rc = 0;

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_ERRLOG("Failed to retrieve controller's geometry\n");
		bdev_ocssd_free_bdev(ocssd_bdev);
		rc = -EIO;
		goto out;
	}

	ocssd_bdev->lba_offsets.lbk = 0;
	ocssd_bdev->lba_offsets.chk = ocssd_bdev->lba_offsets.lbk +
				      geometry->lbaf.lbk_len;
	ocssd_bdev->lba_offsets.pu  = ocssd_bdev->lba_offsets.chk +
				      geometry->lbaf.chk_len;
	ocssd_bdev->lba_offsets.grp = ocssd_bdev->lba_offsets.pu +
				      geometry->lbaf.pu_len;

	nvme_bdev->disk.blockcnt = geometry->num_grp * geometry->num_pu *
				   geometry->num_chk * geometry->clba;
	nvme_bdev->disk.zone_size = geometry->clba;
	nvme_bdev->disk.max_open_zones = geometry->maxoc;
	nvme_bdev->disk.optimal_open_zones = geometry->num_grp * geometry->num_pu;
	nvme_bdev->disk.write_unit_size = geometry->ws_opt;
	nvme_bdev->active = true;

	if (geometry->maxocpu != 0 && geometry->maxocpu != geometry->maxoc) {
		SPDK_WARNLOG("Maximum open chunks per PU is not zero. Reducing the maximum "
			     "number of open zones: %"PRIu32" -> %"PRIu32"\n",
			     geometry->maxoc, geometry->maxocpu);
		nvme_bdev->disk.max_open_zones = geometry->maxocpu;
	}

	rc = spdk_bdev_register(&nvme_bdev->disk);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Failed to register bdev %s\n", nvme_bdev->disk.name);
		bdev_ocssd_free_bdev(ocssd_bdev);
	} else {
		nvme_bdev_attach_bdev_to_ctrlr(nvme_bdev->nvme_bdev_ctrlr, nvme_bdev);
		bdev_name = nvme_bdev->disk.name;
	}
out:
	create_ctx->cb_fn(bdev_name, rc, create_ctx->cb_arg);
	free(create_ctx);
}

int
spdk_bdev_ocssd_create_bdev(const char *ctrlr_name, const char *bdev_name, uint32_t nsid,
			    spdk_bdev_ocssd_create_cb cb_fn, void *cb_arg)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;
	struct nvme_bdev *nvme_bdev = NULL;
	struct ocssd_bdev *ocssd_bdev = NULL;
	struct bdev_ocssd_create_ctx *create_ctx = NULL;
	struct spdk_nvme_ns *ns;
	int rc = 0;

	nvme_bdev_ctrlr = nvme_bdev_ctrlr_get_by_name(ctrlr_name);
	if (!nvme_bdev_ctrlr) {
		SPDK_ERRLOG("Unable to find controller %s\n", ctrlr_name);
		return -ENODEV;
	}

	if (!spdk_nvme_ctrlr_is_ocssd_supported(nvme_bdev_ctrlr->ctrlr)) {
		SPDK_ERRLOG("Specified controller doesn't support Open Channel\n");
		return -EINVAL;
	}

	ns = spdk_nvme_ctrlr_get_ns(nvme_bdev_ctrlr->ctrlr, nsid);
	if (!ns) {
		SPDK_ERRLOG("Unable to retrieve namespace %"PRIu32"\n", nsid);
		return -ENODEV;
	}

	if (spdk_bdev_get_by_name(bdev_name) != NULL) {
		SPDK_ERRLOG("Device with provided name (%s) already exists\n", bdev_name);
		return -EEXIST;
	}

	/* Only allow one bdev per namespace for now */
	TAILQ_FOREACH(nvme_bdev, &nvme_bdev_ctrlr->bdevs, tailq) {
		if (nvme_bdev->ns == ns) {
			SPDK_ERRLOG("Namespace %"PRIu32" was already claimed by bdev %s\n",
				    nsid, nvme_bdev->disk.name);
			return -EEXIST;
		}
	}

	ocssd_bdev = calloc(1, sizeof(*ocssd_bdev));
	if (!ocssd_bdev) {
		rc = -ENOMEM;
		goto error;
	}

	create_ctx = calloc(1, sizeof(*create_ctx));
	if (!create_ctx) {
		rc = -ENOMEM;
		goto error;
	}

	create_ctx->ocssd_bdev = ocssd_bdev;
	create_ctx->cb_fn = cb_fn;
	create_ctx->cb_arg = cb_arg;

	nvme_bdev = &ocssd_bdev->nvme_bdev;
	nvme_bdev->ns = ns;
	nvme_bdev->nvme_bdev_ctrlr = nvme_bdev_ctrlr;

	nvme_bdev->disk.name = strdup(bdev_name);
	if (!nvme_bdev->disk.name) {
		rc = -ENOMEM;
		goto error;
	}

	nvme_bdev->disk.product_name = "Open Channel SSD";
	nvme_bdev->disk.ctxt = ocssd_bdev;
	nvme_bdev->disk.fn_table = &ocssdlib_fn_table;
	nvme_bdev->disk.module = &ocssd_if;
	nvme_bdev->disk.blocklen = spdk_nvme_ns_get_extended_sector_size(nvme_bdev->ns);
	nvme_bdev->disk.zoned = true;

	rc = spdk_nvme_ocssd_ctrlr_cmd_geometry(nvme_bdev_ctrlr->ctrlr, nsid, &ocssd_bdev->geometry,
						sizeof(ocssd_bdev->geometry),
						bdev_ocssd_geometry_cb, create_ctx);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Failed to retrieve OC geometry: %s\n", spdk_strerror(-rc));
		goto error;
	}

	return 0;
error:
	bdev_ocssd_free_bdev(ocssd_bdev);
	free(create_ctx);

	return rc;
}

struct bdev_ocssd_delete_ctx {
	spdk_bdev_ocssd_delete_cb	cb_fn;
	void				*cb_arg;
};

static void
bdev_ocssd_delete_cb(void *cb_arg, int status)
{
	struct bdev_ocssd_delete_ctx *delete_ctx = cb_arg;

	delete_ctx->cb_fn(status, delete_ctx->cb_arg);
	free(delete_ctx);
}

int
spdk_bdev_ocssd_delete_bdev(const char *bdev_name, spdk_bdev_ocssd_delete_cb cb_fn, void *cb_arg)
{
	struct spdk_bdev *bdev;
	struct bdev_ocssd_delete_ctx *delete_ctx;

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (!bdev) {
		SPDK_ERRLOG("Unable to find bdev %s\n", bdev_name);
		return -ENODEV;
	}

	if (bdev->module != &ocssd_if) {
		SPDK_ERRLOG("Specified bdev %s is not an OCSSD bdev\n", bdev_name);
		return -EINVAL;
	}

	delete_ctx = calloc(1, sizeof(*delete_ctx));
	if (!delete_ctx) {
		SPDK_ERRLOG("Unable to allocate deletion context\n");
		return -ENOMEM;
	}

	delete_ctx->cb_fn = cb_fn;
	delete_ctx->cb_arg = cb_arg;

	spdk_bdev_unregister(bdev, bdev_ocssd_delete_cb, delete_ctx);

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT("bdev_ocssd", SPDK_LOG_BDEV_OCSSD)
