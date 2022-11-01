/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation. All rights reserved.
 *   Copyright (c) 2018 Mellanox Technologies LTD. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/rpc.h"
#include "spdk/util.h"

#include "vhost_fuzz.h"

struct rpc_fuzz_vhost_dev_create {
	char	*socket;
	bool	is_blk;
	bool	use_bogus_buffer;
	bool	use_valid_buffer;
	bool	valid_lun;
	bool	test_scsi_tmf;
};

static const struct spdk_json_object_decoder rpc_fuzz_vhost_dev_create_decoders[] = {
	{"socket", offsetof(struct rpc_fuzz_vhost_dev_create, socket), spdk_json_decode_string},
	{"is_blk", offsetof(struct rpc_fuzz_vhost_dev_create, is_blk), spdk_json_decode_bool, true},
	{"use_bogus_buffer", offsetof(struct rpc_fuzz_vhost_dev_create, use_bogus_buffer), spdk_json_decode_bool, true},
	{"use_valid_buffer", offsetof(struct rpc_fuzz_vhost_dev_create, use_valid_buffer), spdk_json_decode_bool, true},
	{"valid_lun", offsetof(struct rpc_fuzz_vhost_dev_create, valid_lun), spdk_json_decode_bool, true},
	{"test_scsi_tmf", offsetof(struct rpc_fuzz_vhost_dev_create, test_scsi_tmf), spdk_json_decode_bool, true},
};

static void
spdk_rpc_fuzz_vhost_create_dev(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_fuzz_vhost_dev_create req = {0};
	int rc;

	if (spdk_json_decode_object(params, rpc_fuzz_vhost_dev_create_decoders,
				    SPDK_COUNTOF(rpc_fuzz_vhost_dev_create_decoders), &req)) {
		fprintf(stderr, "Unable to parse the request.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Unable to parse the object parameters.\n");
		return;
	}

	if (strlen(req.socket) > PATH_MAX) {
		fprintf(stderr, "Socket address is too long.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Unable to parse the object parameters.\n");
		free(req.socket);
		return;
	}

	rc = fuzz_vhost_dev_init(req.socket, req.is_blk, req.use_bogus_buffer, req.use_valid_buffer,
				 req.valid_lun, req.test_scsi_tmf);

	if (rc != 0) {
		if (rc == -ENOMEM) {
			fprintf(stderr, "No valid memory for device initialization.\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "No memory returned from host.\n");
		} else if (rc == -EINVAL) {
			fprintf(stderr, "Invalid device parameters provided.\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Parameters provided were invalid.\n");
		} else {
			fprintf(stderr, "unknown error from the guest.\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Unexpected error code.\n");
		}
	} else {
		spdk_jsonrpc_send_bool_response(request, true);
	}

	free(req.socket);
	return;
}
SPDK_RPC_REGISTER("fuzz_vhost_create_dev", spdk_rpc_fuzz_vhost_create_dev, SPDK_RPC_STARTUP);
