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
#include "spdk/bdev_module.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/nvme_ocssd.h"
#include "spdk/nvme_ocssd_spec.h"
#include "spdk_internal/log.h"
#include "common.h"
#include "bdev_ocssd.h"

struct bdev_ocssd_ns {
	struct nvme_bdev_ctrlr		*nvme_bdev_ctrlr;
	struct spdk_ocssd_geometry_data	geometry;
	uint32_t			nsid;
	bool				valid;
};

struct ocssd_bdev_ctrlr {
	struct bdev_ocssd_ns *ns;
};

static int
bdev_ocssd_library_init(void)
{
	return 0;
}

static void
bdev_ocssd_library_fini(void)
{
}

static int
bdev_ocssd_config_json(struct spdk_json_write_ctx *w)
{
	return 0;
}

static int
bdev_ocssd_get_ctx_size(void)
{
	return 0;
}

static struct spdk_bdev_module ocssd_if = {
	.name = "ocssd",
	.module_init = bdev_ocssd_library_init,
	.module_fini = bdev_ocssd_library_fini,
	.config_json = bdev_ocssd_config_json,
	.get_ctx_size = bdev_ocssd_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(ocssd, &ocssd_if);

static void
bdev_ocssd_probe_done(struct nvme_async_probe_ctx *ctx, int rc)
{
	ctx->create_cb_fn(ctx->create_cb_ctx, 0, rc);
}

static void bdev_ocssd_get_geometry(struct nvme_async_probe_ctx *ctx);

static void
bdev_ocssd_geometry_cb(void *_ctx, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_async_probe_ctx *ctx = _ctx;
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;
	struct bdev_ocssd_ns *ocssd_ns;

	nvme_bdev_ctrlr = nvme_bdev_ctrlr_get(&ctx->trid);
	assert(nvme_bdev_ctrlr != NULL);

	ocssd_ns = &nvme_bdev_ctrlr->ocssd_ctrlr->ns[ctx->count];
	ocssd_ns->valid = !spdk_nvme_cpl_is_error(cpl);

	if (++ctx->count < nvme_bdev_ctrlr->num_ns) {
		bdev_ocssd_get_geometry(ctx);
	} else {
		bdev_ocssd_probe_done(ctx, 0);
	}
}

static void
bdev_ocssd_get_geometry(struct nvme_async_probe_ctx *ctx)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;
	struct bdev_ocssd_ns *ocssd_ns;
	int rc;

	nvme_bdev_ctrlr = nvme_bdev_ctrlr_get(&ctx->trid);
	if (!nvme_bdev_ctrlr) {
		SPDK_ERRLOG("Failed to find NVMe controller: %s\n", ctx->trid.traddr);
		bdev_ocssd_probe_done(ctx, -ENODEV);
		return;
	}

	if (ctx->count == nvme_bdev_ctrlr->num_ns) {
		bdev_ocssd_probe_done(ctx, 0);
		return;
	}

	ocssd_ns = &nvme_bdev_ctrlr->ocssd_ctrlr->ns[ctx->count];
	rc = spdk_nvme_ocssd_ctrlr_cmd_geometry(nvme_bdev_ctrlr->ctrlr, ctx->count + 1,
						&ocssd_ns->geometry,
						sizeof(ocssd_ns->geometry),
						bdev_ocssd_geometry_cb, ctx);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Failed to retrieve OC geometry: %s\n", spdk_strerror(-rc));
		if (++ctx->count == nvme_bdev_ctrlr->num_ns) {
			bdev_ocssd_probe_done(ctx, 0);
		}
	}
}

void
spdk_bdev_ocssd_create_bdevs(struct nvme_async_probe_ctx *ctx, spdk_bdev_create_nvme_fn cb_fn,
			     void *cb_arg)
{
	ctx->create_cb_fn = cb_fn;
	ctx->create_cb_ctx = cb_arg;
	ctx->count = 0;

	bdev_ocssd_get_geometry(ctx);
}

int
spdk_bdev_ocssd_init_ctrlr(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr)
{
	struct ocssd_bdev_ctrlr *ocssd_ctrlr;
	uint32_t nsid;

	assert(spdk_nvme_ctrlr_is_ocssd_supported(nvme_bdev_ctrlr->ctrlr));

	ocssd_ctrlr = calloc(1, sizeof(*ocssd_ctrlr));
	if (!ocssd_ctrlr) {
		return -ENOMEM;
	}

	ocssd_ctrlr->ns = calloc(nvme_bdev_ctrlr->num_ns, sizeof(*ocssd_ctrlr->ns));
	if (!ocssd_ctrlr->ns) {
		free(ocssd_ctrlr);
		return -ENOMEM;
	}

	nvme_bdev_ctrlr->ocssd_ctrlr = ocssd_ctrlr;
	for (nsid = 0; nsid < nvme_bdev_ctrlr->num_ns; ++nsid) {
		ocssd_ctrlr->ns[nsid].nvme_bdev_ctrlr = nvme_bdev_ctrlr;
		ocssd_ctrlr->ns[nsid].nsid = nsid + 1;
		ocssd_ctrlr->ns[nsid].valid = false;
	}

	return 0;
}

void
spdk_bdev_ocssd_fini_ctrlr(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr)
{
	free(nvme_bdev_ctrlr->ocssd_ctrlr->ns);
	free(nvme_bdev_ctrlr->ocssd_ctrlr);
	nvme_bdev_ctrlr->ocssd_ctrlr = NULL;
}

SPDK_LOG_REGISTER_COMPONENT("bdev_ocssd", SPDK_LOG_BDEV_OCSSD)
