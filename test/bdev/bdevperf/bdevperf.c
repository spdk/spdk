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
#include "spdk/copy_engine.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/rpc.h"

struct bdevperf_task {
	struct iovec			iov;
	struct io_target		*target;
	struct spdk_bdev_io		*bdev_io;
	void				*buf;
	void				*md_buf;
	uint64_t			offset_blocks;
	enum spdk_bdev_io_type		io_type;
	TAILQ_ENTRY(bdevperf_task)	link;
	struct spdk_bdev_io_wait_entry	bdev_io_wait;
};

static const char *g_workload_type;
static int g_io_size = 0;
static uint64_t g_buf_size = 0;
/* initialize to invalid value so we can detect if user overrides it. */
static int g_rw_percentage = -1;
static int g_is_random;
static bool g_verify = false;
static bool g_reset = false;
static bool g_continue_on_failure = false;
static bool g_unmap = false;
static bool g_write_zeroes = false;
static bool g_flush = false;
static int g_queue_depth;
static uint64_t g_time_in_usec;
static int g_show_performance_real_time = 0;
static uint64_t g_show_performance_period_in_usec = 1000000;
static uint64_t g_show_performance_period_num = 0;
static uint64_t g_show_performance_ema_period = 0;
static int g_run_rc = 0;
static bool g_shutdown = false;
static uint64_t g_shutdown_tsc;
static bool g_zcopy = true;
static unsigned g_master_core;
static int g_time_in_sec;
static bool g_mix_specified;
static const char *g_target_bdev_name;
static bool g_wait_for_tests = false;
static struct spdk_jsonrpc_request *g_request = NULL;
static bool g_every_core_for_each_bdev = false;

static struct spdk_poller *g_perf_timer = NULL;

static void bdevperf_submit_single(struct io_target *target, struct bdevperf_task *task);
static void performance_dump(uint64_t io_time_in_usec, uint64_t ema_period);
static void rpc_perform_tests_cb(void);

struct io_target {
	char				*name;
	struct spdk_bdev		*bdev;
	struct spdk_bdev_desc		*bdev_desc;
	struct spdk_io_channel		*ch;
	TAILQ_ENTRY(io_target)		link;
	struct io_target_group		*group;
	unsigned			lcore;
	uint64_t			io_completed;
	uint64_t			prev_io_completed;
	double				ema_io_per_second;
	int				current_queue_depth;
	uint64_t			size_in_ios;
	uint64_t			offset_in_ios;
	uint64_t			io_size_blocks;
	uint32_t			dif_check_flags;
	bool				is_draining;
	struct spdk_poller		*run_timer;
	struct spdk_poller		*reset_timer;
	TAILQ_HEAD(, bdevperf_task)	task_list;
};

struct io_target_group {
	TAILQ_HEAD(, io_target)		targets;
	uint32_t			lcore;
	TAILQ_ENTRY(io_target_group)	link;
};

struct spdk_bdevperf {
	TAILQ_HEAD(, io_target_group)	groups;
};

static struct spdk_bdevperf g_bdevperf = {
	.groups = TAILQ_HEAD_INITIALIZER(g_bdevperf.groups),
};

struct io_target_group *g_next_tg;
static uint32_t g_target_count = 0;

/*
 * Used to determine how the I/O buffers should be aligned.
 *  This alignment will be bumped up for blockdevs that
 *  require alignment based on block length - for example,
 *  AIO blockdevs.
 */
static size_t g_min_alignment = 8;

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
bdevperf_free_target(struct io_target *target)
{
	struct bdevperf_task *task, *tmp;

	TAILQ_FOREACH_SAFE(task, &target->task_list, link, tmp) {
		TAILQ_REMOVE(&target->task_list, task, link);
		spdk_free(task->buf);
		spdk_free(task->md_buf);
		free(task);
	}

	free(target->name);
	free(target);
}

static void
bdevperf_free_targets(void)
{
	struct io_target_group *group, *tmp_group;
	struct io_target *target, *tmp_target;

	TAILQ_FOREACH_SAFE(group, &g_bdevperf.groups, link, tmp_group) {
		TAILQ_FOREACH_SAFE(target, &group->targets, link, tmp_target) {
			TAILQ_REMOVE(&group->targets, target, link);
			bdevperf_free_target(target);
		}
	}
}

static void
_end_target(struct io_target *target)
{
	spdk_poller_unregister(&target->run_timer);
	if (g_reset) {
		spdk_poller_unregister(&target->reset_timer);
	}

	target->is_draining = true;
}

static void
_target_gone(void *arg1, void *arg2)
{
	struct io_target *target = arg1;

	_end_target(target);
}

static void
bdevperf_target_gone(void *arg)
{
	struct io_target *target = arg;
	struct spdk_event *event;

	event = spdk_event_allocate(target->lcore, _target_gone, target, NULL);
	spdk_event_call(event);
}

static int
bdevperf_construct_target(struct spdk_bdev *bdev)
{
	struct io_target_group *group;
	struct io_target *target;
	size_t align;
	int block_size, data_block_size;
	int rc;

	if (g_unmap && !spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
		printf("Skipping %s because it does not support unmap\n", spdk_bdev_get_name(bdev));
		return 0;
	}

	target = malloc(sizeof(struct io_target));
	if (!target) {
		fprintf(stderr, "Unable to allocate memory for new target.\n");
		/* Return immediately because all mallocs will presumably fail after this */
		return -ENOMEM;
	}

	target->name = strdup(spdk_bdev_get_name(bdev));
	if (!target->name) {
		fprintf(stderr, "Unable to allocate memory for target name.\n");
		free(target);
		/* Return immediately because all mallocs will presumably fail after this */
		return -ENOMEM;
	}

	rc = spdk_bdev_open(bdev, true, bdevperf_target_gone, target, &target->bdev_desc);
	if (rc != 0) {
		SPDK_ERRLOG("Could not open leaf bdev %s, error=%d\n", spdk_bdev_get_name(bdev), rc);
		free(target->name);
		free(target);
		return 0;
	}

	target->bdev = bdev;
	target->io_completed = 0;
	target->current_queue_depth = 0;
	target->offset_in_ios = 0;

	block_size = spdk_bdev_get_block_size(bdev);
	data_block_size = spdk_bdev_get_data_block_size(bdev);
	target->io_size_blocks = g_io_size / data_block_size;
	if ((g_io_size % data_block_size) != 0) {
		SPDK_ERRLOG("IO size (%d) is not multiples of data block size of bdev %s (%"PRIu32")\n",
			    g_io_size, spdk_bdev_get_name(bdev), data_block_size);
		spdk_bdev_close(target->bdev_desc);
		free(target->name);
		free(target);
		return 0;
	}

	g_buf_size = spdk_max(g_buf_size, target->io_size_blocks * block_size);

	target->dif_check_flags = 0;
	if (spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_REFTAG)) {
		target->dif_check_flags |= SPDK_DIF_FLAGS_REFTAG_CHECK;
	}
	if (spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_GUARD)) {
		target->dif_check_flags |= SPDK_DIF_FLAGS_GUARD_CHECK;
	}

	target->size_in_ios = spdk_bdev_get_num_blocks(bdev) / target->io_size_blocks;
	align = spdk_bdev_get_buf_align(bdev);
	/*
	 * TODO: This should actually use the LCM of align and g_min_alignment, but
	 * it is fairly safe to assume all alignments are powers of two for now.
	 */
	g_min_alignment = spdk_max(g_min_alignment, align);

	target->is_draining = false;
	target->run_timer = NULL;
	target->reset_timer = NULL;
	TAILQ_INIT(&target->task_list);

	/* Mapping each created target to target group */
	if (g_next_tg == NULL) {
		g_next_tg = TAILQ_FIRST(&g_bdevperf.groups);
		assert(g_next_tg != NULL);
	}
	group = g_next_tg;
	g_next_tg = TAILQ_NEXT(g_next_tg, link);
	target->lcore = group->lcore;
	target->group = group;
	TAILQ_INSERT_TAIL(&group->targets, target, link);
	g_target_count++;

	return 0;
}

static void
bdevperf_construct_targets(void)
{
	struct spdk_bdev *bdev;
	int rc;
	uint8_t core_idx, core_count_for_each_bdev;

	if (g_every_core_for_each_bdev == false) {
		core_count_for_each_bdev = 1;
	} else {
		core_count_for_each_bdev = spdk_env_get_core_count();
	}

	if (g_target_bdev_name != NULL) {
		bdev = spdk_bdev_get_by_name(g_target_bdev_name);
		if (!bdev) {
			fprintf(stderr, "Unable to find bdev '%s'\n", g_target_bdev_name);
			return;
		}

		for (core_idx = 0; core_idx < core_count_for_each_bdev; core_idx++) {
			rc = bdevperf_construct_target(bdev);
			if (rc != 0) {
				return;
			}
		}
	} else {
		bdev = spdk_bdev_first_leaf();
		while (bdev != NULL) {
			for (core_idx = 0; core_idx < core_count_for_each_bdev; core_idx++) {
				rc = bdevperf_construct_target(bdev);
				if (rc != 0) {
					return;
				}
			}

			bdev = spdk_bdev_next_leaf(bdev);
		}
	}
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
	struct io_target_group *group;

	ch = spdk_io_channel_iter_get_channel(i);
	group = spdk_io_channel_get_ctx(ch);

	TAILQ_REMOVE(&g_bdevperf.groups, group, link);

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
bdevperf_test_done(void)
{
	bdevperf_free_targets();

	if (g_request && !g_shutdown) {
		rpc_perform_tests_cb();
	} else {
		bdevperf_fini();
	}
}

static void
end_run(void *arg1, void *arg2)
{
	struct io_target *target = arg1;

	spdk_bdev_close(target->bdev_desc);
	if (--g_target_count == 0) {
		if (g_show_performance_real_time) {
			spdk_poller_unregister(&g_perf_timer);
		}
		if (g_shutdown) {
			g_time_in_usec = g_shutdown_tsc * 1000000 / spdk_get_ticks_hz();
			printf("Received shutdown signal, test time is about %.6f seconds\n",
			       (double)g_time_in_usec / 1000000);
		}

		if (g_time_in_usec) {
			if (!g_run_rc) {
				performance_dump(g_time_in_usec, 0);
			}
		} else {
			printf("Test time less than one microsecond, no performance data will be shown\n");
		}

		bdevperf_test_done();
	}
}

static void
bdevperf_queue_io_wait_with_cb(struct bdevperf_task *task, spdk_bdev_io_wait_cb cb_fn)
{
	struct io_target	*target = task->target;

	task->bdev_io_wait.bdev = target->bdev;
	task->bdev_io_wait.cb_fn = cb_fn;
	task->bdev_io_wait.cb_arg = task;
	spdk_bdev_queue_io_wait(target->bdev, target->ch, &task->bdev_io_wait);
}

static void
bdevperf_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct io_target	*target;
	struct bdevperf_task	*task = cb_arg;
	struct spdk_event	*complete;
	struct iovec		*iovs;
	int			iovcnt;
	bool			md_check;

	target = task->target;
	md_check = spdk_bdev_get_dif_type(target->bdev) == SPDK_DIF_DISABLE;

	if (!success) {
		if (!g_reset && !g_continue_on_failure) {
			_end_target(target);
			g_run_rc = -1;
			printf("task offset: %lu on target bdev=%s fails\n",
			       task->offset_blocks, target->name);
		}
	} else if (g_verify || g_reset) {
		spdk_bdev_io_get_iovec(bdev_io, &iovs, &iovcnt);
		assert(iovcnt == 1);
		assert(iovs != NULL);
		if (!verify_data(task->buf, g_buf_size, iovs[0].iov_base, iovs[0].iov_len,
				 spdk_bdev_get_block_size(target->bdev),
				 task->md_buf, spdk_bdev_io_get_md_buf(bdev_io),
				 spdk_bdev_get_md_size(target->bdev),
				 target->io_size_blocks, md_check)) {
			printf("Buffer mismatch! Disk Offset: %lu\n", task->offset_blocks);
			_end_target(target);
			g_run_rc = -1;
		}
	}

	target->current_queue_depth--;

	if (success) {
		target->io_completed++;
	}

	spdk_bdev_free_io(bdev_io);

	/*
	 * is_draining indicates when time has expired for the test run
	 * and we are just waiting for the previously submitted I/O
	 * to complete.  In this case, do not submit a new I/O to replace
	 * the one just completed.
	 */
	if (!target->is_draining) {
		bdevperf_submit_single(target, task);
	} else {
		TAILQ_INSERT_TAIL(&target->task_list, task, link);
		if (target->current_queue_depth == 0) {
			spdk_put_io_channel(target->ch);
			complete = spdk_event_allocate(g_master_core, end_run, target, NULL);
			spdk_event_call(complete);
		}
	}
}

static void
bdevperf_verify_submit_read(void *cb_arg)
{
	struct io_target	*target;
	struct bdevperf_task	*task = cb_arg;
	int			rc;

	target = task->target;

	/* Read the data back in */
	if (spdk_bdev_is_md_separate(target->bdev)) {
		rc = spdk_bdev_read_blocks_with_md(target->bdev_desc, target->ch, NULL, NULL,
						   task->offset_blocks, target->io_size_blocks,
						   bdevperf_complete, task);
	} else {
		rc = spdk_bdev_read_blocks(target->bdev_desc, target->ch, NULL,
					   task->offset_blocks, target->io_size_blocks,
					   bdevperf_complete, task);
	}

	if (rc == -ENOMEM) {
		bdevperf_queue_io_wait_with_cb(task, bdevperf_verify_submit_read);
	} else if (rc != 0) {
		printf("Failed to submit read: %d\n", rc);
		_end_target(target);
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
	struct io_target	*target = task->target;
	struct spdk_bdev	*bdev = target->bdev;
	struct spdk_dif_ctx	dif_ctx;
	int			rc;

	rc = spdk_dif_ctx_init(&dif_ctx,
			       spdk_bdev_get_block_size(bdev),
			       spdk_bdev_get_md_size(bdev),
			       spdk_bdev_is_md_interleaved(bdev),
			       spdk_bdev_is_dif_head_of_md(bdev),
			       spdk_bdev_get_dif_type(bdev),
			       target->dif_check_flags,
			       task->offset_blocks, 0, 0, 0, 0);
	if (rc != 0) {
		fprintf(stderr, "Initialization of DIF context failed\n");
		return rc;
	}

	if (spdk_bdev_is_md_interleaved(bdev)) {
		rc = spdk_dif_generate(&task->iov, 1, target->io_size_blocks, &dif_ctx);
	} else {
		struct iovec md_iov = {
			.iov_base	= task->md_buf,
			.iov_len	= spdk_bdev_get_md_size(bdev) * target->io_size_blocks,
		};

		rc = spdk_dix_generate(&task->iov, 1, &md_iov, target->io_size_blocks, &dif_ctx);
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
	struct io_target	*target = task->target;
	struct spdk_bdev_desc	*desc;
	struct spdk_io_channel	*ch;
	spdk_bdev_io_completion_cb cb_fn;
	int			rc = 0;

	desc = target->bdev_desc;
	ch = target->ch;

	switch (task->io_type) {
	case SPDK_BDEV_IO_TYPE_WRITE:
		if (spdk_bdev_get_md_size(target->bdev) != 0 && target->dif_check_flags != 0) {
			rc = bdevperf_generate_dif(task);
		}
		if (rc == 0) {
			cb_fn = (g_verify || g_reset) ? bdevperf_verify_write_complete : bdevperf_complete;

			if (g_zcopy) {
				spdk_bdev_zcopy_end(task->bdev_io, true, cb_fn, task);
				return;
			} else {
				if (spdk_bdev_is_md_separate(target->bdev)) {
					rc = spdk_bdev_writev_blocks_with_md(desc, ch, &task->iov, 1,
									     task->md_buf,
									     task->offset_blocks,
									     target->io_size_blocks,
									     cb_fn, task);
				} else {
					rc = spdk_bdev_writev_blocks(desc, ch, &task->iov, 1,
								     task->offset_blocks,
								     target->io_size_blocks,
								     cb_fn, task);
				}
			}
		}
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush_blocks(desc, ch, task->offset_blocks,
					    target->io_size_blocks, bdevperf_complete, task);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = spdk_bdev_unmap_blocks(desc, ch, task->offset_blocks,
					    target->io_size_blocks, bdevperf_complete, task);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		rc = spdk_bdev_write_zeroes_blocks(desc, ch, task->offset_blocks,
						   target->io_size_blocks, bdevperf_complete, task);
		break;
	case SPDK_BDEV_IO_TYPE_READ:
		if (g_zcopy) {
			rc = spdk_bdev_zcopy_start(desc, ch, task->offset_blocks, target->io_size_blocks,
						   true, bdevperf_zcopy_populate_complete, task);
		} else {
			if (spdk_bdev_is_md_separate(target->bdev)) {
				rc = spdk_bdev_read_blocks_with_md(desc, ch, task->buf, task->md_buf,
								   task->offset_blocks,
								   target->io_size_blocks,
								   bdevperf_complete, task);
			} else {
				rc = spdk_bdev_read_blocks(desc, ch, task->buf, task->offset_blocks,
							   target->io_size_blocks, bdevperf_complete, task);
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
		_end_target(target);
		g_run_rc = rc;
		return;
	}

	target->current_queue_depth++;
}

static void
bdevperf_zcopy_get_buf_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct bdevperf_task	*task = cb_arg;
	struct io_target	*target = task->target;
	struct iovec		*iovs;
	int			iovcnt;

	if (!success) {
		_end_target(target);
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

		copy_data(iovs[0].iov_base, iovs[0].iov_len, task->buf, g_buf_size,
			  spdk_bdev_get_block_size(target->bdev),
			  spdk_bdev_io_get_md_buf(bdev_io), task->md_buf,
			  spdk_bdev_get_md_size(target->bdev), target->io_size_blocks);
	}

	bdevperf_submit_task(task);
}

static void
bdevperf_prep_zcopy_write_task(void *arg)
{
	struct bdevperf_task	*task = arg;
	struct io_target	*target = task->target;
	int			rc;

	rc = spdk_bdev_zcopy_start(target->bdev_desc, target->ch,
				   task->offset_blocks, target->io_size_blocks,
				   false, bdevperf_zcopy_get_buf_complete, task);
	if (rc != 0) {
		assert(rc == -ENOMEM);
		bdevperf_queue_io_wait_with_cb(task, bdevperf_prep_zcopy_write_task);
		return;
	}

	target->current_queue_depth++;
}

static struct bdevperf_task *
bdevperf_target_get_task(struct io_target *target)
{
	struct bdevperf_task *task;

	task = TAILQ_FIRST(&target->task_list);
	if (!task) {
		printf("Task allocation failed\n");
		abort();
	}

	TAILQ_REMOVE(&target->task_list, task, link);
	return task;
}

static __thread unsigned int seed = 0;

static void
bdevperf_submit_single(struct io_target *target, struct bdevperf_task *task)
{
	uint64_t offset_in_ios;

	if (g_is_random) {
		offset_in_ios = rand_r(&seed) % target->size_in_ios;
	} else {
		offset_in_ios = target->offset_in_ios++;
		if (target->offset_in_ios == target->size_in_ios) {
			target->offset_in_ios = 0;
		}
	}

	task->offset_blocks = offset_in_ios * target->io_size_blocks;
	if (g_verify || g_reset) {
		generate_data(task->buf, g_buf_size,
			      spdk_bdev_get_block_size(target->bdev),
			      task->md_buf, spdk_bdev_get_md_size(target->bdev),
			      target->io_size_blocks, rand_r(&seed) % 256);
		if (g_zcopy) {
			bdevperf_prep_zcopy_write_task(task);
			return;
		} else {
			task->iov.iov_base = task->buf;
			task->iov.iov_len = g_buf_size;
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
			task->iov.iov_len = g_buf_size;
			task->io_type = SPDK_BDEV_IO_TYPE_WRITE;
		}
	}

	bdevperf_submit_task(task);
}

static void
bdevperf_submit_io(struct io_target *target, int queue_depth)
{
	struct bdevperf_task *task;

	while (queue_depth-- > 0) {
		task = bdevperf_target_get_task(target);
		bdevperf_submit_single(target, task);
	}
}

static int
end_target(void *arg)
{
	struct io_target *target = arg;

	_end_target(target);

	return -1;
}

static int reset_target(void *arg);

static void
reset_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct bdevperf_task	*task = cb_arg;
	struct io_target	*target = task->target;

	if (!success) {
		printf("Reset blockdev=%s failed\n", spdk_bdev_get_name(target->bdev));
		_end_target(target);
		g_run_rc = -1;
	}

	TAILQ_INSERT_TAIL(&target->task_list, task, link);
	spdk_bdev_free_io(bdev_io);

	target->reset_timer = spdk_poller_register(reset_target, target,
			      10 * 1000000);
}

static int
reset_target(void *arg)
{
	struct io_target *target = arg;
	struct bdevperf_task *task;
	int rc;

	spdk_poller_unregister(&target->reset_timer);

	/* Do reset. */
	task = bdevperf_target_get_task(target);
	rc = spdk_bdev_reset(target->bdev_desc, target->ch,
			     reset_cb, task);
	if (rc) {
		printf("Reset failed: %d\n", rc);
		_end_target(target);
		g_run_rc = -1;
	}

	return -1;
}

static void
bdevperf_submit_on_group(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch;
	struct io_target_group *group;
	struct io_target *target;

	ch = spdk_io_channel_iter_get_channel(i);
	group = spdk_io_channel_get_ctx(ch);

	/* Submit initial I/O for each block device. Each time one
	 * completes, another will be submitted. */
	TAILQ_FOREACH(target, &group->targets, link) {
		target->ch = spdk_bdev_get_io_channel(target->bdev_desc);
		if (!target->ch) {
			printf("Skip this device (%s) as IO channel not setup.\n",
			       spdk_bdev_get_name(target->bdev));
			g_target_count--;
			g_run_rc = -1;
			spdk_bdev_close(target->bdev_desc);
			continue;
		}

		/* Start a timer to stop this I/O chain when the run is over */
		target->run_timer = spdk_poller_register(end_target, target,
				    g_time_in_usec);
		if (g_reset) {
			target->reset_timer = spdk_poller_register(reset_target, target,
					      10 * 1000000);
		}
		bdevperf_submit_io(target, g_queue_depth);
	}

	spdk_for_each_channel_continue(i, 0);
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
	printf(" -T <target>               target bdev\n");
	printf(" -f                        continue processing I/O even after failures\n");
	printf(" -z                        start bdevperf, but wait for RPC to start tests\n");
	printf(" -C                        enable every core to send I/Os to each bdev\n");
}

/*
 * Cumulative Moving Average (CMA): average of all data up to current
 * Exponential Moving Average (EMA): weighted mean of the previous n data and more weight is given to recent
 * Simple Moving Average (SMA): unweighted mean of the previous n data
 *
 * Bdevperf supports CMA and EMA.
 */
static double
get_cma_io_per_second(struct io_target *target, uint64_t io_time_in_usec)
{
	return (double)target->io_completed * 1000000 / io_time_in_usec;
}

static double
get_ema_io_per_second(struct io_target *target, uint64_t ema_period)
{
	double io_completed, io_per_second;

	io_completed = target->io_completed;
	io_per_second = (double)(io_completed - target->prev_io_completed) * 1000000
			/ g_show_performance_period_in_usec;
	target->prev_io_completed = io_completed;

	target->ema_io_per_second += (io_per_second - target->ema_io_per_second) * 2
				     / (ema_period + 1);
	return target->ema_io_per_second;
}

static void
performance_dump(uint64_t io_time_in_usec, uint64_t ema_period)
{
	unsigned lcore_id;
	double io_per_second, mb_per_second;
	double total_io_per_second, total_mb_per_second;
	struct io_target_group *group;
	struct io_target *target;

	total_io_per_second = 0;
	total_mb_per_second = 0;
	TAILQ_FOREACH(group, &g_bdevperf.groups, link) {
		if (!TAILQ_EMPTY(&group->targets)) {
			lcore_id = group->lcore;
			printf("\r Logical core: %u\n", lcore_id);
		}
		TAILQ_FOREACH(target, &group->targets, link) {
			if (ema_period == 0) {
				io_per_second = get_cma_io_per_second(target, io_time_in_usec);
			} else {
				io_per_second = get_ema_io_per_second(target, ema_period);
			}
			mb_per_second = io_per_second * g_io_size / (1024 * 1024);
			printf("\r %-20s: %10.2f IOPS %10.2f MiB/s\n",
			       target->name, io_per_second, mb_per_second);
			total_io_per_second += io_per_second;
			total_mb_per_second += mb_per_second;
		}
	}

	printf("\r =====================================================\n");
	printf("\r %-20s: %10.2f IOPS %10.2f MiB/s\n",
	       "Total", total_io_per_second, total_mb_per_second);
	fflush(stdout);

}

static int
performance_statistics_thread(void *arg)
{
	g_show_performance_period_num++;
	performance_dump(g_show_performance_period_num * g_show_performance_period_in_usec,
			 g_show_performance_ema_period);
	return -1;
}

static struct bdevperf_task *bdevperf_construct_task_on_target(struct io_target *target)
{
	struct bdevperf_task *task;

	task = calloc(1, sizeof(struct bdevperf_task));
	if (!task) {
		fprintf(stderr, "Failed to allocate task from memory\n");
		return NULL;
	}

	task->buf = spdk_zmalloc(g_io_size, g_min_alignment, NULL, SPDK_ENV_LCORE_ID_ANY,
				 SPDK_MALLOC_DMA);
	if (!task->buf) {
		fprintf(stderr, "Cannot allocate buf for task=%p\n", task);
		free(task);
		return NULL;
	}

	if (spdk_bdev_is_md_separate(target->bdev)) {
		task->md_buf = spdk_zmalloc(target->io_size_blocks *
					    spdk_bdev_get_md_size(target->bdev), 0, NULL,
					    SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (!task->md_buf) {
			fprintf(stderr, "Cannot allocate md buf for task=%p\n", task);
			free(task->buf);
			free(task);
			return NULL;
		}
	}

	task->target = target;

	return task;
}

static int
bdevperf_construct_targets_tasks(void)
{
	struct io_target_group *group;
	struct io_target *target;
	struct bdevperf_task *task;
	int i, task_num = g_queue_depth;

	/*
	 * Create the task pool after we have enumerated the targets, so that we know
	 *  the min buffer alignment.  Some backends such as AIO have alignment restrictions
	 *  that must be accounted for.
	 */
	if (g_reset) {
		task_num += 1;
	}

	/* Initialize task list for each target */
	TAILQ_FOREACH(group, &g_bdevperf.groups, link) {
		TAILQ_FOREACH(target, &group->targets, link) {
			for (i = 0; i < task_num; i++) {
				task = bdevperf_construct_task_on_target(target);
				if (task == NULL) {
					goto ret;
				}
				TAILQ_INSERT_TAIL(&target->task_list, task, link);
			}
		}
	}

	return 0;

ret:
	fprintf(stderr, "Bdevperf program exits due to memory allocation issue\n");
	fprintf(stderr, "Use -d XXX to allocate more huge pages, e.g., -d 4096\n");
	return -1;
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
		if (opts->reactor_mask) {
			fprintf(stderr, "Ignoring -m option. Verify can only run with a single core.\n");
			opts->reactor_mask = NULL;
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

static int
bdevperf_test(void)
{
	int rc;

	if (g_target_count == 0) {
		fprintf(stderr, "No valid bdevs found.\n");
		return -ENODEV;
	}

	rc = bdevperf_construct_targets_tasks();
	if (rc) {
		return rc;
	}

	printf("Running I/O for %" PRIu64 " seconds...\n", g_time_in_usec / 1000000);
	fflush(stdout);

	/* Start a timer to dump performance numbers */
	g_shutdown_tsc = spdk_get_ticks();
	if (g_show_performance_real_time) {
		g_perf_timer = spdk_poller_register(performance_statistics_thread, NULL,
						    g_show_performance_period_in_usec);
	}

	/* Iterate target groups to start all I/O */
	spdk_for_each_channel(&g_bdevperf, bdevperf_submit_on_group, NULL, NULL);

	return 0;
}

static int
io_target_group_create(void *io_device, void *ctx_buf)
{
	struct io_target_group *group = ctx_buf;

	TAILQ_INIT(&group->targets);
	group->lcore = spdk_env_get_current_core();

	return 0;
}

static void
io_target_group_destroy(void *io_device, void *ctx_buf)
{
}

static void
_bdevperf_init_thread_done(void *ctx)
{
	int rc;

	g_master_core = spdk_env_get_current_core();

	if (g_wait_for_tests) {
		/* Do not perform any tests until RPC is received */
		return;
	}

	bdevperf_construct_targets();

	rc = bdevperf_test();
	if (rc) {
		g_run_rc = rc;
		bdevperf_test_done();
		return;
	}
}

static void
_bdevperf_init_thread(void *ctx)
{
	struct spdk_io_channel *ch;
	struct io_target_group *group;

	ch = spdk_get_io_channel(&g_bdevperf);
	group = spdk_io_channel_get_ctx(ch);

	TAILQ_INSERT_TAIL(&g_bdevperf.groups, group, link);
}

static void
bdevperf_run(void *arg1)
{
	spdk_io_device_register(&g_bdevperf, io_target_group_create, io_target_group_destroy,
				sizeof(struct io_target_group), "bdevperf");

	/* Send a message to each thread and create a target group */
	spdk_for_each_thread(_bdevperf_init_thread, NULL, _bdevperf_init_thread_done);
}

static void
bdevperf_stop_io_on_group(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch;
	struct io_target_group *group;
	struct io_target *target;

	ch = spdk_io_channel_iter_get_channel(i);
	group = spdk_io_channel_get_ctx(ch);

	/* Stop I/O for each block device. */
	TAILQ_FOREACH(target, &group->targets, link) {
		end_target(target);
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
spdk_bdevperf_shutdown_cb(void)
{
	g_shutdown = true;

	if (TAILQ_EMPTY(&g_bdevperf.groups)) {
		spdk_app_stop(0);
		return;
	}

	if (g_target_count == 0) {
		bdevperf_test_done();
		return;
	}

	g_shutdown_tsc = spdk_get_ticks() - g_shutdown_tsc;

	/* Send events to stop all I/O on each target group */
	spdk_for_each_channel(&g_bdevperf, bdevperf_stop_io_on_group, NULL, NULL);
}

static int
bdevperf_parse_arg(int ch, char *arg)
{
	long long tmp;

	if (ch == 'w') {
		g_workload_type = optarg;
	} else if (ch == 'T') {
		g_target_bdev_name = optarg;
	} else if (ch == 'z') {
		g_wait_for_tests = true;
	} else if (ch == 'C') {
		g_every_core_for_each_bdev = true;
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
	int rc;

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

	bdevperf_construct_targets();

	rc = bdevperf_test();
	if (rc) {
		g_run_rc = rc;
		bdevperf_test_done();
	}
}
SPDK_RPC_REGISTER("perform_tests", rpc_perform_tests, SPDK_RPC_RUNTIME)

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

	/* default value */
	g_queue_depth = 0;
	g_io_size = 0;
	g_workload_type = NULL;
	g_time_in_sec = 0;
	g_mix_specified = false;

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
