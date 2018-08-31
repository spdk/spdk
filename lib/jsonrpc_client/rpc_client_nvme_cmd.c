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
#include "spdk/util.h"
#include "spdk/nvme_spec.h"
#include "spdk/base64.h"
#include "spdk/json.h"
#include "spdk/jsonrpc_client_cmd.h"
#include "jsonrpc_client_internal.h"

//static const struct spdk_json_object_encoder rpc_nvme_cmd_req_encoders[] = {
//	{"name", offsetof(struct rpc_nvme_cmd_req, name), spdk_json_decode_string},
//	{"cmd_type", offsetof(struct rpc_nvme_cmd_req, cmd_type), rpc_decode_cmd_type},
//	{"data_direction", offsetof(struct rpc_nvme_cmd_req, data_direction), rpc_decode_data_direction},
//	{"cmdbuf", offsetof(struct rpc_nvme_cmd_req, cmdbuf), rpc_decode_cmdbuf},
//	{"timeout_ms", offsetof(struct rpc_nvme_cmd_req, timeout_ms), spdk_json_decode_uint32, true},
//	{"data_len", 0, rpc_decode_data_len, true},
//	{"metadata_len", 0, rpc_decode_metadata_len, true},
//	{"data", 0, rpc_decode_data, true},
//	{"metadata", 0, rpc_decode_metadata, true},
//};


/* response decode start */
struct nvme_cmd_resp {
	struct spdk_nvme_cpl cpl;
	char		*data;
	char		*md;
	uint32_t	data_len;
	uint32_t	md_len;
};


static int
rpc_decode_cpl(const struct spdk_json_val *val, void *out)
{
	char *text = NULL;
	size_t text_strlen, raw_len;
	struct spdk_nvme_cpl *cpl, **_cpl = out;
	int rc;

	rc = spdk_json_decode_string(val, &text);
	if (rc) {
		return rc = val->type == SPDK_JSON_VAL_STRING ? -ENOMEM : -EINVAL;
	}

	text_strlen = strlen(text);
	raw_len = spdk_base64_get_decoded_len(text_strlen);
	cpl = malloc(raw_len);
	if (!cpl) {
		rc = -ENOMEM;
		goto out;
	}

	rc = spdk_base64_urlsafe_decode(cpl, &raw_len, text);
	if (rc || raw_len != sizeof(*cpl)) {
		rc = -EINVAL;
	}

	*_cpl = cpl;

out:
	free(text);
	return rc;
}

static int
rpc_decode_data(const struct spdk_json_val *val, void *out)
{
	struct nvme_cmd_resp *resp = (struct nvme_cmd_resp *)out;
	char *text = NULL, *raw = NULL;
	size_t text_strlen, raw_len;
	int rc;

	rc = spdk_json_decode_string(val, &text);
	if (rc) {
		return rc = val->type == SPDK_JSON_VAL_STRING ? -ENOMEM : -EINVAL;
	}

	text_strlen = strlen(text);
	raw_len = spdk_base64_get_decoded_len(text_strlen);

	raw = malloc(raw_len);
	if (!raw) {
		rc = -ENOMEM;
		goto out;
	}
	rc = spdk_base64_urlsafe_decode(raw, &raw_len, text);
	if (rc || raw_len != resp->data_len) {
		rc = -EINVAL;
		goto out;
	}

	memcpy(resp->data, raw, raw_len);
	printf("data text is %s\n", text);
out:
	free(raw);
	free(text);
	return rc;
}

static int
rpc_decode_metadata(const struct spdk_json_val *val, void *out)
{
	struct nvme_cmd_resp *resp = (struct nvme_cmd_resp *)out;
	char *text = NULL, *raw = NULL;
	size_t text_strlen, raw_len;
	int rc;

	rc = spdk_json_decode_string(val, &text);
	if (rc) {
		return rc = val->type == SPDK_JSON_VAL_STRING ? -ENOMEM : -EINVAL;
	}

	text_strlen = strlen(text);
	raw_len = spdk_base64_get_decoded_len(text_strlen);

	raw = malloc(raw_len);
	if (!raw) {
		rc = -ENOMEM;
		goto out;
	}
	rc = spdk_base64_urlsafe_decode(raw, &raw_len, text);
	if (rc || raw_len != resp->md_len) {
		rc = -EINVAL;
		goto out;
	}

	memcpy(resp->md, raw, raw_len);

out:
	free(raw);
	free(text);
	return rc;
}

static const struct spdk_json_object_decoder nvme_cmd_resp_decoder[] = {
	{"cpl", offsetof(struct nvme_cmd_resp, cpl), rpc_decode_cpl},
	{"data", 0, rpc_decode_data, true},
	{"metadata", 0, rpc_decode_metadata, true},
};

static int
nvme_cmd_json_parser(void *parser_ctx,
		     const struct spdk_json_val *result)
{
	struct nvme_cmd_resp *resp = (struct nvme_cmd_resp *)parser_ctx;

	return spdk_json_decode_object(result, nvme_cmd_resp_decoder,
				       SPDK_COUNTOF(nvme_cmd_resp_decoder),
				       resp);
}
/* response decode end */

#define spdk_json_write_named_base64_string(w, name, raw, raw_len) \
{\
	char *text = calloc(1, spdk_base64_get_encoded_strlen(raw_len) + 1); \
	if (!text) { \
		return -ENOMEM; \
	} \
	spdk_base64_urlsafe_encode(text, raw, raw_len); \
	spdk_json_write_named_string(w, name, text); \
	free(text); \
}

int
_spdk_rpc_client_nvme_cmd(struct spdk_jsonrpc_client_conn *conn,
			  const char *device_name, int cmd_type, int data_direction,
			  const char *cmdbuf, size_t cmdbuf_len,
			  char *data, size_t data_len, char *metadata, size_t metadata_len,
			  uint32_t timeout_ms, uint32_t *result)
{
	struct spdk_json_write_ctx *w;
	struct nvme_cmd_resp resp = {};
	int rc;
	char *cmd_type_str;
	char *data_direction_str;

	w = spdk_jsonrpc_begin_request(&conn->request, "nvme_cmd");
	spdk_json_write_name(w, "params");
	spdk_json_write_object_begin(w);

	if (device_name) {
		spdk_json_write_named_string(w, "name", device_name);
	}

	if (cmdbuf) {
		spdk_json_write_named_base64_string(w, "cmdbuf", cmdbuf, cmdbuf_len);
	}

	if (cmd_type == NVME_CMD_ADMIN) {
		cmd_type_str = "admin";
	} else if (cmd_type == NVME_CMD_IO) {
		cmd_type_str = "io";
	} else {
		return -1;
	}
	if (data_direction == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		data_direction_str = "h2c";

		if (data) {
			spdk_json_write_named_base64_string(w, "data", data, data_len);
		}
		if (metadata) {
			spdk_json_write_named_base64_string(w, "metadata", metadata, metadata_len);
		}
	} else if (data_direction == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		data_direction_str = "c2h";

		if (data_len) {
			spdk_json_write_named_int32(w, "data_len", data_len);
		}
		if (metadata_len) {
			spdk_json_write_named_int32(w, "metadata_len", metadata_len);
		}
	} else {
		return -1;
	}

	spdk_json_write_named_string(w, "cmd_type", cmd_type_str);
	spdk_json_write_named_string(w, "data_direction", data_direction_str);
	if (timeout_ms) {
		spdk_json_write_named_int32(w, "timeout_ms", timeout_ms);
	}

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_request(&conn->request, w);
	spdk_jsonrpc_client_send_request(conn);

	resp.data = data;
	resp.data_len = data_len;
	resp.md = metadata;
	resp.md_len = metadata_len;

	conn->method_parser = nvme_cmd_json_parser;
	conn->parser_ctx = &resp;

	rc = spdk_jsonrpc_client_recv_response(conn);
	if (rc == 0) {
		/* Get result from response */
		*result = resp.cpl.cdw0;
		rc = resp.cpl.status.sct << 8 | resp.cpl.status.sc;
	}

	return rc;
}

int
spdk_rpc_client_nvme_cmd(const char *rpcsock_addr,
			 const char *device_name, int cmd_type, int data_direction,
			 const char *cmdbuf, size_t cmdbuf_len,
			 char *data, size_t data_len, char *metadata, size_t metadata_len,
			 uint32_t timeout_ms, uint32_t *result)
{
	struct spdk_jsonrpc_client_conn *conn;
	int rc;

	conn = spdk_jsonrpc_client_connect(rpcsock_addr);
	if (!conn) {
		return -1;
	}

	rc = _spdk_rpc_client_nvme_cmd(conn, device_name, cmd_type, data_direction, cmdbuf, cmdbuf_len,
				       data, data_len, metadata, metadata_len, timeout_ms, result);

	spdk_jsonrpc_client_close(conn);

	return rc;
}
