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
