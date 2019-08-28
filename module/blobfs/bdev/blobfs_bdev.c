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
#include "spdk/blobfs.h"
#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/event.h"
#include "spdk/blob_bdev.h"
#include "spdk/blobfs_bdev.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

/* Dummy bdev module used to to claim bdevs. */
static struct spdk_bdev_module blobfs_bdev_module = {
	.name	= "blobfs",
};

static void
blobfs_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
		     void *event_ctx)
{
	SPDK_WARNLOG("Async event(%d) is triggered in bdev %s\n", type, spdk_bdev_get_name(bdev));
}

struct blobfs_bdev_operation_ctx {
	const char *bdev_name;
	struct spdk_filesystem *fs;

	spdk_blobfs_bdev_op_complete cb_fn;
	void *cb_arg;
};

static void
_blobfs_bdev_unload_cb(void *_ctx, int fserrno)
{
	struct blobfs_bdev_operation_ctx *ctx = _ctx;

	if (fserrno) {
		SPDK_ERRLOG("Failed to unload blobfs on bdev %s: errno %d\n", ctx->bdev_name, fserrno);
	}

	ctx->cb_fn(ctx->cb_arg, fserrno);
	free(ctx);
}

static void
blobfs_bdev_unload(void *_ctx)
{
	struct blobfs_bdev_operation_ctx *ctx = _ctx;

	spdk_fs_unload(ctx->fs, _blobfs_bdev_unload_cb, ctx);
}

static void
blobfs_bdev_load_cb_to_unload(void *_ctx, struct spdk_filesystem *fs, int fserrno)
{
	struct blobfs_bdev_operation_ctx *ctx = _ctx;

	if (fserrno) {
		ctx->cb_fn(ctx->cb_arg, fserrno);
		free(ctx);
		return;
	}

	ctx->fs = fs;
	spdk_thread_send_msg(spdk_get_thread(), blobfs_bdev_unload, ctx);
}

int
spdk_blobfs_bdev_detect(const char *bdev_name,
			spdk_blobfs_bdev_op_complete cb_fn, void *cb_arg)
{
	struct blobfs_bdev_operation_ctx *ctx;
	struct spdk_bs_dev *bs_dev;
	struct spdk_bdev_desc *desc;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate ctx.\n");
		return -ENOMEM;
	}

	ctx->bdev_name = bdev_name;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	rc = spdk_bdev_open_ext(bdev_name, true, blobfs_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		SPDK_INFOLOG(SPDK_LOG_BLOBFS, "Failed to open bdev(%s): %s\n", ctx->bdev_name, spdk_strerror(rc));

		goto invalid;
	}

	bs_dev = spdk_bdev_create_bs_dev_from_desc(desc);
	if (bs_dev == NULL) {
		SPDK_INFOLOG(SPDK_LOG_BLOBFS,  "Failed to create a blobstore block device from bdev desc");
		rc = -ENOMEM;
		spdk_bdev_close(desc);

		goto invalid;
	}

	spdk_fs_load(bs_dev, NULL, blobfs_bdev_load_cb_to_unload, ctx);

	return 0;

invalid:
	free(ctx);

	return rc;
}

int
spdk_blobfs_bdev_create(const char *bdev_name, uint32_t cluster_sz,
			spdk_blobfs_bdev_op_complete cb_fn, void *cb_arg)
{
	struct blobfs_bdev_operation_ctx *ctx;
	struct spdk_blobfs_opts blobfs_opt;
	struct spdk_bs_dev *bs_dev;
	struct spdk_bdev_desc *desc;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate ctx.\n");
		return -ENOMEM;
	}

	ctx->bdev_name = bdev_name;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	/* Creation requires WRITE operation */
	rc = spdk_bdev_open_ext(bdev_name, true, blobfs_bdev_event_cb, NULL, &desc);
	if (rc != 0) {
		SPDK_INFOLOG(SPDK_LOG_BLOBFS, "Failed to open bdev(%s): %s\n", ctx->bdev_name, spdk_strerror(rc));

		goto invalid;
	}

	bs_dev = spdk_bdev_create_bs_dev_from_desc(desc);
	if (bs_dev == NULL) {
		SPDK_INFOLOG(SPDK_LOG_BLOBFS,  "Failed to create a blobstore block device from bdev desc\n");
		rc = -ENOMEM;
		spdk_bdev_close(desc);

		goto invalid;
	}

	rc = spdk_bs_bdev_claim(bs_dev, &blobfs_bdev_module);
	if (rc) {
		goto invalid;
	}

	spdk_fs_opts_init(&blobfs_opt);
	if (cluster_sz) {
		blobfs_opt.cluster_sz = cluster_sz;
	}

	spdk_fs_init(bs_dev, NULL, NULL, blobfs_bdev_load_cb_to_unload, ctx);

	return 0;

invalid:
	free(ctx);

	return rc;
}
