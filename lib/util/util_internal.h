/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_UTIL_INTERNAL_H
#define SPDK_UTIL_INTERNAL_H

#include "spdk/stdinc.h"

/**
 * IEEE CRC-32 polynomial (bit reflected)
 */
#define SPDK_CRC32_POLYNOMIAL_REFLECT 0xedb88320UL

/**
 * CRC-32C (Castagnoli) polynomial (bit reflected)
 */
#define SPDK_CRC32C_POLYNOMIAL_REFLECT 0x82f63b78UL

struct spdk_crc32_table {
	uint32_t table[256];
};

/**
 * Initialize a CRC32 lookup table for a given polynomial.
 *
 * \param table Table to fill with precalculated CRC-32 data.
 * \param polynomial_reflect Bit-reflected CRC-32 polynomial.
 */
void crc32_table_init(struct spdk_crc32_table *table,
		      uint32_t polynomial_reflect);


/**
 * Calculate a partial CRC-32 checksum.
 *
 * \param table CRC-32 table initialized with crc32_table_init().
 * \param buf Data buffer to checksum.
 * \param len Length of buf in bytes.
 * \param crc Previous CRC-32 value.
 * \return Updated CRC-32 value.
 */
uint32_t crc32_update(const struct spdk_crc32_table *table,
		      const void *buf, size_t len,
		      uint32_t crc);

#endif /* SPDK_UTIL_INTERNAL_H */
