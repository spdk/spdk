/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "util_internal.h"
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
	size_t count;
	const uint64_t *dword_buf;

	count = len & 7;
	while (count--) {
		crc = __crc32b(crc, *(const uint8_t *)buf);
		buf++;
	}
	dword_buf = (const uint64_t *)buf;

	count = len / 8;
	while (count--) {
		crc = __crc32d(crc, *dword_buf);
		dword_buf++;
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
