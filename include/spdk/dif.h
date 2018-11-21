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

#ifndef SPDK_T10DIF_H
#define SPDK_T10DIF_H

#include "spdk/stdinc.h"
#include "spdk/assert.h"

#define SPDK_T10DIF_GUARD_CHECK		0x1
#define SPDK_T10DIF_APPTAG_CHECK	0x2
#define SPDK_T10DIF_REFTAG_CHECK	0x4

struct spdk_t10dif {
	uint16_t guard;
	uint16_t app_tag;
	uint32_t ref_tag;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_t10dif) == 8, "Incorrect size");

/**
 * Generate T10 DIF in each extended logical block.
 *
 * Currently only T10 DIF Type 1 is suppported and T10 DIF is limited to append
 * to the first eight byte of the metadata.
 *
 * \param iovs A scatter gather list of buffers to be written from.
 * \param iovcnt The number of elements in iovs.
 * \param data_block_size The data block size in a block.
 * \param metadata_size The metadata size in a block.
 * \param dif_flags The flag to specify the T10 DIF action.
 * \param ref_tag Start reference tag.
 * \param app_tag Application tag.
 */
void spdk_t10dif_generate(struct iovec *iovs, int iovcnt,
			  uint32_t data_block_size, uint32_t metadata_size,
			  uint32_t dif_flags, uint32_t ref_tag, uint16_t app_tag);

/**
 * Verify T10 DIF in each extended logical block.
 *
 * Currently only T10 DIF Type 1 is suppported and T10 DIF is limited to append
 * to the first eight byte of the metadata.
 *
 * \param iovs A scatter gather list of buffers to be read to.
 * \param iovcnt The number of elements in iovs.
 * \param data_block_size The data block size in a block.
 * \param metadata_size The metadata size in a block.
 * \param dif_flags The flag to specify the T10 DIF action.
 * \param ref_tag Start reference tag.
 * \param apptag_mask Application tag mask.
 * \param app_tag Application tag.
 *
 * \return 0 on success and negated errno otherwise.
 */
int spdk_t10dif_verify(struct iovec *iovs, int iovcnt,
		       uint32_t data_block_size, uint32_t metadata_size,
		       uint32_t dif_flags, uint32_t ref_tag,
		       uint16_t apptag_mask, uint16_t app_tag);

/**
 * Copy each block data and append its T10 DIF to the bounce buffer.
 *
 * Currently only T10 DIF Type 1 is suppported and T10 DIF is limited to append
 * to the first eight byte of the metadata.
 *
 * \param bounce_buf A contiguous buffer forming extended logical block.
 * \param bounce_buf_len Size of the contiguous buffer.
 * \param iovs A scatter gather list of buffers to be written from.
 * \param iovcnt The number of elements in iovs.
 * \param data_block_size The data block size in a block.
 * \param metadata_size The metadata size in a block.
 * \param dif_flags The flag to specify the T10 DIF action.
 * \param ref_tag Start reference tag.
 * \param app_tag Application tag.
 *
 * \return 0 on success and negated error otherwise..
 */
int spdk_t10dif_generate_copy(void *bounce_buf, uint32_t bounce_buf_len,
			      struct iovec *iovs, int iovcnt,
			      uint32_t data_block_size, uint32_t metadata_size,
			      uint32_t dif_flags, uint32_t ref_tag, uint16_t app_tag);

/**
 * Verify T10 DIF and copy the corresponding block data to the original scatter-gather list.
 *
 * Currently only T10 DIF Type 1 is suppported and T10 DIF is limited to append
 * to the first eight byte of the metadata.
 *
 * \param iovs A scatter gather list of buffers to be read to.
 * \param iovcnt The number of elements in iovs.
 * \param bounce_buf A contiguous buffer forming extended logical block.
 * \param bounce_buf_len Size of the contiguous buffer.
 * \param data_block_size The data block size in a block.
 * \param metadata_size The metadata size in a block.
 * \param dif_flags The flag to specify the T10 DIF action.
 * \param ref_tag Start reference tag.
 * \param apptag_mask Application tag mask.
 * \param app_tag Application tag.
 *
 * \return 0 on success and negated errno otherwise.
 */
int spdk_t10dif_verify_copy(struct iovec *iovs, int iovcnt,
			    void *bounce_buf, uint32_t bounce_buf_len,
			    uint32_t data_block_size, uint32_t metadata_size,
			    uint32_t dif_flags, uint32_t ref_tag,
			    uint16_t apptag_mask, uint16_t app_tag);

/**
 * Generate and append T10 DIX to the separate metadata buffer for each logical block.
 *
 * Currently only T10 DIX Type 1 is suppported and T10 DIX is limited to append
 * to the first eight byte of the metadata.
 *
 * \param iovs A scatter gather list of buffers to be written from.
 * \param iovcnt The number of elements in iovs.
 * \param metadata_buf A contiguous buffer for metadata.
 * \param metadata_buf_len Size of the contiguous metadata buffer.
 * \param data_block_size The data block size in a block.
 * \param metadata_size The metadata size in a block.
 * \param dif_flags The flag to specify the T10 DIF action.
 * \param ref_tag Start reference tag.
 * \param app_tag Application tag.
 *
 * \return 0 on success and negated errno otherwise.
 */
int spdk_t10dix_generate(struct iovec *iovs, int iovcnt,
			 void *metadata_buf, uint32_t metadata_buf_len,
			 uint32_t data_block_size, uint32_t metadata_size,
			 uint32_t dif_flags, uint32_t ref_tag, uint16_t app_tag);

/**
 * Verify T10 DIX for each logical block..
 *
 * Currently only T10 DIX Type 1 is suppported and T10 DIX is limited to append
 * to the first eight byte of the metadata.
 *
 * \param iovs A scatter gather list of buffers to be read to.
 * \param iovcnt The number of elements in iovs.
 * \param data_block_size The data block size in a block.
 * \param metadata_size The metadata size in a block.
 * \param dif_flags The flag to specify the T10 DIF action.
 * \param ref_tag Start reference tag.
 * \param apptag_mask Application tag mask.
 * \param app_tag Application tag.
 *
 * \return 0 on success and negated errno otherwise.
 */
int spdk_t10dix_verify(struct iovec *iovs, int iovcnt,
		       void *metadata_buf, uint32_t metadata_buf_len,
		       uint32_t data_block_size, uint32_t metadata_size,
		       uint32_t dif_flags, uint32_t ref_tag,
		       uint16_t apptag_mask, uint16_t app_tag);

/**
 * Inject bit flip error to data or T10DIF in the extended logical block payload.
 *
 * Currently only T10 DIX Type 1 is suppported and T10 DIX is limited to append
 * to the first eight byte of the metadata.
 *
 * \param iovs A scatter gather list of buffers to be read to.
 * \param iovcnt The number of elements in iovs.
 * \param data_block_size The data block size in a block.
 * \param metadata_size The metadata size in a block.
 * \param inject_flags The flag to specify the action of error injection.
 *
 * \return 0 on success and negated errno otherwise.
 */
int spdk_t10dif_inject_error(struct iovec *iovs, int iovcnt,
			     uint32_t data_block_size, uint32_t metadata_size,
			     uint32_t inject_flags);
#endif /* SPDK_T10DIF_H */
