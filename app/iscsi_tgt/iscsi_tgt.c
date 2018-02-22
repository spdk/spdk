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
#include "iscsi/iscsi.h"
#include "spdk/log.h"
#include "spdk/net.h"

static int g_daemon_mode = 0;

static void
spdk_sigusr1(int signo __attribute__((__unused__)))
{
	char *config_str = NULL;
	if (spdk_app_get_running_config(&config_str, "iscsi.conf") < 0) {
		fprintf(stderr, "Error getting config\n");
	} else {
		fprintf(stdout, "============================\n");
		fprintf(stdout, " iSCSI target running config\n");
		fprintf(stdout, "=============================\n");
		fprintf(stdout, "%s", config_str);
	}
	free(config_str);
}

static void
iscsi_usage(void)
{
	printf(" -b         run iscsi target background, the default is foreground\n");
}

static void
spdk_startup(void *arg1, void *arg2)
{
	if (getenv("MEMZONE_DUMP") != NULL) {
		spdk_memzone_dump(stdout);
		fflush(stdout);
	}
}

static void
iscsi_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 'b':
		g_daemon_mode = 1;
		break;
	default:
		assert(false);
		break;
	}
}

int
main(int argc, char **argv)
{
	int rc;
	struct spdk_app_opts opts = {};

	spdk_app_opts_init(&opts);
	opts.config_file = SPDK_ISCSI_DEFAULT_CONFIG;
	opts.name = "iscsi";
	if (spdk_app_parse_args(argc, argv, &opts, "b",
				iscsi_parse_arg, iscsi_usage)) {
		exit(EXIT_FAILURE);
	}

	if (g_daemon_mode) {
		if (daemon(1, 0) < 0) {
			SPDK_ERRLOG("Start iscsi target daemon faild.\n");
			exit(EXIT_FAILURE);
		}
	}

	opts.shutdown_cb = NULL;
	opts.usr1_handler = spdk_sigusr1;

	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, spdk_startup, NULL, NULL);
	if (rc < 0) {
		SPDK_ERRLOG("Start iscsi target daemon:  spdk_app_start() unable to start spdk_startup()\n");
	}

	spdk_app_fini();

	return rc;
}
