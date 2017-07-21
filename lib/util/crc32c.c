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

#include "spdk/crc32.h"

#include "spdk_internal/assert.h"

#if defined(__x86_64__) && defined(__SSE4_2__)
#include <x86intrin.h>

uint32_t
spdk_crc32c_update(const void *buf, size_t len, uint32_t crc)
{
	uint64_t crc_tmp64;
	size_t count;

#define CRC32(bits, tmp) \
	do { \
		uint ## bits ## _t v; \
		/* Use memcpy() to avoid unaligned loads, which are undefined behavior in C. */ \
		/* The compiler will optimize out the memcpy() in release builds. */ \
		memcpy(&v, buf, sizeof(v)); \
		buf += sizeof(v); \
		tmp = _mm_crc32_u ## bits (tmp, v); \
	} while (0)

	/* _mm_crc32_u64() needs a 64-bit intermediate value */
	crc_tmp64 = crc;

	/* Process as much of the buffer as possible in 64-bit blocks. */
	count = len / 8;
	while (count--) {
		CRC32(64, crc_tmp64);
	}
	crc = (uint32_t)crc_tmp64;

	/* Handle any trailing bytes. */
	switch (len & 7) {
	case 0:
		return crc;
	case 1:
		CRC32(8, crc);
		return crc;
	case 2:
		CRC32(16, crc);
		return crc;
	case 3:
		CRC32(16, crc);
		CRC32(8, crc);
		return crc;
	case 4:
		CRC32(32, crc);
		return crc;
	case 5:
		CRC32(32, crc);
		CRC32(8, crc);
		return crc;
	case 6:
		CRC32(32, crc);
		CRC32(16, crc);
		return crc;
	case 7:
		CRC32(32, crc);
		CRC32(16, crc);
		CRC32(8, crc);
		return crc;
	}

	SPDK_UNREACHABLE();
}

#else /* SSE 4.2 (CRC32 instruction) not available */

static struct spdk_crc32_table g_crc32c_table;

__attribute__((constructor)) static void
spdk_crc32c_init(void)
{
	spdk_crc32_table_init(&g_crc32c_table, SPDK_CRC32C_POLYNOMIAL_REFLECT);
}

uint32_t
spdk_crc32c_update(const void *buf, size_t len, uint32_t crc)
{
	return spdk_crc32_update(&g_crc32c_table, buf, len, crc);
}

#endif
