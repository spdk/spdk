/*-
 *   BSD LICENSE
 *
 *   Copyright(c) Intel Corporation. All rights reserved.
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

#include "spdk_internal/log.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/env.h"

#include "spdk/scsi.h"
#include "spdk/vhost.h"
#include "vhost_internal.h"
#include "spdk/bdev.h"

struct rpc_vhost_scsi_ctrlr {
	char *ctrlr;
	char *cpumask;
};

static void
free_rpc_vhost_scsi_ctrlr(struct rpc_vhost_scsi_ctrlr *req)
{
	free(req->ctrlr);
	free(req->cpumask);
}

static const struct spdk_json_object_decoder rpc_construct_vhost_ctrlr[] = {
	{"ctrlr", offsetof(struct rpc_vhost_scsi_ctrlr, ctrlr), spdk_json_decode_string },
	{"cpumask", offsetof(struct rpc_vhost_scsi_ctrlr, cpumask), spdk_json_decode_string, true},
};

static void
spdk_rpc_construct_vhost_scsi_controller(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct rpc_vhost_scsi_ctrlr req = {0};
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_vhost_ctrlr,
				    SPDK_COUNTOF(rpc_construct_vhost_ctrlr),
				    &req)) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_RPC, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_vhost_scsi_dev_construct(req.ctrlr, req.cpumask);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_vhost_scsi_ctrlr(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	free_rpc_vhost_scsi_ctrlr(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("construct_vhost_scsi_controller", spdk_rpc_construct_vhost_scsi_controller,
		  SPDK_RPC_RUNTIME)

struct rpc_add_vhost_scsi_ctrlr_lun {
	char *ctrlr;
	uint32_t scsi_target_num;
	char *bdev_name;

	struct spdk_jsonrpc_request *request;
};

static void
free_rpc_add_vhost_scsi_ctrlr_lun(struct rpc_add_vhost_scsi_ctrlr_lun *req)
{
	free(req->ctrlr);
	free(req->bdev_name);
	free(req);
}

static const struct spdk_json_object_decoder rpc_vhost_add_lun[] = {
	{"ctrlr", offsetof(struct rpc_add_vhost_scsi_ctrlr_lun, ctrlr), spdk_json_decode_string },
	{"scsi_target_num", offsetof(struct rpc_add_vhost_scsi_ctrlr_lun, scsi_target_num), spdk_json_decode_uint32},
	{"bdev_name", offsetof(struct rpc_add_vhost_scsi_ctrlr_lun, bdev_name), spdk_json_decode_string },
};

static int
spdk_rpc_add_vhost_scsi_lun_cb(struct spdk_vhost_dev *vdev, void *arg)
{
	struct rpc_add_vhost_scsi_ctrlr_lun *rpc = arg;
	struct spdk_jsonrpc_request *request = rpc->request;
	struct spdk_json_write_ctx *w;
	int rc;

	if (vdev == NULL) {
		rc = -ENODEV;
		goto invalid;
	}

	rc = spdk_vhost_scsi_dev_add_tgt(vdev, rpc->scsi_target_num, rpc->bdev_name);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_add_vhost_scsi_ctrlr_lun(rpc);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return -1;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return 0;

invalid:
	free_rpc_add_vhost_scsi_ctrlr_lun(rpc);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	return rc;
}

static void
spdk_rpc_add_vhost_scsi_lun(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_add_vhost_scsi_ctrlr_lun *req;
	int rc;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		rc = -ENOMEM;
		goto invalid;
	}

	req->request = request;
	if (spdk_json_decode_object(params, rpc_vhost_add_lun,
				    SPDK_COUNTOF(rpc_vhost_add_lun),
				    req)) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_RPC, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	if (req->ctrlr == NULL) {
		SPDK_ERRLOG("No controller name\n");
		rc = -EINVAL;
		goto invalid;
	}

	spdk_vhost_call_external_event(req->ctrlr, spdk_rpc_add_vhost_scsi_lun_cb, req);

	return;

invalid:
	if (req) {
		free_rpc_add_vhost_scsi_ctrlr_lun(req);
	}
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("add_vhost_scsi_lun", spdk_rpc_add_vhost_scsi_lun, SPDK_RPC_RUNTIME)

struct rpc_remove_vhost_scsi_ctrlr_target {
	char *ctrlr;
	uint32_t scsi_target_num;

	struct spdk_jsonrpc_request *request;
};

static void
free_rpc_remove_vhost_scsi_ctrlr_target(struct rpc_remove_vhost_scsi_ctrlr_target *req)
{
	free(req->ctrlr);
	free(req);
}

static const struct spdk_json_object_decoder rpc_vhost_remove_target[] = {
	{"ctrlr", offsetof(struct rpc_remove_vhost_scsi_ctrlr_target, ctrlr), spdk_json_decode_string },
	{"scsi_target_num", offsetof(struct rpc_remove_vhost_scsi_ctrlr_target, scsi_target_num), spdk_json_decode_uint32},
};

static int
spdk_rpc_remove_vhost_scsi_target_finish_cb(struct spdk_vhost_dev *vdev, void *arg)
{
	struct rpc_remove_vhost_scsi_ctrlr_target *rpc = arg;
	struct spdk_jsonrpc_request *request = rpc->request;
	struct spdk_json_write_ctx *w;

	free_rpc_remove_vhost_scsi_ctrlr_target(rpc);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return -1;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return 0;
}

static int
spdk_rpc_remove_vhost_scsi_target_cb(struct spdk_vhost_dev *vdev, void *arg)
{
	struct rpc_remove_vhost_scsi_ctrlr_target *rpc = arg;
	struct spdk_jsonrpc_request *request = rpc->request;
	int rc;

	if (vdev == NULL) {
		rc = -ENODEV;
		goto invalid;
	}

	rc = spdk_vhost_scsi_dev_remove_tgt(vdev, rpc->scsi_target_num,
					    spdk_rpc_remove_vhost_scsi_target_finish_cb, rpc);
	if (rc < 0) {
		goto invalid;
	}

	return 0;

invalid:
	free_rpc_remove_vhost_scsi_ctrlr_target(rpc);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
	return rc;
}

static void
spdk_rpc_remove_vhost_scsi_target(struct spdk_jsonrpc_request *request,
				  const struct spdk_json_val *params)
{
	struct rpc_remove_vhost_scsi_ctrlr_target *req;
	int rc;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		rc = -ENOMEM;
		goto invalid;
	}

	req->request = request;
	if (spdk_json_decode_object(params, rpc_vhost_remove_target,
				    SPDK_COUNTOF(rpc_vhost_remove_target),
				    req)) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_RPC, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	spdk_vhost_call_external_event(req->ctrlr, spdk_rpc_remove_vhost_scsi_target_cb, req);

	return;

invalid:
	if (req) {
		free_rpc_remove_vhost_scsi_ctrlr_target(req);
	}
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}

SPDK_RPC_REGISTER("remove_vhost_scsi_target", spdk_rpc_remove_vhost_scsi_target, SPDK_RPC_RUNTIME)

struct rpc_vhost_blk_ctrlr {
	char *ctrlr;
	char *dev_name;
	char *cpumask;
	bool readonly;
};

static const struct spdk_json_object_decoder rpc_construct_vhost_blk_ctrlr[] = {
	{"ctrlr", offsetof(struct rpc_vhost_blk_ctrlr, ctrlr), spdk_json_decode_string },
	{"dev_name", offsetof(struct rpc_vhost_blk_ctrlr, dev_name), spdk_json_decode_string },
	{"cpumask", offsetof(struct rpc_vhost_blk_ctrlr, cpumask), spdk_json_decode_string, true},
	{"readonly", offsetof(struct rpc_vhost_blk_ctrlr, readonly), spdk_json_decode_bool, true},
};

static void
free_rpc_vhost_blk_ctrlr(struct rpc_vhost_blk_ctrlr *req)
{
	free(req->ctrlr);
	free(req->dev_name);
	free(req->cpumask);
}

static void
spdk_rpc_construct_vhost_blk_controller(struct spdk_jsonrpc_request *request,
					const struct spdk_json_val *params)
{
	struct rpc_vhost_blk_ctrlr req = {0};
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_vhost_blk_ctrlr,
				    SPDK_COUNTOF(rpc_construct_vhost_blk_ctrlr),
				    &req)) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_RPC, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_vhost_blk_construct(req.ctrlr, req.cpumask, req.dev_name, req.readonly);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_vhost_blk_ctrlr(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	free_rpc_vhost_blk_ctrlr(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));

}
SPDK_RPC_REGISTER("construct_vhost_blk_controller", spdk_rpc_construct_vhost_blk_controller,
		  SPDK_RPC_RUNTIME)

struct rpc_remove_vhost_ctrlr {
	char *ctrlr;

	struct spdk_jsonrpc_request *request;
};

static const struct spdk_json_object_decoder rpc_remove_vhost_ctrlr[] = {
	{"ctrlr", offsetof(struct rpc_remove_vhost_ctrlr, ctrlr), spdk_json_decode_string },
};

static void
free_rpc_remove_vhost_ctrlr(struct rpc_remove_vhost_ctrlr *req)
{
	free(req->ctrlr);
	free(req);
}

static int
spdk_rpc_remove_vhost_controller_cb(struct spdk_vhost_dev *vdev, void *arg)
{
	struct rpc_remove_vhost_ctrlr *ctx = arg;
	struct spdk_jsonrpc_request *request = ctx->request;
	struct spdk_json_write_ctx *w;
	int rc;

	if (vdev == NULL) {
		rc = -ENODEV;
		goto invalid;
	}

	rc = spdk_vhost_dev_remove(vdev);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_remove_vhost_ctrlr(ctx);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return 0;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return 0;

invalid:
	free_rpc_remove_vhost_ctrlr(ctx);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	return -1;
}

static void
spdk_rpc_remove_vhost_controller(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_remove_vhost_ctrlr *req;
	int rc;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		rc = -ENOMEM;
		goto invalid;
	}

	req->request = request;
	if (spdk_json_decode_object(params, rpc_remove_vhost_ctrlr,
				    SPDK_COUNTOF(rpc_remove_vhost_ctrlr), req)) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_RPC, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	spdk_vhost_call_external_event(req->ctrlr, spdk_rpc_remove_vhost_controller_cb, req);
	return;

invalid:
	if (req) {
		free_rpc_remove_vhost_ctrlr(req);
	}
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));

}
SPDK_RPC_REGISTER("remove_vhost_controller", spdk_rpc_remove_vhost_controller, SPDK_RPC_RUNTIME)

struct rpc_get_vhost_ctrlrs {
	char *name;
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_request *request;
};

static void
_spdk_rpc_get_vhost_controller(struct spdk_json_write_ctx *w, struct spdk_vhost_dev *vdev)
{
	uint32_t delay_base_us, iops_threshold;

	spdk_vhost_get_coalescing(vdev, &delay_base_us, &iops_threshold);

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "ctrlr", spdk_vhost_dev_get_name(vdev));
	spdk_json_write_named_string_fmt(w, "cpumask", "0x%s", spdk_cpuset_fmt(vdev->cpumask));
	spdk_json_write_named_uint32(w, "delay_base_us", delay_base_us);
	spdk_json_write_named_uint32(w, "iops_threshold", iops_threshold);
	spdk_json_write_named_string(w, "socket", vdev->path);

	spdk_json_write_named_object_begin(w, "backend_specific");
	spdk_vhost_dump_info_json(vdev, w);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static int
spdk_rpc_get_vhost_controllers_cb(struct spdk_vhost_dev *vdev, void *arg)
{
	struct rpc_get_vhost_ctrlrs *ctx = arg;

	assert(ctx->name == NULL);

	if (vdev == NULL) {
		spdk_json_write_array_end(ctx->w);
		spdk_jsonrpc_end_result(ctx->request, ctx->w);
		free(ctx);
		return 0;
	}

	_spdk_rpc_get_vhost_controller(ctx->w, vdev);
	return 0;
}

static int
spdk_rpc_get_vhost_controller_cb(struct spdk_vhost_dev *vdev, void *arg)
{
	struct rpc_get_vhost_ctrlrs *ctx = arg;

	assert(ctx->name != NULL);

	if (vdev == NULL) {
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 spdk_strerror(ENODEV));
		goto free_name_ctx;
	}

	ctx->w = spdk_jsonrpc_begin_result(ctx->request);
	if (ctx->w == NULL) {
		goto free_name_ctx;
	}

	spdk_json_write_array_begin(ctx->w);
	_spdk_rpc_get_vhost_controller(ctx->w, vdev);
	spdk_json_write_array_end(ctx->w);

	spdk_jsonrpc_end_result(ctx->request, ctx->w);

free_name_ctx:
	free(ctx->name);
	free(ctx);
	return 0;
}

static const struct spdk_json_object_decoder rpc_get_vhost_ctrlrs_decoders[] = {
	{"name", offsetof(struct rpc_get_vhost_ctrlrs, name), spdk_json_decode_string, true},
};

static void
spdk_rpc_get_vhost_controllers(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_get_vhost_ctrlrs *ctx;
	struct spdk_json_write_ctx *w;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(ENOMEM));
		return;
	}

	if (params && spdk_json_decode_object(params, rpc_get_vhost_ctrlrs_decoders,
					      SPDK_COUNTOF(rpc_get_vhost_ctrlrs_decoders), ctx)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		free(ctx);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}

	if (ctx->name) {
		ctx->request = request;
		spdk_vhost_call_external_event(ctx->name, spdk_rpc_get_vhost_controller_cb, ctx);
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		free(ctx);
		return;
	}

	spdk_json_write_array_begin(w);

	ctx->w = w;
	ctx->request = request;
	spdk_vhost_call_external_event_foreach(spdk_rpc_get_vhost_controllers_cb, ctx);
}
SPDK_RPC_REGISTER("get_vhost_controllers", spdk_rpc_get_vhost_controllers, SPDK_RPC_RUNTIME)


struct rpc_vhost_ctrlr_coalescing {
	char *ctrlr;
	uint32_t delay_base_us;
	uint32_t iops_threshold;
	struct spdk_jsonrpc_request *request;
};

static const struct spdk_json_object_decoder rpc_set_vhost_ctrlr_coalescing[] = {
	{"ctrlr", offsetof(struct rpc_vhost_ctrlr_coalescing, ctrlr), spdk_json_decode_string },
	{"delay_base_us", offsetof(struct rpc_vhost_ctrlr_coalescing, delay_base_us), spdk_json_decode_uint32},
	{"iops_threshold", offsetof(struct rpc_vhost_ctrlr_coalescing, iops_threshold), spdk_json_decode_uint32},
};

static void
free_rpc_set_vhost_controllers_event_coalescing(struct rpc_vhost_ctrlr_coalescing *req)
{
	if (!req) {
		return;
	}

	free(req->ctrlr);
	free(req);
}

static int
spdk_rpc_set_vhost_controller_coalescing_cb(struct spdk_vhost_dev *vdev, void *arg)
{
	struct rpc_vhost_ctrlr_coalescing *req = arg;
	struct spdk_json_write_ctx *w;
	int rc;

	if (vdev == NULL) {
		rc = -ENODEV;
		goto invalid;
	}

	rc = spdk_vhost_set_coalescing(vdev, req->delay_base_us, req->iops_threshold);
	if (rc) {
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(req->request);
	if (w != NULL) {
		spdk_json_write_bool(w, true);
		spdk_jsonrpc_end_result(req->request, w);
	}

	free_rpc_set_vhost_controllers_event_coalescing(req);
	return 0;

invalid:
	spdk_jsonrpc_send_error_response(req->request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	free_rpc_set_vhost_controllers_event_coalescing(req);
	return 0;
}

static void
spdk_rpc_set_vhost_controller_coalescing(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct rpc_vhost_ctrlr_coalescing *req;
	int rc;

	req = calloc(1, sizeof(struct rpc_vhost_ctrlr_coalescing));
	if (!req) {
		rc = -ENOMEM;
		goto invalid;
	}

	if (spdk_json_decode_object(params, rpc_set_vhost_ctrlr_coalescing,
				    SPDK_COUNTOF(rpc_set_vhost_ctrlr_coalescing), req)) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_RPC, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	req->request = request;
	spdk_vhost_call_external_event(req->ctrlr, spdk_rpc_set_vhost_controller_coalescing_cb, req);
	return;

invalid:
	free_rpc_set_vhost_controllers_event_coalescing(req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("set_vhost_controller_coalescing", spdk_rpc_set_vhost_controller_coalescing,
		  SPDK_RPC_RUNTIME)

struct rpc_vhost_nvme_ctrlr {
	char *ctrlr;
	uint32_t io_queues;
	char *cpumask;
};

static const struct spdk_json_object_decoder rpc_construct_vhost_nvme_ctrlr[] = {
	{"ctrlr", offsetof(struct rpc_vhost_nvme_ctrlr, ctrlr), spdk_json_decode_string },
	{"io_queues", offsetof(struct rpc_vhost_nvme_ctrlr, io_queues), spdk_json_decode_uint32},
	{"cpumask", offsetof(struct rpc_vhost_nvme_ctrlr, cpumask), spdk_json_decode_string, true},
};

static void
free_rpc_vhost_nvme_ctrlr(struct rpc_vhost_nvme_ctrlr *req)
{
	free(req->ctrlr);
	free(req->cpumask);
}

static void
spdk_rpc_construct_vhost_nvme_controller(struct spdk_jsonrpc_request *request,
		const struct spdk_json_val *params)
{
	struct rpc_vhost_nvme_ctrlr req = {0};
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_vhost_nvme_ctrlr,
				    SPDK_COUNTOF(rpc_construct_vhost_nvme_ctrlr),
				    &req)) {
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_vhost_nvme_dev_construct(req.ctrlr, req.cpumask, req.io_queues);
	if (rc < 0) {
		free_rpc_vhost_nvme_ctrlr(&req);
		goto invalid;
	}

	free_rpc_vhost_nvme_ctrlr(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));

}
SPDK_RPC_REGISTER("construct_vhost_nvme_controller", spdk_rpc_construct_vhost_nvme_controller,
		  SPDK_RPC_RUNTIME)

struct rpc_add_vhost_nvme_ctrlr_ns {
	char *ctrlr;
	char *bdev_name;
	struct spdk_jsonrpc_request *request;
};

static void
free_rpc_add_vhost_nvme_ctrlr_ns(struct rpc_add_vhost_nvme_ctrlr_ns *req)
{
	free(req->ctrlr);
	free(req->bdev_name);
	free(req);
}

static const struct spdk_json_object_decoder rpc_vhost_nvme_add_ns[] = {
	{"ctrlr", offsetof(struct rpc_add_vhost_nvme_ctrlr_ns, ctrlr), spdk_json_decode_string },
	{"bdev_name", offsetof(struct rpc_add_vhost_nvme_ctrlr_ns, bdev_name), spdk_json_decode_string },
};

static int
spdk_rpc_add_vhost_nvme_ns_cb(struct spdk_vhost_dev *vdev, void *arg)
{
	struct rpc_add_vhost_nvme_ctrlr_ns *rpc = arg;
	struct spdk_jsonrpc_request *request = rpc->request;
	struct spdk_json_write_ctx *w;
	int rc;

	if (vdev == NULL) {
		rc = -ENODEV;
		goto invalid;
	}

	rc = spdk_vhost_nvme_dev_add_ns(vdev, rpc->bdev_name);
	if (rc < 0) {
		goto invalid;
	}
	free_rpc_add_vhost_nvme_ctrlr_ns(rpc);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return -1;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return 0;

invalid:
	free_rpc_add_vhost_nvme_ctrlr_ns(rpc);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
	return rc;
}

static void
spdk_rpc_add_vhost_nvme_ns(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_add_vhost_nvme_ctrlr_ns *req;
	int rc;

	req = calloc(1, sizeof(*req));
	if (req == NULL) {
		rc = -ENOMEM;
		goto invalid;
	}

	req->request = request;
	if (spdk_json_decode_object(params, rpc_vhost_nvme_add_ns,
				    SPDK_COUNTOF(rpc_vhost_nvme_add_ns),
				    req)) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_RPC, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	spdk_vhost_call_external_event(req->ctrlr, spdk_rpc_add_vhost_nvme_ns_cb, req);
	return;

invalid:
	if (req) {
		free_rpc_add_vhost_nvme_ctrlr_ns(req);
	}
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("add_vhost_nvme_ns", spdk_rpc_add_vhost_nvme_ns, SPDK_RPC_RUNTIME)


SPDK_LOG_REGISTER_COMPONENT("vhost_rpc", SPDK_LOG_VHOST_RPC)
