/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#include "spdk/accel_engine.h"
#include "spdk/config.h"
#include "spdk/endian.h"
#include "spdk/bdev.h"
#include "spdk/json.h"
#include "spdk/likely.h"
#include "spdk/nvme.h"
#include "spdk/nvme_ocssd.h"
#include "spdk/nvme_zns.h"
#include "spdk/opal.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk/bdev_module.h"
#include "spdk/log.h"

#include "spdk_internal/usdt.h"

#define SPDK_BDEV_NVME_DEFAULT_DELAY_CMD_SUBMIT true
#define SPDK_BDEV_NVME_DEFAULT_KEEP_ALIVE_TIMEOUT_IN_MS	(10000)

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

	/** I/O path the current I/O or admin passthrough is submitted on, or the I/O path
	 *  being reset in a reset I/O.
	 */
	struct nvme_io_path *io_path;

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

	/** Extended IO opts passed by the user to bdev layer and mapped to NVME format */
	struct spdk_nvme_ns_cmd_ext_io_opts ext_opts;

	/** Originating thread */
	struct spdk_thread *orig_thread;

	/** Keeps track if first of fused commands was submitted */
	bool first_fused_submitted;

	/** Keeps track if first of fused commands was completed */
	bool first_fused_completed;

	/** Temporary pointer to zone report buffer */
	struct spdk_nvme_zns_zone_report *zone_report_buf;

	/** Keep track of how many zones that have been copied to the spdk_bdev_zone_info struct */
	uint64_t handled_zones;

	/** Expiration value in ticks to retry the current I/O. */
	uint64_t retry_ticks;

	/* How many times the current I/O was retried. */
	int32_t retry_count;
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
	.timeout_admin_us = 0,
	.keep_alive_timeout_ms = SPDK_BDEV_NVME_DEFAULT_KEEP_ALIVE_TIMEOUT_IN_MS,
	.transport_retry_count = 4,
	.arbitration_burst = 0,
	.low_priority_weight = 0,
	.medium_priority_weight = 0,
	.high_priority_weight = 0,
	.nvme_adminq_poll_period_us = 10000ULL,
	.nvme_ioq_poll_period_us = 0,
	.io_queue_requests = 0,
	.delay_cmd_submit = SPDK_BDEV_NVME_DEFAULT_DELAY_CMD_SUBMIT,
	.bdev_retry_count = 3,
};

#define NVME_HOTPLUG_POLL_PERIOD_MAX			10000000ULL
#define NVME_HOTPLUG_POLL_PERIOD_DEFAULT		100000ULL

static int g_hot_insert_nvme_controller_index = 0;
static uint64_t g_nvme_hotplug_poll_period_us = NVME_HOTPLUG_POLL_PERIOD_DEFAULT;
static bool g_nvme_hotplug_enabled = false;
static struct spdk_thread *g_bdev_nvme_init_thread;
static struct spdk_poller *g_hotplug_poller;
static struct spdk_poller *g_hotplug_probe_poller;
static struct spdk_nvme_probe_ctx *g_hotplug_probe_ctx;

static void nvme_ctrlr_populate_namespaces(struct nvme_ctrlr *nvme_ctrlr,
		struct nvme_async_probe_ctx *ctx);
static void nvme_ctrlr_populate_namespaces_done(struct nvme_ctrlr *nvme_ctrlr,
		struct nvme_async_probe_ctx *ctx);
static int bdev_nvme_library_init(void);
static void bdev_nvme_library_fini(void);
static void bdev_nvme_submit_request(struct spdk_io_channel *ch,
				     struct spdk_bdev_io *bdev_io);
static int bdev_nvme_readv(struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
			   void *md, uint64_t lba_count, uint64_t lba,
			   uint32_t flags, struct spdk_bdev_ext_io_opts *ext_opts);
static int bdev_nvme_no_pi_readv(struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
				 void *md, uint64_t lba_count, uint64_t lba);
static int bdev_nvme_writev(struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
			    void *md, uint64_t lba_count, uint64_t lba,
			    uint32_t flags, struct spdk_bdev_ext_io_opts *ext_opts);
static int bdev_nvme_zone_appendv(struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
				  void *md, uint64_t lba_count,
				  uint64_t zslba, uint32_t flags);
static int bdev_nvme_comparev(struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
			      void *md, uint64_t lba_count, uint64_t lba,
			      uint32_t flags);
static int bdev_nvme_comparev_and_writev(struct nvme_bdev_io *bio,
		struct iovec *cmp_iov, int cmp_iovcnt, struct iovec *write_iov,
		int write_iovcnt, void *md, uint64_t lba_count, uint64_t lba,
		uint32_t flags);
static int bdev_nvme_get_zone_info(struct nvme_bdev_io *bio, uint64_t zone_id,
				   uint32_t num_zones, struct spdk_bdev_zone_info *info);
static int bdev_nvme_zone_management(struct nvme_bdev_io *bio, uint64_t zone_id,
				     enum spdk_bdev_zone_action action);
static void bdev_nvme_admin_passthru(struct nvme_bdev_channel *nbdev_ch,
				     struct nvme_bdev_io *bio,
				     struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes);
static int bdev_nvme_io_passthru(struct nvme_bdev_io *bio, struct spdk_nvme_cmd *cmd,
				 void *buf, size_t nbytes);
static int bdev_nvme_io_passthru_md(struct nvme_bdev_io *bio, struct spdk_nvme_cmd *cmd,
				    void *buf, size_t nbytes, void *md_buf, size_t md_len);
static void bdev_nvme_abort(struct nvme_bdev_channel *nbdev_ch,
			    struct nvme_bdev_io *bio, struct nvme_bdev_io *bio_to_abort);
static void bdev_nvme_reset_io(struct nvme_bdev_channel *nbdev_ch, struct nvme_bdev_io *bio);
static int bdev_nvme_reset(struct nvme_ctrlr *nvme_ctrlr);
static int bdev_nvme_failover(struct nvme_ctrlr *nvme_ctrlr, bool remove);
static void remove_cb(void *cb_ctx, struct spdk_nvme_ctrlr *ctrlr);
static int nvme_ctrlr_read_ana_log_page(struct nvme_ctrlr *nvme_ctrlr);

static int
nvme_ns_cmp(struct nvme_ns *ns1, struct nvme_ns *ns2)
{
	return ns1->id - ns2->id;
}

RB_GENERATE_STATIC(nvme_ns_tree, nvme_ns, node, nvme_ns_cmp);

struct spdk_nvme_qpair *
bdev_nvme_get_io_qpair(struct spdk_io_channel *ctrlr_io_ch)
{
	struct nvme_ctrlr_channel *ctrlr_ch;

	assert(ctrlr_io_ch != NULL);

	ctrlr_ch = spdk_io_channel_get_ctx(ctrlr_io_ch);

	return ctrlr_ch->qpair;
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
	.config_json = bdev_nvme_config_json,
	.get_ctx_size = bdev_nvme_get_ctx_size,

};
SPDK_BDEV_MODULE_REGISTER(nvme, &nvme_if)

struct nvme_bdev_ctrlrs g_nvme_bdev_ctrlrs = TAILQ_HEAD_INITIALIZER(g_nvme_bdev_ctrlrs);
pthread_mutex_t g_bdev_nvme_mutex = PTHREAD_MUTEX_INITIALIZER;
bool g_bdev_nvme_module_finish;

struct nvme_bdev_ctrlr *
nvme_bdev_ctrlr_get_by_name(const char *name)
{
	struct nvme_bdev_ctrlr *nbdev_ctrlr;

	TAILQ_FOREACH(nbdev_ctrlr, &g_nvme_bdev_ctrlrs, tailq) {
		if (strcmp(name, nbdev_ctrlr->name) == 0) {
			break;
		}
	}

	return nbdev_ctrlr;
}

static struct nvme_ctrlr *
nvme_bdev_ctrlr_get_ctrlr(struct nvme_bdev_ctrlr *nbdev_ctrlr,
			  const struct spdk_nvme_transport_id *trid)
{
	struct nvme_ctrlr *nvme_ctrlr;

	TAILQ_FOREACH(nvme_ctrlr, &nbdev_ctrlr->ctrlrs, tailq) {
		if (spdk_nvme_transport_id_compare(trid, &nvme_ctrlr->active_path_id->trid) == 0) {
			break;
		}
	}

	return nvme_ctrlr;
}

static struct nvme_bdev *
nvme_bdev_ctrlr_get_bdev(struct nvme_bdev_ctrlr *nbdev_ctrlr, uint32_t nsid)
{
	struct nvme_bdev *bdev;

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	TAILQ_FOREACH(bdev, &nbdev_ctrlr->bdevs, tailq) {
		if (bdev->nsid == nsid) {
			break;
		}
	}
	pthread_mutex_unlock(&g_bdev_nvme_mutex);

	return bdev;
}

struct nvme_ns *
nvme_ctrlr_get_ns(struct nvme_ctrlr *nvme_ctrlr, uint32_t nsid)
{
	struct nvme_ns ns;

	assert(nsid > 0);

	ns.id = nsid;
	return RB_FIND(nvme_ns_tree, &nvme_ctrlr->namespaces, &ns);
}

struct nvme_ns *
nvme_ctrlr_get_first_active_ns(struct nvme_ctrlr *nvme_ctrlr)
{
	return RB_MIN(nvme_ns_tree, &nvme_ctrlr->namespaces);
}

struct nvme_ns *
nvme_ctrlr_get_next_active_ns(struct nvme_ctrlr *nvme_ctrlr, struct nvme_ns *ns)
{
	if (ns == NULL) {
		return NULL;
	}

	return RB_NEXT(nvme_ns_tree, &nvme_ctrlr->namespaces, ns);
}

static struct nvme_ctrlr *
nvme_ctrlr_get(const struct spdk_nvme_transport_id *trid)
{
	struct nvme_bdev_ctrlr	*nbdev_ctrlr;
	struct nvme_ctrlr	*nvme_ctrlr = NULL;

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	TAILQ_FOREACH(nbdev_ctrlr, &g_nvme_bdev_ctrlrs, tailq) {
		nvme_ctrlr = nvme_bdev_ctrlr_get_ctrlr(nbdev_ctrlr, trid);
		if (nvme_ctrlr != NULL) {
			break;
		}
	}
	pthread_mutex_unlock(&g_bdev_nvme_mutex);

	return nvme_ctrlr;
}

struct nvme_ctrlr *
nvme_ctrlr_get_by_name(const char *name)
{
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	struct nvme_ctrlr *nvme_ctrlr = NULL;

	if (name == NULL) {
		return NULL;
	}

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name(name);
	if (nbdev_ctrlr != NULL) {
		nvme_ctrlr = TAILQ_FIRST(&nbdev_ctrlr->ctrlrs);
	}
	pthread_mutex_unlock(&g_bdev_nvme_mutex);

	return nvme_ctrlr;
}

void
nvme_bdev_ctrlr_for_each(nvme_bdev_ctrlr_for_each_fn fn, void *ctx)
{
	struct nvme_bdev_ctrlr *nbdev_ctrlr;

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	TAILQ_FOREACH(nbdev_ctrlr, &g_nvme_bdev_ctrlrs, tailq) {
		fn(nbdev_ctrlr, ctx);
	}
	pthread_mutex_unlock(&g_bdev_nvme_mutex);
}

void
nvme_bdev_dump_trid_json(const struct spdk_nvme_transport_id *trid, struct spdk_json_write_ctx *w)
{
	const char *trtype_str;
	const char *adrfam_str;

	trtype_str = spdk_nvme_transport_id_trtype_str(trid->trtype);
	if (trtype_str) {
		spdk_json_write_named_string(w, "trtype", trtype_str);
	}

	adrfam_str = spdk_nvme_transport_id_adrfam_str(trid->adrfam);
	if (adrfam_str) {
		spdk_json_write_named_string(w, "adrfam", adrfam_str);
	}

	if (trid->traddr[0] != '\0') {
		spdk_json_write_named_string(w, "traddr", trid->traddr);
	}

	if (trid->trsvcid[0] != '\0') {
		spdk_json_write_named_string(w, "trsvcid", trid->trsvcid);
	}

	if (trid->subnqn[0] != '\0') {
		spdk_json_write_named_string(w, "subnqn", trid->subnqn);
	}
}

static void
nvme_bdev_ctrlr_delete(struct nvme_bdev_ctrlr *nbdev_ctrlr,
		       struct nvme_ctrlr *nvme_ctrlr)
{
	SPDK_DTRACE_PROBE1(bdev_nvme_ctrlr_delete, nvme_ctrlr->nbdev_ctrlr->name);
	pthread_mutex_lock(&g_bdev_nvme_mutex);

	TAILQ_REMOVE(&nbdev_ctrlr->ctrlrs, nvme_ctrlr, tailq);
	if (!TAILQ_EMPTY(&nbdev_ctrlr->ctrlrs)) {
		pthread_mutex_unlock(&g_bdev_nvme_mutex);

		return;
	}
	TAILQ_REMOVE(&g_nvme_bdev_ctrlrs, nbdev_ctrlr, tailq);

	pthread_mutex_unlock(&g_bdev_nvme_mutex);

	assert(TAILQ_EMPTY(&nbdev_ctrlr->bdevs));

	free(nbdev_ctrlr->name);
	free(nbdev_ctrlr);
}

static void
_nvme_ctrlr_delete(struct nvme_ctrlr *nvme_ctrlr)
{
	struct nvme_path_id *path_id, *tmp_path;
	struct nvme_ns *ns, *tmp_ns;

	free(nvme_ctrlr->copied_ana_desc);
	spdk_free(nvme_ctrlr->ana_log_page);

	if (nvme_ctrlr->opal_dev) {
		spdk_opal_dev_destruct(nvme_ctrlr->opal_dev);
		nvme_ctrlr->opal_dev = NULL;
	}

	if (nvme_ctrlr->nbdev_ctrlr) {
		nvme_bdev_ctrlr_delete(nvme_ctrlr->nbdev_ctrlr, nvme_ctrlr);
	}

	RB_FOREACH_SAFE(ns, nvme_ns_tree, &nvme_ctrlr->namespaces, tmp_ns) {
		RB_REMOVE(nvme_ns_tree, &nvme_ctrlr->namespaces, ns);
		free(ns);
	}

	TAILQ_FOREACH_SAFE(path_id, &nvme_ctrlr->trids, link, tmp_path) {
		TAILQ_REMOVE(&nvme_ctrlr->trids, path_id, link);
		free(path_id);
	}

	pthread_mutex_destroy(&nvme_ctrlr->mutex);

	free(nvme_ctrlr);

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	if (g_bdev_nvme_module_finish && TAILQ_EMPTY(&g_nvme_bdev_ctrlrs)) {
		pthread_mutex_unlock(&g_bdev_nvme_mutex);
		spdk_io_device_unregister(&g_nvme_bdev_ctrlrs, NULL);
		spdk_bdev_module_fini_done();
		return;
	}
	pthread_mutex_unlock(&g_bdev_nvme_mutex);
}

static int
nvme_detach_poller(void *arg)
{
	struct nvme_ctrlr *nvme_ctrlr = arg;
	int rc;

	rc = spdk_nvme_detach_poll_async(nvme_ctrlr->detach_ctx);
	if (rc != -EAGAIN) {
		spdk_poller_unregister(&nvme_ctrlr->reset_detach_poller);
		_nvme_ctrlr_delete(nvme_ctrlr);
	}

	return SPDK_POLLER_BUSY;
}

static void
nvme_ctrlr_delete(struct nvme_ctrlr *nvme_ctrlr)
{
	int rc;

	spdk_poller_unregister(&nvme_ctrlr->reconnect_delay_timer);

	/* First, unregister the adminq poller, as the driver will poll adminq if necessary */
	spdk_poller_unregister(&nvme_ctrlr->adminq_timer_poller);

	/* If we got here, the reset/detach poller cannot be active */
	assert(nvme_ctrlr->reset_detach_poller == NULL);
	nvme_ctrlr->reset_detach_poller = SPDK_POLLER_REGISTER(nvme_detach_poller,
					  nvme_ctrlr, 1000);
	if (nvme_ctrlr->reset_detach_poller == NULL) {
		SPDK_ERRLOG("Failed to register detach poller\n");
		goto error;
	}

	rc = spdk_nvme_detach_async(nvme_ctrlr->ctrlr, &nvme_ctrlr->detach_ctx);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to detach the NVMe controller\n");
		goto error;
	}

	return;
error:
	/* We don't have a good way to handle errors here, so just do what we can and delete the
	 * controller without detaching the underlying NVMe device.
	 */
	spdk_poller_unregister(&nvme_ctrlr->reset_detach_poller);
	_nvme_ctrlr_delete(nvme_ctrlr);
}

static void
nvme_ctrlr_unregister_cb(void *io_device)
{
	struct nvme_ctrlr *nvme_ctrlr = io_device;

	nvme_ctrlr_delete(nvme_ctrlr);
}

static void
nvme_ctrlr_unregister(struct nvme_ctrlr *nvme_ctrlr)
{
	spdk_io_device_unregister(nvme_ctrlr, nvme_ctrlr_unregister_cb);
}

static bool
nvme_ctrlr_can_be_unregistered(struct nvme_ctrlr *nvme_ctrlr)
{
	if (!nvme_ctrlr->destruct) {
		return false;
	}

	if (nvme_ctrlr->ref > 0) {
		return false;
	}

	if (nvme_ctrlr->resetting) {
		return false;
	}

	if (nvme_ctrlr->ana_log_page_updating) {
		return false;
	}

	return true;
}

static void
nvme_ctrlr_release(struct nvme_ctrlr *nvme_ctrlr)
{
	pthread_mutex_lock(&nvme_ctrlr->mutex);
	SPDK_DTRACE_PROBE2(bdev_nvme_ctrlr_release, nvme_ctrlr->nbdev_ctrlr->name, nvme_ctrlr->ref);

	assert(nvme_ctrlr->ref > 0);
	nvme_ctrlr->ref--;

	if (!nvme_ctrlr_can_be_unregistered(nvme_ctrlr)) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		return;
	}

	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	nvme_ctrlr_unregister(nvme_ctrlr);
}

static struct nvme_io_path *
_bdev_nvme_get_io_path(struct nvme_bdev_channel *nbdev_ch, struct nvme_ns *nvme_ns)
{
	struct nvme_io_path *io_path;

	STAILQ_FOREACH(io_path, &nbdev_ch->io_path_list, stailq) {
		if (io_path->nvme_ns == nvme_ns) {
			break;
		}
	}

	return io_path;
}

static int
_bdev_nvme_add_io_path(struct nvme_bdev_channel *nbdev_ch, struct nvme_ns *nvme_ns)
{
	struct nvme_io_path *io_path;
	struct spdk_io_channel *ch;

	io_path = calloc(1, sizeof(*io_path));
	if (io_path == NULL) {
		SPDK_ERRLOG("Failed to alloc io_path.\n");
		return -ENOMEM;
	}

	ch = spdk_get_io_channel(nvme_ns->ctrlr);
	if (ch == NULL) {
		free(io_path);
		SPDK_ERRLOG("Failed to alloc io_channel.\n");
		return -ENOMEM;
	}

	io_path->ctrlr_ch = spdk_io_channel_get_ctx(ch);
	TAILQ_INSERT_TAIL(&io_path->ctrlr_ch->io_path_list, io_path, tailq);

	io_path->nvme_ns = nvme_ns;

	io_path->nbdev_ch = nbdev_ch;
	STAILQ_INSERT_TAIL(&nbdev_ch->io_path_list, io_path, stailq);

	nbdev_ch->current_io_path = NULL;

	return 0;
}

static void
_bdev_nvme_delete_io_path(struct nvme_bdev_channel *nbdev_ch, struct nvme_io_path *io_path)
{
	struct spdk_io_channel *ch;

	nbdev_ch->current_io_path = NULL;

	STAILQ_REMOVE(&nbdev_ch->io_path_list, io_path, nvme_io_path, stailq);

	TAILQ_REMOVE(&io_path->ctrlr_ch->io_path_list, io_path, tailq);
	ch = spdk_io_channel_from_ctx(io_path->ctrlr_ch);
	spdk_put_io_channel(ch);

	free(io_path);
}

static void
_bdev_nvme_delete_io_paths(struct nvme_bdev_channel *nbdev_ch)
{
	struct nvme_io_path *io_path, *tmp_io_path;

	STAILQ_FOREACH_SAFE(io_path, &nbdev_ch->io_path_list, stailq, tmp_io_path) {
		_bdev_nvme_delete_io_path(nbdev_ch, io_path);
	}
}

static int
bdev_nvme_create_bdev_channel_cb(void *io_device, void *ctx_buf)
{
	struct nvme_bdev_channel *nbdev_ch = ctx_buf;
	struct nvme_bdev *nbdev = io_device;
	struct nvme_ns *nvme_ns;
	int rc;

	STAILQ_INIT(&nbdev_ch->io_path_list);
	TAILQ_INIT(&nbdev_ch->retry_io_list);

	pthread_mutex_lock(&nbdev->mutex);
	TAILQ_FOREACH(nvme_ns, &nbdev->nvme_ns_list, tailq) {
		rc = _bdev_nvme_add_io_path(nbdev_ch, nvme_ns);
		if (rc != 0) {
			pthread_mutex_unlock(&nbdev->mutex);

			_bdev_nvme_delete_io_paths(nbdev_ch);
			return rc;
		}
	}
	pthread_mutex_unlock(&nbdev->mutex);

	return 0;
}

static void
bdev_nvme_abort_retry_ios(struct nvme_bdev_channel *nbdev_ch)
{
	struct spdk_bdev_io *bdev_io, *tmp_io;

	TAILQ_FOREACH_SAFE(bdev_io, &nbdev_ch->retry_io_list, module_link, tmp_io) {
		TAILQ_REMOVE(&nbdev_ch->retry_io_list, bdev_io, module_link);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_ABORTED);
	}

	spdk_poller_unregister(&nbdev_ch->retry_io_poller);
}

static void
bdev_nvme_destroy_bdev_channel_cb(void *io_device, void *ctx_buf)
{
	struct nvme_bdev_channel *nbdev_ch = ctx_buf;

	bdev_nvme_abort_retry_ios(nbdev_ch);
	_bdev_nvme_delete_io_paths(nbdev_ch);
}

static inline bool
bdev_nvme_io_type_is_admin(enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_NVME_ADMIN:
	case SPDK_BDEV_IO_TYPE_ABORT:
		return true;
	default:
		break;
	}

	return false;
}

static inline bool
nvme_ns_is_accessible(struct nvme_ns *nvme_ns)
{
	if (spdk_unlikely(nvme_ns->ana_state_updating)) {
		return false;
	}

	switch (nvme_ns->ana_state) {
	case SPDK_NVME_ANA_OPTIMIZED_STATE:
	case SPDK_NVME_ANA_NON_OPTIMIZED_STATE:
		return true;
	default:
		break;
	}

	return false;
}

static inline bool
nvme_io_path_is_connected(struct nvme_io_path *io_path)
{
	return io_path->ctrlr_ch->qpair != NULL;
}

static inline bool
nvme_io_path_is_available(struct nvme_io_path *io_path)
{
	if (spdk_unlikely(!nvme_io_path_is_connected(io_path))) {
		return false;
	}

	if (spdk_unlikely(!nvme_ns_is_accessible(io_path->nvme_ns))) {
		return false;
	}

	return true;
}

static inline bool
nvme_io_path_is_failed(struct nvme_io_path *io_path)
{
	struct nvme_ctrlr *nvme_ctrlr;

	nvme_ctrlr = nvme_ctrlr_channel_get_ctrlr(io_path->ctrlr_ch);

	if (nvme_ctrlr->destruct) {
		return true;
	}

	if (nvme_ctrlr->fast_io_fail_timedout) {
		return true;
	}

	if (nvme_ctrlr->resetting) {
		if (nvme_ctrlr->reconnect_delay_sec != 0) {
			return false;
		} else {
			return true;
		}
	}

	if (nvme_ctrlr->reconnect_is_delayed) {
		return false;
	}

	if (spdk_nvme_ctrlr_is_failed(nvme_ctrlr->ctrlr)) {
		return true;
	} else {
		return false;
	}
}

static bool
nvme_ctrlr_is_available(struct nvme_ctrlr *nvme_ctrlr)
{
	if (nvme_ctrlr->destruct) {
		return false;
	}

	if (spdk_nvme_ctrlr_is_failed(nvme_ctrlr->ctrlr)) {
		return false;
	}

	if (nvme_ctrlr->resetting || nvme_ctrlr->reconnect_is_delayed) {
		return false;
	}

	return true;
}

static inline struct nvme_io_path *
bdev_nvme_find_io_path(struct nvme_bdev_channel *nbdev_ch)
{
	struct nvme_io_path *io_path, *non_optimized = NULL;

	if (spdk_likely(nbdev_ch->current_io_path != NULL)) {
		return nbdev_ch->current_io_path;
	}

	STAILQ_FOREACH(io_path, &nbdev_ch->io_path_list, stailq) {
		if (spdk_unlikely(!nvme_io_path_is_connected(io_path))) {
			/* The device is currently resetting. */
			continue;
		}

		if (spdk_unlikely(io_path->nvme_ns->ana_state_updating)) {
			continue;
		}

		switch (io_path->nvme_ns->ana_state) {
		case SPDK_NVME_ANA_OPTIMIZED_STATE:
			nbdev_ch->current_io_path = io_path;
			return io_path;
		case SPDK_NVME_ANA_NON_OPTIMIZED_STATE:
			if (non_optimized == NULL) {
				non_optimized = io_path;
			}
			break;
		default:
			break;
		}
	}

	return non_optimized;
}

/* Return true if there is any io_path whose qpair is active or ctrlr is not failed,
 * or false otherwise.
 *
 * If any io_path has an active qpair but find_io_path() returned NULL, its namespace
 * is likely to be non-accessible now but may become accessible.
 *
 * If any io_path has an unfailed ctrlr but find_io_path() returned NULL, the ctrlr
 * is likely to be resetting now but the reset may succeed. A ctrlr is set to unfailed
 * when starting to reset it but it is set to failed when the reset failed. Hence, if
 * a ctrlr is unfailed, it is likely that it works fine or is resetting.
 */
static bool
any_io_path_may_become_available(struct nvme_bdev_channel *nbdev_ch)
{
	struct nvme_io_path *io_path;

	STAILQ_FOREACH(io_path, &nbdev_ch->io_path_list, stailq) {
		if (nvme_io_path_is_connected(io_path) ||
		    !nvme_io_path_is_failed(io_path)) {
			return true;
		}
	}

	return false;
}

static bool
any_ctrlr_may_become_available(struct nvme_bdev_channel *nbdev_ch)
{
	struct nvme_io_path *io_path;

	STAILQ_FOREACH(io_path, &nbdev_ch->io_path_list, stailq) {
		if (!nvme_io_path_is_failed(io_path)) {
			return true;
		}
	}

	return false;
}

static int
bdev_nvme_retry_ios(void *arg)
{
	struct nvme_bdev_channel *nbdev_ch = arg;
	struct spdk_io_channel *ch = spdk_io_channel_from_ctx(nbdev_ch);
	struct spdk_bdev_io *bdev_io, *tmp_bdev_io;
	struct nvme_bdev_io *bio;
	uint64_t now, delay_us;

	now = spdk_get_ticks();

	TAILQ_FOREACH_SAFE(bdev_io, &nbdev_ch->retry_io_list, module_link, tmp_bdev_io) {
		bio = (struct nvme_bdev_io *)bdev_io->driver_ctx;
		if (bio->retry_ticks > now) {
			break;
		}

		TAILQ_REMOVE(&nbdev_ch->retry_io_list, bdev_io, module_link);

		bdev_nvme_submit_request(ch, bdev_io);
	}

	spdk_poller_unregister(&nbdev_ch->retry_io_poller);

	bdev_io = TAILQ_FIRST(&nbdev_ch->retry_io_list);
	if (bdev_io != NULL) {
		bio = (struct nvme_bdev_io *)bdev_io->driver_ctx;

		delay_us = (bio->retry_ticks - now) * SPDK_SEC_TO_USEC / spdk_get_ticks_hz();

		nbdev_ch->retry_io_poller = SPDK_POLLER_REGISTER(bdev_nvme_retry_ios, nbdev_ch,
					    delay_us);
	}

	return SPDK_POLLER_BUSY;
}

static void
bdev_nvme_queue_retry_io(struct nvme_bdev_channel *nbdev_ch,
			 struct nvme_bdev_io *bio, uint64_t delay_ms)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);
	struct spdk_bdev_io *tmp_bdev_io;
	struct nvme_bdev_io *tmp_bio;

	bio->retry_ticks = spdk_get_ticks() + delay_ms * spdk_get_ticks_hz() / 1000ULL;

	TAILQ_FOREACH_REVERSE(tmp_bdev_io, &nbdev_ch->retry_io_list, retry_io_head, module_link) {
		tmp_bio = (struct nvme_bdev_io *)tmp_bdev_io->driver_ctx;

		if (tmp_bio->retry_ticks <= bio->retry_ticks) {
			TAILQ_INSERT_AFTER(&nbdev_ch->retry_io_list, tmp_bdev_io, bdev_io,
					   module_link);
			return;
		}
	}

	/* No earlier I/Os were found. This I/O must be the new head. */
	TAILQ_INSERT_HEAD(&nbdev_ch->retry_io_list, bdev_io, module_link);

	spdk_poller_unregister(&nbdev_ch->retry_io_poller);

	nbdev_ch->retry_io_poller = SPDK_POLLER_REGISTER(bdev_nvme_retry_ios, nbdev_ch,
				    delay_ms * 1000ULL);
}

static inline void
bdev_nvme_io_complete_nvme_status(struct nvme_bdev_io *bio,
				  const struct spdk_nvme_cpl *cpl)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);
	struct nvme_bdev_channel *nbdev_ch;
	struct nvme_ctrlr *nvme_ctrlr;
	const struct spdk_nvme_ctrlr_data *cdata;
	uint64_t delay_ms;

	assert(!bdev_nvme_io_type_is_admin(bdev_io->type));

	if (spdk_likely(spdk_nvme_cpl_is_success(cpl))) {
		goto complete;
	}

	if (cpl->status.dnr != 0 || (g_opts.bdev_retry_count != -1 &&
				     bio->retry_count >= g_opts.bdev_retry_count)) {
		goto complete;
	}

	nbdev_ch = spdk_io_channel_get_ctx(spdk_bdev_io_get_io_channel(bdev_io));

	assert(bio->io_path != NULL);
	nvme_ctrlr = nvme_ctrlr_channel_get_ctrlr(bio->io_path->ctrlr_ch);

	if (spdk_nvme_cpl_is_path_error(cpl) ||
	    spdk_nvme_cpl_is_aborted_sq_deletion(cpl) ||
	    !nvme_io_path_is_available(bio->io_path) ||
	    !nvme_ctrlr_is_available(nvme_ctrlr)) {
		nbdev_ch->current_io_path = NULL;
		if (spdk_nvme_cpl_is_ana_error(cpl)) {
			if (nvme_ctrlr_read_ana_log_page(nvme_ctrlr) == 0) {
				bio->io_path->nvme_ns->ana_state_updating = true;
			}
		}
		delay_ms = 0;
	} else if (spdk_nvme_cpl_is_aborted_by_request(cpl)) {
		goto complete;
	} else {
		bio->retry_count++;

		cdata = spdk_nvme_ctrlr_get_data(nvme_ctrlr->ctrlr);

		if (cpl->status.crd != 0) {
			delay_ms = cdata->crdt[cpl->status.crd] * 100;
		} else {
			delay_ms = 0;
		}
	}

	if (any_io_path_may_become_available(nbdev_ch)) {
		bdev_nvme_queue_retry_io(nbdev_ch, bio, delay_ms);
		return;
	}

complete:
	bio->retry_count = 0;
	spdk_bdev_io_complete_nvme_status(bdev_io, cpl->cdw0, cpl->status.sct, cpl->status.sc);
}

static inline void
bdev_nvme_io_complete(struct nvme_bdev_io *bio, int rc)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);
	struct nvme_bdev_channel *nbdev_ch;
	enum spdk_bdev_io_status io_status;

	switch (rc) {
	case 0:
		io_status = SPDK_BDEV_IO_STATUS_SUCCESS;
		break;
	case -ENOMEM:
		io_status = SPDK_BDEV_IO_STATUS_NOMEM;
		break;
	case -ENXIO:
		nbdev_ch = spdk_io_channel_get_ctx(spdk_bdev_io_get_io_channel(bdev_io));

		nbdev_ch->current_io_path = NULL;

		if (any_io_path_may_become_available(nbdev_ch)) {
			bdev_nvme_queue_retry_io(nbdev_ch, bio, 1000ULL);
			return;
		}

	/* fallthrough */
	default:
		io_status = SPDK_BDEV_IO_STATUS_FAILED;
		break;
	}

	bio->retry_count = 0;
	spdk_bdev_io_complete(bdev_io, io_status);
}

static inline void
bdev_nvme_admin_passthru_complete(struct nvme_bdev_io *bio, int rc)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);
	struct nvme_bdev_channel *nbdev_ch;
	enum spdk_bdev_io_status io_status;

	switch (rc) {
	case 0:
		io_status = SPDK_BDEV_IO_STATUS_SUCCESS;
		break;
	case -ENOMEM:
		io_status = SPDK_BDEV_IO_STATUS_NOMEM;
		break;
	case -ENXIO:
		nbdev_ch = spdk_io_channel_get_ctx(spdk_bdev_io_get_io_channel(bdev_io));

		if (any_ctrlr_may_become_available(nbdev_ch)) {
			bdev_nvme_queue_retry_io(nbdev_ch, bio, 1000ULL);
			return;
		}

	/* fallthrough */
	default:
		io_status = SPDK_BDEV_IO_STATUS_FAILED;
		break;
	}

	bio->retry_count = 0;
	spdk_bdev_io_complete(bdev_io, io_status);
}

static void
_bdev_nvme_clear_io_path_cache(struct nvme_ctrlr_channel *ctrlr_ch)
{
	struct nvme_io_path *io_path;

	TAILQ_FOREACH(io_path, &ctrlr_ch->io_path_list, tailq) {
		io_path->nbdev_ch->current_io_path = NULL;
	}
}

static struct nvme_ctrlr_channel *
nvme_poll_group_get_ctrlr_channel(struct nvme_poll_group *group,
				  struct spdk_nvme_qpair *qpair)
{
	struct nvme_ctrlr_channel *ctrlr_ch;

	TAILQ_FOREACH(ctrlr_ch, &group->ctrlr_ch_list, tailq) {
		if (ctrlr_ch->qpair == qpair) {
			break;
		}
	}

	return ctrlr_ch;
}

static void
bdev_nvme_destroy_qpair(struct nvme_ctrlr_channel *ctrlr_ch)
{
	struct nvme_ctrlr *nvme_ctrlr __attribute__((unused));

	if (ctrlr_ch->qpair != NULL) {
		nvme_ctrlr = nvme_ctrlr_channel_get_ctrlr(ctrlr_ch);
		SPDK_DTRACE_PROBE2(bdev_nvme_destroy_qpair, nvme_ctrlr->nbdev_ctrlr->name,
				   spdk_nvme_qpair_get_id(ctrlr_ch->qpair));
		spdk_nvme_ctrlr_free_io_qpair(ctrlr_ch->qpair);
		ctrlr_ch->qpair = NULL;
	}

	_bdev_nvme_clear_io_path_cache(ctrlr_ch);
}

static void
bdev_nvme_disconnected_qpair_cb(struct spdk_nvme_qpair *qpair, void *poll_group_ctx)
{
	struct nvme_poll_group *group = poll_group_ctx;
	struct nvme_ctrlr_channel *ctrlr_ch;
	struct nvme_ctrlr *nvme_ctrlr;

	SPDK_NOTICELOG("qpair %p is disconnected, free the qpair and reset controller.\n", qpair);
	/*
	 * Free the I/O qpair and reset the nvme_ctrlr.
	 */
	ctrlr_ch = nvme_poll_group_get_ctrlr_channel(group, qpair);
	if (ctrlr_ch != NULL) {
		bdev_nvme_destroy_qpair(ctrlr_ch);

		nvme_ctrlr = nvme_ctrlr_channel_get_ctrlr(ctrlr_ch);
		bdev_nvme_reset(nvme_ctrlr);
	}
}

static int
bdev_nvme_poll(void *arg)
{
	struct nvme_poll_group *group = arg;
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

	return num_completions > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static int
bdev_nvme_poll_adminq(void *arg)
{
	int32_t rc;
	struct nvme_ctrlr *nvme_ctrlr = arg;

	assert(nvme_ctrlr != NULL);

	rc = spdk_nvme_ctrlr_process_admin_completions(nvme_ctrlr->ctrlr);
	if (rc < 0) {
		bdev_nvme_failover(nvme_ctrlr, false);
	}

	return rc == 0 ? SPDK_POLLER_IDLE : SPDK_POLLER_BUSY;
}

static void
_bdev_nvme_unregister_dev_cb(void *io_device)
{
	struct nvme_bdev *nvme_disk = io_device;

	free(nvme_disk->disk.name);
	free(nvme_disk);
}

static int
bdev_nvme_destruct(void *ctx)
{
	struct nvme_bdev *nvme_disk = ctx;
	struct nvme_ns *nvme_ns, *tmp_nvme_ns;

	SPDK_DTRACE_PROBE2(bdev_nvme_destruct, nvme_disk->nbdev_ctrlr->name, nvme_disk->nsid);

	TAILQ_FOREACH_SAFE(nvme_ns, &nvme_disk->nvme_ns_list, tailq, tmp_nvme_ns) {
		pthread_mutex_lock(&nvme_ns->ctrlr->mutex);

		nvme_ns->bdev = NULL;

		assert(nvme_ns->id > 0);

		if (nvme_ctrlr_get_ns(nvme_ns->ctrlr, nvme_ns->id) == NULL) {
			pthread_mutex_unlock(&nvme_ns->ctrlr->mutex);

			nvme_ctrlr_release(nvme_ns->ctrlr);
			free(nvme_ns);
		} else {
			pthread_mutex_unlock(&nvme_ns->ctrlr->mutex);
		}
	}

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	TAILQ_REMOVE(&nvme_disk->nbdev_ctrlr->bdevs, nvme_disk, tailq);
	pthread_mutex_unlock(&g_bdev_nvme_mutex);

	spdk_io_device_unregister(nvme_disk, _bdev_nvme_unregister_dev_cb);

	return 0;
}

static int
bdev_nvme_flush(struct nvme_bdev_io *bio, uint64_t offset, uint64_t nbytes)
{
	bdev_nvme_io_complete(bio, 0);

	return 0;
}

static int
bdev_nvme_create_qpair(struct nvme_ctrlr_channel *ctrlr_ch)
{
	struct nvme_ctrlr *nvme_ctrlr;
	struct spdk_nvme_io_qpair_opts opts;
	struct spdk_nvme_qpair *qpair;
	int rc;

	nvme_ctrlr = nvme_ctrlr_channel_get_ctrlr(ctrlr_ch);

	spdk_nvme_ctrlr_get_default_io_qpair_opts(nvme_ctrlr->ctrlr, &opts, sizeof(opts));
	opts.delay_cmd_submit = g_opts.delay_cmd_submit;
	opts.create_only = true;
	opts.async_mode = true;
	opts.io_queue_requests = spdk_max(g_opts.io_queue_requests, opts.io_queue_requests);
	g_opts.io_queue_requests = opts.io_queue_requests;

	qpair = spdk_nvme_ctrlr_alloc_io_qpair(nvme_ctrlr->ctrlr, &opts, sizeof(opts));
	if (qpair == NULL) {
		return -1;
	}

	SPDK_DTRACE_PROBE3(bdev_nvme_create_qpair, nvme_ctrlr->nbdev_ctrlr->name,
			   spdk_nvme_qpair_get_id(qpair), spdk_thread_get_id(nvme_ctrlr->thread));

	assert(ctrlr_ch->group != NULL);

	rc = spdk_nvme_poll_group_add(ctrlr_ch->group->group, qpair);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to begin polling on NVMe Channel.\n");
		goto err;
	}

	rc = spdk_nvme_ctrlr_connect_io_qpair(nvme_ctrlr->ctrlr, qpair);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to connect I/O qpair.\n");
		goto err;
	}

	ctrlr_ch->qpair = qpair;

	_bdev_nvme_clear_io_path_cache(ctrlr_ch);

	return 0;

err:
	spdk_nvme_ctrlr_free_io_qpair(qpair);

	return rc;
}

static void
bdev_nvme_complete_pending_resets(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct nvme_ctrlr_channel *ctrlr_ch = spdk_io_channel_get_ctx(_ch);
	enum spdk_bdev_io_status status = SPDK_BDEV_IO_STATUS_SUCCESS;
	struct spdk_bdev_io *bdev_io;

	if (spdk_io_channel_iter_get_ctx(i) != NULL) {
		status = SPDK_BDEV_IO_STATUS_FAILED;
	}

	while (!TAILQ_EMPTY(&ctrlr_ch->pending_resets)) {
		bdev_io = TAILQ_FIRST(&ctrlr_ch->pending_resets);
		TAILQ_REMOVE(&ctrlr_ch->pending_resets, bdev_io, module_link);
		spdk_bdev_io_complete(bdev_io, status);
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
bdev_nvme_failover_trid(struct nvme_ctrlr *nvme_ctrlr, bool remove)
{
	struct nvme_path_id *path_id, *next_path;
	int rc __attribute__((unused));

	path_id = TAILQ_FIRST(&nvme_ctrlr->trids);
	assert(path_id);
	assert(path_id == nvme_ctrlr->active_path_id);
	next_path = TAILQ_NEXT(path_id, link);

	path_id->is_failed = true;

	if (next_path) {
		assert(path_id->trid.trtype != SPDK_NVME_TRANSPORT_PCIE);

		SPDK_NOTICELOG("Start failover from %s:%s to %s:%s\n", path_id->trid.traddr,
			       path_id->trid.trsvcid,	next_path->trid.traddr, next_path->trid.trsvcid);

		spdk_nvme_ctrlr_fail(nvme_ctrlr->ctrlr);
		nvme_ctrlr->active_path_id = next_path;
		rc = spdk_nvme_ctrlr_set_trid(nvme_ctrlr->ctrlr, &next_path->trid);
		assert(rc == 0);
		TAILQ_REMOVE(&nvme_ctrlr->trids, path_id, link);
		if (!remove) {
			/** Shuffle the old trid to the end of the list and use the new one.
			 * Allows for round robin through multiple connections.
			 */
			TAILQ_INSERT_TAIL(&nvme_ctrlr->trids, path_id, link);
		} else {
			free(path_id);
		}
	}
}

static bool
bdev_nvme_check_ctrlr_loss_timeout(struct nvme_ctrlr *nvme_ctrlr)
{
	int32_t elapsed;

	if (nvme_ctrlr->ctrlr_loss_timeout_sec == 0 ||
	    nvme_ctrlr->ctrlr_loss_timeout_sec == -1) {
		return false;
	}

	elapsed = (spdk_get_ticks() - nvme_ctrlr->reset_start_tsc) / spdk_get_ticks_hz();
	if (elapsed >= nvme_ctrlr->ctrlr_loss_timeout_sec) {
		return true;
	} else {
		return false;
	}
}

static bool
bdev_nvme_check_fast_io_fail_timeout(struct nvme_ctrlr *nvme_ctrlr)
{
	uint32_t elapsed;

	if (nvme_ctrlr->fast_io_fail_timeout_sec == 0) {
		return false;
	}

	elapsed = (spdk_get_ticks() - nvme_ctrlr->reset_start_tsc) / spdk_get_ticks_hz();
	if (elapsed >= nvme_ctrlr->fast_io_fail_timeout_sec) {
		return true;
	} else {
		return false;
	}
}

enum bdev_nvme_op_after_reset {
	OP_NONE,
	OP_COMPLETE_PENDING_DESTRUCT,
	OP_DESTRUCT,
	OP_DELAYED_RECONNECT,
};

typedef enum bdev_nvme_op_after_reset _bdev_nvme_op_after_reset;

static _bdev_nvme_op_after_reset
bdev_nvme_check_op_after_reset(struct nvme_ctrlr *nvme_ctrlr, bool success)
{
	if (nvme_ctrlr_can_be_unregistered(nvme_ctrlr)) {
		/* Complete pending destruct after reset completes. */
		return OP_COMPLETE_PENDING_DESTRUCT;
	} else if (success || nvme_ctrlr->reconnect_delay_sec == 0) {
		nvme_ctrlr->reset_start_tsc = 0;
		return OP_NONE;
	} else if (bdev_nvme_check_ctrlr_loss_timeout(nvme_ctrlr)) {
		return OP_DESTRUCT;
	} else {
		if (bdev_nvme_check_fast_io_fail_timeout(nvme_ctrlr)) {
			nvme_ctrlr->fast_io_fail_timedout = true;
		}
		bdev_nvme_failover_trid(nvme_ctrlr, false);
		return OP_DELAYED_RECONNECT;
	}
}

static int _bdev_nvme_delete(struct nvme_ctrlr *nvme_ctrlr, bool hotplug);
static void bdev_nvme_reconnect_ctrlr(struct nvme_ctrlr *nvme_ctrlr);

static int
bdev_nvme_reconnect_delay_timer_expired(void *ctx)
{
	struct nvme_ctrlr *nvme_ctrlr = ctx;

	pthread_mutex_lock(&nvme_ctrlr->mutex);

	spdk_poller_unregister(&nvme_ctrlr->reconnect_delay_timer);

	assert(nvme_ctrlr->reconnect_is_delayed == true);
	nvme_ctrlr->reconnect_is_delayed = false;

	if (nvme_ctrlr->destruct) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		return SPDK_POLLER_BUSY;
	}

	assert(nvme_ctrlr->resetting == false);
	nvme_ctrlr->resetting = true;

	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	spdk_poller_resume(nvme_ctrlr->adminq_timer_poller);

	bdev_nvme_reconnect_ctrlr(nvme_ctrlr);
	return SPDK_POLLER_BUSY;
}

static void
bdev_nvme_start_reconnect_delay_timer(struct nvme_ctrlr *nvme_ctrlr)
{
	spdk_poller_pause(nvme_ctrlr->adminq_timer_poller);

	assert(nvme_ctrlr->reconnect_is_delayed == false);
	nvme_ctrlr->reconnect_is_delayed = true;

	assert(nvme_ctrlr->reconnect_delay_timer == NULL);
	nvme_ctrlr->reconnect_delay_timer = SPDK_POLLER_REGISTER(bdev_nvme_reconnect_delay_timer_expired,
					    nvme_ctrlr,
					    nvme_ctrlr->reconnect_delay_sec * SPDK_SEC_TO_USEC);
}

static void
_bdev_nvme_reset_complete(struct spdk_io_channel_iter *i, int status)
{
	struct nvme_ctrlr *nvme_ctrlr = spdk_io_channel_iter_get_io_device(i);
	bool success = spdk_io_channel_iter_get_ctx(i) == NULL;
	struct nvme_path_id *path_id;
	bdev_nvme_reset_cb reset_cb_fn = nvme_ctrlr->reset_cb_fn;
	void *reset_cb_arg = nvme_ctrlr->reset_cb_arg;
	enum bdev_nvme_op_after_reset op_after_reset;

	assert(nvme_ctrlr->thread == spdk_get_thread());

	nvme_ctrlr->reset_cb_fn = NULL;
	nvme_ctrlr->reset_cb_arg = NULL;

	if (!success) {
		SPDK_ERRLOG("Resetting controller failed.\n");
	} else {
		SPDK_NOTICELOG("Resetting controller successful.\n");
	}

	pthread_mutex_lock(&nvme_ctrlr->mutex);
	nvme_ctrlr->resetting = false;

	path_id = TAILQ_FIRST(&nvme_ctrlr->trids);
	assert(path_id != NULL);
	assert(path_id == nvme_ctrlr->active_path_id);

	path_id->is_failed = !success;

	op_after_reset = bdev_nvme_check_op_after_reset(nvme_ctrlr, success);

	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	if (reset_cb_fn) {
		reset_cb_fn(reset_cb_arg, success);
	}

	switch (op_after_reset) {
	case OP_COMPLETE_PENDING_DESTRUCT:
		nvme_ctrlr_unregister(nvme_ctrlr);
		break;
	case OP_DESTRUCT:
		_bdev_nvme_delete(nvme_ctrlr, false);
		break;
	case OP_DELAYED_RECONNECT:
		spdk_nvme_ctrlr_disconnect(nvme_ctrlr->ctrlr);
		bdev_nvme_start_reconnect_delay_timer(nvme_ctrlr);
		break;
	default:
		break;
	}
}

static void
bdev_nvme_reset_complete(struct nvme_ctrlr *nvme_ctrlr, bool success)
{
	/* Make sure we clear any pending resets before returning. */
	spdk_for_each_channel(nvme_ctrlr,
			      bdev_nvme_complete_pending_resets,
			      success ? NULL : (void *)0x1,
			      _bdev_nvme_reset_complete);
}

static void
bdev_nvme_reset_create_qpairs_failed(struct spdk_io_channel_iter *i, int status)
{
	struct nvme_ctrlr *nvme_ctrlr = spdk_io_channel_iter_get_io_device(i);

	bdev_nvme_reset_complete(nvme_ctrlr, false);
}

static void
bdev_nvme_reset_destroy_qpair(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct nvme_ctrlr_channel *ctrlr_ch = spdk_io_channel_get_ctx(ch);

	bdev_nvme_destroy_qpair(ctrlr_ch);

	spdk_for_each_channel_continue(i, 0);
}

static void
bdev_nvme_reset_create_qpairs_done(struct spdk_io_channel_iter *i, int status)
{
	struct nvme_ctrlr *nvme_ctrlr = spdk_io_channel_iter_get_io_device(i);

	if (status == 0) {
		bdev_nvme_reset_complete(nvme_ctrlr, true);
	} else {
		/* Delete the added qpairs and quiesce ctrlr to make the states clean. */
		spdk_for_each_channel(nvme_ctrlr,
				      bdev_nvme_reset_destroy_qpair,
				      NULL,
				      bdev_nvme_reset_create_qpairs_failed);
	}
}

static void
bdev_nvme_reset_create_qpair(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct nvme_ctrlr_channel *ctrlr_ch = spdk_io_channel_get_ctx(_ch);
	int rc;

	rc = bdev_nvme_create_qpair(ctrlr_ch);

	spdk_for_each_channel_continue(i, rc);
}

static int
bdev_nvme_reconnect_ctrlr_poll(void *arg)
{
	struct nvme_ctrlr *nvme_ctrlr = arg;
	int rc = -ETIMEDOUT;

	if (!bdev_nvme_check_ctrlr_loss_timeout(nvme_ctrlr)) {
		rc = spdk_nvme_ctrlr_reconnect_poll_async(nvme_ctrlr->ctrlr);
		if (rc == -EAGAIN) {
			return SPDK_POLLER_BUSY;
		}
	}

	spdk_poller_unregister(&nvme_ctrlr->reset_detach_poller);
	if (rc == 0) {
		/* Recreate all of the I/O queue pairs */
		spdk_for_each_channel(nvme_ctrlr,
				      bdev_nvme_reset_create_qpair,
				      NULL,
				      bdev_nvme_reset_create_qpairs_done);
	} else {
		bdev_nvme_reset_complete(nvme_ctrlr, false);
	}
	return SPDK_POLLER_BUSY;
}

static void
bdev_nvme_reconnect_ctrlr(struct nvme_ctrlr *nvme_ctrlr)
{
	spdk_nvme_ctrlr_reconnect_async(nvme_ctrlr->ctrlr);

	assert(nvme_ctrlr->reset_detach_poller == NULL);
	nvme_ctrlr->reset_detach_poller = SPDK_POLLER_REGISTER(bdev_nvme_reconnect_ctrlr_poll,
					  nvme_ctrlr, 0);
}

static void bdev_nvme_reset_complete(struct nvme_ctrlr *nvme_ctrlr, bool success);

static void
bdev_nvme_reset_ctrlr(struct spdk_io_channel_iter *i, int status)
{
	struct nvme_ctrlr *nvme_ctrlr = spdk_io_channel_iter_get_io_device(i);
	int rc;

	assert(status == 0);

	rc = spdk_nvme_ctrlr_disconnect(nvme_ctrlr->ctrlr);
	if (rc != 0) {
		/* Disconnect fails if ctrlr is already resetting or removed. In this case,
		 * fail the reset sequence immediately.
		 */
		bdev_nvme_reset_complete(nvme_ctrlr, false);
		return;
	}

	bdev_nvme_reconnect_ctrlr(nvme_ctrlr);
}

static void
_bdev_nvme_reset(void *ctx)
{
	struct nvme_ctrlr *nvme_ctrlr = ctx;

	assert(nvme_ctrlr->resetting == true);
	assert(nvme_ctrlr->thread == spdk_get_thread());

	spdk_nvme_ctrlr_prepare_for_reset(nvme_ctrlr->ctrlr);

	/* First, delete all NVMe I/O queue pairs. */
	spdk_for_each_channel(nvme_ctrlr,
			      bdev_nvme_reset_destroy_qpair,
			      NULL,
			      bdev_nvme_reset_ctrlr);
}

static int
bdev_nvme_reset(struct nvme_ctrlr *nvme_ctrlr)
{
	pthread_mutex_lock(&nvme_ctrlr->mutex);
	if (nvme_ctrlr->destruct) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		return -ENXIO;
	}

	if (nvme_ctrlr->resetting) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		SPDK_NOTICELOG("Unable to perform reset, already in progress.\n");
		return -EBUSY;
	}

	if (nvme_ctrlr->reconnect_is_delayed) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		SPDK_NOTICELOG("Reconnect is already scheduled.\n");
		return -EBUSY;
	}

	nvme_ctrlr->resetting = true;

	assert(nvme_ctrlr->reset_start_tsc == 0);
	nvme_ctrlr->reset_start_tsc = spdk_get_ticks();

	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	spdk_thread_send_msg(nvme_ctrlr->thread, _bdev_nvme_reset, nvme_ctrlr);
	return 0;
}

int
bdev_nvme_reset_rpc(struct nvme_ctrlr *nvme_ctrlr, bdev_nvme_reset_cb cb_fn, void *cb_arg)
{
	int rc;

	rc = bdev_nvme_reset(nvme_ctrlr);
	if (rc == 0) {
		nvme_ctrlr->reset_cb_fn = cb_fn;
		nvme_ctrlr->reset_cb_arg = cb_arg;
	}
	return rc;
}

static int _bdev_nvme_reset_io(struct nvme_io_path *io_path, struct nvme_bdev_io *bio);

static void
bdev_nvme_reset_io_complete(struct nvme_bdev_io *bio)
{
	enum spdk_bdev_io_status io_status;

	if (bio->cpl.cdw0 == 0) {
		io_status = SPDK_BDEV_IO_STATUS_SUCCESS;
	} else {
		io_status = SPDK_BDEV_IO_STATUS_FAILED;
	}

	spdk_bdev_io_complete(spdk_bdev_io_from_ctx(bio), io_status);
}

static void
_bdev_nvme_reset_io_continue(void *ctx)
{
	struct nvme_bdev_io *bio = ctx;
	struct nvme_io_path *prev_io_path, *next_io_path;
	int rc;

	prev_io_path = bio->io_path;
	bio->io_path = NULL;

	if (bio->cpl.cdw0 != 0) {
		goto complete;
	}

	next_io_path = STAILQ_NEXT(prev_io_path, stailq);
	if (next_io_path == NULL) {
		goto complete;
	}

	rc = _bdev_nvme_reset_io(next_io_path, bio);
	if (rc == 0) {
		return;
	}

	bio->cpl.cdw0 = 1;

complete:
	bdev_nvme_reset_io_complete(bio);
}

static void
bdev_nvme_reset_io_continue(void *cb_arg, bool success)
{
	struct nvme_bdev_io *bio = cb_arg;

	bio->cpl.cdw0 = !success;

	spdk_thread_send_msg(bio->orig_thread, _bdev_nvme_reset_io_continue, bio);
}

static int
_bdev_nvme_reset_io(struct nvme_io_path *io_path, struct nvme_bdev_io *bio)
{
	struct nvme_ctrlr_channel *ctrlr_ch = io_path->ctrlr_ch;
	struct nvme_ctrlr *nvme_ctrlr;
	struct spdk_bdev_io *bdev_io;
	int rc;

	nvme_ctrlr = nvme_ctrlr_channel_get_ctrlr(ctrlr_ch);

	rc = bdev_nvme_reset(nvme_ctrlr);
	if (rc == 0) {
		assert(bio->io_path == NULL);
		bio->io_path = io_path;

		assert(nvme_ctrlr->reset_cb_fn == NULL);
		assert(nvme_ctrlr->reset_cb_arg == NULL);
		nvme_ctrlr->reset_cb_fn = bdev_nvme_reset_io_continue;
		nvme_ctrlr->reset_cb_arg = bio;
	} else if (rc == -EBUSY) {
		/*
		 * Reset call is queued only if it is from the app framework. This is on purpose so that
		 * we don't interfere with the app framework reset strategy. i.e. we are deferring to the
		 * upper level. If they are in the middle of a reset, we won't try to schedule another one.
		 */
		bdev_io = spdk_bdev_io_from_ctx(bio);
		TAILQ_INSERT_TAIL(&ctrlr_ch->pending_resets, bdev_io, module_link);
	} else {
		return rc;
	}

	return 0;
}

static void
bdev_nvme_reset_io(struct nvme_bdev_channel *nbdev_ch, struct nvme_bdev_io *bio)
{
	struct nvme_io_path *io_path;
	int rc;

	bio->cpl.cdw0 = 0;
	bio->orig_thread = spdk_get_thread();

	/* Reset only the first nvme_ctrlr in the nvme_bdev_ctrlr for now.
	 *
	 * TODO: Reset all nvme_ctrlrs in the nvme_bdev_ctrlr sequentially.
	 * This will be done in the following patches.
	 */
	io_path = STAILQ_FIRST(&nbdev_ch->io_path_list);
	assert(io_path != NULL);

	rc = _bdev_nvme_reset_io(io_path, bio);
	if (rc != 0) {
		bio->cpl.cdw0 = 1;
		bdev_nvme_reset_io_complete(bio);
	}
}

static int
bdev_nvme_failover(struct nvme_ctrlr *nvme_ctrlr, bool remove)
{
	pthread_mutex_lock(&nvme_ctrlr->mutex);
	if (nvme_ctrlr->destruct) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		/* Don't bother resetting if the controller is in the process of being destructed. */
		return -ENXIO;
	}

	if (nvme_ctrlr->resetting) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		SPDK_NOTICELOG("Unable to perform reset, already in progress.\n");
		return -EBUSY;
	}

	bdev_nvme_failover_trid(nvme_ctrlr, remove);

	if (nvme_ctrlr->reconnect_is_delayed) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		SPDK_NOTICELOG("Reconnect is already scheduled.\n");

		/* We rely on the next reconnect for the failover. */
		return 0;
	}

	nvme_ctrlr->resetting = true;

	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	spdk_thread_send_msg(nvme_ctrlr->thread, _bdev_nvme_reset, nvme_ctrlr);
	return 0;
}

static int bdev_nvme_unmap(struct nvme_bdev_io *bio, uint64_t offset_blocks,
			   uint64_t num_blocks);

static int bdev_nvme_write_zeroes(struct nvme_bdev_io *bio, uint64_t offset_blocks,
				  uint64_t num_blocks);

static void
bdev_nvme_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		     bool success)
{
	struct nvme_bdev_io *bio = (struct nvme_bdev_io *)bdev_io->driver_ctx;
	struct spdk_bdev *bdev = bdev_io->bdev;
	int ret;

	if (!success) {
		ret = -EINVAL;
		goto exit;
	}

	if (spdk_unlikely(!nvme_io_path_is_available(bio->io_path))) {
		ret = -ENXIO;
		goto exit;
	}

	ret = bdev_nvme_readv(bio,
			      bdev_io->u.bdev.iovs,
			      bdev_io->u.bdev.iovcnt,
			      bdev_io->u.bdev.md_buf,
			      bdev_io->u.bdev.num_blocks,
			      bdev_io->u.bdev.offset_blocks,
			      bdev->dif_check_flags,
			      bdev_io->internal.ext_opts);

exit:
	if (spdk_unlikely(ret != 0)) {
		bdev_nvme_io_complete(bio, ret);
	}
}

static void
bdev_nvme_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct nvme_bdev_channel *nbdev_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct nvme_bdev_io *nbdev_io = (struct nvme_bdev_io *)bdev_io->driver_ctx;
	struct nvme_bdev_io *nbdev_io_to_abort;
	int rc = 0;

	nbdev_io->io_path = bdev_nvme_find_io_path(nbdev_ch);
	if (spdk_unlikely(!nbdev_io->io_path)) {
		if (!bdev_nvme_io_type_is_admin(bdev_io->type)) {
			rc = -ENXIO;
			goto exit;
		}

		/* Admin commands do not use the optimal I/O path.
		 * Simply fall through even if it is not found.
		 */
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		if (bdev_io->u.bdev.iovs && bdev_io->u.bdev.iovs[0].iov_base) {
			rc = bdev_nvme_readv(nbdev_io,
					     bdev_io->u.bdev.iovs,
					     bdev_io->u.bdev.iovcnt,
					     bdev_io->u.bdev.md_buf,
					     bdev_io->u.bdev.num_blocks,
					     bdev_io->u.bdev.offset_blocks,
					     bdev->dif_check_flags,
					     bdev_io->internal.ext_opts);
		} else {
			spdk_bdev_io_get_buf(bdev_io, bdev_nvme_get_buf_cb,
					     bdev_io->u.bdev.num_blocks * bdev->blocklen);
			rc = 0;
		}
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = bdev_nvme_writev(nbdev_io,
				      bdev_io->u.bdev.iovs,
				      bdev_io->u.bdev.iovcnt,
				      bdev_io->u.bdev.md_buf,
				      bdev_io->u.bdev.num_blocks,
				      bdev_io->u.bdev.offset_blocks,
				      bdev->dif_check_flags,
				      bdev_io->internal.ext_opts);
		break;
	case SPDK_BDEV_IO_TYPE_COMPARE:
		rc = bdev_nvme_comparev(nbdev_io,
					bdev_io->u.bdev.iovs,
					bdev_io->u.bdev.iovcnt,
					bdev_io->u.bdev.md_buf,
					bdev_io->u.bdev.num_blocks,
					bdev_io->u.bdev.offset_blocks,
					bdev->dif_check_flags);
		break;
	case SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE:
		rc = bdev_nvme_comparev_and_writev(nbdev_io,
						   bdev_io->u.bdev.iovs,
						   bdev_io->u.bdev.iovcnt,
						   bdev_io->u.bdev.fused_iovs,
						   bdev_io->u.bdev.fused_iovcnt,
						   bdev_io->u.bdev.md_buf,
						   bdev_io->u.bdev.num_blocks,
						   bdev_io->u.bdev.offset_blocks,
						   bdev->dif_check_flags);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = bdev_nvme_unmap(nbdev_io,
				     bdev_io->u.bdev.offset_blocks,
				     bdev_io->u.bdev.num_blocks);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		rc =  bdev_nvme_write_zeroes(nbdev_io,
					     bdev_io->u.bdev.offset_blocks,
					     bdev_io->u.bdev.num_blocks);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		nbdev_io->io_path = NULL;
		bdev_nvme_reset_io(nbdev_ch, nbdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = bdev_nvme_flush(nbdev_io,
				     bdev_io->u.bdev.offset_blocks,
				     bdev_io->u.bdev.num_blocks);
		break;
	case SPDK_BDEV_IO_TYPE_ZONE_APPEND:
		rc = bdev_nvme_zone_appendv(nbdev_io,
					    bdev_io->u.bdev.iovs,
					    bdev_io->u.bdev.iovcnt,
					    bdev_io->u.bdev.md_buf,
					    bdev_io->u.bdev.num_blocks,
					    bdev_io->u.bdev.offset_blocks,
					    bdev->dif_check_flags);
		break;
	case SPDK_BDEV_IO_TYPE_GET_ZONE_INFO:
		rc = bdev_nvme_get_zone_info(nbdev_io,
					     bdev_io->u.zone_mgmt.zone_id,
					     bdev_io->u.zone_mgmt.num_zones,
					     bdev_io->u.zone_mgmt.buf);
		break;
	case SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT:
		rc = bdev_nvme_zone_management(nbdev_io,
					       bdev_io->u.zone_mgmt.zone_id,
					       bdev_io->u.zone_mgmt.zone_action);
		break;
	case SPDK_BDEV_IO_TYPE_NVME_ADMIN:
		nbdev_io->io_path = NULL;
		bdev_nvme_admin_passthru(nbdev_ch,
					 nbdev_io,
					 &bdev_io->u.nvme_passthru.cmd,
					 bdev_io->u.nvme_passthru.buf,
					 bdev_io->u.nvme_passthru.nbytes);
		break;
	case SPDK_BDEV_IO_TYPE_NVME_IO:
		rc = bdev_nvme_io_passthru(nbdev_io,
					   &bdev_io->u.nvme_passthru.cmd,
					   bdev_io->u.nvme_passthru.buf,
					   bdev_io->u.nvme_passthru.nbytes);
		break;
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		rc = bdev_nvme_io_passthru_md(nbdev_io,
					      &bdev_io->u.nvme_passthru.cmd,
					      bdev_io->u.nvme_passthru.buf,
					      bdev_io->u.nvme_passthru.nbytes,
					      bdev_io->u.nvme_passthru.md_buf,
					      bdev_io->u.nvme_passthru.md_len);
		break;
	case SPDK_BDEV_IO_TYPE_ABORT:
		nbdev_io->io_path = NULL;
		nbdev_io_to_abort = (struct nvme_bdev_io *)bdev_io->u.abort.bio_to_abort->driver_ctx;
		bdev_nvme_abort(nbdev_ch,
				nbdev_io,
				nbdev_io_to_abort);
		break;
	default:
		rc = -EINVAL;
		break;
	}

exit:
	if (spdk_unlikely(rc != 0)) {
		bdev_nvme_io_complete(nbdev_io, rc);
	}
}

static bool
bdev_nvme_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct nvme_bdev *nbdev = ctx;
	struct nvme_ns *nvme_ns;
	struct spdk_nvme_ns *ns;
	struct spdk_nvme_ctrlr *ctrlr;
	const struct spdk_nvme_ctrlr_data *cdata;

	nvme_ns = TAILQ_FIRST(&nbdev->nvme_ns_list);
	assert(nvme_ns != NULL);
	ns = nvme_ns->ns;
	ctrlr = spdk_nvme_ns_get_ctrlr(ns);

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_NVME_ADMIN:
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_ABORT:
		return true;

	case SPDK_BDEV_IO_TYPE_COMPARE:
		return spdk_nvme_ns_supports_compare(ns);

	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		return spdk_nvme_ns_get_md_size(ns) ? true : false;

	case SPDK_BDEV_IO_TYPE_UNMAP:
		cdata = spdk_nvme_ctrlr_get_data(ctrlr);
		return cdata->oncs.dsm;

	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		cdata = spdk_nvme_ctrlr_get_data(ctrlr);
		return cdata->oncs.write_zeroes;

	case SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE:
		if (spdk_nvme_ctrlr_get_flags(ctrlr) &
		    SPDK_NVME_CTRLR_COMPARE_AND_WRITE_SUPPORTED) {
			return true;
		}
		return false;

	case SPDK_BDEV_IO_TYPE_GET_ZONE_INFO:
	case SPDK_BDEV_IO_TYPE_ZONE_MANAGEMENT:
		return spdk_nvme_ns_get_csi(ns) == SPDK_NVME_CSI_ZNS;

	case SPDK_BDEV_IO_TYPE_ZONE_APPEND:
		return spdk_nvme_ns_get_csi(ns) == SPDK_NVME_CSI_ZNS &&
		       spdk_nvme_ctrlr_get_flags(ctrlr) & SPDK_NVME_CTRLR_ZONE_APPEND_SUPPORTED;

	default:
		return false;
	}
}

static int
bdev_nvme_create_ctrlr_channel_cb(void *io_device, void *ctx_buf)
{
	struct nvme_ctrlr *nvme_ctrlr = io_device;
	struct nvme_ctrlr_channel *ctrlr_ch = ctx_buf;
	struct spdk_io_channel *pg_ch;
	int rc;

	pg_ch = spdk_get_io_channel(&g_nvme_bdev_ctrlrs);
	if (!pg_ch) {
		return -1;
	}

	ctrlr_ch->group = spdk_io_channel_get_ctx(pg_ch);
	TAILQ_INSERT_TAIL(&ctrlr_ch->group->ctrlr_ch_list, ctrlr_ch, tailq);

#ifdef SPDK_CONFIG_VTUNE
	ctrlr_ch->group->collect_spin_stat = true;
#else
	ctrlr_ch->group->collect_spin_stat = false;
#endif

	TAILQ_INIT(&ctrlr_ch->pending_resets);
	TAILQ_INIT(&ctrlr_ch->io_path_list);

	rc = bdev_nvme_create_qpair(ctrlr_ch);
	if (rc != 0) {
		/* nvme ctrlr can't create IO qpair during reset. In that case ctrlr_ch->qpair
		 * pointer will be NULL and IO qpair will be created when reset completes.
		 * If the user submits IO requests during reset, they will be queued and resubmitted later */
		if (!nvme_ctrlr->resetting) {
			goto err_qpair;
		}
	}

	return 0;

err_qpair:
	TAILQ_REMOVE(&ctrlr_ch->group->ctrlr_ch_list, ctrlr_ch, tailq);
	spdk_put_io_channel(pg_ch);

	return rc;
}

static void
bdev_nvme_destroy_ctrlr_channel_cb(void *io_device, void *ctx_buf)
{
	struct nvme_ctrlr_channel *ctrlr_ch = ctx_buf;

	assert(ctrlr_ch->group != NULL);

	bdev_nvme_destroy_qpair(ctrlr_ch);

	TAILQ_REMOVE(&ctrlr_ch->group->ctrlr_ch_list, ctrlr_ch, tailq);

	spdk_put_io_channel(spdk_io_channel_from_ctx(ctrlr_ch->group));
}

static void
bdev_nvme_submit_accel_crc32c(void *ctx, uint32_t *dst, struct iovec *iov,
			      uint32_t iov_cnt, uint32_t seed,
			      spdk_nvme_accel_completion_cb cb_fn, void *cb_arg)
{
	struct nvme_poll_group *group = ctx;
	int rc;

	assert(group->accel_channel != NULL);
	assert(cb_fn != NULL);

	rc = spdk_accel_submit_crc32cv(group->accel_channel, dst, iov, iov_cnt, seed, cb_fn, cb_arg);
	if (rc) {
		/* For the two cases, spdk_accel_submit_crc32cv does not call the user's cb_fn */
		if (rc == -ENOMEM || rc == -EINVAL) {
			cb_fn(cb_arg, rc);
		}
		SPDK_ERRLOG("Cannot complete the accelerated crc32c operation with iov=%p\n", iov);
	}
}

static struct spdk_nvme_accel_fn_table g_bdev_nvme_accel_fn_table = {
	.table_size		= sizeof(struct spdk_nvme_accel_fn_table),
	.submit_accel_crc32c	= bdev_nvme_submit_accel_crc32c,
};

static int
bdev_nvme_create_poll_group_cb(void *io_device, void *ctx_buf)
{
	struct nvme_poll_group *group = ctx_buf;

	TAILQ_INIT(&group->ctrlr_ch_list);

	group->group = spdk_nvme_poll_group_create(group, &g_bdev_nvme_accel_fn_table);
	if (group->group == NULL) {
		return -1;
	}

	group->accel_channel = spdk_accel_engine_get_io_channel();
	if (!group->accel_channel) {
		spdk_nvme_poll_group_destroy(group->group);
		SPDK_ERRLOG("Cannot get the accel_channel for bdev nvme polling group=%p\n",
			    group);
		return -1;
	}

	group->poller = SPDK_POLLER_REGISTER(bdev_nvme_poll, group, g_opts.nvme_ioq_poll_period_us);

	if (group->poller == NULL) {
		spdk_put_io_channel(group->accel_channel);
		spdk_nvme_poll_group_destroy(group->group);
		return -1;
	}

	return 0;
}

static void
bdev_nvme_destroy_poll_group_cb(void *io_device, void *ctx_buf)
{
	struct nvme_poll_group *group = ctx_buf;

	assert(TAILQ_EMPTY(&group->ctrlr_ch_list));

	if (group->accel_channel) {
		spdk_put_io_channel(group->accel_channel);
	}

	spdk_poller_unregister(&group->poller);
	if (spdk_nvme_poll_group_destroy(group->group)) {
		SPDK_ERRLOG("Unable to destroy a poll group for the NVMe bdev module.\n");
		assert(false);
	}
}

static struct spdk_io_channel *
bdev_nvme_get_io_channel(void *ctx)
{
	struct nvme_bdev *nvme_bdev = ctx;

	return spdk_get_io_channel(nvme_bdev);
}

static void *
bdev_nvme_get_module_ctx(void *ctx)
{
	struct nvme_bdev *nvme_bdev = ctx;
	struct nvme_ns *nvme_ns;

	if (!nvme_bdev || nvme_bdev->disk.module != &nvme_if) {
		return NULL;
	}

	nvme_ns = TAILQ_FIRST(&nvme_bdev->nvme_ns_list);
	if (!nvme_ns) {
		return NULL;
	}

	return nvme_ns->ns;
}

static const char *
_nvme_ana_state_str(enum spdk_nvme_ana_state ana_state)
{
	switch (ana_state) {
	case SPDK_NVME_ANA_OPTIMIZED_STATE:
		return "optimized";
	case SPDK_NVME_ANA_NON_OPTIMIZED_STATE:
		return "non_optimized";
	case SPDK_NVME_ANA_INACCESSIBLE_STATE:
		return "inaccessible";
	case SPDK_NVME_ANA_PERSISTENT_LOSS_STATE:
		return "persistent_loss";
	case SPDK_NVME_ANA_CHANGE_STATE:
		return "change";
	default:
		return NULL;
	}
}

static int
bdev_nvme_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct nvme_bdev *nbdev = ctx;
	struct nvme_ns *nvme_ns;

	nvme_ns = TAILQ_FIRST(&nbdev->nvme_ns_list);
	assert(nvme_ns != NULL);

	return spdk_nvme_ctrlr_get_memory_domains(nvme_ns->ctrlr->ctrlr, domains, array_size);
}

static void
nvme_namespace_info_json(struct spdk_json_write_ctx *w,
			 struct nvme_ns *nvme_ns)
{
	struct spdk_nvme_ns *ns;
	struct spdk_nvme_ctrlr *ctrlr;
	const struct spdk_nvme_ctrlr_data *cdata;
	const struct spdk_nvme_transport_id *trid;
	union spdk_nvme_vs_register vs;
	char buf[128];

	ns = nvme_ns->ns;
	ctrlr = spdk_nvme_ns_get_ctrlr(ns);

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	trid = spdk_nvme_ctrlr_get_transport_id(ctrlr);
	vs = spdk_nvme_ctrlr_get_regs_vs(ctrlr);

	spdk_json_write_object_begin(w);

	if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE) {
		spdk_json_write_named_string(w, "pci_address", trid->traddr);
	}

	spdk_json_write_named_object_begin(w, "trid");

	nvme_bdev_dump_trid_json(trid, w);

	spdk_json_write_object_end(w);

#ifdef SPDK_CONFIG_NVME_CUSE
	size_t cuse_name_size = 128;
	char cuse_name[cuse_name_size];

	int rc = spdk_nvme_cuse_get_ns_name(ctrlr, spdk_nvme_ns_get_id(ns),
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

	if (cdata->subnqn[0] != '\0') {
		spdk_json_write_named_string(w, "subnqn", cdata->subnqn);
	}

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

	spdk_json_write_named_object_begin(w, "ns_data");

	spdk_json_write_named_uint32(w, "id", spdk_nvme_ns_get_id(ns));

	if (cdata->cmic.ana_reporting) {
		spdk_json_write_named_string(w, "ana_state",
					     _nvme_ana_state_str(nvme_ns->ana_state));
	}

	spdk_json_write_object_end(w);

	if (cdata->oacs.security) {
		spdk_json_write_named_object_begin(w, "security");

		spdk_json_write_named_bool(w, "opal", nvme_ns->bdev->opal);

		spdk_json_write_object_end(w);
	}

	spdk_json_write_object_end(w);
}

static int
bdev_nvme_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct nvme_bdev *nvme_bdev = ctx;
	struct nvme_ns *nvme_ns;

	pthread_mutex_lock(&nvme_bdev->mutex);
	spdk_json_write_named_array_begin(w, "nvme");
	TAILQ_FOREACH(nvme_ns, &nvme_bdev->nvme_ns_list, tailq) {
		nvme_namespace_info_json(w, nvme_ns);
	}
	spdk_json_write_array_end(w);
	pthread_mutex_unlock(&nvme_bdev->mutex);

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
	struct nvme_bdev_channel *nbdev_ch = spdk_io_channel_get_ctx(ch);
	struct nvme_io_path *io_path;
	struct nvme_poll_group *group;
	uint64_t spin_time = 0;

	STAILQ_FOREACH(io_path, &nbdev_ch->io_path_list, stailq) {
		group = io_path->ctrlr_ch->group;

		if (!group || !group->collect_spin_stat) {
			continue;
		}

		if (group->end_ticks != 0) {
			group->spin_ticks += (group->end_ticks - group->start_ticks);
			group->end_ticks = 0;
		}

		spin_time += group->spin_ticks;
		group->start_ticks = 0;
		group->spin_ticks = 0;
	}

	return (spin_time * 1000000ULL) / spdk_get_ticks_hz();
}

static const struct spdk_bdev_fn_table nvmelib_fn_table = {
	.destruct		= bdev_nvme_destruct,
	.submit_request		= bdev_nvme_submit_request,
	.io_type_supported	= bdev_nvme_io_type_supported,
	.get_io_channel		= bdev_nvme_get_io_channel,
	.dump_info_json		= bdev_nvme_dump_info_json,
	.write_config_json	= bdev_nvme_write_config_json,
	.get_spin_time		= bdev_nvme_get_spin_time,
	.get_module_ctx		= bdev_nvme_get_module_ctx,
	.get_memory_domains	= bdev_nvme_get_memory_domains,
};

typedef int (*bdev_nvme_parse_ana_log_page_cb)(
	const struct spdk_nvme_ana_group_descriptor *desc, void *cb_arg);

static int
bdev_nvme_parse_ana_log_page(struct nvme_ctrlr *nvme_ctrlr,
			     bdev_nvme_parse_ana_log_page_cb cb_fn, void *cb_arg)
{
	struct spdk_nvme_ana_group_descriptor *copied_desc;
	uint8_t *orig_desc;
	uint32_t i, desc_size, copy_len;
	int rc = 0;

	if (nvme_ctrlr->ana_log_page == NULL) {
		return -EINVAL;
	}

	copied_desc = nvme_ctrlr->copied_ana_desc;

	orig_desc = (uint8_t *)nvme_ctrlr->ana_log_page + sizeof(struct spdk_nvme_ana_page);
	copy_len = nvme_ctrlr->ana_log_page_size - sizeof(struct spdk_nvme_ana_page);

	for (i = 0; i < nvme_ctrlr->ana_log_page->num_ana_group_desc; i++) {
		memcpy(copied_desc, orig_desc, copy_len);

		rc = cb_fn(copied_desc, cb_arg);
		if (rc != 0) {
			break;
		}

		desc_size = sizeof(struct spdk_nvme_ana_group_descriptor) +
			    copied_desc->num_of_nsid * sizeof(uint32_t);
		orig_desc += desc_size;
		copy_len -= desc_size;
	}

	return rc;
}

static int
nvme_ns_set_ana_state(const struct spdk_nvme_ana_group_descriptor *desc, void *cb_arg)
{
	struct nvme_ns *nvme_ns = cb_arg;
	uint32_t i;

	for (i = 0; i < desc->num_of_nsid; i++) {
		if (desc->nsid[i] != spdk_nvme_ns_get_id(nvme_ns->ns)) {
			continue;
		}
		nvme_ns->ana_group_id = desc->ana_group_id;
		nvme_ns->ana_state = desc->ana_state;
		return 1;
	}

	return 0;
}

static int
nvme_disk_create(struct spdk_bdev *disk, const char *base_name,
		 struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns,
		 uint32_t prchk_flags, void *ctx)
{
	const struct spdk_uuid		*uuid;
	const uint8_t *nguid;
	const struct spdk_nvme_ctrlr_data *cdata;
	const struct spdk_nvme_ns_data	*nsdata;
	enum spdk_nvme_csi		csi;
	uint32_t atomic_bs, phys_bs, bs;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	csi = spdk_nvme_ns_get_csi(ns);

	switch (csi) {
	case SPDK_NVME_CSI_NVM:
		disk->product_name = "NVMe disk";
		break;
	case SPDK_NVME_CSI_ZNS:
		disk->product_name = "NVMe ZNS disk";
		disk->zoned = true;
		disk->zone_size = spdk_nvme_zns_ns_get_zone_size_sectors(ns);
		disk->max_zone_append_size = spdk_nvme_zns_ctrlr_get_max_zone_append_size(ctrlr) /
					     spdk_nvme_ns_get_extended_sector_size(ns);
		disk->max_open_zones = spdk_nvme_zns_ns_get_max_open_zones(ns);
		disk->max_active_zones = spdk_nvme_zns_ns_get_max_active_zones(ns);
		break;
	default:
		SPDK_ERRLOG("unsupported CSI: %u\n", csi);
		return -ENOTSUP;
	}

	disk->name = spdk_sprintf_alloc("%sn%d", base_name, spdk_nvme_ns_get_id(ns));
	if (!disk->name) {
		return -ENOMEM;
	}

	disk->write_cache = 0;
	if (cdata->vwc.present) {
		/* Enable if the Volatile Write Cache exists */
		disk->write_cache = 1;
	}
	if (cdata->oncs.write_zeroes) {
		disk->max_write_zeroes = UINT16_MAX + 1;
	}
	disk->blocklen = spdk_nvme_ns_get_extended_sector_size(ns);
	disk->blockcnt = spdk_nvme_ns_get_num_sectors(ns);
	disk->optimal_io_boundary = spdk_nvme_ns_get_optimal_io_boundary(ns);

	nguid = spdk_nvme_ns_get_nguid(ns);
	if (!nguid) {
		uuid = spdk_nvme_ns_get_uuid(ns);
		if (uuid) {
			disk->uuid = *uuid;
		}
	} else {
		memcpy(&disk->uuid, nguid, sizeof(disk->uuid));
	}

	nsdata = spdk_nvme_ns_get_data(ns);
	bs = spdk_nvme_ns_get_sector_size(ns);
	atomic_bs = bs;
	phys_bs = bs;
	if (nsdata->nabo == 0) {
		if (nsdata->nsfeat.ns_atomic_write_unit && nsdata->nawupf) {
			atomic_bs = bs * (1 + nsdata->nawupf);
		} else {
			atomic_bs = bs * (1 + cdata->awupf);
		}
	}
	if (nsdata->nsfeat.optperf) {
		phys_bs = bs * (1 + nsdata->npwg);
	}
	disk->phys_blocklen = spdk_min(phys_bs, atomic_bs);

	disk->md_len = spdk_nvme_ns_get_md_size(ns);
	if (disk->md_len != 0) {
		disk->md_interleave = nsdata->flbas.extended;
		disk->dif_type = (enum spdk_dif_type)spdk_nvme_ns_get_pi_type(ns);
		if (disk->dif_type != SPDK_DIF_DISABLE) {
			disk->dif_is_head_of_md = nsdata->dps.md_start;
			disk->dif_check_flags = prchk_flags;
		}
	}

	if (!(spdk_nvme_ctrlr_get_flags(ctrlr) &
	      SPDK_NVME_CTRLR_COMPARE_AND_WRITE_SUPPORTED)) {
		disk->acwu = 0;
	} else if (nsdata->nsfeat.ns_atomic_write_unit) {
		disk->acwu = nsdata->nacwu + 1; /* 0-based */
	} else {
		disk->acwu = cdata->acwu + 1; /* 0-based */
	}

	disk->ctxt = ctx;
	disk->fn_table = &nvmelib_fn_table;
	disk->module = &nvme_if;

	return 0;
}

static int
nvme_bdev_create(struct nvme_ctrlr *nvme_ctrlr, struct nvme_ns *nvme_ns)
{
	struct nvme_bdev *bdev;
	int rc;

	bdev = calloc(1, sizeof(*bdev));
	if (!bdev) {
		SPDK_ERRLOG("bdev calloc() failed\n");
		return -ENOMEM;
	}

	rc = pthread_mutex_init(&bdev->mutex, NULL);
	if (rc != 0) {
		free(bdev);
		return rc;
	}

	bdev->ref = 1;
	TAILQ_INIT(&bdev->nvme_ns_list);
	TAILQ_INSERT_TAIL(&bdev->nvme_ns_list, nvme_ns, tailq);
	bdev->opal = nvme_ctrlr->opal_dev != NULL;

	rc = nvme_disk_create(&bdev->disk, nvme_ctrlr->nbdev_ctrlr->name, nvme_ctrlr->ctrlr,
			      nvme_ns->ns, nvme_ctrlr->prchk_flags, bdev);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to create NVMe disk\n");
		pthread_mutex_destroy(&bdev->mutex);
		free(bdev);
		return rc;
	}

	spdk_io_device_register(bdev,
				bdev_nvme_create_bdev_channel_cb,
				bdev_nvme_destroy_bdev_channel_cb,
				sizeof(struct nvme_bdev_channel),
				bdev->disk.name);

	rc = spdk_bdev_register(&bdev->disk);
	if (rc != 0) {
		SPDK_ERRLOG("spdk_bdev_register() failed\n");
		spdk_io_device_unregister(bdev, NULL);
		pthread_mutex_destroy(&bdev->mutex);
		free(bdev->disk.name);
		free(bdev);
		return rc;
	}

	nvme_ns->bdev = bdev;
	bdev->nsid = nvme_ns->id;

	bdev->nbdev_ctrlr = nvme_ctrlr->nbdev_ctrlr;
	TAILQ_INSERT_TAIL(&nvme_ctrlr->nbdev_ctrlr->bdevs, bdev, tailq);

	return 0;
}

static bool
bdev_nvme_compare_ns(struct spdk_nvme_ns *ns1, struct spdk_nvme_ns *ns2)
{
	const struct spdk_nvme_ns_data *nsdata1, *nsdata2;
	const struct spdk_uuid *uuid1, *uuid2;

	nsdata1 = spdk_nvme_ns_get_data(ns1);
	nsdata2 = spdk_nvme_ns_get_data(ns2);
	uuid1 = spdk_nvme_ns_get_uuid(ns1);
	uuid2 = spdk_nvme_ns_get_uuid(ns2);

	return memcmp(nsdata1->nguid, nsdata2->nguid, sizeof(nsdata1->nguid)) == 0 &&
	       nsdata1->eui64 == nsdata2->eui64 &&
	       ((uuid1 == NULL && uuid2 == NULL) ||
		(uuid1 != NULL && uuid2 != NULL && spdk_uuid_compare(uuid1, uuid2) == 0)) &&
	       spdk_nvme_ns_get_csi(ns1) == spdk_nvme_ns_get_csi(ns2);
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
	opts->disable_read_ana_log_page = true;

	SPDK_DEBUGLOG(bdev_nvme, "Attaching to %s\n", trid->traddr);

	return true;
}

static void
nvme_abort_cpl(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_ctrlr *nvme_ctrlr = ctx;

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_WARNLOG("Abort failed. Resetting controller. sc is %u, sct is %u.\n", cpl->status.sc,
			     cpl->status.sct);
		bdev_nvme_reset(nvme_ctrlr);
	} else if (cpl->cdw0 & 0x1) {
		SPDK_WARNLOG("Specified command could not be aborted.\n");
		bdev_nvme_reset(nvme_ctrlr);
	}
}

static void
timeout_cb(void *cb_arg, struct spdk_nvme_ctrlr *ctrlr,
	   struct spdk_nvme_qpair *qpair, uint16_t cid)
{
	struct nvme_ctrlr *nvme_ctrlr = cb_arg;
	union spdk_nvme_csts_register csts;
	int rc;

	assert(nvme_ctrlr->ctrlr == ctrlr);

	SPDK_WARNLOG("Warning: Detected a timeout. ctrlr=%p qpair=%p cid=%u\n", ctrlr, qpair, cid);

	/* Only try to read CSTS if it's a PCIe controller or we have a timeout on an I/O
	 * queue.  (Note: qpair == NULL when there's an admin cmd timeout.)  Otherwise we
	 * would submit another fabrics cmd on the admin queue to read CSTS and check for its
	 * completion recursively.
	 */
	if (nvme_ctrlr->active_path_id->trid.trtype == SPDK_NVME_TRANSPORT_PCIE || qpair != NULL) {
		csts = spdk_nvme_ctrlr_get_regs_csts(ctrlr);
		if (csts.bits.cfs) {
			SPDK_ERRLOG("Controller Fatal Status, reset required\n");
			bdev_nvme_reset(nvme_ctrlr);
			return;
		}
	}

	switch (g_opts.action_on_timeout) {
	case SPDK_BDEV_NVME_TIMEOUT_ACTION_ABORT:
		if (qpair) {
			/* Don't send abort to ctrlr when ctrlr is not available. */
			pthread_mutex_lock(&nvme_ctrlr->mutex);
			if (!nvme_ctrlr_is_available(nvme_ctrlr)) {
				pthread_mutex_unlock(&nvme_ctrlr->mutex);
				SPDK_NOTICELOG("Quit abort. Ctrlr is not available.\n");
				return;
			}
			pthread_mutex_unlock(&nvme_ctrlr->mutex);

			rc = spdk_nvme_ctrlr_cmd_abort(ctrlr, qpair, cid,
						       nvme_abort_cpl, nvme_ctrlr);
			if (rc == 0) {
				return;
			}

			SPDK_ERRLOG("Unable to send abort. Resetting, rc is %d.\n", rc);
		}

	/* FALLTHROUGH */
	case SPDK_BDEV_NVME_TIMEOUT_ACTION_RESET:
		bdev_nvme_reset(nvme_ctrlr);
		break;
	case SPDK_BDEV_NVME_TIMEOUT_ACTION_NONE:
		SPDK_DEBUGLOG(bdev_nvme, "No action for nvme controller timeout.\n");
		break;
	default:
		SPDK_ERRLOG("An invalid timeout action value is found.\n");
		break;
	}
}

static void
nvme_ctrlr_populate_namespace_done(struct nvme_ns *nvme_ns, int rc)
{
	struct nvme_ctrlr *nvme_ctrlr = nvme_ns->ctrlr;
	struct nvme_async_probe_ctx *ctx = nvme_ns->probe_ctx;

	if (rc == 0) {
		nvme_ns->probe_ctx = NULL;
		pthread_mutex_lock(&nvme_ctrlr->mutex);
		nvme_ctrlr->ref++;
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
	} else {
		RB_REMOVE(nvme_ns_tree, &nvme_ctrlr->namespaces, nvme_ns);
		free(nvme_ns);
	}

	if (ctx) {
		ctx->populates_in_progress--;
		if (ctx->populates_in_progress == 0) {
			nvme_ctrlr_populate_namespaces_done(nvme_ctrlr, ctx);
		}
	}
}

static void
bdev_nvme_add_io_path(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct nvme_bdev_channel *nbdev_ch = spdk_io_channel_get_ctx(_ch);
	struct nvme_ns *nvme_ns = spdk_io_channel_iter_get_ctx(i);
	int rc;

	rc = _bdev_nvme_add_io_path(nbdev_ch, nvme_ns);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to add I/O path to bdev_channel dynamically.\n");
	}

	spdk_for_each_channel_continue(i, rc);
}

static void
bdev_nvme_delete_io_path(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct nvme_bdev_channel *nbdev_ch = spdk_io_channel_get_ctx(_ch);
	struct nvme_ns *nvme_ns = spdk_io_channel_iter_get_ctx(i);
	struct nvme_io_path *io_path;

	io_path = _bdev_nvme_get_io_path(nbdev_ch, nvme_ns);
	if (io_path != NULL) {
		_bdev_nvme_delete_io_path(nbdev_ch, io_path);
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
bdev_nvme_add_io_path_failed(struct spdk_io_channel_iter *i, int status)
{
	struct nvme_ns *nvme_ns = spdk_io_channel_iter_get_ctx(i);

	nvme_ctrlr_populate_namespace_done(nvme_ns, -1);
}

static void
bdev_nvme_add_io_path_done(struct spdk_io_channel_iter *i, int status)
{
	struct nvme_ns *nvme_ns = spdk_io_channel_iter_get_ctx(i);
	struct nvme_bdev *bdev = spdk_io_channel_iter_get_io_device(i);

	if (status == 0) {
		nvme_ctrlr_populate_namespace_done(nvme_ns, 0);
	} else {
		/* Delete the added io_paths and fail populating the namespace. */
		spdk_for_each_channel(bdev,
				      bdev_nvme_delete_io_path,
				      nvme_ns,
				      bdev_nvme_add_io_path_failed);
	}
}

static int
nvme_bdev_add_ns(struct nvme_bdev *bdev, struct nvme_ns *nvme_ns)
{
	struct nvme_ns *tmp_ns;
	const struct spdk_nvme_ns_data *nsdata;

	nsdata = spdk_nvme_ns_get_data(nvme_ns->ns);
	if (!nsdata->nmic.can_share) {
		SPDK_ERRLOG("Namespace cannot be shared.\n");
		return -EINVAL;
	}

	pthread_mutex_lock(&bdev->mutex);

	tmp_ns = TAILQ_FIRST(&bdev->nvme_ns_list);
	assert(tmp_ns != NULL);

	if (!bdev_nvme_compare_ns(nvme_ns->ns, tmp_ns->ns)) {
		pthread_mutex_unlock(&bdev->mutex);
		SPDK_ERRLOG("Namespaces are not identical.\n");
		return -EINVAL;
	}

	bdev->ref++;
	TAILQ_INSERT_TAIL(&bdev->nvme_ns_list, nvme_ns, tailq);
	nvme_ns->bdev = bdev;

	pthread_mutex_unlock(&bdev->mutex);

	/* Add nvme_io_path to nvme_bdev_channels dynamically. */
	spdk_for_each_channel(bdev,
			      bdev_nvme_add_io_path,
			      nvme_ns,
			      bdev_nvme_add_io_path_done);

	return 0;
}

static void
nvme_ctrlr_populate_namespace(struct nvme_ctrlr *nvme_ctrlr, struct nvme_ns *nvme_ns)
{
	struct spdk_nvme_ns	*ns;
	struct nvme_bdev	*bdev;
	int			rc = 0;

	ns = spdk_nvme_ctrlr_get_ns(nvme_ctrlr->ctrlr, nvme_ns->id);
	if (!ns) {
		SPDK_DEBUGLOG(bdev_nvme, "Invalid NS %d\n", nvme_ns->id);
		rc = -EINVAL;
		goto done;
	}

	nvme_ns->ns = ns;
	nvme_ns->ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;

	if (nvme_ctrlr->ana_log_page != NULL) {
		bdev_nvme_parse_ana_log_page(nvme_ctrlr, nvme_ns_set_ana_state, nvme_ns);
	}

	bdev = nvme_bdev_ctrlr_get_bdev(nvme_ctrlr->nbdev_ctrlr, nvme_ns->id);
	if (bdev == NULL) {
		rc = nvme_bdev_create(nvme_ctrlr, nvme_ns);
	} else {
		rc = nvme_bdev_add_ns(bdev, nvme_ns);
		if (rc == 0) {
			return;
		}
	}
done:
	nvme_ctrlr_populate_namespace_done(nvme_ns, rc);
}

static void
nvme_ctrlr_depopulate_namespace_done(struct nvme_ns *nvme_ns)
{
	struct nvme_ctrlr *nvme_ctrlr = nvme_ns->ctrlr;

	assert(nvme_ctrlr != NULL);

	pthread_mutex_lock(&nvme_ctrlr->mutex);

	RB_REMOVE(nvme_ns_tree, &nvme_ctrlr->namespaces, nvme_ns);

	if (nvme_ns->bdev != NULL) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		return;
	}

	free(nvme_ns);
	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	nvme_ctrlr_release(nvme_ctrlr);
}

static void
bdev_nvme_delete_io_path_done(struct spdk_io_channel_iter *i, int status)
{
	struct nvme_ns *nvme_ns = spdk_io_channel_iter_get_ctx(i);

	nvme_ctrlr_depopulate_namespace_done(nvme_ns);
}

static void
nvme_ctrlr_depopulate_namespace(struct nvme_ctrlr *nvme_ctrlr, struct nvme_ns *nvme_ns)
{
	struct nvme_bdev *bdev;

	bdev = nvme_ns->bdev;
	if (bdev != NULL) {
		pthread_mutex_lock(&bdev->mutex);

		assert(bdev->ref > 0);
		bdev->ref--;
		if (bdev->ref == 0) {
			pthread_mutex_unlock(&bdev->mutex);

			spdk_bdev_unregister(&bdev->disk, NULL, NULL);
		} else {
			/* spdk_bdev_unregister() is not called until the last nvme_ns is
			 * depopulated. Hence we need to remove nvme_ns from bdev->nvme_ns_list
			 * and clear nvme_ns->bdev here.
			 */
			TAILQ_REMOVE(&bdev->nvme_ns_list, nvme_ns, tailq);
			nvme_ns->bdev = NULL;

			pthread_mutex_unlock(&bdev->mutex);

			/* Delete nvme_io_paths from nvme_bdev_channels dynamically. After that,
			 * we call depopulate_namespace_done() to avoid use-after-free.
			 */
			spdk_for_each_channel(bdev,
					      bdev_nvme_delete_io_path,
					      nvme_ns,
					      bdev_nvme_delete_io_path_done);
			return;
		}
	}

	nvme_ctrlr_depopulate_namespace_done(nvme_ns);
}

static void
nvme_ctrlr_populate_namespaces(struct nvme_ctrlr *nvme_ctrlr,
			       struct nvme_async_probe_ctx *ctx)
{
	struct spdk_nvme_ctrlr	*ctrlr = nvme_ctrlr->ctrlr;
	struct nvme_ns	*nvme_ns, *next;
	struct spdk_nvme_ns	*ns;
	struct nvme_bdev	*bdev;
	uint32_t		nsid;
	int			rc;
	uint64_t		num_sectors;

	if (ctx) {
		/* Initialize this count to 1 to handle the populate functions
		 * calling nvme_ctrlr_populate_namespace_done() immediately.
		 */
		ctx->populates_in_progress = 1;
	}

	/* First loop over our existing namespaces and see if they have been
	 * removed. */
	nvme_ns = nvme_ctrlr_get_first_active_ns(nvme_ctrlr);
	while (nvme_ns != NULL) {
		next = nvme_ctrlr_get_next_active_ns(nvme_ctrlr, nvme_ns);

		if (spdk_nvme_ctrlr_is_active_ns(ctrlr, nvme_ns->id)) {
			/* NS is still there but attributes may have changed */
			ns = spdk_nvme_ctrlr_get_ns(ctrlr, nvme_ns->id);
			num_sectors = spdk_nvme_ns_get_num_sectors(ns);
			bdev = nvme_ns->bdev;
			assert(bdev != NULL);
			if (bdev->disk.blockcnt != num_sectors) {
				SPDK_NOTICELOG("NSID %u is resized: bdev name %s, old size %" PRIu64 ", new size %" PRIu64 "\n",
					       nvme_ns->id,
					       bdev->disk.name,
					       bdev->disk.blockcnt,
					       num_sectors);
				rc = spdk_bdev_notify_blockcnt_change(&bdev->disk, num_sectors);
				if (rc != 0) {
					SPDK_ERRLOG("Could not change num blocks for nvme bdev: name %s, errno: %d.\n",
						    bdev->disk.name, rc);
				}
			}
		} else {
			/* Namespace was removed */
			nvme_ctrlr_depopulate_namespace(nvme_ctrlr, nvme_ns);
		}

		nvme_ns = next;
	}

	/* Loop through all of the namespaces at the nvme level and see if any of them are new */
	nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	while (nsid != 0) {
		nvme_ns = nvme_ctrlr_get_ns(nvme_ctrlr, nsid);

		if (nvme_ns == NULL) {
			/* Found a new one */
			nvme_ns = calloc(1, sizeof(struct nvme_ns));
			if (nvme_ns == NULL) {
				SPDK_ERRLOG("Failed to allocate namespace\n");
				/* This just fails to attach the namespace. It may work on a future attempt. */
				continue;
			}

			nvme_ns->id = nsid;
			nvme_ns->ctrlr = nvme_ctrlr;

			nvme_ns->bdev = NULL;

			if (ctx) {
				ctx->populates_in_progress++;
			}
			nvme_ns->probe_ctx = ctx;

			RB_INSERT(nvme_ns_tree, &nvme_ctrlr->namespaces, nvme_ns);

			nvme_ctrlr_populate_namespace(nvme_ctrlr, nvme_ns);
		}

		nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid);
	}

	if (ctx) {
		/* Decrement this count now that the loop is over to account
		 * for the one we started with.  If the count is then 0, we
		 * know any populate_namespace functions completed immediately,
		 * so we'll kick the callback here.
		 */
		ctx->populates_in_progress--;
		if (ctx->populates_in_progress == 0) {
			nvme_ctrlr_populate_namespaces_done(nvme_ctrlr, ctx);
		}
	}

}

static void
nvme_ctrlr_depopulate_namespaces(struct nvme_ctrlr *nvme_ctrlr)
{
	struct nvme_ns *nvme_ns, *tmp;

	RB_FOREACH_SAFE(nvme_ns, nvme_ns_tree, &nvme_ctrlr->namespaces, tmp) {
		nvme_ctrlr_depopulate_namespace(nvme_ctrlr, nvme_ns);
	}
}

static int
nvme_ctrlr_set_ana_states(const struct spdk_nvme_ana_group_descriptor *desc,
			  void *cb_arg)
{
	struct nvme_ctrlr *nvme_ctrlr = cb_arg;
	struct nvme_ns *nvme_ns;
	uint32_t i, nsid;

	for (i = 0; i < desc->num_of_nsid; i++) {
		nsid = desc->nsid[i];
		if (nsid == 0) {
			continue;
		}

		nvme_ns = nvme_ctrlr_get_ns(nvme_ctrlr, nsid);

		assert(nvme_ns != NULL);
		if (nvme_ns == NULL) {
			/* Target told us that an inactive namespace had an ANA change */
			continue;
		}

		nvme_ns->ana_group_id = desc->ana_group_id;
		nvme_ns->ana_state = desc->ana_state;
		nvme_ns->ana_state_updating = false;
	}

	return 0;
}

static void
bdev_nvme_clear_io_path_cache(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct nvme_ctrlr_channel *ctrlr_ch = spdk_io_channel_get_ctx(_ch);

	_bdev_nvme_clear_io_path_cache(ctrlr_ch);

	spdk_for_each_channel_continue(i, 0);
}

static void
bdev_nvme_clear_io_path_cache_done(struct spdk_io_channel_iter *i, int status)
{
	struct nvme_ctrlr *nvme_ctrlr = spdk_io_channel_iter_get_io_device(i);

	pthread_mutex_lock(&nvme_ctrlr->mutex);

	assert(nvme_ctrlr->ana_log_page_updating == true);
	nvme_ctrlr->ana_log_page_updating = false;

	if (!nvme_ctrlr_can_be_unregistered(nvme_ctrlr)) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		return;
	}

	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	nvme_ctrlr_unregister(nvme_ctrlr);
}

static void
bdev_nvme_disable_read_ana_log_page(struct nvme_ctrlr *nvme_ctrlr)
{
	struct nvme_ns *nvme_ns;

	free(nvme_ctrlr->ana_log_page);
	nvme_ctrlr->ana_log_page = NULL;

	for (nvme_ns = nvme_ctrlr_get_first_active_ns(nvme_ctrlr);
	     nvme_ns != NULL;
	     nvme_ns = nvme_ctrlr_get_next_active_ns(nvme_ctrlr, nvme_ns)) {
		nvme_ns->ana_state_updating = false;
		nvme_ns->ana_state = SPDK_NVME_ANA_OPTIMIZED_STATE;
	}
}

static void
nvme_ctrlr_read_ana_log_page_done(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_ctrlr *nvme_ctrlr = ctx;

	if (cpl != NULL && spdk_nvme_cpl_is_success(cpl)) {
		bdev_nvme_parse_ana_log_page(nvme_ctrlr, nvme_ctrlr_set_ana_states,
					     nvme_ctrlr);
	} else {
		bdev_nvme_disable_read_ana_log_page(nvme_ctrlr);
	}

	spdk_for_each_channel(nvme_ctrlr,
			      bdev_nvme_clear_io_path_cache,
			      NULL,
			      bdev_nvme_clear_io_path_cache_done);
}

static int
nvme_ctrlr_read_ana_log_page(struct nvme_ctrlr *nvme_ctrlr)
{
	int rc;

	if (nvme_ctrlr->ana_log_page == NULL) {
		return -EINVAL;
	}

	pthread_mutex_lock(&nvme_ctrlr->mutex);
	if (!nvme_ctrlr_is_available(nvme_ctrlr) ||
	    nvme_ctrlr->ana_log_page_updating) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		return -EBUSY;
	}

	nvme_ctrlr->ana_log_page_updating = true;
	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	rc = spdk_nvme_ctrlr_cmd_get_log_page(nvme_ctrlr->ctrlr,
					      SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS,
					      SPDK_NVME_GLOBAL_NS_TAG,
					      nvme_ctrlr->ana_log_page,
					      nvme_ctrlr->ana_log_page_size, 0,
					      nvme_ctrlr_read_ana_log_page_done,
					      nvme_ctrlr);
	if (rc != 0) {
		nvme_ctrlr_read_ana_log_page_done(nvme_ctrlr, NULL);
	}

	return rc;
}

static void
aer_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_ctrlr *nvme_ctrlr		= arg;
	union spdk_nvme_async_event_completion	event;

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_WARNLOG("AER request execute failed");
		return;
	}

	event.raw = cpl->cdw0;
	if ((event.bits.async_event_type == SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE) &&
	    (event.bits.async_event_info == SPDK_NVME_ASYNC_EVENT_NS_ATTR_CHANGED)) {
		nvme_ctrlr_populate_namespaces(nvme_ctrlr, NULL);
	} else if ((event.bits.async_event_type == SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE) &&
		   (event.bits.async_event_info == SPDK_NVME_ASYNC_EVENT_ANA_CHANGE)) {
		nvme_ctrlr_read_ana_log_page(nvme_ctrlr);
	}
}

static void
populate_namespaces_cb(struct nvme_async_probe_ctx *ctx, size_t count, int rc)
{
	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_ctx, count, rc);
	}

	ctx->namespaces_populated = true;
	if (ctx->probe_done) {
		/* The probe was already completed, so we need to free the context
		 * here.  This can happen for cases like OCSSD, where we need to
		 * send additional commands to the SSD after attach.
		 */
		free(ctx);
	}
}

static void
nvme_ctrlr_create_done(struct nvme_ctrlr *nvme_ctrlr,
		       struct nvme_async_probe_ctx *ctx)
{
	spdk_io_device_register(nvme_ctrlr,
				bdev_nvme_create_ctrlr_channel_cb,
				bdev_nvme_destroy_ctrlr_channel_cb,
				sizeof(struct nvme_ctrlr_channel),
				nvme_ctrlr->nbdev_ctrlr->name);

	nvme_ctrlr_populate_namespaces(nvme_ctrlr, ctx);
}

static void
nvme_ctrlr_init_ana_log_page_done(void *_ctx, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_ctrlr *nvme_ctrlr = _ctx;
	struct nvme_async_probe_ctx *ctx = nvme_ctrlr->probe_ctx;

	nvme_ctrlr->probe_ctx = NULL;

	if (spdk_nvme_cpl_is_error(cpl)) {
		nvme_ctrlr_delete(nvme_ctrlr);

		if (ctx != NULL) {
			populate_namespaces_cb(ctx, 0, -1);
		}
		return;
	}

	nvme_ctrlr_create_done(nvme_ctrlr, ctx);
}

static int
nvme_ctrlr_init_ana_log_page(struct nvme_ctrlr *nvme_ctrlr,
			     struct nvme_async_probe_ctx *ctx)
{
	struct spdk_nvme_ctrlr *ctrlr = nvme_ctrlr->ctrlr;
	const struct spdk_nvme_ctrlr_data *cdata;
	uint32_t ana_log_page_size;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	ana_log_page_size = sizeof(struct spdk_nvme_ana_page) + cdata->nanagrpid *
			    sizeof(struct spdk_nvme_ana_group_descriptor) + cdata->mnan *
			    sizeof(uint32_t);

	nvme_ctrlr->ana_log_page = spdk_zmalloc(ana_log_page_size, 64, NULL,
						SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (nvme_ctrlr->ana_log_page == NULL) {
		SPDK_ERRLOG("could not allocate ANA log page buffer\n");
		return -ENXIO;
	}

	/* Each descriptor in a ANA log page is not ensured to be 8-bytes aligned.
	 * Hence copy each descriptor to a temporary area when parsing it.
	 *
	 * Allocate a buffer whose size is as large as ANA log page buffer because
	 * we do not know the size of a descriptor until actually reading it.
	 */
	nvme_ctrlr->copied_ana_desc = calloc(1, ana_log_page_size);
	if (nvme_ctrlr->copied_ana_desc == NULL) {
		SPDK_ERRLOG("could not allocate a buffer to parse ANA descriptor\n");
		return -ENOMEM;
	}

	nvme_ctrlr->ana_log_page_size = ana_log_page_size;

	nvme_ctrlr->probe_ctx = ctx;

	return spdk_nvme_ctrlr_cmd_get_log_page(ctrlr,
						SPDK_NVME_LOG_ASYMMETRIC_NAMESPACE_ACCESS,
						SPDK_NVME_GLOBAL_NS_TAG,
						nvme_ctrlr->ana_log_page,
						nvme_ctrlr->ana_log_page_size, 0,
						nvme_ctrlr_init_ana_log_page_done,
						nvme_ctrlr);
}

/* hostnqn and subnqn were already verified before attaching a controller.
 * Hence check only the multipath capability and cntlid here.
 */
static bool
bdev_nvme_check_multipath(struct nvme_bdev_ctrlr *nbdev_ctrlr, struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_ctrlr *tmp;
	const struct spdk_nvme_ctrlr_data *cdata, *tmp_cdata;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (!cdata->cmic.multi_ctrlr) {
		SPDK_ERRLOG("Ctrlr%u does not support multipath.\n", cdata->cntlid);
		return false;
	}

	TAILQ_FOREACH(tmp, &nbdev_ctrlr->ctrlrs, tailq) {
		tmp_cdata = spdk_nvme_ctrlr_get_data(tmp->ctrlr);

		if (!tmp_cdata->cmic.multi_ctrlr) {
			SPDK_ERRLOG("Ctrlr%u does not support multipath.\n", cdata->cntlid);
			return false;
		}
		if (cdata->cntlid == tmp_cdata->cntlid) {
			SPDK_ERRLOG("cntlid %u are duplicated.\n", tmp_cdata->cntlid);
			return false;
		}
	}

	return true;
}

static int
nvme_bdev_ctrlr_create(const char *name, struct nvme_ctrlr *nvme_ctrlr)
{
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	struct spdk_nvme_ctrlr *ctrlr = nvme_ctrlr->ctrlr;
	int rc = 0;

	pthread_mutex_lock(&g_bdev_nvme_mutex);

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name(name);
	if (nbdev_ctrlr != NULL) {
		if (!bdev_nvme_check_multipath(nbdev_ctrlr, ctrlr)) {
			rc = -EINVAL;
			goto exit;
		}
	} else {
		nbdev_ctrlr = calloc(1, sizeof(*nbdev_ctrlr));
		if (nbdev_ctrlr == NULL) {
			SPDK_ERRLOG("Failed to allocate nvme_bdev_ctrlr.\n");
			rc = -ENOMEM;
			goto exit;
		}
		nbdev_ctrlr->name = strdup(name);
		if (nbdev_ctrlr->name == NULL) {
			SPDK_ERRLOG("Failed to allocate name of nvme_bdev_ctrlr.\n");
			free(nbdev_ctrlr);
			goto exit;
		}
		TAILQ_INIT(&nbdev_ctrlr->ctrlrs);
		TAILQ_INIT(&nbdev_ctrlr->bdevs);
		TAILQ_INSERT_TAIL(&g_nvme_bdev_ctrlrs, nbdev_ctrlr, tailq);
	}
	nvme_ctrlr->nbdev_ctrlr = nbdev_ctrlr;
	TAILQ_INSERT_TAIL(&nbdev_ctrlr->ctrlrs, nvme_ctrlr, tailq);
exit:
	pthread_mutex_unlock(&g_bdev_nvme_mutex);
	return rc;
}

static int
nvme_ctrlr_create(struct spdk_nvme_ctrlr *ctrlr,
		  const char *name,
		  const struct spdk_nvme_transport_id *trid,
		  struct nvme_async_probe_ctx *ctx)
{
	struct nvme_ctrlr *nvme_ctrlr;
	struct nvme_path_id *path_id;
	const struct spdk_nvme_ctrlr_data *cdata;
	int rc;

	nvme_ctrlr = calloc(1, sizeof(*nvme_ctrlr));
	if (nvme_ctrlr == NULL) {
		SPDK_ERRLOG("Failed to allocate device struct\n");
		return -ENOMEM;
	}

	rc = pthread_mutex_init(&nvme_ctrlr->mutex, NULL);
	if (rc != 0) {
		free(nvme_ctrlr);
		return rc;
	}

	TAILQ_INIT(&nvme_ctrlr->trids);

	RB_INIT(&nvme_ctrlr->namespaces);

	path_id = calloc(1, sizeof(*path_id));
	if (path_id == NULL) {
		SPDK_ERRLOG("Failed to allocate trid entry pointer\n");
		rc = -ENOMEM;
		goto err;
	}

	path_id->trid = *trid;
	if (ctx != NULL) {
		memcpy(path_id->hostid.hostaddr, ctx->opts.src_addr, sizeof(path_id->hostid.hostaddr));
		memcpy(path_id->hostid.hostsvcid, ctx->opts.src_svcid, sizeof(path_id->hostid.hostsvcid));
	}
	nvme_ctrlr->active_path_id = path_id;
	TAILQ_INSERT_HEAD(&nvme_ctrlr->trids, path_id, link);

	nvme_ctrlr->thread = spdk_get_thread();
	nvme_ctrlr->ctrlr = ctrlr;
	nvme_ctrlr->ref = 1;

	if (spdk_nvme_ctrlr_is_ocssd_supported(ctrlr)) {
		SPDK_ERRLOG("OCSSDs are not supported");
		rc = -ENOTSUP;
		goto err;
	}

	if (ctx != NULL) {
		nvme_ctrlr->prchk_flags = ctx->prchk_flags;
		nvme_ctrlr->ctrlr_loss_timeout_sec = ctx->ctrlr_loss_timeout_sec;
		nvme_ctrlr->reconnect_delay_sec = ctx->reconnect_delay_sec;
		nvme_ctrlr->fast_io_fail_timeout_sec = ctx->fast_io_fail_timeout_sec;
	}

	nvme_ctrlr->adminq_timer_poller = SPDK_POLLER_REGISTER(bdev_nvme_poll_adminq, nvme_ctrlr,
					  g_opts.nvme_adminq_poll_period_us);

	if (g_opts.timeout_us > 0) {
		/* Register timeout callback. Timeout values for IO vs. admin reqs can be different. */
		/* If timeout_admin_us is 0 (not specified), admin uses same timeout as IO. */
		uint64_t adm_timeout_us = (g_opts.timeout_admin_us == 0) ?
					  g_opts.timeout_us : g_opts.timeout_admin_us;
		spdk_nvme_ctrlr_register_timeout_callback(ctrlr, g_opts.timeout_us,
				adm_timeout_us, timeout_cb, nvme_ctrlr);
	}

	spdk_nvme_ctrlr_register_aer_callback(ctrlr, aer_cb, nvme_ctrlr);
	spdk_nvme_ctrlr_set_remove_cb(ctrlr, remove_cb, nvme_ctrlr);

	if (spdk_nvme_ctrlr_get_flags(ctrlr) &
	    SPDK_NVME_CTRLR_SECURITY_SEND_RECV_SUPPORTED) {
		nvme_ctrlr->opal_dev = spdk_opal_dev_construct(ctrlr);
	}

	rc = nvme_bdev_ctrlr_create(name, nvme_ctrlr);
	if (rc != 0) {
		goto err;
	}

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (cdata->cmic.ana_reporting) {
		rc = nvme_ctrlr_init_ana_log_page(nvme_ctrlr, ctx);
		if (rc == 0) {
			return 0;
		}
	} else {
		nvme_ctrlr_create_done(nvme_ctrlr, ctx);
		return 0;
	}

err:
	nvme_ctrlr_delete(nvme_ctrlr);
	return rc;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	char *name;

	name = spdk_sprintf_alloc("HotInNvme%d", g_hot_insert_nvme_controller_index++);
	if (!name) {
		SPDK_ERRLOG("Failed to assign name to NVMe device\n");
		return;
	}

	SPDK_DEBUGLOG(bdev_nvme, "Attached to %s (%s)\n", trid->traddr, name);

	nvme_ctrlr_create(ctrlr, name, trid, NULL);

	free(name);
}

static void
_nvme_ctrlr_destruct(void *ctx)
{
	struct nvme_ctrlr *nvme_ctrlr = ctx;

	nvme_ctrlr_depopulate_namespaces(nvme_ctrlr);
	nvme_ctrlr_release(nvme_ctrlr);
}

static int
_bdev_nvme_delete(struct nvme_ctrlr *nvme_ctrlr, bool hotplug)
{
	struct nvme_probe_skip_entry *entry;

	pthread_mutex_lock(&nvme_ctrlr->mutex);

	/* The controller's destruction was already started */
	if (nvme_ctrlr->destruct) {
		pthread_mutex_unlock(&nvme_ctrlr->mutex);
		return 0;
	}

	if (!hotplug &&
	    nvme_ctrlr->active_path_id->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
		entry = calloc(1, sizeof(*entry));
		if (!entry) {
			pthread_mutex_unlock(&nvme_ctrlr->mutex);
			return -ENOMEM;
		}
		entry->trid = nvme_ctrlr->active_path_id->trid;
		TAILQ_INSERT_TAIL(&g_skipped_nvme_ctrlrs, entry, tailq);
	}

	nvme_ctrlr->destruct = true;
	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	_nvme_ctrlr_destruct(nvme_ctrlr);

	return 0;
}

static void
remove_cb(void *cb_ctx, struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_ctrlr *nvme_ctrlr = cb_ctx;

	_bdev_nvme_delete(nvme_ctrlr, true);
}

static int
bdev_nvme_hotplug_probe(void *arg)
{
	if (g_hotplug_probe_ctx == NULL) {
		spdk_poller_unregister(&g_hotplug_probe_poller);
		return SPDK_POLLER_IDLE;
	}

	if (spdk_nvme_probe_poll_async(g_hotplug_probe_ctx) != -EAGAIN) {
		g_hotplug_probe_ctx = NULL;
		spdk_poller_unregister(&g_hotplug_probe_poller);
	}

	return SPDK_POLLER_BUSY;
}

static int
bdev_nvme_hotplug(void *arg)
{
	struct spdk_nvme_transport_id trid_pcie;

	if (g_hotplug_probe_ctx) {
		return SPDK_POLLER_BUSY;
	}

	memset(&trid_pcie, 0, sizeof(trid_pcie));
	spdk_nvme_trid_populate_transport(&trid_pcie, SPDK_NVME_TRANSPORT_PCIE);

	g_hotplug_probe_ctx = spdk_nvme_probe_async(&trid_pcie, NULL,
			      hotplug_probe_cb, attach_cb, NULL);

	if (g_hotplug_probe_ctx) {
		assert(g_hotplug_probe_poller == NULL);
		g_hotplug_probe_poller = SPDK_POLLER_REGISTER(bdev_nvme_hotplug_probe, NULL, 1000);
	}

	return SPDK_POLLER_BUSY;
}

void
bdev_nvme_get_opts(struct spdk_bdev_nvme_opts *opts)
{
	*opts = g_opts;
}

static int
bdev_nvme_validate_opts(const struct spdk_bdev_nvme_opts *opts)
{
	if ((opts->timeout_us == 0) && (opts->timeout_admin_us != 0)) {
		/* Can't set timeout_admin_us without also setting timeout_us */
		SPDK_WARNLOG("Invalid options: Can't have (timeout_us == 0) with (timeout_admin_us > 0)\n");
		return -EINVAL;
	}

	if (opts->bdev_retry_count < -1) {
		SPDK_WARNLOG("Invalid option: bdev_retry_count can't be less than -1.\n");
		return -EINVAL;
	}

	return 0;
}

int
bdev_nvme_set_opts(const struct spdk_bdev_nvme_opts *opts)
{
	int ret = bdev_nvme_validate_opts(opts);
	if (ret) {
		SPDK_WARNLOG("Failed to set nvme opts.\n");
		return ret;
	}

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
bdev_nvme_set_hotplug(bool enabled, uint64_t period_us, spdk_msg_fn cb, void *cb_ctx)
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
nvme_ctrlr_populate_namespaces_done(struct nvme_ctrlr *nvme_ctrlr,
				    struct nvme_async_probe_ctx *ctx)
{
	struct nvme_ns	*nvme_ns;
	struct nvme_bdev	*nvme_bdev;
	size_t			j;

	assert(nvme_ctrlr != NULL);

	if (ctx->names == NULL) {
		populate_namespaces_cb(ctx, 0, 0);
		return;
	}

	/*
	 * Report the new bdevs that were created in this call.
	 * There can be more than one bdev per NVMe controller.
	 */
	j = 0;
	nvme_ns = nvme_ctrlr_get_first_active_ns(nvme_ctrlr);
	while (nvme_ns != NULL) {
		nvme_bdev = nvme_ns->bdev;
		if (j < ctx->count) {
			ctx->names[j] = nvme_bdev->disk.name;
			j++;
		} else {
			SPDK_ERRLOG("Maximum number of namespaces supported per NVMe controller is %du. Unable to return all names of created bdevs\n",
				    ctx->count);
			populate_namespaces_cb(ctx, 0, -ERANGE);
			return;
		}

		nvme_ns = nvme_ctrlr_get_next_active_ns(nvme_ctrlr, nvme_ns);
	}

	populate_namespaces_cb(ctx, j, 0);
}

static int
bdev_nvme_compare_trids(struct nvme_ctrlr *nvme_ctrlr,
			struct spdk_nvme_ctrlr *new_ctrlr,
			struct spdk_nvme_transport_id *trid)
{
	struct nvme_path_id *tmp_trid;

	if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE) {
		SPDK_ERRLOG("PCIe failover is not supported.\n");
		return -ENOTSUP;
	}

	/* Currently we only support failover to the same transport type. */
	if (nvme_ctrlr->active_path_id->trid.trtype != trid->trtype) {
		return -EINVAL;
	}

	/* Currently we only support failover to the same NQN. */
	if (strncmp(trid->subnqn, nvme_ctrlr->active_path_id->trid.subnqn, SPDK_NVMF_NQN_MAX_LEN)) {
		return -EINVAL;
	}

	/* Skip all the other checks if we've already registered this path. */
	TAILQ_FOREACH(tmp_trid, &nvme_ctrlr->trids, link) {
		if (!spdk_nvme_transport_id_compare(&tmp_trid->trid, trid)) {
			return -EEXIST;
		}
	}

	return 0;
}

static int
bdev_nvme_compare_namespaces(struct nvme_ctrlr *nvme_ctrlr,
			     struct spdk_nvme_ctrlr *new_ctrlr)
{
	struct nvme_ns *nvme_ns;
	struct spdk_nvme_ns *new_ns;

	nvme_ns = nvme_ctrlr_get_first_active_ns(nvme_ctrlr);
	while (nvme_ns != NULL) {
		new_ns = spdk_nvme_ctrlr_get_ns(new_ctrlr, nvme_ns->id);
		assert(new_ns != NULL);

		if (!bdev_nvme_compare_ns(nvme_ns->ns, new_ns)) {
			return -EINVAL;
		}

		nvme_ns = nvme_ctrlr_get_next_active_ns(nvme_ctrlr, nvme_ns);
	}

	return 0;
}

static int
_bdev_nvme_add_secondary_trid(struct nvme_ctrlr *nvme_ctrlr,
			      struct spdk_nvme_transport_id *trid)
{
	struct nvme_path_id *new_trid, *tmp_trid;

	new_trid = calloc(1, sizeof(*new_trid));
	if (new_trid == NULL) {
		return -ENOMEM;
	}
	new_trid->trid = *trid;
	new_trid->is_failed = false;

	TAILQ_FOREACH(tmp_trid, &nvme_ctrlr->trids, link) {
		if (tmp_trid->is_failed && tmp_trid != nvme_ctrlr->active_path_id) {
			TAILQ_INSERT_BEFORE(tmp_trid, new_trid, link);
			return 0;
		}
	}

	TAILQ_INSERT_TAIL(&nvme_ctrlr->trids, new_trid, link);
	return 0;
}

/* This is the case that a secondary path is added to an existing
 * nvme_ctrlr for failover. After checking if it can access the same
 * namespaces as the primary path, it is disconnected until failover occurs.
 */
static int
bdev_nvme_add_secondary_trid(struct nvme_ctrlr *nvme_ctrlr,
			     struct spdk_nvme_ctrlr *new_ctrlr,
			     struct spdk_nvme_transport_id *trid)
{
	int rc;

	assert(nvme_ctrlr != NULL);

	pthread_mutex_lock(&nvme_ctrlr->mutex);

	rc = bdev_nvme_compare_trids(nvme_ctrlr, new_ctrlr, trid);
	if (rc != 0) {
		goto exit;
	}

	rc = bdev_nvme_compare_namespaces(nvme_ctrlr, new_ctrlr);
	if (rc != 0) {
		goto exit;
	}

	rc = _bdev_nvme_add_secondary_trid(nvme_ctrlr, trid);

exit:
	pthread_mutex_unlock(&nvme_ctrlr->mutex);

	spdk_nvme_detach(new_ctrlr);

	return rc;
}

static void
connect_attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvme_ctrlr_opts *user_opts = cb_ctx;
	struct nvme_async_probe_ctx *ctx;
	int rc;

	ctx = SPDK_CONTAINEROF(user_opts, struct nvme_async_probe_ctx, opts);
	ctx->ctrlr_attached = true;

	rc = nvme_ctrlr_create(ctrlr, ctx->base_name, &ctx->trid, ctx);
	if (rc != 0) {
		populate_namespaces_cb(ctx, 0, rc);
	}
}

static void
connect_set_failover_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
			struct spdk_nvme_ctrlr *ctrlr,
			const struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvme_ctrlr_opts *user_opts = cb_ctx;
	struct nvme_ctrlr *nvme_ctrlr;
	struct nvme_async_probe_ctx *ctx;
	int rc;

	ctx = SPDK_CONTAINEROF(user_opts, struct nvme_async_probe_ctx, opts);
	ctx->ctrlr_attached = true;

	nvme_ctrlr = nvme_ctrlr_get_by_name(ctx->base_name);
	if (nvme_ctrlr) {
		rc = bdev_nvme_add_secondary_trid(nvme_ctrlr, ctrlr, &ctx->trid);
	} else {
		rc = -ENODEV;
	}

	populate_namespaces_cb(ctx, 0, rc);
}

static int
bdev_nvme_async_poll(void *arg)
{
	struct nvme_async_probe_ctx	*ctx = arg;
	int				rc;

	rc = spdk_nvme_probe_poll_async(ctx->probe_ctx);
	if (spdk_unlikely(rc != -EAGAIN)) {
		ctx->probe_done = true;
		spdk_poller_unregister(&ctx->poller);
		if (!ctx->ctrlr_attached) {
			/* The probe is done, but no controller was attached.
			 * That means we had a failure, so report -EIO back to
			 * the caller (usually the RPC). populate_namespaces_cb()
			 * will take care of freeing the nvme_async_probe_ctx.
			 */
			populate_namespaces_cb(ctx, 0, -EIO);
		} else if (ctx->namespaces_populated) {
			/* The namespaces for the attached controller were all
			 * populated and the response was already sent to the
			 * caller (usually the RPC).  So free the context here.
			 */
			free(ctx);
		}
	}

	return SPDK_POLLER_BUSY;
}

static bool
bdev_nvme_check_multipath_params(int32_t ctrlr_loss_timeout_sec,
				 uint32_t reconnect_delay_sec,
				 uint32_t fast_io_fail_timeout_sec)
{
	if (ctrlr_loss_timeout_sec < -1) {
		SPDK_ERRLOG("ctrlr_loss_timeout_sec can't be less than -1.\n");
		return false;
	} else if (ctrlr_loss_timeout_sec == -1) {
		if (reconnect_delay_sec == 0) {
			SPDK_ERRLOG("reconnect_delay_sec can't be 0 if ctrlr_loss_timeout_sec is not 0.\n");
			return false;
		} else if (fast_io_fail_timeout_sec != 0 &&
			   fast_io_fail_timeout_sec < reconnect_delay_sec) {
			SPDK_ERRLOG("reconnect_delay_sec can't be more than fast_io-fail_timeout_sec.\n");
			return false;
		}
	} else if (ctrlr_loss_timeout_sec != 0) {
		if (reconnect_delay_sec == 0) {
			SPDK_ERRLOG("reconnect_delay_sec can't be 0 if ctrlr_loss_timeout_sec is not 0.\n");
			return false;
		} else if (reconnect_delay_sec > (uint32_t)ctrlr_loss_timeout_sec) {
			SPDK_ERRLOG("reconnect_delay_sec can't be more than ctrlr_loss_timeout_sec.\n");
			return false;
		} else if (fast_io_fail_timeout_sec != 0) {
			if (fast_io_fail_timeout_sec < reconnect_delay_sec) {
				SPDK_ERRLOG("reconnect_delay_sec can't be more than fast_io_fail_timeout_sec.\n");
				return false;
			} else if (fast_io_fail_timeout_sec > (uint32_t)ctrlr_loss_timeout_sec) {
				SPDK_ERRLOG("fast_io_fail_timeout_sec can't be more than ctrlr_loss_timeout_sec.\n");
				return false;
			}
		}
	} else if (reconnect_delay_sec != 0 || fast_io_fail_timeout_sec != 0) {
		SPDK_ERRLOG("Both reconnect_delay_sec and fast_io_fail_timeout_sec must be 0 if ctrlr_loss_timeout_sec is 0.\n");
		return false;
	}

	return true;
}

int
bdev_nvme_create(struct spdk_nvme_transport_id *trid,
		 const char *base_name,
		 const char **names,
		 uint32_t count,
		 uint32_t prchk_flags,
		 spdk_bdev_create_nvme_fn cb_fn,
		 void *cb_ctx,
		 struct spdk_nvme_ctrlr_opts *opts,
		 bool multipath,
		 int32_t ctrlr_loss_timeout_sec,
		 uint32_t reconnect_delay_sec,
		 uint32_t fast_io_fail_timeout_sec)
{
	struct nvme_probe_skip_entry	*entry, *tmp;
	struct nvme_async_probe_ctx	*ctx;
	spdk_nvme_attach_cb attach_cb;

	/* TODO expand this check to include both the host and target TRIDs.
	 * Only if both are the same should we fail.
	 */
	if (nvme_ctrlr_get(trid) != NULL) {
		SPDK_ERRLOG("A controller with the provided trid (traddr: %s) already exists.\n", trid->traddr);
		return -EEXIST;
	}

	if (!bdev_nvme_check_multipath_params(ctrlr_loss_timeout_sec, reconnect_delay_sec,
					      fast_io_fail_timeout_sec)) {
		return -EINVAL;
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
	ctx->ctrlr_loss_timeout_sec = ctrlr_loss_timeout_sec;
	ctx->reconnect_delay_sec = reconnect_delay_sec;
	ctx->fast_io_fail_timeout_sec = fast_io_fail_timeout_sec;

	if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE) {
		TAILQ_FOREACH_SAFE(entry, &g_skipped_nvme_ctrlrs, tailq, tmp) {
			if (spdk_nvme_transport_id_compare(trid, &entry->trid) == 0) {
				TAILQ_REMOVE(&g_skipped_nvme_ctrlrs, entry, tailq);
				free(entry);
				break;
			}
		}
	}

	if (opts) {
		memcpy(&ctx->opts, opts, sizeof(*opts));
	} else {
		spdk_nvme_ctrlr_get_default_ctrlr_opts(&ctx->opts, sizeof(ctx->opts));
	}

	ctx->opts.transport_retry_count = g_opts.transport_retry_count;
	ctx->opts.keep_alive_timeout_ms = g_opts.keep_alive_timeout_ms;
	ctx->opts.disable_read_ana_log_page = true;

	if (nvme_bdev_ctrlr_get_by_name(base_name) == NULL || multipath) {
		attach_cb = connect_attach_cb;
	} else {
		attach_cb = connect_set_failover_cb;
	}

	ctx->probe_ctx = spdk_nvme_connect_async(trid, &ctx->opts, attach_cb);
	if (ctx->probe_ctx == NULL) {
		SPDK_ERRLOG("No controller was found with provided trid (traddr: %s)\n", trid->traddr);
		free(ctx);
		return -ENODEV;
	}
	ctx->poller = SPDK_POLLER_REGISTER(bdev_nvme_async_poll, ctx, 1000);

	return 0;
}

int
bdev_nvme_delete(const char *name, const struct nvme_path_id *path_id)
{
	struct nvme_bdev_ctrlr	*nbdev_ctrlr;
	struct nvme_ctrlr	*nvme_ctrlr, *tmp_nvme_ctrlr;
	struct nvme_path_id	*p, *t;
	int			rc = -ENXIO;

	if (name == NULL || path_id == NULL) {
		return -EINVAL;
	}

	nbdev_ctrlr = nvme_bdev_ctrlr_get_by_name(name);
	if (nbdev_ctrlr == NULL) {
		SPDK_ERRLOG("Failed to find NVMe bdev controller\n");
		return -ENODEV;
	}

	TAILQ_FOREACH_SAFE(nvme_ctrlr, &nbdev_ctrlr->ctrlrs, tailq, tmp_nvme_ctrlr) {
		TAILQ_FOREACH_REVERSE_SAFE(p, &nvme_ctrlr->trids, nvme_paths, link, t) {
			if (path_id->trid.trtype != 0) {
				if (path_id->trid.trtype == SPDK_NVME_TRANSPORT_CUSTOM) {
					if (strcasecmp(path_id->trid.trstring, p->trid.trstring) != 0) {
						continue;
					}
				} else {
					if (path_id->trid.trtype != p->trid.trtype) {
						continue;
					}
				}
			}

			if (!spdk_mem_all_zero(path_id->trid.traddr, sizeof(path_id->trid.traddr))) {
				if (strcasecmp(path_id->trid.traddr, p->trid.traddr) != 0) {
					continue;
				}
			}

			if (path_id->trid.adrfam != 0) {
				if (path_id->trid.adrfam != p->trid.adrfam) {
					continue;
				}
			}

			if (!spdk_mem_all_zero(path_id->trid.trsvcid, sizeof(path_id->trid.trsvcid))) {
				if (strcasecmp(path_id->trid.trsvcid, p->trid.trsvcid) != 0) {
					continue;
				}
			}

			if (!spdk_mem_all_zero(path_id->trid.subnqn, sizeof(path_id->trid.subnqn))) {
				if (strcmp(path_id->trid.subnqn, p->trid.subnqn) != 0) {
					continue;
				}
			}

			if (!spdk_mem_all_zero(path_id->hostid.hostaddr, sizeof(path_id->hostid.hostaddr))) {
				if (strcmp(path_id->hostid.hostaddr, p->hostid.hostaddr) != 0) {
					continue;
				}
			}

			if (!spdk_mem_all_zero(path_id->hostid.hostsvcid, sizeof(path_id->hostid.hostsvcid))) {
				if (strcmp(path_id->hostid.hostsvcid, p->hostid.hostsvcid) != 0) {
					continue;
				}
			}

			/* If we made it here, then this path is a match! Now we need to remove it. */
			if (p == nvme_ctrlr->active_path_id) {
				/* This is the active path in use right now. The active path is always the first in the list. */

				if (!TAILQ_NEXT(p, link)) {
					/* The current path is the only path. */
					rc = _bdev_nvme_delete(nvme_ctrlr, false);
				} else {
					/* There is an alternative path. */
					rc = bdev_nvme_failover(nvme_ctrlr, true);
				}
			} else {
				/* We are not using the specified path. */
				TAILQ_REMOVE(&nvme_ctrlr->trids, p, link);
				free(p);
				rc = 0;
			}

			if (rc < 0 && rc != -ENXIO) {
				return rc;
			}


		}
	}

	/* All nvme_ctrlrs were deleted or no nvme_ctrlr which had the trid was found. */
	return rc;
}

struct discovery_ctrlr_ctx {
	char						name[128];
	struct spdk_nvme_transport_id			trid;
	struct spdk_nvme_ctrlr_opts			opts;
	struct spdk_nvmf_discovery_log_page_entry	entry;
	TAILQ_ENTRY(discovery_ctrlr_ctx)		tailq;
	struct discovery_ctx				*ctx;
};

struct discovery_ctx {
	char					*name;
	spdk_bdev_nvme_start_discovery_fn	start_cb_fn;
	spdk_bdev_nvme_stop_discovery_fn	stop_cb_fn;
	void					*cb_ctx;
	struct spdk_nvme_probe_ctx		*probe_ctx;
	struct spdk_nvme_detach_ctx		*detach_ctx;
	struct spdk_nvme_ctrlr			*ctrlr;
	struct spdk_poller			*poller;
	struct spdk_nvme_ctrlr_opts		opts;
	TAILQ_ENTRY(discovery_ctx)		tailq;
	TAILQ_HEAD(, discovery_ctrlr_ctx)	ctrlr_ctxs;
	int					rc;
	/* Denotes if a discovery is currently in progress for this context.
	 * That includes connecting to newly discovered subsystems.  Used to
	 * ensure we do not start a new discovery until an existing one is
	 * complete.
	 */
	bool					in_progress;

	/* Denotes if another discovery is needed after the one in progress
	 * completes.  Set when we receive an AER completion while a discovery
	 * is already in progress.
	 */
	bool					pending;

	/* Signal to the discovery context poller that it should detach from
	 * the discovery controller.
	 */
	bool					detach;

	struct spdk_thread			*calling_thread;
	uint32_t				index;
	uint32_t				attach_in_progress;
	char					*hostnqn;
};

TAILQ_HEAD(discovery_ctxs, discovery_ctx);
static struct discovery_ctxs g_discovery_ctxs = TAILQ_HEAD_INITIALIZER(g_discovery_ctxs);

static void get_discovery_log_page(struct discovery_ctx *ctx);

static void
free_discovery_ctx(struct discovery_ctx *ctx)
{
	free(ctx->hostnqn);
	free(ctx->name);
	free(ctx);
}

static void
discovery_complete(struct discovery_ctx *ctx)
{
	ctx->in_progress = false;
	if (ctx->pending) {
		ctx->pending = false;
		get_discovery_log_page(ctx);
	}
}

static void
build_trid_from_log_page_entry(struct spdk_nvme_transport_id *trid,
			       struct spdk_nvmf_discovery_log_page_entry *entry)
{
	char *space;

	trid->trtype = entry->trtype;
	trid->adrfam = entry->adrfam;
	memcpy(trid->traddr, entry->traddr, sizeof(trid->traddr));
	memcpy(trid->trsvcid, entry->trsvcid, sizeof(trid->trsvcid));
	memcpy(trid->subnqn, entry->subnqn, sizeof(trid->subnqn));

	/* We want the traddr, trsvcid and subnqn fields to be NULL-terminated.
	 * But the log page entries typically pad them with spaces, not zeroes.
	 * So add a NULL terminator to each of these fields at the appropriate
	 * location.
	 */
	space = strchr(trid->traddr, ' ');
	if (space) {
		*space = 0;
	}
	space = strchr(trid->trsvcid, ' ');
	if (space) {
		*space = 0;
	}
	space = strchr(trid->subnqn, ' ');
	if (space) {
		*space = 0;
	}
}

static void
discovery_attach_controller_done(void *cb_ctx, size_t bdev_count, int rc)
{
	struct discovery_ctrlr_ctx *ctrlr_ctx = cb_ctx;
	struct discovery_ctx *ctx = ctrlr_ctx->ctx;;

	SPDK_DEBUGLOG(bdev_nvme, "attach %s done\n", ctrlr_ctx->name);
	ctx->attach_in_progress--;
	if (ctx->attach_in_progress == 0) {
		discovery_complete(ctx);
	}
}

static void
discovery_log_page_cb(void *cb_arg, int rc, const struct spdk_nvme_cpl *cpl,
		      struct spdk_nvmf_discovery_log_page *log_page)
{
	struct discovery_ctx *ctx = cb_arg;
	struct discovery_ctrlr_ctx *ctrlr_ctx, *tmp;
	struct spdk_nvmf_discovery_log_page_entry *new_entry, *old_entry;
	uint64_t numrec, i;
	bool found;

	if (rc || spdk_nvme_cpl_is_error(cpl)) {
		SPDK_ERRLOG("could not get discovery log page\n");
		return;
	}

	assert(ctx->attach_in_progress == 0);
	numrec = from_le64(&log_page->numrec);
	TAILQ_FOREACH_SAFE(ctrlr_ctx, &ctx->ctrlr_ctxs, tailq, tmp) {
		found = false;
		old_entry = &ctrlr_ctx->entry;
		for (i = 0; i < numrec; i++) {
			new_entry = &log_page->entries[i];
			if (!memcmp(old_entry, new_entry, sizeof(*old_entry))) {
				found = true;
				break;
			}
		}
		if (!found) {
			struct nvme_path_id path = {};

			SPDK_DEBUGLOG(bdev_nvme, "detach controller\n");

			path.trid = ctrlr_ctx->trid;
			bdev_nvme_delete(ctrlr_ctx->name, &path);
			TAILQ_REMOVE(&ctx->ctrlr_ctxs, ctrlr_ctx, tailq);
			free(ctrlr_ctx);
		}
	}
	for (i = 0; i < numrec; i++) {
		found = false;
		new_entry = &log_page->entries[i];
		TAILQ_FOREACH(ctrlr_ctx, &ctx->ctrlr_ctxs, tailq) {
			old_entry = &ctrlr_ctx->entry;
			if (!memcmp(new_entry, old_entry, sizeof(*new_entry))) {
				found = true;
				break;
			}
		}
		if (!found) {
			struct discovery_ctrlr_ctx *subnqn_ctx, *new_ctx;

			TAILQ_FOREACH(subnqn_ctx, &ctx->ctrlr_ctxs, tailq) {
				if (!memcmp(subnqn_ctx->entry.subnqn, new_entry->subnqn,
					    sizeof(new_entry->subnqn))) {
					break;
				}
			}

			new_ctx = calloc(1, sizeof(*ctrlr_ctx));
			if (new_ctx == NULL) {
				SPDK_ERRLOG("could not allocate new ctrlr_ctx\n");
				break;
			}

			new_ctx->ctx = ctx;
			memcpy(&new_ctx->entry, new_entry, sizeof(*new_entry));
			build_trid_from_log_page_entry(&new_ctx->trid, new_entry);
			if (subnqn_ctx) {
				snprintf(new_ctx->name, sizeof(new_ctx->name), "%s", subnqn_ctx->name);
			} else {
				snprintf(new_ctx->name, sizeof(new_ctx->name), "%s%d", ctx->name, ctx->index++);
			}
			spdk_nvme_ctrlr_get_default_ctrlr_opts(&new_ctx->opts, sizeof(new_ctx->opts));
			snprintf(new_ctx->opts.hostnqn, sizeof(new_ctx->opts.hostnqn), "%s", ctx->hostnqn);
			rc = bdev_nvme_create(&new_ctx->trid, new_ctx->name, NULL, 0, 0,
					      discovery_attach_controller_done, new_ctx,
					      &new_ctx->opts, true, 0, 0, 0);
			if (rc == 0) {
				TAILQ_INSERT_TAIL(&ctx->ctrlr_ctxs, new_ctx, tailq);
				ctx->attach_in_progress++;
			} else {
				SPDK_ERRLOG("bdev_nvme_create failed (%s)\n", spdk_strerror(-rc));
			}
		}
	}
	free(log_page);

	if (ctx->attach_in_progress == 0) {
		discovery_complete(ctx);
	}
}

static void
get_discovery_log_page(struct discovery_ctx *ctx)
{
	int rc;

	assert(ctx->in_progress == false);
	ctx->in_progress = true;
	rc = spdk_nvme_ctrlr_get_discovery_log_page(ctx->ctrlr, discovery_log_page_cb, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("could not get discovery log page\n");
	}
	SPDK_DEBUGLOG(bdev_nvme, "sent discovery log page command\n");
}

static void
discovery_aer_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct discovery_ctx *ctx = arg;
	uint32_t log_page_id = (cpl->cdw0 & 0xFF0000) >> 16;

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_ERRLOG("aer failed\n");
		return;
	}

	if (log_page_id != SPDK_NVME_LOG_DISCOVERY) {
		SPDK_ERRLOG("unexpected log page 0x%x\n", log_page_id);
		return;
	}

	SPDK_DEBUGLOG(bdev_nvme, "got aer\n");
	if (ctx->in_progress) {
		ctx->pending = true;
		return;
	}

	get_discovery_log_page(ctx);
}

static void
start_discovery_done(void *cb_ctx)
{
	struct discovery_ctx *ctx = cb_ctx;

	SPDK_DEBUGLOG(bdev_nvme, "start discovery done\n");
	ctx->start_cb_fn(ctx->cb_ctx, ctx->rc);
	if (ctx->rc != 0) {
		SPDK_ERRLOG("could not connect to discovery ctrlr\n");
		TAILQ_REMOVE(&g_discovery_ctxs, ctx, tailq);
		free_discovery_ctx(ctx);
	}
}

static int
discovery_poller(void *arg)
{
	struct discovery_ctx *ctx = arg;
	int rc;

	if (ctx->probe_ctx) {
		rc = spdk_nvme_probe_poll_async(ctx->probe_ctx);
		if (rc != -EAGAIN) {
			ctx->rc = rc;
			spdk_thread_send_msg(ctx->calling_thread, start_discovery_done, ctx);
			if (rc == 0) {
				get_discovery_log_page(ctx);
			}
		}
	} else if (ctx->detach) {
		bool detach_done = false;

		if (ctx->detach_ctx == NULL) {
			rc = spdk_nvme_detach_async(ctx->ctrlr, &ctx->detach_ctx);
			if (rc != 0) {
				SPDK_ERRLOG("could not detach discovery ctrlr\n");
				detach_done = true;
			}
		} else {
			rc = spdk_nvme_detach_poll_async(ctx->detach_ctx);
			if (rc != -EAGAIN) {
				detach_done = true;
			}
		}
		if (detach_done) {
			spdk_poller_unregister(&ctx->poller);
			TAILQ_REMOVE(&g_discovery_ctxs, ctx, tailq);
			ctx->stop_cb_fn(ctx->cb_ctx);
			free_discovery_ctx(ctx);
		}
	} else {
		spdk_nvme_ctrlr_process_admin_completions(ctx->ctrlr);
	}

	return SPDK_POLLER_BUSY;
}

static void
discovery_attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
		    struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvme_ctrlr_opts *user_opts = cb_ctx;
	struct discovery_ctx *ctx;

	ctx = SPDK_CONTAINEROF(user_opts, struct discovery_ctx, opts);

	SPDK_DEBUGLOG(bdev_nvme, "discovery ctrlr attached\n");
	ctx->probe_ctx = NULL;
	ctx->ctrlr = ctrlr;
	spdk_nvme_ctrlr_register_aer_callback(ctx->ctrlr, discovery_aer_cb, ctx);
}

static void
start_discovery_poller(void *arg)
{
	struct discovery_ctx *ctx = arg;

	TAILQ_INSERT_TAIL(&g_discovery_ctxs, ctx, tailq);
	ctx->poller = SPDK_POLLER_REGISTER(discovery_poller, ctx, 1000);
}

int
bdev_nvme_start_discovery(struct spdk_nvme_transport_id *trid,
			  const char *base_name,
			  struct spdk_nvme_ctrlr_opts *opts,
			  spdk_bdev_nvme_start_discovery_fn cb_fn,
			  void *cb_ctx)
{
	struct discovery_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	ctx->name = strdup(base_name);
	if (ctx->name == NULL) {
		free_discovery_ctx(ctx);
		return -ENOMEM;
	}
	ctx->start_cb_fn = cb_fn;
	ctx->cb_ctx = cb_ctx;
	memcpy(&ctx->opts, opts, sizeof(*opts));
	ctx->calling_thread = spdk_get_thread();
	TAILQ_INIT(&ctx->ctrlr_ctxs);
	snprintf(trid->subnqn, sizeof(trid->subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);
	/* Even if user did not specify hostnqn, we can still strdup("\0"); */
	ctx->hostnqn = strdup(ctx->opts.hostnqn);
	if (ctx->hostnqn == NULL) {
		free_discovery_ctx(ctx);
		return -ENOMEM;
	}
	ctx->probe_ctx = spdk_nvme_connect_async(trid, &ctx->opts, discovery_attach_cb);
	if (ctx->probe_ctx == NULL) {
		SPDK_ERRLOG("could not start discovery connect\n");
		free_discovery_ctx(ctx);
		return -EIO;
	}

	spdk_thread_send_msg(g_bdev_nvme_init_thread, start_discovery_poller, ctx);
	return 0;
}

int
bdev_nvme_stop_discovery(const char *name, spdk_bdev_nvme_stop_discovery_fn cb_fn, void *cb_ctx)
{
	struct discovery_ctx *ctx;

	TAILQ_FOREACH(ctx, &g_discovery_ctxs, tailq) {
		if (strcmp(name, ctx->name) == 0) {
			if (ctx->detach) {
				return -EALREADY;
			}
			ctx->detach = true;
			ctx->stop_cb_fn = cb_fn;
			ctx->cb_ctx = cb_ctx;
			while (!TAILQ_EMPTY(&ctx->ctrlr_ctxs)) {
				struct discovery_ctrlr_ctx *ctrlr_ctx;
				struct nvme_path_id path = {};

				ctrlr_ctx = TAILQ_FIRST(&ctx->ctrlr_ctxs);
				path.trid = ctrlr_ctx->trid;
				bdev_nvme_delete(ctrlr_ctx->name, &path);
				TAILQ_REMOVE(&ctx->ctrlr_ctxs, ctrlr_ctx, tailq);
				free(ctrlr_ctx);
			}
			return 0;
		}
	}

	return -ENOENT;
}

static int
bdev_nvme_library_init(void)
{
	g_bdev_nvme_init_thread = spdk_get_thread();

	spdk_io_device_register(&g_nvme_bdev_ctrlrs, bdev_nvme_create_poll_group_cb,
				bdev_nvme_destroy_poll_group_cb,
				sizeof(struct nvme_poll_group),  "nvme_poll_groups");

	return 0;
}

static void
bdev_nvme_fini_destruct_ctrlrs(void)
{
	struct nvme_bdev_ctrlr *nbdev_ctrlr;
	struct nvme_ctrlr *nvme_ctrlr;

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	TAILQ_FOREACH(nbdev_ctrlr, &g_nvme_bdev_ctrlrs, tailq) {
		TAILQ_FOREACH(nvme_ctrlr, &nbdev_ctrlr->ctrlrs, tailq) {
			pthread_mutex_lock(&nvme_ctrlr->mutex);
			if (nvme_ctrlr->destruct) {
				/* This controller's destruction was already started
				 * before the application started shutting down
				 */
				pthread_mutex_unlock(&nvme_ctrlr->mutex);
				continue;
			}
			nvme_ctrlr->destruct = true;
			pthread_mutex_unlock(&nvme_ctrlr->mutex);

			spdk_thread_send_msg(nvme_ctrlr->thread, _nvme_ctrlr_destruct,
					     nvme_ctrlr);
		}
	}

	g_bdev_nvme_module_finish = true;
	if (TAILQ_EMPTY(&g_nvme_bdev_ctrlrs)) {
		pthread_mutex_unlock(&g_bdev_nvme_mutex);
		spdk_io_device_unregister(&g_nvme_bdev_ctrlrs, NULL);
		spdk_bdev_module_fini_done();
		return;
	}

	pthread_mutex_unlock(&g_bdev_nvme_mutex);
}

static void
check_discovery_fini(void *arg)
{
	if (TAILQ_EMPTY(&g_discovery_ctxs)) {
		bdev_nvme_fini_destruct_ctrlrs();
	}
}

static void
bdev_nvme_library_fini(void)
{
	struct nvme_probe_skip_entry *entry, *entry_tmp;
	struct discovery_ctx *ctx;

	spdk_poller_unregister(&g_hotplug_poller);
	free(g_hotplug_probe_ctx);
	g_hotplug_probe_ctx = NULL;

	TAILQ_FOREACH_SAFE(entry, &g_skipped_nvme_ctrlrs, tailq, entry_tmp) {
		TAILQ_REMOVE(&g_skipped_nvme_ctrlrs, entry, tailq);
		free(entry);
	}

	assert(spdk_get_thread() == g_bdev_nvme_init_thread);
	if (TAILQ_EMPTY(&g_discovery_ctxs)) {
		bdev_nvme_fini_destruct_ctrlrs();
	} else {
		TAILQ_FOREACH(ctx, &g_discovery_ctxs, tailq) {
			ctx->detach = true;
			ctx->stop_cb_fn = check_discovery_fini;
		}
	}
}

static void
bdev_nvme_verify_pi_error(struct nvme_bdev_io *bio)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);
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

	if (spdk_nvme_cpl_is_success(cpl)) {
		/* Run PI verification for read data buffer. */
		bdev_nvme_verify_pi_error(bio);
	}

	/* Return original completion status */
	bdev_nvme_io_complete_nvme_status(bio, &bio->cpl);
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

		if (spdk_likely(nvme_io_path_is_available(bio->io_path))) {
			/* Read without PI checking to verify PI error. */
			ret = bdev_nvme_no_pi_readv(bio,
						    bdev_io->u.bdev.iovs,
						    bdev_io->u.bdev.iovcnt,
						    bdev_io->u.bdev.md_buf,
						    bdev_io->u.bdev.num_blocks,
						    bdev_io->u.bdev.offset_blocks);
			if (ret == 0) {
				return;
			}
		}
	}

	bdev_nvme_io_complete_nvme_status(bio, cpl);
}

static void
bdev_nvme_writev_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;

	if (spdk_nvme_cpl_is_pi_error(cpl)) {
		SPDK_ERRLOG("writev completed with PI error (sct=%d, sc=%d)\n",
			    cpl->status.sct, cpl->status.sc);
		/* Run PI verification for write data buffer if PI error is detected. */
		bdev_nvme_verify_pi_error(bio);
	}

	bdev_nvme_io_complete_nvme_status(bio, cpl);
}

static void
bdev_nvme_zone_appendv_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);

	/* spdk_bdev_io_get_append_location() requires that the ALBA is stored in offset_blocks.
	 * Additionally, offset_blocks has to be set before calling bdev_nvme_verify_pi_error().
	 */
	bdev_io->u.bdev.offset_blocks = *(uint64_t *)&cpl->cdw0;

	if (spdk_nvme_cpl_is_pi_error(cpl)) {
		SPDK_ERRLOG("zone append completed with PI error (sct=%d, sc=%d)\n",
			    cpl->status.sct, cpl->status.sc);
		/* Run PI verification for zone append data buffer if PI error is detected. */
		bdev_nvme_verify_pi_error(bio);
	}

	bdev_nvme_io_complete_nvme_status(bio, cpl);
}

static void
bdev_nvme_comparev_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;

	if (spdk_nvme_cpl_is_pi_error(cpl)) {
		SPDK_ERRLOG("comparev completed with PI error (sct=%d, sc=%d)\n",
			    cpl->status.sct, cpl->status.sc);
		/* Run PI verification for compare data buffer if PI error is detected. */
		bdev_nvme_verify_pi_error(bio);
	}

	bdev_nvme_io_complete_nvme_status(bio, cpl);
}

static void
bdev_nvme_comparev_and_writev_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;

	/* Compare operation completion */
	if (!bio->first_fused_completed) {
		/* Save compare result for write callback */
		bio->cpl = *cpl;
		bio->first_fused_completed = true;
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

		bdev_nvme_io_complete_nvme_status(bio, &bio->cpl);
	} else {
		bdev_nvme_io_complete_nvme_status(bio, cpl);
	}
}

static void
bdev_nvme_queued_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;

	bdev_nvme_io_complete_nvme_status(bio, cpl);
}

static int
fill_zone_from_report(struct spdk_bdev_zone_info *info, struct spdk_nvme_zns_zone_desc *desc)
{
	switch (desc->zs) {
	case SPDK_NVME_ZONE_STATE_EMPTY:
		info->state = SPDK_BDEV_ZONE_STATE_EMPTY;
		break;
	case SPDK_NVME_ZONE_STATE_IOPEN:
		info->state = SPDK_BDEV_ZONE_STATE_IMP_OPEN;
		break;
	case SPDK_NVME_ZONE_STATE_EOPEN:
		info->state = SPDK_BDEV_ZONE_STATE_EXP_OPEN;
		break;
	case SPDK_NVME_ZONE_STATE_CLOSED:
		info->state = SPDK_BDEV_ZONE_STATE_CLOSED;
		break;
	case SPDK_NVME_ZONE_STATE_RONLY:
		info->state = SPDK_BDEV_ZONE_STATE_READ_ONLY;
		break;
	case SPDK_NVME_ZONE_STATE_FULL:
		info->state = SPDK_BDEV_ZONE_STATE_FULL;
		break;
	case SPDK_NVME_ZONE_STATE_OFFLINE:
		info->state = SPDK_BDEV_ZONE_STATE_OFFLINE;
		break;
	default:
		SPDK_ERRLOG("Invalid zone state: %#x in zone report\n", desc->zs);
		return -EIO;
	}

	info->zone_id = desc->zslba;
	info->write_pointer = desc->wp;
	info->capacity = desc->zcap;

	return 0;
}

static void
bdev_nvme_get_zone_info_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);
	uint64_t zone_id = bdev_io->u.zone_mgmt.zone_id;
	uint32_t zones_to_copy = bdev_io->u.zone_mgmt.num_zones;
	struct spdk_bdev_zone_info *info = bdev_io->u.zone_mgmt.buf;
	uint64_t max_zones_per_buf, i;
	uint32_t zone_report_bufsize;
	struct spdk_nvme_ns *ns;
	struct spdk_nvme_qpair *qpair;
	int ret;

	if (spdk_nvme_cpl_is_error(cpl)) {
		goto out_complete_io_nvme_cpl;
	}

	if (spdk_unlikely(!nvme_io_path_is_available(bio->io_path))) {
		ret = -ENXIO;
		goto out_complete_io_ret;
	}

	ns = bio->io_path->nvme_ns->ns;
	qpair = bio->io_path->ctrlr_ch->qpair;

	zone_report_bufsize = spdk_nvme_ns_get_max_io_xfer_size(ns);
	max_zones_per_buf = (zone_report_bufsize - sizeof(*bio->zone_report_buf)) /
			    sizeof(bio->zone_report_buf->descs[0]);

	if (bio->zone_report_buf->nr_zones > max_zones_per_buf) {
		ret = -EINVAL;
		goto out_complete_io_ret;
	}

	if (!bio->zone_report_buf->nr_zones) {
		ret = -EINVAL;
		goto out_complete_io_ret;
	}

	for (i = 0; i < bio->zone_report_buf->nr_zones && bio->handled_zones < zones_to_copy; i++) {
		ret = fill_zone_from_report(&info[bio->handled_zones],
					    &bio->zone_report_buf->descs[i]);
		if (ret) {
			goto out_complete_io_ret;
		}
		bio->handled_zones++;
	}

	if (bio->handled_zones < zones_to_copy) {
		uint64_t zone_size_lba = spdk_nvme_zns_ns_get_zone_size_sectors(ns);
		uint64_t slba = zone_id + (zone_size_lba * bio->handled_zones);

		memset(bio->zone_report_buf, 0, zone_report_bufsize);
		ret = spdk_nvme_zns_report_zones(ns, qpair,
						 bio->zone_report_buf, zone_report_bufsize,
						 slba, SPDK_NVME_ZRA_LIST_ALL, true,
						 bdev_nvme_get_zone_info_done, bio);
		if (!ret) {
			return;
		} else {
			goto out_complete_io_ret;
		}
	}

out_complete_io_nvme_cpl:
	free(bio->zone_report_buf);
	bio->zone_report_buf = NULL;
	bdev_nvme_io_complete_nvme_status(bio, cpl);
	return;

out_complete_io_ret:
	free(bio->zone_report_buf);
	bio->zone_report_buf = NULL;
	bdev_nvme_io_complete(bio, ret);
}

static void
bdev_nvme_zone_management_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;

	bdev_nvme_io_complete_nvme_status(bio, cpl);
}

static void
bdev_nvme_admin_passthru_complete_nvme_status(void *ctx)
{
	struct nvme_bdev_io *bio = ctx;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);
	const struct spdk_nvme_cpl *cpl = &bio->cpl;
	struct nvme_bdev_channel *nbdev_ch;
	struct nvme_ctrlr *nvme_ctrlr;
	const struct spdk_nvme_ctrlr_data *cdata;
	uint64_t delay_ms;

	assert(bdev_nvme_io_type_is_admin(bdev_io->type));

	if (spdk_likely(spdk_nvme_cpl_is_success(cpl))) {
		goto complete;
	}

	if (cpl->status.dnr != 0 || (g_opts.bdev_retry_count != -1 &&
				     bio->retry_count >= g_opts.bdev_retry_count)) {
		goto complete;
	}

	nbdev_ch = spdk_io_channel_get_ctx(spdk_bdev_io_get_io_channel(bdev_io));
	nvme_ctrlr = nvme_ctrlr_channel_get_ctrlr(bio->io_path->ctrlr_ch);

	if (spdk_nvme_cpl_is_path_error(cpl) ||
	    spdk_nvme_cpl_is_aborted_sq_deletion(cpl) ||
	    !nvme_ctrlr_is_available(nvme_ctrlr)) {
		delay_ms = 0;
	} else if (spdk_nvme_cpl_is_aborted_by_request(cpl)) {
		goto complete;
	} else {
		bio->retry_count++;

		cdata = spdk_nvme_ctrlr_get_data(nvme_ctrlr->ctrlr);

		if (cpl->status.crd != 0) {
			delay_ms = cdata->crdt[cpl->status.crd] * 100;
		} else {
			delay_ms = 0;
		}
	}

	if (any_ctrlr_may_become_available(nbdev_ch)) {
		bdev_nvme_queue_retry_io(nbdev_ch, bio, delay_ms);
		return;
	}

complete:
	bio->retry_count = 0;
	spdk_bdev_io_complete_nvme_status(bdev_io, cpl->cdw0, cpl->status.sct, cpl->status.sc);
}

static void
bdev_nvme_abort_complete(void *ctx)
{
	struct nvme_bdev_io *bio = ctx;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);

	if (spdk_nvme_cpl_is_abort_success(&bio->cpl)) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
bdev_nvme_abort_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;

	bio->cpl = *cpl;
	spdk_thread_send_msg(bio->orig_thread, bdev_nvme_abort_complete, bio);
}

static void
bdev_nvme_admin_passthru_done(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_bdev_io *bio = ref;

	bio->cpl = *cpl;
	spdk_thread_send_msg(bio->orig_thread,
			     bdev_nvme_admin_passthru_complete_nvme_status, bio);
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
bdev_nvme_no_pi_readv(struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
		      void *md, uint64_t lba_count, uint64_t lba)
{
	int rc;

	SPDK_DEBUGLOG(bdev_nvme, "read %" PRIu64 " blocks with offset %#" PRIx64 " without PI check\n",
		      lba_count, lba);

	bio->iovs = iov;
	bio->iovcnt = iovcnt;
	bio->iovpos = 0;
	bio->iov_offset = 0;

	rc = spdk_nvme_ns_cmd_readv_with_md(bio->io_path->nvme_ns->ns,
					    bio->io_path->ctrlr_ch->qpair,
					    lba, lba_count,
					    bdev_nvme_no_pi_readv_done, bio, 0,
					    bdev_nvme_queued_reset_sgl, bdev_nvme_queued_next_sge,
					    md, 0, 0);

	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("no_pi_readv failed: rc = %d\n", rc);
	}
	return rc;
}

static int
bdev_nvme_readv(struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
		void *md, uint64_t lba_count, uint64_t lba, uint32_t flags,
		struct spdk_bdev_ext_io_opts *ext_opts)
{
	struct spdk_nvme_ns *ns = bio->io_path->nvme_ns->ns;
	struct spdk_nvme_qpair *qpair = bio->io_path->ctrlr_ch->qpair;
	int rc;

	SPDK_DEBUGLOG(bdev_nvme, "read %" PRIu64 " blocks with offset %#" PRIx64 "\n",
		      lba_count, lba);

	bio->iovs = iov;
	bio->iovcnt = iovcnt;
	bio->iovpos = 0;
	bio->iov_offset = 0;

	if (ext_opts) {
		bio->ext_opts.size = sizeof(struct spdk_nvme_ns_cmd_ext_io_opts);
		bio->ext_opts.memory_domain = ext_opts->memory_domain;
		bio->ext_opts.memory_domain_ctx = ext_opts->memory_domain_ctx;
		bio->ext_opts.io_flags = flags;
		bio->ext_opts.metadata = md;

		rc = spdk_nvme_ns_cmd_readv_ext(ns, qpair, lba, lba_count,
						bdev_nvme_readv_done, bio,
						bdev_nvme_queued_reset_sgl, bdev_nvme_queued_next_sge,
						&bio->ext_opts);
	} else if (iovcnt == 1) {
		rc = spdk_nvme_ns_cmd_read_with_md(ns, qpair, iov[0].iov_base, md, lba,
						   lba_count,
						   bdev_nvme_readv_done, bio,
						   flags,
						   0, 0);
	} else {
		rc = spdk_nvme_ns_cmd_readv_with_md(ns, qpair, lba, lba_count,
						    bdev_nvme_readv_done, bio, flags,
						    bdev_nvme_queued_reset_sgl, bdev_nvme_queued_next_sge,
						    md, 0, 0);
	}

	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("readv failed: rc = %d\n", rc);
	}
	return rc;
}

static int
bdev_nvme_writev(struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
		 void *md, uint64_t lba_count, uint64_t lba,
		 uint32_t flags, struct spdk_bdev_ext_io_opts *ext_opts)
{
	struct spdk_nvme_ns *ns = bio->io_path->nvme_ns->ns;
	struct spdk_nvme_qpair *qpair = bio->io_path->ctrlr_ch->qpair;
	int rc;

	SPDK_DEBUGLOG(bdev_nvme, "write %" PRIu64 " blocks with offset %#" PRIx64 "\n",
		      lba_count, lba);

	bio->iovs = iov;
	bio->iovcnt = iovcnt;
	bio->iovpos = 0;
	bio->iov_offset = 0;

	if (ext_opts) {
		bio->ext_opts.size = sizeof(struct spdk_nvme_ns_cmd_ext_io_opts);
		bio->ext_opts.memory_domain = ext_opts->memory_domain;
		bio->ext_opts.memory_domain_ctx = ext_opts->memory_domain_ctx;
		bio->ext_opts.io_flags = flags;
		bio->ext_opts.metadata = md;

		rc = spdk_nvme_ns_cmd_writev_ext(ns, qpair, lba, lba_count,
						 bdev_nvme_writev_done, bio,
						 bdev_nvme_queued_reset_sgl, bdev_nvme_queued_next_sge,
						 &bio->ext_opts);
	} else if (iovcnt == 1) {
		rc = spdk_nvme_ns_cmd_write_with_md(ns, qpair, iov[0].iov_base, md, lba,
						    lba_count,
						    bdev_nvme_writev_done, bio,
						    flags,
						    0, 0);
	} else {
		rc = spdk_nvme_ns_cmd_writev_with_md(ns, qpair, lba, lba_count,
						     bdev_nvme_writev_done, bio, flags,
						     bdev_nvme_queued_reset_sgl, bdev_nvme_queued_next_sge,
						     md, 0, 0);
	}

	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("writev failed: rc = %d\n", rc);
	}
	return rc;
}

static int
bdev_nvme_zone_appendv(struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
		       void *md, uint64_t lba_count, uint64_t zslba,
		       uint32_t flags)
{
	struct spdk_nvme_ns *ns = bio->io_path->nvme_ns->ns;
	struct spdk_nvme_qpair *qpair = bio->io_path->ctrlr_ch->qpair;
	int rc;

	SPDK_DEBUGLOG(bdev_nvme, "zone append %" PRIu64 " blocks to zone start lba %#" PRIx64 "\n",
		      lba_count, zslba);

	bio->iovs = iov;
	bio->iovcnt = iovcnt;
	bio->iovpos = 0;
	bio->iov_offset = 0;

	if (iovcnt == 1) {
		rc = spdk_nvme_zns_zone_append_with_md(ns, qpair, iov[0].iov_base, md, zslba,
						       lba_count,
						       bdev_nvme_zone_appendv_done, bio,
						       flags,
						       0, 0);
	} else {
		rc = spdk_nvme_zns_zone_appendv_with_md(ns, qpair, zslba, lba_count,
							bdev_nvme_zone_appendv_done, bio, flags,
							bdev_nvme_queued_reset_sgl, bdev_nvme_queued_next_sge,
							md, 0, 0);
	}

	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("zone append failed: rc = %d\n", rc);
	}
	return rc;
}

static int
bdev_nvme_comparev(struct nvme_bdev_io *bio, struct iovec *iov, int iovcnt,
		   void *md, uint64_t lba_count, uint64_t lba,
		   uint32_t flags)
{
	int rc;

	SPDK_DEBUGLOG(bdev_nvme, "compare %" PRIu64 " blocks with offset %#" PRIx64 "\n",
		      lba_count, lba);

	bio->iovs = iov;
	bio->iovcnt = iovcnt;
	bio->iovpos = 0;
	bio->iov_offset = 0;

	rc = spdk_nvme_ns_cmd_comparev_with_md(bio->io_path->nvme_ns->ns,
					       bio->io_path->ctrlr_ch->qpair,
					       lba, lba_count,
					       bdev_nvme_comparev_done, bio, flags,
					       bdev_nvme_queued_reset_sgl, bdev_nvme_queued_next_sge,
					       md, 0, 0);

	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("comparev failed: rc = %d\n", rc);
	}
	return rc;
}

static int
bdev_nvme_comparev_and_writev(struct nvme_bdev_io *bio, struct iovec *cmp_iov, int cmp_iovcnt,
			      struct iovec *write_iov, int write_iovcnt,
			      void *md, uint64_t lba_count, uint64_t lba, uint32_t flags)
{
	struct spdk_nvme_ns *ns = bio->io_path->nvme_ns->ns;
	struct spdk_nvme_qpair *qpair = bio->io_path->ctrlr_ch->qpair;
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);
	int rc;

	SPDK_DEBUGLOG(bdev_nvme, "compare and write %" PRIu64 " blocks with offset %#" PRIx64 "\n",
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
		bio->first_fused_completed = false;
	}

	if (!bio->first_fused_submitted) {
		flags |= SPDK_NVME_IO_FLAGS_FUSE_FIRST;
		memset(&bio->cpl, 0, sizeof(bio->cpl));

		rc = spdk_nvme_ns_cmd_comparev_with_md(ns, qpair, lba, lba_count,
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

	rc = spdk_nvme_ns_cmd_writev_with_md(ns, qpair, lba, lba_count,
					     bdev_nvme_comparev_and_writev_done, bio, flags,
					     bdev_nvme_queued_reset_fused_sgl, bdev_nvme_queued_next_fused_sge, md, 0, 0);
	if (rc != 0 && rc != -ENOMEM) {
		SPDK_ERRLOG("write failed: rc = %d\n", rc);
		rc = 0;
	}

	return rc;
}

static int
bdev_nvme_unmap(struct nvme_bdev_io *bio, uint64_t offset_blocks, uint64_t num_blocks)
{
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

	rc = spdk_nvme_ns_cmd_dataset_management(bio->io_path->nvme_ns->ns,
			bio->io_path->ctrlr_ch->qpair,
			SPDK_NVME_DSM_ATTR_DEALLOCATE,
			dsm_ranges, num_ranges,
			bdev_nvme_queued_done, bio);

	return rc;
}

static int
bdev_nvme_write_zeroes(struct nvme_bdev_io *bio, uint64_t offset_blocks, uint64_t num_blocks)
{
	if (num_blocks > UINT16_MAX + 1) {
		SPDK_ERRLOG("NVMe write zeroes is limited to 16-bit block count\n");
		return -EINVAL;
	}

	return spdk_nvme_ns_cmd_write_zeroes(bio->io_path->nvme_ns->ns,
					     bio->io_path->ctrlr_ch->qpair,
					     offset_blocks, num_blocks,
					     bdev_nvme_queued_done, bio,
					     0);
}

static int
bdev_nvme_get_zone_info(struct nvme_bdev_io *bio, uint64_t zone_id, uint32_t num_zones,
			struct spdk_bdev_zone_info *info)
{
	struct spdk_nvme_ns *ns = bio->io_path->nvme_ns->ns;
	struct spdk_nvme_qpair *qpair = bio->io_path->ctrlr_ch->qpair;
	uint32_t zone_report_bufsize = spdk_nvme_ns_get_max_io_xfer_size(ns);
	uint64_t zone_size = spdk_nvme_zns_ns_get_zone_size_sectors(ns);
	uint64_t total_zones = spdk_nvme_zns_ns_get_num_zones(ns);

	if (zone_id % zone_size != 0) {
		return -EINVAL;
	}

	if (num_zones > total_zones || !num_zones) {
		return -EINVAL;
	}

	assert(!bio->zone_report_buf);
	bio->zone_report_buf = calloc(1, zone_report_bufsize);
	if (!bio->zone_report_buf) {
		return -ENOMEM;
	}

	bio->handled_zones = 0;

	return spdk_nvme_zns_report_zones(ns, qpair, bio->zone_report_buf, zone_report_bufsize,
					  zone_id, SPDK_NVME_ZRA_LIST_ALL, true,
					  bdev_nvme_get_zone_info_done, bio);
}

static int
bdev_nvme_zone_management(struct nvme_bdev_io *bio, uint64_t zone_id,
			  enum spdk_bdev_zone_action action)
{
	struct spdk_nvme_ns *ns = bio->io_path->nvme_ns->ns;
	struct spdk_nvme_qpair *qpair = bio->io_path->ctrlr_ch->qpair;

	switch (action) {
	case SPDK_BDEV_ZONE_CLOSE:
		return spdk_nvme_zns_close_zone(ns, qpair, zone_id, false,
						bdev_nvme_zone_management_done, bio);
	case SPDK_BDEV_ZONE_FINISH:
		return spdk_nvme_zns_finish_zone(ns, qpair, zone_id, false,
						 bdev_nvme_zone_management_done, bio);
	case SPDK_BDEV_ZONE_OPEN:
		return spdk_nvme_zns_open_zone(ns, qpair, zone_id, false,
					       bdev_nvme_zone_management_done, bio);
	case SPDK_BDEV_ZONE_RESET:
		return spdk_nvme_zns_reset_zone(ns, qpair, zone_id, false,
						bdev_nvme_zone_management_done, bio);
	case SPDK_BDEV_ZONE_OFFLINE:
		return spdk_nvme_zns_offline_zone(ns, qpair, zone_id, false,
						  bdev_nvme_zone_management_done, bio);
	default:
		return -EINVAL;
	}
}

static void
bdev_nvme_admin_passthru(struct nvme_bdev_channel *nbdev_ch, struct nvme_bdev_io *bio,
			 struct spdk_nvme_cmd *cmd, void *buf, size_t nbytes)
{
	struct nvme_io_path *io_path;
	struct nvme_ctrlr *nvme_ctrlr;
	uint32_t max_xfer_size;
	int rc = -ENXIO;

	/* Choose the first ctrlr which is not failed. */
	STAILQ_FOREACH(io_path, &nbdev_ch->io_path_list, stailq) {
		nvme_ctrlr = nvme_ctrlr_channel_get_ctrlr(io_path->ctrlr_ch);

		/* We should skip any unavailable nvme_ctrlr rather than checking
		 * if the return value of spdk_nvme_ctrlr_cmd_admin_raw() is -ENXIO.
		 */
		if (!nvme_ctrlr_is_available(nvme_ctrlr)) {
			continue;
		}

		max_xfer_size = spdk_nvme_ctrlr_get_max_xfer_size(nvme_ctrlr->ctrlr);

		if (nbytes > max_xfer_size) {
			SPDK_ERRLOG("nbytes is greater than MDTS %" PRIu32 ".\n", max_xfer_size);
			rc = -EINVAL;
			goto err;
		}

		bio->io_path = io_path;
		bio->orig_thread = spdk_get_thread();

		rc = spdk_nvme_ctrlr_cmd_admin_raw(nvme_ctrlr->ctrlr, cmd, buf, (uint32_t)nbytes,
						   bdev_nvme_admin_passthru_done, bio);
		if (rc == 0) {
			return;
		}
	}

err:
	bdev_nvme_admin_passthru_complete(bio, rc);
}

static int
bdev_nvme_io_passthru(struct nvme_bdev_io *bio, struct spdk_nvme_cmd *cmd,
		      void *buf, size_t nbytes)
{
	struct spdk_nvme_ns *ns = bio->io_path->nvme_ns->ns;
	struct spdk_nvme_qpair *qpair = bio->io_path->ctrlr_ch->qpair;
	uint32_t max_xfer_size = spdk_nvme_ns_get_max_io_xfer_size(ns);
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);

	if (nbytes > max_xfer_size) {
		SPDK_ERRLOG("nbytes is greater than MDTS %" PRIu32 ".\n", max_xfer_size);
		return -EINVAL;
	}

	/*
	 * Each NVMe bdev is a specific namespace, and all NVMe I/O commands require a nsid,
	 * so fill it out automatically.
	 */
	cmd->nsid = spdk_nvme_ns_get_id(ns);

	return spdk_nvme_ctrlr_cmd_io_raw(ctrlr, qpair, cmd, buf,
					  (uint32_t)nbytes, bdev_nvme_queued_done, bio);
}

static int
bdev_nvme_io_passthru_md(struct nvme_bdev_io *bio, struct spdk_nvme_cmd *cmd,
			 void *buf, size_t nbytes, void *md_buf, size_t md_len)
{
	struct spdk_nvme_ns *ns = bio->io_path->nvme_ns->ns;
	struct spdk_nvme_qpair *qpair = bio->io_path->ctrlr_ch->qpair;
	size_t nr_sectors = nbytes / spdk_nvme_ns_get_extended_sector_size(ns);
	uint32_t max_xfer_size = spdk_nvme_ns_get_max_io_xfer_size(ns);
	struct spdk_nvme_ctrlr *ctrlr = spdk_nvme_ns_get_ctrlr(ns);

	if (nbytes > max_xfer_size) {
		SPDK_ERRLOG("nbytes is greater than MDTS %" PRIu32 ".\n", max_xfer_size);
		return -EINVAL;
	}

	if (md_len != nr_sectors * spdk_nvme_ns_get_md_size(ns)) {
		SPDK_ERRLOG("invalid meta data buffer size\n");
		return -EINVAL;
	}

	/*
	 * Each NVMe bdev is a specific namespace, and all NVMe I/O commands require a nsid,
	 * so fill it out automatically.
	 */
	cmd->nsid = spdk_nvme_ns_get_id(ns);

	return spdk_nvme_ctrlr_cmd_io_raw_with_md(ctrlr, qpair, cmd, buf,
			(uint32_t)nbytes, md_buf, bdev_nvme_queued_done, bio);
}

static void
bdev_nvme_abort(struct nvme_bdev_channel *nbdev_ch, struct nvme_bdev_io *bio,
		struct nvme_bdev_io *bio_to_abort)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(bio);
	struct spdk_bdev_io *bdev_io_to_abort;
	struct nvme_io_path *io_path;
	struct nvme_ctrlr *nvme_ctrlr;
	int rc = 0;

	bio->orig_thread = spdk_get_thread();

	/* Traverse the retry_io_list first. */
	TAILQ_FOREACH(bdev_io_to_abort, &nbdev_ch->retry_io_list, module_link) {
		if ((struct nvme_bdev_io *)bdev_io_to_abort->driver_ctx == bio_to_abort) {
			TAILQ_REMOVE(&nbdev_ch->retry_io_list, bdev_io_to_abort, module_link);
			spdk_bdev_io_complete(bdev_io_to_abort, SPDK_BDEV_IO_STATUS_ABORTED);

			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
			return;
		}
	}

	/* Even admin commands, they were submitted to only nvme_ctrlrs which were
	 * on any io_path. So traverse the io_path list for not only I/O commands
	 * but also admin commands.
	 */
	STAILQ_FOREACH(io_path, &nbdev_ch->io_path_list, stailq) {
		nvme_ctrlr = nvme_ctrlr_channel_get_ctrlr(io_path->ctrlr_ch);

		rc = spdk_nvme_ctrlr_cmd_abort_ext(nvme_ctrlr->ctrlr,
						   io_path->ctrlr_ch->qpair,
						   bio_to_abort,
						   bdev_nvme_abort_done, bio);
		if (rc == -ENOENT) {
			/* If no command was found in I/O qpair, the target command may be
			 * admin command.
			 */
			rc = spdk_nvme_ctrlr_cmd_abort_ext(nvme_ctrlr->ctrlr,
							   NULL,
							   bio_to_abort,
							   bdev_nvme_abort_done, bio);
		}

		if (rc != -ENOENT) {
			break;
		}
	}

	if (rc != 0) {
		/* If no command was found or there was any error, complete the abort
		 * request with failure.
		 */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
bdev_nvme_opts_config_json(struct spdk_json_write_ctx *w)
{
	const char	*action;

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
	spdk_json_write_named_uint64(w, "timeout_admin_us", g_opts.timeout_admin_us);
	spdk_json_write_named_uint32(w, "keep_alive_timeout_ms", g_opts.keep_alive_timeout_ms);
	spdk_json_write_named_uint32(w, "transport_retry_count", g_opts.transport_retry_count);
	spdk_json_write_named_uint32(w, "arbitration_burst", g_opts.arbitration_burst);
	spdk_json_write_named_uint32(w, "low_priority_weight", g_opts.low_priority_weight);
	spdk_json_write_named_uint32(w, "medium_priority_weight", g_opts.medium_priority_weight);
	spdk_json_write_named_uint32(w, "high_priority_weight", g_opts.high_priority_weight);
	spdk_json_write_named_uint64(w, "nvme_adminq_poll_period_us", g_opts.nvme_adminq_poll_period_us);
	spdk_json_write_named_uint64(w, "nvme_ioq_poll_period_us", g_opts.nvme_ioq_poll_period_us);
	spdk_json_write_named_uint32(w, "io_queue_requests", g_opts.io_queue_requests);
	spdk_json_write_named_bool(w, "delay_cmd_submit", g_opts.delay_cmd_submit);
	spdk_json_write_named_int32(w, "bdev_retry_count", g_opts.bdev_retry_count);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static void
nvme_ctrlr_config_json(struct spdk_json_write_ctx *w,
		       struct nvme_ctrlr *nvme_ctrlr)
{
	struct spdk_nvme_transport_id	*trid;

	trid = &nvme_ctrlr->active_path_id->trid;

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_nvme_attach_controller");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", nvme_ctrlr->nbdev_ctrlr->name);
	nvme_bdev_dump_trid_json(trid, w);
	spdk_json_write_named_bool(w, "prchk_reftag",
				   (nvme_ctrlr->prchk_flags & SPDK_NVME_IO_FLAGS_PRCHK_REFTAG) != 0);
	spdk_json_write_named_bool(w, "prchk_guard",
				   (nvme_ctrlr->prchk_flags & SPDK_NVME_IO_FLAGS_PRCHK_GUARD) != 0);
	spdk_json_write_named_int32(w, "ctrlr_loss_timeout_sec", nvme_ctrlr->ctrlr_loss_timeout_sec);
	spdk_json_write_named_uint32(w, "reconnect_delay_sec", nvme_ctrlr->reconnect_delay_sec);
	spdk_json_write_named_uint32(w, "fast_io_fail_timeout_sec", nvme_ctrlr->fast_io_fail_timeout_sec);

	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static void
bdev_nvme_hotplug_config_json(struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "bdev_nvme_set_hotplug");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_uint64(w, "period_us", g_nvme_hotplug_poll_period_us);
	spdk_json_write_named_bool(w, "enable", g_nvme_hotplug_enabled);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static int
bdev_nvme_config_json(struct spdk_json_write_ctx *w)
{
	struct nvme_bdev_ctrlr	*nbdev_ctrlr;
	struct nvme_ctrlr	*nvme_ctrlr;

	bdev_nvme_opts_config_json(w);

	pthread_mutex_lock(&g_bdev_nvme_mutex);

	TAILQ_FOREACH(nbdev_ctrlr, &g_nvme_bdev_ctrlrs, tailq) {
		TAILQ_FOREACH(nvme_ctrlr, &nbdev_ctrlr->ctrlrs, tailq) {
			nvme_ctrlr_config_json(w, nvme_ctrlr);
		}
	}

	/* Dump as last parameter to give all NVMe bdevs chance to be constructed
	 * before enabling hotplug poller.
	 */
	bdev_nvme_hotplug_config_json(w);

	pthread_mutex_unlock(&g_bdev_nvme_mutex);
	return 0;
}

struct spdk_nvme_ctrlr *
bdev_nvme_get_ctrlr(struct spdk_bdev *bdev)
{
	struct nvme_bdev *nbdev;
	struct nvme_ns *nvme_ns;

	if (!bdev || bdev->module != &nvme_if) {
		return NULL;
	}

	nbdev = SPDK_CONTAINEROF(bdev, struct nvme_bdev, disk);
	nvme_ns = TAILQ_FIRST(&nbdev->nvme_ns_list);
	assert(nvme_ns != NULL);

	return nvme_ns->ctrlr->ctrlr;
}

SPDK_LOG_REGISTER_COMPONENT(bdev_nvme)
