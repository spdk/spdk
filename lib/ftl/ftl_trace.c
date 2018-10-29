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

#include <spdk/stdinc.h>
#include <spdk/likely.h>
#include <spdk/env.h>
#include <spdk/log.h>
#include <spdk/ftl.h>
#include "ftl_trace.h"
#include "ftl_utils.h"
#include "ftl_io.h"
#include "ftl_band.h"
#include "ftl_rwb.h"

#if enabled(FTL_TRACE)

/* Size of the comm pipe */
#define FTL_TRACE_RING_SIZE	4096
/* Number of events in the pool */
#define FTL_TRACE_EVENT_CNT	(1024*64)
/* Maximum event size */
#define FTL_TRACE_EVENT_SIZE	64
/* Size of the mapped trace file */
#define FTL_TRACE_MAP_SIZE	(1024*1024*64)

typedef _Atomic uint64_t atomic_uint64_t;

struct ftl_trace {
	/* Thread descriptor */
	struct ftl_thread	*thread;

	/* Event memory pool */
	struct spdk_mempool	*pool;

	/* Buffer for dumping entries */
	void			*buf;

	/* Offset within the dump buffer */
	size_t			offset;

	/* Trace file descriptor */
	int			fd;

	/* Monotonically incrementing event id */
	atomic_uint64_t		id;

	/* Number of events lost */
	atomic_uint64_t		num_lost;
};

static uint64_t
ftl_trace_next_id(struct ftl_trace *trace)
{
	return atomic_fetch_add(&trace->id, 1);
}

static void *
ftl_event_get_buf(const struct ftl_event *event)
{
	return (char *)(event + 1) + event->size;
}

static size_t
ftl_event_size(struct ftl_event *event)
{
	return sizeof(*event) + event->size;
}

static size_t
ftl_trace_current_offset(struct ftl_trace *trace)
{
	return (size_t)(trace->offset % FTL_TRACE_MAP_SIZE);
}

static void
_ftl_event_add_data(struct ftl_event *event, uint8_t type,
		    const void *buf, size_t size)
{
	assert(ftl_event_size(event) + size + sizeof(type) <= FTL_TRACE_EVENT_SIZE);
	memcpy(ftl_event_get_buf(event), &type, sizeof(type));
	event->size += sizeof(type);
	memcpy(ftl_event_get_buf(event), buf, size);
	event->size += size;
}

#define ftl_event_add_data(event, t, v) \
	_ftl_event_add_data(event, t, v, sizeof(*(v)))

static void
ftl_trace_send_event(struct ftl_trace *trace, struct ftl_event *event)
{
	size_t num_sent;

	num_sent = spdk_ring_enqueue(trace->thread->ring, (void **)&event, 1);
	if (num_sent != 1) {
		atomic_fetch_add(&trace->num_lost, 1);
	}
}

static struct ftl_event *
ftl_event_init(struct ftl_trace *trace, uint8_t src,
	       ftl_trace_group_t id)
{
	struct ftl_event *event;
	struct timespec ts;

	event = spdk_mempool_get(trace->pool);
	if (spdk_unlikely(!event)) {
		atomic_fetch_add(&trace->num_lost, 1);
		return NULL;
	}

	if (spdk_unlikely(clock_gettime(CLOCK_MONOTONIC_RAW, &ts))) {
		atomic_fetch_add(&trace->num_lost, 1);
		spdk_mempool_put(trace->pool, event);
		return NULL;
	}

	event->ts = ts.tv_sec * 1000000UL + ts.tv_nsec / 1000UL;

	if (id != FTL_TRACE_INVALID_ID) {
		event->id = id;
	} else {
		event->id = ftl_trace_next_id(trace);
	}

	event->size = 0;
	ftl_event_add_data(event, FTL_TRACE_DATA_SOURCE, &src);
	return event;
}

void
ftl_trace_defrag_band(struct ftl_trace *trace, const struct ftl_band *band)
{
	struct ftl_event *event;
	uint16_t band16;
	uint32_t num_vld32;
	uint8_t type;

	event = ftl_event_init(trace, FTL_TRACE_SOURCE_INTERNAL, FTL_TRACE_INVALID_ID);
	if (spdk_unlikely(!event)) {
		return;
	}

	assert(band->id	<= UINT16_MAX);
	assert(band->md.num_vld <= UINT32_MAX);
	band16		= (uint16_t)band->id;
	num_vld32	= (uint32_t)band->md.num_vld;
	type		= FTL_TRACE_TYPE_BAND_DEFRAG;

	ftl_event_add_data(event, FTL_TRACE_DATA_TRACE_TYPE, &type);
	ftl_event_add_data(event, FTL_TRACE_DATA_BAND_ID, &band16);
	ftl_event_add_data(event, FTL_TRACE_DATA_VLD_CNT, &num_vld32);
	ftl_trace_send_event(trace, event);
}

void
ftl_trace_write_band(struct ftl_trace *trace, const struct ftl_band *band)
{
	struct ftl_event *event;
	uint16_t band16;
	uint8_t type;

	event = ftl_event_init(trace, FTL_TRACE_SOURCE_INTERNAL, FTL_TRACE_INVALID_ID);
	if (spdk_unlikely(!event)) {
		return;
	}

	assert(band->id	<= UINT16_MAX);
	band16		= (uint16_t)band->id;
	type		= FTL_TRACE_TYPE_BAND_WRITE;

	ftl_event_add_data(event, FTL_TRACE_DATA_TRACE_TYPE, &type);
	ftl_event_add_data(event, FTL_TRACE_DATA_BAND_ID, &band16);
	ftl_trace_send_event(trace, event);
}

static uint8_t
ftl_io2trace_type(const struct ftl_io *io)
{
	static const enum ftl_trace_type type[][2] = {
		[FTL_IO_READ][0]	= FTL_TRACE_TYPE_READ,
		[FTL_IO_READ][1]	= FTL_TRACE_TYPE_MD_READ,
		[FTL_IO_WRITE][0]	= FTL_TRACE_TYPE_WRITE,
		[FTL_IO_WRITE][1]	= FTL_TRACE_TYPE_MD_WRITE,
		[FTL_IO_ERASE][0]	= FTL_TRACE_TYPE_ERASE,
		[FTL_IO_ERASE][1]	= FTL_TRACE_TYPE_ERASE,
	};

	return type[io->type][ftl_io_md(io)];
}

static uint8_t
ftl_io2trace_source(const struct ftl_io *io)
{
	if (ftl_io_internal(io)) {
		return FTL_TRACE_SOURCE_INTERNAL;
	} else {
		return FTL_TRACE_SOURCE_USER;
	}
}

static struct ftl_event *
ftl_io_event_init(struct ftl_trace *trace, const struct ftl_io *io,
		  enum ftl_trace_point point)
{
	struct ftl_event *event;
	uint8_t type8, point8;

	event = ftl_event_init(trace, ftl_io2trace_source(io), io->trace);
	if (spdk_unlikely(!event)) {
		return NULL;
	}

	type8	= (uint8_t)ftl_io2trace_type(io);
	point8	= point;

	ftl_event_add_data(event, FTL_TRACE_DATA_TRACE_TYPE, &type8);
	ftl_event_add_data(event, FTL_TRACE_DATA_TRACE_POINT, &point8);
	return event;
}

void
ftl_trace_lba_io_init(struct ftl_trace *trace, const struct ftl_io *io)
{
	struct ftl_event *event;
	uint8_t lbk_cnt = (uint8_t)io->lbk_cnt;

	event = ftl_io_event_init(trace, io, FTL_TRACE_POINT_SCHEDULED);
	if (spdk_unlikely(!event)) {
		return;
	}

	ftl_event_add_data(event, FTL_TRACE_DATA_LBA, &io->lba);
	ftl_event_add_data(event, FTL_TRACE_DATA_LBK_CNT, &lbk_cnt);
	ftl_trace_send_event(trace, event);
}

void
ftl_trace_rwb_fill(struct ftl_trace *trace, const struct ftl_io *io)
{
	struct ftl_event *event;

	event = ftl_io_event_init(trace, io, FTL_TRACE_POINT_RWB_FILL);
	if (spdk_unlikely(!event)) {
		return;
	}

	ftl_event_add_data(event, FTL_TRACE_DATA_LBA, &io->lba);
	ftl_trace_send_event(trace, event);
}

void ftl_trace_rwb_pop(struct ftl_trace *trace,
		       const struct ftl_rwb_entry *entry)
{
	struct ftl_event *event;
	uint8_t point, type;

	event = ftl_event_init(trace, FTL_TRACE_SOURCE_INTERNAL, entry->trace);
	if (spdk_unlikely(!event)) {
		return;
	}

	point	= FTL_TRACE_POINT_RWB_POP;
	type	= FTL_TRACE_TYPE_WRITE;

	ftl_event_add_data(event, FTL_TRACE_DATA_TRACE_TYPE, &type);
	ftl_event_add_data(event, FTL_TRACE_DATA_TRACE_POINT, &point);
	ftl_event_add_data(event, FTL_TRACE_DATA_LBA, &entry->lba);
	ftl_event_add_data(event, FTL_TRACE_DATA_PPA, &entry->ppa);
	ftl_trace_send_event(trace, event);
}

void
ftl_trace_completion(struct ftl_trace *trace, const struct ftl_io *io,
		     enum ftl_trace_completion type)
{
	struct ftl_event *event;
	uint8_t type8;

	event = ftl_io_event_init(trace, io, FTL_TRACE_POINT_COMPLETION);
	if (spdk_unlikely(!event)) {
		return;
	}

	type8 = (uint8_t)type;
	ftl_event_add_data(event, FTL_TRACE_DATA_LBA, &io->lba);
	ftl_event_add_data(event, FTL_TRACE_DATA_COMPLETION, &type8);
	ftl_trace_send_event(trace, event);
}

void
ftl_trace_submission(struct ftl_trace *trace, const struct ftl_io *io,
		     struct ftl_ppa ppa, size_t ppa_cnt)
{
	struct ftl_event *event;
	uint8_t ppa_cnt8;

	event = ftl_io_event_init(trace, io, FTL_TRACE_POINT_SUBMISSION);
	if (spdk_unlikely(!event)) {
		return;
	}

	ppa_cnt8 = (uint8_t)ppa_cnt;
	ftl_event_add_data(event, FTL_TRACE_DATA_PPA, &ppa);
	ftl_event_add_data(event, FTL_TRACE_DATA_LBK_CNT, &ppa_cnt8);
	ftl_trace_send_event(trace, event);
}

void
ftl_trace_limits(struct ftl_trace *trace, const size_t *limits,
		 size_t num_free)
{
	struct ftl_event *event;
	uint16_t user16, internal16, num_free16;
	uint8_t type;

	event = ftl_event_init(trace, FTL_TRACE_SOURCE_INTERNAL, FTL_TRACE_INVALID_ID);
	if (spdk_unlikely(!event)) {
		return;
	}

	assert(limits[FTL_RWB_TYPE_USER]	<= UINT16_MAX);
	assert(limits[FTL_RWB_TYPE_INTERNAL]	<= UINT16_MAX);
	assert(num_free				<= UINT16_MAX);
	user16		= (uint16_t)limits[FTL_RWB_TYPE_USER];
	internal16	= (uint16_t)limits[FTL_RWB_TYPE_INTERNAL];
	num_free16	= (uint16_t)num_free;
	type		= FTL_TRACE_TYPE_APPLIED_LIMITS;

	ftl_event_add_data(event, FTL_TRACE_DATA_TRACE_TYPE, &type);
	ftl_event_add_data(event, FTL_TRACE_DATA_RWB_USER_SIZE, &user16);
	ftl_event_add_data(event, FTL_TRACE_DATA_RWB_INTERNAL_SIZE, &internal16);
	ftl_event_add_data(event, FTL_TRACE_DATA_BAND_CNT, &num_free16);
	ftl_trace_send_event(trace, event);
}

static int
ftl_trace_map_file(struct ftl_trace *trace)
{
	int errsv;

	if (trace->buf) {
		if (munmap(trace->buf, FTL_TRACE_MAP_SIZE)) {
			SPDK_ERRLOG("Failed to unmap the trace file\n");
			return -1;
		}
	}

	assert(trace->offset % FTL_TRACE_MAP_SIZE == 0);

	if (fallocate(trace->fd, 0, trace->offset, FTL_TRACE_MAP_SIZE)) {
		SPDK_ERRLOG("Failed to allocate space for the trace file\n");
		return -1;
	}


	trace->buf = mmap(NULL, FTL_TRACE_MAP_SIZE, PROT_WRITE | PROT_READ,
			  MAP_SHARED, trace->fd, trace->offset);

	if (trace->buf == MAP_FAILED) {
		errsv = errno;
		SPDK_ERRLOG("Failed to map the trace file: %s\n", strerror(errsv));
		trace->buf = NULL;
		return -1;
	}
	return 0;
}

static void
ftl_trace_dump_event(struct ftl_trace *trace, struct ftl_event *event)
{
	size_t size, offset;
	void *buf;

	buf = event;
	size = ftl_event_size(event);

	assert(size <= FTL_TRACE_EVENT_SIZE);

	if (ftl_trace_current_offset(trace) + size > FTL_TRACE_MAP_SIZE) {
		offset = FTL_TRACE_MAP_SIZE - ftl_trace_current_offset(trace);
		memcpy(trace->buf + ftl_trace_current_offset(trace), event, offset);
		trace->offset += offset;

		if (ftl_trace_map_file(trace)) {
			assert(0 && "Failed to map the trace file");
			return;
		}

		buf = (char *)buf + offset;
		size -= offset;
	}

	memcpy(trace->buf + ftl_trace_current_offset(trace), buf, size);
	trace->offset += size;

	spdk_mempool_put(trace->pool, event);
}

static void
ftl_trace_loop(void *ctx)
{
#define FTL_TRACE_MAX_EVENTS 64
	struct ftl_trace *trace = ctx;
	struct ftl_event *event[FTL_TRACE_MAX_EVENTS];
	size_t num_events, i;

	while (ftl_thread_running(trace->thread)) {
		num_events = spdk_ring_dequeue(trace->thread->ring, (void **)&event,
					       FTL_TRACE_MAX_EVENTS);

		for (i = 0; i < num_events; ++i) {
			ftl_trace_dump_event(trace, event[i]);
		}
	}
}

ftl_trace_group_t
ftl_trace_alloc_group(struct ftl_trace *trace)
{
	if (!trace) {
		return FTL_TRACE_INVALID_ID;
	}

	return ftl_trace_next_id(trace);
}

struct ftl_trace *
ftl_trace_init(const char *fname)
{
	struct ftl_trace *trace;
	int errsv;

	trace = calloc(1, sizeof(*trace));
	trace->fd = open(fname, O_RDWR | O_TRUNC | O_CREAT, 0660);
	if (trace->fd < 0) {
		errsv = errno;
		SPDK_ERRLOG("%s: %s\n", fname, strerror(errsv));
		goto error;
	}

	if (ftl_trace_map_file(trace)) {
		goto error;
	}

	trace->pool = spdk_mempool_create("ocssd-trace", FTL_TRACE_EVENT_CNT,
					  FTL_TRACE_EVENT_SIZE,
					  SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					  SPDK_ENV_SOCKET_ID_ANY);
	if (!trace->pool) {
		goto error;
	}

	trace->thread = ftl_thread_init("ocssd-trace", FTL_TRACE_RING_SIZE,
					ftl_trace_loop, trace, 0);
	if (!trace->thread) {
		goto error;
	}

	if (ftl_thread_start(trace->thread)) {
		goto error;
	}

	return trace;
error:
	ftl_trace_free(trace);
	return NULL;
}

void
ftl_trace_free(struct ftl_trace *trace)
{
	if (!trace) {
		return;
	}

	if (trace->thread) {
		ftl_thread_stop(trace->thread);
		ftl_thread_join(trace->thread);
		ftl_thread_free(trace->thread);
	}

	if (trace->pool) {
		spdk_mempool_free(trace->pool);
	}

	if (trace->buf) {
		munmap(trace->buf, FTL_TRACE_MAP_SIZE);
	}

	if (trace->fd > 0) {
		close(trace->fd);
	}

	free(trace);
}

#endif /* enabled(FTL_TRACE) */
