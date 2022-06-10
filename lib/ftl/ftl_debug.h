/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_DEBUG_H
#define FTL_DEBUG_H

#include "ftl_internal.h"
#include "ftl_band.h"
#include "ftl_core.h"

typedef void (*ftl_band_validate_md_cb)(struct ftl_band *band, bool valid);

#if defined(DEBUG)
void ftl_dev_dump_bands(struct spdk_ftl_dev *dev);
#else
static inline void
ftl_dev_dump_bands(struct spdk_ftl_dev *dev)
{
}
#endif

void ftl_dev_dump_stats(const struct spdk_ftl_dev *dev);

#endif /* FTL_DEBUG_H */
