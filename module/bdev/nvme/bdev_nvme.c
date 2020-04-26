/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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
#include "bdev_ocssd.h"

#include "spdk/config.h"
#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/bdev.h"
#include "spdk/json.h"
#include "spdk/nvme.h"
#include "spdk/nvme_ocssd.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/likely.h"
#include "spdk/util.h"

#include "spdk/bdev_module.h"
#include "spdk_internal/log.h"

#define SPDK_BDEV_NVME_DEFAULT_DELAY_CMD_SUBMIT true

static void bdev_nvme_get_spdk_running_config(FILE *fp);
static int bdev_nvme_config_json(struct spdk_json_write_ctx *w);

struct nvme_bdev_io {
	/** array of iovecs to transfer. */
	struct iovec *iovs;

	/** Number of iovecs in iovs array. */
	int iovcnt;

	/** Current iovec position. */
	int iovpos;

	/** Offset in current iovec. */
	uint32_t iov_offset;

	/** array of iovecs to transfer. */
	struct iovec *fused_iovs;

	/** Number of iovecs in iovs array. */
	int fused_iovcnt;

	/** Current iovec position. */
	int fused_iovpos;

	/** Offset in current iovec. */
	uint32_t fused_iov_offset;

	/** Saved status for admin passthru completion event, PI error verification, or intermediate compare-and-write status */
	struct spdk_nvme_cpl cpl;

	/** Originating thread */
	struct spdk_thread *orig_thread;

	/** Keeps track if first of fused commands was submitted */
	bool first_fused_submitted;
};

struct nvme_probe_ctx {
	size_t count;
	struct spdk_nvme_transport_id trids[NVME_MAX_CONTROLLERS];
	struct spdk_nvme_host_id hostids[NVME_MAX_CONTROLLERS];
	const char *names[NVME_MAX_CONTROLLERS];
	uint32_t prchk_flags[NVME_MAX_CONTROLLERS];
	const char *hostnqn;
};

struct nvme_probe_skip_entry {
	struct spdk_nvme_transport_id		trid;
	TAILQ_ENTRY(nvme_probe_skip_entry)	tailq;
};
/* All the controllers deleted by users via RPC are skipped by hotplug monitor */
static TAILQ_HEAD(, nvme_probe_skip_entry) g_skipped_nvme_ctrlrs = TAILQ_HEAD_INITIALIZER(
			g_skipped_nvme_ctrlrs);

static struct spdk_bdev_nvme_opts g_opts = {
	.action_on_timeout = SPDK_BDEV_NVME_TIMEOUT_ACTION_NONE,
	.timeout_us = 0,
	.retry_count = 4,
	.arbitration_burst = 0,
	.low_priority_weight = 0,
	.medium_priority_weight = 0,
	.high_priority_weight = 0,
	.nvme_adminq_poll_period_us = 10000ULL,
	.nvme_ioq_poll_period_us = 0,
	.io_queue_requests = 0,
	.delay_cmd_submit = SPDK_BDEV_NVME_DEFAULT_DELAY_CMD_SUBMIT,
};

#define NVME_HOTPLUG_POLL_PERIOD_MAX			10000000ULL
#define NVME_HOTPLUG_POLL_PERIOD_DEFAULT		100000ULL

static int g_hot_insert_nvme_controller_index = 0;
static uint64_t g_nvme_hotplug_poll_period_us = NVME_HOTPLUG_POLL_PERIOD_DEFAULT;
static bool g_nvme_hotplug_enabled = false;
static struct spdk_thread *g_bdev_nvme_init_thread;
static struct spdk_poller *g_hotplug_poller;
static struct spdk_nvme_probe_ctx *g_hotplug_probe_ctx;
static char *g_nvme_hostnqn = NULL;

static void nvme_ctrlr_populate_namespaces(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr,
		struct nvme_async_probe_ctx *ctx);
static void nvme_ctrlr_populate_namespaces_done(struct nvme_async_probe_ctx *ctx);
static int bdev_nvme_library_init(void);
static void bdev_nvme_library_fini(void);
static int bdev_nvme_readv(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
			   struct nvme_bdev_io *bio,
			   struct iovec *iov, int iovcnt, void *md, uint64_t lba_count, uint64_t lba);
static int bdev_nvme_no_pi_readv(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
				 struct nvme_bdev_io *bio,
				 struct iovec *iov, int iovcnt, void *md, uint64_t lba_count, uint64_t lba);
static int bdev_nvme_writev(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
			    struct nvme_bdev_io *bio,
			    struct iovec *iov, int iovcnt, void *md, uint64_t lba_count, uint64_t lba);
static int bdev_nvme_comparev(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
			      struct nvme_bdev_io *bio,
			      struct iovec *iov, int iovcnt, void *md, uint64_t lba_count, uint64_t lba);
static int bdev_nvme_comparev_and_writev(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
		struct nvme_bdev_io *bio, struct iovec *cmp_iov, int cmp_iovcnt, struct iovec *write_iov,
		int write_iovcnt, void *md, uint64_t lba_count, uint64_t lba);
static int bdev_nvme_admin_passthru(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
				    struct nvme_bdev_io *bio,
				    struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes);
static int bdev_nvme_io_passthru(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
				 struct nvme_bdev_io *bio,
				 struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes);
static int bdev_nvme_io_passthru_md(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
				    struct nvme_bdev_io *bio,
				    struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes, void *md_buf, size_t md_len);
static int bdev_nvme_reset(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr, struct nvme_bdev_io *bio);

typedef void (*populate_namespace_fn)(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr,
				      struct nvme_bdev_ns *nvme_ns, struct nvme_async_probe_ctx *ctx);
static void nvme_ctrlr_populate_standard_namespace(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr,
		struct nvme_bdev_ns *nvme_ns, struct nvme_async_probe_ctx *ctx);

static populate_namespace_fn g_populate_namespace_fn[] = {
	NULL,
	nvme_ctrlr_populate_standard_namespace,
	bdev_ocssd_populate_namespace,
};

typedef void (*depopulate_namespace_fn)(struct nvme_bdev_ns *ns);
static void nvme_ctrlr_depopulate_standard_namespace(struct nvme_bdev_ns *ns);

static depopulate_namespace_fn g_depopulate_namespace_fn[] = {
	NULL,
	nvme_ctrlr_depopulate_standard_namespace,
	bdev_ocssd_depopulate_namespace,
};

typedef void (*config_json_namespace_fn)(struct spdk_json_write_ctx *w, struct nvme_bdev_ns *ns);
static void nvme_ctrlr_config_json_standard_namespace(struct spdk_json_write_ctx *w,
		struct nvme_bdev_ns *ns);

static config_json_namespace_fn g_config_json_namespace_fn[] = {
	NULL,
	nvme_ctrlr_config_json_standard_namespace,
	bdev_ocssd_namespace_config_json,
};

struct spdk_nvme_qpair *
spdk_bdev_nvme_get_io_qpair(struct spdk_io_channel *ctrlr_io_ch)
{
	struct nvme_io_channel *nvme_ch;

	nvme_ch =  spdk_io_channel_get_ctx(ctrlr_io_ch);

	return nvme_ch->qpair;
}

static int
bdev_nvme_get_ctx_size(void)
{
	return sizeof(struct nvme_bdev_io);
}

static struct spdk_bdev_module nvme_if = {
	.name = "nvme",
	.async_fini = true,
	.module_init = bdev_nvme_library_init,
	.module_fini = bdev_nvme_library_fini,
	.config_text = bdev_nvme_get_spdk_running_config,
	.config_json = bdev_nvme_config_json,
	.get_ctx_size = bdev_nvme_get_ctx_size,

};
SPDK_BDEV_MODULE_REGISTER(nvme, &nvme_if)

static void
bdev_nvme_disconnected_qpair_cb(struct spdk_nvme_qpair *qpair, void *poll_group_ctx)
{
	SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "qpar %p is disconnected, attempting reconnect.\n", qpair);
	/*
	 * Currently, just try to reconnect indefinitely. If we are doing a reset, the reset will
	 * reconnect a qpair and we will stop getting a callback for this one.
	 */
	spdk_nvme_ctrlr_reconnect_io_qpair(qpair);
}

static int
bdev_nvme_poll(void *arg)
{
	struct nvme_bdev_poll_group *group = arg;
	int64_t num_completions;

	if (group->collect_spin_stat && group->start_ticks == 0) {
		group->start_ticks = spdk_get_ticks();
	}

	num_completions = spdk_nvme_poll_group_process_completions(group->group, 0,
			  bdev_nvme_disconnected_qpair_cb);
	if (group->collect_spin_stat) {
		if (num_completions > 0) {
			if (group->end_ticks != 0) {
				group->spin_ticks += (group->end_ticks - group->start_ticks);
				group->end_ticks = 0;
			}
			group->start_ticks = 0;
		} else {
			group->end_ticks = spdk_get_ticks();
		}
	}

	return num_completions;
}

static int
bdev_nvme_poll_adminq(void *arg)
{
	int32_t rc;
	struct spdk_nvme_ctrlr *ctrlr = arg;
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;

	rc = spdk_nvme_ctrlr_process_admin_completions(ctrlr);

	if (rc < 0) {
		nvme_bdev_ctrlr = nvme_bdev_ctrlr_get(spdk_nvme_ctrlr_get_transport_id(ctrlr));
		assert(nvme_bdev_ctrlr != NULL);
		bdev_nvme_reset(nvme_bdev_ctrlr, NULL);
	}

	return rc;
}

static int
bdev_nvme_destruct(void *ctx)
{
	struct nvme_bdev *nvme_disk = ctx;

	nvme_bdev_detach_bdev_from_ns(nvme_disk);

	free(nvme_disk->disk.name);
	free(nvme_disk);

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
_bdev_nvme_complete_pending_resets(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(_ch);
	struct spdk_bdev_io *bdev_io;
	enum spdk_bdev_io_status status = SPDK_BDEV_IO_STATUS_SUCCESS;

	/* A NULL ctx means success. */
	if (spdk_io_channel_iter_get_ctx(i) != NULL) {
		status = SPDK_BDEV_IO_STATUS_FAILED;
	}

	while (!TAILQ_EMPTY(&nvme_ch->pending_resets)) {
		bdev_io = TAILQ_FIRST(&nvme_ch->pending_resets);
		TAILQ_REMOVE(&nvme_ch->pending_resets, bdev_io, module_link);
		spdk_bdev_io_complete(bdev_io, status);
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
_bdev_nvme_reset_complete(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr, int rc)
{
	/* we are using the for_each_channel cb_arg like a return code here. */
	/* If it's zero, we succeeded, otherwise, the reset failed. */
	void *cb_arg = NULL;

	if (rc) {
		cb_arg = (void *)0x1;
		SPDK_ERRLOG("Resetting controller failed.\n");
	} else {
		SPDK_NOTICELOG("Resetting controller successful.\n");
	}

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	nvme_bdev_ctrlr->resetting = false;
	pthread_mutex_unlock(&g_bdev_nvme_mutex);
	/* Make sure we clear any pending resets before returning. */
	spdk_for_each_channel(nvme_bdev_ctrlr,
			      _bdev_nvme_complete_pending_resets,
			      cb_arg, NULL);
}

static void
_bdev_nvme_reset_create_qpairs_done(struct spdk_io_channel_iter *i, int status)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr = spdk_io_channel_iter_get_io_device(i);
	void *ctx = spdk_io_channel_iter_get_ctx(i);
	int rc = SPDK_BDEV_IO_STATUS_SUCCESS;

	if (status) {
		rc = SPDK_BDEV_IO_STATUS_FAILED;
	}
	if (ctx) {
		spdk_bdev_io_complete(spdk_bdev_io_from_ctx(ctx), rc);
	}
	_bdev_nvme_reset_complete(nvme_bdev_ctrlr, status);
}

static void
_bdev_nvme_reset_create_qpair(struct spdk_io_channel_iter *i)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr = spdk_io_channel_iter_get_io_device(i);
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(_ch);
	struct spdk_nvme_io_qpair_opts opts;

	spdk_nvme_ctrlr_get_default_io_qpair_opts(nvme_bdev_ctrlr->ctrlr, &opts, sizeof(opts));
	opts.delay_cmd_submit = g_opts.delay_cmd_submit;
	opts.create_only = true;

	nvme_ch->qpair = spdk_nvme_ctrlr_alloc_io_qpair(nvme_bdev_ctrlr->ctrlr, &opts, sizeof(opts));
	if (!nvme_ch->qpair) {
		spdk_for_each_channel_continue(i, -1);
		return;
	}

	assert(nvme_ch->group != NULL);
	if (spdk_nvme_poll_group_add(nvme_ch->group->group, nvme_ch->qpair) != 0) {
		SPDK_ERRLOG("Unable to begin polling on NVMe Channel.\n");
		spdk_nvme_ctrlr_free_io_qpair(nvme_ch->qpair);
		spdk_for_each_channel_continue(i, -1);
		return;
	}

	if (spdk_nvme_ctrlr_connect_io_qpair(nvme_bdev_ctrlr->ctrlr, nvme_ch->qpair)) {
		SPDK_ERRLOG("Unable to connect I/O qpair.\n");
		spdk_nvme_poll_group_remove(nvme_ch->group->group, nvme_ch->qpair);
		spdk_nvme_ctrlr_free_io_qpair(nvme_ch->qpair);
		spdk_for_each_channel_continue(i, -1);
		return;
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
_bdev_nvme_reset(struct spdk_io_channel_iter *i, int status)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr = spdk_io_channel_iter_get_io_device(i);
	struct nvme_bdev_io *bio = spdk_io_channel_iter_get_ctx(i);
	int rc;

	if (status) {
		if (bio) {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bio), SPDK_BDEV_IO_STATUS_FAILED);
		}
		_bdev_nvme_reset_complete(nvme_bdev_ctrlr, status);
		return;
	}

	rc = spdk_nvme_ctrlr_reset(nvme_bdev_ctrlr->ctrlr);
	if (rc != 0) {
		if (bio) {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bio), SPDK_BDEV_IO_STATUS_FAILED);
		}
		_bdev_nvme_reset_complete(nvme_bdev_ctrlr, rc);
		return;
	}

	/* Recreate all of the I/O queue pairs */
	spdk_for_each_channel(nvme_bdev_ctrlr,
			      _bdev_nvme_reset_create_qpair,
			      bio,
			      _bdev_nvme_reset_create_qpairs_done);


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
bdev_nvme_reset(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr, struct nvme_bdev_io *bio)
{
	struct spdk_io_channel *ch;
	struct nvme_io_channel *nvme_ch;

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	if (nvme_bdev_ctrlr->destruct) {
		/* Don't bother resetting if the controller is in the process of being destructed. */
		if (bio) {
			spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bio), SPDK_BDEV_IO_STATUS_FAILED);
		}
		pthread_mutex_unlock(&g_bdev_nvme_mutex);
		return 0;
	}

	if (!nvme_bdev_ctrlr->resetting) {
		nvme_bdev_ctrlr->resetting = true;
	} else {
		pthread_mutex_unlock(&g_bdev_nvme_mutex);
		SPDK_NOTICELOG("Unable to perform reset, already in progress.\n");
		/*
		 * The internal reset calls won't be queued. This is on purpose so that we don't
		 * interfere with the app framework reset strategy. i.e. we are deferring to the
		 * upper level. If they are in the middle of a reset, we won't try to schedule another one.
		 */
		if (bio) {
			ch = spdk_get_io_channel(nvme_bdev_ctrlr);
			assert(ch != NULL);
			nvme_ch = spdk_io_channel_get_ctx(ch);
			TAILQ_INSERT_TAIL(&nvme_ch->pending_resets, spdk_bdev_io_from_ctx(bio), module_link);
			spdk_put_io_channel(ch);
		}
		return 0;
	}

	pthread_mutex_unlock(&g_bdev_nvme_mutex);
	/* First, delete all NVMe I/O queue pairs. */
	spdk_for_each_channel(nvme_bdev_ctrlr,
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
bdev_nvme_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		     bool success)
{
	int ret;

	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	ret = bdev_nvme_readv((struct nvme_bdev *)bdev_io->bdev->ctxt,
			      ch,
			      (struct nvme_bdev_io *)bdev_io->driver_ctx,
			      bdev_io->u.bdev.iovs,
			      bdev_io->u.bdev.iovcnt,
			      bdev_io->u.bdev.md_buf,
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
	struct nvme_bdev *nbdev = (struct nvme_bdev *)bdev_io->bdev->ctxt;
	struct nvme_bdev_io *nbdev_io = (struct nvme_bdev_io *)bdev_io->driver_ctx;

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
		return bdev_nvme_writev(nbdev,
					ch,
					nbdev_io,
					bdev_io->u.bdev.iovs,
					bdev_io->u.bdev.iovcnt,
					bdev_io->u.bdev.md_buf,
					bdev_io->u.bdev.num_blocks,
					bdev_io->u.bdev.offset_blocks);

	case SPDK_BDEV_IO_TYPE_COMPARE:
		return bdev_nvme_comparev(nbdev,
					  ch,
					  nbdev_io,
					  bdev_io->u.bdev.iovs,
					  bdev_io->u.bdev.iovcnt,
					  bdev_io->u.bdev.md_buf,
					  bdev_io->u.bdev.num_blocks,
					  bdev_io->u.bdev.offset_blocks);

	case SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE:
		return bdev_nvme_comparev_and_writev(nbdev,
						     ch,
						     nbdev_io,
						     bdev_io->u.bdev.iovs,
						     bdev_io->u.bdev.iovcnt,
						     bdev_io->u.bdev.fused_iovs,
						     bdev_io->u.bdev.fused_iovcnt,
						     bdev_io->u.bdev.md_buf,
						     bdev_io->u.bdev.num_blocks,
						     bdev_io->u.bdev.offset_blocks);

	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		return bdev_nvme_unmap(nbdev,
				       ch,
				       nbdev_io,
				       bdev_io->u.bdev.offset_blocks,
				       bdev_io->u.bdev.num_blocks);

	case SPDK_BDEV_IO_TYPE_UNMAP:
		return bdev_nvme_unmap(nbdev,
				       ch,
				       nbdev_io,
				       bdev_io->u.bdev.offset_blocks,
				       bdev_io->u.bdev.num_blocks);

	case SPDK_BDEV_IO_TYPE_RESET:
		return bdev_nvme_reset(nbdev->nvme_bdev_ctrlr, nbdev_io);

	case SPDK_BDEV_IO_TYPE_FLUSH:
		return bdev_nvme_flush(nbdev,
				       nbdev_io,
				       bdev_io->u.bdev.offset_blocks,
				       bdev_io->u.bdev.num_blocks);

	case SPDK_BDEV_IO_TYPE_NVME_ADMIN:
		return bdev_nvme_admin_passthru(nbdev,
						ch,
						nbdev_io,
						&bdev_io->u.nvme_passthru.cmd,
						bdev_io->u.nvme_passthru.buf,
						bdev_io->u.nvme_passthru.nbytes);

	case SPDK_BDEV_IO_TYPE_NVME_IO:
		return bdev_nvme_io_passthru(nbdev,
					     ch,
					     nbdev_io,
					     &bdev_io->u.nvme_passthru.cmd,
					     bdev_io->u.nvme_passthru.buf,
					     bdev_io->u.nvme_passthru.nbytes);

	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		return bdev_nvme_io_passthru_md(nbdev,
						ch,
						nbdev_io,
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

	case SPDK_BDEV_IO_TYPE_COMPARE:
		return spdk_nvme_ns_supports_compare(nbdev->nvme_ns->ns);

	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		return spdk_nvme_ns_get_md_size(nbdev->nvme_ns->ns) ? true : false;

	case SPDK_BDEV_IO_TYPE_UNMAP:
		cdata = spdk_nvme_ctrlr_get_data(nbdev->nvme_bdev_ctrlr->ctrlr);
		return cdata->oncs.dsm;

	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		cdata = spdk_nvme_ctrlr_get_data(nbdev->nvme_bdev_ctrlr->ctrlr);
		/*
		 * If an NVMe controller guarantees reading unallocated blocks returns zero,
		 * we can implement WRITE_ZEROES as an NVMe deallocate command.
		 */
		if (cdata->oncs.dsm &&
		    spdk_nvme_ns_get_dealloc_logical_block_read_value(nbdev->nvme_ns->ns) ==
		    SPDK_NVME_DEALLOC_READ_00) {
			return true;
		}
		/*
		 * The NVMe controller write_zeroes function is currently not used by our driver.
		 * If a user submits an arbitrarily large write_zeroes request to the controller, the request will fail.
		 * Until this is resolved, we only claim support for write_zeroes if deallocated blocks return 0's when read.
		 */
		return false;

	case SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE:
		if (spdk_nvme_ctrlr_get_flags(nbdev->nvme_bdev_ctrlr->ctrlr) &
		    SPDK_NVME_CTRLR_COMPARE_AND_WRITE_SUPPORTED) {
			return true;
		}
		return false;

	default:
		return false;
	}
}

static int
bdev_nvme_create_cb(void *io_device, void *ctx_buf)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr = io_device;
	struct nvme_io_channel *ch = ctx_buf;
	struct spdk_nvme_io_qpair_opts opts;
	struct spdk_io_channel *pg_ch = NULL;
	int rc;

	spdk_nvme_ctrlr_get_default_io_qpair_opts(nvme_bdev_ctrlr->ctrlr, &opts, sizeof(opts));
	opts.delay_cmd_submit = g_opts.delay_cmd_submit;
	opts.io_queue_requests = spdk_max(g_opts.io_queue_requests, opts.io_queue_requests);
	opts.create_only = true;
	g_opts.io_queue_requests = opts.io_queue_requests;

	ch->qpair = spdk_nvme_ctrlr_alloc_io_qpair(nvme_bdev_ctrlr->ctrlr, &opts, sizeof(opts));

	if (ch->qpair == NULL) {
		return -1;
	}

	if (spdk_nvme_ctrlr_is_ocssd_supported(nvme_bdev_ctrlr->ctrlr)) {
		if (bdev_ocssd_create_io_channel(ch)) {
			goto err;
		}
	}

	pg_ch = spdk_get_io_channel(&g_nvme_bdev_ctrlrs);
	if (!pg_ch) {
		goto err;
	}

	ch->group = spdk_io_channel_get_ctx(pg_ch);
	if (spdk_nvme_poll_group_add(ch->group->group, ch->qpair) != 0) {
		goto err;
	}

	rc = spdk_nvme_ctrlr_connect_io_qpair(nvme_bdev_ctrlr->ctrlr, ch->qpair);
	if (rc) {
		spdk_nvme_poll_group_remove(ch->group->group, ch->qpair);
		goto err;
	}

#ifdef SPDK_CONFIG_VTUNE
	ch->group->collect_spin_stat = true;
#else
	ch->group->collect_spin_stat = false;
#endif

	TAILQ_INIT(&ch->pending_resets);
	return 0;

err:
	if (pg_ch) {
		spdk_put_io_channel(pg_ch);
	}
	spdk_nvme_ctrlr_free_io_qpair(ch->qpair);
	return -1;
}

static void
bdev_nvme_destroy_cb(void *io_device, void *ctx_buf)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr = io_device;
	struct nvme_io_channel *ch = ctx_buf;
	struct nvme_bdev_poll_group *group;

	group = ch->group;
	assert(group != NULL);

	if (spdk_nvme_ctrlr_is_ocssd_supported(nvme_bdev_ctrlr->ctrlr)) {
		bdev_ocssd_destroy_io_channel(ch);
	}

	if (ch->qpair != NULL) {
		spdk_nvme_poll_group_remove(group->group, ch->qpair);
	}
	spdk_put_io_channel(spdk_io_channel_from_ctx(group));

	spdk_nvme_ctrlr_free_io_qpair(ch->qpair);
}

static int
bdev_nvme_poll_group_create_cb(void *io_device, void *ctx_buf)
{
	struct nvme_bdev_poll_group *group = ctx_buf;

	group->group = spdk_nvme_poll_group_create(group);
	if (group->group == NULL) {
		return -1;
	}

	group->poller = spdk_poller_register(bdev_nvme_poll, group, g_opts.nvme_ioq_poll_period_us);

	if (group->poller == NULL) {
		spdk_nvme_poll_group_destroy(group->group);
		return -1;
	}

	return 0;
}

static void
bdev_nvme_poll_group_destroy_cb(void *io_device, void *ctx_buf)
{
	struct nvme_bdev_poll_group *group = ctx_buf;

	spdk_poller_unregister(&group->poller);
	if (spdk_nvme_poll_group_destroy(group->group)) {
		SPDK_ERRLOG("Unable to destroy a poll group for the NVMe bdev module.");
		assert(false);
	}
}

static struct spdk_io_channel *
bdev_nvme_get_io_channel(void *ctx)
{
	struct nvme_bdev *nvme_bdev = ctx;

	return spdk_get_io_channel(nvme_bdev->nvme_bdev_ctrlr);
}

static int
bdev_nvme_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct nvme_bdev *nvme_bdev = ctx;
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr = nvme_bdev->nvme_bdev_ctrlr;
	const struct spdk_nvme_ctrlr_data *cdata;
	struct spdk_nvme_ns *ns;
	union spdk_nvme_vs_register vs;
	union spdk_nvme_csts_register csts;
	char buf[128];

	cdata = spdk_nvme_ctrlr_get_data(nvme_bdev->nvme_bdev_ctrlr->ctrlr);
	vs = spdk_nvme_ctrlr_get_regs_vs(nvme_bdev->nvme_bdev_ctrlr->ctrlr);
	csts = spdk_nvme_ctrlr_get_regs_csts(nvme_bdev->nvme_bdev_ctrlr->ctrlr);
	ns = nvme_bdev->nvme_ns->ns;

	spdk_json_write_named_object_begin(w, "nvme");

	if (nvme_bdev_ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
		spdk_json_write_named_string(w, "pci_address", nvme_bdev_ctrlr->trid.traddr);
	}

	spdk_json_write_named_object_begin(w, "trid");

	nvme_bdev_dump_trid_json(&nvme_bdev_ctrlr->trid, w);

	spdk_json_write_object_end(w);

#ifdef SPDK_CONFIG_NVME_CUSE
	size_t cuse_name_size = 128;
	char cuse_name[cuse_name_size];

	int rc = spdk_nvme_cuse_get_ns_name(nvme_bdev->nvme_bdev_ctrlr->ctrlr, spdk_nvme_ns_get_id(ns),
					    cuse_name, &cuse_name_size);
	if (rc == 0) {
		spdk_json_write_named_string(w, "cuse_device", cuse_name);
	}
#endif

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

	if (cdata->oacs.security) {
		spdk_json_write_named_object_begin(w, "security");

		spdk_json_write_named_bool(w, "opal", spdk_opal_supported(nvme_bdev_ctrlr->opal_dev));

		spdk_json_write_object_end(w);
	}

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
	struct nvme_bdev_poll_group *group = nvme_ch->group;
	uint64_t spin_time;

	if (!group || !group->collect_spin_stat) {
		return 0;
	}

	if (group->end_ticks != 0) {
		group->spin_ticks += (group->end_ticks - group->start_ticks);
		group->end_ticks = 0;
	}

	spin_time = (group->spin_ticks * 1000000ULL) / spdk_get_ticks_hz();
	group->start_ticks = 0;
	group->spin_ticks = 0;

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

static void
nvme_ctrlr_populate_standard_namespace(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr,
				       struct nvme_bdev_ns *nvme_ns, struct nvme_async_probe_ctx *ctx)
{
	struct spdk_nvme_ctrlr	*ctrlr = nvme_bdev_ctrlr->ctrlr;
	struct nvme_bdev	*bdev;
	struct spdk_nvme_ns	*ns;
	const struct spdk_uuid	*uuid;
	const struct spdk_nvme_ctrlr_data *cdata;
	const struct spdk_nvme_ns_data *nsdata;
	int			rc;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nvme_ns->id);
	if (!ns) {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "Invalid NS %d\n", nvme_ns->id);
		nvme_ctrlr_populate_namespace_done(ctx, nvme_ns, -EINVAL);
		return;
	}

	bdev = calloc(1, sizeof(*bdev));
	if (!bdev) {
		SPDK_ERRLOG("bdev calloc() failed\n");
		nvme_ctrlr_populate_namespace_done(ctx, nvme_ns, -ENOMEM);
		return;
	}

	bdev->nvme_bdev_ctrlr = nvme_bdev_ctrlr;
	nvme_ns->ns = ns;
	bdev->nvme_ns = nvme_ns;

	bdev->disk.name = spdk_sprintf_alloc("%sn%d", nvme_bdev_ctrlr->name, spdk_nvme_ns_get_id(ns));
	if (!bdev->disk.name) {
		free(bdev);
		nvme_ctrlr_populate_namespace_done(ctx, nvme_ns, -ENOMEM);
		return;
	}
	bdev->disk.product_name = "NVMe disk";

	bdev->disk.write_cache = 0;
	if (cdata->vwc.present) {
		/* Enable if the Volatile Write Cache exists */
		bdev->disk.write_cache = 1;
	}
	bdev->disk.blocklen = spdk_nvme_ns_get_extended_sector_size(ns);
	bdev->disk.blockcnt = spdk_nvme_ns_get_num_sectors(ns);
	bdev->disk.optimal_io_boundary = spdk_nvme_ns_get_optimal_io_boundary(ns);

	uuid = spdk_nvme_ns_get_uuid(ns);
	if (uuid != NULL) {
		bdev->disk.uuid = *uuid;
	}

	nsdata = spdk_nvme_ns_get_data(ns);

	bdev->disk.md_len = spdk_nvme_ns_get_md_size(ns);
	if (bdev->disk.md_len != 0) {
		bdev->disk.md_interleave = nsdata->flbas.extended;
		bdev->disk.dif_type = (enum spdk_dif_type)spdk_nvme_ns_get_pi_type(ns);
		if (bdev->disk.dif_type != SPDK_DIF_DISABLE) {
			bdev->disk.dif_is_head_of_md = nsdata->dps.md_start;
			bdev->disk.dif_check_flags = nvme_bdev_ctrlr->prchk_flags;
		}
	}

	if (!bdev_nvme_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE)) {
		bdev->disk.acwu = 0;
	} else if (nsdata->nsfeat.ns_atomic_write_unit) {
		bdev->disk.acwu = nsdata->nacwu;
	} else {
		bdev->disk.acwu = cdata->acwu;
	}

	bdev->disk.ctxt = bdev;
	bdev->disk.fn_table = &nvmelib_fn_table;
	bdev->disk.module = &nvme_if;
	rc = spdk_bdev_register(&bdev->disk);
	if (rc) {
		free(bdev->disk.name);
		free(bdev);
		nvme_ctrlr_populate_namespace_done(ctx, nvme_ns, rc);
		return;
	}

	nvme_bdev_attach_bdev_to_ns(nvme_ns, bdev);
	nvme_ctrlr_populate_namespace_done(ctx, nvme_ns, 0);
}

static bool
hotplug_probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		 struct spdk_nvme_ctrlr_opts *opts)
{
	struct nvme_probe_skip_entry *entry;

	TAILQ_FOREACH(entry, &g_skipped_nvme_ctrlrs, tailq) {
		if (spdk_nvme_transport_id_compare(trid, &entry->trid) == 0) {
			return false;
		}
	}

	opts->arbitration_burst = (uint8_t)g_opts.arbitration_burst;
	opts->low_priority_weight = (uint8_t)g_opts.low_priority_weight;
	opts->medium_priority_weight = (uint8_t)g_opts.medium_priority_weight;
	opts->high_priority_weight = (uint8_t)g_opts.high_priority_weight;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "Attaching to %s\n", trid->traddr);

	return true;
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	struct nvme_probe_ctx *ctx = cb_ctx;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "Probing device %s\n", trid->traddr);

	if (nvme_bdev_ctrlr_get(trid)) {
		SPDK_ERRLOG("A controller with the provided trid (traddr: %s) already exists.\n",
			    trid->traddr);
		return false;
	}

	if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE) {
		bool claim_device = false;
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

	if (ctx->hostnqn) {
		snprintf(opts->hostnqn, sizeof(opts->hostnqn), "%s", ctx->hostnqn);
	}

	opts->arbitration_burst = (uint8_t)g_opts.arbitration_burst;
	opts->low_priority_weight = (uint8_t)g_opts.low_priority_weight;
	opts->medium_priority_weight = (uint8_t)g_opts.medium_priority_weight;
	opts->high_priority_weight = (uint8_t)g_opts.high_priority_weight;

	return true;
}

static void
spdk_nvme_abort_cpl(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_WARNLOG("Abort failed. Resetting controller.\n");
		nvme_bdev_ctrlr = nvme_bdev_ctrlr_get(spdk_nvme_ctrlr_get_transport_id(ctrlr));
		assert(nvme_bdev_ctrlr != NULL);
		bdev_nvme_reset(nvme_bdev_ctrlr, NULL);
	}
}

static void
timeout_cb(void *cb_arg, struct spdk_nvme_ctrlr *ctrlr,
	   struct spdk_nvme_qpair *qpair, uint16_t cid)
{
	int rc;
	union spdk_nvme_csts_register csts;
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;

	SPDK_WARNLOG("Warning: Detected a timeout. ctrlr=%p qpair=%p cid=%u\n", ctrlr, qpair, cid);

	csts = spdk_nvme_ctrlr_get_regs_csts(ctrlr);
	if (csts.bits.cfs) {
		SPDK_ERRLOG("Controller Fatal Status, reset required\n");
		nvme_bdev_ctrlr = nvme_bdev_ctrlr_get(spdk_nvme_ctrlr_get_transport_id(ctrlr));
		assert(nvme_bdev_ctrlr != NULL);
		bdev_nvme_reset(nvme_bdev_ctrlr, NULL);
		return;
	}

	switch (g_opts.action_on_timeout) {
	case SPDK_BDEV_NVME_TIMEOUT_ACTION_ABORT:
		if (qpair) {
			rc = spdk_nvme_ctrlr_cmd_abort(ctrlr, qpair, cid,
						       spdk_nvme_abort_cpl, ctrlr);
			if (rc == 0) {
				return;
			}

			SPDK_ERRLOG("Unable to send abort. Resetting.\n");
		}

	/* FALLTHROUGH */
	case SPDK_BDEV_NVME_TIMEOUT_ACTION_RESET:
		nvme_bdev_ctrlr = nvme_bdev_ctrlr_get(spdk_nvme_ctrlr_get_transport_id(ctrlr));
		assert(nvme_bdev_ctrlr != NULL);
		bdev_nvme_reset(nvme_bdev_ctrlr, NULL);
		break;
	case SPDK_BDEV_NVME_TIMEOUT_ACTION_NONE:
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "No action for nvme controller timeout.\n");
		break;
	default:
		SPDK_ERRLOG("An invalid timeout action value is found.\n");
		break;
	}
}

void
nvme_ctrlr_depopulate_namespace_done(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr)
{
	pthread_mutex_lock(&g_bdev_nvme_mutex);
	nvme_bdev_ctrlr->ref--;

	if (nvme_bdev_ctrlr->ref == 0 && nvme_bdev_ctrlr->destruct) {
		pthread_mutex_unlock(&g_bdev_nvme_mutex);
		nvme_bdev_ctrlr_destruct(nvme_bdev_ctrlr);
		return;
	}

	pthread_mutex_unlock(&g_bdev_nvme_mutex);
}

static void
nvme_ctrlr_depopulate_standard_namespace(struct nvme_bdev_ns *ns)
{
	struct nvme_bdev *bdev, *tmp;

	TAILQ_FOREACH_SAFE(bdev, &ns->bdevs, tailq, tmp) {
		spdk_bdev_unregister(&bdev->disk, NULL, NULL);
	}

	ns->populated = false;

	nvme_ctrlr_depopulate_namespace_done(ns->ctrlr);
}

static void nvme_ctrlr_populate_namespace(struct nvme_bdev_ctrlr *ctrlr, struct nvme_bdev_ns *ns,
		struct nvme_async_probe_ctx *ctx)
{
	g_populate_namespace_fn[ns->type](ctrlr, ns, ctx);
}

static void nvme_ctrlr_depopulate_namespace(struct nvme_bdev_ctrlr *ctrlr, struct nvme_bdev_ns *ns)
{
	g_depopulate_namespace_fn[ns->type](ns);
}

void
nvme_ctrlr_populate_namespace_done(struct nvme_async_probe_ctx *ctx,
				   struct nvme_bdev_ns *ns, int rc)
{
	if (rc == 0) {
		ns->populated = true;
		pthread_mutex_lock(&g_bdev_nvme_mutex);
		ns->ctrlr->ref++;
		pthread_mutex_unlock(&g_bdev_nvme_mutex);
	} else {
		memset(ns, 0, sizeof(*ns));
	}

	if (ctx) {
		ctx->populates_in_progress--;
		if (ctx->populates_in_progress == 0) {
			nvme_ctrlr_populate_namespaces_done(ctx);
		}
	}
}

static void
nvme_ctrlr_populate_namespaces(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr,
			       struct nvme_async_probe_ctx *ctx)
{
	struct spdk_nvme_ctrlr	*ctrlr = nvme_bdev_ctrlr->ctrlr;
	struct nvme_bdev_ns	*ns;
	struct spdk_nvme_ns	*nvme_ns;
	struct nvme_bdev	*bdev;
	uint32_t		i;
	int			rc;
	uint64_t		num_sectors;
	bool			ns_is_active;

	if (ctx) {
		/* Initialize this count to 1 to handle the populate functions
		 * calling nvme_ctrlr_populate_namespace_done() immediately.
		 */
		ctx->populates_in_progress = 1;
	}

	for (i = 0; i < nvme_bdev_ctrlr->num_ns; i++) {
		uint32_t	nsid = i + 1;

		ns = nvme_bdev_ctrlr->namespaces[i];
		ns_is_active = spdk_nvme_ctrlr_is_active_ns(ctrlr, nsid);

		if (ns->populated && ns_is_active && ns->type == NVME_BDEV_NS_STANDARD) {
			/* NS is still there but attributes may have changed */
			nvme_ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
			num_sectors = spdk_nvme_ns_get_num_sectors(nvme_ns);
			bdev = TAILQ_FIRST(&ns->bdevs);
			if (bdev->disk.blockcnt != num_sectors) {
				SPDK_NOTICELOG("NSID %u is resized: bdev name %s, old size %lu, new size %lu\n",
					       nsid,
					       bdev->disk.name,
					       bdev->disk.blockcnt,
					       num_sectors);
				rc = spdk_bdev_notify_blockcnt_change(&bdev->disk, num_sectors);
				if (rc != 0) {
					SPDK_ERRLOG("Could not change num blocks for nvme bdev: name %s, errno: %d.\n",
						    bdev->disk.name, rc);
				}
			}
		}

		if (!ns->populated && ns_is_active) {
			ns->id = nsid;
			ns->ctrlr = nvme_bdev_ctrlr;
			if (spdk_nvme_ctrlr_is_ocssd_supported(ctrlr)) {
				ns->type = NVME_BDEV_NS_OCSSD;
			} else {
				ns->type = NVME_BDEV_NS_STANDARD;
			}

			TAILQ_INIT(&ns->bdevs);

			if (ctx) {
				ctx->populates_in_progress++;
			}
			nvme_ctrlr_populate_namespace(nvme_bdev_ctrlr, ns, ctx);
		}

		if (ns->populated && !ns_is_active) {
			nvme_ctrlr_depopulate_namespace(nvme_bdev_ctrlr, ns);
		}
	}

	if (ctx) {
		/* Decrement this count now that the loop is over to account
		 * for the one we started with.  If the count is then 0, we
		 * know any populate_namespace functions completed immediately,
		 * so we'll kick the callback here.
		 */
		ctx->populates_in_progress--;
		if (ctx->populates_in_progress == 0) {
			nvme_ctrlr_populate_namespaces_done(ctx);
		}
	}

}

static void
aer_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr		= arg;
	union spdk_nvme_async_event_completion	event;

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_WARNLOG("AER request execute failed");
		return;
	}

	event.raw = cpl->cdw0;
	if ((event.bits.async_event_type == SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE) &&
	    (event.bits.async_event_info == SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGED)) {
		nvme_ctrlr_populate_namespaces(nvme_bdev_ctrlr, NULL);
	} else if ((event.bits.async_event_type == SPDK_NVME_ASYNC_EVENT_TYPE_VENDOR) &&
		   (event.bits.log_page_identifier == SPDK_OCSSD_LOG_CHUNK_NOTIFICATION) &&
		   spdk_nvme_ctrlr_is_ocssd_supported(nvme_bdev_ctrlr->ctrlr)) {
		bdev_ocssd_handle_chunk_notification(nvme_bdev_ctrlr);
	}
}

static int
create_ctrlr(struct spdk_nvme_ctrlr *ctrlr,
	     const char *name,
	     const struct spdk_nvme_transport_id *trid,
	     uint32_t prchk_flags)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;
	uint32_t i;
	int rc;

	nvme_bdev_ctrlr = calloc(1, sizeof(*nvme_bdev_ctrlr));
	if (nvme_bdev_ctrlr == NULL) {
		SPDK_ERRLOG("Failed to allocate device struct\n");
		return -ENOMEM;
	}
	nvme_bdev_ctrlr->num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
	nvme_bdev_ctrlr->namespaces = calloc(nvme_bdev_ctrlr->num_ns, sizeof(struct nvme_bdev_ns *));
	if (!nvme_bdev_ctrlr->namespaces) {
		SPDK_ERRLOG("Failed to allocate block namespaces pointer\n");
		free(nvme_bdev_ctrlr);
		return -ENOMEM;
	}

	for (i = 0; i < nvme_bdev_ctrlr->num_ns; i++) {
		nvme_bdev_ctrlr->namespaces[i] = calloc(1, sizeof(struct nvme_bdev_ns));
		if (nvme_bdev_ctrlr->namespaces[i] == NULL) {
			SPDK_ERRLOG("Failed to allocate block namespace struct\n");
			for (; i > 0; i--) {
				free(nvme_bdev_ctrlr->namespaces[i - 1]);
			}
			free(nvme_bdev_ctrlr->namespaces);
			free(nvme_bdev_ctrlr);
			return -ENOMEM;
		}
	}

	nvme_bdev_ctrlr->adminq_timer_poller = NULL;
	nvme_bdev_ctrlr->ctrlr = ctrlr;
	nvme_bdev_ctrlr->ref = 0;
	nvme_bdev_ctrlr->trid = *trid;
	nvme_bdev_ctrlr->name = strdup(name);
	if (nvme_bdev_ctrlr->name == NULL) {
		free(nvme_bdev_ctrlr->namespaces);
		free(nvme_bdev_ctrlr);
		return -ENOMEM;
	}

	if (spdk_nvme_ctrlr_is_ocssd_supported(nvme_bdev_ctrlr->ctrlr)) {
		rc = bdev_ocssd_init_ctrlr(nvme_bdev_ctrlr);
		if (spdk_unlikely(rc != 0)) {
			SPDK_ERRLOG("Unable to initialize OCSSD controller\n");
			free(nvme_bdev_ctrlr->name);
			free(nvme_bdev_ctrlr->namespaces);
			free(nvme_bdev_ctrlr);
			return rc;
		}
	}

	nvme_bdev_ctrlr->prchk_flags = prchk_flags;

	spdk_io_device_register(nvme_bdev_ctrlr, bdev_nvme_create_cb, bdev_nvme_destroy_cb,
				sizeof(struct nvme_io_channel),
				name);

	nvme_bdev_ctrlr->adminq_timer_poller = SPDK_POLLER_REGISTER(bdev_nvme_poll_adminq, ctrlr,
					       g_opts.nvme_adminq_poll_period_us);

	TAILQ_INSERT_TAIL(&g_nvme_bdev_ctrlrs, nvme_bdev_ctrlr, tailq);

	if (g_opts.timeout_us > 0) {
		spdk_nvme_ctrlr_register_timeout_callback(ctrlr, g_opts.timeout_us,
				timeout_cb, NULL);
	}

	spdk_nvme_ctrlr_register_aer_callback(ctrlr, aer_cb, nvme_bdev_ctrlr);

	if (spdk_nvme_ctrlr_get_flags(nvme_bdev_ctrlr->ctrlr) &
	    SPDK_NVME_CTRLR_SECURITY_SEND_RECV_SUPPORTED) {
		nvme_bdev_ctrlr->opal_dev = spdk_opal_dev_construct(nvme_bdev_ctrlr->ctrlr);
		if (nvme_bdev_ctrlr->opal_dev == NULL) {
			SPDK_ERRLOG("Failed to initialize Opal\n");
		}
	}
	return 0;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;
	struct nvme_probe_ctx *ctx = cb_ctx;
	char *name = NULL;
	uint32_t prchk_flags = 0;
	size_t i;

	if (ctx) {
		for (i = 0; i < ctx->count; i++) {
			if (spdk_nvme_transport_id_compare(trid, &ctx->trids[i]) == 0) {
				prchk_flags = ctx->prchk_flags[i];
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

	create_ctrlr(ctrlr, name, trid, prchk_flags);

	nvme_bdev_ctrlr = nvme_bdev_ctrlr_get(trid);
	if (!nvme_bdev_ctrlr) {
		SPDK_ERRLOG("Failed to find new NVMe controller\n");
		free(name);
		return;
	}

	nvme_ctrlr_populate_namespaces(nvme_bdev_ctrlr, NULL);

	free(name);
}

static void
remove_cb(void *cb_ctx, struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t i;
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;
	struct nvme_bdev_ns *ns;

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	TAILQ_FOREACH(nvme_bdev_ctrlr, &g_nvme_bdev_ctrlrs, tailq) {
		if (nvme_bdev_ctrlr->ctrlr == ctrlr) {
			/* The controller's destruction was already started */
			if (nvme_bdev_ctrlr->destruct) {
				pthread_mutex_unlock(&g_bdev_nvme_mutex);
				return;
			}
			pthread_mutex_unlock(&g_bdev_nvme_mutex);
			for (i = 0; i < nvme_bdev_ctrlr->num_ns; i++) {
				uint32_t	nsid = i + 1;

				ns = nvme_bdev_ctrlr->namespaces[nsid - 1];
				if (ns->populated) {
					assert(ns->id == nsid);
					nvme_ctrlr_depopulate_namespace(nvme_bdev_ctrlr, ns);
				}
			}

			pthread_mutex_lock(&g_bdev_nvme_mutex);
			nvme_bdev_ctrlr->destruct = true;
			if (nvme_bdev_ctrlr->ref == 0) {
				pthread_mutex_unlock(&g_bdev_nvme_mutex);
				nvme_bdev_ctrlr_destruct(nvme_bdev_ctrlr);
			} else {
				pthread_mutex_unlock(&g_bdev_nvme_mutex);
			}
			return;
		}
	}
	pthread_mutex_unlock(&g_bdev_nvme_mutex);
}

static int
bdev_nvme_hotplug(void *arg)
{
	struct spdk_nvme_transport_id trid_pcie;
	int done;

	if (!g_hotplug_probe_ctx) {
		memset(&trid_pcie, 0, sizeof(trid_pcie));
		spdk_nvme_trid_populate_transport(&trid_pcie, SPDK_NVME_TRANSPORT_PCIE);

		g_hotplug_probe_ctx = spdk_nvme_probe_async(&trid_pcie, NULL,
				      hotplug_probe_cb,
				      attach_cb, remove_cb);
		if (!g_hotplug_probe_ctx) {
			return -1;
		}
	}

	done = spdk_nvme_probe_poll_async(g_hotplug_probe_ctx);
	if (done != -EAGAIN) {
		g_hotplug_probe_ctx = NULL;
		return 1;
	}

	return -1;
}

void
spdk_bdev_nvme_get_opts(struct spdk_bdev_nvme_opts *opts)
{
	*opts = g_opts;
}

int
spdk_bdev_nvme_set_opts(const struct spdk_bdev_nvme_opts *opts)
{
	if (g_bdev_nvme_init_thread != NULL) {
		if (!TAILQ_EMPTY(&g_nvme_bdev_ctrlrs)) {
			return -EPERM;
		}
	}

	g_opts = *opts;

	return 0;
}

struct set_nvme_hotplug_ctx {
	uint64_t period_us;
	bool enabled;
	spdk_msg_fn fn;
	void *fn_ctx;
};

static void
set_nvme_hotplug_period_cb(void *_ctx)
{
	struct set_nvme_hotplug_ctx *ctx = _ctx;

	spdk_poller_unregister(&g_hotplug_poller);
	if (ctx->enabled) {
		g_hotplug_poller = SPDK_POLLER_REGISTER(bdev_nvme_hotplug, NULL, ctx->period_us);
	}

	g_nvme_hotplug_poll_period_us = ctx->period_us;
	g_nvme_hotplug_enabled = ctx->enabled;
	if (ctx->fn) {
		ctx->fn(ctx->fn_ctx);
	}

	free(ctx);
}

int
spdk_bdev_nvme_set_hotplug(bool enabled, uint64_t period_us, spdk_msg_fn cb, void *cb_ctx)
{
	struct set_nvme_hotplug_ctx *ctx;

	if (enabled == true && !spdk_process_is_primary()) {
		return -EPERM;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	period_us = period_us == 0 ? NVME_HOTPLUG_POLL_PERIOD_DEFAULT : period_us;
	ctx->period_us = spdk_min(period_us, NVME_HOTPLUG_POLL_PERIOD_MAX);
	ctx->enabled = enabled;
	ctx->fn = cb;
	ctx->fn_ctx = cb_ctx;

	spdk_thread_send_msg(g_bdev_nvme_init_thread, set_nvme_hotplug_period_cb, ctx);
	return 0;
}

static void
populate_namespaces_cb(struct nvme_async_probe_ctx *ctx, size_t count, int rc)
{
	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_ctx, count, rc);
	}

	free(ctx);
}

static void
nvme_ctrlr_populate_namespaces_done(struct nvme_async_probe_ctx *ctx)
{
	struct nvme_bdev_ctrlr	*nvme_bdev_ctrlr;
	struct nvme_bdev_ns	*ns;
	struct nvme_bdev	*nvme_bdev, *tmp;
	uint32_t		i, nsid;
	size_t			j;

	nvme_bdev_ctrlr = nvme_bdev_ctrlr_get(&ctx->trid);
	assert(nvme_bdev_ctrlr != NULL);

	/*
	 * Report the new bdevs that were created in this call.
	 * There can be more than one bdev per NVMe controller.
	 */
	j = 0;
	for (i = 0; i < nvme_bdev_ctrlr->num_ns; i++) {
		nsid = i + 1;
		ns = nvme_bdev_ctrlr->namespaces[nsid - 1];
		if (!ns->populated) {
			continue;
		}
		assert(ns->id == nsid);
		TAILQ_FOREACH_SAFE(nvme_bdev, &ns->bdevs, tailq, tmp) {
			if (j < ctx->count) {
				ctx->names[j] = nvme_bdev->disk.name;
				j++;
			} else {
				SPDK_ERRLOG("Maximum number of namespaces supported per NVMe controller is %du. Unable to return all names of created bdevs\n",
					    ctx->count);
				populate_namespaces_cb(ctx, 0, -ERANGE);
				return;
			}
		}
	}

	populate_namespaces_cb(ctx, j, 0);
}

static void
connect_attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvme_ctrlr_opts *user_opts = cb_ctx;
	struct nvme_bdev_ctrlr	*nvme_bdev_ctrlr;
	struct nvme_async_probe_ctx *ctx;
	int rc;

	ctx = SPDK_CONTAINEROF(user_opts, struct nvme_async_probe_ctx, opts);

	spdk_poller_unregister(&ctx->poller);

	rc = create_ctrlr(ctrlr, ctx->base_name, &ctx->trid, ctx->prchk_flags);
	if (rc) {
		SPDK_ERRLOG("Failed to create new device\n");
		populate_namespaces_cb(ctx, 0, rc);
		return;
	}

	nvme_bdev_ctrlr = nvme_bdev_ctrlr_get(&ctx->trid);
	assert(nvme_bdev_ctrlr != NULL);

	nvme_ctrlr_populate_namespaces(nvme_bdev_ctrlr, ctx);
}

static int
bdev_nvme_async_poll(void *arg)
{
	struct nvme_async_probe_ctx	*ctx = arg;
	int				rc;

	rc = spdk_nvme_probe_poll_async(ctx->probe_ctx);
	if (spdk_unlikely(rc != -EAGAIN && rc != 0)) {
		spdk_poller_unregister(&ctx->poller);
		free(ctx);
	}

	return 1;
}

int
spdk_bdev_nvme_create(struct spdk_nvme_transport_id *trid,
		      struct spdk_nvme_host_id *hostid,
		      const char *base_name,
		      const char **names,
		      uint32_t count,
		      const char *hostnqn,
		      uint32_t prchk_flags,
		      spdk_bdev_create_nvme_fn cb_fn,
		      void *cb_ctx)
{
	struct nvme_probe_skip_entry	*entry, *tmp;
	struct nvme_async_probe_ctx	*ctx;

	if (nvme_bdev_ctrlr_get(trid) != NULL) {
		SPDK_ERRLOG("A controller with the provided trid (traddr: %s) already exists.\n", trid->traddr);
		return -EEXIST;
	}

	if (nvme_bdev_ctrlr_get_by_name(base_name)) {
		SPDK_ERRLOG("A controller with the provided name (%s) already exists.\n", base_name);
		return -EEXIST;
	}

	if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE) {
		TAILQ_FOREACH_SAFE(entry, &g_skipped_nvme_ctrlrs, tailq, tmp) {
			if (spdk_nvme_transport_id_compare(trid, &entry->trid) == 0) {
				TAILQ_REMOVE(&g_skipped_nvme_ctrlrs, entry, tailq);
				free(entry);
				break;
			}
		}
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}
	ctx->base_name = base_name;
	ctx->names = names;
	ctx->count = count;
	ctx->cb_fn = cb_fn;
	ctx->cb_ctx = cb_ctx;
	ctx->prchk_flags = prchk_flags;
	ctx->trid = *trid;

	spdk_nvme_ctrlr_get_default_ctrlr_opts(&ctx->opts, sizeof(ctx->opts));
	ctx->opts.transport_retry_count = g_opts.retry_count;

	if (hostnqn) {
		snprintf(ctx->opts.hostnqn, sizeof(ctx->opts.hostnqn), "%s", hostnqn);
	}

	if (hostid->hostaddr[0] != '\0') {
		snprintf(ctx->opts.src_addr, sizeof(ctx->opts.src_addr), "%s", hostid->hostaddr);
	}

	if (hostid->hostsvcid[0] != '\0') {
		snprintf(ctx->opts.src_svcid, sizeof(ctx->opts.src_svcid), "%s", hostid->hostsvcid);
	}

	ctx->probe_ctx = spdk_nvme_connect_async(trid, &ctx->opts, connect_attach_cb);
	if (ctx->probe_ctx == NULL) {
		SPDK_ERRLOG("No controller was found with provided trid (traddr: %s)\n", trid->traddr);
		free(ctx);
		return -ENODEV;
	}
	ctx->poller = SPDK_POLLER_REGISTER(bdev_nvme_async_poll, ctx, 1000);

	return 0;
}

int
spdk_bdev_nvme_delete(const char *name)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr = NULL;
	struct nvme_probe_skip_entry *entry;

	if (name == NULL) {
		return -EINVAL;
	}

	nvme_bdev_ctrlr = nvme_bdev_ctrlr_get_by_name(name);
	if (nvme_bdev_ctrlr == NULL) {
		SPDK_ERRLOG("Failed to find NVMe controller\n");
		return -ENODEV;
	}

	if (nvme_bdev_ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
		entry = calloc(1, sizeof(*entry));
		if (!entry) {
			return -ENOMEM;
		}
		entry->trid = nvme_bdev_ctrlr->trid;
		TAILQ_INSERT_TAIL(&g_skipped_nvme_ctrlrs, entry, tailq);
	}

	remove_cb(NULL, nvme_bdev_ctrlr->ctrlr);
	return 0;
}

static int
bdev_nvme_library_init(void)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;
	struct spdk_conf_section *sp;
	const char *val;
	int rc = 0;
	int64_t intval = 0;
	size_t i;
	struct nvme_probe_ctx *probe_ctx = NULL;
	int retry_count;
	uint32_t local_nvme_num = 0;
	int64_t hotplug_period;
	bool hotplug_enabled = g_nvme_hotplug_enabled;

	g_bdev_nvme_init_thread = spdk_get_thread();

	spdk_io_device_register(&g_nvme_bdev_ctrlrs, bdev_nvme_poll_group_create_cb,
				bdev_nvme_poll_group_destroy_cb,
				sizeof(struct nvme_bdev_poll_group),  "bdev_nvme_poll_groups");

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

	retry_count = spdk_conf_section_get_intval(sp, "RetryCount");
	if (retry_count >= 0) {
		g_opts.retry_count = retry_count;
	}

	val = spdk_conf_section_get_val(sp, "TimeoutUsec");
	if (val != NULL) {
		intval = spdk_strtoll(val, 10);
		if (intval < 0) {
			SPDK_ERRLOG("Invalid TimeoutUsec value\n");
			rc = -1;
			goto end;
		}
	}

	g_opts.timeout_us = intval;

	if (g_opts.timeout_us > 0) {
		val = spdk_conf_section_get_val(sp, "ActionOnTimeout");
		if (val != NULL) {
			if (!strcasecmp(val, "Reset")) {
				g_opts.action_on_timeout = SPDK_BDEV_NVME_TIMEOUT_ACTION_RESET;
			} else if (!strcasecmp(val, "Abort")) {
				g_opts.action_on_timeout = SPDK_BDEV_NVME_TIMEOUT_ACTION_ABORT;
			}
		}
	}

	intval = spdk_conf_section_get_intval(sp, "AdminPollRate");
	if (intval > 0) {
		g_opts.nvme_adminq_poll_period_us = intval;
	}

	intval = spdk_conf_section_get_intval(sp, "IOPollRate");
	if (intval > 0) {
		g_opts.nvme_ioq_poll_period_us = intval;
	}

	if (spdk_process_is_primary()) {
		hotplug_enabled = spdk_conf_section_get_boolval(sp, "HotplugEnable", false);
	}

	hotplug_period = spdk_conf_section_get_intval(sp, "HotplugPollRate");
	if (hotplug_period < 0) {
		hotplug_period = 0;
	}

	g_nvme_hostnqn = spdk_conf_section_get_val(sp, "HostNQN");
	probe_ctx->hostnqn = g_nvme_hostnqn;

	g_opts.delay_cmd_submit = spdk_conf_section_get_boolval(sp, "DelayCmdSubmit",
				  SPDK_BDEV_NVME_DEFAULT_DELAY_CMD_SUBMIT);

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

		rc = spdk_nvme_host_id_parse(&probe_ctx->hostids[i], val);
		if (rc < 0) {
			SPDK_ERRLOG("Unable to parse HostID: %s\n", val);
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

		val = spdk_conf_section_get_nmval(sp, "TransportID", i, 2);
		if (val != NULL) {
			rc = spdk_nvme_prchk_flags_parse(&probe_ctx->prchk_flags[i], val);
			if (rc < 0) {
				SPDK_ERRLOG("Unable to parse prchk: %s\n", val);
				rc = -1;
				goto end;
			}
		}

		probe_ctx->count++;

		if (probe_ctx->trids[i].trtype != SPDK_NVME_TRANSPORT_PCIE) {
			struct spdk_nvme_ctrlr *ctrlr;
			struct spdk_nvme_ctrlr_opts opts;

			if (nvme_bdev_ctrlr_get(&probe_ctx->trids[i])) {
				SPDK_ERRLOG("A controller with the provided trid (traddr: %s) already exists.\n",
					    probe_ctx->trids[i].traddr);
				rc = -1;
				goto end;
			}

			if (probe_ctx->trids[i].subnqn[0] == '\0') {
				SPDK_ERRLOG("Need to provide subsystem nqn\n");
				rc = -1;
				goto end;
			}

			spdk_nvme_ctrlr_get_default_ctrlr_opts(&opts, sizeof(opts));
			opts.transport_retry_count = g_opts.retry_count;

			if (probe_ctx->hostnqn != NULL) {
				snprintf(opts.hostnqn, sizeof(opts.hostnqn), "%s", probe_ctx->hostnqn);
			}

			if (probe_ctx->hostids[i].hostaddr[0] != '\0') {
				snprintf(opts.src_addr, sizeof(opts.src_addr), "%s", probe_ctx->hostids[i].hostaddr);
			}

			if (probe_ctx->hostids[i].hostsvcid[0] != '\0') {
				snprintf(opts.src_svcid, sizeof(opts.src_svcid), "%s", probe_ctx->hostids[i].hostsvcid);
			}

			ctrlr = spdk_nvme_connect(&probe_ctx->trids[i], &opts, sizeof(opts));
			if (ctrlr == NULL) {
				SPDK_ERRLOG("Unable to connect to provided trid (traddr: %s)\n",
					    probe_ctx->trids[i].traddr);
				rc = -1;
				goto end;
			}

			rc = create_ctrlr(ctrlr, probe_ctx->names[i], &probe_ctx->trids[i], 0);
			if (rc) {
				goto end;
			}

			nvme_bdev_ctrlr = nvme_bdev_ctrlr_get(&probe_ctx->trids[i]);
			if (!nvme_bdev_ctrlr) {
				SPDK_ERRLOG("Failed to find new NVMe controller\n");
				rc = -ENODEV;
				goto end;
			}

			nvme_ctrlr_populate_namespaces(nvme_bdev_ctrlr, NULL);
		} else {
			local_nvme_num++;
		}
	}

	if (local_nvme_num > 0) {
		/* used to probe local NVMe device */
		if (spdk_nvme_probe(NULL, probe_ctx, probe_cb, attach_cb, remove_cb)) {
			rc = -1;
			goto end;
		}

		for (i = 0; i < probe_ctx->count; i++) {
			if (probe_ctx->trids[i].trtype != SPDK_NVME_TRANSPORT_PCIE) {
				continue;
			}

			if (!nvme_bdev_ctrlr_get(&probe_ctx->trids[i])) {
				SPDK_ERRLOG("NVMe SSD \"%s\" could not be found.\n", probe_ctx->trids[i].traddr);
				SPDK_ERRLOG("Check PCIe BDF and that it is attached to UIO/VFIO driver.\n");
			}
		}
	}

	rc = spdk_bdev_nvme_set_hotplug(hotplug_enabled, hotplug_period, NULL, NULL);
	if (rc) {
		SPDK_ERRLOG("Failed to setup hotplug (%d): %s", rc, spdk_strerror(rc));
		rc = -1;
	}
end:
	free(probe_ctx);
	return rc;
}

static void
bdev_nvme_library_fini(void)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr, *tmp;
	struct nvme_probe_skip_entry *entry, *entry_tmp;
	struct nvme_bdev_ns *ns;
	uint32_t i;

	spdk_poller_unregister(&g_hotplug_poller);
	free(g_hotplug_probe_ctx);

	TAILQ_FOREACH_SAFE(entry, &g_skipped_nvme_ctrlrs, tailq, entry_tmp) {
		TAILQ_REMOVE(&g_skipped_nvme_ctrlrs, entry, tailq);
		free(entry);
	}

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	TAILQ_FOREACH_SAFE(nvme_bdev_ctrlr, &g_nvme_bdev_ctrlrs, tailq, tmp) {
		if (nvme_bdev_ctrlr->destruct) {
			/* This controller's destruction was already started
			 * before the application started shutting down
			 */
			continue;
		}

		pthread_mutex_unlock(&g_bdev_nvme_mutex);

		for (i = 0; i < nvme_bdev_ctrlr->num_ns; i++) {
			uint32_t nsid = i + 1;

			ns = nvme_bdev_ctrlr->namespaces[nsid - 1];
			if (ns->populated) {
				assert(ns->id == nsid);
				nvme_ctrlr_depopulate_namespace(nvme_bdev_ctrlr, ns);
			}
		}

		pthread_mutex_lock(&g_bdev_nvme_mutex);
		nvme_bdev_ctrlr->destruct = true;

		if (nvme_bdev_ctrlr->ref == 0) {
			pthread_mutex_unlock(&g_bdev_nvme_mutex);
			nvme_bdev_ctrlr_destruct(nvme_bdev_ctrlr);
			pthread_mutex_lock(&g_bdev_nvme_mutex);
		}
	}

	g_bdev_nvme_module_finish = true;
	if (TAILQ_EMPTY(&g_nvme_bdev_ctrlrs)) {
		pthread_mutex_unlock(&g_bdev_nvme_mutex);
		spdk_io_device_unregister(&g_nvme_bdev_ctrlrs, NULL);
		spdk_bdev_module_finish_done();
		return;
	}

	pthread_mutex_unlock(&g_bdev_nvme_mutex);
}

static void
bdev_nvme_verify_pi_error(struct spdk_bdev_io *bdev_io)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct spdk_dif_ctx dif_ctx;
	struct spdk_dif_error err_blk = {};
	int rc;

	rc = spdk_dif_ctx_init(&dif_ctx,
			       bdev->blocklen, bdev->md_len, bdev->md_interleave,
			       bdev->dif_is_head_of_md, bdev->dif_type, bdev->dif_check_flags,
			       bdev_io->u.bdev.offset_blocks, 0, 0, 0, 0);
	if (rc != 0) {
		SPDK_ERRLOG("Initialization of DIF context failed\n");
		return;
	}

	if (bdev->md_interleave) {
		rc = spdk_dif_verify(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				     bdev_io->u.bdev.num_blocks, &dif_ctx, &err_blk);
	} else {
		struct iovec md_iov = {
			.iov_base	= bdev_io->u.bdev.md_buf,
			.iov_len	= bdev_io->u.bdev.num_blocks * bdev->md_len,
		};

		rc = spdk_dix_verify(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				     &md_iov, bdev_io->u.bdev.num_blocks, &dif_ctx, &err_blk);
	}

	if (rc != 0) {
		SPDK_ERRLOG("DIF error detected. type=%d, offset=%" PRIu32 "\n",
			    err_blk.err_type, err_blk.err_offset);
	} else {
		SPDK_ERRLOG("Hardware reported PI error but SPDK could not find any.\n");
	}
}

static void
bdev_nvme_no_pi_readv_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);

	if (spdk_nvme_cpl_is_success(cpl)) {
		/* Run PI verification for read data buffer. */
		bdev_nvme_verify_pi_error(bdev_io);
	}

	/* Return original completion status */
	spdk_bdev_io_complete_nvme_status(bdev_io, bio->cpl.cdw0, bio->cpl.status.sct,
					  bio->cpl.status.sc);
}

static void
bdev_nvme_readv_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);
	int ret;

	if (spdk_unlikely(spdk_nvme_cpl_is_pi_error(cpl))) {
		SPDK_ERRLOG("readv completed with PI error (sct=%d, sc=%d)\n",
			    cpl->status.sct, cpl->status.sc);

		/* Save completion status to use after verifying PI error. */
		bio->cpl = *cpl;

		/* Read without PI checking to verify PI error. */
		ret = bdev_nvme_no_pi_readv((struct nvme_bdev *)bdev_io->bdev->ctxt,
					    spdk_bdev_io_get_io_channel(bdev_io),
					    bio,
					    bdev_io->u.bdev.iovs,
					    bdev_io->u.bdev.iovcnt,
					    bdev_io->u.bdev.md_buf,
					    bdev_io->u.bdev.num_blocks,
					    bdev_io->u.bdev.offset_blocks);
		if (ret == 0) {
			return;
		}
	}

	spdk_bdev_io_complete_nvme_status(bdev_io, cpl->cdw0, cpl->status.sct, cpl->status.sc);
}

static void
bdev_nvme_writev_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx((struct nvme_bdev_io *)ref);

	if (spdk_nvme_cpl_is_pi_error(cpl)) {
		SPDK_ERRLOG("writev completed with PI error (sct=%d, sc=%d)\n",
			    cpl->status.sct, cpl->status.sc);
		/* Run PI verification for write data buffer if PI error is detected. */
		bdev_nvme_verify_pi_error(bdev_io);
	}

	spdk_bdev_io_complete_nvme_status(bdev_io, cpl->cdw0, cpl->status.sct, cpl->status.sc);
}

static void
bdev_nvme_comparev_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx((struct nvme_bdev_io *)ref);

	if (spdk_nvme_cpl_is_pi_error(cpl)) {
		SPDK_ERRLOG("comparev completed with PI error (sct=%d, sc=%d)\n",
			    cpl->status.sct, cpl->status.sc);
		/* Run PI verification for compare data buffer if PI error is detected. */
		bdev_nvme_verify_pi_error(bdev_io);
	}

	spdk_bdev_io_complete_nvme_status(bdev_io, cpl->cdw0, cpl->status.sct, cpl->status.sc);
}

static void
bdev_nvme_comparev_and_writev_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);

	/* Compare operation completion */
	if ((cpl->cdw0 & 0xFF) == SPDK_NVME_OPC_COMPARE) {
		/* Save compare result for write callback */
		bio->cpl = *cpl;
		return;
	}

	/* Write operation completion */
	if (spdk_nvme_cpl_is_error(&bio->cpl)) {
		/* If bio->cpl is already an error, it means the compare operation failed.  In that case,
		 * complete the IO with the compare operation's status.
		 */
		if (!spdk_nvme_cpl_is_error(cpl)) {
			SPDK_ERRLOG("Unexpected write success after compare failure.\n");
		}

		spdk_bdev_io_complete_nvme_status(bdev_io, bio->cpl.cdw0, bio->cpl.status.sct, bio->cpl.status.sc);
	} else {
		spdk_bdev_io_complete_nvme_status(bdev_io, cpl->cdw0, cpl->status.sct, cpl->status.sc);
	}
}

static void
bdev_nvme_queued_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx((struct nvme_bdev_io *)ref);

	spdk_bdev_io_complete_nvme_status(bdev_io, cpl->cdw0, cpl->status.sct, cpl->status.sc);
}

static void
bdev_nvme_admin_passthru_completion(void *ctx)
{
	struct nvme_bdev_io *bio = ctx;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);

	spdk_bdev_io_complete_nvme_status(bdev_io,
					  bio->cpl.cdw0, bio->cpl.status.sct, bio->cpl.status.sc);
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

static void
bdev_nvme_queued_reset_fused_sgl(void *ref, uint32_t sgl_offset)
{
	struct nvme_bdev_io *bio = ref;
	struct iovec *iov;

	bio->fused_iov_offset = sgl_offset;
	for (bio->fused_iovpos = 0; bio->fused_iovpos < bio->fused_iovcnt; bio->fused_iovpos++) {
		iov = &bio->fused_iovs[bio->fused_iovpos];
		if (bio->fused_iov_offset < iov->iov_len) {
			break;
		}

		bio->fused_iov_offset -= iov->iov_len;
	}
}

static int
bdev_nvme_queued_next_fused_sge(void *ref, void **address, uint32_t *length)
{
	struct nvme_bdev_io *bio = ref;
	struct iovec *iov;

	assert(bio->fused_iovpos < bio->fused_iovcnt);

	iov = &bio->fused_iovs[bio->fused_iovpos];

	*address = iov->iov_base;
	*length = iov->iov_len;

	if (bio->fused_iov_offset) {
		assert(bio->fused_iov_offset <= iov->iov_len);
		*address += bio->fused_iov_offset;
		*length -= bio->fused_iov_offset;
	}

	bio->fused_iov_offset += *length;
	if (bio->fused_iov_offset == iov->iov_len) {
		bio->fused_iovpos++;
		bio->fused_iov_offset = 0;
	}

	return 0;
}

static int
bdev_nvme_no_pi_readv(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
		      struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
		      void *md, uint64_t lba_count, uint64_t lba)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "read %lu blocks with offset %#lx without PI check\n",
		      lba_count, lba);

	bio->iovs = iov;
	bio->iovcnt = iovcnt;
	bio->iovpos = 0;
	bio->iov_offset = 0;

	rc = spdk_nvme_ns_cmd_readv_with_md(nbdev->nvme_ns->ns, nvme_ch->qpair, lba, lba_count,
					    bdev_nvme_no_pi_readv_done, bio, 0,
					    bdev_nvme_queued_reset_sgl, bdev_nvme_queued_next_sge,
					    md, 0, 0);

	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("no_pi_readv failed: rc = %d\n", rc);
	}
	return rc;
}

static int
bdev_nvme_readv(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
		struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
		void *md, uint64_t lba_count, uint64_t lba)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "read %lu blocks with offset %#lx\n",
		      lba_count, lba);

	bio->iovs = iov;
	bio->iovcnt = iovcnt;
	bio->iovpos = 0;
	bio->iov_offset = 0;

	rc = spdk_nvme_ns_cmd_readv_with_md(nbdev->nvme_ns->ns, nvme_ch->qpair, lba, lba_count,
					    bdev_nvme_readv_done, bio, nbdev->disk.dif_check_flags,
					    bdev_nvme_queued_reset_sgl, bdev_nvme_queued_next_sge,
					    md, 0, 0);

	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("readv failed: rc = %d\n", rc);
	}
	return rc;
}

static int
bdev_nvme_writev(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
		 struct nvme_bdev_io *bio,
		 struct iovec *iov, int iovcnt, void *md, uint64_t lba_count, uint64_t lba)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "write %lu blocks with offset %#lx\n",
		      lba_count, lba);

	bio->iovs = iov;
	bio->iovcnt = iovcnt;
	bio->iovpos = 0;
	bio->iov_offset = 0;

	rc = spdk_nvme_ns_cmd_writev_with_md(nbdev->nvme_ns->ns, nvme_ch->qpair, lba, lba_count,
					     bdev_nvme_writev_done, bio, nbdev->disk.dif_check_flags,
					     bdev_nvme_queued_reset_sgl, bdev_nvme_queued_next_sge,
					     md, 0, 0);

	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("writev failed: rc = %d\n", rc);
	}
	return rc;
}

static int
bdev_nvme_comparev(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
		   struct nvme_bdev_io *bio,
		   struct iovec *iov, int iovcnt, void *md, uint64_t lba_count, uint64_t lba)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "compare %lu blocks with offset %#lx\n",
		      lba_count, lba);

	bio->iovs = iov;
	bio->iovcnt = iovcnt;
	bio->iovpos = 0;
	bio->iov_offset = 0;

	rc = spdk_nvme_ns_cmd_comparev_with_md(nbdev->nvme_ns->ns, nvme_ch->qpair, lba, lba_count,
					       bdev_nvme_comparev_done, bio, nbdev->disk.dif_check_flags,
					       bdev_nvme_queued_reset_sgl, bdev_nvme_queued_next_sge,
					       md, 0, 0);

	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("comparev failed: rc = %d\n", rc);
	}
	return rc;
}

static int
bdev_nvme_comparev_and_writev(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
			      struct nvme_bdev_io *bio, struct iovec *cmp_iov, int cmp_iovcnt, struct iovec *write_iov,
			      int write_iovcnt, void *md, uint64_t lba_count, uint64_t lba)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);
	uint32_t flags = nbdev->disk.dif_check_flags;
	int rc;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "compare and write %lu blocks with offset %#lx\n",
		      lba_count, lba);

	bio->iovs = cmp_iov;
	bio->iovcnt = cmp_iovcnt;
	bio->iovpos = 0;
	bio->iov_offset = 0;
	bio->fused_iovs = write_iov;
	bio->fused_iovcnt = write_iovcnt;
	bio->fused_iovpos = 0;
	bio->fused_iov_offset = 0;

	if (bdev_io->num_retries == 0) {
		bio->first_fused_submitted = false;
	}

	if (!bio->first_fused_submitted) {
		flags |= SPDK_NVME_IO_FLAGS_FUSE_FIRST;
		memset(&bio->cpl, 0, sizeof(bio->cpl));

		rc = spdk_nvme_ns_cmd_comparev_with_md(nbdev->nvme_ns->ns, nvme_ch->qpair, lba, lba_count,
						       bdev_nvme_comparev_and_writev_done, bio, flags,
						       bdev_nvme_queued_reset_sgl, bdev_nvme_queued_next_sge, md, 0, 0);
		if (rc == 0) {
			bio->first_fused_submitted = true;
			flags &= ~SPDK_NVME_IO_FLAGS_FUSE_FIRST;
		} else {
			if (rc != -ENOMEM) {
				SPDK_ERRLOG("compare failed: rc = %d\n", rc);
			}
			return rc;
		}
	}

	flags |= SPDK_NVME_IO_FLAGS_FUSE_SECOND;

	rc = spdk_nvme_ns_cmd_writev_with_md(nbdev->nvme_ns->ns, nvme_ch->qpair, lba, lba_count,
					     bdev_nvme_comparev_and_writev_done, bio, flags,
					     bdev_nvme_queued_reset_fused_sgl, bdev_nvme_queued_next_fused_sge, md, 0, 0);
	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("write failed: rc = %d\n", rc);
		rc = 0;
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

	rc = spdk_nvme_ns_cmd_dataset_management(nbdev->nvme_ns->ns, nvme_ch->qpair,
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
	uint32_t max_xfer_size = spdk_nvme_ctrlr_get_max_xfer_size(nbdev->nvme_bdev_ctrlr->ctrlr);

	if (nbytes > max_xfer_size) {
		SPDK_ERRLOG("nbytes is greater than MDTS %" PRIu32 ".\n", max_xfer_size);
		return -EINVAL;
	}

	bio->orig_thread = spdk_io_channel_get_thread(ch);

	return spdk_nvme_ctrlr_cmd_admin_raw(nbdev->nvme_bdev_ctrlr->ctrlr, cmd, buf,
					     (uint32_t)nbytes, bdev_nvme_admin_passthru_done, bio);
}

static int
bdev_nvme_io_passthru(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
		      struct nvme_bdev_io *bio,
		      struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);
	uint32_t max_xfer_size = spdk_nvme_ctrlr_get_max_xfer_size(nbdev->nvme_bdev_ctrlr->ctrlr);

	if (nbytes > max_xfer_size) {
		SPDK_ERRLOG("nbytes is greater than MDTS %" PRIu32 ".\n", max_xfer_size);
		return -EINVAL;
	}

	/*
	 * Each NVMe bdev is a specific namespace, and all NVMe I/O commands require a nsid,
	 * so fill it out automatically.
	 */
	cmd->nsid = spdk_nvme_ns_get_id(nbdev->nvme_ns->ns);

	return spdk_nvme_ctrlr_cmd_io_raw(nbdev->nvme_bdev_ctrlr->ctrlr, nvme_ch->qpair, cmd, buf,
					  (uint32_t)nbytes, bdev_nvme_queued_done, bio);
}

static int
bdev_nvme_io_passthru_md(struct nvme_bdev *nbdev, struct spdk_io_channel *ch,
			 struct nvme_bdev_io *bio,
			 struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes, void *md_buf, size_t md_len)
{
	struct nvme_io_channel *nvme_ch = spdk_io_channel_get_ctx(ch);
	size_t nr_sectors = nbytes / spdk_nvme_ns_get_extended_sector_size(nbdev->nvme_ns->ns);
	uint32_t max_xfer_size = spdk_nvme_ctrlr_get_max_xfer_size(nbdev->nvme_bdev_ctrlr->ctrlr);

	if (nbytes > max_xfer_size) {
		SPDK_ERRLOG("nbytes is greater than MDTS %" PRIu32 ".\n", max_xfer_size);
		return -EINVAL;
	}

	if (md_len != nr_sectors * spdk_nvme_ns_get_md_size(nbdev->nvme_ns->ns)) {
		SPDK_ERRLOG("invalid meta data buffer size\n");
		return -EINVAL;
	}

	/*
	 * Each NVMe bdev is a specific namespace, and all NVMe I/O commands require a nsid,
	 * so fill it out automatically.
	 */
	cmd->nsid = spdk_nvme_ns_get_id(nbdev->nvme_ns->ns);

	return spdk_nvme_ctrlr_cmd_io_raw_with_md(nbdev->nvme_bdev_ctrlr->ctrlr, nvme_ch->qpair, cmd, buf,
			(uint32_t)nbytes, md_buf, bdev_nvme_queued_done, bio);
}

static void
bdev_nvme_get_spdk_running_config(FILE *fp)
{
	struct nvme_bdev_ctrlr	*nvme_bdev_ctrlr;

	fprintf(fp, "\n[Nvme]");
	fprintf(fp, "\n"
		"# NVMe Device Whitelist\n"
		"# Users may specify which NVMe devices to claim by their transport id.\n"
		"# See spdk_nvme_transport_id_parse() in spdk/nvme.h for the correct format.\n"
		"# The second argument is the assigned name, which can be referenced from\n"
		"# other sections in the configuration file. For NVMe devices, a namespace\n"
		"# is automatically appended to each name in the format <YourName>nY, where\n"
		"# Y is the NSID (starts at 1).\n");

	TAILQ_FOREACH(nvme_bdev_ctrlr, &g_nvme_bdev_ctrlrs, tailq) {
		const char *trtype;
		const char *prchk_flags;

		trtype = spdk_nvme_transport_id_trtype_str(nvme_bdev_ctrlr->trid.trtype);
		if (!trtype) {
			continue;
		}

		if (nvme_bdev_ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
			fprintf(fp, "TransportID \"trtype:%s traddr:%s\" %s\n",
				trtype,
				nvme_bdev_ctrlr->trid.traddr, nvme_bdev_ctrlr->name);
		} else {
			const char *adrfam;

			adrfam = spdk_nvme_transport_id_adrfam_str(nvme_bdev_ctrlr->trid.adrfam);
			prchk_flags = spdk_nvme_prchk_flags_str(nvme_bdev_ctrlr->prchk_flags);

			if (adrfam) {
				fprintf(fp, "TransportID \"trtype:%s adrfam:%s traddr:%s trsvcid:%s subnqn:%s\" %s",
					trtype,	adrfam,
					nvme_bdev_ctrlr->trid.traddr, nvme_bdev_ctrlr->trid.trsvcid,
					nvme_bdev_ctrlr->trid.subnqn, nvme_bdev_ctrlr->name);
			} else {
				fprintf(fp, "TransportID \"trtype:%s traddr:%s trsvcid:%s subnqn:%s\" %s",
					trtype,
					nvme_bdev_ctrlr->trid.traddr, nvme_bdev_ctrlr->trid.trsvcid,
					nvme_bdev_ctrlr->trid.subnqn, nvme_bdev_ctrlr->name);
			}

			if (prchk_flags) {
				fprintf(fp, " \"%s\"\n", prchk_flags);
			} else {
				fprintf(fp, "\n");
			}
		}
	}

	fprintf(fp, "\n"
		"# The number of attempts per I/O when an I/O fails. Do not include\n"
		"# this key to get the default behavior.\n");
	fprintf(fp, "RetryCount %d\n", g_opts.retry_count);
	fprintf(fp, "\n"
		"# Timeout for each command, in microseconds. If 0, don't track timeouts.\n");
	fprintf(fp, "TimeoutUsec %"PRIu64"\n", g_opts.timeout_us);

	fprintf(fp, "\n"
		"# Action to take on command time out. Only valid when Timeout is greater\n"
		"# than 0. This may be 'Reset' to reset the controller, 'Abort' to abort\n"
		"# the command, or 'None' to just print a message but do nothing.\n"
		"# Admin command timeouts will always result in a reset.\n");
	switch (g_opts.action_on_timeout) {
	case SPDK_BDEV_NVME_TIMEOUT_ACTION_NONE:
		fprintf(fp, "ActionOnTimeout None\n");
		break;
	case SPDK_BDEV_NVME_TIMEOUT_ACTION_RESET:
		fprintf(fp, "ActionOnTimeout Reset\n");
		break;
	case SPDK_BDEV_NVME_TIMEOUT_ACTION_ABORT:
		fprintf(fp, "ActionOnTimeout Abort\n");
		break;
	}

	fprintf(fp, "\n"
		"# Set how often the admin queue is polled for asynchronous events.\n"
		"# Units in microseconds.\n");
	fprintf(fp, "AdminPollRate %"PRIu64"\n", g_opts.nvme_adminq_poll_period_us);
	fprintf(fp, "IOPollRate %" PRIu64"\n", g_opts.nvme_ioq_poll_period_us);
	fprintf(fp, "\n"
		"# Disable handling of hotplug (runtime insert and remove) events,\n"
		"# users can set to Yes if want to enable it.\n"
		"# Default: No\n");
	fprintf(fp, "HotplugEnable %s\n", g_nvme_hotplug_enabled ? "Yes" : "No");
	fprintf(fp, "\n"
		"# Set how often the hotplug is processed for insert and remove events."
		"# Units in microseconds.\n");
	fprintf(fp, "HotplugPollRate %"PRIu64"\n", g_nvme_hotplug_poll_period_us);
	if (g_nvme_hostnqn) {
		fprintf(fp, "HostNQN %s\n",  g_nvme_hostnqn);
	}
	fprintf(fp, "DelayCmdSubmit %s\n", g_opts.delay_cmd_submit ? "True" : "False");

	fprintf(fp, "\n");
}

static void
nvme_ctrlr_config_json_standard_namespace(struct spdk_json_write_ctx *w, struct nvme_bdev_ns *ns)
{
	/* nop */
}

static void
nvme_namespace_config_json(struct spdk_json_write_ctx *w, struct nvme_bdev_ns *ns)
{
	g_config_json_namespace_fn[ns->type](w, ns);
}

static int
bdev_nvme_config_json(struct spdk_json_write_ctx *w)
{
	struct nvme_bdev_ctrlr		*nvme_bdev_ctrlr;
	struct spdk_nvme_transport_id	*trid;
	const char			*action;
	uint32_t			nsid;

	if (g_opts.action_on_timeout == SPDK_BDEV_NVME_TIMEOUT_ACTION_RESET) {
		action = "reset";
	} else if (g_opts.action_on_timeout == SPDK_BDEV_NVME_TIMEOUT_ACTION_ABORT) {
		action = "abort";
	} else {
		action = "none";
	}

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_nvme_set_options");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "action_on_timeout", action);
	spdk_json_write_named_uint64(w, "timeout_us", g_opts.timeout_us);
	spdk_json_write_named_uint32(w, "retry_count", g_opts.retry_count);
	spdk_json_write_named_uint32(w, "arbitration_burst", g_opts.arbitration_burst);
	spdk_json_write_named_uint32(w, "low_priority_weight", g_opts.low_priority_weight);
	spdk_json_write_named_uint32(w, "medium_priority_weight", g_opts.medium_priority_weight);
	spdk_json_write_named_uint32(w, "high_priority_weight", g_opts.high_priority_weight);
	spdk_json_write_named_uint64(w, "nvme_adminq_poll_period_us", g_opts.nvme_adminq_poll_period_us);
	spdk_json_write_named_uint64(w, "nvme_ioq_poll_period_us", g_opts.nvme_ioq_poll_period_us);
	spdk_json_write_named_uint32(w, "io_queue_requests", g_opts.io_queue_requests);
	spdk_json_write_named_bool(w, "delay_cmd_submit", g_opts.delay_cmd_submit);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	TAILQ_FOREACH(nvme_bdev_ctrlr, &g_nvme_bdev_ctrlrs, tailq) {
		trid = &nvme_bdev_ctrlr->trid;

		spdk_json_write_object_begin(w);

		spdk_json_write_named_string(w, "method", "bdev_nvme_attach_controller");

		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "name", nvme_bdev_ctrlr->name);
		nvme_bdev_dump_trid_json(trid, w);
		spdk_json_write_named_bool(w, "prchk_reftag",
					   (nvme_bdev_ctrlr->prchk_flags & SPDK_NVME_IO_FLAGS_PRCHK_REFTAG) != 0);
		spdk_json_write_named_bool(w, "prchk_guard",
					   (nvme_bdev_ctrlr->prchk_flags & SPDK_NVME_IO_FLAGS_PRCHK_GUARD) != 0);

		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);

		for (nsid = 0; nsid < nvme_bdev_ctrlr->num_ns; ++nsid) {
			if (!nvme_bdev_ctrlr->namespaces[nsid]->populated) {
				continue;
			}

			nvme_namespace_config_json(w, nvme_bdev_ctrlr->namespaces[nsid]);
		}
	}

	/* Dump as last parameter to give all NVMe bdevs chance to be constructed
	 * before enabling hotplug poller.
	 */
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "bdev_nvme_set_hotplug");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_uint64(w, "period_us", g_nvme_hotplug_poll_period_us);
	spdk_json_write_named_bool(w, "enable", g_nvme_hotplug_enabled);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);

	pthread_mutex_unlock(&g_bdev_nvme_mutex);
	return 0;
}

struct spdk_nvme_ctrlr *
spdk_bdev_nvme_get_ctrlr(struct spdk_bdev *bdev)
{
	if (!bdev || bdev->module != &nvme_if) {
		return NULL;
	}

	return SPDK_CONTAINEROF(bdev, struct nvme_bdev, disk)->nvme_bdev_ctrlr->ctrlr;
}

SPDK_LOG_REGISTER_COMPONENT("bdev_nvme", SPDK_LOG_BDEV_NVME)
