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
