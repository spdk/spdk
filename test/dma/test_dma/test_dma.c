/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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
 *     * Neither the name of Nvidia Corporation nor the names of its
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

#include "spdk/dma.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include <infiniband/verbs.h>

struct dma_test_task;

struct dma_test_req {
	struct iovec iov;
	struct spdk_bdev_ext_io_opts io_opts;
	uint64_t submit_tsc;
	struct ibv_mr *mr;
	struct dma_test_task *task;
};

struct dma_test_task_stats {
	uint64_t io_completed;
	uint64_t total_tsc;
	uint64_t min_tsc;
	uint64_t max_tsc;
};

struct dma_test_task {
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *channel;
	uint64_t cur_io_offset;
	uint64_t max_offset_in_ios;
	uint64_t num_blocks_per_io;
	int rw_percentage;
	uint32_t seed;
	uint32_t io_inflight;
	struct dma_test_task_stats stats;
	struct dma_test_task_stats last_stats;
	bool is_draining;
	bool is_random;
	struct dma_test_req *reqs;
	struct spdk_thread *thread;
	const char *bdev_name;
	uint32_t lcore;

	TAILQ_ENTRY(dma_test_task) link;
};

TAILQ_HEAD(, dma_test_task) g_tasks = TAILQ_HEAD_INITIALIZER(g_tasks);

/* User's input */
static char *g_bdev_name;
static const char *g_rw_mode_str;
static int g_rw_percentage = -1;
static uint32_t g_queue_depth;
static uint32_t g_io_size;
static uint32_t g_run_time_sec;
static uint32_t g_run_count;
static bool g_is_random;

static struct spdk_thread *g_main_thread;
static struct spdk_poller *g_runtime_poller;
static struct spdk_memory_domain *g_domain;
static uint64_t g_num_blocks_per_io;
static uint32_t g_num_construct_tasks;
static uint32_t g_num_complete_tasks;
static uint64_t g_start_tsc;
static int g_run_rc;

static void destroy_tasks(void);
static int dma_test_submit_io(struct dma_test_req *req);

static void
print_total_stats(void)
{
	struct dma_test_task *task;
	uint64_t tsc_rate = spdk_get_ticks_hz();
	uint64_t test_time_usec = (spdk_get_ticks() - g_start_tsc) * SPDK_SEC_TO_USEC / tsc_rate;
	uint64_t total_tsc = 0, total_io_completed = 0;
	double task_iops, task_bw, task_min_lat, task_avg_lat, task_max_lat;
	double total_iops = 0, total_bw = 0, total_min_lat = (double)UINT64_MAX, total_max_lat = 0,
	       total_avg_lat;

	printf("==========================================================================\n");
	printf("%*s\n", 55, "Latency [us]");
	printf("%*s %10s %10s %10s %10s\n", 19, "IOPS", "MiB/s", "Average", "min", "max");

	TAILQ_FOREACH(task, &g_tasks, link) {
		if (!task->stats.io_completed) {
			continue;
		}
		task_iops = (double)task->stats.io_completed * SPDK_SEC_TO_USEC / test_time_usec;
		task_bw = task_iops * g_io_size / (1024 * 1024);
		task_avg_lat = (double)task->stats.total_tsc / task->stats.io_completed * SPDK_SEC_TO_USEC /
			       tsc_rate;
		task_min_lat = (double)task->stats.min_tsc * SPDK_SEC_TO_USEC / tsc_rate;
		task_max_lat = (double)task->stats.max_tsc * SPDK_SEC_TO_USEC / tsc_rate;

		total_iops += task_iops;
		total_bw += task_bw;
		total_io_completed += task->stats.io_completed;
		total_tsc += task->stats.total_tsc;
		if (task_min_lat < total_min_lat) {
			total_min_lat = task_min_lat;
		}
		if (task_max_lat > total_max_lat) {
			total_max_lat = task_max_lat;
		}
		printf("Core %2u: %10.2f %10.2f %10.2f %10.2f %10.2f\n",
		       task->lcore, task_iops, task_bw, task_avg_lat, task_min_lat, task_max_lat);
	}

	if (total_io_completed) {
		total_avg_lat = (double)total_tsc / total_io_completed  * SPDK_SEC_TO_USEC / tsc_rate;
		printf("==========================================================================\n");
		printf("%-*s %10.2f %10.2f %10.2f %10.2f %10.2f\n",
		       8, "Total  :", total_iops, total_bw, total_avg_lat, total_min_lat, total_max_lat);
		printf("\n");
	}
}

static void
print_periodic_stats(void)
{
	struct dma_test_task *task;
	uint64_t io_last_sec = 0, tsc_last_sec = 0;
	double lat_last_sec, bw_last_sec;

	TAILQ_FOREACH(task, &g_tasks, link) {
		io_last_sec += task->stats.io_completed - task->last_stats.io_completed;
		tsc_last_sec += task->stats.total_tsc - task->last_stats.total_tsc;
		memcpy(&task->last_stats, &task->stats, sizeof(task->stats));
	}

	printf("Running %3u/%-3u sec", g_run_count, g_run_time_sec);
	if (io_last_sec) {
		lat_last_sec =	(double)tsc_last_sec / io_last_sec * SPDK_SEC_TO_USEC / spdk_get_ticks_hz();
		bw_last_sec = (double)io_last_sec * g_io_size / (1024 * 1024);
		printf(" IOPS: %-8"PRIu64" BW: %-6.2f [MiB/s] avg.lat %-5.2f [us]",
		       io_last_sec, bw_last_sec, lat_last_sec);
	}

	printf("\r");
	fflush(stdout);
}

static void
dma_test_task_complete(void *ctx)
{
	assert(g_num_complete_tasks > 0);

	if (--g_num_complete_tasks == 0) {
		spdk_poller_unregister(&g_runtime_poller);
		print_total_stats();
		spdk_app_stop(g_run_rc);
	}
}

static inline void
dma_test_check_and_signal_task_done(struct dma_test_task *task)
{
	if (task->io_inflight == 0) {
		spdk_put_io_channel(task->channel);
		spdk_bdev_close(task->desc);
		spdk_thread_send_msg(g_main_thread, dma_test_task_complete, task);
	}
}

static inline void
dma_test_task_update_stats(struct dma_test_task *task, uint64_t submit_tsc)
{
	uint64_t tsc_diff = spdk_get_ticks() - submit_tsc;

	task->stats.io_completed++;
	task->stats.total_tsc += tsc_diff;
	if (spdk_unlikely(tsc_diff < task->stats.min_tsc)) {
		task->stats.min_tsc = tsc_diff;
	}
	if (spdk_unlikely(tsc_diff > task->stats.max_tsc)) {
		task->stats.max_tsc = tsc_diff;
	}
}

static void
dma_test_bdev_io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct dma_test_req *req = cb_arg;
	struct dma_test_task *task = req->task;

	assert(task->io_inflight > 0);
	--task->io_inflight;
	dma_test_task_update_stats(task, req->submit_tsc);

	if (!success) {
		if (!g_run_rc) {
			fprintf(stderr, "IO completed with error\n");
			g_run_rc = -1;
		}
		task->is_draining = true;
	}

	spdk_bdev_free_io(bdev_io);

	if (spdk_unlikely(task->is_draining)) {
		dma_test_check_and_signal_task_done(task);
		return;
	}

	dma_test_submit_io(req);
}

static inline uint64_t
dma_test_get_offset_in_ios(struct dma_test_task *task)
{
	uint64_t offset;

	if (task->is_random) {
		offset = rand_r(&task->seed) % task->max_offset_in_ios;
	} else {
		offset = task->cur_io_offset++;
		if (spdk_unlikely(task->cur_io_offset == task->max_offset_in_ios)) {
			task->cur_io_offset = 0;
		}
	}

	return offset;
}

static inline bool
dma_test_task_is_read(struct dma_test_task *task)
{
	if (task->rw_percentage == 100) {
		return true;
	}
	if (task->rw_percentage != 0 && (rand_r(&task->seed) % 100) <  task->rw_percentage) {
		return true;
	}
	return false;
}

static int
dma_test_translate_memory_cb(struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			     struct spdk_memory_domain *dst_domain, struct spdk_memory_domain_translation_ctx *dst_domain_ctx,
			     void *addr, size_t len, struct spdk_memory_domain_translation_result *result)
{
	struct dma_test_req *req = src_domain_ctx;
	struct ibv_qp *dst_domain_qp = (struct ibv_qp *)dst_domain_ctx->rdma.ibv_qp;

	if (spdk_unlikely(!req->mr)) {
		req->mr = ibv_reg_mr(dst_domain_qp->pd, addr, len, IBV_ACCESS_LOCAL_WRITE |
				     IBV_ACCESS_REMOTE_READ |
				     IBV_ACCESS_REMOTE_WRITE);
		if (!req->mr) {
			fprintf(stderr, "Failed to register memory region, errno %d\n", errno);
			return -1;
		}
	}

	result->iov.iov_base = addr;
	result->iov.iov_len = len;
	result->iov_count = 1;
	result->rdma.lkey = req->mr->lkey;
	result->rdma.rkey = req->mr->rkey;
	result->dst_domain = dst_domain;

	return 0;
}

static int
dma_test_submit_io(struct dma_test_req *req)
{
	struct dma_test_task *task = req->task;
	uint64_t offset_in_ios;
	int rc;
	bool is_read;

	offset_in_ios = dma_test_get_offset_in_ios(task);
	is_read = dma_test_task_is_read(task);
	req->submit_tsc = spdk_get_ticks();
	if (is_read) {
		rc = spdk_bdev_readv_blocks_ext(task->desc, task->channel, &req->iov, 1,
						offset_in_ios * task->num_blocks_per_io, task->num_blocks_per_io,
						dma_test_bdev_io_completion_cb, req, &req->io_opts);
	} else {
		rc = spdk_bdev_writev_blocks_ext(task->desc, task->channel, &req->iov, 1,
						 offset_in_ios * task->num_blocks_per_io, task->num_blocks_per_io,
						 dma_test_bdev_io_completion_cb, req, &req->io_opts);
	}

	if (spdk_unlikely(rc)) {
		if (!g_run_rc) {
			/* log an error only once */
			fprintf(stderr, "Failed to submit %s IO, rc %d, stop sending IO\n", is_read ? "read" : "write", rc);
			g_run_rc = rc;
		}
		task->is_draining = true;
		dma_test_check_and_signal_task_done(task);
		return rc;
	}

	task->io_inflight++;

	return 0;
}

static void
dma_test_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	struct dma_test_task *task = event_ctx;

	if (type == SPDK_BDEV_EVENT_REMOVE) {
		task->is_draining = true;
	}
}

static void
dma_test_bdev_dummy_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			     void *event_ctx)
{
}

static void dma_test_task_run(void *ctx)
{
	struct dma_test_task *task = ctx;
	uint32_t i;
	int rc = 0;

	for (i = 0; i < g_queue_depth && rc == 0; i++) {
		rc = dma_test_submit_io(&task->reqs[i]);
	}
}

static void
dma_test_drain_task(void *ctx)
{
	struct dma_test_task *task = ctx;

	task->is_draining = true;
}

static void
dma_test_shutdown_cb(void)
{
	struct dma_test_task *task;

	spdk_poller_unregister(&g_runtime_poller);

	TAILQ_FOREACH(task, &g_tasks, link) {
		spdk_thread_send_msg(task->thread, dma_test_drain_task, task);
	}
}

static int
dma_test_run_time_poller(void *ctx)
{
	g_run_count++;

	if (g_run_count < g_run_time_sec) {
		if (isatty(STDOUT_FILENO)) {
			print_periodic_stats();
		}
	} else {
		dma_test_shutdown_cb();
	}

	return SPDK_POLLER_BUSY;
}

static void
dma_test_construct_task_done(void *ctx)
{
	struct dma_test_task *task;

	assert(g_num_construct_tasks > 0);
	--g_num_construct_tasks;

	if (g_num_construct_tasks != 0) {
		return;
	}

	if (g_run_rc) {
		fprintf(stderr, "Initialization failed with error %d\n", g_run_rc);
		spdk_app_stop(g_run_rc);
		return;
	}

	g_runtime_poller = spdk_poller_register_named(dma_test_run_time_poller, NULL, 1 * 1000 * 1000,
			   "dma_test_run_time_poller");
	if (!g_runtime_poller) {
		fprintf(stderr, "Failed to run timer\n");
		spdk_app_stop(-1);
		return;
	}

	printf("Initialization complete, running %s IO for %u sec on %u cores\n", g_rw_mode_str,
	       g_run_time_sec, spdk_env_get_core_count());
	g_start_tsc = spdk_get_ticks();
	TAILQ_FOREACH(task, &g_tasks, link) {
		spdk_thread_send_msg(task->thread, dma_test_task_run, task);
	}
}

static void
dma_test_construct_task_on_thread(void *ctx)
{
	struct dma_test_task *task = ctx;
	int rc;

	rc = spdk_bdev_open_ext(task->bdev_name, true, dma_test_bdev_event_cb, task, &task->desc);
	if (rc) {
		fprintf(stderr, "Failed to open bdev %s, rc %d\n", task->bdev_name, rc);
		g_run_rc = rc;
		spdk_thread_send_msg(g_main_thread, dma_test_construct_task_done, NULL);
		return;
	}

	task->channel = spdk_bdev_get_io_channel(task->desc);
	if (!task->channel) {
		spdk_bdev_close(task->desc);
		task->desc = NULL;
		fprintf(stderr, "Failed to open bdev %s, rc %d\n", task->bdev_name, rc);
		g_run_rc = rc;
		spdk_thread_send_msg(g_main_thread, dma_test_construct_task_done, NULL);
		return;
	}

	task->max_offset_in_ios = spdk_bdev_get_num_blocks(spdk_bdev_desc_get_bdev(
					  task->desc)) / task->num_blocks_per_io;

	spdk_thread_send_msg(g_main_thread, dma_test_construct_task_done, task);
}

static bool
dma_test_check_bdev_supports_rdma_memory_domain(struct spdk_bdev *bdev)
{
	struct spdk_memory_domain **bdev_domains;
	int bdev_domains_count, bdev_domains_count_tmp, i;
	bool rdma_domain_supported = false;

	bdev_domains_count = spdk_bdev_get_memory_domains(bdev, NULL, 0);

	if (bdev_domains_count < 0) {
		fprintf(stderr, "Failed to get bdev memory domains count, rc %d\n", bdev_domains_count);
		return false;
	} else if (bdev_domains_count == 0) {
		fprintf(stderr, "bdev %s doesn't support any memory domains\n", spdk_bdev_get_name(bdev));
		return false;
	}

	fprintf(stdout, "bdev %s reports %d memory domains\n", spdk_bdev_get_name(bdev),
		bdev_domains_count);

	bdev_domains = calloc((size_t)bdev_domains_count, sizeof(*bdev_domains));
	if (!bdev_domains) {
		fprintf(stderr, "Failed to allocate memory domains\n");
		return false;
	}

	bdev_domains_count_tmp = spdk_bdev_get_memory_domains(bdev, bdev_domains, bdev_domains_count);
	if (bdev_domains_count_tmp != bdev_domains_count) {
		fprintf(stderr, "Unexpected bdev domains return value %d\n", bdev_domains_count_tmp);
		return false;
	}

	for (i = 0; i < bdev_domains_count; i++) {
		if (spdk_memory_domain_get_dma_device_type(bdev_domains[i]) == SPDK_DMA_DEVICE_TYPE_RDMA) {
			/* Bdev supports memory domain of RDMA type, we can try to submit IO request to it using
			 * bdev ext API */
			rdma_domain_supported = true;
			break;
		}
	}

	fprintf(stdout, "bdev %s %s RDMA memory domain\n", spdk_bdev_get_name(bdev),
		rdma_domain_supported ? "supports" : "doesn't support");
	free(bdev_domains);

	return rdma_domain_supported;
}

static int
allocate_task(uint32_t core, const char *bdev_name)
{
	char thread_name[32];
	struct spdk_cpuset cpu_set;
	uint32_t i;
	struct dma_test_task *task;
	struct dma_test_req *req;

	task = calloc(1, sizeof(*task));
	if (!task) {
		fprintf(stderr, "Failed to allocate per thread task\n");
		return -ENOMEM;
	}

	TAILQ_INSERT_TAIL(&g_tasks, task, link);

	task->reqs = calloc(g_queue_depth, sizeof(*task->reqs));
	if (!task->reqs) {
		fprintf(stderr, "Failed to allocate requests\n");
		return -ENOMEM;
	}

	for (i = 0; i < g_queue_depth; i++) {
		req = &task->reqs[i];
		req->task = task;
		req->iov.iov_len = g_io_size;
		req->iov.iov_base = malloc(req->iov.iov_len);
		if (!req->iov.iov_base) {
			fprintf(stderr, "Failed to allocate request data buffer\n");
			return -ENOMEM;
		}
		memset(req->iov.iov_base, 0xc, req->iov.iov_len);
		req->io_opts.size = sizeof(req->io_opts);
		req->io_opts.memory_domain = g_domain;
		req->io_opts.memory_domain_ctx = req;
	}

	snprintf(thread_name, 32, "task_%u", core);
	spdk_cpuset_zero(&cpu_set);
	spdk_cpuset_set_cpu(&cpu_set, core, true);
	task->thread = spdk_thread_create(thread_name, &cpu_set);
	if (!task->thread) {
		fprintf(stderr, "Failed to create SPDK thread, core %u, cpu_mask %s\n", core,
			spdk_cpuset_fmt(&cpu_set));
		return -ENOMEM;
	}

	task->seed = core;
	task->lcore = core;
	task->bdev_name = bdev_name;
	task->is_random = g_is_random;
	task->rw_percentage = g_rw_percentage;
	task->num_blocks_per_io = g_num_blocks_per_io;
	task->stats.min_tsc = UINT64_MAX;

	return 0;
}

static void
destroy_task(struct dma_test_task *task)
{
	struct dma_test_req *req;
	uint32_t i;

	for (i = 0; i < g_queue_depth; i++) {
		req = &task->reqs[i];
		if (req->mr) {
			ibv_dereg_mr(req->mr);
		}
		free(req->iov.iov_base);
	}
	free(task->reqs);
	TAILQ_REMOVE(&g_tasks, task, link);
	free(task);
}

static void
destroy_tasks(void)
{
	struct dma_test_task *task, *tmp_task;

	TAILQ_FOREACH_SAFE(task, &g_tasks, link, tmp_task) {
		destroy_task(task);
	}
}

static void
dma_test_start(void *arg)
{
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	struct dma_test_task *task;
	uint32_t block_size, i;
	int rc;

	rc = spdk_bdev_open_ext(g_bdev_name, true, dma_test_bdev_dummy_event_cb, NULL, &desc);
	if (rc) {
		fprintf(stderr, "Can't find bdev %s\n", g_bdev_name);
		spdk_app_stop(-ENODEV);
		return;
	}
	bdev = spdk_bdev_desc_get_bdev(desc);
	if (!dma_test_check_bdev_supports_rdma_memory_domain(bdev)) {
		spdk_bdev_close(desc);
		spdk_app_stop(-ENODEV);
		return;
	}

	g_main_thread = spdk_get_thread();

	block_size = spdk_bdev_get_block_size(bdev);
	if (g_io_size < block_size || g_io_size % block_size != 0) {
		fprintf(stderr, "Invalid io_size %u requested, bdev block size %u\n", g_io_size, block_size);
		spdk_bdev_close(desc);
		spdk_app_stop(-EINVAL);
		return;
	}
	g_num_blocks_per_io = g_io_size / block_size;

	/* Create a memory domain to represent the source memory domain.
	 * Since we don't actually have a remote memory domain in this test, this will describe memory
	 * on the local system and the translation to the destination memory domain will be trivial.
	 * But this at least allows us to demonstrate the flow and test the functionality. */
	rc = spdk_memory_domain_create(&g_domain, SPDK_DMA_DEVICE_TYPE_RDMA, NULL, "test_dma");
	if (rc != 0) {
		spdk_bdev_close(desc);
		spdk_app_stop(rc);
		return;
	}
	spdk_memory_domain_set_translation(g_domain, dma_test_translate_memory_cb);

	SPDK_ENV_FOREACH_CORE(i) {
		rc = allocate_task(i, g_bdev_name);
		if (rc) {
			destroy_tasks();
			spdk_bdev_close(desc);
			spdk_app_stop(rc);
			return;
		}
		g_num_construct_tasks++;
		g_num_complete_tasks++;
	}

	TAILQ_FOREACH(task, &g_tasks, link) {
		spdk_thread_send_msg(task->thread, dma_test_construct_task_on_thread, task);
	}

	spdk_bdev_close(desc);
}

static void
print_usage(void)
{
	printf(" -b <bdev>         bdev name for test\n");
	printf(" -q <val>          io depth\n");
	printf(" -o <val>          io size in bytes\n");
	printf(" -t <val>          run time in seconds\n");
	printf(" -w <str>          io pattern (read, write, randread, randwrite, randrw)\n");
	printf(" -M <0-100>        rw percentage (100 for reads, 0 for writes)\n");
}

static int
parse_arg(int ch, char *arg)
{
	long tmp;

	switch (ch) {
	case 'q':
	case 'o':
	case 't':
	case 'M':
		tmp = spdk_strtol(arg, 10);
		if (tmp < 0) {
			fprintf(stderr, "Invalid option %c value %s\n", ch, arg);
			return 1;
		}

		switch (ch) {
		case 'q':
			g_queue_depth = (uint32_t) tmp;
			break;
		case 'o':
			g_io_size = (uint32_t) tmp;
			break;
		case 't':
			g_run_time_sec = (uint32_t) tmp;
			break;
		case 'M':
			g_rw_percentage = (uint32_t) tmp;
			break;
		}
		break;
	case 'w':
		g_rw_mode_str = arg;
		break;
	case 'b':
		g_bdev_name = arg;
		break;

	default:
		fprintf(stderr, "Unknown option %c\n", ch);
		return 1;
	}

	return 0;
}

static int
verify_args(void)
{
	const char *rw_mode = g_rw_mode_str;

	if (g_queue_depth == 0) {
		fprintf(stderr, "queue depth (-q) is not set\n");
		return 1;
	}
	if (g_io_size == 0) {
		fprintf(stderr, "io size (-o) is not set\n");
		return 1;
	}
	if (g_run_time_sec == 0) {
		fprintf(stderr, "test run time (-t) is not set\n");
		return 1;
	}
	if (!rw_mode) {
		fprintf(stderr, "io pattern (-w) is not set\n");
		return 1;
	}
	if (strncmp(rw_mode, "rand", 4) == 0) {
		g_is_random = true;
		rw_mode = &rw_mode[4];
	}
	if (strcmp(rw_mode, "read") == 0 || strcmp(rw_mode, "write") == 0) {
		if (g_rw_percentage > 0) {
			fprintf(stderr, "Ignoring -M option\n");
		}
		g_rw_percentage = strcmp(rw_mode, "read") == 0 ? 100 : 0;
	} else if (strcmp(rw_mode, "rw") == 0) {
		if (g_rw_percentage < 0 || g_rw_percentage > 100) {
			fprintf(stderr, "Invalid -M value (%d) must be 0..100\n", g_rw_percentage);
			return 1;
		}
	} else {
		fprintf(stderr, "io pattern (-w) one of [read, write, randread, randwrite, rw, randrw]\n");
		return 1;
	}
	if (!g_bdev_name) {
		fprintf(stderr, "bdev name (-b) is not set\n");
		return 1;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "test_dma";
	opts.shutdown_cb = dma_test_shutdown_cb;

	rc = spdk_app_parse_args(argc, argv, &opts, "b:q:o:t:w:M:", NULL, parse_arg, print_usage);
	if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	rc = verify_args();
	if (rc) {
		exit(rc);
	}

	rc = spdk_app_start(&opts, dma_test_start, NULL);
	destroy_tasks();
	spdk_app_fini();

	return rc;
}
