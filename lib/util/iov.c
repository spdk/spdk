/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/util.h"

size_t
spdk_ioviter_first(struct spdk_ioviter *iter,
		   struct iovec *siov, size_t siovcnt,
		   struct iovec *diov, size_t diovcnt,
		   void **src, void **dst)
{
	iter->siov = siov;
	iter->siovcnt = siovcnt;

	iter->diov = diov;
	iter->diovcnt = diovcnt;

	iter->sidx = 0;
	iter->didx = 0;
	iter->siov_len = siov[0].iov_len;
	iter->siov_base = siov[0].iov_base;
	iter->diov_len = diov[0].iov_len;
	iter->diov_base = diov[0].iov_base;

	return spdk_ioviter_next(iter, src, dst);
}

size_t
spdk_ioviter_next(struct spdk_ioviter *iter, void **src, void **dst)
{
	size_t len = 0;

	if (iter->sidx == iter->siovcnt ||
	    iter->didx == iter->diovcnt ||
	    iter->siov_len == 0 ||
	    iter->diov_len == 0) {
		return 0;
	}

	*src = iter->siov_base;
	*dst = iter->diov_base;
	len = spdk_min(iter->siov_len, iter->diov_len);

	if (iter->siov_len == iter->diov_len) {
		/* Advance both iovs to the next element */
		iter->sidx++;
		if (iter->sidx == iter->siovcnt) {
			return len;
		}

		iter->didx++;
		if (iter->didx == iter->diovcnt) {
			return len;
		}

		iter->siov_len = iter->siov[iter->sidx].iov_len;
		iter->siov_base = iter->siov[iter->sidx].iov_base;
		iter->diov_len = iter->diov[iter->didx].iov_len;
		iter->diov_base = iter->diov[iter->didx].iov_base;
	} else if (iter->siov_len < iter->diov_len) {
		/* Advance only the source to the next element */
		iter->sidx++;
		if (iter->sidx == iter->siovcnt) {
			return len;
		}

		iter->diov_base += iter->siov_len;
		iter->diov_len -= iter->siov_len;
		iter->siov_len = iter->siov[iter->sidx].iov_len;
		iter->siov_base = iter->siov[iter->sidx].iov_base;
	} else {
		/* Advance only the destination to the next element */
		iter->didx++;
		if (iter->didx == iter->diovcnt) {
			return len;
		}

		iter->siov_base += iter->diov_len;
		iter->siov_len -= iter->diov_len;
		iter->diov_len = iter->diov[iter->didx].iov_len;
		iter->diov_base = iter->diov[iter->didx].iov_base;
	}

	return len;
}

size_t
spdk_iovcpy(struct iovec *siov, size_t siovcnt, struct iovec *diov, size_t diovcnt)
{
	struct spdk_ioviter iter;
	size_t len, total_sz;
	void *src, *dst;

	total_sz = 0;
	for (len = spdk_ioviter_first(&iter, siov, siovcnt, diov, diovcnt, &src, &dst);
	     len != 0;
	     len = spdk_ioviter_next(&iter, &src, &dst)) {
		memcpy(dst, src, len);
		total_sz += len;
	}

	return total_sz;
}
