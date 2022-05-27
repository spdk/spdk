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

#ifndef FTL_LAYOUT_UPGRADE_H
#define FTL_LAYOUT_UPGRADE_H

#include "ftl_core.h"
#include "ftl_layout.h"

struct spdk_ftl_dev;
struct ftl_layout_region;
struct ftl_layout_upgrade_ctx;

enum ftl_layout_upgrade_result {
	/* Continue with the selected region upgrade */
	ftl_layout_upgrade_continue = 0,

	/* Layout upgrade done */
	ftl_layout_upgrade_done,

	/* Layout upgrade fault */
	ftl_layout_upgrade_fault,
};

/* MD region upgrade verify fn: return 0 on success */
typedef int (*ftl_region_upgrade_verify_fn)(struct spdk_ftl_dev *dev, struct ftl_layout_region *region);

/* MD region upgrade fn: return 0 on success */
typedef int (*ftl_region_upgrade_fn)(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx);

/* MD region upgrade descriptor */
struct ftl_region_upgrade_desc
{
	/* Qualifies the region for upgrade */
	ftl_region_upgrade_verify_fn verify;

	/* Upgrades the region */
	ftl_region_upgrade_fn upgrade;

	/* New region version (i.e. after the upgrade) */
	uint32_t new_version;

	/* Context buffer allocated for upgrade() */
	size_t ctx_size;
};

/* MD layout upgrade descriptor (i.e. all regions) */
struct ftl_layout_upgrade_desc
{
	/* # of entries in the region upgrade descriptor */
	size_t reg_upgrade_desc_sz;

	/* Region upgrade descriptor */
	struct ftl_region_upgrade_desc *reg_upgrade_desc;
};

/* Region upgrade callback */
typedef void (*ftl_region_upgrade_cb)(struct spdk_ftl_dev *dev, void *ctx, int status);

/* MD layout upgrade context */
struct ftl_layout_upgrade_ctx
{
	/* MD region being upgraded */
	struct ftl_layout_region *reg;

	/* MD region upgrade descriptor */
	struct ftl_layout_upgrade_desc *upgrade;

	/* New region version (i.e. after the upgrade) */
	uint64_t next_reg_ver;

	/* Context buffer for the region upgrade */
	void *ctx;

	/* Region upgrade callback */
	ftl_region_upgrade_cb cb;

	/* Ctx for the region upgrade callback */
	void *cb_ctx;
};

/**
 * @brief Disable region upgrade for particular version.
 * 
 * @param dev FTL device
 * @param region FTL layout region descriptor
 * @return int -1
 */
int ftl_region_upgrade_disabled(struct spdk_ftl_dev *dev, struct ftl_layout_region *region);

/**
 * @brief Enable region upgrade for particular version.
 * 
 * @param dev FTL device
 * @param region FTL layout region descriptor
 * @return int -1 (i.e. disable) if SB is dirty or SHM clean, 0 otherwise (i.e. enable)
 */
int ftl_region_upgrade_enabled(struct spdk_ftl_dev *dev, struct ftl_layout_region *region);

/**
 * @brief Upgrade the superblock.
 * 
 * This call is sychronous.
 * 
 * @param dev FTL device
 * @return int 0: success, error code otherwise
 */
int ftl_superblock_upgrade(struct spdk_ftl_dev *dev);

/**
 * @brief Qualify the MD layout for upgrade.
 * 
 * The SB MD layout is built or loaded.
 * If loaded, walk through all MD layout and filter out all MD regions that need an upgrade.
 * Call .verify() on region upgrade descriptors for all such regions.
 * 
 * @param dev FTL device
 * @return int 0: success, error code otherwise
 */
int ftl_layout_verify(struct spdk_ftl_dev *dev);

/**
 * @brief Dump the FTL layout
 * 
 * Verify MD layout in terms of region overlaps.
 * 
 * @param dev FTL device
 * @return int 0: success, error code otherwise
 */
int ftl_layout_dump(struct spdk_ftl_dev *dev);

/**
 * @brief Upgrade the MD region.
 * 
 * Call .upgrade() on the selected region upgrade descriptor.
 * When returned 0, this call is asynchronous.
 * The .upgrade() is expected to convert and persist the metadata.
 * When that's done, the call to ftl_region_upgrade_completed() is expected
 * to continue with the layout upgrade.
 * 
 * When returned an error code, the caller is responsible to abort the mngt pipeline.
 * 
 * @param dev FTL device
 * @param ctx Layout upgrade context
 * @return int 0: upgrade in progress, error code otherwise
 */
int ftl_region_upgrade(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx);

/**
 * @brief Called when MD region upgrade is completed - see ftl_region_upgrade().
 * 
 * Upgrades the SB MD layout and region's prev version descriptor to the just upgraded version.
 * Executes the layout upgrade owner's callback (mngt region_upgrade_cb()) to continue
 * with the layout upgrade.
 * 
 * @param dev FTL device
 * @param ctx Layout upgrade context
 * @param status Region upgrade status: 0: success, error code otherwise
 */
void ftl_region_upgrade_completed(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx, int status);

/**
 * @brief Initialize the layout upgrade context.
 * 
 * Select the next region to be upgraded.
 * 
 * @param dev FTL device
 * @param ctx Layout upgrade context
 * @return int see enum ftl_layout_upgrade_result
 */
int ftl_layout_upgrade_init_ctx(struct spdk_ftl_dev *dev, struct ftl_layout_upgrade_ctx *ctx);

#endif /* FTL_LAYOUT_UPGRADE_H */
