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

#ifndef SPDK_VBDEV_CRYPTO_H
#define SPDK_VBDEV_CRYPTO_H

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"

#include "spdk/bdev.h"

#define AESNI_MB "crypto_aesni_mb"
#define QAT "crypto_qat"
#define QAT_ASYM "crypto_qat_asym"
#define MLX5 "mlx5_pci"

/* Supported ciphers */
#define AES_CBC "AES_CBC" /* QAT and AESNI_MB */
#define AES_XTS "AES_XTS" /* QAT and MLX5 */

/* Specific to AES_CBC. */
#define AES_CBC_KEY_LENGTH	     16

#define AES_XTS_128_BLOCK_KEY_LENGTH 16 /* AES-XTS-128 block key size. */
#define AES_XTS_256_BLOCK_KEY_LENGTH 32 /* AES-XTS-256 block key size. */
#define AES_XTS_512_BLOCK_KEY_LENGTH 64 /* AES-XTS-512 block key size. */

#define AES_XTS_TWEAK_KEY_LENGTH     16 /* XTS part key size is always 128 bit. */

/* Structure to hold crypto options for crypto pmd setup. */
struct vbdev_crypto_opts {
	char				*vbdev_name;	/* name of the vbdev to create */
	char				*bdev_name;	/* base bdev name */

	char				*drv_name;	/* name of the crypto device driver */
	char				*cipher;	/* AES_CBC or AES_XTS */

	/* Note, for dev/test we allow use of key in the config file, for production
	 * use, you must use an RPC to specify the key for security reasons.
	 */
	uint8_t				*key;		/* key per bdev */
	uint8_t				key_size;	/* key size */
	uint8_t				*key2;		/* key #2 for AES_XTS, per bdev */
	uint8_t				key2_size;	/* key #2 size */
	uint8_t				*xts_key;	/* key + key 2 */
};

typedef void (*spdk_delete_crypto_complete)(void *cb_arg, int bdeverrno);

/**
 * Create new crypto bdev.
 *
 * \param opts Crypto options populated by create_crypto_opts()
 * \return 0 on success, other on failure.
 */
int create_crypto_disk(struct vbdev_crypto_opts *opts);

/**
 * Delete crypto bdev.
 *
 * \param bdev Pointer to crypto bdev.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Argument to pass to cb_fn.
 */
void delete_crypto_disk(struct spdk_bdev *bdev, spdk_delete_crypto_complete cb_fn,
			void *cb_arg);

/**
 * Release crypto opts created with create_crypto_opts()
 *
 * \param opts Crypto opts to release
 */
void free_crypto_opts(struct vbdev_crypto_opts *opts);

static inline int
__c2v(char c)
{
	if ((c >= '0') && (c <= '9')) {
		return c - '0';
	}
	if ((c >= 'a') && (c <= 'f')) {
		return c - 'a' + 10;
	}
	if ((c >= 'A') && (c <= 'F')) {
		return c - 'A' + 10;
	}
	return -1;
}

static inline char
__v2c(int c)
{
	const char hexchar[] = "0123456789abcdef";
	if (c < 0 || c > 15) {
		return -1;
	}
	return hexchar[c];
}

/**
 * Convert a binary array to hexlified string terminated by zero.
 *
 * \param bin A binary array pointer.
 * \param len Length of the binary array.
 * \return Pointer to hexlified version of @bin or NULL on failure.
 */
static inline char *
hexlify(const char *bin, size_t len)
{
	char *hex, *phex;

	hex = malloc((len * 2) + 1);
	if (hex == NULL) {
		return NULL;
	}
	phex = hex;
	for (size_t i = 0; i < len; i++) {
		char c0 = __v2c((bin[i] >> 4) & 0x0f);
		char c1 = __v2c((bin[i]) & 0x0f);
		if (c0 < 0 || c1 < 0) {
			assert(false);
			free(hex);
			return NULL;
		}
		*phex++ = c0;
		*phex++ = c1;
	}
	*phex = '\0';
	return hex;
}

/**
 * Convert hexlified string to binary array of size strlen(hex) / 2.
 *
 * \param hex A hexlified string terminated by zero.
 * \return Binary array pointer or NULL on failure.
 */
static inline char *
unhexlify(const char *hex)
{
	char *res, *pres;
	size_t len = strlen(hex);

	if (len % 2 != 0) {
		SPDK_ERRLOG("Invalid hex string len %d. It must be mod of 2.\n", (int)len);
		return NULL;
	}
	res = malloc(len / 2);
	if (res == NULL) {
		return NULL;
	}
	pres = res;
	for (size_t i = 0; i < len; i += 2) {
		int v0 = __c2v(hex[i]);
		int v1 = __c2v(hex[i + 1]);
		if (v0 < 0 || v1 < 0) {
			SPDK_ERRLOG("Invalid hex string \"%s\"\n", hex);
			free(res);
			return NULL;
		}
		*pres++ = (v0 << 4) + v1;
	}
	return res;
}

#endif /* SPDK_VBDEV_CRYPTO_H */
