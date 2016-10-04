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

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/param.h>

#include <pthread.h>

#include <rte_config.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_lcore.h>
#include <rte_malloc.h>

#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/bdev.h"
#include "spdk/nvme.h"
#include "spdk/io_channel.h"

#include "bdev_module.h"

#define MAX_NVME_NAME_LENGTH 64

static void blockdev_nvme_get_spdk_running_config(FILE *fp);

struct nvme_device {
	/**
	 * points to pinned, physically contiguous memory region;
	 * contains 4KB IDENTIFY structure for controller which is
	 *  target for CONTROLLER IDENTIFY command during initialization
	 */
	struct spdk_nvme_ctrlr		*ctrlr;

	/** linked list pointer for device list */
	TAILQ_ENTRY(nvme_device)	tailq;

	int				id;
};

struct nvme_blockdev {
	struct spdk_bdev	disk;
	struct spdk_nvme_ctrlr	*ctrlr;
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

struct nvme_bdf_whitelist {
	uint16_t	domain;
	uint8_t		bus;
	uint8_t		dev;
	uint8_t		func;
	char		name[MAX_NVME_NAME_LENGTH];
};

#define NVME_MAX_BLOCKDEVS_PER_CONTROLLER 256
#define NVME_MAX_CONTROLLERS 16
#define NVME_MAX_BLOCKDEVS (NVME_MAX_BLOCKDEVS_PER_CONTROLLER * NVME_MAX_CONTROLLERS)
static struct nvme_blockdev g_blockdev[NVME_MAX_BLOCKDEVS];
static int blockdev_index_max = 0;
static int nvme_luns_per_ns = 1;
static int nvme_controller_index = 0;
static int LunSizeInMB = 0;
static int num_controllers = -1;

static TAILQ_HEAD(, nvme_device)	g_nvme_devices = TAILQ_HEAD_INITIALIZER(g_nvme_devices);;

static void nvme_ctrlr_initialize_blockdevs(struct spdk_nvme_ctrlr *ctrlr,
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

SPDK_BDEV_MODULE_REGISTER(nvme_library_init, NULL, blockdev_nvme_get_spdk_running_config,
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

static const struct spdk_bdev_fn_table nvmelib_fn_table = {
	.destruct		= blockdev_nvme_destruct,
	.submit_request		= blockdev_nvme_submit_request,
	.io_type_supported	= blockdev_nvme_io_type_supported,
	.get_io_channel		= blockdev_nvme_get_io_channel,
};

struct nvme_probe_ctx {
	int controllers_remaining;
	int num_whitelist_controllers;
	struct nvme_bdf_whitelist whitelist[NVME_MAX_CONTROLLERS];
};

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *pci_dev, struct spdk_nvme_ctrlr_opts *opts)
{
	struct nvme_probe_ctx *ctx = cb_ctx;
	uint16_t found_domain = spdk_pci_device_get_domain(pci_dev);
	uint8_t found_bus    = spdk_pci_device_get_bus(pci_dev);
	uint8_t found_dev    = spdk_pci_device_get_dev(pci_dev);
	uint8_t found_func   = spdk_pci_device_get_func(pci_dev);
	int i;
	bool claim_device = false;

	SPDK_NOTICELOG("Probing device %x:%x:%x.%x\n",
		       found_domain, found_bus, found_dev, found_func);

	if (ctx->controllers_remaining == 0) {
		return false;
	}

	if (ctx->num_whitelist_controllers == 0) {
		claim_device = true;
	} else {
		for (i = 0; i < NVME_MAX_CONTROLLERS; i++) {
			if (found_domain == ctx->whitelist[i].domain &&
			    found_bus == ctx->whitelist[i].bus &&
			    found_dev == ctx->whitelist[i].dev &&
			    found_func == ctx->whitelist[i].func) {
				claim_device = true;
				break;
			}
		}
	}

	if (!claim_device) {
		return false;
	}

	/* Claim the device in case conflict with other process */
	if (spdk_pci_device_claim(pci_dev) != 0) {
		return false;
	}

	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *pci_dev, struct spdk_nvme_ctrlr *ctrlr,
	  const struct spdk_nvme_ctrlr_opts *opts)
{
	struct nvme_probe_ctx *ctx = cb_ctx;
	struct nvme_device *dev;

	dev = malloc(sizeof(struct nvme_device));
	if (dev == NULL) {
		SPDK_ERRLOG("Failed to allocate device struct\n");
		return;
	}

	dev->ctrlr = ctrlr;
	dev->id = nvme_controller_index++;

	nvme_ctrlr_initialize_blockdevs(dev->ctrlr, nvme_luns_per_ns, dev->id);
	spdk_io_device_register(ctrlr, blockdev_nvme_create_cb, blockdev_nvme_destroy_cb,
				sizeof(struct nvme_io_channel));
	TAILQ_INSERT_TAIL(&g_nvme_devices, dev, tailq);

	if (ctx->controllers_remaining > 0) {
		ctx->controllers_remaining--;
	}
}


static int
nvme_library_init(void)
{
	struct spdk_conf_section *sp;
	const char *val;
	int i, rc;
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

	LunSizeInMB = spdk_conf_section_get_intval(sp, "LunSizeInMB");

	if (LunSizeInMB < 0)
		LunSizeInMB = 0;

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
			unsigned int domain, bus, dev, func;

			val = spdk_conf_section_get_nmval(sp, "BDF", i, 0);
			if (val == NULL) {
				break;
			}

			rc = sscanf(val, "%x:%x:%x.%x", &domain, &bus, &dev, &func);
			if (rc != 4) {
				SPDK_ERRLOG("Invalid format for BDF: %s\n", val);
				return -1;
			}

			probe_ctx.whitelist[probe_ctx.num_whitelist_controllers].domain = domain;
			probe_ctx.whitelist[probe_ctx.num_whitelist_controllers].bus = bus;
			probe_ctx.whitelist[probe_ctx.num_whitelist_controllers].dev = dev;
			probe_ctx.whitelist[probe_ctx.num_whitelist_controllers].func = func;

			val = spdk_conf_section_get_nmval(sp, "BDF", i, 1);
			if (val == NULL) {
				SPDK_ERRLOG("BDF section with no device name\n");
				return -1;
			}

			snprintf(probe_ctx.whitelist[probe_ctx.num_whitelist_controllers].name, MAX_NVME_NAME_LENGTH, "%s",
				 val);

			probe_ctx.num_whitelist_controllers++;
		}
	}

	probe_ctx.controllers_remaining = num_controllers;

	if (spdk_nvme_probe(&probe_ctx, probe_cb, attach_cb, NULL)) {
		return -1;
	}

	return 0;
}

__attribute__((destructor)) void
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

void
nvme_ctrlr_initialize_blockdevs(struct spdk_nvme_ctrlr *ctrlr, int bdev_per_ns, int ctrlr_id)
{
	struct nvme_blockdev	*bdev;
	struct spdk_nvme_ns	*ns;
	const struct spdk_nvme_ctrlr_data *cdata;
	uint64_t		bdev_size, lba_offset, sectors_per_stripe;
	int			ns_id, num_ns, bdev_idx;
	uint64_t LunSizeInsector;

	num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	for (ns_id = 1; ns_id <= num_ns; ns_id++) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, ns_id);
		bdev_size = spdk_nvme_ns_get_num_sectors(ns) / bdev_per_ns;

		/*
		 * Align each blockdev on a 1MB boundary - this helps cover Fultondale case
		 *  where I/O that span a 128KB boundary must be split for optimal performance.
		 *  Using a 1MB hardcoded boundary here so that we do not have to export
		 *  stripe size information from the NVMe driver for now.
		 */
		sectors_per_stripe = (1 << 20) / spdk_nvme_ns_get_sector_size(ns);

		LunSizeInsector = ((uint64_t)LunSizeInMB << 20) / spdk_nvme_ns_get_sector_size(ns);
		if ((LunSizeInMB > 0) && (LunSizeInsector < bdev_size))
			bdev_size = LunSizeInsector;

		bdev_size &= ~(sectors_per_stripe - 1);

		lba_offset = 0;
		for (bdev_idx = 0; bdev_idx < bdev_per_ns; bdev_idx++) {
			if (blockdev_index_max >= NVME_MAX_BLOCKDEVS)
				return;

			bdev = &g_blockdev[blockdev_index_max];
			bdev->ctrlr = ctrlr;
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
			bdev->disk.write_cache = 1;
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
	struct nvme_blockio *bio = ref;
	enum spdk_bdev_io_status status;

	if (spdk_nvme_cpl_is_error(cpl)) {
		status = SPDK_BDEV_IO_STATUS_FAILED;
	} else {
		status = SPDK_BDEV_IO_STATUS_SUCCESS;
	}

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bio), status);
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

static int
queued_next_sge(void *ref, uint64_t *address, uint32_t *length)
{
	struct nvme_blockio *bio = ref;
	struct iovec *iov;

	assert(bio->iovpos < bio->iovcnt);

	iov = &bio->iovs[bio->iovpos];
	bio->iovpos++;

	*address = spdk_vtophys(iov->iov_base);
	*length = iov->iov_len;

	if (bio->iov_offset) {
		assert(bio->iov_offset <= iov->iov_len);
		*address += bio->iov_offset;
		*length -= bio->iov_offset;
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
	if (LunSizeInMB != 0) {
		fprintf(fp, "  LunSizeInMB %d\n", LunSizeInMB);
	}
}

SPDK_LOG_REGISTER_TRACE_FLAG("bdev_nvme", SPDK_TRACE_BDEV_NVME)
