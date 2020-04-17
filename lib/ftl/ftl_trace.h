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

#ifndef FTL_TRACE_H
#define FTL_TRACE_H

#include "ftl_addr.h"

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
void ftl_trace_defrag_band(struct spdk_ftl_dev *dev, const struct ftl_band *band);
void ftl_trace_write_band(struct spdk_ftl_dev *dev, const struct ftl_band *band);
void ftl_trace_lba_io_init(struct spdk_ftl_dev *dev, const struct ftl_io *io);
void ftl_trace_wbuf_fill(struct spdk_ftl_dev *dev, const struct ftl_io *io);
void ftl_trace_wbuf_pop(struct spdk_ftl_dev *dev, const struct ftl_wbuf_entry *entry);
void ftl_trace_submission(struct spdk_ftl_dev *dev,
			  const struct ftl_io *io,
			  struct ftl_addr addr, size_t addr_cnt);
void ftl_trace_completion(struct spdk_ftl_dev *dev,
			  const struct ftl_io *io,
			  enum ftl_trace_completion type);
void ftl_trace_limits(struct spdk_ftl_dev *dev, int limit, size_t num_free);
#else /* defined(DEBUG) */
#define ftl_trace_alloc_id(dev) FTL_TRACE_INVALID_ID
#define ftl_trace_defrag_band(dev, band)
#define ftl_trace_write_band(dev, band)
#define ftl_trace_lba_io_init(dev, io)
#define ftl_trace_wbuf_fill(dev, io)
#define ftl_trace_wbuf_pop(dev, entry)
#define ftl_trace_submission(dev, io, addr, addr_cnt)
#define ftl_trace_completion(dev, io, type)
#define ftl_trace_limits(dev, limits, num_free)
#endif

#endif /* FTL_TRACE_H */
