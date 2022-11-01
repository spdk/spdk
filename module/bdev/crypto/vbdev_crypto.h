/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
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
 * \param bdev_name Crypto bdev name.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Argument to pass to cb_fn.
 */
void delete_crypto_disk(const char *bdev_name, spdk_delete_crypto_complete cb_fn,
			void *cb_arg);

/**
 * Release crypto opts created with create_crypto_opts()
 *
 * \param opts Crypto opts to release
 */
void free_crypto_opts(struct vbdev_crypto_opts *opts);

#endif /* SPDK_VBDEV_CRYPTO_H */
