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

#include "spdk/bdev.h"
#include "spdk/log.h"
#include "spdk/rpc.h"

static void
spdk_rpc_get_bdevs(struct spdk_jsonrpc_server_conn *conn,
		   const struct spdk_json_val *params,
		   const struct spdk_json_val *id)
{
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_bdevs requires no parameters");
		return;
	}

	if (id == NULL) {
		return;
	}

	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_array_begin(w);

	for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {
		spdk_json_write_object_begin(w);

		spdk_json_write_name(w, "name");
		spdk_json_write_string(w, bdev->name);

		spdk_json_write_name(w, "product_name");
		spdk_json_write_string(w, bdev->product_name);

		spdk_json_write_name(w, "block_size");
		spdk_json_write_uint32(w, bdev->blocklen);

		spdk_json_write_name(w, "num_blocks");
		spdk_json_write_uint64(w, bdev->blockcnt);

		spdk_json_write_name(w, "claimed");
		spdk_json_write_bool(w, bdev->claimed);

		spdk_json_write_name(w, "driver_specific");
		spdk_json_write_object_begin(w);
		spdk_bdev_dump_config_json(bdev, w);
		spdk_json_write_object_end(w);

		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(conn, w);
}
SPDK_RPC_REGISTER("get_bdevs", spdk_rpc_get_bdevs)
