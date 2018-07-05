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
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/bdev_module.h"
#include "spdk_internal/log.h"

#include "bdev_nvme.h"

#include "spdk/base64.h"
#include "spdk/nvme_msg.h"

struct nvme_rpc_ctx {
	struct spdk_jsonrpc_request	*jsonrpc_request;
	struct spdk_nvme_rpc_req	req;
	struct spdk_nvme_rpc_resp	resp;
	void				*op_dev;
	struct spdk_nvme_rpc_ops	*ops;
};

struct spdk_nvme_rpc_ops {
	/* Search whether name specified device is mastered by operator */
	void *(*dev_lookup_func)(const char *name);
	/* Process admin type nvme-cmd */
	int (*admin_cmd_func)(void *dev, const struct spdk_nvme_cmd *cmd, void *buf,
			      size_t nbytes, uint32_t timeout_ms, struct nvme_rpc_ctx *ctx);
	/* Process passthrough io type nvme-cmd */
	int (*io_raw_cmd_func)(void *dev, const struct spdk_nvme_cmd *cmd, void *buf,
			       size_t nbytes, void *md_buf, size_t md_len, uint32_t timeout_ms,
			       struct nvme_rpc_ctx *ctx);

	TAILQ_ENTRY(spdk_nvme_rpc_ops) tailq;
};

TAILQ_HEAD(spdk_nvme_rpc_ops_list, spdk_nvme_rpc_ops);
static struct spdk_nvme_rpc_ops_list g_nvme_rpc_ops = TAILQ_HEAD_INITIALIZER(
			g_nvme_rpc_ops);

void spdk_add_nvme_rpc_ops(struct spdk_nvme_rpc_ops *ops);

void
spdk_add_nvme_rpc_ops(struct spdk_nvme_rpc_ops *ops)
{
	TAILQ_INSERT_TAIL(&g_nvme_rpc_ops, ops, tailq);
}

/**
 * \brief Register a new spdk_nvme_rpc_ops
 */
#define SPDK_NVME_RPC_OPS_REGISTER(_name) \
	__attribute__((constructor)) static void _name ## _register(void)	\
	{									\
		spdk_add_nvme_rpc_ops(&_name);					\
	}

static int
spdk_nvme_rpc_req_unmarshal(struct spdk_nvme_rpc_req *req, char *text)
{
	uint32_t text_strlen, binary_len, binary_len_ex, len_required;
	uint8_t data_direction;
	char *binary, *bin_step;
	int ret;

	text_strlen = strlen(text);
	binary_len_ex = spdk_base64_get_bin_len_extension(text_strlen);

	binary = malloc(binary_len_ex);
	if (!binary) {
		return -1;
	}

	ret = spdk_base64_urlsafe_decode(binary, &binary_len, text, text_strlen);
	if (ret) {
		free(binary);
		return -1;
	}

	/* req header */
	if (binary_len < NVME_RPC_REQ_HEAD_LEN) {
		free(binary);
		return -1;
	}
	memcpy(req, binary, NVME_RPC_REQ_HEAD_LEN);

	/* req cmdbuf */
	data_direction = req->data_direction;
	len_required = NVME_RPC_REQ_HEAD_LEN + req->cmdbuf_len;
	if (data_direction == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		len_required += req->data_len + req->md_len;
	}
	if (len_required != binary_len) {
		free(binary);
		return -1;
	}

	bin_step = binary + NVME_RPC_REQ_HEAD_LEN;
	if (req->cmdbuf_len) {
		req->cmdbuf = malloc(req->cmdbuf_len);
		if (!req->cmdbuf) {
			free(binary);
			return -1;
		}
		memcpy(req->cmdbuf, bin_step, req->cmdbuf_len);
		bin_step += req->cmdbuf_len;
	}

	/* req data & md */
	if (req->data_len) {
		req->data = spdk_dma_malloc(req->data_len, 0x1000, NULL);
		if (!req->data) {
			free(req->cmdbuf);
			free(binary);
			return -1;
		}
		if (data_direction == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
			memcpy(req->data, bin_step, req->data_len);
			bin_step += req->data_len;
		}
	}
	if (req->md_len) {
		req->md = spdk_dma_malloc(req->md_len, 0x1000, NULL);
		if (!req->md) {
			spdk_dma_free(req->data);
			free(req->cmdbuf);
			free(binary);
			return -1;
		}
		if (data_direction == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
			memcpy(req->data, bin_step, req->md_len);
		}
	}

	free(binary);
	return 0;
}

static int
spdk_nvme_rpc_resp_marshal(char **_text, struct spdk_nvme_rpc_resp *resp)
{
	uint32_t text_len, binary_len;
	char *binary, *bin_step, *text;

	binary_len = NVME_RPC_RESP_HEAD_LEN + resp->data_len + resp->md_len;
	/* the terminating null byte '\0' */
	text_len = spdk_base64_get_text_strlen(binary_len) + 1;

	binary = malloc(binary_len);
	if (!binary) {
		return -1;
	}
	text = malloc(text_len);
	if (!text) {
		free(binary);
		return -1;
	}

	memcpy(binary, resp, NVME_RPC_RESP_HEAD_LEN);

	bin_step = binary + NVME_RPC_RESP_HEAD_LEN;
	if (resp->data_len) {
		memcpy(bin_step, resp->data, resp->data_len);
		bin_step += resp->data_len;
	}
	if (resp->md_len) {
		memcpy(bin_step, resp->data, resp->data_len);
	}
	spdk_base64_urlsafe_encode(text, binary, binary_len);

	free(binary);
	*_text = text;
	return 0;
}

static void
spdk_nvme_rpc_resq_construct(struct spdk_nvme_rpc_resp *resp,
			     struct spdk_nvme_rpc_req *req,
			     uint32_t status, uint32_t result)
{
	resp->status = status;
	resp->result = result;
	if (req->data_direction == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		resp->data_len = req->data_len;
		resp->md_len = req->md_len;
		resp->data = req->data;
		resp->md = req->md;
	}
}

static int
spdk_nvme_rpc_exec(struct nvme_rpc_ctx *ctx)
{
	struct spdk_nvme_rpc_req *req = &ctx->req;
	int ret = -1;

	switch (req->cmd_type) {
	case NVME_ADMIN_CMD:
		ret = ctx->ops->admin_cmd_func(ctx->op_dev,
					       req->cmdbuf, req->data, req->data_len, req->timeout_ms, ctx);
		break;
	case NVME_IO_RAW_CMD:
		ret = ctx->ops->io_raw_cmd_func(ctx->op_dev,
						req->cmdbuf, req->data, req->data_len, req->md, req->md_len, req->timeout_ms, ctx);
		break;
	default:
		SPDK_WARNLOG("Unknown nvme cmd type\n");
	}

	return ret;
}

struct rpc_nvme_cmd {
	char *name;
	char *cmd_text;
};

static void
free_rpc_nvme_cmd(struct rpc_nvme_cmd *req)
{
	free(req->name);
	free(req->cmd_text);
}

static const struct spdk_json_object_decoder rpc_nvme_cmd_decoders[] = {
	{"name", offsetof(struct rpc_nvme_cmd, name), spdk_json_decode_string},
	{"cmd_text", offsetof(struct rpc_nvme_cmd, cmd_text), spdk_json_decode_string},
};

static void
spdk_rpc_nvme_cmd_complete(struct nvme_rpc_ctx *ctx, uint32_t status, uint32_t result)
{
	struct spdk_jsonrpc_request *request = ctx->jsonrpc_request;
	struct spdk_json_write_ctx *w;
	char *text = NULL;
	int ret;

	spdk_nvme_rpc_resq_construct(&ctx->resp, &ctx->req, status, result);

	ret = spdk_nvme_rpc_resp_marshal(&text, &ctx->resp);
	if (ret) {
		SPDK_ERRLOG("Failed at marshalling nvme-cmd-rpc-resp\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal Error");
		goto out;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		SPDK_ERRLOG("Failed at spdk_jsonrpc_begin_result\n");
		goto out;
	}
	spdk_json_write_string(w, text);
	spdk_jsonrpc_end_result(request, w);

out:
	free(text);
	if (ctx->req.cmdbuf) {
		free(ctx->req.cmdbuf);
	}
	if (ctx->req.data) {
		spdk_dma_free(ctx->req.data);
	}
	if (ctx->req.md) {
		spdk_dma_free(ctx->req.md);
	}
	free(ctx);
}

static void
spdk_rpc_nvme_cmd(struct spdk_jsonrpc_request *request,
		  const struct spdk_json_val *params)
{
	struct rpc_nvme_cmd req = {};
	struct spdk_nvme_rpc_ops *ops;
	struct nvme_rpc_ctx *ctx;
	int ret;

	if (params && spdk_json_decode_object(params, rpc_nvme_cmd_decoders,
					      SPDK_COUNTOF(rpc_nvme_cmd_decoders),
					      &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto out;
	}

	if (!req.name) {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "Lost nvme device name\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto out;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("Failed at malloc ctx\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		goto out;
	}

	TAILQ_FOREACH(ops, &g_nvme_rpc_ops, tailq) {
		ctx->op_dev = ops->dev_lookup_func(req.name);
		if (ctx->op_dev) {
			ctx->ops = ops;
			break;
		}
	}

	if (!ctx->op_dev) {
		free(ctx);
		/* no operator has such dev */
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "Failed at dev_lookup_func\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid device name");
		goto out;
	}

	ctx->jsonrpc_request = request;
	ret = spdk_nvme_rpc_req_unmarshal(&ctx->req, req.cmd_text);
	if (ret) {
		free(ctx);
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "Failed at spdk_nvme_rpc_req_unmarshal\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto out;
	}

	ret = spdk_nvme_rpc_exec(ctx);
	if (ret < 0) {
		SPDK_NOTICELOG("Failed at spdk_nvme_rpc_exec\n");
		spdk_rpc_nvme_cmd_complete(ctx, ret, 0);
	}

out:
	free_rpc_nvme_cmd(&req);
	return;
}
SPDK_RPC_REGISTER("nvme_cmd", spdk_rpc_nvme_cmd, SPDK_RPC_RUNTIME)
