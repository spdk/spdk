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
	struct spdk_ocssd_geometry_data	geometry;
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

struct bdev_ocssd_populate_ns_ctx {
	struct nvme_async_probe_ctx	*nvme_ctx;
	struct nvme_bdev_ns		*nvme_ns;
};

static void
bdev_ocssd_geometry_cb(void *_ctx, const struct spdk_nvme_cpl *cpl)
{
	struct bdev_ocssd_populate_ns_ctx *ctx = _ctx;
	struct nvme_bdev_ns *nvme_ns = ctx->nvme_ns;
	int rc = 0;

	if (spdk_unlikely(spdk_nvme_cpl_is_error(cpl))) {
		SPDK_ERRLOG("Failed to retrieve geometry for namespace %"PRIu32"\n", nvme_ns->id);
		free(nvme_ns->type_ctx);
		nvme_ns->type_ctx = NULL;
		rc = -EIO;
	}

	nvme_ctrlr_populate_namespace_done(ctx->nvme_ctx, nvme_ns, rc);
	free(ctx);
}

void
bdev_ocssd_populate_namespace(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr,
			      struct nvme_bdev_ns *nvme_ns,
			      struct nvme_async_probe_ctx *nvme_ctx)
{
	struct bdev_ocssd_ns *ocssd_ns;
	struct bdev_ocssd_populate_ns_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		nvme_ctrlr_populate_namespace_done(nvme_ctx, nvme_ns, -ENOMEM);
		return;
	}

	ocssd_ns = calloc(1, sizeof(*ocssd_ns));
	if (ocssd_ns == NULL) {
		nvme_ctrlr_populate_namespace_done(nvme_ctx, nvme_ns, -ENOMEM);
		free(ctx);
		return;
	}

	nvme_ns->type_ctx = ocssd_ns;
	ctx->nvme_ctx = nvme_ctx;
	ctx->nvme_ns = nvme_ns;

	rc = spdk_nvme_ocssd_ctrlr_cmd_geometry(nvme_bdev_ctrlr->ctrlr, nvme_ns->id,
						&ocssd_ns->geometry,
						sizeof(ocssd_ns->geometry),
						bdev_ocssd_geometry_cb, ctx);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Failed to retrieve OC geometry: %s\n", spdk_strerror(-rc));
		nvme_ns->type_ctx = NULL;
		nvme_ctrlr_populate_namespace_done(nvme_ctx, nvme_ns, rc);
		free(ocssd_ns);
		free(ctx);
	}
}

void
bdev_ocssd_depopulate_namespace(struct nvme_bdev_ns *ns)
{
	free(ns->type_ctx);
	ns->populated = false;
	ns->type_ctx = NULL;
}

SPDK_LOG_REGISTER_COMPONENT("bdev_ocssd", SPDK_LOG_BDEV_OCSSD)
