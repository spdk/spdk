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

#include "spdk/log.h"
#include "spdk/jsonrpc_util.h"

#include "spdk_internal/bdev.h"

static const struct spdk_jsonrpc_params get_bdevs_params[] = {
	{"name", SPDK_RPC_PARAM_STRING, true},
	{NULL}
};

static void
spdk_rpc_dump_bdev_info(struct spdk_jsonrpc_util_req *req, struct spdk_bdev *bdev)
{
	spdk_jsonrpc_object_begin(req);

	spdk_jsonrpc_string_create(req, "name", "%s", spdk_bdev_get_name(bdev));
	spdk_jsonrpc_string_create(req, "product_name", "%s" , spdk_bdev_get_product_name(bdev));
	spdk_jsonrpc_uint_create(req, "block_size", spdk_bdev_get_block_size(bdev));
	spdk_jsonrpc_uint_create(req, "num_blocks", spdk_bdev_get_num_blocks(bdev));

	spdk_jsonrpc_bool_create(req, "claimed", bdev->claim_module != NULL);

	spdk_jsonrpc_object_create(req, "supported_io_types");
	spdk_jsonrpc_bool_create(req, "read", spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_READ));
	spdk_jsonrpc_bool_create(req, "write", spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE));
	spdk_jsonrpc_bool_create(req, "unmap", spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP));
	spdk_jsonrpc_bool_create(req, "write_zeroes", spdk_bdev_io_type_supported(bdev,
				 SPDK_BDEV_IO_TYPE_WRITE_ZEROES));
	spdk_jsonrpc_bool_create(req, "flush", spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_FLUSH));
	spdk_jsonrpc_bool_create(req, "reset", spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_RESET));
	spdk_jsonrpc_bool_create(req, "nvme_admin", spdk_bdev_io_type_supported(bdev,
				 SPDK_BDEV_IO_TYPE_NVME_ADMIN));
	spdk_jsonrpc_bool_create(req, "nvme_io", spdk_bdev_io_type_supported(bdev,
				 SPDK_BDEV_IO_TYPE_NVME_IO));
	spdk_jsonrpc_object_end(req);

	spdk_jsonrpc_object_create(req, "driver_specific");
	spdk_bdev_dump_config_json(bdev, spdk_jsonrpc_response_ctx(req));
	spdk_jsonrpc_object_end(req);

	spdk_jsonrpc_object_end(req);
}

static void
get_bdevs(struct spdk_jsonrpc_util_req *req)
{
	const char *name = spdk_jsonrpc_param_str(req, "name", NULL);
	struct spdk_bdev *bdev = name ? spdk_bdev_get_by_name(name) : NULL;

	if (name != NULL && bdev == NULL) {
		SPDK_ERRLOG("bdev '%s' does not exist\n", name);
		spdk_jsonrpc_send_response(req, false, "bdev '%s' does not exist", name);
		return;
	}

	if (!spdk_jsonrpc_response_ctx(req)) {
		return;
	}

	spdk_jsonrpc_array_begin(req);

	if (bdev != NULL) {
		spdk_rpc_dump_bdev_info(req, bdev);
	} else {
		for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {
			spdk_rpc_dump_bdev_info(req, bdev);
		}
	}

	spdk_jsonrpc_array_end(req);
	spdk_jsonrpc_end_response(req);
}
SPDK_JSONRPC_CMD(get_bdevs, get_bdevs_params)

static const struct spdk_jsonrpc_params delete_bdev_params[] = {
	{"name", SPDK_RPC_PARAM_STRING, true},
	{NULL}
};

static void
delete_bdev_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_util_req *req = cb_arg;

	spdk_jsonrpc_send_errno_response(req, bdeverrno);
}

static void
delete_bdev(struct spdk_jsonrpc_util_req *req)
{
	struct spdk_bdev *bdev;
	const char *name = spdk_jsonrpc_param_str(req, "name", "");

	bdev = spdk_bdev_get_by_name(name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev '%s' does not exist\n", name);
		spdk_jsonrpc_send_response(req, false, "bdev '%s' does not exist", name);
		return;
	}

	spdk_bdev_unregister(bdev, delete_bdev_cb, req);
}
SPDK_JSONRPC_CMD(delete_bdev, delete_bdev_params)
