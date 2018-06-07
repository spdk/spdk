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
#include "spdk/likely.h"
#include "spdk/util.h"

#include "spdk/bdev_module.h"
#include "spdk_internal/log.h"

static void bdev_nvme_get_spdk_running_config(FILE *fp);
static int bdev_nvme_config_json(struct spdk_json_write_ctx *w);

struct nvme_io_channel {
	struct spdk_nvme_qpair	*qpair;
	struct spdk_poller	*poller;

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
static struct spdk_poller *g_hotplug_poller;
static pthread_mutex_t g_bdev_nvme_mutex = PTHREAD_MUTEX_INITIALIZER;

static TAILQ_HEAD(, nvme_ctrlr)	g_nvme_ctrlrs = TAILQ_HEAD_INITIALIZER(g_nvme_ctrlrs);
static TAILQ_HEAD(, nvme_bdev) g_nvme_bdevs = TAILQ_HEAD_INITIALIZER(g_nvme_bdevs);

static int nvme_ctrlr_create_bdevs(struct nvme_ctrlr *nvme_ctrlr);
static int bdev_nvme_library_init(void);
static void bdev_nvme_library_fini(void);
static int bdev_nvme_queue_cmd(struct nvme_bdev *bdev, struct spdk_nvme_qpair *qpair,
			       struct nvme_bdev_io *bio,
			       int direction, struct iovec *iov, int iovcnt, uint64_t lba_count,
			       uint64_t lba);
static int bdev_nvme_admin_passthru(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
				    struct nvme_bdev_io *bio,
				    struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes);
static int bdev_nvme_io_passthru(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
				 struct nvme_bdev_io *bio,
				 struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes);
static int bdev_nvme_io_passthru_md(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
				    struct nvme_bdev_io *bio,
				    struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes, void *md_buf, size_t md_len);

static int
bdev_nvme_get_ctx_size(void)
{
	return sizeof(struct nvme_bdev_io);
}

static struct spdk_bdev_module nvme_if = {
	.name = "nvme",
	.module_init = bdev_nvme_library_init,
	.module_fini = bdev_nvme_library_fini,
	.config_text = bdev_nvme_get_spdk_running_config,
	.config_json = bdev_nvme_config_json,
	.get_ctx_size = bdev_nvme_get_ctx_size,

};
SPDK_BDEV_MODULE_REGISTER(&nvme_if)

static int
bdev_nvme_readv(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
		struct nvme_bdev_io *bio,
		struct iovec *iov, int iovcnt, uint64_t lba_count, uint64_t lba)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "read %lu blocks with offset %#lx\n",
		      lba_count, lba);

	return bdev_nvme_queue_cmd(nbdev, nvme_ch->qpair, bio, BDEV_DISK_READ,
				   iov, iovcnt, lba_count, lba);
}

static int
bdev_nvme_writev(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
		 struct nvme_bdev_io *bio,
		 struct iovec *iov, int iovcnt, uint64_t lba_count, uint64_t lba)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "write %lu blocks with offset %#lx\n",
		      lba_count, lba);

	return bdev_nvme_queue_cmd(nbdev, nvme_ch->qpair, bio, BDEV_DISK_WRITE,
				   iov, iovcnt, lba_count, lba);
}

static int
bdev_nvme_poll(void *arg)
{
	struct nvme_io_channel *ch = arg;
	int32_t num_completions;

	if (ch->qpair == NULL) {
		return -1;
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

	return num_completions;
}

static int
bdev_nvme_poll_adminq(void *arg)
{
	struct spdk_nvme_ctrlr *ctrlr = arg;

	return spdk_nvme_ctrlr_process_admin_completions(ctrlr);
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
	TAILQ_REMOVE(&g_nvme_bdevs, nvme_disk, link);
	nvme_ctrlr->ref--;
	free(nvme_disk->disk.name);
	free(nvme_disk);
	if (nvme_ctrlr->ref == 0) {
		TAILQ_REMOVE(&g_nvme_ctrlrs, nvme_ctrlr, tailq);
		pthread_mutex_unlock(&g_bdev_nvme_mutex);
		spdk_io_device_unregister(nvme_ctrlr->ctrlr, bdev_nvme_unregister_cb);
		spdk_poller_unregister(&nvme_ctrlr->adminq_timer_poller);
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
_bdev_nvme_reset_done(struct spdk_io_channel_iter *i, int status)
{
	void *ctx = spdk_io_channel_iter_get_ctx(i);
	int rc = SPDK_BDEV_IO_STATUS_SUCCESS;

	if (status) {
		rc = SPDK_BDEV_IO_STATUS_FAILED;
	}
	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(ctx), rc);
}

static void
_bdev_nvme_reset_create_qpair(struct spdk_io_channel_iter *i)
{
	struct spdk_nvme_ctrlr *ctrlr = spdk_io_channel_iter_get_io_device(i);
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(_ch);

	nvme_ch->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
	if (!nvme_ch->qpair) {
		spdk_for_each_channel_continue(i, -1);
		return;
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
_bdev_nvme_reset(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_nvme_ctrlr *ctrlr = spdk_io_channel_iter_get_io_device(i);
	struct nvme_bdev_io *bio = spdk_io_channel_iter_get_ctx(i);
	int rc;

	if (status) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bio), SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	rc = spdk_nvme_ctrlr_reset(ctrlr);
	if (rc != 0) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bio), SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	/* Recreate all of the I/O queue pairs */
	spdk_for_each_channel(ctrlr,
			      _bdev_nvme_reset_create_qpair,
			      bio,
			      _bdev_nvme_reset_done);


}

static void
_bdev_nvme_reset_destroy_qpair(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	rc = spdk_nvme_ctrlr_free_io_qpair(nvme_ch->qpair);
	if (!rc) {
		nvme_ch->qpair = NULL;
	}

	spdk_for_each_channel_continue(i, rc);
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
		uint64_t offset_blocks,
		uint64_t num_blocks);

static void
bdev_nvme_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	int ret;

	ret = bdev_nvme_readv((struct nvme_bdev *)bdev_io->bdev->ctxt,
			      ch,
			      (struct nvme_bdev_io *)bdev_io->driver_ctx,
			      bdev_io->u.bdev.iovs,
			      bdev_io->u.bdev.iovcnt,
			      bdev_io->u.bdev.num_blocks,
			      bdev_io->u.bdev.offset_blocks);

	if (spdk_likely(ret == 0)) {
		return;
	} else if (ret == -ENOMEM) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
	} else {
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
		spdk_bdev_io_get_buf(bdev_io, bdev_nvme_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return 0;

	case SPDK_BDEV_IO_TYPE_WRITE:
		return bdev_nvme_writev((struct nvme_bdev *)bdev_io->bdev->ctxt,
					ch,
					(struct nvme_bdev_io *)bdev_io->driver_ctx,
					bdev_io->u.bdev.iovs,
					bdev_io->u.bdev.iovcnt,
					bdev_io->u.bdev.num_blocks,
					bdev_io->u.bdev.offset_blocks);

	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		return bdev_nvme_unmap((struct nvme_bdev *)bdev_io->bdev->ctxt,
				       ch,
				       (struct nvme_bdev_io *)bdev_io->driver_ctx,
				       bdev_io->u.bdev.offset_blocks,
				       bdev_io->u.bdev.num_blocks);

	case SPDK_BDEV_IO_TYPE_UNMAP:
		return bdev_nvme_unmap((struct nvme_bdev *)bdev_io->bdev->ctxt,
				       ch,
				       (struct nvme_bdev_io *)bdev_io->driver_ctx,
				       bdev_io->u.bdev.offset_blocks,
				       bdev_io->u.bdev.num_blocks);

	case SPDK_BDEV_IO_TYPE_RESET:
		return bdev_nvme_reset((struct nvme_bdev *)bdev_io->bdev->ctxt,
				       (struct nvme_bdev_io *)bdev_io->driver_ctx);

	case SPDK_BDEV_IO_TYPE_FLUSH:
		return bdev_nvme_flush((struct nvme_bdev *)bdev_io->bdev->ctxt,
				       (struct nvme_bdev_io *)bdev_io->driver_ctx,
				       bdev_io->u.bdev.offset_blocks,
				       bdev_io->u.bdev.num_blocks);

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

	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		return bdev_nvme_io_passthru_md((struct nvme_bdev *)bdev_io->bdev->ctxt,
						ch,
						(struct nvme_bdev_io *)bdev_io->driver_ctx,
						&bdev_io->u.nvme_passthru.cmd,
						bdev_io->u.nvme_passthru.buf,
						bdev_io->u.nvme_passthru.nbytes,
						bdev_io->u.nvme_passthru.md_buf,
						bdev_io->u.nvme_passthru.md_len);

	default:
		return -EINVAL;
	}
	return 0;
}

static void
bdev_nvme_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	int rc = _bdev_nvme_submit_request(ch, bdev_io);

	if (spdk_unlikely(rc != 0)) {
		if (rc == -ENOMEM) {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		} else {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
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

	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		return spdk_nvme_ns_get_md_size(nbdev->ns) ? true : false;

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

	ch->poller = spdk_poller_register(bdev_nvme_poll, ch, 0);
	return 0;
}

static void
bdev_nvme_destroy_cb(void *io_device, void *ctx_buf)
{
	struct nvme_io_channel *ch = ctx_buf;

	spdk_nvme_ctrlr_free_io_qpair(ch->qpair);
	spdk_poller_unregister(&ch->poller);
}

static struct spdk_io_channel *
bdev_nvme_get_io_channel(void *ctx)
{
	struct nvme_bdev *nvme_bdev = ctx;

	return spdk_get_io_channel(nvme_bdev->nvme_ctrlr->ctrlr);
}

static int
bdev_nvme_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
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

	spdk_json_write_named_object_begin(w, "nvme");

	if (nvme_ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
		spdk_json_write_named_string(w, "pci_address", nvme_ctrlr->trid.traddr);
	}

	spdk_json_write_named_object_begin(w, "trid");

	trtype_str = spdk_nvme_transport_id_trtype_str(nvme_ctrlr->trid.trtype);
	if (trtype_str) {
		spdk_json_write_named_string(w, "trtype", trtype_str);
	}

	adrfam_str = spdk_nvme_transport_id_adrfam_str(nvme_ctrlr->trid.adrfam);
	if (adrfam_str) {
		spdk_json_write_named_string(w, "adrfam", adrfam_str);
	}

	if (nvme_ctrlr->trid.traddr[0] != '\0') {
		spdk_json_write_named_string(w, "traddr", nvme_ctrlr->trid.traddr);
	}

	if (nvme_ctrlr->trid.trsvcid[0] != '\0') {
		spdk_json_write_named_string(w, "trsvcid", nvme_ctrlr->trid.trsvcid);
	}

	if (nvme_ctrlr->trid.subnqn[0] != '\0') {
		spdk_json_write_named_string(w, "subnqn", nvme_ctrlr->trid.subnqn);
	}

	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "ctrlr_data");

	spdk_json_write_named_string_fmt(w, "vendor_id", "0x%04x", cdata->vid);

	snprintf(buf, sizeof(cdata->mn) + 1, "%s", cdata->mn);
	spdk_str_trim(buf);
	spdk_json_write_named_string(w, "model_number", buf);

	snprintf(buf, sizeof(cdata->sn) + 1, "%s", cdata->sn);
	spdk_str_trim(buf);
	spdk_json_write_named_string(w, "serial_number", buf);

	snprintf(buf, sizeof(cdata->fr) + 1, "%s", cdata->fr);
	spdk_str_trim(buf);
	spdk_json_write_named_string(w, "firmware_revision", buf);

	spdk_json_write_named_object_begin(w, "oacs");

	spdk_json_write_named_uint32(w, "security", cdata->oacs.security);
	spdk_json_write_named_uint32(w, "format", cdata->oacs.format);
	spdk_json_write_named_uint32(w, "firmware", cdata->oacs.firmware);
	spdk_json_write_named_uint32(w, "ns_manage", cdata->oacs.ns_manage);

	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "vs");

	spdk_json_write_name(w, "nvme_version");
	if (vs.bits.ter) {
		spdk_json_write_string_fmt(w, "%u.%u.%u", vs.bits.mjr, vs.bits.mnr, vs.bits.ter);
	} else {
		spdk_json_write_string_fmt(w, "%u.%u", vs.bits.mjr, vs.bits.mnr);
	}

	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "csts");

	spdk_json_write_named_uint32(w, "rdy", csts.bits.rdy);
	spdk_json_write_named_uint32(w, "cfs", csts.bits.cfs);

	spdk_json_write_object_end(w);

	spdk_json_write_named_object_begin(w, "ns_data");

	spdk_json_write_named_uint32(w, "id", spdk_nvme_ns_get_id(ns));

	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);

	return 0;
}

static void
bdev_nvme_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/* No config per bdev needed */
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
	.dump_info_json		= bdev_nvme_dump_info_json,
	.write_config_json	= bdev_nvme_write_config_json,
	.get_spin_time		= bdev_nvme_get_spin_time,
};

static bool
hotplug_probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		 struct spdk_nvme_ctrlr_opts *opts)
{
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "Attaching to %s\n", trid->traddr);

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

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "Probing device %s\n", trid->traddr);

	if (nvme_ctrlr_get(trid)) {
		SPDK_ERRLOG("A controller with the provided trid (traddr: %s) already exists.\n",
			    trid->traddr);
		return false;
	}

	if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE) {
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
			SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "Not claiming device at %s\n", trid->traddr);
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

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "Attached to %s (%s)\n", trid->traddr, name);

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

	if (nvme_ctrlr_create_bdevs(nvme_ctrlr) != 0) {
		spdk_io_device_unregister(ctrlr, NULL);
		free(nvme_ctrlr->name);
		free(nvme_ctrlr);
		return;
	}

	nvme_ctrlr->adminq_timer_poller = spdk_poller_register(bdev_nvme_poll_adminq, ctrlr,
					  g_nvme_adminq_poll_timeout_us);

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

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	TAILQ_FOREACH_SAFE(nvme_bdev, &g_nvme_bdevs, link, btmp) {
		if (nvme_bdev->nvme_ctrlr->ctrlr == ctrlr) {
			pthread_mutex_unlock(&g_bdev_nvme_mutex);
			spdk_bdev_unregister(&nvme_bdev->disk, NULL, NULL);
			pthread_mutex_lock(&g_bdev_nvme_mutex);
		}
	}
	pthread_mutex_unlock(&g_bdev_nvme_mutex);
}

static int
bdev_nvme_hotplug(void *arg)
{
	if (spdk_nvme_probe(NULL, NULL, hotplug_probe_cb, attach_cb, remove_cb) != 0) {
		SPDK_ERRLOG("spdk_nvme_probe() failed\n");
	}

	return -1;
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
				SPDK_ERRLOG("Maximum number of namespaces supported per NVMe controller is %zu. Unable to return all names of created bdevs\n",
					    *count);
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
		g_hotplug_poller = spdk_poller_register(bdev_nvme_hotplug, NULL,
							g_nvme_hotplug_poll_timeout_us);
	}

end:
	free(probe_ctx);
	return rc;
}

static void
bdev_nvme_library_fini(void)
{
	if (g_nvme_hotplug_enabled) {
		spdk_poller_unregister(&g_hotplug_poller);
	}
}

static int
nvme_ctrlr_create_bdevs(struct nvme_ctrlr *nvme_ctrlr)
{
	struct nvme_bdev	*bdev;
	struct spdk_nvme_ctrlr	*ctrlr = nvme_ctrlr->ctrlr;
	struct spdk_nvme_ns	*ns;
	const struct spdk_nvme_ctrlr_data *cdata;
	const struct spdk_uuid	*uuid;
	int			rc;
	int			bdev_created = 0;
	uint32_t		nsid;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	     nsid != 0; nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (!ns) {
			SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "Skipping invalid NS %d\n", nsid);
			continue;
		}

		if (!spdk_nvme_ns_is_active(ns)) {
			SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "Skipping inactive NS %d\n", nsid);
			continue;
		}

		bdev = calloc(1, sizeof(*bdev));
		if (!bdev) {
			break;
		}

		bdev->nvme_ctrlr = nvme_ctrlr;
		bdev->ns = ns;
		nvme_ctrlr->ref++;

		bdev->disk.name = spdk_sprintf_alloc("%sn%d", nvme_ctrlr->name, spdk_nvme_ns_get_id(ns));
		if (!bdev->disk.name) {
			free(bdev);
			break;
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

		uuid = spdk_nvme_ns_get_uuid(ns);
		if (uuid != NULL) {
			bdev->disk.uuid = *uuid;
		}

		bdev->disk.ctxt = bdev;
		bdev->disk.fn_table = &nvmelib_fn_table;
		bdev->disk.module = &nvme_if;
		rc = spdk_bdev_register(&bdev->disk);
		if (rc) {
			free(bdev->disk.name);
			free(bdev);
			break;
		}

		TAILQ_INSERT_TAIL(&g_nvme_bdevs, bdev, link);

		bdev_created++;
	}

	return (bdev_created > 0) ? 0 : -1;
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
		if (bio->iov_offset < iov->iov_len) {
			break;
		}

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
		    int direction, struct iovec *iov, int iovcnt, uint64_t lba_count,
		    uint64_t lba)
{
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

	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("%s failed: rc = %d\n", direction == BDEV_DISK_READ ? "readv" : "writev", rc);
	}
	return rc;
}

static int
bdev_nvme_unmap(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
		struct nvme_bdev_io *bio,
		uint64_t offset_blocks,
		uint64_t num_blocks)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_nvme_dsm_range dsm_ranges[SPDK_NVME_DATASET_MANAGEMENT_MAX_RANGES];
	struct spdk_nvme_dsm_range *range;
	uint64_t offset, remaining;
	uint64_t num_ranges_u64;
	uint16_t num_ranges;
	int rc;

	num_ranges_u64 = (num_blocks + SPDK_NVME_DATASET_MANAGEMENT_RANGE_MAX_BLOCKS - 1) /
			 SPDK_NVME_DATASET_MANAGEMENT_RANGE_MAX_BLOCKS;
	if (num_ranges_u64 > SPDK_COUNTOF(dsm_ranges)) {
		SPDK_ERRLOG("Unmap request for %" PRIu64 " blocks is too large\n", num_blocks);
		return -EINVAL;
	}
	num_ranges = (uint16_t)num_ranges_u64;

	offset = offset_blocks;
	remaining = num_blocks;
	range = &dsm_ranges[0];

	/* Fill max-size ranges until the remaining blocks fit into one range */
	while (remaining > SPDK_NVME_DATASET_MANAGEMENT_RANGE_MAX_BLOCKS) {
		range->attributes.raw = 0;
		range->length = SPDK_NVME_DATASET_MANAGEMENT_RANGE_MAX_BLOCKS;
		range->starting_lba = offset;

		offset += SPDK_NVME_DATASET_MANAGEMENT_RANGE_MAX_BLOCKS;
		remaining -= SPDK_NVME_DATASET_MANAGEMENT_RANGE_MAX_BLOCKS;
		range++;
	}

	/* Final range describes the remaining blocks */
	range->attributes.raw = 0;
	range->length = remaining;
	range->starting_lba = offset;

	rc = spdk_nvme_ns_cmd_dataset_management(nbdev->ns, nvme_ch->qpair,
			SPDK_NVME_DSM_ATTR_DEALLOCATE,
			dsm_ranges, num_ranges,
			bdev_nvme_queued_done, bio);

	return rc;
}

static int
bdev_nvme_admin_passthru(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
			 struct nvme_bdev_io *bio,
			 struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes)
{
	uint32_t max_xfer_size = spdk_nvme_ctrlr_get_max_xfer_size(nbdev->nvme_ctrlr->ctrlr);

	if (nbytes > max_xfer_size) {
		SPDK_ERRLOG("nbytes is greater than MDTS %" PRIu32 ".\n", max_xfer_size);
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
	uint32_t max_xfer_size = spdk_nvme_ctrlr_get_max_xfer_size(nbdev->nvme_ctrlr->ctrlr);

	if (nbytes > max_xfer_size) {
		SPDK_ERRLOG("nbytes is greater than MDTS %" PRIu32 ".\n", max_xfer_size);
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

static int
bdev_nvme_io_passthru_md(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
			 struct nvme_bdev_io *bio,
			 struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes, void *md_buf, size_t md_len)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);
	size_t nr_sectors = nbytes / spdk_nvme_ns_get_sector_size(nbdev->ns);
	uint32_t max_xfer_size = spdk_nvme_ctrlr_get_max_xfer_size(nbdev->nvme_ctrlr->ctrlr);

	if (nbytes > max_xfer_size) {
		SPDK_ERRLOG("nbytes is greater than MDTS %" PRIu32 ".\n", max_xfer_size);
		return -EINVAL;
	}

	if (md_len != nr_sectors * spdk_nvme_ns_get_md_size(nbdev->ns)) {
		SPDK_ERRLOG("invalid meta data buffer size\n");
		return -EINVAL;
	}

	/*
	 * Each NVMe bdev is a specific namespace, and all NVMe I/O commands require a nsid,
	 * so fill it out automatically.
	 */
	cmd->nsid = spdk_nvme_ns_get_id(nbdev->ns);

	return spdk_nvme_ctrlr_cmd_io_raw_with_md(nbdev->nvme_ctrlr->ctrlr, nvme_ch->qpair, cmd, buf,
			(uint32_t)nbytes, md_buf, bdev_nvme_queued_done, bio);
}

static void
bdev_nvme_get_spdk_running_config(FILE *fp)
{
	struct nvme_ctrlr	*nvme_ctrlr;

	fprintf(fp, "\n[Nvme]");
	fprintf(fp, "\n"
		"# NVMe Device Whitelist\n"
		"# Users may specify which NVMe devices to claim by their transport id.\n"
		"# See spdk_nvme_transport_id_parse() in spdk/nvme.h for the correct format.\n"
		"# The second argument is the assigned name, which can be referenced from\n"
		"# other sections in the configuration file. For NVMe devices, a namespace\n"
		"# is automatically appended to each name in the format <YourName>nY, where\n"
		"# Y is the NSID (starts at 1).\n");

	TAILQ_FOREACH(nvme_ctrlr, &g_nvme_ctrlrs, tailq) {
		const char *trtype;

		trtype = spdk_nvme_transport_id_trtype_str(nvme_ctrlr->trid.trtype);
		if (!trtype) {
			continue;
		}

		if (nvme_ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
			fprintf(fp, "TransportId \"trtype:%s traddr:%s\" %s\n",
				trtype,
				nvme_ctrlr->trid.traddr, nvme_ctrlr->name);
		} else {
			const char *adrfam;

			adrfam = spdk_nvme_transport_id_adrfam_str(nvme_ctrlr->trid.adrfam);

			if (adrfam) {
				fprintf(fp, "TransportId \"trtype:%s adrfam:%s traddr:%s trsvcid:%s subnqn:%s\" %s\n",
					trtype,	adrfam,
					nvme_ctrlr->trid.traddr, nvme_ctrlr->trid.trsvcid,
					nvme_ctrlr->trid.subnqn, nvme_ctrlr->name);
			} else {
				fprintf(fp, "TransportId \"trtype:%s traddr:%s trsvcid:%s subnqn:%s\" %s\n",
					trtype,
					nvme_ctrlr->trid.traddr, nvme_ctrlr->trid.trsvcid,
					nvme_ctrlr->trid.subnqn, nvme_ctrlr->name);
			}

		}
	}

	fprintf(fp, "\n"
		"# The number of attempts per I/O when an I/O fails. Do not include\n"
		"# this key to get the default behavior.\n");
	fprintf(fp, "RetryCount %d\n", spdk_nvme_retry_count);
	fprintf(fp, "\n"
		"# Timeout for each command, in seconds. If 0, don't track timeouts.\n");
	fprintf(fp, "Timeout %d\n", g_timeout);

	fprintf(fp, "\n"
		"# Action to take on command time out. Only valid when Timeout is greater\n"
		"# than 0. This may be 'Reset' to reset the controller, 'Abort' to abort\n"
		"# the command, or 'None' to just print a message but do nothing.\n"
		"# Admin command timeouts will always result in a reset.\n");
	switch (g_action_on_timeout) {
	case TIMEOUT_ACTION_NONE:
		fprintf(fp, "ActionOnTimeout None\n");
		break;
	case TIMEOUT_ACTION_RESET:
		fprintf(fp, "ActionOnTimeout Reset\n");
		break;
	case TIMEOUT_ACTION_ABORT:
		fprintf(fp, "ActionOnTimeout Abort\n");
		break;
	}

	fprintf(fp, "\n"
		"# Set how often the admin queue is polled for asynchronous events.\n"
		"# Units in microseconds.\n");
	fprintf(fp, "AdminPollRate %d\n", g_nvme_adminq_poll_timeout_us);
	fprintf(fp, "\n"
		"# Disable handling of hotplug (runtime insert and remove) events,\n"
		"# users can set to Yes if want to enable it.\n"
		"# Default: No\n");
	fprintf(fp, "HotplugEnable %s\n", g_nvme_hotplug_enabled ? "Yes" : "No");
	fprintf(fp, "\n"
		"# Set how often the hotplug is processed for insert and remove events."
		"# Units in microseconds.\n");
	fprintf(fp, "HotplugPollRate %d\n", g_nvme_hotplug_poll_timeout_us);

	fprintf(fp, "\n");
}

static int
bdev_nvme_config_json(struct spdk_json_write_ctx *w)
{
	struct nvme_ctrlr		*nvme_ctrlr;
	struct spdk_nvme_transport_id	*trid;
	const char			*adrfam;

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	TAILQ_FOREACH(nvme_ctrlr, &g_nvme_ctrlrs, tailq) {
		trid = &nvme_ctrlr->trid;

		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "method", "construct_nvme_bdev");

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "name", nvme_ctrlr->name);
		spdk_json_write_named_string(w, "trtype", spdk_nvme_transport_id_trtype_str(trid->trtype));
		spdk_json_write_named_string(w, "traddr", trid->traddr);

		adrfam = spdk_nvme_transport_id_adrfam_str(trid->adrfam);
		if (adrfam) {
			spdk_json_write_named_string(w, "adrfam", adrfam);
		}

		if (trid->trsvcid[0] != '\0') {
			spdk_json_write_named_string(w, "trsvcid", trid->trsvcid);
		}

		if (trid->subnqn[0] != '\0') {
			spdk_json_write_named_string(w, "subnqn", trid->subnqn);
		}

		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}

	pthread_mutex_unlock(&g_bdev_nvme_mutex);
	return 0;
}

struct spdk_nvme_ctrlr *
spdk_bdev_nvme_get_ctrlr(struct spdk_bdev *bdev)
{
	if (!bdev || bdev->module != &nvme_if) {
		return NULL;
	}

	return SPDK_CONTAINEROF(bdev, struct nvme_bdev, disk)->nvme_ctrlr->ctrlr;
}

SPDK_LOG_REGISTER_COMPONENT("bdev_nvme", SPDK_LOG_BDEV_NVME)
