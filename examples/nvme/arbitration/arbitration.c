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

#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/nvme_intel.h"

struct ctrlr_entry {
	struct spdk_nvme_ctrlr			*ctrlr;
	struct spdk_nvme_intel_rw_latency_page	latency_page;
	TAILQ_ENTRY(ctrlr_entry)		link;
	char					name[1024];
};

struct ns_entry {
	struct {
		struct spdk_nvme_ctrlr		*ctrlr;
		struct spdk_nvme_ns		*ns;
	} nvme;

	TAILQ_ENTRY(ns_entry)			link;
	uint32_t				io_size_blocks;
	uint64_t				size_in_ios;
	char					name[1024];
};

struct ns_worker_ctx {
	struct ns_entry				*entry;
	uint64_t				io_completed;
	uint64_t				current_queue_depth;
	uint64_t				offset_in_ios;
	bool					is_draining;
	struct spdk_nvme_qpair			*qpair;
	TAILQ_ENTRY(ns_worker_ctx)		link;
};

struct arb_task {
	struct ns_worker_ctx			*ns_ctx;
	void					*buf;
};

struct worker_thread {
	TAILQ_HEAD(, ns_worker_ctx)		ns_ctx;
	TAILQ_ENTRY(worker_thread)		link;
	unsigned				lcore;
	enum spdk_nvme_qprio			qprio;
};

struct arb_context {
	int					shm_id;
	int					outstanding_commands;
	int					num_namespaces;
	int					num_workers;
	int					rw_percentage;
	int					is_random;
	int					queue_depth;
	int					time_in_sec;
	int					io_count;
	uint8_t					latency_tracking_enable;
	uint8_t					arbitration_mechanism;
	uint8_t					arbitration_config;
	uint32_t				io_size_bytes;
	uint32_t				max_completions;
	uint64_t				tsc_rate;
	const char				*core_mask;
	const char				*workload_type;
};

struct feature {
	uint32_t				result;
	bool					valid;
};

static struct spdk_mempool *task_pool		= NULL;

static TAILQ_HEAD(, ctrlr_entry) g_controllers	= TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(, ns_entry) g_namespaces	= TAILQ_HEAD_INITIALIZER(g_namespaces);
static TAILQ_HEAD(, worker_thread) g_workers	= TAILQ_HEAD_INITIALIZER(g_workers);

static struct feature features[SPDK_NVME_FEAT_ARBITRATION + 1] = {};

static struct arb_context g_arbitration = {
	.shm_id					= -1,
	.outstanding_commands			= 0,
	.num_workers				= 0,
	.num_namespaces				= 0,
	.rw_percentage				= 50,
	.queue_depth				= 64,
	.time_in_sec				= 60,
	.io_count				= 100000,
	.latency_tracking_enable		= 0,
	.arbitration_mechanism			= SPDK_NVME_CC_AMS_RR,
	.arbitration_config			= 0,
	.io_size_bytes				= 131072,
	.max_completions			= 0,
	/* Default 4 cores for urgent/high/medium/low */
	.core_mask				= "0xf",
	.workload_type				= "randrw",
};

/*
 * For weighted round robin arbitration mechanism, the smaller value between
 * weight and burst will be picked to execute the commands in one queue.
 */
#define USER_SPECIFIED_HIGH_PRIORITY_WEIGHT	32
#define USER_SPECIFIED_MEDIUM_PRIORITY_WEIGHT	16
#define USER_SPECIFIED_LOW_PRIORITY_WEIGHT	8

static void task_complete(struct arb_task *task);

static void io_complete(void *ctx, const struct spdk_nvme_cpl *completion);

static void get_arb_feature(struct spdk_nvme_ctrlr *ctrlr);

static int set_arb_feature(struct spdk_nvme_ctrlr *ctrlr);

static const char *print_qprio(enum spdk_nvme_qprio);


static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;
	const struct spdk_nvme_ctrlr_data *cdata;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (spdk_nvme_ns_get_size(ns) < g_arbitration.io_size_bytes ||
	    spdk_nvme_ns_get_extended_sector_size(ns) > g_arbitration.io_size_bytes ||
	    g_arbitration.io_size_bytes % spdk_nvme_ns_get_extended_sector_size(ns)) {
		printf("WARNING: controller %-20.20s (%-20.20s) ns %u has invalid "
		       "ns size %" PRIu64 " / block size %u for I/O size %u\n",
		       cdata->mn, cdata->sn, spdk_nvme_ns_get_id(ns),
		       spdk_nvme_ns_get_size(ns), spdk_nvme_ns_get_extended_sector_size(ns),
		       g_arbitration.io_size_bytes);
		return;
	}

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	entry->nvme.ctrlr = ctrlr;
	entry->nvme.ns = ns;

	entry->size_in_ios = spdk_nvme_ns_get_size(ns) / g_arbitration.io_size_bytes;
	entry->io_size_blocks = g_arbitration.io_size_bytes / spdk_nvme_ns_get_sector_size(ns);

	snprintf(entry->name, 44, "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	g_arbitration.num_namespaces++;
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);
}

static void
enable_latency_tracking_complete(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("enable_latency_tracking_complete failed\n");
	}
	g_arbitration.outstanding_commands--;
}

static void
set_latency_tracking_feature(struct spdk_nvme_ctrlr *ctrlr, bool enable)
{
	int res;
	union spdk_nvme_intel_feat_latency_tracking latency_tracking;

	if (enable) {
		latency_tracking.bits.enable = 0x01;
	} else {
		latency_tracking.bits.enable = 0x00;
	}

	res = spdk_nvme_ctrlr_cmd_set_feature(ctrlr, SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING,
					      latency_tracking.raw, 0, NULL, 0, enable_latency_tracking_complete, NULL);
	if (res) {
		printf("fail to allocate nvme request.\n");
		return;
	}
	g_arbitration.outstanding_commands++;

	while (g_arbitration.outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}
}

static void
register_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	uint32_t nsid;
	struct spdk_nvme_ns *ns;
	struct ctrlr_entry *entry = calloc(1, sizeof(struct ctrlr_entry));
	union spdk_nvme_cap_register cap = spdk_nvme_ctrlr_get_regs_cap(ctrlr);
	const struct spdk_nvme_ctrlr_data *cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	entry->ctrlr = ctrlr;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);

	if ((g_arbitration.latency_tracking_enable != 0) &&
	    spdk_nvme_ctrlr_is_feature_supported(ctrlr, SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING)) {
		set_latency_tracking_feature(ctrlr, true);
	}

	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns == NULL) {
			continue;
		}
		register_ns(ctrlr, ns);
	}

	if (g_arbitration.arbitration_mechanism == SPDK_NVME_CAP_AMS_WRR &&
	    (cap.bits.ams & SPDK_NVME_CAP_AMS_WRR)) {
		get_arb_feature(ctrlr);

		if (g_arbitration.arbitration_config != 0) {
			set_arb_feature(ctrlr);
			get_arb_feature(ctrlr);
		}
	}
}

static __thread unsigned int seed = 0;

static void
submit_single_io(struct ns_worker_ctx *ns_ctx)
{
	struct arb_task		*task = NULL;
	uint64_t		offset_in_ios;
	int			rc;
	struct ns_entry		*entry = ns_ctx->entry;

	task = spdk_mempool_get(task_pool);
	if (!task) {
		fprintf(stderr, "Failed to get task from task_pool\n");
		exit(1);
	}

	task->buf = spdk_dma_zmalloc(g_arbitration.io_size_bytes, 0x200, NULL);
	if (!task->buf) {
		spdk_mempool_put(task_pool, task);
		fprintf(stderr, "task->buf spdk_dma_zmalloc failed\n");
		exit(1);
	}

	task->ns_ctx = ns_ctx;

	if (g_arbitration.is_random) {
		offset_in_ios = rand_r(&seed) % entry->size_in_ios;
	} else {
		offset_in_ios = ns_ctx->offset_in_ios++;
		if (ns_ctx->offset_in_ios == entry->size_in_ios) {
			ns_ctx->offset_in_ios = 0;
		}
	}

	if ((g_arbitration.rw_percentage == 100) ||
	    (g_arbitration.rw_percentage != 0 &&
	     ((rand_r(&seed) % 100) < g_arbitration.rw_percentage))) {
		rc = spdk_nvme_ns_cmd_read(entry->nvme.ns, ns_ctx->qpair, task->buf,
					   offset_in_ios * entry->io_size_blocks,
					   entry->io_size_blocks, io_complete, task, 0);
	} else {
		rc = spdk_nvme_ns_cmd_write(entry->nvme.ns, ns_ctx->qpair, task->buf,
					    offset_in_ios * entry->io_size_blocks,
					    entry->io_size_blocks, io_complete, task, 0);
	}

	if (rc != 0) {
		fprintf(stderr, "starting I/O failed\n");
	} else {
		ns_ctx->current_queue_depth++;
	}
}

static void
task_complete(struct arb_task *task)
{
	struct ns_worker_ctx	*ns_ctx;

	ns_ctx = task->ns_ctx;
	ns_ctx->current_queue_depth--;
	ns_ctx->io_completed++;

	spdk_dma_free(task->buf);
	spdk_mempool_put(task_pool, task);

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
	task_complete((struct arb_task *)ctx);
}

static void
check_io(struct ns_worker_ctx *ns_ctx)
{
	spdk_nvme_qpair_process_completions(ns_ctx->qpair, g_arbitration.max_completions);
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
init_ns_worker_ctx(struct ns_worker_ctx *ns_ctx, enum spdk_nvme_qprio qprio)
{
	struct spdk_nvme_ctrlr *ctrlr = ns_ctx->entry->nvme.ctrlr;
	struct spdk_nvme_io_qpair_opts opts;

	spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
	opts.qprio = qprio;

	ns_ctx->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &opts, sizeof(opts));
	if (!ns_ctx->qpair) {
		printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair failed\n");
		return 1;
	}

	return 0;
}

static void
cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	spdk_nvme_ctrlr_free_io_qpair(ns_ctx->qpair);
}

static void
cleanup(uint32_t task_count)
{
	struct ns_entry *entry, *tmp_entry;
	struct worker_thread *worker, *tmp_worker;
	struct ns_worker_ctx *ns_ctx, *tmp_ns_ctx;

	TAILQ_FOREACH_SAFE(entry, &g_namespaces, link, tmp_entry) {
		TAILQ_REMOVE(&g_namespaces, entry, link);
		free(entry);
	};

	TAILQ_FOREACH_SAFE(worker, &g_workers, link, tmp_worker) {
		TAILQ_REMOVE(&g_workers, worker, link);

		/* ns_worker_ctx is a list in the worker */
		TAILQ_FOREACH_SAFE(ns_ctx, &worker->ns_ctx, link, tmp_ns_ctx) {
			TAILQ_REMOVE(&worker->ns_ctx, ns_ctx, link);
			free(ns_ctx);
		}

		free(worker);
	};

	if (spdk_mempool_count(task_pool) != (size_t)task_count) {
		fprintf(stderr, "task_pool count is %zu but should be %u\n",
			spdk_mempool_count(task_pool), task_count);
	}
	spdk_mempool_free(task_pool);
}

static int
work_fn(void *arg)
{
	uint64_t tsc_end;
	struct worker_thread *worker = (struct worker_thread *)arg;
	struct ns_worker_ctx *ns_ctx;

	printf("Starting thread on core %u with %s\n", worker->lcore, print_qprio(worker->qprio));

	/* Allocate a queue pair for each namespace. */
	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		if (init_ns_worker_ctx(ns_ctx, worker->qprio) != 0) {
			printf("ERROR: init_ns_worker_ctx() failed\n");
			return 1;
		}
	}

	tsc_end = spdk_get_ticks() + g_arbitration.time_in_sec * g_arbitration.tsc_rate;

	/* Submit initial I/O for each namespace. */
	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		submit_io(ns_ctx, g_arbitration.queue_depth);
	}

	while (1) {
		/*
		 * Check for completed I/O for each controller. A new
		 * I/O will be submitted in the io_complete callback
		 * to replace each I/O that is completed.
		 */
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			check_io(ns_ctx);
		}

		if (spdk_get_ticks() > tsc_end) {
			break;
		}
	}

	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		drain_io(ns_ctx);
		cleanup_ns_worker_ctx(ns_ctx);
	}

	return 0;
}

static void
usage(char *program_name)
{
	printf("%s options", program_name);
	printf("\n");
	printf("\t[-q io depth]\n");
	printf("\t[-s io size in bytes]\n");
	printf("\t[-w io pattern type, must be one of\n");
	printf("\t\t(read, write, randread, randwrite, rw, randrw)]\n");
	printf("\t[-M rwmixread (100 for reads, 0 for writes)]\n");
	printf("\t[-l enable latency tracking, default: disabled]\n");
	printf("\t\t(0 - disabled; 1 - enabled)\n");
	printf("\t[-t time in seconds]\n");
	printf("\t[-c core mask for I/O submission/completion.]\n");
	printf("\t\t(default: 0xf - 4 cores)]\n");
	printf("\t[-m max completions per poll]\n");
	printf("\t\t(default: 0 - unlimited)\n");
	printf("\t[-a arbitration mechanism, must be one of below]\n");
	printf("\t\t(0, 1, 2)]\n");
	printf("\t\t(0: default round robin mechanism)]\n");
	printf("\t\t(1: weighted round robin mechanism)]\n");
	printf("\t\t(2: vendor specific mechanism)]\n");
	printf("\t[-b enable arbitration user configuration, default: disabled]\n");
	printf("\t\t(0 - disabled; 1 - enabled)\n");
	printf("\t[-n subjected IOs for performance comparison]\n");
	printf("\t[-i shared memory group ID]\n");
}

static const char *
print_qprio(enum spdk_nvme_qprio qprio)
{
	switch (qprio) {
	case SPDK_NVME_QPRIO_URGENT:
		return "urgent priority queue";
	case SPDK_NVME_QPRIO_HIGH:
		return "high priority queue";
	case SPDK_NVME_QPRIO_MEDIUM:
		return "medium priority queue";
	case SPDK_NVME_QPRIO_LOW:
		return "low priority queue";
	default:
		return "invalid priority queue";
	}
}


static void
print_configuration(char *program_name)
{
	printf("%s run with configuration:\n", program_name);
	printf("%s -q %d -s %d -w %s -M %d -l %d -t %d -c %s -m %d -a %d -b %d -n %d -i %d\n",
	       program_name,
	       g_arbitration.queue_depth,
	       g_arbitration.io_size_bytes,
	       g_arbitration.workload_type,
	       g_arbitration.rw_percentage,
	       g_arbitration.latency_tracking_enable,
	       g_arbitration.time_in_sec,
	       g_arbitration.core_mask,
	       g_arbitration.max_completions,
	       g_arbitration.arbitration_mechanism,
	       g_arbitration.arbitration_config,
	       g_arbitration.io_count,
	       g_arbitration.shm_id);
}


static void
print_performance(void)
{
	float io_per_second, sent_all_io_in_secs;
	struct worker_thread	*worker;
	struct ns_worker_ctx	*ns_ctx;

	TAILQ_FOREACH(worker, &g_workers, link) {
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			io_per_second = (float)ns_ctx->io_completed / g_arbitration.time_in_sec;
			sent_all_io_in_secs = g_arbitration.io_count / io_per_second;
			printf("%-43.43s core %u: %8.2f IO/s %8.2f secs/%d ios\n",
			       ns_ctx->entry->name, worker->lcore,
			       io_per_second, sent_all_io_in_secs, g_arbitration.io_count);
		}
	}
	printf("========================================================\n");

	printf("\n");
}

static void
print_latency_page(struct ctrlr_entry *entry)
{
	int i;

	printf("\n");
	printf("%s\n", entry->name);
	printf("--------------------------------------------------------\n");

	for (i = 0; i < 32; i++) {
		if (entry->latency_page.buckets_32us[i])
			printf("Bucket %dus - %dus: %d\n", i * 32, (i + 1) * 32,
			       entry->latency_page.buckets_32us[i]);
	}
	for (i = 0; i < 31; i++) {
		if (entry->latency_page.buckets_1ms[i])
			printf("Bucket %dms - %dms: %d\n", i + 1, i + 2,
			       entry->latency_page.buckets_1ms[i]);
	}
	for (i = 0; i < 31; i++) {
		if (entry->latency_page.buckets_32ms[i])
			printf("Bucket %dms - %dms: %d\n", (i + 1) * 32, (i + 2) * 32,
			       entry->latency_page.buckets_32ms[i]);
	}
}

static void
print_latency_statistics(const char *op_name, enum spdk_nvme_intel_log_page log_page)
{
	struct ctrlr_entry	*ctrlr;

	printf("%s Latency Statistics:\n", op_name);
	printf("========================================================\n");
	TAILQ_FOREACH(ctrlr, &g_controllers, link) {
		if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr->ctrlr, log_page)) {
			if (spdk_nvme_ctrlr_cmd_get_log_page(
				    ctrlr->ctrlr, log_page,
				    SPDK_NVME_GLOBAL_NS_TAG,
				    &ctrlr->latency_page,
				    sizeof(struct spdk_nvme_intel_rw_latency_page),
				    0,
				    enable_latency_tracking_complete,
				    NULL)) {
				printf("nvme_ctrlr_cmd_get_log_page() failed\n");
				exit(1);
			}

			g_arbitration.outstanding_commands++;
		} else {
			printf("Controller %s: %s latency statistics not supported\n",
			       ctrlr->name, op_name);
		}
	}

	while (g_arbitration.outstanding_commands) {
		TAILQ_FOREACH(ctrlr, &g_controllers, link) {
			spdk_nvme_ctrlr_process_admin_completions(ctrlr->ctrlr);
		}
	}

	TAILQ_FOREACH(ctrlr, &g_controllers, link) {
		if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr->ctrlr, log_page)) {
			print_latency_page(ctrlr);
		}
	}
	printf("\n");
}

static void
print_stats(void)
{
	print_performance();
	if (g_arbitration.latency_tracking_enable) {
		if (g_arbitration.rw_percentage != 0) {
			print_latency_statistics("Read", SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY);
		}
		if (g_arbitration.rw_percentage != 100) {
			print_latency_statistics("Write", SPDK_NVME_INTEL_LOG_WRITE_CMD_LATENCY);
		}
	}
}

static int
parse_args(int argc, char **argv)
{
	const char *workload_type	= NULL;
	int op				= 0;
	bool mix_specified		= false;
	long int val;

	while ((op = getopt(argc, argv, "c:l:i:m:q:s:t:w:M:a:b:n:h")) != -1) {
		switch (op) {
		case 'c':
			g_arbitration.core_mask = optarg;
			break;
		case 'w':
			g_arbitration.workload_type = optarg;
			break;
		case 'h':
		case '?':
			usage(argv[0]);
			return 1;
		default:
			val = spdk_strtol(optarg, 10);
			if (val < 0) {
				fprintf(stderr, "Converting a string to integer failed\n");
				return val;
			}
			switch (op) {
			case 'i':
				g_arbitration.shm_id = val;
				break;
			case 'l':
				g_arbitration.latency_tracking_enable = val;
				break;
			case 'm':
				g_arbitration.max_completions = val;
				break;
			case 'q':
				g_arbitration.queue_depth = val;
				break;
			case 's':
				g_arbitration.io_size_bytes = val;
				break;
			case 't':
				g_arbitration.time_in_sec = val;
				break;
			case 'M':
				g_arbitration.rw_percentage = val;
				mix_specified = true;
				break;
			case 'a':
				g_arbitration.arbitration_mechanism = val;
				break;
			case 'b':
				g_arbitration.arbitration_config = val;
				break;
			case 'n':
				g_arbitration.io_count = val;
				break;
			default:
				usage(argv[0]);
				return -EINVAL;
			}
		}
	}

	workload_type = g_arbitration.workload_type;

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
		g_arbitration.rw_percentage = 100;
	}

	if (!strcmp(workload_type, "write") ||
	    !strcmp(workload_type, "randwrite")) {
		g_arbitration.rw_percentage = 0;
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
		if (g_arbitration.rw_percentage < 0 || g_arbitration.rw_percentage > 100) {
			fprintf(stderr,
				"-M must be specified to value from 0 to 100 "
				"for rw or randrw.\n");
			return 1;
		}
	}

	if (!strcmp(workload_type, "read") ||
	    !strcmp(workload_type, "write") ||
	    !strcmp(workload_type, "rw")) {
		g_arbitration.is_random = 0;
	} else {
		g_arbitration.is_random = 1;
	}

	if (g_arbitration.latency_tracking_enable != 0 &&
	    g_arbitration.latency_tracking_enable != 1) {
		fprintf(stderr,
			"-l must be specified to value 0 or 1.\n");
		return 1;
	}

	switch (g_arbitration.arbitration_mechanism) {
	case SPDK_NVME_CC_AMS_RR:
	case SPDK_NVME_CC_AMS_WRR:
	case SPDK_NVME_CC_AMS_VS:
		break;
	default:
		fprintf(stderr,
			"-a must be specified to value 0, 1, or 7.\n");
		return 1;
	}

	if (g_arbitration.arbitration_config != 0 &&
	    g_arbitration.arbitration_config != 1) {
		fprintf(stderr,
			"-b must be specified to value 0 or 1.\n");
		return 1;
	} else if (g_arbitration.arbitration_config == 1 &&
		   g_arbitration.arbitration_mechanism != SPDK_NVME_CC_AMS_WRR) {
		fprintf(stderr,
			"-a must be specified to 1 (WRR) together.\n");
		return 1;
	}

	return 0;
}

static int
register_workers(void)
{
	uint32_t i;
	struct worker_thread *worker;
	enum spdk_nvme_qprio qprio = SPDK_NVME_QPRIO_URGENT;

	SPDK_ENV_FOREACH_CORE(i) {
		worker = calloc(1, sizeof(*worker));
		if (worker == NULL) {
			fprintf(stderr, "Unable to allocate worker\n");
			return -1;
		}

		TAILQ_INIT(&worker->ns_ctx);
		worker->lcore = i;
		TAILQ_INSERT_TAIL(&g_workers, worker, link);
		g_arbitration.num_workers++;

		if (g_arbitration.arbitration_mechanism == SPDK_NVME_CAP_AMS_WRR) {
			qprio++;
		}

		worker->qprio = qprio & SPDK_NVME_CREATE_IO_SQ_QPRIO_MASK;
	}

	return 0;
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	/* Update with user specified arbitration configuration */
	opts->arb_mechanism = g_arbitration.arbitration_mechanism;

	printf("Attaching to %s\n", trid->traddr);

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attached to %s\n", trid->traddr);

	/* Update with actual arbitration configuration in use */
	g_arbitration.arbitration_mechanism = opts->arb_mechanism;

	register_ctrlr(ctrlr);
}

static int
register_controllers(void)
{
	printf("Initializing NVMe Controllers\n");

	if (spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}

	if (g_arbitration.num_namespaces == 0) {
		fprintf(stderr, "No valid namespaces to continue IO testing\n");
		return 1;
	}

	return 0;
}

static void
unregister_controllers(void)
{
	struct ctrlr_entry *entry, *tmp;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	TAILQ_FOREACH_SAFE(entry, &g_controllers, link, tmp) {
		TAILQ_REMOVE(&g_controllers, entry, link);
		if (g_arbitration.latency_tracking_enable &&
		    spdk_nvme_ctrlr_is_feature_supported(entry->ctrlr, SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING)) {
			set_latency_tracking_feature(entry->ctrlr, false);
		}
		spdk_nvme_detach_async(entry->ctrlr, &detach_ctx);
		free(entry);
	}

	while (detach_ctx && spdk_nvme_detach_poll_async(detach_ctx) == -EAGAIN) {
		;
	}
}

static int
associate_workers_with_ns(void)
{
	struct ns_entry		*entry = TAILQ_FIRST(&g_namespaces);
	struct worker_thread	*worker = TAILQ_FIRST(&g_workers);
	struct ns_worker_ctx	*ns_ctx;
	int			i, count;

	count = g_arbitration.num_namespaces > g_arbitration.num_workers ?
		g_arbitration.num_namespaces : g_arbitration.num_workers;

	for (i = 0; i < count; i++) {
		if (entry == NULL) {
			break;
		}

		ns_ctx = malloc(sizeof(struct ns_worker_ctx));
		if (!ns_ctx) {
			return 1;
		}
		memset(ns_ctx, 0, sizeof(*ns_ctx));

		printf("Associating %s with lcore %d\n", entry->name, worker->lcore);
		ns_ctx->entry = entry;
		TAILQ_INSERT_TAIL(&worker->ns_ctx, ns_ctx, link);

		worker = TAILQ_NEXT(worker, link);
		if (worker == NULL) {
			worker = TAILQ_FIRST(&g_workers);
		}

		entry = TAILQ_NEXT(entry, link);
		if (entry == NULL) {
			entry = TAILQ_FIRST(&g_namespaces);
		}

	}

	return 0;
}

static void
get_feature_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct feature *feature = cb_arg;
	int fid = feature - features;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("get_feature(0x%02X) failed\n", fid);
	} else {
		feature->result = cpl->cdw0;
		feature->valid = true;
	}

	g_arbitration.outstanding_commands--;
}

static int
get_feature(struct spdk_nvme_ctrlr *ctrlr, uint8_t fid)
{
	struct spdk_nvme_cmd cmd = {};
	struct feature *feature = &features[fid];

	feature->valid = false;

	cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd.cdw10_bits.get_features.fid = fid;

	return spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, get_feature_completion, feature);
}

static void
get_arb_feature(struct spdk_nvme_ctrlr *ctrlr)
{
	get_feature(ctrlr, SPDK_NVME_FEAT_ARBITRATION);

	g_arbitration.outstanding_commands++;

	while (g_arbitration.outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	if (features[SPDK_NVME_FEAT_ARBITRATION].valid) {
		union spdk_nvme_cmd_cdw11 arb;
		arb.feat_arbitration.raw = features[SPDK_NVME_FEAT_ARBITRATION].result;

		printf("Current Arbitration Configuration\n");
		printf("===========\n");
		printf("Arbitration Burst:           ");
		if (arb.feat_arbitration.bits.ab == SPDK_NVME_ARBITRATION_BURST_UNLIMITED) {
			printf("no limit\n");
		} else {
			printf("%u\n", 1u << arb.feat_arbitration.bits.ab);
		}

		printf("Low Priority Weight:         %u\n", arb.feat_arbitration.bits.lpw + 1);
		printf("Medium Priority Weight:      %u\n", arb.feat_arbitration.bits.mpw + 1);
		printf("High Priority Weight:        %u\n", arb.feat_arbitration.bits.hpw + 1);
		printf("\n");
	}
}

static void
set_feature_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct feature *feature = cb_arg;
	int fid = feature - features;

	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("set_feature(0x%02X) failed\n", fid);
		feature->valid = false;
	} else {
		printf("Set Arbitration Feature Successfully\n");
	}

	g_arbitration.outstanding_commands--;
}

static int
set_arb_feature(struct spdk_nvme_ctrlr *ctrlr)
{
	int ret;
	struct spdk_nvme_cmd cmd = {};

	cmd.opc = SPDK_NVME_OPC_SET_FEATURES;
	cmd.cdw10_bits.set_features.fid = SPDK_NVME_FEAT_ARBITRATION;

	g_arbitration.outstanding_commands = 0;

	if (features[SPDK_NVME_FEAT_ARBITRATION].valid) {
		cmd.cdw11_bits.feat_arbitration.bits.ab = SPDK_NVME_ARBITRATION_BURST_UNLIMITED;
		cmd.cdw11_bits.feat_arbitration.bits.lpw = USER_SPECIFIED_LOW_PRIORITY_WEIGHT;
		cmd.cdw11_bits.feat_arbitration.bits.mpw = USER_SPECIFIED_MEDIUM_PRIORITY_WEIGHT;
		cmd.cdw11_bits.feat_arbitration.bits.hpw = USER_SPECIFIED_HIGH_PRIORITY_WEIGHT;
	}

	ret = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0,
					    set_feature_completion, &features[SPDK_NVME_FEAT_ARBITRATION]);
	if (ret) {
		printf("Set Arbitration Feature: Failed 0x%x\n", ret);
		return 1;
	}

	g_arbitration.outstanding_commands++;

	while (g_arbitration.outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}

	if (!features[SPDK_NVME_FEAT_ARBITRATION].valid) {
		printf("Set Arbitration Feature failed and use default configuration\n");
	}

	return 0;
}

int
main(int argc, char **argv)
{
	int rc;
	struct worker_thread *worker, *master_worker;
	unsigned master_core;
	char task_pool_name[30];
	uint32_t task_count;
	struct spdk_env_opts opts;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	spdk_env_opts_init(&opts);
	opts.name = "arb";
	opts.core_mask = g_arbitration.core_mask;
	opts.shm_id = g_arbitration.shm_id;
	if (spdk_env_init(&opts) < 0) {
		return 1;
	}

	g_arbitration.tsc_rate = spdk_get_ticks_hz();

	if (register_workers() != 0) {
		return 1;
	}

	if (register_controllers() != 0) {
		return 1;
	}

	if (associate_workers_with_ns() != 0) {
		return 1;
	}

	snprintf(task_pool_name, sizeof(task_pool_name), "task_pool_%d", getpid());

	/*
	 * The task_count will be dynamically calculated based on the
	 * number of attached active namespaces, queue depth and number
	 * of cores (workers) involved in the IO perations.
	 */
	task_count = g_arbitration.num_namespaces > g_arbitration.num_workers ?
		     g_arbitration.num_namespaces : g_arbitration.num_workers;
	task_count *= g_arbitration.queue_depth;

	task_pool = spdk_mempool_create(task_pool_name, task_count,
					sizeof(struct arb_task), 0, SPDK_ENV_SOCKET_ID_ANY);
	if (task_pool == NULL) {
		fprintf(stderr, "could not initialize task pool\n");
		return 1;
	}

	print_configuration(argv[0]);

	printf("Initialization complete. Launching workers.\n");

	/* Launch all of the slave workers */
	master_core = spdk_env_get_current_core();
	master_worker = NULL;
	TAILQ_FOREACH(worker, &g_workers, link) {
		if (worker->lcore != master_core) {
			spdk_env_thread_launch_pinned(worker->lcore, work_fn, worker);
		} else {
			assert(master_worker == NULL);
			master_worker = worker;
		}
	}

	assert(master_worker != NULL);
	rc = work_fn(master_worker);

	spdk_env_thread_wait_all();

	print_stats();

	unregister_controllers();

	cleanup(task_count);

	if (rc != 0) {
		fprintf(stderr, "%s: errors occured\n", argv[0]);
	}

	return rc;
}
