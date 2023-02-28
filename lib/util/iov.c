/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/util.h"

void
spdk_iov_memset(struct iovec *iovs, int iovcnt, int c)
{
	int iov_idx = 0;
	struct iovec *iov;

	while (iov_idx < iovcnt) {
		iov = &iovs[iov_idx];
		memset(iov->iov_base, c, iov->iov_len);
		iov_idx++;
	}
}

void
spdk_iov_one(struct iovec *iov, int *iovcnt, void *buf, size_t buflen)
{
	iov->iov_base = buf;
	iov->iov_len = buflen;
	*iovcnt = 1;
}

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

size_t
spdk_iovmove(struct iovec *siov, size_t siovcnt, struct iovec *diov, size_t diovcnt)
{
	struct spdk_ioviter iter;
	size_t len, total_sz;
	void *src, *dst;

	total_sz = 0;
	for (len = spdk_ioviter_first(&iter, siov, siovcnt, diov, diovcnt, &src, &dst);
	     len != 0;
	     len = spdk_ioviter_next(&iter, &src, &dst)) {
		memmove(dst, src, len);
		total_sz += len;
	}

	return total_sz;
}

void
spdk_iov_xfer_init(struct spdk_iov_xfer *ix, struct iovec *iovs, int iovcnt)
{
	ix->iovs = iovs;
	ix->iovcnt = iovcnt;
	ix->cur_iov_idx = 0;
	ix->cur_iov_offset = 0;
}

static size_t
iov_xfer(struct spdk_iov_xfer *ix, const void *buf, size_t buf_len, bool to_buf)
{
	size_t len, iov_remain_len, copied_len = 0;
	struct iovec *iov;

	if (buf_len == 0) {
		return 0;
	}

	while (ix->cur_iov_idx < ix->iovcnt) {
		iov = &ix->iovs[ix->cur_iov_idx];
		iov_remain_len = iov->iov_len - ix->cur_iov_offset;
		if (iov_remain_len == 0) {
			ix->cur_iov_idx++;
			ix->cur_iov_offset = 0;
			continue;
		}

		len = spdk_min(iov_remain_len, buf_len - copied_len);

		if (to_buf) {
			memcpy((char *)buf + copied_len,
			       iov->iov_base + ix->cur_iov_offset, len);
		} else {
			memcpy((char *)iov->iov_base + ix->cur_iov_offset,
			       (const char *)buf + copied_len, len);
		}
		copied_len += len;
		ix->cur_iov_offset += len;

		if (buf_len == copied_len) {
			return copied_len;
		}
	}

	return copied_len;
}

size_t
spdk_iov_xfer_from_buf(struct spdk_iov_xfer *ix, const void *buf, size_t buf_len)
{
	return iov_xfer(ix, buf, buf_len, false);
}

size_t
spdk_iov_xfer_to_buf(struct spdk_iov_xfer *ix, const void *buf, size_t buf_len)
{
	return iov_xfer(ix, buf, buf_len, true);
}

void
spdk_copy_iovs_to_buf(void *buf, size_t buf_len, struct iovec *iovs, int iovcnt)
{
	struct spdk_iov_xfer ix;

	spdk_iov_xfer_init(&ix, iovs, iovcnt);
	spdk_iov_xfer_to_buf(&ix, buf, buf_len);
}

void
spdk_copy_buf_to_iovs(struct iovec *iovs, int iovcnt, void *buf, size_t buf_len)
{
	struct spdk_iov_xfer ix;

	spdk_iov_xfer_init(&ix, iovs, iovcnt);
	spdk_iov_xfer_from_buf(&ix, buf, buf_len);
}
