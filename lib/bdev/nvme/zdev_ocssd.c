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
#include "spdk/zdev_module.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/nvme_ocssd.h"
#include "spdk/nvme_ocssd_spec.h"
#include "spdk_internal/log.h"
#include "spdk/nvme.h"
#include "spdk_internal/log.h"
#include "common.h"
#include "zdev_ocssd.h"

struct zdev_ocssd_lba_offsets {
	uint32_t	grp;
	uint32_t	pu;
	uint32_t	chk;
	uint32_t	lbk;
};

struct zdev_ocssd_io_channel {
	struct spdk_nvme_qpair	*qpair;
	struct spdk_poller	*poller;
};

struct zdev_ocssd_io {
	size_t	iov_pos;
	size_t	iov_off;
};

struct nvme_bdev {
	struct spdk_zdev		disk;
	struct spdk_nvme_ns		*ns;
	struct nvme_bdev_ctrlr		*ctrlr;
	struct spdk_ocssd_geometry_data	geometry;
	struct zdev_ocssd_lba_offsets	lba_offsets;
};

static int
zdev_ocssd_library_init(void)
{
	return 0;
}

static void
zdev_ocssd_library_fini(void)
{
}

static int
zdev_ocssd_config_json(struct spdk_json_write_ctx *w)
{
	return 0;
}

static int
zdev_ocssd_get_ctx_size(void)
{
	return sizeof(struct zdev_ocssd_io);
}

static struct spdk_bdev_module ocssd_if = {
	.name = "ocssd",
	.module_init = zdev_ocssd_library_init,
	.module_fini = zdev_ocssd_library_fini,
	.config_json = zdev_ocssd_config_json,
	.get_ctx_size = zdev_ocssd_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(ocssd, &ocssd_if);

static int
zdev_ocssd_poll_ioq(void *ctx)
{
	struct zdev_ocssd_io_channel *ioch = ctx;

	return spdk_nvme_qpair_process_completions(ioch->qpair, 0);
}

static int
zdev_ocssd_io_channel_create_cb(void *io_device, void *ctx_buf)
{
	struct spdk_nvme_ctrlr *ctrlr = io_device;
	struct zdev_ocssd_io_channel *ioch = ctx_buf;

	ioch->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
	if (!ioch->qpair) {
		SPDK_ERRLOG("Failed to alloc IO queue pair\n");
		return -ENOMEM;
	}

	ioch->poller = spdk_poller_register(zdev_ocssd_poll_ioq, ioch, 0);
	if (!ioch->poller) {
		SPDK_ERRLOG("Failed to register IO queue poller\n");
		spdk_nvme_ctrlr_free_io_qpair(ioch->qpair);
		return -ENOMEM;
	}

	return 0;
}

static void
zdev_ocssd_io_channel_destroy_cb(void *io_device, void *ctx_buf)
{
	struct zdev_ocssd_io_channel *ioch = ctx_buf;

	spdk_nvme_ctrlr_free_io_qpair(ioch->qpair);
	spdk_poller_unregister(&ioch->poller);
}

static int
zdev_ocssd_poll_adminq(void *ctx)
{
	struct nvme_bdev_ctrlr *ctrlr = ctx;

	return spdk_nvme_ctrlr_process_admin_completions(ctrlr->ctrlr);
}

static int
zdev_ocssd_create_ctrlr(const struct spdk_nvme_transport_id *trid, const char *name,
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
	ctrlr->adminq_timer_poller = spdk_poller_register(zdev_ocssd_poll_adminq, ctrlr,
				     1000000ULL);
	if (!ctrlr->adminq_timer_poller) {
		spdk_nvme_detach(ctrlr->ctrlr);
		free(ctrlr->name);
		free(ctrlr);
		return -ENOMEM;
	}

	spdk_io_device_register(ctrlr->ctrlr, zdev_ocssd_io_channel_create_cb,
				zdev_ocssd_io_channel_destroy_cb,
				sizeof(struct zdev_ocssd_io_channel),
				name);

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	TAILQ_INSERT_HEAD(&g_nvme_bdev_ctrlrs, ctrlr, tailq);
	pthread_mutex_unlock(&g_bdev_nvme_mutex);

	*_ctrlr = ctrlr;

	return 0;
}

static void
zdev_ocssd_unregister_cb(void *io_device)
{
	struct spdk_nvme_ctrlr *ctrlr = io_device;

	spdk_nvme_detach(ctrlr);
}

static void
zdev_ocssd_free_ctrlr(struct nvme_bdev_ctrlr *ctrlr)
{
	assert(ctrlr->ref == 0);

	spdk_io_device_unregister(ctrlr->ctrlr, zdev_ocssd_unregister_cb);
	spdk_poller_unregister(&ctrlr->adminq_timer_poller);

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	TAILQ_REMOVE(&g_nvme_bdev_ctrlrs, ctrlr, tailq);
	pthread_mutex_unlock(&g_bdev_nvme_mutex);

	free(ctrlr->bdevs);
	free(ctrlr->name);
	free(ctrlr);
}

static void
zdev_ocssd_free_bdev(struct nvme_bdev *bdev)
{
	free(bdev->disk.bdev.name);
	memset(bdev, 0, sizeof(*bdev));
}

static int
zdev_ocssd_destruct(void *ctx)
{
	struct nvme_bdev *bdev = ctx;
	struct nvme_bdev_ctrlr *ctrlr = bdev->ctrlr;

	zdev_ocssd_free_bdev(bdev);

	if (--ctrlr->ref == 0) {
		zdev_ocssd_free_ctrlr(ctrlr);
	}

	return 0;
}

static uint64_t
zdev_ocssd_to_disk_lba(struct nvme_bdev *bdev, uint64_t lba)
{
	const struct spdk_ocssd_geometry_data *geo = &bdev->geometry;
	const struct zdev_ocssd_lba_offsets *offsets = &bdev->lba_offsets;
	uint64_t addr_shift, lbk, chk, pu, grp;

	/* To achieve best performance, we need to make sure that adjacent zones can be accessed
	 * in parallel.  We accomplish this by having the following addressing scheme:
	 *
	 * [            zone id              ][  zone offset  ] User's LBA
	 * [ chunk ][ parallel unit ][ group ][ logical block ] Open Channel's LBA
	 *
	 * which means that neighbouring zones are placed in a different group and parallel unit.
	 */
	lbk = lba % geo->clba;
	addr_shift = geo->clba;

	grp = (lba / addr_shift) % geo->num_grp;
	addr_shift *= geo->num_grp;

	pu = (lba / addr_shift) % geo->num_pu;
	addr_shift *= geo->num_pu;

	chk = (lba / addr_shift) % geo->num_chk;

	return (lbk << offsets->lbk) |
	       (chk << offsets->chk) |
	       (pu  << offsets->pu)  |
	       (grp << offsets->grp);
}

static void
zdev_ocssd_reset_sgl(void *cb_arg, uint32_t offset)
{
	struct spdk_bdev_io *bdev_io = cb_arg;
	struct zdev_ocssd_io *zdev_io = (struct zdev_ocssd_io *)bdev_io->driver_ctx;
	struct iovec *iov;

	zdev_io->iov_pos = 0;
	zdev_io->iov_off = 0;

	for (; zdev_io->iov_pos < (size_t)bdev_io->u.bdev.iovcnt; ++zdev_io->iov_pos) {
		iov = &bdev_io->u.bdev.iovs[zdev_io->iov_pos];
		if (offset < iov->iov_len) {
			zdev_io->iov_off = offset;
			return;
		}

		offset -= iov->iov_len;
	}

	assert(false && "Invalid offset length");
}

static int
zdev_ocssd_next_sge(void *cb_arg, void **address, uint32_t *length)
{
	struct spdk_bdev_io *bdev_io = cb_arg;
	struct zdev_ocssd_io *zdev_io = (struct zdev_ocssd_io *)bdev_io->driver_ctx;
	struct iovec *iov;

	assert(zdev_io->iov_pos < (size_t)bdev_io->u.bdev.iovcnt);
	iov = &bdev_io->u.bdev.iovs[zdev_io->iov_pos];

	*address = iov->iov_base;
	*length = iov->iov_len;

	if (zdev_io->iov_off != 0) {
		assert(zdev_io->iov_off < iov->iov_len);
		*address = (char *)*address + zdev_io->iov_off;
		*length -= zdev_io->iov_off;
	}

	assert(zdev_io->iov_off + *length == iov->iov_len);
	zdev_io->iov_off = 0;
	zdev_io->iov_pos++;

	return 0;
}

static void
zdev_ocssd_read_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_bdev_io *bdev_io = ctx;

	spdk_bdev_io_complete_nvme_status(bdev_io, cpl->status.sct, cpl->status.sc);
}

static int
zdev_ocssd_read(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io)
{
	struct nvme_bdev *bdev = bdev_io->bdev->ctxt;
	struct zdev_ocssd_io_channel *zdev_ioch = spdk_io_channel_get_ctx(ioch);
	struct zdev_ocssd_io *zdev_io = (struct zdev_ocssd_io *)bdev_io->driver_ctx;
	const size_t zone_size = bdev->disk.info.zone_size;
	uint64_t lba;

	if ((bdev_io->u.bdev.offset_blocks % zone_size) + bdev_io->u.bdev.num_blocks > zone_size) {
		SPDK_ERRLOG("Zone boundary crossed during read\n");
		return -EINVAL;
	}

	zdev_io->iov_pos = 0;
	zdev_io->iov_off = 0;

	lba = zdev_ocssd_to_disk_lba(bdev, bdev_io->u.bdev.offset_blocks);

	return spdk_nvme_ns_cmd_readv_with_md(bdev->ns, zdev_ioch->qpair, lba,
					      bdev_io->u.bdev.num_blocks, zdev_ocssd_read_cb,
					      bdev_io, 0, zdev_ocssd_reset_sgl,
					      zdev_ocssd_next_sge, bdev_io->u.bdev.md_buf, 0, 0);
}

static void
zdev_ocssd_io_get_buf_cb(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io, bool success)
{
	int rc;

	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		return;
	}

	rc = zdev_ocssd_read(ioch, bdev_io);
	if (spdk_likely(rc != 0)) {
		if (rc == -ENOMEM) {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		} else {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

static void
zdev_ocssd_submit_request(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io)
{
	int rc = 0;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, zdev_ocssd_io_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
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
zdev_ocssd_io_type_supported(void *ctx, enum spdk_bdev_io_type type)
{
	switch (type) {
	case SPDK_BDEV_IO_TYPE_READ:
		return true;

	default:
		return false;
	}
}

static struct spdk_io_channel *
zdev_ocssd_get_io_channel(void *ctx)
{
	struct nvme_bdev *bdev = ctx;

	return spdk_get_io_channel(bdev->ctrlr->ctrlr);
}

static struct spdk_bdev_fn_table ocssdlib_fn_table = {
	.destruct		= zdev_ocssd_destruct,
	.submit_request		= zdev_ocssd_submit_request,
	.io_type_supported	= zdev_ocssd_io_type_supported,
	.get_io_channel		= zdev_ocssd_get_io_channel,
};

struct zdev_ocssd_attach_ctx {
	size_t				*num_bdevs;
	const char			**bdev_names;
	size_t				max_bdevs;
	size_t				num_done;
	spdk_zdev_ocssd_attach_cb	cb_fn;
	void				*cb_ctx;
};

struct zdev_ocssd_create_ctx {
	struct zdev_ocssd_attach_ctx	*attach_ctx;
	struct nvme_bdev		*bdev;
};

static void
zdev_ocssd_geometry_cb(void *_ctx, const struct spdk_nvme_cpl *cpl)
{
	struct zdev_ocssd_create_ctx *create_ctx = _ctx;
	struct zdev_ocssd_attach_ctx *attach_ctx = create_ctx->attach_ctx;
	struct nvme_bdev *nvme_bdev = create_ctx->bdev;
	struct nvme_bdev_ctrlr *ctrlr = nvme_bdev->ctrlr;
	struct spdk_zdev *zdev = &nvme_bdev->disk;
	int rc;

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_ERRLOG("Failed to retrieve controller's geometry\n");
		zdev_ocssd_free_bdev(nvme_bdev);
	} else {
		nvme_bdev->lba_offsets.lbk = 0;
		nvme_bdev->lba_offsets.chk = nvme_bdev->lba_offsets.lbk +
					     nvme_bdev->geometry.lbaf.lbk_len;
		nvme_bdev->lba_offsets.pu  = nvme_bdev->lba_offsets.chk +
					     nvme_bdev->geometry.lbaf.chk_len;
		nvme_bdev->lba_offsets.grp = nvme_bdev->lba_offsets.pu +
					     nvme_bdev->geometry.lbaf.pu_len;

		zdev->bdev.blockcnt = nvme_bdev->geometry.num_grp *
				      nvme_bdev->geometry.num_pu *
				      nvme_bdev->geometry.num_chk *
				      nvme_bdev->geometry.clba;

		zdev->info.zone_size = nvme_bdev->geometry.clba;
		zdev->info.max_open_zones = nvme_bdev->geometry.maxoc;
		zdev->info.optimal_open_zones = nvme_bdev->geometry.num_grp *
						nvme_bdev->geometry.num_pu;

		rc = spdk_bdev_register(&zdev->bdev);
		if (spdk_unlikely(rc != 0)) {
			SPDK_ERRLOG("Failed to register bdev %s\n", zdev->bdev.name);
			zdev_ocssd_free_bdev(nvme_bdev);
		} else {
			if (*attach_ctx->num_bdevs < attach_ctx->max_bdevs) {
				attach_ctx->bdev_names[*attach_ctx->num_bdevs] = zdev->bdev.name;
			} else {
				SPDK_ERRLOG("Reached maximum number of namespaces per create call"
					    "(%zu). Unable to return the name of bdev %s\n",
					    attach_ctx->max_bdevs, zdev->bdev.name);
			}

			(*attach_ctx->num_bdevs)++;
			ctrlr->ref++;
		}
	}

	if (++attach_ctx->num_done == spdk_nvme_ctrlr_get_num_ns(ctrlr->ctrlr)) {
		if (*attach_ctx->num_bdevs == 0) {
			SPDK_ERRLOG("No bdevs could be created for ctrlr %s\n", ctrlr->name);
			zdev_ocssd_free_ctrlr(ctrlr);
		}

		attach_ctx->cb_fn(attach_ctx->cb_ctx);
		free(attach_ctx);
	}

	free(create_ctx);
}

static int
zdev_ocssd_create_bdev(struct nvme_bdev_ctrlr *ctrlr,
		       struct zdev_ocssd_attach_ctx *attach_ctx,
		       uint32_t nsid)
{
	struct nvme_bdev *nvme_bdev = &ctrlr->bdevs[nsid - 1];
	struct zdev_ocssd_create_ctx *create_ctx;
	struct spdk_zdev *zdev = &nvme_bdev->disk;
	int rc;

	if (!spdk_nvme_ctrlr_is_ocssd_supported(ctrlr->ctrlr)) {
		SPDK_ERRLOG("Specified controller doesn't support Open Channel\n");
		return -EINVAL;
	}

	nvme_bdev->ns = spdk_nvme_ctrlr_get_ns(ctrlr->ctrlr, nsid);
	if (!nvme_bdev->ns) {
		SPDK_ERRLOG("Unable to retrieve namespace %u", nsid);
		return -ENODEV;
	}

	create_ctx = calloc(1, sizeof(*create_ctx));
	if (!create_ctx) {
		return -ENOMEM;
	}

	zdev->bdev.name = spdk_sprintf_alloc("%sn%"PRIu32, ctrlr->name, nsid);
	if (!zdev->bdev.name) {
		zdev_ocssd_free_bdev(nvme_bdev);
		free(create_ctx);
		return -ENOMEM;
	}

	create_ctx->attach_ctx = attach_ctx;
	create_ctx->bdev = nvme_bdev;

	nvme_bdev->ctrlr = ctrlr;
	zdev->bdev.product_name = "Open Channel SSD";
	zdev->bdev.ctxt = nvme_bdev;
	zdev->bdev.fn_table = &ocssdlib_fn_table;
	zdev->bdev.module = &ocssd_if;
	zdev->bdev.blocklen = spdk_nvme_ns_get_extended_sector_size(nvme_bdev->ns);
	zdev->bdev.is_zdev = true;

	rc = spdk_nvme_ocssd_ctrlr_cmd_geometry(ctrlr->ctrlr, nsid, &nvme_bdev->geometry,
						sizeof(nvme_bdev->geometry),
						zdev_ocssd_geometry_cb, create_ctx);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Failed to retrieve controllers geometry: %s\n", spdk_strerror(-rc));
		zdev_ocssd_free_bdev(nvme_bdev);
		free(create_ctx);
		return rc;
	}

	return 0;
}

int
spdk_zdev_ocssd_attach_controller(const struct spdk_nvme_transport_id *trid, const char *base_name,
				  const char **names, size_t *count,
				  spdk_zdev_ocssd_attach_cb cb_fn, void *cb_ctx)
{
	struct nvme_bdev_ctrlr *ctrlr;
	struct zdev_ocssd_attach_ctx *ctx;
	uint32_t nsid, num_created = 0;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	rc = zdev_ocssd_create_ctrlr(trid, base_name, &ctrlr);
	if (spdk_unlikely(rc != 0)) {
		free(ctx);
		return rc;
	}

	ctx->cb_fn = cb_fn;
	ctx->cb_ctx = cb_ctx;
	ctx->bdev_names = names;
	ctx->max_bdevs = *count;
	ctx->num_bdevs = count;
	*count = 0;

	for (nsid = 1; nsid <= spdk_nvme_ctrlr_get_num_ns(ctrlr->ctrlr); ++nsid) {
		rc = zdev_ocssd_create_bdev(ctrlr, ctx, nsid);
		if (spdk_unlikely(rc != 0)) {
			SPDK_ERRLOG("Failed to create OCSSD bdev for namespace %"PRIu32"\n", nsid);
			continue;
		}

		num_created++;
	}

	if (num_created == 0) {
		SPDK_ERRLOG("Couldn't create any bdevs on controller (traddr: %s)\n", trid->traddr);
		zdev_ocssd_free_ctrlr(ctrlr);
		free(ctx);
		return -ENODEV;
	}

	return 0;
}

int
spdk_zdev_ocssd_detach_controller(const char *name)
{
	struct nvme_bdev_ctrlr *ctrlr;
	struct nvme_bdev *nvme_bdev;
	uint32_t nsid;

	if (!name) {
		return -EINVAL;
	}

	ctrlr = nvme_bdev_ctrlr_get_by_name(name);
	if (!ctrlr) {
		SPDK_ERRLOG("Failed to find NVMe controller: %s\n", name);
		return -ENODEV;
	}

	for (nsid = 0; nsid < spdk_nvme_ctrlr_get_num_ns(ctrlr->ctrlr); ++nsid) {
		nvme_bdev = &ctrlr->bdevs[nsid];
		spdk_bdev_unregister(&nvme_bdev->disk.bdev, NULL, NULL);
	}

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT("zdev_ocssd", SPDK_LOG_ZDEV_OCSSD)
