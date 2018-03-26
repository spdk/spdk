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

#include "spdk_internal/log.h"
#include "vbdev_error.h"

#define ERROR_BDEV_IO_TYPE_INVALID (SPDK_BDEV_IO_TYPE_RESET + 1)
#define ERROR_BDEV_ERROR_TYPE_INVALID (VBDEV_IO_PENDING + 1)

static uint32_t
spdk_rpc_error_bdev_io_type_parse(char *name)
{
	if (strcmp(name, "read") == 0) {
		return SPDK_BDEV_IO_TYPE_READ;
	} else if (strcmp(name, "write") == 0) {
		return SPDK_BDEV_IO_TYPE_WRITE;
	} else if (strcmp(name, "flush") == 0) {
		return SPDK_BDEV_IO_TYPE_FLUSH;
	} else if (strcmp(name, "unmap") == 0) {
		return SPDK_BDEV_IO_TYPE_UNMAP;
	} else if (strcmp(name, "all") == 0) {
		return 0xffffffff;
	} else if (strcmp(name, "clear") == 0) {
		return 0;
	}
	return ERROR_BDEV_IO_TYPE_INVALID;
}

static uint32_t
spdk_rpc_error_bdev_error_type_parse(char *name)
{
	if (strcmp(name, "failure") == 0) {
		return VBDEV_IO_FAILURE;
	} else if (strcmp(name, "pending") == 0) {
		return VBDEV_IO_PENDING;
	}
	return ERROR_BDEV_ERROR_TYPE_INVALID;
}

struct rpc_construct_error_bdev {
	char *base_name;
};

static void
free_rpc_construct_error_bdev(struct rpc_construct_error_bdev *req)
{
	free(req->base_name);
}

static const struct spdk_json_object_decoder rpc_construct_error_bdev_decoders[] = {
	{"base_name", offsetof(struct rpc_construct_error_bdev, base_name), spdk_json_decode_string},
};

static void
spdk_rpc_construct_error_bdev(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_construct_error_bdev req = {};
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_construct_error_bdev_decoders,
				    SPDK_COUNTOF(rpc_construct_error_bdev_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (spdk_vbdev_error_create(req.base_name)) {
		SPDK_ERRLOG("Could not create ErrorInjection bdev %s\n", req.base_name);
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		free_rpc_construct_error_bdev(&req);
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

	free_rpc_construct_error_bdev(&req);

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_construct_error_bdev(&req);
}
SPDK_RPC_REGISTER("construct_error_bdev", spdk_rpc_construct_error_bdev)

struct rpc_error_information {
	char *name;
	char *io_type;
	char *error_type;
	uint32_t num;
};

static const struct spdk_json_object_decoder rpc_error_information_decoders[] = {
	{"name", offsetof(struct rpc_error_information, name), spdk_json_decode_string},
	{"io_type", offsetof(struct rpc_error_information, io_type), spdk_json_decode_string},
	{"error_type", offsetof(struct rpc_error_information, error_type), spdk_json_decode_string},
	{"num", offsetof(struct rpc_error_information, num), spdk_json_decode_uint32, true},
};

static void
free_rpc_error_information(struct rpc_error_information *p)
{
	free(p->name);
	free(p->io_type);
	free(p->error_type);
}

static void
spdk_rpc_bdev_inject_error(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_error_information req = {};
	struct spdk_json_write_ctx *w;
	uint32_t io_type;
	uint32_t error_type;
	int ret;

	if (spdk_json_decode_object(params, rpc_error_information_decoders,
				    SPDK_COUNTOF(rpc_error_information_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	io_type = spdk_rpc_error_bdev_io_type_parse(req.io_type);
	if (io_type == ERROR_BDEV_IO_TYPE_INVALID) {
		goto invalid;
	}

	error_type = spdk_rpc_error_bdev_error_type_parse(req.error_type);
	if (error_type == ERROR_BDEV_ERROR_TYPE_INVALID) {
		goto invalid;
	}

	ret = spdk_vbdev_inject_error(req.name, io_type, error_type, req.num);
	if (ret) {
		goto invalid;
	}

	free_rpc_error_information(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_error_information(&req);
}
SPDK_RPC_REGISTER("bdev_inject_error", spdk_rpc_bdev_inject_error)
