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

#include "spdk_internal/log.h"

struct rpc_construct_pmem {
	char *pmem_file;
	char *name;
};

static void
free_rpc_construct_pmem_bdev(struct rpc_construct_pmem *req)
{
	free(req->pmem_file);
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_construct_pmem_decoders[] = {
	{"pmem_file", offsetof(struct rpc_construct_pmem, pmem_file), spdk_json_decode_string},
	{"name", offsetof(struct rpc_construct_pmem, name), spdk_json_decode_string},
};

static void
spdk_rpc_construct_pmem_bdev(struct spdk_jsonrpc_request *request,
			     const struct spdk_json_val *params)
{
	struct rpc_construct_pmem req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_pmem_decoders,
				    SPDK_COUNTOF(rpc_construct_pmem_decoders),
				    &req)) {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_PMEM, "spdk_json_decode_object failed\n");
		rc = EINVAL;
		goto invalid;
	}
	rc = spdk_create_pmem_disk(req.pmem_file, req.name, &bdev);
	if (rc != 0) {
		goto invalid;
	}
	if (bdev == NULL) {
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	spdk_jsonrpc_end_result(request, w);

	free_rpc_construct_pmem_bdev(&req);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(rc));
	free_rpc_construct_pmem_bdev(&req);
}
SPDK_RPC_REGISTER("construct_pmem_bdev", spdk_rpc_construct_pmem_bdev, SPDK_RPC_RUNTIME)

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
_spdk_rpc_delete_pmem_bdev_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);

	spdk_json_write_bool(w, bdeverrno == 0);
	spdk_jsonrpc_end_result(request, w);
}

static void
spdk_rpc_delete_pmem_bdev(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_delete_pmem req = {NULL};
	struct spdk_bdev *bdev;
	int rc;

	if (spdk_json_decode_object(params, rpc_delete_pmem_decoders,
				    SPDK_COUNTOF(rpc_delete_pmem_decoders),
				    &req)) {
		rc = -EINVAL;
		goto invalid;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		rc = -ENODEV;
		goto invalid;
	}

	spdk_delete_pmem_disk(bdev, _spdk_rpc_delete_pmem_bdev_cb, request);
	free_rpc_delete_pmem(&req);
	return;

invalid:
	free_rpc_delete_pmem(&req);
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, spdk_strerror(-rc));
}
SPDK_RPC_REGISTER("delete_pmem_bdev", spdk_rpc_delete_pmem_bdev, SPDK_RPC_RUNTIME)

struct rpc_create_pmem_pool {
	char *pmem_file;
	uint64_t num_blocks;
	uint32_t block_size;
};

static const struct spdk_json_object_decoder rpc_create_pmem_pool_decoders[] = {
	{"pmem_file", offsetof(struct rpc_create_pmem_pool, pmem_file), spdk_json_decode_string},
	{"num_blocks", offsetof(struct rpc_create_pmem_pool, num_blocks), spdk_json_decode_uint64},
	{"block_size", offsetof(struct rpc_create_pmem_pool, block_size), spdk_json_decode_uint32},
};

static void
free_rpc_create_pmem_pool(struct rpc_create_pmem_pool *req)
{
	free(req->pmem_file);
}

static void
spdk_rpc_create_pmem_pool(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_create_pmem_pool req = {};
	struct spdk_json_write_ctx *w;
	uint64_t pool_size;
	PMEMblkpool *pbp;

	if (spdk_json_decode_object(params, rpc_create_pmem_pool_decoders,
				    SPDK_COUNTOF(rpc_create_pmem_pool_decoders),
				    &req)) {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_PMEM, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	/* libpmemblk pool has to contain at least 256 blocks */
	if (req.num_blocks < 256) {
		goto invalid;
	}

	pool_size = req.num_blocks * req.block_size;
	if (pool_size < PMEMBLK_MIN_POOL) {
		goto invalid;
	}

	pbp = pmemblk_create(req.pmem_file, req.block_size, pool_size, 0666);
	if (pbp == NULL) {
		const char *msg = pmemblk_errormsg();

		SPDK_ERRLOG("pmemblk_create() failed: %s\n", msg ? msg : "(logs disabled)");
		goto invalid;
	}

	pmemblk_close(pbp);

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_create_pmem_pool(&req);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_create_pmem_pool(&req);
}
SPDK_RPC_REGISTER("create_pmem_pool", spdk_rpc_create_pmem_pool, SPDK_RPC_RUNTIME)

struct rpc_pmem_pool_info {
	char *pmem_file;
};

static const struct spdk_json_object_decoder rpc_pmem_pool_info_decoders[] = {
	{"pmem_file", offsetof(struct rpc_pmem_pool_info, pmem_file), spdk_json_decode_string},
};

static void
free_rpc_pmem_pool_info(struct rpc_pmem_pool_info *req)
{
	free(req->pmem_file);
}

static void
spdk_rpc_pmem_pool_info(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_pmem_pool_info req = {};
	struct spdk_json_write_ctx *w;
	size_t num_blocks, block_size;
	PMEMblkpool *pbp;

	if (spdk_json_decode_object(params, rpc_pmem_pool_info_decoders,
				    SPDK_COUNTOF(rpc_pmem_pool_info_decoders),
				    &req)) {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_PMEM, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	pbp = pmemblk_open(req.pmem_file, 0);
	if (pbp == NULL) {
		goto invalid;
	}

	block_size = pmemblk_bsize(pbp);
	num_blocks = pmemblk_nblock(pbp);


	pmemblk_close(pbp);

	/* Check pmem pool consistency */
	if (pmemblk_check(req.pmem_file, block_size) != 1) {
		goto invalid;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint64(w, "num_blocks", num_blocks);
	spdk_json_write_named_uint64(w, "block_size", block_size);
	spdk_json_write_object_end(w);
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_pmem_pool_info(&req);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_pmem_pool_info(&req);
}
SPDK_RPC_REGISTER("pmem_pool_info", spdk_rpc_pmem_pool_info, SPDK_RPC_RUNTIME)

struct rpc_delete_pmem_pool {
	char *pmem_file;
};

static const struct spdk_json_object_decoder rpc_delete_pmem_pool_decoders[] = {
	{"pmem_file", offsetof(struct rpc_delete_pmem_pool, pmem_file), spdk_json_decode_string},
};

static void
free_rpc_delete_pmem_pool(struct rpc_delete_pmem_pool *req)
{
	free(req->pmem_file);
}

static void
spdk_rpc_delete_pmem_pool(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_delete_pmem_pool req = {};
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_delete_pmem_pool_decoders,
				    SPDK_COUNTOF(rpc_delete_pmem_pool_decoders),
				    &req)) {
		SPDK_DEBUGLOG(SPDK_LOG_BDEV_PMEM, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	/* Check if file is actually pmem pool */
	rc = pmemblk_check(req.pmem_file, 0);
	if (rc != 1) {
		const char *msg = pmemblk_errormsg();

		SPDK_ERRLOG("pmemblk_check() failed (%d): %s\n", rc, msg ? msg : "(logs disabled)");
		goto invalid;
	}

	unlink(req.pmem_file);

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_delete_pmem_pool(&req);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_delete_pmem_pool(&req);
}
SPDK_RPC_REGISTER("delete_pmem_pool", spdk_rpc_delete_pmem_pool, SPDK_RPC_RUNTIME)
