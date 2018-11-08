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
#include <spdk/nvme_spec.h>
#include <spdk/nvme_ocssd_spec.h>
#include <spdk/thread.h>
#include <spdk/ftl.h>
#include "ftl_anm.h"
#include "ftl_utils.h"
#include "ftl_core.h"
#include "ftl_nvme.h"

/* Number of log pages read in single get_log_page call */
#define FTL_ANM_LOG_ENTRIES 16

/* Structure aggregating ANM callback registered by ftl device */
struct ftl_anm_poller {
	struct ftl_dev				*dev;

	ftl_anm_fn					fn;

	LIST_ENTRY(ftl_anm_poller)			list_entry;
};

struct ftl_anm_ctrlr {
	/* NVMe controller */
	struct ftl_nvme_ctrlr				*ctrlr;

	/* Outstanding ANM event counter */
	volatile int					anm_outstanding;

	/* Indicates if get log page command has been submitted to controller */
	volatile int					processing;

	/* Notification counter */
	uint64_t					nc;

	/* DMA allocated buffer for log pages */
	struct spdk_ocssd_chunk_notification_entry	*log;

	/* List link */
	LIST_ENTRY(ftl_anm_ctrlr)			list_entry;

	/* List of registered pollers */
	LIST_HEAD(, ftl_anm_poller)			pollers;
};

struct ftl_anm {
	/* Thread descriptor */
	struct ftl_thread			*thread;

	pthread_mutex_t				lock;

	/* List of registered controllers */
	LIST_HEAD(, ftl_anm_ctrlr)		ctrlrs;
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
ftl_anm_event_alloc(struct ftl_dev *dev, struct ftl_ppa ppa, enum ftl_anm_range range)
{
	struct ftl_anm_event *event;

	event = calloc(1, sizeof(*event));
	if (!event) {
		return NULL;
	}

	event->dev = dev;
	event->ppa = ppa;
	event->range = range;

	return event;
}

static int
ftl_anm_process_log(struct ftl_anm_poller *poller, struct ftl_anm_ctrlr *ctrlr,
		    struct spdk_ocssd_chunk_notification_entry *log)
{
	struct ftl_anm_event *event;
	struct ftl_ppa ppa = ftl_ppa_addr_unpack(poller->dev, log->lba);

	/* TODO We need to parse log and decide if action is needed. */
	/* For now we check only if ppa is in device range. */
	if (!ftl_ppa_in_range(poller->dev, ppa)) {
		return -1;
	}

	event = ftl_anm_event_alloc(poller->dev, ppa, ftl_anm_log_range(log));
	poller->fn(event);

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

	ctrlr->processing = 0;

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_ERRLOG("Unexpected status code: [%d], status code type: [%d]\n",
			    cpl->status.sc, cpl->status.sct);
		return;
	}

	for (size_t i = 0; i < FTL_ANM_LOG_ENTRIES; ++i) {
		if (!ftl_anm_log_valid(ctrlr, &ctrlr->log[i])) {
			return;
		}

		LIST_FOREACH(poller, &ctrlr->pollers, list_entry) {
			if (!ftl_anm_process_log(poller, ctrlr, &ctrlr->log[i])) {
				break;
			}
		}
	}

	/* We increment anm_outstanding counter in case there are more logs in controller */
	/* than we get in single log page call */
	ctrlr->anm_outstanding++;
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

	if (event.bits.async_event_type == SPDK_NVME_ASYNC_EVENT_TYPE_VENDOR &&
	    event.bits.log_page_identifier == SPDK_OCSSD_LOG_CHUNK_NOTIFICATION) {
		ctrlr->anm_outstanding++;
	}
}

static int
ftl_anm_get_log_page(struct ftl_anm_ctrlr *ctrlr)
{
	ctrlr->anm_outstanding = 0;

	if (ftl_nvme_get_log_page(ctrlr->ctrlr, SPDK_OCSSD_LOG_CHUNK_NOTIFICATION,
				  ctrlr->log, sizeof(*ctrlr->log) * FTL_ANM_LOG_ENTRIES, 0,
				  ftl_anm_log_page_cb, (void *)ctrlr)) {
		return -1;
	}

	ctrlr->processing = 1;

	return 0;
}

static void
ftl_anm_thread(void *ctx)
{
	struct ftl_anm *anm = ctx;
	struct ftl_anm_ctrlr *ctrlr;

	while (anm->thread->running) {
		pthread_mutex_lock(&anm->lock);
		LIST_FOREACH(ctrlr, &anm->ctrlrs, list_entry) {
			ftl_nvme_process_admin_completions(ctrlr->ctrlr);

			if (ctrlr->anm_outstanding && !ctrlr->processing) {
				if (ftl_anm_get_log_page(ctrlr)) {
					SPDK_ERRLOG("Failed to get log page from controller %p",
						    ctrlr->ctrlr);
				}
			}
		}

		pthread_mutex_unlock(&anm->lock);

		/* TODO this value need to be adjusted and should be configurable */
		usleep(100);
	}
}

static struct ftl_anm_poller *
ftl_anm_alloc_poller(ftl_anm_fn fn, struct ftl_dev *dev)
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
	ftl_nvme_register_aer_callback(ctrlr->ctrlr, NULL, NULL);

	LIST_REMOVE(ctrlr, list_entry);

	spdk_dma_free(ctrlr->log);
	free(ctrlr);
}

static struct ftl_anm_ctrlr *
ftl_anm_ctrlr_alloc(struct ftl_nvme_ctrlr *nvme_ctrlr)
{
	struct ftl_anm_ctrlr *ctrlr;

	ctrlr = calloc(1, sizeof(*ctrlr));
	if (!ctrlr) {
		return NULL;
	}

	ctrlr->log = spdk_dma_zmalloc(sizeof(*ctrlr->log) * FTL_ANM_LOG_ENTRIES,
				      PAGE_SIZE, NULL);
	if (!ctrlr->log) {
		free(ctrlr);
		return NULL;
	}


	/* Set the outstanding counter to force log page retrieval */
	/* to consume events already present on the controller */
	ctrlr->anm_outstanding = 1;
	ctrlr->ctrlr = nvme_ctrlr;
	LIST_INIT(&ctrlr->pollers);

	ftl_nvme_register_aer_callback(ctrlr->ctrlr, ftl_anm_aer_cb, ctrlr);
	return ctrlr;
}

static struct ftl_anm_ctrlr *
ftl_anm_find_ctrlr(struct ftl_anm *anm, struct ftl_nvme_ctrlr *ctrlr)
{
	struct ftl_anm_ctrlr *anm_ctrlr;

	LIST_FOREACH(anm_ctrlr, &anm->ctrlrs, list_entry) {
		if (ctrlr == anm_ctrlr->ctrlr) {
			return anm_ctrlr;
		}
	}

	return NULL;
}

static void
ftl_anm_add_poller_to_ctrlr(struct ftl_anm *anm, struct ftl_anm_poller *poller)
{
	struct ftl_anm_ctrlr *anm_ctrlr;

	anm_ctrlr = ftl_anm_find_ctrlr(anm, poller->dev->ctrlr);
	LIST_INSERT_HEAD(&anm_ctrlr->pollers, poller, list_entry);
}

void
ftl_anm_event_complete(struct ftl_anm_event *event)
{
	free(event);
}

int
ftl_anm_register_device(struct ftl_dev *dev, ftl_anm_fn fn)
{
	struct ftl_anm_poller *poller;
	int rc = 0;

	pthread_mutex_lock(&g_anm.lock);

	poller = ftl_anm_alloc_poller(fn, dev);
	if (!poller) {
		rc = -1;
		goto out;
	}

	ftl_anm_add_poller_to_ctrlr(&g_anm, poller);
out:
	pthread_mutex_unlock(&g_anm.lock);
	return rc;
}

void
ftl_anm_unregister_device(struct ftl_dev *dev)
{
	struct ftl_anm_ctrlr *ctrlr;
	struct ftl_anm_poller *poller, *temp_poller;

	pthread_mutex_lock(&g_anm.lock);
	ctrlr = ftl_anm_find_ctrlr(&g_anm, dev->ctrlr);

	LIST_FOREACH_SAFE(poller, &ctrlr->pollers, list_entry, temp_poller) {
		if (poller->dev == dev) {
			LIST_REMOVE(poller, list_entry);
			free(poller);
		}
	}

	pthread_mutex_unlock(&g_anm.lock);
}

int
ftl_anm_register_ctrlr(struct ftl_nvme_ctrlr *ctrlr)
{
	struct ftl_anm_ctrlr *anm_ctrlr;

	pthread_mutex_lock(&g_anm.lock);
	anm_ctrlr = ftl_anm_find_ctrlr(&g_anm, ctrlr);

	if (!anm_ctrlr) {
		anm_ctrlr = ftl_anm_ctrlr_alloc(ctrlr);
		if (!anm_ctrlr) {
			pthread_mutex_unlock(&g_anm.lock);
			return -1;
		}

		LIST_INSERT_HEAD(&g_anm.ctrlrs, anm_ctrlr, list_entry);
	}

	pthread_mutex_unlock(&g_anm.lock);
	return 0;
}

void
ftl_anm_unregister_ctrlr(struct ftl_nvme_ctrlr *ctrlr)
{
	struct ftl_anm_ctrlr *anm_ctrlr;

	pthread_mutex_lock(&g_anm.lock);
	anm_ctrlr = ftl_anm_find_ctrlr(&g_anm, ctrlr);

	if (anm_ctrlr && LIST_EMPTY(&anm_ctrlr->pollers)) {
		ftl_anm_ctrlr_free(anm_ctrlr);
	}

	pthread_mutex_unlock(&g_anm.lock);
}

int
ftl_anm_init(void)
{
	g_anm.thread = ftl_thread_init("anm_thread", 4096,
				       ftl_anm_thread, &g_anm, 0);

	if (!g_anm.thread) {
		return -1;
	}

	return ftl_thread_start(g_anm.thread);
}

void
ftl_anm_free(void)
{
	if (!g_anm.thread) {
		return;
	}

	ftl_thread_stop(g_anm.thread);
	ftl_thread_join(g_anm.thread);
	ftl_thread_free(g_anm.thread);

	g_anm.thread = NULL;
}
