/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/event.h"
#include "spdk/string.h"
#include "spdk/thread.h"

static int g_time_in_sec;
static struct spdk_poller *test_end_poller;
static struct spdk_poller *poller_100ms;
static struct spdk_poller *poller_250ms;
static struct spdk_poller *poller_500ms;
static struct spdk_poller *poller_oneshot;
static struct spdk_poller *poller_unregister;

static int
test_end(void *arg)
{
	printf("test_end\n");

	spdk_poller_unregister(&test_end_poller);
	spdk_poller_unregister(&poller_100ms);
	spdk_poller_unregister(&poller_250ms);
	spdk_poller_unregister(&poller_500ms);

	spdk_app_stop(0);
	return -1;
}

static int
tick(void *arg)
{
	uintptr_t period = (uintptr_t)arg;

	printf("tick %" PRIu64 "\n", (uint64_t)period);

	return -1;
}

static int
oneshot(void *arg)
{
	printf("oneshot\n");
	spdk_poller_unregister(&poller_oneshot);

	return -1;
}

static int
nop(void *arg)
{
	return -1;
}

static void
test_start(void *arg1)
{
	printf("test_start\n");

	/* Register a poller that will stop the test after the time has elapsed. */
	test_end_poller = SPDK_POLLER_REGISTER(test_end, NULL, g_time_in_sec * 1000000ULL);

	poller_100ms = SPDK_POLLER_REGISTER(tick, (void *)100, 100000);
	poller_250ms = SPDK_POLLER_REGISTER(tick, (void *)250, 250000);
	poller_500ms = SPDK_POLLER_REGISTER(tick, (void *)500, 500000);
	poller_oneshot = SPDK_POLLER_REGISTER(oneshot, NULL, 0);

	poller_unregister = SPDK_POLLER_REGISTER(nop, NULL, 0);
	spdk_poller_unregister(&poller_unregister);
}

static void
usage(const char *program_name)
{
	printf("%s options\n", program_name);
	printf("\t[-t time in seconds]\n");
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts;
	int op;
	int rc = 0;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "reactor";

	g_time_in_sec = 0;

	while ((op = getopt(argc, argv, "t:")) != -1) {
		switch (op) {
		case 't':
			g_time_in_sec = spdk_strtol(optarg, 10);
			break;
		default:
			usage(argv[0]);
			exit(1);
		}
	}

	if (g_time_in_sec <= 0) {
		usage(argv[0]);
		exit(1);
	}

	rc = spdk_app_start(&opts, test_start, NULL);

	spdk_app_fini();

	return rc;
}
