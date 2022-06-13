/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_INTERNAL_H
#define FTL_INTERNAL_H

#include "spdk/stdinc.h"
#include "spdk/crc32.h"
#include "spdk/util.h"
#include "spdk/uuid.h"

/* Marks address as invalid */
#define FTL_ADDR_INVALID	((ftl_addr)-1)
/* Marks LBA as invalid */
#define FTL_LBA_INVALID		((uint64_t)-1)
/* Smallest data unit size */
#define FTL_BLOCK_SIZE		4096ULL

/*
 * This type represents address in the ftl address space. Values from 0 to based bdev size are
 * mapped directly to base device lbas. Values above that represent nv cache lbas.
 */
typedef uint64_t ftl_addr;

/* Number of LBAs that could be stored in a single block */
#define FTL_NUM_LBA_IN_BLOCK	(FTL_BLOCK_SIZE / sizeof(uint64_t))

/*
 * Mapping of physical (actual location on disk) to logical (user's POV) addresses. Used in two main scenarios:
 * - during relocation FTL needs to pin L2P pages (this allows to check which pages to pin) and move still valid blocks
 * (valid map allows for preliminary elimination of invalid physical blocks, but user data could invalidate a location
 * during read/write operation, so actual comparision against L2P needs to be done)
 * - After dirty shutdown the state of the L2P is unknown and needs to be rebuilt - it is done by applying all P2L, taking
 * into account ordering of user writes
 */
struct ftl_p2l_map {
	/* Number of valid LBAs */
	size_t					num_valid;

	/* P2L map's reference count, prevents premature release of resources during dirty shutdown recovery for open bands */
	size_t					ref_cnt;

	/* P2L map (only valid for open/relocating bands) */
	union {
		uint64_t	*band_map;
		void		*chunk_map;
	};

	/* DMA buffer for region's metadata entry */
	union {
		struct ftl_band_md		*band_dma_md;

		struct ftl_nv_cache_chunk_md	*chunk_dma_md;
	};
};

#endif /* FTL_INTERNAL_H */
