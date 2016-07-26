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

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "spdk/event.h"

static int g_time_in_sec;
static struct spdk_poller test_end_poller;
static struct spdk_poller poller_100ms;
static struct spdk_poller poller_250ms;
static struct spdk_poller poller_500ms;

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
test_start(spdk_event_t evt)
{
	printf("test_start\n");

	/* Register a poller that will stop the test after the time has elapsed. */
	test_end_poller.fn = test_end;
	spdk_poller_register(&test_end_poller, 0, NULL, g_time_in_sec * 1000000ULL);

	poller_100ms.fn = tick;
	poller_100ms.arg = (void *)100;
	spdk_poller_register(&poller_100ms, 0, NULL, 100000);

	poller_250ms.fn = tick;
	poller_250ms.arg = (void *)250;
	spdk_poller_register(&poller_250ms, 0, NULL, 250000);

	poller_500ms.fn = tick;
	poller_500ms.arg = (void *)500;
	spdk_poller_register(&poller_500ms, 0, NULL, 500000);
}

static void
test_cleanup(void)
{
	printf("test_cleanup\n");

	spdk_poller_unregister(&test_end_poller, NULL);
	spdk_poller_unregister(&poller_100ms, NULL);
	spdk_poller_unregister(&poller_250ms, NULL);
	spdk_poller_unregister(&poller_500ms, NULL);
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

	optind = 1;

	opts.shutdown_cb = test_cleanup;

	spdk_app_opts_init(&opts);
	spdk_app_init(&opts);

	spdk_app_start(test_start, NULL, NULL);

	test_cleanup();

	spdk_app_fini();

	return 0;
}
