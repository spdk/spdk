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

#ifndef FTL_SB_COMMON_H
#define FTL_SB_COMMON_H

#include "spdk/stdinc.h"
#include "utils/ftl_defs.h"

/* Size of superblock on NV cache, make it bigger for future fields */
#define FTL_SUPERBLOCK_SIZE (128ULL * KiB)

#define FTL_MAGIC(a, b, c, d) \
    ((UINT64_C(a) << 48) | (UINT64_C(b) << 32) | (UINT64_C(c) << 16) | \
     UINT64_C(d))

/**
 * Magic number identifies FTL superblock
 */
#define FTL_SUPERBLOCK_MAGIC FTL_MAGIC(0x1410, 0x1683, 0x1920, 0x1989)

struct ftl_superblock_gc_info {
	uint64_t band_id_high_prio;
	uint64_t band_id;
	uint64_t band_phys_id;
	uint64_t is_valid;
};

struct ftl_superblock_header {
	uint64_t crc;
	uint64_t magic;
	uint64_t version;
};

struct ftl_superblock_md_region {
	uint32_t		type;
	uint32_t		version;
	uint64_t		blk_offs;
	uint64_t		blk_sz;
};

struct ftl_superblock_shm {
	/* SHM initialization completed */
	bool				shm_ready;

	/* SHM status - fast restart */
	bool				shm_clean;

	/* Used to continue trim after SHM recovery */
	struct {
		bool			in_progress;
		uint64_t		start_lba;
		uint64_t		num_blocks;
	} trim;

	struct ftl_superblock_gc_info gc_info;
};

#endif /* FTL_SB_COMMON_H */
