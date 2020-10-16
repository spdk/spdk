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

#include "spdk/barrier.h"
#include "spdk/fd.h"
#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/nvme_intel.h"
#include "spdk/histogram_data.h"

#if HAVE_LIBAIO
#include <libaio.h>
#endif

struct ctrlr_entry {
	struct spdk_nvme_ctrlr			*ctrlr;
	TAILQ_ENTRY(ctrlr_entry)		link;
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
			struct spdk_nvme_qpair	*qpair;
		} nvme;
#if HAVE_LIBAIO
		struct {
			int			fd;
			struct io_event		*events;
			io_context_t		ctx;
		} aio;
#endif
	} u;

	uint32_t		io_size_blocks;
	uint64_t		size_in_ios;
	bool			is_draining;
	uint32_t		current_queue_depth;
	char			name[1024];
	struct ns_entry		*next;

	struct spdk_histogram_data	*submit_histogram;
	struct spdk_histogram_data	*complete_histogram;
};

struct perf_task {
	void			*buf;
	uint64_t		submit_tsc;
#if HAVE_LIBAIO
	struct iocb		iocb;
#endif
};

static bool g_enable_histogram = false;

static TAILQ_HEAD(, ctrlr_entry) g_ctrlr = TAILQ_HEAD_INITIALIZER(g_ctrlr);
static struct ns_entry *g_ns = NULL;

static uint64_t g_tsc_rate;

static uint32_t g_io_size_bytes;
static int g_time_in_sec;

static int g_aio_optind; /* Index of first AIO filename in argv */

struct perf_task *g_task;
uint64_t g_tsc_submit = 0;
uint64_t g_tsc_submit_min = UINT64_MAX;
uint64_t g_tsc_submit_max = 0;
uint64_t g_tsc_complete = 0;
uint64_t g_tsc_complete_min = UINT64_MAX;
uint64_t g_tsc_complete_max = 0;
uint64_t g_io_completed = 0;

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

	entry = calloc(1, sizeof(struct ns_entry));
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
	entry->submit_histogram = spdk_histogram_data_alloc();
	entry->complete_histogram = spdk_histogram_data_alloc();

	snprintf(entry->name, 44, "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	entry->next = g_ns;
	g_ns = entry;
}

static void
register_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	int num_ns;
	struct ctrlr_entry *entry = malloc(sizeof(struct ctrlr_entry));
	const struct spdk_nvme_ctrlr_data *cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	entry->ctrlr = ctrlr;

	TAILQ_INSERT_TAIL(&g_ctrlr, entry, link);

	num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
	/* Only register the first namespace. */
	if (num_ns < 1) {
		fprintf(stderr, "controller found with no namespaces\n");
		return;
	}

	register_ns(ctrlr, spdk_nvme_ctrlr_get_ns(ctrlr, 1));
}

#if HAVE_LIBAIO
static int
register_aio_file(const char *path)
{
	struct ns_entry *entry;

	int fd;
	uint64_t size;
	uint32_t blklen;

	fd = open(path, O_RDWR | O_DIRECT);
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

	entry = calloc(1, sizeof(struct ns_entry));
	if (entry == NULL) {
		close(fd);
		perror("aio ns_entry malloc");
		return -1;
	}

	entry->type = ENTRY_TYPE_AIO_FILE;
	entry->u.aio.fd = fd;
	entry->size_in_ios = size / g_io_size_bytes;
	entry->io_size_blocks = g_io_size_bytes / blklen;
	entry->submit_histogram = spdk_histogram_data_alloc();
	entry->complete_histogram = spdk_histogram_data_alloc();

	snprintf(entry->name, sizeof(entry->name), "%s", path);

	g_ns = entry;

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
aio_check_io(void)
{
	int count, i;
	struct timespec timeout;

	timeout.tv_sec = 0;
	timeout.tv_nsec = 0;

	count = io_getevents(g_ns->u.aio.ctx, 1, 1, g_ns->u.aio.events, &timeout);
	if (count < 0) {
		fprintf(stderr, "io_getevents error\n");
		exit(1);
	}

	for (i = 0; i < count; i++) {
		g_ns->current_queue_depth--;
	}
}
#endif /* HAVE_LIBAIO */

static void io_complete(void *ctx, const struct spdk_nvme_cpl *completion);

static __thread unsigned int seed = 0;

static void
submit_single_io(void)
{
	uint64_t		offset_in_ios;
	uint64_t		start;
	int			rc;
	struct ns_entry		*entry = g_ns;
	uint64_t		tsc_submit;

	offset_in_ios = rand_r(&seed) % entry->size_in_ios;

	start = spdk_get_ticks();
	spdk_rmb();
#if HAVE_LIBAIO
	if (entry->type == ENTRY_TYPE_AIO_FILE) {
		rc = aio_submit(g_ns->u.aio.ctx, &g_task->iocb, entry->u.aio.fd, IO_CMD_PREAD, g_task->buf,
				g_io_size_bytes, offset_in_ios * g_io_size_bytes, g_task);
	} else
#endif
	{
		rc = spdk_nvme_ns_cmd_read(entry->u.nvme.ns, g_ns->u.nvme.qpair, g_task->buf,
					   offset_in_ios * entry->io_size_blocks,
					   entry->io_size_blocks, io_complete, g_task, 0);
	}

	spdk_rmb();
	tsc_submit = spdk_get_ticks() - start;
	g_tsc_submit += tsc_submit;
	if (tsc_submit < g_tsc_submit_min) {
		g_tsc_submit_min = tsc_submit;
	}
	if (tsc_submit > g_tsc_submit_max) {
		g_tsc_submit_max = tsc_submit;
	}
	if (g_enable_histogram) {
		spdk_histogram_data_tally(entry->submit_histogram, tsc_submit);
	}

	if (rc != 0) {
		fprintf(stderr, "starting I/O failed\n");
	} else {
		g_ns->current_queue_depth++;
	}
}

static void
io_complete(void *ctx, const struct spdk_nvme_cpl *completion)
{
	g_ns->current_queue_depth--;
}

uint64_t g_complete_tsc_start;

static uint64_t
check_io(void)
{
	uint64_t end, tsc_complete;

	spdk_rmb();
#if HAVE_LIBAIO
	if (g_ns->type == ENTRY_TYPE_AIO_FILE) {
		aio_check_io();
	} else
#endif
	{
		spdk_nvme_qpair_process_completions(g_ns->u.nvme.qpair, 0);
	}
	spdk_rmb();
	end = spdk_get_ticks();
	if (g_ns->current_queue_depth == 1) {
		/*
		 * Account for race condition in AIO case where interrupt occurs
		 *  after checking for queue depth.  If the timestamp capture
		 *  is too big compared to the last capture, assume that an
		 *  interrupt fired, and do not bump the start tsc forward.  This
		 *  will ensure this extra time is accounted for next time through
		 *  when we see current_queue_depth drop to 0.
		 */
		if (g_ns->type == ENTRY_TYPE_NVME_NS || (end - g_complete_tsc_start) < 500) {
			g_complete_tsc_start = end;
		}
	} else {
		tsc_complete = end - g_complete_tsc_start;
		g_tsc_complete += tsc_complete;
		if (tsc_complete < g_tsc_complete_min) {
			g_tsc_complete_min = tsc_complete;
		}
		if (tsc_complete > g_tsc_complete_max) {
			g_tsc_complete_max = tsc_complete;
		}
		if (g_enable_histogram) {
			spdk_histogram_data_tally(g_ns->complete_histogram, tsc_complete);
		}
		g_io_completed++;
		if (!g_ns->is_draining) {
			submit_single_io();
		}
		end = g_complete_tsc_start = spdk_get_ticks();
	}

	return end;
}

static void
drain_io(void)
{
	g_ns->is_draining = true;
	while (g_ns->current_queue_depth > 0) {
		check_io();
	}
}

static int
init_ns_worker_ctx(void)
{
	if (g_ns->type == ENTRY_TYPE_AIO_FILE) {
#ifdef HAVE_LIBAIO
		g_ns->u.aio.events = calloc(1, sizeof(struct io_event));
		if (!g_ns->u.aio.events) {
			return -1;
		}
		g_ns->u.aio.ctx = 0;
		if (io_setup(1, &g_ns->u.aio.ctx) < 0) {
			free(g_ns->u.aio.events);
			perror("io_setup");
			return -1;
		}
#endif
	} else {
		/*
		 * TODO: If a controller has multiple namespaces, they could all use the same queue.
		 *  For now, give each namespace/thread combination its own queue.
		 */
		g_ns->u.nvme.qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ns->u.nvme.ctrlr, NULL, 0);
		if (!g_ns->u.nvme.qpair) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair failed\n");
			return -1;
		}
	}

	return 0;
}

static void
cleanup_ns_worker_ctx(void)
{
	if (g_ns->type == ENTRY_TYPE_AIO_FILE) {
#ifdef HAVE_LIBAIO
		io_destroy(g_ns->u.aio.ctx);
		free(g_ns->u.aio.events);
#endif
	} else {
		spdk_nvme_ctrlr_free_io_qpair(g_ns->u.nvme.qpair);
	}
}

static int
work_fn(void)
{
	uint64_t tsc_end, current;

	/* Allocate a queue pair for each namespace. */
	if (init_ns_worker_ctx() != 0) {
		printf("ERROR: init_ns_worker_ctx() failed\n");
		return 1;
	}

	tsc_end = spdk_get_ticks() + g_time_in_sec * g_tsc_rate;

	/* Submit initial I/O for each namespace. */
	submit_single_io();
	g_complete_tsc_start = spdk_get_ticks();

	while (1) {
		/*
		 * Check for completed I/O for each controller. A new
		 * I/O will be submitted in the io_complete callback
		 * to replace each I/O that is completed.
		 */
		current = check_io();

		if (current > tsc_end) {
			break;
		}
	}

	drain_io();
	cleanup_ns_worker_ctx();

	return 0;
}

static void usage(char *program_name)
{
	printf("%s options", program_name);
#if HAVE_LIBAIO
	printf(" [AIO device(s)]...");
#endif
	printf("\n");
	printf("\t[-s io size in bytes]\n");
	printf("\t[-t time in seconds]\n");
	printf("\t\t(default: 1)]\n");
	printf("\t[-H enable histograms]\n");
}

static void
print_bucket(void *ctx, uint64_t start, uint64_t end, uint64_t count,
	     uint64_t total, uint64_t so_far)
{
	double so_far_pct;

	if (count == 0) {
		return;
	}

	so_far_pct = (double)so_far * 100 / total;

	printf("%9.3f - %9.3f: %9.4f%%  (%9ju)\n",
	       (double)start * 1000 * 1000 / g_tsc_rate,
	       (double)end * 1000 * 1000 / g_tsc_rate,
	       so_far_pct, count);
}

static void
print_stats(void)
{
	double divisor = (double)g_tsc_rate / (1000 * 1000 * 1000);

	printf("submit (in ns)   avg, min, max = %8.1f, %8.1f, %8.1f\n",
	       (double)g_tsc_submit / g_io_completed / divisor,
	       (double)g_tsc_submit_min / divisor,
	       (double)g_tsc_submit_max / divisor);
	printf("complete (in ns) avg, min, max = %8.1f, %8.1f, %8.1f\n",
	       (double)g_tsc_complete / g_io_completed / divisor,
	       (double)g_tsc_complete_min / divisor,
	       (double)g_tsc_complete_max / divisor);

	if (!g_enable_histogram) {
		return;
	}

	printf("\n");
	printf("Submit histogram\n");
	printf("================\n");
	printf("       Range in us     Cumulative     Count\n");
	spdk_histogram_data_iterate(g_ns->submit_histogram, print_bucket, NULL);
	printf("\n");

	printf("Complete histogram\n");
	printf("==================\n");
	printf("       Range in us     Cumulative     Count\n");
	spdk_histogram_data_iterate(g_ns->complete_histogram, print_bucket, NULL);
	printf("\n");

}

static int
parse_args(int argc, char **argv)
{
	int op;
	long int val;

	/* default value */
	g_io_size_bytes = 0;
	g_time_in_sec = 0;

	while ((op = getopt(argc, argv, "hs:t:H")) != -1) {
		switch (op) {
		case 'h':
			usage(argv[0]);
			exit(0);
			break;
		case 's':
			val = spdk_strtol(optarg, 10);
			if (val < 0) {
				fprintf(stderr, "Invalid io size\n");
				return val;
			}
			g_io_size_bytes = (uint32_t)val;
			break;
		case 't':
			g_time_in_sec = spdk_strtol(optarg, 10);
			if (g_time_in_sec < 0) {
				fprintf(stderr, "Invalid run time\n");
				return g_time_in_sec;
			}
			break;
		case 'H':
			g_enable_histogram = true;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!g_io_size_bytes) {
		usage(argv[0]);
		return 1;
	}
	if (!g_time_in_sec) {
		usage(argv[0]);
		return 1;
	}

	g_aio_optind = optind;

	return 0;
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	static uint32_t ctrlr_found = 0;

	if (ctrlr_found == 1) {
		fprintf(stderr, "only attaching to one controller, so skipping\n");
		fprintf(stderr, " controller at PCI address %s\n",
			trid->traddr);
		return false;
	}
	ctrlr_found = 1;

	printf("Attaching to %s\n", trid->traddr);

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attached to %s\n", trid->traddr);

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

	if (g_ns == NULL) {
		fprintf(stderr, "no NVMe controller found - check that device is bound to uio/vfio\n");
		return 1;
	}

	return 0;
}

static void
cleanup(void)
{
	struct ns_entry *ns_entry = g_ns;
	struct ctrlr_entry *ctrlr_entry, *tmp_ctrlr_entry;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	while (ns_entry) {
		struct ns_entry *next = ns_entry->next;

		spdk_histogram_data_free(ns_entry->submit_histogram);
		spdk_histogram_data_free(ns_entry->complete_histogram);
		free(ns_entry);
		ns_entry = next;
	}

	TAILQ_FOREACH_SAFE(ctrlr_entry, &g_ctrlr, link, tmp_ctrlr_entry) {
		TAILQ_REMOVE(&g_ctrlr, ctrlr_entry, link);
		spdk_nvme_detach_async(ctrlr_entry->ctrlr, &detach_ctx);
		free(ctrlr_entry);
	}

	while (detach_ctx && spdk_nvme_detach_poll_async(detach_ctx) == -EAGAIN) {
		;
	}
}

int main(int argc, char **argv)
{
	int			rc;
	struct spdk_env_opts	opts;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	spdk_env_opts_init(&opts);
	opts.name = "overhead";
	opts.core_mask = "0x1";
	opts.shm_id = 0;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	g_task = spdk_zmalloc(sizeof(struct perf_task), 0, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (g_task == NULL) {
		fprintf(stderr, "g_task alloc failed\n");
		exit(1);
	}

	g_task->buf = spdk_zmalloc(g_io_size_bytes, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (g_task->buf == NULL) {
		fprintf(stderr, "g_task->buf spdk_zmalloc failed\n");
		exit(1);
	}

	g_tsc_rate = spdk_get_ticks_hz();

#if HAVE_LIBAIO
	if (g_aio_optind < argc) {
		printf("Measuring overhead for AIO device %s.\n", argv[g_aio_optind]);
		if (register_aio_file(argv[g_aio_optind]) != 0) {
			cleanup();
			return -1;
		}
	} else
#endif
	{
		if (register_controllers() != 0) {
			cleanup();
			return -1;
		}
	}

	printf("Initialization complete. Launching workers.\n");

	rc = work_fn();

	print_stats();

	cleanup();

	if (rc != 0) {
		fprintf(stderr, "%s: errors occured\n", argv[0]);
	}

	return rc;
}
