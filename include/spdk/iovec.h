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

#ifndef SPDK_IOVEC_H
#define SPDK_IOVEC_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Copy data between a scatter gather list and a contiguous buffer.
 *
 * \param iovs A scatter gather list of buffers to be read into or written from.
 * \params iovcnt The number of elements in iov.
 * \params buf A contiguous buffer to be read into or written from.
 * \params buf_len Size of the contiguous buffer.
 * \params to_buf Copy direction. true if from iovs to buf or false otherwise.
 *
 * \return Copied bytes.
 */
size_t spdk_iovec_copy_buf(struct iovec *iovs, int iovcnt, void *buf,
			   size_t buf_len, bool to_buf);

/**
 * Check if buffer is allocated to the scatter gather list.
 *
 * \param iovs A scatter gather list to be checked.
 *
 * \return true if buffer is allocated or false otherwise.
 */
static inline bool
spdk_iovec_buf_is_allocated(struct iovec *iovs)
{
	return iovs[0].iov_base != NULL;
}

/**
 * Check if each buffer of a scatter gather list is aligned to the required size.
 *
 * \param iovs A scatter gather list to be checked.
 * \param iovcnt The number of elements in iov.
 * \param alignment Required alignment in bytes.
 *
 * \return true if aligned or false otherwise.
 */
bool spdk_iovec_is_aligned(struct iovec *iovs, int iovcnt, uint32_t alignment);

/**
 * Check if the size of each buffer of a scatter gather list has required
 * granularity..
 *
 * \param iovs A scatter gather list to be checked.
 * \param iovcnt The number of elements in iov.
 * \param alignment Required granularity in bytes.
 *
 * \return true if having required granularity or false otherwise.
 */
bool spdk_iovec_has_granularity(struct iovec *iovs, int iovcnt,
				uint32_t granularity);

/*
 * Contest to iterate a scatter gather list.
 */
struct spdk_iovec_iter {
	/* Current iovec in the iteration */
	struct iovec *iov;

	/* Remaining count of iovecs in the iteration. */
	int iovcnt;

	/* Current offset in the iovec */
	uint32_t iov_offset;
};

#define spdk_iovec_iter_init(i, iovs, iovcnt) {	\
	i.iov = iovs;				\
	i.iovcnt = iovcnt;			\
	i.iov_offset = 0;			\
}

#define spdk_iovec_iter_cont(i)			\
	while (i.iovcnt != 0)

#define spdk_iovec_iter_advance(i, step) {	\
	i.iov_offset += step;			\
	if (i.iov_offset == i.iov->iov_len) {	\
		i.iov++;			\
		i.iovcnt--;			\
		i.iov_offset = 0;		\
	}					\
}

#define spdk_iovec_iter_get_buf(i, buf, buf_len) {	\
	buf = i.iov->iov_base + i.iov_offset;		\
	buf_len = i.iov->iov_len - i.iov_offset;	\
}

#ifdef __cplusplus
}
#endif

#endif /* SPDK_IOVEC_H */
