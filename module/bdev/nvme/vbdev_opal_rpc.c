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

struct rpc_opal_init {
	char *base_bdev_name;
	char *password;
};

static void
free_rpc_opal_init(struct rpc_opal_init *req)
{
	free(req->base_bdev_name);
	free(req->password);
}

static const struct spdk_json_object_decoder rpc_opal_init_decoders[] = {
	{"base_bdev_name", offsetof(struct rpc_opal_init, base_bdev_name), spdk_json_decode_string},
	{"password", offsetof(struct rpc_opal_init, password), spdk_json_decode_string},
};

static void
spdk_rpc_opal_init(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_opal_init req = {};
	struct spdk_json_write_ctx *w;
	struct nvme_bdev_ctrlr *nvme_ctrlr;
	int rc;

	if (spdk_json_decode_object(params, rpc_opal_init_decoders,
				    SPDK_COUNTOF(rpc_opal_init_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	/* check if opal supported */
	nvme_ctrlr = spdk_vbdev_opal_get_nvme_ctrlr_by_bdev_name(req.base_bdev_name);
	if (nvme_ctrlr == NULL || !spdk_opal_supported(nvme_ctrlr->opal_dev)) {
		SPDK_ERRLOG("%s not support opal\n", req.base_bdev_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	/* take ownership */
	rc = spdk_opal_cmd_take_ownership(nvme_ctrlr->opal_dev, req.password);
	if (rc) {
		SPDK_ERRLOG("Take ownership failure: %d\n", rc);
		switch (rc) {
		case -1:
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "SP Busy, try again later");
			break;
		case -2:
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "This drive has been owned");
			break;
		default:
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		}
		goto out;
	}

	/* activate locking SP */
	rc = spdk_opal_cmd_activate_locking_sp(nvme_ctrlr->opal_dev, req.password);
	if (rc) {
		SPDK_ERRLOG("Activate locking SP failure: %d\n", rc);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		goto out;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

out:
	free_rpc_opal_init(&req);
}
SPDK_RPC_REGISTER("nvme_opal_init", spdk_rpc_opal_init, SPDK_RPC_RUNTIME)

struct rpc_opal_revert_tper {
	char *base_bdev_name;
	char *password;
};

static void
free_rpc_opal_revert_tper(struct rpc_opal_revert_tper *req)
{
	free(req->base_bdev_name);
	free(req->password);
}

static const struct spdk_json_object_decoder rpc_opal_revert_tper_decoders[] = {
	{"base_bdev_name", offsetof(struct rpc_opal_revert_tper, base_bdev_name), spdk_json_decode_string},
	{"password", offsetof(struct rpc_opal_revert_tper, password), spdk_json_decode_string},
};

static int
revert_tper_done(struct spdk_opal_dev *dev, void *data)
{
	struct nvme_bdev_ctrlr *ctrlr = data;

	SPDK_NOTICELOG("%s revert TPer done\n", ctrlr->name);
	return 0;
}

static void
spdk_rpc_opal_revert_tper(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_opal_revert_tper req = {};
	struct spdk_json_write_ctx *w;
	struct nvme_bdev_ctrlr *nvme_ctrlr;
	int rc;

	if (spdk_json_decode_object(params, rpc_opal_revert_tper_decoders,
				    SPDK_COUNTOF(rpc_opal_revert_tper_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	/* check if opal supported */
	nvme_ctrlr = spdk_vbdev_opal_get_nvme_ctrlr_by_bdev_name(req.base_bdev_name);
	if (nvme_ctrlr == NULL || !spdk_opal_supported(nvme_ctrlr->opal_dev)) {
		SPDK_ERRLOG("%s not support opal\n", req.base_bdev_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	/* TODO: delete all opal vbdev before revert TPer */

	rc = spdk_opal_cmd_revert_tper_async(nvme_ctrlr->opal_dev, req.password, revert_tper_done,
					     nvme_ctrlr);
	if (rc) {
		SPDK_ERRLOG("Revert TPer failure: %d\n", rc);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		goto out;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

out:
	free_rpc_opal_revert_tper(&req);
}
SPDK_RPC_REGISTER("opal_revert_tper", spdk_rpc_opal_revert_tper, SPDK_RPC_RUNTIME)

struct rpc_construct_opal_vbdev {
	char *base_bdev_name;
	uint16_t locking_range_id;
	uint64_t range_start;
	uint64_t range_length;
	char *password;
};

static void
free_rpc_construct_opal_vbdev(struct rpc_construct_opal_vbdev *req)
{
	free(req->base_bdev_name);
	free(req->password);
}

static const struct spdk_json_object_decoder rpc_construct_opal_vbdev_decoders[] = {
	{"base_bdev_name", offsetof(struct rpc_construct_opal_vbdev, base_bdev_name), spdk_json_decode_string},
	{"locking_range_id", offsetof(struct rpc_construct_opal_vbdev, locking_range_id), spdk_json_decode_uint16},
	{"range_start", offsetof(struct rpc_construct_opal_vbdev, range_start), spdk_json_decode_uint64},
	{"range_length", offsetof(struct rpc_construct_opal_vbdev, range_length), spdk_json_decode_uint64},
	{"password", offsetof(struct rpc_construct_opal_vbdev, password), spdk_json_decode_string},
};

static void
spdk_rpc_construct_opal_vbdev(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_construct_opal_vbdev req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *base_bdev;
	int rc;
	struct spdk_vbdev_opal_config *cfg = NULL;

	if (spdk_json_decode_object(params, rpc_construct_opal_vbdev_decoders,
				    SPDK_COUNTOF(rpc_construct_opal_vbdev_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	rc = spdk_vbdev_opal_config_init(req.base_bdev_name, req.locking_range_id, req.range_start,
					 req.range_length, req.password, &cfg);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Failed to add config '%s': %s",
						     req.base_bdev_name, spdk_strerror(-rc));
		goto out;
	}

	rc = spdk_vbdev_opal_create(cfg);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
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

		/* print all the part bdev name with the same base bdev */
		TAILQ_FOREACH(opal_part, part_tailq, tailq) {
			opal_bdev = spdk_bdev_part_get_bdev(opal_part);
			spdk_json_write_string(w, spdk_bdev_get_name(opal_bdev));
		}
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

out:
	free_rpc_construct_opal_vbdev(&req);
}
SPDK_RPC_REGISTER("construct_opal_vbdev", spdk_rpc_construct_opal_vbdev, SPDK_RPC_RUNTIME)

struct rpc_opal_get_info {
	char *bdev_name;
	char *password;
};

static void
free_rpc_opal_get_info(struct rpc_opal_get_info *req)
{
	free(req->bdev_name);
	free(req->password);
}

static const struct spdk_json_object_decoder rpc_opal_get_info_decoders[] = {
	{"bdev_name", offsetof(struct rpc_opal_get_info, bdev_name), spdk_json_decode_string},
	{"password", offsetof(struct rpc_opal_get_info, password), spdk_json_decode_string},
};

static void
spdk_rpc_opal_get_info(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_opal_get_info req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_opal_locking_range_info *info;
	struct spdk_bdev *opal_bdev;

	if (spdk_json_decode_object(params, rpc_opal_get_info_decoders,
				    SPDK_COUNTOF(rpc_opal_get_info_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	opal_bdev = spdk_bdev_get_by_name(req.bdev_name);
	if (opal_bdev == NULL) {
		SPDK_ERRLOG("Can't find bdev %s\n", req.bdev_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	info = spdk_vbdev_opal_get_info_from_bdev(opal_bdev, req.password);
	if (info == NULL) {
		SPDK_ERRLOG("Get opal info failure\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		goto out;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "name", req.bdev_name);
	spdk_json_write_named_uint64(w, "range_start", info->range_start);
	spdk_json_write_named_uint64(w, "range_length", info->range_length);
	spdk_json_write_named_bool(w, "read lock enabled", info->read_lock_enabled);
	spdk_json_write_named_bool(w, "write lock enabled", info->write_lock_enabled);
	spdk_json_write_named_bool(w, "read locked", info->read_locked);
	spdk_json_write_named_bool(w, "write locked", info->write_locked);

	spdk_json_write_object_end(w);
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

out:
	free_rpc_opal_get_info(&req);
}
SPDK_RPC_REGISTER("opal_get_info", spdk_rpc_opal_get_info, SPDK_RPC_RUNTIME)

struct rpc_destruct_opal_vbdev {
	char *bdev_name;
	char *password;
};

static void
free_rpc_destruct_opal_vbdev(struct rpc_destruct_opal_vbdev *req)
{
	free(req->bdev_name);
	free(req->password);
}

static const struct spdk_json_object_decoder rpc_destruct_opal_vbdev_decoders[] = {
	{"bdev_name", offsetof(struct rpc_destruct_opal_vbdev, bdev_name), spdk_json_decode_string},
	{"password", offsetof(struct rpc_destruct_opal_vbdev, password), spdk_json_decode_string},
};

static void
spdk_rpc_destruct_opal_vbdev(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_destruct_opal_vbdev req = {};
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_destruct_opal_vbdev_decoders,
				    SPDK_COUNTOF(rpc_destruct_opal_vbdev_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	rc = spdk_vbdev_opal_destruct(req.bdev_name, req.password);
	if (rc < 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
		goto out;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
out:
	free_rpc_destruct_opal_vbdev(&req);
}
SPDK_RPC_REGISTER("destruct_opal_vbdev", spdk_rpc_destruct_opal_vbdev, SPDK_RPC_RUNTIME)
