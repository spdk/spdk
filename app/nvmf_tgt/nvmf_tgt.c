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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include <rte_config.h>
#include <rte_memzone.h>
#include <rte_mempool.h>

#include "conf.h"

#include "spdk/event.h"

#include "nvmf/transport.h"
#include "nvmf/subsystem.h"

#include "spdk/log.h"
#include "spdk/nvme.h"

struct rte_mempool *request_mempool;

#define SPDK_NVMF_BUILD_ETC "/usr/local/etc/nvmf"
#define SPDK_NVMF_DEFAULT_CONFIG SPDK_NVMF_BUILD_ETC "/nvmf.conf"

#define ACCEPT_TIMEOUT_US		10000 /* 10ms */

static struct spdk_poller *g_acceptor_poller = NULL;

static void
acceptor_poller_unregistered_event(struct spdk_event *event)
{
	int rc;

	spdk_nvmf_acceptor_fini();
	spdk_nvmf_transport_fini();
	spdk_shutdown_nvmf_subsystems();

	rc = spdk_nvmf_check_pools();

	spdk_app_stop(rc);
}

static void
spdk_nvmf_shutdown_cb(void)
{
	struct spdk_event *event;

	fprintf(stdout, "\n=========================\n");
	fprintf(stdout, "   NVMF shutdown signal\n");
	fprintf(stdout, "=========================\n");

	event = spdk_event_allocate(spdk_app_get_current_core(), acceptor_poller_unregistered_event,
				    NULL, NULL, NULL);
	spdk_poller_unregister(&g_acceptor_poller, event);
}

static void
usage(void)
{
	printf("nvmf [options]\n");
	printf("options:\n");
	printf(" -c config  - config file (default %s)\n", SPDK_NVMF_DEFAULT_CONFIG);
	printf(" -e mask    - tracepoint group mask for spdk trace buffers (default 0x0)\n");
	printf(" -m mask    - core mask for DPDK\n");
	printf(" -i instance ID\n");
	printf(" -l facility - use specific syslog facility (default %s)\n",
	       SPDK_APP_DEFAULT_LOG_FACILITY);
	printf(" -n channel number of memory channels used for DPDK\n");
	printf(" -p core    master (primary) core for DPDK\n");
	printf(" -s size    memory size in MB for DPDK\n");

	spdk_tracelog_usage(stdout, "-t");

	printf(" -v         - verbose (enable warnings)\n");
	printf(" -H         - show this usage\n");
	printf(" -d         - disable coredump file enabling\n");
}

static void
acceptor_poll(void *arg)
{
	spdk_nvmf_acceptor_poll();
}

static void
spdk_nvmf_startup(spdk_event_t event)
{
	int rc;

	rc = spdk_nvmf_parse_conf();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_nvmf_parse_conf() failed\n");
		goto initialize_error;
	}

	rc = spdk_nvmf_transport_init();
	if (rc <= 0) {
		SPDK_ERRLOG("Transport initialization failed\n");
		goto initialize_error;
	}

	rc = spdk_nvmf_acceptor_init();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_nvmf_acceptor_start() failed\n");
		goto initialize_error;
	}

	spdk_poller_register(&g_acceptor_poller, acceptor_poll, NULL,
			     g_spdk_nvmf_tgt_conf.acceptor_lcore, NULL, ACCEPT_TIMEOUT_US);

	SPDK_NOTICELOG("Acceptor running on core %u\n", g_spdk_nvmf_tgt_conf.acceptor_lcore);

	if (getenv("MEMZONE_DUMP") != NULL) {
		rte_memzone_dump(stdout);
		fflush(stdout);
	}

	return;

initialize_error:
	spdk_app_stop(rc);
}

/*! \file

This is the main file.

*/

/*!

\brief This is the main function for the NVMf target application.

\msc

	c_runtime [label="C Runtime"], dpdk [label="DPDK"], nvmf [label="NVMf target"];
	c_runtime=>nvmf [label="main()"];
	nvmf=> [label="rte_eal_init()"];
	nvmf=>nvmf [label="spdk_app_init()"];
	nvmf=>nvmf [label="spdk_event_allocate()"];
	nvmf=>nvmf [label="spdk_app_start()"];
	nvmf=>nvmf [label="spdk_app_fini()"];
	nvmf=>nvmf [label="spdk_nvmf_check_pools()"];
	c_runtime<<nvmf;

\endmsc

*/

int
main(int argc, char **argv)
{
	int ch;
	int rc;
	struct spdk_app_opts opts = {};

	/* default value in opts */
	spdk_app_opts_init(&opts);

	opts.name = "nvmf";
	opts.config_file = SPDK_NVMF_DEFAULT_CONFIG;
	opts.max_delay_us = 1000; /* 1 ms */

	while ((ch = getopt(argc, argv, "c:de:i:l:m:n:p:qs:t:DH")) != -1) {
		switch (ch) {
		case 'd':
			opts.enable_coredump = false;
			break;
		case 'c':
			opts.config_file = optarg;
			break;
		case 'i':
			opts.instance_id = atoi(optarg);
			break;
		case 'l':
			opts.log_facility = optarg;
			break;
		case 't':
			rc = spdk_log_set_trace_flag(optarg);
			if (rc < 0) {
				fprintf(stderr, "unknown flag\n");
				usage();
				exit(EXIT_FAILURE);
			}
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
			opts.dpdk_mem_channel = atoi(optarg);
			break;
		case 'p':
			opts.dpdk_master_core = atoi(optarg);
			break;
		case 's':
			opts.dpdk_mem_size = atoi(optarg);
			break;
		case 'e':
			opts.tpoint_group_mask = optarg;
			break;
		case 'q':
			spdk_g_notice_stderr_flag = 0;
			break;
		case 'D':
		case 'H':
		default:
			usage();
			exit(EXIT_SUCCESS);
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

	opts.shutdown_cb = spdk_nvmf_shutdown_cb;
	spdk_app_init(&opts);

	printf("Total cores available: %d\n", rte_lcore_count());
	/* Blocks until the application is exiting */
	rc = spdk_app_start(spdk_nvmf_startup, NULL, NULL);

	spdk_app_fini();

	return rc;
}
