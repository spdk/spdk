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

static float percentage(uint64_t percent)
{
	return percent / 10.;
}


void print_usage_stats(struct ocf_stats_usage *usage)
{
	printf("╔══════════════════╤══════════╤═══════╤═════════════╗\n");
	printf("║ Usage statistics │  Count   │   %%   │   Units     ║\n");
	printf("╠══════════════════╪══════════╪═══════╪═════════════╣\n");
	printf("║ Occupancy        │ %8lu │ %5.1f │ 4KiB blocks ║\n", usage->occupancy.value,
	       percentage(usage->occupancy.percent));
	printf("║ Free             │ %8lu │ %5.1f │ 4KiB blocks ║\n", usage->free.value,
	       percentage(usage->free.percent));
	printf("║ Clean            │ %8lu │ %5.1f │ 4KiB blocks ║\n", usage->clean.value,
	       percentage(usage->clean.percent));
	printf("║ Dirty            │ %8lu │ %5.1f │ 4KiB blocks ║\n", usage->dirty.value,
	       percentage(usage->dirty.percent));
	printf("╚══════════════════╧══════════╧═══════╧═════════════╝\n");
}


void print_reqs_stats(struct ocf_stats_requests *reqs)
{
	printf("╔══════════════════════╤══════════╤═══════╤══════════╗\n");
	printf("║ Request statistics   │  Count   │   %%   │ Units    ║\n");
	printf("╠══════════════════════╪══════════╪═══════╪══════════╣\n");
	printf("║ Read hits            │ %8lu │ %5.1f │ Requests ║\n", reqs->rd_hits.value,
	       percentage(reqs->rd_hits.percent));
	printf("║ Read partial misses  │ %8lu │ %5.1f │ Requests ║\n",
	       reqs->rd_partial_misses.value, percentage(reqs->rd_partial_misses.percent));
	printf("║ Read full misses     │ %8lu │ %5.1f │ Requests ║\n", reqs->rd_full_misses.value,
	       percentage(reqs->rd_full_misses.percent));
	printf("║ Read total           │ %8lu │ %5.1f │ Requests ║\n", reqs->rd_total.value,
	       percentage(reqs->rd_total.percent));
	printf("╟──────────────────────┼──────────┼───────┼──────────╢\n");
	printf("║ Write hits           │ %8lu │ %5.1f │ Requests ║\n", reqs->wr_hits.value,
	       percentage(reqs->wr_hits.percent));
	printf("║ Write partial misses │ %8lu │ %5.1f │ Requests ║\n",
	       reqs->wr_partial_misses.value, percentage(reqs->wr_partial_misses.percent));
	printf("║ Write full misses    │ %8lu │ %5.1f │ Requests ║\n", reqs->wr_full_misses.value,
	       percentage(reqs->wr_full_misses.percent));
	printf("║ Write total          │ %8lu │ %5.1f │ Requests ║\n", reqs->wr_total.value,
	       percentage(reqs->wr_total.percent));
	printf("╟──────────────────────┼──────────┼───────┼──────────╢\n");
	printf("║ Pass-Through reads   │ %8lu │ %5.1f │ Requests ║\n", reqs->rd_pt.value,
	       percentage(reqs->rd_pt.percent));
	printf("║ Pass-Through writes  │ %8lu │ %5.1f │ Requests ║\n", reqs->wr_pt.value,
	       percentage(reqs->wr_pt.percent));
	printf("║ Serviced requests    │ %8lu │ %5.1f │ Requests ║\n", reqs->serviced.value,
	       percentage(reqs->serviced.percent));
	printf("╟──────────────────────┼──────────┼───────┼──────────╢\n");
	printf("║ Total requests       │ %8lu │ %5.1f │ Requests ║\n", reqs->total.value,
	       percentage(reqs->total.percent));
	printf("╚══════════════════════╧══════════╧═══════╧══════════╝\n");
}

void print_blocks_stats(struct ocf_stats_blocks *blks)
{
	printf("╔════════════════════════════════════╤══════════╤═══════╤═════════════╗\n");
	printf("║ Block statistics                   │  Count   │   %%   │   Units     ║\n");
	printf("╠════════════════════════════════════╪══════════╪═══════╪═════════════╣\n");
	printf("║ Reads from core data object(s)     │ %8lu │ %5.1f │ 4KiB blocks ║\n",
	       blks->core_obj_rd.value, percentage(blks->core_obj_rd.percent));
	printf("║ Writes to core data object(s)      │ %8lu │ %5.1f │ 4KiB blocks ║\n",
	       blks->core_obj_wr.value, percentage(blks->core_obj_wr.percent));
	printf("║ Total to/from core data object (s) │ %8lu │ %5.1f │ 4KiB blocks ║\n",
	       blks->core_obj_total.value, percentage(blks->core_obj_total.percent));
	printf("╟────────────────────────────────────┼──────────┼───────┼─────────────╢\n");
	printf("║ Reads from cache data object       │ %8lu │ %5.1f │ 4KiB blocks ║\n",
	       blks->cache_obj_rd.value, percentage(blks->cache_obj_rd.percent));
	printf("║ Writes to cache data object        │ %8lu │ %5.1f │ 4KiB blocks ║\n",
	       blks->cache_obj_wr.value, percentage(blks->cache_obj_wr.percent));
	printf("║ Total to/from cache data object    │ %8lu │ %5.1f │ 4KiB blocks ║\n",
	       blks->cache_obj_total.value, percentage(blks->cache_obj_total.percent));
	printf("╟────────────────────────────────────┼──────────┼───────┼─────────────╢\n");
	printf("║ Reads from volume                  │ %8lu │ %5.1f │ 4KiB blocks ║\n",
	       blks->volume_rd.value, percentage(blks->volume_rd.percent));
	printf("║ Writes to volume                   │ %8lu │ %5.1f │ 4KiB blocks ║\n",
	       blks->volume_wr.value, percentage(blks->volume_wr.percent));
	printf("║ Total to/from volume               │ %8lu │ %5.1f │ 4KiB blocks ║\n",
	       blks->volume_total.value, percentage(blks->volume_total.percent));
	printf("╚════════════════════════════════════╧══════════╧═══════╧═════════════╝\n");

}

void print_errors_stats(struct ocf_stats_errors *errs)
{
	printf("╔════════════════════╤══════════╤═══════╤══════════╗\n");
	printf("║ Error statistics   │  Count   │   %%   │ Units    ║\n");
	printf("╠════════════════════╪══════════╪═══════╪══════════╣\n");
	printf("║ Cache read errors  │ %8lu │ %5.1f │ Requests ║\n", errs->core_obj_rd.value,
	       percentage(errs->core_obj_rd.percent));
	printf("║ Cache write errors │ %8lu │ %5.1f │ Requests ║\n", errs->core_obj_wr.value,
	       percentage(errs->core_obj_wr.percent));
	printf("║ Cache total errors │ %8lu │ %5.1f │ Requests ║\n", errs->core_obj_total.value,
	       percentage(errs->core_obj_total.percent));
	printf("╟────────────────────┼──────────┼───────┼──────────╢\n");
	printf("║ Core read errors   │ %8lu │ %5.1f │ Requests ║\n", errs->cache_obj_rd.value,
	       percentage(errs->cache_obj_rd.percent));
	printf("║ Core write errors  │ %8lu │ %5.1f │ Requests ║\n", errs->cache_obj_wr.value,
	       percentage(errs->cache_obj_wr.percent));
	printf("║ Core total errors  │ %8lu │ %5.1f │ Requests ║\n", errs->cache_obj_total.value,
	       percentage(errs->cache_obj_total.percent));
	printf("╟────────────────────┼──────────┼───────┼──────────╢\n");
	printf("║ Total errors       │ %8lu │ %5.1f │ Requests ║\n", errs->total.value,
	       percentage(errs->total.percent));
	printf("╚════════════════════╧══════════╧═══════╧══════════╝\n");
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
