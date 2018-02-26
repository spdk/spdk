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
#include "spdk/rpc.h"
#include "spdk/util.h"

#include "spdk_internal/bdev.h"

static void
spdk_rpc_dump_bdev_info(struct spdk_json_write_ctx *w,
			struct spdk_bdev *bdev)
{
	struct spdk_bdev_alias *tmp;

	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "name");
	spdk_json_write_string(w, spdk_bdev_get_name(bdev));

	spdk_json_write_name(w, "aliases");
	spdk_json_write_array_begin(w);

	TAILQ_FOREACH(tmp, spdk_bdev_get_aliases(bdev), tailq) {
		spdk_json_write_string(w, tmp->alias);
	}

	spdk_json_write_array_end(w);

	spdk_json_write_name(w, "product_name");
	spdk_json_write_string(w, spdk_bdev_get_product_name(bdev));

	spdk_json_write_name(w, "block_size");
	spdk_json_write_uint32(w, spdk_bdev_get_block_size(bdev));

	spdk_json_write_name(w, "num_blocks");
	spdk_json_write_uint64(w, spdk_bdev_get_num_blocks(bdev));

	spdk_json_write_name(w, "claimed");
	spdk_json_write_bool(w, (bdev->claim_module != NULL));

	spdk_json_write_name(w, "supported_io_types");
	spdk_json_write_object_begin(w);
	spdk_json_write_name(w, "read");
	spdk_json_write_bool(w, spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_READ));
	spdk_json_write_name(w, "write");
	spdk_json_write_bool(w, spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE));
	spdk_json_write_name(w, "unmap");
	spdk_json_write_bool(w, spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP));
	spdk_json_write_name(w, "write_zeroes");
	spdk_json_write_bool(w, spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES));
	spdk_json_write_name(w, "flush");
	spdk_json_write_bool(w, spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_FLUSH));
	spdk_json_write_name(w, "reset");
	spdk_json_write_bool(w, spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_RESET));
	spdk_json_write_name(w, "nvme_admin");
	spdk_json_write_bool(w, spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_ADMIN));
	spdk_json_write_name(w, "nvme_io");
	spdk_json_write_bool(w, spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_NVME_IO));
	spdk_json_write_object_end(w);

	spdk_json_write_name(w, "driver_specific");
	spdk_json_write_object_begin(w);
	spdk_bdev_dump_config_json(bdev, w);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

struct rpc_get_bdevs {
	char *name;
};

static void
free_rpc_get_bdevs(struct rpc_get_bdevs *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_get_bdevs_decoders[] = {
	{"name", offsetof(struct rpc_get_bdevs, name), spdk_json_decode_string},
};

static void
spdk_rpc_get_bdevs(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_get_bdevs req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev = NULL;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_get_bdevs_decoders,
					    SPDK_COUNTOF(rpc_get_bdevs_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			goto invalid;
		} else {
			if (req.name == NULL) {
				SPDK_ERRLOG("missing name param\n");
				goto invalid;
			}

			bdev = spdk_bdev_get_by_name(req.name);
			if (bdev == NULL) {
				SPDK_ERRLOG("bdev '%s' does not exist\n", req.name);
				goto invalid;
			}

			free_rpc_get_bdevs(&req);
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);

	if (bdev != NULL) {
		spdk_rpc_dump_bdev_info(w, bdev);
	} else {
		for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {
			spdk_rpc_dump_bdev_info(w, bdev);
		}
	}

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");

	free_rpc_get_bdevs(&req);
}
SPDK_RPC_REGISTER("get_bdevs", spdk_rpc_get_bdevs)

static void
spdk_rpc_dump_bdev_dependency_info(struct spdk_json_write_ctx *w,
				   struct spdk_bdev *bdev)
{
	struct spdk_bdev *vbdev;

	spdk_json_write_object_begin(w);

	spdk_json_write_name(w, "name");
	spdk_json_write_string(w, spdk_bdev_get_name(bdev));

	spdk_json_write_name(w, "dependent");

	spdk_json_write_array_begin(w);

	for (vbdev = spdk_vbdev_on_which_depends_first(bdev);
	     vbdev != NULL;
	     vbdev = spdk_vbdev_on_which_depends_next(bdev, vbdev)) {
		spdk_json_write_string(w, spdk_bdev_get_name(vbdev));
	}

	spdk_json_write_array_end(w);

	spdk_json_write_object_end(w);
}

static void
spdk_rpc_get_bdev_dependencies(struct spdk_jsonrpc_request *request,
			       const struct spdk_json_val *params)
{
	struct rpc_get_bdevs req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev = NULL;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_get_bdevs_decoders,
					    SPDK_COUNTOF(rpc_get_bdevs_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			goto invalid;
		} else {
			if (req.name == NULL) {
				SPDK_ERRLOG("missing name param\n");
				goto invalid;
			}

			bdev = spdk_bdev_get_by_name(req.name);
			if (bdev == NULL) {
				SPDK_ERRLOG("bdev '%s' does not exist\n", req.name);
				goto invalid;
			}

			free_rpc_get_bdevs(&req);
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);

	if (bdev != NULL) {
		spdk_rpc_dump_bdev_dependency_info(w, bdev);
	} else {
		for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {
			spdk_rpc_dump_bdev_dependency_info(w, bdev);
		}
	}

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");

	free_rpc_get_bdevs(&req);
}
SPDK_RPC_REGISTER("get_bdev_dependencies", spdk_rpc_get_bdev_dependencies)

struct rpc_delete_bdev {
	char *name;
};

static void
free_rpc_delete_bdev(struct rpc_delete_bdev *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_delete_bdev_decoders[] = {
	{"name", offsetof(struct rpc_delete_bdev, name), spdk_json_decode_string},
};

static void
_spdk_rpc_delete_bdev_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w;

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, bdeverrno == 0);
	spdk_jsonrpc_end_result(request, w);
}

static void
spdk_rpc_delete_bdev(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_delete_bdev req = {};
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_delete_bdev_decoders,
				    SPDK_COUNTOF(rpc_delete_bdev_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.name == NULL) {
		SPDK_ERRLOG("missing name param\n");
		goto invalid;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev '%s' does not exist\n", req.name);
		goto invalid;
	}

	spdk_bdev_unregister(bdev, _spdk_rpc_delete_bdev_cb, request);

	free_rpc_delete_bdev(&req);

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_delete_bdev(&req);
}
SPDK_RPC_REGISTER("delete_bdev", spdk_rpc_delete_bdev)
