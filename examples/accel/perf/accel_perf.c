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

#define DATA_PATTERN 0x5a
#define ALIGN_4K 0x1000

static uint64_t	g_tsc_rate;
static uint64_t g_tsc_us_rate;
static uint64_t g_tsc_end;
static int g_xfer_size_bytes = 4096;
static int g_queue_depth = 32;
static int g_time_in_sec = 5;
static uint32_t g_crc32c_seed = 0;
static int g_fail_percent_goal = 0;
static uint8_t g_fill_pattern = 255;
static bool g_verify = false;
static const char *g_workload_type = NULL;
static enum accel_capability g_workload_selection;
static struct worker_thread *g_workers = NULL;
static int g_num_workers = 0;
static pthread_mutex_t g_workers_lock = PTHREAD_MUTEX_INITIALIZER;

struct worker_thread {
	struct spdk_io_channel		*ch;
	uint64_t			xfer_completed;
	uint64_t			xfer_failed;
	uint64_t			injected_miscompares;
	uint64_t			current_queue_depth;
	struct spdk_mempool		*task_pool;
	struct worker_thread		*next;
	unsigned			core;
	struct spdk_thread		*thread;
	bool				is_draining;
	struct spdk_poller		*is_draining_poller;
	struct spdk_poller		*stop_poller;
};

struct ap_task {
	void			*src;
	void			*dst;
	void			*dst2;
	struct worker_thread	*worker;
	int			status;
	int			expected_status; /* used for compare */
};

inline static struct ap_task *
__ap_task_from_accel_task(struct spdk_accel_task *at)
{
	return (struct ap_task *)((uintptr_t)at - sizeof(struct ap_task));
}

inline static struct spdk_accel_task *
__accel_task_from_ap_task(struct ap_task *ap)
{
	return (struct spdk_accel_task *)((uintptr_t)ap + sizeof(struct ap_task));
}

static void
dump_user_config(struct spdk_app_opts *opts)
{
	printf("SPDK Configuration:\n");
	printf("Core mask:      %s\n\n", opts->reactor_mask);
	printf("Accel Perf Configuration:\n");
	printf("Workload Type:  %s\n", g_workload_type);
	if (g_workload_selection == ACCEL_CRC32C) {
		printf("CRC-32C seed:   %u\n", g_crc32c_seed);
	} else if (g_workload_selection == ACCEL_FILL) {
		printf("Fill pattern:   0x%x\n", g_fill_pattern);
	} else if ((g_workload_selection == ACCEL_COMPARE) && g_fail_percent_goal > 0) {
		printf("Failure inject: %u percent\n", g_fail_percent_goal);
	}
	printf("Transfer size:  %u bytes\n", g_xfer_size_bytes);
	printf("Queue depth:    %u\n", g_queue_depth);
	printf("Run time:       %u seconds\n", g_time_in_sec);
	printf("Verify:         %s\n\n", g_verify ? "Yes" : "No");
}

static void
usage(void)
{
	printf("accel_perf options:\n");
	printf("\t[-h help message]\n");
	printf("\t[-q queue depth]\n");
	printf("\t[-n number of channels]\n");
	printf("\t[-o transfer size in bytes]\n");
	printf("\t[-t time in seconds]\n");
	printf("\t[-w workload type must be one of these: copy, fill, crc32c, compare, dualcast\n");
	printf("\t[-s for crc32c workload, use this seed value (default 0)\n");
	printf("\t[-P for compare workload, percentage of operations that should miscompare (percent, default 0)\n");
	printf("\t[-f for fill workload, use this BYTE value (default 255)\n");
	printf("\t[-y verify result if this switch is on]\n");
}

static int
parse_args(int argc, char *argv)
{
	switch (argc) {
	case 'f':
		g_fill_pattern = (uint8_t)spdk_strtol(optarg, 10);
		break;
	case 'o':
		g_xfer_size_bytes = spdk_strtol(optarg, 10);
		break;
	case 'P':
		g_fail_percent_goal = spdk_strtol(optarg, 10);
		break;
	case 'q':
		g_queue_depth = spdk_strtol(optarg, 10);
		break;
	case 's':
		g_crc32c_seed = spdk_strtol(optarg, 10);
		break;
	case 't':
		g_time_in_sec = spdk_strtol(optarg, 10);
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

static void
unregister_worker(void *arg1)
{
	struct worker_thread *worker = arg1;

	spdk_mempool_free(worker->task_pool);
	spdk_put_io_channel(worker->ch);
	pthread_mutex_lock(&g_workers_lock);
	assert(g_num_workers >= 1);
	if (--g_num_workers == 0) {
		pthread_mutex_unlock(&g_workers_lock);
		spdk_app_stop(0);
	}
	pthread_mutex_unlock(&g_workers_lock);
}

static void accel_done(void *ref, int status);

static void
_submit_single(void *arg1, void *arg2)
{
	struct worker_thread *worker = arg1;
	struct ap_task *task = arg2;
	int random_num;
	int rc = 0;

	assert(worker);

	task->worker = worker;
	task->worker->current_queue_depth++;
	switch (g_workload_selection) {
	case ACCEL_COPY:
		rc = spdk_accel_submit_copy(__accel_task_from_ap_task(task),
					    worker->ch, task->dst,
					    task->src, g_xfer_size_bytes, accel_done);
		break;
	case ACCEL_FILL:
		/* For fill use the first byte of the task->dst buffer */
		rc = spdk_accel_submit_fill(__accel_task_from_ap_task(task),
					    worker->ch, task->dst, *(uint8_t *)task->src,
					    g_xfer_size_bytes, accel_done);
		break;
	case ACCEL_CRC32C:
		rc = spdk_accel_submit_crc32c(__accel_task_from_ap_task(task),
					      worker->ch, (uint32_t *)task->dst, task->src, g_crc32c_seed,
					      g_xfer_size_bytes, accel_done);
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
		rc = spdk_accel_submit_compare(__accel_task_from_ap_task(task),
					       worker->ch, task->dst, task->src,
					       g_xfer_size_bytes, accel_done);
		break;
	case ACCEL_DUALCAST:
		rc = spdk_accel_submit_dualcast(__accel_task_from_ap_task(task),
						worker->ch, task->dst, task->dst2,
						task->src, g_xfer_size_bytes, accel_done);
		break;
	default:
		assert(false);
		break;

	}

	if (rc) {
		accel_done(__accel_task_from_ap_task(task), rc);
	}
}

static void
_accel_done(void *arg1)
{
	struct ap_task *task = arg1;
	struct worker_thread *worker = task->worker;
	uint32_t sw_crc32c;

	assert(worker);
	assert(worker->current_queue_depth > 0);

	if (g_verify && task->status == 0) {
		switch (g_workload_selection) {
		case ACCEL_CRC32C:
			/* calculate sw CRC-32C and compare to sw aceel result. */
			sw_crc32c = spdk_crc32c_update(task->src, g_xfer_size_bytes, ~g_crc32c_seed);
			if (*(uint32_t *)task->dst != sw_crc32c) {
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
		default:
			assert(false);
			break;
		}
	}

	if (task->expected_status == -EILSEQ) {
		assert(task->status != 0);
		worker->injected_miscompares++;
	} else if (task->status) {
		/* Expected to pass but API reported error. */
		worker->xfer_failed++;
	}

	worker->xfer_completed++;
	worker->current_queue_depth--;

	if (!worker->is_draining) {
		_submit_single(worker, task);
	} else {
		spdk_free(task->src);
		spdk_free(task->dst);
		if (g_workload_selection == ACCEL_DUALCAST) {
			spdk_free(task->dst2);
		}
		spdk_mempool_put(worker->task_pool, task);
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

	printf("\nCore           Transfers     Bandwidth     Failed     Miscompares\n");
	printf("-----------------------------------------------------------------\n");
	while (worker != NULL) {

		uint64_t xfer_per_sec = worker->xfer_completed / g_time_in_sec;
		uint64_t bw_in_MiBps = (worker->xfer_completed * g_xfer_size_bytes) /
				       (g_time_in_sec * 1024 * 1024);

		total_completed += worker->xfer_completed;
		total_failed += worker->xfer_failed;
		total_miscompared += worker->injected_miscompares;

		if (xfer_per_sec) {
			printf("%10d%12" PRIu64 "/s%8" PRIu64 " MiB/s%11" PRIu64 " %11" PRIu64 "\n",
			       worker->core, xfer_per_sec,
			       bw_in_MiBps, worker->xfer_failed, worker->injected_miscompares);
		}

		worker = worker->next;
	}

	total_xfer_per_sec = total_completed / g_time_in_sec;
	total_bw_in_MiBps = (total_completed * g_xfer_size_bytes) /
			    (g_time_in_sec * 1024 * 1024);

	printf("==================================================================\n");
	printf("Total:%16" PRIu64 "/s%8" PRIu64 " MiB/s%11" PRIu64 " %11" PRIu64"\n\n",
	       total_xfer_per_sec, total_bw_in_MiBps, total_failed, total_miscompared);

	return total_failed ? 1 : 0;
}

static int
_check_draining(void *arg)
{
	struct worker_thread *worker = arg;

	assert(worker);

	if (worker->current_queue_depth == 0) {
		spdk_poller_unregister(&worker->is_draining_poller);
		unregister_worker(worker);
	}

	return -1;
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

	return 0;
}

static void
_init_thread_done(void *ctx)
{
}

static void
_init_thread(void *arg1)
{
	struct worker_thread *worker;
	char task_pool_name[30];
	struct ap_task *task;
	int i;
	uint32_t align = 0;

	worker = calloc(1, sizeof(*worker));
	if (worker == NULL) {
		fprintf(stderr, "Unable to allocate worker\n");
		return;
	}

	/* For dualcast, the DSA HW requires 4K alignment on destination addresses but
	 * we do this for all engines to keep it simple.
	 */
	if (g_workload_selection == ACCEL_DUALCAST) {
		align = ALIGN_4K;
	}

	worker->core = spdk_env_get_current_core();
	worker->thread = spdk_get_thread();
	worker->next = g_workers;
	worker->ch = spdk_accel_engine_get_io_channel();

	snprintf(task_pool_name, sizeof(task_pool_name), "task_pool_%d", g_num_workers);
	worker->task_pool = spdk_mempool_create(task_pool_name,
						g_queue_depth,
						spdk_accel_task_size() + sizeof(struct ap_task),
						SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
						SPDK_ENV_SOCKET_ID_ANY);
	if (!worker->task_pool) {
		fprintf(stderr, "Could not allocate buffer pool.\n");
		free(worker);
		return;
	}

	/* Register a poller that will stop the worker at time elapsed */
	worker->stop_poller = SPDK_POLLER_REGISTER(_worker_stop, worker,
			      g_time_in_sec * 1000000ULL);

	g_workers = worker;
	pthread_mutex_lock(&g_workers_lock);
	g_num_workers++;
	pthread_mutex_unlock(&g_workers_lock);

	for (i = 0; i < g_queue_depth; i++) {
		task = spdk_mempool_get(worker->task_pool);
		if (!task) {
			fprintf(stderr, "Unable to get accel_task\n");
			return;
		}

		task->src = spdk_dma_zmalloc(g_xfer_size_bytes, 0, NULL);
		if (task->src == NULL) {
			fprintf(stderr, "Unable to alloc src buffer\n");
			return;
		}
		memset(task->src, DATA_PATTERN, g_xfer_size_bytes);

		task->dst = spdk_dma_zmalloc(g_xfer_size_bytes, align, NULL);
		if (task->dst == NULL) {
			fprintf(stderr, "Unable to alloc dst buffer\n");
			return;
		}

		if (g_workload_selection == ACCEL_DUALCAST) {
			task->dst2 = spdk_dma_zmalloc(g_xfer_size_bytes, align, NULL);
			if (task->dst2 == NULL) {
				fprintf(stderr, "Unable to alloc dst buffer\n");
				return;
			}
			memset(task->dst2, ~DATA_PATTERN, g_xfer_size_bytes);
		}

		/* For compare we want the buffers to match, otherwise not. */
		if (g_workload_selection == ACCEL_COMPARE) {
			memset(task->dst, DATA_PATTERN, g_xfer_size_bytes);
		} else {
			memset(task->dst, ~DATA_PATTERN, g_xfer_size_bytes);
		}

		_submit_single(worker, task);
	}
}

static void
accel_done(void *ref, int status)
{
	struct ap_task *task = __ap_task_from_accel_task(ref);
	struct worker_thread *worker = task->worker;

	assert(worker);

	task->status = status;
	spdk_thread_send_msg(worker->thread, _accel_done, task);
}

static void
accel_perf_start(void *arg1)
{
	uint64_t capabilites;
	struct spdk_io_channel *accel_ch;

	accel_ch = spdk_accel_engine_get_io_channel();
	capabilites = spdk_accel_get_capabilities(accel_ch);
	spdk_put_io_channel(accel_ch);

	if ((capabilites & g_workload_selection) != g_workload_selection) {
		SPDK_ERRLOG("Selected workload is not supported by the current engine\n");
		SPDK_NOTICELOG("Software engine is selected by default, enable a HW engine via RPC\n\n");
		spdk_app_stop(-1);
		return;
	}

	g_tsc_rate = spdk_get_ticks_hz();
	g_tsc_us_rate = g_tsc_rate / (1000 * 1000);
	g_tsc_end = spdk_get_ticks() + g_time_in_sec * g_tsc_rate;

	printf("Running for %d seconds...\n", g_time_in_sec);
	fflush(stdout);

	spdk_for_each_thread(_init_thread, NULL, _init_thread_done);
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	struct worker_thread *worker, *tmp;
	int rc = 0;

	pthread_mutex_init(&g_workers_lock, NULL);
	spdk_app_opts_init(&opts);
	opts.reactor_mask = "0x1";
	if ((rc = spdk_app_parse_args(argc, argv, &opts, "o:q:t:yw:P:f:", NULL, parse_args,
				      usage)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
		rc = -1;
		goto cleanup;
	}

	if ((g_workload_selection != ACCEL_COPY) &&
	    (g_workload_selection != ACCEL_FILL) &&
	    (g_workload_selection != ACCEL_CRC32C) &&
	    (g_workload_selection != ACCEL_COMPARE) &&
	    (g_workload_selection != ACCEL_DUALCAST)) {
		usage();
		rc = -1;
		goto cleanup;
	}

	dump_user_config(&opts);
	rc = spdk_app_start(&opts, accel_perf_start, NULL);
	if (rc) {
		SPDK_ERRLOG("ERROR starting application\n");
	} else {
		dump_result();
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
	return rc;
}
