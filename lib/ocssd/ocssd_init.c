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

#include <spdk/stdinc.h>
#include <spdk/nvme.h>
#include <spdk/io_channel.h>
#include <spdk/bdev_module.h>
#include <spdk_internal/log.h>
#include <spdk/ocssd.h>
#include "ocssd_core.h"
#include "ocssd_anm.h"
#include "ocssd_io.h"
#include "ocssd_reloc.h"
#include "ocssd_rwb.h"
#include "ocssd_band.h"
#include "ocssd_debug.h"
#include "ocssd_nvme.h"

#define OCSSD_CORE_RING_SIZE	4096
#define OCSSD_INIT_TIMEOUT	30

struct ocssd_admin_cmpl {
	struct spdk_nvme_cpl			status;

	int					complete;
};

static STAILQ_HEAD(, ocssd_dev)	g_ocssd_queue = STAILQ_HEAD_INITIALIZER(g_ocssd_queue);
static pthread_mutex_t		g_ocssd_queue_lock = PTHREAD_MUTEX_INITIALIZER;

static void
ocssd_admin_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct ocssd_admin_cmpl *cmpl = ctx;

	cmpl->complete = 1;
	cmpl->status = *cpl;
}

static int
ocssd_band_init_md(struct ocssd_band *band)
{
	struct ocssd_md *md = &band->md;

	md->vld_map = calloc(ocssd_vld_map_size(band->dev), 1);
	if (!md->vld_map) {
		return -ENOMEM;
	}

	pthread_spin_init(&md->lock, PTHREAD_PROCESS_PRIVATE);
	ocssd_band_md_clear(&band->md);
	return 0;
}

static int
ocssd_check_init_opts(const struct ocssd_init_opts *opts,
		      const struct spdk_ocssd_geometry_data *geo)
{
	struct ocssd_dev *dev;
	struct spdk_nvme_transport_id trid;
	size_t num_punits = geo->num_pu * geo->num_grp;
	int rc = 0;

	if (opts->range.begin > opts->range.end || opts->range.end >= num_punits) {
		return -1;
	}

	pthread_mutex_lock(&g_ocssd_queue_lock);

	STAILQ_FOREACH(dev, &g_ocssd_queue, stailq) {
		trid = ocssd_nvme_ctrlr_get_trid(dev->ctrlr);
		if (spdk_nvme_transport_id_compare(&trid, &opts->trid)) {
			continue;
		}

		if (ocssd_range_intersect(opts->range.begin, opts->range.end,
					  dev->range.begin, dev->range.end)) {
			rc = -1;
			goto out;
		}
	}

out:
	pthread_mutex_unlock(&g_ocssd_queue_lock);
	return rc;
}

static int
ocssd_retrieve_bbt_page(struct ocssd_dev *dev, uint64_t offset,
			struct spdk_ocssd_chunk_information_entry *info,
			unsigned int num_entries)
{
	volatile struct ocssd_admin_cmpl cmpl = {};

	assert((long)info % PAGE_SIZE == 0);

	if (ocssd_nvme_get_log_page(dev->ctrlr, SPDK_OCSSD_LOG_CHUNK_INFO,
				    info, num_entries * sizeof(*info),
				    offset * sizeof(*info),
				    ocssd_admin_cb, (void *)&cmpl)) {
		return -1;
	}

	while (!cmpl.complete) {
		usleep(100);
	}

	if (spdk_nvme_cpl_is_error(&cmpl.status)) {
		SPDK_ERRLOG("Unexpected status code: [%d], status code type: [%d]\n",
			    cmpl.status.status.sc, cmpl.status.status.sct);
		return -1;
	}

	return 0;
}

static int
ocssd_retrieve_bbt(struct ocssd_dev *dev, const struct ocssd_punit *punit,
		   struct spdk_ocssd_chunk_information_entry *info)
{
	unsigned int i = 0;
	unsigned int num_entries = PAGE_SIZE / sizeof(*info);
	uint64_t off = (punit->start_ppa.grp * dev->geo.num_pu + punit->start_ppa.pu) *
		       dev->geo.num_chk;

	/* TODO: we should be chunking it by MDTS instead of PAGE_SIZE */
	SPDK_STATIC_ASSERT(PAGE_SIZE % sizeof(*info) == 0, "Invalid chunk info size");

	for (i = 0; i < dev->geo.num_chk; i += num_entries) {
		if (num_entries > dev->geo.num_chk - i) {
			num_entries = dev->geo.num_chk - i;
		}

		if (ocssd_retrieve_bbt_page(dev, off + i, &info[i], num_entries)) {
			return -1;
		}
	}

	return 0;
}

static unsigned char
ocssd_get_chunk_state(const struct spdk_ocssd_chunk_information_entry *info)
{
	if (info->cs.free) {
		return OCSSD_CHUNK_STATE_FREE;
	}
	if (info->cs.open) {
		/* TODO: Add dirty shutdown recovery to return open block state during initialization */
		/* For now we will treat open blocks as bad blocks */
#if defined(SPDK_CONFIG_INTEL_DIRECT_ACCESS_SSD)
		return OCSSD_CHUNK_STATE_BAD;
#else
		/* TODO: Investigate why qemu reports all blocks as open */
		/* For now we will treat open blocks as closed blocks */
		return OCSSD_CHUNK_STATE_CLOSED;
#endif
	}
	if (info->cs.closed) {
		return OCSSD_CHUNK_STATE_CLOSED;
	}
	if (info->cs.offline) {
		return OCSSD_CHUNK_STATE_BAD;
	}
#if defined(SPDK_CONFIG_INTEL_DIRECT_ACCESS_SSD)
	if (info->cs.reserved) {
		return OCSSD_CHUNK_STATE_VACANT;
	}
#endif

	assert(0 && "Invalid block state");
	return OCSSD_CHUNK_STATE_BAD;
}

static void
ocssd_remove_empty_bands(struct ocssd_dev *dev)
{
	struct ocssd_band *band, *temp_band;

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
ocssd_dev_init_bands(struct ocssd_dev *dev)
{
	struct spdk_ocssd_chunk_information_entry	*info;
	struct ocssd_band				*band, *pband;
	struct ocssd_punit				*punit;
	struct ocssd_chunk				*chunk;
	unsigned int					i, j;
	char						buf[128];
	int						rc = 0;

	LIST_INIT(&dev->free_bands);
	LIST_INIT(&dev->shut_bands);

	dev->num_free = 0;
	dev->num_bands = ocssd_dev_num_bands(dev);
	dev->bands = calloc(ocssd_dev_num_bands(dev), sizeof(*dev->bands));
	if (!dev->bands) {
		return -1;
	}

	info = spdk_dma_zmalloc(sizeof(*info) * dev->geo.num_chk, PAGE_SIZE, NULL);
	if (!info) {
		return -1;
	}

	for (i = 0; i < ocssd_dev_num_bands(dev); ++i) {
		band = &dev->bands[i];
		band->id = i;
		band->dev = dev;
		band->state = OCSSD_BAND_STATE_CLOSED;

		if (LIST_EMPTY(&dev->shut_bands)) {
			LIST_INSERT_HEAD(&dev->shut_bands, band, list_entry);
		} else {
			LIST_INSERT_AFTER(pband, band, list_entry);
		}
		pband = band;

		CIRCLEQ_INIT(&band->chunks);
		band->chunk_buf = calloc(ocssd_dev_num_punits(dev), sizeof(*band->chunk_buf));
		if (!band->chunk_buf) {
			SPDK_ERRLOG("Failed to allocate block state table for band: [%u]\n", i);
			rc = -1;
			goto out;
		}

		rc = ocssd_band_init_md(band);
		if (rc) {
			SPDK_ERRLOG("Failed to initialize metadata structures for band [%u]\n", i);
			goto out;
		}
	}

	for (i = 0; i < ocssd_dev_num_punits(dev); ++i) {
		punit = &dev->punits[i];

		rc = ocssd_retrieve_bbt(dev, punit, info);
		if (rc) {
			SPDK_ERRLOG("Failed to retrieve bbt for @ppa: %s [%lu]\n",
				    ocssd_ppa2str(punit->start_ppa, buf, sizeof(buf)),
				    ocssd_ppa_addr_pack(dev, punit->start_ppa));
			goto out;
		}

		for (j = 0; j < ocssd_dev_num_bands(dev); ++j) {
			band = &dev->bands[j];
			chunk = &band->chunk_buf[i];
			chunk->pos = i;
			chunk->state = ocssd_get_chunk_state(&info[j]);
			chunk->punit = punit;
			chunk->start_ppa = punit->start_ppa;
			chunk->start_ppa.chk = band->id;

			if (!ocssd_chunk_is_bad(chunk)) {
				band->num_chunks++;
				CIRCLEQ_INSERT_TAIL(&band->chunks, chunk, circleq);
			}
		}
	}

	ocssd_remove_empty_bands(dev);
out:
	spdk_dma_free(info);
	return rc;
}

static int
ocssd_dev_init_punits(struct ocssd_dev *dev)
{
	unsigned int i, punit;

	dev->punits = calloc(ocssd_dev_num_punits(dev), sizeof(*dev->punits));
	if (!dev->punits) {
		return -1;
	}

	for (i = 0; i < ocssd_dev_num_punits(dev); ++i) {
		dev->punits[i].dev = dev;
		punit = dev->range.begin + i;

		dev->punits[i].start_ppa.ppa = 0;
		dev->punits[i].start_ppa.grp = punit % dev->geo.num_grp;
		dev->punits[i].start_ppa.pu = punit / dev->geo.num_grp;
	}

	return 0;
}

static int
ocssd_dev_retrieve_geo(struct ocssd_dev *dev)
{
	volatile struct ocssd_admin_cmpl cmpl = {};
	struct spdk_ocssd_geometry_data *buf;
	int rc = -1;

	/* The buffer needs to be at least 4K due to some kind of spdk limitation */
	buf = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
	if (!buf) {
		SPDK_ERRLOG("Memory allocation failure\n");
		return -1;
	}

	if (ocssd_nvme_get_geometry(dev->ctrlr, buf, PAGE_SIZE, ocssd_admin_cb, (void *)&cmpl)) {
		SPDK_ERRLOG("Unable to retrieve geometry\n");
		goto out;
	}

	/* TODO: add a timeout */
	while (!cmpl.complete) {
		usleep(100);
	}

	dev->geo = *buf;

	if (spdk_nvme_cpl_is_error(&cmpl.status)) {
		SPDK_ERRLOG("Unexpected status code: [%d], status code type: [%d]\n",
			    cmpl.status.status.sc, cmpl.status.status.sct);
		goto out;
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

	rc = 0;
out:
	free(buf);
	return rc;
}

static int
ocssd_dev_nvme_init(struct ocssd_dev *dev, const struct ocssd_init_opts *opts)
{
	dev->ctrlr = ocssd_nvme_ctrlr_init(opts->ctrlr, &opts->trid);
	if (!dev->ctrlr) {
		return -1;
	}

	dev->md_size = ocssd_nvme_get_md_size(dev->ctrlr);
	if (dev->md_size % sizeof(uint32_t) != 0) {
		/* Metadata pointer must be dword aligned */
		SPDK_ERRLOG("Unsupported metadata size (%zu)\n", dev->md_size);
		return -1;
	}

	return 0;
}

static int
ocssd_conf_validate(const struct ocssd_conf *conf)
{
	size_t i;

	if (conf->defrag.invld_thld >= 100) {
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
	if (conf->rwb_size % OCSSD_BLOCK_SIZE != 0) {
		return -1;
	}

	for (i = 0; i < OCSSD_LIMIT_MAX; ++i) {
		if (conf->defrag.limits[i].limit > 100) {
			return -1;
		}
	}

	return 0;
}

void
ocssd_conf_init_defaults(struct ocssd_conf *conf)
{
	if (!conf) {
		return;
	}

	*conf = (struct ocssd_conf) {
		.defrag = {
			.limits = {
				/* 5 free bands  / 0 % host writes */
				[OCSSD_LIMIT_CRIT]  = { .thld = 5,  .limit = 0 },
				/* 10 free bands / 5 % host writes */
				[OCSSD_LIMIT_HIGH]  = { .thld = 10, .limit = 5 },
				/* 20 free bands / 40 % host writes */
				[OCSSD_LIMIT_LOW]   = { .thld = 20, .limit = 40 },
				/* 40 free bands / 100 % host writes - defrag starts running */
				[OCSSD_LIMIT_START] = { .thld = 40, .limit = 100 },
			},
			/* 10 percent valid lbks */
			.invld_thld = 10,
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
		/* Enable traces */
		.trace = 0,
		/* Default trace path */
		.trace_path = "/var/log/ocssd.log",
	};
}

static int
ocssd_init_wptr_list(struct ocssd_dev *dev)
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

	/* We need to reserve at least 2 buffers for band close / open sequence */
	/* alone, plus additional (8) buffers for handling write errors. */
	dev->lba_pool = spdk_mempool_create(pool_name, 2 + 8,
					    ocssd_num_band_lbks(dev) * sizeof(uint64_t),
					    SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					    SPDK_ENV_SOCKET_ID_ANY);
	if (!dev->lba_pool) {
		return -ENOMEM;
	}

	return 0;
}

static size_t
ocssd_dev_band_max_seq(struct ocssd_dev *dev)
{
	struct ocssd_band *band;
	size_t seq = 0;

	LIST_FOREACH(band, &dev->shut_bands, list_entry) {
		if (band->md.seq > seq) {
			seq = band->md.seq;
		}
	}

	return seq;
}

static void
_ocssd_init_bands_state(void *ctx)
{
	struct ocssd_band *band, *temp_band;
	struct ocssd_dev *dev = ctx;

	dev->seq = ocssd_dev_band_max_seq(dev);

	LIST_FOREACH_SAFE(band, &dev->shut_bands, list_entry, temp_band) {
		if (!band->md.num_vld) {
			ocssd_band_set_state(band, OCSSD_BAND_STATE_FREE);
		}
	}

	ocssd_reloc_resume(dev->reloc);
	/* Clear the limit applications as they're incremented incorrectly by */
	/* the initialization code */
	memset(dev->stats.limits, 0, sizeof(dev->stats.limits));
}

static int
ocssd_init_num_free_bands(struct ocssd_dev *dev)
{
	struct ocssd_band *band;
	int cnt = 0;

	LIST_FOREACH(band, &dev->shut_bands, list_entry) {
		if (band->num_chunks && !band->md.num_vld) {
			cnt++;
		}
	}
	return cnt;
}

static int
ocssd_init_bands_state(struct ocssd_dev *dev)
{
	/* TODO: Should we abort initialization or expose read only device */
	/* if there is no free bands? */
	/* If we abort initialization should we depend on condition that */
	/* we have no free bands or should we have some minimal number of */
	/* free bands? */
	if (!ocssd_init_num_free_bands(dev)) {
		return -1;
	}

	ocssd_thread_send_msg(ocssd_get_core_thread(dev), _ocssd_init_bands_state, dev);
	return 0;
}

static int
ocssd_wait_threads_initialized(struct ocssd_dev *dev)
{
	struct timespec timeout, now;

	if (clock_gettime(CLOCK_MONOTONIC, &timeout)) {
		SPDK_ERRLOG("Unable to retrieve current time\n");
		return -1;
	}

	timeout.tv_sec += OCSSD_INIT_TIMEOUT;

	while (!ocssd_thread_initialized(ocssd_get_core_thread(dev)) ||
	       !ocssd_thread_initialized(ocssd_get_read_thread(dev))) {
		if (clock_gettime(CLOCK_MONOTONIC, &now)) {
			SPDK_ERRLOG("Unable to retrieve current time\n");
			return -1;
		}

		if (now.tv_sec > timeout.tv_sec) {
			SPDK_ERRLOG("Thread initialization timed out\n");
			return -1;
		}

		usleep(100);
	}

	return 0;
}

static int
ocssd_dev_init_io_thread(struct ocssd_dev *dev, struct ocssd_io_thread *io_thread,
			 const char *name, ocssd_thread_fn fn)
{
	io_thread->dev = dev;
	io_thread->thread = ocssd_thread_init(name, OCSSD_CORE_RING_SIZE,
					      fn, dev, 0);
	if (!io_thread->thread) {
		SPDK_ERRLOG("Unable to initialize thread\n");
		return -1;
	}

	io_thread->qpair = ocssd_nvme_alloc_io_qpair(dev->ctrlr, NULL, 0);
	if (!io_thread->qpair) {
		SPDK_ERRLOG("Unable to initialize qpair\n");
		return -1;
	}

	if (ocssd_thread_start(io_thread->thread)) {
		SPDK_ERRLOG("Unable to start core thread\n");
		return -1;
	}

	return 0;
}

static int
ocssd_dev_init_threads(struct ocssd_dev *dev, int read_thread)
{
	if (ocssd_dev_init_io_thread(dev, &dev->thread[OCSSD_THREAD_ID_CORE],
				     "ocssd_core", ocssd_core_thread)) {
		SPDK_ERRLOG("Unable to initialize core thread\n");
		return -1;
	}

	if (!read_thread) {
		dev->thread[OCSSD_THREAD_ID_READ].thread = ocssd_get_core_thread(dev);
		dev->thread[OCSSD_THREAD_ID_READ].qpair = ocssd_get_write_qpair(dev);
	} else {
		if (ocssd_dev_init_io_thread(dev, &dev->thread[OCSSD_THREAD_ID_READ],
					     "ocssd_read", ocssd_read_thread)) {
			SPDK_ERRLOG("Unable to initialize read thread\n");
			return -1;
		}
	}

	if (ocssd_wait_threads_initialized(dev)) {
		SPDK_ERRLOG("Unable to start threads\n");
		return -1;
	}

	return 0;
}

static void
ocssd_dev_free_io_thread(struct ocssd_dev *dev, struct ocssd_io_thread *thread)
{
	ocssd_thread_join(thread->thread);
	ocssd_thread_free(thread->thread);
	ocssd_nvme_free_io_qpair(dev->ctrlr, thread->qpair);
	thread->thread = NULL;
	thread->qpair = NULL;
}

static int
ocssd_dev_l2p_alloc(struct ocssd_dev *dev)
{
	size_t addr_size;
	uint64_t i;

	if (dev->l2p_len == 0) {
		SPDK_DEBUGLOG(SPDK_LOG_OCSSD_INIT, "Invalid l2p table size\n");
		return -1;
	}
	if (dev->l2p) {
		SPDK_DEBUGLOG(SPDK_LOG_OCSSD_INIT, "L2p table already allocated\n");
		return -1;
	}

	addr_size = dev->ppa_len >= 32 ? 8 : 4;
	dev->l2p = malloc(dev->l2p_len * addr_size);
	if (!dev->l2p) {
		SPDK_DEBUGLOG(SPDK_LOG_OCSSD_INIT, "Failed to allocate l2p table\n");
		return -1;
	}

	for (i = 0; i < dev->l2p_len; ++i) {
		ocssd_l2p_set(dev, i, ocssd_to_ppa(OCSSD_PPA_INVALID));
	}

	return 0;
}

static void
ocssd_setup_initial_state(struct ocssd_dev *dev)
{
	struct ocssd_conf *conf = &dev->conf;

	spdk_uuid_generate(&dev->uuid);

	dev->l2p_len = 0;

	for (size_t i = 0; i < ocssd_dev_num_bands(dev); ++i) {
		dev->l2p_len += ocssd_band_num_usable_lbks(&dev->bands[i]);
	}

	dev->l2p_len = (dev->l2p_len * (100 - conf->lba_rsvd)) / 100;
}

static struct ocssd_restore *
ocssd_setup_restore_state(struct ocssd_dev *dev, const struct ocssd_init_opts *opts)
{
	struct ocssd_restore *restore = NULL;
	struct spdk_uuid zero_uuid = { 0 };

	if (spdk_uuid_compare(&opts->uuid, &zero_uuid) == 0) {
		SPDK_ERRLOG("Non-zero UUID required in restore mode\n");
		goto err;
	}
	dev->uuid = opts->uuid;
	restore = ocssd_restore_init(dev);
	if (!restore) {
		SPDK_ERRLOG("Unable to initialize restore structures\n");
		goto err;
	}
	if (ocssd_restore_check_device(dev, restore)) {
		SPDK_ERRLOG("Unable to recover valid ocssd data\n");
		goto err;
	}

	return restore;
err:
	ocssd_restore_free(restore);
	return NULL;
}

struct ocssd_dev *
ocssd_dev_init(const struct ocssd_init_opts *opts)
{
	struct ocssd_dev *dev;
	struct ocssd_restore *restore = NULL;

	if (!opts || !opts->ctrlr) {
		return NULL;
	}

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		return NULL;
	}

	ocssd_conf_init_defaults(&dev->conf);
	if (opts->conf) {
		if (ocssd_conf_validate(opts->conf)) {
			SPDK_ERRLOG("Invalid configuration\n");
			goto err;
		}

		memcpy(&dev->conf, opts->conf, sizeof(dev->conf));
	}

	dev->range = opts->range;
	dev->limit = OCSSD_LIMIT_MAX;
	dev->name = strdup(opts->name);
	if (!dev->name) {
		SPDK_ERRLOG("Unable to set device name\n");
		goto err;
	}

	if (ocssd_dev_nvme_init(dev, opts)) {
		SPDK_ERRLOG("Unable to initialize NVMe structures\n");
		goto err;
	}

	if (ocssd_anm_register_ctrlr(dev->ctrlr)) {
		SPDK_ERRLOG("Unable to register controller to anm thread\n");
		goto err;
	}

	/* In case of errors, we free all of the memory in ocssd_dev_free(), */
	/* so we don't have to clean up in each of the init functions. */
	if (ocssd_dev_retrieve_geo(dev)) {
		SPDK_ERRLOG("Unable to retrieve geometry\n");
		goto err;
	}

	if (ocssd_check_init_opts(opts, &dev->geo)) {
		SPDK_ERRLOG("Invalid device configuration\n");
		goto err;
	}

	if (ocssd_dev_init_punits(dev)) {
		SPDK_ERRLOG("Unable to initialize LUNs\n");
		goto err;
	}

	if (ocssd_init_wptr_list(dev)) {
		SPDK_ERRLOG("Unable to init wptr\n");
		goto err;
	}

	if (ocssd_dev_init_bands(dev)) {
		SPDK_ERRLOG("Unable to initialize band array\n");
		goto err;
	}

	if (dev->conf.trace) {
		dev->stats.trace = ocssd_trace_init(dev->conf.trace_path);
		if (!dev->stats.trace) {
			SPDK_ERRLOG("Unable to initialize trace module\n");
			goto err;
		}
	}

	dev->rwb = ocssd_rwb_init(&dev->conf, dev->geo.ws_opt, dev->md_size);
	if (!dev->rwb) {
		SPDK_ERRLOG("Unable to initialize rwb structures\n");
		goto err;
	}

	dev->reloc = ocssd_reloc_init(dev);
	if (!dev->reloc) {
		SPDK_ERRLOG("Unable to initialize reloc structures\n");
		goto err;
	}

	if (ocssd_dev_init_threads(dev, opts->mode & OCSSD_MODE_READ_ISOLATION)) {
		SPDK_ERRLOG("Unable to initialize device threads\n");
		goto err;
	}

	/* In case of Create just initialize L2P size and allocate L2P later; */
	/* When restoring we want to read enough to verify the data is correct, get L2P size and */
	/* allocate it, then restore the full state (including L2P itself) */
	/* L2P size needs to be saved in case a bad chunk appears - we want to surface a constant */
	/* LBA range */
	if (opts->mode & OCSSD_MODE_CREATE) {
		ocssd_setup_initial_state(dev);
	} else {
		restore = ocssd_setup_restore_state(dev, opts);
		if (!restore) {
			SPDK_ERRLOG("Failed to initialize restore state\n");
			goto err;
		}
	}

	if (ocssd_dev_l2p_alloc(dev)) {
		SPDK_ERRLOG("Unable to init l2p table\n");
		goto err;
	}

	if (!(opts->mode & OCSSD_MODE_CREATE)) {
		if (ocssd_restore_state(dev, restore)) {
			SPDK_ERRLOG("Unable to recover ocssd l2p\n");
			goto err;
		}
	}

	if (ocssd_init_bands_state(dev)) {
		SPDK_ERRLOG("Unable to finish the initialization\n");
		goto err;
	}

	pthread_mutex_lock(&g_ocssd_queue_lock);
	STAILQ_INSERT_HEAD(&g_ocssd_queue, dev, stailq);
	pthread_mutex_unlock(&g_ocssd_queue_lock);

	ocssd_restore_free(restore);
	return dev;
err:
	ocssd_restore_free(restore);
	ocssd_dev_free(dev);
	return NULL;
}

static void
_ocssd_halt_defrag(void *arg)
{
	struct ocssd_dev *dev = arg;
	ocssd_reloc_halt(dev->reloc);
}

static void
ocssd_free_threads(struct ocssd_dev *dev)
{
	struct ocssd_thread *t_core, *t_read;

	t_core = ocssd_get_core_thread(dev);
	t_read = ocssd_get_read_thread(dev);

	/* Read thread is valid if and only if core thread is initialized */
	/* so we can return immediately */
	if (!t_core) {
		assert(t_read == NULL);
		return;
	}

	ocssd_thread_stop(t_core);
	ocssd_thread_stop(t_read);

	/* Make sure both threads are already stopped before freeing them */
	ocssd_thread_join(t_core);

	if (ocssd_get_read_thread(dev) != t_core) {
		ocssd_thread_join(t_read);
		ocssd_dev_free_io_thread(dev, &dev->thread[OCSSD_THREAD_ID_READ]);
	}

	ocssd_dev_free_io_thread(dev, &dev->thread[OCSSD_THREAD_ID_CORE]);
}

void
ocssd_dev_free(struct ocssd_dev *dev)
{
	struct ocssd_dev *iter;

	if (!dev) {
		return;
	}

	pthread_mutex_lock(&g_ocssd_queue_lock);
	STAILQ_FOREACH(iter, &g_ocssd_queue, stailq) {
		if (iter == dev) {
			STAILQ_REMOVE(&g_ocssd_queue, dev, ocssd_dev, stailq);
			break;
		}
	}
	pthread_mutex_unlock(&g_ocssd_queue_lock);

	if (ocssd_get_core_thread(dev)) {
		ocssd_thread_send_msg(ocssd_get_core_thread(dev), _ocssd_halt_defrag, dev);
	}

	ocssd_free_threads(dev);
	ocssd_trace_free(dev->stats.trace);

	/* Keep this after the threads are stopped, to make sure the device is */
	/* unregistered before unregistering the ctrlr. */
	ocssd_anm_unregister_ctrlr(dev->ctrlr);

	assert(LIST_EMPTY(&dev->wptr_list));

	ocssd_dev_dump_bands(dev);
	ocssd_dev_dump_stats(dev);

	if (dev->bands) {
		for (size_t i = 0; i < ocssd_dev_num_bands(dev); ++i) {
			free(dev->bands[i].chunk_buf);
			free(dev->bands[i].md.vld_map);
		}
	}

	spdk_mempool_free(dev->lba_pool);

	ocssd_nvme_ctrlr_free(dev->ctrlr);
	ocssd_rwb_free(dev->rwb);
	ocssd_reloc_free(dev->reloc);

	free(dev->name);
	free(dev->punits);
	free(dev->bands);
	free(dev->l2p);
	free(dev);
}

int
ocssd_init(void)
{
	return ocssd_anm_init();
}

void
ocssd_deinit(void)
{
	ocssd_anm_free();
	ocssd_nvme_unregister_drivers();
}

SPDK_LOG_REGISTER_COMPONENT("ocssd_init", SPDK_LOG_OCSSD_INIT)
