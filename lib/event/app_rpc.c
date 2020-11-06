/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
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

#include "spdk/event.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/env.h"
#include "spdk/thread.h"

#include "spdk/log.h"
#include "spdk_internal/event.h"
#include "spdk_internal/thread.h"

struct rpc_spdk_kill_instance {
	char *sig_name;
};

static void
free_rpc_spdk_kill_instance(struct rpc_spdk_kill_instance *req)
{
	free(req->sig_name);
}

static const struct spdk_json_object_decoder rpc_spdk_kill_instance_decoders[] = {
	{"sig_name", offsetof(struct rpc_spdk_kill_instance, sig_name), spdk_json_decode_string},
};

static void
rpc_spdk_kill_instance(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	static const struct {
		const char	*signal_string;
		int32_t		signal;
	} signals[] = {
		{"SIGINT",	SIGINT},
		{"SIGTERM",	SIGTERM},
		{"SIGQUIT",	SIGQUIT},
		{"SIGHUP",	SIGHUP},
		{"SIGKILL",	SIGKILL},
		{"SIGUSR1",	SIGUSR1},
	};
	size_t i, sig_count;
	int signal;
	struct rpc_spdk_kill_instance req = {};

	if (spdk_json_decode_object(params, rpc_spdk_kill_instance_decoders,
				    SPDK_COUNTOF(rpc_spdk_kill_instance_decoders),
				    &req)) {
		SPDK_DEBUGLOG(app_rpc, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	sig_count = SPDK_COUNTOF(signals);
	signal = spdk_strtol(req.sig_name, 10);
	for (i = 0 ; i < sig_count; i++) {
		if (strcmp(req.sig_name, signals[i].signal_string) == 0 ||
		    signal == signals[i].signal) {
			break;
		}
	}

	if (i == sig_count) {
		goto invalid;
	}

	SPDK_DEBUGLOG(app_rpc, "sending signal %d\n", signals[i].signal);
	free_rpc_spdk_kill_instance(&req);
	kill(getpid(), signals[i].signal);

	spdk_jsonrpc_send_bool_response(request, true);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_spdk_kill_instance(&req);
}
SPDK_RPC_REGISTER("spdk_kill_instance", rpc_spdk_kill_instance, SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(spdk_kill_instance, kill_instance)


struct rpc_framework_monitor_context_switch {
	bool enabled;
};

static const struct spdk_json_object_decoder rpc_framework_monitor_context_switch_decoders[] = {
	{"enabled", offsetof(struct rpc_framework_monitor_context_switch, enabled), spdk_json_decode_bool},
};

static void
rpc_framework_monitor_context_switch(struct spdk_jsonrpc_request *request,
				     const struct spdk_json_val *params)
{
	struct rpc_framework_monitor_context_switch req = {};
	struct spdk_json_write_ctx *w;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_framework_monitor_context_switch_decoders,
					    SPDK_COUNTOF(rpc_framework_monitor_context_switch_decoders),
					    &req)) {
			SPDK_DEBUGLOG(app_rpc, "spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
			return;
		}

		spdk_framework_enable_context_switch_monitor(req.enabled);
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);

	spdk_json_write_named_bool(w, "enabled", spdk_framework_context_switch_monitor_enabled());

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
}

SPDK_RPC_REGISTER("framework_monitor_context_switch", rpc_framework_monitor_context_switch,
		  SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(framework_monitor_context_switch, context_switch_monitor)

struct rpc_get_stats_ctx {
	struct spdk_jsonrpc_request *request;
	struct spdk_json_write_ctx *w;
	uint64_t now;
};

static void
rpc_thread_get_stats_done(void *arg)
{
	struct rpc_get_stats_ctx *ctx = arg;

	spdk_json_write_array_end(ctx->w);
	spdk_json_write_object_end(ctx->w);
	spdk_jsonrpc_end_result(ctx->request, ctx->w);

	free(ctx);
}

static void
rpc_thread_get_stats_for_each(struct spdk_jsonrpc_request *request, spdk_msg_fn fn)
{
	struct rpc_get_stats_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error");
		return;
	}
	ctx->request = request;

	ctx->w = spdk_jsonrpc_begin_result(ctx->request);
	spdk_json_write_object_begin(ctx->w);
	spdk_json_write_named_uint64(ctx->w, "tick_rate", spdk_get_ticks_hz());
	spdk_json_write_named_array_begin(ctx->w, "threads");

	spdk_for_each_thread(fn, ctx, rpc_thread_get_stats_done);
}

static void
_rpc_thread_get_stats(void *arg)
{
	struct rpc_get_stats_ctx *ctx = arg;
	struct spdk_thread *thread = spdk_get_thread();
	struct spdk_poller *poller;
	struct spdk_thread_stats stats;
	uint64_t active_pollers_count = 0;
	uint64_t timed_pollers_count = 0;
	uint64_t paused_pollers_count = 0;

	TAILQ_FOREACH(poller, &thread->active_pollers, tailq) {
		active_pollers_count++;
	}
	TAILQ_FOREACH(poller, &thread->timed_pollers, tailq) {
		timed_pollers_count++;
	}
	TAILQ_FOREACH(poller, &thread->paused_pollers, tailq) {
		paused_pollers_count++;
	}

	if (0 == spdk_thread_get_stats(&stats)) {
		spdk_json_write_object_begin(ctx->w);
		spdk_json_write_named_string(ctx->w, "name", spdk_thread_get_name(thread));
		spdk_json_write_named_uint64(ctx->w, "id", spdk_thread_get_id(thread));
		spdk_json_write_named_string(ctx->w, "cpumask",
					     spdk_cpuset_fmt(spdk_thread_get_cpumask(thread)));
		spdk_json_write_named_uint64(ctx->w, "busy", stats.busy_tsc);
		spdk_json_write_named_uint64(ctx->w, "idle", stats.idle_tsc);
		spdk_json_write_named_uint64(ctx->w, "active_pollers_count", active_pollers_count);
		spdk_json_write_named_uint64(ctx->w, "timed_pollers_count", timed_pollers_count);
		spdk_json_write_named_uint64(ctx->w, "paused_pollers_count", paused_pollers_count);
		spdk_json_write_object_end(ctx->w);
	}
}

static void
rpc_thread_get_stats(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *params)
{
	if (params) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "'thread_get_stats' requires no arguments");
		return;
	}

	rpc_thread_get_stats_for_each(request, _rpc_thread_get_stats);
}

SPDK_RPC_REGISTER("thread_get_stats", rpc_thread_get_stats, SPDK_RPC_RUNTIME)

static void
rpc_get_poller(struct spdk_poller *poller, struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", poller->name);
	spdk_json_write_named_string(w, "state", spdk_poller_state_str(poller->state));
	spdk_json_write_named_uint64(w, "run_count", poller->run_count);
	spdk_json_write_named_uint64(w, "busy_count", poller->busy_count);
	if (poller->period_ticks) {
		spdk_json_write_named_uint64(w, "period_ticks", poller->period_ticks);
	}
	spdk_json_write_object_end(w);
}

static void
_rpc_thread_get_pollers(void *arg)
{
	struct rpc_get_stats_ctx *ctx = arg;
	struct spdk_thread *thread = spdk_get_thread();
	struct spdk_poller *poller;

	spdk_json_write_object_begin(ctx->w);
	spdk_json_write_named_string(ctx->w, "name", spdk_thread_get_name(thread));
	spdk_json_write_named_uint64(ctx->w, "id", spdk_thread_get_id(thread));

	spdk_json_write_named_array_begin(ctx->w, "active_pollers");
	TAILQ_FOREACH(poller, &thread->active_pollers, tailq) {
		rpc_get_poller(poller, ctx->w);
	}
	spdk_json_write_array_end(ctx->w);

	spdk_json_write_named_array_begin(ctx->w, "timed_pollers");
	TAILQ_FOREACH(poller, &thread->timed_pollers, tailq) {
		rpc_get_poller(poller, ctx->w);
	}
	spdk_json_write_array_end(ctx->w);

	spdk_json_write_named_array_begin(ctx->w, "paused_pollers");
	TAILQ_FOREACH(poller, &thread->paused_pollers, tailq) {
		rpc_get_poller(poller, ctx->w);
	}
	spdk_json_write_array_end(ctx->w);

	spdk_json_write_object_end(ctx->w);
}

static void
rpc_thread_get_pollers(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	if (params) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "'thread_get_pollers' requires no arguments");
		return;
	}

	rpc_thread_get_stats_for_each(request, _rpc_thread_get_pollers);
}

SPDK_RPC_REGISTER("thread_get_pollers", rpc_thread_get_pollers, SPDK_RPC_RUNTIME)

static void
rpc_get_io_channel(struct spdk_io_channel *ch, struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", spdk_io_device_get_name(ch->dev));
	spdk_json_write_named_uint32(w, "ref", ch->ref);
	spdk_json_write_object_end(w);
}

static void
_rpc_thread_get_io_channels(void *arg)
{
	struct rpc_get_stats_ctx *ctx = arg;
	struct spdk_thread *thread = spdk_get_thread();
	struct spdk_io_channel *ch;

	spdk_json_write_object_begin(ctx->w);
	spdk_json_write_named_string(ctx->w, "name", spdk_thread_get_name(thread));

	spdk_json_write_named_array_begin(ctx->w, "io_channels");
	TAILQ_FOREACH(ch, &thread->io_channels, tailq) {
		rpc_get_io_channel(ch, ctx->w);
	}
	spdk_json_write_array_end(ctx->w);

	spdk_json_write_object_end(ctx->w);
}

static void
rpc_thread_get_io_channels(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	if (params) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "'thread_get_io_channels' requires no arguments");
		return;
	}

	rpc_thread_get_stats_for_each(request, _rpc_thread_get_io_channels);
}

SPDK_RPC_REGISTER("thread_get_io_channels", rpc_thread_get_io_channels, SPDK_RPC_RUNTIME);

static void
rpc_framework_get_reactors_done(void *arg1, void *arg2)
{
	struct rpc_get_stats_ctx *ctx = arg1;

	spdk_json_write_array_end(ctx->w);
	spdk_json_write_object_end(ctx->w);
	spdk_jsonrpc_end_result(ctx->request, ctx->w);

	free(ctx);
}

#define GET_DELTA(end, start)	(end >= start ? end - start : 0)

static void
_rpc_framework_get_reactors(void *arg1, void *arg2)
{
	struct rpc_get_stats_ctx *ctx = arg1;
	uint32_t current_core;
	struct spdk_reactor *reactor;
	struct spdk_lw_thread *lw_thread;
	struct spdk_thread *thread;

	current_core = spdk_env_get_current_core();
	reactor = spdk_reactor_get(current_core);

	assert(reactor != NULL);

	spdk_json_write_object_begin(ctx->w);
	spdk_json_write_named_uint32(ctx->w, "lcore", current_core);
	spdk_json_write_named_uint64(ctx->w, "busy", reactor->busy_tsc);
	spdk_json_write_named_uint64(ctx->w, "idle", reactor->idle_tsc);

	spdk_json_write_named_array_begin(ctx->w, "lw_threads");
	TAILQ_FOREACH(lw_thread, &reactor->threads, link) {
		thread = spdk_thread_get_from_ctx(lw_thread);

		spdk_json_write_object_begin(ctx->w);
		spdk_json_write_named_string(ctx->w, "name", spdk_thread_get_name(thread));
		spdk_json_write_named_uint64(ctx->w, "id", spdk_thread_get_id(thread));
		spdk_json_write_named_string(ctx->w, "cpumask",
					     spdk_cpuset_fmt(spdk_thread_get_cpumask(thread)));
		spdk_json_write_named_uint64(ctx->w, "elapsed",
					     GET_DELTA(ctx->now, lw_thread->tsc_start));
		spdk_json_write_object_end(ctx->w);
	}
	spdk_json_write_array_end(ctx->w);

	spdk_json_write_object_end(ctx->w);
}

static void
rpc_framework_get_reactors(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_get_stats_ctx *ctx;

	if (params) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "`framework_get_reactors` requires no arguments");
		return;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation error");
		return;
	}

	ctx->now = spdk_get_ticks();
	ctx->request = request;
	ctx->w = spdk_jsonrpc_begin_result(ctx->request);

	spdk_json_write_object_begin(ctx->w);
	spdk_json_write_named_uint64(ctx->w, "tick_rate", spdk_get_ticks_hz());
	spdk_json_write_named_array_begin(ctx->w, "reactors");

	spdk_for_each_reactor(_rpc_framework_get_reactors, ctx, NULL,
			      rpc_framework_get_reactors_done);
}

SPDK_RPC_REGISTER("framework_get_reactors", rpc_framework_get_reactors, SPDK_RPC_RUNTIME)

struct rpc_set_scheduler_ctx {
	char *name;
};

static void
free_rpc_framework_set_scheduler(struct rpc_set_scheduler_ctx *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_set_scheduler_decoders[] = {
	{"name", offsetof(struct rpc_set_scheduler_ctx, name), spdk_json_decode_string},
};

static void
rpc_framework_set_scheduler(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_set_scheduler_ctx req = {NULL};
	int ret;

	ret = spdk_json_decode_object(params, rpc_set_scheduler_decoders,
				      SPDK_COUNTOF(rpc_set_scheduler_decoders),
				      &req);
	if (ret) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		goto end;
	}

	ret = _spdk_scheduler_set(req.name);
	if (ret) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(ret));
		goto end;
	}

	spdk_jsonrpc_send_bool_response(request, true);

end:
	free_rpc_framework_set_scheduler(&req);
}
SPDK_RPC_REGISTER("framework_set_scheduler", rpc_framework_set_scheduler, SPDK_RPC_STARTUP)

struct rpc_thread_set_cpumask_ctx {
	struct spdk_jsonrpc_request *request;
	struct spdk_cpuset cpumask;
	int status;
	struct spdk_thread *orig_thread;
};

static void
rpc_thread_set_cpumask_done(void *_ctx)
{
	struct rpc_thread_set_cpumask_ctx *ctx = _ctx;

	if (ctx->status == 0) {
		spdk_jsonrpc_send_bool_response(ctx->request, true);
	} else {
		spdk_jsonrpc_send_error_response(ctx->request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 spdk_strerror(-ctx->status));
	}

	free(ctx);
}

static void
_rpc_thread_set_cpumask(void *_ctx)
{
	struct rpc_thread_set_cpumask_ctx *ctx = _ctx;

	ctx->status = spdk_thread_set_cpumask(&ctx->cpumask);

	spdk_thread_send_msg(ctx->orig_thread, rpc_thread_set_cpumask_done, ctx);
}

struct rpc_thread_set_cpumask {
	uint64_t id;
	char *cpumask;
};

static const struct spdk_json_object_decoder rpc_thread_set_cpumask_decoders[] = {
	{"id", offsetof(struct rpc_thread_set_cpumask, id), spdk_json_decode_uint64},
	{"cpumask", offsetof(struct rpc_thread_set_cpumask, cpumask), spdk_json_decode_string},
};

static void
rpc_thread_set_cpumask(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_thread_set_cpumask req = {};
	struct rpc_thread_set_cpumask_ctx *ctx;
	const struct spdk_cpuset *coremask;
	struct spdk_cpuset tmp_mask;
	struct spdk_thread *thread;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		SPDK_ERRLOG("Memory allocation failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Memory allocation failed");
		return;
	}

	if (spdk_json_decode_object(params, rpc_thread_set_cpumask_decoders,
				    SPDK_COUNTOF(rpc_thread_set_cpumask_decoders),
				    &req)) {
		SPDK_ERRLOG("spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		goto err;
	}

	thread = spdk_thread_get_by_id(req.id);
	if (thread == NULL) {
		SPDK_ERRLOG("Thread %" PRIu64 " does not exist\n", req.id);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						     "Thread %" PRIu64 " does not exist", req.id);
		goto err;
	}

	rc = spdk_app_parse_core_mask(req.cpumask, &ctx->cpumask);
	if (rc != 0) {
		SPDK_ERRLOG("Invalid cpumask %s\n", req.cpumask);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "Invalid cpumask %s", req.cpumask);
		goto err;
	}

	if (spdk_cpuset_count(&ctx->cpumask) == 0) {
		coremask = spdk_app_get_core_mask();
		spdk_cpuset_copy(&tmp_mask, coremask);
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						     "No CPU is selected from reactor mask %s\n",
						     spdk_cpuset_fmt(&tmp_mask));
		goto err;
	}

	ctx->request = request;
	ctx->orig_thread = spdk_get_thread();

	spdk_thread_send_msg(thread, _rpc_thread_set_cpumask, ctx);

	free(req.cpumask);
	return;

err:
	free(req.cpumask);
	free(ctx);
}
SPDK_RPC_REGISTER("thread_set_cpumask", rpc_thread_set_cpumask, SPDK_RPC_RUNTIME)
SPDK_LOG_REGISTER_COMPONENT(app_rpc)
