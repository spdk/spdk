/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
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

/**
 * Create crypto opts from rpc @req. Validate req fields and populate the
 * correspoending fields in @opts.
 *
 * \param rpc Pointer to the rpc req.
 * \param request Pointer to json request.
 * \return Allocated and populated crypto opts or NULL on failure.
 */
static struct vbdev_crypto_opts *
create_crypto_opts(struct rpc_construct_crypto *rpc,
		   struct spdk_jsonrpc_request *request)
{
	struct vbdev_crypto_opts *opts;
	int key_size, key2_size;

	if (strcmp(rpc->crypto_pmd, AESNI_MB) == 0 && strcmp(rpc->cipher, AES_XTS) == 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid cipher. AES_XTS is not available on AESNI_MB.");
		return NULL;
	}

	if (strcmp(rpc->crypto_pmd, MLX5) == 0 && strcmp(rpc->cipher, AES_XTS) != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid cipher. %s is not available on MLX5.",
						     rpc->cipher);
		return NULL;
	}

	if (strcmp(rpc->cipher, AES_XTS) == 0 && rpc->key2 == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid key. A 2nd key is needed for AES_XTS.");
		return NULL;
	}

	if (strcmp(rpc->cipher, AES_CBC) == 0 && rpc->key2 != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid key. A 2nd key is needed only for AES_XTS.");
		return NULL;
	}

	opts = calloc(1, sizeof(struct vbdev_crypto_opts));
	if (!opts) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to allocate memory for crypto_opts.");
		return NULL;
	}

	opts->bdev_name = strdup(rpc->base_bdev_name);
	if (!opts->bdev_name) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to allocate memory for bdev_name.");
		goto error_alloc_bname;
	}

	opts->vbdev_name = strdup(rpc->name);
	if (!opts->vbdev_name) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to allocate memory for vbdev_name.");
		goto error_alloc_vname;
	}

	opts->drv_name = strdup(rpc->crypto_pmd);
	if (!opts->drv_name) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to allocate memory for drv_name.");
		goto error_alloc_dname;
	}

	if (strcmp(opts->drv_name, MLX5) == 0) {
		/* Only AES-XTS supported. */

		/* We cannot use strlen() after unhexlify() because of possible \0 chars
		 * used in the key. Hexlified version of key is twice as longer. */
		key_size = strnlen(rpc->key, (AES_XTS_512_BLOCK_KEY_LENGTH * 2) + 1);
		if (key_size != AES_XTS_256_BLOCK_KEY_LENGTH * 2 &&
		    key_size != AES_XTS_512_BLOCK_KEY_LENGTH * 2) {
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							     "Invalid AES_XTS key string length for mlx5: %d. "
							     "Supported sizes in hex form: %d or %d.",
							     key_size, AES_XTS_256_BLOCK_KEY_LENGTH * 2,
							     AES_XTS_512_BLOCK_KEY_LENGTH * 2);
			goto error_invalid_key;
		}
	} else {
		if (strncmp(rpc->cipher, AES_XTS, sizeof(AES_XTS)) == 0) {
			/* AES_XTS for qat uses 128bit key. */
			key_size = strnlen(rpc->key, (AES_XTS_128_BLOCK_KEY_LENGTH * 2) + 1);
			if (key_size != AES_XTS_128_BLOCK_KEY_LENGTH * 2) {
				spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
								     "Invalid AES_XTS key string length: %d. "
								     "Supported size in hex form: %d.",
								     key_size, AES_XTS_128_BLOCK_KEY_LENGTH * 2);
				goto error_invalid_key;
			}
		} else {
			key_size = strnlen(rpc->key, (AES_CBC_KEY_LENGTH * 2) + 1);
			if (key_size != AES_CBC_KEY_LENGTH * 2) {
				spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
								     "Invalid AES_CBC key string length: %d. "
								     "Supported size in hex form: %d.",
								     key_size, AES_CBC_KEY_LENGTH * 2);
				goto error_invalid_key;
			}
		}
	}
	opts->key = unhexlify(rpc->key);
	if (!opts->key) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Failed to unhexlify key.");
		goto error_alloc_key;
	}
	opts->key_size = key_size / 2;

	if (strncmp(rpc->cipher, AES_XTS, sizeof(AES_XTS)) == 0) {
		opts->cipher = AES_XTS;
		assert(rpc->key2);
		key2_size = strnlen(rpc->key2, (AES_XTS_TWEAK_KEY_LENGTH * 2) + 1);
		if (key2_size != AES_XTS_TWEAK_KEY_LENGTH * 2) {
			spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							     "Invalid AES_XTS key2 length %d. "
							     "Supported size in hex form: %d.",
							     key2_size, AES_XTS_TWEAK_KEY_LENGTH * 2);
			goto error_invalid_key2;
		}
		opts->key2 = unhexlify(rpc->key2);
		if (!opts->key2) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Failed to unhexlify key2.");
			goto error_alloc_key2;
		}
		opts->key2_size = key2_size / 2;

		/* DPDK expects the keys to be concatenated together. */
		opts->xts_key = calloc(1, opts->key_size + opts->key2_size + 1);
		if (opts->xts_key == NULL) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
							 "Failed to allocate memory for XTS key.");
			goto error_alloc_xts;
		}
		memcpy(opts->xts_key, opts->key, opts->key_size);
		memcpy(opts->xts_key + opts->key_size, opts->key2, opts->key2_size);
	} else if (strncmp(rpc->cipher, AES_CBC, sizeof(AES_CBC)) == 0) {
		opts->cipher = AES_CBC;
	} else {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Invalid param. Cipher %s is not supported.",
						     rpc->cipher);
		goto error_cipher;
	}
	return opts;

	/* Error cleanup paths. */
error_cipher:
error_alloc_xts:
error_alloc_key2:
error_invalid_key2:
	if (opts->key) {
		memset(opts->key, 0, opts->key_size);
		free(opts->key);
	}
	opts->key_size = 0;
error_alloc_key:
error_invalid_key:
	free(opts->drv_name);
error_alloc_dname:
	free(opts->vbdev_name);
error_alloc_vname:
	free(opts->bdev_name);
error_alloc_bname:
	free(opts);
	return NULL;
}

/* Decode the parameters for this RPC method and properly construct the crypto
 * device. Error status returned in the failed cases.
 */
static void
rpc_bdev_crypto_create(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_construct_crypto req = {NULL};
	struct vbdev_crypto_opts *crypto_opts;
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_construct_crypto_decoders,
				    SPDK_COUNTOF(rpc_construct_crypto_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Failed to decode crypto disk create parameters.");
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

	crypto_opts = create_crypto_opts(&req, request);
	if (crypto_opts == NULL) {
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
	struct rpc_delete_crypto req = {NULL};

	if (spdk_json_decode_object(params, rpc_delete_crypto_decoders,
				    SPDK_COUNTOF(rpc_delete_crypto_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto cleanup;
	}

	delete_crypto_disk(req.name, rpc_bdev_crypto_delete_cb, request);

	free_rpc_delete_crypto(&req);

	return;

cleanup:
	free_rpc_delete_crypto(&req);
}
SPDK_RPC_REGISTER("bdev_crypto_delete", rpc_bdev_crypto_delete, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(bdev_crypto_delete, delete_crypto_bdev)
