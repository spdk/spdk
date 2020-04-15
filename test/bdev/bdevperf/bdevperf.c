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

#include "spdk/bdev.h"
#include "spdk/accel_engine.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/rpc.h"
#include "spdk/bit_array.h"

struct bdevperf_task {
	struct iovec			iov;
	struct bdevperf_job		*job;
	struct spdk_bdev_io		*bdev_io;
	void				*buf;
	void				*md_buf;
	uint64_t			offset_blocks;
	enum spdk_bdev_io_type		io_type;
	TAILQ_ENTRY(bdevperf_task)	link;
	struct spdk_bdev_io_wait_entry	bdev_io_wait;
};

static const char *g_workload_type = NULL;
static int g_io_size = 0;
/* initialize to invalid value so we can detect if user overrides it. */
static int g_rw_percentage = -1;
static int g_is_random;
static bool g_verify = false;
static bool g_reset = false;
static bool g_continue_on_failure = false;
static bool g_unmap = false;
static bool g_write_zeroes = false;
static bool g_flush = false;
static int g_queue_depth = 0;
static uint64_t g_time_in_usec;
static int g_show_performance_real_time = 0;
static uint64_t g_show_performance_period_in_usec = 1000000;
static uint64_t g_show_performance_period_num = 0;
static uint64_t g_show_performance_ema_period = 0;
static int g_run_rc = 0;
static bool g_shutdown = false;
static uint64_t g_shutdown_tsc;
static bool g_zcopy = true;
static struct spdk_thread *g_master_thread;
static int g_time_in_sec = 0;
static bool g_mix_specified = false;
static const char *g_job_bdev_name;
static bool g_wait_for_tests = false;
static struct spdk_jsonrpc_request *g_request = NULL;
static bool g_multithread_mode = false;
static uint32_t g_core_ordinal = 0;
pthread_mutex_t g_ordinal_lock = PTHREAD_MUTEX_INITIALIZER;

static struct spdk_poller *g_perf_timer = NULL;

static void bdevperf_submit_single(struct bdevperf_job *job, struct bdevperf_task *task);
static void rpc_perform_tests_cb(void);

struct bdevperf_job {
	char				*name;
	struct spdk_bdev		*bdev;
	struct spdk_bdev_desc		*bdev_desc;
	struct spdk_io_channel		*ch;
	TAILQ_ENTRY(bdevperf_job)	link;
	struct bdevperf_reactor		*reactor;
	uint64_t			io_completed;
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
	TAILQ_HEAD(, bdevperf_task)	task_list;
};

struct bdevperf_reactor {
	struct spdk_thread		*thread;
	TAILQ_HEAD(, bdevperf_job)	jobs;
	uint32_t			lcore;
	uint32_t			multiplier;
	TAILQ_ENTRY(bdevperf_reactor)	link;
};

struct spdk_bdevperf {
	TAILQ_HEAD(, bdevperf_reactor)	reactors;
	uint32_t			num_reactors;
	uint32_t			running_jobs;
};

static struct spdk_bdevperf g_bdevperf = {
	.reactors = TAILQ_HEAD_INITIALIZER(g_bdevperf.reactors),
	.num_reactors = 0,
	.running_jobs = 0,
};

struct bdevperf_reactor *g_next_reactor;

static bool g_performance_dump_active = false;

struct bdevperf_aggregate_stats {
	uint64_t			io_time_in_usec;
	uint64_t			ema_period;
	double				total_io_per_second;
	double				total_mb_per_second;
};

static struct bdevperf_aggregate_stats g_stats = {};

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
	return (double)job->io_completed * 1000000 / io_time_in_usec;
}

static double
get_ema_io_per_second(struct bdevperf_job *job, uint64_t ema_period)
{
	double io_completed, io_per_second;

	io_completed = job->io_completed;
	io_per_second = (double)(io_completed - job->prev_io_completed) * 1000000
			/ g_show_performance_period_in_usec;
	job->prev_io_completed = io_completed;

	job->ema_io_per_second += (io_per_second - job->ema_io_per_second) * 2
				  / (ema_period + 1);
	return job->ema_io_per_second;
}

static void
performance_dump_job(struct bdevperf_aggregate_stats *stats, struct bdevperf_job *job)
{
	double io_per_second, mb_per_second;

	printf("\r Thread name: %s\n", spdk_thread_get_name(job->reactor->thread));
	printf("\r Core Mask: 0x%s\n", spdk_cpuset_fmt(spdk_thread_get_cpumask(job->reactor->thread)));

	if (stats->ema_period == 0) {
		io_per_second = get_cma_io_per_second(job, stats->io_time_in_usec);
	} else {
		io_per_second = get_ema_io_per_second(job, stats->ema_period);
	}
	mb_per_second = io_per_second * g_io_size / (1024 * 1024);
	printf("\r %-20s: %10.2f IOPS %10.2f MiB/s\n",
	       job->name, io_per_second, mb_per_second);
	stats->total_io_per_second += io_per_second;
	stats->total_mb_per_second += mb_per_second;
}

static void
generate_data(void *buf, int buf_len, int block_size, void *md_buf, int md_size,
	      int num_blocks, int seed)
{
	int offset_blocks = 0, md_offset, data_block_size;

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
		memset(buf, seed, data_block_size);
		memset(md_buf, seed, md_size);
		buf += block_size;
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
_bdevperf_fini_thread_done(struct spdk_io_channel_iter *i, int status)
{
	spdk_io_device_unregister(&g_bdevperf, NULL);

	spdk_app_stop(g_run_rc);
}

static void
_bdevperf_fini_thread(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch;
	struct bdevperf_reactor *reactor;

	ch = spdk_io_channel_iter_get_channel(i);
	reactor = spdk_io_channel_get_ctx(ch);

	TAILQ_REMOVE(&g_bdevperf.reactors, reactor, link);

	spdk_put_io_channel(ch);

	spdk_for_each_channel_continue(i, 0);
}

static void
bdevperf_fini(void)
{
	spdk_for_each_channel(&g_bdevperf, _bdevperf_fini_thread, NULL,
			      _bdevperf_fini_thread_done);
}

static void
bdevperf_test_done(void *ctx)
{
	struct bdevperf_reactor *reactor;
	struct bdevperf_job *job, *jtmp;
	struct bdevperf_task *task, *ttmp;

	if (g_time_in_usec && !g_run_rc) {
		g_stats.io_time_in_usec = g_time_in_usec;

		if (g_performance_dump_active) {
			spdk_thread_send_msg(spdk_get_thread(), bdevperf_test_done, NULL);
			return;
		}
	} else {
		printf("Job run time less than one microsecond, no performance data will be shown\n");
	}

	if (g_show_performance_real_time) {
		spdk_poller_unregister(&g_perf_timer);
	}

	if (g_shutdown) {
		g_time_in_usec = g_shutdown_tsc * 1000000 / spdk_get_ticks_hz();
		printf("Received shutdown signal, test time was about %.6f seconds\n",
		       (double)g_time_in_usec / 1000000);
	}

	TAILQ_FOREACH(reactor, &g_bdevperf.reactors, link) {
		TAILQ_FOREACH_SAFE(job, &reactor->jobs, link, jtmp) {
			TAILQ_REMOVE(&reactor->jobs, job, link);

			performance_dump_job(&g_stats, job);

			TAILQ_FOREACH_SAFE(task, &job->task_list, link, ttmp) {
				TAILQ_REMOVE(&job->task_list, task, link);
				spdk_free(task->buf);
				spdk_free(task->md_buf);
				free(task);
			}

			if (g_verify) {
				spdk_bit_array_free(&job->outstanding);
			}

			free(job->name);
			free(job);
		}
	}

	printf("\r =====================================================\n");
	printf("\r %-20s: %10.2f IOPS %10.2f MiB/s\n",
	       "Total", g_stats.total_io_per_second, g_stats.total_mb_per_second);
	fflush(stdout);

	if (g_request && !g_shutdown) {
		rpc_perform_tests_cb();
	} else {
		bdevperf_fini();
	}
}

static void
bdevperf_job_end(void *ctx)
{
	assert(g_master_thread == spdk_get_thread());

	if (--g_bdevperf.running_jobs == 0) {
		bdevperf_test_done(NULL);
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
	if (g_reset) {
		spdk_poller_unregister(&job->reset_timer);
	}

	job->is_draining = true;

	return -1;
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

	job = task->job;
	md_check = spdk_bdev_get_dif_type(job->bdev) == SPDK_DIF_DISABLE;

	if (!success) {
		if (!g_reset && !g_continue_on_failure) {
			bdevperf_job_drain(job);
			g_run_rc = -1;
			printf("task offset: %lu on job bdev=%s fails\n",
			       task->offset_blocks, job->name);
		}
	} else if (g_verify || g_reset) {
		spdk_bdev_io_get_iovec(bdev_io, &iovs, &iovcnt);
		assert(iovcnt == 1);
		assert(iovs != NULL);
		if (!verify_data(task->buf, job->buf_size, iovs[0].iov_base, iovs[0].iov_len,
				 spdk_bdev_get_block_size(job->bdev),
				 task->md_buf, spdk_bdev_io_get_md_buf(bdev_io),
				 spdk_bdev_get_md_size(job->bdev),
				 job->io_size_blocks, md_check)) {
			printf("Buffer mismatch! Target: %s Disk Offset: %lu\n", job->name, task->offset_blocks);
			printf("   First dword expected 0x%x got 0x%x\n", *(int *)task->buf, *(int *)iovs[0].iov_base);
			bdevperf_job_drain(job);
			g_run_rc = -1;
		}
	}

	job->current_queue_depth--;

	if (success) {
		job->io_completed++;
	}

	if (g_verify) {
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
		TAILQ_INSERT_TAIL(&job->task_list, task, link);
		if (job->current_queue_depth == 0) {
			spdk_put_io_channel(job->ch);
			spdk_bdev_close(job->bdev_desc);
			spdk_thread_send_msg(g_master_thread, bdevperf_job_end, NULL);
		}
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
	if (spdk_bdev_is_md_separate(job->bdev)) {
		rc = spdk_bdev_read_blocks_with_md(job->bdev_desc, job->ch, NULL, NULL,
						   task->offset_blocks, job->io_size_blocks,
						   bdevperf_complete, task);
	} else {
		rc = spdk_bdev_read_blocks(job->bdev_desc, job->ch, NULL,
					   task->offset_blocks, job->io_size_blocks,
					   bdevperf_complete, task);
	}

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
			cb_fn = (g_verify || g_reset) ? bdevperf_verify_write_complete : bdevperf_complete;

			if (g_zcopy) {
				spdk_bdev_zcopy_end(task->bdev_io, true, cb_fn, task);
				return;
			} else {
				if (spdk_bdev_is_md_separate(job->bdev)) {
					rc = spdk_bdev_writev_blocks_with_md(desc, ch, &task->iov, 1,
									     task->md_buf,
									     task->offset_blocks,
									     job->io_size_blocks,
									     cb_fn, task);
				} else {
					rc = spdk_bdev_writev_blocks(desc, ch, &task->iov, 1,
								     task->offset_blocks,
								     job->io_size_blocks,
								     cb_fn, task);
				}
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
			rc = spdk_bdev_zcopy_start(desc, ch, task->offset_blocks, job->io_size_blocks,
						   true, bdevperf_zcopy_populate_complete, task);
		} else {
			if (spdk_bdev_is_md_separate(job->bdev)) {
				rc = spdk_bdev_read_blocks_with_md(desc, ch, task->buf, task->md_buf,
								   task->offset_blocks,
								   job->io_size_blocks,
								   bdevperf_complete, task);
			} else {
				rc = spdk_bdev_read_blocks(desc, ch, task->buf, task->offset_blocks,
							   job->io_size_blocks, bdevperf_complete, task);
			}
		}
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
		if (g_verify) {
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

	if (g_verify || g_reset) {
		/* When g_verify or g_reset is enabled, task->buf is used for
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

	rc = spdk_bdev_zcopy_start(job->bdev_desc, job->ch,
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

static __thread unsigned int seed = 0;

static void
bdevperf_submit_single(struct bdevperf_job *job, struct bdevperf_task *task)
{
	uint64_t offset_in_ios;

	if (g_is_random) {
		offset_in_ios = rand_r(&seed) % job->size_in_ios;
	} else {
		offset_in_ios = job->offset_in_ios++;
		if (job->offset_in_ios == job->size_in_ios) {
			job->offset_in_ios = 0;
		}

		/* Increment of offset_in_ios if there's already an outstanding IO
		 * to that location. We only need this with g_verify as random
		 * offsets are not supported with g_verify at this time.
		 */
		if (g_verify) {
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

	if (g_verify || g_reset) {
		generate_data(task->buf, job->buf_size,
			      spdk_bdev_get_block_size(job->bdev),
			      task->md_buf, spdk_bdev_get_md_size(job->bdev),
			      job->io_size_blocks, rand_r(&seed) % 256);
		if (g_zcopy) {
			bdevperf_prep_zcopy_write_task(task);
			return;
		} else {
			task->iov.iov_base = task->buf;
			task->iov.iov_len = job->buf_size;
			task->io_type = SPDK_BDEV_IO_TYPE_WRITE;
		}
	} else if (g_flush) {
		task->io_type = SPDK_BDEV_IO_TYPE_FLUSH;
	} else if (g_unmap) {
		task->io_type = SPDK_BDEV_IO_TYPE_UNMAP;
	} else if (g_write_zeroes) {
		task->io_type = SPDK_BDEV_IO_TYPE_WRITE_ZEROES;
	} else if ((g_rw_percentage == 100) ||
		   (g_rw_percentage != 0 && ((rand_r(&seed) % 100) < g_rw_percentage))) {
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
						10 * 1000000);
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
bdevperf_job_run(struct bdevperf_job *job)
{
	struct bdevperf_task *task;
	int i;

	/* Submit initial I/O for this job. Each time one
	 * completes, another will be submitted. */

	/* Start a timer to stop this I/O chain when the run is over */
	job->run_timer = SPDK_POLLER_REGISTER(bdevperf_job_drain, job, g_time_in_usec);
	if (g_reset) {
		job->reset_timer = SPDK_POLLER_REGISTER(reset_job, job,
							10 * 1000000);
	}

	for (i = 0; i < g_queue_depth; i++) {
		task = bdevperf_job_get_task(job);
		bdevperf_submit_single(job, task);
	}
}

static void
bdevperf_submit_on_reactor(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch;
	struct bdevperf_reactor *reactor;
	struct bdevperf_job *job;

	ch = spdk_io_channel_iter_get_channel(i);
	reactor = spdk_io_channel_get_ctx(ch);

	/* Submit initial I/O for each block device. Each time one
	 * completes, another will be submitted. */
	TAILQ_FOREACH(job, &reactor->jobs, link) {
		bdevperf_job_run(job);
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
_performance_dump_done(struct spdk_io_channel_iter *i, int status)
{
	struct bdevperf_aggregate_stats *stats;

	stats = spdk_io_channel_iter_get_ctx(i);

	printf("\r =====================================================\n");
	printf("\r %-20s: %10.2f IOPS %10.2f MiB/s\n",
	       "Total", stats->total_io_per_second, stats->total_mb_per_second);
	fflush(stdout);

	g_performance_dump_active = false;

	free(stats);
}

static void
_performance_dump(struct spdk_io_channel_iter *i)
{
	struct bdevperf_aggregate_stats *stats;
	struct spdk_io_channel *ch;
	struct bdevperf_reactor *reactor;
	struct bdevperf_job *job;

	stats = spdk_io_channel_iter_get_ctx(i);
	ch = spdk_io_channel_iter_get_channel(i);
	reactor = spdk_io_channel_get_ctx(ch);

	if (TAILQ_EMPTY(&reactor->jobs)) {
		goto exit;
	}

	TAILQ_FOREACH(job, &reactor->jobs, link) {
		performance_dump_job(stats, job);
	}

	fflush(stdout);

exit:
	spdk_for_each_channel_continue(i, 0);
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

	g_show_performance_period_num++;

	stats->io_time_in_usec = g_show_performance_period_num * g_show_performance_period_in_usec;
	stats->ema_period = g_show_performance_ema_period;

	spdk_for_each_channel(&g_bdevperf, _performance_dump, stats,
			      _performance_dump_done);
	return -1;
}

static void
bdevperf_test(void)
{
	printf("Running I/O for %" PRIu64 " seconds...\n", g_time_in_usec / 1000000);
	fflush(stdout);

	/* Start a timer to dump performance numbers */
	g_shutdown_tsc = spdk_get_ticks();
	if (g_show_performance_real_time) {
		g_perf_timer = SPDK_POLLER_REGISTER(performance_statistics_thread, NULL,
						    g_show_performance_period_in_usec);
	}

	/* Iterate reactors to start all I/O */
	spdk_for_each_channel(&g_bdevperf, bdevperf_submit_on_reactor, NULL, NULL);
}

static void
bdevperf_bdev_removed(void *arg)
{
	struct bdevperf_job *job = arg;

	assert(spdk_io_channel_get_thread(spdk_io_channel_from_ctx(job->reactor)) ==
	       spdk_get_thread());

	bdevperf_job_drain(job);
}

static uint32_t g_construct_job_count = 0;

static void
_bdevperf_construct_job_done(void *ctx)
{
	/* Update g_bdevperf.running_jobs on the master thread. */
	g_bdevperf.running_jobs++;

	if (--g_construct_job_count == 0) {
		if (g_run_rc != 0) {
			/* Something failed. */
			bdevperf_test_done(NULL);
			return;
		}

		/* Ready to run the test */
		bdevperf_test();
	}
}

static void
_bdevperf_construct_job(void *ctx)
{
	struct bdevperf_job *job = ctx;
	int rc;

	rc = spdk_bdev_open(job->bdev, true, bdevperf_bdev_removed, job, &job->bdev_desc);
	if (rc != 0) {
		SPDK_ERRLOG("Could not open leaf bdev %s, error=%d\n", spdk_bdev_get_name(job->bdev), rc);
		g_run_rc = -EINVAL;
		goto end;
	}

	job->ch = spdk_bdev_get_io_channel(job->bdev_desc);
	if (!job->ch) {
		SPDK_ERRLOG("Could not get io_channel for device %s, error=%d\n", spdk_bdev_get_name(job->bdev),
			    rc);
		g_run_rc = -ENOMEM;
		goto end;
	}

end:
	spdk_thread_send_msg(g_master_thread, _bdevperf_construct_job_done, NULL);
}

static int
bdevperf_construct_job(struct spdk_bdev *bdev, struct bdevperf_reactor *reactor)
{
	struct bdevperf_job *job;
	struct bdevperf_task *task;
	int block_size, data_block_size;
	int rc;
	int task_num, n;

	/* This function runs on the master thread. */
	assert(g_master_thread == spdk_get_thread());

	block_size = spdk_bdev_get_block_size(bdev);
	data_block_size = spdk_bdev_get_data_block_size(bdev);

	if (g_unmap && !spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
		printf("Skipping %s because it does not support unmap\n", spdk_bdev_get_name(bdev));
		return -ENOTSUP;
	}

	if ((g_io_size % data_block_size) != 0) {
		SPDK_ERRLOG("IO size (%d) is not multiples of data block size of bdev %s (%"PRIu32")\n",
			    g_io_size, spdk_bdev_get_name(bdev), data_block_size);
		return -ENOTSUP;
	}

	job = calloc(1, sizeof(struct bdevperf_job));
	if (!job) {
		fprintf(stderr, "Unable to allocate memory for new job.\n");
		return -ENOMEM;
	}

	job->name = strdup(spdk_bdev_get_name(bdev));
	if (!job->name) {
		fprintf(stderr, "Unable to allocate memory for job name.\n");
		free(job);
		return -ENOMEM;
	}

	job->bdev = bdev;
	job->io_size_blocks = g_io_size / data_block_size;
	job->buf_size = job->io_size_blocks * block_size;

	if (spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_REFTAG)) {
		job->dif_check_flags |= SPDK_DIF_FLAGS_REFTAG_CHECK;
	}
	if (spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_GUARD)) {
		job->dif_check_flags |= SPDK_DIF_FLAGS_GUARD_CHECK;
	}

	job->size_in_ios = spdk_bdev_get_num_blocks(bdev) / job->io_size_blocks;
	job->offset_in_ios = 0;

	if (g_multithread_mode) {
		job->size_in_ios = job->size_in_ios / g_bdevperf.num_reactors;
		job->ios_base = reactor->multiplier * job->size_in_ios;
	} else {
		job->ios_base = 0;
	}

	if (g_verify) {
		job->outstanding = spdk_bit_array_create(job->size_in_ios);
		if (job->outstanding == NULL) {
			SPDK_ERRLOG("Could not create outstanding array bitmap for bdev %s\n",
				    spdk_bdev_get_name(bdev));
			free(job->name);
			free(job);
			return -ENOMEM;
		}
	}

	TAILQ_INIT(&job->task_list);

	task_num = g_queue_depth;
	if (g_reset) {
		task_num += 1;
	}

	TAILQ_INSERT_TAIL(&reactor->jobs, job, link);

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

	job->reactor = reactor;

	g_construct_job_count++;

	rc = spdk_thread_send_msg(reactor->thread, _bdevperf_construct_job, job);
	assert(rc == 0);

	return rc;
}

static void
bdevperf_construct_multithread_jobs(void)
{
	struct spdk_bdev *bdev;
	struct bdevperf_reactor *reactor;
	int rc;

	if (g_job_bdev_name != NULL) {
		bdev = spdk_bdev_get_by_name(g_job_bdev_name);
		if (!bdev) {
			fprintf(stderr, "Unable to find bdev '%s'\n", g_job_bdev_name);
			return;
		}

		/* Build a job for each reactor */
		TAILQ_FOREACH(reactor, &g_bdevperf.reactors, link) {
			rc = bdevperf_construct_job(bdev, reactor);
			if (rc < 0) {
				g_run_rc = rc;
				break;
			}
		}
	} else {
		bdev = spdk_bdev_first_leaf();
		while (bdev != NULL) {
			/* Build a job for each reactor */
			TAILQ_FOREACH(reactor, &g_bdevperf.reactors, link) {
				rc = bdevperf_construct_job(bdev, reactor);
				if (rc < 0) {
					g_run_rc = rc;
					break;
				}
			}

			if (g_run_rc != 0) {
				break;
			}

			bdev = spdk_bdev_next_leaf(bdev);
		}
	}
}


static struct bdevperf_reactor *
get_next_bdevperf_reactor(void)
{
	struct bdevperf_reactor *reactor;

	if (g_next_reactor == NULL) {
		g_next_reactor = TAILQ_FIRST(&g_bdevperf.reactors);
		assert(g_next_reactor != NULL);
	}

	reactor = g_next_reactor;
	g_next_reactor = TAILQ_NEXT(g_next_reactor, link);

	return reactor;
}

static void
bdevperf_construct_jobs(void)
{
	struct spdk_bdev *bdev;
	struct bdevperf_reactor *reactor;
	int rc;

	/* There are two entirely separate modes for allocating jobs. Standard mode
	 * (the default) creates one job per bdev and assigns them to reactors round-robin.
	 *
	 * The -C flag places bdevperf into "multithread" mode, meaning it creates
	 * one job per bdev per REACTOR.
	 * This runs multiple threads per bdev, effectively.
	 */

	/* Increment initial construct_jobs count so that it will never reach 0 in the middle
	 * of iteration.
	 */
	g_construct_job_count = 1;

	if (g_multithread_mode) {
		bdevperf_construct_multithread_jobs();
		goto end;
	}

	if (g_job_bdev_name != NULL) {
		bdev = spdk_bdev_get_by_name(g_job_bdev_name);
		if (bdev) {
			/* Select the reactor for this job */
			reactor = get_next_bdevperf_reactor();

			/* Construct the job */
			rc = bdevperf_construct_job(bdev, reactor);
			if (rc < 0) {
				g_run_rc = rc;
			}
		} else {
			fprintf(stderr, "Unable to find bdev '%s'\n", g_job_bdev_name);
		}
	} else {
		bdev = spdk_bdev_first_leaf();
		while (bdev != NULL) {
			/* Select the reactor for this job */
			reactor = get_next_bdevperf_reactor();

			/* Construct the job */
			rc = bdevperf_construct_job(bdev, reactor);
			if (rc < 0) {
				g_run_rc = rc;
				break;
			}

			bdev = spdk_bdev_next_leaf(bdev);
		}
	}

end:
	if (--g_construct_job_count == 0) {
		if (g_run_rc != 0) {
			/* Something failed. */
			bdevperf_test_done(NULL);
			return;
		}

		bdevperf_test();
	}
}

static int
bdevperf_reactor_create(void *io_device, void *ctx_buf)
{
	struct bdevperf_reactor *reactor = ctx_buf;

	TAILQ_INIT(&reactor->jobs);
	reactor->lcore = spdk_env_get_current_core();
	pthread_mutex_lock(&g_ordinal_lock);
	reactor->multiplier = g_core_ordinal++;
	pthread_mutex_unlock(&g_ordinal_lock);
	reactor->thread = spdk_get_thread();

	return 0;
}

static void
bdevperf_reactor_destroy(void *io_device, void *ctx_buf)
{
	struct bdevperf_reactor *reactor = ctx_buf;
	struct spdk_io_channel *ch;
	struct spdk_thread *thread;

	ch = spdk_io_channel_from_ctx(reactor);
	thread = spdk_io_channel_get_thread(ch);

	assert(thread == spdk_get_thread());

	spdk_thread_exit(thread);
}

static void
_bdevperf_init_thread_done(void *ctx)
{
	struct bdevperf_reactor *reactor = ctx;

	TAILQ_INSERT_TAIL(&g_bdevperf.reactors, reactor, link);

	assert(g_bdevperf.num_reactors < spdk_env_get_core_count());

	if (++g_bdevperf.num_reactors < spdk_env_get_core_count()) {
		return;
	}

	if (g_wait_for_tests) {
		/* Do not perform any tests until RPC is received */
		return;
	}

	bdevperf_construct_jobs();
}

static void
_bdevperf_init_thread(void *ctx)
{
	struct spdk_io_channel *ch;
	struct bdevperf_reactor *reactor;

	ch = spdk_get_io_channel(&g_bdevperf);
	reactor = spdk_io_channel_get_ctx(ch);

	spdk_thread_send_msg(g_master_thread, _bdevperf_init_thread_done, reactor);
}

static void
bdevperf_run(void *arg1)
{
	struct spdk_cpuset tmp_cpumask = {};
	uint32_t i;
	char thread_name[32];
	struct spdk_thread *thread;

	g_master_thread = spdk_get_thread();

	spdk_io_device_register(&g_bdevperf, bdevperf_reactor_create, bdevperf_reactor_destroy,
				sizeof(struct bdevperf_reactor), "bdevperf");

	/* Create threads for CPU cores active for this application, and send a
	 * message to each thread to create a reactor on it.
	 */
	SPDK_ENV_FOREACH_CORE(i) {
		spdk_cpuset_zero(&tmp_cpumask);
		spdk_cpuset_set_cpu(&tmp_cpumask, i, true);
		snprintf(thread_name, sizeof(thread_name), "bdevperf_reactor_%u", i);

		thread = spdk_thread_create(thread_name, &tmp_cpumask);
		assert(thread != NULL);

		spdk_thread_send_msg(thread, _bdevperf_init_thread, NULL);
	}
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

	/* Reset g_run_rc to 0 for the next test run. */
	g_run_rc = 0;
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

	bdevperf_construct_jobs();
}
SPDK_RPC_REGISTER("perform_tests", rpc_perform_tests, SPDK_RPC_RUNTIME)

static void
bdevperf_stop_io_on_reactor(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch;
	struct bdevperf_reactor *reactor;
	struct bdevperf_job *job;

	ch = spdk_io_channel_iter_get_channel(i);
	reactor = spdk_io_channel_get_ctx(ch);

	/* Stop I/O for each block device. */
	TAILQ_FOREACH(job, &reactor->jobs, link) {
		bdevperf_job_drain(job);
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
spdk_bdevperf_shutdown_cb(void)
{
	g_shutdown = true;

	if (TAILQ_EMPTY(&g_bdevperf.reactors)) {
		spdk_app_stop(0);
		return;
	}

	if (g_bdevperf.running_jobs == 0) {
		bdevperf_test_done(NULL);
		return;
	}

	g_shutdown_tsc = spdk_get_ticks() - g_shutdown_tsc;

	/* Send events to stop all I/O on each reactor */
	spdk_for_each_channel(&g_bdevperf, bdevperf_stop_io_on_reactor, NULL, NULL);
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
	} else if (ch == 'C') {
		g_multithread_mode = true;
	} else if (ch == 'f') {
		g_continue_on_failure = true;
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
		case 'M':
			g_rw_percentage = tmp;
			g_mix_specified = true;
			break;
		case 'P':
			g_show_performance_ema_period = tmp;
			break;
		case 'S':
			g_show_performance_real_time = 1;
			g_show_performance_period_in_usec = tmp * 1000000;
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
	printf(" -M <percent>              rwmixread (100 for reads, 0 for writes)\n");
	printf(" -P <num>                  number of moving average period\n");
	printf("\t\t(If set to n, show weighted mean of the previous n IO/s in real time)\n");
	printf("\t\t(Formula: M = 2 / (n + 1), EMA[i+1] = IO/s * M + (1 - M) * EMA[i])\n");
	printf("\t\t(only valid with -S)\n");
	printf(" -S <period>               show performance result in real time every <period> seconds\n");
	printf(" -T <bdev>                 bdev to run against. Default: all available bdevs.\n");
	printf(" -f                        continue processing I/O even after failures\n");
	printf(" -z                        start bdevperf, but wait for RPC to start tests\n");
	printf(" -C                        enable every core to send I/Os to each bdev\n");
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

	if (g_queue_depth <= 0) {
		spdk_app_usage();
		bdevperf_usage();
		return 1;
	}
	if (g_io_size <= 0) {
		spdk_app_usage();
		bdevperf_usage();
		return 1;
	}
	if (!g_workload_type) {
		spdk_app_usage();
		bdevperf_usage();
		return 1;
	}
	if (g_time_in_sec <= 0) {
		spdk_app_usage();
		bdevperf_usage();
		return 1;
	}
	g_time_in_usec = g_time_in_sec * 1000000LL;

	if (g_show_performance_ema_period > 0 &&
	    g_show_performance_real_time == 0) {
		fprintf(stderr, "-P option must be specified with -S option\n");
		return 1;
	}

	if (strcmp(g_workload_type, "read") &&
	    strcmp(g_workload_type, "write") &&
	    strcmp(g_workload_type, "randread") &&
	    strcmp(g_workload_type, "randwrite") &&
	    strcmp(g_workload_type, "rw") &&
	    strcmp(g_workload_type, "randrw") &&
	    strcmp(g_workload_type, "verify") &&
	    strcmp(g_workload_type, "reset") &&
	    strcmp(g_workload_type, "unmap") &&
	    strcmp(g_workload_type, "write_zeroes") &&
	    strcmp(g_workload_type, "flush")) {
		fprintf(stderr,
			"io pattern type must be one of\n"
			"(read, write, randread, randwrite, rw, randrw, verify, reset, unmap, flush)\n");
		return 1;
	}

	if (!strcmp(g_workload_type, "read") ||
	    !strcmp(g_workload_type, "randread")) {
		g_rw_percentage = 100;
	}

	if (!strcmp(g_workload_type, "write") ||
	    !strcmp(g_workload_type, "randwrite")) {
		g_rw_percentage = 0;
	}

	if (!strcmp(g_workload_type, "unmap")) {
		g_unmap = true;
	}

	if (!strcmp(g_workload_type, "write_zeroes")) {
		g_write_zeroes = true;
	}

	if (!strcmp(g_workload_type, "flush")) {
		g_flush = true;
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

	if (!strcmp(g_workload_type, "read") ||
	    !strcmp(g_workload_type, "write") ||
	    !strcmp(g_workload_type, "rw") ||
	    !strcmp(g_workload_type, "verify") ||
	    !strcmp(g_workload_type, "reset") ||
	    !strcmp(g_workload_type, "unmap") ||
	    !strcmp(g_workload_type, "write_zeroes")) {
		g_is_random = 0;
	} else {
		g_is_random = 1;
	}

	if (g_io_size > SPDK_BDEV_LARGE_BUF_MAX_SIZE) {
		printf("I/O size of %d is greater than zero copy threshold (%d).\n",
		       g_io_size, SPDK_BDEV_LARGE_BUF_MAX_SIZE);
		printf("Zero copy mechanism will not be used.\n");
		g_zcopy = false;
	}

	return 0;
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts);
	opts.name = "bdevperf";
	opts.rpc_addr = NULL;
	opts.reactor_mask = NULL;
	opts.shutdown_cb = spdk_bdevperf_shutdown_cb;

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "zfq:o:t:w:CM:P:S:T:", NULL,
				      bdevperf_parse_arg, bdevperf_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		return rc;
	}

	if (verify_test_params(&opts) != 0) {
		exit(1);
	}

	rc = spdk_app_start(&opts, bdevperf_run, NULL);

	spdk_app_fini();
	return rc;
}
