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

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

static void bdev_nvme_get_spdk_running_config(FILE *fp);

struct nvme_ctrlr {
	/**
	 * points to pinned, physically contiguous memory region;
	 * contains 4KB IDENTIFY structure for controller which is
	 *  target for CONTROLLER IDENTIFY command during initialization
	 */
	struct spdk_nvme_ctrlr		*ctrlr;
	struct spdk_nvme_transport_id	trid;
	const char			*name;
	int				ref;

	struct spdk_poller		*adminq_timer_poller;

	/** linked list pointer for device list */
	TAILQ_ENTRY(nvme_ctrlr)	tailq;
};

struct nvme_bdev {
	struct spdk_bdev	disk;
	struct nvme_ctrlr	*nvme_ctrlr;
	struct spdk_nvme_ns	*ns;

	TAILQ_ENTRY(nvme_bdev)	link;
};

struct nvme_io_channel {
	struct spdk_nvme_qpair	*qpair;
	struct spdk_poller	*poller;
};

#define NVME_DEFAULT_MAX_UNMAP_BDESC_COUNT	1
struct nvme_bdev_io {
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

struct nvme_probe_ctx {
	size_t count;
	struct spdk_nvme_transport_id trids[NVME_MAX_CONTROLLERS];
	const char *names[NVME_MAX_CONTROLLERS];
};

enum timeout_action {
	TIMEOUT_ACTION_NONE = 0,
	TIMEOUT_ACTION_RESET,
	TIMEOUT_ACTION_ABORT,
};

static int g_hot_insert_nvme_controller_index = 0;
static enum timeout_action g_action_on_timeout = TIMEOUT_ACTION_NONE;
static int g_timeout = 0;
static int g_nvme_adminq_poll_timeout_us = 0;
static bool g_nvme_hotplug_enabled;
static int g_nvme_hotplug_poll_timeout_us = 0;
static int g_nvme_hotplug_poll_core = 0;
static struct spdk_poller *g_hotplug_poller;
static pthread_mutex_t g_bdev_nvme_mutex = PTHREAD_MUTEX_INITIALIZER;

static TAILQ_HEAD(, nvme_ctrlr)	g_nvme_ctrlrs = TAILQ_HEAD_INITIALIZER(g_nvme_ctrlrs);
static TAILQ_HEAD(, nvme_bdev) g_nvme_bdevs = TAILQ_HEAD_INITIALIZER(g_nvme_bdevs);

static void nvme_ctrlr_create_bdevs(struct nvme_ctrlr *nvme_ctrlr);
static int bdev_nvme_library_init(void);
static void bdev_nvme_library_fini(void);
static int bdev_nvme_queue_cmd(struct nvme_bdev *bdev, struct spdk_nvme_qpair *qpair,
			       struct nvme_bdev_io *bio,
			       int direction, struct iovec *iov, int iovcnt, uint64_t nbytes,
			       uint64_t offset);

static int
bdev_nvme_get_ctx_size(void)
{
	return sizeof(struct nvme_bdev_io);
}

SPDK_BDEV_MODULE_REGISTER(bdev_nvme_library_init, bdev_nvme_library_fini,
			  bdev_nvme_get_spdk_running_config,
			  bdev_nvme_get_ctx_size)

static int64_t
bdev_nvme_readv(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
		struct nvme_bdev_io *bio,
		struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);
	int64_t rc;

	SPDK_TRACELOG(SPDK_TRACE_BDEV_NVME, "read %lu bytes with offset %#lx\n",
		      nbytes, offset);

	rc = bdev_nvme_queue_cmd(nbdev, nvme_ch->qpair, bio, BDEV_DISK_READ,
				 iov, iovcnt, nbytes, offset);
	if (rc < 0)
		return -1;

	return nbytes;
}

static int64_t
bdev_nvme_writev(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
		 struct nvme_bdev_io *bio,
		 struct iovec *iov, int iovcnt, size_t len, uint64_t offset)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);
	int64_t rc;

	SPDK_TRACELOG(SPDK_TRACE_BDEV_NVME, "write %lu bytes with offset %#lx\n",
		      len, offset);

	rc = bdev_nvme_queue_cmd(nbdev, nvme_ch->qpair, bio, BDEV_DISK_WRITE,
				 iov, iovcnt, len, offset);
	if (rc < 0)
		return -1;

	return len;
}

static void
bdev_nvme_poll(void *arg)
{
	struct spdk_nvme_qpair *qpair = arg;

	spdk_nvme_qpair_process_completions(qpair, 0);
}

static void
bdev_nvme_poll_adminq(void *arg)
{
	struct spdk_nvme_ctrlr *ctrlr = arg;

	spdk_nvme_ctrlr_process_admin_completions(ctrlr);
}

static int
bdev_nvme_destruct(void *ctx)
{
	struct nvme_bdev *nvme_disk = ctx;
	struct nvme_ctrlr *nvme_ctrlr = nvme_disk->nvme_ctrlr;

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	nvme_ctrlr->ref--;

	TAILQ_REMOVE(&g_nvme_bdevs, nvme_disk, link);
	free(nvme_disk);

	if (nvme_ctrlr->ref == 0) {
		TAILQ_REMOVE(&g_nvme_ctrlrs, nvme_ctrlr, tailq);
		pthread_mutex_unlock(&g_bdev_nvme_mutex);
		spdk_io_device_unregister(nvme_ctrlr->ctrlr);
		spdk_poller_unregister(&nvme_ctrlr->adminq_timer_poller, NULL);
		spdk_nvme_detach(nvme_ctrlr->ctrlr);
		free(nvme_ctrlr);
		return 0;
	}

	pthread_mutex_unlock(&g_bdev_nvme_mutex);
	return 0;

}

static int
bdev_nvme_flush(struct nvme_bdev *nbdev, struct nvme_bdev_io *bio,
		uint64_t offset, uint64_t nbytes)
{
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bio), SPDK_BDEV_IO_STATUS_SUCCESS);

	return 0;
}

static int
bdev_nvme_reset(struct nvme_bdev *nbdev, struct nvme_bdev_io *bio)
{
	int rc;
	enum spdk_bdev_io_status status;

	status = SPDK_BDEV_IO_STATUS_SUCCESS;
	rc = spdk_nvme_ctrlr_reset(nbdev->nvme_ctrlr->ctrlr);
	if (rc != 0) {
		status = SPDK_BDEV_IO_STATUS_FAILED;
	}

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bio), status);
	return rc;
}

static int
bdev_nvme_unmap(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
		struct nvme_bdev_io *bio,
		struct spdk_scsi_unmap_bdesc *umap_d,
		uint16_t bdesc_count);

static void
bdev_nvme_get_rbuf_cb(struct spdk_bdev_io *bdev_io)
{
	int ret;

	ret = bdev_nvme_readv((struct nvme_bdev *)bdev_io->ctx,
			      bdev_io->ch,
			      (struct nvme_bdev_io *)bdev_io->driver_ctx,
			      bdev_io->u.read.iovs,
			      bdev_io->u.read.iovcnt,
			      bdev_io->u.read.len,
			      bdev_io->u.read.offset);

	if (ret < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static int
_bdev_nvme_submit_request(struct spdk_bdev_io *bdev_io)
{
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_rbuf(bdev_io, bdev_nvme_get_rbuf_cb);
		return 0;

	case SPDK_BDEV_IO_TYPE_WRITE:
		return bdev_nvme_writev((struct nvme_bdev *)bdev_io->ctx,
					bdev_io->ch,
					(struct nvme_bdev_io *)bdev_io->driver_ctx,
					bdev_io->u.write.iovs,
					bdev_io->u.write.iovcnt,
					bdev_io->u.write.len,
					bdev_io->u.write.offset);

	case SPDK_BDEV_IO_TYPE_UNMAP:
		return bdev_nvme_unmap((struct nvme_bdev *)bdev_io->ctx,
				       bdev_io->ch,
				       (struct nvme_bdev_io *)bdev_io->driver_ctx,
				       bdev_io->u.unmap.unmap_bdesc,
				       bdev_io->u.unmap.bdesc_count);

	case SPDK_BDEV_IO_TYPE_RESET:
		return bdev_nvme_reset((struct nvme_bdev *)bdev_io->ctx,
				       (struct nvme_bdev_io *)bdev_io->driver_ctx);

	case SPDK_BDEV_IO_TYPE_FLUSH:
		return bdev_nvme_flush((struct nvme_bdev *)bdev_io->ctx,
				       (struct nvme_bdev_io *)bdev_io->driver_ctx,
				       bdev_io->u.flush.offset,
				       bdev_io->u.flush.length);

	default:
		return -1;
	}
	return 0;
}

static void
bdev_nvme_submit_request(struct spdk_bdev_io *bdev_io)
{
	if (_bdev_nvme_submit_request(bdev_io) < 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
bdev_nvme_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct nvme_bdev *nbdev = ctx;
	const struct spdk_nvme_ctrlr_data *cdata;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		return true;

	case SPDK_BDEV_IO_TYPE_UNMAP:
		cdata = spdk_nvme_ctrlr_get_data(nbdev->nvme_ctrlr->ctrlr);
		return cdata->oncs.dsm;

	default:
		return false;
	}
}

static int
bdev_nvme_create_cb(void *io_device, uint32_t priority, void *ctx_buf, void *unique_ctx)
{
	struct spdk_nvme_ctrlr *ctrlr = io_device;
	struct nvme_io_channel *ch = ctx_buf;

	ch->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, 0);

	if (ch->qpair == NULL) {
		return -1;
	}

	spdk_poller_register(&ch->poller, bdev_nvme_poll, ch->qpair,
			     spdk_env_get_current_core(), 0);
	return 0;
}

static void
bdev_nvme_destroy_cb(void *io_device, void *ctx_buf)
{
	struct nvme_io_channel *ch = ctx_buf;

	spdk_nvme_ctrlr_free_io_qpair(ch->qpair);
	spdk_poller_unregister(&ch->poller, NULL);
}

static struct spdk_io_channel *
bdev_nvme_get_io_channel(void *ctx, uint32_t priority)
{
	struct nvme_bdev *nvme_bdev = ctx;

	return spdk_get_io_channel(nvme_bdev->nvme_ctrlr->ctrlr, priority, false, NULL);
}

static int
bdev_nvme_dump_config_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct nvme_bdev *nvme_bdev = ctx;
	struct nvme_ctrlr *nvme_ctrlr = nvme_bdev->nvme_ctrlr;
	const struct spdk_nvme_ctrlr_data *cdata;
	struct spdk_nvme_ns *ns;
	union spdk_nvme_vs_register vs;
	union spdk_nvme_csts_register csts;
	char buf[128];

	cdata = spdk_nvme_ctrlr_get_data(nvme_bdev->nvme_ctrlr->ctrlr);
	vs = spdk_nvme_ctrlr_get_regs_vs(nvme_bdev->nvme_ctrlr->ctrlr);
	csts = spdk_nvme_ctrlr_get_regs_csts(nvme_bdev->nvme_ctrlr->ctrlr);
	ns = nvme_bdev->ns;

	spdk_json_write_name(w, "nvme");
	spdk_json_write_object_begin(w);

	if (nvme_ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
		spdk_json_write_name(w, "pci_address");
		spdk_json_write_string(w, nvme_ctrlr->trid.traddr);
	}

	spdk_json_write_name(w, "trid");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "trtype");
	if (nvme_ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
		spdk_json_write_string(w, "PCIe");
	} else if (nvme_ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_RDMA) {
		spdk_json_write_string(w, "RDMA");
	} else {
		spdk_json_write_string(w, "Unknown");
	}

	if (nvme_ctrlr->trid.traddr[0] != '\0') {
		spdk_json_write_name(w, "traddr");
		spdk_json_write_string(w, nvme_ctrlr->trid.traddr);
	}

	if (nvme_ctrlr->trid.trsvcid[0] != '\0') {
		spdk_json_write_name(w, "trsvcid");
		spdk_json_write_string(w, nvme_ctrlr->trid.trsvcid);
	}

	if (nvme_ctrlr->trid.subnqn[0] != '\0') {
		spdk_json_write_name(w, "subnqn");
		spdk_json_write_string(w, nvme_ctrlr->trid.subnqn);
	}

	spdk_json_write_object_end(w);

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

	spdk_json_write_name(w, "oacs");
	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "security");
	spdk_json_write_uint32(w, cdata->oacs.security);

	spdk_json_write_name(w, "format");
	spdk_json_write_uint32(w, cdata->oacs.format);

	spdk_json_write_name(w, "firmware");
	spdk_json_write_uint32(w, cdata->oacs.firmware);

	spdk_json_write_name(w, "ns_manage");
	spdk_json_write_uint32(w, cdata->oacs.ns_manage);

	spdk_json_write_object_end(w);

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

	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);

	return 0;
}

static const struct spdk_bdev_fn_table nvmelib_fn_table = {
	.destruct		= bdev_nvme_destruct,
	.submit_request		= bdev_nvme_submit_request,
	.io_type_supported	= bdev_nvme_io_type_supported,
	.get_io_channel		= bdev_nvme_get_io_channel,
	.dump_config_json	= bdev_nvme_dump_config_json,
};

static bool
hotplug_probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		 struct spdk_nvme_ctrlr_opts *opts)
{
	SPDK_TRACELOG(SPDK_TRACE_BDEV_NVME, "Attaching to %s\n", trid->traddr);

	return true;
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	struct nvme_probe_ctx *ctx = cb_ctx;
	size_t i;
	bool claim_device = false;

	SPDK_TRACELOG(SPDK_TRACE_BDEV_NVME, "Probing device %s\n", trid->traddr);

	for (i = 0; i < ctx->count; i++) {
		if (spdk_nvme_transport_id_compare(trid, &ctx->trids[i]) == 0) {
			claim_device = true;
			break;
		}
	}

	if (!claim_device) {
		SPDK_TRACELOG(SPDK_TRACE_BDEV_NVME, "Not claiming device at %s\n", trid->traddr);
		return false;
	}

	if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE) {
		struct spdk_pci_addr pci_addr;

		if (spdk_pci_addr_parse(&pci_addr, trid->traddr)) {
			return false;
		}

		if (spdk_pci_device_claim(&pci_addr) != 0) {
			return false;
		}
	}

	return true;
}

static void
spdk_nvme_abort_cpl(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;
	int rc;

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_WARNLOG("Abort failed. Resetting controller.\n");
		rc = spdk_nvme_ctrlr_reset(ctrlr);
		if (rc) {
			SPDK_ERRLOG("Resetting controller failed.\n");
		}
	}
}

static void
timeout_cb(void *cb_arg, struct spdk_nvme_ctrlr *ctrlr,
	   struct spdk_nvme_qpair *qpair, uint16_t cid)
{
	int rc;

	SPDK_WARNLOG("Warning: Detected a timeout. ctrlr=%p qpair=%p cid=%u\n", ctrlr, qpair, cid);

	switch (g_action_on_timeout) {
	case TIMEOUT_ACTION_ABORT:
		if (qpair) {
			rc = spdk_nvme_ctrlr_cmd_abort(ctrlr, qpair, cid,
						       spdk_nvme_abort_cpl, ctrlr);
			if (rc == 0) {
				return;
			}

			SPDK_ERRLOG("Unable to send abort. Resetting.\n");
		}

	/* Fallthrough */
	case TIMEOUT_ACTION_RESET:
		rc = spdk_nvme_ctrlr_reset(ctrlr);
		if (rc) {
			SPDK_ERRLOG("Resetting controller failed.\n");
		}
		break;
	case TIMEOUT_ACTION_NONE:
		break;
	}
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct nvme_ctrlr *nvme_ctrlr;
	struct nvme_probe_ctx *ctx = cb_ctx;
	const char *name = NULL;
	size_t i;

	if (ctx) {
		for (i = 0; i < ctx->count; i++) {
			if (spdk_nvme_transport_id_compare(trid, &ctx->trids[i]) == 0) {
				name = strdup(ctx->names[i]);
				break;
			}
		}
	} else {
		name = spdk_sprintf_alloc("HotInNvme%d", g_hot_insert_nvme_controller_index++);
	}
	if (!name) {
		SPDK_ERRLOG("Failed to assign name to NVMe device\n");
		return;
	}

	SPDK_TRACELOG(SPDK_TRACE_BDEV_NVME, "Attached to %s (%s)\n", trid->traddr, name);

	nvme_ctrlr = calloc(1, sizeof(*nvme_ctrlr));
	if (nvme_ctrlr == NULL) {
		SPDK_ERRLOG("Failed to allocate device struct\n");
		free((void *)name);
		return;
	}

	nvme_ctrlr->adminq_timer_poller = NULL;
	nvme_ctrlr->ctrlr = ctrlr;
	nvme_ctrlr->ref = 0;
	nvme_ctrlr->trid = *trid;
	nvme_ctrlr->name = name;

	nvme_ctrlr_create_bdevs(nvme_ctrlr);

	spdk_poller_register(&nvme_ctrlr->adminq_timer_poller, bdev_nvme_poll_adminq, ctrlr,
			     spdk_env_get_current_core(), g_nvme_adminq_poll_timeout_us);

	spdk_io_device_register(ctrlr, bdev_nvme_create_cb, bdev_nvme_destroy_cb,
				sizeof(struct nvme_io_channel));
	TAILQ_INSERT_TAIL(&g_nvme_ctrlrs, nvme_ctrlr, tailq);

	if (g_action_on_timeout != TIMEOUT_ACTION_NONE) {
		spdk_nvme_ctrlr_register_timeout_callback(ctrlr, g_timeout,
				timeout_cb, NULL);
	}
}

static struct nvme_ctrlr *
nvme_ctrlr_get(const struct spdk_nvme_transport_id *trid)
{
	struct nvme_ctrlr	*nvme_ctrlr;

	TAILQ_FOREACH(nvme_ctrlr, &g_nvme_ctrlrs, tailq) {
		if (spdk_nvme_transport_id_compare(trid, &nvme_ctrlr->trid) == 0) {
			return nvme_ctrlr;
		}
	}

	return NULL;
}

static void
remove_cb(void *cb_ctx, struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_bdev *nvme_bdev, *btmp;
	TAILQ_HEAD(, nvme_bdev) removed_bdevs;

	TAILQ_INIT(&removed_bdevs);
	pthread_mutex_lock(&g_bdev_nvme_mutex);
	TAILQ_FOREACH_SAFE(nvme_bdev, &g_nvme_bdevs, link, btmp) {
		if (nvme_bdev->nvme_ctrlr->ctrlr == ctrlr) {
			TAILQ_REMOVE(&g_nvme_bdevs, nvme_bdev, link);
			TAILQ_INSERT_TAIL(&removed_bdevs, nvme_bdev, link);
		}
	}
	pthread_mutex_unlock(&g_bdev_nvme_mutex);

	TAILQ_FOREACH_SAFE(nvme_bdev, &removed_bdevs, link, btmp) {
		TAILQ_REMOVE(&removed_bdevs, nvme_bdev, link);
		spdk_bdev_unregister(&nvme_bdev->disk);
	}
}

static void
blockdev_nvme_hotplug(void *arg)
{
	if (spdk_nvme_probe(NULL, NULL, hotplug_probe_cb, attach_cb, remove_cb) != 0) {
		SPDK_ERRLOG("spdk_nvme_probe() failed\n");
	}
}

int
spdk_bdev_nvme_create(struct spdk_nvme_transport_id *trid,
		      const char *base_name,
		      const char **names, size_t *count)
{
	struct nvme_probe_ctx	probe_ctx;
	struct nvme_ctrlr	*nvme_ctrlr;
	struct nvme_bdev	*nvme_bdev;
	size_t			j;

	if (nvme_ctrlr_get(trid) != NULL) {
		SPDK_ERRLOG("A controller with the provided trid (traddr: %s) already exists.\n", trid->traddr);
		return -1;
	}

	probe_ctx.count = 1;
	probe_ctx.trids[0] = *trid;
	probe_ctx.names[0] = base_name;
	if (spdk_nvme_probe(trid, &probe_ctx, probe_cb, attach_cb, NULL)) {
		SPDK_ERRLOG("Failed to probe for new devices\n");
		return -1;
	}

	nvme_ctrlr = nvme_ctrlr_get(trid);
	if (!nvme_ctrlr) {
		SPDK_ERRLOG("Failed to find new NVMe controller\n");
		return -1;
	}

	/*
	 * Report the new bdevs that were created in this call.
	 * There can be more than one bdev per NVMe controller since one bdev is created per namespace.
	 */
	j = 0;
	TAILQ_FOREACH(nvme_bdev, &g_nvme_bdevs, link) {
		if (nvme_bdev->nvme_ctrlr == nvme_ctrlr) {
			if (j < *count) {
				names[j] = nvme_bdev->disk.name;
				j++;
			} else {
				SPDK_ERRLOG("Unable to return all names of created bdevs\n");
				return -1;
			}
		}
	}
	*count = j;

	return 0;
}

static int
bdev_nvme_library_init(void)
{
	struct spdk_conf_section *sp;
	const char *val;
	int rc;
	size_t i;
	struct nvme_probe_ctx probe_ctx = {};
	int retry_count;

	sp = spdk_conf_find_section(NULL, "Nvme");
	if (sp == NULL) {
		return 0;
	}

	if ((retry_count = spdk_conf_section_get_intval(sp, "RetryCount")) < 0) {
		if ((retry_count = spdk_conf_section_get_intval(sp, "NvmeRetryCount")) < 0) {
			retry_count = SPDK_NVME_DEFAULT_RETRY_COUNT;
		} else {
			SPDK_WARNLOG("NvmeRetryCount was renamed to RetryCount\n");
			SPDK_WARNLOG("Please update your configuration file");
		}
	}

	spdk_nvme_retry_count = retry_count;

	for (i = 0; i < NVME_MAX_CONTROLLERS; i++) {
		val = spdk_conf_section_get_nmval(sp, "TransportID", i, 0);
		if (val == NULL) {
			break;
		}

		rc = spdk_nvme_transport_id_parse(&probe_ctx.trids[i], val);
		if (rc < 0) {
			SPDK_ERRLOG("Unable to parse TransportID: %s\n", val);
			return -1;
		}

		val = spdk_conf_section_get_nmval(sp, "TransportID", i, 1);
		if (val == NULL) {
			SPDK_ERRLOG("No name provided for TransportID\n");
			return -1;
		}

		probe_ctx.names[i] = val;

		probe_ctx.count++;
	}

	if ((g_timeout = spdk_conf_section_get_intval(sp, "Timeout")) < 0) {
		/* Check old name for backward compatibility */
		if ((g_timeout = spdk_conf_section_get_intval(sp, "NvmeTimeoutValue")) < 0) {
			g_timeout = 0;
		} else {
			SPDK_WARNLOG("NvmeTimeoutValue was renamed to Timeout\n");
			SPDK_WARNLOG("Please update your configuration file\n");
		}
	}

	if (g_timeout > 0) {
		val = spdk_conf_section_get_val(sp, "ActionOnTimeout");
		if (val != NULL) {
			if (!strcasecmp(val, "Reset")) {
				g_action_on_timeout = TIMEOUT_ACTION_RESET;
			} else if (!strcasecmp(val, "Abort")) {
				g_action_on_timeout = TIMEOUT_ACTION_ABORT;
			}
		} else {
			/* Handle old name for backward compatibility */
			val = spdk_conf_section_get_val(sp, "ResetControllerOnTimeout");
			if (val) {
				SPDK_WARNLOG("ResetControllerOnTimeout was renamed to ActionOnTimeout\n");
				SPDK_WARNLOG("Please update your configuration file\n");

				if (spdk_conf_section_get_boolval(sp, "ResetControllerOnTimeout", false)) {
					g_action_on_timeout = TIMEOUT_ACTION_RESET;
				}
			}
		}
	}

	g_nvme_adminq_poll_timeout_us = spdk_conf_section_get_intval(sp, "AdminPollRate");
	if (g_nvme_adminq_poll_timeout_us <= 0) {
		g_nvme_adminq_poll_timeout_us = 1000000;
	}

	g_nvme_hotplug_enabled = spdk_conf_section_get_boolval(sp, "HotplugEnable", true);

	g_nvme_hotplug_poll_timeout_us = spdk_conf_section_get_intval(sp, "HotplugPollRate");
	if (g_nvme_hotplug_poll_timeout_us <= 0 || g_nvme_hotplug_poll_timeout_us > 100000) {
		g_nvme_hotplug_poll_timeout_us = 100000;
	}

	g_nvme_hotplug_poll_core = spdk_conf_section_get_intval(sp, "HotplugPollCore");
	if (g_nvme_hotplug_poll_core <= 0) {
		g_nvme_hotplug_poll_core = spdk_env_get_current_core();
	}

	if (spdk_nvme_probe(NULL, &probe_ctx, probe_cb, attach_cb, NULL)) {
		return -1;
	}

	if (g_nvme_hotplug_enabled) {
		spdk_poller_register(&g_hotplug_poller, blockdev_nvme_hotplug, NULL,
				     g_nvme_hotplug_poll_core, g_nvme_hotplug_poll_timeout_us);
	}

	return 0;
}

static void
bdev_nvme_library_fini(void)
{
	struct nvme_bdev *nvme_bdev, *btmp;

	if (g_nvme_hotplug_enabled) {
		spdk_poller_unregister(&g_hotplug_poller, NULL);
	}

	TAILQ_FOREACH_SAFE(nvme_bdev, &g_nvme_bdevs, link, btmp) {
		bdev_nvme_destruct(&nvme_bdev->disk);
	}
}

static void
nvme_ctrlr_create_bdevs(struct nvme_ctrlr *nvme_ctrlr)
{
	struct nvme_bdev	*bdev;
	struct spdk_nvme_ctrlr	*ctrlr = nvme_ctrlr->ctrlr;
	struct spdk_nvme_ns	*ns;
	const struct spdk_nvme_ctrlr_data *cdata;
	int			ns_id, num_ns;

	num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	for (ns_id = 1; ns_id <= num_ns; ns_id++) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, ns_id);
		if (!ns) {
			SPDK_TRACELOG(SPDK_TRACE_BDEV_NVME, "Skipping invalid NS %d\n", ns_id);
			continue;
		}

		if (!spdk_nvme_ns_is_active(ns)) {
			SPDK_TRACELOG(SPDK_TRACE_BDEV_NVME, "Skipping inactive NS %d\n", ns_id);
			continue;
		}

		bdev = calloc(1, sizeof(*bdev));
		if (!bdev) {
			return;
		}

		bdev->nvme_ctrlr = nvme_ctrlr;
		bdev->ns = ns;
		nvme_ctrlr->ref++;

		snprintf(bdev->disk.name, SPDK_BDEV_MAX_NAME_LENGTH,
			 "%sn%d", nvme_ctrlr->name, spdk_nvme_ns_get_id(ns));
		snprintf(bdev->disk.product_name, SPDK_BDEV_MAX_PRODUCT_NAME_LENGTH,
			 "NVMe disk");

		if (cdata->oncs.dsm) {
			/*
			 * Enable the thin provisioning
			 * if nvme controller supports
			 * DataSet Management command.
			 */
			bdev->disk.thin_provisioning = 1;
			bdev->disk.max_unmap_bdesc_count = NVME_DEFAULT_MAX_UNMAP_BDESC_COUNT;
		}

		bdev->disk.write_cache = 0;
		if (cdata->vwc.present) {
			/* Enable if the Volatile Write Cache exists */
			bdev->disk.write_cache = 1;
		}
		bdev->disk.blocklen = spdk_nvme_ns_get_sector_size(ns);
		bdev->disk.blockcnt = spdk_nvme_ns_get_num_sectors(ns);
		bdev->disk.ctxt = bdev;
		bdev->disk.fn_table = &nvmelib_fn_table;
		spdk_bdev_register(&bdev->disk);

		TAILQ_INSERT_TAIL(&g_nvme_bdevs, bdev, link);
	}
}

static void
bdev_nvme_queued_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx((struct nvme_bdev_io *)ref);

	spdk_bdev_io_complete_nvme_status(bdev_io, cpl->status.sct, cpl->status.sc);
}

static void
bdev_nvme_queued_reset_sgl(void *ref, uint32_t sgl_offset)
{
	struct nvme_bdev_io *bio = ref;
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

static int
bdev_nvme_queue_cmd(struct nvme_bdev *bdev, struct spdk_nvme_qpair *qpair,
		    struct nvme_bdev_io *bio,
		    int direction, struct iovec *iov, int iovcnt, uint64_t nbytes,
		    uint64_t offset)
{
	uint32_t ss = spdk_nvme_ns_get_sector_size(bdev->ns);
	uint32_t lba_count;
	uint64_t lba = offset / bdev->disk.blocklen;
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
		rc = spdk_nvme_ns_cmd_readv(bdev->ns, qpair, lba,
					    lba_count, bdev_nvme_queued_done, bio, 0,
					    bdev_nvme_queued_reset_sgl, bdev_nvme_queued_next_sge);
	} else {
		rc = spdk_nvme_ns_cmd_writev(bdev->ns, qpair, lba,
					     lba_count, bdev_nvme_queued_done, bio, 0,
					     bdev_nvme_queued_reset_sgl, bdev_nvme_queued_next_sge);
	}

	if (rc != 0) {
		SPDK_ERRLOG("IO failed\n");
	}
	return rc;
}

static int
bdev_nvme_unmap(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
		struct nvme_bdev_io *bio,
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
		dsm_range[i].starting_lba = from_be64(&unmap_d->lba);
		dsm_range[i].length = from_be32(&unmap_d->block_count);
		dsm_range[i].attributes.raw = 0;
		unmap_d++;
	}

	rc = spdk_nvme_ns_cmd_dataset_management(nbdev->ns, nvme_ch->qpair,
			SPDK_NVME_DSM_ATTR_DEALLOCATE,
			dsm_range, bdesc_count,
			bdev_nvme_queued_done, bio);

	if (rc != 0)
		return -1;

	return 0;
}

static void
bdev_nvme_get_spdk_running_config(FILE *fp)
{
	/* TODO */
}

SPDK_LOG_REGISTER_TRACE_FLAG("bdev_nvme", SPDK_TRACE_BDEV_NVME)
