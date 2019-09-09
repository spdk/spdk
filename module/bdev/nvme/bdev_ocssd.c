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
#include "spdk_internal/log.h"
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
	struct spdk_ocssd_geometry_data	geometry;
	struct bdev_ocssd_lba_offsets	lba_offsets;
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

static int
bdev_ocssd_poll_ioq(void *ctx)
{
	struct bdev_ocssd_io_channel *ioch = ctx;

	return spdk_nvme_qpair_process_completions(ioch->qpair, 0);
}

static int
bdev_ocssd_io_channel_create_cb(void *io_device, void *ctx_buf)
{
	struct spdk_nvme_ctrlr *ctrlr = io_device;
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

static int
bdev_ocssd_create_ctrlr(const struct spdk_nvme_transport_id *trid, const char *name,
			struct nvme_bdev_ctrlr **_ctrlr)
{
	struct nvme_bdev_ctrlr *ctrlr;

	if (nvme_bdev_ctrlr_get(trid) != NULL) {
		SPDK_ERRLOG("Controller with the provided trid (traddr: %s) already exists\n",
			    trid->traddr);
		return -EEXIST;
	}

	ctrlr = calloc(1, sizeof(*ctrlr));
	if (!ctrlr) {
		return -ENOMEM;
	}

	ctrlr->name = strdup(name);
	if (!ctrlr->name) {
		free(ctrlr);
		return -ENOMEM;
	}

	ctrlr->ctrlr = spdk_nvme_connect(trid, NULL, 0);
	if (!ctrlr->ctrlr) {
		SPDK_ERRLOG("Unable to connect to provided trid (traddr: %s)\n", trid->traddr);
		free(ctrlr->name);
		free(ctrlr);
		return -ENODEV;
	}

	if (!spdk_nvme_ctrlr_is_ocssd_supported(ctrlr->ctrlr)) {
		SPDK_ERRLOG("Specified controller doesn't support Open Channel\n");
		spdk_nvme_detach(ctrlr->ctrlr);
		free(ctrlr->name);
		free(ctrlr);
		return -EINVAL;
	}

	if (spdk_nvme_ctrlr_get_num_ns(ctrlr->ctrlr) == 0) {
		SPDK_ERRLOG("Controller with the provided trid (traddr: %s)"
			    "doesn't contain any namespaces\n", trid->traddr);
		spdk_nvme_detach(ctrlr->ctrlr);
		free(ctrlr->name);
		free(ctrlr);
		return -ENODEV;
	}

	ctrlr->bdevs = calloc(spdk_nvme_ctrlr_get_num_ns(ctrlr->ctrlr), sizeof(*ctrlr->bdevs));
	if (!ctrlr->bdevs) {
		spdk_nvme_detach(ctrlr->ctrlr);
		free(ctrlr->name);
		free(ctrlr);
		return -ENOMEM;
	}

	ctrlr->ref = 0;
	ctrlr->trid = *trid;
	ctrlr->adminq_timer_poller = spdk_poller_register(bdev_ocssd_poll_adminq, ctrlr,
				     100ULL);
	if (!ctrlr->adminq_timer_poller) {
		spdk_nvme_detach(ctrlr->ctrlr);
		free(ctrlr->bdevs);
		free(ctrlr->name);
		free(ctrlr);
		return -ENOMEM;
	}

	spdk_io_device_register(ctrlr->ctrlr, bdev_ocssd_io_channel_create_cb,
				bdev_ocssd_io_channel_destroy_cb,
				sizeof(struct bdev_ocssd_io_channel),
				name);

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	TAILQ_INSERT_HEAD(&g_nvme_bdev_ctrlrs, ctrlr, tailq);
	pthread_mutex_unlock(&g_bdev_nvme_mutex);

	*_ctrlr = ctrlr;

	return 0;
}

static void
bdev_ocssd_unregister_cb(void *io_device)
{
	struct spdk_nvme_ctrlr *ctrlr = io_device;

	spdk_nvme_detach(ctrlr);
}

static void
bdev_ocssd_free_ctrlr(struct nvme_bdev_ctrlr *ctrlr)
{
	assert(ctrlr->ref == 0);

	spdk_io_device_unregister(ctrlr->ctrlr, bdev_ocssd_unregister_cb);
	spdk_poller_unregister(&ctrlr->adminq_timer_poller);

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	TAILQ_REMOVE(&g_nvme_bdev_ctrlrs, ctrlr, tailq);
	pthread_mutex_unlock(&g_bdev_nvme_mutex);

	free(ctrlr->bdevs);
	free(ctrlr->name);
	free(ctrlr);
}

static void
bdev_ocssd_free_bdev(struct nvme_bdev *bdev)
{
	free(bdev->disk.name);
	free(bdev->ocssd_bdev);
	memset(bdev, 0, sizeof(*bdev));
}

static int
bdev_ocssd_destruct(void *ctx)
{
	struct nvme_bdev *bdev = ctx;
	struct nvme_bdev_ctrlr *ctrlr = bdev->nvme_bdev_ctrlr;

	bdev_ocssd_free_bdev(bdev);

	if (--ctrlr->ref == 0) {
		bdev_ocssd_free_ctrlr(ctrlr);
	}

	return 0;
}

static void
bdev_ocssd_translate_lba(struct nvme_bdev *bdev, uint64_t lba, uint64_t *grp,
			 uint64_t *pu, uint64_t *chk, uint64_t *lbk)
{
	struct ocssd_bdev *ocssd_bdev = bdev->ocssd_bdev;
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
bdev_ocssd_to_disk_lba(struct nvme_bdev *bdev, uint64_t lba)
{
	struct ocssd_bdev *ocssd_bdev = bdev->ocssd_bdev;
	const struct bdev_ocssd_lba_offsets *offsets = &ocssd_bdev->lba_offsets;
	uint64_t lbk, chk, pu, grp;

	bdev_ocssd_translate_lba(bdev, lba, &grp, &pu, &chk, &lbk);

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
	struct nvme_bdev *bdev = bdev_io->bdev->ctxt;
	struct bdev_ocssd_io_channel *ocdev_ioch = spdk_io_channel_get_ctx(ioch);
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;
	const size_t zone_size = bdev->disk.zone_size;
	uint64_t lba;

	if ((bdev_io->u.bdev.offset_blocks % zone_size) + bdev_io->u.bdev.num_blocks > zone_size) {
		SPDK_ERRLOG("Tried to cross zone boundary during read command\n");
		return -EINVAL;
	}

	ocdev_io->iov_pos = 0;
	ocdev_io->iov_off = 0;

	lba = bdev_ocssd_to_disk_lba(bdev, bdev_io->u.bdev.offset_blocks);

	return spdk_nvme_ns_cmd_readv_with_md(bdev->ns, ocdev_ioch->qpair, lba,
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
	struct nvme_bdev *bdev = bdev_io->bdev->ctxt;
	struct bdev_ocssd_io_channel *ocdev_ioch = spdk_io_channel_get_ctx(ioch);
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;
	const size_t zone_size = bdev->disk.zone_size;
	uint64_t lba;

	if ((bdev_io->u.bdev.offset_blocks % zone_size) + bdev_io->u.bdev.num_blocks > zone_size) {
		SPDK_ERRLOG("Tried to cross zone boundary during write commnad\n");
		return -EINVAL;
	}

	ocdev_io->iov_pos = 0;
	ocdev_io->iov_off = 0;

	lba = bdev_ocssd_to_disk_lba(bdev, bdev_io->u.bdev.offset_blocks);

	return spdk_nvme_ns_cmd_writev_with_md(bdev->ns, ocdev_ioch->qpair, lba,
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
	struct nvme_bdev *bdev = bdev_io->bdev->ctxt;
	struct bdev_ocssd_io_channel *ocdev_ioch = spdk_io_channel_get_ctx(ioch);
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;
	size_t offset, zone_size = bdev->disk.zone_size;

	if (num_zones > SPDK_NVME_OCSSD_MAX_LBAL_ENTRIES) {
		SPDK_ERRLOG("Exceeded maximum number of zones per single reset: %d\n",
			    SPDK_NVME_OCSSD_MAX_LBAL_ENTRIES);
		return -EINVAL;
	}

	for (offset = 0; offset < num_zones; ++offset) {
		ocdev_io->lba[offset] = bdev_ocssd_to_disk_lba(bdev, slba + offset * zone_size);
	}

	return spdk_nvme_ocssd_ns_cmd_vector_reset(bdev->ns, ocdev_ioch->qpair, ocdev_io->lba,
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
bdev_ocssd_zone_info_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_bdev_io *bdev_io = ctx;
	struct nvme_bdev *bdev = bdev_io->bdev->ctxt;
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;
	struct spdk_ocssd_chunk_information_entry *chunk_info = &ocdev_io->chunk_info;
	struct spdk_bdev_zone_info *zone_info;
	int rc;

	if (spdk_unlikely(spdk_nvme_cpl_is_error(cpl))) {
		spdk_bdev_io_complete_nvme_status(bdev_io, cpl->status.sct, cpl->status.sc);
		return;
	}

	zone_info = ((struct spdk_bdev_zone_info *)bdev_io->u.zdev.buf) + ocdev_io->chunk_offset;
	zone_info->zone_id = bdev_io->u.zdev.zone_id + ocdev_io->chunk_offset *
			     bdev->disk.zone_size;
	zone_info->write_pointer = zone_info->zone_id;

	if (chunk_info->cs.free) {
		zone_info->state = SPDK_BDEV_ZONE_STATE_EMPTY;
	} else if (chunk_info->cs.closed) {
		zone_info->state = SPDK_BDEV_ZONE_STATE_FULL;
	} else if (chunk_info->cs.open) {
		zone_info->state = SPDK_BDEV_ZONE_STATE_OPEN;
		zone_info->write_pointer += chunk_info->wp % bdev->disk.zone_size;
	} else if (chunk_info->cs.offline) {
		zone_info->state = SPDK_BDEV_ZONE_STATE_OFFLINE;
	} else {
		SPDK_ERRLOG("Unknown chunk state, assuming offline\n");
		zone_info->state = SPDK_BDEV_ZONE_STATE_OFFLINE;
	}

	if (chunk_info->ct.size_deviate) {
		zone_info->capacity = chunk_info->cnlb;
	} else {
		zone_info->capacity = bdev->disk.zone_size;
	}

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
	struct nvme_bdev *bdev = bdev_io->bdev->ctxt;
	const struct spdk_ocssd_geometry_data *geo = &bdev->ocssd_bdev->geometry;
	struct bdev_ocssd_io *ocdev_io = (struct bdev_ocssd_io *)bdev_io->driver_ctx;
	uint64_t lba, grp, pu, chk, lbk, offset;

	lba = bdev_io->u.zdev.zone_id + ocdev_io->chunk_offset * bdev->disk.zone_size;
	bdev_ocssd_translate_lba(bdev, lba, &grp, &pu, &chk, &lbk);
	offset = grp * geo->num_pu * geo->num_chk + pu * geo->num_chk + chk;

	return spdk_nvme_ctrlr_cmd_get_log_page(bdev->nvme_bdev_ctrlr->ctrlr,
						SPDK_OCSSD_LOG_CHUNK_INFO,
						spdk_nvme_ns_get_id(bdev->ns),
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
	struct nvme_bdev *bdev = ctx;

	return spdk_get_io_channel(bdev->nvme_bdev_ctrlr->ctrlr);
}

static struct spdk_bdev_fn_table ocssdlib_fn_table = {
	.destruct		= bdev_ocssd_destruct,
	.submit_request		= bdev_ocssd_submit_request,
	.io_type_supported	= bdev_ocssd_io_type_supported,
	.get_io_channel		= bdev_ocssd_get_io_channel,
};

struct bdev_ocssd_attach_ctx {
	size_t				*num_bdevs;
	const char			**bdev_names;
	size_t				max_bdevs;
	size_t				num_done;
	spdk_bdev_ocssd_attach_cb	cb_fn;
	void				*cb_ctx;
};

struct bdev_ocssd_create_ctx {
	struct bdev_ocssd_attach_ctx	*attach_ctx;
	struct nvme_bdev		*bdev;
};

static void
bdev_ocssd_geometry_cb(void *_ctx, const struct spdk_nvme_cpl *cpl)
{
	struct bdev_ocssd_create_ctx *create_ctx = _ctx;
	struct bdev_ocssd_attach_ctx *attach_ctx = create_ctx->attach_ctx;
	struct nvme_bdev *nvme_bdev = create_ctx->bdev;
	struct ocssd_bdev *ocssd_bdev = nvme_bdev->ocssd_bdev;
	const struct spdk_ocssd_geometry_data *geometry = &ocssd_bdev->geometry;
	struct nvme_bdev_ctrlr *ctrlr = nvme_bdev->nvme_bdev_ctrlr;
	struct spdk_bdev *bdev = &nvme_bdev->disk;
	int rc;

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_ERRLOG("Failed to retrieve controller's geometry\n");
		bdev_ocssd_free_bdev(nvme_bdev);
	} else {
		ocssd_bdev->lba_offsets.lbk = 0;
		ocssd_bdev->lba_offsets.chk = ocssd_bdev->lba_offsets.lbk +
					      geometry->lbaf.lbk_len;
		ocssd_bdev->lba_offsets.pu  = ocssd_bdev->lba_offsets.chk +
					      geometry->lbaf.chk_len;
		ocssd_bdev->lba_offsets.grp = ocssd_bdev->lba_offsets.pu +
					      geometry->lbaf.pu_len;

		bdev->blockcnt = geometry->num_grp * geometry->num_pu *
				 geometry->num_chk * geometry->clba;
		bdev->zone_size = geometry->clba;
		bdev->max_open_zones = geometry->maxoc;
		bdev->optimal_open_zones = geometry->num_grp * geometry->num_pu;
		bdev->write_unit_size = geometry->ws_opt;
		bdev->md_interleave = false;
		bdev->md_len = 8;

		if (geometry->maxocpu != 0 && geometry->maxocpu != geometry->maxoc) {
			SPDK_WARNLOG("Maximum open chunks per PU is not zero. Reducing the maximum "
				     "number of open zones: %"PRIu32" -> %"PRIu32"\n",
				     geometry->maxoc, geometry->maxocpu);
			bdev->max_open_zones = geometry->maxocpu;
		}

		rc = spdk_bdev_register(bdev);
		if (spdk_unlikely(rc != 0)) {
			SPDK_ERRLOG("Failed to register bdev %s\n", bdev->name);
			bdev_ocssd_free_bdev(nvme_bdev);
		} else {
			if (*attach_ctx->num_bdevs < attach_ctx->max_bdevs) {
				attach_ctx->bdev_names[*attach_ctx->num_bdevs] = bdev->name;
			} else {
				SPDK_ERRLOG("Reached maximum number of namespaces per create call"
					    "(%zu). Unable to return the name of bdev %s\n",
					    attach_ctx->max_bdevs, bdev->name);
			}

			(*attach_ctx->num_bdevs)++;
			ctrlr->ref++;
		}
	}

	if (++attach_ctx->num_done == spdk_nvme_ctrlr_get_num_ns(ctrlr->ctrlr)) {
		attach_ctx->cb_fn(attach_ctx->cb_ctx);
		free(attach_ctx);
	}

	free(create_ctx);
}

static int
bdev_ocssd_create_bdev(struct nvme_bdev_ctrlr *ctrlr,
		       struct bdev_ocssd_attach_ctx *attach_ctx,
		       uint32_t nsid)
{
	struct nvme_bdev *nvme_bdev = &ctrlr->bdevs[nsid - 1];
	struct bdev_ocssd_create_ctx *create_ctx;
	struct spdk_bdev *bdev = &nvme_bdev->disk;
	struct ocssd_bdev *ocssd_bdev;
	int rc;

	nvme_bdev->ns = spdk_nvme_ctrlr_get_ns(ctrlr->ctrlr, nsid);
	if (!nvme_bdev->ns) {
		SPDK_ERRLOG("Unable to retrieve namespace %u", nsid);
		bdev_ocssd_free_bdev(nvme_bdev);
		return -ENODEV;
	}

	create_ctx = calloc(1, sizeof(*create_ctx));
	if (!create_ctx) {
		bdev_ocssd_free_bdev(nvme_bdev);
		return -ENOMEM;
	}

	ocssd_bdev = nvme_bdev->ocssd_bdev = calloc(1, sizeof(*ocssd_bdev));
	if (!ocssd_bdev) {
		bdev_ocssd_free_bdev(nvme_bdev);
		free(create_ctx);
		return -ENOMEM;
	}

	bdev->name = spdk_sprintf_alloc("%sn%"PRIu32, ctrlr->name, nsid);
	if (!bdev->name) {
		bdev_ocssd_free_bdev(nvme_bdev);
		free(create_ctx);
		return -ENOMEM;
	}

	create_ctx->attach_ctx = attach_ctx;
	create_ctx->bdev = nvme_bdev;

	nvme_bdev->nvme_bdev_ctrlr = ctrlr;
	bdev->product_name = "Open Channel SSD";
	bdev->ctxt = nvme_bdev;
	bdev->fn_table = &ocssdlib_fn_table;
	bdev->module = &ocssd_if;
	bdev->blocklen = spdk_nvme_ns_get_extended_sector_size(nvme_bdev->ns);
	bdev->zoned = true;

	rc = spdk_nvme_ocssd_ctrlr_cmd_geometry(ctrlr->ctrlr, nsid, &ocssd_bdev->geometry,
						sizeof(ocssd_bdev->geometry),
						bdev_ocssd_geometry_cb, create_ctx);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Failed to retrieve controllers geometry: %s\n", spdk_strerror(-rc));
		bdev_ocssd_free_bdev(nvme_bdev);
		free(create_ctx);
		return rc;
	}

	return 0;
}

int
spdk_bdev_ocssd_attach_controller(const struct spdk_nvme_transport_id *trid, const char *base_name,
				  const char **names, size_t *count,
				  spdk_bdev_ocssd_attach_cb cb_fn, void *cb_ctx)
{
	struct nvme_bdev_ctrlr *ctrlr;
	struct bdev_ocssd_attach_ctx *attach_ctx;
	uint32_t nsid, ns_count;
	int rc;

	attach_ctx = calloc(1, sizeof(*attach_ctx));
	if (!attach_ctx) {
		return -ENOMEM;
	}

	rc = bdev_ocssd_create_ctrlr(trid, base_name, &ctrlr);
	if (spdk_unlikely(rc != 0)) {
		free(attach_ctx);
		return rc;
	}

	attach_ctx->cb_fn = cb_fn;
	attach_ctx->cb_ctx = cb_ctx;
	attach_ctx->bdev_names = names;
	attach_ctx->max_bdevs = *count;
	attach_ctx->num_bdevs = count;
	*count = 0;

	ns_count = spdk_nvme_ctrlr_get_num_ns(ctrlr->ctrlr);
	/* Necessary to stop scan-build from complaining about attach_ctx memory leak. */
	assert(ns_count > 0);

	for (nsid = 1; nsid <= ns_count; ++nsid) {
		rc = bdev_ocssd_create_bdev(ctrlr, attach_ctx, nsid);
		if (spdk_unlikely(rc != 0)) {
			SPDK_ERRLOG("Failed to create OCSSD bdev for namespace %"PRIu32"\n", nsid);

			if (++attach_ctx->num_done == ns_count) {
				/* Necessary to get rid of scan-build's false complaints regarding
				 * use-after-free of the attach_ctx. */
				assert(nsid == ns_count);
				free(attach_ctx);
				cb_fn(cb_ctx);
			}
		}
	}

	return 0;
}

int
spdk_bdev_ocssd_detach_controller(const char *name)
{
	struct nvme_bdev_ctrlr *ctrlr;
	struct nvme_bdev *nvme_bdev;
	uint32_t nsid, ns_count;

	if (!name) {
		return -EINVAL;
	}

	ctrlr = nvme_bdev_ctrlr_get_by_name(name);
	if (!ctrlr) {
		SPDK_ERRLOG("Failed to find NVMe controller: %s\n", name);
		return -ENODEV;
	}

	ns_count = spdk_nvme_ctrlr_get_num_ns(ctrlr->ctrlr);
	for (nsid = 0; nsid < ns_count; ++nsid) {
		nvme_bdev = &ctrlr->bdevs[nsid];
		spdk_bdev_unregister(&nvme_bdev->disk, NULL, NULL);
	}

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT("bdev_ocssd", SPDK_LOG_BDEV_OCSSD)
