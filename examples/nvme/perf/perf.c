/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
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

#include <pciaccess.h>

#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_mempool.h>
#include <rte_malloc.h>
#include <rte_lcore.h>

#include "spdk/nvme.h"
#include "spdk/pci.h"

struct ctrlr_entry {
	struct nvme_controller	*ctrlr;
	struct ctrlr_entry	*next;
};

struct ns_entry {
	struct nvme_controller	*ctrlr;
	struct nvme_namespace	*ns;
	struct ns_entry		*next;
	uint32_t		io_size_blocks;
	int			io_completed;
	int			current_queue_depth;
	uint64_t		size_in_ios;
	uint64_t		offset_in_ios;
	bool			is_draining;
	char			name[1024];
};

struct perf_task {
	struct ns_entry		*entry;
	void			*buf;
};

struct worker_thread {
	struct ns_entry 	*namespaces;
	struct worker_thread	*next;
	unsigned		lcore;
};

struct rte_mempool *request_mempool;
static struct rte_mempool *task_pool;

static struct ctrlr_entry *g_controllers = NULL;
static struct worker_thread *g_workers = NULL;
static struct worker_thread *g_current_worker = NULL;

static uint64_t g_tsc_rate;

static int g_io_size_bytes;
static int g_rw_percentage;
static int g_is_random;
static int g_queue_depth;
static int g_time_in_sec;

static const char *g_core_mask;


static void
register_ns(struct nvme_controller *ctrlr, struct pci_device *pci_dev, struct nvme_namespace *ns)
{
	struct worker_thread *worker;
	struct ns_entry *entry = malloc(sizeof(struct ns_entry));
	const struct nvme_controller_data *cdata = nvme_ctrlr_get_data(ctrlr);

	worker = g_current_worker;

	entry->ctrlr = ctrlr;
	entry->ns = ns;
	entry->next = worker->namespaces;
	entry->io_completed = 0;
	entry->current_queue_depth = 0;
	entry->offset_in_ios = 0;
	entry->size_in_ios = nvme_ns_get_size(ns) /
			     g_io_size_bytes;
	entry->io_size_blocks = g_io_size_bytes / nvme_ns_get_sector_size(ns);
	entry->is_draining = false;

	snprintf(entry->name, sizeof(cdata->mn), "%s", cdata->mn);
	printf("Assigning namespace %s to lcore %u\n", entry->name, worker->lcore);
	worker->namespaces = entry;

	if (worker->next == NULL) {
		g_current_worker = g_workers;
	} else {
		g_current_worker = worker->next;
	}
}

static void
register_ctrlr(struct nvme_controller *ctrlr, struct pci_device *pci_dev)
{
	int nsid, num_ns;
	struct ctrlr_entry *entry = malloc(sizeof(struct ctrlr_entry));

	entry->ctrlr = ctrlr;
	entry->next = g_controllers;
	g_controllers = entry;

	num_ns = nvme_ctrlr_get_num_ns(ctrlr);
	for (nsid = 1; nsid <= num_ns; nsid++) {
		register_ns(ctrlr, pci_dev, nvme_ctrlr_get_ns(ctrlr, nsid));
	}

}

void task_ctor(struct rte_mempool *mp, void *arg, void *__task, unsigned id)
{
	struct perf_task *task = __task;
	task->buf = rte_malloc(NULL, g_io_size_bytes, 0x200);
}

static void io_complete(void *ctx, const struct nvme_completion *completion);

static unsigned int __thread seed = 0;

static void
submit_single_io(struct ns_entry *entry)
{
	struct perf_task	*task = NULL;
	uint64_t		offset_in_ios;
	int			rc;

	rte_mempool_get(task_pool, (void **)&task);

	task->entry = entry;

	if (g_is_random) {
		offset_in_ios = rand_r(&seed) % entry->size_in_ios;
	} else {
		offset_in_ios = entry->offset_in_ios++;
		if (entry->offset_in_ios == entry->size_in_ios) {
			entry->offset_in_ios = 0;
		}
	}

	if ((g_rw_percentage == 100) ||
	    (g_rw_percentage != 0 && ((rand_r(&seed) % 100) < g_rw_percentage))) {
		rc = nvme_ns_cmd_read(entry->ns, task->buf, offset_in_ios * entry->io_size_blocks,
				      entry->io_size_blocks, io_complete, task);
	} else {
		rc = nvme_ns_cmd_write(entry->ns, task->buf, offset_in_ios * entry->io_size_blocks,
				       entry->io_size_blocks, io_complete, task);
	}

	if (rc != 0) {
		fprintf(stderr, "starting I/O failed\n");
	}

	entry->current_queue_depth++;
}

static void
io_complete(void *ctx, const struct nvme_completion *completion)
{
	struct perf_task	*task;
	struct ns_entry		*entry;

	task = (struct perf_task *)ctx;

	entry = task->entry;
	entry->current_queue_depth--;
	entry->io_completed++;

	rte_mempool_put(task_pool, task);

	/*
	 * is_draining indicates when time has expired for the test run
	 * and we are just waiting for the previously submitted I/O
	 * to complete.  In this case, do not submit a new I/O to replace
	 * the one just completed.
	 */
	if (!entry->is_draining) {
		submit_single_io(entry);
	}
}

static void
check_io(struct ns_entry *entry)
{
	nvme_ctrlr_process_io_completions(entry->ctrlr);
}

static void
submit_io(struct ns_entry *entry, int queue_depth)
{
	while (queue_depth-- > 0) {
		submit_single_io(entry);
	}
}

static void
drain_io(struct ns_entry *entry)
{
	entry->is_draining = true;
	while (entry->current_queue_depth > 0) {
		check_io(entry);
	}
}

static int
work_fn(void *arg)
{
	uint64_t tsc_end = rte_get_timer_cycles() + g_time_in_sec * g_tsc_rate;
	struct worker_thread *worker = (struct worker_thread *)arg;
	struct ns_entry *entry = NULL;

	printf("Starting thread on core %u\n", worker->lcore);

	nvme_register_io_thread();

	/* Submit initial I/O for each namespace. */
	entry = worker->namespaces;
	while (entry != NULL) {

		submit_io(entry, g_queue_depth);
		entry = entry->next;
	}

	while (1) {
		/*
		 * Check for completed I/O for each controller. A new
		 * I/O will be submitted in the io_complete callback
		 * to replace each I/O that is completed.
		 */
		entry = worker->namespaces;
		while (entry != NULL) {
			check_io(entry);
			entry = entry->next;
		}

		if (rte_get_timer_cycles() > tsc_end) {
			break;
		}
	}

	entry = worker->namespaces;
	while (entry != NULL) {
		drain_io(entry);
		entry = entry->next;
	}

	nvme_unregister_io_thread();

	return 0;
}

static void usage(char *program_name)
{
	printf("%s options\n", program_name);
	printf("\t[-q io depth]\n");
	printf("\t[-s io size in bytes]\n");
	printf("\t[-w io pattern type, must be one of\n");
	printf("\t\t(read, write, randread, randwrite, rw, randrw)]\n");
	printf("\t[-M rwmixread (100 for reads, 0 for writes)]\n");
	printf("\t[-t time in seconds]\n");
	printf("\t[-m core mask for I/O submission/completion.]\n");
	printf("\t\t(default: 1)]\n");
}

static void
print_stats(void)
{
	float io_per_second, mb_per_second;
	float total_io_per_second, total_mb_per_second;
	struct worker_thread *worker;

	total_io_per_second = 0;
	total_mb_per_second = 0;

	worker = g_workers;
	while (worker != NULL) {
		struct ns_entry *entry = worker->namespaces;
		while (entry != NULL) {
			io_per_second = (float)entry->io_completed /
					g_time_in_sec;
			mb_per_second = io_per_second * g_io_size_bytes /
					(1024 * 1024);
			printf("%-.20s: %10.2f IO/s %10.2f MB/s on lcore %u\n",
			       entry->name, io_per_second,
			       mb_per_second, worker->lcore);
			total_io_per_second += io_per_second;
			total_mb_per_second += mb_per_second;
			entry = entry->next;
		}
		worker = worker->next;
	}
	printf("=====================================================\n");
	printf("%-20s: %10.2f IO/s %10.2f MB/s\n",
	       "Total", total_io_per_second, total_mb_per_second);
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
	g_core_mask = NULL;

	while ((op = getopt(argc, argv, "m:q:s:t:w:M:")) != -1) {
		switch (op) {
		case 'm':
			g_core_mask = optarg;
			break;
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
	unsigned lcore;
	struct worker_thread *worker;
	struct worker_thread *prev_worker;

	worker = malloc(sizeof(struct worker_thread));
	memset(worker, 0, sizeof(struct worker_thread));
	worker->lcore = rte_get_master_lcore();

	g_workers = g_current_worker = worker;

	RTE_LCORE_FOREACH_SLAVE(lcore) {
		prev_worker = worker;
		worker = malloc(sizeof(struct worker_thread));
		memset(worker, 0, sizeof(struct worker_thread));
		worker->lcore = lcore;
		prev_worker->next = worker;
	}

	return 0;
}

static int
register_controllers(void)
{
	struct pci_device_iterator	*pci_dev_iter;
	struct pci_device		*pci_dev;
	struct pci_id_match		match;
	int				rc;

	printf("Initializing NVMe Controllers\n");

	pci_system_init();

	match.vendor_id =	PCI_MATCH_ANY;
	match.subvendor_id =	PCI_MATCH_ANY;
	match.subdevice_id =	PCI_MATCH_ANY;
	match.device_id =	PCI_MATCH_ANY;
	match.device_class =	NVME_CLASS_CODE;
	match.device_class_mask = 0xFFFFFF;

	pci_dev_iter = pci_id_match_iterator_create(&match);

	rc = 0;
	while ((pci_dev = pci_device_next(pci_dev_iter))) {
		struct nvme_controller *ctrlr;

		if (pci_device_has_non_null_driver(pci_dev)) {
			fprintf(stderr, "non-null kernel driver attached to nvme\n");
			fprintf(stderr, " controller at pci bdf %d:%d:%d\n",
				pci_dev->bus, pci_dev->dev, pci_dev->func);
			fprintf(stderr, " skipping...\n");
			continue;
		}

		pci_device_probe(pci_dev);

		ctrlr = nvme_attach(pci_dev);
		if (ctrlr == NULL) {
			fprintf(stderr, "nvme_attach failed for controller at pci bdf %d:%d:%d\n",
				pci_dev->bus, pci_dev->dev, pci_dev->func);
			rc = 1;
			continue;
		}

		register_ctrlr(ctrlr, pci_dev);
	}

	pci_iterator_destroy(pci_dev_iter);

	return rc;
}

static void
unregister_controllers(void)
{
	struct ctrlr_entry *entry = g_controllers;
	while (entry) {
		struct ctrlr_entry *next = entry->next;
		nvme_detach(entry->ctrlr);
		free(entry);
		entry = next;
	}
}

static const char *ealargs[] = {
	"perf",
	"-c 0x1", /* This must be the second parameter. It is overwritten by index in main(). */
	"-n 4",
};

int main(int argc, char **argv)
{
	int rc;
	char core_mask_arg[128];
	struct worker_thread *worker;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	if (g_core_mask != NULL) {
		snprintf(core_mask_arg, sizeof(core_mask_arg), "-c %s", g_core_mask);
		ealargs[1] = strdup(core_mask_arg);
	}

	rc = rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]),
			  (char **)(void *)(uintptr_t)ealargs);
	if (rc < 0) {
		fprintf(stderr, "could not initialize dpdk\n");
		return 1;
	}

	request_mempool = rte_mempool_create("nvme_request", 8192,
					     nvme_request_size(), 128, 0,
					     NULL, NULL, NULL, NULL,
					     SOCKET_ID_ANY, 0);

	if (request_mempool == NULL) {
		fprintf(stderr, "could not initialize request mempool\n");
		return 1;
	}

	task_pool = rte_mempool_create("task_pool", 2048,
				       sizeof(struct perf_task),
				       64, 0, NULL, NULL, task_ctor, NULL,
				       SOCKET_ID_ANY, 0);

	g_tsc_rate = rte_get_timer_hz();

	register_workers();
	register_controllers();

	/* Launch all of the slave workers */
	worker = g_workers->next;
	while (worker != NULL) {
		if (worker->namespaces != NULL) {
			rte_eal_remote_launch(work_fn, worker, worker->lcore);
		}
		worker = worker->next;
	}

	work_fn(g_workers);

	worker = g_workers->next;
	while (worker != NULL) {
		if (worker->namespaces != NULL) {
			if (rte_eal_wait_lcore(worker->lcore) < 0) {
				return -1;
			}
		}
		worker = worker->next;
	}

	print_stats();

	unregister_controllers();

	return 0;
}
