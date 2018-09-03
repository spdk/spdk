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

static int g_trace_fd = -1;
static char g_shm_name[64];

struct spdk_trace_histories *g_trace_histories;

void
_spdk_trace_record(uint64_t tsc, uint16_t tpoint_id, uint16_t poller_id, uint32_t size,
		   uint64_t object_id, uint64_t arg1)
{
	struct spdk_trace_history *lcore_history;
	struct spdk_trace_entry *next_entry;
	unsigned lcore;

	lcore = spdk_env_get_current_core();
	if (lcore >= SPDK_TRACE_MAX_LCORE) {
		return;
	}

	lcore_history = &g_trace_histories->per_lcore_history[lcore];
	if (tsc == 0) {
		tsc = spdk_get_ticks();
	}

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

int
spdk_trace_init(const char *shm_name)
{
	int i = 0;

	snprintf(g_shm_name, sizeof(g_shm_name), "%s", shm_name);

	g_trace_fd = shm_open(shm_name, O_RDWR | O_CREAT, 0600);
	if (g_trace_fd == -1) {
		fprintf(stderr, "could not shm_open spdk_trace\n");
		fprintf(stderr, "errno=%d %s\n", errno, spdk_strerror(errno));
		return 1;
	}

	if (ftruncate(g_trace_fd, sizeof(*g_trace_histories)) != 0) {
		fprintf(stderr, "could not truncate shm\n");
		goto trace_init_err;
	}

	g_trace_histories = mmap(NULL, sizeof(*g_trace_histories), PROT_READ | PROT_WRITE,
				 MAP_SHARED, g_trace_fd, 0);
	if (g_trace_histories == MAP_FAILED) {
		fprintf(stderr, "could not mmap shm\n");
		goto trace_init_err;
	}

	/* TODO: On FreeBSD, mlock on shm_open'd memory doesn't seem to work.  Docs say that kern.ipc.shm_use_phys=1
	 * should allow it, but forcing that doesn't seem to work either.  So for now just skip mlock on FreeBSD
	 * altogether.
	 */
#if defined(__linux__)
	if (mlock(g_trace_histories, sizeof(*g_trace_histories)) != 0) {
		fprintf(stderr, "Could not mlock shm for tracing - %s.\n", spdk_strerror(errno));
		if (errno == ENOMEM) {
			fprintf(stderr, "Check /dev/shm for old tracing files that can be deleted.\n");
		}
		goto trace_init_err;
	}
#endif

	memset(g_trace_histories, 0, sizeof(*g_trace_histories));

	g_trace_flags = &g_trace_histories->flags;

	g_trace_flags->tsc_rate = spdk_get_ticks_hz();

	for (i = 0; i < SPDK_TRACE_MAX_LCORE; i++) {
		g_trace_histories->per_lcore_history[i].lcore = i;
	}

	spdk_trace_flags_init();

	return 0;

trace_init_err:
	if (g_trace_histories != MAP_FAILED) {
		munmap(g_trace_histories, sizeof(*g_trace_histories));
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

	if (g_trace_histories == NULL) {
		return;
	}

	/*
	 * Only unlink the shm if there were no tracepoints enabled.  This ensures the file
	 * can be used after this process exits/crashes for debugging.
	 * Note that we have to calculate this value before g_trace_histories gets unmapped.
	 */
	unlink = spdk_mem_all_zero(g_trace_flags->tpoint_mask, sizeof(g_trace_flags->tpoint_mask));
	munmap(g_trace_histories, sizeof(struct spdk_trace_histories));
	g_trace_histories = NULL;
	close(g_trace_fd);

	if (unlink) {
		shm_unlink(g_shm_name);
	}
}
