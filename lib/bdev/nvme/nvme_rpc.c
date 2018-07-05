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

extern TAILQ_HEAD(, nvme_ctrlr)	g_nvme_ctrlrs;

struct nvme_rpc_ctx {
	struct spdk_jsonrpc_request	*jsonrpc_request;
	struct spdk_nvme_rpc_req	req;
	struct spdk_nvme_rpc_resp	resp;
	void				*op_dev;
	struct spdk_nvme_rpc_ops	*ops;
};

struct spdk_nvme_rpc_ops {
	/** Name for the ops being defined. */
	const char *name;

	/**
	 * Function called to check whether name specified device is mastered by
	 * this operator. If yes, return one none-NULL pointer which will
	 * represent the device in subsequent operations.
	 */
	void *(*dev_lookup_func)(const char *name);

	/**
	 * Function called to process admin type nvme-cmd.
	 *
	 * \return 0 on success
	 */
	int (*admin_cmd_func)(void *dev, const struct spdk_nvme_cmd *cmd, void *buf,
			      size_t nbytes, uint32_t timeout_ms, struct nvme_rpc_ctx *ctx);
	/**
	 * Function called to process passthrough io type nvme-cmd.
	 *
	 * \return 0 on success
	 */
	int (*io_cmd_func)(void *dev, const struct spdk_nvme_cmd *cmd, void *buf,
			   size_t nbytes, void *md_buf, size_t md_len, uint32_t timeout_ms,
			   struct nvme_rpc_ctx *ctx);


	/* TODO: consider proper function elements. */
	/* List names of devices mastered by this ops */
	int (*dev_list_func)(void);

	TAILQ_ENTRY(spdk_nvme_rpc_ops) tailq;
};

TAILQ_HEAD(spdk_nvme_rpc_ops_list, spdk_nvme_rpc_ops);
static struct spdk_nvme_rpc_ops_list g_nvme_rpc_ops = TAILQ_HEAD_INITIALIZER(
			g_nvme_rpc_ops);

static void
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

static void
spdk_nvme_rpc_resq_construct(struct spdk_nvme_rpc_resp *resp,
			     struct spdk_nvme_rpc_req *req,
			     uint32_t status, uint32_t result)
{
	resp->resp.status = *(struct spdk_nvme_status *)&status;
	resp->resp.cdw0 = result;
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
	case NVME_IO_CMD:
		ret = ctx->ops->io_cmd_func(ctx->op_dev,
					    req->cmdbuf, req->data, req->data_len, req->md, req->md_len, req->timeout_ms, ctx);
		break;
	default:
		SPDK_WARNLOG("Unknown nvme cmd type\n");
	}

	return ret;
}

struct rpc_nvme_cmd {
	char *name;
	char *type;
	char *data_direction;
	char *cmdbuf;
	uint32_t timeout_ms;
	uint32_t data_len;
	uint32_t metadata_len;
	char *data;
	char *metadata;
};

static void
free_rpc_nvme_cmd(struct rpc_nvme_cmd *req)
{
	free(req->name);
	free(req->type);
	free(req->data_direction);
	free(req->cmdbuf);
	free(req->data);
	free(req->metadata);
}

static const struct spdk_json_object_decoder rpc_nvme_cmd_decoders[] = {
	{"name", offsetof(struct rpc_nvme_cmd, name), spdk_json_decode_string},
	{"type", offsetof(struct rpc_nvme_cmd, type), spdk_json_decode_string},
	{"data_direction", offsetof(struct rpc_nvme_cmd, data_direction), spdk_json_decode_string},
	{"cmdbuf", offsetof(struct rpc_nvme_cmd, cmdbuf), spdk_json_decode_string},
	{"timeout_ms", offsetof(struct rpc_nvme_cmd, timeout_ms), spdk_json_decode_uint32, true},
	{"data_len", offsetof(struct rpc_nvme_cmd, data_len), spdk_json_decode_uint32, true},
	{"metadata_len", offsetof(struct rpc_nvme_cmd, metadata_len), spdk_json_decode_uint32, true},
	{"data", offsetof(struct rpc_nvme_cmd, data), spdk_json_decode_string, true},
	{"metadata", offsetof(struct rpc_nvme_cmd, metadata), spdk_json_decode_string, true},
};

static void
free_nvme_rpc_ctx(struct nvme_rpc_ctx *ctx)
{
	if (ctx) {
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
}

static void
spdk_rpc_nvme_cmd_complete(struct nvme_rpc_ctx *ctx, uint32_t status, uint32_t result)
{
	struct spdk_jsonrpc_request *request = ctx->jsonrpc_request;
	struct spdk_json_write_ctx *w;
	char *text = NULL;
	int ret;

	spdk_nvme_rpc_resq_construct(&ctx->resp, &ctx->req, status, result);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		goto out;
	}

	spdk_json_write_name(w, "resp");
	ret = spdk_base64_urlsafe_encode(&text, NULL, (const void *)&ctx->resp.resp,
					 sizeof(ctx->resp.resp));
	if (ret) {
		goto out;
	}
	spdk_json_write_string(w, text);
	free(text);

	if (ctx->resp.data_len) {
		spdk_json_write_name(w, "data");
		ret = spdk_base64_urlsafe_encode(&text, NULL, ctx->resp.data, ctx->resp.data_len);
		if (ret) {
			goto out;
		}
		spdk_json_write_string(w, text);
		free(text);
	}

	if (ctx->resp.md_len) {
		spdk_json_write_name(w, "metadata");
		ret = spdk_base64_urlsafe_encode(&text, NULL, ctx->resp.md, ctx->resp.md_len);
		if (ret) {
			goto out;
		}
		spdk_json_write_string(w, text);
		free(text);
	}

out:
	spdk_jsonrpc_end_result(request, w);
	free_nvme_rpc_ctx(ctx);
}

static void
spdk_rpc_nvme_cmd(struct spdk_jsonrpc_request *request,
		  const struct spdk_json_val *params)
{
	struct rpc_nvme_cmd req = {};
	struct spdk_nvme_rpc_ops *ops;
	struct nvme_rpc_ctx *ctx;
	char *str;
	int ret;

	if (spdk_json_decode_object(params, rpc_nvme_cmd_decoders,
				    SPDK_COUNTOF(rpc_nvme_cmd_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto out;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("Failed at malloc ctx\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
		goto invalid;
	}

	if (strcmp(req.type, "admin") == 0) {
		ctx->req.cmd_type = NVME_ADMIN_CMD;
	} else if (strcmp(req.type, "io") == 0) {
		ctx->req.cmd_type = NVME_IO_CMD;
	} else {
		SPDK_ERRLOG("Invalid cmd type '%s'\n", req.type);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid cmd type '%s'", req.type);
		goto invalid;
	}

	if (strcmp(req.data_direction, "c2h") == 0) {
		ctx->req.data_direction = SPDK_NVME_DATA_CONTROLLER_TO_HOST;
		ctx->req.data_len = req.data_len;
		ctx->req.md_len = req.metadata_len;

		if (ctx->req.data_len) {
			ctx->req.data = spdk_dma_malloc(ctx->req.data_len, 0x1000, NULL);
			if (!ctx->req.data) {
				SPDK_ERRLOG("Failed at malloc data\n");
				spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
				goto invalid;
			}
		}

		if (ctx->req.md_len) {
			ctx->req.md = spdk_dma_malloc(ctx->req.md_len, 0x1000, NULL);
			if (!ctx->req.md) {
				SPDK_ERRLOG("Failed at malloc md\n");
				spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
				goto invalid;
			}
		}
	} else if (strcmp(req.data_direction, "h2c") == 0) {
		ctx->req.data_direction = SPDK_NVME_DATA_HOST_TO_CONTROLLER;
		if (req.data) {
			ret = spdk_base64_urlsafe_decode((void **)&str, (size_t *)&ctx->req.data_len, req.data);
			if (ret) {
				SPDK_ERRLOG("Invalid data '%s'\n", req.data);
				spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
								     "Invalid data '%s'", req.data);
				goto invalid;
			}
			ctx->req.data = spdk_dma_malloc(ctx->req.data_len, 0x1000, NULL);
			if (!ctx->req.data) {
				free(str);
				SPDK_ERRLOG("Failed at malloc data\n");
				spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
				goto invalid;
			}
			memcpy(ctx->req.data, str, ctx->req.data_len);
			free(str);
		}
		if (req.metadata) {
			ret = spdk_base64_urlsafe_decode((void **)&str, (size_t *)&ctx->req.md_len, req.metadata);
			if (ret) {
				SPDK_ERRLOG("Invalid metadata '%s'\n", req.metadata);
				spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
								     "Invalid metadata '%s'", req.metadata);
				goto invalid;

			}
			ctx->req.md = spdk_dma_malloc(ctx->req.md_len, 0x1000, NULL);
			if (!ctx->req.md) {
				free(str);
				SPDK_ERRLOG("Failed at malloc md\n");
				spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Out of memory");
				goto invalid;
			}
			memcpy(ctx->req.md, str, ctx->req.md_len);
			free(str);
		}
	} else {
		SPDK_ERRLOG("Invalid cmd type '%s'\n", req.type);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid cmd type '%s'", req.type);
		goto invalid;
	}

	ret = spdk_base64_urlsafe_decode((void **)&ctx->req.cmdbuf, NULL, req.cmdbuf);
	if (ret) {
		SPDK_ERRLOG("Invalid cmdbuf '%s'\n", req.cmdbuf);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid cmdbuf '%s'", req.cmdbuf);
		goto invalid;
	}
	ctx->req.timeout_ms = req.timeout_ms;


	TAILQ_FOREACH(ops, &g_nvme_rpc_ops, tailq) {
		ctx->op_dev = ops->dev_lookup_func(req.name);
		if (ctx->op_dev) {
			SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "%s is processed by %s\n", req.name, ops->name);
			ctx->ops = ops;
			break;
		}
	}

	if (!ctx->op_dev) {
		/* no operator has such dev */
		SPDK_ERRLOG("Failed at dev_lookup_func\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid device name");
		goto invalid;
	}

	ctx->jsonrpc_request = request;

	ret = spdk_nvme_rpc_exec(ctx);
	if (ret < 0) {
		SPDK_NOTICELOG("Failed at spdk_nvme_rpc_exec\n");
		spdk_rpc_nvme_cmd_complete(ctx, ret, 0);
	}

out:
	free_rpc_nvme_cmd(&req);
	return;

invalid:
	free_nvme_rpc_ctx(ctx);
	free_rpc_nvme_cmd(&req);
	return;
}
SPDK_RPC_REGISTER("nvme_cmd", spdk_rpc_nvme_cmd, SPDK_RPC_RUNTIME)

struct nvme_rpc_bdev_ctx {
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	struct nvme_rpc_ctx *ctx;
};

static void
nvme_rpc_bdev_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct nvme_rpc_bdev_ctx *bdev_ctx = cb_arg;
	struct nvme_rpc_ctx *ctx = bdev_ctx->ctx;
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

	spdk_rpc_nvme_cmd_complete(ctx, status, 0);
}

static int
nvme_rpc_admin_cmd_bdev(void *dev, const struct spdk_nvme_cmd *cmd,
			void *buf, size_t nbytes, uint32_t timeout_ms, struct nvme_rpc_ctx *ctx)
{
	struct spdk_bdev *bdev = (struct spdk_bdev *)dev;
	struct nvme_rpc_bdev_ctx *bdev_ctx;
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
					    nvme_rpc_bdev_cb, bdev_ctx);

	if (ret < 0) {
		spdk_put_io_channel(bdev_ctx->ch);
		spdk_bdev_close(bdev_ctx->desc);
		free(bdev_ctx);
	}

	return ret;
}

static int
nvme_rpc_io_cmd_bdev(void *dev, const struct spdk_nvme_cmd *cmd,
		     void *buf, size_t nbytes, void *md_buf, size_t md_len,
		     uint32_t timeout_ms, struct nvme_rpc_ctx *ctx)
{
	struct spdk_bdev *bdev = (struct spdk_bdev *)dev;
	struct nvme_rpc_bdev_ctx *bdev_ctx;
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
					    nvme_rpc_bdev_cb, bdev_ctx);

	if (ret < 0) {
		spdk_put_io_channel(bdev_ctx->ch);
		spdk_bdev_close(bdev_ctx->desc);
		free(bdev_ctx);
	}

	return ret;
}

static void *nvme_rpc_dev_lookup_bdev(const char *name)
{
	return (void *)spdk_bdev_get_by_name(name);
}

static struct spdk_nvme_rpc_ops nvme_rpc_ops_bdev = {
	.name = "nvme_rpc_ops_bdev",
	.dev_lookup_func = nvme_rpc_dev_lookup_bdev,
	.admin_cmd_func = nvme_rpc_admin_cmd_bdev,
	.io_cmd_func = nvme_rpc_io_cmd_bdev,
};

SPDK_NVME_RPC_OPS_REGISTER(nvme_rpc_ops_bdev);

static void
nvme_rpc_bdev_nvme_cb(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_rpc_ctx *ctx = (struct nvme_rpc_ctx *)ref;
	uint32_t status, result;
	int sct, sc;

	sct = cpl->status.sct;
	sc = cpl->status.sc;

	status = sct << 8 | sc;
	result = cpl->cdw0;
	if (status) {
		SPDK_NOTICELOG("submit_admin command error: SC %x SCT %x\n", sc, sct);
	}

	spdk_rpc_nvme_cmd_complete(ctx, status, result);
}

static int
nvme_rpc_admin_cmd_bdev_nvme(void *dev, const struct spdk_nvme_cmd *cmd,
			     void *buf, size_t nbytes, uint32_t timeout_ms, struct nvme_rpc_ctx *ctx)
{
	struct nvme_ctrlr *nvme_ctrlr = (struct nvme_ctrlr *)dev;
	int ret;

	ret = spdk_nvme_ctrlr_cmd_admin_raw(nvme_ctrlr->ctrlr, (struct spdk_nvme_cmd *)cmd, buf,
					    (uint32_t)nbytes, nvme_rpc_bdev_nvme_cb, ctx);

	return ret;
}

static int
nvme_rpc_io_cmd_bdev_nvme(void *dev, const struct spdk_nvme_cmd *cmd,
			  void *buf, size_t nbytes, void *md_buf, size_t md_len,
			  uint32_t timeout_ms, struct nvme_rpc_ctx *ctx)
{
	return -1;
}

static void *
nvme_rpc_dev_lookup_bdev_nvme(const char *name)
{
	struct nvme_ctrlr	*nvme_ctrlr;

	TAILQ_FOREACH(nvme_ctrlr, &g_nvme_ctrlrs, tailq) {
		if (strcmp(name, nvme_ctrlr->name) == 0) {
			return (void *)nvme_ctrlr;
		}
	}

	return NULL;
}

static struct spdk_nvme_rpc_ops nvme_rpc_ops_bdev_nvme = {
	.name = "nvme_rpc_ops_bdev_nvme",
	.dev_lookup_func = nvme_rpc_dev_lookup_bdev_nvme,
	.admin_cmd_func = nvme_rpc_admin_cmd_bdev_nvme,
	.io_cmd_func = nvme_rpc_io_cmd_bdev_nvme,
};

SPDK_NVME_RPC_OPS_REGISTER(nvme_rpc_ops_bdev_nvme);
