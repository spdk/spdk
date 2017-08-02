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

#include <linux/nbd.h>

#include "spdk/nbd.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/util.h"

static struct spdk_poller *g_nbd_poller;
static struct spdk_nbd_disk *g_nbd_disk;
static char *g_bdev_name;
static char *g_nbd_name = "/dev/nbd0";

#include "../common.c"

static void
nbd_shutdown(void)
{
	spdk_poller_unregister(&g_nbd_poller, NULL);
	spdk_nbd_stop(g_nbd_disk);
	spdk_app_stop(0);
}

static void
nbd_poll(void *arg)
{
	int rc;

	rc = spdk_nbd_poll(g_nbd_disk);
	if (rc < 0) {
		SPDK_NOTICELOG("spdk_nbd_poll() returned %d; shutting down", rc);
		nbd_shutdown();
	}
}

static void
nbd_start(void *arg1, void *arg2)
{
	struct spdk_bdev	*bdev;

	bdev = spdk_bdev_get_by_name(g_bdev_name);
	if (bdev == NULL) {
		SPDK_ERRLOG("no bdev %s exists\n", g_bdev_name);
		spdk_app_stop(-1);
		return;
	}

	g_nbd_disk = spdk_nbd_start(bdev, g_nbd_name);
	if (g_nbd_disk == NULL) {
		spdk_app_stop(-1);
		return;
	}

	spdk_poller_register(&g_nbd_poller, nbd_poll, NULL, spdk_env_get_current_core(), 0);
}

static void usage(char *program_name)
{
	printf("%s options\n", program_name);
	printf(" -b bdev    export bdev via NBD (required)\n");
	printf(" -c conf    configuration file (required)\n");
	printf(" -m mask    core mask for distributing I/O submission/completion work\n");
	printf("            (default: 0x1 - use core 0 only)\n");
	printf(" -n dev     nbd device name\n");
	printf("            (default: /dev/nbd0)\n");
	spdk_tracelog_usage(stdout, "-t");
}

int
main(int argc, char **argv)
{
	const char *config_file;
	const char *core_mask;
	int op;
	struct spdk_app_opts opts = {};

	/* default value */
	config_file = NULL;
	core_mask = NULL;

	while ((op = getopt(argc, argv, "b:c:m:n:t:")) != -1) {
		switch (op) {
		case 'b':
			g_bdev_name = optarg;
			break;
		case 'c':
			config_file = optarg;
			break;
		case 'm':
			core_mask = optarg;
			break;
		case 'n':
			g_nbd_name = optarg;
			break;
		case 't':
			if (spdk_log_set_trace_flag(optarg) < 0) {
				fprintf(stderr, "unknown flag\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
#ifndef DEBUG
			fprintf(stderr, "%s must be rebuilt with CONFIG_DEBUG=y for -t flag.\n",
				argv[0]);
			usage(argv[0]);
			exit(EXIT_FAILURE);
#endif
			break;
		default:
			usage(argv[0]);
			exit(1);
		}
	}

	if (!config_file || !g_bdev_name) {
		usage(argv[0]);
		exit(1);
	}

	bdevtest_init(config_file, core_mask, &opts);
	opts.shutdown_cb = nbd_shutdown;

	spdk_app_start(&opts, nbd_start, NULL, NULL);

	spdk_app_fini();
	return 0;
}
