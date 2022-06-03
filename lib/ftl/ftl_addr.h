/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
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
/*        - offset inside the disk */
/*        - cache_offset inside the cache (indicated by the cached flag) */
/*        - packed version of the two formats above (can be only used when the */
/*          offset can be represented in less than 32 bits) */
/* Packed format is used, when possible, to avoid wasting RAM on the L2P table. */
struct ftl_addr {
	union {
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

				uint32_t offset;
			};
			uint32_t rsvd;
		} pack;

		uint64_t offset;
	};
};

#endif /* FTL_ADDR_H */
