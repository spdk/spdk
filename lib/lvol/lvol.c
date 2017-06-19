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

#include "spdk_internal/lvolstore.h"
#include "spdk_internal/log.h"

static void
spdk_lvs_init_cb(void *cb_arg, struct spdk_blob_store *bs, int lvserrno)
{
	struct spdk_lvol_store_req *lvs_req = cb_arg;
	struct spdk_lvol_store *lvs = lvs_req->u.lvol_handle.lvol_store;

	if (lvserrno != 0) {
		assert(bs == NULL);
		SPDK_ERRLOG("lvol store init failed: could not initialize blobstore\n");
		free(lvs);
		lvs = NULL;
	} else {
		assert(bs != NULL);
		lvs->blobstore = bs;

		SPDK_TRACELOG(SPDK_TRACE_LVOL, "lvol store initialized from bdev\n");
	}
	lvs_req->u.lvol_handle.cb_fn(lvs_req->u.lvol_handle.cb_arg, lvs, lvserrno);
	free(lvs_req);
}

static void
spdk_lvs_unload_cb(void *cb_arg, int lvserrno)
{
	struct spdk_lvol_store_req *lvs_req = cb_arg;

	SPDK_TRACELOG(SPDK_TRACE_LVOL, "lvol store removed from bdev\n");
	lvs_req->u.lvol_basic.cb_fn(lvs_req->u.lvol_basic.cb_arg, lvserrno);
	free(lvs_req);
}

int
spdk_lvs_init(struct spdk_bs_dev *bs_dev, spdk_lvs_op_with_handle_complete cb_fn,
	      void *cb_arg)
{
	struct spdk_lvol_store *lvs;
	struct spdk_lvol_store_req *lvs_req;

	lvs = calloc(1, sizeof(*lvs));
	if (!lvs) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store base pointer\n");
		return -1;
	}

	lvs->guid = SPDK_GPT_GUID(0, 0, 0, 0, 0);

	lvs_req = calloc(1, sizeof(*lvs_req));
	if (!lvs_req) {
		free(lvs);
		SPDK_ERRLOG("Cannot alloc memory for lvol store request pointer\n");
		return -1;
	}

	lvs_req->u.lvol_handle.cb_fn = cb_fn;
	lvs_req->u.lvol_handle.cb_arg = cb_arg;
	lvs_req->u.lvol_handle.lvol_store = lvs;
	lvs->bs_dev = bs_dev;

	SPDK_TRACELOG(SPDK_TRACE_LVOL, "Loading lvol store from bdev\n");
	spdk_bs_init(bs_dev, NULL, spdk_lvs_init_cb, lvs_req);

	return 0;
}

int
spdk_lvs_unload(struct spdk_lvol_store *lvs, spdk_lvs_op_complete cb_fn,
		void *cb_arg)
{
	struct spdk_lvol_store_req *lvs_req;

	if (lvs == NULL) {
		SPDK_ERRLOG("Lvol store is NULL\n");
		return -1;
	}

	lvs_req = calloc(1, sizeof(*lvs_req));
	if (!lvs_req) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store request pointer\n");
		return -1;
	}

	lvs_req->u.lvol_basic.cb_fn = cb_fn;
	lvs_req->u.lvol_basic.cb_arg = cb_arg;

	SPDK_TRACELOG(SPDK_TRACE_LVOL, "Freeing lvol store from bdev\n");
	spdk_bs_unload(lvs->blobstore, spdk_lvs_unload_cb, lvs_req);
	free(lvs);

	return 0;
}

SPDK_LOG_REGISTER_TRACE_FLAG("lvol", SPDK_TRACE_LVOL)
