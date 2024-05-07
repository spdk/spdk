/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#ifndef SPDK_MD5_H
#define SPDK_MD5_H

#include "spdk/stdinc.h"

#include <openssl/md5.h>
#include <openssl/evp.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_MD5DIGEST_LEN MD5_DIGEST_LENGTH

struct spdk_md5ctx {
	EVP_MD_CTX *md5ctx;
};

/**
 * Init md5 context
 *
 * \param md5ctx context
 * \return 0 on success, -1 on failure
 */
int spdk_md5init(struct spdk_md5ctx *md5ctx);

/**
 * Update \b md5ctx digest with hash of \b len bytes of \b data.
 *
 * This function can be called several times on the same md5ctx to hash additional data
 *
 * \param md5ctx context
 * \param data data pointer
 * \param len length of data buffer in bytes
 * \return 0 on success, -1 on failure
 */
int spdk_md5update(struct spdk_md5ctx *md5ctx, const void *data, size_t len);

/**
 * Retrieves the digest from \b ctx and places it in \b md5 buffer.
 *
 * \b md5 buffer must be \b SPDK_MD5DIGEST_LEN bytes length. \b md5ctx is released, it can be used again after
 * initialization via \ref spdk_md5init
 *
 * \param md5
 * \param md5ctx
 * \return 0 on success, -1 on failure
 */
int spdk_md5final(void *md5, struct spdk_md5ctx *md5ctx);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_MD5_H */
