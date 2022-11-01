/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_TRACE_H
#define FTL_TRACE_H

#include "ftl_internal.h"

#define FTL_TRACE_INVALID_ID ((uint64_t) -1)

enum ftl_trace_completion {
	FTL_TRACE_COMPLETION_INVALID,
	FTL_TRACE_COMPLETION_CACHE,
	FTL_TRACE_COMPLETION_DISK,
};

struct ftl_trace {
	/* Monotonically incrementing event id */
	uint64_t		id;
};

struct spdk_ftl_dev;
struct ftl_trace;
struct ftl_io;
struct ftl_wbuf_entry;
struct ftl_band;

#if defined(DEBUG)
uint64_t ftl_trace_alloc_id(struct spdk_ftl_dev *dev);
void ftl_trace_reloc_band(struct spdk_ftl_dev *dev, const struct ftl_band *band);
void ftl_trace_write_band(struct spdk_ftl_dev *dev, const struct ftl_band *band);
void ftl_trace_lba_io_init(struct spdk_ftl_dev *dev, const struct ftl_io *io);
void ftl_trace_submission(struct spdk_ftl_dev *dev, const struct ftl_io *io, ftl_addr addr,
			  size_t addr_cnt);
void ftl_trace_completion(struct spdk_ftl_dev *dev, const struct ftl_io *io,
			  enum ftl_trace_completion type);
void ftl_trace_limits(struct spdk_ftl_dev *dev, int limit, size_t num_free);
#else /* defined(DEBUG) */
#define ftl_trace_alloc_id(dev) FTL_TRACE_INVALID_ID
#define ftl_trace_reloc_band(dev, band)
#define ftl_trace_write_band(dev, band)
#define ftl_trace_lba_io_init(dev, io)
#define ftl_trace_submission(dev, io, addr, addr_cnt)
#define ftl_trace_completion(dev, io, type)
#define ftl_trace_limits(dev, limits, num_free)
#endif

#endif /* FTL_TRACE_H */
