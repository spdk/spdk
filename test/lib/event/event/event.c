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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include <rte_config.h>
#include <rte_eal.h>
#include <rte_ring.h>
#include <rte_lcore.h>

#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"

static uint64_t g_tsc_rate;
static uint64_t g_tsc_us_rate;

static int g_time_in_sec;

static __thread uint64_t __call_count = 0;
static uint64_t call_count[RTE_MAX_LCORE];

static void
submit_new_event(spdk_event_t event)
{
	static __thread uint32_t next_lcore = RTE_MAX_LCORE;

	if (next_lcore == RTE_MAX_LCORE) {
		next_lcore = rte_get_next_lcore(rte_lcore_id(), 0, 1);
	}

	++__call_count;
	event = spdk_event_allocate(next_lcore, submit_new_event, NULL, NULL, NULL);
	spdk_event_call(event);
}

static int
event_work_fn(void *arg)
{
	uint64_t tsc_end;

	tsc_end = spdk_get_ticks() + g_time_in_sec * g_tsc_rate;

	submit_new_event(NULL);
	submit_new_event(NULL);
	submit_new_event(NULL);
	submit_new_event(NULL);

	while (1) {

		spdk_event_queue_run_batch(rte_lcore_id());

		if (spdk_get_ticks() > tsc_end) {
			break;
		}
	}

	call_count[rte_lcore_id()] = __call_count;

	return 0;
}

static void
usage(char *program_name)
{
	printf("%s options\n", program_name);
	printf("\t[-m core mask for distributing I/O submission/completion work\n");
	printf("\t\t(default: 0x1 - use core 0 only)]\n");
	printf("\t[-t time in seconds]\n");
}

static void
performance_dump(int io_time)
{
	uint32_t i;

	printf("\n");
	RTE_LCORE_FOREACH(i) {
		printf("lcore %2d: %8ju\n", i, call_count[i] / g_time_in_sec);
	}

	fflush(stdout);
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts;
	int op;
	int i;

	spdk_app_opts_init(&opts);
	opts.name = "event";

	g_time_in_sec = 0;

	while ((op = getopt(argc, argv, "m:t:")) != -1) {
		switch (op) {
		case 'm':
			opts.reactor_mask = optarg;
			break;
		case 't':
			g_time_in_sec = atoi(optarg);
			break;
		default:
			usage(argv[0]);
			exit(1);
		}
	}

	if (!g_time_in_sec) {
		usage(argv[0]);
		exit(1);
	}

	optind = 1;  /*reset the optind */

	spdk_app_init(&opts);

	g_tsc_rate = spdk_get_ticks_hz();
	g_tsc_us_rate = g_tsc_rate / (1000 * 1000);

	printf("Running I/O for %d seconds...", g_time_in_sec);
	fflush(stdout);

	/* call event_work_fn on each slave lcore */
	RTE_LCORE_FOREACH_SLAVE(i) {
		rte_eal_remote_launch(event_work_fn, NULL, i);
	}

	/* call event_work_fn on lcore0 */
	event_work_fn(NULL);

	rte_eal_mp_wait_lcore();

	performance_dump(g_time_in_sec);

	printf("done.\n");
	return 0;
}
