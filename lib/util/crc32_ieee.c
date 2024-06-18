/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "util_internal.h"
#include "spdk/crc32.h"

static struct spdk_crc32_table g_crc32_ieee_table;

__attribute__((constructor)) static void
crc32_ieee_init(void)
{
	crc32_table_init(&g_crc32_ieee_table, SPDK_CRC32_POLYNOMIAL_REFLECT);
}

uint32_t
spdk_crc32_ieee_update(const void *buf, size_t len, uint32_t crc)
{
	return crc32_update(&g_crc32_ieee_table, buf, len, crc);
}
