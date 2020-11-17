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
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/likely.h"

struct ctrlr_entry {
	struct spdk_nvme_ctrlr		*ctrlr;
	enum spdk_nvme_transport_type	trtype;

	TAILQ_ENTRY(ctrlr_entry)	link;
	char				name[1024];
};

struct ns_entry {
	struct spdk_nvme_ctrlr		*ctrlr;
	struct spdk_nvme_ns		*ns;

	TAILQ_ENTRY(ns_entry)		link;
	uint32_t			io_size_blocks;
	uint32_t			num_io_requests;
	uint64_t			size_in_ios;
	uint32_t			block_size;
	char				name[1024];
};

struct ctrlr_worker_ctx {
	pthread_mutex_t			mutex;
	struct ctrlr_entry		*entry;
	uint64_t			abort_submitted;
	uint64_t			abort_submit_failed;
	uint64_t			successful_abort;
	uint64_t			unsuccessful_abort;
	uint64_t			abort_failed;
	uint64_t			current_queue_depth;
	struct spdk_nvme_ctrlr		*ctrlr;
	TAILQ_ENTRY(ctrlr_worker_ctx)	link;
};

struct ns_worker_ctx {
	struct ns_entry			*entry;
	uint64_t			io_submitted;
	uint64_t			io_completed;
	uint64_t			io_aborted;
	uint64_t			io_failed;
	uint64_t			current_queue_depth;
	uint64_t			offset_in_ios;
	bool				is_draining;
	struct spdk_nvme_qpair		*qpair;
	struct ctrlr_worker_ctx		*ctrlr_ctx;
	TAILQ_ENTRY(ns_worker_ctx)	link;
};

struct perf_task {
	struct ns_worker_ctx		*ns_ctx;
	void				*buf;
};

struct worker_thread {
	TAILQ_HEAD(, ns_worker_ctx)	ns_ctx;
	TAILQ_HEAD(, ctrlr_worker_ctx)	ctrlr_ctx;
	TAILQ_ENTRY(worker_thread)	link;
	unsigned			lcore;
};

static const char *g_workload_type = "read";
static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);
static int g_num_namespaces;
static TAILQ_HEAD(, worker_thread) g_workers = TAILQ_HEAD_INITIALIZER(g_workers);
static int g_num_workers = 0;
static uint32_t g_master_core;

static int g_abort_interval = 1;

static uint64_t g_tsc_rate;

static uint32_t g_io_size_bytes = 131072;
static uint32_t g_max_io_size_blocks;
static int g_rw_percentage = -1;
static int g_is_random;
static int g_queue_depth = 128;
static int g_time_in_sec = 3;
static int g_dpdk_mem;
static int g_shm_id = -1;
static bool g_no_pci;
static bool g_warn;
static bool g_mix_specified;

static const char *g_core_mask;

struct trid_entry {
	struct spdk_nvme_transport_id	trid;
	uint16_t			nsid;
	TAILQ_ENTRY(trid_entry)		tailq;
};

static TAILQ_HEAD(, trid_entry) g_trid_list = TAILQ_HEAD_INITIALIZER(g_trid_list);

static void io_complete(void *ctx, const struct spdk_nvme_cpl *cpl);

static int
build_nvme_name(char *name, size_t length, struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_transport_id *trid;
	int res = 0;

	trid = spdk_nvme_ctrlr_get_transport_id(ctrlr);

	switch (trid->trtype) {
	case SPDK_NVME_TRANSPORT_PCIE:
		res = snprintf(name, length, "PCIE (%s)", trid->traddr);
		break;
	case SPDK_NVME_TRANSPORT_RDMA:
		res = snprintf(name, length, "RDMA (addr:%s subnqn:%s)", trid->traddr, trid->subnqn);
		break;
	case SPDK_NVME_TRANSPORT_TCP:
		res = snprintf(name, length, "TCP (addr:%s subnqn:%s)", trid->traddr, trid->subnqn);
		break;
	case SPDK_NVME_TRANSPORT_CUSTOM:
		res = snprintf(name, length, "CUSTOM (%s)", trid->traddr);
		break;

	default:
		fprintf(stderr, "Unknown transport type %d\n", trid->trtype);
		break;
	}
	return res;
}

static void
build_nvme_ns_name(char *name, size_t length, struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	int res = 0;

	res = build_nvme_name(name, length, ctrlr);
	if (res > 0) {
		snprintf(name + res, length - res, " NSID %u", nsid);
	}

}

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;
	const struct spdk_nvme_ctrlr_data *cdata;
	uint32_t max_xfer_size, entries, sector_size;
	uint64_t ns_size;
	struct spdk_nvme_io_qpair_opts opts;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (!spdk_nvme_ns_is_active(ns)) {
		printf("Controller %-20.20s (%-20.20s): Skipping inactive NS %u\n",
		       cdata->mn, cdata->sn,
		       spdk_nvme_ns_get_id(ns));
		g_warn = true;
		return;
	}

	ns_size = spdk_nvme_ns_get_size(ns);
	sector_size = spdk_nvme_ns_get_sector_size(ns);

	if (ns_size < g_io_size_bytes || sector_size > g_io_size_bytes) {
		printf("WARNING: controller %-20.20s (%-20.20s) ns %u has invalid "
		       "ns size %" PRIu64 " / block size %u for I/O size %u\n",
		       cdata->mn, cdata->sn, spdk_nvme_ns_get_id(ns),
		       ns_size, spdk_nvme_ns_get_sector_size(ns), g_io_size_bytes);
		g_warn = true;
		return;
	}

	max_xfer_size = spdk_nvme_ns_get_max_io_xfer_size(ns);
	spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
	/* NVMe driver may add additional entries based on
	 * stripe size and maximum transfer size, we assume
	 * 1 more entry be used for stripe.
	 */
	entries = (g_io_size_bytes - 1) / max_xfer_size + 2;
	if ((g_queue_depth * entries) > opts.io_queue_size) {
		printf("controller IO queue size %u less than required\n",
		       opts.io_queue_size);
		printf("Consider using lower queue depth or small IO size because "
		       "IO requests may be queued at the NVMe driver.\n");
	}
	/* For requests which have children requests, parent request itself
	 * will also occupy 1 entry.
	 */
	entries += 1;

	entry = calloc(1, sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	entry->ctrlr = ctrlr;
	entry->ns = ns;
	entry->num_io_requests = g_queue_depth * entries;

	entry->size_in_ios = ns_size / g_io_size_bytes;
	entry->io_size_blocks = g_io_size_bytes / sector_size;

	entry->block_size = spdk_nvme_ns_get_sector_size(ns);

	if (g_max_io_size_blocks < entry->io_size_blocks) {
		g_max_io_size_blocks = entry->io_size_blocks;
	}

	build_nvme_ns_name(entry->name, sizeof(entry->name), ctrlr, spdk_nvme_ns_get_id(ns));

	g_num_namespaces++;
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);
}

static void
unregister_namespaces(void)
{
	struct ns_entry *entry, *tmp;

	TAILQ_FOREACH_SAFE(entry, &g_namespaces, link, tmp) {
		TAILQ_REMOVE(&g_namespaces, entry, link);
		free(entry);
	}
}

static void
register_ctrlr(struct spdk_nvme_ctrlr *ctrlr, struct trid_entry *trid_entry)
{
	struct spdk_nvme_ns *ns;
	struct ctrlr_entry *entry = malloc(sizeof(struct ctrlr_entry));
	uint32_t nsid;

	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	build_nvme_name(entry->name, sizeof(entry->name), ctrlr);

	entry->ctrlr = ctrlr;
	entry->trtype = trid_entry->trid.trtype;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);

	if (trid_entry->nsid == 0) {
		for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
		     nsid != 0; nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
			ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
			if (ns == NULL) {
				continue;
			}
			register_ns(ctrlr, ns);
		}
	} else {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, trid_entry->nsid);
		if (!ns) {
			perror("Namespace does not exist.");
			exit(1);
		}

		register_ns(ctrlr, ns);
	}
}

static void
abort_complete(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct ctrlr_worker_ctx	*ctrlr_ctx = ctx;

	ctrlr_ctx->current_queue_depth--;
	if (spdk_unlikely(spdk_nvme_cpl_is_error(cpl))) {
		ctrlr_ctx->abort_failed++;
	} else if ((cpl->cdw0 & 0x1) == 0) {
		ctrlr_ctx->successful_abort++;
	} else {
		ctrlr_ctx->unsuccessful_abort++;
	}
}

static void
abort_task(struct perf_task *task)
{
	struct ns_worker_ctx	*ns_ctx = task->ns_ctx;
	struct ctrlr_worker_ctx	*ctrlr_ctx = ns_ctx->ctrlr_ctx;
	int			rc;

	/* Hold mutex to guard ctrlr_ctx->current_queue_depth. */
	pthread_mutex_lock(&ctrlr_ctx->mutex);

	rc = spdk_nvme_ctrlr_cmd_abort_ext(ctrlr_ctx->ctrlr, ns_ctx->qpair, task, abort_complete,
					   ctrlr_ctx);

	if (spdk_unlikely(rc != 0)) {
		ctrlr_ctx->abort_submit_failed++;
	} else {
		ctrlr_ctx->current_queue_depth++;
		ctrlr_ctx->abort_submitted++;
	}

	pthread_mutex_unlock(&ctrlr_ctx->mutex);
}

static __thread unsigned int seed = 0;

static inline void
submit_single_io(struct perf_task *task)
{
	uint64_t		offset_in_ios, lba;
	int			rc;
	struct ns_worker_ctx	*ns_ctx = task->ns_ctx;
	struct ns_entry		*entry = ns_ctx->entry;

	if (g_is_random) {
		offset_in_ios = rand_r(&seed) % entry->size_in_ios;
	} else {
		offset_in_ios = ns_ctx->offset_in_ios++;
		if (ns_ctx->offset_in_ios == entry->size_in_ios) {
			ns_ctx->offset_in_ios = 0;
		}
	}

	lba = offset_in_ios * entry->io_size_blocks;

	if ((g_rw_percentage == 100) ||
	    (g_rw_percentage != 0 && (rand_r(&seed) % 100) < g_rw_percentage)) {
		rc = spdk_nvme_ns_cmd_read(entry->ns, ns_ctx->qpair, task->buf,
					   lba, entry->io_size_blocks, io_complete, task, 0);
	} else {
		rc = spdk_nvme_ns_cmd_write(entry->ns, ns_ctx->qpair, task->buf,
					    lba, entry->io_size_blocks, io_complete, task, 0);
	}

	if (spdk_unlikely(rc != 0)) {
		fprintf(stderr, "I/O submission failed\n");
	} else {
		ns_ctx->current_queue_depth++;
		ns_ctx->io_submitted++;

		if ((ns_ctx->io_submitted % g_abort_interval) == 0) {
			abort_task(task);
		}
	}

}

static void
io_complete(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct perf_task	*task = ctx;
	struct ns_worker_ctx	*ns_ctx = task->ns_ctx;

	ns_ctx->current_queue_depth--;
	if (spdk_unlikely(spdk_nvme_cpl_is_error(cpl))) {
		ns_ctx->io_failed++;
	} else {
		ns_ctx->io_completed++;
	}

	/* is_draining indicates when time has expired for the test run and we are
	 * just waiting for the previously submitted I/O to complete. In this case,
	 * do not submit a new I/O to replace the one just completed.
	 */
	if (spdk_unlikely(ns_ctx->is_draining)) {
		spdk_dma_free(task->buf);
		free(task);
	} else {
		submit_single_io(task);
	}
}

static struct perf_task *
allocate_task(struct ns_worker_ctx *ns_ctx)
{
	struct perf_task *task;

	task = calloc(1, sizeof(*task));
	if (task == NULL) {
		fprintf(stderr, "Failed to allocate task\n");
		exit(1);
	}

	task->buf = spdk_dma_zmalloc(g_io_size_bytes, 0x200, NULL);
	if (task->buf == NULL) {
		free(task);
		fprintf(stderr, "Failed to allocate task->buf\n");
		exit(1);
	}

	task->ns_ctx = ns_ctx;

	return task;
}

static void
submit_io(struct ns_worker_ctx *ns_ctx, int queue_depth)
{
	struct perf_task *task;

	while (queue_depth-- > 0) {
		task = allocate_task(ns_ctx);
		submit_single_io(task);
	}
}

static int
work_fn(void *arg)
{
	struct worker_thread *worker = (struct worker_thread *)arg;
	struct ns_worker_ctx *ns_ctx;
	struct ctrlr_worker_ctx *ctrlr_ctx;
	struct ns_entry *ns_entry;
	struct spdk_nvme_io_qpair_opts opts;
	uint64_t tsc_end;
	uint32_t unfinished_ctx;

	/* Allocate queue pair for each namespace. */
	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		ns_entry = ns_ctx->entry;

		spdk_nvme_ctrlr_get_default_io_qpair_opts(ns_entry->ctrlr, &opts, sizeof(opts));
		if (opts.io_queue_requests < ns_entry->num_io_requests) {
			opts.io_queue_requests = ns_entry->num_io_requests;
		}

		ns_ctx->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, &opts, sizeof(opts));
		if (ns_ctx->qpair == NULL) {
			fprintf(stderr, "spdk_nvme_ctrlr_alloc_io_qpair failed\n");
			return 1;
		}
	}

	tsc_end = spdk_get_ticks() + g_time_in_sec * g_tsc_rate;

	/* Submit initial I/O for each namespace. */
	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		submit_io(ns_ctx, g_queue_depth);
	}

	while (1) {
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			spdk_nvme_qpair_process_completions(ns_ctx->qpair, 0);
		}

		if (worker->lcore == g_master_core) {
			TAILQ_FOREACH(ctrlr_ctx, &worker->ctrlr_ctx, link) {
				/* Hold mutex to guard ctrlr_ctx->current_queue_depth. */
				pthread_mutex_lock(&ctrlr_ctx->mutex);
				spdk_nvme_ctrlr_process_admin_completions(ctrlr_ctx->ctrlr);
				pthread_mutex_unlock(&ctrlr_ctx->mutex);
			}
		}

		if (spdk_get_ticks() > tsc_end) {
			break;
		}
	}

	do {
		unfinished_ctx = 0;

		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
			if (!ns_ctx->is_draining) {
				ns_ctx->is_draining = true;
			}
			if (ns_ctx->current_queue_depth > 0) {
				spdk_nvme_qpair_process_completions(ns_ctx->qpair, 0);
				if (ns_ctx->current_queue_depth == 0) {
					spdk_nvme_ctrlr_free_io_qpair(ns_ctx->qpair);
				} else {
					unfinished_ctx++;
				}
			}
		}
	} while (unfinished_ctx > 0);

	if (worker->lcore == g_master_core) {
		do {
			unfinished_ctx = 0;

			TAILQ_FOREACH(ctrlr_ctx, &worker->ctrlr_ctx, link) {
				pthread_mutex_lock(&ctrlr_ctx->mutex);
				if (ctrlr_ctx->current_queue_depth > 0) {
					spdk_nvme_ctrlr_process_admin_completions(ctrlr_ctx->ctrlr);
					if (ctrlr_ctx->current_queue_depth > 0) {
						unfinished_ctx++;
					}
				}
				pthread_mutex_unlock(&ctrlr_ctx->mutex);
			}
		} while (unfinished_ctx > 0);
	}

	return 0;
}

static void
usage(char *program_name)
{
	printf("%s options", program_name);

	printf("\n");
	printf("\t[-q io depth]\n");
	printf("\t[-o io size in bytes]\n");
	printf("\t[-w io pattern type, must be one of\n");
	printf("\t\t(read, write, randread, randwrite, rw, randrw)]\n");
	printf("\t[-M rwmixread (100 for reads, 0 for writes)]\n");
	printf("\t[-t time in seconds]\n");
	printf("\t[-c core mask for I/O submission/completion.]\n");
	printf("\t\t(default: 1)\n");
	printf("\t[-r Transport ID for local PCIe NVMe or NVMeoF]\n");
	printf("\t Format: 'key:value [key:value] ...'\n");
	printf("\t Keys:\n");
	printf("\t  trtype      Transport type (e.g. PCIe, RDMA)\n");
	printf("\t  adrfam      Address family (e.g. IPv4, IPv6)\n");
	printf("\t  traddr      Transport address (e.g. 0000:04:00.0 for PCIe or 192.168.100.8 for RDMA)\n");
	printf("\t  trsvcid     Transport service identifier (e.g. 4420)\n");
	printf("\t  subnqn      Subsystem NQN (default: %s)\n", SPDK_NVMF_DISCOVERY_NQN);
	printf("\t Example: -r 'trtype:PCIe traddr:0000:04:00.0' for PCIe or\n");
	printf("\t          -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.100.8 trsvcid:4420' for NVMeoF\n");
	printf("\t[-s DPDK huge memory size in MB.]\n");
	printf("\t[-i shared memory group ID]\n");
	printf("\t[-a abort interval.]\n");
	printf("\t");
	spdk_log_usage(stdout, "-T");
#ifdef DEBUG
	printf("\t[-G enable debug logging]\n");
#else
	printf("\t[-G enable debug logging (flag disabled, must reconfigure with --enable-debug)\n");
#endif
}

static void
unregister_trids(void)
{
	struct trid_entry *trid_entry, *tmp;

	TAILQ_FOREACH_SAFE(trid_entry, &g_trid_list, tailq, tmp) {
		TAILQ_REMOVE(&g_trid_list, trid_entry, tailq);
		free(trid_entry);
	}
}

static int
add_trid(const char *trid_str)
{
	struct trid_entry *trid_entry;
	struct spdk_nvme_transport_id *trid;
	char *ns;

	trid_entry = calloc(1, sizeof(*trid_entry));
	if (trid_entry == NULL) {
		return -1;
	}

	trid = &trid_entry->trid;
	trid->trtype = SPDK_NVME_TRANSPORT_PCIE;
	snprintf(trid->subnqn, sizeof(trid->subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	if (spdk_nvme_transport_id_parse(trid, trid_str) != 0) {
		fprintf(stderr, "Invalid transport ID format '%s'\n", trid_str);
		free(trid_entry);
		return 1;
	}

	spdk_nvme_transport_id_populate_trstring(trid,
			spdk_nvme_transport_id_trtype_str(trid->trtype));

	ns = strcasestr(trid_str, "ns:");
	if (ns) {
		char nsid_str[6]; /* 5 digits maximum in an nsid */
		int len;
		int nsid;

		ns += 3;

		len = strcspn(ns, " \t\n");
		if (len > 5) {
			fprintf(stderr, "NVMe namespace IDs must be 5 digits or less\n");
			free(trid_entry);
			return 1;
		}

		memcpy(nsid_str, ns, len);
		nsid_str[len] = '\0';

		nsid = spdk_strtol(nsid_str, 10);
		if (nsid <= 0 || nsid > 65535) {
			fprintf(stderr, "NVMe namespace IDs must be less than 65536 and greater than 0\n");
			free(trid_entry);
			return 1;
		}

		trid_entry->nsid = (uint16_t)nsid;
	}

	TAILQ_INSERT_TAIL(&g_trid_list, trid_entry, tailq);
	return 0;
}

static int
parse_args(int argc, char **argv)
{
	int op;
	long int val;
	int rc;

	while ((op = getopt(argc, argv, "a:c:i:o:q:r:s:t:w:M:")) != -1) {
		switch (op) {
		case 'a':
		case 'i':
		case 'o':
		case 'q':
		case 's':
		case 't':
		case 'M':
			val = spdk_strtol(optarg, 10);
			if (val < 0) {
				fprintf(stderr, "Converting a string to integer failed\n");
				return val;
			}
			switch (op) {
			case 'a':
				g_abort_interval = val;
				break;
			case 'i':
				g_shm_id = val;
				break;
			case 'o':
				g_io_size_bytes = val;
				break;
			case 'q':
				g_queue_depth = val;
				break;
			case 's':
				g_dpdk_mem = val;
				break;
			case 't':
				g_time_in_sec = val;
				break;
			case 'M':
				g_rw_percentage = val;
				g_mix_specified = true;
				break;
			}
			break;
		case 'c':
			g_core_mask = optarg;
			break;
		case 'r':
			if (add_trid(optarg)) {
				usage(argv[0]);
				return 1;
			}
			break;
		case 'w':
			g_workload_type = optarg;
			break;
		case 'G':
#ifndef DEBUG
			fprintf(stderr, "%s must be configured with --enable-debug for -G flag\n",
				argv[0]);
			usage(argv[0]);
			return 1;
#else
			spdk_log_set_flag("nvme");
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
			break;
#endif
		case 'T':
			rc = spdk_log_set_flag(optarg);
			if (rc < 0) {
				fprintf(stderr, "unknown flag\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
#ifdef DEBUG
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
#endif
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!g_queue_depth) {
		fprintf(stderr, "missing -q (queue size) operand\n");
		usage(argv[0]);
		return 1;
	}
	if (!g_io_size_bytes) {
		fprintf(stderr, "missing -o (block size) operand\n");
		usage(argv[0]);
		return 1;
	}
	if (!g_workload_type) {
		fprintf(stderr, "missing -t (test time in seconds) operand\n");
		usage(argv[0]);
		return 1;
	}

	if (!g_time_in_sec) {
		usage(argv[0]);
		return 1;
	}

	if (strncmp(g_workload_type, "rand", 4) == 0) {
		g_is_random = 1;
		g_workload_type = &g_workload_type[4];
	}

	if (strcmp(g_workload_type, "read") == 0 || strcmp(g_workload_type, "write") == 0) {
		g_rw_percentage = strcmp(g_workload_type, "read") == 0 ? 100 : 0;
		if (g_mix_specified) {
			fprintf(stderr, "Ignoring -M option... Please use -M option"
				" only when using rw or randrw.\n");
		}
	} else if (strcmp(g_workload_type, "rw") == 0) {
		if (g_rw_percentage < 0 || g_rw_percentage > 100) {
			fprintf(stderr,
				"-M must be specified to value from 0 to 100 "
				"for rw or randrw.\n");
			return 1;
		}
	} else {
		fprintf(stderr,
			"io pattern type must be one of\n"
			"(read, write, randread, randwrite, rw, randrw)\n");
		return 1;
	}

	if (TAILQ_EMPTY(&g_trid_list)) {
		/* If no transport IDs specified, default to enumerating all local PCIe devices */
		add_trid("trtype:PCIe");
	} else {
		struct trid_entry *trid_entry, *trid_entry_tmp;

		g_no_pci = true;
		/* check whether there is local PCIe type */
		TAILQ_FOREACH_SAFE(trid_entry, &g_trid_list, tailq, trid_entry_tmp) {
			if (trid_entry->trid.trtype == SPDK_NVME_TRANSPORT_PCIE) {
				g_no_pci = false;
				break;
			}
		}
	}

	return 0;
}

static int
register_workers(void)
{
	uint32_t i;
	struct worker_thread *worker;

	SPDK_ENV_FOREACH_CORE(i) {
		worker = calloc(1, sizeof(*worker));
		if (worker == NULL) {
			fprintf(stderr, "Unable to allocate worker\n");
			return -1;
		}

		TAILQ_INIT(&worker->ns_ctx);
		TAILQ_INIT(&worker->ctrlr_ctx);
		worker->lcore = i;
		TAILQ_INSERT_TAIL(&g_workers, worker, link);
		g_num_workers++;
	}

	return 0;
}

static void
unregister_workers(void)
{
	struct worker_thread *worker, *tmp_worker;
	struct ns_worker_ctx *ns_ctx, *tmp_ns_ctx;
	struct ctrlr_worker_ctx *ctrlr_ctx, *tmp_ctrlr_ctx;

	/* Free namespace context and worker thread */
	TAILQ_FOREACH_SAFE(worker, &g_workers, link, tmp_worker) {
		TAILQ_REMOVE(&g_workers, worker, link);

		TAILQ_FOREACH_SAFE(ns_ctx, &worker->ns_ctx, link, tmp_ns_ctx) {
			TAILQ_REMOVE(&worker->ns_ctx, ns_ctx, link);
			printf("NS: %s I/O completed: %" PRIu64 ", failed: %" PRIu64 "\n",
			       ns_ctx->entry->name, ns_ctx->io_completed, ns_ctx->io_failed);
			free(ns_ctx);
		}

		TAILQ_FOREACH_SAFE(ctrlr_ctx, &worker->ctrlr_ctx, link, tmp_ctrlr_ctx) {
			TAILQ_REMOVE(&worker->ctrlr_ctx, ctrlr_ctx, link);
			printf("CTRLR: %s abort submitted %" PRIu64 ", failed to submit %" PRIu64 "\n",
			       ctrlr_ctx->entry->name, ctrlr_ctx->abort_submitted,
			       ctrlr_ctx->abort_submit_failed);
			printf("\t success %" PRIu64 ", unsuccess %" PRIu64 ", failed %" PRIu64 "\n",
			       ctrlr_ctx->successful_abort, ctrlr_ctx->unsuccessful_abort,
			       ctrlr_ctx->abort_failed);
			free(ctrlr_ctx);
		}

		free(worker);
	}
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct trid_entry       *trid_entry = cb_ctx;
	struct spdk_pci_addr    pci_addr;
	struct spdk_pci_device  *pci_dev;
	struct spdk_pci_id      pci_id;

	if (trid->trtype != SPDK_NVME_TRANSPORT_PCIE) {
		printf("Attached to NVMe over Fabrics controller at %s:%s: %s\n",
		       trid->traddr, trid->trsvcid,
		       trid->subnqn);
	} else {
		if (spdk_pci_addr_parse(&pci_addr, trid->traddr)) {
			return;
		}

		pci_dev = spdk_nvme_ctrlr_get_pci_device(ctrlr);
		if (!pci_dev) {
			return;
		}

		pci_id = spdk_pci_device_get_id(pci_dev);

		printf("Attached to NVMe Controller at %s [%04x:%04x]\n",
		       trid->traddr,
		       pci_id.vendor_id, pci_id.device_id);
	}

	register_ctrlr(ctrlr, trid_entry);
}

static int
register_controllers(void)
{
	struct trid_entry *trid_entry;

	printf("Initializing NVMe Controllers\n");

	TAILQ_FOREACH(trid_entry, &g_trid_list, tailq) {
		if (spdk_nvme_probe(&trid_entry->trid, trid_entry, probe_cb, attach_cb, NULL) != 0) {
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
	struct ctrlr_entry *entry, *tmp;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	TAILQ_FOREACH_SAFE(entry, &g_controllers, link, tmp) {
		TAILQ_REMOVE(&g_controllers, entry, link);
		spdk_nvme_detach_async(entry->ctrlr, &detach_ctx);
		free(entry);
	}

	while (detach_ctx && spdk_nvme_detach_poll_async(detach_ctx) == -EAGAIN) {
		;
	}
}

static int
associate_master_worker_with_ctrlr(void)
{
	struct ctrlr_entry	*entry;
	struct worker_thread	*worker;
	struct ctrlr_worker_ctx	*ctrlr_ctx;

	TAILQ_FOREACH(worker, &g_workers, link) {
		if (worker->lcore == g_master_core) {
			break;
		}
	}

	if (!worker) {
		return -1;
	}

	TAILQ_FOREACH(entry, &g_controllers, link) {
		ctrlr_ctx = calloc(1, sizeof(struct ctrlr_worker_ctx));
		if (!ctrlr_ctx) {
			return -1;
		}

		pthread_mutex_init(&ctrlr_ctx->mutex, NULL);
		ctrlr_ctx->entry = entry;
		ctrlr_ctx->ctrlr = entry->ctrlr;

		TAILQ_INSERT_TAIL(&worker->ctrlr_ctx, ctrlr_ctx, link);
	}

	return 0;
}

static struct ctrlr_worker_ctx *
get_ctrlr_worker_ctx(struct spdk_nvme_ctrlr *ctrlr)
{
	struct worker_thread	*worker;
	struct ctrlr_worker_ctx *ctrlr_ctx;

	TAILQ_FOREACH(worker, &g_workers, link) {
		if (worker->lcore == g_master_core) {
			break;
		}
	}

	if (!worker) {
		return NULL;
	}

	TAILQ_FOREACH(ctrlr_ctx, &worker->ctrlr_ctx, link) {
		if (ctrlr_ctx->ctrlr == ctrlr) {
			return ctrlr_ctx;
		}
	}

	return NULL;
}

static int
associate_workers_with_ns(void)
{
	struct ns_entry		*entry = TAILQ_FIRST(&g_namespaces);
	struct worker_thread	*worker = TAILQ_FIRST(&g_workers);
	struct ns_worker_ctx	*ns_ctx;
	int			i, count;

	count = g_num_namespaces > g_num_workers ? g_num_namespaces : g_num_workers;

	for (i = 0; i < count; i++) {
		if (entry == NULL) {
			break;
		}

		ns_ctx = calloc(1, sizeof(struct ns_worker_ctx));
		if (!ns_ctx) {
			return -1;
		}

		printf("Associating %s with lcore %d\n", entry->name, worker->lcore);
		ns_ctx->entry = entry;
		ns_ctx->ctrlr_ctx = get_ctrlr_worker_ctx(entry->ctrlr);
		if (!ns_ctx->ctrlr_ctx) {
			free(ns_ctx);
			return -1;
		}

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

int main(int argc, char **argv)
{
	int rc;
	struct worker_thread *worker, *master_worker;
	struct spdk_env_opts opts;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	spdk_env_opts_init(&opts);
	opts.name = "abort";
	opts.shm_id = g_shm_id;
	if (g_core_mask) {
		opts.core_mask = g_core_mask;
	}

	if (g_dpdk_mem) {
		opts.mem_size = g_dpdk_mem;
	}
	if (g_no_pci) {
		opts.no_pci = g_no_pci;
	}
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		rc = -1;
		goto cleanup;
	}

	g_tsc_rate = spdk_get_ticks_hz();

	if (register_workers() != 0) {
		rc = -1;
		goto cleanup;
	}

	if (register_controllers() != 0) {
		rc = -1;
		goto cleanup;
	}

	if (g_warn) {
		printf("WARNING: Some requested NVMe devices were skipped\n");
	}

	if (g_num_namespaces == 0) {
		fprintf(stderr, "No valid NVMe controllers found\n");
		goto cleanup;
	}

	if (associate_master_worker_with_ctrlr() != 0) {
		rc = -1;
		goto cleanup;
	}

	if (associate_workers_with_ns() != 0) {
		rc = -1;
		goto cleanup;
	}

	printf("Initialization complete. Launching workers.\n");

	/* Launch all of the slave workers */
	g_master_core = spdk_env_get_current_core();
	master_worker = NULL;
	TAILQ_FOREACH(worker, &g_workers, link) {
		if (worker->lcore != g_master_core) {
			spdk_env_thread_launch_pinned(worker->lcore, work_fn, worker);
		} else {
			assert(master_worker == NULL);
			master_worker = worker;
		}
	}

	assert(master_worker != NULL);
	rc = work_fn(master_worker);

	spdk_env_thread_wait_all();

cleanup:
	unregister_trids();
	unregister_workers();
	unregister_namespaces();
	unregister_controllers();

	if (rc != 0) {
		fprintf(stderr, "%s: errors occured\n", argv[0]);
	}

	return rc;
}
