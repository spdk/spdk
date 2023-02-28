/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/event.h"
#include "spdk/nvme.h"
#include "spdk/string.h"
#include "spdk/thread.h"

static char g_path[256];
static struct spdk_poller *g_poller;
/* default sleep time in ms */
static uint32_t g_sleep_time = 1000;

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

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
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
	printf(" -p core    main (primary) core for DPDK\n");
	printf(" -s size    memory size in MB for DPDK\n");
	printf(" -t msec    sleep time (ms) between checking for admin completions\n");
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
	struct ctrlr_entry *ctrlr_entry, *tmp;

	usleep(g_sleep_time * 1000);
	TAILQ_FOREACH_SAFE(ctrlr_entry, &g_controllers, link, tmp) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr_entry->ctrlr);
	}
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
	spdk_app_opts_init(&opts, sizeof(opts));

	opts.name = "stub";
	opts.rpc_addr = NULL;

	while ((ch = getopt(argc, argv, "i:m:n:p:s:t:H")) != -1) {
		if (ch == 'm') {
			opts.reactor_mask = optarg;
		} else if (ch == '?' || ch == 'H') {
			usage(argv[0]);
			exit(EXIT_SUCCESS);
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
				opts.main_core = val;
				break;
			case 's':
				opts.mem_size = val;
				break;
			case 't':
				g_sleep_time = val;
				break;
			default:
				usage(argv[0]);
				exit(EXIT_FAILURE);
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
