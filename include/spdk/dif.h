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

#define SPDK_DIF_GUARD_CHECK	0x1
#define SPDK_DIF_APPTAG_CHECK	0x2
#define SPDK_DIF_REFTAG_CHECK	0x4

#define SPDK_DIF_GUARD_ERROR	0x1
#define SPDK_DIF_APPTAG_ERROR	0x2
#define SPDK_DIF_REFTAG_ERROR	0x4
#define SPDK_DIF_DATA_ERROR	0x8

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
 * \param iovs A scatter gather list of buffers to be written from.
 * \param iovcnt The number of elements in iovs.
 * \param block_size The block size in a block.
 * \param md_size The metadata size in a block.
 * \param dif_start DIF is set in the first eight bytes of metadata if true,
 * or in the last eight bytes of metadata otherwise.
 * \param dif_type The type of DIF.
 * \param dif_flags The flag to specify the DIF action.
 * \param init_ref_tag Initial Reference Tag.
 * \param app_tag Application Tag.
 *
 * \return 0 on success and negated errno otherwise.
 */
int spdk_dif_generate(struct iovec *iovs, int iovcnt, uint32_t block_size,
		      uint32_t md_size, bool dif_start, enum spdk_dif_type dif_type,
		      uint32_t dif_flags, uint32_t init_ref_tag, uint16_t app_tag);

/**
 * Verify DIF for extended LBA payload.
 *
 * \param iovs A scatter gather list of buffers to be read to.
 * \param iovcnt The number of elements in iovs.
 * \param block_size The block size in a block.
 * \param md_size The metadata size in a block.
 * \param dif_start DIF is set in the first eight bytes of metadata if true,
 * or in the last eight bytes of metadata otherwise.
 * \param dif_type The type of DIF.
 * \param dif_flags The flag to specify the DIF action.
 * \param init_ref_tag Initial Reference Tag.
 * \param apptag_mask Application Tag Mask.
 * \param app_tag Application Tag.
 *
 * \return 0 on success and negated errno otherwise.
 */
int spdk_dif_verify(struct iovec *iovs, int iovcnt, uint32_t block_size,
		    uint32_t md_size, bool dif_start, enum spdk_dif_type dif_type,
		    uint32_t dif_flags, uint32_t init_ref_tag, uint16_t apptag_mask,
		    uint16_t app_tag);

/**
 * Copy data and generate DIF for extended LBA payload.
 *
 * \param bounce_buf A contiguous buffer forming extended LBA payload.
 * \param iovs A scatter gather list of buffers to be written from.
 * \param iovcnt The number of elements in iovs.
 * \param block_size The block size in a block.
 * \param md_size The metadata size in a block.
 * \param dif_start DIF is set in the first eight bytes of metadata if true,
 * or in the last eight bytes of metadata otherwise.
 * \param dif_type The type of DIF.
 * \param dif_flags The flag to specify the DIF action.
 * \param init_ref_tag Initial Reference Tag.
 * \param app_tag Application Tag.
 *
 * \return 0 on success and negated errno otherwise.
 */
int spdk_dif_generate_copy(void *bounce_buf, struct iovec *iovs, int iovcnt,
			   uint32_t block_size, uint32_t md_size,
			   bool dif_start, enum spdk_dif_type dif_type,
			   uint32_t dif_flags, uint32_t init_ref_tag, uint16_t app_tag);

/**
 * Verify DIF and copy data for extended LBA payload.
 *
 * \param iovs A scatter gather list of buffers to be read to.
 * \param iovcnt The number of elements in iovs.
 * \param bounce_buf A contiguous buffer forming extended LBA payload.
 * \param block_size The block size in a block.
 * \param md_size The metadata size in a block.
 * \param dif_start DIF is set in the first eight bytes of metadata if true,
 * or in the last eight bytes of metadata otherwise.
 * \param dif_type The type of DIF.
 * \param dif_flags The flag to specify the DIF action.
 * \param init_ref_tag Initial Reference Tag.
 * \param apptag_mask Application Tag Mask.
 * \param app_tag Application Tag.
 *
 * \return 0 on success and negated errno otherwise.
 */
int spdk_dif_verify_copy(struct iovec *iovs, int iovcnt, void *bounce_buf,
			 uint32_t block_size, uint32_t md_size, bool dif_start,
			 enum spdk_dif_type dif_type, uint32_t dif_flags,
			 uint32_t init_ref_tag, uint16_t apptag_mask, uint16_t app_tag);

/**
 * Inject bit flip error to extended LBA payload.
 *
 * \param iovs A scatter gather list of buffers to be read to.
 * \param iovcnt The number of elements in iovs.
 * \param block_size The block size in a block.
 * \param md_size The metadata size in a block.
 * \param dif_start The flag to specify the error injection action.
 * \param inject_flags The flag to specify the action of error injection.
 *
 * \return 0 on success and negated errno otherwise including no metadata.
 */
int spdk_dif_inject_error(struct iovec *iovs, int iovcnt, uint32_t block_size,
			  uint32_t md_size, bool dif_start, uint32_t inject_flags);
#endif /* SPDK_DIF_H */
