/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/util.h"
#include "spdk/log.h"

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

SPDK_LOG_DEPRECATION_REGISTER(spdk_iov_one, "spdk_iov_one", "v24.05", 0);

void
spdk_iov_one(struct iovec *iov, int *iovcnt, void *buf, size_t buflen)
{
	SPDK_LOG_DEPRECATED(spdk_iov_one);

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
	struct iovec *iovs[2];
	size_t iovcnts[2];
	void *out[2];
	size_t len;

	iovs[0] = siov;
	iovcnts[0] = siovcnt;

	iovs[1] = diov;
	iovcnts[1] = diovcnt;

	len = spdk_ioviter_firstv(iter, 2, iovs, iovcnts, out);

	if (len > 0) {
		*src = out[0];
		*dst = out[1];
	}

	return len;
}

size_t
spdk_ioviter_firstv(struct spdk_ioviter *iter,
		    uint32_t count,
		    struct iovec **iov,
		    size_t *iovcnt,
		    void **out)
{
	struct spdk_single_ioviter *it;
	uint32_t i;

	iter->count = count;

	for (i = 0; i < count; i++) {
		it = &iter->iters[i];
		it->iov = iov[i];
		it->iovcnt = iovcnt[i];
		it->idx = 0;
		it->iov_len = iov[i][0].iov_len;
		it->iov_base = iov[i][0].iov_base;
	}

	return spdk_ioviter_nextv(iter, out);
}

size_t
spdk_ioviter_next(struct spdk_ioviter *iter, void **src, void **dst)
{
	void *out[2];
	size_t len;

	len = spdk_ioviter_nextv(iter, out);

	if (len > 0) {
		*src = out[0];
		*dst = out[1];
	}

	return len;
}

size_t
spdk_ioviter_nextv(struct spdk_ioviter *iter, void **out)
{
	struct spdk_single_ioviter *it;
	size_t len;
	uint32_t i;

	/* Figure out the minimum size of each iovec's next segment */
	len = UINT32_MAX;
	for (i = 0; i < iter->count; i++) {
		it = &iter->iters[i];
		if (it->idx == it->iovcnt || it->iov_len == 0) {
			/* This element has 0 bytes remaining, so we're done. */
			return 0;
		}

		len = spdk_min(len, it->iov_len);
	}

	for (i = 0; i < iter->count; i++) {
		it = &iter->iters[i];

		out[i] = it->iov_base;

		if (it->iov_len == len) {
			/* Advance to next element */
			it->idx++;
			if (it->idx != it->iovcnt) {
				/* Set up for next element */
				it->iov_len = it->iov[it->idx].iov_len;
				it->iov_base = it->iov[it->idx].iov_base;
			}
		} else {
			/* Partial buffer */
			it->iov_base += len;
			it->iov_len -= len;
		}
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
			       (char *)iov->iov_base + ix->cur_iov_offset, len);
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
