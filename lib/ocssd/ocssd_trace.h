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

#ifndef OCSSD_TRACE_H
#define OCSSD_TRACE_H

typedef uint64_t ocssd_trace_group_t;

enum ocssd_trace_source {
	OCSSD_TRACE_SOURCE_INTERNAL,
	OCSSD_TRACE_SOURCE_USER,
};

enum ocssd_trace_type {
	OCSSD_TRACE_TYPE_READ,
	OCSSD_TRACE_TYPE_MD_READ,
	OCSSD_TRACE_TYPE_WRITE,
	OCSSD_TRACE_TYPE_MD_WRITE,
	OCSSD_TRACE_TYPE_ERASE,
	OCSSD_TRACE_TYPE_BAND_DEFRAG,
	OCSSD_TRACE_TYPE_BAND_WRITE,
	OCSSD_TRACE_TYPE_APPLIED_LIMITS,
	OCSSD_TRACE_TYPE_MAX,
};

enum ocssd_trace_point {
	OCSSD_TRACE_POINT_SCHEDULED,
	OCSSD_TRACE_POINT_RWB_FILL,
	OCSSD_TRACE_POINT_RWB_POP,
	OCSSD_TRACE_POINT_SUBMISSION,
	OCSSD_TRACE_POINT_COMPLETION,
	OCSSD_TRACE_POINT_OTHER,
};

enum ocssd_trace_completion {
	OCSSD_TRACE_COMPLETION_INVALID,
	OCSSD_TRACE_COMPLETION_CACHE,
	OCSSD_TRACE_COMPLETION_DISK,
};

/* TODO: We should have a map linking these values with its */
/* sizes to make sure the parser has up-to-date definitions. */
enum ocssd_trace_data_type {
	OCSSD_TRACE_DATA_TRACE_TYPE,
	OCSSD_TRACE_DATA_TRACE_POINT,
	OCSSD_TRACE_DATA_SOURCE,
	OCSSD_TRACE_DATA_PPA,
	OCSSD_TRACE_DATA_LBA,
	OCSSD_TRACE_DATA_LBK_CNT,
	OCSSD_TRACE_DATA_BAND_ID,
	OCSSD_TRACE_DATA_BAND_MERIT,
	OCSSD_TRACE_DATA_RWB_USER_SIZE,
	OCSSD_TRACE_DATA_RWB_INTERNAL_SIZE,
	OCSSD_TRACE_DATA_LIMIT,
	OCSSD_TRACE_DATA_VLD_CNT,
	OCSSD_TRACE_DATA_COMPLETION,
	OCSSD_TRACE_DATA_BAND_CNT,
	OCSSD_TRACE_DATA_MAX,
};

struct ocssd_event {
	/* Timestamp (us granularity) */
	uint64_t		ts;

	/* Id used for grouping multiple events of the same request */
	uint64_t		id;

	/* Following data size */
	uint8_t			size;
} __attribute__((packed));

#define OCSSD_TRACE_INVALID_ID ((uint64_t) -1)

#if defined(OCSSD_INTERNAL)

#include "ocssd_utils.h"
#include "ocssd_ppa.h"

struct ocssd_trace;
struct ocssd_io;
struct ocssd_rwb_entry;
struct ocssd_band;

#if enabled(OCSSD_TRACE)

#define ocssd_trace(fn, trace, ...) \
	do { \
		if (trace) { \
			ocssd_trace_##fn(trace, ## __VA_ARGS__); \
		} \
	} while (0)

struct ocssd_trace *ocssd_trace_init(const char *fname);
void	ocssd_trace_free(struct ocssd_trace *trace);
ocssd_trace_group_t ocssd_trace_alloc_group(struct ocssd_trace *trace);
void	ocssd_trace_defrag_band(struct ocssd_trace *trace, const struct ocssd_band *band);
void	ocssd_trace_write_band(struct ocssd_trace *trace, const struct ocssd_band *band);
void	ocssd_trace_lba_io_init(struct ocssd_trace *trace, const struct ocssd_io *io);
void	ocssd_trace_rwb_fill(struct ocssd_trace *trace, const struct ocssd_io *io);
void	ocssd_trace_rwb_pop(struct ocssd_trace *trace, const struct ocssd_rwb_entry *entry);
void	ocssd_trace_submission(struct ocssd_trace *trace,
			       const struct ocssd_io *io,
			       struct ocssd_ppa ppa, size_t ppa_cnt);
void	ocssd_trace_completion(struct ocssd_trace *trace,
			       const struct ocssd_io *io,
			       enum ocssd_trace_completion type);
void	ocssd_trace_limits(struct ocssd_trace *trace, const size_t *limits, size_t num_free);
#else

#define ocssd_trace_init(p) NULL
#define ocssd_trace_free(t)
#define ocssd_trace_alloc_group(t) OCSSD_TRACE_INVALID_ID
#define ocssd_trace(fn, trace, ...)

#endif /* enabled(OCSSD_TRACE) */
#endif /* defined(OCSSD_INTERNAL) */
#endif /* OCSSD_TRACE_H */
