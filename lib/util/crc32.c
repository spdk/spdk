/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "util_internal.h"
#include "crc_internal.h"
#include "spdk/crc32.h"

void
crc32_table_init(struct spdk_crc32_table *table, uint32_t polynomial_reflect)
{
	int i, j;
	uint32_t val;

	for (i = 0; i < 256; i++) {
		val = i;
		for (j = 0; j < 8; j++) {
			if (val & 1) {
				val = (val >> 1) ^ polynomial_reflect;
			} else {
				val = (val >> 1);
			}
		}
		table->table[i] = val;
	}
}

#ifdef SPDK_HAVE_ARM_CRC

uint32_t
crc32_update(const struct spdk_crc32_table *table, const void *buf, size_t len, uint32_t crc)
{
	size_t count_pre, count_post, count_mid;
	const uint64_t *dword_buf;

	/* process the head and tail bytes seperately to make the buf address
	 * passed to crc32_d is 8 byte aligned. This can avoid unaligned loads.
	 */
	count_pre = ((uint64_t)buf & 7) == 0 ? 0 : 8 - ((uint64_t)buf & 7);
	count_post = (uint64_t)(buf + len) & 7;
	count_mid = (len - count_pre - count_post) / 8;

	while (count_pre--) {
		crc = __crc32b(crc, *(const uint8_t *)buf);
		buf++;
	}

	dword_buf = (const uint64_t *)buf;
	while (count_mid--) {
		crc = __crc32d(crc, *dword_buf);
		dword_buf++;
	}

	buf = dword_buf;
	while (count_post--) {
		crc = __crc32b(crc, *(const uint8_t *)buf);
		buf++;
	}

	return crc;
}

#else

uint32_t
crc32_update(const struct spdk_crc32_table *table, const void *buf, size_t len, uint32_t crc)
{
	const uint8_t *buf_u8 = buf;
	size_t i;

	for (i = 0; i < len; i++) {
		crc = (crc >> 8) ^ table->table[(crc ^ buf_u8[i]) & 0xff];
	}

	return crc;
}

#endif
