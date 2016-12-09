/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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

#include "blockdev_nvme.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/param.h>

#include <pthread.h>

#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/bdev.h"
#include "spdk/json.h"
#include "spdk/nvme.h"
#include "spdk/io_channel.h"
#include "spdk/string.h"

#include "spdk_internal/log.h"

#include "bdev_module.h"

static void blockdev_nvme_get_spdk_running_config(FILE *fp);

struct nvme_device {
	/**
	 * points to pinned, physically contiguous memory region;
	 * contains 4KB IDENTIFY structure for controller which is
	 *  target for CONTROLLER IDENTIFY command during initialization
	 */
	struct spdk_nvme_ctrlr		*ctrlr;
	struct spdk_pci_addr		pci_addr;

	/** linked list pointer for device list */
	TAILQ_ENTRY(nvme_device)	tailq;

	int				id;
};

struct nvme_blockdev {
	struct spdk_bdev	disk;
	struct spdk_nvme_ctrlr	*ctrlr;
	struct nvme_device	*dev;
	struct spdk_nvme_ns	*ns;
	uint64_t		lba_start;
	uint64_t		lba_end;
	uint64_t		blocklen;
};

struct nvme_io_channel {
	struct spdk_nvme_qpair	*qpair;
	struct spdk_poller	*poller;
};

#define NVME_DEFAULT_MAX_UNMAP_BDESC_COUNT	1
struct nvme_blockio {
	/** array of iovecs to transfer. */
	struct iovec *iovs;

	/** Number of iovecs in iovs array. */
	int iovcnt;

	/** Current iovec position. */
	int iovpos;

	/** Offset in current iovec. */
	uint32_t iov_offset;
};

enum data_direction {
	BDEV_DISK_READ = 0,
	BDEV_DISK_WRITE = 1
};

static struct nvme_blockdev g_blockdev[NVME_MAX_BLOCKDEVS];
static int blockdev_index_max = 0;
static int nvme_luns_per_ns = 1;
static int nvme_controller_index = 0;
static int lun_size_in_mb = 0;
static int num_controllers = -1;
static int g_reset_controller_on_timeout = 0;
static int g_timeout = 0;

static TAILQ_HEAD(, nvme_device)	g_nvme_devices = TAILQ_HEAD_INITIALIZER(g_nvme_devices);;

static void nvme_ctrlr_initialize_blockdevs(struct nvme_device *nvme_dev,
		int bdev_per_ns, int ctrlr_id);
static int nvme_library_init(void);
static void nvme_library_fini(void);
static int nvme_queue_cmd(struct nvme_blockdev *bdev, struct spdk_nvme_qpair *qpair,
			  struct nvme_blockio *bio,
			  int direction, struct iovec *iov, int iovcnt, uint64_t nbytes,
			  uint64_t offset);

static int
nvme_get_ctx_size(void)
{
	return sizeof(struct nvme_blockio);
}

SPDK_BDEV_MODULE_REGISTER(nvme_library_init, nvme_library_fini,
			  blockdev_nvme_get_spdk_running_config,
			  nvme_get_ctx_size)

static int64_t
blockdev_nvme_readv(struct nvme_blockdev *nbdev, struct spdk_io_channel *ch,
		    struct nvme_blockio *bio,
		    struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);
	int64_t rc;

	SPDK_TRACELOG(SPDK_TRACE_BDEV_NVME, "read %lu bytes with offset %#lx\n",
		      nbytes, offset);

	rc = nvme_queue_cmd(nbdev, nvme_ch->qpair, bio, BDEV_DISK_READ,
			    iov, iovcnt, nbytes, offset);
	if (rc < 0)
		return -1;

	return nbytes;
}

static int64_t
blockdev_nvme_writev(struct nvme_blockdev *nbdev, struct spdk_io_channel *ch,
		     struct nvme_blockio *bio,
		     struct iovec *iov, int iovcnt, size_t len, uint64_t offset)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);
	int64_t rc;

	SPDK_TRACELOG(SPDK_TRACE_BDEV_NVME, "write %lu bytes with offset %#lx\n",
		      len, offset);

	rc = nvme_queue_cmd(nbdev, nvme_ch->qpair, bio, BDEV_DISK_WRITE,
			    iov, iovcnt, len, offset);
	if (rc < 0)
		return -1;

	return len;
}

static void
blockdev_nvme_poll(void *arg)
{
	struct spdk_nvme_qpair *qpair = arg;

	spdk_nvme_qpair_process_completions(qpair, 0);
}

static int
blockdev_nvme_destruct(struct spdk_bdev *bdev)
{
	return 0;
}

static int
blockdev_nvme_flush(struct nvme_blockdev *nbdev, struct nvme_blockio *bio,
		    uint64_t offset, uint64_t nbytes)
{
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bio), SPDK_BDEV_IO_STATUS_SUCCESS);

	return 0;
}

static int
blockdev_nvme_reset(struct nvme_blockdev *nbdev, struct nvme_blockio *bio)
{
	int rc;
	enum spdk_bdev_io_status status;

	status = SPDK_BDEV_IO_STATUS_SUCCESS;
	rc = spdk_nvme_ctrlr_reset(nbdev->ctrlr);
	if (rc != 0) {
		status = SPDK_BDEV_IO_STATUS_FAILED;
	}

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bio), status);
	return rc;
}

static int
blockdev_nvme_unmap(struct nvme_blockdev *nbdev, struct spdk_io_channel *ch,
		    struct nvme_blockio *bio,
		    struct spdk_scsi_unmap_bdesc *umap_d,
		    uint16_t bdesc_count);

static void blockdev_nvme_get_rbuf_cb(struct spdk_bdev_io *bdev_io)
{
	int ret;

	ret = blockdev_nvme_readv((struct nvme_blockdev *)bdev_io->ctx,
				  bdev_io->ch,
				  (struct nvme_blockio *)bdev_io->driver_ctx,
				  bdev_io->u.read.iovs,
				  bdev_io->u.read.iovcnt,
				  bdev_io->u.read.len,
				  bdev_io->u.read.offset);

	if (ret < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static int _blockdev_nvme_submit_request(struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_rbuf(bdev_io, blockdev_nvme_get_rbuf_cb);
		return 0;

	case SPDK_BDEV_IO_TYPE_WRITE:
		return blockdev_nvme_writev((struct nvme_blockdev *)bdev_io->ctx,
					    bdev_io->ch,
					    (struct nvme_blockio *)bdev_io->driver_ctx,
					    bdev_io->u.write.iovs,
					    bdev_io->u.write.iovcnt,
					    bdev_io->u.write.len,
					    bdev_io->u.write.offset);

	case SPDK_BDEV_IO_TYPE_UNMAP:
		return blockdev_nvme_unmap((struct nvme_blockdev *)bdev_io->ctx,
					   bdev_io->ch,
					   (struct nvme_blockio *)bdev_io->driver_ctx,
					   bdev_io->u.unmap.unmap_bdesc,
					   bdev_io->u.unmap.bdesc_count);

	case SPDK_BDEV_IO_TYPE_RESET:
		return blockdev_nvme_reset((struct nvme_blockdev *)bdev_io->ctx,
					   (struct nvme_blockio *)bdev_io->driver_ctx);

	case SPDK_BDEV_IO_TYPE_FLUSH:
		return blockdev_nvme_flush((struct nvme_blockdev *)bdev_io->ctx,
					   (struct nvme_blockio *)bdev_io->driver_ctx,
					   bdev_io->u.flush.offset,
					   bdev_io->u.flush.length);

	default:
		return -1;
	}
	return 0;
}

static void blockdev_nvme_submit_request(struct spdk_bdev_io *bdev_io)
{
	if (_blockdev_nvme_submit_request(bdev_io) < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
blockdev_nvme_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type)
{
	struct nvme_blockdev *nbdev = (struct nvme_blockdev *)bdev;
	const struct spdk_nvme_ctrlr_data *cdata;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		return true;

	case SPDK_BDEV_IO_TYPE_UNMAP:
		cdata = spdk_nvme_ctrlr_get_data(nbdev->ctrlr);
		return cdata->oncs.dsm;

	default:
		return false;
	}
}

static int
blockdev_nvme_create_cb(void *io_device, uint32_t priority, void *ctx_buf, void *unique_ctx)
{
	struct spdk_nvme_ctrlr *ctrlr = io_device;
	struct nvme_io_channel *ch = ctx_buf;

	ch->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, 0);

	if (ch->qpair == NULL) {
		return -1;
	}

	spdk_poller_register(&ch->poller, blockdev_nvme_poll, ch->qpair,
			     spdk_app_get_current_core(), NULL, 0);
	return 0;
}

static void
blockdev_nvme_destroy_cb(void *io_device, void *ctx_buf)
{
	struct nvme_io_channel *ch = ctx_buf;

	spdk_nvme_ctrlr_free_io_qpair(ch->qpair);
	spdk_poller_unregister(&ch->poller, NULL);
}

static struct spdk_io_channel *
blockdev_nvme_get_io_channel(struct spdk_bdev *bdev, uint32_t priority)
{
	struct nvme_blockdev *nvme_bdev = (struct nvme_blockdev *)bdev;

	return spdk_get_io_channel(nvme_bdev->ctrlr, priority, false, NULL);
}

static int
blockdev_nvme_dump_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct nvme_blockdev *nvme_bdev = (struct nvme_blockdev *)bdev;
	struct nvme_device *nvme_dev = nvme_bdev->dev;
	const struct spdk_nvme_ctrlr_data *cdata;
	struct spdk_nvme_ns *ns;
	union spdk_nvme_vs_register vs;
	union spdk_nvme_csts_register csts;
	char buf[128];

	cdata = spdk_nvme_ctrlr_get_data(nvme_bdev->ctrlr);
	vs = spdk_nvme_ctrlr_get_regs_vs(nvme_bdev->ctrlr);
	csts = spdk_nvme_ctrlr_get_regs_csts(nvme_bdev->ctrlr);
	ns = nvme_bdev->ns;

	spdk_json_write_name(w, "nvme");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "pci_address");
	spdk_json_write_string_fmt(w, "%04x:%02x:%02x.%x", nvme_dev->pci_addr.domain,
				   nvme_dev->pci_addr.bus, nvme_dev->pci_addr.dev,
				   nvme_dev->pci_addr.func);

	spdk_json_write_name(w, "ctrlr_data");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "vendor_id");
	spdk_json_write_string_fmt(w, "0x%04x", cdata->vid);

	snprintf(buf, sizeof(cdata->mn) + 1, "%s", cdata->mn);
	spdk_str_trim(buf);
	spdk_json_write_name(w, "model_number");
	spdk_json_write_string(w, buf);

	snprintf(buf, sizeof(cdata->sn) + 1, "%s", cdata->sn);
	spdk_str_trim(buf);
	spdk_json_write_name(w, "serial_number");
	spdk_json_write_string(w, buf);

	snprintf(buf, sizeof(cdata->fr) + 1, "%s", cdata->fr);
	spdk_str_trim(buf);
	spdk_json_write_name(w, "firmware_revision");
	spdk_json_write_string(w, buf);

	spdk_json_write_object_end(w);

	spdk_json_write_name(w, "vs");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "nvme_version");
	if (vs.bits.ter) {
		spdk_json_write_string_fmt(w, "%u.%u.%u", vs.bits.mjr, vs.bits.mnr, vs.bits.ter);
	} else {
		spdk_json_write_string_fmt(w, "%u.%u", vs.bits.mjr, vs.bits.mnr);
	}

	spdk_json_write_object_end(w);

	spdk_json_write_name(w, "csts");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "rdy");
	spdk_json_write_uint32(w, csts.bits.rdy);

	spdk_json_write_name(w, "cfs");
	spdk_json_write_uint32(w, csts.bits.cfs);

	spdk_json_write_object_end(w);

	spdk_json_write_name(w, "ns_data");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "id");
	spdk_json_write_uint32(w, spdk_nvme_ns_get_id(ns));

	spdk_json_write_name(w, "block_size");
	spdk_json_write_uint32(w, spdk_nvme_ns_get_sector_size(ns));

	spdk_json_write_name(w, "total_size");
	spdk_json_write_uint64(w, spdk_nvme_ns_get_size(ns));

	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);

	return 0;
}

static const struct spdk_bdev_fn_table nvmelib_fn_table = {
	.destruct		= blockdev_nvme_destruct,
	.submit_request		= blockdev_nvme_submit_request,
	.io_type_supported	= blockdev_nvme_io_type_supported,
	.get_io_channel		= blockdev_nvme_get_io_channel,
	.dump_config_json	= blockdev_nvme_dump_config_json,
};

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	struct nvme_probe_ctx *ctx = cb_ctx;
	int i;
	bool claim_device = false;
	struct spdk_pci_addr pci_addr;

	if (spdk_pci_addr_parse(&pci_addr, trid->traddr)) {
		return false;
	}

	SPDK_NOTICELOG("Probing device %s\n",
		       trid->traddr);

	if (ctx->controllers_remaining == 0) {
		return false;
	}

	if (ctx->num_whitelist_controllers == 0) {
		claim_device = true;
	} else {
		for (i = 0; i < NVME_MAX_CONTROLLERS; i++) {
			if (spdk_pci_addr_compare(&pci_addr, &ctx->whitelist[i]) == 0) {
				claim_device = true;
				break;
			}
		}
	}

	if (!claim_device) {
		return false;
	}

	/* Claim the device in case conflict with other process */
	if (spdk_pci_device_claim(&pci_addr) != 0) {
		return false;
	}

	return true;
}

static void
blockdev_nvme_timeout_cb(struct spdk_nvme_ctrlr *ctrlr,
			 struct spdk_nvme_qpair *qpair, void *cb_arg)
{
	int rc;

	SPDK_WARNLOG("Warning: Detected a timeout. ctrlr=%p qpair=%p\n", ctrlr, qpair);

	rc = spdk_nvme_ctrlr_reset(ctrlr);
	if (rc) {
		SPDK_ERRLOG("resetting controller failed\n");
	}
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct nvme_probe_ctx *ctx = cb_ctx;
	struct nvme_device *dev;

	dev = malloc(sizeof(struct nvme_device));
	if (dev == NULL) {
		SPDK_ERRLOG("Failed to allocate device struct\n");
		return;
	}

	dev->ctrlr = ctrlr;
	spdk_pci_addr_parse(&dev->pci_addr, trid->traddr);
	dev->id = nvme_controller_index++;

	nvme_ctrlr_initialize_blockdevs(dev, nvme_luns_per_ns, dev->id);
	spdk_io_device_register(ctrlr, blockdev_nvme_create_cb, blockdev_nvme_destroy_cb,
				sizeof(struct nvme_io_channel));
	TAILQ_INSERT_TAIL(&g_nvme_devices, dev, tailq);

	if (ctx->controllers_remaining > 0) {
		ctx->controllers_remaining--;
	}

	if (g_reset_controller_on_timeout) {
		spdk_nvme_ctrlr_register_timeout_callback(ctrlr, g_timeout,
				blockdev_nvme_timeout_cb, NULL);
	}
}

static bool
blockdev_nvme_exist(struct nvme_probe_ctx *ctx)
{
	int i;
	struct nvme_device *nvme_dev;

	for (i = 0; i < ctx->num_whitelist_controllers; i++) {
		TAILQ_FOREACH(nvme_dev, &g_nvme_devices, tailq) {
			if (spdk_pci_addr_compare(&nvme_dev->pci_addr, &ctx->whitelist[i]) == 0) {
				return true;
			}
		}
	}
	return false;
}

int
spdk_bdev_nvme_create(struct nvme_probe_ctx *ctx)
{
	int prev_index_max, i;

	if (blockdev_nvme_exist(ctx)) {
		return -1;
	}

	prev_index_max = blockdev_index_max;

	if (spdk_nvme_probe(NULL, ctx, probe_cb, attach_cb, NULL)) {
		return -1;
	}

	/*
	 * Report the new bdevs that were created in this call.
	 * There can be more than one bdev per NVMe controller since one bdev is created per namespace.
	 */
	ctx->num_created_bdevs = 0;
	for (i = prev_index_max; i < blockdev_index_max; i++) {
		ctx->created_bdevs[ctx->num_created_bdevs++] = &g_blockdev[i].disk;
	}

	return 0;
}

static int
nvme_library_init(void)
{
	struct spdk_conf_section *sp;
	const char *val;
	int i;
	struct nvme_probe_ctx probe_ctx;

	sp = spdk_conf_find_section(NULL, "Nvme");
	if (sp == NULL) {
		/*
		 * If configuration file did not specify the Nvme section, do
		 *  not take the time to initialize the NVMe devices.
		 */
		return 0;
	}

	nvme_luns_per_ns = spdk_conf_section_get_intval(sp, "NvmeLunsPerNs");
	if (nvme_luns_per_ns < 1)
		nvme_luns_per_ns = 1;

	if (nvme_luns_per_ns > NVME_MAX_BLOCKDEVS_PER_CONTROLLER) {
		SPDK_ERRLOG("The input value nvme_luns_per_ns(%d) exceeds the maximal "
			    "value(%d)\n", nvme_luns_per_ns, NVME_MAX_BLOCKDEVS_PER_CONTROLLER);
		return -1;
	}

	lun_size_in_mb = spdk_conf_section_get_intval(sp, "LunSizeInMB");

	if (lun_size_in_mb < 0)
		lun_size_in_mb = 0;

	spdk_nvme_retry_count = spdk_conf_section_get_intval(sp, "NvmeRetryCount");
	if (spdk_nvme_retry_count < 0)
		spdk_nvme_retry_count = SPDK_NVME_DEFAULT_RETRY_COUNT;

	/*
	 * If NumControllers is not found, this will return -1, which we
	 *  will later use to denote that we should initialize all
	 *  controllers.
	 */
	num_controllers = spdk_conf_section_get_intval(sp, "NumControllers");

	/* Init the whitelist */
	probe_ctx.num_whitelist_controllers = 0;

	if (num_controllers > 0) {
		for (i = 0; ; i++) {
			val = spdk_conf_section_get_nmval(sp, "BDF", i, 0);
			if (val == NULL) {
				break;
			}

			if (spdk_pci_addr_parse(&probe_ctx.whitelist[probe_ctx.num_whitelist_controllers], val) < 0) {
				SPDK_ERRLOG("Invalid format for BDF: %s\n", val);
				return -1;
			}

			probe_ctx.num_whitelist_controllers++;
		}
	}

	probe_ctx.controllers_remaining = num_controllers;

	val = spdk_conf_section_get_val(sp, "ResetControllerOnTimeout");
	if (val != NULL) {
		if (!strcmp(val, "Yes")) {
			g_reset_controller_on_timeout = 1;
		}
	}

	if ((g_timeout = spdk_conf_section_get_intval(sp, "NvmeTimeoutValue")) < 0) {
		g_timeout = 0;
	}

	return spdk_bdev_nvme_create(&probe_ctx);
}

static void
nvme_library_fini(void)
{
	struct nvme_device *dev;

	while (!TAILQ_EMPTY(&g_nvme_devices)) {
		dev = TAILQ_FIRST(&g_nvme_devices);
		TAILQ_REMOVE(&g_nvme_devices, dev, tailq);
		spdk_nvme_detach(dev->ctrlr);
		free(dev);
	}
}

static void
nvme_ctrlr_initialize_blockdevs(struct nvme_device *nvme_dev, int bdev_per_ns, int ctrlr_id)
{
	struct nvme_blockdev	*bdev;
	struct spdk_nvme_ctrlr	*ctrlr = nvme_dev->ctrlr;
	struct spdk_nvme_ns	*ns;
	const struct spdk_nvme_ctrlr_data *cdata;
	uint64_t		bdev_size, lba_offset, sectors_per_stripe;
	int			ns_id, num_ns, bdev_idx;
	uint64_t lun_size_in_sector;

	num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	for (ns_id = 1; ns_id <= num_ns; ns_id++) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, ns_id);

		if (!spdk_nvme_ns_is_active(ns)) {
			SPDK_TRACELOG(SPDK_TRACE_BDEV_NVME, "Skipping inactive NS %d\n", ns_id);
			continue;
		}

		bdev_size = spdk_nvme_ns_get_num_sectors(ns) / bdev_per_ns;

		/*
		 * Align each blockdev on a 1MB boundary - this helps cover Fultondale case
		 *  where I/O that span a 128KB boundary must be split for optimal performance.
		 *  Using a 1MB hardcoded boundary here so that we do not have to export
		 *  stripe size information from the NVMe driver for now.
		 */
		sectors_per_stripe = (1 << 20) / spdk_nvme_ns_get_sector_size(ns);

		lun_size_in_sector = ((uint64_t)lun_size_in_mb << 20) / spdk_nvme_ns_get_sector_size(ns);
		if ((lun_size_in_mb > 0) && (lun_size_in_sector < bdev_size))
			bdev_size = lun_size_in_sector;

		bdev_size &= ~(sectors_per_stripe - 1);

		lba_offset = 0;
		for (bdev_idx = 0; bdev_idx < bdev_per_ns; bdev_idx++) {
			if (blockdev_index_max >= NVME_MAX_BLOCKDEVS)
				return;

			bdev = &g_blockdev[blockdev_index_max];
			bdev->ctrlr = ctrlr;
			bdev->dev = nvme_dev;
			bdev->ns = ns;
			bdev->lba_start = lba_offset;
			bdev->lba_end = lba_offset + bdev_size - 1;
			lba_offset += bdev_size;

			snprintf(bdev->disk.name, SPDK_BDEV_MAX_NAME_LENGTH,
				 "Nvme%dn%dp%d", ctrlr_id, spdk_nvme_ns_get_id(ns), bdev_idx);
			snprintf(bdev->disk.product_name, SPDK_BDEV_MAX_PRODUCT_NAME_LENGTH,
				 "NVMe disk");

			if (cdata->oncs.dsm) {
				/*
				 * Enable the thin provisioning
				 * if nvme controller supports
				 * DataSet Management command.
				 */
				bdev->disk.thin_provisioning = 1;
				bdev->disk.max_unmap_bdesc_count =
					NVME_DEFAULT_MAX_UNMAP_BDESC_COUNT;
			}

			bdev->disk.write_cache = 0;
			if (cdata->vwc.present) {
				/* Enable if the Volatile Write Cache exists */
				bdev->disk.write_cache = 1;
			}
			bdev->blocklen = spdk_nvme_ns_get_sector_size(ns);
			bdev->disk.blocklen = bdev->blocklen;
			bdev->disk.blockcnt = bdev->lba_end - bdev->lba_start + 1;
			bdev->disk.ctxt = bdev;
			bdev->disk.fn_table = &nvmelib_fn_table;
			spdk_bdev_register(&bdev->disk);

			blockdev_index_max++;
		}
	}
}

static void
queued_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx((struct nvme_blockio *)ref);
	enum spdk_bdev_io_status status;

	if (spdk_nvme_cpl_is_error(cpl)) {
		bdev_io->error.nvme.sct = cpl->status.sct;
		bdev_io->error.nvme.sc = cpl->status.sc;
		status = SPDK_BDEV_IO_STATUS_NVME_ERROR;
	} else {
		status = SPDK_BDEV_IO_STATUS_SUCCESS;
	}

	spdk_bdev_io_complete(bdev_io, status);
}

static void
queued_reset_sgl(void *ref, uint32_t sgl_offset)
{
	struct nvme_blockio *bio = ref;
	struct iovec *iov;

	bio->iov_offset = sgl_offset;
	for (bio->iovpos = 0; bio->iovpos < bio->iovcnt; bio->iovpos++) {
		iov = &bio->iovs[bio->iovpos];
		if (bio->iov_offset < iov->iov_len)
			break;

		bio->iov_offset -= iov->iov_len;
	}
}

#define min(a, b) (((a)<(b))?(a):(b))

#define _2MB_OFFSET(ptr)	(((uintptr_t)ptr) &  (0x200000 - 1))

static int
queued_next_sge(void *ref, void **address, uint32_t *length)
{
	struct nvme_blockio *bio = ref;
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

	*length = min(*length, 0x200000 - _2MB_OFFSET(*address));

	bio->iov_offset += *length;
	if (bio->iov_offset == iov->iov_len) {
		bio->iovpos++;
		bio->iov_offset = 0;
	}

	return 0;
}

int
nvme_queue_cmd(struct nvme_blockdev *bdev, struct spdk_nvme_qpair *qpair,
	       struct nvme_blockio *bio,
	       int direction, struct iovec *iov, int iovcnt, uint64_t nbytes,
	       uint64_t offset)
{
	uint32_t ss = spdk_nvme_ns_get_sector_size(bdev->ns);
	uint32_t lba_count;
	uint64_t relative_lba = offset / bdev->blocklen;
	uint64_t next_lba = relative_lba + bdev->lba_start;
	int rc;

	if (nbytes % ss) {
		SPDK_ERRLOG("Unaligned IO request length\n");
		return -1;
	}


	lba_count = nbytes / ss;

	bio->iovs = iov;
	bio->iovcnt = iovcnt;
	bio->iovpos = 0;
	bio->iov_offset = 0;

	if (direction == BDEV_DISK_READ) {
		rc = spdk_nvme_ns_cmd_readv(bdev->ns, qpair, next_lba,
					    lba_count, queued_done, bio, 0,
					    queued_reset_sgl, queued_next_sge);
	} else {
		rc = spdk_nvme_ns_cmd_writev(bdev->ns, qpair, next_lba,
					     lba_count, queued_done, bio, 0,
					     queued_reset_sgl, queued_next_sge);
	}

	if (rc != 0) {
		SPDK_ERRLOG("IO failed\n");
	}
	return rc;
}

static int
blockdev_nvme_unmap(struct nvme_blockdev *nbdev, struct spdk_io_channel *ch,
		    struct nvme_blockio *bio,
		    struct spdk_scsi_unmap_bdesc *unmap_d,
		    uint16_t bdesc_count)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);
	int rc = 0, i;
	struct spdk_nvme_dsm_range dsm_range[NVME_DEFAULT_MAX_UNMAP_BDESC_COUNT];

	if (bdesc_count > NVME_DEFAULT_MAX_UNMAP_BDESC_COUNT) {
		return -1;
	}

	for (i = 0; i < bdesc_count; i++) {
		dsm_range[i].starting_lba = nbdev->lba_start + from_be64(&unmap_d->lba);
		dsm_range[i].length = from_be32(&unmap_d->block_count);
		dsm_range[i].attributes.raw = 0;
		unmap_d++;
	}

	rc = spdk_nvme_ns_cmd_dataset_management(nbdev->ns, nvme_ch->qpair,
			SPDK_NVME_DSM_ATTR_DEALLOCATE,
			dsm_range, bdesc_count,
			queued_done, bio);

	if (rc != 0)
		return -1;

	return 0;
}

static void
blockdev_nvme_get_spdk_running_config(FILE *fp)
{
	fprintf(fp,
		"\n"
		"# Users may change this to partition an NVMe namespace into multiple LUNs.\n"
		"[Nvme]\n"
		"  NvmeLunsPerNs %d\n",
		nvme_luns_per_ns);
	if (num_controllers != -1) {
		fprintf(fp, "  NumControllers %d\n", num_controllers);
	}
	if (lun_size_in_mb != 0) {
		fprintf(fp, "  LunSizeInMB %d\n", lun_size_in_mb);
	}
}

SPDK_LOG_REGISTER_TRACE_FLAG("bdev_nvme", SPDK_TRACE_BDEV_NVME)
