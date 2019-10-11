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
	union {
		struct {
			size_t		iov_pos;
			size_t		iov_off;
			uint64_t	lba[SPDK_NVME_OCSSD_MAX_LBAL_ENTRIES];
		};
		struct {
			size_t						chunk_offset;
			struct spdk_ocssd_chunk_information_entry	chunk_info;
		};
	};
};

struct ocssd_bdev {
	struct nvme_bdev		nvme_bdev;
	struct bdev_ocssd_ns		*ns;
};

struct bdev_ocssd_ns {
	struct nvme_bdev_ctrlr		*nvme_bdev_ctrlr;
	struct spdk_ocssd_geometry_data	geometry;
	struct bdev_ocssd_lba_offsets	lba_offsets;
	uint32_t			nsid;
	bool				valid;
};

struct ocssd_bdev_ctrlr {
	struct bdev_ocssd_ns *ns;
};

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

static void
bdev_ocssd_translate_lba(struct ocssd_bdev *ocssd_bdev, uint64_t lba, uint64_t *grp,
			 uint64_t *pu, uint64_t *chk, uint64_t *lbk)
{
	const struct spdk_ocssd_geometry_data *geo = &ocssd_bdev->ns->geometry;
	uint64_t addr_shift;

	/* To achieve best performance, we need to make sure that adjacent zones can be accessed
	 * in parallel.  We accomplish this by having the following addressing scheme:
	 *
	 * [            zone id              ][  zone offset  ] User's LBA
	 * [ chunk ][ group ][ parallel unit ][ logical block ] Open Channel's LBA
	 *
	 * which means that neighbouring zones are placed in a different group and parallel unit.
	 */
	*lbk = lba % geo->clba;
	addr_shift = geo->clba;

	*pu = (lba / addr_shift) % geo->num_pu;
	addr_shift *= geo->num_pu;

	*grp = (lba / addr_shift) % geo->num_grp;
	addr_shift *= geo->num_grp;

	*chk = (lba / addr_shift) % geo->num_chk;
}

static uint64_t
bdev_ocssd_from_disk_lba(struct ocssd_bdev *ocssd_bdev, uint64_t lba)
{
	const struct bdev_ocssd_lba_offsets *offsets = &ocssd_bdev->ns->lba_offsets;
	const struct spdk_ocssd_geometry_data *geometry = &ocssd_bdev->ns->geometry;
	uint64_t lbk, chk, pu, grp;

	lbk = (lba >> offsets->lbk) & ((1 << geometry->lbaf.lbk_len) - 1);
	chk = (lba >> offsets->chk) & ((1 << geometry->lbaf.chk_len) - 1);
	pu  = (lba >> offsets->pu)  & ((1 << geometry->lbaf.pu_len)  - 1);
	grp = (lba >> offsets->grp) & ((1 << geometry->lbaf.grp_len) - 1);

	return lbk + pu * geometry->clba + grp * geometry->num_pu * geometry->clba +
	       chk * geometry->num_pu * geometry->num_grp * geometry->clba;
}

static uint64_t
bdev_ocssd_to_disk_lba(struct ocssd_bdev *ocssd_bdev, uint64_t lba)
{
	const struct bdev_ocssd_lba_offsets *offsets = &ocssd_bdev->ns->lba_offsets;
	uint64_t lbk, chk, pu, grp;

	bdev_ocssd_translate_lba(ocssd_bdev, lba, &grp, &pu, &chk, &lbk);

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

static int _bdev_ocssd_get_zone_info(struct spdk_bdev_io *bdev_io);

static void
bdev_ocssd_fill_zone_info(struct ocssd_bdev *ocssd_bdev, struct spdk_bdev_zone_info *zone_info,
			  const struct spdk_ocssd_chunk_information_entry *chunk_info)
{
	struct nvme_bdev *nvme_bdev = &ocssd_bdev->nvme_bdev;

	zone_info->zone_id = bdev_ocssd_from_disk_lba(ocssd_bdev, chunk_info->slba);
	zone_info->write_pointer = zone_info->zone_id;

	if (chunk_info->cs.free) {
		zone_info->state = SPDK_BDEV_ZONE_STATE_EMPTY;
	} else if (chunk_info->cs.closed) {
		zone_info->state = SPDK_BDEV_ZONE_STATE_FULL;
	} else if (chunk_info->cs.open) {
		zone_info->state = SPDK_BDEV_ZONE_STATE_OPEN;
		zone_info->write_pointer += chunk_info->wp % nvme_bdev->disk.zone_size;
	} else if (chunk_info->cs.offline) {
		zone_info->state = SPDK_BDEV_ZONE_STATE_OFFLINE;
	} else {
		SPDK_ERRLOG("Unknown chunk state, assuming offline\n");
		zone_info->state = SPDK_BDEV_ZONE_STATE_OFFLINE;
	}

	if (chunk_info->ct.size_deviate) {
		zone_info->capacity = chunk_info->cnlb;
	} else {
		zone_info->capacity = nvme_bdev->disk.zone_size;
	}
}

static void
bdev_ocssd_zone_info_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_bdev_io *bdev_io = ctx;
	struct ocssd_bdev *ocssd_bdev = bdev_io->bdev->ctxt;
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;
	struct spdk_ocssd_chunk_information_entry *chunk_info = &ocdev_io->chunk_info;
	struct spdk_bdev_zone_info *zone_info;
	int rc;

	if (spdk_unlikely(spdk_nvme_cpl_is_error(cpl))) {
		spdk_bdev_io_complete_nvme_status(bdev_io, cpl->status.sct, cpl->status.sc);
		return;
	}

	zone_info = ((struct spdk_bdev_zone_info *)bdev_io->u.zone_mgmt.buf) + ocdev_io->chunk_offset;
	bdev_ocssd_fill_zone_info(ocssd_bdev, zone_info, chunk_info);

	if (++ocdev_io->chunk_offset == bdev_io->u.zone_mgmt.num_zones) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		rc = _bdev_ocssd_get_zone_info(bdev_io);
		if (spdk_unlikely(rc != 0)) {
			if (rc == -ENOMEM) {
				spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
			} else {
				spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
			}
		}
	}
}

static int
_bdev_ocssd_get_zone_info(struct spdk_bdev_io *bdev_io)
{
	struct ocssd_bdev *ocssd_bdev = bdev_io->bdev->ctxt;
	struct nvme_bdev *nvme_bdev = &ocssd_bdev->nvme_bdev;
	const struct spdk_ocssd_geometry_data *geo = &ocssd_bdev->ns->geometry;
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;
	uint64_t lba, grp, pu, chk, lbk, offset;

	lba = bdev_io->u.zone_mgmt.zone_id + ocdev_io->chunk_offset * nvme_bdev->disk.zone_size;
	bdev_ocssd_translate_lba(ocssd_bdev, lba, &grp, &pu, &chk, &lbk);
	offset = grp * geo->num_pu * geo->num_chk + pu * geo->num_chk + chk;

	return spdk_nvme_ctrlr_cmd_get_log_page(nvme_bdev->nvme_bdev_ctrlr->ctrlr,
						SPDK_OCSSD_LOG_CHUNK_INFO,
						spdk_nvme_ns_get_id(nvme_bdev->ns),
						&ocdev_io->chunk_info, sizeof(ocdev_io->chunk_info),
						offset * sizeof(ocdev_io->chunk_info),
						bdev_ocssd_zone_info_cb, (void *)bdev_io);
}

static int
bdev_ocssd_get_zone_info(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io)
{
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;

	if (bdev_io->u.zone_mgmt.num_zones < 1) {
		return -EINVAL;
	}

	if (bdev_io->u.zone_mgmt.zone_id % bdev_io->bdev->zone_size != 0) {
		return -EINVAL;
	}

	ocdev_io->chunk_offset = 0;

	return _bdev_ocssd_get_zone_info(bdev_io);
}

static int
bdev_ocssd_zone_management(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->u.zone_mgmt.zone_action) {
	case SPDK_BDEV_ZONE_RESET:
		return bdev_ocssd_reset_zone(ioch, bdev_io, bdev_io->u.zone_mgmt.zone_id,
					     bdev_io->u.zone_mgmt.num_zones);
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

	case SPDK_BDEV_IO_TYPE_GET_ZONE_INFO:
		rc = bdev_ocssd_get_zone_info(ioch, bdev_io);
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

	return spdk_get_io_channel(ocssd_bdev->nvme_bdev.nvme_bdev_ctrlr);
}

static struct spdk_bdev_fn_table ocssdlib_fn_table = {
	.destruct		= bdev_ocssd_destruct,
	.submit_request		= bdev_ocssd_submit_request,
	.io_type_supported	= bdev_ocssd_io_type_supported,
	.get_io_channel		= bdev_ocssd_get_io_channel,
};

int
spdk_bdev_ocssd_create_bdev(const char *ctrlr_name, const char *bdev_name, uint32_t nsid,
			    spdk_bdev_ocssd_create_cb cb_fn, void *cb_arg)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;
	struct nvme_bdev *nvme_bdev = NULL;
	struct ocssd_bdev *ocssd_bdev = NULL;
	struct spdk_nvme_ns *ns;
	struct bdev_ocssd_ns *ocssd_ns;
	struct spdk_ocssd_geometry_data *geometry;
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

	if (!spdk_nvme_ns_is_active(ns)) {
		SPDK_ERRLOG("Namespace %"PRIu32" is inactive\n", nsid);
		return -EACCES;
	}

	assert(nsid <= nvme_bdev_ctrlr->num_ns);
	ocssd_ns = &nvme_bdev_ctrlr->ocssd_ctrlr->ns[nsid - 1];
	if (!ocssd_ns->valid) {
		SPDK_ERRLOG("Namespace %"PRIu32" is not initialized\n", nsid);
		return -EINVAL;
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

	nvme_bdev = &ocssd_bdev->nvme_bdev;
	nvme_bdev->ns = ns;
	nvme_bdev->nvme_bdev_ctrlr = nvme_bdev_ctrlr;
	ocssd_bdev->ns = ocssd_ns;
	geometry = &ocssd_ns->geometry;

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
	nvme_bdev->disk.blockcnt = geometry->num_grp * geometry->num_pu *
				   geometry->num_chk * geometry->clba;
	nvme_bdev->disk.zone_size = geometry->clba;
	nvme_bdev->disk.max_open_zones = geometry->maxoc;
	nvme_bdev->disk.optimal_open_zones = geometry->num_grp * geometry->num_pu;
	nvme_bdev->disk.write_unit_size = geometry->ws_opt;

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

	cb_fn(bdev_name, 0, cb_arg);
	return 0;
error:
	bdev_ocssd_free_bdev(ocssd_bdev);
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

static void
bdev_ocssd_probe_done(struct nvme_async_probe_ctx *ctx, int rc)
{
	ctx->create_cb_fn(ctx->create_cb_ctx, 0, rc);
}

static void bdev_ocssd_get_geometry(struct nvme_async_probe_ctx *ctx);

static void
bdev_ocssd_geometry_cb(void *_ctx, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_async_probe_ctx *ctx = _ctx;
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;
	struct bdev_ocssd_ns *ocssd_ns;

	nvme_bdev_ctrlr = nvme_bdev_ctrlr_get(&ctx->trid);
	assert(nvme_bdev_ctrlr != NULL);

	ocssd_ns = &nvme_bdev_ctrlr->ocssd_ctrlr->ns[ctx->count];
	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_ERRLOG("Failed to retrieve geometry for namespace %"PRIu32"\n", ocssd_ns->nsid);
		ocssd_ns->valid = false;
	} else {
		ocssd_ns->valid = true;
		ocssd_ns->lba_offsets.lbk = 0;
		ocssd_ns->lba_offsets.chk = ocssd_ns->lba_offsets.lbk +
					    ocssd_ns->geometry.lbaf.lbk_len;
		ocssd_ns->lba_offsets.pu  = ocssd_ns->lba_offsets.chk +
					    ocssd_ns->geometry.lbaf.chk_len;
		ocssd_ns->lba_offsets.grp = ocssd_ns->lba_offsets.pu +
					    ocssd_ns->geometry.lbaf.pu_len;
	}

	if (++ctx->count < nvme_bdev_ctrlr->num_ns) {
		bdev_ocssd_get_geometry(ctx);
	} else {
		bdev_ocssd_probe_done(ctx, 0);
	}
}

static void
bdev_ocssd_get_geometry(struct nvme_async_probe_ctx *ctx)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;
	struct bdev_ocssd_ns *ocssd_ns;
	int rc;

	nvme_bdev_ctrlr = nvme_bdev_ctrlr_get(&ctx->trid);
	if (!nvme_bdev_ctrlr) {
		SPDK_ERRLOG("Failed to find NVMe controller: %s\n", ctx->trid.traddr);
		bdev_ocssd_probe_done(ctx, -ENODEV);
		return;
	}

	if (ctx->count == nvme_bdev_ctrlr->num_ns) {
		bdev_ocssd_probe_done(ctx, 0);
		return;
	}

	ocssd_ns = &nvme_bdev_ctrlr->ocssd_ctrlr->ns[ctx->count];
	rc = spdk_nvme_ocssd_ctrlr_cmd_geometry(nvme_bdev_ctrlr->ctrlr, ctx->count + 1,
						&ocssd_ns->geometry,
						sizeof(ocssd_ns->geometry),
						bdev_ocssd_geometry_cb, ctx);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Failed to retrieve OC geometry: %s\n", spdk_strerror(-rc));
		if (++ctx->count == nvme_bdev_ctrlr->num_ns) {
			bdev_ocssd_probe_done(ctx, 0);
		}
	}
}

void
spdk_bdev_ocssd_create_bdevs(struct nvme_async_probe_ctx *ctx, spdk_bdev_create_nvme_fn cb_fn,
			     void *cb_arg)
{
	ctx->create_cb_fn = cb_fn;
	ctx->create_cb_ctx = cb_arg;
	ctx->count = 0;

	bdev_ocssd_get_geometry(ctx);
}

int
spdk_bdev_ocssd_init_ctrlr(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr)
{
	struct ocssd_bdev_ctrlr *ocssd_ctrlr;
	uint32_t nsid;

	assert(spdk_nvme_ctrlr_is_ocssd_supported(nvme_bdev_ctrlr->ctrlr));

	ocssd_ctrlr = calloc(1, sizeof(*ocssd_ctrlr));
	if (!ocssd_ctrlr) {
		return -ENOMEM;
	}

	ocssd_ctrlr->ns = calloc(nvme_bdev_ctrlr->num_ns, sizeof(*ocssd_ctrlr->ns));
	if (!ocssd_ctrlr->ns) {
		free(ocssd_ctrlr);
		return -ENOMEM;
	}

	nvme_bdev_ctrlr->ocssd_ctrlr = ocssd_ctrlr;
	for (nsid = 0; nsid < nvme_bdev_ctrlr->num_ns; ++nsid) {
		ocssd_ctrlr->ns[nsid].nvme_bdev_ctrlr = nvme_bdev_ctrlr;
		ocssd_ctrlr->ns[nsid].nsid = nsid + 1;
		ocssd_ctrlr->ns[nsid].valid = false;
	}

	return 0;
}

void
spdk_bdev_ocssd_fini_ctrlr(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr)
{
	free(nvme_bdev_ctrlr->ocssd_ctrlr->ns);
	free(nvme_bdev_ctrlr->ocssd_ctrlr);
	nvme_bdev_ctrlr->ocssd_ctrlr = NULL;
}

SPDK_LOG_REGISTER_COMPONENT("bdev_ocssd", SPDK_LOG_BDEV_OCSSD)
