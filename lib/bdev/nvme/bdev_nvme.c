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

#include "spdk/stdinc.h"

#include "bdev_nvme.h"

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
	char				*name;
	int				ref;

	struct spdk_bdev_poller		*adminq_timer_poller;

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
	struct spdk_bdev_poller	*poller;

	bool			collect_spin_stat;
	uint64_t		spin_ticks;
	uint64_t		start_ticks;
	uint64_t		end_ticks;
};

struct nvme_bdev_io {
	/** array of iovecs to transfer. */
	struct iovec *iovs;

	/** Number of iovecs in iovs array. */
	int iovcnt;

	/** Current iovec position. */
	int iovpos;

	/** Offset in current iovec. */
	uint32_t iov_offset;

	/** Saved status for admin passthru completion event. */
	struct spdk_nvme_cpl cpl;

	/** Originating thread */
	struct spdk_thread *orig_thread;
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
static bool g_nvme_hotplug_enabled = false;
static int g_nvme_hotplug_poll_timeout_us = 0;
static int g_nvme_hotplug_poll_core = 0;
static struct spdk_bdev_poller *g_hotplug_poller;
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
static int bdev_nvme_admin_passthru(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
				    struct nvme_bdev_io *bio,
				    struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes);
static int bdev_nvme_io_passthru(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
				 struct nvme_bdev_io *bio,
				 struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes);

static int
bdev_nvme_get_ctx_size(void)
{
	return sizeof(struct nvme_bdev_io);
}

SPDK_BDEV_MODULE_REGISTER(nvme, bdev_nvme_library_init, bdev_nvme_library_fini,
			  bdev_nvme_get_spdk_running_config,
			  bdev_nvme_get_ctx_size, NULL)

static int
bdev_nvme_readv(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
		struct nvme_bdev_io *bio,
		struct iovec *iov, int iovcnt, uint64_t nbytes, uint64_t offset)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);

	SPDK_DEBUGLOG(SPDK_TRACE_BDEV_NVME, "read %lu bytes with offset %#lx\n",
		      nbytes, offset);

	return bdev_nvme_queue_cmd(nbdev, nvme_ch->qpair, bio, BDEV_DISK_READ,
				   iov, iovcnt, nbytes, offset);
}

static int
bdev_nvme_writev(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
		 struct nvme_bdev_io *bio,
		 struct iovec *iov, int iovcnt, size_t len, uint64_t offset)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);

	SPDK_DEBUGLOG(SPDK_TRACE_BDEV_NVME, "write %lu bytes with offset %#lx\n",
		      len, offset);

	return bdev_nvme_queue_cmd(nbdev, nvme_ch->qpair, bio, BDEV_DISK_WRITE,
				   iov, iovcnt, len, offset);
}

static void
bdev_nvme_poll(void *arg)
{
	struct nvme_io_channel *ch = arg;
	int32_t num_completions;

	if (ch->qpair == NULL) {
		return;
	}

	if (ch->collect_spin_stat && ch->start_ticks == 0) {
		ch->start_ticks = spdk_get_ticks();
	}

	num_completions = spdk_nvme_qpair_process_completions(ch->qpair, 0);

	if (ch->collect_spin_stat) {
		if (num_completions > 0) {
			if (ch->end_ticks != 0) {
				ch->spin_ticks += (ch->end_ticks - ch->start_ticks);
				ch->end_ticks = 0;
			}
			ch->start_ticks = 0;
		} else {
			ch->end_ticks = spdk_get_ticks();
		}
	}
}

static void
bdev_nvme_poll_adminq(void *arg)
{
	struct spdk_nvme_ctrlr *ctrlr = arg;

	spdk_nvme_ctrlr_process_admin_completions(ctrlr);
}

static void
bdev_nvme_unregister_cb(void *io_device)
{
	struct spdk_nvme_ctrlr *ctrlr = io_device;

	spdk_nvme_detach(ctrlr);
}

static int
bdev_nvme_destruct(void *ctx)
{
	struct nvme_bdev *nvme_disk = ctx;
	struct nvme_ctrlr *nvme_ctrlr = nvme_disk->nvme_ctrlr;

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	nvme_ctrlr->ref--;

	TAILQ_REMOVE(&g_nvme_bdevs, nvme_disk, link);
	free(nvme_disk->disk.name);
	free(nvme_disk);

	if (nvme_ctrlr->ref == 0) {
		TAILQ_REMOVE(&g_nvme_ctrlrs, nvme_ctrlr, tailq);
		pthread_mutex_unlock(&g_bdev_nvme_mutex);
		spdk_io_device_unregister(nvme_ctrlr->ctrlr, bdev_nvme_unregister_cb);
		spdk_bdev_poller_stop(&nvme_ctrlr->adminq_timer_poller);
		free(nvme_ctrlr->name);
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

static void
_bdev_nvme_reset_done(void *io_device, void *ctx)
{
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(ctx), SPDK_BDEV_IO_STATUS_SUCCESS);
}

static void
_bdev_nvme_reset_create_qpair(void *io_device, struct spdk_io_channel *ch,
			      void *ctx)
{
	struct spdk_nvme_ctrlr *ctrlr = io_device;
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);

	nvme_ch->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
	assert(nvme_ch->qpair != NULL); /* Currently, no good way to handle this error */
}

static void
_bdev_nvme_reset(void *io_device, void *ctx)
{
	struct spdk_nvme_ctrlr *ctrlr = io_device;
	struct nvme_bdev_io *bio = ctx;
	int rc;

	rc = spdk_nvme_ctrlr_reset(ctrlr);
	if (rc != 0) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bio), SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	/* Recreate all of the I/O queue pairs */
	spdk_for_each_channel(ctrlr,
			      _bdev_nvme_reset_create_qpair,
			      ctx,
			      _bdev_nvme_reset_done);


}

static void
_bdev_nvme_reset_destroy_qpair(void *io_device, struct spdk_io_channel *ch,
			       void *ctx)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);

	spdk_nvme_ctrlr_free_io_qpair(nvme_ch->qpair);
	nvme_ch->qpair = NULL;
}

static int
bdev_nvme_reset(struct nvme_bdev *nbdev, struct nvme_bdev_io *bio)
{
	/* First, delete all NVMe I/O queue pairs. */
	spdk_for_each_channel(nbdev->nvme_ctrlr->ctrlr,
			      _bdev_nvme_reset_destroy_qpair,
			      bio,
			      _bdev_nvme_reset);

	return 0;
}

static int
bdev_nvme_unmap(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
		struct nvme_bdev_io *bio,
		uint64_t offset,
		uint64_t nbytes);

static void
bdev_nvme_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	int ret;

	ret = bdev_nvme_readv((struct nvme_bdev *)bdev_io->bdev->ctxt,
			      ch,
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
_bdev_nvme_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);
	if (nvme_ch->qpair == NULL) {
		/* The device is currently resetting */
		return -1;
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, bdev_nvme_get_buf_cb);
		return 0;

	case SPDK_BDEV_IO_TYPE_WRITE:
		return bdev_nvme_writev((struct nvme_bdev *)bdev_io->bdev->ctxt,
					ch,
					(struct nvme_bdev_io *)bdev_io->driver_ctx,
					bdev_io->u.write.iovs,
					bdev_io->u.write.iovcnt,
					bdev_io->u.write.len,
					bdev_io->u.write.offset);

	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		return bdev_nvme_unmap((struct nvme_bdev *)bdev_io->bdev->ctxt,
				       ch,
				       (struct nvme_bdev_io *)bdev_io->driver_ctx,
				       bdev_io->u.write.len,
				       bdev_io->u.write.offset);

	case SPDK_BDEV_IO_TYPE_UNMAP:
		return bdev_nvme_unmap((struct nvme_bdev *)bdev_io->bdev->ctxt,
				       ch,
				       (struct nvme_bdev_io *)bdev_io->driver_ctx,
				       bdev_io->u.unmap.offset,
				       bdev_io->u.unmap.len);

	case SPDK_BDEV_IO_TYPE_RESET:
		return bdev_nvme_reset((struct nvme_bdev *)bdev_io->bdev->ctxt,
				       (struct nvme_bdev_io *)bdev_io->driver_ctx);

	case SPDK_BDEV_IO_TYPE_FLUSH:
		return bdev_nvme_flush((struct nvme_bdev *)bdev_io->bdev->ctxt,
				       (struct nvme_bdev_io *)bdev_io->driver_ctx,
				       bdev_io->u.flush.offset,
				       bdev_io->u.flush.len);

	case SPDK_BDEV_IO_TYPE_NVME_ADMIN:
		return bdev_nvme_admin_passthru((struct nvme_bdev *)bdev_io->bdev->ctxt,
						ch,
						(struct nvme_bdev_io *)bdev_io->driver_ctx,
						&bdev_io->u.nvme_passthru.cmd,
						bdev_io->u.nvme_passthru.buf,
						bdev_io->u.nvme_passthru.nbytes);

	case SPDK_BDEV_IO_TYPE_NVME_IO:
		return bdev_nvme_io_passthru((struct nvme_bdev *)bdev_io->bdev->ctxt,
					     ch,
					     (struct nvme_bdev_io *)bdev_io->driver_ctx,
					     &bdev_io->u.nvme_passthru.cmd,
					     bdev_io->u.nvme_passthru.buf,
					     bdev_io->u.nvme_passthru.nbytes);

	default:
		return -EINVAL;
	}
	return 0;
}

static void
bdev_nvme_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	if (_bdev_nvme_submit_request(ch, bdev_io) < 0) {
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
	case SPDK_BDEV_IO_TYPE_NVME_ADMIN:
	case SPDK_BDEV_IO_TYPE_NVME_IO:
		return true;

	case SPDK_BDEV_IO_TYPE_UNMAP:
		cdata = spdk_nvme_ctrlr_get_data(nbdev->nvme_ctrlr->ctrlr);
		return cdata->oncs.dsm;

	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		cdata = spdk_nvme_ctrlr_get_data(nbdev->nvme_ctrlr->ctrlr);
		/*
		 * If an NVMe controller guarantees reading unallocated blocks returns zero,
		 * we can implement WRITE_ZEROES as an NVMe deallocate command.
		 */
		if (cdata->oncs.dsm &&
		    spdk_nvme_ns_get_dealloc_logical_block_read_value(nbdev->ns) == SPDK_NVME_DEALLOC_READ_00) {
			return true;
		}
		/*
		 * The NVMe controller write_zeroes function is currently not used by our driver.
		 * If a user submits an arbitrarily large write_zeroes request to the controller, the request will fail.
		 * Until this is resolved, we only claim support for write_zeroes if deallocated blocks return 0's when read.
		 */
		return false;

	default:
		return false;
	}
}

static int
bdev_nvme_create_cb(void *io_device, void *ctx_buf)
{
	struct spdk_nvme_ctrlr *ctrlr = io_device;
	struct nvme_io_channel *ch = ctx_buf;

#ifdef SPDK_CONFIG_VTUNE
	ch->collect_spin_stat = true;
#else
	ch->collect_spin_stat = false;
#endif

	ch->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);

	if (ch->qpair == NULL) {
		return -1;
	}

	spdk_bdev_poller_start(&ch->poller, bdev_nvme_poll, ch,
			       spdk_env_get_current_core(), 0);
	return 0;
}

static void
bdev_nvme_destroy_cb(void *io_device, void *ctx_buf)
{
	struct nvme_io_channel *ch = ctx_buf;

	spdk_nvme_ctrlr_free_io_qpair(ch->qpair);
	spdk_bdev_poller_stop(&ch->poller);
}

static struct spdk_io_channel *
bdev_nvme_get_io_channel(void *ctx)
{
	struct nvme_bdev *nvme_bdev = ctx;

	return spdk_get_io_channel(nvme_bdev->nvme_ctrlr->ctrlr);
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
	const char *trtype_str;
	const char *adrfam_str;
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

	trtype_str = spdk_nvme_transport_id_trtype_str(nvme_ctrlr->trid.trtype);
	if (trtype_str) {
		spdk_json_write_name(w, "trtype");
		spdk_json_write_string(w, trtype_str);
	}

	adrfam_str = spdk_nvme_transport_id_adrfam_str(nvme_ctrlr->trid.adrfam);
	if (adrfam_str) {
		spdk_json_write_name(w, "adrfam");
		spdk_json_write_string(w, adrfam_str);
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

static uint64_t
bdev_nvme_get_spin_time(struct spdk_io_channel *ch)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);
	uint64_t spin_time;

	if (!nvme_ch->collect_spin_stat) {
		return 0;
	}

	if (nvme_ch->end_ticks != 0) {
		nvme_ch->spin_ticks += (nvme_ch->end_ticks - nvme_ch->start_ticks);
		nvme_ch->end_ticks = 0;
	}

	spin_time = (nvme_ch->spin_ticks * 1000000ULL) / spdk_get_ticks_hz();
	nvme_ch->start_ticks = 0;
	nvme_ch->spin_ticks = 0;

	return spin_time;
}

static const struct spdk_bdev_fn_table nvmelib_fn_table = {
	.destruct		= bdev_nvme_destruct,
	.submit_request		= bdev_nvme_submit_request,
	.io_type_supported	= bdev_nvme_io_type_supported,
	.get_io_channel		= bdev_nvme_get_io_channel,
	.dump_config_json	= bdev_nvme_dump_config_json,
	.get_spin_time		= bdev_nvme_get_spin_time,
};

static bool
hotplug_probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		 struct spdk_nvme_ctrlr_opts *opts)
{
	SPDK_DEBUGLOG(SPDK_TRACE_BDEV_NVME, "Attaching to %s\n", trid->traddr);

	return true;
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

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{

	SPDK_DEBUGLOG(SPDK_TRACE_BDEV_NVME, "Probing device %s\n", trid->traddr);

	if (nvme_ctrlr_get(trid)) {
		SPDK_ERRLOG("A controller with the provided trid (traddr: %s) already exists.\n",
			    trid->traddr);
		return false;
	}

	if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE) {
		struct spdk_pci_addr pci_addr;
		bool claim_device = false;
		struct nvme_probe_ctx *ctx = cb_ctx;
		size_t i;

		for (i = 0; i < ctx->count; i++) {
			if (spdk_nvme_transport_id_compare(trid, &ctx->trids[i]) == 0) {
				claim_device = true;
				break;
			}
		}

		if (!claim_device) {
			SPDK_DEBUGLOG(SPDK_TRACE_BDEV_NVME, "Not claiming device at %s\n", trid->traddr);
			return false;
		}

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

	/* FALLTHROUGH */
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
	char *name = NULL;
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

	SPDK_DEBUGLOG(SPDK_TRACE_BDEV_NVME, "Attached to %s (%s)\n", trid->traddr, name);

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

	spdk_io_device_register(ctrlr, bdev_nvme_create_cb, bdev_nvme_destroy_cb,
				sizeof(struct nvme_io_channel));
	nvme_ctrlr_create_bdevs(nvme_ctrlr);

	spdk_bdev_poller_start(&nvme_ctrlr->adminq_timer_poller, bdev_nvme_poll_adminq, ctrlr,
			       spdk_env_get_current_core(), g_nvme_adminq_poll_timeout_us);

	TAILQ_INSERT_TAIL(&g_nvme_ctrlrs, nvme_ctrlr, tailq);

	if (g_action_on_timeout != TIMEOUT_ACTION_NONE) {
		spdk_nvme_ctrlr_register_timeout_callback(ctrlr, g_timeout,
				timeout_cb, NULL);
	}
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
bdev_nvme_hotplug(void *arg)
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
	struct nvme_probe_ctx	*probe_ctx;
	struct nvme_ctrlr	*nvme_ctrlr;
	struct nvme_bdev	*nvme_bdev;
	size_t			j;

	if (nvme_ctrlr_get(trid) != NULL) {
		SPDK_ERRLOG("A controller with the provided trid (traddr: %s) already exists.\n", trid->traddr);
		return -1;
	}

	probe_ctx = calloc(1, sizeof(*probe_ctx));
	if (probe_ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate probe_ctx\n");
		return -1;
	}

	probe_ctx->count = 1;
	probe_ctx->trids[0] = *trid;
	probe_ctx->names[0] = base_name;
	if (spdk_nvme_probe(trid, probe_ctx, probe_cb, attach_cb, NULL)) {
		SPDK_ERRLOG("Failed to probe for new devices\n");
		free(probe_ctx);
		return -1;
	}

	nvme_ctrlr = nvme_ctrlr_get(trid);
	if (!nvme_ctrlr) {
		SPDK_ERRLOG("Failed to find new NVMe controller\n");
		free(probe_ctx);
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
				free(probe_ctx);
				return -1;
			}
		}
	}
	*count = j;

	free(probe_ctx);
	return 0;
}

static int
bdev_nvme_library_init(void)
{
	struct spdk_conf_section *sp;
	const char *val;
	int rc = 0;
	size_t i;
	struct nvme_probe_ctx *probe_ctx = NULL;
	int retry_count;
	uint32_t local_nvme_num = 0;

	sp = spdk_conf_find_section(NULL, "Nvme");
	if (sp == NULL) {
		goto end;
	}

	probe_ctx = calloc(1, sizeof(*probe_ctx));
	if (probe_ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate probe_ctx\n");
		rc = -1;
		goto end;
	}

	if ((retry_count = spdk_conf_section_get_intval(sp, "RetryCount")) < 0) {
		if ((retry_count = spdk_conf_section_get_intval(sp, "NvmeRetryCount")) < 0) {
			retry_count = SPDK_NVME_DEFAULT_RETRY_COUNT;
		} else {
			SPDK_WARNLOG("NvmeRetryCount was renamed to RetryCount\n");
			SPDK_WARNLOG("Please update your configuration file\n");
		}
	}

	spdk_nvme_retry_count = retry_count;

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

	if (spdk_process_is_primary()) {
		g_nvme_hotplug_enabled = spdk_conf_section_get_boolval(sp, "HotplugEnable", false);
	}

	g_nvme_hotplug_poll_timeout_us = spdk_conf_section_get_intval(sp, "HotplugPollRate");
	if (g_nvme_hotplug_poll_timeout_us <= 0 || g_nvme_hotplug_poll_timeout_us > 100000) {
		g_nvme_hotplug_poll_timeout_us = 100000;
	}

	g_nvme_hotplug_poll_core = spdk_conf_section_get_intval(sp, "HotplugPollCore");
	if (g_nvme_hotplug_poll_core <= 0) {
		g_nvme_hotplug_poll_core = spdk_env_get_current_core();
	}


	for (i = 0; i < NVME_MAX_CONTROLLERS; i++) {
		val = spdk_conf_section_get_nmval(sp, "TransportID", i, 0);
		if (val == NULL) {
			break;
		}

		rc = spdk_nvme_transport_id_parse(&probe_ctx->trids[i], val);
		if (rc < 0) {
			SPDK_ERRLOG("Unable to parse TransportID: %s\n", val);
			rc = -1;
			goto end;
		}

		val = spdk_conf_section_get_nmval(sp, "TransportID", i, 1);
		if (val == NULL) {
			SPDK_ERRLOG("No name provided for TransportID\n");
			rc = -1;
			goto end;
		}

		probe_ctx->names[i] = val;
		probe_ctx->count++;

		if (probe_ctx->trids[i].trtype != SPDK_NVME_TRANSPORT_PCIE) {
			if (probe_ctx->trids[i].subnqn[0] == '\0') {
				SPDK_ERRLOG("Need to provide subsystem nqn\n");
				rc = -1;
				goto end;
			}

			if (spdk_nvme_probe(&probe_ctx->trids[i], probe_ctx, probe_cb, attach_cb, NULL)) {
				rc = -1;
				goto end;
			}
		} else {
			local_nvme_num++;
		}
	}

	if (local_nvme_num > 0) {
		/* used to probe local NVMe device */
		if (spdk_nvme_probe(NULL, probe_ctx, probe_cb, attach_cb, NULL)) {
			rc = -1;
			goto end;
		}
	}

	if (g_nvme_hotplug_enabled) {
		spdk_bdev_poller_start(&g_hotplug_poller, bdev_nvme_hotplug, NULL,
				       g_nvme_hotplug_poll_core,
				       g_nvme_hotplug_poll_timeout_us);
	}

end:
	free(probe_ctx);
	return rc;
}

static void
bdev_nvme_library_fini(void)
{
	struct nvme_bdev *nvme_bdev, *btmp;

	if (g_nvme_hotplug_enabled) {
		spdk_bdev_poller_stop(&g_hotplug_poller);
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
			SPDK_DEBUGLOG(SPDK_TRACE_BDEV_NVME, "Skipping invalid NS %d\n", ns_id);
			continue;
		}

		if (!spdk_nvme_ns_is_active(ns)) {
			SPDK_DEBUGLOG(SPDK_TRACE_BDEV_NVME, "Skipping inactive NS %d\n", ns_id);
			continue;
		}

		bdev = calloc(1, sizeof(*bdev));
		if (!bdev) {
			return;
		}

		bdev->nvme_ctrlr = nvme_ctrlr;
		bdev->ns = ns;
		nvme_ctrlr->ref++;

		bdev->disk.name = spdk_sprintf_alloc("%sn%d", nvme_ctrlr->name, spdk_nvme_ns_get_id(ns));
		if (!bdev->disk.name) {
			free(bdev);
			return;
		}
		bdev->disk.product_name = "NVMe disk";

		bdev->disk.write_cache = 0;
		if (cdata->vwc.present) {
			/* Enable if the Volatile Write Cache exists */
			bdev->disk.write_cache = 1;
		}
		bdev->disk.blocklen = spdk_nvme_ns_get_sector_size(ns);
		bdev->disk.blockcnt = spdk_nvme_ns_get_num_sectors(ns);
		bdev->disk.optimal_io_boundary = spdk_nvme_ns_get_optimal_io_boundary(ns);
		bdev->disk.ctxt = bdev;
		bdev->disk.fn_table = &nvmelib_fn_table;
		bdev->disk.module = SPDK_GET_BDEV_MODULE(nvme);
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
bdev_nvme_admin_passthru_completion(void *ctx)
{
	struct nvme_bdev_io *bio = ctx;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);

	spdk_bdev_io_complete_nvme_status(bdev_io,
					  bio->cpl.status.sct, bio->cpl.status.sc);
}

static void
bdev_nvme_admin_passthru_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;

	bio->cpl = *cpl;
	spdk_thread_send_msg(bio->orig_thread, bdev_nvme_admin_passthru_completion, bio);
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
	uint32_t lba_count = nbytes / bdev->disk.blocklen;
	uint64_t lba = offset / bdev->disk.blocklen;
	int rc;

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
		SPDK_ERRLOG("%s failed: rc = %d\n", direction == BDEV_DISK_READ ? "readv" : "writev", rc);
	}
	return rc;
}

static int
bdev_nvme_unmap(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
		struct nvme_bdev_io *bio,
		uint64_t offset,
		uint64_t nbytes)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);
	int rc = 0;
	struct spdk_nvme_dsm_range dsm_range = {};

	dsm_range.starting_lba = offset / nbdev->disk.blocklen;
	dsm_range.length = nbytes / nbdev->disk.blocklen;

	rc = spdk_nvme_ns_cmd_dataset_management(nbdev->ns, nvme_ch->qpair,
			SPDK_NVME_DSM_ATTR_DEALLOCATE,
			&dsm_range, 1,
			bdev_nvme_queued_done, bio);

	return rc;
}

static int
bdev_nvme_admin_passthru(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
			 struct nvme_bdev_io *bio,
			 struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes)
{
	if (nbytes > UINT32_MAX) {
		SPDK_ERRLOG("nbytes is greater than UINT32_MAX.\n");
		return -EINVAL;
	}

	bio->orig_thread = spdk_io_channel_get_thread(ch);

	return spdk_nvme_ctrlr_cmd_admin_raw(nbdev->nvme_ctrlr->ctrlr, cmd, buf,
					     (uint32_t)nbytes, bdev_nvme_admin_passthru_done, bio);
}

static int
bdev_nvme_io_passthru(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
		      struct nvme_bdev_io *bio,
		      struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);

	if (nbytes > UINT32_MAX) {
		SPDK_ERRLOG("nbytes is greater than UINT32_MAX.\n");
		return -EINVAL;
	}

	/*
	 * Each NVMe bdev is a specific namespace, and all NVMe I/O commands require a nsid,
	 * so fill it out automatically.
	 */
	cmd->nsid = spdk_nvme_ns_get_id(nbdev->ns);

	return spdk_nvme_ctrlr_cmd_io_raw(nbdev->nvme_ctrlr->ctrlr, nvme_ch->qpair, cmd, buf,
					  (uint32_t)nbytes, bdev_nvme_queued_done, bio);
}

static void
bdev_nvme_get_spdk_running_config(FILE *fp)
{
	/* TODO */
}

SPDK_LOG_REGISTER_TRACE_FLAG("bdev_nvme", SPDK_TRACE_BDEV_NVME)
