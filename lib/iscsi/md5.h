/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_MD5_H
#define SPDK_MD5_H

#include "spdk/stdinc.h"

#include <openssl/md5.h>
#include <openssl/evp.h>

#define SPDK_MD5DIGEST_LEN MD5_DIGEST_LENGTH

struct spdk_md5ctx {
	EVP_MD_CTX *md5ctx;
};

int md5init(struct spdk_md5ctx *md5ctx);
int md5final(void *md5, struct spdk_md5ctx *md5ctx);
int md5update(struct spdk_md5ctx *md5ctx, const void *data, size_t len);

#endif /* SPDK_MD5_H */
