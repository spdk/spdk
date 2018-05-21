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

/**
 * \file
 * LNVM specification definitions
 */

#ifndef SPDK_LNVM_SPEC_H
#define SPDK_LNVM_SPEC_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/assert.h"

struct __attribute__((packed)) spdk_lnvm_dev_lba_fmt {
	uint8_t grp_len;
	uint8_t pu_len;
	uint8_t chk_len;
	uint8_t lbk_len;
	uint8_t res[4];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_lnvm_dev_lba_fmt) == 8, "Incorrect size");

/* Shall be aligned to 4096B when sent to device (spdk/dpdk limitation on freeing dma buffer) */
struct __attribute__((packed)) spdk_lnvm_geometry_data {
	/** Major Version Number */
	uint8_t		mjr;

	/** Minor Version Number */
	uint8_t		mnr;

	uint8_t		reserved1[6];

	/** LBA format */
	struct spdk_lnvm_dev_lba_fmt	lbaf;

	/** Media and Controller Capabilities */
	uint32_t	mccap;

	uint8_t		reserved2[12];

	/** Wear-level Index Delta Threshold */
	uint8_t		wit;

	uint8_t		reserved3[31];

	/** Number of Groups */
	uint16_t	num_grp;

	/** Number of parallel units per group */
	uint16_t	num_pu;

	/** Number of chunks per parallel unit */
	uint32_t	num_chk;

	/** Chunk Size */
	uint32_t	clba;

	uint8_t		reserved4[52];

	/** Minimum Write Size */
	uint32_t	ws_min;

	/** Optimal Write Size */
	uint32_t	ws_opt;

	/** Cache Minimum Write Size Units */
	uint32_t	mw_cunits;

	/** Maximum Open Chunks */
	uint32_t	maxoc;

	/** Maximum Open Chunks per PU */
	uint32_t	maxocpu;

	uint8_t		reserved5[44];

	/** tRD Typical */
	uint32_t	trdt;

	/** tRD Max */
	uint32_t	trdm;

	/** tWR Typical */
	uint32_t	twrt;

	/** tWR Max */
	uint32_t	twrm;

	/** tCRS Typical */
	uint32_t	tcrst;

	/** tCRS Max */
	uint32_t	tcrsm;

	uint8_t		reserved6[40];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_lnvm_geometry_data) == 256, "Incorrect size");

struct __attribute__((packed)) spdk_lnvm_chunk_information {
	/** Chunk State */
	struct {
		/** if set to 1 chunk is free */
		uint8_t free		: 1;

		/** if set to 1 chunk is closed */
		uint8_t closed		: 1;

		/** if set to 1 chunk is open */
		uint8_t open		: 1;

		/** if set to 1 chunk is offline */
		uint8_t offline		: 1;

		uint8_t reserved	: 4;
	} cs;

	/** Chunk Type */
	struct {
		/** If set to 1 chunk must be written sequentially */
		uint8_t seq_write		: 1;

		/** If set to 1 chunk allows random writes */
		uint8_t rnd_write		: 1;

		uint8_t reserved1		: 2;

		/**
		 * If set to 1 chunk deviates from the chunk size reported
		 * in identify geometry command.
		 */
		uint8_t d_size			: 1;

		uint8_t reserved2		: 3;
	} ct;

	/** Wear-level Index */
	uint8_t wli;

	uint8_t rsvd[5];

	/** Starting LBA */
	uint64_t slba;

	/** Number of blocks in chunk */
	uint64_t cnlb;

	/** Write Pointer */
	uint64_t wp;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_lnvm_chunk_information) == 32, "Incorrect size");

/**
 * LNVM admin command set opcodes
 */
enum spdk_lnvm_admin_opcode {
	SPDK_LNVM_OPC_GEOMETRY	= 0xE2
};

/**
 * LNVM command set opcodes
 */
enum spdk_lnvm_nvm_opcode {
	SPDK_LNVM_OPC_VECTOR_RESET	= 0x90,
	SPDK_LNVM_OPC_VECTOR_WRITE	= 0x91,
	SPDK_LNVM_OPC_VECTOR_READ	= 0x92,
	SPDK_LNVM_OPC_VECTOR_COPY	= 0x93
};

/**
 * Log page identifiers for SPDK_NVME_OPC_GET_LOG_PAGE
 */
enum spdk_lnvm_log_page {
	/** Chunk Information */
	SPDK_LNVM_LOG_CHUNK_INFO	= 0xCA,
};

/**
 * LNVM feature identifiers
 * Defines OCSSD specific features that may be configured with Set Features and
 * retrieved with Get Features.
 */
enum spdk_lnvm_feat {
	/**  Media Feedback feature identifier */
	SPDK_LNVM_FEAT_MEDIA_FEEDBACK	= 0xCA
};

#define SPDK_LNVM_IO_FLAGS_LIMITED_RETRY (1U << 31)

#ifdef __cplusplus
}
#endif

#endif
