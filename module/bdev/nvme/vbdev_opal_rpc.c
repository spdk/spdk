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

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk_internal/log.h"

#include "vbdev_opal.h"

struct rpc_bdev_opal_create {
	char *base_bdev_name;
	uint16_t locking_range_id;
	uint64_t range_start;
	uint64_t range_length;
	char *password;
};

static void
free_rpc_bdev_opal_create(struct rpc_bdev_opal_create *req)
{
	free(req->base_bdev_name);
	free(req->password);
}

static const struct spdk_json_object_decoder rpc_bdev_opal_create_decoders[] = {
	{"base_bdev_name", offsetof(struct rpc_bdev_opal_create, base_bdev_name), spdk_json_decode_string},
	{"locking_range_id", offsetof(struct rpc_bdev_opal_create, locking_range_id), spdk_json_decode_uint16},
	{"range_start", offsetof(struct rpc_bdev_opal_create, range_start), spdk_json_decode_uint64},
	{"range_length", offsetof(struct rpc_bdev_opal_create, range_length), spdk_json_decode_uint64},
	{"password", offsetof(struct rpc_bdev_opal_create, password), spdk_json_decode_string},
};

static void
spdk_rpc_bdev_opal_create(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_bdev_opal_create req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *base_bdev;
	int rc;
	struct spdk_vbdev_opal_config *cfg = NULL;

	if (spdk_json_decode_object(params, rpc_bdev_opal_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_opal_create_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	rc = spdk_vbdev_opal_config_init(req.base_bdev_name, req.locking_range_id, req.range_start,
					 req.range_length, req.password, &cfg);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Failed to add config '%s': %s",
						     req.base_bdev_name, spdk_strerror(-rc));
		goto out;
	}

	rc = spdk_vbdev_opal_create(cfg);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Failed to create opal vbdev from '%s': %s",
						     req.base_bdev_name, spdk_strerror(-rc));
		goto out;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	base_bdev = spdk_bdev_get_by_name(req.base_bdev_name);
	if (base_bdev != NULL) {
		struct spdk_bdev_part_base *opal_base;
		struct bdev_part_tailq *part_tailq;
		struct spdk_bdev_part *opal_part;
		struct spdk_bdev *opal_bdev;

		opal_base = spdk_vbdev_opal_get_part_base(base_bdev);

		assert(opal_base != NULL);

		part_tailq = spdk_bdev_part_base_get_tailq(opal_base);

		/* print all the part bdev names with the same base bdev */
		TAILQ_FOREACH(opal_part, part_tailq, tailq) {
			opal_bdev = spdk_bdev_part_get_bdev(opal_part);
			spdk_json_write_string(w, spdk_bdev_get_name(opal_bdev));
		}
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

out:
	free_rpc_bdev_opal_create(&req);
}
SPDK_RPC_REGISTER("bdev_opal_create", spdk_rpc_bdev_opal_create, SPDK_RPC_RUNTIME)

struct rpc_bdev_opal_delete {
	char *bdev_name;
	char *password;
};

static void
free_rpc_bdev_opal_delete(struct rpc_bdev_opal_delete *req)
{
	free(req->bdev_name);
	free(req->password);
}

static const struct spdk_json_object_decoder rpc_bdev_opal_delete_decoders[] = {
	{"bdev_name", offsetof(struct rpc_bdev_opal_delete, bdev_name), spdk_json_decode_string},
	{"password", offsetof(struct rpc_bdev_opal_delete, password), spdk_json_decode_string},
};

static void
spdk_rpc_bdev_opal_delete(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_bdev_opal_delete req = {};
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_opal_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_opal_delete_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	rc = spdk_vbdev_opal_destruct(req.bdev_name, req.password);
	if (rc < 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
		goto out;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
out:
	free_rpc_bdev_opal_delete(&req);
}
SPDK_RPC_REGISTER("bdev_opal_delete", spdk_rpc_bdev_opal_delete, SPDK_RPC_RUNTIME)
