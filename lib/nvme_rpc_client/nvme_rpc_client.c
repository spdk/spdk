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
#include "spdk/nvme_rpc_client.h"

#define decimal_strlen(num) ({ \
		int i = 0; \
		while (num / 10) i++; \
		i; \
	})

#define option_strlen(option) (strlen(option) + strlen("  "))

struct raw_data_buf {
	char *raw_data;
	size_t raw_data_len;
};

enum rpc_cmdline_arg_types {
	ARG_CMD_NAME,
	ARG_STRING_REF,
	ARG_STRING_BASE64,
	ARG_NUM_REF,
};

struct rpc_cmdline_arg {
	const char *option;
	int arg_type;
	void *value;
	int optional;
};

static size_t
rpc_cmdline_arg_strlen(const struct rpc_cmdline_arg *args)
{
	int i = 0;
	size_t len_sum = 0;
	int optional;
	struct raw_data_buf *buf;

	while (args[i].option != NULL) {
		optional = args[i].optional;
		if (args[i].value == NULL) {
			if (optional) {
				break;
			} else {
				return -1;
			}
		}

		switch (args[i].arg_type) {
		case ARG_CMD_NAME:
			len_sum += strlen(*(char **)args[i].value);
			break;
		case ARG_STRING_REF:
			len_sum += option_strlen(args[i].option);
			len_sum += strlen(*(char **)args[i].value);
			break;
		case ARG_STRING_BASE64:
			buf = (struct raw_data_buf *)args[i].value;
			if (buf->raw_data == NULL) {
				if (optional) {
					break;
				} else {
					return -1;
				}
			}
			len_sum += option_strlen(args[i].option);
			len_sum += spdk_base64_get_encoded_strlen(buf->raw_data_len);
			break;
		case ARG_NUM_REF:
			len_sum += option_strlen(args[i].option);
			len_sum += decimal_strlen(*(int *)args[i].value);
			break;
		default:
			continue;
		}
		i++;
	}

	return len_sum;
}

static int
rpc_cmdline_arg_str(const struct rpc_cmdline_arg *args, char *rpc_cmd_str, size_t rpc_cmd_strlen)
{
	int i = 0;
	int optional;
	struct raw_data_buf *buf;
	size_t str_setp = 0;
	char *text;

	while (args[i].option) {
		optional = args[i].optional;
		if (args[i].value == NULL) {
			if (optional) {
				break;
			} else {
				return -1;
			}
		}

		switch (args[i].arg_type) {
		case ARG_CMD_NAME:
			str_setp += snprintf(rpc_cmd_str + str_setp, rpc_cmd_strlen + 1 - str_setp,
					     "%s", *(char **)args[i].value);
			break;
		case ARG_STRING_REF:
			str_setp += snprintf(rpc_cmd_str + str_setp, rpc_cmd_strlen + 1 - str_setp,
					     " %s ", args[i].option);
			str_setp += snprintf(rpc_cmd_str + str_setp, rpc_cmd_strlen + 1 - str_setp,
					     "%s", *(char **)args[i].value);
			break;
		case ARG_STRING_BASE64:
			buf = (struct raw_data_buf *)args[i].value;
			if (buf->raw_data == NULL) {
				if (optional) {
					break;
				} else {
					return -1;
				}
			}

			text = malloc(spdk_base64_get_encoded_strlen(buf->raw_data_len) + 1);
			if (!text) {
				return -ENOMEM;
			}
			spdk_base64_urlsafe_encode(text, buf->raw_data, buf->raw_data_len);

			str_setp += snprintf(rpc_cmd_str + str_setp, rpc_cmd_strlen + 1 - str_setp,
					     " %s ", args[i].option);
			str_setp += snprintf(rpc_cmd_str + str_setp, rpc_cmd_strlen + 1 - str_setp,
					     "%s", text);
			free(text);
			break;
		case ARG_NUM_REF:
			str_setp += snprintf(rpc_cmd_str + str_setp, rpc_cmd_strlen + 1 - str_setp,
					     " %s ", args[i].option);
			str_setp += snprintf(rpc_cmd_str + str_setp, rpc_cmd_strlen + 1 - str_setp,
					     "%d", *(int *)args[i].value);
			break;
		default:
			continue;
		}
		i++;
	}

	return 0;
}

static int
spdk_rpc_rpcpy_str(const char *rpcpy_path, const char *rpcsock_path,
		   const char *rpc_cmd_str, char **_rpcpy_str)
{
	int rpcpy_cmd_strlen;
	char *rpcpy_str;
	int ret;
	const char *postfix = "2>&1";
	const struct rpc_cmdline_arg rpcpy_cmd_args[] = {
		{"",	ARG_CMD_NAME,	(void *) &rpcpy_path},
		{"-s",	ARG_STRING_REF,	(void *) &rpcsock_path, true},
		{"",	ARG_STRING_REF,	(void *) &rpc_cmd_str},
		{"",	ARG_STRING_REF,	(void *) &postfix},
		{}
	};

	rpcpy_cmd_strlen = rpc_cmdline_arg_strlen(rpcpy_cmd_args);
	if (rpcpy_cmd_strlen > 0) {
		rpcpy_str = calloc(1, rpcpy_cmd_strlen + 1);
	} else {
		return -1;
	}

	ret = rpc_cmdline_arg_str(rpcpy_cmd_args, rpcpy_str, rpcpy_cmd_strlen);
	if (ret) {
		free(rpcpy_str);
		return ret;
	}

	*_rpcpy_str = rpcpy_str;

	return 0;
}

static int
spdk_rpc_exec_rpcpy(const char *rpcpy_path, const char *rpcsock_path,
		    const char *rpc_cmd_str, char **_rpcpy_resp)
{
	char *rpc_cmd;
	FILE *fp = NULL;
	char *rpcpy_resp_step, *rpcpy_resp;
	uint32_t resp_buf_step_len = 4096;
	uint32_t resp_buf_len = resp_buf_step_len;
	int ret;

	ret = spdk_rpc_rpcpy_str(rpcpy_path, rpcsock_path, rpc_cmd_str, &rpc_cmd);
	if (ret) {
		return ret;
	}

	fp = popen(rpc_cmd, "r");
	free(rpc_cmd);
	if (!fp) {
		return -1;
	}

	/* Allocate buffer to receive json-rpc response */
	rpcpy_resp = malloc(resp_buf_step_len);
	rpcpy_resp_step = rpcpy_resp;
	ret = fread(rpcpy_resp_step, 1, resp_buf_step_len, fp);
	while (ret == (int)resp_buf_step_len) {
		resp_buf_len += resp_buf_step_len;
		rpcpy_resp = realloc(rpcpy_resp, resp_buf_len);
		if (!rpcpy_resp) {
			pclose(fp);
			return -1;
		}

		rpcpy_resp_step = rpcpy_resp + resp_buf_len - resp_buf_step_len;
		ret = fread(rpcpy_resp_step, 1, resp_buf_step_len, fp);
	}

	if (_rpcpy_resp) {
		*_rpcpy_resp = rpcpy_resp;
	}

	ret = pclose(fp);
	return ret;
}

static int
spdk_rpc_nvme_cmd_str_req(const char *device_name, int cmd_type, int data_direction,
			  const char *cmdbuf, size_t cmdbuf_len,
			  char *data, size_t data_len, char *metadata, size_t metadata_len,
			  uint32_t timeout_ms, char **rpc_nvme_cmd_str)
{
	char *nvme_cmd = "nvme_cmd";
	char *cmd_type_str;
	char *data_direction_str;
	struct raw_data_buf _cmdbuf = {
		.raw_data = (char *)cmdbuf,
		.raw_data_len = cmdbuf_len,
	};
	struct raw_data_buf _data = {
		.raw_data = data,
		.raw_data_len = data_len,
	};
	struct raw_data_buf _metadata = {
		.raw_data = metadata,
		.raw_data_len = metadata_len,
	};
	const struct rpc_cmdline_arg nvme_cmd_h2c_args[] = {
		{"",	ARG_CMD_NAME,	&nvme_cmd},
		{"-n",	ARG_STRING_REF,	&device_name},
		{"-t",	ARG_STRING_REF,	&cmd_type_str},
		{"-r",	ARG_STRING_REF,	&data_direction_str},
		{"-c",	ARG_STRING_BASE64,	&_cmdbuf},
		{"-d",	ARG_STRING_BASE64,	&_data},
		{"-m",	ARG_STRING_BASE64,	&_metadata},
		{"-T",	ARG_NUM_REF,	&timeout_ms},
		{},
	};
	const struct rpc_cmdline_arg nvme_cmd_c2h_args[] = {
		{"",	ARG_CMD_NAME, &nvme_cmd},
		{"-n",	ARG_STRING_REF, &device_name},
		{"-t",	ARG_STRING_REF,	&cmd_type_str},
		{"-r",	ARG_STRING_REF,	&data_direction_str},
		{"-c",	ARG_STRING_BASE64,	&_cmdbuf},
		{"-D", ARG_NUM_REF, &data_len},
		{"-M", ARG_NUM_REF, &metadata_len},
		{"-T", ARG_NUM_REF, &timeout_ms},
		{},
	};
	const struct rpc_cmdline_arg *nvme_cmd_args;
	char *rpc_nvme_cmd;
	int rpc_nvme_cmd_strlen;
	int ret;

	if (cmd_type == NVME_ADMIN_CMD) {
		cmd_type_str = "admin";
	} else if (cmd_type == NVME_IO_CMD) {
		cmd_type_str = "io";
	} else {
		return -1;
	}
	if (data_direction == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		data_direction_str = "h2c";
		nvme_cmd_args = nvme_cmd_h2c_args;
	} else if (data_direction == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		data_direction_str = "c2h";
		nvme_cmd_args = nvme_cmd_c2h_args;
	} else {
		return -1;
	}

	rpc_nvme_cmd_strlen = rpc_cmdline_arg_strlen(nvme_cmd_args);
	if (rpc_nvme_cmd_strlen > 0) {
		rpc_nvme_cmd = calloc(1, rpc_nvme_cmd_strlen + 1);
	} else {
		return -1;
	}

	ret = rpc_cmdline_arg_str(nvme_cmd_args, rpc_nvme_cmd, rpc_nvme_cmd_strlen);
	if (ret) {
		free(rpc_nvme_cmd);
		return ret;
	}

	*rpc_nvme_cmd_str = rpc_nvme_cmd;
	return 0;
}

#define SPDK_JSONRPC_MAX_VALUES 128

static int
rpc_decode_nvme_resp(const struct spdk_json_val *val, void *out)
{
	struct spdk_nvme_cpl *resp = out;
	char *str = NULL;
	int ret, resp_len;

	ret = spdk_json_decode_string(val, &str);
	if (ret) {
		return -1;
	}

	ret = spdk_base64_urlsafe_decode((void *)resp, (size_t *)&resp_len, str);
	free(str);
	if (ret || resp_len != sizeof(*resp)) {
		return -1;
	}

	return 0;
}

struct rpc_nvme_cmd_resp {
	struct spdk_nvme_cpl cpl;
	char *data;
	char *metadata;
};

static const struct spdk_json_object_decoder rpc_nvme_cmd_resp_decoder[] = {
	{"cpl", offsetof(struct rpc_nvme_cmd_resp, cpl), rpc_decode_nvme_resp},
	{"data", offsetof(struct rpc_nvme_cmd_resp, data), spdk_json_decode_string, true},
	{"metadata", offsetof(struct rpc_nvme_cmd_resp, metadata), spdk_json_decode_string, true},
};

static void
free_rpc_nvme_cmd_resp(struct rpc_nvme_cmd_resp *resp)
{
	free(resp->data);
	free(resp->metadata);
}

static int
spdk_rpc_nvme_cmd_str_resp(char *rpcpy_resp, char *data,
			   char *metadata, uint32_t *result)
{
	struct spdk_json_val values[SPDK_JSONRPC_MAX_VALUES] = {};
	struct rpc_nvme_cmd_resp resp = {};
	void *end = NULL;
	int rc;

	/* Check to see if we have received a full JSON value. */
	rc = spdk_json_parse(rpcpy_resp, strlen(rpcpy_resp), NULL, 0, &end, 0);
	if (rc == SPDK_JSON_PARSE_INCOMPLETE) {
		return -EIO;
	}

	rc = spdk_json_parse(rpcpy_resp, strlen(rpcpy_resp), values, SPDK_JSONRPC_MAX_VALUES, &end,
			     SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE);

	if (spdk_json_decode_object(values, rpc_nvme_cmd_resp_decoder,
				    SPDK_COUNTOF(rpc_nvme_cmd_resp_decoder),
				    &resp)) {
		goto out;
	}

	if (data && resp.data) {
		rc = spdk_base64_urlsafe_decode(data, NULL, resp.data);
		if (rc) {
			goto out;
		}
	}

	if (metadata && resp.metadata) {
		rc = spdk_base64_urlsafe_decode(metadata, NULL, resp.metadata);
		if (rc) {
			goto out;
		}
	}

	*result = resp.cpl.cdw0;
	rc = resp.cpl.status.sct << 8 | resp.cpl.status.sc;

out:
	free_rpc_nvme_cmd_resp(&resp);
	return rc;
}

int
spdk_rpc_exec_nvme_cmd(const char *rpcpy_path, const char *rpcsock_path,
		       const char *device_name, int cmd_type, int data_direction,
		       const char *cmdbuf, size_t cmdbuf_len,
		       char *data, size_t data_len, char *metadata, size_t metadata_len,
		       uint32_t timeout_ms, uint32_t *result)
{
	int ret;
	char *cmd_str, *resp_str;

	ret = spdk_rpc_nvme_cmd_str_req(device_name, cmd_type, data_direction, cmdbuf, cmdbuf_len,
					data, data_len, metadata, metadata_len, timeout_ms,
					&cmd_str);

	if (ret) {
		return ret;
	}

	ret = spdk_rpc_exec_rpcpy(rpcpy_path, rpcsock_path, cmd_str, &resp_str);
	free(cmd_str);
	if (ret) {
		return ret;
	}

	ret = spdk_rpc_nvme_cmd_str_resp(resp_str, data, metadata, result);
	free(resp_str);

	return ret;
}
