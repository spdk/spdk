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

#include "spdk/log.h"
#include "spdk/conf.h"
#include "spdk/event.h"

#include "spdk/vhost.h"


#define SPDK_VHOST_DEFAULT_CONFIG "/usr/local/etc/spdk/vhost.conf"
#define SPDK_VHOST_DEFAULT_ENABLE_COREDUMP true
#define SPDK_VHOST_DEFAULT_MEM_SIZE 1024

static void
vhost_app_opts_init(struct spdk_app_opts *opts)
{
	spdk_app_opts_init(opts);
	opts->name = "vhost";
	opts->config_file = SPDK_VHOST_DEFAULT_CONFIG;
	opts->dpdk_mem_size = SPDK_VHOST_DEFAULT_MEM_SIZE;
}

static void
usage(char *executable_name)
{
	struct spdk_app_opts defaults;

	vhost_app_opts_init(&defaults);

	printf("%s [options]\n", executable_name);
	printf("options:\n");
	printf(" -c config  config file (default: %s)\n", defaults.config_file);
	printf(" -e mask    tracepoint group mask for spdk trace buffers (default: 0x0)\n");
	printf(" -m mask    reactor core mask (default: 0x1)\n");
	printf(" -l facility use specific syslog facility (default: %s)\n", defaults.log_facility);
	printf(" -n channel number of memory channels used for DPDK\n");
	printf(" -p core    master (primary) core for DPDK\n");
	printf(" -s size    memory size in MB for DPDK (default: %dMB)\n", defaults.dpdk_mem_size);
	printf(" -S dir     directory where to create vhost sockets (default: pwd)\n");
	spdk_tracelog_usage(stdout, "-t");
	printf(" -h         show this usage\n");
	printf(" -d         disable coredump file enabling\n");
	printf(" -q         disable notice level logging to stderr\n");
}

int
main(int argc, char *argv[])
{
	struct spdk_app_opts opts = {};
	char ch;
	int rc;
	const char *socket_path = NULL;

	vhost_app_opts_init(&opts);

	while ((ch = getopt(argc, argv, "c:de:l:m:p:qs:S:t:h")) != -1) {
		switch (ch) {
		case 'c':
			opts.config_file = optarg;
			break;
		case 'd':
			opts.enable_coredump = false;
			break;
		case 'e':
			opts.tpoint_group_mask = optarg;
			break;
		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
		case 'l':
			opts.log_facility = optarg;
			break;
		case 'm':
			opts.reactor_mask = optarg;
			break;
		case 'p':
			opts.dpdk_master_core = strtoul(optarg, NULL, 10);
			break;
		case 'q':
			spdk_g_notice_stderr_flag = 0;
			break;
		case 's':
			opts.dpdk_mem_size = strtoul(optarg, NULL, 10);
			break;
		case 'S':
			socket_path = optarg;
			break;
		case 't':
			rc = spdk_log_set_trace_flag(optarg);
			if (rc < 0) {
				fprintf(stderr, "unknown flag\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
#ifndef DEBUG
			fprintf(stderr, "%s must be rebuilt with CONFIG_DEBUG=y for -t flag.\n",
				argv[0]);
			usage(argv[0]);
			exit(EXIT_FAILURE);
#endif
			break;
		default:
			fprintf(stderr, "%s Unknown option '-%c'.\n", argv[0], ch);
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (spdk_g_notice_stderr_flag == 1 &&
	    isatty(STDERR_FILENO) &&
	    !strncmp(ttyname(STDERR_FILENO), "/dev/tty", strlen("/dev/tty"))) {
		printf("Warning: printing stderr to console terminal without -q option specified.\n");
		printf("Suggest using -q to disable logging to stderr and monitor syslog, or\n");
		printf("redirect stderr to a file.\n");
		printf("(Delaying for 10 seconds...)\n");
		sleep(10);
	}

	opts.shutdown_cb = spdk_vhost_shutdown_cb;
	spdk_app_init(&opts);

	/* Blocks until the application is exiting */
	rc = spdk_app_start(spdk_vhost_startup, (void *)socket_path, NULL);

	spdk_app_fini();

	return rc;
}
