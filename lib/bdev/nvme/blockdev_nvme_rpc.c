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

#include "blockdev_nvme.h"
#include "spdk/rpc.h"

#include "spdk_internal/log.h"

struct rpc_construct_nvme {
	char *pci_address;
};

static void
free_rpc_construct_nvme(struct rpc_construct_nvme *req)
{
	free(req->pci_address);
}

static const struct spdk_json_object_decoder rpc_construct_nvme_decoders[] = {
	{"pci_address", offsetof(struct rpc_construct_nvme, pci_address), spdk_json_decode_string},
};

static void
spdk_rpc_construct_nvme_bdev(struct spdk_jsonrpc_server_conn *conn,
			     const struct spdk_json_val *params,
			     const struct spdk_json_val *id)
{
	struct rpc_construct_nvme req = {};
	struct spdk_json_write_ctx *w;
	struct nvme_probe_ctx ctx = {};
	int i;

	if (spdk_json_decode_object(params, rpc_construct_nvme_decoders,
				    sizeof(rpc_construct_nvme_decoders) / sizeof(*rpc_construct_nvme_decoders),
				    &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	ctx.controllers_remaining = 1;
	ctx.num_whitelist_controllers = 1;

	if (spdk_pci_addr_parse(&ctx.whitelist[0], req.pci_address) < 0) {
		goto invalid;
	}

	if (spdk_bdev_nvme_create(&ctx)) {
		goto invalid;
	}

	free_rpc_construct_nvme(&req);

	if (id == NULL) {
		return;
	}

	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_array_begin(w);
	for (i = 0; i < ctx.num_created_bdevs; i++) {
		spdk_json_write_string(w, ctx.created_bdevs[i]->name);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(conn, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_construct_nvme(&req);
}
SPDK_RPC_REGISTER("construct_nvme_bdev", spdk_rpc_construct_nvme_bdev)
