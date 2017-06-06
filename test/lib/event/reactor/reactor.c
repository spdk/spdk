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

#include "spdk/event.h"

static int g_time_in_sec;
static struct spdk_poller *test_end_poller;
static struct spdk_poller *poller_100ms;
static struct spdk_poller *poller_250ms;
static struct spdk_poller *poller_500ms;
static struct spdk_poller *poller_oneshot;
static struct spdk_poller *poller_unregister;

static void
test_end(void *arg)
{
	printf("test_end\n");
	spdk_app_stop(0);
}

static void
tick(void *arg)
{
	uintptr_t period = (uintptr_t)arg;

	printf("tick %" PRIu64 "\n", (uint64_t)period);
}

static void
oneshot(void *arg)
{
	printf("oneshot\n");
	spdk_poller_unregister(&poller_oneshot, NULL);
}

static void
nop(void *arg)
{
}

static void
test_start(void *arg1, void *arg2)
{
	printf("test_start\n");

	/* Register a poller that will stop the test after the time has elapsed. */
	spdk_poller_register(&test_end_poller, test_end, NULL, 0, g_time_in_sec * 1000000ULL);

	spdk_poller_register(&poller_100ms, tick, (void *)100, 0, 100000);
	spdk_poller_register(&poller_250ms, tick, (void *)250, 0, 250000);
	spdk_poller_register(&poller_500ms, tick, (void *)500, 0, 500000);
	spdk_poller_register(&poller_oneshot, oneshot, NULL, 0, 0);

	spdk_poller_register(&poller_unregister, nop, NULL, 0, 0);
	spdk_poller_unregister(&poller_unregister, NULL);
}

static void
test_cleanup(void)
{
	printf("test_cleanup\n");

	spdk_poller_unregister(&test_end_poller, NULL);
	spdk_poller_unregister(&poller_100ms, NULL);
	spdk_poller_unregister(&poller_250ms, NULL);
	spdk_poller_unregister(&poller_500ms, NULL);
	/* poller_oneshot unregisters itself */
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

	spdk_app_opts_init(&opts);
	opts.name = "reactor";
	opts.max_delay_us = 1000;

	g_time_in_sec = 0;

	while ((op = getopt(argc, argv, "t:")) != -1) {
		switch (op) {
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

	opts.shutdown_cb = test_cleanup;

	spdk_app_start(&opts, test_start, NULL, NULL);

	test_cleanup();

	spdk_app_fini();

	return 0;
}
