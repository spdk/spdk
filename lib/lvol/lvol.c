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

#include "spdk/lvol.h"
#include "spdk/blob.h"
#include "spdk/log.h"

#include "spdk_internal/log.h"

static void
lvol_store_init_cb(void *cb_arg, struct spdk_blob_store *bs, int bserrno)
{
	struct spdk_lvol_store_req *lvs_req = cb_arg;
	struct spdk_lvol_store *lvol_store = lvs_req->lvol_store;

	if (bserrno != 0) {
		SPDK_ERRLOG("Cannot initialize lvol store\n");
		free(lvs_req);
		return;
	}
	SPDK_TRACELOG(SPDK_TRACE_LVOL, "lvol store initialized from bdev\n");
	lvol_store->blobstore = bs;
	lvs_req->cb_fn(cb_arg, lvol_store, bserrno);
	free(lvs_req);

	return;
}

/*
static void
lvol_store_load_cb(void *cb_arg, struct spdk_blob_store *bs, int bserrno)
{
	struct spdk_lvol_store_req *lvs_req = cb_arg;

	if (bserrno != 0) {
		SPDK_ERRLOG("Cannot load lvol store\n");
		SPDK_TRACELOG(SPDK_TRACE_LVOL, "initializing lvol store\n");
		spdk_bs_init(lvs_req->bs_dev, NULL, lvol_store_init_cb, lvs_req);
		free(lvs_req->lvol_store);
		free(lvs_req);
		return;
	}

	SPDK_TRACELOG(SPDK_TRACE_LVOL, "lvol store loaded from bdev\n");
	lvs_req->lvol_store->blobstore = bs;
	lvs_req->cb_fn(cb_arg, lvs_req->lvol_store, bserrno);
	free(lvs_req);

	return;
}
 */

int
lvol_store_initialize(struct spdk_bs_dev *bs_dev, spdk_lvol_store_op_complete cb_fn, void *cb_arg)
{
	struct spdk_lvol_store *new_lvol_store;
	struct spdk_lvol_store_req *lvs_req;

	new_lvol_store = calloc(1, sizeof(*new_lvol_store));
	if (!new_lvol_store) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store base pointer\n");
		return -1;
	}

	lvs_req = calloc(1, sizeof(*lvs_req));
	if (!new_lvol_store) {
		SPDK_ERRLOG("Cannot alloc memory for lvol store request pointer\n");
		return -1;
	}

	lvs_req->cb_fn = cb_fn;
	lvs_req->cb_arg = cb_arg;
	lvs_req->lvol_store = new_lvol_store;
	lvs_req->bs_dev = bs_dev;

	SPDK_TRACELOG(SPDK_TRACE_LVOL, "Loading lvol store from bdev\n");
	//spdk_bs_load(bs_dev, lvol_store_load_cb, lvs_req);
	spdk_bs_init(lvs_req->bs_dev, NULL, lvol_store_init_cb, lvs_req);

	return 0;
}

SPDK_LOG_REGISTER_TRACE_FLAG("lvol", SPDK_TRACE_LVOL)
