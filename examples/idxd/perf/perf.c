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

#include "spdk/idxd.h"
#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/crc32.h"
#include "spdk/util.h"

enum idxd_capability {
	IDXD_COPY = 1,
	IDXD_FILL,
	IDXD_DUALCAST,
	IDXD_COMPARE,
	IDXD_CRC32C,
	IDXD_DIF,
	IDXD_COPY_CRC32C,
};

#define DATA_PATTERN 0x5a
#define ALIGN_4K 0x1000

static int g_xfer_size_bytes = 4096;

/* g_allocate_depth indicates how many tasks we allocate per work_chan. It will
 * be at least as much as the queue depth.
 */
static int g_queue_depth = 32;
static int g_idxd_max_per_core = 1;
static char *g_core_mask = "0x1";
static bool g_idxd_kernel_mode = false;
static int g_allocate_depth = 0;
static int g_time_in_sec = 5;
static uint32_t g_crc32c_seed = 0;
static uint32_t g_crc32c_chained_count = 1;
static int g_fail_percent_goal = 0;
static uint8_t g_fill_pattern = 255;
static bool g_verify = false;
static const char *g_workload_type = NULL;
static enum idxd_capability g_workload_selection;
static struct worker_thread *g_workers = NULL;
static int g_num_workers = 0;

struct worker_thread;
struct idxd_chan_entry;
static void idxd_done(void *ref, int status);

struct idxd_device {
	struct				spdk_idxd_device *idxd;
	TAILQ_ENTRY(idxd_device)	tailq;
};
static uint32_t g_num_devices = 0;

static TAILQ_HEAD(, idxd_device) g_idxd_devices = TAILQ_HEAD_INITIALIZER(g_idxd_devices);
static struct idxd_device *g_next_device;

struct idxd_task {
	void			*src;
	struct iovec		*iovs;
	uint32_t		iov_cnt;
	void			*dst;
	void			*dst2;
	uint32_t		crc_dst;
	struct idxd_chan_entry	*worker_chan;
	int			status;
	int			expected_status; /* used for the compare operation */
	TAILQ_ENTRY(idxd_task)	link;
};

struct idxd_chan_entry {
	int				idxd_chan_id;
	struct spdk_idxd_io_channel	*ch;
	uint64_t			xfer_completed;
	uint64_t			xfer_failed;
	uint64_t			injected_miscompares;
	uint64_t			current_queue_depth;
	TAILQ_HEAD(, idxd_task)		tasks_pool_head;
	TAILQ_HEAD(, idxd_task)		resubmits;
	unsigned			core;
	bool				is_draining;
	void				*task_base;
	struct idxd_chan_entry		*next;
};

struct worker_thread {
	struct idxd_chan_entry	*ctx;
	struct worker_thread	*next;
	int			chan_num;
	unsigned		core;
};

static void
dump_user_config(void)
{
	printf("SPDK Configuration:\n");
	printf("Core mask:      %s\n\n", g_core_mask);
	printf("Idxd Perf Configuration:\n");
	printf("Workload Type:   %s\n", g_workload_type);
	if (g_workload_selection == IDXD_CRC32C || g_workload_selection == IDXD_COPY_CRC32C) {
		printf("CRC-32C seed:    %u\n", g_crc32c_seed);
		printf("vector count     %u\n", g_crc32c_chained_count);
	} else if (g_workload_selection == IDXD_FILL) {
		printf("Fill pattern:    0x%x\n", g_fill_pattern);
	} else if ((g_workload_selection == IDXD_COMPARE) && g_fail_percent_goal > 0) {
		printf("Failure inject:  %u percent\n", g_fail_percent_goal);
	}
	if (g_workload_selection == IDXD_COPY_CRC32C) {
		printf("Vector size:     %u bytes\n", g_xfer_size_bytes);
		printf("Transfer size:   %u bytes\n", g_xfer_size_bytes * g_crc32c_chained_count);
	} else {
		printf("Transfer size:   %u bytes\n", g_xfer_size_bytes);
	}
	printf("Queue depth:     %u\n", g_queue_depth);
	printf("Allocated depth: %u\n", g_allocate_depth);
	printf("Run time:        %u seconds\n", g_time_in_sec);
	printf("Verify:          %s\n\n", g_verify ? "Yes" : "No");
}

static void
attach_cb(void *cb_ctx, struct spdk_idxd_device *idxd)
{
	struct idxd_device *dev;

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		fprintf(stderr, "Failed to allocate device struct\n");
		return;
	}

	dev->idxd = idxd;

	TAILQ_INSERT_TAIL(&g_idxd_devices, dev, tailq);
	g_num_devices++;
}

static int
idxd_init(void)
{
	spdk_idxd_set_config(0, g_idxd_kernel_mode);

	if (spdk_idxd_probe(NULL, attach_cb) != 0) {
		fprintf(stderr, "idxd_probe() failed\n");
		return 1;
	}

	return 0;
}

static void
idxd_exit(void)
{
	struct idxd_device *dev;

	while (!TAILQ_EMPTY(&g_idxd_devices)) {
		dev = TAILQ_FIRST(&g_idxd_devices);
		TAILQ_REMOVE(&g_idxd_devices, dev, tailq);
		if (dev->idxd) {
			spdk_idxd_detach(dev->idxd);
		}
		free(dev);
	}
}

static void
usage(void)
{
	printf("idxd_perf options:\n");
	printf("\t[-h help message]\n");
	printf("\t[-a tasks to allocate per core (default: same value as -q)]\n");
	printf("\t[-C for crc32c workload, use this value to configure the io vector size to test (default 1)\n");
	printf("\t[-f for fill workload, use this BYTE value (default 255)\n");
	printf("\t[-k use kernel idxd driver]\n");
	printf("\t[-m core mask for distributing I/O submission/completion work]\n");
	printf("\t[-o transfer size in bytes]\n");
	printf("\t[-P for compare workload, percentage of operations that should miscompare (percent, default 0)\n");
	printf("\t[-q queue depth per core]\n");
	printf("\t[-R max idxd devices per core can drive (default 1)]\n");
	printf("\t[-s for crc32c workload, use this seed value (default 0)\n");
	printf("\t[-t time in seconds]\n");
	printf("\t[-w workload type must be one of these: copy, fill, crc32c, copy_crc32c, compare, dualcast\n");
	printf("\t[-y verify result if this switch is on]\n");
	printf("\t\tCan be used to spread operations across a wider range of memory.\n");
}

static int
parse_args(int argc, char **argv)
{
	int argval = 0;
	int op;

	while ((op = getopt(argc, argv, "a:C:f:hkm:o:P:q:r:t:yw:")) != -1) {
		switch (op) {
		case 'a':
		case 'C':
		case 'f':
		case 'o':
		case 'P':
		case 'q':
		case 'r':
		case 's':
		case 't':
			argval = spdk_strtol(optarg, 10);
			if (argval < 0) {
				fprintf(stderr, "-%c option must be non-negative.\n", argc);
				usage();
				return 1;
			}
			break;
		default:
			break;
		};

		switch (op) {
		case 'a':
			g_allocate_depth = argval;
			break;
		case 'C':
			g_crc32c_chained_count = argval;
			break;
		case 'f':
			g_fill_pattern = (uint8_t)argval;
			break;
		case 'k':
			g_idxd_kernel_mode = true;
			break;
		case 'm':
			g_core_mask = optarg;
			break;
		case 'o':
			g_xfer_size_bytes = argval;
			break;
		case 'P':
			g_fail_percent_goal = argval;
			break;
		case 'q':
			g_queue_depth = argval;
			break;
		case 'r':
			g_idxd_max_per_core = argval;
			break;
		case 's':
			g_crc32c_seed = argval;
			break;
		case 't':
			g_time_in_sec = argval;
			break;
		case 'y':
			g_verify = true;
			break;
		case 'w':
			g_workload_type = optarg;
			if (!strcmp(g_workload_type, "copy")) {
				g_workload_selection = IDXD_COPY;
			} else if (!strcmp(g_workload_type, "fill")) {
				g_workload_selection = IDXD_FILL;
			} else if (!strcmp(g_workload_type, "crc32c")) {
				g_workload_selection = IDXD_CRC32C;
			} else if (!strcmp(g_workload_type, "copy_crc32c")) {
				g_workload_selection = IDXD_COPY_CRC32C;
			} else if (!strcmp(g_workload_type, "compare")) {
				g_workload_selection = IDXD_COMPARE;
			} else if (!strcmp(g_workload_type, "dualcast")) {
				g_workload_selection = IDXD_DUALCAST;
			}
			break;
		case 'h':
			usage();
			exit(0);
		default:
			usage();
			return 1;
		}
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
_free_task_buffers(struct idxd_task *task)
{
	uint32_t i;

	if (g_workload_selection == IDXD_CRC32C) {
		if (task->iovs) {
			for (i = 0; i < task->iov_cnt; i++) {
				if (task->iovs[i].iov_base) {
					spdk_dma_free(task->iovs[i].iov_base);
				}
			}
			free(task->iovs);
		}
	} else {
		spdk_dma_free(task->src);
	}

	spdk_dma_free(task->dst);
	if (g_workload_selection == IDXD_DUALCAST) {
		spdk_dma_free(task->dst2);
	}
}

static inline void
_free_task_buffers_in_pool(struct idxd_chan_entry *t)
{
	struct idxd_task *task;

	assert(t);
	while ((task = TAILQ_FIRST(&t->tasks_pool_head))) {
		TAILQ_REMOVE(&t->tasks_pool_head, task, link);
		_free_task_buffers(task);
	}
}

static void
free_idxd_chan_entry_resource(struct idxd_chan_entry *entry)
{
	assert(entry != NULL);

	if (entry->ch) {
		spdk_idxd_put_channel(entry->ch);
	}

	_free_task_buffers_in_pool(entry);
	free(entry->task_base);
	free(entry);
}

static void
unregister_workers(void)
{
	struct worker_thread *worker = g_workers, *next_worker;
	struct idxd_chan_entry *entry, *entry1;

	/* Free worker thread */
	while (worker) {
		next_worker = worker->next;

		entry = worker->ctx;
		while (entry) {
			entry1 = entry->next;
			free_idxd_chan_entry_resource(entry);
			entry = entry1;
		}

		free(worker);
		worker = next_worker;
		g_num_workers--;
	}

	assert(g_num_workers == 0);
}

static int
_get_task_data_bufs(struct idxd_task *task)
{
	uint32_t align = 0;
	uint32_t i = 0;
	int dst_buff_len = g_xfer_size_bytes;

	/* For dualcast, the DSA HW requires 4K alignment on destination addresses but
	 * we do this for all engines to keep it simple.
	 */
	if (g_workload_selection == IDXD_DUALCAST) {
		align = ALIGN_4K;
	}

	if (g_workload_selection == IDXD_CRC32C || g_workload_selection == IDXD_COPY_CRC32C) {
		assert(g_crc32c_chained_count > 0);
		task->iov_cnt = g_crc32c_chained_count;
		task->iovs = calloc(task->iov_cnt, sizeof(struct iovec));
		if (!task->iovs) {
			fprintf(stderr, "cannot allocated task->iovs fot task=%p\n", task);
			return -ENOMEM;
		}

		if (g_workload_selection == IDXD_COPY_CRC32C) {
			dst_buff_len = g_xfer_size_bytes * g_crc32c_chained_count;
		}

		for (i = 0; i < task->iov_cnt; i++) {
			task->iovs[i].iov_base = spdk_dma_zmalloc(g_xfer_size_bytes, 0, NULL);
			if (task->iovs[i].iov_base == NULL) {
				return -ENOMEM;
			}
			memset(task->iovs[i].iov_base, DATA_PATTERN, g_xfer_size_bytes);
			task->iovs[i].iov_len = g_xfer_size_bytes;
		}

	} else {
		task->src = spdk_dma_zmalloc(g_xfer_size_bytes, 0, NULL);
		if (task->src == NULL) {
			fprintf(stderr, "Unable to alloc src buffer\n");
			return -ENOMEM;
		}

		/* For fill, set the entire src buffer so we can check if verify is enabled. */
		if (g_workload_selection == IDXD_FILL) {
			memset(task->src, g_fill_pattern, g_xfer_size_bytes);
		} else {
			memset(task->src, DATA_PATTERN, g_xfer_size_bytes);
		}
	}

	if (g_workload_selection != IDXD_COPY_CRC32C) {
		task->dst = spdk_dma_zmalloc(dst_buff_len, align, NULL);
		if (task->dst == NULL) {
			fprintf(stderr, "Unable to alloc dst buffer\n");
			return -ENOMEM;
		}

		/* For compare we want the buffers to match, otherwise not. */
		if (g_workload_selection == IDXD_COMPARE) {
			memset(task->dst, DATA_PATTERN, dst_buff_len);
		} else {
			memset(task->dst, ~DATA_PATTERN, dst_buff_len);
		}
	}

	if (g_workload_selection == IDXD_DUALCAST) {
		task->dst2 = spdk_dma_zmalloc(g_xfer_size_bytes, align, NULL);
		if (task->dst2 == NULL) {
			fprintf(stderr, "Unable to alloc dst buffer\n");
			return -ENOMEM;
		}
		memset(task->dst2, ~DATA_PATTERN, g_xfer_size_bytes);
	}

	return 0;
}

inline static struct idxd_task *
_get_task(struct idxd_chan_entry *t)
{
	struct idxd_task *task;

	if (!TAILQ_EMPTY(&t->tasks_pool_head)) {
		task = TAILQ_FIRST(&t->tasks_pool_head);
		TAILQ_REMOVE(&t->tasks_pool_head, task, link);
	} else {
		fprintf(stderr, "Unable to get idxd_task\n");
		return NULL;
	}

	return task;
}

static int idxd_chan_poll(struct idxd_chan_entry *chan);

static void
drain_io(struct idxd_chan_entry *t)
{
	while (t->current_queue_depth > 0) {
		idxd_chan_poll(t);
	}
}

/* Submit one operation using the same idxd task that just completed. */
static void
_submit_single(struct idxd_chan_entry *t, struct idxd_task *task)
{
	int random_num;
	int rc = 0;
	struct iovec siov = {};
	struct iovec diov = {};

	assert(t);

	t->current_queue_depth++;

	if (!TAILQ_EMPTY(&t->resubmits)) {
		rc = -EBUSY;
		goto queue;
	}

	switch (g_workload_selection) {
	case IDXD_COPY:
		siov.iov_base = task->src;
		siov.iov_len = g_xfer_size_bytes;
		diov.iov_base = task->dst;
		diov.iov_len = g_xfer_size_bytes;
		rc = spdk_idxd_submit_copy(t->ch, &diov, 1, &siov, 1,
					   idxd_done, task);
		break;
	case IDXD_FILL:
		/* For fill use the first byte of the task->dst buffer */
		diov.iov_base = task->dst;
		diov.iov_len = g_xfer_size_bytes;
		rc = spdk_idxd_submit_fill(t->ch, &diov, 1, *(uint8_t *)task->src,
					   idxd_done, task);
		break;
	case IDXD_CRC32C:
		assert(task->iovs != NULL);
		assert(task->iov_cnt > 0);
		rc = spdk_idxd_submit_crc32c(t->ch, task->iovs, task->iov_cnt,
					     g_crc32c_seed, &task->crc_dst,
					     idxd_done, task);
		break;
	case IDXD_COMPARE:
		random_num = rand() % 100;
		assert(task->dst != NULL);
		if (random_num < g_fail_percent_goal) {
			task->expected_status = -EILSEQ;
			*(uint8_t *)task->dst = ~DATA_PATTERN;
		} else {
			task->expected_status = 0;
			*(uint8_t *)task->dst = DATA_PATTERN;
		}
		siov.iov_base = task->src;
		siov.iov_len = g_xfer_size_bytes;
		diov.iov_base = task->dst;
		diov.iov_len = g_xfer_size_bytes;
		rc = spdk_idxd_submit_compare(t->ch, &siov, 1, &diov, 1, idxd_done, task);
		break;
	case IDXD_DUALCAST:
		rc = spdk_idxd_submit_dualcast(t->ch, task->dst, task->dst2,
					       task->src, g_xfer_size_bytes, idxd_done, task);
		break;
	default:
		assert(false);
		break;

	}

queue:
	if (rc) {
		/* Queue the task to be resubmitted on the next poll. */
		if (rc != -EBUSY && rc != -EAGAIN) {
			t->xfer_failed++;
		}

		TAILQ_INSERT_TAIL(&t->resubmits, task, link);
	}
}

static int
_vector_memcmp(void *_dst, struct iovec *src_iovs, uint32_t iovcnt)
{
	uint32_t i;
	uint32_t ttl_len = 0;
	uint8_t *dst = (uint8_t *)_dst;

	for (i = 0; i < iovcnt; i++) {
		if (memcmp(dst, src_iovs[i].iov_base, src_iovs[i].iov_len)) {
			return -1;
		}
		dst += src_iovs[i].iov_len;
		ttl_len += src_iovs[i].iov_len;
	}

	if (ttl_len != iovcnt * g_xfer_size_bytes) {
		return -1;
	}

	return 0;
}

static void
idxd_done(void *arg1, int status)
{
	struct idxd_task *task = arg1;
	struct idxd_chan_entry *chan = task->worker_chan;
	uint32_t sw_crc32c;

	assert(chan);
	assert(chan->current_queue_depth > 0);

	if (g_verify && status == 0) {
		switch (g_workload_selection) {
		case IDXD_COPY_CRC32C:
			sw_crc32c = spdk_crc32c_iov_update(task->iovs, task->iov_cnt, ~g_crc32c_seed);
			if (task->crc_dst != sw_crc32c) {
				SPDK_NOTICELOG("CRC-32C miscompare\n");
				chan->xfer_failed++;
			}
			if (_vector_memcmp(task->dst, task->iovs, task->iov_cnt)) {
				SPDK_NOTICELOG("Data miscompare\n");
				chan->xfer_failed++;
			}
			break;
		case IDXD_CRC32C:
			sw_crc32c = spdk_crc32c_iov_update(task->iovs, task->iov_cnt, ~g_crc32c_seed);
			if (task->crc_dst != sw_crc32c) {
				SPDK_NOTICELOG("CRC-32C miscompare\n");
				chan->xfer_failed++;
			}
			break;
		case IDXD_COPY:
			if (memcmp(task->src, task->dst, g_xfer_size_bytes)) {
				SPDK_NOTICELOG("Data miscompare\n");
				chan->xfer_failed++;
			}
			break;
		case IDXD_DUALCAST:
			if (memcmp(task->src, task->dst, g_xfer_size_bytes)) {
				SPDK_NOTICELOG("Data miscompare, first destination\n");
				chan->xfer_failed++;
			}
			if (memcmp(task->src, task->dst2, g_xfer_size_bytes)) {
				SPDK_NOTICELOG("Data miscompare, second destination\n");
				chan->xfer_failed++;
			}
			break;
		case IDXD_FILL:
			if (memcmp(task->dst, task->src, g_xfer_size_bytes)) {
				SPDK_NOTICELOG("Data miscompare\n");
				chan->xfer_failed++;
			}
			break;
		case IDXD_COMPARE:
			break;
		default:
			assert(false);
			break;
		}
	}

	if (task->expected_status == -EILSEQ) {
		assert(status != 0);
		chan->injected_miscompares++;
	} else if (status) {
		/* Expected to pass but the idxd module reported an error (ex: COMPARE operation). */
		chan->xfer_failed++;
	}

	chan->xfer_completed++;
	chan->current_queue_depth--;

	if (!chan->is_draining) {
		_submit_single(chan, task);
	} else {
		TAILQ_INSERT_TAIL(&chan->tasks_pool_head, task, link);
	}
}

static int
dump_result(void)
{
	uint64_t total_completed = 0;
	uint64_t total_failed = 0;
	uint64_t total_miscompared = 0;
	uint64_t total_xfer_per_sec, total_bw_in_MiBps;
	struct worker_thread *worker = g_workers;
	struct idxd_chan_entry *t;

	printf("\nIDXD_ChanID   Core      Transfers      Bandwidth     Failed     Miscompares\n");
	printf("------------------------------------------------------------------------\n");
	while (worker != NULL) {
		t = worker->ctx;
		while (t) {
			uint64_t xfer_per_sec = t->xfer_completed / g_time_in_sec;
			uint64_t bw_in_MiBps = (t->xfer_completed * g_xfer_size_bytes) /
					       (g_time_in_sec * 1024 * 1024);

			total_completed += t->xfer_completed;
			total_failed += t->xfer_failed;
			total_miscompared += t->injected_miscompares;

			if (xfer_per_sec) {
				printf("%10d%5u%15" PRIu64 "/s%9" PRIu64 " MiB/s%7" PRIu64 " %11" PRIu64 "\n",
				       t->idxd_chan_id, worker->core, xfer_per_sec, bw_in_MiBps, t->xfer_failed,
				       t->injected_miscompares);
			}
			t = t->next;
		}

		worker = worker->next;
	}

	total_xfer_per_sec = total_completed / g_time_in_sec;
	total_bw_in_MiBps = (total_completed * g_xfer_size_bytes) /
			    (g_time_in_sec * 1024 * 1024);

	printf("=========================================================================\n");
	printf("Total:%25" PRIu64 "/s%9" PRIu64 " MiB/s%6" PRIu64 " %11" PRIu64"\n\n",
	       total_xfer_per_sec, total_bw_in_MiBps, total_failed, total_miscompared);

	return total_failed ? 1 : 0;
}

static int
submit_all(struct idxd_chan_entry *t)
{
	int i;
	int remaining = g_queue_depth;
	struct idxd_task *task;

	for (i = 0; i < remaining; i++) {
		task = _get_task(t);
		if (task == NULL) {
			_free_task_buffers_in_pool(t);
			return -1;
		}

		/* Submit as single task */
		_submit_single(t, task);
	}

	return 0;
}

static int
idxd_chan_poll(struct idxd_chan_entry *chan)
{
	int			rc;
	struct idxd_task	*task, *tmp;
	TAILQ_HEAD(, idxd_task)	swap;

	rc = spdk_idxd_process_events(chan->ch);
	if (rc < 0) {
		return rc;
	}

	if (!TAILQ_EMPTY(&chan->resubmits)) {
		TAILQ_INIT(&swap);
		TAILQ_SWAP(&swap, &chan->resubmits, idxd_task, link);
		TAILQ_FOREACH_SAFE(task, &swap, link, tmp) {
			TAILQ_REMOVE(&swap, task, link);
			chan->current_queue_depth--;
			if (!chan->is_draining) {
				_submit_single(chan, task);
			} else {
				TAILQ_INSERT_TAIL(&chan->tasks_pool_head, task, link);
			}
		}
	}

	return rc;
}

static int
work_fn(void *arg)
{
	uint64_t tsc_end;
	struct worker_thread *worker = (struct worker_thread *)arg;
	struct idxd_chan_entry *t = NULL;

	printf("Starting thread on core %u\n", worker->core);

	tsc_end = spdk_get_ticks() + g_time_in_sec * spdk_get_ticks_hz();

	t = worker->ctx;
	while (t != NULL) {
		if (submit_all(t) != 0) {
			return -1;
		}
		t = t->next;
	}

	while (1) {
		t = worker->ctx;
		while (t != NULL) {
			idxd_chan_poll(t);
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
init_env(void)
{
	struct spdk_env_opts opts;

	spdk_env_opts_init(&opts);
	opts.name = "idxd_perf";
	opts.core_mask = g_core_mask;
	if (spdk_env_init(&opts) < 0) {
		return 1;
	}

	return 0;
}

static struct spdk_idxd_device *
get_next_idxd(void)
{
	struct spdk_idxd_device *idxd;

	if (g_next_device == NULL) {
		return NULL;
	}

	idxd = g_next_device->idxd;

	g_next_device = TAILQ_NEXT(g_next_device, tailq);

	return idxd;
}

static int
init_idxd_chan_entry(struct idxd_chan_entry *t, struct spdk_idxd_device *idxd)
{
	int num_tasks = g_allocate_depth;
	struct idxd_task *task;
	int i;

	assert(t != NULL);

	TAILQ_INIT(&t->tasks_pool_head);
	TAILQ_INIT(&t->resubmits);
	t->ch = spdk_idxd_get_channel(idxd);
	if (t->ch == NULL) {
		fprintf(stderr, "Failed to get channel\n");
		goto err;
	}

	t->task_base = calloc(g_allocate_depth, sizeof(struct idxd_task));
	if (t->task_base == NULL) {
		fprintf(stderr, "Could not allocate task base.\n");
		goto err;
	}

	task = t->task_base;
	for (i = 0; i < num_tasks; i++) {
		TAILQ_INSERT_TAIL(&t->tasks_pool_head, task, link);
		task->worker_chan = t;
		if (_get_task_data_bufs(task)) {
			fprintf(stderr, "Unable to get data bufs\n");
			goto err;
		}
		task++;
	}

	return 0;

err:
	free_idxd_chan_entry_resource(t);
	return -1;
}

static int
associate_workers_with_idxd_device(void)
{
	struct spdk_idxd_device *idxd = get_next_idxd();
	struct worker_thread	*worker = g_workers;
	int i = 0;
	struct idxd_chan_entry	*t;

	while (idxd != NULL) {
		if (worker->chan_num >= g_idxd_max_per_core) {
			fprintf(stdout, "Notice: we cannot let single worker assign idxd devices\n"
				"more than %d, you need use -r while starting app to change this value\n",
				g_idxd_max_per_core);
			break;
		}

		t = calloc(1, sizeof(struct idxd_chan_entry));
		if (!t) {
			return -1;
		}

		t->idxd_chan_id = i;

		if (init_idxd_chan_entry(t, idxd)) {
			fprintf(stdout, "idxd device=%p is bound on core=%d\n", idxd, worker->core);
			return -1;

		}
		fprintf(stdout, "idxd device=%p is bound on core=%d\n", idxd, worker->core);

		t->next = worker->ctx;
		worker->ctx = t;
		worker->chan_num++;

		worker = worker->next;
		if (worker == NULL) {
			worker = g_workers;
		}

		idxd = get_next_idxd();
		i++;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	int rc;
	struct worker_thread *worker, *main_worker;
	unsigned main_core;

	if (parse_args(argc, argv) != 0) {
		return -1;
	}

	if (init_env() != 0) {
		return -1;
	}

	if (register_workers() != 0) {
		rc = -1;
		goto cleanup;
	}

	if (idxd_init() != 0) {
		rc = -1;
		goto cleanup;
	}

	if (g_num_devices == 0) {
		printf("No idxd device found\n");
		rc = -1;
		goto cleanup;
	}

	if ((g_workload_selection != IDXD_COPY) &&
	    (g_workload_selection != IDXD_FILL) &&
	    (g_workload_selection != IDXD_CRC32C) &&
	    (g_workload_selection != IDXD_COPY_CRC32C) &&
	    (g_workload_selection != IDXD_COMPARE) &&
	    (g_workload_selection != IDXD_DUALCAST)) {
		usage();
		rc = -1;
		goto cleanup;
	}

	if (g_allocate_depth > 0 && g_queue_depth > g_allocate_depth) {
		fprintf(stdout, "allocate depth must be at least as big as queue depth\n");
		usage();
		rc = -1;
		goto cleanup;
	}

	if (g_allocate_depth == 0) {
		g_allocate_depth = g_queue_depth;
	}

	if ((g_workload_selection == IDXD_CRC32C || g_workload_selection == IDXD_COPY_CRC32C) &&
	    g_crc32c_chained_count == 0) {
		usage();
		rc = -1;
		goto cleanup;
	}

	g_next_device = TAILQ_FIRST(&g_idxd_devices);
	if (associate_workers_with_idxd_device() != 0) {
		rc = -1;
		goto cleanup;
	}

	dump_user_config();
	/* Launch all of the secondary workers */
	main_core = spdk_env_get_current_core();
	main_worker = NULL;
	worker = g_workers;
	while (worker != NULL) {
		if (worker->core != main_core) {
			spdk_env_thread_launch_pinned(worker->core, work_fn, worker);
		} else {
			assert(main_worker == NULL);
			main_worker = worker;
		}
		worker = worker->next;
	}

	assert(main_worker != NULL);
	rc = work_fn(main_worker);
	if (rc != 0) {
		goto cleanup;
	}

	spdk_env_thread_wait_all();

	rc = dump_result();
cleanup:
	unregister_workers();
	idxd_exit();

	spdk_env_fini();
	return rc;
}
