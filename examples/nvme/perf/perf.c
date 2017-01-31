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
#include <rte_mempool.h>
#include <rte_lcore.h>

#include "spdk/fd.h"
#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/nvme_intel.h"

#if HAVE_LIBAIO
#include <libaio.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

struct ctrlr_entry {
	struct spdk_nvme_ctrlr			*ctrlr;
	struct spdk_nvme_intel_rw_latency_page	*latency_page;
	struct ctrlr_entry			*next;
	char					name[1024];
};

enum entry_type {
	ENTRY_TYPE_NVME_NS,
	ENTRY_TYPE_AIO_FILE,
};

struct ns_entry {
	enum entry_type		type;

	union {
		struct {
			struct spdk_nvme_ctrlr	*ctrlr;
			struct spdk_nvme_ns	*ns;
		} nvme;
#if HAVE_LIBAIO
		struct {
			int			fd;
		} aio;
#endif
	} u;

	struct ns_entry		*next;
	uint32_t		io_size_blocks;
	uint64_t		size_in_ios;
	char			name[1024];
};

struct ns_worker_ctx {
	struct ns_entry		*entry;
	uint64_t		io_completed;
	uint64_t		total_tsc;
	uint64_t		min_tsc;
	uint64_t		max_tsc;
	uint64_t		current_queue_depth;
	uint64_t		offset_in_ios;
	bool			is_draining;

	union {
		struct {
			struct spdk_nvme_qpair	*qpair;
		} nvme;

#if HAVE_LIBAIO
		struct {
			struct io_event		*events;
			io_context_t		ctx;
		} aio;
#endif
	} u;

	struct ns_worker_ctx	*next;
};

struct perf_task {
	struct ns_worker_ctx	*ns_ctx;
	void			*buf;
	uint64_t		submit_tsc;
#if HAVE_LIBAIO
	struct iocb		iocb;
#endif
};

struct worker_thread {
	struct ns_worker_ctx 	*ns_ctx;
	struct worker_thread	*next;
	unsigned		lcore;
};

static int g_outstanding_commands;

static bool g_latency_tracking_enable = false;

static struct rte_mempool *task_pool;

static struct ctrlr_entry *g_controllers = NULL;
static struct ns_entry *g_namespaces = NULL;
static int g_num_namespaces = 0;
static struct worker_thread *g_workers = NULL;
static int g_num_workers = 0;

static uint64_t g_tsc_rate;

static uint32_t g_io_size_bytes;
static int g_rw_percentage;
static int g_is_random;
static int g_queue_depth;
static int g_time_in_sec;
static uint32_t g_max_completions;
static int g_dpdk_mem;

static const char *g_core_mask;

struct trid_entry {
	struct spdk_nvme_transport_id	trid;
	TAILQ_ENTRY(trid_entry)		tailq;
};

static TAILQ_HEAD(, trid_entry) g_trid_list = TAILQ_HEAD_INITIALIZER(g_trid_list);

static int g_aio_optind; /* Index of first AIO filename in argv */

static void
task_complete(struct perf_task *task);

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;
	const struct spdk_nvme_ctrlr_data *cdata;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (!spdk_nvme_ns_is_active(ns)) {
		printf("Controller %-20.20s (%-20.20s): Skipping inactive NS %u\n",
		       cdata->mn, cdata->sn,
		       spdk_nvme_ns_get_id(ns));
		return;
	}

	if (spdk_nvme_ns_get_size(ns) < g_io_size_bytes ||
	    spdk_nvme_ns_get_sector_size(ns) > g_io_size_bytes) {
		printf("WARNING: controller %-20.20s (%-20.20s) ns %u has invalid "
		       "ns size %" PRIu64 " / block size %u for I/O size %u\n",
		       cdata->mn, cdata->sn, spdk_nvme_ns_get_id(ns),
		       spdk_nvme_ns_get_size(ns), spdk_nvme_ns_get_sector_size(ns), g_io_size_bytes);
		return;
	}

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	entry->type = ENTRY_TYPE_NVME_NS;
	entry->u.nvme.ctrlr = ctrlr;
	entry->u.nvme.ns = ns;

	entry->size_in_ios = spdk_nvme_ns_get_size(ns) /
			     g_io_size_bytes;
	entry->io_size_blocks = g_io_size_bytes / spdk_nvme_ns_get_sector_size(ns);

	snprintf(entry->name, 44, "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	g_num_namespaces++;
	entry->next = g_namespaces;
	g_namespaces = entry;
}

static void
unregister_namespaces(void)
{
	struct ns_entry *entry = g_namespaces;

	while (entry) {
		struct ns_entry *next = entry->next;
		free(entry);
		entry = next;
	}
}

static void
enable_latency_tracking_complete(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		printf("enable_latency_tracking_complete failed\n");
	}
	g_outstanding_commands--;
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
	g_outstanding_commands++;

	while (g_outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}
}

static void
register_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	int nsid, num_ns;
	struct ctrlr_entry *entry = malloc(sizeof(struct ctrlr_entry));
	const struct spdk_nvme_ctrlr_data *cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	entry->latency_page = spdk_zmalloc(sizeof(struct spdk_nvme_intel_rw_latency_page),
					   4096, NULL);
	if (entry->latency_page == NULL) {
		printf("Allocation error (latency page)\n");
		exit(1);
	}

	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	entry->ctrlr = ctrlr;
	entry->next = g_controllers;
	g_controllers = entry;

	if (g_latency_tracking_enable &&
	    spdk_nvme_ctrlr_is_feature_supported(ctrlr, SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING))
		set_latency_tracking_feature(ctrlr, true);

	num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
	for (nsid = 1; nsid <= num_ns; nsid++) {
		register_ns(ctrlr, spdk_nvme_ctrlr_get_ns(ctrlr, nsid));
	}

}

#if HAVE_LIBAIO
static int
register_aio_file(const char *path)
{
	struct ns_entry *entry;

	int flags, fd;
	uint64_t size;
	uint32_t blklen;

	if (g_rw_percentage == 100) {
		flags = O_RDONLY;
	} else if (g_rw_percentage == 0) {
		flags = O_WRONLY;
	} else {
		flags = O_RDWR;
	}

	flags |= O_DIRECT;

	fd = open(path, flags);
	if (fd < 0) {
		fprintf(stderr, "Could not open AIO device %s: %s\n", path, strerror(errno));
		return -1;
	}

	size = spdk_fd_get_size(fd);
	if (size == 0) {
		fprintf(stderr, "Could not determine size of AIO device %s\n", path);
		close(fd);
		return -1;
	}

	blklen = spdk_fd_get_blocklen(fd);
	if (blklen == 0) {
		fprintf(stderr, "Could not determine block size of AIO device %s\n", path);
		close(fd);
		return -1;
	}

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		close(fd);
		perror("aio ns_entry malloc");
		return -1;
	}

	entry->type = ENTRY_TYPE_AIO_FILE;
	entry->u.aio.fd = fd;
	entry->size_in_ios = size / g_io_size_bytes;
	entry->io_size_blocks = g_io_size_bytes / blklen;

	snprintf(entry->name, sizeof(entry->name), "%s", path);

	g_num_namespaces++;
	entry->next = g_namespaces;
	g_namespaces = entry;

	return 0;
}

static int
aio_submit(io_context_t aio_ctx, struct iocb *iocb, int fd, enum io_iocb_cmd cmd, void *buf,
	   unsigned long nbytes, uint64_t offset, void *cb_ctx)
{
	iocb->aio_fildes = fd;
	iocb->aio_reqprio = 0;
	iocb->aio_lio_opcode = cmd;
	iocb->u.c.buf = buf;
	iocb->u.c.nbytes = nbytes;
	iocb->u.c.offset = offset;
	iocb->data = cb_ctx;

	if (io_submit(aio_ctx, 1, &iocb) < 0) {
		printf("io_submit");
		return -1;
	}

	return 0;
}

static void
aio_check_io(struct ns_worker_ctx *ns_ctx)
{
	int count, i;
	struct timespec timeout;

	timeout.tv_sec = 0;
	timeout.tv_nsec = 0;

	count = io_getevents(ns_ctx->u.aio.ctx, 1, g_queue_depth, ns_ctx->u.aio.events, &timeout);
	if (count < 0) {
		fprintf(stderr, "io_getevents error\n");
		exit(1);
	}

	for (i = 0; i < count; i++) {
		task_complete(ns_ctx->u.aio.events[i].data);
	}
}
#endif /* HAVE_LIBAIO */

static void task_ctor(struct rte_mempool *mp, void *arg, void *__task, unsigned id)
{
	struct perf_task *task = __task;
	task->buf = spdk_zmalloc(g_io_size_bytes, 0x200, NULL);
	if (task->buf == NULL) {
		fprintf(stderr, "task->buf spdk_zmalloc failed\n");
		exit(1);
	}
	memset(task->buf, id % 8, g_io_size_bytes);
}

static void io_complete(void *ctx, const struct spdk_nvme_cpl *completion);

static __thread unsigned int seed = 0;

static void
submit_single_io(struct ns_worker_ctx *ns_ctx)
{
	struct perf_task	*task = NULL;
	uint64_t		offset_in_ios;
	int			rc;
	struct ns_entry		*entry = ns_ctx->entry;

	if (rte_mempool_get(task_pool, (void **)&task) != 0) {
		fprintf(stderr, "task_pool rte_mempool_get failed\n");
		exit(1);
	}

	task->ns_ctx = ns_ctx;

	if (g_is_random) {
		offset_in_ios = rand_r(&seed) % entry->size_in_ios;
	} else {
		offset_in_ios = ns_ctx->offset_in_ios++;
		if (ns_ctx->offset_in_ios == entry->size_in_ios) {
			ns_ctx->offset_in_ios = 0;
		}
	}

	task->submit_tsc = spdk_get_ticks();

	if ((g_rw_percentage == 100) ||
	    (g_rw_percentage != 0 && ((rand_r(&seed) % 100) < g_rw_percentage))) {
#if HAVE_LIBAIO
		if (entry->type == ENTRY_TYPE_AIO_FILE) {
			rc = aio_submit(ns_ctx->u.aio.ctx, &task->iocb, entry->u.aio.fd, IO_CMD_PREAD, task->buf,
					g_io_size_bytes, offset_in_ios * g_io_size_bytes, task);
		} else
#endif
		{
			rc = spdk_nvme_ns_cmd_read(entry->u.nvme.ns, ns_ctx->u.nvme.qpair, task->buf,
						   offset_in_ios * entry->io_size_blocks,
						   entry->io_size_blocks, io_complete, task, 0);
		}
	} else {
#if HAVE_LIBAIO
		if (entry->type == ENTRY_TYPE_AIO_FILE) {
			rc = aio_submit(ns_ctx->u.aio.ctx, &task->iocb, entry->u.aio.fd, IO_CMD_PWRITE, task->buf,
					g_io_size_bytes, offset_in_ios * g_io_size_bytes, task);
		} else
#endif
		{
			rc = spdk_nvme_ns_cmd_write(entry->u.nvme.ns, ns_ctx->u.nvme.qpair, task->buf,
						    offset_in_ios * entry->io_size_blocks,
						    entry->io_size_blocks, io_complete, task, 0);
		}
	}

	if (rc != 0) {
		fprintf(stderr, "starting I/O failed\n");
	}

	ns_ctx->current_queue_depth++;
}

static void
task_complete(struct perf_task *task)
{
	struct ns_worker_ctx	*ns_ctx;
	uint64_t		tsc_diff;

	ns_ctx = task->ns_ctx;
	ns_ctx->current_queue_depth--;
	ns_ctx->io_completed++;
	tsc_diff = spdk_get_ticks() - task->submit_tsc;
	ns_ctx->total_tsc += tsc_diff;
	if (ns_ctx->min_tsc > tsc_diff) {
		ns_ctx->min_tsc = tsc_diff;
	}
	if (ns_ctx->max_tsc < tsc_diff) {
		ns_ctx->max_tsc = tsc_diff;
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
	task_complete((struct perf_task *)ctx);
}

static void
check_io(struct ns_worker_ctx *ns_ctx)
{
#if HAVE_LIBAIO
	if (ns_ctx->entry->type == ENTRY_TYPE_AIO_FILE) {
		aio_check_io(ns_ctx);
	} else
#endif
	{
		spdk_nvme_qpair_process_completions(ns_ctx->u.nvme.qpair, g_max_completions);
	}
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
init_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	if (ns_ctx->entry->type == ENTRY_TYPE_AIO_FILE) {
#ifdef HAVE_LIBAIO
		ns_ctx->u.aio.events = calloc(g_queue_depth, sizeof(struct io_event));
		if (!ns_ctx->u.aio.events) {
			return -1;
		}
		ns_ctx->u.aio.ctx = 0;
		if (io_setup(g_queue_depth, &ns_ctx->u.aio.ctx) < 0) {
			free(ns_ctx->u.aio.events);
			perror("io_setup");
			return -1;
		}
#endif
	} else {
		/*
		 * TODO: If a controller has multiple namespaces, they could all use the same queue.
		 *  For now, give each namespace/thread combination its own queue.
		 */
		ns_ctx->u.nvme.qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_ctx->entry->u.nvme.ctrlr, 0);
		if (!ns_ctx->u.nvme.qpair) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair failed\n");
			return -1;
		}
	}

	return 0;
}

static void
cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	if (ns_ctx->entry->type == ENTRY_TYPE_AIO_FILE) {
#ifdef HAVE_LIBAIO
		io_destroy(ns_ctx->u.aio.ctx);
		free(ns_ctx->u.aio.events);
#endif
	} else {
		spdk_nvme_ctrlr_free_io_qpair(ns_ctx->u.nvme.qpair);
	}
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
		if (init_ns_worker_ctx(ns_ctx) != 0) {
			printf("ERROR: init_ns_worker_ctx() failed\n");
			return 1;
		}
		ns_ctx = ns_ctx->next;
	}

	tsc_end = spdk_get_ticks() + g_time_in_sec * g_tsc_rate;

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

		if (spdk_get_ticks() > tsc_end) {
			break;
		}
	}

	ns_ctx = worker->ns_ctx;
	while (ns_ctx != NULL) {
		drain_io(ns_ctx);
		cleanup_ns_worker_ctx(ns_ctx);
		ns_ctx = ns_ctx->next;
	}

	return 0;
}

static void usage(char *program_name)
{
	printf("%s options", program_name);
#if HAVE_LIBAIO
	printf(" [AIO device(s)]...");
#endif
	printf("\n");
	printf("\t[-q io depth]\n");
	printf("\t[-s io size in bytes]\n");
	printf("\t[-w io pattern type, must be one of\n");
	printf("\t\t(read, write, randread, randwrite, rw, randrw)]\n");
	printf("\t[-M rwmixread (100 for reads, 0 for writes)]\n");
	printf("\t[-l enable latency tracking, default: disabled]\n");
	printf("\t[-t time in seconds]\n");
	printf("\t[-c core mask for I/O submission/completion.]\n");
	printf("\t\t(default: 1)]\n");
	printf("\t[-r discover info of remote NVMe over Fabrics target]\n");
	printf("\t Format: 'key:value [key:value] ...'\n");
	printf("\t Keys:\n");
	printf("\t  trtype      Transport type (e.g. RDMA)\n");
	printf("\t  adrfam      Address family (e.g. IPv4, IPv6)\n");
	printf("\t  traddr      Transport address (e.g. 192.168.100.8)\n");
	printf("\t  trsvcid     Transport service identifier (e.g. 4420)\n");
	printf("\t  subnqn      Subsystem NQN (default: %s)\n", SPDK_NVMF_DISCOVERY_NQN);
	printf("\t Example: -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.100.8 trsvcid:4420'\n");
	printf("\t[-d DPDK huge memory size in MB.]\n");
	printf("\t[-m max completions per poll]\n");
	printf("\t\t(default: 0 - unlimited)\n");
}

static void
print_performance(void)
{
	uint64_t total_io_completed;
	float io_per_second, mb_per_second, average_latency, min_latency, max_latency;
	float total_io_per_second, total_mb_per_second;
	float sum_ave_latency, sum_min_latency, sum_max_latency;
	int ns_count;
	struct worker_thread	*worker;
	struct ns_worker_ctx	*ns_ctx;

	total_io_per_second = 0;
	total_mb_per_second = 0;
	total_io_completed = 0;
	sum_ave_latency = 0;
	sum_min_latency = 0;
	sum_max_latency = 0;
	ns_count = 0;

	printf("========================================================\n");
	printf("%103s\n", "Latency(us)");
	printf("%-55s: %10s %10s %10s %10s %10s\n",
	       "Device Information", "IOPS", "MB/s", "Average", "min", "max");

	worker = g_workers;
	while (worker) {
		ns_ctx = worker->ns_ctx;
		while (ns_ctx) {
			io_per_second = (float)ns_ctx->io_completed / g_time_in_sec;
			mb_per_second = io_per_second * g_io_size_bytes / (1024 * 1024);
			average_latency = (float)(ns_ctx->total_tsc / ns_ctx->io_completed) * 1000 * 1000 / g_tsc_rate;
			min_latency = (float)ns_ctx->min_tsc * 1000 * 1000 / g_tsc_rate;
			max_latency = (float)ns_ctx->max_tsc * 1000 * 1000 / g_tsc_rate;
			printf("%-43.43s from core %u: %10.2f %10.2f %10.2f %10.2f %10.2f\n",
			       ns_ctx->entry->name, worker->lcore,
			       io_per_second, mb_per_second,
			       average_latency, min_latency, max_latency);
			total_io_per_second += io_per_second;
			total_mb_per_second += mb_per_second;
			total_io_completed += ns_ctx->io_completed;
			sum_ave_latency += average_latency;
			sum_min_latency += min_latency;
			sum_max_latency += max_latency;
			ns_count++;
			ns_ctx = ns_ctx->next;
		}
		worker = worker->next;
	}

	assert(ns_count != 0);
	printf("========================================================\n");
	printf("%-55s: %10.2f %10.2f %10.2f %10.2f %10.2f\n",
	       "Total", total_io_per_second, total_mb_per_second,
	       sum_ave_latency / ns_count, sum_min_latency / ns_count,
	       sum_max_latency / ns_count);
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
		if (entry->latency_page->buckets_32us[i])
			printf("Bucket %dus - %dus: %d\n", i * 32, (i + 1) * 32, entry->latency_page->buckets_32us[i]);
	}
	for (i = 0; i < 31; i++) {
		if (entry->latency_page->buckets_1ms[i])
			printf("Bucket %dms - %dms: %d\n", i + 1, i + 2, entry->latency_page->buckets_1ms[i]);
	}
	for (i = 0; i < 31; i++) {
		if (entry->latency_page->buckets_32ms[i])
			printf("Bucket %dms - %dms: %d\n", (i + 1) * 32, (i + 2) * 32,
			       entry->latency_page->buckets_32ms[i]);
	}
}

static void
print_latency_statistics(const char *op_name, enum spdk_nvme_intel_log_page log_page)
{
	struct ctrlr_entry	*ctrlr;

	printf("%s Latency Statistics:\n", op_name);
	printf("========================================================\n");
	ctrlr = g_controllers;
	while (ctrlr) {
		if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr->ctrlr, log_page)) {
			if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr->ctrlr, log_page, SPDK_NVME_GLOBAL_NS_TAG,
							     ctrlr->latency_page, sizeof(struct spdk_nvme_intel_rw_latency_page), 0,
							     enable_latency_tracking_complete,
							     NULL)) {
				printf("nvme_ctrlr_cmd_get_log_page() failed\n");
				exit(1);
			}

			g_outstanding_commands++;
		} else {
			printf("Controller %s: %s latency statistics not supported\n", ctrlr->name, op_name);
		}
		ctrlr = ctrlr->next;
	}

	while (g_outstanding_commands) {
		ctrlr = g_controllers;
		while (ctrlr) {
			spdk_nvme_ctrlr_process_admin_completions(ctrlr->ctrlr);
			ctrlr = ctrlr->next;
		}
	}

	ctrlr = g_controllers;
	while (ctrlr) {
		if (spdk_nvme_ctrlr_is_log_page_supported(ctrlr->ctrlr, log_page)) {
			print_latency_page(ctrlr);
		}
		ctrlr = ctrlr->next;
	}
	printf("\n");
}

static void
print_stats(void)
{
	print_performance();
	if (g_latency_tracking_enable) {
		if (g_rw_percentage != 0) {
			print_latency_statistics("Read", SPDK_NVME_INTEL_LOG_READ_CMD_LATENCY);
		}
		if (g_rw_percentage != 100) {
			print_latency_statistics("Write", SPDK_NVME_INTEL_LOG_WRITE_CMD_LATENCY);
		}
	}
}

static void
unregister_trids(void)
{
	struct trid_entry *trid_entry, *tmp;

	TAILQ_FOREACH_SAFE(trid_entry, &g_trid_list, tailq, tmp) {
		free(trid_entry);
	}
}

static int
add_trid(const char *trid_str)
{
	struct trid_entry *trid_entry;
	struct spdk_nvme_transport_id *trid;

	trid_entry = calloc(1, sizeof(*trid_entry));
	if (trid_entry == NULL) {
		return -1;
	}

	trid = &trid_entry->trid;
	memset(trid, 0, sizeof(*trid));
	trid->trtype = SPDK_NVME_TRANSPORT_PCIE;
	snprintf(trid->subnqn, sizeof(trid->subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	if (spdk_nvme_transport_id_parse(trid, trid_str) != 0) {
		fprintf(stderr, "Invalid transport ID format '%s'\n", trid_str);
		free(trid_entry);
		return 1;
	}

	TAILQ_INSERT_TAIL(&g_trid_list, trid_entry, tailq);
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
	g_core_mask = NULL;
	g_max_completions = 0;

	while ((op = getopt(argc, argv, "c:d:lm:q:r:s:t:w:M:")) != -1) {
		switch (op) {
		case 'c':
			g_core_mask = optarg;
			break;
		case 'd':
			g_dpdk_mem = atoi(optarg);
			break;
		case 'l':
			g_latency_tracking_enable = true;
			break;
		case 'm':
			g_max_completions = atoi(optarg);
			break;
		case 'q':
			g_queue_depth = atoi(optarg);
			break;
		case 'r':
			if (add_trid(optarg)) {
				usage(argv[0]);
				return 1;
			}
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

	if (TAILQ_EMPTY(&g_trid_list)) {
		/* If no transport IDs specified, default to enumerating all local PCIe devices */
		add_trid("trtype:pcie");
	}

	g_aio_optind = optind;
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
	if (worker == NULL) {
		perror("worker_thread malloc");
		return -1;
	}

	memset(worker, 0, sizeof(struct worker_thread));
	worker->lcore = rte_get_master_lcore();

	g_workers = worker;
	g_num_workers = 1;

	RTE_LCORE_FOREACH_SLAVE(lcore) {
		prev_worker = worker;
		worker = malloc(sizeof(struct worker_thread));
		if (worker == NULL) {
			perror("worker_thread malloc");
			return -1;
		}

		memset(worker, 0, sizeof(struct worker_thread));
		worker->lcore = lcore;
		prev_worker->next = worker;
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
			free(ns_ctx);
			ns_ctx = next_ns_ctx;
		}

		free(worker);
		worker = next_worker;
	}
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_pci_addr	pci_addr;
	struct spdk_pci_device	*pci_dev;
	struct spdk_pci_id	pci_id;

	if (trid->trtype != SPDK_NVME_TRANSPORT_PCIE) {
		printf("Attaching to NVMe over Fabrics controller at %s:%s: %s\n",
		       trid->traddr, trid->trsvcid,
		       trid->subnqn);
	} else {
		if (spdk_pci_addr_parse(&pci_addr, trid->traddr)) {
			return false;
		}

		pci_dev = spdk_pci_get_device(&pci_addr);
		if (!pci_dev) {
			return false;
		}

		pci_id = spdk_pci_device_get_id(pci_dev);

		printf("Attaching to NVMe Controller at %s [%04x:%04x]\n",
		       trid->traddr,
		       pci_id.vendor_id, pci_id.device_id);
	}

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_pci_addr	pci_addr;
	struct spdk_pci_device	*pci_dev;
	struct spdk_pci_id	pci_id;

	if (trid->trtype != SPDK_NVME_TRANSPORT_PCIE) {
		printf("Attached to NVMe over Fabrics controller at %s:%s: %s\n",
		       trid->traddr, trid->trsvcid,
		       trid->subnqn);
	} else {
		if (spdk_pci_addr_parse(&pci_addr, trid->traddr)) {
			return;
		}

		pci_dev = spdk_pci_get_device(&pci_addr);
		if (!pci_dev) {
			return;
		}

		pci_id = spdk_pci_device_get_id(pci_dev);

		printf("Attached to NVMe Controller at %s [%04x:%04x]\n",
		       trid->traddr,
		       pci_id.vendor_id, pci_id.device_id);
	}

	register_ctrlr(ctrlr);
}

static int
register_controllers(void)
{
	struct trid_entry *trid_entry;

	printf("Initializing NVMe Controllers\n");

	TAILQ_FOREACH(trid_entry, &g_trid_list, tailq) {
		if (spdk_nvme_probe(&trid_entry->trid, NULL, probe_cb, attach_cb, NULL) != 0) {
			fprintf(stderr, "spdk_nvme_probe() failed for transport address '%s'\n",
				trid_entry->trid.traddr);
			return -1;
		}
	}

	return 0;
}

static void
unregister_controllers(void)
{
	struct ctrlr_entry *entry = g_controllers;

	while (entry) {
		struct ctrlr_entry *next = entry->next;
		spdk_free(entry->latency_page);
		if (g_latency_tracking_enable &&
		    spdk_nvme_ctrlr_is_feature_supported(entry->ctrlr, SPDK_NVME_INTEL_FEAT_LATENCY_TRACKING))
			set_latency_tracking_feature(entry->ctrlr, false);
		spdk_nvme_detach(entry->ctrlr);
		free(entry);
		entry = next;
	}
}

static int
register_aio_files(int argc, char **argv)
{
#if HAVE_LIBAIO
	int i;

	/* Treat everything after the options as files for AIO */
	for (i = g_aio_optind; i < argc; i++) {
		if (register_aio_file(argv[i]) != 0) {
			return 1;
		}
	}
#endif /* HAVE_LIBAIO */

	return 0;
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

static char *ealargs[] = {
	"perf",
	"-c 0x1", /* This must be the second parameter. It is overwritten by index in main(). */
	"-n 4",
	"-m 512",  /* This can be overwritten by index in main(). */
	"--proc-type=auto",
#ifdef __linux__
	"--base-virtaddr=0x1000000000",
#endif
};

int main(int argc, char **argv)
{
	int rc;
	struct worker_thread *worker;
	char task_pool_name[30];
	uint32_t task_count;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	ealargs[1] = spdk_sprintf_alloc("-c %s", g_core_mask ? g_core_mask : "0x1");
	if (ealargs[1] == NULL) {
		perror("ealargs spdk_sprintf_alloc");
		return 1;
	}

	ealargs[3] = spdk_sprintf_alloc("-m %d", g_dpdk_mem ? g_dpdk_mem : 512);
	if (ealargs[3] == NULL) {
		free(ealargs[1]);
		perror("ealargs spdk_sprintf_alloc");
		return 1;
	}

	rc = rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]), ealargs);

	free(ealargs[1]);
	free(ealargs[3]);

	if (rc < 0) {
		fprintf(stderr, "could not initialize dpdk\n");
		return 1;
	}

	g_tsc_rate = spdk_get_ticks_hz();

	if (register_workers() != 0) {
		rc = -1;
		goto cleanup;
	}

	if (register_aio_files(argc, argv) != 0) {
		rc = -1;
		goto cleanup;
	}

	if (register_controllers() != 0) {
		rc = -1;
		goto cleanup;
	}

	if (associate_workers_with_ns() != 0) {
		rc = -1;
		goto cleanup;
	}

	snprintf(task_pool_name, sizeof(task_pool_name), "task_pool_%d", getpid());

	/*
	 * The task_count will be dynamically calculated based on the
	 * number of attached active namespaces(aio files), queue depth
	 * and number of cores (workers) involved in the IO operations.
	 */
	task_count = g_num_namespaces > g_num_workers ? g_num_namespaces : g_num_workers;
	task_count *= g_queue_depth;

	task_pool = rte_mempool_create(task_pool_name, task_count,
				       sizeof(struct perf_task),
				       0, 0, NULL, NULL, task_ctor, NULL,
				       SOCKET_ID_ANY, 0);
	if (task_pool == NULL) {
		fprintf(stderr, "could not initialize task pool\n");
		rc = -1;
		goto cleanup;
	}

	printf("Initialization complete. Launching workers.\n");

	/* Launch all of the slave workers */
	worker = g_workers->next;
	while (worker != NULL) {
		rte_eal_remote_launch(work_fn, worker, worker->lcore);
		worker = worker->next;
	}

	rc = work_fn(g_workers);

	worker = g_workers->next;
	while (worker != NULL) {
		if (rte_eal_wait_lcore(worker->lcore) < 0) {
			rc = -1;
		}
		worker = worker->next;
	}

	print_stats();

cleanup:
	unregister_trids();
	unregister_namespaces();
	unregister_controllers();
	unregister_workers();

	if (rc != 0) {
		fprintf(stderr, "%s: errors occured\n", argv[0]);
	}

	return rc;
}
