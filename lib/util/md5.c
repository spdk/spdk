/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/md5.h"
#include "spdk/likely.h"

int
spdk_md5init(struct spdk_md5ctx *md5ctx)
{
	int rc;

	if (spdk_unlikely(md5ctx == NULL)) {
		return -1;
	}

	md5ctx->md5ctx = EVP_MD_CTX_create();
	if (spdk_unlikely(md5ctx->md5ctx == NULL)) {
		return -1;
	}

	rc = EVP_DigestInit_ex(md5ctx->md5ctx, EVP_md5(), NULL);
	/* For EVP_DigestInit_ex, 1 == success, 0 == failure. */
	if (spdk_unlikely(rc == 0)) {
		EVP_MD_CTX_destroy(md5ctx->md5ctx);
		md5ctx->md5ctx = NULL;
		rc = -1;
	}
	return rc;
}

int
spdk_md5final(void *md5, struct spdk_md5ctx *md5ctx)
{
	int rc;

	if (spdk_unlikely(md5ctx == NULL || md5 == NULL)) {
		return -1;
	}
	rc = EVP_DigestFinal_ex(md5ctx->md5ctx, md5, NULL);
	EVP_MD_CTX_destroy(md5ctx->md5ctx);
	md5ctx->md5ctx = NULL;
	return rc;
}

int
spdk_md5update(struct spdk_md5ctx *md5ctx, const void *data, size_t len)
{
	int rc;

	if (spdk_unlikely(md5ctx == NULL)) {
		return -1;
	}
	if (spdk_unlikely(data == NULL || len == 0)) {
		return 0;
	}
	rc = EVP_DigestUpdate(md5ctx->md5ctx, data, len);
	return rc;
}
