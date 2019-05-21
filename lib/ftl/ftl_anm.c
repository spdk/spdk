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
#include "spdk/nvme_spec.h"
#include "spdk/nvme_ocssd_spec.h"
#include "spdk/thread.h"
#include "spdk/ftl.h"
#include "ftl_anm.h"
#include "ftl_core.h"
#include "ftl_band.h"
#include "ftl_debug.h"

/* Number of log pages read in single get_log_page call */
#define FTL_ANM_LOG_ENTRIES 16

/* Structure aggregating ANM callback registered by ftl device */
struct ftl_anm_poller {
	struct spdk_ftl_dev				*dev;

	ftl_anm_fn					fn;

	LIST_ENTRY(ftl_anm_poller)			list_entry;
};

struct ftl_anm_ctrlr {
	/* NVMe controller */
	struct spdk_nvme_ctrlr				*ctrlr;

	/* NVMe namespace */
	struct spdk_nvme_ns				*ns;

	/* Outstanding ANM event counter */
	int						anm_outstanding;

	/* Indicates if get log page command has been submitted to controller */
	int						processing;

	/* Notification counter */
	uint64_t					nc;

	/* DMA allocated buffer for log pages */
	struct spdk_ocssd_chunk_notification_entry	*log;

	/* Protects ctrlr against process_admin_completions from multiple threads */
	pthread_mutex_t					lock;

	/* List link */
	LIST_ENTRY(ftl_anm_ctrlr)			list_entry;

	/* List of registered pollers */
	LIST_HEAD(, ftl_anm_poller)			pollers;
};

struct ftl_anm {
	struct spdk_thread				*thread;
	struct spdk_poller				*poller;

	pthread_mutex_t					lock;
	/* List of registered controllers */
	LIST_HEAD(, ftl_anm_ctrlr)			ctrlrs;
};

struct ftl_anm_init_ctx {
	spdk_ftl_fn					cb;
	void						*cb_arg;
};

static struct ftl_anm g_anm = { .lock = PTHREAD_MUTEX_INITIALIZER };

static int
ftl_anm_log_range(struct spdk_ocssd_chunk_notification_entry *log)
{
	if (log->mask.lblk) {
		return FTL_ANM_RANGE_LBK;
	}

	if (log->mask.chunk) {
		return FTL_ANM_RANGE_CHK;
	}

	if (log->mask.pu) {
		return FTL_ANM_RANGE_PU;
	}

	assert(0);
	return FTL_ANM_RANGE_MAX;
}

static struct ftl_anm_event *
ftl_anm_event_alloc(struct spdk_ftl_dev *dev, struct ftl_ppa ppa,
		    enum ftl_anm_range range, size_t num_lbks)
{
	struct ftl_anm_event *event;

	event = calloc(1, sizeof(*event));
	if (!event) {
		return NULL;
	}

	event->dev = dev;
	event->ppa = ppa;

	switch (range) {
	case FTL_ANM_RANGE_LBK:
		event->num_lbks = num_lbks;
		break;
	case FTL_ANM_RANGE_CHK:
	case FTL_ANM_RANGE_PU:
		event->num_lbks = ftl_dev_lbks_in_chunk(dev);
		break;
	default:
		assert(false);
	}


	return event;
}

static int
ftl_anm_process_log(struct ftl_anm_poller *poller, struct ftl_anm_ctrlr *ctrlr,
		    struct spdk_ocssd_chunk_notification_entry *log)
{
	struct ftl_anm_event *event;
	struct ftl_ppa ppa = ftl_ppa_addr_unpack(poller->dev, log->lba);
	struct spdk_ftl_dev *dev = poller->dev;
	enum ftl_anm_range range = ftl_anm_log_range(log);
	char buf[128];
	int i, num_bands = 1;

	if (ppa.chk >= ftl_dev_num_bands(poller->dev)) {
		SPDK_ERRLOG("ANM log contains invalid @ppa: %s\n",
			    ftl_ppa2str(ppa, buf, sizeof(buf)));
		return -1;
	}

	/* TODO We need to parse log and decide if action is needed. */
	/* For now we check only if ppa is in device range. */
	if (!ftl_ppa_in_range(poller->dev, ppa)) {
		return -1;
	}

	num_bands = range != FTL_ANM_RANGE_PU ? 1 : ftl_dev_num_bands(dev);

	for (i = 0; i < num_bands; ++i) {
		struct ftl_chunk *chk = ftl_band_chunk_from_ppa(&dev->bands[i], ppa);

		if (chk->state == FTL_CHUNK_STATE_BAD) {
			continue;
		}

		event = ftl_anm_event_alloc(dev, ppa, range, log->nlb);
		if (!event) {
			return -ENOMEM;
		}

		poller->fn(event);
		ppa.chk++;
	}

	return 0;
}

static int
ftl_anm_log_valid(struct ftl_anm_ctrlr *ctrlr, struct spdk_ocssd_chunk_notification_entry *log)
{
	/* Initialize ctrlr->nc during the first log page read */
	if (!ctrlr->nc && log->nc) {
		ctrlr->nc = log->nc - 1;
	}

	if (log->nc > ctrlr->nc) {
		ctrlr->nc = log->nc;
		return 1;
	}

	return 0;
}

static void
ftl_anm_log_page_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct ftl_anm_ctrlr *ctrlr = ctx;
	struct ftl_anm_poller *poller;
	int rc;

	pthread_mutex_lock(&ctrlr->lock);

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_ERRLOG("Unexpected status code: [%d], status code type: [%d]\n",
			    cpl->status.sc, cpl->status.sct);
		goto out;
	}

	for (size_t i = 0; i < FTL_ANM_LOG_ENTRIES; ++i) {
		if (!ftl_anm_log_valid(ctrlr, &ctrlr->log[i])) {
			goto out;
		}

		LIST_FOREACH(poller, &ctrlr->pollers, list_entry) {
			rc = ftl_anm_process_log(poller, ctrlr, &ctrlr->log[i]);
			if (rc == 0 || rc == -ENOMEM) {
				break;
			}
		}
	}

	/* We increment anm_outstanding counter in case there are more logs in controller */
	/* than we get in single log page call */
	ctrlr->anm_outstanding++;
out:
	ctrlr->processing = 0;

	pthread_mutex_unlock(&ctrlr->lock);
}

static void
ftl_anm_aer_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	union spdk_nvme_async_event_completion event = { .raw = cpl->cdw0 };
	struct ftl_anm_ctrlr *ctrlr = ctx;

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_ERRLOG("Unexpected status code: [%d], status code type: [%d]\n",
			    cpl->status.sc, cpl->status.sct);
		return;
	}

	pthread_mutex_lock(&ctrlr->lock);

	if (event.bits.async_event_type == SPDK_NVME_ASYNC_EVENT_TYPE_VENDOR &&
	    event.bits.log_page_identifier == SPDK_OCSSD_LOG_CHUNK_NOTIFICATION) {
		ctrlr->anm_outstanding++;
	}

	pthread_mutex_unlock(&ctrlr->lock);
}

static int
ftl_anm_get_log_page(struct ftl_anm_ctrlr *ctrlr)
{
	uint32_t nsid = spdk_nvme_ns_get_id(ctrlr->ns);

	ctrlr->anm_outstanding = 0;

	if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr->ctrlr, SPDK_OCSSD_LOG_CHUNK_NOTIFICATION, nsid,
					     ctrlr->log, sizeof(*ctrlr->log) * FTL_ANM_LOG_ENTRIES, 0,
					     ftl_anm_log_page_cb, (void *)ctrlr)) {
		return -1;
	}

	ctrlr->processing = 1;
	return 0;
}

static int
ftl_anm_poller_cb(void *ctx)
{
	struct ftl_anm *anm = ctx;
	struct ftl_anm_ctrlr *ctrlr;
	int rc = 0, num_processed = 0;

	pthread_mutex_lock(&anm->lock);
	LIST_FOREACH(ctrlr, &anm->ctrlrs, list_entry) {
		rc = spdk_nvme_ctrlr_process_admin_completions(ctrlr->ctrlr);
		if (rc < 0) {
			SPDK_ERRLOG("Processing admin completions failed\n");
			break;
		}

		num_processed += rc;

		pthread_mutex_lock(&ctrlr->lock);
		if (ctrlr->anm_outstanding && !ctrlr->processing) {
			if (ftl_anm_get_log_page(ctrlr)) {
				SPDK_ERRLOG("Failed to get log page from controller %p",
					    ctrlr->ctrlr);
			}
		}
		pthread_mutex_unlock(&ctrlr->lock);
	}

	pthread_mutex_unlock(&anm->lock);
	return num_processed;
}

static struct ftl_anm_poller *
ftl_anm_alloc_poller(struct spdk_ftl_dev *dev, ftl_anm_fn fn)
{
	struct ftl_anm_poller *poller;

	poller = calloc(1, sizeof(*poller));
	if (!poller) {
		return NULL;
	}

	poller->fn = fn;
	poller->dev = dev;

	return poller;
}

static void
ftl_anm_ctrlr_free(struct ftl_anm_ctrlr *ctrlr)
{
	if (!ctrlr) {
		return;
	}

	/* Unregister ctrlr from aer events */
	spdk_nvme_ctrlr_register_aer_callback(ctrlr->ctrlr, NULL, NULL);

	pthread_mutex_destroy(&ctrlr->lock);
	spdk_dma_free(ctrlr->log);
	free(ctrlr);
}

static struct ftl_anm_ctrlr *
ftl_anm_ctrlr_alloc(struct spdk_ftl_dev *dev)
{
	struct ftl_anm_ctrlr *ctrlr;

	ctrlr = calloc(1, sizeof(*ctrlr));
	if (!ctrlr) {
		return NULL;
	}

	ctrlr->log = spdk_dma_zmalloc(sizeof(*ctrlr->log) * FTL_ANM_LOG_ENTRIES,
				      4096, NULL);
	if (!ctrlr->log) {
		goto free_ctrlr;
	}

	if (pthread_mutex_init(&ctrlr->lock, NULL)) {
		goto free_log;
	}

	/* Set the outstanding counter to force log page retrieval */
	/* to consume events already present on the controller */
	ctrlr->anm_outstanding = 1;
	ctrlr->ctrlr = dev->ctrlr;
	ctrlr->ns = dev->ns;
	LIST_INIT(&ctrlr->pollers);

	spdk_nvme_ctrlr_register_aer_callback(ctrlr->ctrlr, ftl_anm_aer_cb, ctrlr);
	return ctrlr;

free_log:
	free(ctrlr->log);
free_ctrlr:
	free(ctrlr);
	return NULL;
}

static struct ftl_anm_ctrlr *
ftl_anm_find_ctrlr(struct ftl_anm *anm, struct spdk_nvme_ctrlr *ctrlr)
{
	struct ftl_anm_ctrlr *anm_ctrlr;

	LIST_FOREACH(anm_ctrlr, &anm->ctrlrs, list_entry) {
		if (ctrlr == anm_ctrlr->ctrlr) {
			return anm_ctrlr;
		}
	}

	return NULL;
}

void
ftl_anm_event_complete(struct ftl_anm_event *event)
{
	free(event);
}

int
ftl_anm_register_device(struct spdk_ftl_dev *dev, ftl_anm_fn fn)
{
	struct ftl_anm_poller *poller;
	struct ftl_anm_ctrlr *ctrlr;
	int rc = 0;

	pthread_mutex_lock(&g_anm.lock);

	ctrlr = ftl_anm_find_ctrlr(&g_anm, dev->ctrlr);
	if (!ctrlr) {
		ctrlr = ftl_anm_ctrlr_alloc(dev);
		if (!ctrlr) {
			rc = -1;
			goto out;
		}

		LIST_INSERT_HEAD(&g_anm.ctrlrs, ctrlr, list_entry);
	}

	poller = ftl_anm_alloc_poller(dev, fn);
	if (!poller) {
		rc = -1;
		goto out;
	}

	pthread_mutex_lock(&ctrlr->lock);
	LIST_INSERT_HEAD(&ctrlr->pollers, poller, list_entry);
	pthread_mutex_unlock(&ctrlr->lock);
out:
	pthread_mutex_unlock(&g_anm.lock);
	return rc;
}

void
ftl_anm_unregister_device(struct spdk_ftl_dev *dev)
{
	struct ftl_anm_ctrlr *ctrlr;
	struct ftl_anm_poller *poller, *temp_poller;

	pthread_mutex_lock(&g_anm.lock);
	ctrlr = ftl_anm_find_ctrlr(&g_anm, dev->ctrlr);
	assert(ctrlr != NULL);
	pthread_mutex_lock(&ctrlr->lock);

	LIST_FOREACH_SAFE(poller, &ctrlr->pollers, list_entry, temp_poller) {
		if (poller->dev == dev) {
			LIST_REMOVE(poller, list_entry);
			free(poller);
		}
	}

	pthread_mutex_unlock(&ctrlr->lock);

	/* Release the controller if there are no more pollers */
	if (LIST_EMPTY(&ctrlr->pollers)) {
		LIST_REMOVE(ctrlr, list_entry);
		ftl_anm_ctrlr_free(ctrlr);
	}

	pthread_mutex_unlock(&g_anm.lock);
}

static void
ftl_anm_register_poller_cb(void *ctx)
{
	struct ftl_anm_init_ctx *init_ctx = ctx;
	int rc = 0;

	/* TODO: adjust polling timeout */
	g_anm.poller = spdk_poller_register(ftl_anm_poller_cb, &g_anm, 1000);
	if (!g_anm.poller) {
		SPDK_ERRLOG("Unable to register ANM poller\n");
		rc = -ENOMEM;
	}

	init_ctx->cb(init_ctx->cb_arg, rc);
	free(init_ctx);
}

int
ftl_anm_init(struct spdk_thread *thread, spdk_ftl_fn cb, void *cb_arg)
{
	struct ftl_anm_init_ctx *ctx;

	ctx = malloc(sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	g_anm.thread = thread;
	ctx->cb = cb;
	ctx->cb_arg = cb_arg;

	spdk_thread_send_msg(thread, ftl_anm_register_poller_cb, ctx);
	return 0;
}

static void
ftl_anm_unregister_poller_cb(void *ctx)
{
	struct ftl_anm_init_ctx *init_ctx = ctx;

	spdk_poller_unregister(&g_anm.poller);

	init_ctx->cb(init_ctx->cb_arg, 0);
	free(init_ctx);
}

int
ftl_anm_free(spdk_ftl_fn cb, void *cb_arg)
{
	struct ftl_anm_init_ctx *ctx;

	ctx = malloc(sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	ctx->cb = cb;
	ctx->cb_arg = cb_arg;

	spdk_thread_send_msg(g_anm.thread, ftl_anm_unregister_poller_cb, ctx);
	return 0;
}
