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

#ifndef FTL_ADDR_H
#define FTL_ADDR_H

#include "spdk/stdinc.h"

/* Marks address as invalid */
#define FTL_ADDR_INVALID	(-1)
/* Marks LBA as invalid */
#define FTL_LBA_INVALID		((uint64_t)-1)
/* Smallest data unit size */
#define FTL_BLOCK_SIZE		4096

/* This structure represents on-disk address. It can have one of the following */
/* formats: */
/*        - addr describing the raw address */
/*        - cache_offset inside the cache (indicated by the cached flag) */
/*        - packed version of the two formats above (can be only used when the */
/*          raw address can be represented in less than 32 bits) */
/* Packed format is used, when possible, to avoid wasting RAM on the L2P table. */
struct ftl_addr {
	union {
		struct {
			uint64_t offset	 : 32;
			uint64_t zone_id : 16;
			uint64_t pu	 : 15;
			uint64_t rsvd	 : 1;
		};

		struct {
			uint64_t cache_offset : 63;
			uint64_t cached	      : 1;
		};

		struct {
			union {
				struct  {
					uint32_t cache_offset : 31;
					uint32_t cached	      : 1;
				};

				uint32_t addr;
			};
			uint32_t rsvd;
		} pack;

		uint64_t addr;
	};
};

struct ftl_ppa_fmt {
	/* Logical block */
	unsigned int				lbk_offset;
	unsigned int				lbk_mask;

	/* Chunk */
	unsigned int				chk_offset;
	unsigned int				chk_mask;

	/* Parallel unit (NAND die) */
	unsigned int				pu_offset;
	unsigned int				pu_mask;

	/* Group */
	unsigned int				grp_offset;
	unsigned int				grp_mask;
};

#endif /* FTL_ADDR_H */
