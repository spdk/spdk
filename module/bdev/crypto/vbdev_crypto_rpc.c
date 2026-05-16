/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "vbdev_crypto.h"

#include "spdk/hexlify.h"
#include "spdk_internal/rpc_autogen.h"

/* Reasonable bdev name length + cipher's name len */
#define MAX_KEY_NAME_LEN 128

static const struct spdk_json_object_decoder rpc_bdev_crypto_create_decoders[] = {
	{"base_bdev_name", offsetof(struct rpc_bdev_crypto_create_ctx, base_bdev_name), spdk_json_decode_string},
	{"name", offsetof(struct rpc_bdev_crypto_create_ctx, name), spdk_json_decode_string},
	{"crypto_pmd", offsetof(struct rpc_bdev_crypto_create_ctx, crypto_pmd), spdk_json_decode_string, true},
	{"key", offsetof(struct rpc_bdev_crypto_create_ctx, key), spdk_json_decode_string, true},
	{"cipher", offsetof(struct rpc_bdev_crypto_create_ctx, cipher), spdk_json_decode_string, true},
	{"key2", offsetof(struct rpc_bdev_crypto_create_ctx, key2), spdk_json_decode_string, true},
	{"key_name", offsetof(struct rpc_bdev_crypto_create_ctx, key_name), spdk_json_decode_string, true},
};

static struct vbdev_crypto_opts *
create_crypto_opts(struct rpc_bdev_crypto_create_ctx *rpc, struct spdk_accel_crypto_key *key,
		   bool key_owner)
{
	struct vbdev_crypto_opts *opts = calloc(1, sizeof(*opts));

	if (!opts) {
		return NULL;
	}

	opts->bdev_name = strdup(rpc->base_bdev_name);
	if (!opts->bdev_name) {
		free_crypto_opts(opts);
		return NULL;
	}
	opts->vbdev_name = strdup(rpc->name);
	if (!opts->vbdev_name) {
		free_crypto_opts(opts);
		return NULL;
	}

	opts->key = key;
	opts->key_owner = key_owner;

	return opts;
}

/* Decode the parameters for this RPC method and properly construct the crypto
 * device. Error status returned in the failed cases.
 */
static void
rpc_bdev_crypto_create(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_bdev_crypto_create_ctx req = {};
	struct spdk_accel_crypto_key_create_param param = {};
	struct vbdev_crypto_opts *crypto_opts = NULL;
	struct spdk_json_write_ctx *w;
	struct spdk_accel_crypto_key *key = NULL;
	struct spdk_accel_crypto_key *created_key = NULL;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_bdev_crypto_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_crypto_create_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "Failed to decode crypto disk create parameters.");
		goto cleanup;
	}

	if (!req.name) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "crypto_bdev name is missing");
		goto cleanup;
	}

	if (req.key_name) {
		/* New config version */
		key = spdk_accel_crypto_key_get(req.key_name);
		if (key) {
			if (req.key || req.cipher || req.crypto_pmd) {
				SPDK_NOTICELOG("Key name specified, other parameters are ignored\n");
			}
			SPDK_NOTICELOG("Found key \"%s\"\n", req.key_name);
		}
	}

	/* No key_name. Support legacy configuration */
	if (!key) {
		if (req.key_name) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Key was not found");
			goto cleanup;
		}

		if (req.cipher == NULL) {
			req.cipher = strdup(BDEV_CRYPTO_DEFAULT_CIPHER);
			if (req.cipher == NULL) {
				spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
								 "Unable to allocate memory for req.cipher");
				goto cleanup;
			}
		}
		if (req.crypto_pmd) {
			SPDK_WARNLOG("\"crypto_pmd\" parameters is obsolete and ignored\n");
		}

		req.key_name = calloc(1, MAX_KEY_NAME_LEN);
		if (!req.key_name) {
			/* The new API requires key name. Create it as pmd_name + cipher */
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Unable to allocate memory for key_name");
			goto cleanup;
		}
		snprintf(req.key_name, MAX_KEY_NAME_LEN, "%s_%s", req.name, req.cipher);

		/* Try to find a key with generated name, we may be loading from a json config where crypto_bdev had no key_name parameter */
		key = spdk_accel_crypto_key_get(req.key_name);
		if (key) {
			SPDK_NOTICELOG("Found key \"%s\"\n", req.key_name);
		} else {
			param.cipher = req.cipher;
			param.hex_key = req.key;
			param.hex_key2 = req.key2;
			param.key_name = req.key_name;
			rc = spdk_accel_crypto_key_create(&param);
			if (!rc) {
				key = spdk_accel_crypto_key_get(req.key_name);
				created_key = key;
			}
		}
	}

	if (!key) {
		/* We haven't found an existing key or were not able to create a new one */
		SPDK_ERRLOG("No key was found\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "No key was found");
		goto cleanup;
	}

	crypto_opts = create_crypto_opts(&req, key, created_key != NULL);
	if (!crypto_opts) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation failed");
		goto cleanup;
	}

	rc = create_crypto_disk(crypto_opts);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		free_crypto_opts(crypto_opts);
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, req.name);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	if (rc && created_key) {
		spdk_accel_crypto_key_destroy(created_key);
	}
	if (req.key) {
		memset(req.key, 0, strnlen(req.key, SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH));
	}
	if (req.key2) {
		memset(req.key2, 0, strnlen(req.key2, SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH));
	}
	free_rpc_bdev_crypto_create(&req);
}
SPDK_RPC_REGISTER("bdev_crypto_create", rpc_bdev_crypto_create, SPDK_RPC_RUNTIME)

static const struct spdk_json_object_decoder rpc_bdev_crypto_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_crypto_delete_ctx, name), spdk_json_decode_string},
};

static void
rpc_bdev_crypto_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_crypto_delete(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_bdev_crypto_delete_ctx req = {};

	if (spdk_json_decode_object(params, rpc_bdev_crypto_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_crypto_delete_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto cleanup;
	}

	delete_crypto_disk(req.name, rpc_bdev_crypto_delete_cb, request);

	free_rpc_bdev_crypto_delete(&req);

	return;

cleanup:
	free_rpc_bdev_crypto_delete(&req);
}
SPDK_RPC_REGISTER("bdev_crypto_delete", rpc_bdev_crypto_delete, SPDK_RPC_RUNTIME)
