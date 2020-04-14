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
#include "spdk/jsonrpc.h"
#include "spdk/rpc.h"
#include "spdk/event.h"
#include "spdk/util.h"

#define RPC_MAX_THREADS 1024
#define RPC_MAX_POLLERS 1024

struct spdk_jsonrpc_client *g_rpc_client;

struct rpc_thread_info {
	char *name;
	uint64_t id;
	char *cpumask;
	uint64_t busy;
	uint64_t idle;
	uint64_t active_pollers_count;
	uint64_t timed_pollers_count;
	uint64_t paused_pollers_count;
};

struct rpc_threads {
	uint64_t threads_count;
	struct rpc_thread_info thread_info[RPC_MAX_THREADS];
};

struct rpc_threads_stats {
	uint64_t tick_rate;
	struct rpc_threads threads;
};

struct rpc_poller_info {
	char *name;
	char *state;
	uint64_t run_count;
	uint64_t busy_count;
	uint64_t period_ticks;
};

struct rpc_pollers {
	uint64_t pollers_count;
	struct rpc_poller_info pollers[RPC_MAX_POLLERS];
};

struct rpc_poller_thread_info {
	char *name;
	struct rpc_pollers active_pollers;
	struct rpc_pollers timed_pollers;
	struct rpc_pollers paused_pollers;
};

struct rpc_pollers_threads {
	uint64_t threads_count;
	struct rpc_poller_thread_info threads[RPC_MAX_THREADS];
};

struct rpc_pollers_stats {
	uint64_t tick_rate;
	struct rpc_pollers_threads pollers_threads;
};

struct rpc_threads_stats g_threads_stats;
struct rpc_pollers_stats g_pollers_stats;

static void
free_rpc_threads_stats(struct rpc_threads_stats *req)
{
	uint64_t i;

	for (i = 0; i < req->threads.threads_count; i++) {
		free(req->threads.thread_info[i].name);
		req->threads.thread_info[i].name = NULL;
		free(req->threads.thread_info[i].cpumask);
		req->threads.thread_info[i].cpumask = NULL;
	}
}

static const struct spdk_json_object_decoder rpc_thread_info_decoders[] = {
	{"name", offsetof(struct rpc_thread_info, name), spdk_json_decode_string},
	{"id", offsetof(struct rpc_thread_info, id), spdk_json_decode_uint64},
	{"cpumask", offsetof(struct rpc_thread_info, cpumask), spdk_json_decode_string},
	{"busy", offsetof(struct rpc_thread_info, busy), spdk_json_decode_uint64},
	{"idle", offsetof(struct rpc_thread_info, idle), spdk_json_decode_uint64},
	{"active_pollers_count", offsetof(struct rpc_thread_info, active_pollers_count), spdk_json_decode_uint64},
	{"timed_pollers_count", offsetof(struct rpc_thread_info, timed_pollers_count), spdk_json_decode_uint64},
	{"paused_pollers_count", offsetof(struct rpc_thread_info, paused_pollers_count), spdk_json_decode_uint64},
};

static int
rpc_decode_threads_object(const struct spdk_json_val *val, void *out)
{
	struct rpc_thread_info *info = out;

	return spdk_json_decode_object(val, rpc_thread_info_decoders,
				       SPDK_COUNTOF(rpc_thread_info_decoders), info);
}

static int
rpc_decode_threads_array(const struct spdk_json_val *val, void *out)
{
	struct rpc_threads *threads = out;

	return spdk_json_decode_array(val, rpc_decode_threads_object, threads->thread_info, RPC_MAX_THREADS,
				      &threads->threads_count, sizeof(struct rpc_thread_info));
}

static const struct spdk_json_object_decoder rpc_threads_stats_decoders[] = {
	{"tick_rate", offsetof(struct rpc_threads_stats, tick_rate), spdk_json_decode_uint64},
	{"threads", offsetof(struct rpc_threads_stats, threads), rpc_decode_threads_array},
};

static void
free_rpc_poller(struct rpc_poller_info *poller)
{
	free(poller->name);
	poller->name = NULL;
	free(poller->state);
	poller->state = NULL;
}

static void
free_rpc_pollers_stats(struct rpc_pollers_stats *req)
{
	struct rpc_poller_thread_info *thread;
	uint64_t i, j;

	for (i = 0; i < req->pollers_threads.threads_count; i++) {
		thread = &req->pollers_threads.threads[i];

		for (j = 0; j < thread->active_pollers.pollers_count; j++) {
			free_rpc_poller(&thread->active_pollers.pollers[j]);
		}

		for (j = 0; j < thread->timed_pollers.pollers_count; j++) {
			free_rpc_poller(&thread->timed_pollers.pollers[j]);
		}

		for (j = 0; j < thread->paused_pollers.pollers_count; j++) {
			free_rpc_poller(&thread->paused_pollers.pollers[j]);
		}

		free(thread->name);
		thread->name = NULL;
	}
}

static const struct spdk_json_object_decoder rpc_pollers_decoders[] = {
	{"name", offsetof(struct rpc_poller_info, name), spdk_json_decode_string},
	{"state", offsetof(struct rpc_poller_info, state), spdk_json_decode_string},
	{"run_count", offsetof(struct rpc_poller_info, run_count), spdk_json_decode_uint64},
	{"busy_count", offsetof(struct rpc_poller_info, busy_count), spdk_json_decode_uint64},
	{"period_ticks", offsetof(struct rpc_poller_info, period_ticks), spdk_json_decode_uint64, true},
};

static int
rpc_decode_pollers_object(const struct spdk_json_val *val, void *out)
{
	struct rpc_poller_info *info = out;

	return spdk_json_decode_object(val, rpc_pollers_decoders, SPDK_COUNTOF(rpc_pollers_decoders), info);
}

static int
rpc_decode_pollers_array(const struct spdk_json_val *val, void *out)
{
	struct rpc_pollers *pollers = out;

	return spdk_json_decode_array(val, rpc_decode_pollers_object, pollers->pollers, RPC_MAX_THREADS,
				      &pollers->pollers_count, sizeof(struct rpc_poller_info));
}

static const struct spdk_json_object_decoder rpc_pollers_threads_decoders[] = {
	{"name", offsetof(struct rpc_poller_thread_info, name), spdk_json_decode_string},
	{"active_pollers", offsetof(struct rpc_poller_thread_info, active_pollers), rpc_decode_pollers_array},
	{"timed_pollers", offsetof(struct rpc_poller_thread_info, timed_pollers), rpc_decode_pollers_array},
	{"paused_pollers", offsetof(struct rpc_poller_thread_info, paused_pollers), rpc_decode_pollers_array},
};

static int
rpc_decode_pollers_threads_object(const struct spdk_json_val *val, void *out)
{
	struct rpc_poller_thread_info *info = out;

	return spdk_json_decode_object(val, rpc_pollers_threads_decoders,
				       SPDK_COUNTOF(rpc_pollers_threads_decoders), info);
}

static int
rpc_decode_pollers_threads_array(const struct spdk_json_val *val, void *out)
{
	struct rpc_pollers_threads *pollers_threads = out;

	return spdk_json_decode_array(val, rpc_decode_pollers_threads_object, pollers_threads->threads,
				      RPC_MAX_THREADS, &pollers_threads->threads_count, sizeof(struct rpc_poller_thread_info));
}

static const struct spdk_json_object_decoder rpc_pollers_stats_decoders[] = {
	{"tick_rate", offsetof(struct rpc_pollers_stats, tick_rate), spdk_json_decode_uint64},
	{"threads", offsetof(struct rpc_pollers_stats, pollers_threads), rpc_decode_pollers_threads_array},
};

static int
rpc_send_req(char *rpc_name, struct spdk_jsonrpc_client_response **resp)
{
	struct spdk_jsonrpc_client_response *json_resp = NULL;
	struct spdk_json_write_ctx *w;
	struct spdk_jsonrpc_client_request *request;
	int rc;

	request = spdk_jsonrpc_client_create_request();
	if (request == NULL) {
		return -ENOMEM;
	}

	w = spdk_jsonrpc_begin_request(request, 1, rpc_name);
	spdk_jsonrpc_end_request(request, w);
	spdk_jsonrpc_client_send_request(g_rpc_client, request);

	do {
		rc = spdk_jsonrpc_client_poll(g_rpc_client, 1);
	} while (rc == 0 || rc == -ENOTCONN);

	if (rc <= 0) {
		printf("Failed to get response: %d\n", rc);
		return -1;
	}

	json_resp = spdk_jsonrpc_client_get_response(g_rpc_client);
	if (json_resp == NULL) {
		printf("spdk_jsonrpc_client_get_response() failed\n");
		return -1;
	}

	/* Check for error response */
	if (json_resp->error != NULL) {
		printf("Unexpected error response\n");
		return -1;
	}

	assert(json_resp->result);

	*resp = json_resp;

	return 0;
}

static int
get_data(void)
{
	struct spdk_jsonrpc_client_response *json_resp = NULL;
	int rc = 0;

	rc = rpc_send_req("thread_get_stats", &json_resp);
	if (rc) {
		goto end;
	}

	/* Decode json */
	memset(&g_threads_stats, 0, sizeof(g_threads_stats));
	if (spdk_json_decode_object(json_resp->result, rpc_threads_stats_decoders,
				    SPDK_COUNTOF(rpc_threads_stats_decoders), &g_threads_stats)) {
		rc = -EINVAL;
		goto end;
	}

	spdk_jsonrpc_client_free_response(json_resp);

	rc = rpc_send_req("thread_get_pollers", &json_resp);
	if (rc) {
		goto end;
	}

	/* Decode json */
	memset(&g_pollers_stats, 0, sizeof(g_pollers_stats));
	if (spdk_json_decode_object(json_resp->result, rpc_pollers_stats_decoders,
				    SPDK_COUNTOF(rpc_pollers_stats_decoders), &g_pollers_stats)) {
		rc = -EINVAL;
		goto end;
	}

end:
	spdk_jsonrpc_client_free_response(json_resp);
	return rc;
}

static void
free_data(void)
{
	free_rpc_threads_stats(&g_threads_stats);
	free_rpc_pollers_stats(&g_pollers_stats);
}

static void
usage(const char *program_name)
{
	printf("%s [options]", program_name);
	printf("\n");
	printf("options:\n");
	printf(" -r <path>  RPC listen address (default: /var/tmp/spdk.sock\n");
	printf(" -h         show this usage\n");
}

int main(int argc, char **argv)
{
	int op, rc;
	char *socket = SPDK_DEFAULT_RPC_ADDR;

	while ((op = getopt(argc, argv, "r:h")) != -1) {
		switch (op) {
		case 'r':
			socket = optarg;
			break;
		case 'H':
		default:
			usage(argv[0]);
			return 1;
		}
	}

	g_rpc_client = spdk_jsonrpc_client_connect(socket, AF_UNIX);
	if (!g_rpc_client) {
		fprintf(stderr, "spdk_jsonrpc_client_connect() failed: %d\n", errno);
		return 1;
	}

	rc = get_data();
	if (rc) {
		fprintf(stderr, "Error occurred while getting required data from SPDK: %d\n", rc);
		free_data();
		return 1;
	}

	free_data();

	spdk_jsonrpc_client_close(g_rpc_client);

	return (0);
}
