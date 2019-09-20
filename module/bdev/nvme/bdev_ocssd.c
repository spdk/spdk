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

struct ocssd_bdev {
	struct nvme_bdev nvme_bdev;
};

struct bdev_ocssd_ns {
	struct spdk_ocssd_geometry_data	geometry;
};

static struct bdev_ocssd_ns *
bdev_ocssd_get_ns_from_nvme(struct nvme_bdev_ns *nvme_ns)
{
	return nvme_ns->type_ctx;
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

static void
bdev_ocssd_free_bdev(struct ocssd_bdev *ocssd_bdev)
{
	if (!ocssd_bdev) {
		return;
	}

	free(ocssd_bdev->nvme_bdev.disk.name);
	free(ocssd_bdev);
}

static int
bdev_ocssd_destruct(void *ctx)
{
	struct ocssd_bdev *ocssd_bdev = ctx;
	struct nvme_bdev *nvme_bdev = &ocssd_bdev->nvme_bdev;

	nvme_bdev_detach_bdev_from_ns(nvme_bdev);
	bdev_ocssd_free_bdev(ocssd_bdev);

	return 0;
}

static void
bdev_ocssd_submit_request(struct spdk_io_channel *ioch, struct spdk_bdev_io *bdev_io)
{
	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
}

static bool
bdev_ocssd_io_type_supported(void *ctx, enum spdk_bdev_io_type type)
{
	return false;
}

static struct spdk_io_channel *
bdev_ocssd_get_io_channel(void *ctx)
{
	struct ocssd_bdev *ocssd_bdev = ctx;

	return spdk_get_io_channel(ocssd_bdev->nvme_bdev.nvme_bdev_ctrlr);
}

static struct spdk_bdev_fn_table ocssdlib_fn_table = {
	.destruct		= bdev_ocssd_destruct,
	.submit_request		= bdev_ocssd_submit_request,
	.io_type_supported	= bdev_ocssd_io_type_supported,
	.get_io_channel		= bdev_ocssd_get_io_channel,
};

void
bdev_ocssd_create_bdev(const char *ctrlr_name, const char *bdev_name, uint32_t nsid,
		       bdev_ocssd_create_cb cb_fn, void *cb_arg)
{
	struct nvme_bdev_ctrlr *nvme_bdev_ctrlr;
	struct nvme_bdev *nvme_bdev = NULL;
	struct ocssd_bdev *ocssd_bdev = NULL;
	struct spdk_nvme_ns *ns;
	struct nvme_bdev_ns *nvme_ns;
	struct bdev_ocssd_ns *ocssd_ns;
	struct spdk_ocssd_geometry_data *geometry;
	int rc = 0;

	nvme_bdev_ctrlr = nvme_bdev_ctrlr_get_by_name(ctrlr_name);
	if (!nvme_bdev_ctrlr) {
		SPDK_ERRLOG("Unable to find controller %s\n", ctrlr_name);
		rc = -ENODEV;
		goto finish;
	}

	ns = spdk_nvme_ctrlr_get_ns(nvme_bdev_ctrlr->ctrlr, nsid);
	if (!ns) {
		SPDK_ERRLOG("Unable to retrieve namespace %"PRIu32"\n", nsid);
		rc = -ENODEV;
		goto finish;
	}

	if (!spdk_nvme_ns_is_active(ns)) {
		SPDK_ERRLOG("Namespace %"PRIu32" is inactive\n", nsid);
		rc = -EACCES;
		goto finish;
	}

	assert(nsid <= nvme_bdev_ctrlr->num_ns);
	nvme_ns = nvme_bdev_ctrlr->namespaces[nsid - 1];
	if (nvme_ns == NULL) {
		SPDK_ERRLOG("Namespace %"PRIu32" is not initialized\n", nsid);
		rc = -EINVAL;
		goto finish;
	}

	ocssd_ns = bdev_ocssd_get_ns_from_nvme(nvme_ns);
	if (ocssd_ns == NULL) {
		SPDK_ERRLOG("Namespace %"PRIu32" is not an OCSSD namespace\n", nsid);
		rc = -EINVAL;
		goto finish;
	}

	if (spdk_bdev_get_by_name(bdev_name) != NULL) {
		SPDK_ERRLOG("Device with provided name (%s) already exists\n", bdev_name);
		rc = -EEXIST;
		goto finish;
	}

	/* Only allow one bdev per namespace for now */
	if (!TAILQ_EMPTY(&nvme_ns->bdevs)) {
		SPDK_ERRLOG("Namespace %"PRIu32" was already claimed by bdev %s\n",
			    nsid, TAILQ_FIRST(&nvme_ns->bdevs)->disk.name);
		rc = -EEXIST;
		goto finish;
	}

	ocssd_bdev = calloc(1, sizeof(*ocssd_bdev));
	if (!ocssd_bdev) {
		rc = -ENOMEM;
		goto finish;
	}

	nvme_bdev = &ocssd_bdev->nvme_bdev;
	nvme_bdev->nvme_ns = nvme_ns;
	nvme_bdev->nvme_bdev_ctrlr = nvme_bdev_ctrlr;
	geometry = &ocssd_ns->geometry;

	nvme_bdev->disk.name = strdup(bdev_name);
	if (!nvme_bdev->disk.name) {
		rc = -ENOMEM;
		goto finish;
	}

	nvme_bdev->disk.product_name = "Open Channel SSD";
	nvme_bdev->disk.ctxt = ocssd_bdev;
	nvme_bdev->disk.fn_table = &ocssdlib_fn_table;
	nvme_bdev->disk.module = &ocssd_if;
	nvme_bdev->disk.blocklen = spdk_nvme_ns_get_extended_sector_size(ns);
	nvme_bdev->disk.zoned = true;
	nvme_bdev->disk.blockcnt = geometry->num_grp * geometry->num_pu *
				   geometry->num_chk * geometry->clba;
	nvme_bdev->disk.zone_size = geometry->clba;
	nvme_bdev->disk.max_open_zones = geometry->maxoc;
	nvme_bdev->disk.optimal_open_zones = geometry->num_grp * geometry->num_pu;
	nvme_bdev->disk.write_unit_size = geometry->ws_opt;

	if (geometry->maxocpu != 0 && geometry->maxocpu != geometry->maxoc) {
		SPDK_WARNLOG("Maximum open chunks per PU is not zero. Reducing the maximum "
			     "number of open zones: %"PRIu32" -> %"PRIu32"\n",
			     geometry->maxoc, geometry->maxocpu);
		nvme_bdev->disk.max_open_zones = geometry->maxocpu;
	}

	rc = spdk_bdev_register(&nvme_bdev->disk);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Failed to register bdev %s\n", nvme_bdev->disk.name);
		goto finish;
	}

	nvme_bdev_attach_bdev_to_ns(nvme_ns, nvme_bdev);
finish:
	if (spdk_unlikely(rc != 0)) {
		bdev_ocssd_free_bdev(ocssd_bdev);
		bdev_name = NULL;
	}

	cb_fn(bdev_name, rc, cb_arg);
}

struct bdev_ocssd_delete_ctx {
	bdev_ocssd_delete_cb	cb_fn;
	void			*cb_arg;
};

static void
bdev_ocssd_unregister_cb(void *cb_arg, int status)
{
	struct bdev_ocssd_delete_ctx *delete_ctx = cb_arg;

	delete_ctx->cb_fn(status, delete_ctx->cb_arg);
	free(delete_ctx);
}

void
bdev_ocssd_delete_bdev(const char *bdev_name, bdev_ocssd_delete_cb cb_fn, void *cb_arg)
{
	struct spdk_bdev *bdev;
	struct bdev_ocssd_delete_ctx *delete_ctx;

	bdev = spdk_bdev_get_by_name(bdev_name);
	if (!bdev) {
		SPDK_ERRLOG("Unable to find bdev %s\n", bdev_name);
		cb_fn(-ENODEV, cb_arg);
		return;
	}

	if (bdev->module != &ocssd_if) {
		SPDK_ERRLOG("Specified bdev %s is not an OCSSD bdev\n", bdev_name);
		cb_fn(-EINVAL, cb_arg);
		return;
	}

	delete_ctx = calloc(1, sizeof(*delete_ctx));
	if (!delete_ctx) {
		SPDK_ERRLOG("Unable to allocate deletion context\n");
		cb_fn(-ENOMEM, cb_arg);
		return;
	}

	delete_ctx->cb_fn = cb_fn;
	delete_ctx->cb_arg = cb_arg;

	spdk_bdev_unregister(bdev, bdev_ocssd_unregister_cb, delete_ctx);
}

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
		free(nvme_ctx);
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
	struct nvme_bdev *bdev, *tmp;

	TAILQ_FOREACH_SAFE(bdev, &ns->bdevs, tailq, tmp) {
		spdk_bdev_unregister(&bdev->disk, NULL, NULL);
	}

	free(ns->type_ctx);
	ns->populated = false;
	ns->type_ctx = NULL;
}

SPDK_LOG_REGISTER_COMPONENT("bdev_ocssd", SPDK_LOG_BDEV_OCSSD)
