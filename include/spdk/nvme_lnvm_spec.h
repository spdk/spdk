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

/** \file
 * LightNVM specification definitions
 */

#ifndef SPDK_NVME_LNVM_SPEC_H
#define SPDK_NVME_LNVM_SPEC_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/assert.h"

enum spdk_nvme_lnvm_spec_mjr {
	OCSSD_SPEC_1_2 = 1,
	OCSSD_SPEC_2 = 2,
};

struct __attribute__((packed)) spdk_nvme_lnvm_lbaf {
	uint8_t grp_bit_len;
	uint8_t pu_bit_len;
	uint8_t chk_bit_len;
	uint8_t lbk_bit_len;
	uint8_t reserved[4];
};

struct __attribute__((packed)) spdk_nvme_lnvm_geometry_data {
	/** Major Version Number */
	uint8_t		mjr;

	/** Minor Version Number */
	uint8_t		mnr;

	uint8_t		reserved1[6];

	/** LBA format */
	struct spdk_nvme_lnvm_lbaf	lbaf;

	/** Media and Controller Capabilities */
	struct {
		/* supports the Vector Chunk Copy I/O Command */
		uint32_t	vec_chk_cpy	: 1;
		/* supports multiple resets when a chunk is in its free state */
		uint32_t	multi_reset	: 1;
		uint32_t	mccap_rsvd	: 30;
	} mccap;

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

	/** Controller Sector Size */
	uint32_t	csecs;

	/** Sector OOB size */
	uint32_t	sos;

	uint8_t		reserved4[44];

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

	uint8_t		reserved7[3071 - 255];

	/** Vendor Specific */
	uint8_t		vs[4095 - 3071];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_lnvm_geometry_data) == 4096, "Incorrect size");

struct __attribute__((packed)) spdk_nvme_lnvm_chunk_info {
	/** Chunk State */
	struct {
		uint8_t free		: 1; /**< if set to 1 the chunk is free */
		uint8_t closed		: 1; /**< if set to 1 the chunk is closed */
		uint8_t open		: 1; /**< if set to 1 the chunk is open */
		uint8_t offline		: 1; /**< if set to 1 the chunk is offline */
		uint8_t reserved	: 4;
	} cs;

	/** Chunk Type */
	struct {
		uint8_t seq_write		: 1; /**< If set to 1 the chunk must be written sequentially */
		uint8_t rand_write		: 1; /**< if set to 1 the chunk allows random writes */
		uint8_t ct_rsvd1		: 2;
		uint8_t size_deviate	:
		1; /**< if set to 1 the chunk deviates from the chunk size reported in identify geometry command */
		uint8_t ct_rsvd2		: 3;
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
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_lnvm_chunk_info) == 32, "Incorrect size");

/**
 * LNVM media error status codes
 */
enum spdk_nvme_lnvm_media_error_status_code {
	/* Also defined in nvme_spec.h from NVMe 1.3 specification */
//	SPDK_NVME_SC_WRITE_FAULTS			= 0x80,

	SPDK_NVME_SC_OFFLINE_CHUNK			= 0xC0,
	SPDK_NVME_SC_INVALID_RESET			= 0xC1,
	SPDK_NVME_SC_WRITE_FAIL_WRITE_NEXT_UNIT		= 0xF0,
	SPDK_NVME_SC_WRITE_FAIL_CHUNK_EARLY_CLOSE	= 0xF1,
	SPDK_NVME_SC_OUT_OF_ORDER_WRITE			= 0xF2,
	SPDK_NVME_SC_READ_HIGH_ECC			= 0xD0,
};

/**
 * LNVM log page identifiers for SPDK_NVME_OPC_GET_LOG_PAGE
 */
enum spdk_nvme_lnvm_log_page {
	/** Chunk Information */
	SPDK_NVME_LOG_CHUNK_INFO			= 0xCA,
};

/**
 * LNVM specific features
 */
enum spdk_nvme_lnvm_feat {
	/* Also defined in nvme_spec.h from NVMe 1.3 specification */
//	SPDK_NVME_FEAT_ERROR_RECOVERY			= 0x05,

	SPDK_NVME_FEAT_MEDIA_FEEDBACK			= 0xCA,
};

/**
 * Admin opcodes in OCSSD 2.0 spec
 */
enum spdk_nvme_lnvm_admin_opcode {
	SPDK_NVME_OPC_GEOMETRY				= 0xE2,

	/* Also defined in nvme_spec.h from NVMe 1.3 specification */
//	SPDK_NVME_OPC_GET_LOG_PAGE			= 0x02,
//	SPDK_NVME_OPC_SET_FEATURES			= 0x09,
//	SPDK_NVME_OPC_GET_FEATURES			= 0x0a,
};

/**
 * NVM opcodes in OCSSD 2.0 spec
 */
enum spdk_nvme_lnvm_opcode {
	/* Also defined in nvme_spec.h from NVMe 1.3 specification */
//	SPDK_NVME_OPC_WRITE				= 0x01,
//	SPDK_NVME_OPC_READ				= 0x02,
//	SPDK_NVME_OPC_DATASET_MANAGEMENT		= 0x09,

	SPDK_NVME_OPC_VECTOR_RESET			= 0x90,
	SPDK_NVME_OPC_VECTOR_WRITE			= 0x91,
	SPDK_NVME_OPC_VECTOR_READ			= 0x92,
	SPDK_NVME_OPC_VECTOR_COPY			= 0x93,
};

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVME_LNVM_SPEC_H */
