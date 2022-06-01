/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_DEBUG_H
#define FTL_DEBUG_H

#include "ftl_internal.h"
#include "ftl_band.h"
#include "ftl_core.h"

#if defined(DEBUG)
void ftl_band_validate_md(struct ftl_band *band, ftl_band_validate_md_cb cb);
void ftl_dev_dump_bands(struct spdk_ftl_dev *dev);
#else

static void
_validate_cb(void *ctx)
{
	struct ftl_band *band = ctx;

	band->validate_cb(band, true);
}

static inline void
ftl_band_validate_md(struct ftl_band *band, ftl_band_validate_md_cb cb)
{
	/* For release builds this is a NOP operation, but should still be asynchronous to keep the behavior consistent */
	band->validate_cb = cb;
	spdk_thread_send_msg(band->dev->core_thread, _validate_cb, band);
}

static inline void
ftl_dev_dump_bands(struct spdk_ftl_dev *dev)
{
}
#endif

void ftl_dev_dump_stats(const struct spdk_ftl_dev *dev);

#endif /* FTL_DEBUG_H */
