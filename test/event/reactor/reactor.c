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
