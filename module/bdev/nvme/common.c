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
#include "bdev_ocssd.h"
#include "common.h"

struct nvme_bdev_ctrlrs g_nvme_bdev_ctrlrs = TAILQ_HEAD_INITIALIZER(g_nvme_bdev_ctrlrs);
pthread_mutex_t g_bdev_nvme_mutex = PTHREAD_MUTEX_INITIALIZER;
bool g_bdev_nvme_module_finish;

struct nvme_bdev_ctrlr *
nvme_bdev_ctrlr_get(const struct spdk_nvme_transport_id *trid)
{
	struct nvme_bdev_ctrlr	*nvme_bdev_ctrlr;

	TAILQ_FOREACH(nvme_bdev_ctrlr, &g_nvme_bdev_ctrlrs, tailq) {
		if (spdk_nvme_transport_id_compare(trid, nvme_bdev_ctrlr->connected_trid) == 0) {
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
nvme_bdev_unregister_cb(void *io_device)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr = io_device;
	struct nvme_bdev_ctrlr_trid *trid, *tmp_trid;
	uint32_t i;

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	TAILQ_REMOVE(&g_nvme_bdev_ctrlrs, nvme_bdev_ctrlr, tailq);
	pthread_mutex_unlock(&g_bdev_nvme_mutex);
	spdk_nvme_detach(nvme_bdev_ctrlr->ctrlr);
	spdk_poller_unregister(&nvme_bdev_ctrlr->adminq_timer_poller);
	free(nvme_bdev_ctrlr->name);
	for (i = 0; i < nvme_bdev_ctrlr->num_ns; i++) {
		free(nvme_bdev_ctrlr->namespaces[i]);
	}

	TAILQ_FOREACH_SAFE(trid, &nvme_bdev_ctrlr->trids, link, tmp_trid) {
		TAILQ_REMOVE(&nvme_bdev_ctrlr->trids, trid, link);
		free(trid);
	}

	free(nvme_bdev_ctrlr->namespaces);
	free(nvme_bdev_ctrlr);

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	if (g_bdev_nvme_module_finish && TAILQ_EMPTY(&g_nvme_bdev_ctrlrs)) {
		pthread_mutex_unlock(&g_bdev_nvme_mutex);
		spdk_io_device_unregister(&g_nvme_bdev_ctrlrs, NULL);
		spdk_bdev_module_finish_done();
		return;
	}

	pthread_mutex_unlock(&g_bdev_nvme_mutex);
}

int
nvme_bdev_ctrlr_destruct(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr)
{
	assert(nvme_bdev_ctrlr->destruct);
	pthread_mutex_lock(&g_bdev_nvme_mutex);

	spdk_poller_unregister(&nvme_bdev_ctrlr->destruct_poller);

	if (nvme_bdev_ctrlr->resetting) {
		nvme_bdev_ctrlr->destruct_poller =
			SPDK_POLLER_REGISTER((spdk_poller_fn)nvme_bdev_ctrlr_destruct,
					     nvme_bdev_ctrlr, 1000);
		pthread_mutex_unlock(&g_bdev_nvme_mutex);
		return SPDK_POLLER_BUSY;
	}
	pthread_mutex_unlock(&g_bdev_nvme_mutex);

	if (nvme_bdev_ctrlr->opal_dev) {
		spdk_opal_dev_destruct(nvme_bdev_ctrlr->opal_dev);
		nvme_bdev_ctrlr->opal_dev = NULL;
	}

	if (nvme_bdev_ctrlr->ocssd_ctrlr) {
		bdev_ocssd_fini_ctrlr(nvme_bdev_ctrlr);
	}

	spdk_io_device_unregister(nvme_bdev_ctrlr, nvme_bdev_unregister_cb);
	return SPDK_POLLER_BUSY;
}

void
nvme_bdev_attach_bdev_to_ns(struct nvme_bdev_ns *nvme_ns, struct nvme_bdev *nvme_disk)
{
	nvme_ns->ctrlr->ref++;

	TAILQ_INSERT_TAIL(&nvme_ns->bdevs, nvme_disk, tailq);
}

void
nvme_bdev_detach_bdev_from_ns(struct nvme_bdev *nvme_disk)
{
	struct nvme_bdev_ctrlr *ctrlr = nvme_disk->nvme_ns->ctrlr;

	pthread_mutex_lock(&g_bdev_nvme_mutex);
	assert(ctrlr->ref > 0);
	ctrlr->ref--;

	TAILQ_REMOVE(&nvme_disk->nvme_ns->bdevs, nvme_disk, tailq);

	if (ctrlr->ref == 0 && ctrlr->destruct) {
		pthread_mutex_unlock(&g_bdev_nvme_mutex);
		nvme_bdev_ctrlr_destruct(ctrlr);
		return;
	}

	pthread_mutex_unlock(&g_bdev_nvme_mutex);
}
