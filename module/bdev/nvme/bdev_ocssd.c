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
#include "spdk/util.h"
#include "spdk/nvme_ocssd.h"
#include "spdk/nvme_ocssd_spec.h"
#include "spdk_internal/log.h"
#include "spdk/nvme.h"
#include "common.h"
#include "bdev_ocssd.h"

struct bdev_ocssd_lba_offsets {
	uint32_t grp;
	uint32_t pu;
	uint32_t chk;
	uint32_t lbk;
};

struct bdev_ocssd_zone {
	uint64_t	slba;
	uint64_t	write_pointer;
	uint64_t	capacity;
	bool		busy;
};

struct bdev_ocssd_io {
	union {
		struct {
			struct bdev_ocssd_zone	*zone;
			size_t			iov_pos;
			size_t			iov_off;
			uint64_t		lba[SPDK_NVME_OCSSD_MAX_LBAL_ENTRIES];
		} io;
		struct {
			size_t						chunk_offset;
			struct spdk_ocssd_chunk_information_entry	chunk_info;
		} zone_info;
	};
};

struct ocssd_io_channel {
	struct spdk_poller		*pending_poller;
	TAILQ_HEAD(, spdk_bdev_io)	pending_requests;
};

struct ocssd_bdev {
	struct nvme_bdev	nvme_bdev;
	struct bdev_ocssd_zone	*zones;
};

struct bdev_ocssd_ns {
	struct spdk_ocssd_geometry_data	geometry;
	struct bdev_ocssd_lba_offsets	lba_offsets;
};

static struct bdev_ocssd_ns *
bdev_ocssd_get_ns_from_nvme(struct nvme_bdev_ns *nvme_ns)
{
	return nvme_ns->type_ctx;
}

static struct bdev_ocssd_ns *
bdev_ocssd_get_ns_from_bdev(struct ocssd_bdev *ocssd_bdev)
{
	return bdev_ocssd_get_ns_from_nvme(ocssd_bdev->nvme_bdev.nvme_ns);
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

void
bdev_ocssd_namespace_config_json(struct spdk_json_write_ctx *w, struct nvme_bdev_ns *ns)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;
	struct nvme_bdev *nvme_bdev;

	TAILQ_FOREACH(nvme_bdev, &ns->bdevs, tailq) {
		nvme_bdev_ctrlr = nvme_bdev->nvme_bdev_ctrlr;

		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "bdev_ocssd_create");

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "ctrlr_name", nvme_bdev_ctrlr->name);
		spdk_json_write_named_string(w, "bdev_name", nvme_bdev->disk.name);
		spdk_json_write_named_uint32(w, "nsid", nvme_bdev->nvme_ns->id);
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}
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

static struct bdev_ocssd_zone *
bdev_ocssd_get_zone_by_lba(struct ocssd_bdev *ocssd_bdev, uint64_t lba)
{
	struct nvme_bdev *nvme_bdev = &ocssd_bdev->nvme_bdev;
	size_t zone_size = nvme_bdev->disk.zone_size;

	if (lba >= nvme_bdev->disk.blockcnt) {
		return NULL;
	}

	return &ocssd_bdev->zones[lba / zone_size];
}

static struct bdev_ocssd_zone *
bdev_ocssd_get_zone_by_slba(struct ocssd_bdev *ocssd_bdev, uint64_t slba)
{
	struct nvme_bdev *nvme_bdev = &ocssd_bdev->nvme_bdev;

	if (slba % nvme_bdev->disk.zone_size != 0) {
		return NULL;
	}

	return bdev_ocssd_get_zone_by_lba(ocssd_bdev, slba);
}

static void
bdev_ocssd_free_bdev(struct ocssd_bdev *ocssd_bdev)
{
	if (!ocssd_bdev) {
		return;
	}

	free(ocssd_bdev->zones);
	free(ocssd_bdev->nvme_bdev.disk.name);
	free(ocssd_bdev);
}

static int
bdev_ocssd_destruct(void *ctx)
{
	struct ocssd_bdev *ocssd_bdev = ctx;
	struct nvme_bdev *nvme_bdev = &ocssd_bdev->nvme_bdev;

	nvme_bdev_detach_bdev_from_ns(nvme_bdev);
	bdev_ocssd_free_bdev(ocssd_bdev);

	return 0;
}

static void
bdev_ocssd_translate_lba(struct ocssd_bdev *ocssd_bdev, uint64_t lba, uint64_t *grp,
			 uint64_t *pu, uint64_t *chk, uint64_t *lbk)
{
	struct bdev_ocssd_ns *ocssd_ns = bdev_ocssd_get_ns_from_bdev(ocssd_bdev);
	const struct spdk_ocssd_geometry_data *geo = &ocssd_ns->geometry;
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
	struct bdev_ocssd_ns *ocssd_ns = bdev_ocssd_get_ns_from_bdev(ocssd_bdev);
	const struct spdk_ocssd_geometry_data *geometry = &ocssd_ns->geometry;
	const struct bdev_ocssd_lba_offsets *offsets = &ocssd_ns->lba_offsets;
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
	struct bdev_ocssd_ns *ocssd_ns = bdev_ocssd_get_ns_from_bdev(ocssd_bdev);
	const struct bdev_ocssd_lba_offsets *offsets = &ocssd_ns->lba_offsets;
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

	ocdev_io->io.iov_pos = 0;
	ocdev_io->io.iov_off = 0;

	for (; ocdev_io->io.iov_pos < (size_t)bdev_io->u.bdev.iovcnt; ++ocdev_io->io.iov_pos) {
		iov = &bdev_io->u.bdev.iovs[ocdev_io->io.iov_pos];
		if (offset < iov->iov_len) {
			ocdev_io->io.iov_off = offset;
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

	assert(ocdev_io->io.iov_pos < (size_t)bdev_io->u.bdev.iovcnt);
	iov = &bdev_io->u.bdev.iovs[ocdev_io->io.iov_pos];

	*address = iov->iov_base;
	*length = iov->iov_len;

	if (ocdev_io->io.iov_off != 0) {
		assert(ocdev_io->io.iov_off < iov->iov_len);
		*address = (char *)*address + ocdev_io->io.iov_off;
		*length -= ocdev_io->io.iov_off;
	}

	assert(ocdev_io->io.iov_off + *length == iov->iov_len);
	ocdev_io->io.iov_off = 0;
	ocdev_io->io.iov_pos++;

	return 0;
}

static void
bdev_ocssd_read_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_bdev_io *bdev_io = ctx;

	spdk_bdev_io_complete_nvme_status(bdev_io, 0, cpl->status.sct, cpl->status.sc);
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

	ocdev_io->io.iov_pos = 0;
	ocdev_io->io.iov_off = 0;

	lba = bdev_ocssd_to_disk_lba(ocssd_bdev, bdev_io->u.bdev.offset_blocks);

	return spdk_nvme_ns_cmd_readv_with_md(nvme_bdev->nvme_ns->ns, nvme_ioch->qpair, lba,
					      bdev_io->u.bdev.num_blocks, bdev_ocssd_read_cb,
					      bdev_io, 0, bdev_ocssd_reset_sgl,
					      bdev_ocssd_next_sge, bdev_io->u.bdev.md_buf, 0, 0);
}

static void
bdev_ocssd_write_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_bdev_io *bdev_io = ctx;
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;

	ocdev_io->io.zone->write_pointer = bdev_io->u.bdev.offset_blocks +
					   bdev_io->u.bdev.num_blocks;
	assert(ocdev_io->io.zone->write_pointer <= ocdev_io->io.zone->slba +
	       ocdev_io->io.zone->capacity);

	__atomic_store_n(&ocdev_io->io.zone->busy, false, __ATOMIC_SEQ_CST);
	spdk_bdev_io_complete_nvme_status(bdev_io, 0, cpl->status.sct, cpl->status.sc);
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
	int rc;

	if ((bdev_io->u.bdev.offset_blocks % zone_size) + bdev_io->u.bdev.num_blocks > zone_size) {
		SPDK_ERRLOG("Tried to cross zone boundary during write command\n");
		return -EINVAL;
	}

	ocdev_io->io.zone = bdev_ocssd_get_zone_by_lba(ocssd_bdev, bdev_io->u.bdev.offset_blocks);
	if (__atomic_exchange_n(&ocdev_io->io.zone->busy, true, __ATOMIC_SEQ_CST)) {
		return -EINVAL;
	}

	ocdev_io->io.iov_pos = 0;
	ocdev_io->io.iov_off = 0;

	lba = bdev_ocssd_to_disk_lba(ocssd_bdev, bdev_io->u.bdev.offset_blocks);
	rc = spdk_nvme_ns_cmd_writev_with_md(nvme_bdev->nvme_ns->ns, nvme_ioch->qpair, lba,
					     bdev_io->u.bdev.num_blocks, bdev_ocssd_write_cb,
					     bdev_io, 0, bdev_ocssd_reset_sgl,
					     bdev_ocssd_next_sge, bdev_io->u.bdev.md_buf, 0, 0);
	if (spdk_unlikely(rc != 0)) {
		__atomic_store_n(&ocdev_io->io.zone->busy, false, __ATOMIC_SEQ_CST);
	}

	return rc;
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
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;

	ocdev_io->io.zone->write_pointer = ocdev_io->io.zone->slba;
	__atomic_store_n(&ocdev_io->io.zone->busy, false, __ATOMIC_SEQ_CST);
	spdk_bdev_io_complete_nvme_status(bdev_io, 0, cpl->status.sct, cpl->status.sc);
}

static int
bdev_ocssd_reset_zone(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io,
		      uint64_t slba, size_t num_zones)
{
	struct ocssd_bdev *ocssd_bdev = bdev_io->bdev->ctxt;
	struct nvme_bdev *nvme_bdev = &ocssd_bdev->nvme_bdev;
	struct nvme_io_channel *nvme_ioch = spdk_io_channel_get_ctx(ioch);
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;
	uint64_t offset, zone_size = nvme_bdev->disk.zone_size;
	int rc;

	if (num_zones > 1) {
		SPDK_ERRLOG("Exceeded maximum number of zones per single reset: 1\n");
		return -EINVAL;
	}

	ocdev_io->io.zone = bdev_ocssd_get_zone_by_slba(ocssd_bdev, slba);
	if (__atomic_exchange_n(&ocdev_io->io.zone->busy, true, __ATOMIC_SEQ_CST)) {
		return -EINVAL;
	}

	for (offset = 0; offset < num_zones; ++offset) {
		ocdev_io->io.lba[offset] = bdev_ocssd_to_disk_lba(ocssd_bdev,
					   slba + offset * zone_size);
	}

	rc = spdk_nvme_ocssd_ns_cmd_vector_reset(nvme_bdev->nvme_ns->ns, nvme_ioch->qpair,
			ocdev_io->io.lba, num_zones, NULL,
			bdev_ocssd_reset_zone_cb, bdev_io);
	if (spdk_unlikely(rc != 0)) {
		__atomic_store_n(&ocdev_io->io.zone->busy, false, __ATOMIC_SEQ_CST);
	}

	return rc;
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
	struct spdk_ocssd_chunk_information_entry *chunk_info = &ocdev_io->zone_info.chunk_info;
	struct spdk_bdev_zone_info *zone_info;
	int rc;

	if (spdk_unlikely(spdk_nvme_cpl_is_error(cpl))) {
		spdk_bdev_io_complete_nvme_status(bdev_io, 0, cpl->status.sct, cpl->status.sc);
		return;
	}

	zone_info = ((struct spdk_bdev_zone_info *)bdev_io->u.zone_mgmt.buf) +
		    ocdev_io->zone_info.chunk_offset;
	bdev_ocssd_fill_zone_info(ocssd_bdev, zone_info, chunk_info);

	if (++ocdev_io->zone_info.chunk_offset == bdev_io->u.zone_mgmt.num_zones) {
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
	struct bdev_ocssd_ns *ocssd_ns = bdev_ocssd_get_ns_from_bdev(ocssd_bdev);
	const struct spdk_ocssd_geometry_data *geo = &ocssd_ns->geometry;
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;
	uint64_t lba, grp, pu, chk, lbk, offset;

	lba = bdev_io->u.zone_mgmt.zone_id + ocdev_io->zone_info.chunk_offset *
	      nvme_bdev->disk.zone_size;
	bdev_ocssd_translate_lba(ocssd_bdev, lba, &grp, &pu, &chk, &lbk);
	offset = grp * geo->num_pu * geo->num_chk + pu * geo->num_chk + chk;

	return spdk_nvme_ctrlr_cmd_get_log_page(nvme_bdev->nvme_bdev_ctrlr->ctrlr,
						SPDK_OCSSD_LOG_CHUNK_INFO,
						spdk_nvme_ns_get_id(nvme_bdev->nvme_ns->ns),
						&ocdev_io->zone_info.chunk_info,
						sizeof(ocdev_io->zone_info.chunk_info),
						offset * sizeof(ocdev_io->zone_info.chunk_info),
						bdev_ocssd_zone_info_cb, (void *)bdev_io);
}

static int
bdev_ocssd_get_zone_info(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io)
{
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;

	if (bdev_io->u.zone_mgmt.num_zones < 1) {
		SPDK_ERRLOG("Invalid number of zones: %"PRIu32"\n", bdev_io->u.zone_mgmt.num_zones);
		return -EINVAL;
	}

	if (bdev_io->u.zone_mgmt.zone_id % bdev_io->bdev->zone_size != 0) {
		SPDK_ERRLOG("Unaligned zone LBA: %"PRIu64"\n", bdev_io->u.zone_mgmt.zone_id);
		return -EINVAL;
	}

	ocdev_io->zone_info.chunk_offset = 0;

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

static void bdev_ocssd_submit_request(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io);

static int
bdev_ocssd_poll_pending(void *ctx)
{
	struct spdk_io_channel *ioch = ctx;
	struct nvme_io_channel *nvme_ioch;
	struct ocssd_io_channel *ocssd_ioch;
	struct spdk_bdev_io *bdev_io;
	TAILQ_HEAD(, spdk_bdev_io) pending_requests;
	int num_requests = 0;

	nvme_ioch = spdk_io_channel_get_ctx(ioch);
	ocssd_ioch = nvme_ioch->ocssd_ioch;

	TAILQ_INIT(&pending_requests);
	TAILQ_SWAP(&ocssd_ioch->pending_requests, &pending_requests, spdk_bdev_io, module_link);

	while ((bdev_io = TAILQ_FIRST(&pending_requests))) {
		TAILQ_REMOVE(&pending_requests, bdev_io, module_link);
		bdev_ocssd_submit_request(ioch, bdev_io);
		num_requests++;
	}

	if (TAILQ_EMPTY(&ocssd_ioch->pending_requests)) {
		spdk_poller_unregister(&ocssd_ioch->pending_poller);
	}

	return num_requests;
}

static void
bdev_ocssd_delay_request(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io)
{
	struct nvme_io_channel *nvme_ioch = spdk_io_channel_get_ctx(ioch);
	struct ocssd_io_channel *ocssd_ioch = nvme_ioch->ocssd_ioch;

	TAILQ_INSERT_TAIL(&ocssd_ioch->pending_requests, bdev_io, module_link);

	if (ocssd_ioch->pending_poller != NULL) {
		return;
	}

	ocssd_ioch->pending_poller = spdk_poller_register(bdev_ocssd_poll_pending, ioch, 0);
	if (ocssd_ioch->pending_poller == NULL) {
		SPDK_ERRLOG("Failed to register pending requests poller\n");

		/* If the poller cannot be registered, complete all of the pending
		 * requests immediately, as we have no means of resubmitting them.
		 */
		while ((bdev_io = TAILQ_FIRST(&ocssd_ioch->pending_requests))) {
			TAILQ_REMOVE(&ocssd_ioch->pending_requests, bdev_io, module_link);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
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
		switch (rc) {
		case -ENOMEM:
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
			break;
		case -EAGAIN:
			bdev_ocssd_delay_request(ioch, bdev_io);
			break;
		default:
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
			break;
		}
	}
}

static bool
bdev_ocssd_io_type_supported(void *ctx, enum spdk_bdev_io_type type)
{
	switch (type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
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

struct bdev_ocssd_create_ctx {
	struct ocssd_bdev				*ocssd_bdev;
	bdev_ocssd_create_cb				cb_fn;
	void						*cb_arg;
	uint64_t					chunk_offset;
	uint64_t					num_total_chunks;
	uint64_t					num_chunks;
#define OCSSD_BDEV_CHUNK_INFO_COUNT 128
	struct spdk_ocssd_chunk_information_entry	chunk_info[OCSSD_BDEV_CHUNK_INFO_COUNT];
};

static void
bdev_ocssd_create_complete(struct bdev_ocssd_create_ctx *create_ctx, int status)
{
	const char *bdev_name = create_ctx->ocssd_bdev->nvme_bdev.disk.name;

	if (spdk_unlikely(status != 0)) {
		bdev_ocssd_free_bdev(create_ctx->ocssd_bdev);
	}

	create_ctx->cb_fn(bdev_name, status, create_ctx->cb_arg);
	free(create_ctx);
}

static int bdev_ocssd_init_zone(struct bdev_ocssd_create_ctx *create_ctx);

static void
bdev_ocssd_register_bdev(void *ctx)
{
	struct bdev_ocssd_create_ctx *create_ctx = ctx;
	struct ocssd_bdev *ocssd_bdev = create_ctx->ocssd_bdev;
	struct nvme_bdev *nvme_bdev = &ocssd_bdev->nvme_bdev;
	int rc;

	rc = spdk_bdev_register(&nvme_bdev->disk);
	if (spdk_likely(rc == 0)) {
		nvme_bdev_attach_bdev_to_ns(nvme_bdev->nvme_ns, nvme_bdev);
	} else {
		SPDK_ERRLOG("Failed to register bdev %s\n", nvme_bdev->disk.name);
	}

	bdev_ocssd_create_complete(create_ctx, rc);
}

static void
bdev_occsd_init_zone_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct bdev_ocssd_create_ctx *create_ctx = ctx;
	struct bdev_ocssd_zone *ocssd_zone;
	struct ocssd_bdev *ocssd_bdev = create_ctx->ocssd_bdev;
	struct spdk_bdev_zone_info zone_info = {};
	uint64_t offset;
	int rc = 0;

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_ERRLOG("Chunk information log page failed\n");
		bdev_ocssd_create_complete(create_ctx, -EIO);
		return;
	}

	for (offset = 0; offset < create_ctx->num_chunks; ++offset) {
		bdev_ocssd_fill_zone_info(ocssd_bdev, &zone_info, &create_ctx->chunk_info[offset]);

		ocssd_zone = bdev_ocssd_get_zone_by_slba(ocssd_bdev, zone_info.zone_id);
		if (!ocssd_zone) {
			SPDK_ERRLOG("Received invalid zone starting LBA: %"PRIu64"\n",
				    zone_info.zone_id);
			bdev_ocssd_create_complete(create_ctx, -EINVAL);
			return;
		}

		/* Make sure we're not filling the same zone twice */
		assert(ocssd_zone->busy);

		ocssd_zone->busy = false;
		ocssd_zone->slba = zone_info.zone_id;
		ocssd_zone->capacity = zone_info.capacity;
		ocssd_zone->write_pointer = zone_info.write_pointer;
	}

	create_ctx->chunk_offset += create_ctx->num_chunks;
	if (create_ctx->chunk_offset < create_ctx->num_total_chunks) {
		rc = bdev_ocssd_init_zone(create_ctx);
		if (spdk_unlikely(rc != 0)) {
			SPDK_ERRLOG("Failed to send chunk info log page\n");
			bdev_ocssd_create_complete(create_ctx, rc);
		}
	} else {
		/* Make sure all zones have been processed */
		for (offset = 0; offset < create_ctx->num_total_chunks; ++offset) {
			assert(!ocssd_bdev->zones[offset].busy);
		}

		/* Schedule the last bit of work (io_device, bdev registration) to be done in a
		 * context that is not tied to admin command's completion callback.
		 */
		spdk_thread_send_msg(spdk_get_thread(), bdev_ocssd_register_bdev, create_ctx);
	}
}

static int
bdev_ocssd_init_zone(struct bdev_ocssd_create_ctx *create_ctx)
{
	struct ocssd_bdev *ocssd_bdev = create_ctx->ocssd_bdev;
	struct nvme_bdev *nvme_bdev = &ocssd_bdev->nvme_bdev;

	create_ctx->num_chunks = spdk_min(create_ctx->num_total_chunks - create_ctx->chunk_offset,
					  OCSSD_BDEV_CHUNK_INFO_COUNT);
	assert(create_ctx->num_chunks > 0);

	return spdk_nvme_ctrlr_cmd_get_log_page(nvme_bdev->nvme_bdev_ctrlr->ctrlr,
						SPDK_OCSSD_LOG_CHUNK_INFO,
						spdk_nvme_ns_get_id(nvme_bdev->nvme_ns->ns),
						&create_ctx->chunk_info,
						sizeof(create_ctx->chunk_info[0]) *
						create_ctx->num_chunks,
						sizeof(create_ctx->chunk_info[0]) *
						create_ctx->chunk_offset,
						bdev_occsd_init_zone_cb, create_ctx);
}

static int
bdev_ocssd_init_zones(struct bdev_ocssd_create_ctx *create_ctx)
{
	struct ocssd_bdev *ocssd_bdev = create_ctx->ocssd_bdev;
	struct spdk_bdev *bdev = &ocssd_bdev->nvme_bdev.disk;
	uint64_t offset;

	ocssd_bdev->zones = calloc(bdev->blockcnt / bdev->zone_size, sizeof(*ocssd_bdev->zones));
	if (!ocssd_bdev->zones) {
		return -ENOMEM;
	}

	create_ctx->num_total_chunks = bdev->blockcnt / bdev->zone_size;
	create_ctx->chunk_offset = 0;

	/* Mark all zones as busy and clear it as their info is filled */
	for (offset = 0; offset < create_ctx->num_total_chunks; ++offset) {
		ocssd_bdev->zones[offset].busy = true;
	}

	return bdev_ocssd_init_zone(create_ctx);
}

void
bdev_ocssd_create_bdev(const char *ctrlr_name, const char *bdev_name, uint32_t nsid,
		       bdev_ocssd_create_cb cb_fn, void *cb_arg)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;
	struct bdev_ocssd_create_ctx *create_ctx = NULL;
	struct nvme_bdev *nvme_bdev = NULL;
	struct ocssd_bdev *ocssd_bdev = NULL;
	struct spdk_nvme_ns *ns;
	struct nvme_bdev_ns *nvme_ns;
	struct bdev_ocssd_ns *ocssd_ns;
	struct spdk_ocssd_geometry_data *geometry;
	int rc = 0;

	nvme_bdev_ctrlr = nvme_bdev_ctrlr_get_by_name(ctrlr_name);
	if (!nvme_bdev_ctrlr) {
		SPDK_ERRLOG("Unable to find controller %s\n", ctrlr_name);
		rc = -ENODEV;
		goto error;
	}

	ns = spdk_nvme_ctrlr_get_ns(nvme_bdev_ctrlr->ctrlr, nsid);
	if (!ns) {
		SPDK_ERRLOG("Unable to retrieve namespace %"PRIu32"\n", nsid);
		rc = -ENODEV;
		goto error;
	}

	if (!spdk_nvme_ns_is_active(ns)) {
		SPDK_ERRLOG("Namespace %"PRIu32" is inactive\n", nsid);
		rc = -EACCES;
		goto error;
	}

	assert(nsid <= nvme_bdev_ctrlr->num_ns);
	nvme_ns = nvme_bdev_ctrlr->namespaces[nsid - 1];
	if (nvme_ns == NULL) {
		SPDK_ERRLOG("Namespace %"PRIu32" is not initialized\n", nsid);
		rc = -EINVAL;
		goto error;
	}

	ocssd_ns = bdev_ocssd_get_ns_from_nvme(nvme_ns);
	if (ocssd_ns == NULL) {
		SPDK_ERRLOG("Namespace %"PRIu32" is not an OCSSD namespace\n", nsid);
		rc = -EINVAL;
		goto error;
	}

	if (spdk_bdev_get_by_name(bdev_name) != NULL) {
		SPDK_ERRLOG("Device with provided name (%s) already exists\n", bdev_name);
		rc = -EEXIST;
		goto error;
	}

	/* Only allow one bdev per namespace for now */
	if (!TAILQ_EMPTY(&nvme_ns->bdevs)) {
		SPDK_ERRLOG("Namespace %"PRIu32" was already claimed by bdev %s\n",
			    nsid, TAILQ_FIRST(&nvme_ns->bdevs)->disk.name);
		rc = -EEXIST;
		goto error;
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
	nvme_bdev->nvme_ns = nvme_ns;
	nvme_bdev->nvme_bdev_ctrlr = nvme_bdev_ctrlr;
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
	nvme_bdev->disk.blocklen = spdk_nvme_ns_get_extended_sector_size(ns);
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

	rc = bdev_ocssd_init_zones(create_ctx);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Failed to initialize zones on bdev %s\n", nvme_bdev->disk.name);
		goto error;
	}

	return;
error:
	bdev_ocssd_free_bdev(ocssd_bdev);
	cb_fn(NULL, rc, cb_arg);
	free(create_ctx);
}

struct bdev_ocssd_delete_ctx {
	bdev_ocssd_delete_cb	cb_fn;
	void			*cb_arg;
};

static void
bdev_ocssd_unregister_cb(void *cb_arg, int status)
{
	struct bdev_ocssd_delete_ctx *delete_ctx = cb_arg;

	delete_ctx->cb_fn(status, delete_ctx->cb_arg);
	free(delete_ctx);
}

void
bdev_ocssd_delete_bdev(const char *bdev_name, bdev_ocssd_delete_cb cb_fn, void *cb_arg)
{
	struct spdk_bdev *bdev;
	struct bdev_ocssd_delete_ctx *delete_ctx;

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (!bdev) {
		SPDK_ERRLOG("Unable to find bdev %s\n", bdev_name);
		cb_fn(-ENODEV, cb_arg);
		return;
	}

	if (bdev->module != &ocssd_if) {
		SPDK_ERRLOG("Specified bdev %s is not an OCSSD bdev\n", bdev_name);
		cb_fn(-EINVAL, cb_arg);
		return;
	}

	delete_ctx = calloc(1, sizeof(*delete_ctx));
	if (!delete_ctx) {
		SPDK_ERRLOG("Unable to allocate deletion context\n");
		cb_fn(-ENOMEM, cb_arg);
		return;
	}

	delete_ctx->cb_fn = cb_fn;
	delete_ctx->cb_arg = cb_arg;

	spdk_bdev_unregister(bdev, bdev_ocssd_unregister_cb, delete_ctx);
}

struct bdev_ocssd_populate_ns_ctx {
	struct nvme_async_probe_ctx	*nvme_ctx;
	struct nvme_bdev_ns		*nvme_ns;
};

static void
bdev_ocssd_geometry_cb(void *_ctx, const struct spdk_nvme_cpl *cpl)
{
	struct bdev_ocssd_populate_ns_ctx *ctx = _ctx;
	struct nvme_bdev_ns *nvme_ns = ctx->nvme_ns;
	struct bdev_ocssd_ns *ocssd_ns = bdev_ocssd_get_ns_from_nvme(nvme_ns);
	int rc = 0;

	if (spdk_unlikely(spdk_nvme_cpl_is_error(cpl))) {
		SPDK_ERRLOG("Failed to retrieve geometry for namespace %"PRIu32"\n", nvme_ns->id);
		free(nvme_ns->type_ctx);
		nvme_ns->type_ctx = NULL;
		rc = -EIO;
	} else {
		ocssd_ns->lba_offsets.lbk = 0;
		ocssd_ns->lba_offsets.chk = ocssd_ns->lba_offsets.lbk +
					    ocssd_ns->geometry.lbaf.lbk_len;
		ocssd_ns->lba_offsets.pu  = ocssd_ns->lba_offsets.chk +
					    ocssd_ns->geometry.lbaf.chk_len;
		ocssd_ns->lba_offsets.grp = ocssd_ns->lba_offsets.pu +
					    ocssd_ns->geometry.lbaf.pu_len;
	}

	nvme_ctrlr_populate_namespace_done(ctx->nvme_ctx, nvme_ns, rc);
	free(ctx);
}

void
bdev_ocssd_populate_namespace(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr,
			      struct nvme_bdev_ns *nvme_ns,
			      struct nvme_async_probe_ctx *nvme_ctx)
{
	struct bdev_ocssd_ns *ocssd_ns;
	struct bdev_ocssd_populate_ns_ctx *ctx;
	struct spdk_nvme_ns *ns;
	int rc;

	ns = spdk_nvme_ctrlr_get_ns(nvme_bdev_ctrlr->ctrlr, nvme_ns->id);
	if (ns == NULL) {
		nvme_ctrlr_populate_namespace_done(nvme_ctx, nvme_ns, -EINVAL);
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		nvme_ctrlr_populate_namespace_done(nvme_ctx, nvme_ns, -ENOMEM);
		return;
	}

	ocssd_ns = calloc(1, sizeof(*ocssd_ns));
	if (ocssd_ns == NULL) {
		nvme_ctrlr_populate_namespace_done(nvme_ctx, nvme_ns, -ENOMEM);
		free(ctx);
		return;
	}

	nvme_ns->type_ctx = ocssd_ns;
	nvme_ns->ns = ns;
	ctx->nvme_ctx = nvme_ctx;
	ctx->nvme_ns = nvme_ns;

	rc = spdk_nvme_ocssd_ctrlr_cmd_geometry(nvme_bdev_ctrlr->ctrlr, nvme_ns->id,
						&ocssd_ns->geometry,
						sizeof(ocssd_ns->geometry),
						bdev_ocssd_geometry_cb, ctx);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Failed to retrieve OC geometry: %s\n", spdk_strerror(-rc));
		nvme_ns->type_ctx = NULL;
		nvme_ctrlr_populate_namespace_done(nvme_ctx, nvme_ns, rc);
		free(ocssd_ns);
		free(ctx);
	}
}

void
bdev_ocssd_depopulate_namespace(struct nvme_bdev_ns *ns)
{
	struct nvme_bdev *bdev, *tmp;

	TAILQ_FOREACH_SAFE(bdev, &ns->bdevs, tailq, tmp) {
		spdk_bdev_unregister(&bdev->disk, NULL, NULL);
	}

	free(ns->type_ctx);
	ns->populated = false;
	ns->type_ctx = NULL;
}

int
bdev_ocssd_create_io_channel(struct nvme_io_channel *ioch)
{
	ioch->ocssd_ioch = calloc(1, sizeof(*ioch->ocssd_ioch));
	if (ioch->ocssd_ioch == NULL) {
		return -ENOMEM;
	}

	TAILQ_INIT(&ioch->ocssd_ioch->pending_requests);

	return 0;
}

void
bdev_ocssd_destroy_io_channel(struct nvme_io_channel *ioch)
{
	free(ioch->ocssd_ioch);
}

SPDK_LOG_REGISTER_COMPONENT("bdev_ocssd", SPDK_LOG_BDEV_OCSSD)
