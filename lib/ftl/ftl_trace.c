/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/trace.h"
#include "spdk_internal/trace_defs.h"

#include "ftl_core.h"
#include "ftl_trace.h"
#include "ftl_io.h"
#include "ftl_band.h"

#if defined(DEBUG)

enum ftl_trace_source {
	FTL_TRACE_SOURCE_INTERNAL,
	FTL_TRACE_SOURCE_USER,
	FTL_TRACE_SOURCE_MAX,
};

#define FTL_TPOINT_ID(id, src) SPDK_TPOINT_ID(TRACE_GROUP_FTL, (((id) << 1) | (!!(src))))

#define FTL_TRACE_BAND_RELOC(src)		FTL_TPOINT_ID(0, src)
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

#define FTL_TRACE_UNMAP_SCHEDULE(src)		FTL_TPOINT_ID(19, src)
#define FTL_TRACE_UNMAP_SUBMISSION(src)		FTL_TPOINT_ID(20, src)
#define FTL_TRACE_UNMAP_COMPLETION(src)		FTL_TPOINT_ID(21, src)

SPDK_TRACE_REGISTER_FN(ftl_trace_func, "ftl", TRACE_GROUP_FTL)
{
	const char source[] = { 'i', 'u' };
	char descbuf[128];
	int i;

	spdk_trace_register_owner(OWNER_FTL, 'f');

	for (i = 0; i < FTL_TRACE_SOURCE_MAX; ++i) {
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "band_reloc");
		spdk_trace_register_description(descbuf, FTL_TRACE_BAND_RELOC(i), OWNER_FTL, OBJECT_NONE, 0, 0,
						"band: ");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "band_write");
		spdk_trace_register_description(descbuf, FTL_TRACE_BAND_WRITE(i), OWNER_FTL, OBJECT_NONE, 0, 0,
						"band: ");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "limits");
		spdk_trace_register_description(descbuf, FTL_TRACE_LIMITS(i), OWNER_FTL, OBJECT_NONE, 0, 0,
						"limits: ");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "rwb_pop");
		spdk_trace_register_description(descbuf, FTL_TRACE_WBUF_POP(i), OWNER_FTL, OBJECT_NONE, 0, 0,
						"lba: ");

		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "md_read_sched");
		spdk_trace_register_description(descbuf, FTL_TRACE_MD_READ_SCHEDULE(i), OWNER_FTL, OBJECT_NONE, 0,
						0, "addr: ");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "md_read_submit");
		spdk_trace_register_description(descbuf, FTL_TRACE_MD_READ_SUBMISSION(i), OWNER_FTL, OBJECT_NONE, 0,
						0, "addr: ");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "md_read_cmpl");
		spdk_trace_register_description(descbuf, FTL_TRACE_MD_READ_COMPLETION(i), OWNER_FTL, OBJECT_NONE, 0,
						0, "lba: ");

		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "md_write_sched");
		spdk_trace_register_description(descbuf, FTL_TRACE_MD_WRITE_SCHEDULE(i), OWNER_FTL, OBJECT_NONE, 0,
						0, "addr: ");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "md_write_submit");
		spdk_trace_register_description(descbuf, FTL_TRACE_MD_WRITE_SUBMISSION(i), OWNER_FTL, OBJECT_NONE,
						0, 0, "addr: ");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "md_write_cmpl");
		spdk_trace_register_description(descbuf, FTL_TRACE_MD_WRITE_COMPLETION(i), OWNER_FTL, OBJECT_NONE,
						0, 0, "lba: ");

		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "read_sched");
		spdk_trace_register_description(descbuf, FTL_TRACE_READ_SCHEDULE(i), OWNER_FTL, OBJECT_NONE, 0, 0,
						"lba: ");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "read_submit");
		spdk_trace_register_description(descbuf, FTL_TRACE_READ_SUBMISSION(i), OWNER_FTL, OBJECT_NONE, 0, 0,
						"addr: ");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "read_cmpl_invld");
		spdk_trace_register_description(descbuf, FTL_TRACE_READ_COMPLETION_INVALID(i), OWNER_FTL,
						OBJECT_NONE, 0, 0, "lba: ");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "read_cmpl_cache");
		spdk_trace_register_description(descbuf, FTL_TRACE_READ_COMPLETION_CACHE(i), OWNER_FTL, OBJECT_NONE,
						0, 0, "lba: ");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "read_cmpl_ssd");
		spdk_trace_register_description(descbuf, FTL_TRACE_READ_COMPLETION_DISK(i), OWNER_FTL, OBJECT_NONE,
						0, 0, "lba: ");

		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "write_sched");
		spdk_trace_register_description(descbuf, FTL_TRACE_WRITE_SCHEDULE(i), OWNER_FTL, OBJECT_NONE, 0, 0,
						"lba: ");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "rwb_fill");
		spdk_trace_register_description(descbuf, FTL_TRACE_WRITE_WBUF_FILL(i), OWNER_FTL, OBJECT_NONE, 0, 0,
						"lba: ");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "write_submit");
		spdk_trace_register_description(descbuf, FTL_TRACE_WRITE_SUBMISSION(i), OWNER_FTL, OBJECT_NONE, 0,
						0, "addr: ");
		snprintf(descbuf, sizeof(descbuf), "%c %s", source[i], "write_cmpl");
		spdk_trace_register_description(descbuf, FTL_TRACE_WRITE_COMPLETION(i), OWNER_FTL, OBJECT_NONE, 0,
						0, "lba: ");
	}
}

static uint64_t
ftl_trace_next_id(struct ftl_trace *trace)
{
	assert(trace->id != FTL_TRACE_INVALID_ID);
	return __atomic_fetch_add(&trace->id, 1, __ATOMIC_SEQ_CST);
}

void
ftl_trace_reloc_band(struct spdk_ftl_dev *dev, const struct ftl_band *band)
{
	struct ftl_trace *trace = &dev->trace;

	spdk_trace_record(FTL_TRACE_BAND_RELOC(FTL_TRACE_SOURCE_INTERNAL), ftl_trace_next_id(trace), 0,
			  band->p2l_map.num_valid, band->id);
}

void
ftl_trace_write_band(struct spdk_ftl_dev *dev, const struct ftl_band *band)
{
	struct ftl_trace *trace = &dev->trace;

	spdk_trace_record(FTL_TRACE_BAND_WRITE(FTL_TRACE_SOURCE_INTERNAL), ftl_trace_next_id(trace), 0, 0,
			  band->id);
}

void
ftl_trace_lba_io_init(struct spdk_ftl_dev *dev, const struct ftl_io *io)
{
	uint16_t tpoint_id = 0, source;

	assert(io->trace != FTL_TRACE_INVALID_ID);
	source = FTL_TRACE_SOURCE_USER;

	switch (io->type) {
	case FTL_IO_READ:
		tpoint_id = FTL_TRACE_READ_SCHEDULE(source);
		break;
	case FTL_IO_WRITE:
		tpoint_id = FTL_TRACE_WRITE_SCHEDULE(source);
		break;
	case FTL_IO_UNMAP:
		tpoint_id = FTL_TRACE_UNMAP_SCHEDULE(source);
		break;
	default:
		assert(0);
	}

	spdk_trace_record(tpoint_id, io->trace, io->num_blocks, 0, ftl_io_get_lba(io, 0));
}

void
ftl_trace_completion(struct spdk_ftl_dev *dev, const struct ftl_io *io,
		     enum ftl_trace_completion completion)
{
	uint16_t tpoint_id = 0, source;

	assert(io->trace != FTL_TRACE_INVALID_ID);
	source = FTL_TRACE_SOURCE_USER;

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
	case FTL_IO_UNMAP:
		tpoint_id = FTL_TRACE_UNMAP_COMPLETION(source);
		break;
	default:
		assert(0);
	}

	spdk_trace_record(tpoint_id, io->trace, 0, 0, ftl_io_get_lba(io, io->pos - 1));
}

void
ftl_trace_submission(struct spdk_ftl_dev *dev, const struct ftl_io *io, ftl_addr addr,
		     size_t addr_cnt)
{
	uint16_t tpoint_id = 0, source;

	assert(io->trace != FTL_TRACE_INVALID_ID);
	source = FTL_TRACE_SOURCE_USER;

	switch (io->type) {
	case FTL_IO_READ:
		tpoint_id = FTL_TRACE_READ_SUBMISSION(source);
		break;
	case FTL_IO_WRITE:
		tpoint_id = FTL_TRACE_WRITE_SUBMISSION(source);
		break;
	case FTL_IO_UNMAP:
		tpoint_id = FTL_TRACE_UNMAP_SUBMISSION(source);
		break;
	default:
		assert(0);
	}

	spdk_trace_record(tpoint_id, io->trace, addr_cnt, 0, addr);
}

void
ftl_trace_limits(struct spdk_ftl_dev *dev, int limit, size_t num_free)
{
	struct ftl_trace *trace = &dev->trace;

	spdk_trace_record(FTL_TRACE_LIMITS(FTL_TRACE_SOURCE_INTERNAL), ftl_trace_next_id(trace), num_free,
			  limit, 0);
}

uint64_t
ftl_trace_alloc_id(struct spdk_ftl_dev *dev)
{
	struct ftl_trace *trace = &dev->trace;

	return ftl_trace_next_id(trace);
}

#endif /* defined(DEBUG) */
