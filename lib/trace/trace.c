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

static char g_shm_name[64];

static struct spdk_trace_histories *g_trace_histories;

void
spdk_trace_record(uint16_t tpoint_id, uint16_t poller_id, uint32_t size,
		  uint64_t object_id, uint64_t arg1)
{
	struct spdk_trace_history *lcore_history;
	struct spdk_trace_entry *next_entry;
	uint64_t tsc;
	unsigned lcore;

	/*
	 * Tracepoint group ID is encoded in the tpoint_id.  Lower 6 bits determine the tracepoint
	 *  within the group, the remaining upper bits determine the tracepoint group.  Each
	 *  tracepoint group has its own tracepoint mask.
	 */
	if (g_trace_histories == NULL ||
	    !((1ULL << (tpoint_id & 0x3F)) & g_trace_histories->flags.tpoint_mask[tpoint_id >> 6])) {
		return;
	}

	lcore = spdk_env_get_current_core();
	if (lcore >= SPDK_TRACE_MAX_LCORE) {
		return;
	}

	lcore_history = &g_trace_histories->per_lcore_history[lcore];
	tsc = spdk_get_ticks();

	lcore_history->tpoint_count[tpoint_id]++;

	next_entry = &lcore_history->entries[lcore_history->next_entry];
	next_entry->tsc = tsc;
	next_entry->tpoint_id = tpoint_id;
	next_entry->poller_id = poller_id;
	next_entry->size = size;
	next_entry->object_id = object_id;
	next_entry->arg1 = arg1;

	lcore_history->next_entry++;
	if (lcore_history->next_entry == SPDK_TRACE_SIZE) {
		lcore_history->next_entry = 0;
	}
}

void
spdk_trace_init(const char *shm_name)
{
	int trace_fd;
	int i = 0;

	snprintf(g_shm_name, sizeof(g_shm_name), "%s", shm_name);

	trace_fd = shm_open(shm_name, O_RDWR | O_CREAT, 0600);
	if (trace_fd == -1) {
		fprintf(stderr, "could not shm_open spdk_trace\n");
		fprintf(stderr, "errno=%d %s\n", errno, spdk_strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (ftruncate(trace_fd, sizeof(*g_trace_histories)) != 0) {
		fprintf(stderr, "could not truncate shm\n");
		exit(EXIT_FAILURE);
	}

	g_trace_histories = mmap(NULL, sizeof(*g_trace_histories), PROT_READ | PROT_WRITE,
				 MAP_SHARED, trace_fd, 0);
	if (g_trace_histories == NULL) {
		fprintf(stderr, "could not mmap shm\n");
		exit(EXIT_FAILURE);
	}

	memset(g_trace_histories, 0, sizeof(*g_trace_histories));

	g_trace_flags = &g_trace_histories->flags;

	g_trace_flags->tsc_rate = spdk_get_ticks_hz();

	for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
		g_trace_histories->per_lcore_history[i].lcore = i;
	}

	spdk_trace_flags_init();
}

void
spdk_trace_cleanup(void)
{
	if (g_trace_histories) {
		munmap(g_trace_histories, sizeof(struct spdk_trace_histories));
		g_trace_histories = NULL;
	}
	shm_unlink(g_shm_name);
}
