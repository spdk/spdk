/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/accel.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/bit_array.h"
#include "spdk/conf.h"
#include "spdk/zipf.h"
#include "spdk/histogram_data.h"

#define BDEVPERF_CONFIG_MAX_FILENAME 1024
#define BDEVPERF_CONFIG_UNDEFINED -1
#define BDEVPERF_CONFIG_ERROR -2

struct bdevperf_task {
	struct iovec			iov;
	struct bdevperf_job		*job;
	struct spdk_bdev_io		*bdev_io;
	void				*buf;
	void				*md_buf;
	uint64_t			offset_blocks;
	struct bdevperf_task		*task_to_abort;
	enum spdk_bdev_io_type		io_type;
	TAILQ_ENTRY(bdevperf_task)	link;
	struct spdk_bdev_io_wait_entry	bdev_io_wait;
};

static const char *g_workload_type = NULL;
static int g_io_size = 0;
/* initialize to invalid value so we can detect if user overrides it. */
static int g_rw_percentage = -1;
static bool g_verify = false;
static bool g_reset = false;
static bool g_continue_on_failure = false;
static bool g_abort = false;
static bool g_error_to_exit = false;
static int g_queue_depth = 0;
static uint64_t g_time_in_usec;
static int g_show_performance_real_time = 0;
static uint64_t g_show_performance_period_in_usec = SPDK_SEC_TO_USEC;
static uint64_t g_show_performance_period_num = 0;
static uint64_t g_show_performance_ema_period = 0;
static int g_run_rc = 0;
static bool g_shutdown = false;
static uint64_t g_start_tsc;
static uint64_t g_shutdown_tsc;
static bool g_zcopy = false;
static struct spdk_thread *g_main_thread;
static int g_time_in_sec = 0;
static bool g_mix_specified = false;
static const char *g_job_bdev_name;
static bool g_wait_for_tests = false;
static struct spdk_jsonrpc_request *g_request = NULL;
static bool g_multithread_mode = false;
static int g_timeout_in_sec;
static struct spdk_conf *g_bdevperf_conf = NULL;
static const char *g_bdevperf_conf_file = NULL;
static double g_zipf_theta;

static struct spdk_cpuset g_all_cpuset;
static struct spdk_poller *g_perf_timer = NULL;

static void bdevperf_submit_single(struct bdevperf_job *job, struct bdevperf_task *task);
static void rpc_perform_tests_cb(void);

static uint32_t g_bdev_count = 0;
static uint32_t g_latency_display_level;

static const double g_latency_cutoffs[] = {
	0.01,
	0.10,
	0.25,
	0.50,
	0.75,
	0.90,
	0.95,
	0.98,
	0.99,
	0.995,
	0.999,
	0.9999,
	0.99999,
	0.999999,
	0.9999999,
	-1,
};

struct latency_info {
	uint64_t	min;
	uint64_t	max;
	uint64_t	total;
};

struct bdevperf_job {
	char				*name;
	struct spdk_bdev		*bdev;
	struct spdk_bdev_desc		*bdev_desc;
	struct spdk_io_channel		*ch;
	TAILQ_ENTRY(bdevperf_job)	link;
	struct spdk_thread		*thread;

	const char			*workload_type;
	int				io_size;
	int				rw_percentage;
	bool				is_random;
	bool				verify;
	bool				reset;
	bool				continue_on_failure;
	bool				unmap;
	bool				write_zeroes;
	bool				flush;
	bool				abort;
	int				queue_depth;
	unsigned int			seed;

	uint64_t			io_completed;
	uint64_t			io_failed;
	uint64_t			io_timeout;
	uint64_t			prev_io_completed;
	double				ema_io_per_second;
	int				current_queue_depth;
	uint64_t			size_in_ios;
	uint64_t			ios_base;
	uint64_t			offset_in_ios;
	uint64_t			io_size_blocks;
	uint64_t			buf_size;
	uint32_t			dif_check_flags;
	bool				is_draining;
	struct spdk_poller		*run_timer;
	struct spdk_poller		*reset_timer;
	struct spdk_bit_array		*outstanding;
	struct spdk_zipf		*zipf;
	TAILQ_HEAD(, bdevperf_task)	task_list;
	uint64_t			run_time_in_usec;

	/* keep channel's histogram data before being destroyed */
	struct spdk_histogram_data	*histogram;
};

struct spdk_bdevperf {
	TAILQ_HEAD(, bdevperf_job)	jobs;
	uint32_t			running_jobs;
};

static struct spdk_bdevperf g_bdevperf = {
	.jobs = TAILQ_HEAD_INITIALIZER(g_bdevperf.jobs),
	.running_jobs = 0,
};

enum job_config_rw {
	JOB_CONFIG_RW_READ = 0,
	JOB_CONFIG_RW_WRITE,
	JOB_CONFIG_RW_RANDREAD,
	JOB_CONFIG_RW_RANDWRITE,
	JOB_CONFIG_RW_RW,
	JOB_CONFIG_RW_RANDRW,
	JOB_CONFIG_RW_VERIFY,
	JOB_CONFIG_RW_RESET,
	JOB_CONFIG_RW_UNMAP,
	JOB_CONFIG_RW_FLUSH,
	JOB_CONFIG_RW_WRITE_ZEROES,
};

/* Storing values from a section of job config file */
struct job_config {
	const char			*name;
	const char			*filename;
	struct spdk_cpuset		cpumask;
	int				bs;
	int				iodepth;
	int				rwmixread;
	int64_t				offset;
	uint64_t			length;
	enum job_config_rw		rw;
	TAILQ_ENTRY(job_config)	link;
};

TAILQ_HEAD(, job_config) job_config_list
	= TAILQ_HEAD_INITIALIZER(job_config_list);

static bool g_performance_dump_active = false;

struct bdevperf_aggregate_stats {
	struct bdevperf_job		*current_job;
	uint64_t			io_time_in_usec;
	uint64_t			ema_period;
	double				total_io_per_second;
	double				total_mb_per_second;
	double				total_failed_per_second;
	double				total_timeout_per_second;
	double				min_latency;
	double				max_latency;
	uint64_t			total_io_completed;
	uint64_t			total_tsc;
};

static struct bdevperf_aggregate_stats g_stats = {.min_latency = (double)UINT64_MAX};

/*
 * Cumulative Moving Average (CMA): average of all data up to current
 * Exponential Moving Average (EMA): weighted mean of the previous n data and more weight is given to recent
 * Simple Moving Average (SMA): unweighted mean of the previous n data
 *
 * Bdevperf supports CMA and EMA.
 */
static double
get_cma_io_per_second(struct bdevperf_job *job, uint64_t io_time_in_usec)
{
	return (double)job->io_completed * SPDK_SEC_TO_USEC / io_time_in_usec;
}

static double
get_ema_io_per_second(struct bdevperf_job *job, uint64_t ema_period)
{
	double io_completed, io_per_second;

	io_completed = job->io_completed;
	io_per_second = (double)(io_completed - job->prev_io_completed) * SPDK_SEC_TO_USEC
			/ g_show_performance_period_in_usec;
	job->prev_io_completed = io_completed;

	job->ema_io_per_second += (io_per_second - job->ema_io_per_second) * 2
				  / (ema_period + 1);
	return job->ema_io_per_second;
}

static void
get_avg_latency(void *ctx, uint64_t start, uint64_t end, uint64_t count,
		uint64_t total, uint64_t so_far)
{
	struct latency_info *latency_info = ctx;

	if (count == 0) {
		return;
	}

	latency_info->total += (start + end) / 2 * count;

	if (so_far == count) {
		latency_info->min = start;
	}

	if (so_far == total) {
		latency_info->max = end;
	}
}

static void
performance_dump_job(struct bdevperf_aggregate_stats *stats, struct bdevperf_job *job)
{
	double io_per_second, mb_per_second, failed_per_second, timeout_per_second;
	double average_latency = 0.0, min_latency, max_latency;
	uint64_t time_in_usec;
	uint64_t tsc_rate;
	uint64_t total_io;
	struct latency_info latency_info = {};

	printf("\r Job: %s (Core Mask 0x%s)\n", spdk_thread_get_name(job->thread),
	       spdk_cpuset_fmt(spdk_thread_get_cpumask(job->thread)));

	if (job->io_failed > 0 && !job->reset && !job->continue_on_failure) {
		printf("\r Job: %s ended in about %.2f seconds with error\n",
		       spdk_thread_get_name(job->thread), (double)job->run_time_in_usec / SPDK_SEC_TO_USEC);
	}
	if (job->verify) {
		printf("\t Verification LBA range: start 0x%" PRIx64 " length 0x%" PRIx64 "\n",
		       job->ios_base, job->size_in_ios);
	}

	if (g_performance_dump_active == true) {
		/* Use job's actual run time as Job has ended */
		if (job->io_failed > 0 && !job->continue_on_failure) {
			time_in_usec = job->run_time_in_usec;
		} else {
			time_in_usec = stats->io_time_in_usec;
		}
	} else {
		time_in_usec = job->run_time_in_usec;
	}

	if (stats->ema_period == 0) {
		io_per_second = get_cma_io_per_second(job, time_in_usec);
	} else {
		io_per_second = get_ema_io_per_second(job, stats->ema_period);
	}

	tsc_rate = spdk_get_ticks_hz();
	mb_per_second = io_per_second * job->io_size / (1024 * 1024);

	spdk_histogram_data_iterate(job->histogram, get_avg_latency, &latency_info);

	total_io = job->io_completed + job->io_failed;
	if (total_io != 0) {
		average_latency = (double)latency_info.total / total_io * SPDK_SEC_TO_USEC / tsc_rate;
	}
	min_latency = (double)latency_info.min * SPDK_SEC_TO_USEC / tsc_rate;
	max_latency = (double)latency_info.max * SPDK_SEC_TO_USEC / tsc_rate;

	failed_per_second = (double)job->io_failed * SPDK_SEC_TO_USEC / time_in_usec;
	timeout_per_second = (double)job->io_timeout * SPDK_SEC_TO_USEC / time_in_usec;

	printf("\t %-20s: %10.2f %10.2f %10.2f",
	       job->name, (float)time_in_usec / SPDK_SEC_TO_USEC, io_per_second, mb_per_second);
	printf(" %10.2f %8.2f",
	       failed_per_second, timeout_per_second);
	printf(" %10.2f %10.2f %10.2f\n",
	       average_latency, min_latency, max_latency);

	stats->total_io_per_second += io_per_second;
	stats->total_mb_per_second += mb_per_second;
	stats->total_failed_per_second += failed_per_second;
	stats->total_timeout_per_second += timeout_per_second;
	stats->total_io_completed += job->io_completed + job->io_failed;
	stats->total_tsc += latency_info.total;
	if (min_latency < stats->min_latency) {
		stats->min_latency = min_latency;
	}
	if (max_latency > stats->max_latency) {
		stats->max_latency = max_latency;
	}
}

static void
generate_data(void *buf, int buf_len, int block_size, void *md_buf, int md_size,
	      int num_blocks)
{
	int offset_blocks = 0, md_offset, data_block_size, inner_offset;

	if (buf_len < num_blocks * block_size) {
		return;
	}

	if (md_buf == NULL) {
		data_block_size = block_size - md_size;
		md_buf = (char *)buf + data_block_size;
		md_offset = block_size;
	} else {
		data_block_size = block_size;
		md_offset = md_size;
	}

	while (offset_blocks < num_blocks) {
		inner_offset = 0;
		while (inner_offset < data_block_size) {
			*(uint32_t *)buf = offset_blocks + inner_offset;
			inner_offset += sizeof(uint32_t);
			buf += sizeof(uint32_t);
		}
		memset(md_buf, offset_blocks, md_size);
		md_buf += md_offset;
		offset_blocks++;
	}
}

static bool
copy_data(void *wr_buf, int wr_buf_len, void *rd_buf, int rd_buf_len, int block_size,
	  void *wr_md_buf, void *rd_md_buf, int md_size, int num_blocks)
{
	if (wr_buf_len < num_blocks * block_size || rd_buf_len < num_blocks * block_size) {
		return false;
	}

	assert((wr_md_buf != NULL) == (rd_md_buf != NULL));

	memcpy(wr_buf, rd_buf, block_size * num_blocks);

	if (wr_md_buf != NULL) {
		memcpy(wr_md_buf, rd_md_buf, md_size * num_blocks);
	}

	return true;
}

static bool
verify_data(void *wr_buf, int wr_buf_len, void *rd_buf, int rd_buf_len, int block_size,
	    void *wr_md_buf, void *rd_md_buf, int md_size, int num_blocks, bool md_check)
{
	int offset_blocks = 0, md_offset, data_block_size;

	if (wr_buf_len < num_blocks * block_size || rd_buf_len < num_blocks * block_size) {
		return false;
	}

	assert((wr_md_buf != NULL) == (rd_md_buf != NULL));

	if (wr_md_buf == NULL) {
		data_block_size = block_size - md_size;
		wr_md_buf = (char *)wr_buf + data_block_size;
		rd_md_buf = (char *)rd_buf + data_block_size;
		md_offset = block_size;
	} else {
		data_block_size = block_size;
		md_offset = md_size;
	}

	while (offset_blocks < num_blocks) {
		if (memcmp(wr_buf, rd_buf, data_block_size) != 0) {
			return false;
		}

		wr_buf += block_size;
		rd_buf += block_size;

		if (md_check) {
			if (memcmp(wr_md_buf, rd_md_buf, md_size) != 0) {
				return false;
			}

			wr_md_buf += md_offset;
			rd_md_buf += md_offset;
		}

		offset_blocks++;
	}

	return true;
}

static void
free_job_config(void)
{
	struct job_config *config, *tmp;

	spdk_conf_free(g_bdevperf_conf);
	g_bdevperf_conf = NULL;

	TAILQ_FOREACH_SAFE(config, &job_config_list, link, tmp) {
		TAILQ_REMOVE(&job_config_list, config, link);
		free(config);
	}
}

static void
bdevperf_job_free(struct bdevperf_job *job)
{
	spdk_histogram_data_free(job->histogram);
	spdk_bit_array_free(&job->outstanding);
	spdk_zipf_free(&job->zipf);
	free(job->name);
	free(job);
}

static void
job_thread_exit(void *ctx)
{
	spdk_thread_exit(spdk_get_thread());
}

static void
check_cutoff(void *ctx, uint64_t start, uint64_t end, uint64_t count,
	     uint64_t total, uint64_t so_far)
{
	double so_far_pct;
	double **cutoff = ctx;
	uint64_t tsc_rate;

	if (count == 0) {
		return;
	}

	tsc_rate = spdk_get_ticks_hz();
	so_far_pct = (double)so_far / total;
	while (so_far_pct >= **cutoff && **cutoff > 0) {
		printf("%9.5f%% : %9.3fus\n", **cutoff * 100, (double)end * SPDK_SEC_TO_USEC / tsc_rate);
		(*cutoff)++;
	}
}

static void
print_bucket(void *ctx, uint64_t start, uint64_t end, uint64_t count,
	     uint64_t total, uint64_t so_far)
{
	double so_far_pct;
	uint64_t tsc_rate;

	if (count == 0) {
		return;
	}

	tsc_rate = spdk_get_ticks_hz();
	so_far_pct = (double)so_far * 100 / total;
	printf("%9.3f - %9.3f: %9.4f%%  (%9ju)\n",
	       (double)start * SPDK_SEC_TO_USEC / tsc_rate,
	       (double)end * SPDK_SEC_TO_USEC / tsc_rate,
	       so_far_pct, count);
}

static void
bdevperf_test_done(void *ctx)
{
	struct bdevperf_job *job, *jtmp;
	struct bdevperf_task *task, *ttmp;
	double average_latency = 0.0;
	uint64_t time_in_usec;
	int rc;

	if (g_time_in_usec) {
		g_stats.io_time_in_usec = g_time_in_usec;

		if (!g_run_rc && g_performance_dump_active) {
			spdk_thread_send_msg(spdk_get_thread(), bdevperf_test_done, NULL);
			return;
		}
	}

	if (g_show_performance_real_time) {
		spdk_poller_unregister(&g_perf_timer);
	}

	if (g_shutdown) {
		g_shutdown_tsc = spdk_get_ticks() - g_start_tsc;
		time_in_usec = g_shutdown_tsc * SPDK_SEC_TO_USEC / spdk_get_ticks_hz();
		g_time_in_usec = (g_time_in_usec > time_in_usec) ? time_in_usec : g_time_in_usec;
		printf("Received shutdown signal, test time was about %.6f seconds\n",
		       (double)g_time_in_usec / SPDK_SEC_TO_USEC);
	}

	printf("\n%*s\n", 107, "Latency(us)");
	printf("\r %-*s: %10s %10s %10s %10s %8s %10s %10s %10s\n",
	       28, "Device Information", "runtime(s)", "IOPS", "MiB/s", "Fail/s", "TO/s", "Average", "min", "max");

	TAILQ_FOREACH_SAFE(job, &g_bdevperf.jobs, link, jtmp) {
		performance_dump_job(&g_stats, job);
	}

	printf("\r =================================================================================="
	       "=================================\n");
	printf("\r %-28s: %10s %10.2f %10.2f",
	       "Total", "", g_stats.total_io_per_second, g_stats.total_mb_per_second);
	printf(" %10.2f %8.2f",
	       g_stats.total_failed_per_second, g_stats.total_timeout_per_second);

	if (g_stats.total_io_completed != 0) {
		average_latency = ((double)g_stats.total_tsc / g_stats.total_io_completed) * SPDK_SEC_TO_USEC /
				  spdk_get_ticks_hz();
	}
	printf(" %10.2f %10.2f %10.2f\n", average_latency, g_stats.min_latency, g_stats.max_latency);

	fflush(stdout);

	if (g_latency_display_level == 0 || g_stats.total_io_completed == 0) {
		goto clean;
	}

	printf("\n Latency summary\n");
	TAILQ_FOREACH_SAFE(job, &g_bdevperf.jobs, link, jtmp) {
		printf("\r =============================================\n");
		printf("\r Job: %s (Core Mask 0x%s)\n", spdk_thread_get_name(job->thread),
		       spdk_cpuset_fmt(spdk_thread_get_cpumask(job->thread)));

		const double *cutoff = g_latency_cutoffs;

		spdk_histogram_data_iterate(job->histogram, check_cutoff, &cutoff);

		printf("\n");
	}

	if (g_latency_display_level == 1) {
		goto clean;
	}

	printf("\r Latency histogram\n");
	TAILQ_FOREACH_SAFE(job, &g_bdevperf.jobs, link, jtmp) {
		printf("\r =============================================\n");
		printf("\r Job: %s (Core Mask 0x%s)\n", spdk_thread_get_name(job->thread),
		       spdk_cpuset_fmt(spdk_thread_get_cpumask(job->thread)));

		spdk_histogram_data_iterate(job->histogram, print_bucket, NULL);
		printf("\n");
	}

clean:
	TAILQ_FOREACH_SAFE(job, &g_bdevperf.jobs, link, jtmp) {
		TAILQ_REMOVE(&g_bdevperf.jobs, job, link);

		spdk_thread_send_msg(job->thread, job_thread_exit, NULL);

		TAILQ_FOREACH_SAFE(task, &job->task_list, link, ttmp) {
			TAILQ_REMOVE(&job->task_list, task, link);
			spdk_free(task->buf);
			spdk_free(task->md_buf);
			free(task);
		}

		bdevperf_job_free(job);
	}

	rc = g_run_rc;
	if (g_request && !g_shutdown) {
		rpc_perform_tests_cb();
		if (rc != 0) {
			spdk_app_stop(rc);
		}
	} else {
		spdk_app_stop(rc);
	}
}

static void
bdevperf_job_end(void *ctx)
{
	assert(g_main_thread == spdk_get_thread());

	if (--g_bdevperf.running_jobs == 0) {
		bdevperf_test_done(NULL);
	}
}

static void
bdevperf_channel_get_histogram_cb(void *cb_arg, int status, struct spdk_histogram_data *histogram)
{
	struct spdk_histogram_data *job_hist = cb_arg;

	if (status == 0) {
		spdk_histogram_data_merge(job_hist, histogram);
	}
}

static void
bdevperf_job_empty(struct bdevperf_job *job)
{
	uint64_t end_tsc = 0;

	end_tsc = spdk_get_ticks() - g_start_tsc;
	job->run_time_in_usec = end_tsc * SPDK_SEC_TO_USEC / spdk_get_ticks_hz();
	/* keep histogram info before channel is destroyed */
	spdk_bdev_channel_get_histogram(job->ch, bdevperf_channel_get_histogram_cb,
					job->histogram);
	spdk_put_io_channel(job->ch);
	spdk_bdev_close(job->bdev_desc);
	spdk_thread_send_msg(g_main_thread, bdevperf_job_end, NULL);
}

static void
bdevperf_end_task(struct bdevperf_task *task)
{
	struct bdevperf_job     *job = task->job;

	TAILQ_INSERT_TAIL(&job->task_list, task, link);
	if (job->is_draining) {
		if (job->current_queue_depth == 0) {
			bdevperf_job_empty(job);
		}
	}
}

static void
bdevperf_queue_io_wait_with_cb(struct bdevperf_task *task, spdk_bdev_io_wait_cb cb_fn)
{
	struct bdevperf_job	*job = task->job;

	task->bdev_io_wait.bdev = job->bdev;
	task->bdev_io_wait.cb_fn = cb_fn;
	task->bdev_io_wait.cb_arg = task;
	spdk_bdev_queue_io_wait(job->bdev, job->ch, &task->bdev_io_wait);
}

static int
bdevperf_job_drain(void *ctx)
{
	struct bdevperf_job *job = ctx;

	spdk_poller_unregister(&job->run_timer);
	if (job->reset) {
		spdk_poller_unregister(&job->reset_timer);
	}

	job->is_draining = true;

	return -1;
}

static int
bdevperf_job_drain_timer(void *ctx)
{
	struct bdevperf_job *job = ctx;

	bdevperf_job_drain(ctx);
	if (job->current_queue_depth == 0) {
		bdevperf_job_empty(job);
	}

	return SPDK_POLLER_BUSY;
}

static void
bdevperf_abort_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct bdevperf_task	*task = cb_arg;
	struct bdevperf_job	*job = task->job;

	job->current_queue_depth--;

	if (success) {
		job->io_completed++;
	} else {
		job->io_failed++;
		if (!job->continue_on_failure) {
			bdevperf_job_drain(job);
			g_run_rc = -1;
		}
	}

	spdk_bdev_free_io(bdev_io);
	bdevperf_end_task(task);
}

static int
bdevperf_verify_dif(struct bdevperf_task *task, struct iovec *iovs, int iovcnt)
{
	struct bdevperf_job	*job = task->job;
	struct spdk_bdev	*bdev = job->bdev;
	struct spdk_dif_ctx	dif_ctx;
	struct spdk_dif_error	err_blk = {};
	int			rc;

	rc = spdk_dif_ctx_init(&dif_ctx,
			       spdk_bdev_get_block_size(bdev),
			       spdk_bdev_get_md_size(bdev),
			       spdk_bdev_is_md_interleaved(bdev),
			       spdk_bdev_is_dif_head_of_md(bdev),
			       spdk_bdev_get_dif_type(bdev),
			       job->dif_check_flags,
			       task->offset_blocks, 0, 0, 0, 0);
	if (rc != 0) {
		fprintf(stderr, "Initialization of DIF context failed\n");
		return rc;
	}

	if (spdk_bdev_is_md_interleaved(bdev)) {
		rc = spdk_dif_verify(iovs, iovcnt, job->io_size_blocks, &dif_ctx, &err_blk);
	} else {
		struct iovec md_iov = {
			.iov_base	= task->md_buf,
			.iov_len	= spdk_bdev_get_md_size(bdev) * job->io_size_blocks,
		};

		rc = spdk_dix_verify(iovs, iovcnt, &md_iov, job->io_size_blocks, &dif_ctx, &err_blk);
	}

	if (rc != 0) {
		fprintf(stderr, "DIF/DIX error detected. type=%d, offset=%" PRIu32 "\n",
			err_blk.err_type, err_blk.err_offset);
	}

	return rc;
}

static void
bdevperf_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct bdevperf_job	*job;
	struct bdevperf_task	*task = cb_arg;
	struct iovec		*iovs;
	int			iovcnt;
	bool			md_check;
	uint64_t		offset_in_ios;
	int			rc;

	job = task->job;
	md_check = spdk_bdev_get_dif_type(job->bdev) == SPDK_DIF_DISABLE;

	if (g_error_to_exit == true) {
		bdevperf_job_drain(job);
	} else if (!success) {
		if (!job->reset && !job->continue_on_failure) {
			bdevperf_job_drain(job);
			g_run_rc = -1;
			g_error_to_exit = true;
			printf("task offset: %" PRIu64 " on job bdev=%s fails\n",
			       task->offset_blocks, job->name);
		}
	} else if (job->verify || job->reset) {
		spdk_bdev_io_get_iovec(bdev_io, &iovs, &iovcnt);
		assert(iovcnt == 1);
		assert(iovs != NULL);
		if (!verify_data(task->buf, job->buf_size, iovs[0].iov_base, iovs[0].iov_len,
				 spdk_bdev_get_block_size(job->bdev),
				 task->md_buf, spdk_bdev_io_get_md_buf(bdev_io),
				 spdk_bdev_get_md_size(job->bdev),
				 job->io_size_blocks, md_check)) {
			printf("Buffer mismatch! Target: %s Disk Offset: %" PRIu64 "\n", job->name, task->offset_blocks);
			printf("   First dword expected 0x%x got 0x%x\n", *(int *)task->buf, *(int *)iovs[0].iov_base);
			bdevperf_job_drain(job);
			g_run_rc = -1;
		}
	} else if (job->dif_check_flags != 0) {
		if (task->io_type == SPDK_BDEV_IO_TYPE_READ && spdk_bdev_get_md_size(job->bdev) != 0) {
			spdk_bdev_io_get_iovec(bdev_io, &iovs, &iovcnt);
			assert(iovcnt == 1);
			assert(iovs != NULL);
			rc = bdevperf_verify_dif(task, iovs, iovcnt);
			if (rc != 0) {
				printf("DIF error detected. task offset: %" PRIu64 " on job bdev=%s\n",
				       task->offset_blocks, job->name);

				success = false;
				if (!job->reset && !job->continue_on_failure) {
					bdevperf_job_drain(job);
					g_run_rc = -1;
					g_error_to_exit = true;
				}
			}
		}
	}

	job->current_queue_depth--;

	if (success) {
		job->io_completed++;
	} else {
		job->io_failed++;
	}

	if (job->verify) {
		assert(task->offset_blocks / job->io_size_blocks >= job->ios_base);
		offset_in_ios = task->offset_blocks / job->io_size_blocks - job->ios_base;

		assert(spdk_bit_array_get(job->outstanding, offset_in_ios) == true);
		spdk_bit_array_clear(job->outstanding, offset_in_ios);
	}

	spdk_bdev_free_io(bdev_io);

	/*
	 * is_draining indicates when time has expired for the test run
	 * and we are just waiting for the previously submitted I/O
	 * to complete.  In this case, do not submit a new I/O to replace
	 * the one just completed.
	 */
	if (!job->is_draining) {
		bdevperf_submit_single(job, task);
	} else {
		bdevperf_end_task(task);
	}
}

static void
bdevperf_verify_submit_read(void *cb_arg)
{
	struct bdevperf_job	*job;
	struct bdevperf_task	*task = cb_arg;
	int			rc;

	job = task->job;

	/* Read the data back in */
	rc = spdk_bdev_read_blocks_with_md(job->bdev_desc, job->ch, NULL, NULL,
					   task->offset_blocks, job->io_size_blocks,
					   bdevperf_complete, task);

	if (rc == -ENOMEM) {
		bdevperf_queue_io_wait_with_cb(task, bdevperf_verify_submit_read);
	} else if (rc != 0) {
		printf("Failed to submit read: %d\n", rc);
		bdevperf_job_drain(job);
		g_run_rc = rc;
	}
}

static void
bdevperf_verify_write_complete(struct spdk_bdev_io *bdev_io, bool success,
			       void *cb_arg)
{
	if (success) {
		spdk_bdev_free_io(bdev_io);
		bdevperf_verify_submit_read(cb_arg);
	} else {
		bdevperf_complete(bdev_io, success, cb_arg);
	}
}

static void
bdevperf_zcopy_populate_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	if (!success) {
		bdevperf_complete(bdev_io, success, cb_arg);
		return;
	}

	spdk_bdev_zcopy_end(bdev_io, false, bdevperf_complete, cb_arg);
}

static int
bdevperf_generate_dif(struct bdevperf_task *task)
{
	struct bdevperf_job	*job = task->job;
	struct spdk_bdev	*bdev = job->bdev;
	struct spdk_dif_ctx	dif_ctx;
	int			rc;

	rc = spdk_dif_ctx_init(&dif_ctx,
			       spdk_bdev_get_block_size(bdev),
			       spdk_bdev_get_md_size(bdev),
			       spdk_bdev_is_md_interleaved(bdev),
			       spdk_bdev_is_dif_head_of_md(bdev),
			       spdk_bdev_get_dif_type(bdev),
			       job->dif_check_flags,
			       task->offset_blocks, 0, 0, 0, 0);
	if (rc != 0) {
		fprintf(stderr, "Initialization of DIF context failed\n");
		return rc;
	}

	if (spdk_bdev_is_md_interleaved(bdev)) {
		rc = spdk_dif_generate(&task->iov, 1, job->io_size_blocks, &dif_ctx);
	} else {
		struct iovec md_iov = {
			.iov_base	= task->md_buf,
			.iov_len	= spdk_bdev_get_md_size(bdev) * job->io_size_blocks,
		};

		rc = spdk_dix_generate(&task->iov, 1, &md_iov, job->io_size_blocks, &dif_ctx);
	}

	if (rc != 0) {
		fprintf(stderr, "Generation of DIF/DIX failed\n");
	}

	return rc;
}

static void
bdevperf_submit_task(void *arg)
{
	struct bdevperf_task	*task = arg;
	struct bdevperf_job	*job = task->job;
	struct spdk_bdev_desc	*desc;
	struct spdk_io_channel	*ch;
	spdk_bdev_io_completion_cb cb_fn;
	uint64_t		offset_in_ios;
	int			rc = 0;

	desc = job->bdev_desc;
	ch = job->ch;

	switch (task->io_type) {
	case SPDK_BDEV_IO_TYPE_WRITE:
		if (spdk_bdev_get_md_size(job->bdev) != 0 && job->dif_check_flags != 0) {
			rc = bdevperf_generate_dif(task);
		}
		if (rc == 0) {
			cb_fn = (job->verify || job->reset) ? bdevperf_verify_write_complete : bdevperf_complete;

			if (g_zcopy) {
				spdk_bdev_zcopy_end(task->bdev_io, true, cb_fn, task);
				return;
			} else {
				rc = spdk_bdev_writev_blocks_with_md(desc, ch, &task->iov, 1,
								     task->md_buf,
								     task->offset_blocks,
								     job->io_size_blocks,
								     cb_fn, task);
			}
		}
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush_blocks(desc, ch, task->offset_blocks,
					    job->io_size_blocks, bdevperf_complete, task);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = spdk_bdev_unmap_blocks(desc, ch, task->offset_blocks,
					    job->io_size_blocks, bdevperf_complete, task);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		rc = spdk_bdev_write_zeroes_blocks(desc, ch, task->offset_blocks,
						   job->io_size_blocks, bdevperf_complete, task);
		break;
	case SPDK_BDEV_IO_TYPE_READ:
		if (g_zcopy) {
			rc = spdk_bdev_zcopy_start(desc, ch, NULL, 0, task->offset_blocks, job->io_size_blocks,
						   true, bdevperf_zcopy_populate_complete, task);
		} else {
			rc = spdk_bdev_read_blocks_with_md(desc, ch, task->buf, task->md_buf,
							   task->offset_blocks,
							   job->io_size_blocks,
							   bdevperf_complete, task);
		}
		break;
	case SPDK_BDEV_IO_TYPE_ABORT:
		rc = spdk_bdev_abort(desc, ch, task->task_to_abort, bdevperf_abort_complete, task);
		break;
	default:
		assert(false);
		rc = -EINVAL;
		break;
	}

	if (rc == -ENOMEM) {
		bdevperf_queue_io_wait_with_cb(task, bdevperf_submit_task);
		return;
	} else if (rc != 0) {
		printf("Failed to submit bdev_io: %d\n", rc);
		if (job->verify) {
			assert(task->offset_blocks / job->io_size_blocks >= job->ios_base);
			offset_in_ios = task->offset_blocks / job->io_size_blocks - job->ios_base;

			assert(spdk_bit_array_get(job->outstanding, offset_in_ios) == true);
			spdk_bit_array_clear(job->outstanding, offset_in_ios);
		}
		bdevperf_job_drain(job);
		g_run_rc = rc;
		return;
	}

	job->current_queue_depth++;
}

static void
bdevperf_zcopy_get_buf_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct bdevperf_task	*task = cb_arg;
	struct bdevperf_job	*job = task->job;
	struct iovec		*iovs;
	int			iovcnt;

	if (!success) {
		bdevperf_job_drain(job);
		g_run_rc = -1;
		return;
	}

	task->bdev_io = bdev_io;
	task->io_type = SPDK_BDEV_IO_TYPE_WRITE;

	if (job->verify || job->reset) {
		/* When job->verify or job->reset is enabled, task->buf is used for
		 *  verification of read after write.  For write I/O, when zcopy APIs
		 *  are used, task->buf cannot be used, and data must be written to
		 *  the data buffer allocated underneath bdev layer instead.
		 *  Hence we copy task->buf to the allocated data buffer here.
		 */
		spdk_bdev_io_get_iovec(bdev_io, &iovs, &iovcnt);
		assert(iovcnt == 1);
		assert(iovs != NULL);

		copy_data(iovs[0].iov_base, iovs[0].iov_len, task->buf, job->buf_size,
			  spdk_bdev_get_block_size(job->bdev),
			  spdk_bdev_io_get_md_buf(bdev_io), task->md_buf,
			  spdk_bdev_get_md_size(job->bdev), job->io_size_blocks);
	}

	bdevperf_submit_task(task);
}

static void
bdevperf_prep_zcopy_write_task(void *arg)
{
	struct bdevperf_task	*task = arg;
	struct bdevperf_job	*job = task->job;
	int			rc;

	rc = spdk_bdev_zcopy_start(job->bdev_desc, job->ch, NULL, 0,
				   task->offset_blocks, job->io_size_blocks,
				   false, bdevperf_zcopy_get_buf_complete, task);
	if (rc != 0) {
		assert(rc == -ENOMEM);
		bdevperf_queue_io_wait_with_cb(task, bdevperf_prep_zcopy_write_task);
		return;
	}

	job->current_queue_depth++;
}

static struct bdevperf_task *
bdevperf_job_get_task(struct bdevperf_job *job)
{
	struct bdevperf_task *task;

	task = TAILQ_FIRST(&job->task_list);
	if (!task) {
		printf("Task allocation failed\n");
		abort();
	}

	TAILQ_REMOVE(&job->task_list, task, link);
	return task;
}

static void
bdevperf_submit_single(struct bdevperf_job *job, struct bdevperf_task *task)
{
	uint64_t offset_in_ios;
	uint64_t rand_value;

	if (job->zipf) {
		offset_in_ios = spdk_zipf_generate(job->zipf);
	} else if (job->is_random) {
		/* RAND_MAX is only INT32_MAX, so use 2 calls to rand_r to
		 * get a large enough value to ensure we are issuing I/O
		 * uniformly across the whole bdev.
		 */
		rand_value = (uint64_t)rand_r(&job->seed) * RAND_MAX + rand_r(&job->seed);
		offset_in_ios = rand_value % job->size_in_ios;
	} else {
		offset_in_ios = job->offset_in_ios++;
		if (job->offset_in_ios == job->size_in_ios) {
			job->offset_in_ios = 0;
		}

		/* Increment of offset_in_ios if there's already an outstanding IO
		 * to that location. We only need this with job->verify as random
		 * offsets are not supported with job->verify at this time.
		 */
		if (job->verify) {
			assert(spdk_bit_array_find_first_clear(job->outstanding, 0) != UINT32_MAX);

			while (spdk_bit_array_get(job->outstanding, offset_in_ios)) {
				offset_in_ios = job->offset_in_ios++;
				if (job->offset_in_ios == job->size_in_ios) {
					job->offset_in_ios = 0;
				}
			}
			spdk_bit_array_set(job->outstanding, offset_in_ios);
		}
	}

	/* For multi-thread to same job, offset_in_ios is relative
	 * to the LBA range assigned for that job. job->offset_blocks
	 * is absolute (entire bdev LBA range).
	 */
	task->offset_blocks = (offset_in_ios + job->ios_base) * job->io_size_blocks;

	if (job->verify || job->reset) {
		generate_data(task->buf, job->buf_size,
			      spdk_bdev_get_block_size(job->bdev),
			      task->md_buf, spdk_bdev_get_md_size(job->bdev),
			      job->io_size_blocks);
		if (g_zcopy) {
			bdevperf_prep_zcopy_write_task(task);
			return;
		} else {
			task->iov.iov_base = task->buf;
			task->iov.iov_len = job->buf_size;
			task->io_type = SPDK_BDEV_IO_TYPE_WRITE;
		}
	} else if (job->flush) {
		task->io_type = SPDK_BDEV_IO_TYPE_FLUSH;
	} else if (job->unmap) {
		task->io_type = SPDK_BDEV_IO_TYPE_UNMAP;
	} else if (job->write_zeroes) {
		task->io_type = SPDK_BDEV_IO_TYPE_WRITE_ZEROES;
	} else if ((job->rw_percentage == 100) ||
		   (job->rw_percentage != 0 && ((rand_r(&job->seed) % 100) < job->rw_percentage))) {
		task->io_type = SPDK_BDEV_IO_TYPE_READ;
	} else {
		if (g_zcopy) {
			bdevperf_prep_zcopy_write_task(task);
			return;
		} else {
			task->iov.iov_base = task->buf;
			task->iov.iov_len = job->buf_size;
			task->io_type = SPDK_BDEV_IO_TYPE_WRITE;
		}
	}

	bdevperf_submit_task(task);
}

static int reset_job(void *arg);

static void
reset_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct bdevperf_task	*task = cb_arg;
	struct bdevperf_job	*job = task->job;

	if (!success) {
		printf("Reset blockdev=%s failed\n", spdk_bdev_get_name(job->bdev));
		bdevperf_job_drain(job);
		g_run_rc = -1;
	}

	TAILQ_INSERT_TAIL(&job->task_list, task, link);
	spdk_bdev_free_io(bdev_io);

	job->reset_timer = SPDK_POLLER_REGISTER(reset_job, job,
						10 * SPDK_SEC_TO_USEC);
}

static int
reset_job(void *arg)
{
	struct bdevperf_job *job = arg;
	struct bdevperf_task *task;
	int rc;

	spdk_poller_unregister(&job->reset_timer);

	/* Do reset. */
	task = bdevperf_job_get_task(job);
	rc = spdk_bdev_reset(job->bdev_desc, job->ch,
			     reset_cb, task);
	if (rc) {
		printf("Reset failed: %d\n", rc);
		bdevperf_job_drain(job);
		g_run_rc = -1;
	}

	return -1;
}

static void
bdevperf_timeout_cb(void *cb_arg, struct spdk_bdev_io *bdev_io)
{
	struct bdevperf_job *job = cb_arg;
	struct bdevperf_task *task;

	job->io_timeout++;

	if (job->is_draining || !job->abort ||
	    !spdk_bdev_io_type_supported(job->bdev, SPDK_BDEV_IO_TYPE_ABORT)) {
		return;
	}

	task = bdevperf_job_get_task(job);
	if (task == NULL) {
		return;
	}

	task->task_to_abort = spdk_bdev_io_get_cb_arg(bdev_io);
	task->io_type = SPDK_BDEV_IO_TYPE_ABORT;

	bdevperf_submit_task(task);
}

static void
bdevperf_job_run(void *ctx)
{
	struct bdevperf_job *job = ctx;
	struct bdevperf_task *task;
	int i;

	/* Submit initial I/O for this job. Each time one
	 * completes, another will be submitted. */

	/* Start a timer to stop this I/O chain when the run is over */
	job->run_timer = SPDK_POLLER_REGISTER(bdevperf_job_drain_timer, job, g_time_in_usec);
	if (job->reset) {
		job->reset_timer = SPDK_POLLER_REGISTER(reset_job, job,
							10 * SPDK_SEC_TO_USEC);
	}

	spdk_bdev_set_timeout(job->bdev_desc, g_timeout_in_sec, bdevperf_timeout_cb, job);

	for (i = 0; i < job->queue_depth; i++) {
		task = bdevperf_job_get_task(job);
		bdevperf_submit_single(job, task);
	}
}

static void
_performance_dump_done(void *ctx)
{
	struct bdevperf_aggregate_stats *stats = ctx;
	double average_latency;

	printf("\r =================================================================================="
	       "=================================\n");
	printf("\r %-28s: %10s %10.2f %10.2f",
	       "Total", "", stats->total_io_per_second, stats->total_mb_per_second);
	printf(" %10.2f %8.2f",
	       stats->total_failed_per_second, stats->total_timeout_per_second);

	average_latency = ((double)stats->total_tsc / stats->total_io_completed) * SPDK_SEC_TO_USEC /
			  spdk_get_ticks_hz();
	printf(" %10.2f %10.2f %10.2f\n", average_latency, stats->min_latency, stats->max_latency);
	printf("\n");

	fflush(stdout);

	g_performance_dump_active = false;

	free(stats);
}

static void
_performance_dump(void *ctx)
{
	struct bdevperf_aggregate_stats *stats = ctx;

	performance_dump_job(stats, stats->current_job);

	/* This assumes the jobs list is static after start up time.
	 * That's true right now, but if that ever changed this would need a lock. */
	stats->current_job = TAILQ_NEXT(stats->current_job, link);
	if (stats->current_job == NULL) {
		spdk_thread_send_msg(g_main_thread, _performance_dump_done, stats);
	} else {
		spdk_thread_send_msg(stats->current_job->thread, _performance_dump, stats);
	}
}

static int
performance_statistics_thread(void *arg)
{
	struct bdevperf_aggregate_stats *stats;

	if (g_performance_dump_active) {
		return -1;
	}

	g_performance_dump_active = true;

	stats = calloc(1, sizeof(*stats));
	if (stats == NULL) {
		return -1;
	}

	stats->min_latency = (double)UINT64_MAX;

	g_show_performance_period_num++;

	stats->io_time_in_usec = g_show_performance_period_num * g_show_performance_period_in_usec;
	stats->ema_period = g_show_performance_ema_period;

	/* Iterate all of the jobs to gather stats
	 * These jobs will not get removed here until a final performance dump is run,
	 * so this should be safe without locking.
	 */
	stats->current_job = TAILQ_FIRST(&g_bdevperf.jobs);
	if (stats->current_job == NULL) {
		spdk_thread_send_msg(g_main_thread, _performance_dump_done, stats);
	} else {
		spdk_thread_send_msg(stats->current_job->thread, _performance_dump, stats);
	}

	return -1;
}

static void
bdevperf_test(void)
{
	struct bdevperf_job *job;

	printf("Running I/O for %" PRIu64 " seconds...\n", g_time_in_usec / (uint64_t)SPDK_SEC_TO_USEC);
	fflush(stdout);

	/* Start a timer to dump performance numbers */
	g_start_tsc = spdk_get_ticks();
	if (g_show_performance_real_time && !g_perf_timer) {
		printf("%*s\n", 107, "Latency(us)");
		printf("\r %-*s: %10s %10s %10s %10s %8s %10s %10s %10s\n",
		       28, "Device Information", "runtime(s)", "IOPS", "MiB/s", "Fail/s", "TO/s", "Average", "min", "max");

		g_perf_timer = SPDK_POLLER_REGISTER(performance_statistics_thread, NULL,
						    g_show_performance_period_in_usec);
	}

	/* Iterate jobs to start all I/O */
	TAILQ_FOREACH(job, &g_bdevperf.jobs, link) {
		g_bdevperf.running_jobs++;
		spdk_thread_send_msg(job->thread, bdevperf_job_run, job);
	}
}

static void
bdevperf_bdev_removed(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	struct bdevperf_job *job = event_ctx;

	if (SPDK_BDEV_EVENT_REMOVE == type) {
		bdevperf_job_drain(job);
	}
}

static void
bdevperf_histogram_status_cb(void *cb_arg, int status)
{
	if (status != 0) {
		g_run_rc = status;
		if (g_continue_on_failure == false) {
			g_error_to_exit = true;
		}
	}

	if (--g_bdev_count == 0) {
		if (g_run_rc == 0) {
			/* Ready to run the test */
			bdevperf_test();
		} else {
			bdevperf_test_done(NULL);
		}
	}
}

static uint32_t g_construct_job_count = 0;

static void
_bdevperf_enable_histogram(bool enable)
{
	struct spdk_bdev *bdev;
	/* increment initial g_bdev_count so that it will never reach 0 in the middle of iteration */
	g_bdev_count = 1;

	if (g_job_bdev_name != NULL) {
		bdev = spdk_bdev_get_by_name(g_job_bdev_name);
		if (bdev) {
			g_bdev_count++;

			spdk_bdev_histogram_enable(bdev, bdevperf_histogram_status_cb, NULL, enable);
		} else {
			fprintf(stderr, "Unable to find bdev '%s'\n", g_job_bdev_name);
		}
	} else {
		bdev = spdk_bdev_first_leaf();

		while (bdev != NULL) {
			g_bdev_count++;

			spdk_bdev_histogram_enable(bdev, bdevperf_histogram_status_cb, NULL, enable);
			bdev = spdk_bdev_next_leaf(bdev);
		}
	}

	bdevperf_histogram_status_cb(NULL, 0);
}

static void
_bdevperf_construct_job_done(void *ctx)
{
	if (--g_construct_job_count == 0) {
		if (g_run_rc != 0) {
			/* Something failed. */
			bdevperf_test_done(NULL);
			return;
		}

		/* always enable histogram. */
		_bdevperf_enable_histogram(true);
	} else if (g_run_rc != 0) {
		/* Reset error as some jobs constructed right */
		g_run_rc = 0;
		if (g_continue_on_failure == false) {
			g_error_to_exit = true;
		}
	}
}

/* Checkformat will not allow to use inlined type,
   this is a workaround */
typedef struct spdk_thread *spdk_thread_t;

static spdk_thread_t
construct_job_thread(struct spdk_cpuset *cpumask, const char *tag)
{
	struct spdk_cpuset tmp;

	/* This function runs on the main thread. */
	assert(g_main_thread == spdk_get_thread());

	/* Handle default mask */
	if (spdk_cpuset_count(cpumask) == 0) {
		cpumask = &g_all_cpuset;
	}

	/* Warn user that mask might need to be changed */
	spdk_cpuset_copy(&tmp, cpumask);
	spdk_cpuset_or(&tmp, &g_all_cpuset);
	if (!spdk_cpuset_equal(&tmp, &g_all_cpuset)) {
		fprintf(stderr, "cpumask for '%s' is too big\n", tag);
	}

	return spdk_thread_create(tag, cpumask);
}

static uint32_t
_get_next_core(void)
{
	static uint32_t current_core = SPDK_ENV_LCORE_ID_ANY;

	if (current_core == SPDK_ENV_LCORE_ID_ANY) {
		current_core = spdk_env_get_first_core();
		return current_core;
	}

	current_core = spdk_env_get_next_core(current_core);
	if (current_core == SPDK_ENV_LCORE_ID_ANY) {
		current_core = spdk_env_get_first_core();
	}

	return current_core;
}

static void
_bdevperf_construct_job(void *ctx)
{
	struct bdevperf_job *job = ctx;
	int rc;

	rc = spdk_bdev_open_ext(spdk_bdev_get_name(job->bdev), true, bdevperf_bdev_removed, job,
				&job->bdev_desc);
	if (rc != 0) {
		SPDK_ERRLOG("Could not open leaf bdev %s, error=%d\n", spdk_bdev_get_name(job->bdev), rc);
		g_run_rc = -EINVAL;
		goto end;
	}

	if (g_zcopy) {
		if (!spdk_bdev_io_type_supported(job->bdev, SPDK_BDEV_IO_TYPE_ZCOPY)) {
			printf("Test requires ZCOPY but bdev module does not support ZCOPY\n");
			g_run_rc = -ENOTSUP;
			goto end;
		}
	}

	job->ch = spdk_bdev_get_io_channel(job->bdev_desc);
	if (!job->ch) {
		SPDK_ERRLOG("Could not get io_channel for device %s, error=%d\n", spdk_bdev_get_name(job->bdev),
			    rc);
		spdk_bdev_close(job->bdev_desc);
		TAILQ_REMOVE(&g_bdevperf.jobs, job, link);
		g_run_rc = -ENOMEM;
		goto end;
	}

end:
	spdk_thread_send_msg(g_main_thread, _bdevperf_construct_job_done, NULL);
}

static void
job_init_rw(struct bdevperf_job *job, enum job_config_rw rw)
{
	switch (rw) {
	case JOB_CONFIG_RW_READ:
		job->rw_percentage = 100;
		break;
	case JOB_CONFIG_RW_WRITE:
		job->rw_percentage = 0;
		break;
	case JOB_CONFIG_RW_RANDREAD:
		job->is_random = true;
		job->rw_percentage = 100;
		job->seed = rand();
		break;
	case JOB_CONFIG_RW_RANDWRITE:
		job->is_random = true;
		job->rw_percentage = 0;
		job->seed = rand();
		break;
	case JOB_CONFIG_RW_RW:
		job->is_random = false;
		break;
	case JOB_CONFIG_RW_RANDRW:
		job->is_random = true;
		job->seed = rand();
		break;
	case JOB_CONFIG_RW_VERIFY:
		job->verify = true;
		job->rw_percentage = 50;
		break;
	case JOB_CONFIG_RW_RESET:
		job->reset = true;
		job->verify = true;
		job->rw_percentage = 50;
		break;
	case JOB_CONFIG_RW_UNMAP:
		job->unmap = true;
		break;
	case JOB_CONFIG_RW_FLUSH:
		job->flush = true;
		break;
	case JOB_CONFIG_RW_WRITE_ZEROES:
		job->write_zeroes = true;
		break;
	}
}

static int
bdevperf_construct_job(struct spdk_bdev *bdev, struct job_config *config,
		       struct spdk_thread *thread)
{
	struct bdevperf_job *job;
	struct bdevperf_task *task;
	int block_size, data_block_size;
	int rc;
	int task_num, n;

	block_size = spdk_bdev_get_block_size(bdev);
	data_block_size = spdk_bdev_get_data_block_size(bdev);

	job = calloc(1, sizeof(struct bdevperf_job));
	if (!job) {
		fprintf(stderr, "Unable to allocate memory for new job.\n");
		return -ENOMEM;
	}

	job->name = strdup(spdk_bdev_get_name(bdev));
	if (!job->name) {
		fprintf(stderr, "Unable to allocate memory for job name.\n");
		bdevperf_job_free(job);
		return -ENOMEM;
	}

	job->workload_type = g_workload_type;
	job->io_size = config->bs;
	job->rw_percentage = config->rwmixread;
	job->continue_on_failure = g_continue_on_failure;
	job->queue_depth = config->iodepth;
	job->bdev = bdev;
	job->io_size_blocks = job->io_size / data_block_size;
	job->buf_size = job->io_size_blocks * block_size;
	job->abort = g_abort;
	job_init_rw(job, config->rw);

	if ((job->io_size % data_block_size) != 0) {
		SPDK_ERRLOG("IO size (%d) is not multiples of data block size of bdev %s (%"PRIu32")\n",
			    job->io_size, spdk_bdev_get_name(bdev), data_block_size);
		bdevperf_job_free(job);
		return -ENOTSUP;
	}

	if (job->unmap && !spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
		printf("Skipping %s because it does not support unmap\n", spdk_bdev_get_name(bdev));
		bdevperf_job_free(job);
		return -ENOTSUP;
	}

	if (spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_REFTAG)) {
		job->dif_check_flags |= SPDK_DIF_FLAGS_REFTAG_CHECK;
	}
	if (spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_GUARD)) {
		job->dif_check_flags |= SPDK_DIF_FLAGS_GUARD_CHECK;
	}

	job->offset_in_ios = 0;

	if (config->length != 0) {
		/* Use subset of disk */
		job->size_in_ios = config->length / job->io_size_blocks;
		job->ios_base = config->offset / job->io_size_blocks;
	} else {
		/* Use whole disk */
		job->size_in_ios = spdk_bdev_get_num_blocks(bdev) / job->io_size_blocks;
		job->ios_base = 0;
	}

	if (job->is_random && g_zipf_theta > 0) {
		job->zipf = spdk_zipf_create(job->size_in_ios, g_zipf_theta, 0);
	}

	if (job->verify) {
		job->outstanding = spdk_bit_array_create(job->size_in_ios);
		if (job->outstanding == NULL) {
			SPDK_ERRLOG("Could not create outstanding array bitmap for bdev %s\n",
				    spdk_bdev_get_name(bdev));
			bdevperf_job_free(job);
			return -ENOMEM;
		}
		if (job->queue_depth > (int)job->size_in_ios) {
			SPDK_WARNLOG("Due to constraints of verify job, queue depth (-q, %d) can't exceed the number of IO "
				     "requests which can be submitted to the bdev %s simultaneously (%"PRIu64"). "
				     "Queue depth is limited to %"PRIu64"\n",
				     job->queue_depth, job->name, job->size_in_ios, job->size_in_ios);
			job->queue_depth = (int)job->size_in_ios;
		}
	}

	job->histogram = spdk_histogram_data_alloc();
	if (job->histogram == NULL) {
		fprintf(stderr, "Failed to allocate histogram\n");
		bdevperf_job_free(job);
		return -ENOMEM;
	}

	TAILQ_INIT(&job->task_list);

	task_num = job->queue_depth;
	if (job->reset) {
		task_num += 1;
	}
	if (job->abort) {
		task_num += job->queue_depth;
	}

	TAILQ_INSERT_TAIL(&g_bdevperf.jobs, job, link);

	for (n = 0; n < task_num; n++) {
		task = calloc(1, sizeof(struct bdevperf_task));
		if (!task) {
			fprintf(stderr, "Failed to allocate task from memory\n");
			return -ENOMEM;
		}

		task->buf = spdk_zmalloc(job->buf_size, spdk_bdev_get_buf_align(job->bdev), NULL,
					 SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (!task->buf) {
			fprintf(stderr, "Cannot allocate buf for task=%p\n", task);
			free(task);
			return -ENOMEM;
		}

		if (spdk_bdev_is_md_separate(job->bdev)) {
			task->md_buf = spdk_zmalloc(job->io_size_blocks *
						    spdk_bdev_get_md_size(job->bdev), 0, NULL,
						    SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
			if (!task->md_buf) {
				fprintf(stderr, "Cannot allocate md buf for task=%p\n", task);
				spdk_free(task->buf);
				free(task);
				return -ENOMEM;
			}
		}

		task->job = job;
		TAILQ_INSERT_TAIL(&job->task_list, task, link);
	}

	job->thread = thread;

	g_construct_job_count++;

	rc = spdk_thread_send_msg(thread, _bdevperf_construct_job, job);
	assert(rc == 0);

	return rc;
}

static int
parse_rw(const char *str, enum job_config_rw ret)
{
	if (str == NULL) {
		return ret;
	}

	if (!strcmp(str, "read")) {
		ret = JOB_CONFIG_RW_READ;
	} else if (!strcmp(str, "randread")) {
		ret = JOB_CONFIG_RW_RANDREAD;
	} else if (!strcmp(str, "write")) {
		ret = JOB_CONFIG_RW_WRITE;
	} else if (!strcmp(str, "randwrite")) {
		ret = JOB_CONFIG_RW_RANDWRITE;
	} else if (!strcmp(str, "verify")) {
		ret = JOB_CONFIG_RW_VERIFY;
	} else if (!strcmp(str, "reset")) {
		ret = JOB_CONFIG_RW_RESET;
	} else if (!strcmp(str, "unmap")) {
		ret = JOB_CONFIG_RW_UNMAP;
	} else if (!strcmp(str, "write_zeroes")) {
		ret = JOB_CONFIG_RW_WRITE_ZEROES;
	} else if (!strcmp(str, "flush")) {
		ret = JOB_CONFIG_RW_FLUSH;
	} else if (!strcmp(str, "rw")) {
		ret = JOB_CONFIG_RW_RW;
	} else if (!strcmp(str, "randrw")) {
		ret = JOB_CONFIG_RW_RANDRW;
	} else {
		fprintf(stderr, "rw must be one of\n"
			"(read, write, randread, randwrite, rw, randrw, verify, reset, unmap, flush)\n");
		ret = BDEVPERF_CONFIG_ERROR;
	}

	return ret;
}

static const char *
config_filename_next(const char *filename, char *out)
{
	int i, k;

	if (filename == NULL) {
		out[0] = '\0';
		return NULL;
	}

	if (filename[0] == ':') {
		filename++;
	}

	for (i = 0, k = 0;
	     filename[i] != '\0' &&
	     filename[i] != ':' &&
	     i < BDEVPERF_CONFIG_MAX_FILENAME;
	     i++) {
		if (filename[i] == ' ' || filename[i] == '\t') {
			continue;
		}

		out[k++] = filename[i];
	}
	out[k] = 0;

	return filename + i;
}

static void
bdevperf_construct_jobs(void)
{
	char filename[BDEVPERF_CONFIG_MAX_FILENAME];
	struct spdk_thread *thread;
	struct job_config *config;
	struct spdk_bdev *bdev;
	const char *filenames;
	int rc;

	TAILQ_FOREACH(config, &job_config_list, link) {
		filenames = config->filename;

		thread = construct_job_thread(&config->cpumask, config->name);
		assert(thread);

		while (filenames) {
			filenames = config_filename_next(filenames, filename);
			if (strlen(filename) == 0) {
				break;
			}

			bdev = spdk_bdev_get_by_name(filename);
			if (!bdev) {
				fprintf(stderr, "Unable to find bdev '%s'\n", filename);
				g_run_rc = -EINVAL;
				return;
			}

			rc = bdevperf_construct_job(bdev, config, thread);
			if (rc < 0) {
				g_run_rc = rc;
				return;
			}
		}
	}
}

static int
make_cli_job_config(const char *filename, int64_t offset, uint64_t range)
{
	struct job_config *config = calloc(1, sizeof(*config));

	if (config == NULL) {
		fprintf(stderr, "Unable to allocate memory for job config\n");
		return -ENOMEM;
	}

	config->name = filename;
	config->filename = filename;
	spdk_cpuset_zero(&config->cpumask);
	spdk_cpuset_set_cpu(&config->cpumask, _get_next_core(), true);
	config->bs = g_io_size;
	config->iodepth = g_queue_depth;
	config->rwmixread = g_rw_percentage;
	config->offset = offset;
	config->length = range;
	config->rw = parse_rw(g_workload_type, BDEVPERF_CONFIG_ERROR);
	if ((int)config->rw == BDEVPERF_CONFIG_ERROR) {
		free(config);
		return -EINVAL;
	}

	TAILQ_INSERT_TAIL(&job_config_list, config, link);
	return 0;
}

static void
bdevperf_construct_multithread_job_configs(void)
{
	struct spdk_bdev *bdev;
	uint32_t i;
	uint32_t num_cores;
	uint64_t blocks_per_job;
	int64_t offset;

	num_cores = 0;
	SPDK_ENV_FOREACH_CORE(i) {
		num_cores++;
	}

	if (num_cores == 0) {
		g_run_rc = -EINVAL;
		return;
	}

	if (g_job_bdev_name != NULL) {
		bdev = spdk_bdev_get_by_name(g_job_bdev_name);
		if (!bdev) {
			fprintf(stderr, "Unable to find bdev '%s'\n", g_job_bdev_name);
			return;
		}

		blocks_per_job = spdk_bdev_get_num_blocks(bdev) / num_cores;
		offset = 0;

		SPDK_ENV_FOREACH_CORE(i) {
			g_run_rc = make_cli_job_config(g_job_bdev_name, offset, blocks_per_job);
			if (g_run_rc) {
				return;
			}

			offset += blocks_per_job;
		}
	} else {
		bdev = spdk_bdev_first_leaf();
		while (bdev != NULL) {
			blocks_per_job = spdk_bdev_get_num_blocks(bdev) / num_cores;
			offset = 0;

			SPDK_ENV_FOREACH_CORE(i) {
				g_run_rc = make_cli_job_config(spdk_bdev_get_name(bdev),
							       offset, blocks_per_job);
				if (g_run_rc) {
					return;
				}

				offset += blocks_per_job;
			}

			bdev = spdk_bdev_next_leaf(bdev);
		}
	}
}

static void
bdevperf_construct_job_configs(void)
{
	struct spdk_bdev *bdev;

	/* There are three different modes for allocating jobs. Standard mode
	 * (the default) creates one spdk_thread per bdev and runs the I/O job there.
	 *
	 * The -C flag places bdevperf into "multithread" mode, meaning it creates
	 * one spdk_thread per bdev PER CORE, and runs a copy of the job on each.
	 * This runs multiple threads per bdev, effectively.
	 *
	 * The -j flag implies "FIO" mode which tries to mimic semantic of FIO jobs.
	 * In "FIO" mode, threads are spawned per-job instead of per-bdev.
	 * Each FIO job can be individually parameterized by filename, cpu mask, etc,
	 * which is different from other modes in that they only support global options.
	 */

	if (g_bdevperf_conf) {
		goto end;
	} else if (g_multithread_mode) {
		bdevperf_construct_multithread_job_configs();
		goto end;
	}

	if (g_job_bdev_name != NULL) {
		bdev = spdk_bdev_get_by_name(g_job_bdev_name);
		if (bdev) {
			/* Construct the job */
			g_run_rc = make_cli_job_config(g_job_bdev_name, 0, 0);
		} else {
			fprintf(stderr, "Unable to find bdev '%s'\n", g_job_bdev_name);
		}
	} else {
		bdev = spdk_bdev_first_leaf();

		while (bdev != NULL) {
			/* Construct the job */
			g_run_rc = make_cli_job_config(spdk_bdev_get_name(bdev), 0, 0);
			if (g_run_rc) {
				break;
			}

			bdev = spdk_bdev_next_leaf(bdev);
		}
	}

end:
	/* Increment initial construct_jobs count so that it will never reach 0 in the middle
	 * of iteration.
	 */
	g_construct_job_count = 1;

	if (g_run_rc == 0) {
		bdevperf_construct_jobs();
	}

	_bdevperf_construct_job_done(NULL);
}

static int
parse_uint_option(struct spdk_conf_section *s, const char *name, int def)
{
	const char *job_name;
	int tmp;

	tmp = spdk_conf_section_get_intval(s, name);
	if (tmp == -1) {
		/* Field was not found. Check default value
		 * In [global] section it is ok to have undefined values
		 * but for other sections it is not ok */
		if (def == BDEVPERF_CONFIG_UNDEFINED) {
			job_name = spdk_conf_section_get_name(s);
			if (strcmp(job_name, "global") == 0) {
				return def;
			}

			fprintf(stderr,
				"Job '%s' has no '%s' assigned\n",
				job_name, name);
			return BDEVPERF_CONFIG_ERROR;
		}
		return def;
	}

	/* NOTE: get_intval returns nonnegative on success */
	if (tmp < 0) {
		fprintf(stderr, "Job '%s' has bad '%s' value.\n",
			spdk_conf_section_get_name(s), name);
		return BDEVPERF_CONFIG_ERROR;
	}

	return tmp;
}

/* CLI arguments override parameters for global sections */
static void
config_set_cli_args(struct job_config *config)
{
	if (g_job_bdev_name) {
		config->filename = g_job_bdev_name;
	}
	if (g_io_size > 0) {
		config->bs = g_io_size;
	}
	if (g_queue_depth > 0) {
		config->iodepth = g_queue_depth;
	}
	if (g_rw_percentage > 0) {
		config->rwmixread = g_rw_percentage;
	}
	if (g_workload_type) {
		config->rw = parse_rw(g_workload_type, config->rw);
	}
}

static int
read_job_config(void)
{
	struct job_config global_default_config;
	struct job_config global_config;
	struct spdk_conf_section *s;
	struct job_config *config;
	const char *cpumask;
	const char *rw;
	bool is_global;
	int n = 0;
	int val;

	if (g_bdevperf_conf_file == NULL) {
		return 0;
	}

	g_bdevperf_conf = spdk_conf_allocate();
	if (g_bdevperf_conf == NULL) {
		fprintf(stderr, "Could not allocate job config structure\n");
		return 1;
	}

	spdk_conf_disable_sections_merge(g_bdevperf_conf);
	if (spdk_conf_read(g_bdevperf_conf, g_bdevperf_conf_file)) {
		fprintf(stderr, "Invalid job config");
		return 1;
	}

	/* Initialize global defaults */
	global_default_config.filename = NULL;
	/* Zero mask is the same as g_all_cpuset
	 * The g_all_cpuset is not initialized yet,
	 * so use zero mask as the default instead */
	spdk_cpuset_zero(&global_default_config.cpumask);
	global_default_config.bs = BDEVPERF_CONFIG_UNDEFINED;
	global_default_config.iodepth = BDEVPERF_CONFIG_UNDEFINED;
	/* bdevperf has no default for -M option but in FIO the default is 50 */
	global_default_config.rwmixread = 50;
	global_default_config.offset = 0;
	/* length 0 means 100% */
	global_default_config.length = 0;
	global_default_config.rw = BDEVPERF_CONFIG_UNDEFINED;
	config_set_cli_args(&global_default_config);

	if ((int)global_default_config.rw == BDEVPERF_CONFIG_ERROR) {
		return 1;
	}

	/* There is only a single instance of global job_config
	 * We just reset its value when we encounter new [global] section */
	global_config = global_default_config;

	for (s = spdk_conf_first_section(g_bdevperf_conf);
	     s != NULL;
	     s = spdk_conf_next_section(s)) {
		config = calloc(1, sizeof(*config));
		if (config == NULL) {
			fprintf(stderr, "Unable to allocate memory for job config\n");
			return 1;
		}

		config->name = spdk_conf_section_get_name(s);
		is_global = strcmp(config->name, "global") == 0;

		if (is_global) {
			global_config = global_default_config;
		}

		config->filename = spdk_conf_section_get_val(s, "filename");
		if (config->filename == NULL) {
			config->filename = global_config.filename;
		}
		if (!is_global) {
			if (config->filename == NULL) {
				fprintf(stderr, "Job '%s' expects 'filename' parameter\n", config->name);
				goto error;
			} else if (strnlen(config->filename, BDEVPERF_CONFIG_MAX_FILENAME)
				   >= BDEVPERF_CONFIG_MAX_FILENAME) {
				fprintf(stderr,
					"filename for '%s' job is too long. Max length is %d\n",
					config->name, BDEVPERF_CONFIG_MAX_FILENAME);
				goto error;
			}
		}

		cpumask = spdk_conf_section_get_val(s, "cpumask");
		if (cpumask == NULL) {
			config->cpumask = global_config.cpumask;
		} else if (spdk_cpuset_parse(&config->cpumask, cpumask)) {
			fprintf(stderr, "Job '%s' has bad 'cpumask' value\n", config->name);
			goto error;
		}

		config->bs = parse_uint_option(s, "bs", global_config.bs);
		if (config->bs == BDEVPERF_CONFIG_ERROR) {
			goto error;
		} else if (config->bs == 0) {
			fprintf(stderr, "'bs' of job '%s' must be greater than 0\n", config->name);
			goto error;
		}

		config->iodepth = parse_uint_option(s, "iodepth", global_config.iodepth);
		if (config->iodepth == BDEVPERF_CONFIG_ERROR) {
			goto error;
		} else if (config->iodepth == 0) {
			fprintf(stderr,
				"'iodepth' of job '%s' must be greater than 0\n",
				config->name);
			goto error;
		}

		config->rwmixread = parse_uint_option(s, "rwmixread", global_config.rwmixread);
		if (config->rwmixread == BDEVPERF_CONFIG_ERROR) {
			goto error;
		} else if (config->rwmixread > 100) {
			fprintf(stderr,
				"'rwmixread' value of '%s' job is not in 0-100 range\n",
				config->name);
			goto error;
		}

		config->offset = parse_uint_option(s, "offset", global_config.offset);
		if (config->offset == BDEVPERF_CONFIG_ERROR) {
			goto error;
		}

		val = parse_uint_option(s, "length", global_config.length);
		if (val == BDEVPERF_CONFIG_ERROR) {
			goto error;
		}
		config->length = val;

		rw = spdk_conf_section_get_val(s, "rw");
		config->rw = parse_rw(rw, global_config.rw);
		if ((int)config->rw == BDEVPERF_CONFIG_ERROR) {
			fprintf(stderr, "Job '%s' has bad 'rw' value\n", config->name);
			goto error;
		} else if (!is_global && (int)config->rw == BDEVPERF_CONFIG_UNDEFINED) {
			fprintf(stderr, "Job '%s' has no 'rw' assigned\n", config->name);
			goto error;
		}

		if (is_global) {
			config_set_cli_args(config);
			global_config = *config;
			free(config);
		} else {
			TAILQ_INSERT_TAIL(&job_config_list, config, link);
			n++;
		}
	}

	printf("Using job config with %d jobs\n", n);
	return 0;
error:
	free(config);
	return 1;
}

static void
bdevperf_run(void *arg1)
{
	uint32_t i;

	g_main_thread = spdk_get_thread();

	spdk_cpuset_zero(&g_all_cpuset);
	SPDK_ENV_FOREACH_CORE(i) {
		spdk_cpuset_set_cpu(&g_all_cpuset, i, true);
	}

	if (g_wait_for_tests) {
		/* Do not perform any tests until RPC is received */
		return;
	}

	bdevperf_construct_job_configs();
}

static void
rpc_perform_tests_reset(void)
{
	/* Reset g_run_rc to 0 for the next test run. */
	g_run_rc = 0;

	/* Reset g_stats to 0 for the next test run. */
	memset(&g_stats, 0, sizeof(g_stats));

	/* Reset g_show_performance_period_num to 0 for the next test run. */
	g_show_performance_period_num = 0;
}

static void
rpc_perform_tests_cb(void)
{
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_request *request = g_request;

	g_request = NULL;

	if (g_run_rc == 0) {
		w = spdk_jsonrpc_begin_result(request);
		spdk_json_write_uint32(w, g_run_rc);
		spdk_jsonrpc_end_result(request, w);
	} else {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "bdevperf failed with error %s", spdk_strerror(-g_run_rc));
	}

	rpc_perform_tests_reset();
}

static void
rpc_perform_tests(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "perform_tests method requires no parameters");
		return;
	}
	if (g_request != NULL) {
		fprintf(stderr, "Another test is already in progress.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-EINPROGRESS));
		return;
	}
	g_request = request;

	/* Only construct job configs at the first test run.  */
	if (TAILQ_EMPTY(&job_config_list)) {
		bdevperf_construct_job_configs();
	} else {
		bdevperf_construct_jobs();
	}
}
SPDK_RPC_REGISTER("perform_tests", rpc_perform_tests, SPDK_RPC_RUNTIME)

static void
_bdevperf_job_drain(void *ctx)
{
	bdevperf_job_drain(ctx);
}

static void
spdk_bdevperf_shutdown_cb(void)
{
	g_shutdown = true;
	struct bdevperf_job *job, *tmp;

	if (g_bdevperf.running_jobs == 0) {
		bdevperf_test_done(NULL);
		return;
	}

	/* Iterate jobs to stop all I/O */
	TAILQ_FOREACH_SAFE(job, &g_bdevperf.jobs, link, tmp) {
		spdk_thread_send_msg(job->thread, _bdevperf_job_drain, job);
	}
}

static int
bdevperf_parse_arg(int ch, char *arg)
{
	long long tmp;

	if (ch == 'w') {
		g_workload_type = optarg;
	} else if (ch == 'T') {
		g_job_bdev_name = optarg;
	} else if (ch == 'z') {
		g_wait_for_tests = true;
	} else if (ch == 'Z') {
		g_zcopy = true;
	} else if (ch == 'X') {
		g_abort = true;
	} else if (ch == 'C') {
		g_multithread_mode = true;
	} else if (ch == 'f') {
		g_continue_on_failure = true;
	} else if (ch == 'j') {
		g_bdevperf_conf_file = optarg;
	} else if (ch == 'F') {
		char *endptr;

		errno = 0;
		g_zipf_theta = strtod(optarg, &endptr);
		if (errno || optarg == endptr || g_zipf_theta < 0) {
			fprintf(stderr, "Illegal zipf theta value %s\n", optarg);
			return -EINVAL;
		}
	} else if (ch == 'l') {
		g_latency_display_level++;
	} else {
		tmp = spdk_strtoll(optarg, 10);
		if (tmp < 0) {
			fprintf(stderr, "Parse failed for the option %c.\n", ch);
			return tmp;
		} else if (tmp >= INT_MAX) {
			fprintf(stderr, "Parsed option was too large %c.\n", ch);
			return -ERANGE;
		}

		switch (ch) {
		case 'q':
			g_queue_depth = tmp;
			break;
		case 'o':
			g_io_size = tmp;
			break;
		case 't':
			g_time_in_sec = tmp;
			break;
		case 'k':
			g_timeout_in_sec = tmp;
			break;
		case 'M':
			g_rw_percentage = tmp;
			g_mix_specified = true;
			break;
		case 'P':
			g_show_performance_ema_period = tmp;
			break;
		case 'S':
			g_show_performance_real_time = 1;
			g_show_performance_period_in_usec = tmp * SPDK_SEC_TO_USEC;
			break;
		default:
			return -EINVAL;
		}
	}
	return 0;
}

static void
bdevperf_usage(void)
{
	printf(" -q <depth>                io depth\n");
	printf(" -o <size>                 io size in bytes\n");
	printf(" -w <type>                 io pattern type, must be one of (read, write, randread, randwrite, rw, randrw, verify, reset, unmap, flush)\n");
	printf(" -t <time>                 time in seconds\n");
	printf(" -k <timeout>              timeout in seconds to detect starved I/O (default is 0 and disabled)\n");
	printf(" -M <percent>              rwmixread (100 for reads, 0 for writes)\n");
	printf(" -P <num>                  number of moving average period\n");
	printf("\t\t(If set to n, show weighted mean of the previous n IO/s in real time)\n");
	printf("\t\t(Formula: M = 2 / (n + 1), EMA[i+1] = IO/s * M + (1 - M) * EMA[i])\n");
	printf("\t\t(only valid with -S)\n");
	printf(" -S <period>               show performance result in real time every <period> seconds\n");
	printf(" -T <bdev>                 bdev to run against. Default: all available bdevs.\n");
	printf(" -f                        continue processing I/O even after failures\n");
	printf(" -F <zipf theta>           use zipf distribution for random I/O\n");
	printf(" -Z                        enable using zcopy bdev API for read or write I/O\n");
	printf(" -z                        start bdevperf, but wait for RPC to start tests\n");
	printf(" -X                        abort timed out I/O\n");
	printf(" -C                        enable every core to send I/Os to each bdev\n");
	printf(" -j <filename>             use job config file\n");
	printf(" -l                        display latency histogram, default: disable. -l display summary, -ll display details\n");
}

static int
verify_test_params(struct spdk_app_opts *opts)
{
	/* When RPC is used for starting tests and
	 * no rpc_addr was configured for the app,
	 * use the default address. */
	if (g_wait_for_tests && opts->rpc_addr == NULL) {
		opts->rpc_addr = SPDK_DEFAULT_RPC_ADDR;
	}

	if (!g_bdevperf_conf_file && g_queue_depth <= 0) {
		goto out;
	}
	if (!g_bdevperf_conf_file && g_io_size <= 0) {
		goto out;
	}
	if (!g_bdevperf_conf_file && !g_workload_type) {
		goto out;
	}
	if (g_time_in_sec <= 0) {
		goto out;
	}
	g_time_in_usec = g_time_in_sec * SPDK_SEC_TO_USEC;

	if (g_timeout_in_sec < 0) {
		goto out;
	}

	if (g_abort && !g_timeout_in_sec) {
		printf("Timeout must be set for abort option, Ignoring g_abort\n");
	}

	if (g_show_performance_ema_period > 0 &&
	    g_show_performance_real_time == 0) {
		fprintf(stderr, "-P option must be specified with -S option\n");
		return 1;
	}

	if (g_io_size > SPDK_BDEV_LARGE_BUF_MAX_SIZE) {
		printf("I/O size of %d is greater than zero copy threshold (%d).\n",
		       g_io_size, SPDK_BDEV_LARGE_BUF_MAX_SIZE);
		printf("Zero copy mechanism will not be used.\n");
		g_zcopy = false;
	}

	if (g_bdevperf_conf_file) {
		/* workload_type verification happens during config file parsing */
		return 0;
	}

	if (!strcmp(g_workload_type, "verify") ||
	    !strcmp(g_workload_type, "reset")) {
		g_rw_percentage = 50;
		if (g_io_size > SPDK_BDEV_LARGE_BUF_MAX_SIZE) {
			fprintf(stderr, "Unable to exceed max I/O size of %d for verify. (%d provided).\n",
				SPDK_BDEV_LARGE_BUF_MAX_SIZE, g_io_size);
			return 1;
		}
		g_verify = true;
		if (!strcmp(g_workload_type, "reset")) {
			g_reset = true;
		}
	}

	if (!strcmp(g_workload_type, "read") ||
	    !strcmp(g_workload_type, "randread") ||
	    !strcmp(g_workload_type, "write") ||
	    !strcmp(g_workload_type, "randwrite") ||
	    !strcmp(g_workload_type, "verify") ||
	    !strcmp(g_workload_type, "reset") ||
	    !strcmp(g_workload_type, "unmap") ||
	    !strcmp(g_workload_type, "write_zeroes") ||
	    !strcmp(g_workload_type, "flush")) {
		if (g_mix_specified) {
			fprintf(stderr, "Ignoring -M option... Please use -M option"
				" only when using rw or randrw.\n");
		}
	}

	if (!strcmp(g_workload_type, "rw") ||
	    !strcmp(g_workload_type, "randrw")) {
		if (g_rw_percentage < 0 || g_rw_percentage > 100) {
			fprintf(stderr,
				"-M must be specified to value from 0 to 100 "
				"for rw or randrw.\n");
			return 1;
		}
	}

	return 0;
out:
	spdk_app_usage();
	bdevperf_usage();
	return 1;
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc;

	/* Use the runtime PID to set the random seed */
	srand(getpid());

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "bdevperf";
	opts.rpc_addr = NULL;
	opts.shutdown_cb = spdk_bdevperf_shutdown_cb;

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "Zzfq:o:t:w:k:CF:M:P:S:T:Xlj:", NULL,
				      bdevperf_parse_arg, bdevperf_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		return rc;
	}

	if (read_job_config()) {
		free_job_config();
		return 1;
	}

	if (verify_test_params(&opts) != 0) {
		free_job_config();
		exit(1);
	}

	rc = spdk_app_start(&opts, bdevperf_run, NULL);

	spdk_app_fini();
	free_job_config();
	return rc;
}
