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

enum spdk_nvme_rpc_type {
	NVME_ADMIN_CMD = 0,
	NVME_IO_CMD,
};

struct rpc_nvme_cmd_ctx;

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
			      size_t nbytes, uint32_t timeout_ms, struct rpc_nvme_cmd_ctx *ctx);
	/**
	 * Function called to process passthrough io type nvme-cmd.
	 *
	 * \return 0 on success
	 */
	int (*io_cmd_func)(void *dev, const struct spdk_nvme_cmd *cmd, void *buf,
			   size_t nbytes, void *md_buf, size_t md_len, uint32_t timeout_ms,
			   struct rpc_nvme_cmd_ctx *ctx);

	/* TODO: consider proper function elements. */
	/* List names of devices mastered by this ops */
	int (*dev_list_func)(void);

	TAILQ_ENTRY(spdk_nvme_rpc_ops) tailq;
};

struct rpc_nvme_cmd_req {
	char	*name;
	int	cmd_type;
	int	data_direction;
	uint32_t	timeout_ms;
	uint32_t	data_len;
	uint32_t	md_len;

	struct spdk_nvme_cmd	*cmdbuf;
	char		*data;
	char		*md;
};

struct rpc_nvme_cmd_resp {
	char	*cpl_text;
	char	*data_text;
	char	*md_text;
};

struct rpc_nvme_cmd_ctx {
	struct spdk_jsonrpc_request	*jsonrpc_request;
	struct rpc_nvme_cmd_req		req;
	struct rpc_nvme_cmd_resp	resp;
	void				*op_dev;
	struct spdk_nvme_rpc_ops	*ops;
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

static void
free_rpc_nvme_cmd_ctx(struct rpc_nvme_cmd_ctx *ctx)
{
	if (ctx) {
		free(ctx->req.name);
		free(ctx->req.cmdbuf);
		spdk_dma_free(ctx->req.data);
		spdk_dma_free(ctx->req.md);
		free(ctx->resp.cpl_text);
		free(ctx->resp.data_text);
		free(ctx->resp.md_text);
		free(ctx);
	}
}

static int
rpc_nvme_cmd_resq_construct(struct rpc_nvme_cmd_resp *resp,
			    struct rpc_nvme_cmd_req *req,
			    int sct, int sc, uint32_t result)
{
	struct spdk_nvme_cpl cpl = {};

	cpl.status.sct = sct;
	cpl.status.sc = sc;
	cpl.cdw0 = result;

	resp->cpl_text = malloc(spdk_base64_get_encoded_strlen(sizeof(cpl)) + 1);
	if (!resp->cpl_text) {
		return -ENOMEM;
	}
	spdk_base64_urlsafe_encode(resp->cpl_text, &cpl, sizeof(cpl));

	if (req->data_direction == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		if (req->data_len) {
			resp->data_text = malloc(spdk_base64_get_encoded_strlen(req->data_len) + 1);
			if (!resp->data_text) {
				return -ENOMEM;
			}
			spdk_base64_urlsafe_encode(resp->data_text, req->data,
						   req->data_len);
		}
		if (req->md_len) {
			resp->md_text = malloc(spdk_base64_get_encoded_strlen(req->md_len) + 1);
			if (!resp->md_text) {
				return -ENOMEM;
			}
			spdk_base64_urlsafe_encode(resp->md_text, req->md, req->md_len);
		}
	}

	return 0;
}

void spdk_rpc_nvme_cmd_complete(struct rpc_nvme_cmd_ctx *ctx, int sct, int sc, uint32_t result);

void
spdk_rpc_nvme_cmd_complete(struct rpc_nvme_cmd_ctx *ctx, int sct, int sc, uint32_t result)
{
	struct spdk_jsonrpc_request *request = ctx->jsonrpc_request;
	struct spdk_json_write_ctx *w;
	int ret;

	ret = rpc_nvme_cmd_resq_construct(&ctx->resp, &ctx->req, sct, sc, result);
	if (ret) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-ret));
		goto out;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		goto out;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_name(w, "cpl");
	spdk_json_write_string(w, ctx->resp.cpl_text);

	if (ctx->resp.data_text) {
		spdk_json_write_name(w, "data");
		spdk_json_write_string(w, ctx->resp.data_text);
	}

	if (ctx->resp.md_text) {
		spdk_json_write_name(w, "metadata");
		spdk_json_write_string(w, ctx->resp.md_text);
	}

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

out:
	free_rpc_nvme_cmd_ctx(ctx);
	return;
}

static int
rpc_nvme_cmd_exec(struct rpc_nvme_cmd_ctx *ctx)
{
	struct rpc_nvme_cmd_req *req = &ctx->req;
	int ret = -1;

	switch (req->cmd_type) {
	case NVME_ADMIN_CMD:
		ret = ctx->ops->admin_cmd_func(ctx->op_dev, req->cmdbuf, req->data,
					       req->data_len, req->timeout_ms, ctx);
		break;
	case NVME_IO_CMD:
		ret = ctx->ops->io_cmd_func(ctx->op_dev, req->cmdbuf, req->data,
					    req->data_len, req->md, req->md_len, req->timeout_ms, ctx);
		break;
	}

	return ret;
}

static int
rpc_decode_cmd_type(const struct spdk_json_val *val, void *out)
{
	int *cmd_type = out;

	if (spdk_json_strequal(val, "admin") == true) {
		*cmd_type = NVME_ADMIN_CMD;
	} else if (spdk_json_strequal(val, "io") == true) {
		*cmd_type = NVME_IO_CMD;
	} else {
		SPDK_NOTICELOG("Invalid parameter value: cmd_type\n");
		return -EINVAL;
	}

	return 0;
}

static int
rpc_decode_data_direction(const struct spdk_json_val *val, void *out)
{
	int *data_direction = out;

	if (spdk_json_strequal(val, "h2c") == true) {
		*data_direction = SPDK_NVME_DATA_HOST_TO_CONTROLLER;
	} else if (spdk_json_strequal(val, "c2h") == true) {
		*data_direction = SPDK_NVME_DATA_CONTROLLER_TO_HOST;
	} else {
		SPDK_NOTICELOG("Invalid parameter value: data_direction\n");
		return -EINVAL;
	}

	return 0;
}

static int
rpc_decode_cmdbuf(const struct spdk_json_val *val, void *out)
{
	char *text;
	size_t text_strlen, raw_len;
	struct spdk_nvme_cmd *cmdbuf, **_cmdbuf = out;
	int rc;

	text = spdk_json_strdup(val);
	if (!text) {
		return -1;
	}

	text_strlen = strlen(text);
	raw_len = spdk_base64_get_decoded_len(text_strlen);
	cmdbuf = malloc(raw_len);
	if (!cmdbuf) {
		rc = -ENOMEM;
		goto out;
	}

	rc = spdk_base64_urlsafe_decode(cmdbuf, &raw_len, text);
	if (rc || raw_len != sizeof(*cmdbuf)) {
		rc = -EINVAL;
	}

	*_cmdbuf = cmdbuf;

out:
	free(text);
	return rc;
}

static int
rpc_decode_data(const struct spdk_json_val *val, void *out)
{
	struct rpc_nvme_cmd_req *req = (struct rpc_nvme_cmd_req *)out;
	char *text;
	size_t text_strlen;
	int rc;

	if (req->data) {
		return -EINVAL;
	}

	text = spdk_json_strdup(val);
	if (!text) {
		return -1;
	}

	text_strlen = strlen(text);
	req->data_len = spdk_base64_get_decoded_len(text_strlen);
	req->data = spdk_dma_malloc(req->data_len, 0x1000, NULL);
	if (!req->data) {
		rc = -ENOMEM;
		goto out;
	}

	rc = spdk_base64_urlsafe_decode(req->data, (size_t *)&req->data_len, text);

out:
	free(text);
	return rc;
}

static int
rpc_decode_data_len(const struct spdk_json_val *val, void *out)
{
	struct rpc_nvme_cmd_req *req = (struct rpc_nvme_cmd_req *)out;
	int rc;

	if (req->data) {
		return -EINVAL;
	}

	rc = spdk_json_decode_uint32(val, &req->data_len);
	if (rc) {
		return rc;
	}
	req->data = spdk_dma_malloc(req->data_len, 0x1000, NULL);
	if (!req->data) {
		rc = -ENOMEM;
	}

	return rc;
}

static int
rpc_decode_metadata(const struct spdk_json_val *val, void *out)
{
	struct rpc_nvme_cmd_req *req = *(struct rpc_nvme_cmd_req **)out;
	char *text;
	size_t text_strlen;
	int rc;

	if (req->md) {
		return -EINVAL;
	}

	text = spdk_json_strdup(val);
	if (!text) {
		return -1;
	}

	text_strlen = strlen(text);
	req->md_len = spdk_base64_get_decoded_len(text_strlen);
	req->md = spdk_dma_malloc(req->md_len, 0x1000, NULL);
	if (!req->md) {
		rc = -ENOMEM;
		goto out;
	}

	rc = spdk_base64_urlsafe_decode(req->md, (size_t *)&req->md_len, text);

out:
	free(text);
	return rc;
}

static int
rpc_decode_metadata_len(const struct spdk_json_val *val, void *out)
{
	struct rpc_nvme_cmd_req *req = *(struct rpc_nvme_cmd_req **)out;
	int rc;

	if (req->md) {
		return -EINVAL;
	}

	rc = spdk_json_decode_uint32(val, &req->md_len);
	if (rc) {
		return rc;
	}

	req->md = spdk_dma_malloc(req->md_len, 0x1000, NULL);
	if (!req->md) {
		rc = -ENOMEM;
	}

	return rc;
}

static const struct spdk_json_object_decoder rpc_nvme_cmd_req_decoders[] = {
	{"name", offsetof(struct rpc_nvme_cmd_req, name), spdk_json_decode_string},
	{"cmd_type", offsetof(struct rpc_nvme_cmd_req, cmd_type), rpc_decode_cmd_type},
	{"data_direction", offsetof(struct rpc_nvme_cmd_req, data_direction), rpc_decode_data_direction},
	{"cmdbuf", offsetof(struct rpc_nvme_cmd_req, cmdbuf), rpc_decode_cmdbuf},
	{"timeout_ms", offsetof(struct rpc_nvme_cmd_req, timeout_ms), spdk_json_decode_uint32, true},
	{"data_len", 0, rpc_decode_data_len, true},
	{"metadata_len", 0, rpc_decode_metadata_len, true},
	{"data", 0, rpc_decode_data, true},
	{"metadata", 0, rpc_decode_metadata, true},
};

static void
spdk_rpc_nvme_cmd(struct spdk_jsonrpc_request *request,
		  const struct spdk_json_val *params)
{
	struct spdk_nvme_rpc_ops *ops;
	struct rpc_nvme_cmd_ctx *ctx;
	int ret, error_code;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("Failed at Malloc ctx\n");
		error_code = SPDK_JSONRPC_ERROR_INTERNAL_ERROR;
		ret = -ENOMEM;
		goto invalid;
	}

	if (spdk_json_decode_object(params, rpc_nvme_cmd_req_decoders,
				    SPDK_COUNTOF(rpc_nvme_cmd_req_decoders),
				    &ctx->req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		error_code = SPDK_JSONRPC_ERROR_INVALID_PARAMS;
		ret = -EINVAL;
		goto invalid;
	}

	TAILQ_FOREACH(ops, &g_nvme_rpc_ops, tailq) {
		ctx->op_dev = ops->dev_lookup_func(ctx->req.name);
		if (ctx->op_dev) {
			SPDK_DEBUGLOG(SPDK_LOG_BDEV_NVME, "%s is processed by %s\n", ctx->req.name, ops->name);
			ctx->ops = ops;
			break;
		}
	}

	if (!ctx->op_dev) {
		/* no operator has such dev */
		SPDK_ERRLOG("Failed at device lookup\n");
		error_code = SPDK_JSONRPC_ERROR_INVALID_PARAMS;
		ret = -EINVAL;
		goto invalid;
	}

	ctx->jsonrpc_request = request;

	ret = rpc_nvme_cmd_exec(ctx);
	if (ret < 0) {
		SPDK_NOTICELOG("Failed at rpc_nvme_cmd_exec\n");
		error_code = SPDK_JSONRPC_ERROR_INTERNAL_ERROR;
		goto invalid;
	}

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, error_code, spdk_strerror(-ret));
	free_rpc_nvme_cmd_ctx(ctx);
	return;
}
SPDK_RPC_REGISTER("nvme_cmd", spdk_rpc_nvme_cmd, SPDK_RPC_RUNTIME)
