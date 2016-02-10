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

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_mempool.h>
#include <rte_malloc.h>
#include <rte_lcore.h>

#include "spdk/file.h"
#include "spdk/nvme.h"
#include "spdk/pci.h"
#include "spdk/string.h"

struct ctrlr_entry {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct ctrlr_entry	*next;
	char			name[1024];
};

struct ns_entry {
	struct spdk_nvme_ns	*ns;
	struct ns_entry		*next;
	uint32_t		io_size_blocks;
	uint64_t		size_in_ios;
	char			name[1024];
};

struct ns_worker_ctx {
	struct ns_entry		*entry;
	struct ctrlr_entry	*ctr_entry;
	uint64_t		io_completed;
	uint64_t		io_completed_error;
	uint64_t		io_submitted;
	uint64_t		current_queue_depth;
	uint64_t		offset_in_ios;
	bool			is_draining;

	struct ns_worker_ctx	*next;
};

struct reset_task {
	struct ns_worker_ctx	*ns_ctx;
	void			*buf;
};

struct worker_thread {
	struct ns_worker_ctx 	*ns_ctx;
	unsigned		lcore;
};

struct rte_mempool *request_mempool;
static struct rte_mempool *task_pool;

static struct ctrlr_entry *g_controllers = NULL;
static struct ns_entry *g_namespaces = NULL;
static int g_num_namespaces = 0;
static struct worker_thread *g_workers = NULL;

static uint64_t g_tsc_rate;

static int g_io_size_bytes;
static int g_rw_percentage;
static int g_is_random;
static int g_queue_depth;
static int g_time_in_sec;

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;
	const struct spdk_nvme_ctrlr_data *cdata;

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	entry->ns = ns;
	entry->size_in_ios = spdk_nvme_ns_get_size(ns) /
			     g_io_size_bytes;
	entry->io_size_blocks = g_io_size_bytes / spdk_nvme_ns_get_sector_size(ns);

	snprintf(entry->name, 44, "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	g_num_namespaces++;
	entry->next = g_namespaces;
	g_namespaces = entry;
}

static void
register_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	int nsid, num_ns;
	struct ctrlr_entry *entry = malloc(sizeof(struct ctrlr_entry));

	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	entry->ctrlr = ctrlr;
	entry->next = g_controllers;
	g_controllers = entry;

	num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
	for (nsid = 1; nsid <= num_ns; nsid++) {
		register_ns(ctrlr, spdk_nvme_ctrlr_get_ns(ctrlr, nsid));
	}
}

static void task_ctor(struct rte_mempool *mp, void *arg, void *__task, unsigned id)
{
	struct reset_task *task = __task;

	task->buf = rte_malloc(NULL, g_io_size_bytes, 0x200);
	if (task->buf == NULL) {
		fprintf(stderr, "task->buf rte_malloc failed\n");
		exit(1);
	}
}

static void io_complete(void *ctx, const struct spdk_nvme_cpl *completion);

static __thread unsigned int seed = 0;

static void
submit_single_io(struct ns_worker_ctx *ns_ctx)
{
	struct reset_task	*task = NULL;
	uint64_t		offset_in_ios;
	int			rc;
	struct ns_entry		*entry = ns_ctx->entry;

	if (rte_mempool_get(task_pool, (void **)&task) != 0) {
		fprintf(stderr, "task_pool rte_mempool_get failed\n");
		exit(1);
	}

	task->ns_ctx = ns_ctx;
	task->ns_ctx->io_submitted++;

	if (g_is_random) {
		offset_in_ios = rand_r(&seed) % entry->size_in_ios;
	} else {
		offset_in_ios = ns_ctx->offset_in_ios++;
		if (ns_ctx->offset_in_ios == entry->size_in_ios) {
			ns_ctx->offset_in_ios = 0;
		}
	}

	if ((g_rw_percentage == 100) ||
	    (g_rw_percentage != 0 && ((rand_r(&seed) % 100) < g_rw_percentage))) {
		rc = spdk_nvme_ns_cmd_read(entry->ns, task->buf, offset_in_ios * entry->io_size_blocks,
					   entry->io_size_blocks, io_complete, task, 0);
	} else {
		rc = spdk_nvme_ns_cmd_write(entry->ns, task->buf, offset_in_ios * entry->io_size_blocks,
					    entry->io_size_blocks, io_complete, task, 0);
	}

	if (rc != 0) {
		fprintf(stderr, "starting I/O failed\n");
	}

	ns_ctx->current_queue_depth++;
}

static void
task_complete(struct reset_task *task, const struct spdk_nvme_cpl *completion)
{
	struct ns_worker_ctx	*ns_ctx;

	ns_ctx = task->ns_ctx;
	ns_ctx->current_queue_depth--;

	if (spdk_nvme_cpl_is_error(completion)) {
		ns_ctx->io_completed_error++;
	} else {
		ns_ctx->io_completed++;
	}

	rte_mempool_put(task_pool, task);

	/*
	 * is_draining indicates when time has expired for the test run
	 * and we are just waiting for the previously submitted I/O
	 * to complete.  In this case, do not submit a new I/O to replace
	 * the one just completed.
	 */
	if (!ns_ctx->is_draining) {
		submit_single_io(ns_ctx);
	}
}

static void
io_complete(void *ctx, const struct spdk_nvme_cpl *completion)
{
	task_complete((struct reset_task *)ctx, completion);
}

static void
check_io(struct ns_worker_ctx *ns_ctx)
{
	spdk_nvme_ctrlr_process_io_completions(ns_ctx->ctr_entry->ctrlr, 0);
}

static void
submit_io(struct ns_worker_ctx *ns_ctx, int queue_depth)
{
	while (queue_depth-- > 0) {
		submit_single_io(ns_ctx);
	}
}

static void
drain_io(struct ns_worker_ctx *ns_ctx)
{
	ns_ctx->is_draining = true;
	while (ns_ctx->current_queue_depth > 0) {
		check_io(ns_ctx);
	}
}

static int
work_fn(void *arg)
{
	uint64_t tsc_end = rte_get_timer_cycles() + g_time_in_sec * g_tsc_rate;
	struct worker_thread *worker = (struct worker_thread *)arg;
	struct ns_worker_ctx *ns_ctx = NULL;

	printf("Starting thread on core %u\n", worker->lcore);

	if (spdk_nvme_register_io_thread() != 0) {
		fprintf(stderr, "spdk_nvme_register_io_thread() failed on core %u\n", worker->lcore);
		return -1;
	}

	/* Submit initial I/O for each namespace. */
	ns_ctx = worker->ns_ctx;
	while (ns_ctx != NULL) {
		submit_io(ns_ctx, g_queue_depth);
		ns_ctx = ns_ctx->next;
	}

	while (1) {
		/*
		 * Check for completed I/O for each controller. A new
		 * I/O will be submitted in the io_complete callback
		 * to replace each I/O that is completed.
		 */
		ns_ctx = worker->ns_ctx;
		while (ns_ctx != NULL) {
			check_io(ns_ctx);
			ns_ctx = ns_ctx->next;
		}

		if (((tsc_end - rte_get_timer_cycles()) / g_tsc_rate) > (uint64_t)g_time_in_sec / 5 &&
		    ((tsc_end - rte_get_timer_cycles()) / g_tsc_rate) < (uint64_t)(g_time_in_sec / 5 + 10)) {
			ns_ctx = worker->ns_ctx;
			while (ns_ctx != NULL) {
				if (spdk_nvme_ctrlr_reset(ns_ctx->ctr_entry->ctrlr) < 0) {
					fprintf(stderr, "nvme reset failed.\n");
					return -1;
				}
				ns_ctx = ns_ctx->next;
			}
		}

		if (rte_get_timer_cycles() > tsc_end) {
			break;
		}
	}

	ns_ctx = worker->ns_ctx;
	while (ns_ctx != NULL) {
		drain_io(ns_ctx);
		ns_ctx = ns_ctx->next;
	}

	spdk_nvme_unregister_io_thread();

	return 0;
}

static void usage(char *program_name)
{
	printf("%s options", program_name);
	printf("\n");
	printf("\t[-q io depth]\n");
	printf("\t[-s io size in bytes]\n");
	printf("\t[-w io pattern type, must be one of\n");
	printf("\t\t(read, write, randread, randwrite, rw, randrw)]\n");
	printf("\t[-M rwmixread (100 for reads, 0 for writes)]\n");
	printf("\t[-t time in seconds(should be larger than 15 seconds)]\n");
	printf("\t[-m max completions per poll]\n");
	printf("\t\t(default:0 - unlimited)\n");
}

static int
print_stats(void)
{
	uint64_t io_completed, io_submitted, io_completed_error;
	uint64_t total_completed_io, total_submitted_io, total_completed_err_io;
	struct worker_thread	*worker;
	struct ns_worker_ctx	*ns_ctx;

	total_completed_io = 0;
	total_submitted_io = 0;
	total_completed_err_io = 0;

	worker = g_workers;
	ns_ctx = worker->ns_ctx;
	while (ns_ctx) {
		io_completed = ns_ctx->io_completed;
		io_submitted = ns_ctx->io_submitted;
		io_completed_error = ns_ctx->io_completed_error;
		total_completed_io += io_completed;
		total_submitted_io += io_submitted;
		total_completed_err_io += io_completed_error;
		ns_ctx = ns_ctx->next;
	}

	printf("========================================================\n");
	printf("%16lu IO completed successfully\n", total_completed_io);
	printf("%16lu IO completed with error\n", total_completed_err_io);
	printf("--------------------------------------------------------\n");
	printf("%16lu IO completed total\n", total_completed_io + total_completed_err_io);
	printf("%16lu IO submitted\n", total_submitted_io);

	if (total_submitted_io != (total_completed_io + total_completed_err_io)) {
		fprintf(stderr, "Some IO are missing......\n");
		return -1;
	}

	return 0;
}

static int
parse_args(int argc, char **argv)
{
	const char *workload_type;
	int op;
	bool mix_specified = false;

	/* default value*/
	g_queue_depth = 0;
	g_io_size_bytes = 0;
	workload_type = NULL;
	g_time_in_sec = 0;
	g_rw_percentage = -1;

	while ((op = getopt(argc, argv, "m:q:s:t:w:M:")) != -1) {
		switch (op) {
		case 'q':
			g_queue_depth = atoi(optarg);
			break;
		case 's':
			g_io_size_bytes = atoi(optarg);
			break;
		case 't':
			g_time_in_sec = atoi(optarg);
			break;
		case 'w':
			workload_type = optarg;
			break;
		case 'M':
			g_rw_percentage = atoi(optarg);
			mix_specified = true;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!g_queue_depth) {
		usage(argv[0]);
		return 1;
	}
	if (!g_io_size_bytes) {
		usage(argv[0]);
		return 1;
	}
	if (!workload_type) {
		usage(argv[0]);
		return 1;
	}
	if (!g_time_in_sec) {
		usage(argv[0]);
		return 1;
	}

	if (strcmp(workload_type, "read") &&
	    strcmp(workload_type, "write") &&
	    strcmp(workload_type, "randread") &&
	    strcmp(workload_type, "randwrite") &&
	    strcmp(workload_type, "rw") &&
	    strcmp(workload_type, "randrw")) {
		fprintf(stderr,
			"io pattern type must be one of\n"
			"(read, write, randread, randwrite, rw, randrw)\n");
		return 1;
	}

	if (!strcmp(workload_type, "read") ||
	    !strcmp(workload_type, "randread")) {
		g_rw_percentage = 100;
	}

	if (!strcmp(workload_type, "write") ||
	    !strcmp(workload_type, "randwrite")) {
		g_rw_percentage = 0;
	}

	if (!strcmp(workload_type, "read") ||
	    !strcmp(workload_type, "randread") ||
	    !strcmp(workload_type, "write") ||
	    !strcmp(workload_type, "randwrite")) {
		if (mix_specified) {
			fprintf(stderr, "Ignoring -M option... Please use -M option"
				" only when using rw or randrw.\n");
		}
	}

	if (!strcmp(workload_type, "rw") ||
	    !strcmp(workload_type, "randrw")) {
		if (g_rw_percentage < 0 || g_rw_percentage > 100) {
			fprintf(stderr,
				"-M must be specified to value from 0 to 100 "
				"for rw or randrw.\n");
			return 1;
		}
	}

	if (!strcmp(workload_type, "read") ||
	    !strcmp(workload_type, "write") ||
	    !strcmp(workload_type, "rw")) {
		g_is_random = 0;
	} else {
		g_is_random = 1;
	}

	optind = 1;
	return 0;
}

static int
register_workers(void)
{
	struct worker_thread *worker;

	worker = malloc(sizeof(struct worker_thread));
	if (worker == NULL) {
		perror("worker_thread malloc");
		return -1;
	}

	memset(worker, 0, sizeof(struct worker_thread));
	worker->lcore = rte_get_master_lcore();

	g_workers = worker;

	return 0;
}


static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *dev)
{
	if (spdk_pci_device_has_non_uio_driver(dev)) {
		fprintf(stderr, "non-uio kernel driver attached to NVMe\n");
		fprintf(stderr, " controller at PCI address %04x:%02x:%02x.%02x\n",
			spdk_pci_device_get_domain(dev),
			spdk_pci_device_get_bus(dev),
			spdk_pci_device_get_dev(dev),
			spdk_pci_device_get_func(dev));
		fprintf(stderr, " skipping...\n");
		return false;
	}

	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *pci_dev, struct spdk_nvme_ctrlr *ctrlr)
{
	register_ctrlr(ctrlr);
}

static int
register_controllers(void)
{
	printf("Initializing NVMe Controllers\n");

	if (spdk_nvme_probe(NULL, probe_cb, attach_cb) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	return 0;
}

static void
unregister_controllers(void)
{
	struct ctrlr_entry *entry = g_controllers;

	while (entry) {
		struct ctrlr_entry *next = entry->next;
		spdk_nvme_detach(entry->ctrlr);
		free(entry);
		entry = next;
	}
}

static int
associate_workers_with_ns(void)
{
	struct ns_entry		*entry = g_namespaces;
	struct ctrlr_entry	*controller_entry = g_controllers;
	struct worker_thread	*worker = g_workers;
	struct ns_worker_ctx	*ns_ctx;
	int			i, count;

	count = g_num_namespaces;

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
		ns_ctx->entry = entry;
		ns_ctx->ctr_entry = controller_entry;
		ns_ctx->next = worker->ns_ctx;
		worker->ns_ctx = ns_ctx;

		worker = g_workers;

		controller_entry = controller_entry->next;
		entry = entry->next;
		if (entry == NULL) {
			entry = g_namespaces;
		}
	}

	return 0;
}

static int
run_nvme_reset_cycle(int retry_count)
{
	struct worker_thread *worker;
	struct ns_worker_ctx *ns_ctx;

	spdk_nvme_retry_count = retry_count;

	if (work_fn(g_workers) != 0) {
		return -1;
	}

	if (print_stats() != 0) {
		return -1;
	}

	worker = g_workers;
	ns_ctx = worker->ns_ctx;
	while (ns_ctx != NULL) {
		ns_ctx->io_completed = 0;
		ns_ctx->io_completed_error = 0;
		ns_ctx->io_submitted = 0;
		ns_ctx->is_draining = false;
		ns_ctx = ns_ctx->next;
	}

	return 0;
}

static char *ealargs[] = {
	"reset",
	"-c 0x1",
	"-n 4",
};

int main(int argc, char **argv)
{
	int rc;
	int i;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	rc = rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]), ealargs);

	if (rc < 0) {
		fprintf(stderr, "could not initialize dpdk\n");
		return 1;
	}

	request_mempool = rte_mempool_create("nvme_request", 8192,
					     spdk_nvme_request_size(), 128, 0,
					     NULL, NULL, NULL, NULL,
					     SOCKET_ID_ANY, 0);

	if (request_mempool == NULL) {
		fprintf(stderr, "could not initialize request mempool\n");
		return 1;
	}

	task_pool = rte_mempool_create("task_pool", 8192,
				       sizeof(struct reset_task),
				       64, 0, NULL, NULL, task_ctor, NULL,
				       SOCKET_ID_ANY, 0);

	g_tsc_rate = rte_get_timer_hz();

	if (register_workers() != 0) {
		return 1;
	}

	if (register_controllers() != 0) {
		return 1;
	}

	if (associate_workers_with_ns() != 0) {
		rc = 1;
		goto cleanup;
	}

	printf("Initialization complete. Launching workers.\n");

	for (i = 2; i >= 0; i--) {
		rc = run_nvme_reset_cycle(i);
		if (rc != 0) {
			goto cleanup;
		}
	}

cleanup:
	unregister_controllers();

	if (rc != 0) {
		fprintf(stderr, "%s: errors occured\n", argv[0]);
	}

	return rc;
}
