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

#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk_internal/log.h"
#include "common_bdev_nvme.h"

TAILQ_HEAD(, nvme_ctrlr) g_nvme_ctrlrs = TAILQ_HEAD_INITIALIZER(g_nvme_ctrlrs);
pthread_mutex_t g_bdev_nvme_mutex = PTHREAD_MUTEX_INITIALIZER;

struct nvme_ctrlr *
spdk_nvme_ctrlr_get(const struct spdk_nvme_transport_id *trid)
{
	struct nvme_ctrlr	*nvme_ctrlr;

	TAILQ_FOREACH(nvme_ctrlr, &g_nvme_ctrlrs, tailq) {
		if (spdk_nvme_transport_id_compare(trid, &nvme_ctrlr->trid) == 0) {
			return nvme_ctrlr;
		}
	}

	return NULL;
}

struct nvme_ctrlr *
spdk_nvme_ctrlr_get_by_name(const char *name)
{
	struct nvme_ctrlr *nvme_ctrlr;

	if (name == NULL) {
		return NULL;
	}

	TAILQ_FOREACH(nvme_ctrlr, &g_nvme_ctrlrs, tailq) {
		if (strcmp(name, nvme_ctrlr->name) == 0) {
			return nvme_ctrlr;
		}
	}

	return NULL;
}

struct nvme_ctrlr *
spdk_bdev_nvme_first_ctrlr(void)
{
	return TAILQ_FIRST(&g_nvme_ctrlrs);
}

struct nvme_ctrlr *
spdk_bdev_nvme_next_ctrlr(struct nvme_ctrlr *prev)
{
	return TAILQ_NEXT(prev, tailq);
}

struct spdk_nvme_qpair *
spdk_bdev_nvme_get_io_qpair(struct spdk_io_channel *ctrlr_io_ch)
{
	struct nvme_io_channel *nvme_ch;

	nvme_ch =  spdk_io_channel_get_ctx(ctrlr_io_ch);

	return nvme_ch->qpair;
}

void
spdk_bdev_nvme_delete_cb(void *cb_ctx, struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t i;
	struct nvme_ctrlr *nvme_ctrlr;
	struct nvme_bdev *nvme_bdev;
	struct ftl_bdev *ftl_bdev, *tmp;

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	TAILQ_FOREACH(nvme_ctrlr, &g_nvme_ctrlrs, tailq) {
		if (nvme_ctrlr->ctrlr == ctrlr) {
			pthread_mutex_unlock(&g_bdev_nvme_mutex);

#if defined(FTL)
			TAILQ_FOREACH_SAFE(ftl_bdev, &nvme_ctrlr->ftl_bdevs, tailq, tmp) {
				spdk_bdev_unregister(&ftl_bdev->bdev, NULL, NULL);
			}
#endif

			for (i = 0; i < nvme_ctrlr->num_ns; i++) {
				uint32_t	nsid = i + 1;

				nvme_bdev = &nvme_ctrlr->bdevs[nsid - 1];
				assert(nvme_bdev->id == nsid);
				if (nvme_bdev->active) {
					spdk_bdev_unregister(&nvme_bdev->disk, NULL, NULL);
				}
			}

			pthread_mutex_lock(&g_bdev_nvme_mutex);
			assert(!nvme_ctrlr->destruct);
			nvme_ctrlr->destruct = true;
			if (nvme_ctrlr->ref == 0) {
				pthread_mutex_unlock(&g_bdev_nvme_mutex);
				nvme_ctrlr->remove_fn(nvme_ctrlr);
			} else {
				pthread_mutex_unlock(&g_bdev_nvme_mutex);
			}
			return;
		}
	}
	pthread_mutex_unlock(&g_bdev_nvme_mutex);
}

int
spdk_bdev_nvme_delete(const char *name)
{
	struct nvme_ctrlr *nvme_ctrlr = NULL;

	if (name == NULL) {
		return -EINVAL;
	}

	nvme_ctrlr = spdk_nvme_ctrlr_get_by_name(name);
	if (nvme_ctrlr == NULL) {
		SPDK_ERRLOG("Failed to find NVMe controller\n");
		return -ENODEV;
	}

	spdk_bdev_nvme_delete_cb(NULL, nvme_ctrlr->ctrlr);
	return 0;
}

bool
spdk_bdev_nvme_probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
			struct spdk_nvme_ctrlr_opts *opts)
{
	struct nvme_probe_ctx *ctx = cb_ctx;

	SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "Probing device %s\n", trid->traddr);

	if (spdk_nvme_ctrlr_get(trid)) {
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

	return true;
}
