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
#include "spdk/log.h"

#include "bdev_nvme.h"
#include "spdk/base64.h"

enum spdk_nvme_rpc_type {
	NVME_ADMIN_CMD = 1,
	NVME_IO_CMD,
};

struct rpc_bdev_nvme_send_cmd_req {
	char			*name;
	int			cmd_type;
	int			data_direction;
	uint32_t		timeout_ms;
	uint32_t		data_len;
	uint32_t		md_len;

	struct spdk_nvme_cmd	*cmdbuf;
	char			*data;
	char			*md;
};

struct rpc_bdev_nvme_send_cmd_resp {
	char	*cpl_text;
	char	*data_text;
	char	*md_text;
};

struct rpc_bdev_nvme_send_cmd_ctx {
	struct spdk_jsonrpc_request	*jsonrpc_request;
	struct rpc_bdev_nvme_send_cmd_req	req;
	struct rpc_bdev_nvme_send_cmd_resp	resp;
	struct nvme_ctrlr		*nvme_ctrlr;
	struct spdk_io_channel		*ctrlr_io_ch;
};

static void
free_rpc_bdev_nvme_send_cmd_ctx(struct rpc_bdev_nvme_send_cmd_ctx *ctx)
{
	assert(ctx != NULL);

	free(ctx->req.name);
	free(ctx->req.cmdbuf);
	spdk_free(ctx->req.data);
	spdk_free(ctx->req.md);
	free(ctx->resp.cpl_text);
	free(ctx->resp.data_text);
	free(ctx->resp.md_text);
	free(ctx);
}

static int
rpc_bdev_nvme_send_cmd_resp_construct(struct rpc_bdev_nvme_send_cmd_resp *resp,
				      struct rpc_bdev_nvme_send_cmd_req *req,
				      const struct spdk_nvme_cpl *cpl)
{
	resp->cpl_text = malloc(spdk_base64_get_encoded_strlen(sizeof(*cpl)) + 1);
	if (!resp->cpl_text) {
		return -ENOMEM;
	}
	spdk_base64_urlsafe_encode(resp->cpl_text, cpl, sizeof(*cpl));

	if (req->data_direction == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		if (req->data_len) {
			resp->data_text = malloc(spdk_base64_get_encoded_strlen(req->data_len) + 1);
			if (!resp->data_text) {
				return -ENOMEM;
			}
			spdk_base64_urlsafe_encode(resp->data_text, req->data, req->data_len);
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

static void
rpc_bdev_nvme_send_cmd_complete(struct rpc_bdev_nvme_send_cmd_ctx *ctx,
				const struct spdk_nvme_cpl *cpl)
{
	struct spdk_jsonrpc_request *request = ctx->jsonrpc_request;
	struct spdk_json_write_ctx *w;
	int ret;

	ret = rpc_bdev_nvme_send_cmd_resp_construct(&ctx->resp, &ctx->req, cpl);
	if (ret) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-ret));
		goto out;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "cpl", ctx->resp.cpl_text);

	if (ctx->resp.data_text) {
		spdk_json_write_named_string(w, "data", ctx->resp.data_text);
	}

	if (ctx->resp.md_text) {
		spdk_json_write_named_string(w, "metadata", ctx->resp.md_text);
	}

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

out:
	free_rpc_bdev_nvme_send_cmd_ctx(ctx);
	return;
}

static void
nvme_rpc_bdev_nvme_cb(void *ref, const struct spdk_nvme_cpl *cpl)
{
	struct rpc_bdev_nvme_send_cmd_ctx *ctx = (struct rpc_bdev_nvme_send_cmd_ctx *)ref;

	if (ctx->ctrlr_io_ch) {
		spdk_put_io_channel(ctx->ctrlr_io_ch);
		ctx->ctrlr_io_ch = NULL;
	}

	rpc_bdev_nvme_send_cmd_complete(ctx, cpl);
}

static int
nvme_rpc_admin_cmd_bdev_nvme(struct rpc_bdev_nvme_send_cmd_ctx *ctx, struct spdk_nvme_cmd *cmd,
			     void *buf, uint32_t nbytes, uint32_t timeout_ms)
{
	struct nvme_ctrlr *_nvme_ctrlr = ctx->nvme_ctrlr;
	int ret;

	ret = spdk_nvme_ctrlr_cmd_admin_raw(_nvme_ctrlr->ctrlr, cmd, buf,
					    nbytes, nvme_rpc_bdev_nvme_cb, ctx);

	return ret;
}

static int
nvme_rpc_io_cmd_bdev_nvme(struct rpc_bdev_nvme_send_cmd_ctx *ctx, struct spdk_nvme_cmd *cmd,
			  void *buf, uint32_t nbytes, void *md_buf, uint32_t md_len,
			  uint32_t timeout_ms)
{
	struct nvme_ctrlr *_nvme_ctrlr = ctx->nvme_ctrlr;
	struct spdk_nvme_qpair *io_qpair;
	int ret;

	ctx->ctrlr_io_ch = spdk_get_io_channel(_nvme_ctrlr);
	io_qpair = bdev_nvme_get_io_qpair(ctx->ctrlr_io_ch);

	ret = spdk_nvme_ctrlr_cmd_io_raw_with_md(_nvme_ctrlr->ctrlr, io_qpair,
			cmd, buf, nbytes, md_buf, nvme_rpc_bdev_nvme_cb, ctx);
	if (ret) {
		spdk_put_io_channel(ctx->ctrlr_io_ch);
	}

	return ret;

}

static int
rpc_bdev_nvme_send_cmd_exec(struct rpc_bdev_nvme_send_cmd_ctx *ctx)
{
	struct rpc_bdev_nvme_send_cmd_req *req = &ctx->req;
	int ret = -EINVAL;

	switch (req->cmd_type) {
	case NVME_ADMIN_CMD:
		ret = nvme_rpc_admin_cmd_bdev_nvme(ctx, req->cmdbuf, req->data,
						   req->data_len, req->timeout_ms);
		break;
	case NVME_IO_CMD:
		ret = nvme_rpc_io_cmd_bdev_nvme(ctx, req->cmdbuf, req->data,
						req->data_len, req->md, req->md_len, req->timeout_ms);
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
	char *text = NULL;
	size_t text_strlen, raw_len;
	struct spdk_nvme_cmd *cmdbuf, **_cmdbuf = out;
	int rc;

	rc = spdk_json_decode_string(val, &text);
	if (rc) {
		return val->type == SPDK_JSON_VAL_STRING ? -ENOMEM : -EINVAL;
	}

	text_strlen = strlen(text);
	raw_len = spdk_base64_get_decoded_len(text_strlen);
	cmdbuf = malloc(raw_len);
	if (!cmdbuf) {
		rc = -ENOMEM;
		goto out;
	}

	rc = spdk_base64_urlsafe_decode(cmdbuf, &raw_len, text);
	if (rc) {
		free(cmdbuf);
		goto out;
	}
	if (raw_len != sizeof(*cmdbuf)) {
		rc = -EINVAL;
		free(cmdbuf);
		goto out;
	}

	*_cmdbuf = cmdbuf;

out:
	free(text);
	return rc;
}

static int
rpc_decode_data(const struct spdk_json_val *val, void *out)
{
	struct rpc_bdev_nvme_send_cmd_req *req = (struct rpc_bdev_nvme_send_cmd_req *)out;
	char *text = NULL;
	size_t text_strlen;
	int rc;

	rc = spdk_json_decode_string(val, &text);
	if (rc) {
		return val->type == SPDK_JSON_VAL_STRING ? -ENOMEM : -EINVAL;
	}
	text_strlen = strlen(text);

	if (req->data_len) {
		/* data_len is decoded by param "data_len" */
		if (req->data_len != spdk_base64_get_decoded_len(text_strlen)) {
			rc = -EINVAL;
			goto out;
		}
	} else {
		req->data_len = spdk_base64_get_decoded_len(text_strlen);
		req->data = spdk_malloc(req->data_len > 0x1000 ? req->data_len : 0x1000, 0x1000,
					NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (!req->data) {
			rc = -ENOMEM;
			goto out;
		}
	}

	rc = spdk_base64_urlsafe_decode(req->data, (size_t *)&req->data_len, text);

out:
	free(text);
	return rc;
}

static int
rpc_decode_data_len(const struct spdk_json_val *val, void *out)
{
	struct rpc_bdev_nvme_send_cmd_req *req = (struct rpc_bdev_nvme_send_cmd_req *)out;
	uint32_t data_len;
	int rc;

	rc = spdk_json_decode_uint32(val, &data_len);
	if (rc) {
		return rc;
	}

	if (req->data_len) {
		/* data_len is decoded by param "data" */
		if (req->data_len != data_len) {
			rc = -EINVAL;
		}
	} else {
		req->data_len = data_len;
		req->data = spdk_malloc(req->data_len > 0x1000 ? req->data_len : 0x1000, 0x1000,
					NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (!req->data) {
			rc = -ENOMEM;
		}
	}

	return rc;
}

static int
rpc_decode_metadata(const struct spdk_json_val *val, void *out)
{
	struct rpc_bdev_nvme_send_cmd_req *req = (struct rpc_bdev_nvme_send_cmd_req *)out;
	char *text = NULL;
	size_t text_strlen;
	int rc;

	rc = spdk_json_decode_string(val, &text);
	if (rc) {
		return val->type == SPDK_JSON_VAL_STRING ? -ENOMEM : -EINVAL;
	}
	text_strlen = strlen(text);

	if (req->md_len) {
		/* md_len is decoded by param "metadata_len" */
		if (req->md_len != spdk_base64_get_decoded_len(text_strlen)) {
			rc = -EINVAL;
			goto out;
		}
	} else {
		req->md_len = spdk_base64_get_decoded_len(text_strlen);
		req->md = spdk_malloc(req->md_len, 0x1000, NULL,
				      SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (!req->md) {
			rc = -ENOMEM;
			goto out;
		}
	}

	rc = spdk_base64_urlsafe_decode(req->md, (size_t *)&req->md_len, text);

out:
	free(text);
	return rc;
}

static int
rpc_decode_metadata_len(const struct spdk_json_val *val, void *out)
{
	struct rpc_bdev_nvme_send_cmd_req *req = (struct rpc_bdev_nvme_send_cmd_req *)out;
	uint32_t md_len;
	int rc;

	rc = spdk_json_decode_uint32(val, &md_len);
	if (rc) {
		return rc;
	}

	if (req->md_len) {
		/* md_len is decoded by param "metadata" */
		if (req->md_len != md_len) {
			rc = -EINVAL;
		}
	} else {
		req->md_len = md_len;
		req->md = spdk_malloc(req->md_len, 0x1000, NULL,
				      SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (!req->md) {
			rc = -ENOMEM;
		}
	}

	return rc;
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_send_cmd_req_decoders[] = {
	{"name", offsetof(struct rpc_bdev_nvme_send_cmd_req, name), spdk_json_decode_string},
	{"cmd_type", offsetof(struct rpc_bdev_nvme_send_cmd_req, cmd_type), rpc_decode_cmd_type},
	{"data_direction", offsetof(struct rpc_bdev_nvme_send_cmd_req, data_direction), rpc_decode_data_direction},
	{"cmdbuf", offsetof(struct rpc_bdev_nvme_send_cmd_req, cmdbuf), rpc_decode_cmdbuf},
	{"timeout_ms", offsetof(struct rpc_bdev_nvme_send_cmd_req, timeout_ms), spdk_json_decode_uint32, true},
	{"data_len", 0, rpc_decode_data_len, true},
	{"metadata_len", 0, rpc_decode_metadata_len, true},
	{"data", 0, rpc_decode_data, true},
	{"metadata", 0, rpc_decode_metadata, true},
};

static void
rpc_bdev_nvme_send_cmd(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_send_cmd_ctx *ctx;
	int ret, error_code;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("Failed at Malloc ctx\n");
		error_code = SPDK_JSONRPC_ERROR_INTERNAL_ERROR;
		ret = -ENOMEM;
		goto invalid;
	}

	if (spdk_json_decode_object(params, rpc_bdev_nvme_send_cmd_req_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_send_cmd_req_decoders),
				    &ctx->req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		error_code = SPDK_JSONRPC_ERROR_INVALID_PARAMS;
		ret = -EINVAL;
		goto invalid;
	}

	ctx->nvme_ctrlr = nvme_ctrlr_get_by_name(ctx->req.name);
	if (ctx->nvme_ctrlr == NULL) {
		SPDK_ERRLOG("Failed at device lookup\n");
		error_code = SPDK_JSONRPC_ERROR_INVALID_PARAMS;
		ret = -EINVAL;
		goto invalid;
	}

	ctx->jsonrpc_request = request;

	ret = rpc_bdev_nvme_send_cmd_exec(ctx);
	if (ret < 0) {
		SPDK_NOTICELOG("Failed at rpc_bdev_nvme_send_cmd_exec\n");
		error_code = SPDK_JSONRPC_ERROR_INTERNAL_ERROR;
		goto invalid;
	}

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, error_code, spdk_strerror(-ret));
	free_rpc_bdev_nvme_send_cmd_ctx(ctx);
	return;
}
SPDK_RPC_REGISTER("bdev_nvme_send_cmd", rpc_bdev_nvme_send_cmd, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_nvme_send_cmd, send_nvme_cmd)
