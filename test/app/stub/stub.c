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
#include "spdk/nvme.h"
#include "spdk/string.h"
#include "spdk/thread.h"

static char g_path[256];
static struct spdk_poller *g_poller;

struct ctrlr_entry {
	struct spdk_nvme_ctrlr *ctrlr;
	TAILQ_ENTRY(ctrlr_entry) link;
};

static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);

static void
cleanup(void)
{
	struct ctrlr_entry *ctrlr_entry, *tmp;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	TAILQ_FOREACH_SAFE(ctrlr_entry, &g_controllers, link, tmp) {
		TAILQ_REMOVE(&g_controllers, ctrlr_entry, link);
		spdk_nvme_detach_async(ctrlr_entry->ctrlr, &detach_ctx);
		free(ctrlr_entry);
	}

	while (detach_ctx && spdk_nvme_detach_poll_async(detach_ctx) == -EAGAIN) {
		;
	}
}

static void
usage(char *executable_name)
{
	printf("%s [options]\n", executable_name);
	printf("options:\n");
	printf(" -i shared memory ID [required]\n");
	printf(" -m mask    core mask for DPDK\n");
	printf(" -n channel number of memory channels used for DPDK\n");
	printf(" -p core    master (primary) core for DPDK\n");
	printf(" -s size    memory size in MB for DPDK\n");
	printf(" -H         show this usage\n");
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	/*
	 * Set the io_queue_size to UINT16_MAX to initialize
	 * the controller with the possible largest queue size.
	 */
	opts->io_queue_size = UINT16_MAX;
	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct ctrlr_entry *entry;

	entry = malloc(sizeof(struct ctrlr_entry));
	if (entry == NULL) {
		fprintf(stderr, "Malloc error\n");
		exit(1);
	}

	entry->ctrlr = ctrlr;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);
}

static int
stub_sleep(void *arg)
{
	usleep(1000 * 1000);
	return 0;
}

static void
stub_start(void *arg1)
{
	int shm_id = (intptr_t)arg1;

	spdk_unaffinitize_thread();

	if (spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		exit(1);
	}

	snprintf(g_path, sizeof(g_path), "/var/run/spdk_stub%d", shm_id);
	if (mknod(g_path, S_IFREG, 0) != 0) {
		fprintf(stderr, "could not create sentinel file %s\n", g_path);
		exit(1);
	}

	g_poller = SPDK_POLLER_REGISTER(stub_sleep, NULL, 0);
}

static void
stub_shutdown(void)
{
	spdk_poller_unregister(&g_poller);
	unlink(g_path);
	spdk_app_stop(0);
}

int
main(int argc, char **argv)
{
	int ch;
	struct spdk_app_opts opts = {};
	long int val;

	/* default value in opts structure */
	spdk_app_opts_init(&opts);

	opts.name = "stub";
	opts.rpc_addr = NULL;

	while ((ch = getopt(argc, argv, "i:m:n:p:s:H")) != -1) {
		if (ch == 'm') {
			opts.reactor_mask = optarg;
		} else if (ch == '?') {
			usage(argv[0]);
			exit(1);
		} else {
			val = spdk_strtol(optarg, 10);
			if (val < 0) {
				fprintf(stderr, "Converting a string to integer failed\n");
				exit(1);
			}
			switch (ch) {
			case 'i':
				opts.shm_id = val;
				break;
			case 'n':
				opts.mem_channel = val;
				break;
			case 'p':
				opts.master_core = val;
				break;
			case 's':
				opts.mem_size = val;
				break;
			case 'H':
			default:
				usage(argv[0]);
				exit(EXIT_SUCCESS);
			}
		}
	}

	if (opts.shm_id < 0) {
		fprintf(stderr, "%s: -i shared memory ID must be specified\n", argv[0]);
		usage(argv[0]);
		exit(1);
	}

	opts.shutdown_cb = stub_shutdown;

	ch = spdk_app_start(&opts, stub_start, (void *)(intptr_t)opts.shm_id);

	cleanup();
	spdk_app_fini();

	return ch;
}
