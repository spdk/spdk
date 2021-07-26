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
#include "spdk/event.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"

#define MAX_NUM_POLLERS	1000

static int g_time_in_sec;
static int g_period_in_usec;
static int g_num_pollers;

static struct spdk_poller *g_timer;
static struct spdk_poller *g_pollers[MAX_NUM_POLLERS];
static uint64_t g_run_count;

static struct spdk_thread_stats g_start_stats;

static int
poller_run(void *arg)
{
	g_run_count++;

	return SPDK_POLLER_BUSY;
}

static void
_poller_perf_end(void)
{
	struct spdk_thread_stats end_stats;
	uint64_t tsc_hz, busy_cyc, poller_cost_cyc, poller_cost_nsec;
	int i;

	spdk_thread_get_stats(&end_stats);
	busy_cyc = end_stats.busy_tsc - g_start_stats.busy_tsc;

	tsc_hz = spdk_get_ticks_hz();

	printf("\r ======================================\n");

	printf("\r busy:%" PRIu64 " (cyc)\n", busy_cyc);
	printf("\r total_run_count: %" PRIu64 "\n", g_run_count);
	printf("\r tsc_hz: %" PRIu64 " (cyc)\n", tsc_hz);

	printf("\r ======================================\n");

	poller_cost_cyc = busy_cyc / g_run_count;
	poller_cost_nsec = (poller_cost_cyc * SPDK_SEC_TO_NSEC) / tsc_hz;

	printf("\r poller_cost: %" PRIu64 " (cyc), %" PRIu64 " (nsec)\n",
	       poller_cost_cyc, poller_cost_nsec);

	spdk_poller_unregister(&g_timer);

	for (i = 0; i < g_num_pollers; i++) {
		spdk_poller_unregister(&g_pollers[i]);
	}

	spdk_app_stop(0);
}

static int
poller_perf_end(void *arg)
{
	_poller_perf_end();

	return SPDK_POLLER_BUSY;
}

static void
poller_perf_start(void *arg1)
{
	int i;

	printf("Running %d pollers for %d seconds with %d microseconds period.\n",
	       g_num_pollers, g_time_in_sec, g_period_in_usec);
	fflush(stdout);

	for (i = 0; i < g_num_pollers; i++) {
		g_pollers[i] = SPDK_POLLER_REGISTER(poller_run, NULL, g_period_in_usec);
	}

	spdk_thread_get_stats(&g_start_stats);

	g_timer = SPDK_POLLER_REGISTER(poller_perf_end, NULL, g_time_in_sec * SPDK_SEC_TO_USEC);
}

static void
poller_perf_shutdown_cb(void)
{
	_poller_perf_end();
}

static int
poller_perf_parse_arg(int ch, char *arg)
{
	int tmp;

	tmp = spdk_strtol(optarg, 10);
	if (tmp < 0) {
		fprintf(stderr, "Parse failed for the option %c.\n", ch);
		return tmp;
	}

	switch (ch) {
	case 'b':
		g_num_pollers = tmp;
		break;
	case 'l':
		g_period_in_usec = tmp;
		break;
	case 't':
		g_time_in_sec = tmp;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void
poller_perf_usage(void)
{
	printf(" -b <number>            number of pollers\n");
	printf(" -l <period>            poller period in usec\n");
	printf(" -t <time>              run time in seconds\n");
}

static int
poller_perf_verify_params(void)
{
	if (g_num_pollers <= 0 || g_num_pollers > MAX_NUM_POLLERS) {
		fprintf(stderr, "number of pollers must not be more than %d\n", MAX_NUM_POLLERS);
		return -EINVAL;
	}

	if (g_period_in_usec < 0) {
		fprintf(stderr, "period of poller cannot be negative\n");
		return -EINVAL;
	}

	if (g_time_in_sec <= 0) {
		fprintf(stderr, "run time must be positive\n");
		return -EINVAL;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts;
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "poller_perf";
	opts.shutdown_cb = poller_perf_shutdown_cb;

	rc = spdk_app_parse_args(argc, argv, &opts, "b:l:t:", NULL,
				 poller_perf_parse_arg, poller_perf_usage);
	if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
		return rc;
	}

	rc = poller_perf_verify_params();
	if (rc != 0) {
		return rc;
	}

	rc = spdk_app_start(&opts, poller_perf_start, NULL);

	spdk_app_fini();

	return rc;
}
