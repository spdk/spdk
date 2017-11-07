/*-
#include <jsonrpc_util.h>

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
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/env.h"

#include "spdk/scsi.h"
#include "spdk/vhost.h"
#include "spdk/jsonrpc_util.h"
#include "vhost_internal.h"
#include "spdk/bdev.h"

static const struct spdk_jsonrpc_params construct_vhost_scsi_controller_params[] = {
	{"ctrlr", SPDK_RPC_PARAM_STRING},
	{"cpumask", SPDK_RPC_PARAM_STRING, true},
	{NULL}
};

static void
construct_vhost_scsi_controller(struct spdk_jsonrpc_util_req *req)
{
	int rc = spdk_vhost_scsi_dev_construct(spdk_jsonrpc_param_str(req, "ctrlr", NULL),
					       spdk_jsonrpc_param_str(req, "cpumask", NULL));

	spdk_jsonrpc_send_errno_response(req, rc);
}
SPDK_JSONRPC_CMD(construct_vhost_scsi_controller, construct_vhost_scsi_controller_params)

static const struct spdk_jsonrpc_params add_vhost_scsi_lun_params[] = {
	{"ctrlr", SPDK_RPC_PARAM_STRING},
	{"scsi_dev_num", SPDK_RPC_PARAM_UINT32},
	{"lun_name", SPDK_RPC_PARAM_STRING},
	{NULL}
};

static int
add_vhost_scsi_lun_cb(struct spdk_vhost_dev *vdev, void *arg)
{
	struct spdk_jsonrpc_util_req *req = arg;
	int rc = -ENODEV;

	if (vdev != NULL) {
		rc = spdk_vhost_scsi_dev_add_dev(vdev, spdk_jsonrpc_param_uint32(req, "scsi_dev_num", UINT32_MAX),
						 spdk_jsonrpc_param_str(req, "lun_name", NULL));
	}

	spdk_jsonrpc_send_errno_response(req, rc);
	return rc;
}

static void
add_vhost_scsi_lun(struct spdk_jsonrpc_util_req *req)
{
	spdk_vhost_call_external_event(spdk_jsonrpc_param_str(req, "ctrlr", NULL), add_vhost_scsi_lun_cb,
				       req);
}
SPDK_JSONRPC_CMD(add_vhost_scsi_lun, add_vhost_scsi_lun_params)

static const struct spdk_jsonrpc_params remove_vhost_scsi_dev_params[] = {
	{"ctrlr", SPDK_RPC_PARAM_STRING },
	{"scsi_dev_num", SPDK_RPC_PARAM_UINT32},
	{NULL}
};

static int
spdk_rpc_remove_vhost_scsi_dev_finish_cb(struct spdk_vhost_dev *vdev, void *arg)
{
	spdk_jsonrpc_send_response(arg, true, NULL);
	return 0;
}

static int
remove_vhost_scsi_dev_cb(struct spdk_vhost_dev *vdev, void *arg)
{
	struct spdk_jsonrpc_util_req *req = arg;
	int rc = -ENODEV;

	if (vdev != NULL) {
		rc = spdk_vhost_scsi_dev_remove_dev(vdev, spdk_jsonrpc_param_uint32(req, "scsi_dev_num",
						    UINT32_MAX), spdk_rpc_remove_vhost_scsi_dev_finish_cb, req);
	}

	if (rc) {
		spdk_jsonrpc_send_errno_response(req, rc);
	}

	return rc;
}

static void
remove_vhost_scsi_dev(struct spdk_jsonrpc_util_req *req)
{
	spdk_vhost_call_external_event(spdk_jsonrpc_param_str(req, "ctrlr", NULL), remove_vhost_scsi_dev_cb,
				       req);
}

SPDK_JSONRPC_CMD(remove_vhost_scsi_dev, remove_vhost_scsi_dev_params)

static const struct spdk_jsonrpc_params construct_vhost_blk_controller_params[] = {
	{"ctrlr", SPDK_RPC_PARAM_STRING },
	{"dev_name", SPDK_RPC_PARAM_STRING },
	{"cpumask", SPDK_RPC_PARAM_STRING, true},
	{"readonly", SPDK_RPC_PARAM_BOOL, true},
	{NULL}
};

static void
construct_vhost_blk_controller(struct spdk_jsonrpc_util_req *req)
{
	int rc = spdk_vhost_blk_construct(spdk_jsonrpc_param_str(req, "ctrlr", NULL),
					  spdk_jsonrpc_param_str(req, "cpumask", NULL),
					  spdk_jsonrpc_param_str(req, "dev_name", NULL),
					  spdk_jsonrpc_param_bool(req, "readonly", false));

	spdk_jsonrpc_send_errno_response(req, rc);
}
SPDK_JSONRPC_CMD(construct_vhost_blk_controller, construct_vhost_blk_controller_params)

static const struct spdk_jsonrpc_params remove_vhost_controller_params[] = {
	{"ctrlr", SPDK_RPC_PARAM_STRING},
	{NULL}
};

static int
remove_vhost_controller_cb(struct spdk_vhost_dev *vdev, void *arg)
{
	struct spdk_jsonrpc_util_req *req = arg;
	int rc = -ENODEV;

	if (vdev != NULL) {
		rc = spdk_remove_vhost_controller(vdev);
	}

	spdk_jsonrpc_send_errno_response(req, rc);
	return -1;
}

static void
remove_vhost_controller(struct spdk_jsonrpc_util_req *req)
{
	spdk_vhost_call_external_event(spdk_jsonrpc_param_str(req, "ctrlr", NULL),
				       remove_vhost_controller_cb, req);
	return;
}
SPDK_JSONRPC_CMD(remove_vhost_controller, remove_vhost_controller_params)

static int
get_vhost_controllers_cb(struct spdk_vhost_dev *vdev, void *arg)
{
	struct spdk_jsonrpc_util_req *req = arg;
	struct spdk_json_write_ctx *w;

	if (vdev == NULL) {
		spdk_jsonrpc_array_end(req);
		spdk_jsonrpc_end_response(req);
		return 0;
	}

	w = spdk_jsonrpc_response_ctx(req);
	if (!w) {
		return 0;
	}

	spdk_jsonrpc_object_begin(req);

	spdk_jsonrpc_string_create(req, "ctrlr", "%s", spdk_vhost_dev_get_name(vdev));
	spdk_jsonrpc_string_create(req, "cpumask", "%#" PRIx64, spdk_vhost_dev_get_cpumask(vdev));

	spdk_jsonrpc_object_create(req, "backend_specific");
	spdk_vhost_dump_config_json(vdev, w);
	spdk_jsonrpc_object_end(req);

	spdk_jsonrpc_object_end(req);

	return 0;
}


static void
get_vhost_controllers(struct spdk_jsonrpc_util_req *req)
{
	spdk_jsonrpc_array_begin(req);
	spdk_vhost_call_external_event_foreach(get_vhost_controllers_cb, req);
}
SPDK_JSONRPC_CMD(get_vhost_controllers, NULL)


static const struct spdk_jsonrpc_params set_vhost_controller_coalescing_params[] = {
	{"ctrlr", SPDK_RPC_PARAM_STRING},
	{"delay_base_us", SPDK_RPC_PARAM_UINT32},
	{"iops_threshold", SPDK_RPC_PARAM_UINT32},
	{NULL},
};

static int
set_vhost_controller_coalescing_cb(struct spdk_vhost_dev *vdev, void *arg)
{
	struct spdk_jsonrpc_util_req *req = arg;
	int rc = -ENODEV;

	if (vdev) {
		rc = spdk_vhost_set_coalescing(vdev, spdk_jsonrpc_param_uint32(req, "delay_base_us", 0),
					       spdk_jsonrpc_param_uint32(req, "iops_threshold", 0));
	}

	spdk_jsonrpc_send_errno_response(req, rc);
	return 0;
}

static void
set_vhost_controller_coalescing(struct spdk_jsonrpc_util_req *req)
{
	spdk_vhost_call_external_event(spdk_jsonrpc_param_str(req, "ctrlr", NULL),
				       set_vhost_controller_coalescing_cb, req);
}
SPDK_JSONRPC_CMD(set_vhost_controller_coalescing, set_vhost_controller_coalescing_params)
