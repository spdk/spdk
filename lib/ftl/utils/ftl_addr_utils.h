/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_ADDR_UTILS_H
#define FTL_ADDR_UTILS_H

#include "ftl_core.h"

static inline ftl_addr
ftl_addr_load(struct spdk_ftl_dev *dev, void *buffer, uint64_t offset)
{
	if (ftl_addr_packed(dev)) {
		uint32_t *b32 = buffer;
		ftl_addr addr = b32[offset];

		if (addr == (uint32_t)FTL_ADDR_INVALID) {
			return FTL_ADDR_INVALID;
		} else {
			return addr;
		}
	} else {
		uint64_t *b64 = buffer;
		return b64[offset];
	}
}

static inline void
ftl_addr_store(struct spdk_ftl_dev *dev, void *buffer, uint64_t offset, ftl_addr addr)
{
	if (ftl_addr_packed(dev)) {
		uint32_t *b32 = buffer;
		b32[offset] = addr;
	} else {
		uint64_t *b64 = buffer;
		b64[offset] = addr;
	}
}

static inline uint64_t
ftl_lba_load(struct spdk_ftl_dev *dev, void *buffer, uint64_t offset)
{
	if (ftl_addr_packed(dev)) {
		uint32_t *b32 = buffer;
		uint32_t lba = b32[offset];

		if (lba == (uint32_t)FTL_LBA_INVALID) {
			return FTL_LBA_INVALID;
		} else {
			return lba;
		}
	} else {
		uint64_t *b64 = buffer;
		return b64[offset];
	}
}

static inline void
ftl_lba_store(struct spdk_ftl_dev *dev, void *buffer, uint64_t offset, uint64_t lba)
{
	if (ftl_addr_packed(dev)) {
		uint32_t *b32 = buffer;
		b32[offset] = lba;
	} else {
		uint64_t *b64 = buffer;
		b64[offset] = lba;
	}
}

#endif /* FTL_ADDR_UTILS_H */
