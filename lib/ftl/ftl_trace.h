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

#include "ftl_ppa.h"

#define FTL_TRACE_INVALID_ID ((uint64_t) -1)

typedef uint64_t ftl_trace_group_t;

enum ftl_trace_source {
	FTL_TRACE_SOURCE_INTERNAL = 0x20,
	FTL_TRACE_SOURCE_USER,
};

enum ftl_trace_type {
	FTL_TRACE_TYPE_READ,
	FTL_TRACE_TYPE_MD_READ,
	FTL_TRACE_TYPE_WRITE,
	FTL_TRACE_TYPE_MD_WRITE,
	FTL_TRACE_TYPE_ERASE,
	FTL_TRACE_TYPE_OTHER,
};

enum ftl_trace_point {
	FTL_TRACE_POINT_SCHEDULED,
	FTL_TRACE_POINT_RWB_FILL,
	FTL_TRACE_POINT_SUBMISSION,
	FTL_TRACE_POINT_OTHER,
};

enum ftl_trace_completion {
	FTL_TRACE_COMPLETION_INVALID = FTL_TRACE_POINT_OTHER + 1,
	FTL_TRACE_COMPLETION_CACHE,
	FTL_TRACE_COMPLETION_DISK,
};

struct ftl_event {
	/* Id used for grouping multiple events of the same request */
	uint64_t		id;
} __attribute__((packed));


struct ftl_trace;
struct ftl_io;
struct ftl_rwb_entry;
struct ftl_band;

#define ftl_trace(fn, trace, ...) \
	do { \
		if (trace) { \
			ftl_trace_##fn(trace, ## __VA_ARGS__); \
		} \
	} while (0)

struct ftl_trace *ftl_trace_init(void);
void	ftl_trace_free(struct ftl_trace *trace);
ftl_trace_group_t ftl_trace_alloc_group(struct ftl_trace *trace);
void	ftl_trace_defrag_band(struct ftl_trace *trace, const struct ftl_band *band);
void	ftl_trace_write_band(struct ftl_trace *trace, const struct ftl_band *band);
void	ftl_trace_lba_io_init(struct ftl_trace *trace, const struct ftl_io *io);
void	ftl_trace_rwb_fill(struct ftl_trace *trace, const struct ftl_io *io);
void	ftl_trace_rwb_pop(struct ftl_trace *trace, const struct ftl_rwb_entry *entry);
void	ftl_trace_submission(struct ftl_trace *trace,
			     const struct ftl_io *io,
			     struct ftl_ppa ppa, size_t ppa_cnt);
void	ftl_trace_completion(struct ftl_trace *trace,
			     const struct ftl_io *io,
			     enum ftl_trace_completion type);
void	ftl_trace_limits(struct ftl_trace *trace, const size_t *limits, size_t num_free);

#endif /* FTL_TRACE_H */
