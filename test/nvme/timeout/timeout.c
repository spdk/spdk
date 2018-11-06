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
#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/fd.h"
#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/nvme_intel.h"
#include "spdk/histogram_data.h"
#include "spdk/endian.h"
#include "spdk/crc16.h"

#define MAX_DEVS 64

enum timeout_action {
	SPDK_BDEV_NVME_TIMEOUT_ACTION_NONE = 0,
	SPDK_BDEV_NVME_TIMEOUT_ACTION_RESET,
	SPDK_BDEV_NVME_TIMEOUT_ACTION_ABORT,
};

static enum timeout_action g_action_on_timeout = SPDK_BDEV_NVME_TIMEOUT_ACTION_NONE;

struct dev {
	bool				error_expected;
	struct spdk_nvme_ctrlr		*ctrlr;
	struct spdk_nvme_ns		*ns;
	struct spdk_nvme_qpair		*qpair;
	void				*data;
	char				name[SPDK_NVMF_TRADDR_MAX_LEN + 1];
};

struct worker_thread {
	struct ns_worker_ctx	*ns_ctx;
	struct worker_thread	*next;
	unsigned		lcore;
};

struct ns_worker_ctx {
	struct ns_entry				*entry;
	uint64_t				io_completed;
	uint64_t				min_tsc;
	uint64_t				max_tsc;
	uint64_t				current_queue_depth;
	uint64_t				offset_in_ios;
	bool					is_draining;
	struct spdk_nvme_qpair			*qpair;
	struct ns_worker_ctx			*next;
	struct spdk_histogram_data	*histogram;
};

struct ns_entry {
	struct {
		struct spdk_nvme_ctrlr		*ctrlr;
		struct spdk_nvme_ns		*ns;
	} nvme;

	struct ns_entry				*next;
	uint32_t				io_size_blocks;
	uint64_t				size_in_ios;
	char					name[1024];
};

static struct worker_thread *g_workers = NULL;
static struct ns_entry *g_namespaces = NULL;
static int g_num_namespaces = 0;
static int g_num_workers = 0;

static struct dev devs[MAX_DEVS];
static int num_devs = 0;
static int g_shm_id = -1;
static const char *g_core_mask;
static int failed = 0;

#define foreach_dev(iter) \
	for (iter = devs; iter - devs < num_devs; iter++)

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attaching to %s\n", trid->traddr);

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct dev *dev;
	uint32_t nsid;

	/* add to dev list */
	dev = &devs[num_devs++];
	if (num_devs >= MAX_DEVS) {
		return;
	}

	dev->ctrlr = ctrlr;
	nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	dev->ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	if (dev->ns == NULL) {
		failed = 1;
		return;
	}
	dev->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
	if (dev->qpair == NULL) {
		failed = 1;
		return;
	}

	snprintf(dev->name, sizeof(dev->name), "%s",
		 trid->traddr);

	printf("Attached to %s\n", dev->name);
}

static void
spdk_nvme_abort_cpl(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr *ctrlr = ctx;
	int rc;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("Abort failed. Resetting controller.\n");
		rc = spdk_nvme_ctrlr_reset(ctrlr);
		failed += rc;
		if (rc) {
			printf("Resetting controller failed.\n");
		}
	}
}

static void
timeout_cb(void *cb_arg, struct spdk_nvme_ctrlr *ctrlr,
	   struct spdk_nvme_qpair *qpair, uint16_t cid)
{
	int rc;
	union spdk_nvme_csts_register csts;

	printf("Warning: Detected a timeout. ctrlr=%p qpair=%p cid=%u\n", ctrlr, qpair, cid);

	csts = spdk_nvme_ctrlr_get_regs_csts(ctrlr);
	if (csts.bits.cfs) {
		printf("Controller Fatal Status, reset required\n");
		rc = spdk_nvme_ctrlr_reset(ctrlr);
		if (rc) {
			printf("Resetting controller failed.\n");
		}
		return;
	}

	switch (g_action_on_timeout) {
	case SPDK_BDEV_NVME_TIMEOUT_ACTION_ABORT:
		if (qpair) {
			rc = spdk_nvme_ctrlr_cmd_abort(ctrlr, qpair, cid,
						       spdk_nvme_abort_cpl, ctrlr);
			if (rc == 0) {
				return;
			}

			printf("Unable to send abort. Resetting.\n");
		}

	/* FALLTHROUGH */
	case SPDK_BDEV_NVME_TIMEOUT_ACTION_RESET:
		rc = spdk_nvme_ctrlr_reset(ctrlr);
		if (rc) {
			printf("Resetting controller failed.\n");
		}
		break;
	case SPDK_BDEV_NVME_TIMEOUT_ACTION_NONE:
		printf("No action for nvme controller timeout.\n");
		break;
	default:
		printf("An invalid timeout action value is found.\n");
		break;
	}
}

static void usage(char *program_name)
{
	printf("%s options\n", program_name);
	printf("\t[-c core mask for I/O submission/completion.]\n");
	printf("\t[-i shared memory group ID]\n");
	printf("\t[-w timeout action type: reset, abort\n");
}

static int
parse_args(int argc, char **argv)
{
	int op;
	const char *workload_type;

	g_core_mask = NULL;
	workload_type = NULL;

	while ((op = getopt(argc, argv, "c:i:w:")) != -1) {
		switch (op) {
		case 'c':
			g_core_mask = optarg;
			break;
		case 'i':
			g_shm_id = atoi(optarg);
			break;
		case 'w':
			workload_type = optarg;
			if (strcmp(workload_type, "reset")) {
				g_action_on_timeout = SPDK_BDEV_NVME_TIMEOUT_ACTION_RESET;
			} else if (strcmp(workload_type, "abort")) {
				g_action_on_timeout = SPDK_BDEV_NVME_TIMEOUT_ACTION_ABORT;
			} else {
				usage(argv[0]);
				return 1;
			}
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!workload_type) {
		usage(argv[0]);
		exit(1);
	}

	return 0;
}

static int
register_workers(void)
{
	uint32_t i;
	struct worker_thread *worker;

	g_workers = NULL;
	g_num_workers = 0;

	SPDK_ENV_FOREACH_CORE(i) {
		worker = calloc(1, sizeof(*worker));
		if (worker == NULL) {
			fprintf(stderr, "Unable to allocate worker\n");
			return -1;
		}

		worker->lcore = i;
		worker->next = g_workers;
		g_workers = worker;
		g_num_workers++;
	}

	return 0;
}

static void
unregister_workers(void)
{
	struct worker_thread *worker = g_workers;

	/* Free namespace context and worker thread */
	while (worker) {
		struct worker_thread *next_worker = worker->next;
		struct ns_worker_ctx *ns_ctx = worker->ns_ctx;

		while (ns_ctx) {
			struct ns_worker_ctx *next_ns_ctx = ns_ctx->next;
			spdk_histogram_data_free(ns_ctx->histogram);
			free(ns_ctx);
			ns_ctx = next_ns_ctx;
		}

		free(worker);
		worker = next_worker;
	}
}

static int
associate_workers_with_ns(void)
{
	struct ns_entry		*entry = g_namespaces;
	struct worker_thread	*worker = g_workers;
	struct ns_worker_ctx	*ns_ctx;
	int			i, count;

	count = g_num_namespaces > g_num_workers ? g_num_namespaces : g_num_workers;

	for (i = 0; i < count; i++) {
		if (entry == NULL) {
			break;
		}

		ns_ctx = malloc(sizeof(struct ns_worker_ctx));
		if (!ns_ctx) {
			return -1;
		}
		memset(ns_ctx, 0, sizeof(*ns_ctx));

		printf("Associating %s with lcore %d\n", entry->name, worker->lcore);
		ns_ctx->min_tsc = UINT64_MAX;
		ns_ctx->entry = entry;
		ns_ctx->next = worker->ns_ctx;
		ns_ctx->histogram = spdk_histogram_data_alloc();
		worker->ns_ctx = ns_ctx;

		worker = worker->next;
		if (worker == NULL) {
			worker = g_workers;
		}

		entry = entry->next;
		if (entry == NULL) {
			entry = g_namespaces;
		}

	}

	return 0;
}

static int
work_fn(void *arg)
{
	uint64_t tsc_end;
	struct worker_thread *worker = (struct worker_thread *)arg;
	struct ns_worker_ctx *ns_ctx = NULL;

	printf("Starting thread on core %u\n", worker->lcore);

	/* Allocate a queue pair for each namespace. */
	ns_ctx = worker->ns_ctx;
	while (ns_ctx != NULL) {
		ns_ctx = ns_ctx->next;
	}

	tsc_end = spdk_get_ticks();

	ns_ctx = worker->ns_ctx;
	while (ns_ctx != NULL) {
		ns_ctx = ns_ctx->next;
	}

	while (1) {
		ns_ctx = worker->ns_ctx;
		while (ns_ctx != NULL) {
			ns_ctx = ns_ctx->next;
		}

		if (spdk_get_ticks() > tsc_end) {
			break;
		}
	}

	ns_ctx = worker->ns_ctx;
	while (ns_ctx != NULL) {
		ns_ctx = ns_ctx->next;
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct dev		*dev;
	int			i;
	struct spdk_env_opts	opts;
	int			rc;
	uint64_t g_timeout = 10; /* Set a timeout threshold in 'us' */
	struct worker_thread *worker, *master_worker;
	uint32_t master_core;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	spdk_env_opts_init(&opts);
	opts.name = "timeout";
	opts.core_mask = "0x01";
	opts.mem_size = 64;
	opts.shm_id = g_shm_id;
	if (g_core_mask) {
		opts.core_mask = g_core_mask;
	}
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	printf("NVMe Timeout test\n");

	if (register_workers() != 0) {
		rc = -1;
		goto cleanup;
	}

	if (associate_workers_with_ns() != 0) {
		rc = -1;
		goto cleanup;
	}

	printf("Initialization complete. Launching workers.\n");

	if (spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	foreach_dev(dev) {
		spdk_nvme_ctrlr_register_timeout_callback(dev->ctrlr, g_timeout,
				timeout_cb, NULL);
		rc = spdk_nvme_qpair_process_completions(dev->qpair, 0);
		failed += rc;
	}

	/* Launch all of the slave workers */
	master_core = spdk_env_get_current_core();
	master_worker = NULL;
	worker = g_workers;
	while (worker != NULL) {
		if (worker->lcore != master_core) {
			spdk_env_thread_launch_pinned(worker->lcore, work_fn, worker);
		} else {
			assert(master_worker == NULL);
			master_worker = worker;
		}
		worker = worker->next;
	}

	assert(master_worker != NULL);
	rc = work_fn(master_worker);

	spdk_env_thread_wait_all();

cleanup:
	printf("Cleaning up...\n");
	for (i = 0; i < num_devs; i++) {
		struct dev *dev = &devs[i];
		spdk_nvme_detach(dev->ctrlr);
	}

	unregister_workers();
	if (rc != 0) {
		fprintf(stderr, "%s: unexpected errors occured!\n", argv[0]);
	}

	return rc;
}

