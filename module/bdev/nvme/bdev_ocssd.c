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
	uint32_t	grp;
	uint32_t	pu;
	uint32_t	chk;
	uint32_t	lbk;
};

struct bdev_ocssd_io_channel {
	struct spdk_nvme_qpair	*qpair;
	struct spdk_poller	*poller;
};

struct bdev_ocssd_zone {
	uint64_t	slba;
	uint64_t	write_pointer;
	uint64_t	capacity;
	bool		busy;
};

struct bdev_ocssd_config {
	char				*ctrlr_name;
	char				*bdev_name;
	uint32_t			nsid;
	TAILQ_ENTRY(bdev_ocssd_config)	tailq;
};

struct bdev_ocssd_io {
	union {
		struct {
			struct bdev_ocssd_zone	*zone;
			size_t			iov_pos;
			size_t			iov_off;
			uint64_t		lba[SPDK_NVME_OCSSD_MAX_LBAL_ENTRIES];
		};
		struct {
			size_t						chunk_offset;
			struct spdk_ocssd_chunk_information_entry	chunk_info;
		};
	};
};

struct ocssd_bdev {
	struct nvme_bdev		nvme_bdev;
	struct spdk_ocssd_geometry_data	geometry;
	struct bdev_ocssd_lba_offsets	lba_offsets;
	struct bdev_ocssd_zone		*zones;
};

static TAILQ_HEAD(, bdev_ocssd_config) g_ocssd_config = TAILQ_HEAD_INITIALIZER(g_ocssd_config);

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
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;
	struct nvme_bdev *nvme_bdev;

	TAILQ_FOREACH(nvme_bdev_ctrlr, &g_nvme_bdev_ctrlrs, tailq) {
		if (!spdk_nvme_ctrlr_is_ocssd_supported(nvme_bdev_ctrlr->ctrlr)) {
			continue;
		}

		TAILQ_FOREACH(nvme_bdev, &nvme_bdev_ctrlr->bdevs, tailq) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "method", "bdev_ocssd_create");

			spdk_json_write_named_object_begin(w, "params");
			spdk_json_write_named_string(w, "ctrlr_name", nvme_bdev_ctrlr->name);
			spdk_json_write_named_string(w, "bdev_name", nvme_bdev->disk.name);
			spdk_json_write_named_uint32(w, "nsid", spdk_nvme_ns_get_id(nvme_bdev->ns));
			spdk_json_write_object_end(w);

			spdk_json_write_object_end(w);
		}
	}

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

static int
bdev_ocssd_poll_ioq(void *ctx)
{
	struct bdev_ocssd_io_channel *ioch = ctx;

	return spdk_nvme_qpair_process_completions(ioch->qpair, 0);
}

static int
bdev_ocssd_io_channel_create_cb(void *io_device, void *ctx_buf)
{
	struct ocssd_bdev *ocssd_bdev = io_device;
	struct spdk_nvme_ctrlr *ctrlr = ocssd_bdev->nvme_bdev.nvme_bdev_ctrlr->ctrlr;
	struct bdev_ocssd_io_channel *ioch = ctx_buf;

	ioch->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
	if (!ioch->qpair) {
		SPDK_ERRLOG("Failed to alloc IO queue pair\n");
		return -ENOMEM;
	}

	ioch->poller = spdk_poller_register(bdev_ocssd_poll_ioq, ioch, 0);
	if (!ioch->poller) {
		SPDK_ERRLOG("Failed to register IO queue poller\n");
		spdk_nvme_ctrlr_free_io_qpair(ioch->qpair);
		return -ENOMEM;
	}

	return 0;
}

static void
bdev_ocssd_io_channel_destroy_cb(void *io_device, void *ctx_buf)
{
	struct bdev_ocssd_io_channel *ioch = ctx_buf;

	spdk_nvme_ctrlr_free_io_qpair(ioch->qpair);
	spdk_poller_unregister(&ioch->poller);
}

static int
bdev_ocssd_poll_adminq(void *ctx)
{
	struct nvme_bdev_ctrlr *ctrlr = ctx;

	return spdk_nvme_ctrlr_process_admin_completions(ctrlr->ctrlr);
}

static void
bdev_ocssd_destruct_ctrlr_cb(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr)
{
	assert(nvme_bdev_ctrlr->ref == 0);

	spdk_poller_unregister(&nvme_bdev_ctrlr->adminq_timer_poller);
}

static void
bdev_ocssd_free_ctrlr(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr)
{
	bdev_ocssd_destruct_ctrlr_cb(nvme_bdev_ctrlr);
	nvme_bdev_ctrlr_destruct(nvme_bdev_ctrlr);
}

static void bdev_ocssd_free_bdev(struct ocssd_bdev *ocssd_bdev)
{
	struct nvme_bdev *nvme_bdev;

	if (!ocssd_bdev) {
		return;
	}

	nvme_bdev = &ocssd_bdev->nvme_bdev;

	free(nvme_bdev->disk.name);
	free(ocssd_bdev->zones);
	free(ocssd_bdev);
}

static void
bdev_ocssd_unregister_cb(void *io_device)
{
	struct ocssd_bdev *ocssd_bdev = io_device;
	struct nvme_bdev *nvme_bdev = &ocssd_bdev->nvme_bdev;
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr = nvme_bdev->nvme_bdev_ctrlr;

	TAILQ_REMOVE(&nvme_bdev_ctrlr->bdevs, nvme_bdev, tailq);

	assert(nvme_bdev_ctrlr->ref > 0);

	if (--nvme_bdev_ctrlr->ref == 0 && nvme_bdev_ctrlr->destruct) {
		bdev_ocssd_free_ctrlr(nvme_bdev_ctrlr);
	}

	spdk_bdev_destruct_done(&nvme_bdev->disk, 0);
	bdev_ocssd_free_bdev(ocssd_bdev);
}

static int
bdev_ocssd_destruct(void *ctx)
{
	struct ocssd_bdev *ocssd_bdev = ctx;

	spdk_io_device_unregister(ocssd_bdev, bdev_ocssd_unregister_cb);

	/* Return one to indicate that the destruction is deferred */
	return 1;
}

static void
bdev_ocssd_translate_lba(struct ocssd_bdev *ocssd_bdev, uint64_t lba, uint64_t *grp,
			 uint64_t *pu, uint64_t *chk, uint64_t *lbk)
{
	const struct spdk_ocssd_geometry_data *geo = &ocssd_bdev->geometry;
	uint64_t addr_shift;

	/* To achieve best performance, we need to make sure that adjacent zones can be accessed
	 * in parallel.  We accomplish this by having the following addressing scheme:
	 *
	 * [            zone id              ][  zone offset  ] User's LBA
	 * [ chunk ][ parallel unit ][ group ][ logical block ] Open Channel's LBA
	 *
	 * which means that neighbouring zones are placed in a different group and parallel unit.
	 */
	*lbk = lba % geo->clba;
	addr_shift = geo->clba;

	*grp = (lba / addr_shift) % geo->num_grp;
	addr_shift *= geo->num_grp;

	*pu = (lba / addr_shift) % geo->num_pu;
	addr_shift *= geo->num_pu;

	*chk = (lba / addr_shift) % geo->num_chk;
}

static uint64_t
bdev_ocssd_from_disk_lba(struct ocssd_bdev *ocssd_bdev, uint64_t lba)
{
	const struct bdev_ocssd_lba_offsets *offsets = &ocssd_bdev->lba_offsets;
	const struct spdk_ocssd_geometry_data *geometry = &ocssd_bdev->geometry;
	uint64_t lbk, chk, pu, grp;

	lbk = (lba >> offsets->lbk) & ((1 << geometry->lbaf.lbk_len) - 1);
	chk = (lba >> offsets->chk) & ((1 << geometry->lbaf.chk_len) - 1);
	pu  = (lba >> offsets->pu)  & ((1 << geometry->lbaf.pu_len)  - 1);
	grp = (lba >> offsets->grp) & ((1 << geometry->lbaf.grp_len) - 1);

	return lbk + grp * geometry->clba + pu * geometry->num_grp * geometry->clba +
	       chk * geometry->num_pu * geometry->num_grp * geometry->clba;
}

static uint64_t
bdev_ocssd_to_disk_lba(struct ocssd_bdev *ocssd_bdev, uint64_t lba)
{
	const struct bdev_ocssd_lba_offsets *offsets = &ocssd_bdev->lba_offsets;
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
	struct bdev_ocssd_io_channel *ocdev_ioch = spdk_io_channel_get_ctx(ioch);
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

	return spdk_nvme_ns_cmd_readv_with_md(nvme_bdev->ns, ocdev_ioch->qpair, lba,
					      bdev_io->u.bdev.num_blocks, bdev_ocssd_read_cb,
					      bdev_io, 0, bdev_ocssd_reset_sgl,
					      bdev_ocssd_next_sge, bdev_io->u.bdev.md_buf, 0, 0);
}

static void
bdev_ocssd_write_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_bdev_io *bdev_io = ctx;
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;

	if (!spdk_nvme_cpl_is_error(cpl)) {
		ocdev_io->zone->write_pointer = bdev_io->u.bdev.offset_blocks +
						bdev_io->u.bdev.num_blocks;
		assert(ocdev_io->zone->write_pointer <= ocdev_io->zone->slba +
		       ocdev_io->zone->capacity);
	}

	__atomic_store_n(&ocdev_io->zone->busy, false, __ATOMIC_SEQ_CST);
	spdk_bdev_io_complete_nvme_status(bdev_io, cpl->status.sct, cpl->status.sc);
}

static int
bdev_ocssd_write(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io)
{
	struct ocssd_bdev *ocssd_bdev = bdev_io->bdev->ctxt;
	struct nvme_bdev *nvme_bdev = &ocssd_bdev->nvme_bdev;
	struct bdev_ocssd_io_channel *ocdev_ioch = spdk_io_channel_get_ctx(ioch);
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;
	const size_t zone_size = nvme_bdev->disk.zone_size;
	uint64_t lba;
	int rc;

	if ((bdev_io->u.bdev.offset_blocks % zone_size) + bdev_io->u.bdev.num_blocks > zone_size) {
		SPDK_ERRLOG("Tried to cross zone boundary during write commnad\n");
		return -EINVAL;
	}

	ocdev_io->zone = bdev_ocssd_get_zone_by_lba(ocssd_bdev, bdev_io->u.bdev.offset_blocks);
	if (__atomic_exchange_n(&ocdev_io->zone->busy, true, __ATOMIC_SEQ_CST)) {
		return -ENOMEM;
	}

	ocdev_io->iov_pos = 0;
	ocdev_io->iov_off = 0;

	lba = bdev_ocssd_to_disk_lba(ocssd_bdev, bdev_io->u.bdev.offset_blocks);
	rc =  spdk_nvme_ns_cmd_writev_with_md(nvme_bdev->ns, ocdev_ioch->qpair, lba,
					      bdev_io->u.bdev.num_blocks, bdev_ocssd_write_cb,
					      bdev_io, 0, bdev_ocssd_reset_sgl,
					      bdev_ocssd_next_sge, bdev_io->u.bdev.md_buf, 0, 0);
	if (spdk_unlikely(rc != 0)) {
		__atomic_store_n(&ocdev_io->zone->busy, false, __ATOMIC_SEQ_CST);
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

	if (!spdk_nvme_cpl_is_error(cpl)) {
		ocdev_io->zone->write_pointer = ocdev_io->zone->slba;
	}

	__atomic_store_n(&ocdev_io->zone->busy, false, __ATOMIC_SEQ_CST);
	spdk_bdev_io_complete_nvme_status(bdev_io, cpl->status.sct, cpl->status.sc);
}

static int
bdev_ocssd_reset_zone(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io,
		      uint64_t slba, size_t num_zones)
{
	struct ocssd_bdev *ocssd_bdev = bdev_io->bdev->ctxt;
	struct nvme_bdev *nvme_bdev = &ocssd_bdev->nvme_bdev;
	struct bdev_ocssd_io_channel *ocdev_ioch = spdk_io_channel_get_ctx(ioch);
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;
	size_t offset, zone_size = nvme_bdev->disk.zone_size;

	if (num_zones > SPDK_NVME_OCSSD_MAX_LBAL_ENTRIES) {
		SPDK_ERRLOG("Exceeded maximum number of zones per single reset: %d\n",
			    SPDK_NVME_OCSSD_MAX_LBAL_ENTRIES);
		return -EINVAL;
	}

	ocdev_io->zone = bdev_ocssd_get_zone_by_lba(ocssd_bdev, bdev_io->u.bdev.offset_blocks);
	if (__atomic_exchange_n(&ocdev_io->zone->busy, true, __ATOMIC_SEQ_CST)) {
		return -ENOMEM;
	}

	for (offset = 0; offset < num_zones; ++offset) {
		ocdev_io->lba[offset] = bdev_ocssd_to_disk_lba(ocssd_bdev,
					slba + offset * zone_size);
	}

	return spdk_nvme_ocssd_ns_cmd_vector_reset(nvme_bdev->ns, ocdev_ioch->qpair, ocdev_io->lba,
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

	zone_info = ((struct spdk_bdev_zone_info *)bdev_io->u.zdev.buf) + ocdev_io->chunk_offset;
	bdev_ocssd_fill_zone_info(ocssd_bdev, zone_info, chunk_info);

	if (++ocdev_io->chunk_offset == bdev_io->u.zdev.num_zones) {
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
	const struct spdk_ocssd_geometry_data *geo = &ocssd_bdev->geometry;
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;
	uint64_t lba, grp, pu, chk, lbk, offset;

	lba = bdev_io->u.zdev.zone_id + ocdev_io->chunk_offset * nvme_bdev->disk.zone_size;
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

	if (bdev_io->u.zdev.num_zones < 1) {
		return -EINVAL;
	}

	if (bdev_io->u.zdev.zone_id % bdev_io->bdev->zone_size != 0) {
		return -EINVAL;
	}

	ocdev_io->chunk_offset = 0;

	return _bdev_ocssd_get_zone_info(bdev_io);
}

static int
bdev_ocssd_zone_management(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->u.zdev.zone_action) {
	case SPDK_BDEV_ZONE_RESET:
		return bdev_ocssd_reset_zone(ioch, bdev_io, bdev_io->u.zdev.zone_id,
					     bdev_io->u.zdev.num_zones);
	case SPDK_BDEV_ZONE_INFO:
		return bdev_ocssd_get_zone_info(ioch, bdev_io);

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
	return spdk_get_io_channel(ctx);
}

static struct spdk_bdev_fn_table ocssdlib_fn_table = {
	.destruct		= bdev_ocssd_destruct,
	.submit_request		= bdev_ocssd_submit_request,
	.io_type_supported	= bdev_ocssd_io_type_supported,
	.get_io_channel		= bdev_ocssd_get_io_channel,
};

struct bdev_ocssd_create_ctx {
	struct ocssd_bdev				*ocssd_bdev;
	spdk_bdev_ocssd_create_cb			cb_fn;
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
		bdev_name = NULL;
	}

	create_ctx->cb_fn(bdev_name, status, create_ctx->cb_arg);
	free(create_ctx);
}

static int bdev_ocssd_init_zone(struct bdev_ocssd_create_ctx *create_ctx);

static void
bdev_occsd_init_zone_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct bdev_ocssd_create_ctx *create_ctx = ctx;
	struct bdev_ocssd_zone *ocssd_zone;
	struct ocssd_bdev *ocssd_bdev = create_ctx->ocssd_bdev;
	struct nvme_bdev *nvme_bdev = &ocssd_bdev->nvme_bdev;
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
#if DEBUG
		/* Make sure all zones have been processed */
		uint64_t _offset;
		for (_offset = 0; _offset < create_ctx->num_total_chunks; ++_offset) {
			assert(!ocssd_bdev->zones[_offset].busy);
		}
#endif
		spdk_io_device_register(ocssd_bdev, bdev_ocssd_io_channel_create_cb,
					bdev_ocssd_io_channel_destroy_cb,
					sizeof(struct bdev_ocssd_io_channel), nvme_bdev->disk.name);

		rc = spdk_bdev_register(&nvme_bdev->disk);
		if (spdk_unlikely(rc != 0)) {
			SPDK_ERRLOG("Failed to register bdev %s\n", nvme_bdev->disk.name);
			spdk_io_device_unregister(ocssd_bdev, NULL);
		} else {
			TAILQ_INSERT_TAIL(&nvme_bdev->nvme_bdev_ctrlr->bdevs, nvme_bdev, tailq);
			nvme_bdev->nvme_bdev_ctrlr->ref++;
		}

		bdev_ocssd_create_complete(create_ctx, rc);
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
						spdk_nvme_ns_get_id(nvme_bdev->ns),
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

	ocssd_bdev->zones = calloc(bdev->blockcnt / bdev->zone_size, sizeof(*ocssd_bdev->zones));
	if (!ocssd_bdev->zones) {
		return -ENOMEM;
	}

	create_ctx->num_total_chunks = bdev->blockcnt / bdev->zone_size;
	create_ctx->chunk_offset = 0;

#if DEBUG
	/* Mark all zones as busy and clear it as their info is filled */
	uint64_t _offset = 0;
	for (_offset = 0; _offset < create_ctx->num_total_chunks; ++_offset) {
		ocssd_bdev->zones[_offset].busy = true;
	}
#endif
	return bdev_ocssd_init_zone(create_ctx);
}

static void
bdev_ocssd_geometry_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct bdev_ocssd_create_ctx *create_ctx = ctx;
	struct ocssd_bdev *ocssd_bdev = create_ctx->ocssd_bdev;
	const struct spdk_ocssd_geometry_data *geometry = &ocssd_bdev->geometry;
	struct nvme_bdev *nvme_bdev = &ocssd_bdev->nvme_bdev;
	int rc = 0;

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_ERRLOG("Failed to retrieve controller's geometry\n");
		bdev_ocssd_create_complete(create_ctx, -EIO);
		return;
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

	rc = bdev_ocssd_init_zones(create_ctx);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Failed to initialize zones on bdev %s\n", nvme_bdev->disk.name);
		bdev_ocssd_create_complete(create_ctx, rc);
	}
}

static void
bdev_ocssd_free_config(struct bdev_ocssd_config *config)
{
	free(config->ctrlr_name);
	free(config->bdev_name);
	free(config);
}

static int
bdev_ocssd_save_config(const char *ctrlr_name, const char *bdev_name, uint32_t nsid)
{
	struct bdev_ocssd_config *config;

	config = calloc(1, sizeof(*config));
	if (!config) {
		return -ENOMEM;
	}

	config->ctrlr_name = strdup(ctrlr_name);
	if (!config->ctrlr_name) {
		bdev_ocssd_free_config(config);
		return -ENOMEM;
	}

	config->bdev_name = strdup(bdev_name);
	if (!config->bdev_name) {
		bdev_ocssd_free_config(config);
		return -ENOMEM;
	}

	config->nsid = nsid;
	TAILQ_INSERT_TAIL(&g_ocssd_config, config, tailq);

	return 0;
}

static int
bdev_ocssd_create_bdev(const char *ctrlr_name, const char *bdev_name, uint32_t nsid,
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

int
spdk_bdev_ocssd_create_bdev(const char *ctrlr_name, const char *bdev_name, uint32_t nsid,
			    spdk_bdev_ocssd_create_cb cb_fn, void *cb_arg)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;
	int rc = 0;

	nvme_bdev_ctrlr = nvme_bdev_ctrlr_get_by_name(ctrlr_name);
	if (!nvme_bdev_ctrlr) {
		SPDK_ERRLOG("Unable to find controller %s, deferring bdev %s initialization\n",
			    ctrlr_name, bdev_name);

		rc = bdev_ocssd_save_config(ctrlr_name, bdev_name, nsid);
		if (spdk_unlikely(rc != 0)) {
			SPDK_ERRLOG("Unable to save bdev %s configuration\n", bdev_name);
			return rc;
		}

		/* Return bdev's name even though we haven't created it yet to allow creating it
		 * later (e.g. due to changing the order of creating NVMe controller vs. creating
		 * OCSSD bdev druing load_config).
		 */
		cb_fn(bdev_name, 0, cb_arg);

		return 0;
	}

	return bdev_ocssd_create_bdev(ctrlr_name, bdev_name, nsid, cb_fn, cb_arg);
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
bdev_ocssd_create_deferred_cb(const char *bdev_name, int status, void *ctx)
{
	struct bdev_ocssd_config *config = ctx;

	if (spdk_unlikely(status != 0)) {
		SPDK_ERRLOG("Failed to create bdev %s\n", config->bdev_name);
	}

	TAILQ_REMOVE(&g_ocssd_config, config, tailq);
	bdev_ocssd_free_config(config);
}

static void
bdev_ocssd_create_deferred(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr)
{
	struct bdev_ocssd_config *config, *tmp;
	int rc;

	TAILQ_FOREACH_SAFE(config, &g_ocssd_config, tailq, tmp) {
		if (strcmp(config->ctrlr_name, nvme_bdev_ctrlr->name) != 0) {
			continue;
		}

		rc = bdev_ocssd_create_bdev(config->ctrlr_name, config->bdev_name, config->nsid,
					    bdev_ocssd_create_deferred_cb, config);
		if (spdk_unlikely(rc != 0)) {
			SPDK_ERRLOG("Unable to create bdev %s on controller %s, freeing config\n",
				    config->bdev_name, config->ctrlr_name);
			TAILQ_REMOVE(&g_ocssd_config, config, tailq);
			bdev_ocssd_free_config(config);
		}
	}
}

int
spdk_bdev_ocssd_create_ctrlr(const struct spdk_nvme_transport_id *trid)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;

	nvme_bdev_ctrlr = nvme_bdev_ctrlr_get(trid);
	if (!nvme_bdev_ctrlr) {
		SPDK_ERRLOG("Failed to find NVMe controller: %s\n", trid->traddr);
		return -ENODEV;
	}

	if (!spdk_nvme_ctrlr_is_ocssd_supported(nvme_bdev_ctrlr->ctrlr)) {
		SPDK_ERRLOG("Specified controller doesn't support Open Channel\n");
		return -EINVAL;
	}

	nvme_bdev_ctrlr->adminq_timer_poller = spdk_poller_register(bdev_ocssd_poll_adminq,
					       nvme_bdev_ctrlr, 100000ULL);
	if (!nvme_bdev_ctrlr->adminq_timer_poller) {
		SPDK_ERRLOG("Failed to register admin queue poller\n");
		return -ENOMEM;
	}

	nvme_bdev_ctrlr->destruct_ctrlr_fn = bdev_ocssd_destruct_ctrlr_cb;
	nvme_bdev_ctrlr->mode = SPDK_NVME_OCSSD_CTRLR;

	bdev_ocssd_create_deferred(nvme_bdev_ctrlr);

	return 0;
}

int
spdk_bdev_ocssd_create_bdevs(struct nvme_async_probe_ctx *ctx)
{
	ctx->count = 0;
	ctx->bdevs_done = true;
	return 0;
}

SPDK_LOG_REGISTER_COMPONENT("bdev_ocssd", SPDK_LOG_BDEV_OCSSD)
