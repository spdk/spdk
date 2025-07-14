/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/accel.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/memory.h"
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
#define PATTERN_TYPES_STR "(read, write, randread, randwrite, rw, randrw, verify, reset, unmap, flush, write_zeroes)"
#define BDEVPERF_MAX_COREMASK_STRING 64

struct bdevperf_task {
	struct iovec			iov;
	struct bdevperf_job		*job;
	struct spdk_bdev_io		*bdev_io;
	void				*buf;
	void				*verify_buf;
	void				*md_buf;
	void				*verify_md_buf;
	uint64_t			offset_blocks;
	struct bdevperf_task		*task_to_abort;
	enum spdk_bdev_io_type		io_type;
	TAILQ_ENTRY(bdevperf_task)	link;
	struct spdk_bdev_io_wait_entry	bdev_io_wait;
};

static char *g_workload_type = NULL;
static int g_io_size = 0;
/* initialize to invalid value so we can detect if user overrides it. */
static int g_rw_percentage = -1;
static bool g_verify = false;
static bool g_reset = false;
static bool g_continue_on_failure = false;
static bool g_abort = false;
static bool g_error_to_exit = false;
static uint32_t g_queue_depth = 0;
static uint64_t g_time_in_usec;
static bool g_summarize_performance = true;
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
static bool g_random_map = false;
static bool g_unique_writes = false;
static bool g_hide_metadata = false;
static bool g_nohuge_alloc = false;

static struct spdk_cpuset g_all_cpuset;
static struct spdk_poller *g_perf_timer = NULL;

static void bdevperf_submit_single(struct bdevperf_job *job, struct bdevperf_task *task);
static void rpc_perform_tests_cb(void);
static int bdevperf_parse_arg(int ch, char *arg);
static int verify_test_params(void);
static void bdevperf_usage(void);

static uint32_t g_bdev_count = 0;
static uint32_t g_latency_display_level;

static bool g_one_thread_per_lcore = false;

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

static const char *g_rpc_log_file_name = NULL;
static FILE *g_rpc_log_file = NULL;

struct latency_info {
	uint64_t	min;
	uint64_t	max;
	uint64_t	total;
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

struct bdevperf_job {
	char				*name;
	struct spdk_bdev		*bdev;
	struct spdk_bdev_desc		*bdev_desc;
	struct spdk_io_channel		*ch;
	TAILQ_ENTRY(bdevperf_job)	link;
	struct spdk_thread		*thread;

	enum job_config_rw		workload_type;
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
	uint32_t			queue_depth;
	uint64_t			seed;

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
	uint64_t			md_buf_size;
	uint32_t			dif_check_flags;
	bool				is_draining;
	bool				md_check;
	struct spdk_poller		*run_timer;
	struct spdk_poller		*reset_timer;
	struct spdk_bit_array		*outstanding;
	struct spdk_zipf		*zipf;
	TAILQ_HEAD(, bdevperf_task)	task_list;
	uint64_t			run_time_in_usec;

	/* keep channel's histogram data before being destroyed */
	struct spdk_histogram_data	*histogram;
	struct spdk_bit_array		*random_map;

	/* counter used for generating unique write data (-U option) */
	uint32_t			write_io_count;
};

struct spdk_bdevperf {
	TAILQ_HEAD(, bdevperf_job)	jobs;
	uint32_t			running_jobs;
};

static struct spdk_bdevperf g_bdevperf = {
	.jobs = TAILQ_HEAD_INITIALIZER(g_bdevperf.jobs),
	.running_jobs = 0,
};

/* Storing values from a section of job config file */
struct job_config {
	const char			*name;
	const char			*filename;
	struct spdk_cpuset		cpumask;
	int				bs;
	int				iodepth;
	int				rwmixread;
	uint32_t			lcore;
	int64_t				offset;
	uint64_t			length;
	enum job_config_rw		rw;
	TAILQ_ENTRY(job_config)	link;
};

TAILQ_HEAD(, job_config) job_config_list
	= TAILQ_HEAD_INITIALIZER(job_config_list);

static bool g_performance_dump_active = false;

struct bdevperf_stats {
	uint64_t			io_time_in_usec;
	double				total_io_per_second;
	double				total_mb_per_second;
	double				total_failed_per_second;
	double				total_timeout_per_second;
	double				min_latency;
	double				max_latency;
	double				average_latency;
	uint64_t			total_io_completed;
	uint64_t			total_tsc;
};

struct bdevperf_aggregate_stats {
	struct bdevperf_job		*current_job;
	struct bdevperf_stats		total;
};

static struct bdevperf_aggregate_stats g_stats = {.total.min_latency = (double)UINT64_MAX};

struct lcore_thread {
	struct spdk_thread		*thread;
	uint32_t			lcore;
	TAILQ_ENTRY(lcore_thread)	link;
};

TAILQ_HEAD(, lcore_thread) g_lcore_thread_list
	= TAILQ_HEAD_INITIALIZER(g_lcore_thread_list);


static char *
parse_workload_type(enum job_config_rw ret)
{
	switch (ret) {
	case JOB_CONFIG_RW_READ:
		return "read";
	case JOB_CONFIG_RW_RANDREAD:
		return "randread";
	case JOB_CONFIG_RW_WRITE:
		return "write";
	case JOB_CONFIG_RW_RANDWRITE:
		return "randwrite";
	case JOB_CONFIG_RW_VERIFY:
		return "verify";
	case JOB_CONFIG_RW_RESET:
		return "reset";
	case JOB_CONFIG_RW_UNMAP:
		return "unmap";
	case JOB_CONFIG_RW_WRITE_ZEROES:
		return "write_zeroes";
	case JOB_CONFIG_RW_FLUSH:
		return "flush";
	case JOB_CONFIG_RW_RW:
		return "rw";
	case JOB_CONFIG_RW_RANDRW:
		return "randrw";
	default:
		fprintf(stderr, "wrong workload_type code\n");
	}

	return NULL;
}

static void *
bdevperf_alloc(size_t size, size_t alignment, uint32_t node_id)
{
	void *buf;
	int rc;

	if (!g_nohuge_alloc) {
		return spdk_zmalloc(size, alignment, NULL, node_id, SPDK_MALLOC_DMA);
	}

	size = spdk_divide_round_up(size, VALUE_4KB) * VALUE_4KB;
	buf = aligned_alloc(alignment > VALUE_4KB ? alignment : VALUE_4KB, size);
	if (buf == NULL) {
		return buf;
	}

	rc = spdk_mem_register(buf, size);
	if (rc != 0) {
		fprintf(stderr, "Failed to register region: %p-%p: %s\n", buf, (char *)buf + size,
			spdk_strerror(-rc));
		free(buf);
		return NULL;
	}

	return buf;
}

static void
bdevperf_free(void *buf, size_t size)
{
	int rc;

	if (buf == NULL) {
		return;
	}

	if (!g_nohuge_alloc) {
		spdk_free(buf);
		return;
	}

	rc = spdk_mem_unregister(buf, spdk_divide_round_up(size, VALUE_4KB) * VALUE_4KB);
	if (rc != 0) {
		fprintf(stderr, "Failed to unregister region: %p-%p: %s\n", buf, (char *)buf + size,
			spdk_strerror(-rc));
	}

	free(buf);
}

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
bdevperf_job_stats_accumulate(struct bdevperf_stats *aggr_stats,
			      struct bdevperf_stats *job_stats)
{
	aggr_stats->total_io_per_second += job_stats->total_io_per_second;
	aggr_stats->total_mb_per_second += job_stats->total_mb_per_second;
	aggr_stats->total_failed_per_second += job_stats->total_failed_per_second;
	aggr_stats->total_timeout_per_second += job_stats->total_timeout_per_second;
	aggr_stats->total_io_completed += job_stats->total_io_completed;
	aggr_stats->total_tsc += job_stats->total_tsc;

	if (job_stats->min_latency < aggr_stats->min_latency) {
		aggr_stats->min_latency = job_stats->min_latency;
	}
	if (job_stats->max_latency > aggr_stats->max_latency) {
		aggr_stats->max_latency = job_stats->max_latency;
	}
}

static void
bdevperf_job_get_stats(struct bdevperf_job *job,
		       struct bdevperf_stats *job_stats,
		       uint64_t time_in_usec,
		       uint64_t ema_period)
{
	double io_per_second, mb_per_second, failed_per_second, timeout_per_second;
	double average_latency = 0.0, min_latency, max_latency;
	uint64_t tsc_rate;
	uint64_t total_io;
	struct latency_info latency_info = {};

	if (ema_period == 0) {
		io_per_second = get_cma_io_per_second(job, time_in_usec);
	} else {
		io_per_second = get_ema_io_per_second(job, ema_period);
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

	job_stats->total_io_per_second = io_per_second;
	job_stats->total_mb_per_second = mb_per_second;
	job_stats->total_failed_per_second = failed_per_second;
	job_stats->total_timeout_per_second = timeout_per_second;
	job_stats->total_io_completed = total_io;
	job_stats->total_tsc = latency_info.total;
	job_stats->average_latency = average_latency;
	job_stats->min_latency = min_latency;
	job_stats->max_latency = max_latency;
	job_stats->io_time_in_usec = time_in_usec;
}

static void
performance_dump_job_stdout(struct bdevperf_job *job,
			    struct bdevperf_stats *job_stats)
{
	if (job->workload_type == JOB_CONFIG_RW_RW || job->workload_type == JOB_CONFIG_RW_RANDRW) {
		printf("Job: %s (Core Mask 0x%s, workload: %s, percentage: %d, depth: %d, IO size: %d)\n",
		       job->name, spdk_cpuset_fmt(spdk_thread_get_cpumask(job->thread)),
		       parse_workload_type(job->workload_type), job->rw_percentage,
		       job->queue_depth, job->io_size);
	} else {
		printf("Job: %s (Core Mask 0x%s, workload: %s, depth: %d, IO size: %d)\n",
		       job->name, spdk_cpuset_fmt(spdk_thread_get_cpumask(job->thread)),
		       parse_workload_type(job->workload_type), job->queue_depth, job->io_size);
	}


	if (job->io_failed > 0 && !job->reset && !job->continue_on_failure) {
		printf("Job: %s ended in about %.2f seconds with error\n",
		       job->name, (double)job->run_time_in_usec / SPDK_SEC_TO_USEC);
	}
	if (job->verify) {
		printf("\t Verification LBA range: start 0x%" PRIx64 " length 0x%" PRIx64 "\n",
		       job->ios_base, job->size_in_ios);
	}

	printf("\t %-20s: %10.2f %10.2f %10.2f",
	       job->name,
	       (float)job_stats->io_time_in_usec / SPDK_SEC_TO_USEC,
	       job_stats->total_io_per_second,
	       job_stats->total_mb_per_second);
	printf(" %10.2f %8.2f",
	       job_stats->total_failed_per_second,
	       job_stats->total_timeout_per_second);
	printf(" %10.2f %10.2f %10.2f\n",
	       job_stats->average_latency,
	       job_stats->min_latency,
	       job_stats->max_latency);
}

static void
performance_dump_job_json(struct bdevperf_job *job,
			  struct spdk_json_write_ctx *w,
			  struct bdevperf_stats *job_stats)
{
	char core_mask_string[BDEVPERF_MAX_COREMASK_STRING] = {0};

	spdk_json_write_named_string(w, "job", job->name);
	snprintf(core_mask_string, BDEVPERF_MAX_COREMASK_STRING,
		 "0x%s", spdk_cpuset_fmt(spdk_thread_get_cpumask(job->thread)));
	spdk_json_write_named_string(w, "core_mask", core_mask_string);
	spdk_json_write_named_string(w, "workload", parse_workload_type(job->workload_type));

	if (job->workload_type == JOB_CONFIG_RW_RW || job->workload_type == JOB_CONFIG_RW_RANDRW) {
		spdk_json_write_named_uint32(w, "percentage", job->rw_percentage);
	}

	if (g_shutdown) {
		spdk_json_write_named_string(w, "status", "terminated");
	} else if (job->io_failed > 0 && !job->reset && !job->continue_on_failure) {
		spdk_json_write_named_string(w, "status", "failed");
	} else {
		spdk_json_write_named_string(w, "status", "finished");
	}

	if (job->verify) {
		spdk_json_write_named_object_begin(w, "verify_range");
		spdk_json_write_named_uint64(w, "start", job->ios_base);
		spdk_json_write_named_uint64(w, "length", job->size_in_ios);
		spdk_json_write_object_end(w);
	}

	spdk_json_write_named_uint32(w, "queue_depth", job->queue_depth);
	spdk_json_write_named_uint32(w, "io_size", job->io_size);
	spdk_json_write_named_double(w, "runtime", (double)job_stats->io_time_in_usec / SPDK_SEC_TO_USEC);
	spdk_json_write_named_double(w, "iops", job_stats->total_io_per_second);
	spdk_json_write_named_double(w, "mibps", job_stats->total_mb_per_second);
	spdk_json_write_named_uint64(w, "io_failed", job->io_failed);
	spdk_json_write_named_uint64(w, "io_timeout", job->io_timeout);
	spdk_json_write_named_double(w, "avg_latency_us", job_stats->average_latency);
	spdk_json_write_named_double(w, "min_latency_us", job_stats->min_latency);
	spdk_json_write_named_double(w, "max_latency_us", job_stats->max_latency);
}

static void
generate_data(struct bdevperf_job *job, void *buf, void *md_buf, bool unique)
{
	int offset_blocks = 0, md_offset, data_block_size, inner_offset;
	int buf_len = job->buf_size;
	int block_size = spdk_bdev_desc_get_block_size(job->bdev_desc);
	int md_size = spdk_bdev_desc_get_md_size(job->bdev_desc);
	int num_blocks = job->io_size_blocks;

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

	if (unique) {
		uint64_t io_count = job->write_io_count++;
		unsigned int i;

		assert(md_size == 0 || md_size >= (int)sizeof(uint64_t));

		while (offset_blocks < num_blocks) {
			inner_offset = 0;
			while (inner_offset < data_block_size) {
				*(uint64_t *)buf = (io_count << 32) | (offset_blocks + inner_offset);
				inner_offset += sizeof(uint64_t);
				buf += sizeof(uint64_t);
			}
			for (i = 0; i < md_size / sizeof(uint64_t); i++) {
				((uint64_t *)md_buf)[i] = (io_count << 32) | offset_blocks;
			}
			md_buf += md_offset;
			offset_blocks++;
		}
		return;
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
			printf("data_block_size %d, num_blocks %d, offset %d\n", data_block_size, num_blocks,
			       offset_blocks);
			spdk_log_dump(stdout, "rd_buf", rd_buf, data_block_size);
			spdk_log_dump(stdout, "wr_buf", wr_buf, data_block_size);
			return false;
		}

		wr_buf += block_size;
		rd_buf += block_size;

		if (md_check) {
			if (memcmp(wr_md_buf, rd_md_buf, md_size) != 0) {
				printf("md_size %d, num_blocks %d, offset %d\n", md_size, num_blocks, offset_blocks);
				spdk_log_dump(stdout, "rd_md_buf", rd_md_buf, md_size);
				spdk_log_dump(stdout, "wr_md_buf", wr_md_buf, md_size);
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
	if (job->bdev_desc != NULL) {
		spdk_bdev_close(job->bdev_desc);
	}

	spdk_histogram_data_free(job->histogram);
	spdk_bit_array_free(&job->outstanding);
	spdk_bit_array_free(&job->random_map);
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
	struct lcore_thread *lthread, *lttmp;
	double average_latency = 0.0;
	uint64_t time_in_usec;
	int rc;
	struct spdk_json_write_ctx *w = NULL;
	struct bdevperf_stats job_stats = {0};
	struct spdk_cpuset cpu_mask;

	if (g_time_in_usec) {
		g_stats.total.io_time_in_usec = g_time_in_usec;

		if (!g_run_rc && g_performance_dump_active) {
			spdk_thread_send_msg(spdk_get_thread(), bdevperf_test_done, NULL);
			return;
		}
	}

	spdk_poller_unregister(&g_perf_timer);

	if (g_shutdown) {
		g_shutdown_tsc = spdk_get_ticks() - g_start_tsc;
		time_in_usec = g_shutdown_tsc * SPDK_SEC_TO_USEC / spdk_get_ticks_hz();
		g_time_in_usec = (g_time_in_usec > time_in_usec) ? time_in_usec : g_time_in_usec;
		printf("Received shutdown signal, test time was about %.6f seconds\n",
		       (double)g_time_in_usec / SPDK_SEC_TO_USEC);
	}
	/* Send RPC response if g_run_rc indicate success, or shutdown request was sent to bdevperf.
	 * rpc_perform_tests_cb will send error response in case of error.
	 */
	if ((g_run_rc == 0 || g_shutdown) && g_request) {
		w = spdk_jsonrpc_begin_result(g_request);
		spdk_json_write_object_begin(w);
		spdk_json_write_named_array_begin(w, "results");
	}

	printf("\n%*s\n", 107, "Latency(us)");
	printf("\r %-*s: %10s %10s %10s %10s %8s %10s %10s %10s\n",
	       28, "Device Information", "runtime(s)", "IOPS", "MiB/s", "Fail/s", "TO/s", "Average", "min", "max");


	spdk_cpuset_zero(&cpu_mask);
	TAILQ_FOREACH_SAFE(job, &g_bdevperf.jobs, link, jtmp) {
		spdk_cpuset_or(&cpu_mask, spdk_thread_get_cpumask(job->thread));
		memset(&job_stats, 0, sizeof(job_stats));
		bdevperf_job_get_stats(job, &job_stats, job->run_time_in_usec, 0);
		bdevperf_job_stats_accumulate(&g_stats.total, &job_stats);
		performance_dump_job_stdout(job, &job_stats);
		if (w) {
			spdk_json_write_object_begin(w);
			performance_dump_job_json(job, w, &job_stats);
			spdk_json_write_object_end(w);
		}
	}

	if (w) {
		spdk_json_write_array_end(w);
		spdk_json_write_named_uint32(w, "core_count", spdk_cpuset_count(&cpu_mask));
		spdk_json_write_object_end(w);
		spdk_jsonrpc_end_result(g_request, w);
	}
	printf("\r =================================================================================="
	       "=================================\n");
	printf("\r %-28s: %10s %10.2f %10.2f",
	       "Total", "", g_stats.total.total_io_per_second, g_stats.total.total_mb_per_second);
	printf(" %10.2f %8.2f",
	       g_stats.total.total_failed_per_second, g_stats.total.total_timeout_per_second);

	if (g_stats.total.total_io_completed != 0) {
		average_latency = ((double)g_stats.total.total_tsc / g_stats.total.total_io_completed) *
				  SPDK_SEC_TO_USEC /
				  spdk_get_ticks_hz();
	}
	printf(" %10.2f %10.2f %10.2f\n", average_latency, g_stats.total.min_latency,
	       g_stats.total.max_latency);

	if (g_latency_display_level == 0 || g_stats.total.total_io_completed == 0) {
		goto clean;
	}

	printf("\n Latency summary\n");
	TAILQ_FOREACH_SAFE(job, &g_bdevperf.jobs, link, jtmp) {
		printf("\r =============================================\n");
		printf("\r Job: %s (Core Mask 0x%s)\n", job->name,
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
		printf("\r Job: %s (Core Mask 0x%s)\n", job->name,
		       spdk_cpuset_fmt(spdk_thread_get_cpumask(job->thread)));

		spdk_histogram_data_iterate(job->histogram, print_bucket, NULL);
		printf("\n");
	}

clean:
	fflush(stdout);

	TAILQ_FOREACH_SAFE(job, &g_bdevperf.jobs, link, jtmp) {
		TAILQ_REMOVE(&g_bdevperf.jobs, job, link);

		if (!g_one_thread_per_lcore) {
			spdk_thread_send_msg(job->thread, job_thread_exit, NULL);
		}

		TAILQ_FOREACH_SAFE(task, &job->task_list, link, ttmp) {
			TAILQ_REMOVE(&job->task_list, task, link);
			bdevperf_free(task->buf, job->buf_size);
			bdevperf_free(task->verify_buf, job->buf_size);
			bdevperf_free(task->md_buf, job->md_buf_size);
			bdevperf_free(task->verify_md_buf, job->md_buf_size);
			free(task);
		}

		bdevperf_job_free(job);
	}

	if (g_one_thread_per_lcore) {
		TAILQ_FOREACH_SAFE(lthread, &g_lcore_thread_list, link, lttmp) {
			TAILQ_REMOVE(&g_lcore_thread_list, lthread, link);
			spdk_thread_send_msg(lthread->thread, job_thread_exit, NULL);
			free(lthread);
		}
	}

	if (g_bdevperf_conf == NULL) {
		free_job_config();
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
	struct bdevperf_job *job = ctx;

	assert(g_main_thread == spdk_get_thread());

	if (job->bdev_desc != NULL) {
		spdk_bdev_close(job->bdev_desc);
		job->bdev_desc = NULL;
	}

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
	spdk_thread_send_msg(g_main_thread, bdevperf_job_end, job);
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
bdevperf_verify_dif(struct bdevperf_task *task)
{
	struct bdevperf_job	*job = task->job;
	struct spdk_bdev	*bdev = job->bdev;
	struct spdk_dif_ctx	dif_ctx;
	struct spdk_dif_error	err_blk = {};
	int			rc;
	struct spdk_dif_ctx_init_ext_opts dif_opts;

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = spdk_bdev_get_dif_pi_format(bdev);
	rc = spdk_dif_ctx_init(&dif_ctx,
			       spdk_bdev_get_block_size(bdev),
			       spdk_bdev_get_md_size(bdev),
			       spdk_bdev_is_md_interleaved(bdev),
			       spdk_bdev_is_dif_head_of_md(bdev),
			       spdk_bdev_get_dif_type(bdev),
			       job->dif_check_flags,
			       task->offset_blocks, 0, 0, 0, 0, &dif_opts);
	if (rc != 0) {
		fprintf(stderr, "Initialization of DIF context failed\n");
		return rc;
	}

	if (spdk_bdev_is_md_interleaved(bdev)) {
		rc = spdk_dif_verify(&task->iov, 1, job->io_size_blocks, &dif_ctx, &err_blk);
	} else {
		struct iovec md_iov = {
			.iov_base	= task->md_buf,
			.iov_len	= spdk_bdev_get_md_size(bdev) * job->io_size_blocks,
		};

		rc = spdk_dix_verify(&task->iov, 1, &md_iov, job->io_size_blocks, &dif_ctx, &err_blk);
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
	uint64_t		offset_in_ios;
	int			rc;

	job = task->job;

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
		if (!verify_data(task->buf, job->buf_size,
				 task->iov.iov_base, job->buf_size,
				 spdk_bdev_desc_get_block_size(job->bdev_desc),
				 task->md_buf, spdk_bdev_io_get_md_buf(bdev_io),
				 spdk_bdev_desc_get_md_size(job->bdev_desc),
				 job->io_size_blocks, job->md_check)) {
			printf("Buffer mismatch! Target: %s Disk Offset: %" PRIu64 "\n", job->name, task->offset_blocks);
			bdevperf_job_drain(job);
			g_run_rc = -1;
		}
	} else if (job->dif_check_flags != 0) {
		if (task->io_type == SPDK_BDEV_IO_TYPE_READ && spdk_bdev_desc_get_md_size(job->bdev_desc) != 0) {
			rc = bdevperf_verify_dif(task);
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

	task->iov.iov_base = task->verify_buf;
	task->iov.iov_len = job->buf_size;

	/* Read the data back in */
	rc = spdk_bdev_readv_blocks_with_md(job->bdev_desc, job->ch, &task->iov, 1, task->verify_md_buf,
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
	struct spdk_bdev_desc	*desc = job->bdev_desc;
	struct spdk_dif_ctx	dif_ctx;
	int			rc;
	struct spdk_dif_ctx_init_ext_opts dif_opts;

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = spdk_bdev_desc_get_dif_pi_format(desc);
	rc = spdk_dif_ctx_init(&dif_ctx,
			       spdk_bdev_desc_get_block_size(desc),
			       spdk_bdev_desc_get_md_size(desc),
			       spdk_bdev_desc_is_md_interleaved(desc),
			       spdk_bdev_desc_is_dif_head_of_md(desc),
			       spdk_bdev_desc_get_dif_type(desc),
			       job->dif_check_flags,
			       task->offset_blocks, 0, 0, 0, 0, &dif_opts);
	if (rc != 0) {
		fprintf(stderr, "Initialization of DIF context failed\n");
		return rc;
	}

	if (spdk_bdev_desc_is_md_interleaved(desc)) {
		rc = spdk_dif_generate(&task->iov, 1, job->io_size_blocks, &dif_ctx);
	} else {
		struct iovec md_iov = {
			.iov_base	= task->md_buf,
			.iov_len	= spdk_bdev_desc_get_md_size(desc) * job->io_size_blocks,
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
		if (spdk_bdev_desc_get_md_size(desc) != 0 && job->dif_check_flags != 0) {
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
			rc = spdk_bdev_readv_blocks_with_md(desc, ch, &task->iov, 1,
							    task->md_buf,
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
			  spdk_bdev_desc_get_block_size(job->bdev_desc),
			  spdk_bdev_io_get_md_buf(bdev_io), task->md_buf,
			  spdk_bdev_desc_get_md_size(job->bdev_desc), job->io_size_blocks);
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
	uint32_t first_clear;

	if (job->zipf) {
		offset_in_ios = spdk_zipf_generate(job->zipf);
	} else if (job->is_random) {
		rand_value = spdk_rand_xorshift64(&job->seed);
		offset_in_ios = rand_value % job->size_in_ios;

		if (g_random_map) {
			/* Make sure, that the offset does not exceed the maximum size
			 * of the bit array (verified during job creation)
			 */
			assert(offset_in_ios < UINT32_MAX);

			first_clear = spdk_bit_array_find_first_clear(job->random_map, (uint32_t)offset_in_ios);

			if (first_clear == UINT32_MAX) {
				first_clear = spdk_bit_array_find_first_clear(job->random_map, 0);

				if (first_clear == UINT32_MAX) {
					/* If there are no more clear bits in the array, we start over
					 * and select the previously selected random value.
					 */
					spdk_bit_array_clear_mask(job->random_map);
					first_clear = (uint32_t)offset_in_ios;
				}
			}

			spdk_bit_array_set(job->random_map, first_clear);

			offset_in_ios = first_clear;
		}
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

	if (job->flush) {
		task->io_type = SPDK_BDEV_IO_TYPE_FLUSH;
	} else if (job->unmap) {
		task->io_type = SPDK_BDEV_IO_TYPE_UNMAP;
	} else if (job->write_zeroes) {
		task->io_type = SPDK_BDEV_IO_TYPE_WRITE_ZEROES;
	} else if ((job->rw_percentage == 100) ||
		   (job->rw_percentage != 0 &&
		    ((spdk_rand_xorshift64(&job->seed) % 100) < (uint64_t)job->rw_percentage))) {
		assert(!job->verify);
		task->io_type = SPDK_BDEV_IO_TYPE_READ;
		if (!g_zcopy) {
			task->iov.iov_base = task->buf;
			task->iov.iov_len = job->buf_size;
		}
	} else {
		if (job->verify || job->reset || g_unique_writes) {
			generate_data(job, task->buf, task->md_buf, g_unique_writes);
		}
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
	uint32_t i;

	/* Submit initial I/O for this job. Each time one
	 * completes, another will be submitted. */

	/* Start a timer to stop this I/O chain when the run is over */
	job->run_timer = SPDK_POLLER_REGISTER(bdevperf_job_drain_timer, job, g_time_in_usec);
	if (job->reset) {
		job->reset_timer = SPDK_POLLER_REGISTER(reset_job, job,
							10 * SPDK_SEC_TO_USEC);
	}

	for (i = 0; i < job->queue_depth; i++) {
		task = bdevperf_job_get_task(job);
		bdevperf_submit_single(job, task);
	}
}

static void
_performance_dump_done(void *ctx)
{
	struct bdevperf_aggregate_stats *aggregate = ctx;
	struct bdevperf_stats *stats = &aggregate->total;
	double average_latency;

	if (g_summarize_performance) {
		printf("%12.2f IOPS, %8.2f MiB/s", stats->total_io_per_second, stats->total_mb_per_second);
		printf("\r");
	} else {
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
	}

	fflush(stdout);

	g_performance_dump_active = false;

	free(aggregate);
}

static void
_performance_dump(void *ctx)
{
	struct bdevperf_aggregate_stats *stats = ctx;
	struct bdevperf_stats job_stats = {0};
	struct bdevperf_job *job = stats->current_job;
	uint64_t time_in_usec;

	if (job->io_failed > 0 && !job->continue_on_failure) {
		time_in_usec = job->run_time_in_usec;
	} else {
		time_in_usec = stats->total.io_time_in_usec;
	}

	bdevperf_job_get_stats(job, &job_stats, time_in_usec, g_show_performance_ema_period);
	bdevperf_job_stats_accumulate(&stats->total, &job_stats);
	if (!g_summarize_performance) {
		performance_dump_job_stdout(stats->current_job, &job_stats);
	}

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
	struct bdevperf_aggregate_stats *aggregate;
	struct bdevperf_stats *stats;


	if (g_performance_dump_active) {
		return -1;
	}

	g_performance_dump_active = true;

	aggregate = calloc(1, sizeof(*aggregate));
	if (aggregate == NULL) {
		return -1;
	}
	stats = &aggregate->total;
	stats->min_latency = (double)UINT64_MAX;

	g_show_performance_period_num++;

	stats->io_time_in_usec = g_show_performance_period_num * g_show_performance_period_in_usec;

	/* Iterate all of the jobs to gather stats
	 * These jobs will not get removed here until a final performance dump is run,
	 * so this should be safe without locking.
	 */
	aggregate->current_job = TAILQ_FIRST(&g_bdevperf.jobs);
	if (aggregate->current_job == NULL) {
		spdk_thread_send_msg(g_main_thread, _performance_dump_done, aggregate);
	} else {
		spdk_thread_send_msg(aggregate->current_job->thread, _performance_dump, aggregate);
	}

	return -1;
}

static void
bdevperf_test(void)
{
	struct bdevperf_job *job;

	if (TAILQ_EMPTY(&g_bdevperf.jobs)) {
		if (g_request) {
			spdk_jsonrpc_send_error_response_fmt(g_request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							     "No jobs defined or bdevs created");
			g_request = NULL;
		}
		return;
	}

	printf("Running I/O for %" PRIu64 " seconds...\n", g_time_in_usec / (uint64_t)SPDK_SEC_TO_USEC);
	fflush(stdout);

	/* Start a timer to dump performance numbers */
	g_start_tsc = spdk_get_ticks();
	if (!g_summarize_performance) {
		printf("%*s\n", 107, "Latency(us)");
		printf("\r %-*s: %10s %10s %10s %10s %8s %10s %10s %10s\n",
		       28, "Device Information", "runtime(s)", "IOPS", "MiB/s", "Fail/s", "TO/s", "Average", "min", "max");
	}
	if (!g_perf_timer) {
		g_perf_timer = SPDK_POLLER_REGISTER(performance_statistics_thread, NULL,
						    g_show_performance_period_in_usec);
	}

	/* Iterate jobs to start all I/O */
	TAILQ_FOREACH(job, &g_bdevperf.jobs, link) {
		spdk_bdev_set_timeout(job->bdev_desc, g_timeout_in_sec, bdevperf_timeout_cb, job);

		g_bdevperf.running_jobs++;
		spdk_thread_send_msg(job->thread, bdevperf_job_run, job);
	}
}

static void
_bdevperf_job_drain(void *ctx)
{
	bdevperf_job_drain(ctx);
}

static void
bdevperf_bdev_removed(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	struct bdevperf_job *job = event_ctx;

	if (SPDK_BDEV_EVENT_REMOVE == type) {
		spdk_thread_send_msg(job->thread, _bdevperf_job_drain, job);
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

static int
_bdevperf_enable_histogram(void *ctx, struct spdk_bdev *bdev)
{
	bool *enable = ctx;

	g_bdev_count++;

	spdk_bdev_histogram_enable(bdev, bdevperf_histogram_status_cb, NULL, *enable);

	return 0;
}

static void
bdevperf_enable_histogram(bool enable)
{
	struct spdk_bdev *bdev;
	int rc;

	/* increment initial g_bdev_count so that it will never reach 0 in the middle of iteration */
	g_bdev_count = 1;

	if (g_job_bdev_name != NULL) {
		bdev = spdk_bdev_get_by_name(g_job_bdev_name);
		if (bdev) {
			rc = _bdevperf_enable_histogram(&enable, bdev);
		} else {
			fprintf(stderr, "Unable to find bdev '%s'\n", g_job_bdev_name);
			rc = -1;
		}
	} else {
		rc = spdk_for_each_bdev_leaf(&enable, _bdevperf_enable_histogram);
	}

	bdevperf_histogram_status_cb(NULL, rc);
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
		bdevperf_enable_histogram(true);
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

	if (g_zcopy) {
		if (!spdk_bdev_io_type_supported(job->bdev, SPDK_BDEV_IO_TYPE_ZCOPY)) {
			printf("Test requires ZCOPY but bdev module does not support ZCOPY\n");
			g_run_rc = -ENOTSUP;
			goto end;
		}
	}

	job->ch = spdk_bdev_get_io_channel(job->bdev_desc);
	if (!job->ch) {
		SPDK_ERRLOG("Could not get io_channel for device %s\n", spdk_bdev_get_name(job->bdev));
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
		job->seed = spdk_rand_xorshift64_seed();
		break;
	case JOB_CONFIG_RW_RANDWRITE:
		job->is_random = true;
		job->rw_percentage = 0;
		job->seed = spdk_rand_xorshift64_seed();
		break;
	case JOB_CONFIG_RW_RW:
		job->is_random = false;
		break;
	case JOB_CONFIG_RW_RANDRW:
		job->is_random = true;
		job->seed =  spdk_rand_xorshift64_seed();
		break;
	case JOB_CONFIG_RW_RESET:
		/* Reset shares the flow with verify. */
		job->reset = true;
	/* fallthrough */
	case JOB_CONFIG_RW_VERIFY:
		job->verify = true;
		/* For verify flow read is done on write completion
		 * callback only, rw_percentage shall not be used. */
		job->rw_percentage = 0;
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
	struct spdk_bdev_open_opts opts = {};
	struct bdevperf_task *task;
	int block_size, data_block_size;
	int rc;
	int task_num, n;
	int32_t numa_id;

	job = calloc(1, sizeof(struct bdevperf_job));
	if (!job) {
		fprintf(stderr, "Unable to allocate memory for new job.\n");
		return -ENOMEM;
	}

	job->thread = thread;

	job->name = strdup(spdk_bdev_get_name(bdev));
	if (!job->name) {
		fprintf(stderr, "Unable to allocate memory for job name.\n");
		bdevperf_job_free(job);
		return -ENOMEM;
	}

	spdk_bdev_open_opts_init(&opts, sizeof(opts));
	opts.hide_metadata = g_hide_metadata;

	rc = spdk_bdev_open_ext_v2(job->name, true, bdevperf_bdev_removed, job, &opts,
				   &job->bdev_desc);
	if (rc != 0) {
		fprintf(stderr, "Could not open leaf bdev %s, error=%d\n", job->name, rc);
		bdevperf_job_free(job);
		return rc;
	}

	block_size = spdk_bdev_desc_get_block_size(job->bdev_desc);
	data_block_size = spdk_bdev_get_data_block_size(bdev);

	job->workload_type = config->rw;
	job->io_size = config->bs;
	job->rw_percentage = config->rwmixread;
	job->continue_on_failure = g_continue_on_failure;
	job->queue_depth = config->iodepth;
	job->bdev = bdev;
	job->io_size_blocks = job->io_size / data_block_size;
	job->buf_size = job->io_size_blocks * block_size;
	job->md_buf_size = spdk_bdev_get_md_size(bdev) * job->io_size_blocks;
	job->abort = g_abort;
	job_init_rw(job, config->rw);
	job->md_check = spdk_bdev_get_dif_type(job->bdev) == SPDK_DIF_DISABLE;

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

	if (spdk_bdev_desc_is_dif_check_enabled(job->bdev_desc, SPDK_DIF_CHECK_TYPE_REFTAG)) {
		job->dif_check_flags |= SPDK_DIF_FLAGS_REFTAG_CHECK;
	}
	if (spdk_bdev_desc_is_dif_check_enabled(job->bdev_desc, SPDK_DIF_CHECK_TYPE_GUARD)) {
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
		if (job->size_in_ios >= UINT32_MAX) {
			SPDK_ERRLOG("Due to constraints of verify operation, the job storage capacity is too large\n");
			bdevperf_job_free(job);
			return -ENOMEM;
		}
		job->outstanding = spdk_bit_array_create(job->size_in_ios);
		if (job->outstanding == NULL) {
			SPDK_ERRLOG("Could not create outstanding array bitmap for bdev %s\n",
				    spdk_bdev_get_name(bdev));
			bdevperf_job_free(job);
			return -ENOMEM;
		}
		if (job->queue_depth > job->size_in_ios) {
			SPDK_WARNLOG("Due to constraints of verify job, queue depth (-q, %"PRIu32") can't exceed the number of IO "
				     "requests which can be submitted to the bdev %s simultaneously (%"PRIu64"). "
				     "Queue depth is limited to %"PRIu64"\n",
				     job->queue_depth, job->name, job->size_in_ios, job->size_in_ios);
			job->queue_depth = job->size_in_ios;
		}
	}

	job->histogram = spdk_histogram_data_alloc();
	if (job->histogram == NULL) {
		fprintf(stderr, "Failed to allocate histogram\n");
		bdevperf_job_free(job);
		return -ENOMEM;
	}

	TAILQ_INIT(&job->task_list);

	if (g_random_map) {
		if (job->size_in_ios >= UINT32_MAX) {
			SPDK_ERRLOG("Due to constraints of the random map, the job storage capacity is too large\n");
			bdevperf_job_free(job);
			return -ENOMEM;
		}
		job->random_map = spdk_bit_array_create(job->size_in_ios);
		if (job->random_map == NULL) {
			SPDK_ERRLOG("Could not create random_map array bitmap for bdev %s\n",
				    spdk_bdev_get_name(bdev));
			bdevperf_job_free(job);
			return -ENOMEM;
		}
	}

	task_num = job->queue_depth;
	if (job->reset) {
		task_num += 1;
	}
	if (job->abort) {
		task_num += job->queue_depth;
	}

	TAILQ_INSERT_TAIL(&g_bdevperf.jobs, job, link);

	numa_id = spdk_bdev_get_numa_id(job->bdev);

	for (n = 0; n < task_num; n++) {
		task = calloc(1, sizeof(struct bdevperf_task));
		if (!task) {
			fprintf(stderr, "Failed to allocate task from memory\n");
			spdk_zipf_free(&job->zipf);
			return -ENOMEM;
		}

		task->buf = bdevperf_alloc(job->buf_size, spdk_bdev_get_buf_align(job->bdev),
					   numa_id);
		if (!task->buf) {
			fprintf(stderr, "Cannot allocate buf for task=%p\n", task);
			spdk_zipf_free(&job->zipf);
			free(task);
			return -ENOMEM;
		}

		if (job->verify && job->buf_size > SPDK_BDEV_LARGE_BUF_MAX_SIZE) {
			task->verify_buf = bdevperf_alloc(job->buf_size, spdk_bdev_get_buf_align(job->bdev),
							  numa_id);
			if (!task->verify_buf) {
				fprintf(stderr, "Cannot allocate buf_verify for task=%p\n", task);
				bdevperf_free(task->buf, job->buf_size);
				spdk_zipf_free(&job->zipf);
				free(task);
				return -ENOMEM;
			}

			if (spdk_bdev_is_md_separate(job->bdev)) {
				task->verify_md_buf = bdevperf_alloc(job->md_buf_size,
								     spdk_bdev_get_buf_align(job->bdev), numa_id);
				if (!task->verify_md_buf) {
					fprintf(stderr, "Cannot allocate verify_md_buf for task=%p\n", task);
					bdevperf_free(task->buf, job->buf_size);
					bdevperf_free(task->verify_buf, job->buf_size);
					spdk_zipf_free(&job->zipf);
					free(task);
					return -ENOMEM;
				}
			}
		}

		if (spdk_bdev_desc_is_md_separate(job->bdev_desc)) {
			task->md_buf = bdevperf_alloc(job->md_buf_size, 0, numa_id);
			if (!task->md_buf) {
				fprintf(stderr, "Cannot allocate md buf for task=%p\n", task);
				spdk_zipf_free(&job->zipf);
				bdevperf_free(task->verify_buf, job->buf_size);
				bdevperf_free(task->verify_md_buf, job->md_buf_size);
				bdevperf_free(task->buf, job->buf_size);
				free(task);
				return -ENOMEM;
			}
		}

		task->job = job;
		TAILQ_INSERT_TAIL(&job->task_list, task, link);
	}

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
			PATTERN_TYPES_STR "\n");
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
	     i < BDEVPERF_CONFIG_MAX_FILENAME &&
	     k < (BDEVPERF_CONFIG_MAX_FILENAME - 1);
	     i++) {
		if (filename[i] == ' ' || filename[i] == '\t') {
			continue;
		}

		out[k++] = filename[i];
	}
	out[k] = 0;

	return filename + i;
}

static struct spdk_thread *
get_lcore_thread(uint32_t lcore)
{
	struct lcore_thread *lthread;

	TAILQ_FOREACH(lthread, &g_lcore_thread_list, link) {
		if (lthread->lcore == lcore) {
			return lthread->thread;
		}
	}

	return NULL;
}

static void
create_lcore_thread(uint32_t lcore)
{
	struct lcore_thread *lthread;
	struct spdk_cpuset cpumask = {};
	char name[32];

	lthread = calloc(1, sizeof(*lthread));
	assert(lthread != NULL);

	lthread->lcore = lcore;

	snprintf(name, sizeof(name), "lcore_%u", lcore);
	spdk_cpuset_set_cpu(&cpumask, lcore, true);

	lthread->thread = spdk_thread_create(name, &cpumask);
	assert(lthread->thread != NULL);

	TAILQ_INSERT_TAIL(&g_lcore_thread_list, lthread, link);
}

static void
bdevperf_construct_jobs(void)
{
	char filename[BDEVPERF_CONFIG_MAX_FILENAME];
	struct spdk_thread *thread;
	struct job_config *config;
	struct spdk_bdev *bdev;
	const char *filenames;
	uint32_t i;
	int rc;

	if (g_one_thread_per_lcore) {
		SPDK_ENV_FOREACH_CORE(i) {
			create_lcore_thread(i);
		}
	}

	TAILQ_FOREACH(config, &job_config_list, link) {
		filenames = config->filename;

		if (!g_one_thread_per_lcore) {
			thread = construct_job_thread(&config->cpumask, config->name);
		} else {
			thread = get_lcore_thread(config->lcore);
		}
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
	config->lcore = _get_next_core();
	spdk_cpuset_zero(&config->cpumask);
	spdk_cpuset_set_cpu(&config->cpumask, config->lcore, true);
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

static int
bdevperf_construct_multithread_job_config(void *ctx, struct spdk_bdev *bdev)
{
	uint32_t *num_cores = ctx;
	uint32_t i;
	uint64_t blocks_per_job;
	int64_t offset;
	int rc;

	blocks_per_job = spdk_bdev_get_num_blocks(bdev) / *num_cores;
	offset = 0;

	SPDK_ENV_FOREACH_CORE(i) {
		rc = make_cli_job_config(spdk_bdev_get_name(bdev), offset, blocks_per_job);
		if (rc) {
			return rc;
		}

		offset += blocks_per_job;
	}

	return 0;
}

static void
bdevperf_construct_multithread_job_configs(void)
{
	struct spdk_bdev *bdev;
	uint32_t i;
	uint32_t num_cores;

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
		g_run_rc = bdevperf_construct_multithread_job_config(&num_cores, bdev);
	} else {
		g_run_rc = spdk_for_each_bdev_leaf(&num_cores, bdevperf_construct_multithread_job_config);
	}

}

static int
bdevperf_construct_job_config(void *ctx, struct spdk_bdev *bdev)
{
	/* Construct the job */
	return make_cli_job_config(spdk_bdev_get_name(bdev), 0, 0);
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
	 *
	 * Both for standard mode and "multithread" mode, if the -E flag is specified,
	 * it creates one spdk_thread PER CORE. On each core, one spdk_thread is shared by
	 * multiple jobs.
	 */

	if (g_bdevperf_conf) {
		goto end;
	}

	if (g_multithread_mode) {
		bdevperf_construct_multithread_job_configs();
	} else if (g_job_bdev_name != NULL) {
		bdev = spdk_bdev_get_by_name(g_job_bdev_name);
		if (bdev) {
			/* Construct the job */
			g_run_rc = make_cli_job_config(g_job_bdev_name, 0, 0);
		} else {
			fprintf(stderr, "Unable to find bdev '%s'\n", g_job_bdev_name);
		}
	} else {
		g_run_rc = spdk_for_each_bdev_leaf(NULL, bdevperf_construct_job_config);
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
	struct job_config *config = NULL;
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
			config = NULL;
		} else {
			TAILQ_INSERT_TAIL(&job_config_list, config, link);
			n++;
		}
	}

	if (g_rpc_log_file_name != NULL) {
		g_rpc_log_file = fopen(g_rpc_log_file_name, "a");
		if (g_rpc_log_file == NULL) {
			fprintf(stderr, "Failed to open %s\n", g_rpc_log_file_name);
			goto error;
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
	struct spdk_jsonrpc_request *request = g_request;

	g_request = NULL;

	if (g_run_rc) {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "bdevperf failed with error %s", spdk_strerror(-g_run_rc));
	}

	rpc_perform_tests_reset();
}

struct rpc_bdevperf_params {
	int		time_in_sec;
	char		*workload_type;
	uint32_t	queue_depth;
	char		*io_size;
	int		rw_percentage;
};

static const struct spdk_json_object_decoder rpc_bdevperf_params_decoders[] = {
	{"time_in_sec", offsetof(struct rpc_bdevperf_params, time_in_sec), spdk_json_decode_int32, true},
	{"workload_type", offsetof(struct rpc_bdevperf_params, workload_type), spdk_json_decode_string, true},
	{"queue_depth", offsetof(struct rpc_bdevperf_params, queue_depth), spdk_json_decode_uint32, true},
	{"io_size", offsetof(struct rpc_bdevperf_params, io_size), spdk_json_decode_string, true},
	{"rw_percentage", offsetof(struct rpc_bdevperf_params, rw_percentage), spdk_json_decode_int32, true},
};

static void
rpc_apply_bdevperf_params(struct rpc_bdevperf_params *params)
{
	if (params->workload_type) {
		/* we need to clear previously settled parameter to avoid memory leak */
		free(g_workload_type);
		g_workload_type = strdup(params->workload_type);
	}
	if (params->queue_depth) {
		g_queue_depth = params->queue_depth;
	}
	if (params->io_size) {
		bdevperf_parse_arg('o', params->io_size);
	}
	if (params->time_in_sec) {
		g_time_in_sec = params->time_in_sec;
	}
	if (params->rw_percentage) {
		g_rw_percentage = params->rw_percentage;
		g_mix_specified = true;
	} else {
		g_mix_specified = false;
	}
}

static void
rpc_perform_tests(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_bdevperf_params req = {}, backup = {};
	int rc;

	if (g_request != NULL) {
		fprintf(stderr, "Another test is already in progress.\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-EINPROGRESS));
		return;
	}

	if (params) {
		if (spdk_json_decode_object_relaxed(params, rpc_bdevperf_params_decoders,
						    SPDK_COUNTOF(rpc_bdevperf_params_decoders),
						    &req)) {
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
							 "spdk_json_decode_object failed");
			return;
		}

		if (g_workload_type) {
			backup.workload_type = strdup(g_workload_type);
		}
		backup.queue_depth = g_queue_depth;
		if (asprintf(&backup.io_size, "%d", g_io_size) < 0) {
			fprintf(stderr, "Couldn't allocate memory for queue depth");
			goto rpc_error;
		}
		backup.time_in_sec = g_time_in_sec;
		backup.rw_percentage = g_rw_percentage;

		rpc_apply_bdevperf_params(&req);

		free(req.workload_type);
		free(req.io_size);
	}

	rc = verify_test_params();

	if (rc) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_PARSE_ERROR,
						 "Invalid parameters provided");
		/* restore old params on error */
		rpc_apply_bdevperf_params(&backup);
		goto rpc_error;
	}

	g_request = request;

	/* Only construct job configs at the first test run.  */
	if (TAILQ_EMPTY(&job_config_list)) {
		bdevperf_construct_job_configs();
	} else {
		bdevperf_construct_jobs();
	}

rpc_error:
	free(backup.io_size);
	free(backup.workload_type);
}
SPDK_RPC_REGISTER("perform_tests", rpc_perform_tests, SPDK_RPC_RUNTIME)

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
		g_workload_type = strdup(arg);
	} else if (ch == 'T') {
		g_job_bdev_name = arg;
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
		g_bdevperf_conf_file = arg;
	} else if (ch == 'F') {
		char *endptr;

		errno = 0;
		g_zipf_theta = strtod(arg, &endptr);
		if (errno || arg == endptr || g_zipf_theta < 0) {
			fprintf(stderr, "Illegal zipf theta value %s\n", arg);
			return -EINVAL;
		}
	} else if (ch == 'H') {
		g_nohuge_alloc = true;
	} else if (ch == 'l') {
		g_latency_display_level++;
	} else if (ch == 'D') {
		g_random_map = true;
	} else if (ch == 'E') {
		g_one_thread_per_lcore = true;
	} else if (ch == 'J') {
		g_rpc_log_file_name = arg;
	} else if (ch == 'o') {
		uint64_t size;

		if (spdk_parse_capacity(arg, &size, NULL) != 0) {
			fprintf(stderr, "Invalid IO size: %s\n", arg);
			return -EINVAL;
		}
		g_io_size = (int)size;
	} else if (ch == 'U') {
		g_unique_writes = true;
	} else if (ch == 'N') {
		g_hide_metadata = true;
	} else {
		tmp = spdk_strtoll(arg, 10);
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
			g_summarize_performance = false;
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
	printf(" -w <type>                 io pattern type, must be one of " PATTERN_TYPES_STR "\n");
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
	printf(" -z                        start bdevperf, but wait for perform_tests RPC to start tests\n");
	printf("                           (See examples/bdev/bdevperf/bdevperf.py)\n");
	printf(" -X                        abort timed out I/O\n");
	printf(" -C                        enable every core to send I/Os to each bdev\n");
	printf(" -j <filename>             use job config file\n");
	printf(" -l                        display latency histogram, default: disable. -l display summary, -ll display details\n");
	printf(" -D                        use a random map for picking offsets not previously read or written (for all jobs)\n");
	printf(" -E                        share per lcore thread among jobs. Available only if -j is not used.\n");
	printf(" -J                        File name to open with append mode and log JSON RPC calls.\n");
	printf(" -U                        generate unique data for each write I/O, has no effect on non-write I/O\n");
	printf(" -N                        Enable hide_metadata option to each bdev\n");
	printf(" -H                        allocate non-huge data buffers\n");
}

static void
bdevperf_fini(void)
{
	free_job_config();
	free(g_workload_type);

	if (g_rpc_log_file != NULL) {
		fclose(g_rpc_log_file);
		g_rpc_log_file = NULL;
	}
}

static int
verify_test_params(void)
{
	if (!g_bdevperf_conf_file && g_queue_depth == 0) {
		goto out;
	}
	if (!g_bdevperf_conf_file && g_io_size <= 0) {
		goto out;
	}
	if (!g_bdevperf_conf_file && !g_workload_type) {
		goto out;
	}
	if (g_bdevperf_conf_file && g_one_thread_per_lcore) {
		printf("If bdevperf's config file is used, per lcore thread cannot be used\n");
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

	if (g_show_performance_ema_period > 0 && g_summarize_performance) {
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

	if (strcmp(g_workload_type, "randread") &&
	    strcmp(g_workload_type, "randwrite") &&
	    strcmp(g_workload_type, "randrw")) {
		if (g_random_map) {
			fprintf(stderr, "Ignoring -D option... Please use -D option"
				" only when using randread, randwrite or randrw.\n");
			return 1;
		}
	}

	return 0;
out:
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

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "Zzfq:o:t:w:k:CEF:HJ:M:P:S:T:Xlj:DUN", NULL,
				      bdevperf_parse_arg, bdevperf_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		return rc;
	}

	/* Set the default address if no rpc_addr was provided in args
	 * and RPC is used for starting tests */
	if (g_wait_for_tests && opts.rpc_addr == NULL) {
		opts.rpc_addr = SPDK_DEFAULT_RPC_ADDR;
	}

	if (read_job_config()) {
		bdevperf_fini();
		return 1;
	}

	if (g_rpc_log_file != NULL) {
		opts.rpc_log_file = g_rpc_log_file;
	}

	if (verify_test_params() != 0 && !g_wait_for_tests) {
		spdk_app_usage();
		bdevperf_usage();
		bdevperf_fini();
		exit(1);
	}

	rc = spdk_app_start(&opts, bdevperf_run, NULL);

	spdk_app_fini();
	bdevperf_fini();
	return rc;
}
