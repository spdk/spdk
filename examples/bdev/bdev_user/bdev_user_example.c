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

#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/bdev_user.h"

#include <pthread.h>

#define SPDK_NVMF_BUILD_ETC "/usr/local/etc/nvmf"
#define SPDK_NVMF_DEFAULT_CONFIG SPDK_NVMF_BUILD_ETC "/nvmf.conf"

#define NVMF_NULL_TGT_NUM_IOS (64 * 1024)
#define NVMF_NULL_TGT_IO_RING_SIZE (NVMF_NULL_TGT_NUM_IOS)
#define NVMF_NULL_TGT_IO_POOL_CACHE_SIZE (64)
#define NVMF_NULL_TGT_HANDLE_REQUEST_BATCH (8)

#define CONTEXT_VERIFICATION "IO context can be used for user device lookup"

struct spdk_ring *g_io_ring;
struct spdk_mempool *g_io_event_pool;
volatile bool g_io_thread_run;
volatile bool g_main_thread_run;
pthread_t g_completion_thread_tid;
unsigned int g_completion_thread_core = 2;
unsigned int g_reactor_core = 1;
#define REACTOR_MASK "0x2"

uint64_t g_num_ios = 0;
uint64_t g_num_completed_ios = 0;


struct nvmf_null_tgt_ring_entry {
	struct spdk_bdev_io *bdev_io;
};

static int
enqueue_io_request(void *user_ctxt, struct spdk_bdev_io *bdev_io)
{
	int num_enqueued;
	struct nvmf_null_tgt_ring_entry *io_event;
	char *context = user_ctxt;

	if (strcmp(CONTEXT_VERIFICATION, context) != 0) {
		SPDK_ERRLOG("Invalid context %s\n", context);
		assert(false);
	}

	io_event = spdk_mempool_get(g_io_event_pool);
	io_event->bdev_io = bdev_io;

	g_num_ios++;

	num_enqueued = spdk_ring_enqueue(g_io_ring,
		(void **)&io_event, 1);
	assert(num_enqueued == 1);

	return 0;
}

struct bdev_user_fn_table g_fn_table = {
	.submit_request = enqueue_io_request,
};

static void
nvmf_usage(void)
{
}

static void
nvmf_parse_arg(int ch, char *arg)
{
}

static int
nvmf_null_tgt_handle_request_batch(void)
{
	int count;
	void *events[NVMF_NULL_TGT_HANDLE_REQUEST_BATCH];
	struct nvmf_null_tgt_ring_entry *io_event;
	int i;

	count = spdk_ring_dequeue(g_io_ring, events, 1);
	if (count == 0) {
		goto out;
	}

	assert(count < NVMF_NULL_TGT_HANDLE_REQUEST_BATCH);

	for (i = 0; i < count; i++) {
		io_event = events[i];
		bdev_user_submit_completion(io_event->bdev_io, true);
		spdk_mempool_put(g_io_event_pool, io_event);
		g_num_completed_ios++;
	}

out:
	return count;
}

static void
*nvmf_null_tgt_io_thread_fn(void *arg)
{
	int rc;
	cpu_set_t cpu_set;
        struct sched_param param;
	int num_ios;

	/* Since the poller is tight polling on CPU 1, we need to schedule the
	 * completion thread on another core, or else completions will not be
	 * able to get CPU time */
	param.sched_priority = sched_get_priority_max(SCHED_RR);
	assert(param.sched_priority != -1);
	rc = pthread_setschedparam(g_completion_thread_tid, SCHED_RR, &param);
	assert(rc == 0);

	CPU_SET(g_completion_thread_core, &cpu_set);
	rc = pthread_setaffinity_np(g_completion_thread_tid, sizeof(cpu_set_t),
		&cpu_set);

	SPDK_NOTICELOG("Starting IO handler\n");

	bdev_user_register_device("bdev_user_example_device", 100,
		g_reactor_core, CONTEXT_VERIFICATION);

	while (g_io_thread_run) {
		num_ios = nvmf_null_tgt_handle_request_batch();
		if (num_ios == 0) {
			usleep(10);
		}
	}

	SPDK_NOTICELOG("Stopping IO handler\n");

	return NULL;
}

static void
nvmf_tgt_started(void *arg1, void *arg2)
{
	int rc;

	if (getenv("MEMZONE_DUMP") != NULL) {
		spdk_memzone_dump(stdout);
		fflush(stdout);
	}

	/* TODO - instead of sleeping, get a callback from */
	g_io_event_pool = spdk_mempool_create("null_tgt_io_event",
		NVMF_NULL_TGT_NUM_IOS, sizeof(struct nvmf_null_tgt_ring_entry),
		NVMF_NULL_TGT_IO_POOL_CACHE_SIZE,
		SPDK_ENV_SOCKET_ID_ANY);
	assert(g_io_event_pool);

	g_io_ring = spdk_ring_create(SPDK_RING_TYPE_SP_SC,
		NVMF_NULL_TGT_IO_RING_SIZE,
		SPDK_ENV_SOCKET_ID_ANY);
	assert(g_io_ring);

	bdev_user_register_fn_table(&g_fn_table);

	g_io_thread_run = true;
	rc = pthread_create(&g_completion_thread_tid, NULL,
		nvmf_null_tgt_io_thread_fn, NULL);
	assert(rc == 0);

}

static void
shutdown_application_thread(void)
{
	int rc;
	void *thread_retval;

	g_io_thread_run = false;
	rc = pthread_join(g_completion_thread_tid, &thread_retval);
	assert(rc == 0);

	g_main_thread_run = false;
}

static void
*application_thread(void *arg)
{
	int rc;
	struct spdk_app_opts opts = {};

	/* default value in opts */
	spdk_app_opts_init(&opts);
	opts.name = "bdev_user_reference";
	opts.config_file = SPDK_NVMF_DEFAULT_CONFIG;
	opts.max_delay_us = 0;
	opts.rpc_addr = "127.0.0.1";
	opts.reactor_mask = REACTOR_MASK;
	opts.shutdown_cb = shutdown_application_thread;
	rc = spdk_app_parse_args(0, NULL, &opts, "", nvmf_parse_arg,
		nvmf_usage);
	assert(rc == SPDK_APP_PARSE_ARGS_SUCCESS);

	/* Blocks until the application is exiting */
	spdk_app_start(&opts, nvmf_tgt_started, NULL, NULL);

	return NULL;
}

int
main (int argc, char **argv)
{
	pthread_t application_tid;
	int rc;

	rc = pthread_create(&application_tid, NULL, application_thread, NULL);
	assert(rc == 0);

	/* Wait for signal in order to complete */
	g_main_thread_run = true;
	while (g_main_thread_run) {
		sleep(1);
	}

	return 0;
}
