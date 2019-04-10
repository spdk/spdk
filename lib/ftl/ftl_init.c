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
#include "spdk/nvme.h"
#include "spdk/io_channel.h"
#include "spdk/bdev_module.h"
#include "spdk_internal/log.h"
#include "spdk/ftl.h"
#include "ftl_core.h"
#include "ftl_anm.h"
#include "ftl_io.h"
#include "ftl_reloc.h"
#include "ftl_rwb.h"
#include "ftl_band.h"
#include "ftl_debug.h"

#define FTL_CORE_RING_SIZE	4096
#define FTL_INIT_TIMEOUT	30
#define FTL_NSID		1

#define ftl_range_intersect(s1, e1, s2, e2) \
	((s1) <= (e2) && (s2) <= (e1))

struct ftl_admin_cmpl {
	struct spdk_nvme_cpl			status;

	int					complete;
};

static STAILQ_HEAD(, spdk_ftl_dev)	g_ftl_queue = STAILQ_HEAD_INITIALIZER(g_ftl_queue);
static pthread_mutex_t		g_ftl_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static const struct spdk_ftl_conf	g_default_conf = {
	.defrag = {
		.limits = {
			/* 5 free bands  / 0 % host writes */
			[SPDK_FTL_LIMIT_CRIT]  = { .thld = 5,  .limit = 0 },
			/* 10 free bands / 5 % host writes */
			[SPDK_FTL_LIMIT_HIGH]  = { .thld = 10, .limit = 5 },
			/* 20 free bands / 40 % host writes */
			[SPDK_FTL_LIMIT_LOW]   = { .thld = 20, .limit = 40 },
			/* 40 free bands / 100 % host writes - defrag starts running */
			[SPDK_FTL_LIMIT_START] = { .thld = 40, .limit = 100 },
		},
		/* 10 percent valid lbks */
		.invalid_thld = 10,
	},
	/* 20% spare lbks */
	.lba_rsvd = 20,
	/* 6M write buffer */
	.rwb_size = 6 * 1024 * 1024,
	/* 90% band fill threshold */
	.band_thld = 90,
	/* Max 32 IO depth per band relocate */
	.max_reloc_qdepth = 32,
	/* Max 3 active band relocates */
	.max_active_relocs = 3,
	/* IO pool size per user thread (this should be adjusted to thread IO qdepth) */
	.user_io_pool_size = 2048,
	/* 3 for 3D TLC NAND and 1 for others */
	.osp_num_pages = 3,
};

static void ftl_dev_free_sync(struct spdk_ftl_dev *dev);

static void
ftl_admin_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct ftl_admin_cmpl *cmpl = ctx;

	cmpl->complete = 1;
	cmpl->status = *cpl;
}

static int
ftl_band_init_md(struct ftl_band *band)
{
	struct ftl_md *md = &band->md;

	md->vld_map = spdk_bit_array_create(ftl_num_band_lbks(band->dev));
	if (!md->vld_map) {
		return -ENOMEM;
	}

	pthread_spin_init(&md->lock, PTHREAD_PROCESS_PRIVATE);
	ftl_band_md_clear(&band->md);
	return 0;
}

static int
ftl_check_init_opts(const struct spdk_ftl_dev_init_opts *opts,
		    const struct spdk_ocssd_geometry_data *geo)
{
	struct spdk_ftl_dev *dev;
	size_t num_punits = geo->num_pu * geo->num_grp;
	int rc = 0;

	if (opts->range.begin > opts->range.end || opts->range.end >= num_punits) {
		return -1;
	}

	pthread_mutex_lock(&g_ftl_queue_lock);

	STAILQ_FOREACH(dev, &g_ftl_queue, stailq) {
		if (spdk_nvme_transport_id_compare(&dev->trid, &opts->trid)) {
			continue;
		}

		if (ftl_range_intersect(opts->range.begin, opts->range.end,
					dev->range.begin, dev->range.end)) {
			rc = -1;
			goto out;
		}
	}

out:
	pthread_mutex_unlock(&g_ftl_queue_lock);
	return rc;
}

static int
ftl_retrieve_bbt_page(struct spdk_ftl_dev *dev, uint64_t offset,
		      struct spdk_ocssd_chunk_information_entry *info,
		      unsigned int num_entries)
{
	volatile struct ftl_admin_cmpl cmpl = {};
	uint32_t nsid = spdk_nvme_ns_get_id(dev->ns);

	if (spdk_nvme_ctrlr_cmd_get_log_page(dev->ctrlr, SPDK_OCSSD_LOG_CHUNK_INFO, nsid,
					     info, num_entries * sizeof(*info),
					     offset * sizeof(*info),
					     ftl_admin_cb, (void *)&cmpl)) {
		return -1;
	}

	while (!cmpl.complete) {
		spdk_nvme_ctrlr_process_admin_completions(dev->ctrlr);
	}

	if (spdk_nvme_cpl_is_error(&cmpl.status)) {
		SPDK_ERRLOG("Unexpected status code: [%d], status code type: [%d]\n",
			    cmpl.status.status.sc, cmpl.status.status.sct);
		return -1;
	}

	return 0;
}

static int
ftl_retrieve_bbt(struct spdk_ftl_dev *dev, const struct ftl_punit *punit,
		 struct spdk_ocssd_chunk_information_entry *info)
{
	uint32_t i = 0;
	unsigned int num_entries = PAGE_SIZE / sizeof(*info);
	uint64_t off = (punit->start_ppa.grp * dev->geo.num_pu + punit->start_ppa.pu) *
		       dev->geo.num_chk;

	for (i = 0; i < dev->geo.num_chk; i += num_entries) {
		if (num_entries > dev->geo.num_chk - i) {
			num_entries = dev->geo.num_chk - i;
		}

		if (ftl_retrieve_bbt_page(dev, off + i, &info[i], num_entries)) {
			return -1;
		}
	}

	return 0;
}

static unsigned char
ftl_get_chunk_state(const struct spdk_ocssd_chunk_information_entry *info)
{
	if (info->cs.free) {
		return FTL_CHUNK_STATE_FREE;
	}

	if (info->cs.open) {
		return FTL_CHUNK_STATE_OPEN;
	}

	if (info->cs.closed) {
		return FTL_CHUNK_STATE_CLOSED;
	}

	if (info->cs.offline) {
		return FTL_CHUNK_STATE_BAD;
	}

	assert(0 && "Invalid block state");
	return FTL_CHUNK_STATE_BAD;
}

static void
ftl_remove_empty_bands(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band, *temp_band;

	/* Remove band from shut_bands list to prevent further processing */
	/* if all blocks on this band are bad */
	LIST_FOREACH_SAFE(band, &dev->shut_bands, list_entry, temp_band) {
		if (!band->num_chunks) {
			dev->num_bands--;
			LIST_REMOVE(band, list_entry);
		}
	}
}

static int
ftl_dev_init_bands(struct spdk_ftl_dev *dev)
{
	struct spdk_ocssd_chunk_information_entry	*info;
	struct ftl_band					*band, *pband;
	struct ftl_punit				*punit;
	struct ftl_chunk				*chunk;
	unsigned int					i, j;
	char						buf[128];
	int						rc = 0;

	LIST_INIT(&dev->free_bands);
	LIST_INIT(&dev->shut_bands);

	dev->num_free = 0;
	dev->num_bands = ftl_dev_num_bands(dev);
	dev->bands = calloc(ftl_dev_num_bands(dev), sizeof(*dev->bands));
	if (!dev->bands) {
		return -1;
	}

	info = calloc(dev->geo.num_chk, sizeof(*info));
	if (!info) {
		return -1;
	}

	for (i = 0; i < ftl_dev_num_bands(dev); ++i) {
		band = &dev->bands[i];
		band->id = i;
		band->dev = dev;
		band->state = FTL_BAND_STATE_CLOSED;

		if (LIST_EMPTY(&dev->shut_bands)) {
			LIST_INSERT_HEAD(&dev->shut_bands, band, list_entry);
		} else {
			LIST_INSERT_AFTER(pband, band, list_entry);
		}
		pband = band;

		CIRCLEQ_INIT(&band->chunks);
		band->chunk_buf = calloc(ftl_dev_num_punits(dev), sizeof(*band->chunk_buf));
		if (!band->chunk_buf) {
			SPDK_ERRLOG("Failed to allocate block state table for band: [%u]\n", i);
			rc = -1;
			goto out;
		}

		rc = ftl_band_init_md(band);
		if (rc) {
			SPDK_ERRLOG("Failed to initialize metadata structures for band [%u]\n", i);
			goto out;
		}
	}

	for (i = 0; i < ftl_dev_num_punits(dev); ++i) {
		punit = &dev->punits[i];

		rc = ftl_retrieve_bbt(dev, punit, info);
		if (rc) {
			SPDK_ERRLOG("Failed to retrieve bbt for @ppa: %s [%lu]\n",
				    ftl_ppa2str(punit->start_ppa, buf, sizeof(buf)),
				    ftl_ppa_addr_pack(dev, punit->start_ppa));
			goto out;
		}

		for (j = 0; j < ftl_dev_num_bands(dev); ++j) {
			band = &dev->bands[j];
			chunk = &band->chunk_buf[i];
			chunk->pos = i;
			chunk->state = ftl_get_chunk_state(&info[j]);
			chunk->punit = punit;
			chunk->start_ppa = punit->start_ppa;
			chunk->start_ppa.chk = band->id;

			if (chunk->state != FTL_CHUNK_STATE_BAD) {
				band->num_chunks++;
				CIRCLEQ_INSERT_TAIL(&band->chunks, chunk, circleq);
			}
		}
	}

	ftl_remove_empty_bands(dev);
out:
	free(info);
	return rc;
}

static int
ftl_dev_init_punits(struct spdk_ftl_dev *dev)
{
	unsigned int i, punit;

	dev->punits = calloc(ftl_dev_num_punits(dev), sizeof(*dev->punits));
	if (!dev->punits) {
		return -1;
	}

	for (i = 0; i < ftl_dev_num_punits(dev); ++i) {
		dev->punits[i].dev = dev;
		punit = dev->range.begin + i;

		dev->punits[i].start_ppa.ppa = 0;
		dev->punits[i].start_ppa.grp = punit % dev->geo.num_grp;
		dev->punits[i].start_ppa.pu = punit / dev->geo.num_grp;
	}

	return 0;
}

static int
ftl_dev_retrieve_geo(struct spdk_ftl_dev *dev)
{
	volatile struct ftl_admin_cmpl cmpl = {};
	uint32_t nsid = spdk_nvme_ns_get_id(dev->ns);

	if (spdk_nvme_ocssd_ctrlr_cmd_geometry(dev->ctrlr, nsid, &dev->geo, sizeof(dev->geo),
					       ftl_admin_cb, (void *)&cmpl)) {
		SPDK_ERRLOG("Unable to retrieve geometry\n");
		return -1;
	}

	/* TODO: add a timeout */
	while (!cmpl.complete) {
		spdk_nvme_ctrlr_process_admin_completions(dev->ctrlr);
	}

	if (spdk_nvme_cpl_is_error(&cmpl.status)) {
		SPDK_ERRLOG("Unexpected status code: [%d], status code type: [%d]\n",
			    cmpl.status.status.sc, cmpl.status.status.sct);
		return -1;
	}

	/* TODO: add sanity checks for the geo */
	dev->ppa_len = dev->geo.lbaf.grp_len +
		       dev->geo.lbaf.pu_len +
		       dev->geo.lbaf.chk_len +
		       dev->geo.lbaf.lbk_len;

	dev->ppaf.lbk_offset = 0;
	dev->ppaf.lbk_mask   = (1 << dev->geo.lbaf.lbk_len) - 1;
	dev->ppaf.chk_offset = dev->ppaf.lbk_offset + dev->geo.lbaf.lbk_len;
	dev->ppaf.chk_mask   = (1 << dev->geo.lbaf.chk_len) - 1;
	dev->ppaf.pu_offset  = dev->ppaf.chk_offset + dev->geo.lbaf.chk_len;
	dev->ppaf.pu_mask    = (1 << dev->geo.lbaf.pu_len) - 1;
	dev->ppaf.grp_offset = dev->ppaf.pu_offset + dev->geo.lbaf.pu_len;
	dev->ppaf.grp_mask   = (1 << dev->geo.lbaf.grp_len) - 1;

	/* We're using optimal write size as our xfer size */
	dev->xfer_size = dev->geo.ws_opt;

	return 0;
}

static int
ftl_dev_nvme_init(struct spdk_ftl_dev *dev, const struct spdk_ftl_dev_init_opts *opts)
{
	uint32_t block_size;

	dev->ctrlr = opts->ctrlr;

	if (spdk_nvme_ctrlr_get_num_ns(dev->ctrlr) != 1) {
		SPDK_ERRLOG("Unsupported number of namespaces\n");
		return -1;
	}

	dev->ns = spdk_nvme_ctrlr_get_ns(dev->ctrlr, FTL_NSID);
	dev->trid = opts->trid;
	dev->md_size = spdk_nvme_ns_get_md_size(dev->ns);

	block_size = spdk_nvme_ns_get_extended_sector_size(dev->ns);
	if (block_size != FTL_BLOCK_SIZE) {
		SPDK_ERRLOG("Unsupported block size (%"PRIu32")\n", block_size);
		return -1;
	}

	if (dev->md_size % sizeof(uint32_t) != 0) {
		/* Metadata pointer must be dword aligned */
		SPDK_ERRLOG("Unsupported metadata size (%zu)\n", dev->md_size);
		return -1;
	}

	return 0;
}

static int
ftl_conf_validate(const struct spdk_ftl_conf *conf)
{
	size_t i;

	if (conf->defrag.invalid_thld >= 100) {
		return -1;
	}
	if (conf->lba_rsvd >= 100) {
		return -1;
	}
	if (conf->lba_rsvd == 0) {
		return -1;
	}
	if (conf->rwb_size == 0) {
		return -1;
	}
	if (conf->rwb_size % FTL_BLOCK_SIZE != 0) {
		return -1;
	}

	for (i = 0; i < SPDK_FTL_LIMIT_MAX; ++i) {
		if (conf->defrag.limits[i].limit > 100) {
			return -1;
		}
	}

	return 0;
}

void
spdk_ftl_conf_init_defaults(struct spdk_ftl_conf *conf)
{
	*conf = g_default_conf;
}

static int
ftl_init_wptr_list(struct spdk_ftl_dev *dev)
{
#define POOL_NAME_LEN 128
	char pool_name[POOL_NAME_LEN];
	int rc;

	LIST_INIT(&dev->wptr_list);
	LIST_INIT(&dev->flush_list);

	rc = snprintf(pool_name, sizeof(pool_name), "%s-%s", dev->name, "ocssd-lba-pool");
	if (rc < 0 || rc >= POOL_NAME_LEN) {
		return -ENAMETOOLONG;
	}

	/* We need to reserve at least 2 buffers for band close / open sequence
	 * alone, plus additional (8) buffers for handling write errors.
	 * TODO: This memory pool is utilized only by core thread - it introduce
	 * unnecessary overhead and should be replaced by different data structure.
	 */
	dev->lba_pool = spdk_mempool_create(pool_name, 2 + 8,
					    ftl_num_band_lbks(dev) * sizeof(uint64_t),
					    SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					    SPDK_ENV_SOCKET_ID_ANY);
	if (!dev->lba_pool) {
		return -ENOMEM;
	}

	return 0;
}

static size_t
ftl_dev_band_max_seq(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;
	size_t seq = 0;

	LIST_FOREACH(band, &dev->shut_bands, list_entry) {
		if (band->md.seq > seq) {
			seq = band->md.seq;
		}
	}

	return seq;
}

static void
_ftl_init_bands_state(void *ctx)
{
	struct ftl_band *band, *temp_band;
	struct spdk_ftl_dev *dev = ctx;

	dev->seq = ftl_dev_band_max_seq(dev);

	LIST_FOREACH_SAFE(band, &dev->shut_bands, list_entry, temp_band) {
		if (!band->md.num_vld) {
			ftl_band_set_state(band, FTL_BAND_STATE_FREE);
		}
	}

	ftl_reloc_resume(dev->reloc);
	/* Clear the limit applications as they're incremented incorrectly by */
	/* the initialization code */
	memset(dev->stats.limits, 0, sizeof(dev->stats.limits));
}

static int
ftl_init_num_free_bands(struct spdk_ftl_dev *dev)
{
	struct ftl_band *band;
	int cnt = 0;

	LIST_FOREACH(band, &dev->shut_bands, list_entry) {
		if (band->num_chunks && !band->md.num_vld) {
			cnt++;
		}
	}
	return cnt;
}

static int
ftl_init_bands_state(struct spdk_ftl_dev *dev)
{
	/* TODO: Should we abort initialization or expose read only device */
	/* if there is no free bands? */
	/* If we abort initialization should we depend on condition that */
	/* we have no free bands or should we have some minimal number of */
	/* free bands? */
	if (!ftl_init_num_free_bands(dev)) {
		return -1;
	}

	spdk_thread_send_msg(ftl_get_core_thread(dev), _ftl_init_bands_state, dev);
	return 0;
}

static void
_ftl_dev_init_thread(void *ctx)
{
	struct ftl_thread *thread = ctx;
	struct spdk_ftl_dev *dev = thread->dev;

	thread->poller = spdk_poller_register(thread->poller_fn, thread, thread->period_us);
	if (!thread->poller) {
		SPDK_ERRLOG("Unable to register poller\n");
		assert(0);
	}

	if (spdk_get_thread() == ftl_get_core_thread(dev)) {
		ftl_anm_register_device(dev, ftl_process_anm_event);
	}
}

static int
ftl_dev_init_thread(struct spdk_ftl_dev *dev, struct ftl_thread *thread,
		    struct spdk_thread *spdk_thread, spdk_poller_fn fn, uint64_t period_us)
{
	thread->dev = dev;
	thread->poller_fn = fn;
	thread->thread = spdk_thread;
	thread->period_us = period_us;

	thread->qpair = spdk_nvme_ctrlr_alloc_io_qpair(dev->ctrlr, NULL, 0);
	if (!thread->qpair) {
		SPDK_ERRLOG("Unable to initialize qpair\n");
		return -1;
	}

	spdk_thread_send_msg(spdk_thread, _ftl_dev_init_thread, thread);
	return 0;
}

static int
ftl_dev_init_threads(struct spdk_ftl_dev *dev, const struct spdk_ftl_dev_init_opts *opts)
{
	if (!opts->core_thread || !opts->read_thread) {
		return -1;
	}

	if (ftl_dev_init_thread(dev, &dev->core_thread, opts->core_thread, ftl_task_core, 0)) {
		SPDK_ERRLOG("Unable to initialize core thread\n");
		return -1;
	}

	if (ftl_dev_init_thread(dev, &dev->read_thread, opts->read_thread, ftl_task_read, 0)) {
		SPDK_ERRLOG("Unable to initialize read thread\n");
		return -1;
	}

	return 0;
}

static void
ftl_dev_free_thread(struct spdk_ftl_dev *dev, struct ftl_thread *thread)
{
	assert(thread->poller == NULL);

	spdk_nvme_ctrlr_free_io_qpair(thread->qpair);
	thread->thread = NULL;
	thread->qpair = NULL;
}

static int
ftl_dev_l2p_alloc(struct spdk_ftl_dev *dev)
{
	size_t addr_size;
	uint64_t i;

	if (dev->num_lbas == 0) {
		SPDK_DEBUGLOG(SPDK_LOG_FTL_INIT, "Invalid l2p table size\n");
		return -1;
	}

	if (dev->l2p) {
		SPDK_DEBUGLOG(SPDK_LOG_FTL_INIT, "L2p table already allocated\n");
		return -1;
	}

	addr_size = dev->ppa_len >= 32 ? 8 : 4;
	dev->l2p = malloc(dev->num_lbas * addr_size);
	if (!dev->l2p) {
		SPDK_DEBUGLOG(SPDK_LOG_FTL_INIT, "Failed to allocate l2p table\n");
		return -1;
	}

	for (i = 0; i < dev->num_lbas; ++i) {
		ftl_l2p_set(dev, i, ftl_to_ppa(FTL_PPA_INVALID));
	}

	return 0;
}

static void
ftl_init_complete(struct spdk_ftl_dev *dev)
{
	pthread_mutex_lock(&g_ftl_queue_lock);
	STAILQ_INSERT_HEAD(&g_ftl_queue, dev, stailq);
	pthread_mutex_unlock(&g_ftl_queue_lock);

	dev->initialized = 1;

	if (dev->init_cb) {
		dev->init_cb(dev, dev->init_arg, 0);
	}

	dev->init_cb = NULL;
	dev->init_arg = NULL;
}

static int
ftl_setup_initial_state(struct spdk_ftl_dev *dev)
{
	struct spdk_ftl_conf *conf = &dev->conf;
	size_t i;

	spdk_uuid_generate(&dev->uuid);

	dev->num_lbas = 0;
	for (i = 0; i < ftl_dev_num_bands(dev); ++i) {
		dev->num_lbas += ftl_band_num_usable_lbks(&dev->bands[i]);
	}

	dev->num_lbas = (dev->num_lbas * (100 - conf->lba_rsvd)) / 100;

	if (ftl_dev_l2p_alloc(dev)) {
		SPDK_ERRLOG("Unable to init l2p table\n");
		return -1;
	}

	if (ftl_init_bands_state(dev)) {
		SPDK_ERRLOG("Unable to finish the initialization\n");
		return -1;
	}

	ftl_init_complete(dev);
	return 0;
}

struct ftl_init_fail_ctx {
	spdk_ftl_init_fn	cb;
	void			*arg;
};

static void
ftl_init_fail_cb(void *ctx, int status)
{
	struct ftl_init_fail_ctx *fail_cb = ctx;

	fail_cb->cb(NULL, fail_cb->arg, -ENODEV);
	free(fail_cb);
}

static void
ftl_init_fail(struct spdk_ftl_dev *dev)
{
	struct ftl_init_fail_ctx *fail_cb;

	fail_cb = malloc(sizeof(*fail_cb));
	if (!fail_cb) {
		SPDK_ERRLOG("Unable to allocate context to free the device\n");
		return;
	}

	fail_cb->cb = dev->init_cb;
	fail_cb->arg = dev->init_arg;
	dev->halt_cb = NULL;

	if (spdk_ftl_dev_free(dev, ftl_init_fail_cb, fail_cb)) {
		SPDK_ERRLOG("Unable to free the device\n");
		assert(0);
	}
}

static void
ftl_restore_device_cb(struct spdk_ftl_dev *dev, struct ftl_restore *restore, int status)
{
	if (status) {
		SPDK_ERRLOG("Failed to restore the device from the SSD\n");
		goto error;
	}

	if (ftl_init_bands_state(dev)) {
		SPDK_ERRLOG("Unable to finish the initialization\n");
		goto error;
	}

	ftl_init_complete(dev);
	return;
error:
	ftl_init_fail(dev);
}

static void
ftl_restore_md_cb(struct spdk_ftl_dev *dev, struct ftl_restore *restore, int status)
{
	if (status) {
		SPDK_ERRLOG("Failed to restore the metadata from the SSD\n");
		goto error;
	}

	/* After the metadata is read it should be possible to allocate the L2P */
	if (ftl_dev_l2p_alloc(dev)) {
		SPDK_ERRLOG("Failed to allocate the L2P\n");
		goto error;
	}

	if (ftl_restore_device(restore, ftl_restore_device_cb)) {
		SPDK_ERRLOG("Failed to start device restoration from the SSD\n");
		goto error;
	}

	return;
error:
	ftl_init_fail(dev);
}

static int
ftl_restore_state(struct spdk_ftl_dev *dev, const struct spdk_ftl_dev_init_opts *opts)
{
	dev->uuid = opts->uuid;

	if (ftl_restore_md(dev, ftl_restore_md_cb)) {
		SPDK_ERRLOG("Failed to start metadata restoration from the SSD\n");
		return -1;
	}

	return 0;
}

static int
ftl_io_channel_create_cb(void *io_device, void *ctx)
{
	struct ftl_io_channel *ch = ctx;
	char mempool_name[32];
	struct spdk_ftl_dev *dev = io_device;

	snprintf(mempool_name, sizeof(mempool_name), "ftl_io_%p", ch);
	ch->elem_size = sizeof(struct ftl_md_io);
	ch->io_pool = spdk_mempool_create(mempool_name,
					  dev->conf.user_io_pool_size,
					  ch->elem_size,
					  0,
					  SPDK_ENV_SOCKET_ID_ANY);

	if (!ch->io_pool) {
		return -1;
	}

	return 0;
}

static void
ftl_io_channel_destroy_cb(void *io_device, void *ctx)
{
	struct ftl_io_channel *ch = ctx;

	spdk_mempool_free(ch->io_pool);
}

int
spdk_ftl_dev_init(const struct spdk_ftl_dev_init_opts *opts, spdk_ftl_init_fn cb, void *cb_arg)
{
	struct spdk_ftl_dev *dev;

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		return -ENOMEM;
	}

	if (opts->conf) {
		if (ftl_conf_validate(opts->conf)) {
			SPDK_ERRLOG("Invalid configuration\n");
			goto fail_sync;
		}

		memcpy(&dev->conf, opts->conf, sizeof(dev->conf));
	} else {
		spdk_ftl_conf_init_defaults(&dev->conf);
	}

	spdk_io_device_register(dev, ftl_io_channel_create_cb, ftl_io_channel_destroy_cb,
				sizeof(struct ftl_io_channel),
				NULL);

	TAILQ_INIT(&dev->retry_queue);
	dev->ioch = spdk_get_io_channel(dev);
	dev->init_cb = cb;
	dev->init_arg = cb_arg;
	dev->range = opts->range;
	dev->limit = SPDK_FTL_LIMIT_MAX;
	dev->cache_bdev_desc = opts->cache_bdev_desc;

	dev->name = strdup(opts->name);
	if (!dev->name) {
		SPDK_ERRLOG("Unable to set device name\n");
		goto fail_sync;
	}

	if (ftl_dev_nvme_init(dev, opts)) {
		SPDK_ERRLOG("Unable to initialize NVMe structures\n");
		goto fail_sync;
	}

	/* In case of errors, we free all of the memory in ftl_dev_free_sync(), */
	/* so we don't have to clean up in each of the init functions. */
	if (ftl_dev_retrieve_geo(dev)) {
		SPDK_ERRLOG("Unable to retrieve geometry\n");
		goto fail_sync;
	}

	if (ftl_check_init_opts(opts, &dev->geo)) {
		SPDK_ERRLOG("Invalid device configuration\n");
		goto fail_sync;
	}

	if (ftl_dev_init_punits(dev)) {
		SPDK_ERRLOG("Unable to initialize LUNs\n");
		goto fail_sync;
	}

	if (ftl_init_wptr_list(dev)) {
		SPDK_ERRLOG("Unable to init wptr\n");
		goto fail_sync;
	}

	if (ftl_dev_init_bands(dev)) {
		SPDK_ERRLOG("Unable to initialize band array\n");
		goto fail_sync;
	}

	dev->rwb = ftl_rwb_init(&dev->conf, dev->geo.ws_opt, dev->md_size, ftl_dev_num_punits(dev));
	if (!dev->rwb) {
		SPDK_ERRLOG("Unable to initialize rwb structures\n");
		goto fail_sync;
	}

	dev->reloc = ftl_reloc_init(dev);
	if (!dev->reloc) {
		SPDK_ERRLOG("Unable to initialize reloc structures\n");
		goto fail_sync;
	}

	if (ftl_dev_init_threads(dev, opts)) {
		SPDK_ERRLOG("Unable to initialize device threads\n");
		goto fail_sync;
	}

	if (opts->mode & SPDK_FTL_MODE_CREATE) {
		if (ftl_setup_initial_state(dev)) {
			SPDK_ERRLOG("Failed to setup initial state of the device\n");
			goto fail_async;
		}

	} else {
		if (ftl_restore_state(dev, opts)) {
			SPDK_ERRLOG("Unable to restore device's state from the SSD\n");
			goto fail_async;
		}
	}

	return 0;
fail_sync:
	ftl_dev_free_sync(dev);
	return -ENOMEM;
fail_async:
	ftl_init_fail(dev);
	return 0;
}

static void
_ftl_halt_defrag(void *arg)
{
	ftl_reloc_halt(((struct spdk_ftl_dev *)arg)->reloc);
}

static void
ftl_dev_free_sync(struct spdk_ftl_dev *dev)
{
	struct spdk_ftl_dev *iter;
	size_t i;

	if (!dev) {
		return;
	}

	pthread_mutex_lock(&g_ftl_queue_lock);
	STAILQ_FOREACH(iter, &g_ftl_queue, stailq) {
		if (iter == dev) {
			STAILQ_REMOVE(&g_ftl_queue, dev, spdk_ftl_dev, stailq);
			break;
		}
	}
	pthread_mutex_unlock(&g_ftl_queue_lock);

	assert(LIST_EMPTY(&dev->wptr_list));

	ftl_dev_dump_bands(dev);
	ftl_dev_dump_stats(dev);

	spdk_put_io_channel(dev->ioch);
	spdk_io_device_unregister(dev, NULL);

	if (dev->bands) {
		for (i = 0; i < ftl_dev_num_bands(dev); ++i) {
			free(dev->bands[i].chunk_buf);
			spdk_bit_array_free(&dev->bands[i].md.vld_map);
		}
	}

	spdk_mempool_free(dev->lba_pool);

	ftl_rwb_free(dev->rwb);
	ftl_reloc_free(dev->reloc);

	free(dev->name);
	free(dev->punits);
	free(dev->bands);
	free(dev->l2p);
	free(dev);
}

static int
ftl_halt_poller(void *ctx)
{
	struct spdk_ftl_dev *dev = ctx;
	spdk_ftl_fn halt_cb = dev->halt_cb;
	void *halt_arg = dev->halt_arg;

	if (!dev->core_thread.poller && !dev->read_thread.poller) {
		spdk_poller_unregister(&dev->halt_poller);

		ftl_dev_free_thread(dev, &dev->read_thread);
		ftl_dev_free_thread(dev, &dev->core_thread);

		ftl_anm_unregister_device(dev);
		ftl_dev_free_sync(dev);

		if (halt_cb) {
			halt_cb(halt_arg, 0);
		}
	}

	return 0;
}

static void
ftl_add_halt_poller(void *ctx)
{
	struct spdk_ftl_dev *dev = ctx;

	_ftl_halt_defrag(dev);

	assert(!dev->halt_poller);
	dev->halt_poller = spdk_poller_register(ftl_halt_poller, dev, 100);
}

int
spdk_ftl_dev_free(struct spdk_ftl_dev *dev, spdk_ftl_fn cb, void *cb_arg)
{
	if (dev->halt_cb) {
		return -EBUSY;
	}

	dev->halt_cb = cb;
	dev->halt_arg = cb_arg;
	dev->halt = 1;

	spdk_thread_send_msg(ftl_get_core_thread(dev), ftl_add_halt_poller, dev);
	return 0;
}

int
spdk_ftl_module_init(const struct ftl_module_init_opts *opts, spdk_ftl_fn cb, void *cb_arg)
{
	return ftl_anm_init(opts->anm_thread, cb, cb_arg);
}

int
spdk_ftl_module_fini(spdk_ftl_fn cb, void *cb_arg)
{
	return ftl_anm_free(cb, cb_arg);
}

SPDK_LOG_REGISTER_COMPONENT("ftl_init", SPDK_LOG_FTL_INIT)
