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

#include <linux/nbd.h>

#include "spdk/nbd.h"
#include "spdk_internal/log.h"

extern struct spdk_nbd_disk *g_nbd_disk;
extern char *g_nbd_name;

struct rpc_stop_nbd_disk {
	char *nbd_device;
};

static void
free_rpc_stop_nbd_disk(struct rpc_stop_nbd_disk *req)
{
	free(req->nbd_device);
}

static const struct spdk_json_object_decoder rpc_stop_nbd_disk_decoders[] = {
	{"nbd-device", offsetof(struct rpc_stop_nbd_disk, nbd_device), spdk_json_decode_string, true},
	{"nbd_device", offsetof(struct rpc_stop_nbd_disk, nbd_device), spdk_json_decode_string, true},
};

static void
spdk_rpc_stop_nbd_disk(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_stop_nbd_disk req = {};
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_stop_nbd_disk_decoders,
				    SPDK_COUNTOF(rpc_stop_nbd_disk_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.nbd_device == NULL || strcmp(g_nbd_name, req.nbd_device)) {
		goto invalid;
	}

	/*
	 * nbd soft-disconnection to terminate transmission phase.
	 * After receiving this ioctl command, nbd kernel module will send
	 * a NBD_CMD_DISC type io to nbd server in order to inform server.
	 */
	ioctl(g_nbd_disk->dev_fd, NBD_DISCONNECT);

	free_rpc_stop_nbd_disk(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
}

SPDK_RPC_REGISTER("stop_nbd_disk", spdk_rpc_stop_nbd_disk)
