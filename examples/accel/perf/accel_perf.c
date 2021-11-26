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
#include "spdk/thread.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/accel_engine.h"
#include "spdk/crc32.h"
#include "spdk/util.h"

#define DATA_PATTERN 0x5a
#define ALIGN_4K 0x1000

static uint64_t	g_tsc_rate;
static uint64_t g_tsc_end;
static int g_rc;
static int g_xfer_size_bytes = 4096;
static int g_queue_depth = 32;
/* g_allocate_depth indicates how many tasks we allocate per worker. It will
 * be at least as much as the queue depth.
 */
static int g_allocate_depth = 0;
static int g_threads_per_core = 1;
static int g_time_in_sec = 5;
static uint32_t g_crc32c_seed = 0;
static uint32_t g_crc32c_chained_count = 1;
static int g_fail_percent_goal = 0;
static uint8_t g_fill_pattern = 255;
static bool g_verify = false;
static const char *g_workload_type = NULL;
static enum accel_capability g_workload_selection;
static struct worker_thread *g_workers = NULL;
static int g_num_workers = 0;
static pthread_mutex_t g_workers_lock = PTHREAD_MUTEX_INITIALIZER;

struct worker_thread;
static void accel_done(void *ref, int status);

struct display_info {
	int core;
	int thread;
};

struct ap_task {
	void			*src;
	struct iovec		*iovs;
	uint32_t		iov_cnt;
	void			*dst;
	void			*dst2;
	uint32_t		crc_dst;
	struct worker_thread	*worker;
	int			expected_status; /* used for the compare operation */
	TAILQ_ENTRY(ap_task)	link;
};

struct worker_thread {
	struct spdk_io_channel		*ch;
	uint64_t			xfer_completed;
	uint64_t			xfer_failed;
	uint64_t			injected_miscompares;
	uint64_t			current_queue_depth;
	TAILQ_HEAD(, ap_task)		tasks_pool;
	struct worker_thread		*next;
	unsigned			core;
	struct spdk_thread		*thread;
	bool				is_draining;
	struct spdk_poller		*is_draining_poller;
	struct spdk_poller		*stop_poller;
	void				*task_base;
	struct display_info		display;
};

static void
dump_user_config(struct spdk_app_opts *opts)
{
	printf("SPDK Configuration:\n");
	printf("Core mask:      %s\n\n", opts->reactor_mask);
	printf("Accel Perf Configuration:\n");
	printf("Workload Type:  %s\n", g_workload_type);
	if (g_workload_selection == ACCEL_CRC32C || g_workload_selection == ACCEL_COPY_CRC32C) {
		printf("CRC-32C seed:   %u\n", g_crc32c_seed);
		printf("vector count    %u\n", g_crc32c_chained_count);
	} else if (g_workload_selection == ACCEL_FILL) {
		printf("Fill pattern:   0x%x\n", g_fill_pattern);
	} else if ((g_workload_selection == ACCEL_COMPARE) && g_fail_percent_goal > 0) {
		printf("Failure inject: %u percent\n", g_fail_percent_goal);
	}
	if (g_workload_selection == ACCEL_COPY_CRC32C) {
		printf("Vector size:    %u bytes\n", g_xfer_size_bytes);
		printf("Transfer size:  %u bytes\n", g_xfer_size_bytes * g_crc32c_chained_count);
	} else {
		printf("Transfer size:  %u bytes\n", g_xfer_size_bytes);
	}
	printf("Queue depth:    %u\n", g_queue_depth);
	printf("Allocate depth: %u\n", g_allocate_depth);
	printf("# threads/core: %u\n", g_threads_per_core);
	printf("Run time:       %u seconds\n", g_time_in_sec);
	printf("Verify:         %s\n\n", g_verify ? "Yes" : "No");
}

static void
usage(void)
{
	printf("accel_perf options:\n");
	printf("\t[-h help message]\n");
	printf("\t[-q queue depth per core]\n");
	printf("\t[-C for crc32c workload, use this value to configure the io vector size to test (default 1)\n");
	printf("\t[-T number of threads per core\n");
	printf("\t[-n number of channels]\n");
	printf("\t[-o transfer size in bytes]\n");
	printf("\t[-t time in seconds]\n");
	printf("\t[-w workload type must be one of these: copy, fill, crc32c, copy_crc32c, compare, dualcast\n");
	printf("\t[-s for crc32c workload, use this seed value (default 0)\n");
	printf("\t[-P for compare workload, percentage of operations that should miscompare (percent, default 0)\n");
	printf("\t[-f for fill workload, use this BYTE value (default 255)\n");
	printf("\t[-y verify result if this switch is on]\n");
	printf("\t[-a tasks to allocate per core (default: same value as -q)]\n");
	printf("\t\tCan be used to spread operations across a wider range of memory.\n");
}

static int
parse_args(int argc, char *argv)
{
	int argval = 0;

	switch (argc) {
	case 'a':
	case 'C':
	case 'f':
	case 'T':
	case 'o':
	case 'P':
	case 'q':
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

	switch (argc) {
	case 'a':
		g_allocate_depth = argval;
		break;
	case 'C':
		g_crc32c_chained_count = argval;
		break;
	case 'f':
		g_fill_pattern = (uint8_t)argval;
		break;
	case 'T':
		g_threads_per_core = argval;
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
			g_workload_selection = ACCEL_COPY;
		} else if (!strcmp(g_workload_type, "fill")) {
			g_workload_selection = ACCEL_FILL;
		} else if (!strcmp(g_workload_type, "crc32c")) {
			g_workload_selection = ACCEL_CRC32C;
		} else if (!strcmp(g_workload_type, "copy_crc32c")) {
			g_workload_selection = ACCEL_COPY_CRC32C;
		} else if (!strcmp(g_workload_type, "compare")) {
			g_workload_selection = ACCEL_COMPARE;
		} else if (!strcmp(g_workload_type, "dualcast")) {
			g_workload_selection = ACCEL_DUALCAST;
		}
		break;
	default:
		usage();
		return 1;
	}

	return 0;
}

static int dump_result(void);
static void
unregister_worker(void *arg1)
{
	struct worker_thread *worker = arg1;

	free(worker->task_base);
	spdk_put_io_channel(worker->ch);
	pthread_mutex_lock(&g_workers_lock);
	assert(g_num_workers >= 1);
	if (--g_num_workers == 0) {
		pthread_mutex_unlock(&g_workers_lock);
		g_rc = dump_result();
		spdk_app_stop(0);
	}
	pthread_mutex_unlock(&g_workers_lock);
}

static int
_get_task_data_bufs(struct ap_task *task)
{
	uint32_t align = 0;
	uint32_t i = 0;
	int dst_buff_len = g_xfer_size_bytes;

	/* For dualcast, the DSA HW requires 4K alignment on destination addresses but
	 * we do this for all engines to keep it simple.
	 */
	if (g_workload_selection == ACCEL_DUALCAST) {
		align = ALIGN_4K;
	}

	if (g_workload_selection == ACCEL_CRC32C || g_workload_selection == ACCEL_COPY_CRC32C) {
		assert(g_crc32c_chained_count > 0);
		task->iov_cnt = g_crc32c_chained_count;
		task->iovs = calloc(task->iov_cnt, sizeof(struct iovec));
		if (!task->iovs) {
			fprintf(stderr, "cannot allocated task->iovs fot task=%p\n", task);
			return -ENOMEM;
		}

		if (g_workload_selection == ACCEL_COPY_CRC32C) {
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
		if (g_workload_selection == ACCEL_FILL) {
			memset(task->src, g_fill_pattern, g_xfer_size_bytes);
		} else {
			memset(task->src, DATA_PATTERN, g_xfer_size_bytes);
		}
	}

	if (g_workload_selection != ACCEL_CRC32C) {
		task->dst = spdk_dma_zmalloc(dst_buff_len, align, NULL);
		if (task->dst == NULL) {
			fprintf(stderr, "Unable to alloc dst buffer\n");
			return -ENOMEM;
		}

		/* For compare we want the buffers to match, otherwise not. */
		if (g_workload_selection == ACCEL_COMPARE) {
			memset(task->dst, DATA_PATTERN, dst_buff_len);
		} else {
			memset(task->dst, ~DATA_PATTERN, dst_buff_len);
		}
	}

	if (g_workload_selection == ACCEL_DUALCAST) {
		task->dst2 = spdk_dma_zmalloc(g_xfer_size_bytes, align, NULL);
		if (task->dst2 == NULL) {
			fprintf(stderr, "Unable to alloc dst buffer\n");
			return -ENOMEM;
		}
		memset(task->dst2, ~DATA_PATTERN, g_xfer_size_bytes);
	}

	return 0;
}

inline static struct ap_task *
_get_task(struct worker_thread *worker)
{
	struct ap_task *task;

	if (!TAILQ_EMPTY(&worker->tasks_pool)) {
		task = TAILQ_FIRST(&worker->tasks_pool);
		TAILQ_REMOVE(&worker->tasks_pool, task, link);
	} else {
		fprintf(stderr, "Unable to get ap_task\n");
		return NULL;
	}

	return task;
}

/* Submit one operation using the same ap task that just completed. */
static void
_submit_single(struct worker_thread *worker, struct ap_task *task)
{
	int random_num;
	int rc = 0;

	assert(worker);

	switch (g_workload_selection) {
	case ACCEL_COPY:
		rc = spdk_accel_submit_copy(worker->ch, task->dst, task->src,
					    g_xfer_size_bytes, accel_done, task);
		break;
	case ACCEL_FILL:
		/* For fill use the first byte of the task->dst buffer */
		rc = spdk_accel_submit_fill(worker->ch, task->dst, *(uint8_t *)task->src,
					    g_xfer_size_bytes, accel_done, task);
		break;
	case ACCEL_CRC32C:
		rc = spdk_accel_submit_crc32cv(worker->ch, &task->crc_dst,
					       task->iovs, task->iov_cnt, g_crc32c_seed,
					       accel_done, task);
		break;
	case ACCEL_COPY_CRC32C:
		rc = spdk_accel_submit_copy_crc32cv(worker->ch, task->dst, task->iovs, task->iov_cnt,
						    &task->crc_dst, g_crc32c_seed, accel_done, task);
		break;
	case ACCEL_COMPARE:
		random_num = rand() % 100;
		if (random_num < g_fail_percent_goal) {
			task->expected_status = -EILSEQ;
			*(uint8_t *)task->dst = ~DATA_PATTERN;
		} else {
			task->expected_status = 0;
			*(uint8_t *)task->dst = DATA_PATTERN;
		}
		rc = spdk_accel_submit_compare(worker->ch, task->dst, task->src,
					       g_xfer_size_bytes, accel_done, task);
		break;
	case ACCEL_DUALCAST:
		rc = spdk_accel_submit_dualcast(worker->ch, task->dst, task->dst2,
						task->src, g_xfer_size_bytes, accel_done, task);
		break;
	default:
		assert(false);
		break;

	}

	if (rc) {
		accel_done(task, rc);
	}
}

static void
_free_task_buffers(struct ap_task *task)
{
	uint32_t i;

	if (g_workload_selection == ACCEL_CRC32C || g_workload_selection == ACCEL_COPY_CRC32C) {
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
	if (g_workload_selection == ACCEL_DUALCAST) {
		spdk_dma_free(task->dst2);
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
accel_done(void *arg1, int status)
{
	struct ap_task *task = arg1;
	struct worker_thread *worker = task->worker;
	uint32_t sw_crc32c;

	assert(worker);
	assert(worker->current_queue_depth > 0);

	if (g_verify && status == 0) {
		switch (g_workload_selection) {
		case ACCEL_COPY_CRC32C:
			sw_crc32c = spdk_crc32c_iov_update(task->iovs, task->iov_cnt, ~g_crc32c_seed);
			if (task->crc_dst != sw_crc32c) {
				SPDK_NOTICELOG("CRC-32C miscompare\n");
				worker->xfer_failed++;
			}
			if (_vector_memcmp(task->dst, task->iovs, task->iov_cnt)) {
				SPDK_NOTICELOG("Data miscompare\n");
				worker->xfer_failed++;
			}
			break;
		case ACCEL_CRC32C:
			sw_crc32c = spdk_crc32c_iov_update(task->iovs, task->iov_cnt, ~g_crc32c_seed);
			if (task->crc_dst != sw_crc32c) {
				SPDK_NOTICELOG("CRC-32C miscompare\n");
				worker->xfer_failed++;
			}
			break;
		case ACCEL_COPY:
			if (memcmp(task->src, task->dst, g_xfer_size_bytes)) {
				SPDK_NOTICELOG("Data miscompare\n");
				worker->xfer_failed++;
			}
			break;
		case ACCEL_DUALCAST:
			if (memcmp(task->src, task->dst, g_xfer_size_bytes)) {
				SPDK_NOTICELOG("Data miscompare, first destination\n");
				worker->xfer_failed++;
			}
			if (memcmp(task->src, task->dst2, g_xfer_size_bytes)) {
				SPDK_NOTICELOG("Data miscompare, second destination\n");
				worker->xfer_failed++;
			}
			break;
		case ACCEL_FILL:
			if (memcmp(task->dst, task->src, g_xfer_size_bytes)) {
				SPDK_NOTICELOG("Data miscompare\n");
				worker->xfer_failed++;
			}
			break;
		case ACCEL_COMPARE:
			break;
		default:
			assert(false);
			break;
		}
	}

	if (task->expected_status == -EILSEQ) {
		assert(status != 0);
		worker->injected_miscompares++;
	} else if (status) {
		/* Expected to pass but the accel engine reported an error (ex: COMPARE operation). */
		worker->xfer_failed++;
	}

	worker->xfer_completed++;
	worker->current_queue_depth--;

	if (!worker->is_draining) {
		TAILQ_INSERT_TAIL(&worker->tasks_pool, task, link);
		task = _get_task(worker);
		_submit_single(worker, task);
		worker->current_queue_depth++;
	} else {
		TAILQ_INSERT_TAIL(&worker->tasks_pool, task, link);
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

	printf("\nCore,Thread   Transfers     Bandwidth     Failed     Miscompares\n");
	printf("------------------------------------------------------------------------\n");
	while (worker != NULL) {

		uint64_t xfer_per_sec = worker->xfer_completed / g_time_in_sec;
		uint64_t bw_in_MiBps = (worker->xfer_completed * g_xfer_size_bytes) /
				       (g_time_in_sec * 1024 * 1024);

		total_completed += worker->xfer_completed;
		total_failed += worker->xfer_failed;
		total_miscompared += worker->injected_miscompares;

		if (xfer_per_sec) {
			printf("%u,%u%17" PRIu64 "/s%9" PRIu64 " MiB/s%7" PRIu64 " %11" PRIu64 "\n",
			       worker->display.core, worker->display.thread, xfer_per_sec,
			       bw_in_MiBps, worker->xfer_failed, worker->injected_miscompares);
		}

		worker = worker->next;
	}

	total_xfer_per_sec = total_completed / g_time_in_sec;
	total_bw_in_MiBps = (total_completed * g_xfer_size_bytes) /
			    (g_time_in_sec * 1024 * 1024);

	printf("=========================================================================\n");
	printf("Total:%15" PRIu64 "/s%9" PRIu64 " MiB/s%6" PRIu64 " %11" PRIu64"\n\n",
	       total_xfer_per_sec, total_bw_in_MiBps, total_failed, total_miscompared);

	return total_failed ? 1 : 0;
}

static inline void
_free_task_buffers_in_pool(struct worker_thread *worker)
{
	struct ap_task *task;

	assert(worker);
	while ((task = TAILQ_FIRST(&worker->tasks_pool))) {
		TAILQ_REMOVE(&worker->tasks_pool, task, link);
		_free_task_buffers(task);
	}
}

static int
_check_draining(void *arg)
{
	struct worker_thread *worker = arg;

	assert(worker);

	if (worker->current_queue_depth == 0) {
		_free_task_buffers_in_pool(worker);
		spdk_poller_unregister(&worker->is_draining_poller);
		unregister_worker(worker);
	}

	return SPDK_POLLER_BUSY;
}

static int
_worker_stop(void *arg)
{
	struct worker_thread *worker = arg;

	assert(worker);

	spdk_poller_unregister(&worker->stop_poller);

	/* now let the worker drain and check it's outstanding IO with a poller */
	worker->is_draining = true;
	worker->is_draining_poller = SPDK_POLLER_REGISTER(_check_draining, worker, 0);

	return SPDK_POLLER_BUSY;
}

static inline void
identify_accel_engine_usage(struct spdk_io_channel *ch)
{
	uint64_t capabilities;

	assert(ch != NULL);
	capabilities = spdk_accel_get_capabilities(ch);
	if ((capabilities & g_workload_selection) != g_workload_selection) {
		SPDK_WARNLOG("The selected workload is not natively supported by the current engine\n");
		SPDK_WARNLOG("The software engine will be used instead.\n\n");
	}
}

static void
_init_thread(void *arg1)
{
	struct worker_thread *worker;
	struct ap_task *task;
	int i, num_tasks = g_allocate_depth;
	struct display_info *display = arg1;

	worker = calloc(1, sizeof(*worker));
	if (worker == NULL) {
		fprintf(stderr, "Unable to allocate worker\n");
		free(display);
		return;
	}

	worker->display.core = display->core;
	worker->display.thread = display->thread;
	free(display);
	worker->core = spdk_env_get_current_core();
	worker->thread = spdk_get_thread();
	pthread_mutex_lock(&g_workers_lock);
	i = g_num_workers;
	g_num_workers++;
	worker->next = g_workers;
	g_workers = worker;
	pthread_mutex_unlock(&g_workers_lock);
	worker->ch = spdk_accel_engine_get_io_channel();

	if (i == 0) {
		identify_accel_engine_usage(worker->ch);
	}

	TAILQ_INIT(&worker->tasks_pool);

	worker->task_base = calloc(num_tasks, sizeof(struct ap_task));
	if (worker->task_base == NULL) {
		fprintf(stderr, "Could not allocate task base.\n");
		goto error;
	}

	task = worker->task_base;
	for (i = 0; i < num_tasks; i++) {
		TAILQ_INSERT_TAIL(&worker->tasks_pool, task, link);
		task->worker = worker;
		if (_get_task_data_bufs(task)) {
			fprintf(stderr, "Unable to get data bufs\n");
			goto error;
		}
		task++;
	}

	/* Register a poller that will stop the worker at time elapsed */
	worker->stop_poller = SPDK_POLLER_REGISTER(_worker_stop, worker,
			      g_time_in_sec * 1000000ULL);

	/* Load up queue depth worth of operations. */
	for (i = 0; i < g_queue_depth; i++) {
		task = _get_task(worker);
		worker->current_queue_depth++;
		if (task == NULL) {
			goto error;
		}

		_submit_single(worker, task);
	}
	return;
error:

	_free_task_buffers_in_pool(worker);
	free(worker->task_base);
	free(worker);
	spdk_app_stop(-1);
}

static void
accel_perf_start(void *arg1)
{
	struct spdk_cpuset tmp_cpumask = {};
	char thread_name[32];
	uint32_t i;
	int j;
	struct spdk_thread *thread;
	struct display_info *display;

	g_tsc_rate = spdk_get_ticks_hz();
	g_tsc_end = spdk_get_ticks() + g_time_in_sec * g_tsc_rate;

	printf("Running for %d seconds...\n", g_time_in_sec);
	fflush(stdout);

	/* Create worker threads for each core that was specified. */
	SPDK_ENV_FOREACH_CORE(i) {
		for (j = 0; j < g_threads_per_core; j++) {
			snprintf(thread_name, sizeof(thread_name), "ap_worker_%u_%u", i, j);
			spdk_cpuset_zero(&tmp_cpumask);
			spdk_cpuset_set_cpu(&tmp_cpumask, i, true);
			thread = spdk_thread_create(thread_name, &tmp_cpumask);
			display = calloc(1, sizeof(*display));
			if (display == NULL) {
				fprintf(stderr, "Unable to allocate memory\n");
				spdk_app_stop(-1);
				return;
			}
			display->core = i;
			display->thread = j;
			spdk_thread_send_msg(thread, _init_thread, display);
		}
	}
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	struct worker_thread *worker, *tmp;

	pthread_mutex_init(&g_workers_lock, NULL);
	spdk_app_opts_init(&opts, sizeof(opts));
	opts.reactor_mask = "0x1";
	if (spdk_app_parse_args(argc, argv, &opts, "a:C:o:q:t:yw:P:f:T:", NULL, parse_args,
				usage) != SPDK_APP_PARSE_ARGS_SUCCESS) {
		g_rc = -1;
		goto cleanup;
	}

	if ((g_workload_selection != ACCEL_COPY) &&
	    (g_workload_selection != ACCEL_FILL) &&
	    (g_workload_selection != ACCEL_CRC32C) &&
	    (g_workload_selection != ACCEL_COPY_CRC32C) &&
	    (g_workload_selection != ACCEL_COMPARE) &&
	    (g_workload_selection != ACCEL_DUALCAST)) {
		usage();
		g_rc = -1;
		goto cleanup;
	}

	if (g_allocate_depth > 0 && g_queue_depth > g_allocate_depth) {
		fprintf(stdout, "allocate depth must be at least as big as queue depth\n");
		usage();
		g_rc = -1;
		goto cleanup;
	}

	if (g_allocate_depth == 0) {
		g_allocate_depth = g_queue_depth;
	}

	if ((g_workload_selection == ACCEL_CRC32C || g_workload_selection == ACCEL_COPY_CRC32C) &&
	    g_crc32c_chained_count == 0) {
		usage();
		g_rc = -1;
		goto cleanup;
	}

	dump_user_config(&opts);
	g_rc = spdk_app_start(&opts, accel_perf_start, NULL);
	if (g_rc) {
		SPDK_ERRLOG("ERROR starting application\n");
	}

	pthread_mutex_destroy(&g_workers_lock);

	worker = g_workers;
	while (worker) {
		tmp = worker->next;
		free(worker);
		worker = tmp;
	}
cleanup:
	spdk_app_fini();
	return g_rc;
}
