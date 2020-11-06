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

#include "bdev_pmem.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "libpmemblk.h"

#include "spdk/log.h"

struct rpc_construct_pmem {
	char *pmem_file;
	char *name;
};

static void
free_rpc_bdev_pmem_create(struct rpc_construct_pmem *req)
{
	free(req->pmem_file);
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_construct_pmem_decoders[] = {
	{"pmem_file", offsetof(struct rpc_construct_pmem, pmem_file), spdk_json_decode_string},
	{"name", offsetof(struct rpc_construct_pmem, name), spdk_json_decode_string},
};

static void
rpc_bdev_pmem_create(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_construct_pmem req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_pmem_decoders,
				    SPDK_COUNTOF(rpc_construct_pmem_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_pmem, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}
	rc = create_pmem_disk(req.pmem_file, req.name, &bdev);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_pmem_create(&req);
}
SPDK_RPC_REGISTER("bdev_pmem_create", rpc_bdev_pmem_create, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_pmem_create, construct_pmem_bdev)

struct rpc_delete_pmem {
	char *name;
};

static void
free_rpc_delete_pmem(struct rpc_delete_pmem *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_delete_pmem_decoders[] = {
	{"name", offsetof(struct rpc_delete_pmem, name), spdk_json_decode_string},
};

static void
_rpc_bdev_pmem_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	spdk_jsonrpc_send_bool_response(request, bdeverrno == 0);
}

static void
rpc_bdev_pmem_delete(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	struct rpc_delete_pmem req = {NULL};
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_delete_pmem_decoders,
				    SPDK_COUNTOF(rpc_delete_pmem_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_pmem, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		SPDK_DEBUGLOG(bdev_pmem, "bdev '%s' does not exist\n", req.name);
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	delete_pmem_disk(bdev, _rpc_bdev_pmem_delete_cb, request);

cleanup:
	free_rpc_delete_pmem(&req);
}
SPDK_RPC_REGISTER("bdev_pmem_delete", rpc_bdev_pmem_delete, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_pmem_delete, delete_pmem_bdev)

struct rpc_bdev_pmem_create_pool {
	char *pmem_file;
	uint64_t num_blocks;
	uint32_t block_size;
};

static const struct spdk_json_object_decoder rpc_bdev_pmem_create_pool_decoders[] = {
	{"pmem_file", offsetof(struct rpc_bdev_pmem_create_pool, pmem_file), spdk_json_decode_string},
	{"num_blocks", offsetof(struct rpc_bdev_pmem_create_pool, num_blocks), spdk_json_decode_uint64},
	{"block_size", offsetof(struct rpc_bdev_pmem_create_pool, block_size), spdk_json_decode_uint32},
};

static void
free_rpc_bdev_pmem_create_pool(struct rpc_bdev_pmem_create_pool *req)
{
	free(req->pmem_file);
}

static void
rpc_bdev_pmem_create_pool(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_bdev_pmem_create_pool req = {};
	uint64_t pool_size;
	PMEMblkpool *pbp;

	if (spdk_json_decode_object(params, rpc_bdev_pmem_create_pool_decoders,
				    SPDK_COUNTOF(rpc_bdev_pmem_create_pool_decoders),
				    &req)) {
		SPDK_DEBUGLOG(bdev_pmem, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* libpmemblk pool has to contain at least 256 blocks */
	if (req.num_blocks < 256) {
		spdk_jsonrpc_send_error_response(request, -EINVAL,
						 "Pmem pool num_blocks must be at least 256");
		goto cleanup;
	}

	pool_size = req.num_blocks * req.block_size;
	if (pool_size < PMEMBLK_MIN_POOL) {
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
						     "Pmem pool size must be at least %ld", PMEMBLK_MIN_POOL);
		goto cleanup;
	}

	pbp = pmemblk_create(req.pmem_file, req.block_size, pool_size, 0666);
	if (pbp == NULL) {
		const char *msg = pmemblk_errormsg();

		SPDK_DEBUGLOG(bdev_pmem, "pmemblk_create() failed: %s\n", msg ? msg : "(logs disabled)");
		spdk_jsonrpc_send_error_response_fmt(request, -SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "pmemblk_create failed: %s", msg ? msg : "(logs disabled)");
		goto cleanup;
	}

	pmemblk_close(pbp);

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_pmem_create_pool(&req);
}
SPDK_RPC_REGISTER("bdev_pmem_create_pool", rpc_bdev_pmem_create_pool, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_pmem_create_pool, create_pmem_pool)

struct rpc_bdev_pmem_get_pool_info {
	char *pmem_file;
};

static const struct spdk_json_object_decoder rpc_bdev_pmem_get_pool_info_decoders[] = {
	{"pmem_file", offsetof(struct rpc_bdev_pmem_get_pool_info, pmem_file), spdk_json_decode_string},
};

static void
free_rpc_bdev_pmem_get_pool_info(struct rpc_bdev_pmem_get_pool_info *req)
{
	free(req->pmem_file);
}

static void
rpc_bdev_pmem_get_pool_info(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_bdev_pmem_get_pool_info req = {};
	struct spdk_json_write_ctx *w;
	size_t num_blocks, block_size;
	PMEMblkpool *pbp;

	if (spdk_json_decode_object(params, rpc_bdev_pmem_get_pool_info_decoders,
				    SPDK_COUNTOF(rpc_bdev_pmem_get_pool_info_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	pbp = pmemblk_open(req.pmem_file, 0);
	if (pbp == NULL) {
		const char *msg = pmemblk_errormsg();

		spdk_jsonrpc_send_error_response_fmt(request, -SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "pmemblk_open failed: %s", msg ? msg : "(logs disabled)");
		goto cleanup;
	}

	block_size = pmemblk_bsize(pbp);
	num_blocks = pmemblk_nblock(pbp);

	pmemblk_close(pbp);

	/* Check pmem pool consistency */
	if (pmemblk_check(req.pmem_file, block_size) != 1) {
		const char *msg = pmemblk_errormsg();

		spdk_jsonrpc_send_error_response_fmt(request, -SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "pmemblk_check failed: %s", msg ? msg : "(logs disabled)");
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint64(w, "num_blocks", num_blocks);
	spdk_json_write_named_uint64(w, "block_size", block_size);
	spdk_json_write_object_end(w);
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_pmem_get_pool_info(&req);
}
SPDK_RPC_REGISTER("bdev_pmem_get_pool_info", rpc_bdev_pmem_get_pool_info, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_pmem_get_pool_info, pmem_pool_info)

struct rpc_bdev_pmem_delete_pool {
	char *pmem_file;
};

static const struct spdk_json_object_decoder rpc_bdev_pmem_delete_pool_decoders[] = {
	{"pmem_file", offsetof(struct rpc_bdev_pmem_delete_pool, pmem_file), spdk_json_decode_string},
};

static void
free_rpc_bdev_pmem_delete_pool(struct rpc_bdev_pmem_delete_pool *req)
{
	free(req->pmem_file);
}

static void
rpc_bdev_pmem_delete_pool(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_bdev_pmem_delete_pool req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_pmem_delete_pool_decoders,
				    SPDK_COUNTOF(rpc_bdev_pmem_delete_pool_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	/* Check if file is actually pmem pool */
	rc = pmemblk_check(req.pmem_file, 0);
	if (rc != 1) {
		const char *msg = pmemblk_errormsg();

		spdk_jsonrpc_send_error_response_fmt(request, -SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "pmemblk_check failed: %s", msg ? msg : "(logs disabled)");
		goto cleanup;
	}

	unlink(req.pmem_file);

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_pmem_delete_pool(&req);
}
SPDK_RPC_REGISTER("bdev_pmem_delete_pool", rpc_bdev_pmem_delete_pool, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_pmem_delete_pool, delete_pmem_pool)
