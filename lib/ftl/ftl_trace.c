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

#include "spdk/trace.h"

#include "ftl_core.h"
#include "ftl_trace.h"
#include "ftl_io.h"
#include "ftl_band.h"

#include "spdk_internal/trace_defs.h"

#if defined(DEBUG)

enum ftl_trace_source {
	FTL_TRACE_SOURCE_INTERNAL,
	FTL_TRACE_SOURCE_USER,
	FTL_TRACE_SOURCE_MAX,
};

#define FTL_TPOINT_ID(id, src) SPDK_TPOINT_ID(TRACE_GROUP_FTL, (((id) << 1) | (!!(src))))

#define FTL_TRACE_BAND_DEFRAG(src)		FTL_TPOINT_ID(0, src)
#define FTL_TRACE_BAND_WRITE(src)		FTL_TPOINT_ID(1, src)
#define FTL_TRACE_LIMITS(src)			FTL_TPOINT_ID(2, src)
#define FTL_TRACE_WBUF_POP(src)			FTL_TPOINT_ID(3, src)

#define FTL_TRACE_READ_SCHEDULE(src)		FTL_TPOINT_ID(4, src)
#define FTL_TRACE_READ_SUBMISSION(src)		FTL_TPOINT_ID(5, src)
#define FTL_TRACE_READ_COMPLETION_INVALID(src)	FTL_TPOINT_ID(6, src)
#define FTL_TRACE_READ_COMPLETION_CACHE(src)	FTL_TPOINT_ID(7, src)
#define FTL_TRACE_READ_COMPLETION_DISK(src)	FTL_TPOINT_ID(8, src)

#define FTL_TRACE_MD_READ_SCHEDULE(src)		FTL_TPOINT_ID(9,  src)
#define FTL_TRACE_MD_READ_SUBMISSION(src)	FTL_TPOINT_ID(10, src)
#define FTL_TRACE_MD_READ_COMPLETION(src)	FTL_TPOINT_ID(11, src)

#define FTL_TRACE_WRITE_SCHEDULE(src)		FTL_TPOINT_ID(12, src)
#define FTL_TRACE_WRITE_WBUF_FILL(src)		FTL_TPOINT_ID(13, src)
#define FTL_TRACE_WRITE_SUBMISSION(src)		FTL_TPOINT_ID(14, src)
#define FTL_TRACE_WRITE_COMPLETION(src)		FTL_TPOINT_ID(15, src)

#define FTL_TRACE_MD_WRITE_SCHEDULE(src)	FTL_TPOINT_ID(16, src)
#define FTL_TRACE_MD_WRITE_SUBMISSION(src)	FTL_TPOINT_ID(17, src)
#define FTL_TRACE_MD_WRITE_COMPLETION(src)	FTL_TPOINT_ID(18, src)

#define FTL_TRACE_ERASE_SUBMISSION(src)		FTL_TPOINT_ID(19, src)
#define FTL_TRACE_ERASE_COMPLETION(src)		FTL_TPOINT_ID(20, src)

SPDK_TRACE_REGISTER_FN(ftl_trace_func, "ftl", TRACE_GROUP_FTL)
{
	const char source[] = { 'i', 'u' };
	char descbuf[128];
	int i;

	spdk_trace_register_owner(OWNER_FTL, 'f');

	for (i = 0; i < FTL_TRACE_SOURCE_MAX; ++i) {
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "band_defrag");
		spdk_trace_register_description(descbuf, FTL_TRACE_BAND_DEFRAG(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "band");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "band_write");
		spdk_trace_register_description(descbuf, FTL_TRACE_BAND_WRITE(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "band");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "limits");
		spdk_trace_register_description(descbuf, FTL_TRACE_LIMITS(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "limits");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "rwb_pop");
		spdk_trace_register_description(descbuf, FTL_TRACE_WBUF_POP(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "lba");

		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "md_read_sched");
		spdk_trace_register_description(descbuf, FTL_TRACE_MD_READ_SCHEDULE(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "addr");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "md_read_submit");
		spdk_trace_register_description(descbuf, FTL_TRACE_MD_READ_SUBMISSION(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "addr");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "md_read_cmpl");
		spdk_trace_register_description(descbuf, FTL_TRACE_MD_READ_COMPLETION(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "lba");

		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "md_write_sched");
		spdk_trace_register_description(descbuf, FTL_TRACE_MD_WRITE_SCHEDULE(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "addr");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "md_write_submit");
		spdk_trace_register_description(descbuf, FTL_TRACE_MD_WRITE_SUBMISSION(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "addr");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "md_write_cmpl");
		spdk_trace_register_description(descbuf, FTL_TRACE_MD_WRITE_COMPLETION(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "lba");

		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "read_sched");
		spdk_trace_register_description(descbuf, FTL_TRACE_READ_SCHEDULE(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "lba");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "read_submit");
		spdk_trace_register_description(descbuf, FTL_TRACE_READ_SUBMISSION(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "addr");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "read_cmpl_invld");
		spdk_trace_register_description(descbuf, FTL_TRACE_READ_COMPLETION_INVALID(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "lba");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "read_cmpl_cache");
		spdk_trace_register_description(descbuf, FTL_TRACE_READ_COMPLETION_CACHE(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "lba");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "read_cmpl_ssd");
		spdk_trace_register_description(descbuf, FTL_TRACE_READ_COMPLETION_DISK(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "lba");

		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "write_sched");
		spdk_trace_register_description(descbuf, FTL_TRACE_WRITE_SCHEDULE(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "lba");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "rwb_fill");
		spdk_trace_register_description(descbuf, FTL_TRACE_WRITE_WBUF_FILL(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "lba");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "write_submit");
		spdk_trace_register_description(descbuf, FTL_TRACE_WRITE_SUBMISSION(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "addr");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "write_cmpl");
		spdk_trace_register_description(descbuf, FTL_TRACE_WRITE_COMPLETION(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "lba");

		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "erase_submit");
		spdk_trace_register_description(descbuf, FTL_TRACE_ERASE_SUBMISSION(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "addr");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "erase_cmpl");
		spdk_trace_register_description(descbuf, FTL_TRACE_ERASE_COMPLETION(i),
						OWNER_FTL, OBJECT_NONE, 0,
						SPDK_TRACE_ARG_TYPE_INT, "addr");
	}
}

static uint16_t
ftl_trace_io_source(const struct ftl_io *io)
{
	if (io->flags & FTL_IO_INTERNAL) {
		return FTL_TRACE_SOURCE_INTERNAL;
	} else {
		return FTL_TRACE_SOURCE_USER;
	}
}

static uint64_t
ftl_trace_next_id(struct ftl_trace *trace)
{
	assert(trace->id != FTL_TRACE_INVALID_ID);
	return __atomic_fetch_add(&trace->id, 1, __ATOMIC_SEQ_CST);
}

void
ftl_trace_defrag_band(struct spdk_ftl_dev *dev, const struct ftl_band *band)
{
	struct ftl_trace *trace = &dev->stats.trace;

	spdk_trace_record(FTL_TRACE_BAND_DEFRAG(FTL_TRACE_SOURCE_INTERNAL),
			  ftl_trace_next_id(trace), 0, band->lba_map.num_vld, band->id);
}

void
ftl_trace_write_band(struct spdk_ftl_dev *dev, const struct ftl_band *band)
{
	struct ftl_trace *trace = &dev->stats.trace;

	spdk_trace_record(FTL_TRACE_BAND_WRITE(FTL_TRACE_SOURCE_INTERNAL),
			  ftl_trace_next_id(trace), 0, 0, band->id);
}

void
ftl_trace_lba_io_init(struct spdk_ftl_dev *dev, const struct ftl_io *io)
{
	uint16_t tpoint_id = 0, source;

	assert(io->trace != FTL_TRACE_INVALID_ID);
	source = ftl_trace_io_source(io);

	if (io->flags & FTL_IO_MD) {
		switch (io->type) {
		case FTL_IO_READ:
			tpoint_id = FTL_TRACE_MD_READ_SCHEDULE(source);
			break;
		case FTL_IO_WRITE:
			tpoint_id = FTL_TRACE_MD_WRITE_SCHEDULE(source);
			break;
		default:
			assert(0);
		}
	} else {
		switch (io->type) {
		case FTL_IO_READ:
			tpoint_id = FTL_TRACE_READ_SCHEDULE(source);
			break;
		case FTL_IO_WRITE:
			tpoint_id = FTL_TRACE_WRITE_SCHEDULE(source);
			break;
		default:
			assert(0);
		}
	}

	spdk_trace_record(tpoint_id, io->trace, io->num_blocks, 0, ftl_io_get_lba(io, 0));
}

void
ftl_trace_wbuf_fill(struct spdk_ftl_dev *dev, const struct ftl_io *io)
{
	assert(io->trace != FTL_TRACE_INVALID_ID);

	spdk_trace_record(FTL_TRACE_WRITE_WBUF_FILL(ftl_trace_io_source(io)), io->trace,
			  0, 0, ftl_io_current_lba(io));
}

void
ftl_trace_wbuf_pop(struct spdk_ftl_dev *dev, const struct ftl_wbuf_entry *entry)
{
	uint16_t tpoint_id;

	assert(entry->trace != FTL_TRACE_INVALID_ID);

	if (entry->io_flags & FTL_IO_INTERNAL) {
		tpoint_id = FTL_TRACE_WBUF_POP(FTL_TRACE_SOURCE_INTERNAL);
	} else {
		tpoint_id = FTL_TRACE_WBUF_POP(FTL_TRACE_SOURCE_USER);
	}

	spdk_trace_record(tpoint_id, entry->trace, 0, entry->addr.offset, entry->lba);
}

void
ftl_trace_completion(struct spdk_ftl_dev *dev, const struct ftl_io *io,
		     enum ftl_trace_completion completion)
{
	uint16_t tpoint_id = 0, source;

	assert(io->trace != FTL_TRACE_INVALID_ID);
	source = ftl_trace_io_source(io);

	if (io->flags & FTL_IO_MD) {
		switch (io->type) {
		case FTL_IO_READ:
			tpoint_id = FTL_TRACE_MD_READ_COMPLETION(source);
			break;
		case FTL_IO_WRITE:
			tpoint_id = FTL_TRACE_MD_WRITE_COMPLETION(source);
			break;
		default:
			assert(0);
		}
	} else {
		switch (io->type) {
		case FTL_IO_READ:
			switch (completion) {
			case FTL_TRACE_COMPLETION_INVALID:
				tpoint_id = FTL_TRACE_READ_COMPLETION_INVALID(source);
				break;
			case FTL_TRACE_COMPLETION_CACHE:
				tpoint_id = FTL_TRACE_READ_COMPLETION_CACHE(source);
				break;
			case FTL_TRACE_COMPLETION_DISK:
				tpoint_id = FTL_TRACE_READ_COMPLETION_DISK(source);
				break;
			}
			break;
		case FTL_IO_WRITE:
			tpoint_id = FTL_TRACE_WRITE_COMPLETION(source);
			break;
		case FTL_IO_ERASE:
			tpoint_id = FTL_TRACE_ERASE_COMPLETION(source);
			break;
		default:
			assert(0);
		}
	}

	spdk_trace_record(tpoint_id, io->trace, 0, 0, ftl_io_get_lba(io, io->pos - 1));
}

void
ftl_trace_submission(struct spdk_ftl_dev *dev, const struct ftl_io *io, struct ftl_addr addr,
		     size_t addr_cnt)
{
	uint16_t tpoint_id = 0, source;

	assert(io->trace != FTL_TRACE_INVALID_ID);
	source = ftl_trace_io_source(io);

	if (io->flags & FTL_IO_MD) {
		switch (io->type) {
		case FTL_IO_READ:
			tpoint_id = FTL_TRACE_MD_READ_SUBMISSION(source);
			break;
		case FTL_IO_WRITE:
			tpoint_id = FTL_TRACE_MD_WRITE_SUBMISSION(source);
			break;
		default:
			assert(0);
		}
	} else {
		switch (io->type) {
		case FTL_IO_READ:
			tpoint_id = FTL_TRACE_READ_SUBMISSION(source);
			break;
		case FTL_IO_WRITE:
			tpoint_id = FTL_TRACE_WRITE_SUBMISSION(source);
			break;
		case FTL_IO_ERASE:
			tpoint_id = FTL_TRACE_ERASE_SUBMISSION(source);
			break;
		default:
			assert(0);
		}
	}

	spdk_trace_record(tpoint_id, io->trace, addr_cnt, 0, addr.offset);
}

void
ftl_trace_limits(struct spdk_ftl_dev *dev, int limit, size_t num_free)
{
	struct ftl_trace *trace = &dev->stats.trace;

	spdk_trace_record(FTL_TRACE_LIMITS(FTL_TRACE_SOURCE_INTERNAL), ftl_trace_next_id(trace),
			  num_free, limit, 0);
}

uint64_t
ftl_trace_alloc_id(struct spdk_ftl_dev *dev)
{
	struct ftl_trace *trace = &dev->stats.trace;

	return ftl_trace_next_id(trace);
}

#endif /* defined(DEBUG) */
