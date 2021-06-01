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

#ifndef __SGL_INTERNAL_H__
#define __SGL_INTERNAL_H__

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_iov_sgl {
	struct iovec    *iov;
	int             iovcnt;
	uint32_t        iov_offset;
	uint32_t        total_size;
};

/**
 * Initialize struct spdk_iov_sgl with iov, iovcnt and iov_offset.
 *
 * \param s the spdk_iov_sgl to be filled.
 * \param iov the io vector to fill the s
 * \param iovcnt the size the iov
 * \param iov_offset the current filled iov_offset for s.
 */

static inline void
spdk_iov_sgl_init(struct spdk_iov_sgl *s, struct iovec *iov, int iovcnt,
		  uint32_t iov_offset)
{
	s->iov = iov;
	s->iovcnt = iovcnt;
	s->iov_offset = iov_offset;
	s->total_size = 0;
}

/**
 * Consume the iovs in spdk_iov_sgl with passed bytes
 *
 * \param s the spdk_iov_sgl which contains the iov
 * \param step the bytes_size consumed.
 */

static inline void
spdk_iov_sgl_advance(struct spdk_iov_sgl *s, uint32_t step)
{
	s->iov_offset += step;
	while (s->iovcnt > 0) {
		assert(s->iov != NULL);
		if (s->iov_offset < s->iov->iov_len) {
			break;
		}

		s->iov_offset -= s->iov->iov_len;
		s->iov++;
		s->iovcnt--;
	}
}

/**
 * Append the data to the struct spdk_iov_sgl pointed by s
 *
 * \param s the address of the struct spdk_iov_sgl
 * \param data the data buffer to be appended
 * \param data_len the length of the data.
 *
 * \return true if all the data is appended.
 */

static inline bool
spdk_iov_sgl_append(struct spdk_iov_sgl *s, uint8_t *data, uint32_t data_len)
{
	if (s->iov_offset >= data_len) {
		s->iov_offset -= data_len;
	} else {
		assert(s->iovcnt > 0);
		s->iov->iov_base = data + s->iov_offset;
		s->iov->iov_len = data_len - s->iov_offset;
		s->total_size += data_len - s->iov_offset;
		s->iov_offset = 0;
		s->iov++;
		s->iovcnt--;
		if (s->iovcnt == 0) {
			return false;
		}
	}

	return true;
}

#ifdef __cplusplus
}
#endif

#endif /* __SGL_INTERNAL_H__ */
