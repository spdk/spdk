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

struct spdk_ftl_dev;

enum ftl_md_type {
	FTL_MD_TYPE_BAND,
	FTL_MD_TYPE_CHUNK
};

enum ftl_band_type {
	FTL_BAND_TYPE_GC = 1,
	FTL_BAND_TYPE_COMPACTION
};

enum ftl_md_status {
	FTL_MD_SUCCESS,
	/* Metadata read failure */
	FTL_MD_IO_FAILURE,
	/* Invalid version */
	FTL_MD_INVALID_VER,
	/* UUID doesn't match */
	FTL_MD_NO_MD,
	/* UUID and version matches but CRC doesn't */
	FTL_MD_INVALID_CRC,
	/* Vld or p2l map size doesn't match */
	FTL_MD_INVALID_SIZE
};

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

struct spdk_ftl_dev;

struct ftl_reloc *ftl_reloc_init(struct spdk_ftl_dev *dev);

void ftl_reloc_free(struct ftl_reloc *reloc);

void ftl_reloc(struct ftl_reloc *reloc);

void ftl_reloc_halt(struct ftl_reloc *reloc);

void ftl_reloc_resume(struct ftl_reloc *reloc);

bool ftl_reloc_is_halted(const struct ftl_reloc *reloc);

#endif /* FTL_INTERNAL_H */
