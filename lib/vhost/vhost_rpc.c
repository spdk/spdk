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

#include "spdk/vhost.h"
#include "task.h"

static void
json_scsi_dev_write(struct spdk_json_write_ctx *ctx, struct spdk_scsi_dev *dev)
{
	int l, maxlun;

	spdk_json_write_name(ctx, "id");
	spdk_json_write_int32(ctx, spdk_scsi_dev_get_id(dev));

	spdk_json_write_name(ctx, "device_name");
	spdk_json_write_string(ctx, spdk_scsi_dev_get_name(dev));

	spdk_json_write_name(ctx, "luns");
	spdk_json_write_array_begin(ctx);
	maxlun = spdk_scsi_dev_get_max_lun(dev);
	for (l = 0; l < maxlun; l++) {
		struct spdk_scsi_lun *lun = spdk_scsi_dev_get_lun(dev, l);

		if (!lun) {
			continue;
		}

		spdk_json_write_object_begin(ctx);

		spdk_json_write_name(ctx, "id");
		spdk_json_write_int32(ctx, spdk_scsi_lun_get_id(lun));

		spdk_json_write_name(ctx, "name");
		spdk_json_write_string(ctx, spdk_scsi_lun_get_name(lun));

		spdk_json_write_object_end(ctx);
	}
	spdk_json_write_array_end(ctx);
}

static void
spdk_rpc_get_vhost_scsi_controllers(struct spdk_jsonrpc_server_conn *conn,
				    const struct spdk_json_val *params,
				    const struct spdk_json_val *id)
{
	struct spdk_json_write_ctx *w;
	struct spdk_vhost_scsi_ctrlr *ctrlr = NULL;
	struct spdk_scsi_dev *dev;
	uint32_t i;
	char buf[32];

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_vhost_scsi_controllers requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_array_begin(w);
	while ((ctrlr = spdk_vhost_scsi_ctrlr_next(ctrlr)) != NULL) {
		spdk_json_write_object_begin(w);

		spdk_json_write_name(w, "ctrlr");
		spdk_json_write_string(w, spdk_vhost_scsi_ctrlr_get_name(ctrlr));

		spdk_json_write_name(w, "cpu_mask");
		snprintf(buf, sizeof(buf), "%#" PRIx64, spdk_vhost_scsi_ctrlr_get_cpumask(ctrlr));
		spdk_json_write_string(w, buf);

		spdk_json_write_name(w, "scsi_devs");
		spdk_json_write_array_begin(w);

		for (i = 0; i < SPDK_VHOST_SCSI_CTRLR_MAX_DEVS; i++) {
			dev = spdk_vhost_scsi_ctrlr_get_dev(ctrlr, i);
			if (!dev)
				continue;

			spdk_json_write_object_begin(w);
			spdk_json_write_name(w, "scsi_dev_num");
			spdk_json_write_uint32(w, i);
			json_scsi_dev_write(w, dev);
			spdk_json_write_object_end(w);
		}

		spdk_json_write_array_end(w); // devs

		spdk_json_write_object_end(w); // ctrl
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(conn, w);
	return;
}
SPDK_RPC_REGISTER("get_vhost_scsi_controllers", spdk_rpc_get_vhost_scsi_controllers)

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
spdk_rpc_construct_vhost_scsi_controller(struct spdk_jsonrpc_server_conn *conn,
		const struct spdk_json_val *params,
		const struct spdk_json_val *id)
{
	struct rpc_vhost_scsi_ctrlr req = {0};
	struct spdk_json_write_ctx *w;
	int rc;
	uint64_t cpumask;

	if (spdk_json_decode_object(params, rpc_construct_vhost_ctrlr,
				    SPDK_COUNTOF(rpc_construct_vhost_ctrlr),
				    &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	cpumask = spdk_app_get_core_mask();
	if (req.cpumask != NULL && spdk_vhost_parse_core_mask(req.cpumask, &cpumask)) {
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_vhost_scsi_ctrlr_construct(req.ctrlr, cpumask);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_vhost_scsi_ctrlr(&req);

	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(conn, w);
	return;
invalid:
	free_rpc_vhost_scsi_ctrlr(&req);
	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS, strerror(-rc));
}
SPDK_RPC_REGISTER("construct_vhost_scsi_controller", spdk_rpc_construct_vhost_scsi_controller)

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
spdk_rpc_add_vhost_scsi_lun(struct spdk_jsonrpc_server_conn *conn,
			    const struct spdk_json_val *params,
			    const struct spdk_json_val *id)
{
	struct rpc_add_vhost_scsi_ctrlr_lun req = {0};
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_vhost_add_lun,
				    SPDK_COUNTOF(rpc_vhost_add_lun),
				    &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_vhost_scsi_ctrlr_add_dev(req.ctrlr, req.scsi_dev_num, req.lun_name);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_add_vhost_scsi_ctrlr_lun(&req);

	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(conn, w);
	return;
invalid:
	free_rpc_add_vhost_scsi_ctrlr_lun(&req);
	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS, strerror(-rc));
}
SPDK_RPC_REGISTER("add_vhost_scsi_lun", spdk_rpc_add_vhost_scsi_lun)
