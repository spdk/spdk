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
#include "spdk/lvol.h"
#include "spdk/gpt_spec.h"
#include "vbdev_lvol.h"

#include "spdk_internal/log.h"

struct rpc_construct_lvol_store {
	char *base_name;
};

static void
free_rpc_construct_lvol_store(struct rpc_construct_lvol_store *req)
{
	free(req->base_name);
}

static const struct spdk_json_object_decoder rpc_construct_lvol_store_decoders[] = {
	{"base_name", offsetof(struct rpc_construct_lvol_store, base_name), spdk_json_decode_string},
};

void
spdk_rpc_lvol_store_construct_cb(void *cb_arg, struct spdk_lvol_store *lvol_store, int bserrno)
{
	struct spdk_json_write_ctx *w;
	//char *guid;
	struct spdk_lvol_store_rpc_req *rpc = cb_arg;
	struct spdk_jsonrpc_server_conn *conn = rpc->conn;
	const struct spdk_json_val *id = rpc->id;

	if (bserrno != 0) {
		goto invalid;

	}

	//guid = lvol_store->guid;

	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_object_begin(w);
	spdk_json_write_name(w, "GUID");
	spdk_json_write_string(w, "123");
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(conn, w);

	free(rpc);

	return;

invalid:
	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free(rpc);
	return;
}

static void
spdk_rpc_construct_lvol_store(struct spdk_jsonrpc_server_conn *conn,
			      const struct spdk_json_val *params,
			      const struct spdk_json_val *id)
{
	struct rpc_construct_lvol_store req = {};
	struct spdk_lvol_store_rpc_req *rpc;
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_construct_lvol_store_decoders,
				    SPDK_COUNTOF(rpc_construct_lvol_store_decoders),
				    &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	if (req.base_name == NULL) {
		SPDK_ERRLOG("missing name param\n");
		goto invalid;
	}

	bdev = spdk_bdev_get_by_name(req.base_name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev '%s' does not exist\n", req.base_name);
		goto invalid;
	}


	if (id == NULL) {
		free_rpc_construct_lvol_store(&req);
		return;
	}

	rpc = calloc(1, sizeof(*rpc));

	rpc->conn = conn;
	rpc->id = id;

	if (vbdev_construct_lvol_store(bdev, spdk_rpc_lvol_store_construct_cb, rpc)) {
		goto invalid;
	}

	return;

invalid:
	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_construct_lvol_store(&req);
}
SPDK_RPC_REGISTER("construct_lvol_store", spdk_rpc_construct_lvol_store)

struct rpc_destroy_lvol_store {
	char *guid;
};

static void
free_rpc_destroy_lvol_store(struct rpc_destroy_lvol_store *req)
{
	free(req->guid);
}

static const struct spdk_json_object_decoder rpc_destroy_lvol_store_decoders[] = {
	{"guid", offsetof(struct rpc_destroy_lvol_store, guid), spdk_json_decode_string},
};

static void
spdk_rpc_destroy_lvol_store(struct spdk_jsonrpc_server_conn *conn,
			    const struct spdk_json_val *params,
			    const struct spdk_json_val *id)
{
	struct rpc_destroy_lvol_store req = {};
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_destroy_lvol_store_decoders,
				    SPDK_COUNTOF(rpc_destroy_lvol_store_decoders),
				    &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	//TODO: destroy lvol store here

	if (id == NULL) {
		free_rpc_destroy_lvol_store(&req);
		return;
	}

	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(conn, w);

	free_rpc_destroy_lvol_store(&req);

	return;

invalid:
	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_destroy_lvol_store(&req);
}
SPDK_RPC_REGISTER("destroy_lvol_store", spdk_rpc_destroy_lvol_store)

struct rpc_construct_lvol_bdev {
	char *guid;
	char *size;
};

static void
free_rpc_construct_lvol_bdev(struct rpc_construct_lvol_bdev *req)
{
	free(req->guid);
}

static const struct spdk_json_object_decoder rpc_construct_lvol_bdev_decoders[] = {
	{"guid", offsetof(struct rpc_construct_lvol_bdev, guid), spdk_json_decode_string},
	{"size", offsetof(struct rpc_construct_lvol_bdev, size), spdk_json_decode_string},
};

static void
spdk_rpc_construct_lvol_bdev_complete(void *args, int bserrno)
{

}

static void
spdk_rpc_construct_lvol_bdev(struct spdk_jsonrpc_server_conn *conn,
			     const struct spdk_json_val *params,
			     const struct spdk_json_val *id)
{
	struct rpc_construct_lvol_bdev req = {};
//	struct spdk_json_write_ctx *w;
//	struct spdk_bdev *bdev = NULL;
	struct spdk_lvol_store *ls;
	struct spdk_lvol_store_guid guid;
	size_t sz;

	SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL, "Creating blob\n");

	if (spdk_json_decode_object(params, rpc_construct_lvol_bdev_decoders,
				    SPDK_COUNTOF(rpc_construct_lvol_bdev_decoders),
				    &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	guid = SPDK_GPT_GUID(0, 0, 0, 0, 0);

	ls = vbdev_get_lvol_store_by_guid(&guid);

	if (ls == NULL) {
		SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL, "blobstore with guid '%p' not found\n", &guid);
		goto invalid;
	}

	sz = *(req.size);

	SPDK_TRACELOG(SPDK_TRACE_VBDEV_LVOL, "Creating blob\n");
	//lvol_store_create_lvol(ls, sz, spdk_rpc_construct_lvol_bdev_complete, NULL);
	lvol_create_lvol(ls, sz, spdk_rpc_construct_lvol_bdev_complete, NULL);


	/*
		//TODO: create lvol bdev here
		if (bdev == NULL) {
			goto invalid;
		}

		if (id == NULL) {
			free_rpc_construct_lvol_bdev(&req);
			return;
		}

		w = spdk_jsonrpc_begin_result(conn, id);
		spdk_json_write_array_begin(w);
		spdk_json_write_string(w, spdk_bdev_get_name(bdev));
		spdk_json_write_array_end(w);
		spdk_jsonrpc_end_result(conn, w);

		free_rpc_construct_lvol_bdev(&req);
	 */
	return;

invalid:
	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_construct_lvol_bdev(&req);
}
SPDK_RPC_REGISTER("construct_lvol_bdev", spdk_rpc_construct_lvol_bdev)

struct rpc_extend_lvol_bdev {
	char *name;
	uint64_t size;
};

static void
free_rpc_extend_lvol_bdev(struct rpc_extend_lvol_bdev *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_extend_lvol_bdev_decoders[] = {
	{"guid", offsetof(struct rpc_extend_lvol_bdev, name), spdk_json_decode_string},
	{"size", offsetof(struct rpc_extend_lvol_bdev, size), spdk_json_decode_uint64},
};

static void
spdk_rpc_extend_lvol_bdev(struct spdk_jsonrpc_server_conn *conn,
			  const struct spdk_json_val *params,
			  const struct spdk_json_val *id)
{
	struct rpc_extend_lvol_bdev req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev = NULL;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_extend_lvol_bdev_decoders,
				    SPDK_COUNTOF(rpc_extend_lvol_bdev_decoders),
				    &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		SPDK_ERRLOG("bdev '%s' does not exist\n", req.name);
		goto invalid;
	}

	//TODO: extend lvol bdev here
	if (rc < 0) {
		goto invalid;
	}

	if (id == NULL) {
		free_rpc_extend_lvol_bdev(&req);
		return;
	}

	w = spdk_jsonrpc_begin_result(conn, id);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(conn, w);

	free_rpc_extend_lvol_bdev(&req);

	return;

invalid:
	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_extend_lvol_bdev(&req);
}
SPDK_RPC_REGISTER("extend_lvol_bdev", spdk_rpc_extend_lvol_bdev)
