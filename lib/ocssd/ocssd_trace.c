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
#include <spdk/ocssd.h>
#include "ocssd_trace.h"
#include "ocssd_utils.h"
#include "ocssd_io.h"
#include "ocssd_band.h"
#include "ocssd_rwb.h"

#if enabled(OCSSD_TRACE)

/* Size of the comm pipe */
#define OCSSD_TRACE_RING_SIZE	4096
/* Number of events in the pool */
#define OCSSD_TRACE_EVENT_CNT	(1024*64)
/* Maximum event size */
#define OCSSD_TRACE_EVENT_SIZE	64
/* Size of the mapped trace file */
#define OCSSD_TRACE_MAP_SIZE	(1024*1024*64)

typedef _Atomic uint64_t atomic_uint64_t;

struct ocssd_trace {
	/* Thread descriptor */
	struct ocssd_thread	*thread;

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
ocssd_trace_next_id(struct ocssd_trace *trace)
{
	return atomic_fetch_add(&trace->id, 1);
}

static void *
ocssd_event_get_buf(const struct ocssd_event *event)
{
	return (char *)(event + 1) + event->size;
}

static size_t
ocssd_event_size(struct ocssd_event *event)
{
	return sizeof(*event) + event->size;
}

static size_t
ocssd_trace_current_offset(struct ocssd_trace *trace)
{
	return (size_t)(trace->offset % OCSSD_TRACE_MAP_SIZE);
}

static void
_ocssd_event_add_data(struct ocssd_event *event, uint8_t type,
		      const void *buf, size_t size)
{
	assert(ocssd_event_size(event) + size + sizeof(type) <= OCSSD_TRACE_EVENT_SIZE);
	memcpy(ocssd_event_get_buf(event), &type, sizeof(type));
	event->size += sizeof(type);
	memcpy(ocssd_event_get_buf(event), buf, size);
	event->size += size;
}

#define ocssd_event_add_data(event, t, v) \
	_ocssd_event_add_data(event, t, v, sizeof(*(v)))

static void
ocssd_trace_send_event(struct ocssd_trace *trace, struct ocssd_event *event)
{
	size_t num_sent;

	num_sent = spdk_ring_enqueue(trace->thread->ring, (void **)&event, 1);
	if (num_sent != 1) {
		atomic_fetch_add(&trace->num_lost, 1);
	}
}

static struct ocssd_event *
ocssd_event_init(struct ocssd_trace *trace, uint8_t src,
		 ocssd_trace_group_t id)
{
	struct ocssd_event *event;
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

	if (id != OCSSD_TRACE_INVALID_ID) {
		event->id = id;
	} else {
		event->id = ocssd_trace_next_id(trace);
	}

	event->size = 0;
	ocssd_event_add_data(event, OCSSD_TRACE_DATA_SOURCE, &src);
	return event;
}

void
ocssd_trace_defrag_band(struct ocssd_trace *trace, const struct ocssd_band *band)
{
	struct ocssd_event *event;
	uint16_t band16;
	uint32_t num_vld32;
	uint8_t type;

	event = ocssd_event_init(trace, OCSSD_TRACE_SOURCE_INTERNAL, OCSSD_TRACE_INVALID_ID);
	if (spdk_unlikely(!event)) {
		return;
	}

	assert(band->id	<= UINT16_MAX);
	assert(band->md.num_vld <= UINT32_MAX);
	band16		= (uint16_t)band->id;
	num_vld32	= (uint32_t)band->md.num_vld;
	type		= OCSSD_TRACE_TYPE_BAND_DEFRAG;

	ocssd_event_add_data(event, OCSSD_TRACE_DATA_TRACE_TYPE, &type);
	ocssd_event_add_data(event, OCSSD_TRACE_DATA_BAND_ID, &band16);
	ocssd_event_add_data(event, OCSSD_TRACE_DATA_VLD_CNT, &num_vld32);
	ocssd_trace_send_event(trace, event);
}

void
ocssd_trace_write_band(struct ocssd_trace *trace, const struct ocssd_band *band)
{
	struct ocssd_event *event;
	uint16_t band16;
	uint8_t type;

	event = ocssd_event_init(trace, OCSSD_TRACE_SOURCE_INTERNAL, OCSSD_TRACE_INVALID_ID);
	if (spdk_unlikely(!event)) {
		return;
	}

	assert(band->id	<= UINT16_MAX);
	band16		= (uint16_t)band->id;
	type		= OCSSD_TRACE_TYPE_BAND_WRITE;

	ocssd_event_add_data(event, OCSSD_TRACE_DATA_TRACE_TYPE, &type);
	ocssd_event_add_data(event, OCSSD_TRACE_DATA_BAND_ID, &band16);
	ocssd_trace_send_event(trace, event);
}

static uint8_t
ocssd_io2trace_type(const struct ocssd_io *io)
{
	static const enum ocssd_trace_type type[][2] = {
		[OCSSD_IO_READ][0]	= OCSSD_TRACE_TYPE_READ,
		[OCSSD_IO_READ][1]	= OCSSD_TRACE_TYPE_MD_READ,
		[OCSSD_IO_WRITE][0]	= OCSSD_TRACE_TYPE_WRITE,
		[OCSSD_IO_WRITE][1]	= OCSSD_TRACE_TYPE_MD_WRITE,
		[OCSSD_IO_ERASE][0]	= OCSSD_TRACE_TYPE_ERASE,
		[OCSSD_IO_ERASE][1]	= OCSSD_TRACE_TYPE_ERASE,
	};

	return type[io->type][ocssd_io_md(io)];
}

static uint8_t
ocssd_io2trace_source(const struct ocssd_io *io)
{
	if (ocssd_io_internal(io)) {
		return OCSSD_TRACE_SOURCE_INTERNAL;
	} else {
		return OCSSD_TRACE_SOURCE_USER;
	}
}

static struct ocssd_event *
ocssd_io_event_init(struct ocssd_trace *trace, const struct ocssd_io *io,
		    enum ocssd_trace_point point)
{
	struct ocssd_event *event;
	uint8_t type8, point8;

	event = ocssd_event_init(trace, ocssd_io2trace_source(io), io->trace);
	if (spdk_unlikely(!event)) {
		return NULL;
	}

	type8	= (uint8_t)ocssd_io2trace_type(io);
	point8	= point;

	ocssd_event_add_data(event, OCSSD_TRACE_DATA_TRACE_TYPE, &type8);
	ocssd_event_add_data(event, OCSSD_TRACE_DATA_TRACE_POINT, &point8);
	return event;
}

void
ocssd_trace_lba_io_init(struct ocssd_trace *trace, const struct ocssd_io *io)
{
	struct ocssd_event *event;
	uint8_t lbk_cnt = (uint8_t)io->lbk_cnt;

	event = ocssd_io_event_init(trace, io, OCSSD_TRACE_POINT_SCHEDULED);
	if (spdk_unlikely(!event)) {
		return;
	}

	ocssd_event_add_data(event, OCSSD_TRACE_DATA_LBA, &io->lba);
	ocssd_event_add_data(event, OCSSD_TRACE_DATA_LBK_CNT, &lbk_cnt);
	ocssd_trace_send_event(trace, event);
}

void
ocssd_trace_rwb_fill(struct ocssd_trace *trace, const struct ocssd_io *io)
{
	struct ocssd_event *event;

	event = ocssd_io_event_init(trace, io, OCSSD_TRACE_POINT_RWB_FILL);
	if (spdk_unlikely(!event)) {
		return;
	}

	ocssd_event_add_data(event, OCSSD_TRACE_DATA_LBA, &io->lba);
	ocssd_trace_send_event(trace, event);
}

void ocssd_trace_rwb_pop(struct ocssd_trace *trace,
			 const struct ocssd_rwb_entry *entry)
{
	struct ocssd_event *event;
	uint8_t point, type;

	event = ocssd_event_init(trace, OCSSD_TRACE_SOURCE_INTERNAL, entry->trace);
	if (spdk_unlikely(!event)) {
		return;
	}

	point	= OCSSD_TRACE_POINT_RWB_POP;
	type	= OCSSD_TRACE_TYPE_WRITE;

	ocssd_event_add_data(event, OCSSD_TRACE_DATA_TRACE_TYPE, &type);
	ocssd_event_add_data(event, OCSSD_TRACE_DATA_TRACE_POINT, &point);
	ocssd_event_add_data(event, OCSSD_TRACE_DATA_LBA, &entry->lba);
	ocssd_event_add_data(event, OCSSD_TRACE_DATA_PPA, &entry->ppa);
	ocssd_trace_send_event(trace, event);
}

void
ocssd_trace_completion(struct ocssd_trace *trace, const struct ocssd_io *io,
		       enum ocssd_trace_completion type)
{
	struct ocssd_event *event;
	uint8_t type8;

	event = ocssd_io_event_init(trace, io, OCSSD_TRACE_POINT_COMPLETION);
	if (spdk_unlikely(!event)) {
		return;
	}

	type8 = (uint8_t)type;
	ocssd_event_add_data(event, OCSSD_TRACE_DATA_LBA, &io->lba);
	ocssd_event_add_data(event, OCSSD_TRACE_DATA_COMPLETION, &type8);
	ocssd_trace_send_event(trace, event);
}

void
ocssd_trace_submission(struct ocssd_trace *trace, const struct ocssd_io *io,
		       struct ocssd_ppa ppa, size_t ppa_cnt)
{
	struct ocssd_event *event;
	uint8_t ppa_cnt8;

	event = ocssd_io_event_init(trace, io, OCSSD_TRACE_POINT_SUBMISSION);
	if (spdk_unlikely(!event)) {
		return;
	}

	ppa_cnt8 = (uint8_t)ppa_cnt;
	ocssd_event_add_data(event, OCSSD_TRACE_DATA_PPA, &ppa);
	ocssd_event_add_data(event, OCSSD_TRACE_DATA_LBK_CNT, &ppa_cnt8);
	ocssd_trace_send_event(trace, event);
}

void
ocssd_trace_limits(struct ocssd_trace *trace, const size_t *limits,
		   size_t num_free)
{
	struct ocssd_event *event;
	uint16_t user16, internal16, num_free16;
	uint8_t type;

	event = ocssd_event_init(trace, OCSSD_TRACE_SOURCE_INTERNAL, OCSSD_TRACE_INVALID_ID);
	if (spdk_unlikely(!event)) {
		return;
	}

	assert(limits[OCSSD_RWB_TYPE_USER]	<= UINT16_MAX);
	assert(limits[OCSSD_RWB_TYPE_INTERNAL]	<= UINT16_MAX);
	assert(num_free				<= UINT16_MAX);
	user16		= (uint16_t)limits[OCSSD_RWB_TYPE_USER];
	internal16	= (uint16_t)limits[OCSSD_RWB_TYPE_INTERNAL];
	num_free16	= (uint16_t)num_free;
	type		= OCSSD_TRACE_TYPE_APPLIED_LIMITS;

	ocssd_event_add_data(event, OCSSD_TRACE_DATA_TRACE_TYPE, &type);
	ocssd_event_add_data(event, OCSSD_TRACE_DATA_RWB_USER_SIZE, &user16);
	ocssd_event_add_data(event, OCSSD_TRACE_DATA_RWB_INTERNAL_SIZE, &internal16);
	ocssd_event_add_data(event, OCSSD_TRACE_DATA_BAND_CNT, &num_free16);
	ocssd_trace_send_event(trace, event);
}

static int
ocssd_trace_map_file(struct ocssd_trace *trace)
{
	int errsv;

	if (trace->buf) {
		if (munmap(trace->buf, OCSSD_TRACE_MAP_SIZE)) {
			SPDK_ERRLOG("Failed to unmap the trace file\n");
			return -1;
		}
	}

	assert(trace->offset % OCSSD_TRACE_MAP_SIZE == 0);

	if (fallocate(trace->fd, 0, trace->offset, OCSSD_TRACE_MAP_SIZE)) {
		SPDK_ERRLOG("Failed to allocate space for the trace file\n");
		return -1;
	}


	trace->buf = mmap(NULL, OCSSD_TRACE_MAP_SIZE, PROT_WRITE | PROT_READ,
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
ocssd_trace_dump_event(struct ocssd_trace *trace, struct ocssd_event *event)
{
	size_t size, offset;
	void *buf;

	buf = event;
	size = ocssd_event_size(event);

	assert(size <= OCSSD_TRACE_EVENT_SIZE);

	if (ocssd_trace_current_offset(trace) + size > OCSSD_TRACE_MAP_SIZE) {
		offset = OCSSD_TRACE_MAP_SIZE - ocssd_trace_current_offset(trace);
		memcpy(trace->buf + ocssd_trace_current_offset(trace), event, offset);
		trace->offset += offset;

		if (ocssd_trace_map_file(trace)) {
			assert(0 && "Failed to map the trace file");
			return;
		}

		buf = (char *)buf + offset;
		size -= offset;
	}

	memcpy(trace->buf + ocssd_trace_current_offset(trace), buf, size);
	trace->offset += size;

	spdk_mempool_put(trace->pool, event);
}

static void
ocssd_trace_loop(void *ctx)
{
#define OCSSD_TRACE_MAX_EVENTS 64
	struct ocssd_trace *trace = ctx;
	struct ocssd_event *event[OCSSD_TRACE_MAX_EVENTS];
	size_t num_events, i;

	while (ocssd_thread_running(trace->thread)) {
		num_events = spdk_ring_dequeue(trace->thread->ring, (void **)&event,
					       OCSSD_TRACE_MAX_EVENTS);

		for (i = 0; i < num_events; ++i) {
			ocssd_trace_dump_event(trace, event[i]);
		}
	}
}

ocssd_trace_group_t
ocssd_trace_alloc_group(struct ocssd_trace *trace)
{
	if (!trace) {
		return OCSSD_TRACE_INVALID_ID;
	}

	return ocssd_trace_next_id(trace);
}

struct ocssd_trace *
ocssd_trace_init(const char *fname)
{
	struct ocssd_trace *trace;
	int errsv;

	trace = calloc(1, sizeof(*trace));
	trace->fd = open(fname, O_RDWR | O_TRUNC | O_CREAT, 0660);
	if (trace->fd < 0) {
		errsv = errno;
		SPDK_ERRLOG("%s: %s\n", fname, strerror(errsv));
		goto error;
	}

	if (ocssd_trace_map_file(trace)) {
		goto error;
	}

	trace->pool = spdk_mempool_create("ocssd-trace", OCSSD_TRACE_EVENT_CNT,
					  OCSSD_TRACE_EVENT_SIZE,
					  SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					  SPDK_ENV_SOCKET_ID_ANY);
	if (!trace->pool) {
		goto error;
	}

	trace->thread = ocssd_thread_init("ocssd-trace", OCSSD_TRACE_RING_SIZE,
					  ocssd_trace_loop, trace, 0);
	if (!trace->thread) {
		goto error;
	}

	if (ocssd_thread_start(trace->thread)) {
		goto error;
	}

	return trace;
error:
	ocssd_trace_free(trace);
	return NULL;
}

void
ocssd_trace_free(struct ocssd_trace *trace)
{
	if (!trace) {
		return;
	}

	if (trace->thread) {
		ocssd_thread_stop(trace->thread);
		ocssd_thread_join(trace->thread);
		ocssd_thread_free(trace->thread);
	}

	if (trace->pool) {
		spdk_mempool_free(trace->pool);
	}

	if (trace->buf) {
		munmap(trace->buf, OCSSD_TRACE_MAP_SIZE);
	}

	if (trace->fd > 0) {
		close(trace->fd);
	}

	free(trace);
}

#endif /* enabled(OCSSD_TRACE) */
