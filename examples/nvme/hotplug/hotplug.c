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
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/log.h"

struct dev_ctx {
	TAILQ_ENTRY(dev_ctx)	tailq;
	bool			is_new;
	bool			is_removed;
	bool			is_draining;
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	struct spdk_nvme_qpair	*qpair;
	uint32_t		io_size_blocks;
	uint64_t		size_in_ios;
	uint64_t		io_completed;
	uint64_t		prev_io_completed;
	uint64_t		current_queue_depth;
	uint64_t		offset_in_ios;
	char			name[1024];
};

struct perf_task {
	struct dev_ctx		*dev;
	void			*buf;
};

static TAILQ_HEAD(, dev_ctx) g_devs = TAILQ_HEAD_INITIALIZER(g_devs);

static uint64_t g_tsc_rate;

static uint32_t g_io_size_bytes = 4096;
static int g_queue_depth = 4;
static int g_time_in_sec;
static int g_expected_insert_times = -1;
static int g_expected_removal_times = -1;
static int g_insert_times;
static int g_removal_times;
static int g_shm_id = -1;
static const char *g_iova_mode = NULL;
static uint64_t g_timeout_in_us = SPDK_SEC_TO_USEC;
static struct spdk_nvme_detach_ctx *g_detach_ctx;

static void
task_complete(struct perf_task *task);

static void
timeout_cb(void *cb_arg, struct spdk_nvme_ctrlr *ctrlr,
	   struct spdk_nvme_qpair *qpair, uint16_t cid);

static void
register_dev(struct spdk_nvme_ctrlr *ctrlr)
{
	struct dev_ctx *dev;
	const struct spdk_nvme_ctrlr_data *cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		perror("dev_ctx malloc");
		exit(1);
	}

	snprintf(dev->name, sizeof(dev->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	dev->ctrlr = ctrlr;
	dev->is_new = true;
	dev->is_removed = false;
	dev->is_draining = false;

	spdk_nvme_ctrlr_register_timeout_callback(ctrlr, g_timeout_in_us, g_timeout_in_us, timeout_cb,
			NULL);

	dev->ns = spdk_nvme_ctrlr_get_ns(ctrlr, 1);

	if (!dev->ns || !spdk_nvme_ns_is_active(dev->ns)) {
		fprintf(stderr, "Controller %s: No active namespace; skipping\n", dev->name);
		goto skip;
	}

	if (spdk_nvme_ns_get_size(dev->ns) < g_io_size_bytes ||
	    spdk_nvme_ns_get_sector_size(dev->ns) > g_io_size_bytes) {
		fprintf(stderr, "Controller %s: Invalid "
			"ns size %" PRIu64 " / block size %u for I/O size %u\n",
			dev->name,
			spdk_nvme_ns_get_size(dev->ns),
			spdk_nvme_ns_get_sector_size(dev->ns),
			g_io_size_bytes);
		goto skip;
	}

	dev->size_in_ios = spdk_nvme_ns_get_size(dev->ns) / g_io_size_bytes;
	dev->io_size_blocks = g_io_size_bytes / spdk_nvme_ns_get_sector_size(dev->ns);

	dev->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
	if (!dev->qpair) {
		fprintf(stderr, "ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		goto skip;
	}
	g_insert_times++;
	TAILQ_INSERT_TAIL(&g_devs, dev, tailq);
	return;

skip:
	free(dev);
}

static void
unregister_dev(struct dev_ctx *dev)
{
	fprintf(stderr, "unregister_dev: %s\n", dev->name);

	spdk_nvme_ctrlr_free_io_qpair(dev->qpair);
	spdk_nvme_detach_async(dev->ctrlr, &g_detach_ctx);

	TAILQ_REMOVE(&g_devs, dev, tailq);
	free(dev);
}

static struct perf_task *
alloc_task(struct dev_ctx *dev)
{
	struct perf_task *task;

	task = calloc(1, sizeof(*task));
	if (task == NULL) {
		return NULL;
	}

	task->buf = spdk_dma_zmalloc(g_io_size_bytes, 0x200, NULL);
	if (task->buf == NULL) {
		free(task);
		return NULL;
	}

	task->dev = dev;

	return task;
}

static void
free_task(struct perf_task *task)
{
	spdk_dma_free(task->buf);
	free(task);
}

static void io_complete(void *ctx, const struct spdk_nvme_cpl *completion);

static void
submit_single_io(struct perf_task *task)
{
	struct dev_ctx		*dev = task->dev;
	uint64_t		offset_in_ios;
	int			rc;

	offset_in_ios = dev->offset_in_ios++;
	if (dev->offset_in_ios == dev->size_in_ios) {
		dev->offset_in_ios = 0;
	}

	rc = spdk_nvme_ns_cmd_read(dev->ns, dev->qpair, task->buf,
				   offset_in_ios * dev->io_size_blocks,
				   dev->io_size_blocks, io_complete, task, 0);

	if (rc != 0) {
		fprintf(stderr, "starting I/O failed\n");
		free_task(task);
	} else {
		dev->current_queue_depth++;
	}
}

static void
task_complete(struct perf_task *task)
{
	struct dev_ctx *dev;

	dev = task->dev;
	dev->current_queue_depth--;
	dev->io_completed++;

	/*
	 * is_draining indicates when time has expired for the test run
	 * and we are just waiting for the previously submitted I/O
	 * to complete.  In this case, do not submit a new I/O to replace
	 * the one just completed.
	 */
	if (!dev->is_draining && !dev->is_removed) {
		submit_single_io(task);
	} else {
		free_task(task);
	}
}

static void
io_complete(void *ctx, const struct spdk_nvme_cpl *completion)
{
	task_complete((struct perf_task *)ctx);
}

static void
check_io(struct dev_ctx *dev)
{
	spdk_nvme_qpair_process_completions(dev->qpair, 0);
}

static void
submit_io(struct dev_ctx *dev, int queue_depth)
{
	struct perf_task *task;

	while (queue_depth-- > 0) {
		task = alloc_task(dev);
		if (task == NULL) {
			fprintf(stderr, "task allocation failed\n");
			exit(1);
		}

		submit_single_io(task);
	}
}

static void
drain_io(struct dev_ctx *dev)
{
	dev->is_draining = true;
	while (dev->current_queue_depth > 0) {
		check_io(dev);
	}
}

static void
print_stats(void)
{
	struct dev_ctx *dev;

	TAILQ_FOREACH(dev, &g_devs, tailq) {
		fprintf(stderr, "%-43.43s: %10" PRIu64 " I/Os completed (+%" PRIu64 ")\n",
			dev->name,
			dev->io_completed,
			dev->io_completed - dev->prev_io_completed);
		dev->prev_io_completed = dev->io_completed;
	}

	fprintf(stderr, "\n");
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	fprintf(stderr, "Attaching to %s\n", trid->traddr);

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	fprintf(stderr, "Attached to %s\n", trid->traddr);

	register_dev(ctrlr);
}

static void
remove_cb(void *cb_ctx, struct spdk_nvme_ctrlr *ctrlr)
{
	struct dev_ctx *dev;

	TAILQ_FOREACH(dev, &g_devs, tailq) {
		if (dev->ctrlr == ctrlr) {
			/*
			 * Mark the device as removed, but don't detach yet.
			 *
			 * The I/O handling code will detach once it sees that
			 * is_removed is true and all outstanding I/O have been completed.
			 */
			dev->is_removed = true;
			fprintf(stderr, "Controller removed: %s\n", dev->name);
			return;
		}
	}

	/*
	 * If we get here, this remove_cb is for a controller that we are not tracking
	 * in g_devs (for example, because we skipped it during register_dev),
	 * so immediately detach it.
	 */
	spdk_nvme_detach_async(ctrlr, &g_detach_ctx);
}

static void
timeout_cb(void *cb_arg, struct spdk_nvme_ctrlr *ctrlr,
	   struct spdk_nvme_qpair *qpair, uint16_t cid)
{
	/* leave hotplug monitor loop, use the timeout_cb to monitor the hotplug */
	if (spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, remove_cb) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
	}
}

static void
io_loop(void)
{
	struct dev_ctx *dev, *dev_tmp;
	uint64_t tsc_end;
	uint64_t next_stats_tsc;
	int rc;

	tsc_end = spdk_get_ticks() + g_time_in_sec * g_tsc_rate;
	next_stats_tsc = spdk_get_ticks();

	while (1) {
		uint64_t now;

		/*
		 * Check for completed I/O for each controller. A new
		 * I/O will be submitted in the io_complete callback
		 * to replace each I/O that is completed.
		 */
		TAILQ_FOREACH(dev, &g_devs, tailq) {
			if (dev->is_new) {
				/* Submit initial I/O for this controller. */
				submit_io(dev, g_queue_depth);
				dev->is_new = false;
			}

			check_io(dev);
		}

		/*
		 * Check for hotplug events.
		 */
		if (spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, remove_cb) != 0) {
			fprintf(stderr, "spdk_nvme_probe() failed\n");
			break;
		}

		/*
		 * Check for devices which were hot-removed and have finished
		 * processing outstanding I/Os.
		 *
		 * unregister_dev() may remove devs from the list, so use the
		 * removal-safe iterator.
		 */
		TAILQ_FOREACH_SAFE(dev, &g_devs, tailq, dev_tmp) {
			if (dev->is_removed && dev->current_queue_depth == 0) {
				g_removal_times++;
				unregister_dev(dev);
			}
		}

		if (g_detach_ctx) {
			rc = spdk_nvme_detach_poll_async(g_detach_ctx);
			if (rc == 0) {
				g_detach_ctx = NULL;
			}
		}

		now = spdk_get_ticks();
		if (now > tsc_end) {
			break;
		}
		if (now > next_stats_tsc) {
			print_stats();
			next_stats_tsc += g_tsc_rate;
		}

		if (g_insert_times == g_expected_insert_times && g_removal_times == g_expected_removal_times) {
			break;
		}
	}

	TAILQ_FOREACH_SAFE(dev, &g_devs, tailq, dev_tmp) {
		drain_io(dev);
		unregister_dev(dev);
	}

	if (g_detach_ctx) {
		spdk_nvme_detach_poll(g_detach_ctx);
	}
}

static void usage(char *program_name)
{
	printf("%s options", program_name);
	printf("\n");
	printf("\t[-c timeout for each command in second(default:1s)]\n");
	printf("\t[-i shm id (optional)]\n");
	printf("\t[-n expected hot insert times]\n");
	printf("\t[-r expected hot removal times]\n");
	printf("\t[-t time in seconds]\n");
	printf("\t[-m iova mode: pa or va (optional)\n");
	printf("\t[-l log level]\n");
	printf("\t Available log levels:\n");
	printf("\t  disabled, error, warning, notice, info, debug\n");
}

static int
parse_args(int argc, char **argv)
{
	int op;
	long int val;

	/* default value */
	g_time_in_sec = 0;

	while ((op = getopt(argc, argv, "c:i:l:m:n:r:t:")) != -1) {
		if (op == '?') {
			usage(argv[0]);
			return 1;
		}

		switch (op) {
		case 'c':
		case 'i':
		case 'n':
		case 'r':
		case 't':
			val = spdk_strtol(optarg, 10);
			if (val < 0) {
				fprintf(stderr, "Converting a string to integer failed\n");
				return val;
			}
			switch (op) {
			case 'c':
				g_timeout_in_us = val * SPDK_SEC_TO_USEC;
				break;
			case 'i':
				g_shm_id = val;
				break;
			case 'n':
				g_expected_insert_times = val;
				break;
			case 'r':
				g_expected_removal_times = val;
				break;
			case 't':
				g_time_in_sec = val;
				break;
			}
			break;
		case 'm':
			g_iova_mode = optarg;
			break;
		case 'l':
			if (!strcmp(optarg, "disabled")) {
				spdk_log_set_print_level(SPDK_LOG_DISABLED);
			} else if (!strcmp(optarg, "error")) {
				spdk_log_set_print_level(SPDK_LOG_ERROR);
			} else if (!strcmp(optarg, "warning")) {
				spdk_log_set_print_level(SPDK_LOG_WARN);
			} else if (!strcmp(optarg, "notice")) {
				spdk_log_set_print_level(SPDK_LOG_NOTICE);
			} else if (!strcmp(optarg, "info")) {
				spdk_log_set_print_level(SPDK_LOG_INFO);
			} else if (!strcmp(optarg, "debug")) {
				spdk_log_set_print_level(SPDK_LOG_DEBUG);
			} else {
				fprintf(stderr, "Unrecognized log level: %s\n", optarg);
				return 1;
			}
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!g_time_in_sec) {
		usage(argv[0]);
		return 1;
	}

	return 0;
}


static int
register_controllers(void)
{
	fprintf(stderr, "Initializing NVMe Controllers\n");

	if (spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, remove_cb) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		return 1;
	}
	/* Reset g_insert_times to 0 so that we do not count controllers attached at start as hotplug events. */
	g_insert_times = 0;
	return 0;
}

int main(int argc, char **argv)
{
	int rc;
	struct spdk_env_opts opts;

	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	spdk_env_opts_init(&opts);
	opts.name = "hotplug";
	opts.core_mask = "0x1";
	if (g_shm_id > -1) {
		opts.shm_id = g_shm_id;
	}
	if (g_iova_mode) {
		opts.iova_mode = g_iova_mode;
	}
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	g_tsc_rate = spdk_get_ticks_hz();

	/* Detect the controllers that are plugged in at startup. */
	if (register_controllers() != 0) {
		rc = 1;
		goto cleanup;
	}

	fprintf(stderr, "Initialization complete. Starting I/O...\n");
	io_loop();

	if (g_expected_insert_times != -1 && g_insert_times != g_expected_insert_times) {
		fprintf(stderr, "Expected inserts %d != actual inserts %d\n",
			g_expected_insert_times, g_insert_times);
		rc = 1;
		goto cleanup;
	}

	if (g_expected_removal_times != -1 && g_removal_times != g_expected_removal_times) {
		fprintf(stderr, "Expected removals %d != actual removals %d\n",
			g_expected_removal_times, g_removal_times);
		rc = 1;
	}

cleanup:
	spdk_env_fini();
	return rc;
}
