/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_DEBUG_H
#define FTL_DEBUG_H

#include "ftl_addr.h"
#include "ftl_band.h"
#include "ftl_core.h"

#if defined(DEBUG)
/* Debug flags - enabled when defined */
#define FTL_META_DEBUG	1
#define FTL_DUMP_STATS	1

#define ftl_debug(msg, ...) \
	SPDK_ERRLOG(msg, ## __VA_ARGS__)
#else
#define ftl_debug(msg, ...)
#endif

static inline const char *
ftl_addr2str(struct ftl_addr addr, char *buf, size_t size)
{
	snprintf(buf, size, "(%"PRIu64")", addr.offset);
	return buf;
}

#if defined(FTL_META_DEBUG)
bool ftl_band_validate_md(struct ftl_band *band);
void ftl_dev_dump_bands(struct spdk_ftl_dev *dev);
#else
#define ftl_band_validate_md(band)
#define ftl_dev_dump_bands(dev)
#endif

#if defined(FTL_DUMP_STATS)
void ftl_dev_dump_stats(const struct spdk_ftl_dev *dev);
#else
#define ftl_dev_dump_stats(dev)
#endif

#endif /* FTL_DEBUG_H */
