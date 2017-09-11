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

#include "spdk/stdinc.h"

#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk_internal/log.h"

static const struct spdk_json_object_decoder rpc_construct_virtio_controller[] = {

};

struct rpc_virtio_ctrlr {
//	char *ctrlr;
//	char *cpumask;
};

static void
spdk_rpc_construct_virtio_bdev(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_virtio_ctrlr req = {0};
	struct spdk_json_write_ctx *w;
	int rc;
	char buf[64];

	if (spdk_json_decode_object(params, rpc_construct_virtio_controller,
				    SPDK_COUNTOF(rpc_construct_virtio_controller),
				    &req)) {
		SPDK_DEBUGLOG(SPDK_TRACE_VHOST_RPC, "spdk_json_decode_object failed\n");
		rc = -EINVAL;
		goto invalid;
	}

	invalid:
	//TODO: spdk_strrerr_t - is this necessary?
		spdk_strerror_r(-rc, buf, sizeof(buf));
		free_rpc_vhost_scsi_ctrlr(&req);
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, buf);

}
SPDK_RPC_REGISTER("construct_virtio_bdev", spdk_rpc_construct_virtio_bdev);
