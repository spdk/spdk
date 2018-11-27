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

#ifndef SPDK_DIF_H
#define SPDK_DIF_H

#include "spdk/stdinc.h"
#include "spdk/assert.h"

#define SPDK_DIF_REFTAG_CHECK	(1U << 26)
#define SPDK_DIF_APPTAG_CHECK	(1U << 27)
#define SPDK_DIF_GUARD_CHECK	(1U << 28)

enum spdk_dif_type {
	SPDK_DIF_TYPE1 = 1,
	SPDK_DIF_TYPE2,
	SPDK_DIF_TYPE3,
};

struct spdk_dif {
	uint16_t guard;
	uint16_t app_tag;
	uint32_t ref_tag;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_dif) == 8, "Incorrect size");

/**
 * Generate DIF for extended LBA payload.
 *
 * \param iovs iovec array describing the extended LBA payload.
 * \param iovcnt Number of elements in the iovec array.
 * \param block_size Block size in a block.
 * \param md_size Metadata size in a block.
 * \param num_blocks Number of blocks of the payload.
 * \param dif_loc DIF location. If true, DIF is set in the last 8 bytes of metadata.
 * If false, DIF is in the first 8 bytes of metadata.
 * \param dif_type Type of DIF.
 * \param dif_flags Flag to specify the DIF action.
 * \param init_ref_tag Initial Reference Tag. For type 1, this is the
 * starting block address.
 * \param app_tag Application Tag.
 *
 * \return 0 on success and negated errno otherwise.
 */
int spdk_dif_generate(struct iovec *iovs, int iovcnt,
		      uint32_t block_size, uint32_t md_size, uint32_t num_blocks,
		      bool dif_loc, enum spdk_dif_type dif_type, uint32_t dif_flags,
		      uint32_t init_ref_tag, uint16_t app_tag);

/**
 * Verify DIF for extended LBA payload.
 *
 * \param iovs iovec array describing the extended LBA payload.
 * \param iovcnt Number of elements in the iovec array.
 * \param block_size Block size in a block.
 * \param md_size Metadata size in a block.
 * \param num_blocks Number of blocks of the payload.
 * \param dif_loc DIF location. If true, DIF is set in the last 8 bytes of metadata.
 * If false, DIF is set in the first 8 bytes of metadata.
 * \param dif_type Type of DIF.
 * \param dif_flags Flag to specify the DIF action.
 * \param init_ref_tag Initial Reference Tag. For type 1, this is the
 * starting block address.
 * \param apptag_mask Application Tag Mask.
 * \param app_tag Application Tag.
 *
 * \return 0 on success and negated errno otherwise.
 */
int spdk_dif_verify(struct iovec *iovs, int iovcnt,
		    uint32_t block_size, uint32_t md_size, uint32_t num_blocks,
		    bool dif_loc, enum spdk_dif_type dif_type, uint32_t dif_flags,
		    uint32_t init_ref_tag, uint16_t apptag_mask, uint16_t app_tag);
#endif /* SPDK_DIF_H */
