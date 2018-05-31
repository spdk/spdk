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
#include "spdk_internal/event.h"
#include "spdk/log.h"
#include "spdk/io_channel.h"

static uint64_t g_tsc_rate;
static uint64_t g_tsc_us_rate;
static uint64_t g_tsc_end;

static int g_time_in_sec;

struct s_call_count {
	int thread_id;
	uint64_t call_count;
};

static struct s_call_count *call_count;

static bool g_app_stopped = false;

static int g_number_of_threads;

static void
submit_new_event(void *arg1, void *arg2)
{
	struct spdk_event *event;
	static __thread struct spdk_thread *thread = NULL;

	if (spdk_get_ticks() > g_tsc_end) {
		if (__sync_bool_compare_and_swap(&g_app_stopped, false, true)) {
			spdk_app_stop(0);
		}
		return;
	}

	if (thread == NULL) {
		thread = spdk_thread_get_next(spdk_env_get_virt_thread());
		if (thread == NULL) {
			thread = spdk_thread_get_first();
		}
	}

	call_count[spdk_thread_get_id(thread)].call_count++;
	event = spdk_thread_event_allocate(thread, submit_new_event, NULL, NULL);
	spdk_thread_event_call(thread, event);
}

static void
event_work_fn(void *arg1, void *arg2)
{

	submit_new_event(NULL, NULL);
	submit_new_event(NULL, NULL);
	submit_new_event(NULL, NULL);
	submit_new_event(NULL, NULL);
}

static void
event_perf_start(void *arg1, void *arg2)
{
	struct spdk_thread *thread;
	struct spdk_event *event;
	int i = 0;

	g_number_of_threads = spdk_thread_get_total_num();
	call_count = calloc(g_number_of_threads + 1, sizeof(struct s_call_count));

	if (call_count == NULL) {
		fprintf(stderr, "call_count allocation failed\n");
		spdk_app_stop(1);
		return;
	}

	g_tsc_rate = spdk_get_ticks_hz();
	g_tsc_us_rate = g_tsc_rate / (1000 * 1000);
	g_tsc_end = spdk_get_ticks() + g_time_in_sec * g_tsc_rate;

	printf("Running I/O for %d seconds...", g_time_in_sec);
	fflush(stdout);

	for (thread = spdk_thread_get_first();
	     thread != NULL;
	     thread = spdk_thread_get_next(thread)) {
		printf("thread %p found\n", thread);
		call_count[i++].thread_id = spdk_thread_get_id(thread);
		event = spdk_thread_event_allocate(thread, event_work_fn, NULL, NULL);
		spdk_thread_event_call(thread, event);
	}

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
	int i = 0;

	if (call_count == NULL) {
		return;
	}

	printf("\n");
	for (i = 0; i <= g_number_of_threads; i++) {
		printf("thread id %2d: %8ju\n", call_count[i].thread_id, call_count[i].call_count / g_time_in_sec);
	}

	fflush(stdout);
	free(call_count);
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int op;
	int rc = 0;

	opts.name = "event_perf";
	opts.dynamic_threading = true;

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

	printf("Running I/O for %d seconds...", g_time_in_sec);
	fflush(stdout);

	rc = spdk_app_start(&opts, event_perf_start, NULL, NULL);

	performance_dump(g_time_in_sec);

	spdk_app_fini();

	printf("done.\n");
	return rc;
}
