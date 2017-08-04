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

#include "spdk/barrier.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/io_channel.h"
#include "spdk_internal/log.h"

#ifdef __linux__
#include <sys/prctl.h>
#endif

#ifdef __FreeBSD__
#include <pthread_np.h>
#endif

struct rpc_kill_instance {
	char *sig_name;
};

static void
free_rpc_kill_instance(struct rpc_kill_instance *req)
{
	free(req->sig_name);
}

static const struct spdk_json_object_decoder rpc_kill_instance_decoders[] = {
	{"sig_name", offsetof(struct rpc_kill_instance, sig_name), spdk_json_decode_string},
};

static void
spdk_rpc_kill_instance(struct spdk_jsonrpc_request *request,
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
	};
	size_t i, sig_count;
	int signal;
	struct rpc_kill_instance req = {};
	struct spdk_json_write_ctx *w;

	if (spdk_json_decode_object(params, rpc_kill_instance_decoders,
				    SPDK_COUNTOF(rpc_kill_instance_decoders),
				    &req)) {
		SPDK_TRACELOG(SPDK_TRACE_DEBUG, "spdk_json_decode_object failed\n");
		goto invalid;
	}

	sig_count = SPDK_COUNTOF(signals);
	signal = atoi(req.sig_name);
	for (i = 0 ; i < sig_count; i++) {
		if (strcmp(req.sig_name, signals[i].signal_string) == 0 ||
		    signal == signals[i].signal) {
			break;
		}
	}

	if (i == sig_count) {
		goto invalid;
	}

	SPDK_TRACELOG(SPDK_TRACE_DEBUG, "sending signal %d\n", signals[i].signal);
	kill(getpid(), signals[i].signal);
	free_rpc_kill_instance(&req);

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);
	return;

invalid:
	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "Invalid parameters");
	free_rpc_kill_instance(&req);
}
SPDK_RPC_REGISTER("kill_instance", spdk_rpc_kill_instance)

#define SPDK_MAX_LCORES 128

struct spdk_thread_rusage {
	char thread_name[128];
	struct rusage rusage;
};

struct spdk_thread_rusage_array {
	bool done;
	uint32_t thread_count;
	struct spdk_thread_rusage rusages[SPDK_MAX_LCORES];
};

static void
thread_get_rusage(void *ctx)
{
	struct spdk_thread_rusage_array *rusage_array = ctx;
	uint32_t index = rusage_array->thread_count;
	struct spdk_thread_rusage *thread_rusage = &rusage_array->rusages[index];

#if defined(__linux__)
	prctl(PR_GET_NAME, thread_rusage->thread_name, 0, 0, 0);
#elif defined(__FreeBSD__)
	pthread_get_name_np(pthread_self(), thread_rusage->thread_name);
#endif
	getrusage(RUSAGE_THREAD, &thread_rusage->rusage);
	rusage_array->thread_count++;
}

static void
thread_get_rusage_done(void *ctx)
{
	struct spdk_thread_rusage_array *rusage_array = ctx;

	rusage_array->done = true;
}

static void
spdk_rpc_get_threads_rusage(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct spdk_thread_rusage_array thread_rusage_array;
	struct spdk_thread_rusage *thread_rusage;
	uint32_t i;
	struct spdk_json_write_ctx *w;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_portal_groups requires no parameters");
		return;
	}

	memset(&thread_rusage_array, 0, sizeof(struct spdk_thread_rusage_array));

	spdk_for_each_thread(thread_get_rusage, &thread_rusage_array, thread_get_rusage_done);

	while (thread_rusage_array.done == false) {
		spdk_wmb();
		continue;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);
	for (i = 0; i < thread_rusage_array.thread_count; i++) {
		thread_rusage = &thread_rusage_array.rusages[i];
		spdk_json_write_object_begin(w);
		spdk_json_write_name(w, "thread id");
		spdk_json_write_string(w, thread_rusage->thread_name);
		spdk_json_write_name(w, "ru_nvcsw");
		spdk_json_write_int64(w, thread_rusage->rusage.ru_nvcsw);
		spdk_json_write_name(w, "ru_nivcsw");
		spdk_json_write_int64(w, thread_rusage->rusage.ru_nivcsw);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_threads_rusage", spdk_rpc_get_threads_rusage)
