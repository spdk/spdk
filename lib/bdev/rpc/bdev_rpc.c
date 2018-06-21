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
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk/bdev_module.h"

struct rpc_get_bdevs_iostat_ctx {
	int bdev_count;
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;
};

static void
spdk_rpc_get_bdevs_iostat_cb(struct spdk_bdev *bdev,
			     struct spdk_bdev_io_stat *stat, void *cb_arg, int rc)
{
	struct rpc_get_bdevs_iostat_ctx *ctx = cb_arg;
	struct spdk_json_write_ctx *w = ctx->w;
	const char *bdev_name;

	if (rc != 0) {
		goto done;
	}

	bdev_name = spdk_bdev_get_name(bdev);
	if (bdev_name != NULL) {
		spdk_json_write_object_begin(w);

		spdk_json_write_name(w, "name");
		spdk_json_write_string(w, bdev_name);

		spdk_json_write_name(w, "bytes_read");
		spdk_json_write_uint64(w, stat->bytes_read);

		spdk_json_write_name(w, "num_read_ops");
		spdk_json_write_uint64(w, stat->num_read_ops);

		spdk_json_write_name(w, "bytes_written");
		spdk_json_write_uint64(w, stat->bytes_written);

		spdk_json_write_name(w, "num_write_ops");
		spdk_json_write_uint64(w, stat->num_write_ops);

		spdk_json_write_object_end(w);
	}

done:
	free(stat);
	if (--ctx->bdev_count == 0) {
		spdk_json_write_array_end(ctx->w);
		spdk_jsonrpc_end_result(ctx->request, ctx->w);
		free(ctx);
	}
}

struct rpc_get_bdevs_iostat {
	char *name;
};

static void
free_rpc_get_bdevs_iostat(struct rpc_get_bdevs_iostat *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_get_bdevs_iostat_decoders[] = {
	{"name", offsetof(struct rpc_get_bdevs_iostat, name), spdk_json_decode_string, true},
};

static void
spdk_rpc_get_bdevs_iostat(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_get_bdevs_iostat req = {};
	struct spdk_bdev *bdev = NULL;
	struct spdk_json_write_ctx *w;
	struct spdk_bdev_io_stat *stat;
	struct rpc_get_bdevs_iostat_ctx *ctx;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_get_bdevs_iostat_decoders,
					    SPDK_COUNTOF(rpc_get_bdevs_iostat_decoders),
					    &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			goto invalid;
		}

		if (req.name) {
			bdev = spdk_bdev_get_by_name(req.name);
			if (bdev == NULL) {
				SPDK_ERRLOG("bdev '%s' does not exist\n", req.name);
				goto invalid;
			}
		}
	}

	free_rpc_get_bdevs_iostat(&req);

	ctx = calloc(1, sizeof(struct rpc_get_bdevs_iostat_ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate rpc_get_bdevs_iostat_ctx struct\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "No memory left");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		free(ctx);
		return;
	}

	/*
	 * Increment initial bdev_count so that it will never reach 0 in the middle
	 * of iterating.
	 */
	ctx->bdev_count++;
	ctx->request = request;
	ctx->w = w;

	spdk_json_write_array_begin(w);

	if (bdev != NULL) {
		stat = calloc(1, sizeof(struct spdk_bdev_io_stat));
		if (stat == NULL) {
			SPDK_ERRLOG("Failed to allocate rpc_get_bdevs_iostat_ctx struct\n");
		} else {
			ctx->bdev_count++;
			spdk_bdev_get_device_stat(bdev, stat, spdk_rpc_get_bdevs_iostat_cb, ctx);
		}
	} else {
		for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {
			stat = calloc(1, sizeof(struct spdk_bdev_io_stat));
			if (stat == NULL) {
				SPDK_ERRLOG("Failed to allocate spdk_bdev_io_stat struct\n");
				break;
			}
			ctx->bdev_count++;
			spdk_bdev_get_device_stat(bdev, stat, spdk_rpc_get_bdevs_iostat_cb, ctx);
		}
	}

	if (--ctx->bdev_count == 0) {
		spdk_json_write_array_end(w);
		spdk_jsonrpc_end_result(request, w);
		free(ctx);
	}

	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");

	free_rpc_get_bdevs_iostat(&req);
}
SPDK_RPC_REGISTER("get_bdevs_iostat", spdk_rpc_get_bdevs_iostat, SPDK_RPC_RUNTIME)

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

	if (!spdk_mem_all_zero(&bdev->uuid, sizeof(bdev->uuid))) {
		char uuid_str[SPDK_UUID_STRING_LEN];

		spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &bdev->uuid);
		spdk_json_write_named_string(w, "uuid", uuid_str);
	}

	spdk_json_write_name(w, "qos_ios_per_sec");
	spdk_json_write_uint64(w, spdk_bdev_get_qos_ios_per_sec(bdev));

	spdk_json_write_name(w, "claimed");
	spdk_json_write_bool(w, (bdev->internal.claim_module != NULL));

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
	spdk_bdev_dump_info_json(bdev, w);
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
	{"name", offsetof(struct rpc_get_bdevs, name), spdk_json_decode_string, true},
};

static void
spdk_rpc_get_bdevs(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_get_bdevs req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev = NULL;

	if (params && spdk_json_decode_object(params, rpc_get_bdevs_decoders,
					      SPDK_COUNTOF(rpc_get_bdevs_decoders),
					      &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.name) {
		bdev = spdk_bdev_get_by_name(req.name);
		if (bdev == NULL) {
			SPDK_ERRLOG("bdev '%s' does not exist\n", req.name);
			goto invalid;
		}
	}

	free_rpc_get_bdevs(&req);
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
SPDK_RPC_REGISTER("get_bdevs", spdk_rpc_get_bdevs, SPDK_RPC_RUNTIME)

struct rpc_get_bdevs_config {
	char *name;
};

static void
free_rpc_get_bdevs_config(struct rpc_get_bdevs_config *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_dump_bdevs_config_decoders[] = {
	{"name", offsetof(struct rpc_get_bdevs_config, name), spdk_json_decode_string, true},
};

static void
spdk_rpc_get_bdevs_config(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_get_bdevs_config req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev = NULL;

	if (params && spdk_json_decode_object(params, rpc_dump_bdevs_config_decoders,
					      SPDK_COUNTOF(rpc_dump_bdevs_config_decoders),
					      &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
		return;
	}

	if (req.name) {
		bdev = spdk_bdev_get_by_name(req.name);
		if (bdev == NULL) {
			SPDK_ERRLOG("bdev '%s' does not exist\n", req.name);
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							     "Bdev '%s' not exist", req.name);
			free_rpc_get_bdevs_config(&req);
			return;
		}
	}

	free_rpc_get_bdevs_config(&req);
	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);
	if (bdev != NULL) {
		spdk_bdev_config_json(bdev, w);
	} else {
		for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {
			spdk_bdev_config_json(bdev, w);
		}
	}

	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_bdevs_config", spdk_rpc_get_bdevs_config, SPDK_RPC_RUNTIME)

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
SPDK_RPC_REGISTER("delete_bdev", spdk_rpc_delete_bdev, SPDK_RPC_RUNTIME)

struct rpc_set_bdev_qos_limit_iops {
	char		*name;
	uint64_t	ios_per_sec;
};

static void
free_rpc_set_bdev_qos_limit_iops(struct rpc_set_bdev_qos_limit_iops *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_set_bdev_qos_limit_iops_decoders[] = {
	{"name", offsetof(struct rpc_set_bdev_qos_limit_iops, name), spdk_json_decode_string},
	{"ios_per_sec", offsetof(struct rpc_set_bdev_qos_limit_iops, ios_per_sec), spdk_json_decode_uint64},
};

static void
spdk_rpc_set_bdev_qos_limit_iops_complete(void *cb_arg, int status)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w;

	if (status != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Failed to configure IOPS limit: %s",
						     spdk_strerror(-status));
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
}

static void
spdk_rpc_set_bdev_qos_limit_iops(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params)
{
	struct rpc_set_bdev_qos_limit_iops req = {};
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_set_bdev_qos_limit_iops_decoders,
				    SPDK_COUNTOF(rpc_set_bdev_qos_limit_iops_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		goto invalid;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev '%s' does not exist\n", req.name);
		goto invalid;
	}

	free_rpc_set_bdev_qos_limit_iops(&req);
	spdk_bdev_set_qos_limit_iops(bdev, req.ios_per_sec,
				     spdk_rpc_set_bdev_qos_limit_iops_complete, request);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_set_bdev_qos_limit_iops(&req);
}

SPDK_RPC_REGISTER("set_bdev_qos_limit_iops", spdk_rpc_set_bdev_qos_limit_iops, SPDK_RPC_RUNTIME)
