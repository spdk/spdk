/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/thread.h"
#include "spdk/crc32.h"

#include "ftl_core.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "ftl_utils.h"
#include "ftl_internal.h"

void
ftl_mngt_init_layout(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_layout_setup(dev)) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

static bool
is_buffer_needed(enum ftl_layout_region_type type)
{
	switch (type) {
#ifdef SPDK_FTL_VSS_EMU
	case FTL_LAYOUT_REGION_TYPE_VSS:
#endif
	case FTL_LAYOUT_REGION_TYPE_DATA_NVC:
	case FTL_LAYOUT_REGION_TYPE_DATA_BASE:
		return false;

	default:
		return true;
	}
}

void
ftl_mngt_init_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region = layout->region;
	uint64_t i;

	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; i++, region++) {
		if (layout->md[i]) {
			/*
			 * Some metadata objects are initialized by other FTL
			 * components. At the moment it's only used by superblock (and its mirror) -
			 * during load time we need to read it earlier in order to get the layout for the
			 * other regions.
			 */
			continue;
		}
		layout->md[i] = ftl_md_create(dev, region->current.blocks, region->vss_blksz, region->name,
					      !is_buffer_needed(i), region);
		if (NULL == layout->md[i]) {
			ftl_mngt_fail_step(mngt);
			return;
		}
	}

	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_deinit_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region = layout->region;
	uint64_t i;

	for (i = 0; i < FTL_LAYOUT_REGION_TYPE_MAX; i++, region++) {
		if (layout->md[i]) {
			ftl_md_destroy(layout->md[i]);
			layout->md[i] = NULL;
		}
	}

	ftl_mngt_next_step(mngt);
}

#ifdef SPDK_FTL_VSS_EMU
void
ftl_mngt_md_init_vss_emu(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_layout *layout = &dev->layout;
	struct ftl_layout_region *region = &layout->region[FTL_LAYOUT_REGION_TYPE_VSS];

	/* Initialize VSS layout */
	ftl_layout_setup_vss_emu(dev);

	/* Allocate md buf */
	layout->md[FTL_LAYOUT_REGION_TYPE_VSS] = ftl_md_create(dev, region->current.blocks,
			region->vss_blksz, NULL, 0, region);
	if (NULL == layout->md[FTL_LAYOUT_REGION_TYPE_VSS]) {
		ftl_mngt_fail_step(mngt);
		return;
	}
	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_md_deinit_vss_emu(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_layout *layout = &dev->layout;

	if (layout->md[FTL_LAYOUT_REGION_TYPE_VSS]) {
		ftl_md_destroy(layout->md[FTL_LAYOUT_REGION_TYPE_VSS]);
		layout->md[FTL_LAYOUT_REGION_TYPE_VSS] = NULL;
	}

	ftl_mngt_next_step(mngt);
}
#endif
