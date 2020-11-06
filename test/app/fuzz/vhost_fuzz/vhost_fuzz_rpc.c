/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2018 Mellanox Technologies LTD. All rights reserved.
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
