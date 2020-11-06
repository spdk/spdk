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
#include "spdk/log.h"

#include "vbdev_opal.h"

struct rpc_bdev_nvme_opal_init {
	char *nvme_ctrlr_name;
	char *password;
};

static void
free_rpc_bdev_nvme_opal_init(struct rpc_bdev_nvme_opal_init *req)
{
	free(req->nvme_ctrlr_name);
	free(req->password);
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_opal_init_decoders[] = {
	{"nvme_ctrlr_name", offsetof(struct rpc_bdev_nvme_opal_init, nvme_ctrlr_name), spdk_json_decode_string},
	{"password", offsetof(struct rpc_bdev_nvme_opal_init, password), spdk_json_decode_string},
};

static void
rpc_bdev_nvme_opal_init(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_opal_init req = {};
	struct nvme_bdev_ctrlr *nvme_ctrlr;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_nvme_opal_init_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_opal_init_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	/* check if opal supported */
	nvme_ctrlr = nvme_bdev_ctrlr_get_by_name(req.nvme_ctrlr_name);
	if (nvme_ctrlr == NULL || nvme_ctrlr->opal_dev == NULL) {
		SPDK_ERRLOG("%s not support opal\n", req.nvme_ctrlr_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	/* take ownership */
	rc = spdk_opal_cmd_take_ownership(nvme_ctrlr->opal_dev, req.password);
	if (rc) {
		SPDK_ERRLOG("Take ownership failure: %d\n", rc);
		switch (rc) {
		case -EBUSY:
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "SP Busy, try again later");
			break;
		case -EACCES:
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "This drive is already enabled");
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

	spdk_jsonrpc_send_bool_response(request, true);

out:
	free_rpc_bdev_nvme_opal_init(&req);
}
SPDK_RPC_REGISTER("bdev_nvme_opal_init", rpc_bdev_nvme_opal_init, SPDK_RPC_RUNTIME)

struct rpc_bdev_nvme_opal_revert {
	char *nvme_ctrlr_name;
	char *password;
};

static void
free_rpc_bdev_nvme_opal_revert(struct rpc_bdev_nvme_opal_revert *req)
{
	free(req->nvme_ctrlr_name);
	free(req->password);
}

static const struct spdk_json_object_decoder rpc_bdev_nvme_opal_revert_decoders[] = {
	{"nvme_ctrlr_name", offsetof(struct rpc_bdev_nvme_opal_revert, nvme_ctrlr_name), spdk_json_decode_string},
	{"password", offsetof(struct rpc_bdev_nvme_opal_revert, password), spdk_json_decode_string},
};

static void
rpc_bdev_nvme_opal_revert(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_bdev_nvme_opal_revert req = {};
	struct nvme_bdev_ctrlr *nvme_ctrlr;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_nvme_opal_revert_decoders,
				    SPDK_COUNTOF(rpc_bdev_nvme_opal_revert_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	/* check if opal supported */
	nvme_ctrlr = nvme_bdev_ctrlr_get_by_name(req.nvme_ctrlr_name);
	if (nvme_ctrlr == NULL || nvme_ctrlr->opal_dev == NULL) {
		SPDK_ERRLOG("%s not support opal\n", req.nvme_ctrlr_name);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	/* TODO: delete all opal vbdev before revert TPer */

	rc = spdk_opal_cmd_revert_tper(nvme_ctrlr->opal_dev, req.password);
	if (rc) {
		SPDK_ERRLOG("Revert TPer failure: %d\n", rc);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		goto out;
	}

	spdk_jsonrpc_send_bool_response(request, true);

out:
	free_rpc_bdev_nvme_opal_revert(&req);
}
SPDK_RPC_REGISTER("bdev_nvme_opal_revert", rpc_bdev_nvme_opal_revert, SPDK_RPC_RUNTIME)

struct rpc_bdev_opal_create {
	char *nvme_ctrlr_name;
	uint32_t nsid;
	uint16_t locking_range_id;
	uint64_t range_start;
	uint64_t range_length;
	char *password;
};

static void
free_rpc_bdev_opal_create(struct rpc_bdev_opal_create *req)
{
	free(req->nvme_ctrlr_name);
	free(req->password);
}

static const struct spdk_json_object_decoder rpc_bdev_opal_create_decoders[] = {
	{"nvme_ctrlr_name", offsetof(struct rpc_bdev_opal_create, nvme_ctrlr_name), spdk_json_decode_string},
	{"nsid", offsetof(struct rpc_bdev_opal_create, nsid), spdk_json_decode_uint32},
	{"locking_range_id", offsetof(struct rpc_bdev_opal_create, locking_range_id), spdk_json_decode_uint16},
	{"range_start", offsetof(struct rpc_bdev_opal_create, range_start), spdk_json_decode_uint64},
	{"range_length", offsetof(struct rpc_bdev_opal_create, range_length), spdk_json_decode_uint64},
	{"password", offsetof(struct rpc_bdev_opal_create, password), spdk_json_decode_string},
};

static void
rpc_bdev_opal_create(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_opal_create req = {};
	struct spdk_json_write_ctx *w;
	char *opal_bdev_name;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_opal_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_opal_create_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	rc = vbdev_opal_create(req.nvme_ctrlr_name, req.nsid, req.locking_range_id, req.range_start,
			       req.range_length, req.password);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Failed to create opal vbdev from '%s': %s",
						     req.nvme_ctrlr_name, spdk_strerror(-rc));
		goto out;
	}

	w = spdk_jsonrpc_begin_result(request);
	opal_bdev_name = spdk_sprintf_alloc("%sn%dr%d", req.nvme_ctrlr_name, req.nsid,
					    req.locking_range_id);
	spdk_json_write_string(w, opal_bdev_name);
	spdk_jsonrpc_end_result(request, w);
	free(opal_bdev_name);

out:
	free_rpc_bdev_opal_create(&req);
}
SPDK_RPC_REGISTER("bdev_opal_create", rpc_bdev_opal_create, SPDK_RPC_RUNTIME)

struct rpc_bdev_opal_get_info {
	char *bdev_name;
	char *password;
};

static void
free_rpc_bdev_opal_get_info(struct rpc_bdev_opal_get_info *req)
{
	free(req->bdev_name);
	free(req->password);
}

static const struct spdk_json_object_decoder rpc_bdev_opal_get_info_decoders[] = {
	{"bdev_name", offsetof(struct rpc_bdev_opal_get_info, bdev_name), spdk_json_decode_string},
	{"password", offsetof(struct rpc_bdev_opal_get_info, password), spdk_json_decode_string},
};

static void
rpc_bdev_opal_get_info(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_bdev_opal_get_info req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_opal_locking_range_info *info;

	if (spdk_json_decode_object(params, rpc_bdev_opal_get_info_decoders,
				    SPDK_COUNTOF(rpc_bdev_opal_get_info_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	info = vbdev_opal_get_info_from_bdev(req.bdev_name, req.password);
	if (info == NULL) {
		SPDK_ERRLOG("Get opal info failure\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "Internal error");
		goto out;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "name", req.bdev_name);
	spdk_json_write_named_uint64(w, "range_start", info->range_start);
	spdk_json_write_named_uint64(w, "range_length", info->range_length);
	spdk_json_write_named_bool(w, "read_lock_enabled", info->read_lock_enabled);
	spdk_json_write_named_bool(w, "write_lock_enabled", info->write_lock_enabled);
	spdk_json_write_named_bool(w, "read_locked", info->read_locked);
	spdk_json_write_named_bool(w, "write_locked", info->write_locked);

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

out:
	free_rpc_bdev_opal_get_info(&req);
}
SPDK_RPC_REGISTER("bdev_opal_get_info", rpc_bdev_opal_get_info, SPDK_RPC_RUNTIME)

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
rpc_bdev_opal_delete(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_bdev_opal_delete req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_opal_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_opal_delete_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	rc = vbdev_opal_destruct(req.bdev_name, req.password);
	if (rc < 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
		goto out;
	}

	spdk_jsonrpc_send_bool_response(request, true);
out:
	free_rpc_bdev_opal_delete(&req);
}
SPDK_RPC_REGISTER("bdev_opal_delete", rpc_bdev_opal_delete, SPDK_RPC_RUNTIME)

struct rpc_bdev_opal_set_lock_state {
	char *bdev_name;
	uint16_t user_id;
	char *password;
	char *lock_state;
};

static void
free_rpc_bdev_opal_set_lock_state(struct rpc_bdev_opal_set_lock_state *req)
{
	free(req->bdev_name);
	free(req->password);
	free(req->lock_state);
}

static const struct spdk_json_object_decoder rpc_bdev_opal_set_lock_state_decoders[] = {
	{"bdev_name", offsetof(struct rpc_bdev_opal_set_lock_state, bdev_name), spdk_json_decode_string},
	{"user_id", offsetof(struct rpc_bdev_opal_set_lock_state, user_id), spdk_json_decode_uint16},
	{"password", offsetof(struct rpc_bdev_opal_set_lock_state, password), spdk_json_decode_string},
	{"lock_state", offsetof(struct rpc_bdev_opal_set_lock_state, lock_state), spdk_json_decode_string},
};

static void
rpc_bdev_opal_set_lock_state(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_bdev_opal_set_lock_state req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_opal_set_lock_state_decoders,
				    SPDK_COUNTOF(rpc_bdev_opal_set_lock_state_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	rc = vbdev_opal_set_lock_state(req.bdev_name, req.user_id, req.password, req.lock_state);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
		goto out;
	}

	spdk_jsonrpc_send_bool_response(request, true);

out:
	free_rpc_bdev_opal_set_lock_state(&req);
}
SPDK_RPC_REGISTER("bdev_opal_set_lock_state", rpc_bdev_opal_set_lock_state, SPDK_RPC_RUNTIME)

struct rpc_bdev_opal_new_user {
	char *bdev_name;
	char *admin_password;
	uint16_t user_id;
	char *user_password;
};

static void
free_rpc_bdev_opal_new_user(struct rpc_bdev_opal_new_user *req)
{
	free(req->bdev_name);
	free(req->admin_password);
	free(req->user_password);
}

static const struct spdk_json_object_decoder rpc_bdev_opal_new_user_decoders[] = {
	{"bdev_name", offsetof(struct rpc_bdev_opal_new_user, bdev_name), spdk_json_decode_string},
	{"admin_password", offsetof(struct rpc_bdev_opal_new_user, admin_password), spdk_json_decode_string},
	{"user_id", offsetof(struct rpc_bdev_opal_new_user, user_id), spdk_json_decode_uint16},
	{"user_password", offsetof(struct rpc_bdev_opal_new_user, user_password), spdk_json_decode_string},
};

static void
rpc_bdev_opal_new_user(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_bdev_opal_new_user req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_opal_new_user_decoders,
				    SPDK_COUNTOF(rpc_bdev_opal_new_user_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		goto out;
	}

	rc = vbdev_opal_enable_new_user(req.bdev_name, req.admin_password, req.user_id,
					req.user_password);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, spdk_strerror(-rc));
		goto out;
	}

	spdk_jsonrpc_send_bool_response(request, true);

out:
	free_rpc_bdev_opal_new_user(&req);
}
SPDK_RPC_REGISTER("bdev_opal_new_user", rpc_bdev_opal_new_user, SPDK_RPC_RUNTIME)
