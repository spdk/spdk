/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/env.h"
#include "spdk/log.h"
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

static const struct spdk_json_object_decoder rpc_vhost_create_scsi_ctrlr[] = {
	{"ctrlr", offsetof(struct rpc_vhost_scsi_ctrlr, ctrlr), spdk_json_decode_string },
	{"cpumask", offsetof(struct rpc_vhost_scsi_ctrlr, cpumask), spdk_json_decode_string, true},
};

static void
rpc_vhost_create_scsi_controller(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_vhost_scsi_ctrlr req = {0};
	int rc;

	if (spdk_json_decode_object(params, rpc_vhost_create_scsi_ctrlr,
				    SPDK_COUNTOF(rpc_vhost_create_scsi_ctrlr),
				    &req)) {
		SPDK_DEBUGLOG(vhost_rpc, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_vhost_scsi_dev_construct(req.ctrlr, req.cpumask);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_vhost_scsi_ctrlr(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	free_rpc_vhost_scsi_ctrlr(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("vhost_create_scsi_controller", rpc_vhost_create_scsi_controller,
		  SPDK_RPC_RUNTIME)

struct rpc_vhost_scsi_ctrlr_add_target {
	char *ctrlr;
	int32_t scsi_target_num;
	char *bdev_name;
};

static void
free_rpc_vhost_scsi_ctrlr_add_target(struct rpc_vhost_scsi_ctrlr_add_target *req)
{
	free(req->ctrlr);
	free(req->bdev_name);
}

static const struct spdk_json_object_decoder rpc_vhost_scsi_ctrlr_add_target[] = {
	{"ctrlr", offsetof(struct rpc_vhost_scsi_ctrlr_add_target, ctrlr), spdk_json_decode_string },
	{"scsi_target_num", offsetof(struct rpc_vhost_scsi_ctrlr_add_target, scsi_target_num), spdk_json_decode_int32},
	{"bdev_name", offsetof(struct rpc_vhost_scsi_ctrlr_add_target, bdev_name), spdk_json_decode_string },
};

static void
rpc_vhost_scsi_controller_add_target(struct spdk_jsonrpc_request *request,
				     const struct spdk_json_val *params)
{
	struct rpc_vhost_scsi_ctrlr_add_target req = {0};
	struct spdk_json_write_ctx *w;
	struct spdk_vhost_dev *vdev;
	int rc;

	if (spdk_json_decode_object(params, rpc_vhost_scsi_ctrlr_add_target,
				    SPDK_COUNTOF(rpc_vhost_scsi_ctrlr_add_target),
				    &req)) {
		SPDK_DEBUGLOG(vhost_rpc, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	spdk_vhost_lock();
	vdev = spdk_vhost_dev_find(req.ctrlr);
	if (vdev == NULL) {
		spdk_vhost_unlock();
		rc = -ENODEV;
		goto invalid;
	}

	rc = spdk_vhost_scsi_dev_add_tgt(vdev, req.scsi_target_num, req.bdev_name);
	spdk_vhost_unlock();
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_vhost_scsi_ctrlr_add_target(&req);

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_int32(w, rc);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	free_rpc_vhost_scsi_ctrlr_add_target(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("vhost_scsi_controller_add_target", rpc_vhost_scsi_controller_add_target,
		  SPDK_RPC_RUNTIME)

struct rpc_remove_vhost_scsi_ctrlr_target {
	char *ctrlr;
	uint32_t scsi_target_num;
};

static void
free_rpc_remove_vhost_scsi_ctrlr_target(struct rpc_remove_vhost_scsi_ctrlr_target *req)
{
	free(req->ctrlr);
}

static const struct spdk_json_object_decoder rpc_vhost_remove_target[] = {
	{"ctrlr", offsetof(struct rpc_remove_vhost_scsi_ctrlr_target, ctrlr), spdk_json_decode_string },
	{"scsi_target_num", offsetof(struct rpc_remove_vhost_scsi_ctrlr_target, scsi_target_num), spdk_json_decode_uint32},
};

static int
rpc_vhost_scsi_controller_remove_target_finish_cb(struct spdk_vhost_dev *vdev, void *arg)
{
	struct spdk_jsonrpc_request *request = arg;

	spdk_jsonrpc_send_bool_response(request, true);
	return 0;
}

static void
rpc_vhost_scsi_controller_remove_target(struct spdk_jsonrpc_request *request,
					const struct spdk_json_val *params)
{
	struct rpc_remove_vhost_scsi_ctrlr_target req = {0};
	struct spdk_vhost_dev *vdev;
	int rc;

	if (spdk_json_decode_object(params, rpc_vhost_remove_target,
				    SPDK_COUNTOF(rpc_vhost_remove_target),
				    &req)) {
		SPDK_DEBUGLOG(vhost_rpc, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	spdk_vhost_lock();
	vdev = spdk_vhost_dev_find(req.ctrlr);
	if (vdev == NULL) {
		spdk_vhost_unlock();
		rc = -ENODEV;
		goto invalid;
	}

	rc = spdk_vhost_scsi_dev_remove_tgt(vdev, req.scsi_target_num,
					    rpc_vhost_scsi_controller_remove_target_finish_cb,
					    request);
	spdk_vhost_unlock();
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_remove_vhost_scsi_ctrlr_target(&req);
	return;

invalid:
	free_rpc_remove_vhost_scsi_ctrlr_target(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}

SPDK_RPC_REGISTER("vhost_scsi_controller_remove_target",
		  rpc_vhost_scsi_controller_remove_target, SPDK_RPC_RUNTIME)

struct rpc_vhost_blk_ctrlr {
	char *ctrlr;
	char *dev_name;
	char *cpumask;
	char *transport;
};

static const struct spdk_json_object_decoder rpc_construct_vhost_blk_ctrlr[] = {
	{"ctrlr", offsetof(struct rpc_vhost_blk_ctrlr, ctrlr), spdk_json_decode_string },
	{"dev_name", offsetof(struct rpc_vhost_blk_ctrlr, dev_name), spdk_json_decode_string },
	{"cpumask", offsetof(struct rpc_vhost_blk_ctrlr, cpumask), spdk_json_decode_string, true},
	{"transport", offsetof(struct rpc_vhost_blk_ctrlr, transport), spdk_json_decode_string, true},
};

static void
free_rpc_vhost_blk_ctrlr(struct rpc_vhost_blk_ctrlr *req)
{
	free(req->ctrlr);
	free(req->dev_name);
	free(req->cpumask);
	free(req->transport);
}

static void
rpc_vhost_create_blk_controller(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_vhost_blk_ctrlr req = {0};
	int rc;

	if (spdk_json_decode_object_relaxed(params, rpc_construct_vhost_blk_ctrlr,
					    SPDK_COUNTOF(rpc_construct_vhost_blk_ctrlr),
					    &req)) {
		SPDK_DEBUGLOG(vhost_rpc, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	rc = spdk_vhost_blk_construct(req.ctrlr, req.cpumask, req.dev_name, req.transport, params);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_vhost_blk_ctrlr(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	free_rpc_vhost_blk_ctrlr(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));

}
SPDK_RPC_REGISTER("vhost_create_blk_controller", rpc_vhost_create_blk_controller,
		  SPDK_RPC_RUNTIME)

struct rpc_delete_vhost_ctrlr {
	char *ctrlr;
};

static const struct spdk_json_object_decoder rpc_delete_vhost_ctrlr_decoder[] = {
	{"ctrlr", offsetof(struct rpc_delete_vhost_ctrlr, ctrlr), spdk_json_decode_string },
};

static void
free_rpc_delete_vhost_ctrlr(struct rpc_delete_vhost_ctrlr *req)
{
	free(req->ctrlr);
}

static void
rpc_vhost_delete_controller(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_delete_vhost_ctrlr req = {0};
	struct spdk_vhost_dev *vdev;
	int rc;

	if (spdk_json_decode_object(params, rpc_delete_vhost_ctrlr_decoder,
				    SPDK_COUNTOF(rpc_delete_vhost_ctrlr_decoder), &req)) {
		SPDK_DEBUGLOG(vhost_rpc, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	spdk_vhost_lock();
	vdev = spdk_vhost_dev_find(req.ctrlr);
	if (vdev == NULL) {
		spdk_vhost_unlock();
		rc = -ENODEV;
		goto invalid;
	}
	spdk_vhost_unlock();

	rc = spdk_vhost_dev_remove(vdev);
	if (rc < 0) {
		goto invalid;
	}

	free_rpc_delete_vhost_ctrlr(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	free_rpc_delete_vhost_ctrlr(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));

}
SPDK_RPC_REGISTER("vhost_delete_controller", rpc_vhost_delete_controller, SPDK_RPC_RUNTIME)

struct rpc_get_vhost_ctrlrs {
	char *name;
};

static void
_rpc_get_vhost_controller(struct spdk_json_write_ctx *w, struct spdk_vhost_dev *vdev)
{
	uint32_t delay_base_us, iops_threshold;

	spdk_vhost_get_coalescing(vdev, &delay_base_us, &iops_threshold);

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "ctrlr", spdk_vhost_dev_get_name(vdev));
	spdk_json_write_named_string_fmt(w, "cpumask", "0x%s",
					 spdk_cpuset_fmt(spdk_thread_get_cpumask(vdev->thread)));
	spdk_json_write_named_uint32(w, "delay_base_us", delay_base_us);
	spdk_json_write_named_uint32(w, "iops_threshold", iops_threshold);
	spdk_json_write_named_string(w, "socket", vdev->path);

	spdk_json_write_named_object_begin(w, "backend_specific");
	vhost_dump_info_json(vdev, w);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static const struct spdk_json_object_decoder rpc_get_vhost_ctrlrs_decoders[] = {
	{"name", offsetof(struct rpc_get_vhost_ctrlrs, name), spdk_json_decode_string, true},
};

static void
free_rpc_get_vhost_ctrlrs(struct rpc_get_vhost_ctrlrs *req)
{
	free(req->name);
}

static void
rpc_vhost_get_controllers(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_get_vhost_ctrlrs req = {0};
	struct spdk_json_write_ctx *w;
	struct spdk_vhost_dev *vdev;
	int rc;

	if (params && spdk_json_decode_object(params, rpc_get_vhost_ctrlrs_decoders,
					      SPDK_COUNTOF(rpc_get_vhost_ctrlrs_decoders), &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	spdk_vhost_lock();
	if (req.name != NULL) {
		vdev = spdk_vhost_dev_find(req.name);
		if (vdev == NULL) {
			spdk_vhost_unlock();
			rc = -ENODEV;
			goto invalid;
		}

		free_rpc_get_vhost_ctrlrs(&req);

		w = spdk_jsonrpc_begin_result(request);
		spdk_json_write_array_begin(w);

		_rpc_get_vhost_controller(w, vdev);
		spdk_vhost_unlock();

		spdk_json_write_array_end(w);
		spdk_jsonrpc_end_result(request, w);
		return;
	}

	free_rpc_get_vhost_ctrlrs(&req);

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	vdev = spdk_vhost_dev_next(NULL);
	while (vdev != NULL) {
		_rpc_get_vhost_controller(w, vdev);
		vdev = spdk_vhost_dev_next(vdev);
	}
	spdk_vhost_unlock();

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	free_rpc_get_vhost_ctrlrs(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("vhost_get_controllers", rpc_vhost_get_controllers, SPDK_RPC_RUNTIME)


struct rpc_vhost_ctrlr_coalescing {
	char *ctrlr;
	uint32_t delay_base_us;
	uint32_t iops_threshold;
};

static const struct spdk_json_object_decoder rpc_set_vhost_ctrlr_coalescing[] = {
	{"ctrlr", offsetof(struct rpc_vhost_ctrlr_coalescing, ctrlr), spdk_json_decode_string },
	{"delay_base_us", offsetof(struct rpc_vhost_ctrlr_coalescing, delay_base_us), spdk_json_decode_uint32},
	{"iops_threshold", offsetof(struct rpc_vhost_ctrlr_coalescing, iops_threshold), spdk_json_decode_uint32},
};

static void
free_rpc_set_vhost_controllers_event_coalescing(struct rpc_vhost_ctrlr_coalescing *req)
{
	free(req->ctrlr);
}

static void
rpc_vhost_controller_set_coalescing(struct spdk_jsonrpc_request *request,
				    const struct spdk_json_val *params)
{
	struct rpc_vhost_ctrlr_coalescing req = {0};
	struct spdk_vhost_dev *vdev;
	int rc;

	if (spdk_json_decode_object(params, rpc_set_vhost_ctrlr_coalescing,
				    SPDK_COUNTOF(rpc_set_vhost_ctrlr_coalescing), &req)) {
		SPDK_DEBUGLOG(vhost_rpc, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	spdk_vhost_lock();
	vdev = spdk_vhost_dev_find(req.ctrlr);
	if (vdev == NULL) {
		spdk_vhost_unlock();
		rc = -ENODEV;
		goto invalid;
	}

	rc = spdk_vhost_set_coalescing(vdev, req.delay_base_us, req.iops_threshold);
	spdk_vhost_unlock();
	if (rc) {
		goto invalid;
	}

	free_rpc_set_vhost_controllers_event_coalescing(&req);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	free_rpc_set_vhost_controllers_event_coalescing(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
					 spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("vhost_controller_set_coalescing", rpc_vhost_controller_set_coalescing,
		  SPDK_RPC_RUNTIME)

struct rpc_get_transport {
	char *name;
};

static const struct spdk_json_object_decoder rpc_get_transport_decoders[] = {
	{"name", offsetof(struct rpc_get_transport, name), spdk_json_decode_string, true},
};

static void
rpc_virtio_blk_get_transports(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_get_transport req = { 0 };
	struct spdk_json_write_ctx *w;
	struct spdk_virtio_blk_transport *transport = NULL;

	if (params) {
		if (spdk_json_decode_object(params, rpc_get_transport_decoders,
					    SPDK_COUNTOF(rpc_get_transport_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			return;
		}
	}

	if (req.name) {
		transport = virtio_blk_tgt_get_transport(req.name);
		if (transport == NULL) {
			SPDK_ERRLOG("transport '%s' does not exist\n", req.name);
			spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
			free(req.name);
			return;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);

	if (transport) {
		virtio_blk_transport_dump_opts(transport, w);
	} else {
		for (transport = virtio_blk_transport_get_first(); transport != NULL;
		     transport = virtio_blk_transport_get_next(transport)) {
			virtio_blk_transport_dump_opts(transport, w);
		}
	}

	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	free(req.name);
}
SPDK_RPC_REGISTER("virtio_blk_get_transports", rpc_virtio_blk_get_transports, SPDK_RPC_RUNTIME)

struct rpc_virtio_blk_create_transport {
	char *name;
};

static const struct spdk_json_object_decoder rpc_create_virtio_blk_transport[] = {
	{"name", offsetof(struct rpc_virtio_blk_create_transport, name), spdk_json_decode_string},
};

static void
free_rpc_virtio_blk_create_transport(struct rpc_virtio_blk_create_transport *req)
{
	free(req->name);
}

static void
rpc_virtio_blk_create_transport(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct rpc_virtio_blk_create_transport req = {0};
	int rc;

	if (spdk_json_decode_object_relaxed(params, rpc_create_virtio_blk_transport,
					    SPDK_COUNTOF(rpc_create_virtio_blk_transport), &req)) {
		SPDK_DEBUGLOG(vhost_rpc, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	spdk_vhost_lock();
	rc = virtio_blk_transport_create(req.name, params);
	spdk_vhost_unlock();
	if (rc != 0) {
		goto invalid;
	}

	free_rpc_virtio_blk_create_transport(&req);
	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	free_rpc_virtio_blk_create_transport(&req);
	spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("virtio_blk_create_transport", rpc_virtio_blk_create_transport,
		  SPDK_RPC_RUNTIME)

SPDK_LOG_REGISTER_COMPONENT(vhost_rpc)
