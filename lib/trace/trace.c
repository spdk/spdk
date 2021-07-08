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

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/util.h"
#include "spdk/barrier.h"
#include "spdk/log.h"

static int g_trace_fd = -1;
static char g_shm_name[64];

struct spdk_trace_histories *g_trace_histories;

static inline struct spdk_trace_entry *
get_trace_entry(struct spdk_trace_history *history, uint64_t offset)
{
	return &history->entries[offset & (history->num_entries - 1)];
}

void
_spdk_trace_record(uint64_t tsc, uint16_t tpoint_id, uint16_t poller_id, uint32_t size,
		   uint64_t object_id, int num_args, ...)
{
	struct spdk_trace_history *lcore_history;
	struct spdk_trace_entry *next_entry;
	struct spdk_trace_entry_buffer *buffer;
	struct spdk_trace_tpoint *tpoint;
	struct spdk_trace_argument *argument;
	unsigned lcore, i, offset, num_entries, arglen, argoff, curlen;
	uint64_t intval;
	void *argval;
	va_list vl;

	lcore = spdk_env_get_current_core();
	if (lcore >= SPDK_TRACE_MAX_LCORE) {
		return;
	}

	lcore_history = spdk_get_per_lcore_history(g_trace_histories, lcore);
	if (tsc == 0) {
		tsc = spdk_get_ticks();
	}

	lcore_history->tpoint_count[tpoint_id]++;

	tpoint = &g_trace_flags->tpoint[tpoint_id];
	/* Make sure that the number of arguments passed matches tracepoint definition */
	if (tpoint->num_args != num_args) {
		assert(0 && "Unexpected number of tracepoint arguments");
		return;
	}

	/* Get next entry index in the circular buffer */
	next_entry = get_trace_entry(lcore_history, lcore_history->next_entry);
	next_entry->tsc = tsc;
	next_entry->tpoint_id = tpoint_id;
	next_entry->poller_id = poller_id;
	next_entry->size = size;
	next_entry->object_id = object_id;

	num_entries = 1;
	buffer = (struct spdk_trace_entry_buffer *)next_entry;
	/* The initial offset needs to be adjusted by the fields present in the first entry
	 * (poller_id, size, etc.).
	 */
	offset = offsetof(struct spdk_trace_entry, args) -
		 offsetof(struct spdk_trace_entry_buffer, data);

	va_start(vl, num_args);
	for (i = 0; i < tpoint->num_args; ++i) {
		argument = &tpoint->args[i];
		switch (argument->type) {
		case SPDK_TRACE_ARG_TYPE_STR:
			argval = va_arg(vl, void *);
			arglen = strnlen((const char *)argval, argument->size - 1) + 1;
			break;
		case SPDK_TRACE_ARG_TYPE_INT:
		case SPDK_TRACE_ARG_TYPE_PTR:
			intval = va_arg(vl, uint64_t);
			argval = &intval;
			arglen = sizeof(uint64_t);
			break;
		default:
			assert(0 && "Invalid trace argument type");
			return;
		}

		/* Copy argument's data. For some argument types (strings) user is allowed to pass a
		 * value that is either larger or smaller than what's defined in the tracepoint's
		 * description. If the value is larger, we'll truncate it, while if it's smaller,
		 * we'll only fill portion of the buffer, without touching the rest. For instance,
		 * if the definition marks an argument as 40B and user passes 12B string, we'll only
		 * copy 13B (accounting for the NULL terminator).
		 */
		argoff = 0;
		while (argoff < argument->size) {
			/* Current buffer is full, we need to acquire another one */
			if (offset == sizeof(buffer->data)) {
				buffer = (struct spdk_trace_entry_buffer *) get_trace_entry(
						 lcore_history,
						 lcore_history->next_entry + num_entries);
				buffer->tpoint_id = SPDK_TRACE_MAX_TPOINT_ID;
				buffer->tsc = tsc;
				num_entries++;
				offset = 0;
			}

			curlen = spdk_min(sizeof(buffer->data) - offset, argument->size - argoff);
			if (argoff < arglen) {
				assert(argval != NULL);
				memcpy(&buffer->data[offset], (uint8_t *)argval + argoff,
				       spdk_min(curlen, arglen - argoff));
			}

			offset += curlen;
			argoff += curlen;
		}

		/* Make sure that truncated strings are NULL-terminated */
		if (argument->type == SPDK_TRACE_ARG_TYPE_STR) {
			assert(offset > 0);
			buffer->data[offset - 1] = '\0';
		}
	}
	va_end(vl);

	/* Ensure all elements of the trace entry are visible to outside trace tools */
	spdk_smp_wmb();
	lcore_history->next_entry += num_entries;
}

int
spdk_trace_init(const char *shm_name, uint64_t num_entries)
{
	int i = 0;
	int histories_size;
	uint64_t lcore_offsets[SPDK_TRACE_MAX_LCORE + 1];

	/* 0 entries requested - skip trace initialization */
	if (num_entries == 0) {
		return 0;
	}

	lcore_offsets[0] = sizeof(struct spdk_trace_flags);
	for (i = 1; i < (int)SPDK_COUNTOF(lcore_offsets); i++) {
		lcore_offsets[i] = spdk_get_trace_history_size(num_entries) + lcore_offsets[i - 1];
	}
	histories_size = lcore_offsets[SPDK_TRACE_MAX_LCORE];

	snprintf(g_shm_name, sizeof(g_shm_name), "%s", shm_name);

	g_trace_fd = shm_open(shm_name, O_RDWR | O_CREAT, 0600);
	if (g_trace_fd == -1) {
		SPDK_ERRLOG("could not shm_open spdk_trace\n");
		SPDK_ERRLOG("errno=%d %s\n", errno, spdk_strerror(errno));
		return 1;
	}

	if (ftruncate(g_trace_fd, histories_size) != 0) {
		SPDK_ERRLOG("could not truncate shm\n");
		goto trace_init_err;
	}

	g_trace_histories = mmap(NULL, histories_size, PROT_READ | PROT_WRITE,
				 MAP_SHARED, g_trace_fd, 0);
	if (g_trace_histories == MAP_FAILED) {
		SPDK_ERRLOG("could not mmap shm\n");
		goto trace_init_err;
	}

	/* TODO: On FreeBSD, mlock on shm_open'd memory doesn't seem to work.  Docs say that kern.ipc.shm_use_phys=1
	 * should allow it, but forcing that doesn't seem to work either.  So for now just skip mlock on FreeBSD
	 * altogether.
	 */
#if defined(__linux__)
	if (mlock(g_trace_histories, histories_size) != 0) {
		SPDK_ERRLOG("Could not mlock shm for tracing - %s.\n", spdk_strerror(errno));
		if (errno == ENOMEM) {
			SPDK_ERRLOG("Check /dev/shm for old tracing files that can be deleted.\n");
		}
		goto trace_init_err;
	}
#endif

	memset(g_trace_histories, 0, histories_size);

	g_trace_flags = &g_trace_histories->flags;

	g_trace_flags->tsc_rate = spdk_get_ticks_hz();

	for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
		struct spdk_trace_history *lcore_history;

		g_trace_flags->lcore_history_offsets[i] = lcore_offsets[i];
		lcore_history = spdk_get_per_lcore_history(g_trace_histories, i);
		lcore_history->lcore = i;
		lcore_history->num_entries = num_entries;
	}
	g_trace_flags->lcore_history_offsets[SPDK_TRACE_MAX_LCORE] = lcore_offsets[SPDK_TRACE_MAX_LCORE];

	spdk_trace_flags_init();

	return 0;

trace_init_err:
	if (g_trace_histories != MAP_FAILED) {
		munmap(g_trace_histories, histories_size);
	}
	close(g_trace_fd);
	g_trace_fd = -1;
	shm_unlink(shm_name);
	g_trace_histories = NULL;

	return 1;

}

void
spdk_trace_cleanup(void)
{
	bool unlink;
	int i;
	struct spdk_trace_history *lcore_history;

	if (g_trace_histories == NULL) {
		return;
	}

	/*
	 * Only unlink the shm if there were no trace_entry recorded. This ensures the file
	 * can be used after this process exits/crashes for debugging.
	 * Note that we have to calculate this value before g_trace_histories gets unmapped.
	 */
	for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
		lcore_history = spdk_get_per_lcore_history(g_trace_histories, i);
		unlink = lcore_history->entries[0].tsc == 0;
		if (!unlink) {
			break;
		}
	}

	munmap(g_trace_histories, sizeof(struct spdk_trace_histories));
	g_trace_histories = NULL;
	close(g_trace_fd);

	if (unlink) {
		shm_unlink(g_shm_name);
	}
}
