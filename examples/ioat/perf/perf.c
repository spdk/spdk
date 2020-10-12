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

#include "spdk/ioat.h"
#include "spdk/env.h"
#include "spdk/queue.h"
#include "spdk/string.h"

struct user_config {
	int xfer_size_bytes;
	int queue_depth;
	int time_in_sec;
	bool verify;
	char *core_mask;
	int ioat_chan_num;
};

struct ioat_device {
	struct spdk_ioat_chan *ioat;
	TAILQ_ENTRY(ioat_device) tailq;
};

static TAILQ_HEAD(, ioat_device) g_devices = TAILQ_HEAD_INITIALIZER(g_devices);
static struct ioat_device *g_next_device;

static struct user_config g_user_config;

struct ioat_chan_entry {
	struct spdk_ioat_chan *chan;
	int ioat_chan_id;
	uint64_t xfer_completed;
	uint64_t xfer_failed;
	uint64_t current_queue_depth;
	uint64_t waiting_for_flush;
	uint64_t flush_threshold;
	bool is_draining;
	struct spdk_mempool *data_pool;
	struct spdk_mempool *task_pool;
	struct ioat_chan_entry *next;
};

struct worker_thread {
	struct ioat_chan_entry	*ctx;
	struct worker_thread	*next;
	unsigned		core;
};

struct ioat_task {
	struct ioat_chan_entry *ioat_chan_entry;
	void *src;
	void *dst;
};

static struct worker_thread *g_workers = NULL;
static int g_num_workers = 0;
static int g_ioat_chan_num = 0;

static void submit_single_xfer(struct ioat_chan_entry *ioat_chan_entry, struct ioat_task *ioat_task,
			       void *dst, void *src);

static void
construct_user_config(struct user_config *self)
{
	self->xfer_size_bytes = 4096;
	self->ioat_chan_num = 1;
	self->queue_depth = 256;
	self->time_in_sec = 10;
	self->verify = false;
	self->core_mask = "0x1";
}

static void
dump_user_config(struct user_config *self)
{
	printf("User configuration:\n");
	printf("Number of channels:    %u\n", self->ioat_chan_num);
	printf("Transfer size:  %u bytes\n", self->xfer_size_bytes);
	printf("Queue depth:    %u\n", self->queue_depth);
	printf("Run time:       %u seconds\n", self->time_in_sec);
	printf("Core mask:      %s\n", self->core_mask);
	printf("Verify:         %s\n\n", self->verify ? "Yes" : "No");
}

static void
ioat_exit(void)
{
	struct ioat_device *dev;

	while (!TAILQ_EMPTY(&g_devices)) {
		dev = TAILQ_FIRST(&g_devices);
		TAILQ_REMOVE(&g_devices, dev, tailq);
		if (dev->ioat) {
			spdk_ioat_detach(dev->ioat);
		}
		spdk_dma_free(dev);
	}
}

static void
ioat_done(void *cb_arg)
{
	struct ioat_task *ioat_task = (struct ioat_task *)cb_arg;
	struct ioat_chan_entry *ioat_chan_entry = ioat_task->ioat_chan_entry;

	if (g_user_config.verify && memcmp(ioat_task->src, ioat_task->dst, g_user_config.xfer_size_bytes)) {
		ioat_chan_entry->xfer_failed++;
	} else {
		ioat_chan_entry->xfer_completed++;
	}

	ioat_chan_entry->current_queue_depth--;

	if (ioat_chan_entry->is_draining) {
		spdk_mempool_put(ioat_chan_entry->data_pool, ioat_task->src);
		spdk_mempool_put(ioat_chan_entry->data_pool, ioat_task->dst);
		spdk_mempool_put(ioat_chan_entry->task_pool, ioat_task);
	} else {
		submit_single_xfer(ioat_chan_entry, ioat_task, ioat_task->dst, ioat_task->src);
	}
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
			return 1;
		}

		worker->core = i;
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
	struct ioat_chan_entry *entry, *entry1;

	/* Free ioat_chan_entry and worker thread */
	while (worker) {
		struct worker_thread *next_worker = worker->next;
		entry = worker->ctx;
		while (entry) {
			entry1 = entry->next;
			spdk_mempool_free(entry->data_pool);
			spdk_mempool_free(entry->task_pool);
			free(entry);
			entry = entry1;
		}
		free(worker);
		worker = next_worker;
	}
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *pci_dev)
{
	printf(" Found matching device at %04x:%02x:%02x.%x "
	       "vendor:0x%04x device:0x%04x\n",
	       spdk_pci_device_get_domain(pci_dev),
	       spdk_pci_device_get_bus(pci_dev), spdk_pci_device_get_dev(pci_dev),
	       spdk_pci_device_get_func(pci_dev),
	       spdk_pci_device_get_vendor_id(pci_dev), spdk_pci_device_get_device_id(pci_dev));

	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *pci_dev, struct spdk_ioat_chan *ioat)
{
	struct ioat_device *dev;

	if (g_ioat_chan_num >= g_user_config.ioat_chan_num) {
		return;
	}

	dev = spdk_dma_zmalloc(sizeof(*dev), 0, NULL);
	if (dev == NULL) {
		printf("Failed to allocate device struct\n");
		return;
	}

	dev->ioat = ioat;
	g_ioat_chan_num++;
	TAILQ_INSERT_TAIL(&g_devices, dev, tailq);
}

static int
ioat_init(void)
{
	if (spdk_ioat_probe(NULL, probe_cb, attach_cb) != 0) {
		fprintf(stderr, "ioat_probe() failed\n");
		return 1;
	}

	return 0;
}

static void
usage(char *program_name)
{
	printf("%s options\n", program_name);
	printf("\t[-h help message]\n");
	printf("\t[-c core mask for distributing I/O submission/completion work]\n");
	printf("\t[-q queue depth]\n");
	printf("\t[-n number of channels]\n");
	printf("\t[-o transfer size in bytes]\n");
	printf("\t[-t time in seconds]\n");
	printf("\t[-v verify copy result if this switch is on]\n");
}

static int
parse_args(int argc, char **argv)
{
	int op;

	construct_user_config(&g_user_config);
	while ((op = getopt(argc, argv, "c:hn:o:q:t:v")) != -1) {
		switch (op) {
		case 'o':
			g_user_config.xfer_size_bytes = spdk_strtol(optarg, 10);
			break;
		case 'n':
			g_user_config.ioat_chan_num = spdk_strtol(optarg, 10);
			break;
		case 'q':
			g_user_config.queue_depth = spdk_strtol(optarg, 10);
			break;
		case 't':
			g_user_config.time_in_sec = spdk_strtol(optarg, 10);
			break;
		case 'c':
			g_user_config.core_mask = optarg;
			break;
		case 'v':
			g_user_config.verify = true;
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		default:
			usage(argv[0]);
			return 1;
		}
	}
	if (g_user_config.xfer_size_bytes <= 0 || g_user_config.queue_depth <= 0 ||
	    g_user_config.time_in_sec <= 0 || !g_user_config.core_mask ||
	    g_user_config.ioat_chan_num <= 0) {
		usage(argv[0]);
		return 1;
	}

	return 0;
}

static void
drain_io(struct ioat_chan_entry *ioat_chan_entry)
{
	spdk_ioat_flush(ioat_chan_entry->chan);
	while (ioat_chan_entry->current_queue_depth > 0) {
		spdk_ioat_process_events(ioat_chan_entry->chan);
	}
}

static void
submit_single_xfer(struct ioat_chan_entry *ioat_chan_entry, struct ioat_task *ioat_task, void *dst,
		   void *src)
{
	ioat_task->ioat_chan_entry = ioat_chan_entry;
	ioat_task->src = src;
	ioat_task->dst = dst;

	spdk_ioat_build_copy(ioat_chan_entry->chan, ioat_task, ioat_done, dst, src,
			     g_user_config.xfer_size_bytes);
	ioat_chan_entry->waiting_for_flush++;
	if (ioat_chan_entry->waiting_for_flush >= ioat_chan_entry->flush_threshold) {
		spdk_ioat_flush(ioat_chan_entry->chan);
		ioat_chan_entry->waiting_for_flush = 0;
	}

	ioat_chan_entry->current_queue_depth++;
}

static int
submit_xfers(struct ioat_chan_entry *ioat_chan_entry, uint64_t queue_depth)
{
	while (queue_depth-- > 0) {
		void *src = NULL, *dst = NULL;
		struct ioat_task *ioat_task = NULL;

		src = spdk_mempool_get(ioat_chan_entry->data_pool);
		dst = spdk_mempool_get(ioat_chan_entry->data_pool);
		ioat_task = spdk_mempool_get(ioat_chan_entry->task_pool);
		if (!ioat_task) {
			fprintf(stderr, "Unable to get ioat_task\n");
			return 1;
		}

		submit_single_xfer(ioat_chan_entry, ioat_task, dst, src);
	}
	return 0;
}

static int
work_fn(void *arg)
{
	uint64_t tsc_end;
	struct worker_thread *worker = (struct worker_thread *)arg;
	struct ioat_chan_entry *t = NULL;

	printf("Starting thread on core %u\n", worker->core);

	tsc_end = spdk_get_ticks() + g_user_config.time_in_sec * spdk_get_ticks_hz();

	t = worker->ctx;
	while (t != NULL) {
		/* begin to submit transfers */
		t->waiting_for_flush = 0;
		t->flush_threshold = g_user_config.queue_depth / 2;
		if (submit_xfers(t, g_user_config.queue_depth) != 0) {
			return 1;
		}
		t = t->next;
	}

	while (1) {
		t = worker->ctx;
		while (t != NULL) {
			spdk_ioat_process_events(t->chan);
			t = t->next;
		}

		if (spdk_get_ticks() > tsc_end) {
			break;
		}
	}

	t = worker->ctx;
	while (t != NULL) {
		/* begin to drain io */
		t->is_draining = true;
		drain_io(t);
		t = t->next;
	}

	return 0;
}

static int
init(void)
{
	struct spdk_env_opts opts;

	spdk_env_opts_init(&opts);
	opts.name = "ioat_perf";
	opts.core_mask = g_user_config.core_mask;
	if (spdk_env_init(&opts) < 0) {
		return 1;
	}

	return 0;
}

static int
dump_result(void)
{
	uint64_t total_completed = 0;
	uint64_t total_failed = 0;
	uint64_t total_xfer_per_sec, total_bw_in_MiBps;
	struct worker_thread *worker = g_workers;

	printf("Channel_ID     Core     Transfers     Bandwidth     Failed\n");
	printf("-----------------------------------------------------------\n");
	while (worker != NULL) {
		struct ioat_chan_entry *t = worker->ctx;
		while (t) {
			uint64_t xfer_per_sec = t->xfer_completed / g_user_config.time_in_sec;
			uint64_t bw_in_MiBps = (t->xfer_completed * g_user_config.xfer_size_bytes) /
					       (g_user_config.time_in_sec * 1024 * 1024);

			total_completed += t->xfer_completed;
			total_failed += t->xfer_failed;

			if (xfer_per_sec) {
				printf("%10d%10d%12" PRIu64 "/s%8" PRIu64 " MiB/s%11" PRIu64 "\n",
				       t->ioat_chan_id, worker->core, xfer_per_sec,
				       bw_in_MiBps, t->xfer_failed);
			}
			t = t->next;
		}
		worker = worker->next;
	}

	total_xfer_per_sec = total_completed / g_user_config.time_in_sec;
	total_bw_in_MiBps = (total_completed * g_user_config.xfer_size_bytes) /
			    (g_user_config.time_in_sec * 1024 * 1024);

	printf("===========================================================\n");
	printf("Total:%26" PRIu64 "/s%8" PRIu64 " MiB/s%11" PRIu64 "\n",
	       total_xfer_per_sec, total_bw_in_MiBps, total_failed);

	return total_failed ? 1 : 0;
}

static struct spdk_ioat_chan *
get_next_chan(void)
{
	struct spdk_ioat_chan *chan;

	if (g_next_device == NULL) {
		return NULL;
	}

	chan = g_next_device->ioat;

	g_next_device = TAILQ_NEXT(g_next_device, tailq);

	return chan;
}

static int
associate_workers_with_chan(void)
{
	struct spdk_ioat_chan *chan = get_next_chan();
	struct worker_thread	*worker = g_workers;
	struct ioat_chan_entry	*t;
	char buf_pool_name[30], task_pool_name[30];
	int i = 0;

	while (chan != NULL) {
		t = calloc(1, sizeof(struct ioat_chan_entry));
		if (!t) {
			return 1;
		}

		t->ioat_chan_id = i;
		snprintf(buf_pool_name, sizeof(buf_pool_name), "buf_pool_%d", i);
		snprintf(task_pool_name, sizeof(task_pool_name), "task_pool_%d", i);
		t->data_pool = spdk_mempool_create(buf_pool_name,
						   g_user_config.queue_depth * 2, /* src + dst */
						   g_user_config.xfer_size_bytes,
						   SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
						   SPDK_ENV_SOCKET_ID_ANY);
		t->task_pool = spdk_mempool_create(task_pool_name,
						   g_user_config.queue_depth,
						   sizeof(struct ioat_task),
						   SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
						   SPDK_ENV_SOCKET_ID_ANY);
		if (!t->data_pool || !t->task_pool) {
			fprintf(stderr, "Could not allocate buffer pool.\n");
			spdk_mempool_free(t->data_pool);
			spdk_mempool_free(t->task_pool);
			free(t);
			return 1;
		}
		printf("Associating ioat_channel %d with core %d\n", i, worker->core);
		t->chan = chan;
		t->next = worker->ctx;
		worker->ctx = t;

		worker = worker->next;
		if (worker == NULL) {
			worker = g_workers;
		}

		chan = get_next_chan();
		i++;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	int rc;
	struct worker_thread *worker, *master_worker;
	unsigned master_core;

	if (parse_args(argc, argv) != 0) {
		return 1;
	}

	if (init() != 0) {
		return 1;
	}

	if (register_workers() != 0) {
		rc = 1;
		goto cleanup;
	}

	if (ioat_init() != 0) {
		rc = 1;
		goto cleanup;
	}

	if (g_ioat_chan_num == 0) {
		printf("No channels found\n");
		rc = 1;
		goto cleanup;
	}

	if (g_user_config.ioat_chan_num > g_ioat_chan_num) {
		printf("%d channels are requested, but only %d are found,"
		       "so only test %d channels\n", g_user_config.ioat_chan_num,
		       g_ioat_chan_num, g_ioat_chan_num);
		g_user_config.ioat_chan_num = g_ioat_chan_num;
	}

	g_next_device = TAILQ_FIRST(&g_devices);
	dump_user_config(&g_user_config);

	if (associate_workers_with_chan() != 0) {
		rc = 1;
		goto cleanup;
	}

	/* Launch all of the slave workers */
	master_core = spdk_env_get_current_core();
	master_worker = NULL;
	worker = g_workers;
	while (worker != NULL) {
		if (worker->core != master_core) {
			spdk_env_thread_launch_pinned(worker->core, work_fn, worker);
		} else {
			assert(master_worker == NULL);
			master_worker = worker;
		}
		worker = worker->next;
	}

	assert(master_worker != NULL);
	rc = work_fn(master_worker);
	if (rc != 0) {
		goto cleanup;
	}

	spdk_env_thread_wait_all();

	rc = dump_result();

cleanup:
	unregister_workers();
	ioat_exit();

	return rc;
}
