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
#include <spdk/ocssd.h>
#include "ocssd_anm.h"
#include "ocssd_utils.h"
#include "ocssd_core.h"
#include "ocssd_nvme.h"

/* Number of log pages read in single get_log_page call */
#define OCSSD_ANM_LOG_ENTRIES 16

/* Structure aggregating ANM callback registered by ocssd device */
struct ocssd_anm_poller {
	struct ocssd_dev				*dev;

	ocssd_anm_fn					fn;

	LIST_ENTRY(ocssd_anm_poller)			list_entry;
};

struct ocssd_anm_ctrlr {
	/* NVMe controller */
	struct ocssd_nvme_ctrlr				*ctrlr;

	/* Outstanding ANM event counter */
	volatile int					anm_outstanding;

	/* Indicates if get log page command has been submitted to controller */
	volatile int					processing;

	/* Notification counter */
	uint64_t					nc;

	/* DMA allocated buffer for log pages */
	struct spdk_ocssd_chunk_notification_entry	*log;

	/* List link */
	LIST_ENTRY(ocssd_anm_ctrlr)			list_entry;

	/* List of registered pollers */
	LIST_HEAD(, ocssd_anm_poller)			pollers;
};

struct ocssd_anm {
	/* Thread descriptor */
	struct ocssd_thread			*thread;

	pthread_mutex_t				lock;

	/* List of registered controllers */
	LIST_HEAD(, ocssd_anm_ctrlr)		ctrlrs;
};

static struct ocssd_anm g_anm = { .lock = PTHREAD_MUTEX_INITIALIZER };

static int
ocssd_anm_log_range(struct spdk_ocssd_chunk_notification_entry *log)
{
	if (log->mask.lblk) {
		return OCSSD_ANM_RANGE_LBK;
	}

	if (log->mask.chunk) {
		return OCSSD_ANM_RANGE_CHK;
	}

	if (log->mask.pu) {
		return OCSSD_ANM_RANGE_PU;
	}

	assert(0);
	return OCSSD_ANM_RANGE_MAX;
}

static struct ocssd_anm_event *
ocssd_anm_event_alloc(struct ocssd_dev *dev, struct ocssd_ppa ppa, enum ocssd_anm_range range)
{
	struct ocssd_anm_event *event;

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
ocssd_anm_process_log(struct ocssd_anm_poller *poller, struct ocssd_anm_ctrlr *ctrlr,
		      struct spdk_ocssd_chunk_notification_entry *log)
{
	struct ocssd_anm_event *event;
	struct ocssd_ppa ppa = ocssd_ppa_addr_unpack(poller->dev, log->lba);

	/* TODO We need to parse log and decide if action is needed. */
	/* For now we check only if ppa is in device range. */
	if (!ocssd_ppa_in_range(poller->dev, ppa)) {
		return -1;
	}

	event = ocssd_anm_event_alloc(poller->dev, ppa, ocssd_anm_log_range(log));
	poller->fn(event);

	return 0;
}

static int
ocssd_anm_log_valid(struct ocssd_anm_ctrlr *ctrlr, struct spdk_ocssd_chunk_notification_entry *log)
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
ocssd_anm_log_page_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct ocssd_anm_ctrlr *ctrlr = ctx;
	struct ocssd_anm_poller *poller;

	ctrlr->processing = 0;

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_ERRLOG("Unexpected status code: [%d], status code type: [%d]\n",
			    cpl->status.sc, cpl->status.sct);
		return;
	}

	for (size_t i = 0; i < OCSSD_ANM_LOG_ENTRIES; ++i) {
		if (!ocssd_anm_log_valid(ctrlr, &ctrlr->log[i])) {
			return;
		}

		LIST_FOREACH(poller, &ctrlr->pollers, list_entry) {
			if (!ocssd_anm_process_log(poller, ctrlr, &ctrlr->log[i])) {
				break;
			}
		}
	}

	/* We increment anm_outstanding counter in case there are more logs in controller */
	/* than we get in single log page call */
	ctrlr->anm_outstanding++;
}

static void
ocssd_anm_aer_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	union spdk_nvme_async_event_completion event = { .raw = cpl->cdw0 };
	struct ocssd_anm_ctrlr *ctrlr = ctx;

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
ocssd_anm_get_log_page(struct ocssd_anm_ctrlr *ctrlr)
{
	ctrlr->anm_outstanding = 0;

	if (ocssd_nvme_get_log_page(ctrlr->ctrlr, SPDK_OCSSD_LOG_CHUNK_NOTIFICATION,
				    ctrlr->log, sizeof(*ctrlr->log) * OCSSD_ANM_LOG_ENTRIES, 0,
				    ocssd_anm_log_page_cb, (void *)ctrlr)) {
		return -1;
	}

	ctrlr->processing = 1;

	return 0;
}

static void
ocssd_anm_thread(void *ctx)
{
	struct ocssd_anm *anm = ctx;
	struct ocssd_anm_ctrlr *ctrlr;

	while (anm->thread->running) {
		pthread_mutex_lock(&anm->lock);
		LIST_FOREACH(ctrlr, &anm->ctrlrs, list_entry) {
			ocssd_nvme_process_admin_completions(ctrlr->ctrlr);

			if (ctrlr->anm_outstanding && !ctrlr->processing) {
				if (ocssd_anm_get_log_page(ctrlr)) {
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

static struct ocssd_anm_poller *
ocssd_anm_alloc_poller(ocssd_anm_fn fn, struct ocssd_dev *dev)
{
	struct ocssd_anm_poller *poller;

	poller = calloc(1, sizeof(*poller));
	if (!poller) {
		return NULL;
	}

	poller->fn = fn;
	poller->dev = dev;

	return poller;
}

static void
ocssd_anm_ctrlr_free(struct ocssd_anm_ctrlr *ctrlr)
{
	if (!ctrlr) {
		return;
	}

	/* Unregister ctrlr from aer events */
	ocssd_nvme_register_aer_callback(ctrlr->ctrlr, NULL, NULL);

	LIST_REMOVE(ctrlr, list_entry);

	spdk_dma_free(ctrlr->log);
	free(ctrlr);
}

static struct ocssd_anm_ctrlr *
ocssd_anm_ctrlr_alloc(struct ocssd_nvme_ctrlr *nvme_ctrlr)
{
	struct ocssd_anm_ctrlr *ctrlr;

	ctrlr = calloc(1, sizeof(*ctrlr));
	if (!ctrlr) {
		return NULL;
	}

	ctrlr->log = spdk_dma_zmalloc(sizeof(*ctrlr->log) * OCSSD_ANM_LOG_ENTRIES,
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

	ocssd_nvme_register_aer_callback(ctrlr->ctrlr, ocssd_anm_aer_cb, ctrlr);
	return ctrlr;
}

static struct ocssd_anm_ctrlr *
ocssd_anm_find_ctrlr(struct ocssd_anm *anm, struct ocssd_nvme_ctrlr *ctrlr)
{
	struct ocssd_anm_ctrlr *anm_ctrlr;

	LIST_FOREACH(anm_ctrlr, &anm->ctrlrs, list_entry) {
		if (ctrlr == anm_ctrlr->ctrlr) {
			return anm_ctrlr;
		}
	}

	return NULL;
}

static void
ocssd_anm_add_poller_to_ctrlr(struct ocssd_anm *anm, struct ocssd_anm_poller *poller)
{
	struct ocssd_anm_ctrlr *anm_ctrlr;

	anm_ctrlr = ocssd_anm_find_ctrlr(anm, poller->dev->ctrlr);
	LIST_INSERT_HEAD(&anm_ctrlr->pollers, poller, list_entry);
}

void
ocssd_anm_event_complete(struct ocssd_anm_event *event)
{
	free(event);
}

int
ocssd_anm_register_device(struct ocssd_dev *dev, ocssd_anm_fn fn)
{
	struct ocssd_anm_poller *poller;
	int rc = 0;

	pthread_mutex_lock(&g_anm.lock);

	poller = ocssd_anm_alloc_poller(fn, dev);
	if (!poller) {
		rc = -1;
		goto out;
	}

	ocssd_anm_add_poller_to_ctrlr(&g_anm, poller);
out:
	pthread_mutex_unlock(&g_anm.lock);
	return rc;
}

void
ocssd_anm_unregister_device(struct ocssd_dev *dev)
{
	struct ocssd_anm_ctrlr *ctrlr;
	struct ocssd_anm_poller *poller, *temp_poller;

	pthread_mutex_lock(&g_anm.lock);
	ctrlr = ocssd_anm_find_ctrlr(&g_anm, dev->ctrlr);

	LIST_FOREACH_SAFE(poller, &ctrlr->pollers, list_entry, temp_poller) {
		if (poller->dev == dev) {
			LIST_REMOVE(poller, list_entry);
			free(poller);
		}
	}

	pthread_mutex_unlock(&g_anm.lock);
}

int
ocssd_anm_register_ctrlr(struct ocssd_nvme_ctrlr *ctrlr)
{
	struct ocssd_anm_ctrlr *anm_ctrlr;

	pthread_mutex_lock(&g_anm.lock);
	anm_ctrlr = ocssd_anm_find_ctrlr(&g_anm, ctrlr);

	if (!anm_ctrlr) {
		anm_ctrlr = ocssd_anm_ctrlr_alloc(ctrlr);
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
ocssd_anm_unregister_ctrlr(struct ocssd_nvme_ctrlr *ctrlr)
{
	struct ocssd_anm_ctrlr *anm_ctrlr;

	pthread_mutex_lock(&g_anm.lock);
	anm_ctrlr = ocssd_anm_find_ctrlr(&g_anm, ctrlr);

	if (anm_ctrlr && LIST_EMPTY(&anm_ctrlr->pollers)) {
		ocssd_anm_ctrlr_free(anm_ctrlr);
	}

	pthread_mutex_unlock(&g_anm.lock);
}

int
ocssd_anm_init(void)
{
	g_anm.thread = ocssd_thread_init("anm_thread", 4096,
					 ocssd_anm_thread, &g_anm, 0);

	if (!g_anm.thread) {
		return -1;
	}

	return ocssd_thread_start(g_anm.thread);
}

void
ocssd_anm_free(void)
{
	if (!g_anm.thread) {
		return;
	}

	ocssd_thread_stop(g_anm.thread);
	ocssd_thread_join(g_anm.thread);
	ocssd_thread_free(g_anm.thread);

	g_anm.thread = NULL;
}
