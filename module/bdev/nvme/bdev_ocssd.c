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

size_t
bdev_ocssd_get_ns_struct_size(void)
{
	return sizeof(struct bdev_ocssd_ns);
}

static struct bdev_ocssd_ns *
bdev_ocssd_get_ns_from_nvme(struct nvme_namespace *nvme_ns)
{
	return (struct bdev_ocssd_ns *)(nvme_ns + 1);
}

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

struct bdev_ocssd_init_ns_ctx {
	struct nvme_namespace		*nvme_ns;
	spdk_bdev_init_namespaces_fn	cb_fn;
	void				*cb_arg;
};

static void
bdev_ocssd_geometry_cb(void *_ctx, const struct spdk_nvme_cpl *cpl)
{
	struct bdev_ocssd_init_ns_ctx *ctx = _ctx;

	ctx->cb_fn(ctx->cb_arg, ctx->nvme_ns, spdk_nvme_cpl_is_error(cpl) ? -EIO : 0);
	free(ctx);
}

void
bdev_ocssd_init_ns(struct nvme_bdev_ctrlr *nvme_bdev_ctrlr, struct nvme_namespace *nvme_ns,
		   spdk_bdev_init_namespaces_fn cb_fn, void *cb_arg)
{
	struct bdev_ocssd_init_ns_ctx *ctx;
	struct bdev_ocssd_ns *ocssd_ns;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		cb_fn(cb_arg, nvme_ns, -ENOMEM);
		return;
	}

	ctx->nvme_ns = nvme_ns;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	ocssd_ns = bdev_ocssd_get_ns_from_nvme(nvme_ns);
	rc = spdk_nvme_ocssd_ctrlr_cmd_geometry(nvme_bdev_ctrlr->ctrlr, nvme_ns->id,
						&ocssd_ns->geometry,
						sizeof(ocssd_ns->geometry),
						bdev_ocssd_geometry_cb, ctx);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Failed to retrieve OC geometry: %s\n", spdk_strerror(-rc));
		cb_fn(cb_arg, nvme_ns, rc);
		free(ctx);
	}
}

SPDK_LOG_REGISTER_COMPONENT("bdev_ocssd", SPDK_LOG_BDEV_OCSSD)
