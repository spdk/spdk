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
#include "nvmf_tgt.h"
#include "spdk/event.h"
#include "spdk/log.h"

#define SPDK_NVMF_BUILD_ETC "/usr/local/etc/nvmf"
#define SPDK_NVMF_DEFAULT_CONFIG SPDK_NVMF_BUILD_ETC "/nvmf.conf"

static void
usage(void)
{
	struct spdk_app_opts opts;

	spdk_app_opts_init(&opts);

	printf("nvmf [options]\n");
	printf("options:\n");
	printf(" -c config  - config file (default %s)\n", SPDK_NVMF_DEFAULT_CONFIG);
	printf(" -e mask    - tracepoint group mask for spdk trace buffers (default 0x0)\n");
	printf(" -m mask    - core mask for DPDK\n");
	printf(" -i shared memory ID (optional)\n");
	printf(" -n channel number of memory channels used for DPDK\n");
	printf(" -p core    master (primary) core for DPDK\n");
	printf(" -s size    memory size in MB for DPDK\n");

	spdk_tracelog_usage(stdout, "-t");

	printf(" -v         - verbose (enable warnings)\n");
	printf(" -H         - show this usage\n");
	printf(" -d         - disable coredump file enabling\n");
}


int
main(int argc, char **argv)
{
	int ch;
	int rc;
	struct spdk_app_opts opts = {};
	enum spdk_log_level print_level = SPDK_LOG_NOTICE;

	/* default value in opts */
	spdk_app_opts_init(&opts);
	opts.name = "nvmf";
	opts.config_file = SPDK_NVMF_DEFAULT_CONFIG;
	opts.max_delay_us = 1000; /* 1 ms */

	while ((ch = getopt(argc, argv, "c:de:i:m:n:p:qs:t:DH")) != -1) {
		switch (ch) {
		case 'd':
			opts.enable_coredump = false;
			break;
		case 'c':
			opts.config_file = optarg;
			break;
		case 'i':
			opts.shm_id = atoi(optarg);
			break;
		case 't':
			rc = spdk_log_set_trace_flag(optarg);
			if (rc < 0) {
				fprintf(stderr, "unknown flag\n");
				usage();
				exit(EXIT_FAILURE);
			}
			print_level = SPDK_LOG_DEBUG;
#ifndef DEBUG
			fprintf(stderr, "%s must be rebuilt with CONFIG_DEBUG=y for -t flag.\n",
				argv[0]);
			usage();
			exit(EXIT_FAILURE);
#endif
			break;
		case 'm':
			opts.reactor_mask = optarg;
			break;
		case 'n':
			opts.mem_channel = atoi(optarg);
			break;
		case 'p':
			opts.master_core = atoi(optarg);
			break;
		case 's':
			opts.mem_size = atoi(optarg);
			break;
		case 'e':
			opts.tpoint_group_mask = optarg;
			break;
		case 'q':
			print_level = SPDK_LOG_WARN;
			break;
		case 'D':
		case 'H':
		default:
			usage();
			exit(EXIT_SUCCESS);
		}
	}

	if (print_level > SPDK_LOG_WARN &&
	    isatty(STDERR_FILENO) &&
	    !strncmp(ttyname(STDERR_FILENO), "/dev/tty", strlen("/dev/tty"))) {
		printf("Warning: printing stderr to console terminal without -q option specified.\n");
		printf("Suggest using -q to disable logging to stderr and monitor syslog, or\n");
		printf("redirect stderr to a file.\n");
		printf("(Delaying for 10 seconds...)\n");
		sleep(10);
	}

	spdk_log_set_print_level(print_level);

	rc = spdk_nvmf_tgt_start(&opts);

	return rc;
}
