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

#include "vbdev_compress.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"

struct rpc_bdev_compress_get_orphans {
	char *name;
};

static void
free_rpc_bdev_compress_get_orphans(struct rpc_bdev_compress_get_orphans *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_compress_get_orphans_decoders[] = {
	{"name", offsetof(struct rpc_bdev_compress_get_orphans, name), spdk_json_decode_string, true},
};

static void
rpc_bdev_compress_get_orphans(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_bdev_compress_get_orphans req = {};
	struct spdk_json_write_ctx *w;
	struct vbdev_compress *comp_bdev;
	bool found = false;


	if (params && spdk_json_decode_object(params, rpc_bdev_compress_get_orphans_decoders,
					      SPDK_COUNTOF(rpc_bdev_compress_get_orphans_decoders),
					      &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		free_rpc_bdev_compress_get_orphans(&req);
		return;
	}

	if (req.name) {
		if (compress_has_orphan(req.name) == false) {
			spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
			free_rpc_bdev_compress_get_orphans(&req);
			return;
		}
		found = true;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	if (found) {
		spdk_json_write_string(w, req.name);
	} else {
		for (comp_bdev = compress_bdev_first(); comp_bdev != NULL;
		     comp_bdev = compress_bdev_next(comp_bdev)) {
			if (compress_has_orphan(compress_get_name(comp_bdev))) {
				spdk_json_write_string(w, compress_get_name(comp_bdev));
			}
		}
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_bdev_compress_get_orphans(&req);
}
SPDK_RPC_REGISTER("bdev_compress_get_orphans", rpc_bdev_compress_get_orphans, SPDK_RPC_RUNTIME)

struct rpc_compress_set_pmd {
	enum compress_pmd pmd;
};

static const struct spdk_json_object_decoder rpc_compress_pmd_decoder[] = {
	{"pmd", offsetof(struct rpc_compress_set_pmd, pmd), spdk_json_decode_int32},
};

static void
rpc_bdev_compress_set_pmd(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_compress_set_pmd req;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_compress_pmd_decoder,
				    SPDK_COUNTOF(rpc_compress_pmd_decoder),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		return;
	}

	if (req.pmd >= COMPRESS_PMD_MAX) {
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
						     "PMD value %d should be less than %d", req.pmd, COMPRESS_PMD_MAX);
		return;
	}

	rc = compress_set_pmd(&req.pmd);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}

	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("bdev_compress_set_pmd", rpc_bdev_compress_set_pmd,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_compress_set_pmd, set_compress_pmd)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_compress_set_pmd, compress_set_pmd)

/* Structure to hold the parameters for this RPC method. */
struct rpc_construct_compress {
	char *base_bdev_name;
	char *pm_path;
	uint32_t lb_size;
};

/* Free the allocated memory resource after the RPC handling. */
static void
free_rpc_construct_compress(struct rpc_construct_compress *r)
{
	free(r->base_bdev_name);
	free(r->pm_path);
}

/* Structure to decode the input parameters for this RPC method. */
static const struct spdk_json_object_decoder rpc_construct_compress_decoders[] = {
	{"base_bdev_name", offsetof(struct rpc_construct_compress, base_bdev_name), spdk_json_decode_string},
	{"pm_path", offsetof(struct rpc_construct_compress, pm_path), spdk_json_decode_string},
	{"lb_size", offsetof(struct rpc_construct_compress, lb_size), spdk_json_decode_uint32},
};

/* Decode the parameters for this RPC method and properly construct the compress
 * device. Error status returned in the failed cases.
 */
static void
rpc_bdev_compress_create(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_construct_compress req = {NULL};
	struct spdk_json_write_ctx *w;
	char *name;
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_compress_decoders,
				    SPDK_COUNTOF(rpc_construct_compress_decoders),
				    &req)) {
		SPDK_DEBUGLOG(vbdev_compress, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = create_compress_bdev(req.base_bdev_name, req.pm_path, req.lb_size);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	name = spdk_sprintf_alloc("COMP_%s", req.base_bdev_name);
	spdk_json_write_string(w, name);
	spdk_jsonrpc_end_result(request, w);
	free(name);

cleanup:
	free_rpc_construct_compress(&req);
}
SPDK_RPC_REGISTER("bdev_compress_create", rpc_bdev_compress_create, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_compress_create, construct_compress_bdev)

struct rpc_delete_compress {
	char *name;
};

static void
free_rpc_delete_compress(struct rpc_delete_compress *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_delete_compress_decoders[] = {
	{"name", offsetof(struct rpc_delete_compress, name), spdk_json_decode_string},
};

static void
_rpc_bdev_compress_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	spdk_jsonrpc_send_bool_response(request, bdeverrno == 0);
}

static void
rpc_bdev_compress_delete(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct rpc_delete_compress req = {NULL};

	if (spdk_json_decode_object(params, rpc_delete_compress_decoders,
				    SPDK_COUNTOF(rpc_delete_compress_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
	} else {
		bdev_compress_delete(req.name, _rpc_bdev_compress_delete_cb, request);
	}

	free_rpc_delete_compress(&req);
}
SPDK_RPC_REGISTER("bdev_compress_delete", rpc_bdev_compress_delete, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_compress_delete, delete_compress_bdev)
