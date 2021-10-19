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
#include "spdk/likely.h"
#include "spdk/json.h"
#include "spdk/jsonrpc.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"

#include "spdk_internal/event.h"

static bool g_is_running = true;
pthread_mutex_t g_sched_list_mutex = PTHREAD_MUTEX_INITIALIZER;
#define TIMESLICE_US 100 * 1000

struct sched_thread {
	struct spdk_thread *thread;
	struct spdk_poller *poller;
	struct spdk_poller *idle_poller;
	int active_percent;
	struct spdk_jsonrpc_request *request;
	TAILQ_ENTRY(sched_thread) link;
};

static TAILQ_HEAD(, sched_thread) g_sched_threads = TAILQ_HEAD_INITIALIZER(g_sched_threads);

struct rpc_thread_create {
	int active_percent;
	char *name;
	char *cpu_mask;
};

static void
free_rpc_thread_create(struct rpc_thread_create *req)
{
	free(req->name);
	free(req->cpu_mask);
}

static const struct spdk_json_object_decoder rpc_thread_create_decoders[] = {
	{"active", offsetof(struct rpc_thread_create, active_percent), spdk_json_decode_uint64},
	{"name", offsetof(struct rpc_thread_create, name), spdk_json_decode_string, true},
	{"cpu_mask", offsetof(struct rpc_thread_create, cpu_mask), spdk_json_decode_string, true},
};

static void
rpc_scheduler_thread_create_cb(struct spdk_jsonrpc_request *request, uint64_t thread_id)
{
	struct spdk_json_write_ctx *w;

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_uint64(w, thread_id);
	spdk_jsonrpc_end_result(request, w);
}

static void
thread_delete(struct sched_thread *sched_thread)
{
	spdk_poller_unregister(&sched_thread->poller);
	spdk_poller_unregister(&sched_thread->idle_poller);
	spdk_thread_exit(sched_thread->thread);

	TAILQ_REMOVE(&g_sched_threads, sched_thread, link);
	free(sched_thread);

	if (!g_is_running && TAILQ_EMPTY(&g_sched_threads)) {
		spdk_app_stop(0);
	}
}

static int
poller_run_busy(void *arg)
{
	struct sched_thread *sched_thread = arg;

	if (spdk_unlikely(!g_is_running)) {
		pthread_mutex_lock(&g_sched_list_mutex);
		thread_delete(sched_thread);
		pthread_mutex_unlock(&g_sched_list_mutex);
		return SPDK_POLLER_IDLE;
	}

	spdk_delay_us(TIMESLICE_US * sched_thread->active_percent / 100);
	return SPDK_POLLER_BUSY;
}

static int
poller_run_idle(void *arg)
{
	struct sched_thread *sched_thread = arg;

	if (spdk_unlikely(!g_is_running)) {
		pthread_mutex_lock(&g_sched_list_mutex);
		thread_delete(sched_thread);
		pthread_mutex_unlock(&g_sched_list_mutex);
		return SPDK_POLLER_IDLE;
	}

	spdk_delay_us(10);
	return SPDK_POLLER_IDLE;
}

static void
update_pollers(struct sched_thread *sched_thread)
{
	spdk_poller_unregister(&sched_thread->poller);
	if (sched_thread->active_percent > 0) {
		sched_thread->poller = spdk_poller_register_named(poller_run_busy, sched_thread, TIMESLICE_US,
				       spdk_thread_get_name(sched_thread->thread));
		assert(sched_thread->poller != NULL);
	}
	if (sched_thread->idle_poller == NULL) {
		sched_thread->idle_poller = spdk_poller_register_named(poller_run_idle, sched_thread, 0,
					    "idle_poller");
		assert(sched_thread->idle_poller != NULL);
	}
}

static void
rpc_register_poller(void *arg)
{
	struct sched_thread *sched_thread = arg;

	update_pollers(sched_thread);

	if (sched_thread->request != NULL) {
		rpc_scheduler_thread_create_cb(sched_thread->request, spdk_thread_get_id(sched_thread->thread));
		sched_thread->request = NULL;
	}
}

static void
rpc_scheduler_thread_create(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct sched_thread *sched_thread;
	struct rpc_thread_create req = {0};
	struct spdk_cpuset *cpu_set = NULL;
	int rc = 0;

	if (spdk_json_decode_object(params, rpc_thread_create_decoders,
				    SPDK_COUNTOF(rpc_thread_create_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters provided");
		return;
	}

	if (req.active_percent < 0 || req.active_percent > 100) {
		SPDK_ERRLOG("invalid percent value %d\n", req.active_percent);
		spdk_jsonrpc_send_error_response(request, -EINVAL, spdk_strerror(EINVAL));
		free_rpc_thread_create(&req);
		return;
	}

	if (req.cpu_mask != NULL) {
		cpu_set = calloc(1, sizeof(*cpu_set));
		assert(cpu_set != NULL);
		rc = spdk_cpuset_parse(cpu_set, req.cpu_mask);
		if (rc < 0) {
			SPDK_ERRLOG("invalid cpumask %s\n", req.cpu_mask);
			spdk_jsonrpc_send_error_response(request, -EINVAL, spdk_strerror(EINVAL));
			free_rpc_thread_create(&req);
			free(cpu_set);
			return;
		}
	}

	sched_thread = calloc(1, sizeof(*sched_thread));
	assert(sched_thread != NULL);

	sched_thread->thread = spdk_thread_create(req.name, cpu_set);
	assert(sched_thread->thread != NULL);
	free(cpu_set);

	sched_thread->request = request;
	sched_thread->active_percent = req.active_percent;

	spdk_thread_send_msg(sched_thread->thread, rpc_register_poller, sched_thread);

	free_rpc_thread_create(&req);

	pthread_mutex_lock(&g_sched_list_mutex);
	TAILQ_INSERT_TAIL(&g_sched_threads, sched_thread, link);
	pthread_mutex_unlock(&g_sched_list_mutex);

	return;
}

SPDK_RPC_REGISTER("scheduler_thread_create", rpc_scheduler_thread_create, SPDK_RPC_RUNTIME)

struct rpc_thread_set_active_ctx {
	int active_percent;
	struct spdk_jsonrpc_request *request;
};

struct rpc_thread_set_active {
	uint64_t thread_id;
	int active_percent;
};

static const struct spdk_json_object_decoder rpc_thread_set_active_decoders[] = {
	{"thread_id", offsetof(struct rpc_thread_set_active, thread_id), spdk_json_decode_uint64},
	{"active", offsetof(struct rpc_thread_set_active, active_percent), spdk_json_decode_uint64},
};

static void
rpc_scheduler_thread_set_active_cb(void *arg)
{
	struct rpc_thread_set_active_ctx *ctx = arg;
	uint64_t thread_id;
	struct sched_thread *sched_thread;

	thread_id = spdk_thread_get_id(spdk_get_thread());

	pthread_mutex_lock(&g_sched_list_mutex);
	TAILQ_FOREACH(sched_thread, &g_sched_threads, link) {
		if (spdk_thread_get_id(sched_thread->thread) == thread_id) {
			sched_thread->active_percent = ctx->active_percent;
			update_pollers(sched_thread);
			pthread_mutex_unlock(&g_sched_list_mutex);
			spdk_jsonrpc_send_bool_response(ctx->request, true);
			free(ctx);
			return;
		}
	}
	pthread_mutex_unlock(&g_sched_list_mutex);

	spdk_jsonrpc_send_error_response(ctx->request, -ENOENT, spdk_strerror(ENOENT));
	free(ctx);
	return;
}

static void
rpc_scheduler_thread_set_active(struct spdk_jsonrpc_request *request,
				const struct spdk_json_val *params)
{
	struct spdk_thread *thread;
	struct rpc_thread_set_active req = {0};
	struct rpc_thread_set_active_ctx *ctx;

	if (spdk_json_decode_object(params, rpc_thread_set_active_decoders,
				    SPDK_COUNTOF(rpc_thread_set_active_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters provided");
		return;
	}

	if (req.active_percent < 0 || req.active_percent > 100) {
		SPDK_ERRLOG("invalid percent value %d\n", req.active_percent);
		spdk_jsonrpc_send_error_response(request, -EINVAL, spdk_strerror(EINVAL));
		return;
	}

	thread = spdk_thread_get_by_id(req.thread_id);
	if (thread == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENOENT, spdk_strerror(ENOENT));
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(-ENOMEM));
		return;
	}
	ctx->request = request;
	ctx->active_percent = req.active_percent;

	spdk_thread_send_msg(thread, rpc_scheduler_thread_set_active_cb, ctx);
}

SPDK_RPC_REGISTER("scheduler_thread_set_active", rpc_scheduler_thread_set_active, SPDK_RPC_RUNTIME)

struct rpc_thread_delete_ctx {
	struct spdk_jsonrpc_request *request;
};

struct rpc_thread_delete {
	uint64_t thread_id;
};

static const struct spdk_json_object_decoder rpc_thread_delete_decoders[] = {
	{"thread_id", offsetof(struct rpc_thread_delete, thread_id), spdk_json_decode_uint64},
};

static void
rpc_scheduler_thread_delete_cb(void *arg)
{
	struct rpc_thread_delete_ctx *ctx = arg;
	struct sched_thread *sched_thread;
	uint64_t thread_id;

	thread_id = spdk_thread_get_id(spdk_get_thread());

	pthread_mutex_lock(&g_sched_list_mutex);
	TAILQ_FOREACH(sched_thread, &g_sched_threads, link) {
		if (spdk_thread_get_id(sched_thread->thread) == thread_id) {
			thread_delete(sched_thread);
			pthread_mutex_unlock(&g_sched_list_mutex);
			spdk_jsonrpc_send_bool_response(ctx->request, true);
			free(ctx);
			return;
		}
	}
	pthread_mutex_unlock(&g_sched_list_mutex);

	spdk_jsonrpc_send_error_response(ctx->request, -ENOENT, spdk_strerror(ENOENT));
	free(ctx);
	return;
}

static void
rpc_scheduler_thread_delete(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct spdk_thread *thread;
	struct rpc_thread_delete req = {0};
	struct rpc_thread_delete_ctx *ctx;

	if (spdk_json_decode_object(params, rpc_thread_delete_decoders,
				    SPDK_COUNTOF(rpc_thread_delete_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters provided");
		return;
	}

	thread = spdk_thread_get_by_id(req.thread_id);
	if (thread == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENOENT, spdk_strerror(ENOENT));
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, spdk_strerror(-ENOMEM));
		return;
	}
	ctx->request = request;

	spdk_thread_send_msg(thread, rpc_scheduler_thread_delete_cb, ctx);
}

SPDK_RPC_REGISTER("scheduler_thread_delete", rpc_scheduler_thread_delete, SPDK_RPC_RUNTIME)

static void
test_shutdown(void)
{
	g_is_running = false;
	SPDK_NOTICELOG("Scheduler test application stopped.\n");
	pthread_mutex_lock(&g_sched_list_mutex);
	if (TAILQ_EMPTY(&g_sched_threads)) {
		spdk_app_stop(0);
	}
	pthread_mutex_unlock(&g_sched_list_mutex);
}

static void
for_each_nop(void *arg1, void *arg2)
{
}

static void
for_each_done(void *arg1, void *arg2)
{
	spdk_for_each_reactor(for_each_nop, NULL, NULL, for_each_done);
}

static void
test_start(void *arg1)
{
	SPDK_NOTICELOG("Scheduler test application started.\n");
	/* Start an spdk_for_each_reactor operation that just keeps
	 * running over and over again until the app exits.  This
	 * serves as a regression test for SPDK issue #2206, ensuring
	 * that any pending spdk_for_each_reactor operations are
	 * completed before reactors are shut down.
	 */
	for_each_done(NULL, NULL);
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts;
	int rc = 0;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "scheduler";
	opts.shutdown_cb = test_shutdown;

	if ((rc = spdk_app_parse_args(argc, argv, &opts,
				      NULL, NULL, NULL, NULL)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
		return rc;
	}

	rc = spdk_app_start(&opts, test_start, NULL);

	spdk_app_fini();

	return rc;
}
