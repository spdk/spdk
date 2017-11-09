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
	if (spdk_app_get_running_config(&config_str, "iscsi.conf") < 0)
		fprintf(stderr, "Error getting config\n");
	else {
		fprintf(stdout, "============================\n");
		fprintf(stdout, " iSCSI target running config\n");
		fprintf(stdout, "=============================\n");
		fprintf(stdout, "%s", config_str);
	}
	free(config_str);
}

static void
common_usage(const char *executable_name, struct spdk_app_opts *default_opts)
{
	printf("%s [options]\n", executable_name);
	printf("options:\n");
	printf(" -c config  config file (default %s)\n", default_opts->config_file);
	printf(" -e mask    tracepoint group mask for spdk trace buffers (default 0x0)\n");
	printf(" -m mask    core mask for DPDK\n");
	printf(" -i shared memory ID (optional)\n");
	printf(" -n channel number of memory channels used for DPDK\n");
	printf(" -p core    master (primary) core for DPDK\n");
	printf(" -s size    memory size in MB for DPDK\n");
	spdk_tracelog_usage(stdout, "-t");
	printf(" -H         show this usage\n");
	printf(" -d         disable coredump file enabling\n");
	printf(" -q         disable notice level logging to stderr\n");
}

static void
usage(char *executable_name)
{
	struct spdk_app_opts default_opts;

	spdk_app_opts_init(&default_opts);
	default_opts.config_file = SPDK_ISCSI_DEFAULT_CONFIG;
	common_usage(executable_name, &default_opts);
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
iscsi_parse_args(int argc, char **argv, struct spdk_app_opts *opts)
{
	int ch, rc;

	while ((ch = getopt(argc, argv, "bc:de:i:m:n:p:qs:t:H")) != -1) {
		switch (ch) {
		case 'd':
			opts->enable_coredump = false;
			break;
		case 'c':
			opts->config_file = optarg;
			break;
		case 'i':
			opts->shm_id = atoi(optarg);
			break;
		case 't':
			rc = spdk_log_set_trace_flag(optarg);
			if (rc < 0) {
				fprintf(stderr, "unknown flag\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
			opts->print_level = SPDK_LOG_DEBUG;
#ifndef DEBUG
			fprintf(stderr, "%s must be built with CONFIG_DEBUG=y for -t flag\n",
				argv[0]);
			usage(argv[0]);
			exit(EXIT_FAILURE);
#endif
			break;
		case 'e':
			opts->tpoint_group_mask = optarg;
			break;
		case 'q':
			opts->print_level = SPDK_LOG_WARN;
			break;
		case 'm':
			opts->reactor_mask = optarg;
			break;
		case 'n':
			opts->mem_channel = atoi(optarg);
			break;
		case 'p':
			opts->master_core = atoi(optarg);
			break;
		case 's':
			opts->mem_size = atoi(optarg);
			break;
		case 'b':
			g_daemon_mode = 1;
			break;
		case 'H':
		default:
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		}
	}
}

int
main(int argc, char **argv)
{
	int rc;
	struct spdk_app_opts opts = {};

	/* default value in opts structure */
	spdk_app_opts_init(&opts);

	if (access(SPDK_ISCSI_DEFAULT_CONFIG, F_OK) == 0) {
		opts.config_file = SPDK_ISCSI_DEFAULT_CONFIG;
	}
	opts.name = "iscsi";

	iscsi_parse_args(argc, argv, &opts);

	if (g_daemon_mode) {
		if (daemon(1, 0) < 0) {
			SPDK_ERRLOG("Start iscsi target daemon faild.\n");
			exit(EXIT_FAILURE);
		}
	}

	opts.shutdown_cb = spdk_iscsi_shutdown;
	opts.usr1_handler = spdk_sigusr1;

	printf("Using net framework %s\n", spdk_net_framework_get_name());
	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, spdk_startup, NULL, NULL);

	spdk_app_fini();

	return rc;
}
