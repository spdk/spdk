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

#include "spdk/log.h"
#include "common.h"
#include "spdk/string.h"
#include "spdk_internal/log.h"

struct nvme_bdev_ctrlrs g_nvme_bdev_ctrlrs = TAILQ_HEAD_INITIALIZER(g_nvme_bdev_ctrlrs);
pthread_mutex_t g_bdev_nvme_mutex = PTHREAD_MUTEX_INITIALIZER;
/* All the controllers deleted by users via RPC are skipped by hotplug monitor */
struct nvme_probe_skip_entries g_skipped_nvme_ctrlrs = TAILQ_HEAD_INITIALIZER(
			g_skipped_nvme_ctrlrs);
static int g_hot_insert_nvme_controller_index = 0;

struct nvme_bdev_ctrlr *
nvme_bdev_ctrlr_get(const struct spdk_nvme_transport_id *trid)
{
	struct nvme_bdev_ctrlr	*nvme_bdev_ctrlr;

	TAILQ_FOREACH(nvme_bdev_ctrlr, &g_nvme_bdev_ctrlrs, tailq) {
		if (spdk_nvme_transport_id_compare(trid, &nvme_bdev_ctrlr->trid) == 0) {
			return nvme_bdev_ctrlr;
		}
	}

	return NULL;
}

struct nvme_bdev_ctrlr *
nvme_bdev_ctrlr_get_by_name(const char *name)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;

	if (name == NULL) {
		return NULL;
	}

	TAILQ_FOREACH(nvme_bdev_ctrlr, &g_nvme_bdev_ctrlrs, tailq) {
		if (strcmp(name, nvme_bdev_ctrlr->name) == 0) {
			return nvme_bdev_ctrlr;
		}
	}

	return NULL;
}

struct nvme_bdev_ctrlr *
nvme_bdev_first_ctrlr(void)
{
	return TAILQ_FIRST(&g_nvme_bdev_ctrlrs);
}

struct nvme_bdev_ctrlr *
nvme_bdev_next_ctrlr(struct nvme_bdev_ctrlr *prev)
{
	return TAILQ_NEXT(prev, tailq);
}

void
nvme_bdev_dump_trid_json(struct spdk_nvme_transport_id *trid, struct spdk_json_write_ctx *w)
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

void
spdk_bdev_nvme_delete_cb(void *cb_ctx, struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t i;
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;
	struct nvme_bdev *nvme_bdev;

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	TAILQ_FOREACH(nvme_bdev_ctrlr, &g_nvme_bdev_ctrlrs, tailq) {
		if (nvme_bdev_ctrlr->ctrlr == ctrlr) {
			pthread_mutex_unlock(&g_bdev_nvme_mutex);

			for (i = 0; i < nvme_bdev_ctrlr->num_ns; i++) {
				uint32_t	nsid = i + 1;

				nvme_bdev = &nvme_bdev_ctrlr->bdevs[nsid - 1];
				if (nvme_bdev->active) {
					assert(nvme_bdev->id == nsid);
					spdk_bdev_unregister(&nvme_bdev->disk, NULL, NULL);
				}
			}

			pthread_mutex_lock(&g_bdev_nvme_mutex);
			assert(!nvme_bdev_ctrlr->destruct);
			nvme_bdev_ctrlr->destruct = true;
			if (nvme_bdev_ctrlr->ref == 0) {
				pthread_mutex_unlock(&g_bdev_nvme_mutex);
				nvme_bdev_ctrlr->remove_fn(nvme_bdev_ctrlr);
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

	spdk_bdev_nvme_delete_cb(NULL, nvme_bdev_ctrlr->ctrlr);
	return 0;
}

bool
spdk_bdev_nvme_probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
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

	return true;
}

void
spdk_bdev_nvme_attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
			 struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct nvme_probe_ctx *ctx = cb_ctx;
	char *name = NULL;
	uint32_t prchk_flags = 0;
	size_t i;

	assert(ctx->create_ctrlr_fn != NULL);

	if (ctx->count > 0) {
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

	ctx->create_ctrlr_fn(ctrlr, name, trid, prchk_flags);

	free(name);
}
