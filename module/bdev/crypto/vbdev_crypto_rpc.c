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

#include "vbdev_crypto.h"

/* Structure to hold the parameters for this RPC method. */
struct rpc_construct_crypto {
	char *base_bdev_name;
	char *name;
	char *crypto_pmd;
	char *key;
	char *cipher;
	char *key2;
};

/* Free the allocated memory resource after the RPC handling. */
static void
free_rpc_construct_crypto(struct rpc_construct_crypto *r)
{
	free(r->base_bdev_name);
	free(r->name);
	free(r->crypto_pmd);
	free(r->key);
	free(r->cipher);
	free(r->key2);
}

/* Structure to decode the input parameters for this RPC method. */
static const struct spdk_json_object_decoder rpc_construct_crypto_decoders[] = {
	{"base_bdev_name", offsetof(struct rpc_construct_crypto, base_bdev_name), spdk_json_decode_string},
	{"name", offsetof(struct rpc_construct_crypto, name), spdk_json_decode_string},
	{"crypto_pmd", offsetof(struct rpc_construct_crypto, crypto_pmd), spdk_json_decode_string},
	{"key", offsetof(struct rpc_construct_crypto, key), spdk_json_decode_string},
	{"cipher", offsetof(struct rpc_construct_crypto, cipher), spdk_json_decode_string, true},
	{"key2", offsetof(struct rpc_construct_crypto, key2), spdk_json_decode_string, true},
};

/* Decode the parameters for this RPC method and properly construct the crypto
 * device. Error status returned in the failed cases.
 */
static void
rpc_bdev_crypto_create(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_construct_crypto req = {NULL};
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_crypto_decoders,
				    SPDK_COUNTOF(rpc_construct_crypto_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto cleanup;
	}

	if (req.cipher == NULL) {
		req.cipher = strdup(AES_CBC);
		if (req.cipher == NULL) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Unable to allocate memory for req.cipher");
			goto cleanup;
		}
	}

	if (strcmp(req.cipher, AES_XTS) != 0 && strcmp(req.cipher, AES_CBC) != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid cipher: %s",
						     req.cipher);
		goto cleanup;
	}

	if (strcmp(req.crypto_pmd, AESNI_MB) == 0 && strcmp(req.cipher, AES_XTS) == 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid cipher. AES_XTS is only available on QAT.");
		goto cleanup;
	}

	if (strcmp(req.cipher, AES_XTS) == 0 && req.key2 == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid key. A 2nd key is needed for AES_XTS.");
		goto cleanup;
	}

	if (strcmp(req.cipher, AES_CBC) == 0 && req.key2 != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid key. A 2nd key is needed only for AES_XTS.");
		goto cleanup;
	}

	rc = create_crypto_disk(req.base_bdev_name, req.name,
				req.crypto_pmd, req.key, req.cipher, req.key2);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, req.name);
	spdk_jsonrpc_end_result(request, w);
	free_rpc_construct_crypto(&req);
	return;

cleanup:
	free_rpc_construct_crypto(&req);
}
SPDK_RPC_REGISTER("bdev_crypto_create", rpc_bdev_crypto_create, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_crypto_create, construct_crypto_bdev)

struct rpc_delete_crypto {
	char *name;
};

static void
free_rpc_delete_crypto(struct rpc_delete_crypto *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_delete_crypto_decoders[] = {
	{"name", offsetof(struct rpc_delete_crypto, name), spdk_json_decode_string},
};

static void
rpc_bdev_crypto_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	spdk_jsonrpc_send_bool_response(request, bdeverrno == 0);
}

static void
rpc_bdev_crypto_delete(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_delete_crypto req = {NULL};
	struct spdk_bdev *bdev;

	if (spdk_json_decode_object(params, rpc_delete_crypto_decoders,
				    SPDK_COUNTOF(rpc_delete_crypto_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto cleanup;
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	delete_crypto_disk(bdev, rpc_bdev_crypto_delete_cb, request);

	free_rpc_delete_crypto(&req);

	return;

cleanup:
	free_rpc_delete_crypto(&req);
}
SPDK_RPC_REGISTER("bdev_crypto_delete", rpc_bdev_crypto_delete, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_crypto_delete, delete_crypto_bdev)
