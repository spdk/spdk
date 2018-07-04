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
#include "spdk/string.h"

#include "spdk_internal/log.h"
#include "spdk/bdev_module.h"

#include "nvme_cmd_rpc.h"

struct nvme_cmd_rpc_bdev_ctx {
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	struct nvme_cmd_rpc_ctx *ctx;
};

static void
nvme_cmd_rpc_bdev_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct nvme_cmd_rpc_bdev_ctx *bdev_ctx = cb_arg;
	struct nvme_cmd_rpc_ctx *ctx = bdev_ctx->ctx;
	uint32_t status;
	int sct, sc;

	if (success) {
		status = 0;
	} else {
		spdk_bdev_io_get_nvme_status(bdev_io, &sct, &sc);
		status = sct << 8 | sc;
		SPDK_NOTICELOG("submit_admin command error: SC %x SCT %x\n", sc, sct);
	}

	spdk_bdev_free_io(bdev_io);
	spdk_put_io_channel(bdev_ctx->ch);
	spdk_bdev_close(bdev_ctx->desc);
	free(bdev_ctx);

	spdk_nvme_cmd_rpc_complete(ctx, status, 0);
}

static int
nvme_cmd_rpc_admin_cmd_bdev(void *dev, const struct spdk_nvme_cmd *cmd,
			    void *buf, size_t nbytes, uint32_t timeout_ms, struct nvme_cmd_rpc_ctx *ctx)
{
	struct spdk_bdev *bdev = (struct spdk_bdev *)dev;
	struct nvme_cmd_rpc_bdev_ctx *bdev_ctx;
	int ret = -1;

	bdev_ctx = malloc(sizeof(*bdev_ctx));
	if (!bdev_ctx) {
		return -1;
	}
	bdev_ctx->ctx = ctx;

	ret = spdk_bdev_open(bdev, true, NULL, NULL, &bdev_ctx->desc);
	if (ret) {
		free(bdev_ctx);
		return -1;
	}

	bdev_ctx->ch = spdk_bdev_get_io_channel(bdev_ctx->desc);
	if (!bdev_ctx->ch) {
		spdk_bdev_close(bdev_ctx->desc);
		free(bdev_ctx);
		return -1;
	}

	ret = spdk_bdev_nvme_admin_passthru(bdev_ctx->desc, bdev_ctx->ch,
					    cmd, buf, nbytes,
					    nvme_cmd_rpc_bdev_cb, bdev_ctx);

	if (ret < 0) {
		spdk_put_io_channel(bdev_ctx->ch);
		spdk_bdev_close(bdev_ctx->desc);
		free(bdev_ctx);
	}

	return ret;
}

static int
nvme_cmd_rpc_io_raw_cmd_bdev(void *dev, const struct spdk_nvme_cmd *cmd,
			     void *buf, size_t nbytes, void *md_buf, size_t md_len,
			     uint32_t timeout_ms, struct nvme_cmd_rpc_ctx *ctx)
{
	struct spdk_bdev *bdev = (struct spdk_bdev *)dev;
	struct nvme_cmd_rpc_bdev_ctx *bdev_ctx;
	int ret = -1;

	bdev_ctx = malloc(sizeof(*bdev_ctx));
	if (!bdev_ctx) {
		return -1;
	}
	bdev_ctx->ctx = ctx;

	ret = spdk_bdev_open(bdev, true, NULL, NULL, &bdev_ctx->desc);
	if (ret) {
		free(bdev_ctx);
		return -1;
	}

	bdev_ctx->ch = spdk_bdev_get_io_channel(bdev_ctx->desc);
	if (!bdev_ctx->ch) {
		spdk_bdev_close(bdev_ctx->desc);
		free(bdev_ctx);
		return -1;
	}

	ret = spdk_bdev_nvme_io_passthru_md(bdev_ctx->desc, bdev_ctx->ch,
					    cmd, buf, nbytes, md_buf, md_len,
					    nvme_cmd_rpc_bdev_cb, bdev_ctx);

	if (ret < 0) {
		spdk_put_io_channel(bdev_ctx->ch);
		spdk_bdev_close(bdev_ctx->desc);
		free(bdev_ctx);
	}

	return ret;
}

static void *nvme_cmd_rpc_dev_hit_bdev(const char *name)
{
	return (void *)spdk_bdev_get_by_name(name);
}

static struct spdk_nvme_cmd_rpc_operator nvme_cmd_operator_bdev = {
	.dev_hit_func = nvme_cmd_rpc_dev_hit_bdev,
	.admin_cmd_func = nvme_cmd_rpc_admin_cmd_bdev,
	.io_raw_cmd_func = nvme_cmd_rpc_io_raw_cmd_bdev,
};

SPDK_NVME_CMD_RPC_OPERATOR_REGISTER(nvme_cmd_operator_bdev);
