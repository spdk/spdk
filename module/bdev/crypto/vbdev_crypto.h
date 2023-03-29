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
#include "spdk/accel.h"
#include "spdk/accel_module.h"

#include "spdk/bdev.h"

#define BDEV_CRYPTO_DEFAULT_CIPHER "AES_CBC" /* QAT and AESNI_MB */

/* Structure to hold crypto options */
struct vbdev_crypto_opts {
	char				*vbdev_name;	/* name of the vbdev to create */
	char				*bdev_name;	/* base bdev name */
	struct spdk_accel_crypto_key	*key;		/* crypto key */
	bool				key_owner;	/* If wet to true then the key was created by RPC and needs to be destroyed */
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
