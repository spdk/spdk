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
	char buf[64];

	if (spdk_json_decode_object(params, rpc_construct_vhost_ctrlr,
				    SPDK_COUNTOF(rpc_construct_vhost_ctrlr),
				    &req)) {
		SPDK_DEBUGLOG(SPDK_TRACE_VHOST_RPC, "spdk_json_decode_object failed\n");
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
	spdk_strerror_r(-rc, buf, sizeof(buf));
	free_rpc_vhost_scsi_ctrlr(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, buf);
}
SPDK_RPC_REGISTER("construct_vhost_scsi_controller", spdk_rpc_construct_vhost_scsi_controller)

struct rpc_remove_vhost_scsi_ctrlr {
	char *ctrlr;
};

static void
free_rpc_remove_vhost_scsi_ctrlr(struct rpc_remove_vhost_scsi_ctrlr *req)
{
	free(req->ctrlr);
}

static const struct spdk_json_object_decoder rpc_remove_vhost_ctrlr[] = {
	{"ctrlr", offsetof(struct rpc_remove_vhost_scsi_ctrlr, ctrlr), spdk_json_decode_string },
};

static void
spdk_rpc_remove_vhost_scsi_controller(struct spdk_jsonrpc_request *request,
				      const struct spdk_json_val *params)
{
	struct rpc_remove_vhost_scsi_ctrlr req = {NULL};
	struct spdk_json_write_ctx *w;
	struct spdk_vhost_dev *vdev;
	int rc;
	char buf[64];

	if (spdk_json_decode_object(params, rpc_remove_vhost_ctrlr,
				    SPDK_COUNTOF(rpc_remove_vhost_ctrlr),
				    &req)) {
		SPDK_DEBUGLOG(SPDK_TRACE_VHOST_RPC, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	if (!(vdev = spdk_vhost_dev_find(req.ctrlr))) {
		rc = -ENODEV;
		goto invalid;
	}

	rc = spdk_vhost_scsi_dev_remove(vdev);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_remove_vhost_scsi_ctrlr(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_strerror_r(-rc, buf, sizeof(buf));
	free_rpc_remove_vhost_scsi_ctrlr(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, buf);
}
SPDK_RPC_REGISTER("remove_vhost_scsi_controller", spdk_rpc_remove_vhost_scsi_controller)


struct rpc_add_vhost_scsi_ctrlr_lun {
	char *ctrlr;
	uint32_t scsi_dev_num;
	char *lun_name;
};

static void
free_rpc_add_vhost_scsi_ctrlr_lun(struct rpc_add_vhost_scsi_ctrlr_lun *req)
{
	free(req->ctrlr);
	free(req->lun_name);
}

static const struct spdk_json_object_decoder rpc_vhost_add_lun[] = {
	{"ctrlr", offsetof(struct rpc_add_vhost_scsi_ctrlr_lun, ctrlr), spdk_json_decode_string },
	{"scsi_dev_num", offsetof(struct rpc_add_vhost_scsi_ctrlr_lun, scsi_dev_num), spdk_json_decode_uint32},
	{"lun_name", offsetof(struct rpc_add_vhost_scsi_ctrlr_lun, lun_name), spdk_json_decode_string },
};

static void
spdk_rpc_add_vhost_scsi_lun(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_add_vhost_scsi_ctrlr_lun req = {0};
	struct spdk_json_write_ctx *w;
	int rc;
	char buf[64];

	if (spdk_json_decode_object(params, rpc_vhost_add_lun,
				    SPDK_COUNTOF(rpc_vhost_add_lun),
				    &req)) {
		SPDK_DEBUGLOG(SPDK_TRACE_VHOST_RPC, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_vhost_scsi_dev_add_dev(req.ctrlr, req.scsi_dev_num, req.lun_name);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_add_vhost_scsi_ctrlr_lun(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_strerror_r(-rc, buf, sizeof(buf));
	free_rpc_add_vhost_scsi_ctrlr_lun(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, buf);
}
SPDK_RPC_REGISTER("add_vhost_scsi_lun", spdk_rpc_add_vhost_scsi_lun)

struct rpc_remove_vhost_scsi_ctrlr_dev {
	char *ctrlr;
	uint32_t scsi_dev_num;
};

static void
free_rpc_remove_vhost_scsi_ctrlr_dev(struct rpc_remove_vhost_scsi_ctrlr_dev *req)
{
	free(req->ctrlr);
}

static const struct spdk_json_object_decoder rpc_vhost_remove_dev[] = {
	{"ctrlr", offsetof(struct rpc_remove_vhost_scsi_ctrlr_dev, ctrlr), spdk_json_decode_string },
	{"scsi_dev_num", offsetof(struct rpc_remove_vhost_scsi_ctrlr_dev, scsi_dev_num), spdk_json_decode_uint32},
};

static void
spdk_rpc_remove_vhost_scsi_dev(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_remove_vhost_scsi_ctrlr_dev req = {0};
	struct spdk_json_write_ctx *w;
	struct spdk_vhost_dev *vdev;
	int rc;
	char buf[64];

	if (spdk_json_decode_object(params, rpc_vhost_remove_dev,
				    SPDK_COUNTOF(rpc_vhost_remove_dev),
				    &req)) {
		SPDK_DEBUGLOG(SPDK_TRACE_VHOST_RPC, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	if (!(vdev = spdk_vhost_dev_find(req.ctrlr))) {
		rc = -ENODEV;
		goto invalid;
	}

	rc = spdk_vhost_scsi_dev_remove_dev(vdev, req.scsi_dev_num);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_remove_vhost_scsi_ctrlr_dev(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_strerror_r(-rc, buf, sizeof(buf));
	free_rpc_remove_vhost_scsi_ctrlr_dev(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, buf);
}
SPDK_RPC_REGISTER("remove_vhost_scsi_dev", spdk_rpc_remove_vhost_scsi_dev)

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
	char buf[64];

	if (spdk_json_decode_object(params, rpc_construct_vhost_blk_ctrlr,
				    SPDK_COUNTOF(rpc_construct_vhost_blk_ctrlr),
				    &req)) {
		SPDK_DEBUGLOG(SPDK_TRACE_VHOST_RPC, "spdk_json_decode_object failed\n");
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
	spdk_strerror_r(-rc, buf, sizeof(buf));
	free_rpc_vhost_blk_ctrlr(&req);
	spdk_jsonrpc_send_error_response(request,
					 SPDK_JSONRPC_ERROR_INVALID_PARAMS, buf);

}
SPDK_RPC_REGISTER("construct_vhost_blk_controller", spdk_rpc_construct_vhost_blk_controller)

struct rpc_remove_vhost_blk_ctrlr {
	char *ctrlr;
};

static const struct spdk_json_object_decoder rpc_remove_vhost_blk_ctrlr[] = {
	{"ctrlr", offsetof(struct rpc_remove_vhost_blk_ctrlr, ctrlr), spdk_json_decode_string },
};

static void
free_rpc_remove_vhost_blk_ctrlr(struct rpc_remove_vhost_blk_ctrlr *req)
{
	free(req->ctrlr);
}

static void
spdk_rpc_remove_vhost_blk_controller(struct spdk_jsonrpc_request *request,
				     const struct spdk_json_val *params)
{
	struct rpc_remove_vhost_blk_ctrlr req = {NULL};
	struct spdk_json_write_ctx *w;
	struct spdk_vhost_dev *vdev;
	int rc;
	char buf[64];

	if (spdk_json_decode_object(params, rpc_remove_vhost_blk_ctrlr,
				    SPDK_COUNTOF(rpc_remove_vhost_blk_ctrlr), &req)) {
		SPDK_DEBUGLOG(SPDK_TRACE_VHOST_RPC, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	if (!(vdev = spdk_vhost_dev_find(req.ctrlr))) {
		rc = -ENODEV;
		goto invalid;
	}

	rc = spdk_vhost_blk_destroy(vdev);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_remove_vhost_blk_ctrlr(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_strerror_r(-rc, buf, sizeof(buf));
	free_rpc_remove_vhost_blk_ctrlr(&req);
	spdk_jsonrpc_send_error_response(request,
					 SPDK_JSONRPC_ERROR_INVALID_PARAMS, buf);

}
SPDK_RPC_REGISTER("remove_vhost_blk_controller", spdk_rpc_remove_vhost_blk_controller)

static void
spdk_rpc_get_vhost_controllers(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct spdk_vhost_dev *vdev = NULL;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_vhost_controllers requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);
	while ((vdev = spdk_vhost_dev_next(vdev)) != NULL) {
		spdk_json_write_object_begin(w);

		spdk_json_write_name(w, "ctrlr");
		spdk_json_write_string(w, spdk_vhost_dev_get_name(vdev));

		spdk_json_write_name(w, "cpumask");
		spdk_json_write_string_fmt(w, "%#" PRIx64, spdk_vhost_dev_get_cpumask(vdev));

		spdk_json_write_name(w, "backend_specific");

		spdk_json_write_object_begin(w);
		spdk_vhost_dump_config_json(vdev, w);
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_vhost_controllers", spdk_rpc_get_vhost_controllers)

SPDK_LOG_REGISTER_TRACE_FLAG("vhost_rpc", SPDK_TRACE_VHOST_RPC)
