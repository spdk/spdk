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

#include "ctx.h"
#include "stats.h"

#define MAX_STAT_LEN 10000

static float
percentage(uint64_t percent)
{
	return percent / 10.;
}

void
cache_stats_write_usage(struct ocf_stats_usage *usage, cache_get_stats_callback_t callback,
			void  *ctx)
{
	char buff[MAX_STAT_LEN];
	snprintf(buff, MAX_STAT_LEN,
		 "╔══════════════════╤══════════╤═══════╤═════════════╗\n"
		 "║ Usage statistics │  Count   │   %%   │   Units     ║\n"
		 "╠══════════════════╪══════════╪═══════╪═════════════╣\n"
		 "║ Occupancy        │ %8lu │ %5.1f │ 4KiB blocks ║\n"
		 "║ Free             │ %8lu │ %5.1f │ 4KiB blocks ║\n"
		 "║ Clean            │ %8lu │ %5.1f │ 4KiB blocks ║\n"
		 "║ Dirty            │ %8lu │ %5.1f │ 4KiB blocks ║\n"
		 "╚══════════════════╧══════════╧═══════╧═════════════╝\n"
		 , usage->occupancy.value, percentage(usage->occupancy.percent)
		 , usage->free.value, percentage(usage->free.percent)
		 , usage->clean.value, percentage(usage->clean.percent)
		 , usage->dirty.value, percentage(usage->dirty.percent)
		);
	callback(buff, ctx);
}

void
cache_stats_write_reqs(struct ocf_stats_requests *reqs, cache_get_stats_callback_t callback,
		       void *ctx)
{
	char buff[MAX_STAT_LEN];
	snprintf(buff, MAX_STAT_LEN,
		 "╔══════════════════════╤══════════╤═══════╤══════════╗\n"
		 "║ Request statistics   │  Count   │   %%   │ Units    ║\n"
		 "╠══════════════════════╪══════════╪═══════╪══════════╣\n"
		 "║ Read hits            │ %8lu │ %5.1f │ Requests ║\n"
		 "║ Read partial misses  │ %8lu │ %5.1f │ Requests ║\n"
		 "║ Read full misses     │ %8lu │ %5.1f │ Requests ║\n"
		 "║ Read total           │ %8lu │ %5.1f │ Requests ║\n"
		 "╟──────────────────────┼──────────┼───────┼──────────╢\n"
		 "║ Write hits           │ %8lu │ %5.1f │ Requests ║\n"
		 "║ Write partial misses │ %8lu │ %5.1f │ Requests ║\n"
		 "║ Write full misses    │ %8lu │ %5.1f │ Requests ║\n"
		 "║ Write total          │ %8lu │ %5.1f │ Requests ║\n"
		 "╟──────────────────────┼──────────┼───────┼──────────╢\n"
		 "║ Pass-Through reads   │ %8lu │ %5.1f │ Requests ║\n"
		 "║ Pass-Through writes  │ %8lu │ %5.1f │ Requests ║\n"
		 "║ Serviced requests    │ %8lu │ %5.1f │ Requests ║\n"
		 "╟──────────────────────┼──────────┼───────┼──────────╢\n"
		 "║ Total requests       │ %8lu │ %5.1f │ Requests ║\n"
		 "╚══════════════════════╧══════════╧═══════╧══════════╝\n"
		 , reqs->rd_hits.value, percentage(reqs->rd_hits.percent)
		 , reqs->rd_partial_misses.value, percentage(reqs->rd_partial_misses.percent)
		 , reqs->rd_full_misses.value, percentage(reqs->rd_full_misses.percent)
		 , reqs->rd_total.value, percentage(reqs->rd_total.percent)
		 , reqs->wr_hits.value, percentage(reqs->wr_hits.percent)
		 , reqs->wr_partial_misses.value, percentage(reqs->wr_partial_misses.percent)
		 , reqs->wr_full_misses.value, percentage(reqs->wr_full_misses.percent)
		 , reqs->wr_total.value, percentage(reqs->wr_total.percent)
		 , reqs->rd_pt.value, percentage(reqs->rd_pt.percent)
		 , reqs->wr_pt.value, percentage(reqs->wr_pt.percent)
		 , reqs->serviced.value, percentage(reqs->serviced.percent)
		 , reqs->total.value, percentage(reqs->total.percent)
		);
	callback(buff, ctx);
}

void
cache_stats_write_blocks(struct ocf_stats_blocks *blks, cache_get_stats_callback_t callback,
			 void  *ctx)
{
	char buff[MAX_STAT_LEN];
	snprintf(buff, MAX_STAT_LEN,
		 "╔════════════════════════════════════╤══════════╤═══════╤═════════════╗\n"
		 "║ Block statistics                   │  Count   │   %%   │   Units     ║\n"
		 "╠════════════════════════════════════╪══════════╪═══════╪═════════════╣\n"
		 "║ Reads from core data object(s)     │ %8lu │ %5.1f │ 4KiB blocks ║\n"
		 "║ Writes to core data object(s)      │ %8lu │ %5.1f │ 4KiB blocks ║\n"
		 "║ Total to/from core data object (s) │ %8lu │ %5.1f │ 4KiB blocks ║\n"
		 "╟────────────────────────────────────┼──────────┼───────┼─────────────╢\n"
		 "║ Reads from cache data object       │ %8lu │ %5.1f │ 4KiB blocks ║\n"
		 "║ Writes to cache data object        │ %8lu │ %5.1f │ 4KiB blocks ║\n"
		 "║ Total to/from cache data object    │ %8lu │ %5.1f │ 4KiB blocks ║\n"
		 "╟────────────────────────────────────┼──────────┼───────┼─────────────╢\n"
		 "║ Reads from volume                  │ %8lu │ %5.1f │ 4KiB blocks ║\n"
		 "║ Writes to volume                   │ %8lu │ %5.1f │ 4KiB blocks ║\n"
		 "║ Total to/from volume               │ %8lu │ %5.1f │ 4KiB blocks ║\n"
		 "╚════════════════════════════════════╧══════════╧═══════╧═════════════╝\n"
		 , blks->core_obj_rd.value, percentage(blks->core_obj_rd.percent)
		 , blks->core_obj_wr.value, percentage(blks->core_obj_wr.percent)
		 , blks->core_obj_total.value, percentage(blks->core_obj_total.percent)
		 , blks->cache_obj_rd.value, percentage(blks->cache_obj_rd.percent)
		 , blks->cache_obj_wr.value, percentage(blks->cache_obj_wr.percent)
		 , blks->cache_obj_total.value, percentage(blks->cache_obj_total.percent)
		 , blks->volume_rd.value, percentage(blks->volume_rd.percent)
		 , blks->volume_wr.value, percentage(blks->volume_wr.percent)
		 , blks->volume_total.value, percentage(blks->volume_total.percent)
		);
	callback(buff, ctx);
}

void
cache_stats_write_errors(struct ocf_stats_errors *errs, cache_get_stats_callback_t callback,
			 void  *ctx)
{
	char buff[MAX_STAT_LEN];
	snprintf(buff, MAX_STAT_LEN,
		 "╔════════════════════╤══════════╤═══════╤══════════╗\n"
		 "║ Error statistics   │  Count   │   %%   │ Units    ║\n"
		 "╠════════════════════╪══════════╪═══════╪══════════╣\n"
		 "║ Cache read errors  │ %8lu │ %5.1f │ Requests ║\n"
		 "║ Cache write errors │ %8lu │ %5.1f │ Requests ║\n"
		 "║ Cache total errors │ %8lu │ %5.1f │ Requests ║\n"
		 "╟────────────────────┼──────────┼───────┼──────────╢\n"
		 "║ Core read errors   │ %8lu │ %5.1f │ Requests ║\n"
		 "║ Core write errors  │ %8lu │ %5.1f │ Requests ║\n"
		 "║ Core total errors  │ %8lu │ %5.1f │ Requests ║\n"
		 "╟────────────────────┼──────────┼───────┼──────────╢\n"
		 "║ Total errors       │ %8lu │ %5.1f │ Requests ║\n"
		 "╚════════════════════╧══════════╧═══════╧══════════╝\n"
		 , errs->cache_obj_rd.value, percentage(errs->cache_obj_rd.percent)
		 , errs->cache_obj_wr.value, percentage(errs->cache_obj_wr.percent)
		 , errs->cache_obj_total.value, percentage(errs->cache_obj_total.percent)
		 , errs->core_obj_rd.value, percentage(errs->core_obj_rd.percent)
		 , errs->core_obj_wr.value, percentage(errs->core_obj_wr.percent)
		 , errs->core_obj_total.value, percentage(errs->core_obj_total.percent)
		 , errs->total.value, percentage(errs->total.percent)
		);
	callback(buff, ctx);
}

int
cache_get_stats(int cache_id, int core_id, struct cache_stats *stats)
{
	int status;
	struct ocf_stats_core core_stats;

	ocf_cache_t cache;
	ocf_core_t core;

	status = ocf_mngt_cache_get(opencas_ctx, cache_id, &cache);

	if (status) {
		return status;
	}

	status = ocf_core_get(cache, 0, &core);

	if (status) {
		return status;
	}

	status = ocf_core_get_stats(core, &core_stats);

	if (status) {
		return status;
	}

	status = ocf_stats_collect_core(core, &stats->usage, &stats->reqs, &stats->blocks, &stats->errors);
	if (status) {
		return status;
	}

	return 0;
}
