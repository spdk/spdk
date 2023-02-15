/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "util_internal.h"
#include "crc_internal.h"
#include "spdk/crc32.h"

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
	size_t count_pre, count_post, count_mid;
	const uint64_t *dword_buf;
	uint64_t crc_tmp64;

	/* process the head and tail bytes seperately to make the buf address
	 * passed to _mm_crc32_u64 is 8 byte aligned. This can avoid unaligned loads.
	 */
	count_pre = ((uint64_t)buf & 7) == 0 ? 0 : 8 - ((uint64_t)buf & 7);
	count_post = (uint64_t)(buf + len) & 7;
	count_mid = (len - count_pre - count_post) / 8;

	while (count_pre--) {
		crc = _mm_crc32_u8(crc, *(const uint8_t *)buf);
		buf++;
	}

	/* _mm_crc32_u64() needs a 64-bit intermediate value */
	crc_tmp64 = crc;
	dword_buf = (const uint64_t *)buf;

	while (count_mid--) {
		crc_tmp64 = _mm_crc32_u64(crc_tmp64, *dword_buf);
		dword_buf++;
	}

	buf = dword_buf;
	crc = (uint32_t)crc_tmp64;
	while (count_post--) {
		crc = _mm_crc32_u8(crc, *(const uint8_t *)buf);
		buf++;
	}

	return crc;
}

#elif defined(SPDK_HAVE_ARM_CRC)

uint32_t
spdk_crc32c_update(const void *buf, size_t len, uint32_t crc)
{
	size_t count_pre, count_post, count_mid;
	const uint64_t *dword_buf;

	/* process the head and tail bytes seperately to make the buf address
	 * passed to crc32_cd is 8 byte aligned. This can avoid unaligned loads.
	 */
	count_pre = ((uint64_t)buf & 7) == 0 ? 0 : 8 - ((uint64_t)buf & 7);
	count_post = (uint64_t)(buf + len) & 7;
	count_mid = (len - count_pre - count_post) / 8;

	while (count_pre--) {
		crc = __crc32cb(crc, *(const uint8_t *)buf);
		buf++;
	}

	dword_buf = (const uint64_t *)buf;
	while (count_mid--) {
		crc = __crc32cd(crc, *dword_buf);
		dword_buf++;
	}

	buf = dword_buf;
	while (count_post--) {
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
