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

#include "bdev_nvme.h"
#include "nvme_cmd_rpc.h"

extern TAILQ_HEAD(, nvme_ctrlr)	g_nvme_ctrlrs;

static void
nvme_cmd_rpc_bdev_nvme_cb(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_cmd_rpc_ctx *ctx = (struct nvme_cmd_rpc_ctx *)ref;
	uint32_t status, result;
	int sct, sc;

	sct = cpl->status.sct;
	sc = cpl->status.sc;

	status = sct << 8 | sc;
	result = cpl->cdw0;
	if (status) {
		SPDK_NOTICELOG("submit_admin command error: SC %x SCT %x\n", sc, sct);
	}

	spdk_nvme_cmd_rpc_complete(ctx, status, result);
}

static int
nvme_cmd_rpc_admin_cmd_bdev_nvme(void *dev, const struct spdk_nvme_cmd *cmd,
				 void *buf, size_t nbytes, uint32_t timeout_ms, struct nvme_cmd_rpc_ctx *ctx)
{
	struct nvme_ctrlr *nvme_ctrlr = (struct nvme_ctrlr *)dev;
	int ret;

	ret = spdk_nvme_ctrlr_cmd_admin_raw(nvme_ctrlr->ctrlr, (struct spdk_nvme_cmd *)cmd, buf,
					    (uint32_t)nbytes, nvme_cmd_rpc_bdev_nvme_cb, ctx);

	return ret;
}

static int
nvme_cmd_rpc_io_raw_cmd_bdev_nvme(void *dev, const struct spdk_nvme_cmd *cmd,
				  void *buf, size_t nbytes, void *md_buf, size_t md_len,
				  uint32_t timeout_ms, struct nvme_cmd_rpc_ctx *ctx)
{
	return -1;
}

static void *
nvme_cmd_rpc_dev_hit_bdev_nvme(const char *name)
{
	struct nvme_ctrlr	*nvme_ctrlr;

	TAILQ_FOREACH(nvme_ctrlr, &g_nvme_ctrlrs, tailq) {
		if (strcmp(name, nvme_ctrlr->name) == 0) {
			return (void *)nvme_ctrlr;
		}
	}

	return NULL;
}

static struct spdk_nvme_cmd_rpc_operator nvme_cmd_operator_bdev_nvme = {
	.dev_hit_func = nvme_cmd_rpc_dev_hit_bdev_nvme,
	.admin_cmd_func = nvme_cmd_rpc_admin_cmd_bdev_nvme,
	.io_raw_cmd_func = nvme_cmd_rpc_io_raw_cmd_bdev_nvme,
};

SPDK_NVME_CMD_RPC_OPERATOR_REGISTER(nvme_cmd_operator_bdev_nvme);
