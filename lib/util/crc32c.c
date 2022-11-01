/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "util_internal.h"
#include "spdk/crc32.h"

#ifdef SPDK_CONFIG_ISAL
#define SPDK_HAVE_ISAL
#include <isa-l/include/crc.h>
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#define SPDK_HAVE_ARM_CRC
#include <arm_acle.h>
#elif defined(__x86_64__) && defined(__SSE4_2__)
#define SPDK_HAVE_SSE4_2
#include <x86intrin.h>
#endif

#ifdef SPDK_HAVE_ISAL

uint32_t
spdk_crc32c_update(const void *buf, size_t len, uint32_t crc)
{
	return crc32_iscsi((unsigned char *)buf, len, crc);
}

#elif defined(SPDK_HAVE_SSE4_2)

uint32_t
spdk_crc32c_update(const void *buf, size_t len, uint32_t crc)
{
	uint64_t crc_tmp64;
	size_t count;

	/* _mm_crc32_u64() needs a 64-bit intermediate value */
	crc_tmp64 = crc;

	/* Process as much of the buffer as possible in 64-bit blocks. */
	count = len / 8;
	while (count--) {
		uint64_t block;

		/*
		 * Use memcpy() to avoid unaligned loads, which are undefined behavior in C.
		 * The compiler will optimize out the memcpy() in release builds.
		 */
		memcpy(&block, buf, sizeof(block));
		crc_tmp64 = _mm_crc32_u64(crc_tmp64, block);
		buf += sizeof(block);
	}
	crc = (uint32_t)crc_tmp64;

	/* Handle any trailing bytes. */
	count = len & 7;
	while (count--) {
		crc = _mm_crc32_u8(crc, *(const uint8_t *)buf);
		buf++;
	}

	return crc;
}

#elif defined(SPDK_HAVE_ARM_CRC)

uint32_t
spdk_crc32c_update(const void *buf, size_t len, uint32_t crc)
{
	size_t count;

	count = len / 8;
	while (count--) {
		uint64_t block;

		memcpy(&block, buf, sizeof(block));
		crc = __crc32cd(crc, block);
		buf += sizeof(block);
	}

	count = len & 7;
	while (count--) {
		crc = __crc32cb(crc, *(const uint8_t *)buf);
		buf++;
	}

	return crc;
}

#else /* Neither SSE 4.2 nor ARM CRC32 instructions available */

static struct spdk_crc32_table g_crc32c_table;

__attribute__((constructor)) static void
crc32c_init(void)
{
	crc32_table_init(&g_crc32c_table, SPDK_CRC32C_POLYNOMIAL_REFLECT);
}

uint32_t
spdk_crc32c_update(const void *buf, size_t len, uint32_t crc)
{
	return crc32_update(&g_crc32c_table, buf, len, crc);
}

#endif

uint32_t
spdk_crc32c_iov_update(struct iovec *iov, int iovcnt, uint32_t crc32c)
{
	int i;

	if (iov == NULL) {
		return crc32c;
	}

	for (i = 0; i < iovcnt; i++) {
		assert(iov[i].iov_base != NULL);
		assert(iov[i].iov_len != 0);
		crc32c = spdk_crc32c_update(iov[i].iov_base, iov[i].iov_len, crc32c);
	}

	return crc32c;
}
