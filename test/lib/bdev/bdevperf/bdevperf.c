/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#include <rte_config.h>
#include <rte_mempool.h>
#include <rte_lcore.h>

#include "spdk/bdev.h"
#include "spdk/copy_engine.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/io_channel.h"

struct bdevperf_task {
	struct iovec		iov;
	struct io_target	*target;
	void			*buf;
};

static int g_io_size = 0;
/* initialize to invalid value so we can detect if user overrides it. */
static int g_rw_percentage = -1;
static int g_is_random;
static bool g_verify = false;
static bool g_reset = false;
static bool g_unmap = false;
static int g_queue_depth;
static int g_time_in_sec;
static int g_show_performance_real_time = 0;
static bool g_run_failed = false;
static bool g_zcopy = true;

static struct spdk_poller *g_perf_timer = NULL;

static void bdevperf_submit_single(struct io_target *target);

#include "../common.c"

struct io_target {
	struct spdk_bdev	*bdev;
	struct spdk_io_channel	*ch;
	struct io_target	*next;
	unsigned		lcore;
	int			io_completed;
	int			current_queue_depth;
	uint64_t		size_in_ios;
	uint64_t		offset_in_ios;
	bool			is_draining;
	struct spdk_poller	*run_timer;
	struct spdk_poller	*reset_timer;
};

struct io_target *head[RTE_MAX_LCORE];
static int g_target_count = 0;

/*
 * Used to determine how the I/O buffers should be aligned.
 *  This alignment will be bumped up for blockdevs that
 *  require alignment based on block length - for example,
 *  AIO blockdevs.
 */
static uint32_t g_min_alignment = 8;

static void
blockdev_heads_init(void)
{
	int i;

	for (i = 0; i < RTE_MAX_LCORE; i++) {
		head[i] = NULL;
	}
}

static void
bdevperf_construct_targets(void)
{
	int index = 0;
	struct spdk_bdev *bdev;
	struct io_target *target;

	bdev = spdk_bdev_first();
	while (bdev != NULL) {

		if (bdev->claimed) {
			bdev = spdk_bdev_next(bdev);
			continue;
		}

		if (g_unmap && !bdev->thin_provisioning) {
			printf("Skipping %s because it does not support unmap\n", bdev->name);
			bdev = spdk_bdev_next(bdev);
			continue;
		}

		target = malloc(sizeof(struct io_target));
		if (!target) {
			fprintf(stderr, "Unable to allocate memory for new target.\n");
			/* Return immediately because all mallocs will presumably fail after this */
			return;
		}
		target->bdev = bdev;
		/* Mapping each target to lcore */
		index = g_target_count % spdk_app_get_core_count();
		target->next = head[index];
		target->lcore = index;
		target->io_completed = 0;
		target->current_queue_depth = 0;
		target->offset_in_ios = 0;
		target->size_in_ios = (bdev->blockcnt * bdev->blocklen) /
				      g_io_size;
		if (bdev->need_aligned_buffer && g_min_alignment < bdev->blocklen) {
			g_min_alignment = bdev->blocklen;
		}

		target->is_draining = false;
		target->run_timer = NULL;
		target->reset_timer = NULL;

		head[index] = target;
		g_target_count++;

		bdev = spdk_bdev_next(bdev);
	}
}

static void
end_run(spdk_event_t event)
{
	if (--g_target_count == 0) {
		if (g_show_performance_real_time) {
			spdk_poller_unregister(&g_perf_timer, NULL);
		}
		if (g_run_failed) {
			spdk_app_stop(1);
		} else {
			spdk_app_stop(0);
		}
	}
}

struct rte_mempool *task_pool;

static void
bdevperf_complete(spdk_event_t event)
{
	struct io_target	*target;
	struct bdevperf_task	*task = spdk_event_get_arg1(event);
	struct spdk_bdev_io	*bdev_io = spdk_event_get_arg2(event);
	spdk_event_t		complete;

	target = task->target;

	if (bdev_io->status != SPDK_BDEV_IO_STATUS_SUCCESS) {
		if (!g_reset) {
			target->is_draining = true;
			g_run_failed = true;
		}
	} else if (g_verify || g_reset || g_unmap) {
		assert(bdev_io->u.read.iovcnt == 1);
		if (memcmp(task->buf, bdev_io->u.read.iov.iov_base, g_io_size) != 0) {
			printf("Buffer mismatch! Disk Offset: %lu\n", bdev_io->u.read.offset);
			target->is_draining = true;
			g_run_failed = true;
		}
	}

	target->current_queue_depth--;
	target->io_completed++;

	bdev_io->caller_ctx = NULL;
	rte_mempool_put(task_pool, task);

	spdk_bdev_free_io(bdev_io);

	/*
	 * is_draining indicates when time has expired for the test run
	 * and we are just waiting for the previously submitted I/O
	 * to complete.  In this case, do not submit a new I/O to replace
	 * the one just completed.
	 */
	if (!target->is_draining) {
		bdevperf_submit_single(target);
	} else if (target->current_queue_depth == 0) {
		spdk_put_io_channel(target->ch);
		complete = spdk_event_allocate(rte_get_master_lcore(), end_run, NULL, NULL, NULL);
		spdk_event_call(complete);
	}
}

static void
bdevperf_unmap_complete(spdk_event_t event)
{
	struct io_target	*target;
	struct bdevperf_task	*task = spdk_event_get_arg1(event);
	struct spdk_bdev_io	*bdev_io = spdk_event_get_arg2(event);

	target = task->target;

	/* Set the expected buffer to 0. */
	memset(task->buf, 0, g_io_size);

	/* Read the data back in */
	spdk_bdev_read(target->bdev, target->ch, NULL,
		       from_be64(&bdev_io->u.unmap.unmap_bdesc->lba) * target->bdev->blocklen,
		       from_be32(&bdev_io->u.unmap.unmap_bdesc->block_count) * target->bdev->blocklen,
		       bdevperf_complete, task);

	free(bdev_io->u.unmap.unmap_bdesc);
	spdk_bdev_free_io(bdev_io);

}

static void
bdevperf_verify_write_complete(spdk_event_t event)
{
	struct io_target	*target;
	struct bdevperf_task	*task = spdk_event_get_arg1(event);
	struct spdk_bdev_io	*bdev_io = spdk_event_get_arg2(event);

	target = task->target;

	if (g_unmap) {
		/* Unmap the data */
		struct spdk_scsi_unmap_bdesc *bdesc = calloc(1, sizeof(*bdesc));
		if (bdesc == NULL) {
			fprintf(stderr, "memory allocation failure\n");
			exit(1);
		}

		to_be64(&bdesc->lba, bdev_io->u.write.offset / target->bdev->blocklen);
		to_be32(&bdesc->block_count, bdev_io->u.write.len / target->bdev->blocklen);

		spdk_bdev_unmap(target->bdev, target->ch, bdesc, 1, bdevperf_unmap_complete,
				task);
	} else {
		/* Read the data back in */
		spdk_bdev_read(target->bdev, target->ch, NULL,
			       bdev_io->u.write.offset,
			       bdev_io->u.write.len,
			       bdevperf_complete, task);
	}

	spdk_bdev_free_io(bdev_io);
}

static void
task_ctor(struct rte_mempool *mp, void *arg, void *__task, unsigned id)
{
	struct bdevperf_task *task = __task;

	task->buf = spdk_zmalloc(g_io_size, g_min_alignment, NULL);
}

static __thread unsigned int seed = 0;

static void
bdevperf_submit_single(struct io_target *target)
{
	struct spdk_bdev	*bdev;
	struct spdk_io_channel	*ch;
	struct bdevperf_task	*task = NULL;
	uint64_t		offset_in_ios;
	void			*rbuf;

	bdev = target->bdev;
	ch = target->ch;

	if (rte_mempool_get(task_pool, (void **)&task) != 0 || task == NULL) {
		printf("Task pool allocation failed\n");
		abort();
	}

	task->target = target;

	if (g_is_random) {
		offset_in_ios = rand_r(&seed) % target->size_in_ios;
	} else {
		offset_in_ios = target->offset_in_ios++;
		if (target->offset_in_ios == target->size_in_ios) {
			target->offset_in_ios = 0;
		}
	}

	if (g_verify || g_reset || g_unmap) {
		memset(task->buf, rand_r(&seed) % 256, g_io_size);
		task->iov.iov_base = task->buf;
		task->iov.iov_len = g_io_size;
		spdk_bdev_writev(bdev, ch, &task->iov, 1, offset_in_ios * g_io_size, g_io_size,
				 bdevperf_verify_write_complete, task);
	} else if ((g_rw_percentage == 100) ||
		   (g_rw_percentage != 0 && ((rand_r(&seed) % 100) < g_rw_percentage))) {
		rbuf = g_zcopy ? NULL : task->buf;
		spdk_bdev_read(bdev, ch, rbuf, offset_in_ios * g_io_size, g_io_size,
			       bdevperf_complete, task);
	} else {
		task->iov.iov_base = task->buf;
		task->iov.iov_len = g_io_size;
		spdk_bdev_writev(bdev, ch, &task->iov, 1, offset_in_ios * g_io_size, g_io_size,
				 bdevperf_complete, task);
	}

	target->current_queue_depth++;
}

static void
bdevperf_submit_io(struct io_target *target, int queue_depth)
{
	while (queue_depth-- > 0) {
		bdevperf_submit_single(target);
	}
}

static void
end_target(void *arg)
{
	struct io_target *target = arg;

	spdk_poller_unregister(&target->run_timer, NULL);
	if (g_reset) {
		spdk_poller_unregister(&target->reset_timer, NULL);
	}

	target->is_draining = true;
}

static void reset_target(void *arg);

static void
reset_cb(spdk_event_t event)
{
	struct spdk_bdev_io	*bdev_io = spdk_event_get_arg2(event);
	int			status = bdev_io->status;
	struct bdevperf_task	*task = bdev_io->caller_ctx;
	struct io_target	*target = task->target;

	if (status != SPDK_BDEV_IO_STATUS_SUCCESS) {
		printf("Reset blockdev=%s failed\n", target->bdev->name);
		target->is_draining = true;
		g_run_failed = true;
	}

	rte_mempool_put(task_pool, task);

	spdk_poller_register(&target->reset_timer, reset_target, target, target->lcore,
			     NULL, 10 * 1000000);
}

static void
reset_target(void *arg)
{
	struct io_target *target = arg;
	struct bdevperf_task	*task = NULL;

	spdk_poller_unregister(&target->reset_timer, NULL);

	/* Do reset. */
	rte_mempool_get(task_pool, (void **)&task);
	task->target = target;
	spdk_bdev_reset(target->bdev, SPDK_BDEV_RESET_SOFT,
			reset_cb, task);
}

static void
bdevperf_submit_on_core(spdk_event_t event)
{
	struct io_target *target = spdk_event_get_arg1(event);

	/* Submit initial I/O for each block device. Each time one
	 * completes, another will be submitted. */
	while (target != NULL) {
		target->ch = spdk_bdev_get_io_channel(target->bdev, SPDK_IO_PRIORITY_DEFAULT);

		/* Start a timer to stop this I/O chain when the run is over */
		spdk_poller_register(&target->run_timer, end_target, target, target->lcore, NULL,
				     g_time_in_sec * 1000000);
		if (g_reset) {
			spdk_poller_register(&target->reset_timer, reset_target, target,
					     target->lcore, NULL, 10 * 1000000);
		}
		bdevperf_submit_io(target, g_queue_depth);
		target = target->next;
	}
}

static void usage(char *program_name)
{
	printf("%s options\n", program_name);
	printf("\t[-c configuration file]\n");
	printf("\t[-m core mask for distributing I/O submission/completion work\n");
	printf("\t\t(default: 0x1 - use core 0 only)]\n");
	printf("\t[-q io depth]\n");
	printf("\t[-s io size in bytes]\n");
	printf("\t[-w io pattern type, must be one of\n");
	printf("\t\t(read, write, randread, randwrite, rw, randrw, verify, reset)]\n");
	printf("\t[-M rwmixread (100 for reads, 0 for writes)]\n");
	printf("\t[-t time in seconds]\n");
	printf("\t[-S Show performance result in real time]\n");
}

static void
performance_dump(int io_time)
{
	int index;
	unsigned lcore_id;
	float io_per_second, mb_per_second;
	float total_io_per_second, total_mb_per_second;
	struct io_target *target;

	total_io_per_second = 0;
	total_mb_per_second = 0;
	for (index = 0; index < spdk_app_get_core_count(); index++) {
		target = head[index];
		if (target != NULL) {
			lcore_id = target->lcore;
			printf("\r Logical core: %u\n", lcore_id);
		}
		while (target != NULL) {
			io_per_second = (float)target->io_completed /
					io_time;
			mb_per_second = io_per_second * g_io_size /
					(1024 * 1024);
			printf("\r %-20s: %10.2f IO/s %10.2f MB/s\n",
			       target->bdev->name, io_per_second,
			       mb_per_second);
			total_io_per_second += io_per_second;
			total_mb_per_second += mb_per_second;
			target = target->next;
		}
	}

	printf("\r =====================================================\n");
	printf("\r %-20s: %10.2f IO/s %10.2f MB/s\n",
	       "Total", total_io_per_second, total_mb_per_second);
	fflush(stdout);

}

static void
performance_statistics_thread(void *arg)
{
	performance_dump(1);
}

static void
bdevperf_run(spdk_event_t evt)
{
	int i;
	struct io_target *target;
	spdk_event_t event;

	printf("Running I/O for %d seconds...\n", g_time_in_sec);
	fflush(stdout);

	/* Start a timer to dump performance numbers */
	if (g_show_performance_real_time) {
		spdk_poller_register(&g_perf_timer, performance_statistics_thread, NULL,
				     spdk_app_get_current_core(), NULL, 1000000);
	}

	/* Send events to start all I/O */
	RTE_LCORE_FOREACH(i) {
		if (spdk_app_get_core_mask() & (1ULL << i)) {
			target = head[i];
			if (target != NULL) {
				event = spdk_event_allocate(target->lcore, bdevperf_submit_on_core,
							    target, NULL, NULL);
				spdk_event_call(event);
			}
		}
	}
}

int
main(int argc, char **argv)
{
	const char *config_file;
	const char *core_mask;
	const char *workload_type;
	int op;
	bool mix_specified;

	/* default value*/
	config_file = NULL;
	g_queue_depth = 0;
	g_io_size = 0;
	workload_type = NULL;
	g_time_in_sec = 0;
	mix_specified = false;
	core_mask = NULL;

	while ((op = getopt(argc, argv, "c:m:q:s:t:w:M:S")) != -1) {
		switch (op) {
		case 'c':
			config_file = optarg;
			break;
		case 'm':
			core_mask = optarg;
			break;
		case 'q':
			g_queue_depth = atoi(optarg);
			break;
		case 's':
			g_io_size = atoi(optarg);
			break;
		case 't':
			g_time_in_sec = atoi(optarg);
			break;
		case 'w':
			workload_type = optarg;
			break;
		case 'M':
			g_rw_percentage = atoi(optarg);
			mix_specified = true;
			break;
		case 'S':
			g_show_performance_real_time = 1;
			break;
		default:
			usage(argv[0]);
			exit(1);
		}
	}

	if (!config_file) {
		usage(argv[0]);
		exit(1);
	}
	if (!g_queue_depth) {
		usage(argv[0]);
		exit(1);
	}
	if (!g_io_size) {
		usage(argv[0]);
		exit(1);
	}
	if (!workload_type) {
		usage(argv[0]);
		exit(1);
	}
	if (!g_time_in_sec) {
		usage(argv[0]);
		exit(1);
	}

	if (strcmp(workload_type, "read") &&
	    strcmp(workload_type, "write") &&
	    strcmp(workload_type, "randread") &&
	    strcmp(workload_type, "randwrite") &&
	    strcmp(workload_type, "rw") &&
	    strcmp(workload_type, "randrw") &&
	    strcmp(workload_type, "verify") &&
	    strcmp(workload_type, "reset") &&
	    strcmp(workload_type, "unmap")) {
		fprintf(stderr,
			"io pattern type must be one of\n"
			"(read, write, randread, randwrite, rw, randrw, verify, reset, unmap)\n");
		exit(1);
	}

	if (!strcmp(workload_type, "read") ||
	    !strcmp(workload_type, "randread")) {
		g_rw_percentage = 100;
	}

	if (!strcmp(workload_type, "write") ||
	    !strcmp(workload_type, "randwrite")) {
		g_rw_percentage = 0;
	}

	if (!strcmp(workload_type, "verify") ||
	    !strcmp(workload_type, "reset") ||
	    !strcmp(workload_type, "unmap")) {
		g_rw_percentage = 50;
		if (g_io_size > SPDK_BDEV_LARGE_RBUF_MAX_SIZE) {
			fprintf(stderr, "Unable to exceed max I/O size of %d for verify. (%d provided).\n",
				SPDK_BDEV_LARGE_RBUF_MAX_SIZE, g_io_size);
			exit(1);
		}
		if (core_mask) {
			fprintf(stderr, "Ignoring -m option. Verify can only run with a single core.\n");
			core_mask = NULL;
		}
		g_verify = true;
		if (!strcmp(workload_type, "reset")) {
			g_reset = true;
		}
		if (!strcmp(workload_type, "unmap")) {
			g_unmap = true;
		}
	}

	if (!strcmp(workload_type, "read") ||
	    !strcmp(workload_type, "randread") ||
	    !strcmp(workload_type, "write") ||
	    !strcmp(workload_type, "randwrite") ||
	    !strcmp(workload_type, "verify") ||
	    !strcmp(workload_type, "reset") ||
	    !strcmp(workload_type, "unmap")) {
		if (mix_specified) {
			fprintf(stderr, "Ignoring -M option... Please use -M option"
				" only when using rw or randrw.\n");
		}
	}

	if (!strcmp(workload_type, "rw") ||
	    !strcmp(workload_type, "randrw")) {
		if (g_rw_percentage < 0 || g_rw_percentage > 100) {
			fprintf(stderr,
				"-M must be specified to value from 0 to 100 "
				"for rw or randrw.\n");
			exit(1);
		}
	}

	if (!strcmp(workload_type, "read") ||
	    !strcmp(workload_type, "write") ||
	    !strcmp(workload_type, "rw") ||
	    !strcmp(workload_type, "verify") ||
	    !strcmp(workload_type, "reset") ||
	    !strcmp(workload_type, "unmap")) {
		g_is_random = 0;
	} else {
		g_is_random = 1;
	}

	if (g_io_size > SPDK_BDEV_LARGE_RBUF_MAX_SIZE) {
		fprintf(stdout, "I/O size of %d is greather than zero copy threshold (%d).\n",
			g_io_size, SPDK_BDEV_LARGE_RBUF_MAX_SIZE);
		fprintf(stdout, "Zero copy mechanism will not be used.\n");
		g_zcopy = false;
	}

	optind = 1;  /*reset the optind */

	rte_set_log_level(RTE_LOG_ERR);

	blockdev_heads_init();

	bdevtest_init(config_file, core_mask);

	bdevperf_construct_targets();

	task_pool = rte_mempool_create("task_pool", 4096 * spdk_app_get_core_count(),
				       sizeof(struct bdevperf_task),
				       64, 0, NULL, NULL, task_ctor, NULL,
				       SOCKET_ID_ANY, 0);

	spdk_app_start(bdevperf_run, NULL, NULL);

	performance_dump(g_time_in_sec);
	spdk_app_fini();
	printf("done.\n");
	return g_run_failed;
}
