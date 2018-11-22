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

#include "spdk/stdinc.h"
#include "spdk/likely.h"
#include "spdk/util.h"

#include "spdk/iovec.h"

size_t
spdk_iovec_copy_buf(struct iovec *iovs, int iovcnt, void *buf, size_t buf_len,
		    bool to_buf)
{
	int i = 0;
	size_t copied = 0, len;

	while (i < iovcnt && copied < buf_len) {
		len = spdk_min(iovs[i].iov_len, buf_len - copied);

		if (to_buf) {
			memcpy(buf + copied, iovs[i].iov_base, len);
		} else {
			memcpy(iovs[i].iov_base, buf + copied, len);
		}
		copied += len;
	}

	return copied;
}

bool
spdk_iovec_is_aligned(struct iovec *iovs, int iovcnt, uint32_t alignment)
{
	int i;
	uintptr_t iov_base;

	if (spdk_likely(alignment == 1)) {
		return true;
	}

	for (i = 0; i < iovcnt; i++) {
		iov_base = (uintptr_t)iovs[i].iov_base;
		if ((iov_base & (alignment - 1)) != 0) {
			return false;
		}
	}

	return true;
}

bool
spdk_iovec_has_granularity(struct iovec *iovs, int iovcnt, uint32_t granularity)
{
	int i;

	for (i = 0; i < iovcnt; i++) {
		if (iovs[i].iov_len % granularity) {
			return false;
		}
	}

	return true;
}
