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

/*
 * This is a simple example of a virtual block device that takes a single
 * bdev and slices it into multiple smaller bdevs.
 */


#include "spdk/stdinc.h"

#include "spdk/rpc.h"
#include "spdk/conf.h"
#include "spdk/endian.h"
#include "spdk/io_channel.h"
#include "spdk/lvol.h"
#include "spdk/blob.h"
#include "spdk/blob_bdev.h"

#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

static TAILQ_HEAD(, spdk_lvol_store) g_spdk_lvol_stores = TAILQ_HEAD_INITIALIZER(
			g_spdk_lvol_stores);

/* TODO:
static struct spdk_bdev_fn_table vbdev_lvol_fn_table = {
	.destruct		= vbdev_lvol_destruct,
	.io_type_supported	= vbdev_lvol_io_type_supported,
	.submit_request		= vbdev_lvol_submit_request,
	.get_io_channel		= vbdev_lvol_get_io_channel,
	.dump_config_json	= vbdev_lvol_dump_config_json,
};
 */

static void
vbdev_lvol_store_free_cb(void *cb_arg, struct spdk_lvol_store *lvol_store, int bserrno)
{
	struct spdk_lvol_store_rpc_req *rpc = cb_arg;
	struct spdk_bdev *base_bdev = lvol_store->base_bdev;

	TAILQ_REMOVE(&g_spdk_lvol_stores, lvol_store, lvol_stores);
	free(lvol_store);
	spdk_bdev_unclaim(base_bdev);
	SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL, "lvol store bdev deleted\n");
	rpc->rpc_fn(rpc->cb_arg, 0);
}

static void
vbdev_lvol_store_op_complete_cb(void *cb_arg, struct spdk_lvol_store *lvol_store, int bserrno)
{
	struct spdk_lvol_store_rpc_req *rpc = cb_arg;

	if (bserrno != 0) {
		if (rpc != NULL && rpc->rpc_fn != NULL)
			rpc->rpc_fn(rpc, -1);
		//bs_dev->destroy(bs_dev);
		return;
	}
	TAILQ_INSERT_TAIL(&g_spdk_lvol_stores, lvol_store, lvol_stores);
	SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL, "lvol store bdev inserted\n");
	strcpy(lvol_store->guid, "one");

	if (rpc != NULL && rpc->rpc_fn != NULL) {
		rpc->lvol_store = lvol_store;
		rpc->rpc_fn(rpc, 0);
	}

	return;
}

int
vbdev_construct_lvol_store(struct spdk_bdev *base_bdev, struct spdk_lvol_store_rpc_req *rpc)
{
	struct spdk_bs_dev *bs_dev;

	if (!spdk_bdev_claim(base_bdev, NULL, NULL)) {
		SPDK_ERRLOG("Lvol store bdev %s is already claimed\n", spdk_bdev_get_name(base_bdev));
		return -1;
	}
	if (rpc == NULL) {
		rpc = calloc(1, sizeof(*rpc));
		rpc->base_bdev = base_bdev;
		rpc->rpc_fn = NULL;
	}

	bs_dev = spdk_bdev_create_bs_dev(base_bdev);
	if (!bs_dev) {
		spdk_bdev_unclaim(base_bdev);
		return -1;
	}

	if (lvol_store_initialize(bs_dev, vbdev_lvol_store_op_complete_cb, rpc)) {
		spdk_bdev_unclaim(base_bdev);
		return -1;
	}
	return 0;
}

static void
vbdev_destruct_lvol_store(struct spdk_lvol_store *lvol_store, struct spdk_lvol_store_rpc_req *rpc)
{
	lvol_store_free(lvol_store, vbdev_lvol_store_free_cb, rpc);
	return;
}

static void
vbdev_lvol_store_init(void)
{
	struct spdk_conf_section *sp;
	const char *base_bdev_name;
	int i, rc = 0;
	struct spdk_bdev *base_bdev;

	sp = spdk_conf_find_section(NULL, "Lvol");
	if (sp == NULL) {
		rc = 0;
		goto end;
	}

	for (i = 0; ; i++) {
		if (!spdk_conf_section_get_nval(sp, "Split", i)) {
			break;
		}

		base_bdev_name = spdk_conf_section_get_nmval(sp, "Split", i, 0);
		if (!base_bdev_name) {
			SPDK_ERRLOG("lvol configuration missing blockdev name\n");
			rc = -1;
			goto end;
		}

		base_bdev = spdk_bdev_get_by_name(base_bdev_name);
		if (!base_bdev) {
			SPDK_ERRLOG("Could not find lvol bdev %s\n", base_bdev_name);
			rc = -1;
			goto end;
		}
		vbdev_construct_lvol_store(base_bdev, NULL);
	}

end:
	spdk_vbdev_module_init_next(rc);
}

static void
vbdev_lvol_store_fini(void)
{
	struct spdk_lvol_store *spdk_lvol_store, *tmp;

	TAILQ_FOREACH_SAFE(spdk_lvol_store, &g_spdk_lvol_stores, lvol_stores, tmp) {
		vbdev_destruct_lvol_store(spdk_lvol_store, NULL);
	}
}

SPDK_VBDEV_MODULE_REGISTER(vbdev_lvol_store_init, vbdev_lvol_store_fini, NULL, NULL, NULL)
SPDK_LOG_REGISTER_TRACE_FLAG("vbdev_lvol", SPDK_TRACE_VBDEV_LVOL)
