/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/string.h"
#include "spdk/thread.h"

static int g_time_in_sec;
static int g_queue_depth;
static struct spdk_poller *g_test_end_poller;
static uint64_t g_call_count = 0;

static int
__test_end(void *arg)
{
	printf("test_end\n");
	spdk_poller_unregister(&g_test_end_poller);
	spdk_app_stop(0);
	return -1;
}

static void
__submit_next(void *arg1, void *arg2)
{
	struct spdk_event *event;

	g_call_count++;

	event = spdk_event_allocate(spdk_env_get_current_core(),
				    __submit_next, NULL, NULL);
	spdk_event_call(event);
}

static void
test_start(void *arg1)
{
	int i;

	printf("test_start\n");

	/* Register a poller that will stop the test after the time has elapsed. */
	g_test_end_poller = SPDK_POLLER_REGISTER(__test_end, NULL,
			    g_time_in_sec * 1000000ULL);

	for (i = 0; i < g_queue_depth; i++) {
		__submit_next(NULL, NULL);
	}
}

static void
test_cleanup(void)
{
	printf("test_abort\n");

	spdk_poller_unregister(&g_test_end_poller);
	spdk_app_stop(0);
}

static void
usage(const char *program_name)
{
	printf("%s options\n", program_name);
	printf("\t[-q Queue depth (default: 1)]\n");
	printf("\t[-t time in seconds]\n");
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts;
	int op;
	int rc;
	long int val;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "reactor_perf";

	g_time_in_sec = 0;
	g_queue_depth = 1;

	while ((op = getopt(argc, argv, "q:t:")) != -1) {
		if (op == '?') {
			usage(argv[0]);
			exit(1);
		}
		val = spdk_strtol(optarg, 10);
		if (val < 0) {
			fprintf(stderr, "Converting a string to integer failed\n");
			exit(1);
		}
		switch (op) {
		case 'q':
			g_queue_depth = val;
			break;
		case 't':
			g_time_in_sec = val;
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

	opts.shutdown_cb = test_cleanup;

	rc = spdk_app_start(&opts, test_start, NULL);

	spdk_app_fini();

	printf("Performance: %8ju events per second\n", g_call_count / g_time_in_sec);

	return rc;
}
