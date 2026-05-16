#include <spdk/rpc.h>
#include <spdk/bdev.h>
#include <spdk/util.h>
#include <spdk/string.h>
#include <spdk/log.h>
#include "vbdev_wal.h"

/* Structure for the WAL bdev creation RPC request */
struct rpc_bdev_wal_create {
	char *name;
	uint32_t block_sz;
	uint64_t size_mb;
	char *journal_bdev_name;
	char *main_bdev_name;
};

/* JSON decoders for the WAL bdev creation request */
static const struct spdk_json_object_decoder rpc_bdev_wal_create_decoders[] = {
	{"name",
	 offsetof(struct rpc_bdev_wal_create, name),
	 spdk_json_decode_string},
	{"block_size",
	 offsetof(struct rpc_bdev_wal_create, block_sz),
	 spdk_json_decode_uint32,
	 true},
	{"size_mb",
	 offsetof(struct rpc_bdev_wal_create, size_mb),
	 spdk_json_decode_uint64,
	 true},
	{"journal_name",
	 offsetof(struct rpc_bdev_wal_create, journal_bdev_name),
	 spdk_json_decode_string},
	{"bdev_name",
	 offsetof(struct rpc_bdev_wal_create, main_bdev_name),
	 spdk_json_decode_string},
};

/* Free RPC creation request fields */
static void free_rpc_bdev_wal_create(struct rpc_bdev_wal_create *req) {
	free(req->name);
	free(req->journal_bdev_name);
	free(req->main_bdev_name);
}

/* Structure for the WAL bdev deletion RPC request */
struct rpc_bdev_wal_delete {
	char *name;
};

/* Free RPC deletion request fields */
static void free_rpc_bdev_wal_delete(struct rpc_bdev_wal_delete *req) {
	free(req->name);
}

/* JSON decoders for the WAL bdev deletion request */
static const struct spdk_json_object_decoder rpc_bdev_wal_delete_decoders[] = {
	{"name",
	 offsetof(struct rpc_bdev_wal_delete, name),
	 spdk_json_decode_string}};

/* Callback for WAL bdev creation completion */
static void rpc_journaling_bdev_create_cb(void *cb_arg, int rc) {
	struct spdk_jsonrpc_request *request = cb_arg;
	struct spdk_json_write_ctx *w;

	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request,
						 rc,
						 spdk_strerror(-rc));
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, "success");
	spdk_jsonrpc_end_result(request, w);
}

static void rpc_journaling_bdev_create(struct spdk_jsonrpc_request *request,
				       const struct spdk_json_val *params) {
	struct rpc_bdev_wal_create req = {NULL};
	int rc;

	if (spdk_json_decode_object(params,
				    rpc_bdev_wal_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_wal_create_decoders),
				    &req)) {
		SPDK_DEBUGLOG(
			wal_vbdev,
			"spdk_json_decode_object failed\n"); /* TODO: add wal_vbdev logging */
		spdk_jsonrpc_send_error_response(
			request,
			SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
			"spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = wal_bdev_create_disk(req.main_bdev_name,
				  req.journal_bdev_name,
				  req.name,
				  &req.block_sz,
				  &req.size_mb,
				  rpc_journaling_bdev_create_cb,
				  request);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request,
						 rc,
						 spdk_strerror(-rc));
		goto cleanup;
	}

	/* Response is deferred due to asynchronous creation */
	free_rpc_bdev_wal_create(&req);
	return;

cleanup:
	free_rpc_bdev_wal_create(&req);
}
/* Register RPC method for device creation */
SPDK_RPC_REGISTER("wal_bdev_create",
		  rpc_journaling_bdev_create,
		  SPDK_RPC_RUNTIME)

/* Callback for WAL bdev deletion completion */
static void rpc_journaling_bdev_delete_cb(void *cb_arg, int bdeverrno) {
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request,
						 bdeverrno,
						 spdk_strerror(-bdeverrno));
	}
}

/* Delete WAL bdev RPC handler */
static void rpc_wal_bdev_delete(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params) {
	struct rpc_bdev_wal_delete req = {NULL};

	if (spdk_json_decode_object(params,
				    rpc_bdev_wal_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_wal_delete_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(
			request,
			SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
			"spdk_json_decode_object failed");
		goto cleanup;
	}

	wal_bdev_delete_disk(req.name, rpc_journaling_bdev_delete_cb, request);

cleanup:
	free_rpc_bdev_wal_delete(&req);
}

/* Register RPC method for device deletion */
SPDK_RPC_REGISTER("wal_bdev_delete", rpc_wal_bdev_delete, SPDK_RPC_RUNTIME)

struct rpc_wal_bdev_recover {
	char *name;
};

static const struct spdk_json_object_decoder rpc_wal_bdev_recover_decoders[] = {
	{"name",
	 offsetof(struct rpc_wal_bdev_recover, name),
	 spdk_json_decode_string},
};

static void rpc_wal_bdev_recover_done(void *cb_arg, int bdeverrno) {
	struct spdk_jsonrpc_request *request = cb_arg;
	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response_fmt(
			request,
			SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
			"wal_bdev_recover failed: %d",
			bdeverrno);
	}
}

/**
 * @brief Handle the JSON-RPC request to start WAL recovery.
 *
 * Initiates the background recovery process that reads the journal and
 * replays uncommitted data to the main device.
 *
 * @param request The JSON-RPC request context.
 * @param params The JSON parameters containing the WAL bdev name.
 */
static void rpc_wal_bdev_recover(struct spdk_jsonrpc_request *request,
				 const struct spdk_json_val *params) {
	struct rpc_wal_bdev_recover req = {};
	if (spdk_json_decode_object(params,
				    rpc_wal_bdev_recover_decoders,
				    SPDK_COUNTOF(rpc_wal_bdev_recover_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(
			request,
			SPDK_JSONRPC_ERROR_INVALID_PARAMS,
			"invalid params");
		return;
	}

	int rc = wal_bdev_recover(req.name, rpc_wal_bdev_recover_done, request);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(
			request,
			SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
			"wal_bdev_recover start failed: %d",
			rc);
	}
	free(req.name);
}

SPDK_RPC_REGISTER("wal_bdev_recover", rpc_wal_bdev_recover, SPDK_RPC_RUNTIME)
GISTER("wal_bdev_recover", rpc_wal_bdev_recover, SPDK_RPC_RUNTIME)
_bdev_recover, SPDK_RPC_RUNTIME)
SPDK_RPC_RUNTIME)
_RPC_RUNTIME)
PDK_RPC_RUNTIME)
R("wal_bdev_recover", rpc_wal_bdev_recover, SPDK_RPC_RUNTIME)
_bdev_recover, SPDK_RPC_RUNTIME)
SPDK_RPC_RUNTIME)
_RPC_RUNTIME)
PDK_RPC_RUNTIME)
