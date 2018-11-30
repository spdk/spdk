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
#include "ftl_rwb.h"

typedef _Atomic uint64_t atomic_uint64_t;

struct ftl_trace {
	/* Monotonically incrementing event id */
	atomic_uint64_t		id;
};

#define OBJECT_FTL_IO				0x50
#define OBJECT_RWB_IO				0x51

#define	TRACE_GROUP_FTL 0x6

#define FTL_TPOINT_ID(type, source, tpoint)	SPDK_TPOINT_ID(TRACE_GROUP_FTL, ((source * 16) + (type * 8) + tpoint))

#define FTL_TRACE_TYPE_BAND_DEFRAG		FTL_TPOINT_ID(FTL_TRACE_TYPE_OTHER, FTL_TRACE_SOURCE_INTERNAL, 0x0)
#define FTL_TRACE_TYPE_BAND_WRITE		FTL_TPOINT_ID(FTL_TRACE_TYPE_OTHER, FTL_TRACE_SOURCE_INTERNAL, 0x1)
#define FTL_TRACE_TYPE_APPLIED_LIMITS		FTL_TPOINT_ID(FTL_TRACE_TYPE_OTHER, FTL_TRACE_SOURCE_INTERNAL, 0x2)
#define FTL_TRACE_POINT_RWB_POP			FTL_TPOINT_ID(FTL_TRACE_TYPE_OTHER, FTL_TRACE_SOURCE_INTERNAL, 0x3)

SPDK_TRACE_REGISTER_FN(ftl_trace_func, "ftl", TRACE_GROUP_FTL)
{
	enum ftl_trace_type type;
	enum ftl_trace_source source;

	spdk_trace_register_owner(FTL_TRACE_SOURCE_INTERNAL, 'i');
	spdk_trace_register_owner(FTL_TRACE_SOURCE_USER, 'u');
	spdk_trace_register_object(OBJECT_FTL_IO, 'f');

	spdk_trace_register_description("FTL_TRACE_TYPE_BAND_DEFRAG", "", FTL_TRACE_TYPE_BAND_DEFRAG,
					FTL_TRACE_SOURCE_INTERNAL, OBJECT_FTL_IO, 0, 0, "band: ");
	spdk_trace_register_description("FTL_TRACE_TYPE_BAND_WRITE", "", FTL_TRACE_TYPE_BAND_WRITE,
					FTL_TRACE_SOURCE_INTERNAL, OBJECT_FTL_IO, 0, 0, "band: ");
	spdk_trace_register_description("FTL_TRACE_TYPE_APPLIED_LIMITS", "", FTL_TRACE_TYPE_APPLIED_LIMITS,
					FTL_TRACE_SOURCE_INTERNAL, OBJECT_FTL_IO, 0, 0, "limits: ");
	spdk_trace_register_description("FTL_TRACE_POINT_RWB_POP", "", FTL_TRACE_POINT_RWB_POP,
					FTL_TRACE_SOURCE_INTERNAL, OBJECT_RWB_IO, 0, 0, "lba: ");

	for (source = FTL_TRACE_SOURCE_INTERNAL; source <= FTL_TRACE_SOURCE_INTERNAL; source++) {
		for (type = FTL_TRACE_TYPE_READ; type < FTL_TRACE_TYPE_OTHER; type++) {
			/* TODO: Figure out better way to describe each of the traces */
			spdk_trace_register_description("FTL_TRACE_POINT_SCHEDULED", "",
							FTL_TPOINT_ID(type, source, FTL_TRACE_POINT_SCHEDULED),
							source, OBJECT_FTL_IO, 0, 0, "lba: ");
			spdk_trace_register_description("FTL_TRACE_POINT_RWB_FILL", "",
							FTL_TPOINT_ID(type, source, FTL_TRACE_POINT_RWB_FILL),
							source, OBJECT_FTL_IO, 0, 0, "lba: ");
			spdk_trace_register_description("FTL_TRACE_POINT_SUBMISSION", "",
							FTL_TPOINT_ID(type, source, FTL_TRACE_POINT_SUBMISSION),
							source, OBJECT_FTL_IO, 0, 0, "ppa: ");
			spdk_trace_register_description("FTL_TRACE_COMPLETION_INVALID", "",
							FTL_TPOINT_ID(type, source, FTL_TRACE_COMPLETION_INVALID),
							source, OBJECT_FTL_IO, 0, 0, "lba: ");
			spdk_trace_register_description("FTL_TRACE_COMPLETION_CACHE", "",
							FTL_TPOINT_ID(type, source, FTL_TRACE_COMPLETION_CACHE),
							source, OBJECT_FTL_IO, 0, 0, "lba: ");
			spdk_trace_register_description("FTL_TRACE_COMPLETION_DISK", "",
							FTL_TPOINT_ID(type, source, FTL_TRACE_COMPLETION_DISK),
							source, OBJECT_FTL_IO, 0, 0, "lba: ");
		}
	}
}

static uint64_t
ftl_trace_next_id(struct ftl_trace *trace)
{
	assert(trace->id != FTL_TRACE_INVALID_ID);
	return atomic_fetch_add(&trace->id, 1);
}

static uint8_t
ftl_io2trace_source(const struct ftl_io *io)
{
	if (io->flags & FTL_IO_INTERNAL) {
		return FTL_TRACE_SOURCE_INTERNAL;
	} else {
		return FTL_TRACE_SOURCE_USER;
	}
}

static uint64_t
ftl_io2trace_type(const struct ftl_io *io, enum ftl_trace_point point)
{
	static const enum ftl_trace_type type[][2] = {
		[FTL_IO_READ][0]	= FTL_TRACE_TYPE_READ,
		[FTL_IO_READ][1]	= FTL_TRACE_TYPE_MD_READ,
		[FTL_IO_WRITE][0]	= FTL_TRACE_TYPE_WRITE,
		[FTL_IO_WRITE][1]	= FTL_TRACE_TYPE_MD_WRITE,
		[FTL_IO_ERASE][0]	= FTL_TRACE_TYPE_ERASE,
		[FTL_IO_ERASE][1]	= FTL_TRACE_TYPE_ERASE,
	};

	return FTL_TPOINT_ID(type[io->type][io->flags & FTL_IO_MD], ftl_io2trace_source(io), point);
}

void
ftl_trace_defrag_band(struct spdk_ftl_dev *dev, const struct ftl_band *band)
{
	struct ftl_trace *trace = dev->stats.trace;

	if (!trace) {
		return;
	}

	spdk_trace_record(FTL_TRACE_TYPE_BAND_DEFRAG, ftl_trace_next_id(trace), 0, band->id,
			  band->md.num_vld);
}

void
ftl_trace_write_band(struct spdk_ftl_dev *dev, const struct ftl_band *band)
{
	struct ftl_trace *trace = dev->stats.trace;

	if (!trace) {
		return;
	}

	spdk_trace_record(FTL_TRACE_TYPE_BAND_WRITE, ftl_trace_next_id(trace), 0, band->id, 0);
}

void
ftl_trace_lba_io_init(struct spdk_ftl_dev *dev, const struct ftl_io *io)
{
	struct ftl_trace *trace = dev->stats.trace;
	uint64_t type;

	if (!trace) {
		return;
	}

	assert(io->trace != FTL_TRACE_INVALID_ID);
	type = ftl_io2trace_type(io, FTL_TRACE_POINT_SCHEDULED);

	spdk_trace_record(type, io->trace, io->lbk_cnt, io->lba, 0);
}

void
ftl_trace_rwb_fill(struct spdk_ftl_dev *dev, const struct ftl_io *io)
{
	struct ftl_trace *trace = dev->stats.trace;
	uint64_t type;

	if (!trace) {
		return;
	}

	assert(io->trace != FTL_TRACE_INVALID_ID);
	type = ftl_io2trace_type(io, FTL_TRACE_POINT_RWB_FILL);

	spdk_trace_record(type, io->trace, 0, io->lba, 0);
}

void ftl_trace_rwb_pop(struct spdk_ftl_dev *dev, const struct ftl_rwb_entry *entry)
{
	struct ftl_trace *trace = dev->stats.trace;

	if (!trace) {
		return;
	}

	assert(entry->trace != FTL_TRACE_INVALID_ID);

	spdk_trace_record(FTL_TRACE_POINT_RWB_POP, entry->trace, 0,
			  entry->lba, (uint64_t)entry->ppa.ppa);
}

void
ftl_trace_completion(struct spdk_ftl_dev *dev, const struct ftl_io *io,
		     enum ftl_trace_completion completion)
{
	struct ftl_trace *trace = dev->stats.trace;
	uint64_t type;

	if (!trace) {
		return;
	}

	assert(io->trace != FTL_TRACE_INVALID_ID);
	type = ftl_io2trace_type(io, (enum ftl_trace_point)completion);

	spdk_trace_record(type, io->trace, 0, io->lba, 0);
}

void
ftl_trace_submission(struct spdk_ftl_dev *dev, const struct ftl_io *io, struct ftl_ppa ppa,
		     size_t ppa_cnt)
{
	struct ftl_trace *trace = dev->stats.trace;
	uint64_t type;

	if (!trace) {
		return;
	}

	assert(io->trace != FTL_TRACE_INVALID_ID);
	type = ftl_io2trace_type(io, FTL_TRACE_POINT_SUBMISSION);

	spdk_trace_record(type, io->trace, ppa_cnt, ppa.ppa, 0);
}

void
ftl_trace_limits(struct spdk_ftl_dev *dev, const size_t *limits, size_t num_free)
{
	struct ftl_trace *trace = dev->stats.trace;

	if (!trace) {
		return;
	}

	spdk_trace_record(FTL_TRACE_TYPE_APPLIED_LIMITS, ftl_trace_next_id(trace), num_free,
			  limits[FTL_RWB_TYPE_USER], limits[FTL_RWB_TYPE_INTERNAL]);
}

uint64_t
ftl_trace_alloc_group(struct spdk_ftl_dev *dev)
{
	struct ftl_trace *trace = dev->stats.trace;

	if (!trace) {
		return FTL_TRACE_INVALID_ID;
	}

	return ftl_trace_next_id(trace);
}

struct ftl_trace *
ftl_trace_init(void)
{
	return calloc(1, sizeof(struct ftl_trace));
}

void
ftl_trace_free(struct ftl_trace *trace)
{
	free(trace);
}
