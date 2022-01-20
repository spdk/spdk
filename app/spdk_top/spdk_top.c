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
#include "spdk/env.h"

#if defined __has_include
#if __has_include(<ncurses/panel.h>)
#include <ncurses/ncurses.h>
#include <ncurses/panel.h>
#include <ncurses/menu.h>
#else
#include <ncurses.h>
#include <panel.h>
#include <menu.h>
#endif
#else
#include <ncurses.h>
#include <panel.h>
#include <menu.h>
#endif

#define RPC_MAX_THREADS 1024
#define RPC_MAX_POLLERS 1024
#define RPC_MAX_CORES 255
#define MAX_THREAD_NAME 128
#define MAX_POLLER_NAME 128
#define MAX_THREADS 4096
#define RR_MAX_VALUE 255

#define MAX_STRING_LEN 12289 /* 3x 4k monitors + 1 */
#define TAB_WIN_HEIGHT 3
#define TAB_WIN_LOCATION_ROW 1
#define TABS_SPACING 2
#define TABS_LOCATION_ROW 4
#define TABS_LOCATION_COL 0
#define TABS_DATA_START_ROW 3
#define TABS_DATA_START_COL 2
#define TABS_COL_COUNT 10
#define MENU_WIN_HEIGHT 3
#define MENU_WIN_SPACING 4
#define MENU_WIN_LOCATION_COL 0
#define RR_WIN_WIDTH 32
#define RR_WIN_HEIGHT 5
#define MAX_THREAD_NAME_LEN 26
#define MAX_THREAD_COUNT_STR_LEN 14
#define MAX_POLLER_NAME_LEN 36
#define MAX_POLLER_COUNT_STR_LEN 16
#define MAX_POLLER_TYPE_STR_LEN 8
#define MAX_POLLER_IND_STR_LEN 28
#define MAX_CORE_MASK_STR_LEN 16
#define MAX_CORE_STR_LEN 6
#define MAX_CORE_FREQ_STR_LEN 18
#define MAX_TIME_STR_LEN 12
#define MAX_CPU_STR_LEN 8
#define MAX_POLLER_RUN_COUNT 20
#define MAX_PERIOD_STR_LEN 12
#define MAX_INTR_LEN 6
#define WINDOW_HEADER 12
#define FROM_HEX 16
#define THREAD_WIN_WIDTH 69
#define THREAD_WIN_HEIGHT 9
#define THREAD_WIN_FIRST_COL 2
#define CORE_WIN_FIRST_COL 16
#define CORE_WIN_WIDTH 48
#define CORE_WIN_HEIGHT 11
#define POLLER_WIN_HEIGHT 8
#define POLLER_WIN_WIDTH 64
#define POLLER_WIN_FIRST_COL 14
#define FIRST_DATA_ROW 7
#define HELP_WIN_WIDTH 88
#define HELP_WIN_HEIGHT 22

enum tabs {
	THREADS_TAB,
	POLLERS_TAB,
	CORES_TAB,
	NUMBER_OF_TABS,
};

enum column_threads_type {
	COL_THREADS_NAME,
	COL_THREADS_CORE,
	COL_THREADS_ACTIVE_POLLERS,
	COL_THREADS_TIMED_POLLERS,
	COL_THREADS_PAUSED_POLLERS,
	COL_THREADS_IDLE_TIME,
	COL_THREADS_BUSY_TIME,
	COL_THREADS_CPU_USAGE,
	COL_THREADS_NONE = 255,
};

enum column_pollers_type {
	COL_POLLERS_NAME,
	COL_POLLERS_TYPE,
	COL_POLLERS_THREAD_NAME,
	COL_POLLERS_RUN_COUNTER,
	COL_POLLERS_PERIOD,
	COL_POLLERS_BUSY_COUNT,
	COL_POLLERS_NONE = 255,
};

enum column_cores_type {
	COL_CORES_CORE,
	COL_CORES_THREADS,
	COL_CORES_POLLERS,
	COL_CORES_IDLE_TIME,
	COL_CORES_BUSY_TIME,
	COL_CORES_CORE_FREQ,
	COL_CORES_INTR,
	COL_CORES_CPU_USAGE,
	COL_CORES_NONE = 255,
};

enum spdk_poller_type {
	SPDK_ACTIVE_POLLER,
	SPDK_TIMED_POLLER,
	SPDK_PAUSED_POLLER,
	SPDK_POLLER_TYPES_COUNT,
};

struct col_desc {
	const char *name;
	uint8_t name_len;
	uint8_t max_data_string;
	bool disabled;
};

struct run_counter_history {
	uint64_t poller_id;
	uint64_t thread_id;
	uint64_t last_run_counter;
	uint64_t last_busy_counter;
	TAILQ_ENTRY(run_counter_history) link;
};

uint8_t g_sleep_time = 1;
uint16_t g_selected_row;
uint16_t g_max_selected_row;
uint64_t g_tick_rate;
const char *poller_type_str[SPDK_POLLER_TYPES_COUNT] = {"Active", "Timed", "Paused"};
const char *g_tab_title[NUMBER_OF_TABS] = {"[1] THREADS", "[2] POLLERS", "[3] CORES"};
struct spdk_jsonrpc_client *g_rpc_client;
static TAILQ_HEAD(, run_counter_history) g_run_counter_history = TAILQ_HEAD_INITIALIZER(
			g_run_counter_history);
WINDOW *g_menu_win, *g_tab_win[NUMBER_OF_TABS], *g_tabs[NUMBER_OF_TABS];
PANEL *g_panels[NUMBER_OF_TABS];
uint16_t g_max_row, g_max_col;
uint16_t g_data_win_size, g_max_data_rows;
uint32_t g_last_threads_count, g_last_pollers_count, g_last_cores_count;
uint8_t g_current_sort_col[NUMBER_OF_TABS] = {COL_THREADS_NAME, COL_POLLERS_NAME, COL_CORES_CORE};
uint8_t g_current_sort_col2[NUMBER_OF_TABS] = {COL_THREADS_NONE, COL_POLLERS_NONE, COL_CORES_NONE};
bool g_interval_data = true;
bool g_quit_app = false;
pthread_mutex_t g_thread_lock;
static struct col_desc g_col_desc[NUMBER_OF_TABS][TABS_COL_COUNT] = {
	{	{.name = "Thread name", .max_data_string = MAX_THREAD_NAME_LEN},
		{.name = "Core", .max_data_string = MAX_CORE_STR_LEN},
		{.name = "Active pollers", .max_data_string = MAX_POLLER_COUNT_STR_LEN},
		{.name = "Timed pollers", .max_data_string = MAX_POLLER_COUNT_STR_LEN},
		{.name = "Paused pollers", .max_data_string = MAX_POLLER_COUNT_STR_LEN},
		{.name = "Idle [us]", .max_data_string = MAX_TIME_STR_LEN},
		{.name = "Busy [us]", .max_data_string = MAX_TIME_STR_LEN},
		{.name = "CPU %", .max_data_string = MAX_CPU_STR_LEN},
		{.name = (char *)NULL}
	},
	{	{.name = "Poller name", .max_data_string = MAX_POLLER_NAME_LEN},
		{.name = "Type", .max_data_string = MAX_POLLER_TYPE_STR_LEN},
		{.name = "On thread", .max_data_string = MAX_THREAD_NAME_LEN},
		{.name = "Run count", .max_data_string = MAX_POLLER_RUN_COUNT},
		{.name = "Period [us]", .max_data_string = MAX_PERIOD_STR_LEN},
		{.name = "Status (busy count)", .max_data_string = MAX_POLLER_IND_STR_LEN},
		{.name = (char *)NULL}
	},
	{	{.name = "Core", .max_data_string = MAX_CORE_STR_LEN},
		{.name = "Thread count", .max_data_string = MAX_THREAD_COUNT_STR_LEN},
		{.name = "Poller count", .max_data_string = MAX_POLLER_COUNT_STR_LEN},
		{.name = "Idle [us]", .max_data_string = MAX_TIME_STR_LEN},
		{.name = "Busy [us]", .max_data_string = MAX_TIME_STR_LEN},
		{.name = "Frequency [MHz]", .max_data_string = MAX_CORE_FREQ_STR_LEN},
		{.name = "Intr", .max_data_string = MAX_INTR_LEN},
		{.name = "CPU %", .max_data_string = MAX_CPU_STR_LEN},
		{.name = (char *)NULL}
	}
};

struct rpc_thread_info {
	char *name;
	uint64_t id;
	int core_num;
	char *cpumask;
	uint64_t busy;
	uint64_t last_busy;
	uint64_t idle;
	uint64_t last_idle;
	uint64_t active_pollers_count;
	uint64_t timed_pollers_count;
	uint64_t paused_pollers_count;
};

struct rpc_poller_info {
	char *name;
	char *state;
	uint64_t id;
	uint64_t run_count;
	uint64_t busy_count;
	uint64_t period_ticks;
	enum spdk_poller_type type;
	char thread_name[MAX_THREAD_NAME];
	uint64_t thread_id;
};

struct rpc_core_thread_info {
	char *name;
	uint64_t id;
	char *cpumask;
	uint64_t elapsed;
};

struct rpc_core_threads {
	uint64_t threads_count;
	struct rpc_core_thread_info *thread;
};

struct rpc_core_info {
	uint32_t lcore;
	uint64_t pollers_count;
	uint64_t busy;
	uint64_t idle;
	uint32_t core_freq;
	uint64_t last_idle;
	uint64_t last_busy;
	bool in_interrupt;
	struct rpc_core_threads threads;
};

struct rpc_thread_info g_threads_info[RPC_MAX_THREADS];
struct rpc_poller_info g_pollers_info[RPC_MAX_POLLERS];
struct rpc_core_info g_cores_info[RPC_MAX_CORES];

static void
init_str_len(void)
{
	int i, j;

	for (i = 0; i < NUMBER_OF_TABS; i++) {
		for (j = 0; g_col_desc[i][j].name != NULL; j++) {
			g_col_desc[i][j].name_len = strlen(g_col_desc[i][j].name);
		}
	}
}

static void
free_rpc_threads_stats(struct rpc_thread_info *req)
{
	free(req->name);
	req->name = NULL;
	free(req->cpumask);
	req->cpumask = NULL;
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
rpc_decode_threads_array(struct spdk_json_val *val, struct rpc_thread_info *out,
			 uint64_t *current_threads_count)
{
	struct spdk_json_val *thread = val;
	uint64_t i = 0;
	int rc;

	/* Fetch the beginning of threads array */
	rc = spdk_json_find_array(thread, "threads", NULL, &thread);
	if (rc) {
		printf("Could not fetch threads array from JSON.\n");
		goto end;
	}

	for (thread = spdk_json_array_first(thread); thread != NULL; thread = spdk_json_next(thread)) {
		rc = spdk_json_decode_object(thread, rpc_thread_info_decoders,
					     SPDK_COUNTOF(rpc_thread_info_decoders), &out[i]);
		if (rc) {
			printf("Could not decode thread object from JSON.\n");
			break;
		}

		i++;
	}

end:

	*current_threads_count = i;
	return rc;
}

static void
free_rpc_poller(struct rpc_poller_info *poller)
{
	free(poller->name);
	poller->name = NULL;
	free(poller->state);
	poller->state = NULL;
}

static void
free_rpc_core_info(struct rpc_core_info *core_info, size_t size)
{
	struct rpc_core_threads *threads;
	struct rpc_core_thread_info *thread;
	uint64_t i, core_number;

	for (core_number = 0; core_number < size; core_number++) {
		threads = &core_info[core_number].threads;
		for (i = 0; i < threads->threads_count; i++) {
			thread = &threads->thread[i];
			free(thread->name);
			free(thread->cpumask);
		}
		free(threads->thread);
	}
}

static const struct spdk_json_object_decoder rpc_pollers_decoders[] = {
	{"name", offsetof(struct rpc_poller_info, name), spdk_json_decode_string},
	{"state", offsetof(struct rpc_poller_info, state), spdk_json_decode_string},
	{"id", offsetof(struct rpc_poller_info, id), spdk_json_decode_uint64},
	{"run_count", offsetof(struct rpc_poller_info, run_count), spdk_json_decode_uint64},
	{"busy_count", offsetof(struct rpc_poller_info, busy_count), spdk_json_decode_uint64},
	{"period_ticks", offsetof(struct rpc_poller_info, period_ticks), spdk_json_decode_uint64, true},
};

static int
rpc_decode_pollers_array(struct spdk_json_val *poller, struct rpc_poller_info *out,
			 uint64_t *poller_count,
			 const char *thread_name, uint64_t thread_name_length, uint64_t thread_id,
			 enum spdk_poller_type poller_type)
{
	int rc;

	for (poller = spdk_json_array_first(poller); poller != NULL; poller = spdk_json_next(poller)) {
		out[*poller_count].thread_id = thread_id;
		memcpy(out[*poller_count].thread_name, thread_name, sizeof(char) * thread_name_length);
		out[*poller_count].type = poller_type;

		rc = spdk_json_decode_object(poller, rpc_pollers_decoders,
					     SPDK_COUNTOF(rpc_pollers_decoders), &out[*poller_count]);
		if (rc) {
			printf("Could not decode poller object from JSON.\n");
			return rc;
		}

		(*poller_count)++;
		if (*poller_count == RPC_MAX_POLLERS) {
			return -1;
		}
	}

	return 0;
}

static const struct spdk_json_object_decoder rpc_thread_pollers_decoders[] = {
	{"name", offsetof(struct rpc_thread_info, name), spdk_json_decode_string},
	{"id", offsetof(struct rpc_thread_info, id), spdk_json_decode_uint64},
};

static int
rpc_decode_pollers_threads_array(struct spdk_json_val *val, struct rpc_poller_info *out,
				 uint32_t *num_pollers)
{
	struct spdk_json_val *thread = val, *poller;
	/* This is a temporary poller structure to hold thread name and id.
	 * It is filled with data only once per thread change and then
	 * that memory is copied to each poller running on that thread. */
	struct rpc_thread_info thread_info = {};
	uint64_t poller_count = 0, i, thread_name_length;
	int rc;
	const char *poller_typenames[] = { "active_pollers", "timed_pollers", "paused_pollers" };
	enum spdk_poller_type poller_types[] = { SPDK_ACTIVE_POLLER, SPDK_TIMED_POLLER, SPDK_PAUSED_POLLER };

	/* Fetch the beginning of threads array */
	rc = spdk_json_find_array(thread, "threads", NULL, &thread);
	if (rc) {
		printf("Could not fetch threads array from JSON.\n");
		goto end;
	}

	for (thread = spdk_json_array_first(thread); thread != NULL; thread = spdk_json_next(thread)) {
		rc = spdk_json_decode_object_relaxed(thread, rpc_thread_pollers_decoders,
						     SPDK_COUNTOF(rpc_thread_pollers_decoders), &thread_info);
		if (rc) {
			printf("Could not decode thread info from JSON.\n");
			goto end;
		}

		thread_name_length = strlen(thread_info.name);

		for (i = 0; i < SPDK_COUNTOF(poller_types); i++) {
			/* Find poller array */
			rc = spdk_json_find(thread, poller_typenames[i], NULL, &poller,
					    SPDK_JSON_VAL_ARRAY_BEGIN);
			if (rc) {
				printf("Could not fetch pollers array from JSON.\n");
				goto end;
			}

			rc = rpc_decode_pollers_array(poller, out, &poller_count, thread_info.name,
						      thread_name_length,
						      thread_info.id, poller_types[i]);
			if (rc) {
				printf("Could not decode the first object in pollers array.\n");
				goto end;
			}
		}
	}

	*num_pollers = poller_count;

end:
	/* Since we rely in spdk_json_object_decode() to free this value
	 * each time we rewrite it, we need to free the last allocation
	 * manually. */
	free(thread_info.name);

	if (rc) {
		*num_pollers = 0;
		for (i = 0; i < poller_count; i++) {
			free_rpc_poller(&out[i]);
		}
	}

	return rc;
}

static const struct spdk_json_object_decoder rpc_core_thread_info_decoders[] = {
	{"name", offsetof(struct rpc_core_thread_info, name), spdk_json_decode_string},
	{"id", offsetof(struct rpc_core_thread_info, id), spdk_json_decode_uint64},
	{"cpumask", offsetof(struct rpc_core_thread_info, cpumask), spdk_json_decode_string},
	{"elapsed", offsetof(struct rpc_core_thread_info, elapsed), spdk_json_decode_uint64},
};

static int
rpc_decode_core_threads_object(const struct spdk_json_val *val, void *out)
{
	struct rpc_core_thread_info *info = out;

	return spdk_json_decode_object(val, rpc_core_thread_info_decoders,
				       SPDK_COUNTOF(rpc_core_thread_info_decoders), info);
}

#define RPC_THREAD_ENTRY_SIZE (SPDK_COUNTOF(rpc_core_thread_info_decoders) * 2)

static int
rpc_decode_cores_lw_threads(const struct spdk_json_val *val, void *out)
{
	struct rpc_core_threads *threads = out;
	/* The number of thread entries received from RPC can be calculated using
	 * above define value (each JSON line = key + value, hence '* 2' ) and JSON
	 * 'val' value (-2 is to subtract VAL_OBJECT_BEGIN/END). */
	uint16_t threads_count = (spdk_json_val_len(val) - 2) / RPC_THREAD_ENTRY_SIZE;

	threads->thread = calloc(threads_count, sizeof(struct rpc_core_thread_info));
	if (!out) {
		fprintf(stderr, "Unable to allocate memory for a thread array.\n");
		return -1;
	}

	return spdk_json_decode_array(val, rpc_decode_core_threads_object, threads->thread, threads_count,
				      &threads->threads_count, sizeof(struct rpc_core_thread_info));
}

static const struct spdk_json_object_decoder rpc_core_info_decoders[] = {
	{"lcore", offsetof(struct rpc_core_info, lcore), spdk_json_decode_uint32},
	{"busy", offsetof(struct rpc_core_info, busy), spdk_json_decode_uint64},
	{"idle", offsetof(struct rpc_core_info, idle), spdk_json_decode_uint64},
	{"core_freq", offsetof(struct rpc_core_info, core_freq), spdk_json_decode_uint32, true},
	{"in_interrupt", offsetof(struct rpc_core_info, in_interrupt), spdk_json_decode_bool},
	{"lw_threads", offsetof(struct rpc_core_info, threads), rpc_decode_cores_lw_threads},
};

static int
rpc_decode_core_object(const struct spdk_json_val *val, void *out)
{
	struct rpc_core_info *info = out;

	return spdk_json_decode_object(val, rpc_core_info_decoders,
				       SPDK_COUNTOF(rpc_core_info_decoders), info);
}

static int
rpc_decode_cores_array(struct spdk_json_val *val, struct rpc_core_info *out,
		       uint32_t *current_cores_count)
{
	struct spdk_json_val *core = val;
	size_t cores_count;
	int rc;

	/* Fetch the beginning of reactors array. */
	rc = spdk_json_find_array(core, "reactors", NULL, &core);
	if (rc) {
		printf("Could not fetch cores array from JSON.");
		goto end;
	}

	rc = spdk_json_decode_array(core, rpc_decode_core_object, out, RPC_MAX_CORES, &cores_count,
				    sizeof(struct rpc_core_info));

	*current_cores_count = (uint32_t)cores_count;

end:
	return rc;
}

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
		return -1;
	}

	json_resp = spdk_jsonrpc_client_get_response(g_rpc_client);
	if (json_resp == NULL) {
		return -1;
	}

	/* Check for error response */
	if (json_resp->error != NULL) {
		spdk_jsonrpc_client_free_response(json_resp);
		return -1;
	}

	assert(json_resp->result);

	*resp = json_resp;

	return 0;
}

static uint64_t
get_cpu_usage(uint64_t busy_ticks, uint64_t idle_ticks)
{
	if (busy_ticks + idle_ticks > 0) {
		/* Increase numerator to convert fraction into decimal with
		 * additional precision */
		return busy_ticks * 10000 / (busy_ticks + idle_ticks);
	}

	return 0;
}

static int
subsort_threads(enum column_threads_type sort_column, const void *p1, const void *p2)
{
	const struct rpc_thread_info thread_info1 = *(struct rpc_thread_info *)p1;
	const struct rpc_thread_info thread_info2 = *(struct rpc_thread_info *)p2;
	uint64_t count1, count2;

	switch (sort_column) {
	case COL_THREADS_NAME:
		return strcmp(thread_info1.name, thread_info2.name);
	case COL_THREADS_CORE:
		count2 = thread_info1.core_num;
		count1 = thread_info2.core_num;
		break;
	case COL_THREADS_ACTIVE_POLLERS:
		count1 = thread_info1.active_pollers_count;
		count2 = thread_info2.active_pollers_count;
		break;
	case COL_THREADS_TIMED_POLLERS:
		count1 = thread_info1.timed_pollers_count;
		count2 = thread_info2.timed_pollers_count;
		break;
	case COL_THREADS_PAUSED_POLLERS:
		count1 = thread_info1.paused_pollers_count;
		count2 = thread_info2.paused_pollers_count;
		break;
	case COL_THREADS_IDLE_TIME:
		if (g_interval_data) {
			count1 = thread_info1.idle - thread_info1.last_idle;
			count2 = thread_info2.idle - thread_info2.last_idle;
		} else {
			count1 = thread_info1.idle;
			count2 = thread_info2.idle;
		}
		break;
	case COL_THREADS_BUSY_TIME:
		if (g_interval_data) {
			count1 = thread_info1.busy - thread_info1.last_busy;
			count2 = thread_info2.busy - thread_info2.last_busy;
		} else {
			count1 = thread_info1.busy;
			count2 = thread_info2.busy;
		}
		break;
	case COL_THREADS_CPU_USAGE:
		count1 = get_cpu_usage(thread_info1.busy - thread_info1.last_busy,
				       g_cores_info[thread_info1.core_num].busy + g_cores_info[thread_info1.core_num].idle);
		count2 = get_cpu_usage(thread_info2.busy - thread_info2.last_busy,
				       g_cores_info[thread_info2.core_num].busy + g_cores_info[thread_info2.core_num].idle);
		break;
	case COL_THREADS_NONE:
	default:
		return 0;
	}

	if (count2 > count1) {
		return 1;
	} else if (count2 < count1) {
		return -1;
	} else {
		return 0;
	}
}

static int
sort_threads(const void *p1, const void *p2)
{
	int res;

	res = subsort_threads(g_current_sort_col[THREADS_TAB], p1, p2);
	if (res == 0) {
		res = subsort_threads(g_current_sort_col2[THREADS_TAB], p1, p2);
	}
	return res;
}

static void
store_last_counters(uint64_t poller_id, uint64_t thread_id, uint64_t last_run_counter,
		    uint64_t last_busy_counter)
{
	struct run_counter_history *history;

	TAILQ_FOREACH(history, &g_run_counter_history, link) {
		if ((history->poller_id == poller_id) && (history->thread_id == thread_id)) {
			history->last_run_counter = last_run_counter;
			history->last_busy_counter = last_busy_counter;
			return;
		}
	}

	history = calloc(1, sizeof(*history));
	if (history == NULL) {
		fprintf(stderr, "Unable to allocate a history object in store_last_counters.\n");
		return;
	}
	history->poller_id = poller_id;
	history->thread_id = thread_id;
	history->last_run_counter = last_run_counter;
	history->last_busy_counter = last_busy_counter;

	TAILQ_INSERT_TAIL(&g_run_counter_history, history, link);
}

static int
get_thread_data(void)
{
	struct spdk_jsonrpc_client_response *json_resp = NULL;
	struct rpc_thread_info thread_info[RPC_MAX_THREADS], *thread;
	struct rpc_core_info *core_info;
	uint64_t i, j, k, current_threads_count = 0;
	int rc = 0;

	rc = rpc_send_req("thread_get_stats", &json_resp);
	if (rc) {
		return rc;
	}

	/* Decode json */
	memset(thread_info, 0, sizeof(struct rpc_thread_info) * RPC_MAX_THREADS);
	if (rpc_decode_threads_array(json_resp->result, thread_info, &current_threads_count)) {
		rc = -EINVAL;
		for (i = 0; i < current_threads_count; i++) {
			free_rpc_threads_stats(&thread_info[i]);
		}
		goto end;
	}

	pthread_mutex_lock(&g_thread_lock);

	/* This is to free allocated char arrays with old thread names */
	for (i = 0; i < g_last_threads_count; i++) {
		free_rpc_threads_stats(&g_threads_info[i]);
	}

	for (i = 0; i < current_threads_count; i++) {
		for (j = 0; j < g_last_threads_count; j++) {
			if (thread_info[i].id == g_threads_info[j].id) {
				thread_info[i].last_busy = g_threads_info[j].busy;
				thread_info[i].last_idle = g_threads_info[j].idle;
			}
		}
	}
	g_last_threads_count = current_threads_count;

	memcpy(g_threads_info, thread_info, sizeof(struct rpc_thread_info) * RPC_MAX_THREADS);

	for (i = 0; i < g_last_threads_count; i++) {
		g_threads_info[i].core_num = -1;
	}

	for (i = 0; i < g_last_cores_count; i++) {
		core_info = &g_cores_info[i];

		for (j = 0; j < core_info->threads.threads_count; j++) {
			for (k = 0; k < g_last_threads_count; k++) {
				/* For each thread on current core: check if it's ID also exists
				 * in g_thread_info data structure. If it does then assign current
				 * core's number to that thread, otherwise application state is inconsistent
				 * (e.g. scheduler is moving threads between cores). */
				thread = &g_threads_info[k];
				if (thread->id == core_info->threads.thread[j].id) {
					thread->core_num = core_info->lcore;
					break;
				}
			}
		}
	}

	qsort(g_threads_info, g_last_threads_count, sizeof(struct rpc_thread_info), sort_threads);

	pthread_mutex_unlock(&g_thread_lock);

end:
	spdk_jsonrpc_client_free_response(json_resp);
	return rc;
}

static uint64_t
get_last_run_counter(uint64_t poller_id, uint64_t thread_id)
{
	struct run_counter_history *history;

	TAILQ_FOREACH(history, &g_run_counter_history, link) {
		if ((history->poller_id == poller_id) && (history->thread_id == thread_id)) {
			return history->last_run_counter;
		}
	}

	return 0;
}

static uint64_t
get_last_busy_counter(uint64_t poller_id, uint64_t thread_id)
{
	struct run_counter_history *history;

	TAILQ_FOREACH(history, &g_run_counter_history, link) {
		if ((history->poller_id == poller_id) && (history->thread_id == thread_id)) {
			return history->last_busy_counter;
		}
	}

	return 0;
}

static int
subsort_pollers(enum column_pollers_type sort_column, const void *p1, const void *p2)
{
	const struct rpc_poller_info *poller1 = (struct rpc_poller_info *)p1;
	const struct rpc_poller_info *poller2 = (struct rpc_poller_info *)p2;
	uint64_t count1, count2;
	uint64_t last_busy_counter1, last_busy_counter2;

	switch (sort_column) {
	case COL_POLLERS_NAME:
		return strcmp(poller1->name, poller2->name);
	case COL_POLLERS_TYPE:
		return poller1->type - poller2->type;
	case COL_POLLERS_THREAD_NAME:
		return strcmp(poller1->thread_name, poller2->thread_name);
	case COL_POLLERS_RUN_COUNTER:
		if (g_interval_data) {
			count1 = poller1->run_count - get_last_run_counter(poller1->id, poller1->thread_id);
			count2 = poller2->run_count - get_last_run_counter(poller2->id, poller2->thread_id);
		} else {
			count1 = poller1->run_count;
			count2 = poller2->run_count;
		}
		break;
	case COL_POLLERS_PERIOD:
		count1 = poller1->period_ticks;
		count2 = poller2->period_ticks;
		break;
	case COL_POLLERS_BUSY_COUNT:
		count1 = poller1->busy_count;
		count2 = poller2->busy_count;
		if (g_interval_data) {
			last_busy_counter1 = get_last_busy_counter(poller1->id, poller1->thread_id);
			last_busy_counter2 = get_last_busy_counter(poller2->id, poller2->thread_id);
			if (count1 > last_busy_counter1) {
				count1 -= last_busy_counter1;
			}
			if (count2 > last_busy_counter2) {
				count2 -= last_busy_counter2;
			}
		}
		break;
	case COL_POLLERS_NONE:
	default:
		return 0;
	}

	if (count2 > count1) {
		return 1;
	} else if (count2 < count1) {
		return -1;
	} else {
		return 0;
	}
}

static int
sort_pollers(const void *p1, const void *p2)
{
	int rc;

	rc = subsort_pollers(g_current_sort_col[POLLERS_TAB], p1, p2);
	if (rc == 0) {
		rc = subsort_pollers(g_current_sort_col2[POLLERS_TAB], p1, p2);
	}
	return rc;
}

static int
get_pollers_data(void)
{
	struct spdk_jsonrpc_client_response *json_resp = NULL;
	int rc = 0;
	uint64_t i = 0;
	uint32_t current_pollers_count;
	struct rpc_poller_info pollers_info[RPC_MAX_POLLERS];

	rc = rpc_send_req("thread_get_pollers", &json_resp);
	if (rc) {
		return rc;
	}

	/* Decode json */
	memset(&pollers_info, 0, sizeof(pollers_info));
	if (rpc_decode_pollers_threads_array(json_resp->result, pollers_info, &current_pollers_count)) {
		rc = -EINVAL;
		for (i = 0; i < current_pollers_count; i++) {
			free_rpc_poller(&pollers_info[i]);
		}
		goto end;
	}

	pthread_mutex_lock(&g_thread_lock);

	/* Save last run counter of each poller before updating g_pollers_stats. */
	for (i = 0; i < g_last_pollers_count; i++) {
		store_last_counters(g_pollers_info[i].id, g_pollers_info[i].thread_id,
				    g_pollers_info[i].run_count, g_pollers_info[i].busy_count);
	}

	/* Free old pollers values before allocating memory for new ones */
	for (i = 0; i < g_last_pollers_count; i++) {
		free_rpc_poller(&g_pollers_info[i]);
	}

	g_last_pollers_count = current_pollers_count;

	qsort(&pollers_info, g_last_pollers_count, sizeof(struct rpc_poller_info), sort_pollers);

	memcpy(&g_pollers_info, &pollers_info, sizeof(struct rpc_poller_info) * g_last_pollers_count);

	pthread_mutex_unlock(&g_thread_lock);

end:
	spdk_jsonrpc_client_free_response(json_resp);
	return rc;
}

static int
subsort_cores(enum column_cores_type sort_column, const void *p1, const void *p2)
{
	const struct rpc_core_info core_info1 = *(struct rpc_core_info *)p1;
	const struct rpc_core_info core_info2 = *(struct rpc_core_info *)p2;
	uint64_t count1, count2;

	switch (sort_column) {
	case COL_CORES_CORE:
		count1 = core_info2.lcore;
		count2 = core_info1.lcore;
		break;
	case COL_CORES_THREADS:
		count1 = core_info1.threads.threads_count;
		count2 = core_info2.threads.threads_count;
		break;
	case COL_CORES_POLLERS:
		count1 = core_info1.pollers_count;
		count2 = core_info2.pollers_count;
		break;
	case COL_CORES_IDLE_TIME:
		if (g_interval_data) {
			count1 = core_info1.last_idle - core_info1.idle;
			count2 = core_info2.last_idle - core_info2.idle;
		} else {
			count1 = core_info1.idle;
			count2 = core_info2.idle;
		}
		break;
	case COL_CORES_BUSY_TIME:
		if (g_interval_data) {
			count1 = core_info1.last_busy - core_info1.busy;
			count2 = core_info2.last_busy - core_info2.busy;
		} else {
			count1 = core_info1.busy;
			count2 = core_info2.busy;
		}
		break;
	case COL_CORES_CORE_FREQ:
		count1 = core_info1.core_freq;
		count2 = core_info2.core_freq;
		break;
	case COL_CORES_INTR:
		count1 = core_info1.in_interrupt;
		count2 = core_info2.in_interrupt;
		break;
	case COL_CORES_CPU_USAGE:
		count1 = get_cpu_usage(core_info1.last_busy - core_info1.busy,
				       core_info1.last_idle - core_info1.idle);
		count2 = get_cpu_usage(core_info2.last_busy - core_info2.busy,
				       core_info2.last_idle - core_info2.idle);
		break;
	case COL_CORES_NONE:
	default:
		return 0;
	}

	if (count2 > count1) {
		return 1;
	} else if (count2 < count1) {
		return -1;
	} else {
		return 0;
	}
}

static int
sort_cores(const void *p1, const void *p2)
{
	int rc;

	rc = subsort_cores(g_current_sort_col[CORES_TAB], p1, p2);
	if (rc == 0) {
		return subsort_cores(g_current_sort_col[CORES_TAB], p1, p2);
	}
	return rc;
}

static int
get_cores_data(void)
{
	struct spdk_jsonrpc_client_response *json_resp = NULL;
	struct rpc_core_info *core_info;
	uint64_t i, j, k;
	uint32_t current_cores_count;
	struct rpc_core_info cores_info[RPC_MAX_CORES];
	int rc = 0;

	rc = rpc_send_req("framework_get_reactors", &json_resp);
	if (rc) {
		return rc;
	}

	/* Decode json */
	memset(cores_info, 0, sizeof(struct rpc_core_info) * RPC_MAX_CORES);
	if (rpc_decode_cores_array(json_resp->result, cores_info, &current_cores_count)) {
		rc = -EINVAL;
		goto end;
	}

	pthread_mutex_lock(&g_thread_lock);
	for (i = 0; i < current_cores_count; i++) {
		for (j = 0; j < g_last_cores_count; j++) {
			if (cores_info[i].lcore == g_cores_info[j].lcore) {
				cores_info[i].last_busy = g_cores_info[j].busy;
				cores_info[i].last_idle = g_cores_info[j].idle;
			}
			/* Do not consider threads which changed cores when issuing
			 * RPCs to get_core_data and get_thread_data and threads
			 * not currently assigned to this core. */
			if ((int)cores_info[i].lcore == g_threads_info[j].core_num) {
				cores_info[i].pollers_count += g_threads_info[j].active_pollers_count +
							       g_threads_info[j].timed_pollers_count +
							       g_threads_info[j].paused_pollers_count;
			}
		}
	}

	/* Free old cores values before allocating memory for new ones */
	free_rpc_core_info(g_cores_info, current_cores_count);
	memcpy(g_cores_info, cores_info, sizeof(struct rpc_core_info) * current_cores_count);

	for (i = 0; i < g_last_cores_count; i++) {
		core_info = &g_cores_info[i];

		core_info->threads.thread = cores_info[i].threads.thread;

		for (j = 0; j < core_info->threads.threads_count; j++) {
			memcpy(&core_info->threads.thread[j], &cores_info[i].threads.thread[j],
			       sizeof(struct rpc_core_thread_info));
			for (k = 0; k < g_last_threads_count; k++) {
				if (core_info->threads.thread[j].id == g_threads_info[k].id) {
					g_threads_info[k].core_num = core_info->lcore;
				}
			}
		}
	}

	g_last_cores_count = current_cores_count;

	qsort(&g_cores_info, g_last_cores_count, sizeof(struct rpc_core_info), sort_cores);

end:
	pthread_mutex_unlock(&g_thread_lock);
	spdk_jsonrpc_client_free_response(json_resp);
	return rc;
}

enum str_alignment {
	ALIGN_LEFT,
	ALIGN_RIGHT,
};

static void
print_max_len(WINDOW *win, int row, uint16_t col, uint16_t max_len, enum str_alignment alignment,
	      const char *string)
{
	const char dots[] = "...";
	int DOTS_STR_LEN = sizeof(dots) / sizeof(dots[0]);
	char tmp_str[MAX_STRING_LEN];
	int len, max_col, max_str, cmp_len;
	int max_row;

	len = strlen(string);
	getmaxyx(win, max_row, max_col);

	if (row > max_row) {
		/* We are in a process of resizing and this may happen */
		return;
	}

	if (max_len != 0 && col + max_len < max_col) {
		max_col = col + max_len;
	}

	max_str = max_col - col;

	if (max_str <= DOTS_STR_LEN + 1) {
		/* No space to print anything, but we have to let a user know about it */
		mvwprintw(win, row, max_col - DOTS_STR_LEN - 1, "...");
		refresh();
		wrefresh(win);
		return;
	}

	if (max_len) {
		if (alignment == ALIGN_LEFT) {
			snprintf(tmp_str, max_str, "%s%*c", string, max_len - len - 1, ' ');
		} else {
			snprintf(tmp_str, max_str, "%*c%s", max_len - len - 1, ' ', string);
		}
		cmp_len = max_len - 1;
	} else {
		snprintf(tmp_str, max_str, "%s", string);
		cmp_len = len;
	}

	if (col + cmp_len > max_col - 1) {
		snprintf(&tmp_str[max_str - DOTS_STR_LEN - 2], DOTS_STR_LEN, "%s", dots);
	}

	mvwprintw(win, row, col, "%s", tmp_str);

	refresh();
	wrefresh(win);
}

static void
draw_menu_win(void)
{
	wbkgd(g_menu_win, COLOR_PAIR(2));
	box(g_menu_win, 0, 0);
	print_max_len(g_menu_win, 1, 1, 0, ALIGN_LEFT,
		      "  [q] Quit  |  [1-3][Tab] Switch tab  |  [PgUp] Previous page  |  [PgDown] Next page  |  [Enter] Item details  |  [h] Help");
}

static void
draw_tab_win(enum tabs tab)
{
	uint16_t col;
	uint8_t white_spaces = TABS_SPACING * NUMBER_OF_TABS;

	wbkgd(g_tab_win[tab], COLOR_PAIR(2));
	box(g_tab_win[tab], 0, 0);

	col = ((g_max_col - white_spaces) / NUMBER_OF_TABS / 2) - (strlen(g_tab_title[tab]) / 2) -
	      TABS_SPACING;
	print_max_len(g_tab_win[tab], 1, col, 0, ALIGN_LEFT, g_tab_title[tab]);
}

static void
draw_tabs(enum tabs tab_index, uint8_t sort_col, uint8_t sort_col2)
{
	struct col_desc *col_desc = g_col_desc[tab_index];
	WINDOW *tab = g_tabs[tab_index];
	int i, j;
	uint16_t offset, draw_offset;
	uint16_t tab_height = g_max_row - MENU_WIN_HEIGHT - TAB_WIN_HEIGHT - 3;

	for (i = 0; col_desc[i].name != NULL; i++) {
		if (col_desc[i].disabled) {
			continue;
		}

		offset = 1;
		for (j = i; j != 0; j--) {
			if (!col_desc[j - 1].disabled) {
				offset += col_desc[j - 1].max_data_string;
				offset += col_desc[j - 1].name_len % 2 + 1;
			}
		}

		draw_offset = offset + (col_desc[i].max_data_string / 2) - (col_desc[i].name_len / 2);

		if (i == sort_col) {
			wattron(tab, COLOR_PAIR(4));
			print_max_len(tab, 1, draw_offset, 0, ALIGN_LEFT, col_desc[i].name);
			wattroff(tab, COLOR_PAIR(4));
		} else if (i == sort_col2) {
			wattron(tab, COLOR_PAIR(3));
			print_max_len(tab, 1, draw_offset, 0, ALIGN_LEFT, col_desc[i].name);
			wattroff(tab, COLOR_PAIR(3));
		} else {
			print_max_len(tab, 1, draw_offset, 0, ALIGN_LEFT, col_desc[i].name);
		}

		if (offset != 1) {
			print_max_len(tab, 1, offset - 1, 0, ALIGN_LEFT, "|");
		}
	}

	print_max_len(tab, 2, 1, 0, ALIGN_LEFT, ""); /* Move to next line */
	whline(tab, ACS_HLINE, g_max_col - 2);

	/* Border lines */
	mvwhline(tab, 0, 1, ACS_HLINE, g_max_col - 2);
	mvwhline(tab, tab_height, 1, ACS_HLINE, g_max_col - 2);

	wrefresh(tab);
}

static void
resize_interface(enum tabs tab)
{
	int i;

	clear();
	wclear(g_menu_win);
	mvwin(g_menu_win, g_max_row - MENU_WIN_SPACING, MENU_WIN_LOCATION_COL);
	wresize(g_menu_win, MENU_WIN_HEIGHT, g_max_col);
	draw_menu_win();

	for (i = 0; i < NUMBER_OF_TABS; i++) {
		wclear(g_tabs[i]);
		wresize(g_tabs[i], g_max_row - MENU_WIN_HEIGHT - TAB_WIN_HEIGHT - 2, g_max_col);
		mvwin(g_tabs[i], TABS_LOCATION_ROW, TABS_LOCATION_COL);
		draw_tabs(i, g_current_sort_col[i], g_current_sort_col2[i]);
	}

	draw_tabs(tab, g_current_sort_col[tab], g_current_sort_col2[tab]);

	for (i = 0; i < NUMBER_OF_TABS; i++) {
		wclear(g_tab_win[i]);
		wresize(g_tab_win[i], TAB_WIN_HEIGHT,
			(g_max_col - (TABS_SPACING * NUMBER_OF_TABS)) / NUMBER_OF_TABS);
		mvwin(g_tab_win[i], TAB_WIN_LOCATION_ROW, 1 + (g_max_col / NUMBER_OF_TABS) * i);
		draw_tab_win(i);
	}

	update_panels();
	doupdate();
}

static void
switch_tab(enum tabs tab)
{
	wclear(g_tabs[tab]);
	draw_tabs(tab, g_current_sort_col[tab], g_current_sort_col2[tab]);
	top_panel(g_panels[tab]);
	update_panels();
	doupdate();
}

static void
get_time_str(uint64_t ticks, char *time_str)
{
	uint64_t time;

	time = ticks * SPDK_SEC_TO_USEC / g_tick_rate;
	snprintf(time_str, MAX_TIME_STR_LEN, "%" PRIu64, time);
}

static void
draw_row_background(uint8_t item_index, uint8_t tab)
{
	int k;

	if (item_index == g_selected_row) {
		wattron(g_tabs[tab], COLOR_PAIR(2));
	}
	for (k = 1; k < g_max_col - 1; k++) {
		mvwprintw(g_tabs[tab], TABS_DATA_START_ROW + item_index, k, " ");
	}
}

static void
get_cpu_usage_str(uint64_t busy_ticks, uint64_t total_ticks, char *cpu_str)
{
	if (total_ticks > 0) {
		snprintf(cpu_str, MAX_CPU_STR_LEN, "%.2f",
			 (double)(busy_ticks) * 100 / (double)(total_ticks));
	} else {
		cpu_str[0] = '\0';
	}
}

static void
draw_thread_tab_row(uint64_t current_row, uint8_t item_index)
{
	struct col_desc *col_desc = g_col_desc[THREADS_TAB];
	uint16_t col = TABS_DATA_START_COL;
	int core_num;
	char pollers_number[MAX_POLLER_COUNT_STR_LEN], idle_time[MAX_TIME_STR_LEN],
	     busy_time[MAX_TIME_STR_LEN], core_str[MAX_CORE_MASK_STR_LEN],
	     cpu_usage[MAX_CPU_STR_LEN];

	if (!col_desc[COL_THREADS_NAME].disabled) {
		print_max_len(g_tabs[THREADS_TAB], TABS_DATA_START_ROW + item_index, col,
			      col_desc[COL_THREADS_NAME].max_data_string, ALIGN_LEFT, g_threads_info[current_row].name);
		col += col_desc[COL_THREADS_NAME].max_data_string;
	}

	if (!col_desc[COL_THREADS_CORE].disabled) {
		snprintf(core_str, MAX_CORE_STR_LEN, "%d", g_threads_info[current_row].core_num);
		print_max_len(g_tabs[THREADS_TAB], TABS_DATA_START_ROW + item_index,
			      col, col_desc[COL_THREADS_CORE].max_data_string, ALIGN_RIGHT, core_str);
		col += col_desc[COL_THREADS_CORE].max_data_string + 2;
	}

	if (!col_desc[COL_THREADS_ACTIVE_POLLERS].disabled) {
		snprintf(pollers_number, MAX_POLLER_COUNT_STR_LEN, "%ld",
			 g_threads_info[current_row].active_pollers_count);
		print_max_len(g_tabs[THREADS_TAB], TABS_DATA_START_ROW + item_index,
			      col + (col_desc[COL_THREADS_ACTIVE_POLLERS].name_len / 2),
			      col_desc[COL_THREADS_ACTIVE_POLLERS].max_data_string, ALIGN_LEFT, pollers_number);
		col += col_desc[COL_THREADS_ACTIVE_POLLERS].max_data_string + 2;
	}

	if (!col_desc[COL_THREADS_TIMED_POLLERS].disabled) {
		snprintf(pollers_number, MAX_POLLER_COUNT_STR_LEN, "%ld",
			 g_threads_info[current_row].timed_pollers_count);
		print_max_len(g_tabs[THREADS_TAB], TABS_DATA_START_ROW + item_index,
			      col + (col_desc[COL_THREADS_TIMED_POLLERS].name_len / 2),
			      col_desc[COL_THREADS_TIMED_POLLERS].max_data_string, ALIGN_LEFT, pollers_number);
		col += col_desc[COL_THREADS_TIMED_POLLERS].max_data_string + 1;
	}

	if (!col_desc[COL_THREADS_PAUSED_POLLERS].disabled) {
		snprintf(pollers_number, MAX_POLLER_COUNT_STR_LEN, "%ld",
			 g_threads_info[current_row].paused_pollers_count);
		print_max_len(g_tabs[THREADS_TAB], TABS_DATA_START_ROW + item_index,
			      col + (col_desc[COL_THREADS_PAUSED_POLLERS].name_len / 2),
			      col_desc[COL_THREADS_PAUSED_POLLERS].max_data_string, ALIGN_LEFT, pollers_number);
		col += col_desc[COL_THREADS_PAUSED_POLLERS].max_data_string + 2;
	}

	if (!col_desc[COL_THREADS_IDLE_TIME].disabled) {
		if (g_interval_data == true) {
			get_time_str(g_threads_info[current_row].idle - g_threads_info[current_row].last_idle, idle_time);
		} else {
			get_time_str(g_threads_info[current_row].idle, idle_time);
		}
		print_max_len(g_tabs[THREADS_TAB], TABS_DATA_START_ROW + item_index, col,
			      col_desc[COL_THREADS_IDLE_TIME].max_data_string, ALIGN_RIGHT, idle_time);
		col += col_desc[COL_THREADS_IDLE_TIME].max_data_string;
	}

	if (!col_desc[COL_THREADS_BUSY_TIME].disabled) {
		if (g_interval_data == true) {
			get_time_str(g_threads_info[current_row].busy - g_threads_info[current_row].last_busy, busy_time);
		} else {
			get_time_str(g_threads_info[current_row].busy, busy_time);
		}
		print_max_len(g_tabs[THREADS_TAB], TABS_DATA_START_ROW + item_index, col,
			      col_desc[COL_THREADS_BUSY_TIME].max_data_string, ALIGN_RIGHT, busy_time);
		col += col_desc[COL_THREADS_BUSY_TIME].max_data_string + 3;
	}

	if (!col_desc[COL_THREADS_CPU_USAGE].disabled) {
		core_num = g_threads_info[current_row].core_num;
		if (core_num >= 0 && core_num < RPC_MAX_CORES) {
			get_cpu_usage_str(g_threads_info[current_row].busy - g_threads_info[current_row].last_busy,
					  g_cores_info[core_num].busy + g_cores_info[core_num].idle,
					  cpu_usage);
		} else {
			snprintf(cpu_usage, sizeof(cpu_usage), "n/a");
		}

		print_max_len(g_tabs[THREADS_TAB], TABS_DATA_START_ROW + item_index, col,
			      col_desc[COL_THREADS_CPU_USAGE].max_data_string, ALIGN_RIGHT, cpu_usage);
	}
}

static uint8_t
refresh_threads_tab(uint8_t current_page)
{
	uint64_t i, j, threads_count;
	uint16_t empty_col = 0;
	uint8_t max_pages, item_index;

	threads_count = g_last_threads_count;

	max_pages = (threads_count + g_max_data_rows - 1) / g_max_data_rows;

	for (i = current_page * g_max_data_rows;
	     i < (uint64_t)((current_page + 1) * g_max_data_rows);
	     i++) {
		item_index = i - (current_page * g_max_data_rows);

		/* When number of threads decreases, this will print spaces in places
		 * where non existent threads were previously displayed. */
		if (i >= threads_count) {
			for (j = 1; j < (uint64_t)g_max_col - 1; j++) {
				mvwprintw(g_tabs[THREADS_TAB], item_index + TABS_DATA_START_ROW, j, " ");
			}

			empty_col++;
			continue;
		}

		draw_row_background(item_index, THREADS_TAB);
		draw_thread_tab_row(i, item_index);

		if (item_index == g_selected_row) {
			wattroff(g_tabs[THREADS_TAB], COLOR_PAIR(2));
		}
	}

	g_max_selected_row = i - current_page * g_max_data_rows - 1 - empty_col;

	return max_pages;
}

static void
draw_poller_tab_row(uint64_t current_row, uint8_t item_index)
{
	struct col_desc *col_desc = g_col_desc[POLLERS_TAB];
	uint64_t last_run_counter, last_busy_counter;
	uint16_t col = TABS_DATA_START_COL;
	char run_count[MAX_POLLER_RUN_COUNT], period_ticks[MAX_PERIOD_STR_LEN],
	     status[MAX_POLLER_IND_STR_LEN];

	last_busy_counter = get_last_busy_counter(g_pollers_info[current_row].id,
			    g_pollers_info[current_row].thread_id);

	if (!col_desc[COL_POLLERS_NAME].disabled) {
		print_max_len(g_tabs[POLLERS_TAB], TABS_DATA_START_ROW + item_index, col + 1,
			      col_desc[COL_POLLERS_NAME].max_data_string, ALIGN_LEFT, g_pollers_info[current_row].name);
		col += col_desc[COL_POLLERS_NAME].max_data_string + 2;
	}

	if (!col_desc[COL_POLLERS_TYPE].disabled) {
		print_max_len(g_tabs[POLLERS_TAB], TABS_DATA_START_ROW + item_index, col,
			      col_desc[COL_POLLERS_TYPE].max_data_string, ALIGN_LEFT,
			      poller_type_str[g_pollers_info[current_row].type]);
		col += col_desc[COL_POLLERS_TYPE].max_data_string + 2;
	}

	if (!col_desc[COL_POLLERS_THREAD_NAME].disabled) {
		print_max_len(g_tabs[POLLERS_TAB], TABS_DATA_START_ROW + item_index, col,
			      col_desc[COL_POLLERS_THREAD_NAME].max_data_string, ALIGN_LEFT,
			      g_pollers_info[current_row].thread_name);
		col += col_desc[COL_POLLERS_THREAD_NAME].max_data_string + 1;
	}

	if (!col_desc[COL_POLLERS_RUN_COUNTER].disabled) {
		last_run_counter = get_last_run_counter(g_pollers_info[current_row].id,
							g_pollers_info[current_row].thread_id);
		if (g_interval_data == true) {
			snprintf(run_count, MAX_POLLER_RUN_COUNT, "%" PRIu64,
				 g_pollers_info[current_row].run_count - last_run_counter);
		} else {
			snprintf(run_count, MAX_POLLER_RUN_COUNT, "%" PRIu64, g_pollers_info[current_row].run_count);
		}
		print_max_len(g_tabs[POLLERS_TAB], TABS_DATA_START_ROW + item_index, col,
			      col_desc[COL_POLLERS_RUN_COUNTER].max_data_string, ALIGN_RIGHT, run_count);
		col += col_desc[COL_POLLERS_RUN_COUNTER].max_data_string;
	}

	if (!col_desc[COL_POLLERS_PERIOD].disabled) {
		if (g_pollers_info[current_row].period_ticks != 0) {
			get_time_str(g_pollers_info[current_row].period_ticks, period_ticks);
			print_max_len(g_tabs[POLLERS_TAB], TABS_DATA_START_ROW + item_index, col,
				      col_desc[COL_POLLERS_PERIOD].max_data_string, ALIGN_RIGHT, period_ticks);
		}
		col += col_desc[COL_POLLERS_PERIOD].max_data_string + 7;
	}

	if (!col_desc[COL_POLLERS_BUSY_COUNT].disabled) {
		if (g_pollers_info[current_row].busy_count > last_busy_counter) {
			if (g_interval_data == true) {
				snprintf(status, MAX_POLLER_IND_STR_LEN, "Busy (%" PRIu64 ")",
					 g_pollers_info[current_row].busy_count - last_busy_counter);
			} else {
				snprintf(status, MAX_POLLER_IND_STR_LEN, "Busy (%" PRIu64 ")",
					 g_pollers_info[current_row].busy_count);
			}

			if (item_index != g_selected_row) {
				wattron(g_tabs[POLLERS_TAB], COLOR_PAIR(6));
				print_max_len(g_tabs[POLLERS_TAB], TABS_DATA_START_ROW + item_index, col,
					      col_desc[COL_POLLERS_BUSY_COUNT].max_data_string, ALIGN_LEFT, status);
				wattroff(g_tabs[POLLERS_TAB], COLOR_PAIR(6));
			} else {
				wattron(g_tabs[POLLERS_TAB], COLOR_PAIR(8));
				print_max_len(g_tabs[POLLERS_TAB], TABS_DATA_START_ROW + item_index, col,
					      col_desc[COL_POLLERS_BUSY_COUNT].max_data_string, ALIGN_LEFT, status);
				wattroff(g_tabs[POLLERS_TAB], COLOR_PAIR(8));
			}
		} else {
			if (g_interval_data == true) {
				snprintf(status, MAX_POLLER_IND_STR_LEN, "%s", "Idle");
			} else {
				snprintf(status, MAX_POLLER_IND_STR_LEN, "Idle (%" PRIu64 ")",
					 g_pollers_info[current_row].busy_count);
			}

			if (item_index != g_selected_row) {
				wattron(g_tabs[POLLERS_TAB], COLOR_PAIR(7));
				print_max_len(g_tabs[POLLERS_TAB], TABS_DATA_START_ROW + item_index, col,
					      col_desc[COL_POLLERS_BUSY_COUNT].max_data_string, ALIGN_LEFT, status);
				wattroff(g_tabs[POLLERS_TAB], COLOR_PAIR(7));
			} else {
				wattron(g_tabs[POLLERS_TAB], COLOR_PAIR(9));
				print_max_len(g_tabs[POLLERS_TAB], TABS_DATA_START_ROW + item_index, col,
					      col_desc[COL_POLLERS_BUSY_COUNT].max_data_string, ALIGN_LEFT, status);
				wattroff(g_tabs[POLLERS_TAB], COLOR_PAIR(9));
			}
		}
	}
}

static uint8_t
refresh_pollers_tab(uint8_t current_page)
{
	uint64_t i, j;
	uint16_t empty_col = 0;
	uint8_t max_pages, item_index;

	max_pages = (g_last_pollers_count + g_max_data_rows - 1) / g_max_data_rows;

	/* Display info */
	for (i = current_page * g_max_data_rows;
	     i < (uint64_t)((current_page + 1) * g_max_data_rows);
	     i++) {
		item_index = i - (current_page * g_max_data_rows);

		/* When number of pollers decreases, this will print spaces in places
		 * where non existent pollers were previously displayed. */
		if (i >= g_last_pollers_count) {
			for (j = 1; j < (uint64_t)g_max_col - 1; j++) {
				mvwprintw(g_tabs[POLLERS_TAB], item_index + TABS_DATA_START_ROW, j, " ");
			}

			empty_col++;
			continue;
		}

		draw_row_background(item_index, POLLERS_TAB);
		draw_poller_tab_row(i, item_index);

		if (item_index == g_selected_row) {
			wattroff(g_tabs[POLLERS_TAB], COLOR_PAIR(2));
		}
	}

	g_max_selected_row = i - current_page * g_max_data_rows - 1 - empty_col;

	return max_pages;
}

static void
draw_core_tab_row(uint64_t current_row, uint8_t item_index)
{
	struct col_desc *col_desc = g_col_desc[CORES_TAB];
	uint16_t col = 1;
	char core[MAX_CORE_STR_LEN], threads_number[MAX_THREAD_COUNT_STR_LEN],  cpu_usage[MAX_CPU_STR_LEN],
	     pollers_number[MAX_POLLER_COUNT_STR_LEN], idle_time[MAX_TIME_STR_LEN],
	     busy_time[MAX_TIME_STR_LEN], core_freq[MAX_CORE_FREQ_STR_LEN],
	     in_interrupt[MAX_INTR_LEN];

	snprintf(threads_number, MAX_THREAD_COUNT_STR_LEN, "%ld",
		 g_cores_info[current_row].threads.threads_count);
	snprintf(pollers_number, MAX_POLLER_COUNT_STR_LEN, "%ld", g_cores_info[current_row].pollers_count);

	if (!col_desc[COL_CORES_CORE].disabled) {
		snprintf(core, MAX_CORE_STR_LEN, "%d", g_cores_info[current_row].lcore);
		print_max_len(g_tabs[CORES_TAB], TABS_DATA_START_ROW + item_index, col,
			      col_desc[COL_CORES_CORE].max_data_string, ALIGN_RIGHT, core);
		col += col_desc[COL_CORES_CORE].max_data_string + 2;
	}

	if (!col_desc[COL_CORES_THREADS].disabled) {
		print_max_len(g_tabs[CORES_TAB], TABS_DATA_START_ROW + item_index,
			      col + (col_desc[COL_CORES_THREADS].name_len / 2), col_desc[COL_CORES_THREADS].max_data_string,
			      ALIGN_LEFT, threads_number);
		col += col_desc[COL_CORES_THREADS].max_data_string + 2;
	}

	if (!col_desc[COL_CORES_POLLERS].disabled) {
		print_max_len(g_tabs[CORES_TAB], TABS_DATA_START_ROW + item_index,
			      col + (col_desc[COL_CORES_POLLERS].name_len / 2), col_desc[COL_CORES_POLLERS].max_data_string,
			      ALIGN_LEFT, pollers_number);
		col += col_desc[COL_CORES_POLLERS].max_data_string;
	}

	if (!col_desc[COL_CORES_IDLE_TIME].disabled) {
		if (g_interval_data == true) {
			get_time_str(g_cores_info[current_row].idle - g_cores_info[current_row].last_idle, idle_time);
		} else {
			get_time_str(g_cores_info[current_row].idle, idle_time);
		}
		print_max_len(g_tabs[CORES_TAB], TABS_DATA_START_ROW + item_index, col,
			      col_desc[COL_CORES_IDLE_TIME].max_data_string, ALIGN_RIGHT, idle_time);
		col += col_desc[COL_CORES_IDLE_TIME].max_data_string + 2;
	}

	if (!col_desc[COL_CORES_BUSY_TIME].disabled) {
		if (g_interval_data == true) {
			get_time_str(g_cores_info[current_row].busy - g_cores_info[current_row].last_busy, busy_time);
		} else {
			get_time_str(g_cores_info[current_row].busy, busy_time);
		}
		print_max_len(g_tabs[CORES_TAB], TABS_DATA_START_ROW + item_index, col,
			      col_desc[COL_CORES_BUSY_TIME].max_data_string, ALIGN_RIGHT, busy_time);
		col += col_desc[COL_CORES_BUSY_TIME].max_data_string + 2;
	}

	if (!col_desc[COL_CORES_CORE_FREQ].disabled) {
		if (!g_cores_info[current_row].core_freq) {
			snprintf(core_freq, MAX_CORE_FREQ_STR_LEN, "%s", "N/A");
		} else {
			snprintf(core_freq, MAX_CORE_FREQ_STR_LEN, "%" PRIu32,
				 g_cores_info[current_row].core_freq);
		}
		print_max_len(g_tabs[CORES_TAB], TABS_DATA_START_ROW + item_index, col,
			      col_desc[COL_CORES_CORE_FREQ].max_data_string, ALIGN_RIGHT, core_freq);
		col += col_desc[COL_CORES_CORE_FREQ].max_data_string + 2;
	}

	if (!col_desc[COL_CORES_INTR].disabled) {
		snprintf(in_interrupt, MAX_INTR_LEN, "%s", g_cores_info[current_row].in_interrupt ? "Yes" : "No");
		print_max_len(g_tabs[CORES_TAB], TABS_DATA_START_ROW + item_index,
			      col + (col_desc[COL_CORES_INTR].name_len / 2), col_desc[COL_CORES_INTR].max_data_string,
			      ALIGN_LEFT, in_interrupt);
		col += col_desc[COL_CORES_INTR].max_data_string + 1;
	}

	if (!col_desc[COL_CORES_CPU_USAGE].disabled) {
		get_cpu_usage_str(g_cores_info[current_row].busy - g_cores_info[current_row].last_busy,
				  g_cores_info[current_row].busy + g_cores_info[current_row].idle,
				  cpu_usage);
		print_max_len(g_tabs[CORES_TAB], TABS_DATA_START_ROW + item_index, col,
			      col_desc[COL_CORES_CPU_USAGE].max_data_string, ALIGN_RIGHT, cpu_usage);
	}
}

static uint8_t
refresh_cores_tab(uint8_t current_page)
{
	uint64_t i;
	uint16_t count = 0;
	uint8_t max_pages, item_index;

	count = g_last_cores_count;

	max_pages = (count + g_max_row - WINDOW_HEADER - 1) / (g_max_row - WINDOW_HEADER);

	for (i = current_page * g_max_data_rows;
	     i < spdk_min(count, (uint64_t)((current_page + 1) * g_max_data_rows));
	     i++) {
		item_index = i - (current_page * g_max_data_rows);

		draw_row_background(item_index, CORES_TAB);
		draw_core_tab_row(i, item_index);

		if (item_index == g_selected_row) {
			wattroff(g_tabs[CORES_TAB], COLOR_PAIR(2));
		}
	}

	g_max_selected_row = i - current_page * g_max_data_rows - 1;

	return max_pages;
}

static uint8_t
refresh_tab(enum tabs tab, uint8_t current_page)
{
	uint8_t (*refresh_function[NUMBER_OF_TABS])(uint8_t current_page) = {refresh_threads_tab, refresh_pollers_tab, refresh_cores_tab};
	int color_pair[NUMBER_OF_TABS] = {COLOR_PAIR(2), COLOR_PAIR(2), COLOR_PAIR(2)};
	int i;
	uint8_t max_pages = 0;

	color_pair[tab] = COLOR_PAIR(1);

	for (i = 0; i < NUMBER_OF_TABS; i++) {
		wbkgd(g_tab_win[i], color_pair[i]);
	}

	max_pages = (*refresh_function[tab])(current_page);
	refresh();

	for (i = 0; i < NUMBER_OF_TABS; i++) {
		wrefresh(g_tab_win[i]);
	}
	draw_menu_win();

	return max_pages;
}

static void
print_in_middle(WINDOW *win, int starty, int startx, int width, char *string, chtype color)
{
	int length, temp;

	length = strlen(string);
	temp = (width - length) / 2;
	wattron(win, color);
	mvwprintw(win, starty, startx + temp, "%s", string);
	wattroff(win, color);
	refresh();
}

static void
print_left(WINDOW *win, int starty, int startx, int width, char *string, chtype color)
{
	wattron(win, color);
	mvwprintw(win, starty, startx, "%s", string);
	wattroff(win, color);
	refresh();
}

static void
apply_filters(enum tabs tab)
{
	wclear(g_tabs[tab]);
	draw_tabs(tab, g_current_sort_col[tab], g_current_sort_col2[tab]);
}

static ITEM **
draw_filtering_menu(uint8_t position, WINDOW *filter_win, uint8_t tab, MENU **my_menu)
{
	const int ADDITIONAL_ELEMENTS = 3;
	const int ROW_PADDING = 6;
	const int WINDOW_START_X = 1;
	const int WINDOW_START_Y = 3;
	const int WINDOW_COLUMNS = 2;
	struct col_desc *col_desc = g_col_desc[tab];
	ITEM **my_items;
	MENU *menu;
	int i, elements;
	uint8_t len = 0;

	for (i = 0; col_desc[i].name != NULL; ++i) {
		len = spdk_max(col_desc[i].name_len, len);
	}

	elements = i;

	my_items = (ITEM **)calloc(elements * WINDOW_COLUMNS + ADDITIONAL_ELEMENTS, sizeof(ITEM *));
	if (my_items == NULL) {
		fprintf(stderr, "Unable to allocate an item list in draw_filtering_menu.\n");
		return NULL;
	}

	for (i = 0; i < elements * 2; i++) {
		my_items[i] = new_item(col_desc[i / WINDOW_COLUMNS].name, NULL);
		i++;
		my_items[i] = new_item(col_desc[i / WINDOW_COLUMNS].disabled ? "[ ]" : "[*]", NULL);
	}

	my_items[i] = new_item("     CLOSE", NULL);
	set_item_userptr(my_items[i], apply_filters);

	menu = new_menu((ITEM **)my_items);

	menu_opts_off(menu, O_SHOWDESC);
	set_menu_format(menu, elements + 1, WINDOW_COLUMNS);

	set_menu_win(menu, filter_win);
	set_menu_sub(menu, derwin(filter_win, elements + 1, len + ROW_PADDING, WINDOW_START_Y,
				  WINDOW_START_X));

	*my_menu = menu;

	post_menu(menu);
	refresh();
	wrefresh(filter_win);

	for (i = 0; i < position / WINDOW_COLUMNS; i++) {
		menu_driver(menu, REQ_DOWN_ITEM);
	}

	return my_items;
}

static void
delete_filtering_menu(MENU *my_menu, ITEM **my_items, uint8_t elements)
{
	int i;

	unpost_menu(my_menu);
	free_menu(my_menu);
	for (i = 0; i < elements * 2 + 2; ++i) {
		free_item(my_items[i]);
	}
	free(my_items);
}

static ITEM **
refresh_filtering_menu(MENU **my_menu, WINDOW *filter_win, uint8_t tab, ITEM **my_items,
		       uint8_t elements, uint8_t position)
{
	delete_filtering_menu(*my_menu, my_items, elements);
	return draw_filtering_menu(position, filter_win, tab, my_menu);
}

static void
filter_columns(uint8_t tab)
{
	const int WINDOW_HEADER_LEN = 5;
	const int WINDOW_BORDER_LEN = 8;
	const int WINDOW_HEADER_END_LINE = 2;
	const int WINDOW_COLUMNS = 2;
	struct col_desc *col_desc = g_col_desc[tab];
	PANEL *filter_panel;
	WINDOW *filter_win;
	ITEM **my_items;
	MENU *my_menu = NULL;
	int i, c, elements;
	bool stop_loop = false;
	ITEM *cur;
	void (*p)(enum tabs tab);
	uint8_t current_index, len = 0;
	bool disabled[TABS_COL_COUNT];

	for (i = 0; col_desc[i].name != NULL; ++i) {
		len = spdk_max(col_desc[i].name_len, len);
	}

	elements = i;

	filter_win = newwin(elements + WINDOW_HEADER_LEN, len + WINDOW_BORDER_LEN,
			    (g_max_row - elements - 1) / 2, (g_max_col - len) / 2);
	assert(filter_win != NULL);
	keypad(filter_win, TRUE);
	filter_panel = new_panel(filter_win);
	assert(filter_panel != NULL);

	top_panel(filter_panel);
	update_panels();
	doupdate();

	box(filter_win, 0, 0);

	print_in_middle(filter_win, 1, 0, len + WINDOW_BORDER_LEN, "Filtering", COLOR_PAIR(3));
	mvwaddch(filter_win, WINDOW_HEADER_END_LINE, 0, ACS_LTEE);
	mvwhline(filter_win, WINDOW_HEADER_END_LINE, 1, ACS_HLINE, len + WINDOW_BORDER_LEN - 2);
	mvwaddch(filter_win, WINDOW_HEADER_END_LINE, len + WINDOW_BORDER_LEN - 1, ACS_RTEE);

	my_items = draw_filtering_menu(0, filter_win, tab, &my_menu);
	if (my_items == NULL || my_menu == NULL) {
		goto fail;
	}

	for (int i = 0; i < TABS_COL_COUNT; i++) {
		disabled[i] = col_desc[i].disabled;
	}

	while (!stop_loop) {
		c = wgetch(filter_win);

		switch (c) {
		case KEY_DOWN:
			menu_driver(my_menu, REQ_DOWN_ITEM);
			break;
		case KEY_UP:
			menu_driver(my_menu, REQ_UP_ITEM);
			break;
		case 27: /* ESC */
		case 'q':
			for (int i = 0; i < TABS_COL_COUNT; i++) {
				cur = current_item(my_menu);
				col_desc[i].disabled = disabled[i];

				my_items = refresh_filtering_menu(&my_menu, filter_win, tab, my_items, elements,
								  item_index(cur) + 1);
				if (my_items == NULL || my_menu == NULL) {
					goto fail;
				}
			}

			stop_loop = true;
			break;
		case ' ': /* Space */
			cur = current_item(my_menu);
			current_index = item_index(cur) / WINDOW_COLUMNS;
			col_desc[current_index].disabled = !col_desc[current_index].disabled;
			my_items = refresh_filtering_menu(&my_menu, filter_win, tab, my_items, elements,
							  item_index(cur) + 1);
			if (my_items == NULL || my_menu == NULL) {
				goto fail;
			}
			break;
		case 10: /* Enter */
			cur = current_item(my_menu);
			current_index = item_index(cur) / WINDOW_COLUMNS;
			if (current_index == elements) {
				stop_loop = true;
				p = item_userptr(cur);
				p(tab);
			} else {
				col_desc[current_index].disabled = !col_desc[current_index].disabled;
				my_items = refresh_filtering_menu(&my_menu, filter_win, tab, my_items, elements,
								  item_index(cur) + 1);
				if (my_items == NULL || my_menu == NULL) {
					goto fail;
				}
			}
			break;
		}
		wrefresh(filter_win);
	}

	delete_filtering_menu(my_menu, my_items, elements);

	del_panel(filter_panel);
	delwin(filter_win);

	wclear(g_menu_win);
	draw_menu_win();
	return;

fail:
	fprintf(stderr, "Unable to filter the columns due to allocation failure.\n");
	assert(false);
}

static void
sort_type(enum tabs tab, int item_index)
{
	g_current_sort_col[tab] = item_index;
}

static void
sort_type2(enum tabs tab, int item_index)
{
	g_current_sort_col2[tab] = item_index;
}

static void
change_sorting(uint8_t tab, int winnum, bool *pstop_loop)
{
	const int WINDOW_HEADER_LEN = 4;
	const int WINDOW_BORDER_LEN = 3;
	const int WINDOW_START_X = 1;
	const int WINDOW_START_Y = 4;
	const int WINDOW_HEADER_END_LINE = 3;
	const int WINDOW_MIN_WIDTH = 31;
	PANEL *sort_panel;
	WINDOW *sort_win;
	ITEM **my_items;
	MENU *my_menu;
	int i, c, elements;
	bool stop_loop = false;
	ITEM *cur;
	char *name;
	char *help;
	void (*p)(enum tabs tab, int item_index);
	uint8_t len = WINDOW_MIN_WIDTH;

	for (i = 0; g_col_desc[tab][i].name != NULL; ++i) {
		len = spdk_max(len, g_col_desc[tab][i].name_len);
	}

	elements = i;

	my_items = (ITEM **)calloc(elements + 1, sizeof(ITEM *));
	if (my_items == NULL) {
		fprintf(stderr, "Unable to allocate an item list in change_sorting.\n");
		return;
	}

	for (i = 0; i < elements; ++i) {
		my_items[i] = new_item(g_col_desc[tab][i].name, NULL);
		set_item_userptr(my_items[i], (winnum == 0) ? sort_type : sort_type2);
	}

	my_menu = new_menu((ITEM **)my_items);

	menu_opts_off(my_menu, O_SHOWDESC);

	sort_win = newwin(elements + WINDOW_HEADER_LEN + 1, len + WINDOW_BORDER_LEN,
			  (g_max_row - elements) / 2,
			  (g_max_col - len) / 2 - len + len * winnum);
	assert(sort_win != NULL);
	keypad(sort_win, TRUE);
	sort_panel = new_panel(sort_win);
	assert(sort_panel != NULL);

	top_panel(sort_panel);
	update_panels();
	doupdate();

	set_menu_win(my_menu, sort_win);
	set_menu_sub(my_menu, derwin(sort_win, elements, len + 1, WINDOW_START_Y, WINDOW_START_X));
	box(sort_win, 0, 0);

	if (winnum == 0) {
		name = "Sorting #1";
		help = "Right key for second sorting";
	} else {
		name = "Sorting #2";
		help = "Left key for first sorting";
	}

	print_in_middle(sort_win, 1, 0, len + WINDOW_BORDER_LEN, name, COLOR_PAIR(3));
	print_in_middle(sort_win, 2, 0, len + WINDOW_BORDER_LEN, help, COLOR_PAIR(3));
	mvwaddch(sort_win, WINDOW_HEADER_END_LINE, 0, ACS_LTEE);
	mvwhline(sort_win, WINDOW_HEADER_END_LINE, 1, ACS_HLINE, len + 1);
	mvwaddch(sort_win, WINDOW_HEADER_END_LINE, len + WINDOW_BORDER_LEN - 1, ACS_RTEE);

	post_menu(my_menu);
	refresh();
	wrefresh(sort_win);

	while (!stop_loop) {
		c = wgetch(sort_win);
		/*
		 * First sorting window:
		 * Up/Down - select first sorting column;
		 * Enter - apply current column;
		 * Right - open second sorting window.
		 * Second sorting window:
		 * Up/Down - select second sorting column;
		 * Enter - apply current column of both sorting windows;
		 * Left - exit second window and reset second sorting key;
		 * Right - do nothing.
		 */
		switch (c) {
		case KEY_DOWN:
			menu_driver(my_menu, REQ_DOWN_ITEM);
			break;
		case KEY_UP:
			menu_driver(my_menu, REQ_UP_ITEM);
			break;
		case KEY_RIGHT:
			if (winnum > 0) {
				break;
			}
			change_sorting(tab, winnum + 1, &stop_loop);
			/* Restore input. */
			keypad(sort_win, TRUE);
			post_menu(my_menu);
			refresh();
			wrefresh(sort_win);
			redrawwin(sort_win);
			if (winnum == 0) {
				cur = current_item(my_menu);
				p = item_userptr(cur);
				p(tab, item_index(cur));
			}
			break;
		case 27: /* ESC */
			stop_loop = true;
			break;
		case KEY_LEFT:
			if (winnum > 0) {
				sort_type2(tab, COL_THREADS_NONE);
			}
		/* FALLTHROUGH */
		case 10: /* Enter */
			stop_loop = true;
			if (winnum > 0 && c == 10) {
				*pstop_loop = true;
			}
			if (c == 10) {
				cur = current_item(my_menu);
				p = item_userptr(cur);
				p(tab, item_index(cur));
			}
			break;
		}
		wrefresh(sort_win);
	}

	if (winnum == 0) {
		wclear(g_tabs[tab]);
		draw_tabs(tab, g_current_sort_col[tab], g_current_sort_col2[tab]);
	}

	unpost_menu(my_menu);
	free_menu(my_menu);

	for (i = 0; i < elements; ++i) {
		free_item(my_items[i]);
	}

	free(my_items);

	del_panel(sort_panel);
	delwin(sort_win);

	if (winnum == 0) {
		wclear(g_menu_win);
		draw_menu_win();
	}
}

static void
change_refresh_rate(void)
{
	const int WINDOW_HEADER_END_LINE = 2;
	PANEL *refresh_panel;
	WINDOW *refresh_win;
	int c;
	bool stop_loop = false;
	uint32_t rr_tmp, refresh_rate = 0;
	char refresh_rate_str[MAX_STRING_LEN];

	refresh_win = newwin(RR_WIN_HEIGHT, RR_WIN_WIDTH, (g_max_row - RR_WIN_HEIGHT - 1) / 2,
			     (g_max_col - RR_WIN_WIDTH) / 2);
	assert(refresh_win != NULL);
	keypad(refresh_win, TRUE);
	refresh_panel = new_panel(refresh_win);
	assert(refresh_panel != NULL);

	top_panel(refresh_panel);
	update_panels();
	doupdate();

	box(refresh_win, 0, 0);

	print_in_middle(refresh_win, 1, 0, RR_WIN_WIDTH + 1, "Enter refresh rate value [s]", COLOR_PAIR(3));
	mvwaddch(refresh_win, WINDOW_HEADER_END_LINE, 0, ACS_LTEE);
	mvwhline(refresh_win, WINDOW_HEADER_END_LINE, 1, ACS_HLINE, RR_WIN_WIDTH - 2);
	mvwaddch(refresh_win, WINDOW_HEADER_END_LINE, RR_WIN_WIDTH, ACS_RTEE);
	mvwprintw(refresh_win, WINDOW_HEADER_END_LINE + 1, (RR_WIN_WIDTH - 1) / 2, "%d", refresh_rate);

	refresh();
	wrefresh(refresh_win);

	while (!stop_loop) {
		c = wgetch(refresh_win);

		switch (c) {
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			rr_tmp = refresh_rate * 10 + c - '0';

			if (rr_tmp <= RR_MAX_VALUE) {
				refresh_rate = rr_tmp;
				snprintf(refresh_rate_str, MAX_STRING_LEN - 1, "%d", refresh_rate);
				mvwprintw(refresh_win, WINDOW_HEADER_END_LINE + 1,
					  (RR_WIN_WIDTH - 1 - strlen(refresh_rate_str)) / 2, "%d", refresh_rate);
				refresh();
				wrefresh(refresh_win);
			}
			break;
		case KEY_BACKSPACE:
		case 127:
		case '\b':
			refresh_rate = refresh_rate / 10;
			snprintf(refresh_rate_str, MAX_STRING_LEN - 1, "%d", refresh_rate);
			mvwprintw(refresh_win, WINDOW_HEADER_END_LINE + 1,
				  (RR_WIN_WIDTH - 1 - strlen(refresh_rate_str) - 2) / 2, "       ");
			mvwprintw(refresh_win, WINDOW_HEADER_END_LINE + 1,
				  (RR_WIN_WIDTH - 1 - strlen(refresh_rate_str)) / 2, "%d", refresh_rate);
			refresh();
			wrefresh(refresh_win);
			break;
		case 27: /* ESC */
		case 'q':
			stop_loop = true;
			break;
		case 10: /* Enter */
			g_sleep_time = refresh_rate;
			stop_loop = true;
			break;
		}
		wrefresh(refresh_win);
	}

	del_panel(refresh_panel);
	delwin(refresh_win);
}

static void
free_poller_history(void)
{
	struct run_counter_history *history, *tmp;

	TAILQ_FOREACH_SAFE(history, &g_run_counter_history, link, tmp) {
		TAILQ_REMOVE(&g_run_counter_history, history, link);
		free(history);
	}
}

static uint64_t
get_position_for_window(uint64_t window_size, uint64_t max_size)
{
	/* This function calculates position for pop-up detail window.
	 * Since horizontal and vertical positions are calculated the same way
	 * there is no need for separate functions. */
	window_size = spdk_min(window_size, max_size);

	return (max_size - window_size) / 2;
}

static void
print_bottom_message(char *msg)
{
	uint64_t i;

	for (i = 1; i < (uint64_t)g_max_col - 1; i++) {
		mvprintw(g_max_row - 1, i, " ");
	}
	mvprintw(g_max_row - 1, g_max_col - strlen(msg) - 2, "%s", msg);
}

static void
draw_thread_win_content(WINDOW *thread_win, struct rpc_thread_info *thread_info)
{
	uint64_t current_row, i, time;
	char idle_time[MAX_TIME_STR_LEN], busy_time[MAX_TIME_STR_LEN];

	box(thread_win, 0, 0);

	print_in_middle(thread_win, 1, 0, THREAD_WIN_WIDTH, thread_info->name,
			COLOR_PAIR(3));
	mvwhline(thread_win, 2, 1, ACS_HLINE, THREAD_WIN_WIDTH - 2);
	mvwaddch(thread_win, 2, THREAD_WIN_WIDTH, ACS_RTEE);

	print_left(thread_win, 3, THREAD_WIN_FIRST_COL, THREAD_WIN_WIDTH,
		   "Core:                Idle [us]:            Busy [us]:", COLOR_PAIR(5));
	mvwprintw(thread_win, 3, THREAD_WIN_FIRST_COL + 6, "%d",
		  thread_info->core_num);

	if (g_interval_data) {
		get_time_str(thread_info->idle - thread_info->last_idle, idle_time);
		mvwprintw(thread_win, 3, THREAD_WIN_FIRST_COL + 32, "%s", idle_time);
		get_time_str(thread_info->busy - thread_info->last_busy, busy_time);
		mvwprintw(thread_win, 3, THREAD_WIN_FIRST_COL + 54, "%s", busy_time);
	} else {
		get_time_str(thread_info->idle, idle_time);
		mvwprintw(thread_win, 3, THREAD_WIN_FIRST_COL + 32, "%s", idle_time);
		get_time_str(thread_info->busy, busy_time);
		mvwprintw(thread_win, 3, THREAD_WIN_FIRST_COL + 54, "%s", busy_time);
	}

	print_left(thread_win, 4, THREAD_WIN_FIRST_COL, THREAD_WIN_WIDTH,
		   "Active pollers:      Timed pollers:        Paused pollers:", COLOR_PAIR(5));
	mvwprintw(thread_win, 4, THREAD_WIN_FIRST_COL + 17, "%" PRIu64,
		  thread_info->active_pollers_count);
	mvwprintw(thread_win, 4, THREAD_WIN_FIRST_COL + 36, "%" PRIu64,
		  thread_info->timed_pollers_count);
	mvwprintw(thread_win, 4, THREAD_WIN_FIRST_COL + 59, "%" PRIu64,
		  thread_info->paused_pollers_count);

	mvwhline(thread_win, 5, 1, ACS_HLINE, THREAD_WIN_WIDTH - 2);

	print_in_middle(thread_win, 6, 0, THREAD_WIN_WIDTH,
			"Pollers                          Type    Total run count   Period", COLOR_PAIR(5));

	mvwhline(thread_win, 7, 1, ACS_HLINE, THREAD_WIN_WIDTH - 2);

	current_row = 8;

	for (i = 0; i < g_last_pollers_count; i++) {
		if (g_pollers_info[i].thread_id == thread_info->id) {
			mvwprintw(thread_win, current_row, THREAD_WIN_FIRST_COL, "%s", g_pollers_info[i].name);
			if (g_pollers_info[i].type == SPDK_ACTIVE_POLLER) {
				mvwprintw(thread_win, current_row, THREAD_WIN_FIRST_COL + 33, "Active");
			} else if (g_pollers_info[i].type == SPDK_TIMED_POLLER) {
				mvwprintw(thread_win, current_row, THREAD_WIN_FIRST_COL + 33, "Timed");
			} else {
				mvwprintw(thread_win, current_row, THREAD_WIN_FIRST_COL + 33, "Paused");
			}
			mvwprintw(thread_win, current_row, THREAD_WIN_FIRST_COL + 41, "%" PRIu64,
				  g_pollers_info[i].run_count);
			if (g_pollers_info[i].period_ticks) {
				time = g_pollers_info[i].period_ticks * SPDK_SEC_TO_USEC / g_tick_rate;
				mvwprintw(thread_win, current_row, THREAD_WIN_FIRST_COL + 59, "%" PRIu64, time);
			}
			current_row++;
		}
	}

	refresh();
	wrefresh(thread_win);
}

static void
display_thread(uint64_t thread_id, uint8_t current_page)
{
	PANEL *thread_panel;
	WINDOW *thread_win;
	struct rpc_thread_info thread_info;
	uint64_t pollers_count, i;
	int c;
	bool stop_loop = false;

	memset(&thread_info, 0, sizeof(thread_info));
	pthread_mutex_lock(&g_thread_lock);

	/* Use local copy of thread_info */
	for (i = 0; i < g_last_threads_count; i++) {
		if (g_threads_info[i].id == thread_id) {
			memcpy(&thread_info, &g_threads_info[i], sizeof(struct rpc_thread_info));
			break;
		}
	}

	/* We did not find this thread, so we cannot show its information. */
	if (i == g_last_threads_count) {
		print_bottom_message("This thread does not exist.");
		pthread_mutex_unlock(&g_thread_lock);
		return;
	}

	pollers_count = thread_info.active_pollers_count +
			thread_info.timed_pollers_count +
			thread_info.paused_pollers_count;

	thread_win = newwin(pollers_count + THREAD_WIN_HEIGHT, THREAD_WIN_WIDTH,
			    get_position_for_window(THREAD_WIN_HEIGHT + pollers_count, g_max_row),
			    get_position_for_window(THREAD_WIN_WIDTH, g_max_col));
	keypad(thread_win, TRUE);
	thread_panel = new_panel(thread_win);

	top_panel(thread_panel);
	update_panels();
	doupdate();

	draw_thread_win_content(thread_win, &thread_info);

	pthread_mutex_unlock(&g_thread_lock);

	while (!stop_loop) {
		c = wgetch(thread_win);

		switch (c) {
		case 27: /* ESC */
			stop_loop = true;
			break;
		default:
			break;
		}
	}

	del_panel(thread_panel);
	delwin(thread_win);
}

static void
show_thread(uint8_t current_page)
{
	uint64_t thread_number = current_page * g_max_data_rows + g_selected_row;
	uint64_t thread_id;

	pthread_mutex_lock(&g_thread_lock);
	assert(thread_number < g_last_threads_count);
	thread_id = g_threads_info[thread_number].id;
	pthread_mutex_unlock(&g_thread_lock);

	display_thread(thread_id, current_page);
}

static void
show_single_thread(uint64_t thread_id, uint8_t current_page)
{
	uint64_t i;

	pthread_mutex_lock(&g_thread_lock);
	for (i = 0; i < g_last_threads_count; i++) {
		if (g_threads_info[i].id == thread_id) {
			pthread_mutex_unlock(&g_thread_lock);
			display_thread(thread_id, current_page);
			return;
		}
	}
	pthread_mutex_unlock(&g_thread_lock);
}

static void
show_core(uint8_t current_page)
{
	PANEL *core_panel;
	WINDOW *core_win;
	uint64_t core_number = current_page * g_max_data_rows + g_selected_row;
	struct rpc_core_info *core_info = &g_cores_info[core_number];
	uint64_t threads_count, i;
	uint64_t thread_id = 0;
	uint16_t current_threads_row;
	int c;
	char core_win_title[25];
	bool stop_loop = false;
	char idle_time[MAX_TIME_STR_LEN], busy_time[MAX_TIME_STR_LEN];

	pthread_mutex_lock(&g_thread_lock);
	assert(core_number < g_last_cores_count);

	threads_count = g_cores_info[core_number].threads.threads_count;

	core_win = newwin(threads_count + CORE_WIN_HEIGHT, CORE_WIN_WIDTH,
			  get_position_for_window(CORE_WIN_HEIGHT + threads_count, g_max_row),
			  get_position_for_window(CORE_WIN_WIDTH, g_max_col));

	keypad(core_win, TRUE);
	core_panel = new_panel(core_win);

	top_panel(core_panel);
	update_panels();
	doupdate();

	box(core_win, 0, 0);
	snprintf(core_win_title, sizeof(core_win_title), "Core %" PRIu32 " details",
		 core_info->lcore);
	print_in_middle(core_win, 1, 0, CORE_WIN_WIDTH, core_win_title, COLOR_PAIR(3));

	mvwaddch(core_win, -1, 0, ACS_LTEE);
	mvwhline(core_win, 2, 1, ACS_HLINE, CORE_WIN_WIDTH - 2);
	mvwaddch(core_win, 2, CORE_WIN_WIDTH, ACS_RTEE);
	print_left(core_win, 3, 1, CORE_WIN_WIDTH - (CORE_WIN_WIDTH / 3),
		   "Frequency:             Intr:", COLOR_PAIR(5));
	if (core_info->core_freq) {
		mvwprintw(core_win, 3, CORE_WIN_FIRST_COL - 3, "%" PRIu32,
			  core_info->core_freq);
	} else {
		mvwprintw(core_win, 3, CORE_WIN_FIRST_COL - 3, "%s", "N/A");
	}

	mvwprintw(core_win, 3, CORE_WIN_FIRST_COL + 15, "%s",
		  core_info->in_interrupt ? "Yes" : "No");

	mvwaddch(core_win, -1, 0, ACS_LTEE);
	mvwhline(core_win, 4, 1, ACS_HLINE, CORE_WIN_WIDTH - 2);
	mvwaddch(core_win, 4, CORE_WIN_WIDTH, ACS_RTEE);
	print_left(core_win, 5, 1, CORE_WIN_WIDTH, "Thread count:          Idle time:", COLOR_PAIR(5));

	mvwprintw(core_win, 5, CORE_WIN_FIRST_COL, "%" PRIu64,
		  core_info->threads.threads_count);

	if (g_interval_data == true) {
		get_time_str(core_info->idle - core_info->last_idle, idle_time);
		get_time_str(core_info->busy - core_info->last_busy, busy_time);
	} else {
		get_time_str(core_info->idle, idle_time);
		get_time_str(core_info->busy, busy_time);
	}
	mvwprintw(core_win, 5, CORE_WIN_FIRST_COL + 20, "%s", idle_time);
	mvwhline(core_win, 6, 1, ACS_HLINE, CORE_WIN_WIDTH - 2);

	print_left(core_win, 7, 1, CORE_WIN_WIDTH, "Poller count:          Busy time:", COLOR_PAIR(5));
	mvwprintw(core_win, 7, CORE_WIN_FIRST_COL, "%" PRIu64,
		  core_info->pollers_count);

	mvwprintw(core_win, 7, CORE_WIN_FIRST_COL + 20, "%s", busy_time);

	mvwhline(core_win, 8, 1, ACS_HLINE, CORE_WIN_WIDTH - 2);
	print_left(core_win, 9, 1, CORE_WIN_WIDTH, "Threads on this core", COLOR_PAIR(5));

	for (i = 0; i < core_info->threads.threads_count; i++) {
		mvwprintw(core_win, i + 10, 1, "%s", core_info->threads.thread[i].name);
	}
	pthread_mutex_unlock(&g_thread_lock);

	refresh();
	wrefresh(core_win);

	current_threads_row = 0;

	while (!stop_loop) {
		pthread_mutex_lock(&g_thread_lock);
		for (i = 0; i < core_info->threads.threads_count; i++) {
			if (i != current_threads_row) {
				mvwprintw(core_win, i + 10, 1, "%s", core_info->threads.thread[i].name);
			} else {
				print_left(core_win, i + 10, 1, CORE_WIN_WIDTH - 2,
					   core_info->threads.thread[i].name, COLOR_PAIR(2));
			}
		}
		pthread_mutex_unlock(&g_thread_lock);

		wrefresh(core_win);

		c = wgetch(core_win);
		switch (c) {
		case 10: /* ENTER */
			pthread_mutex_lock(&g_thread_lock);
			if (core_info->threads.threads_count > 0) {
				thread_id = core_info->threads.thread[current_threads_row].id;
			}
			pthread_mutex_unlock(&g_thread_lock);

			if (thread_id != 0) {
				show_single_thread(thread_id, current_page);
			}
			break;
		case 27: /* ESC */
			stop_loop = true;
			break;
		case KEY_UP:
			if (current_threads_row != 0) {
				current_threads_row--;
			}
			break;
		case KEY_DOWN:
			pthread_mutex_lock(&g_thread_lock);
			if (current_threads_row != core_info->threads.threads_count - 1) {
				current_threads_row++;
			}
			pthread_mutex_unlock(&g_thread_lock);
			break;
		default:
			break;
		}
	}

	del_panel(core_panel);
	delwin(core_win);
}

static void
show_poller(uint8_t current_page)
{
	PANEL *poller_panel;
	WINDOW *poller_win;
	uint64_t last_run_counter, last_busy_counter, busy_count;
	uint64_t poller_number = current_page * g_max_data_rows + g_selected_row;
	struct rpc_poller_info *poller;
	bool stop_loop = false;
	char poller_period[MAX_TIME_STR_LEN];
	int c;

	pthread_mutex_lock(&g_thread_lock);

	assert(poller_number < g_last_pollers_count);
	poller = &g_pollers_info[poller_number];

	poller_win = newwin(POLLER_WIN_HEIGHT, POLLER_WIN_WIDTH,
			    get_position_for_window(POLLER_WIN_HEIGHT, g_max_row),
			    get_position_for_window(POLLER_WIN_WIDTH, g_max_col));

	keypad(poller_win, TRUE);
	poller_panel = new_panel(poller_win);

	top_panel(poller_panel);
	update_panels();
	doupdate();

	box(poller_win, 0, 0);

	print_in_middle(poller_win, 1, 0, POLLER_WIN_WIDTH, poller->name, COLOR_PAIR(3));
	mvwhline(poller_win, 2, 1, ACS_HLINE, POLLER_WIN_WIDTH - 2);
	mvwaddch(poller_win, 2, POLLER_WIN_WIDTH, ACS_RTEE);

	print_left(poller_win, 3, 2, POLLER_WIN_WIDTH, "Type:                  On thread:", COLOR_PAIR(5));
	mvwprintw(poller_win, 3, POLLER_WIN_FIRST_COL, "%s",
		  poller_type_str[poller->type]);
	mvwprintw(poller_win, 3, POLLER_WIN_FIRST_COL + 23, "%s", poller->thread_name);

	print_left(poller_win, 4, 2, POLLER_WIN_WIDTH, "Run count:", COLOR_PAIR(5));

	last_run_counter = get_last_run_counter(poller->id, poller->thread_id);
	last_busy_counter = get_last_busy_counter(poller->id, poller->thread_id);
	if (g_interval_data) {
		mvwprintw(poller_win, 4, POLLER_WIN_FIRST_COL, "%" PRIu64, poller->run_count - last_run_counter);
	} else {
		mvwprintw(poller_win, 4, POLLER_WIN_FIRST_COL, "%" PRIu64, poller->run_count);
	}

	if (poller->period_ticks != 0) {
		print_left(poller_win, 4, 28, POLLER_WIN_WIDTH, "Period:", COLOR_PAIR(5));
		get_time_str(poller->period_ticks, poller_period);
		mvwprintw(poller_win, 4, POLLER_WIN_FIRST_COL + 23, "%s", poller_period);
	}
	mvwhline(poller_win, 5, 1, ACS_HLINE, POLLER_WIN_WIDTH - 2);

	busy_count = g_interval_data ? poller->busy_count - last_busy_counter : poller->busy_count;
	if (busy_count != 0) {
		print_left(poller_win, 6, 2, POLLER_WIN_WIDTH,  "Status:               Busy count:", COLOR_PAIR(5));

		if (g_interval_data == false && poller->busy_count == last_busy_counter) {
			print_left(poller_win, 6, POLLER_WIN_FIRST_COL, POLLER_WIN_WIDTH, "Idle", COLOR_PAIR(7));
		} else {
			print_left(poller_win, 6, POLLER_WIN_FIRST_COL, POLLER_WIN_WIDTH, "Busy", COLOR_PAIR(6));
		}

		mvwprintw(poller_win, 6, POLLER_WIN_FIRST_COL + 23, "%" PRIu64, busy_count);
	} else {
		print_in_middle(poller_win, 6, 1, POLLER_WIN_WIDTH - 7, "Status:", COLOR_PAIR(5));
		print_in_middle(poller_win, 6, 1, POLLER_WIN_WIDTH + 6, "Idle", COLOR_PAIR(7));
	}

	refresh();
	wrefresh(poller_win);

	pthread_mutex_unlock(&g_thread_lock);
	while (!stop_loop) {
		c = wgetch(poller_win);
		switch (c) {
		case 27: /* ESC */
			stop_loop = true;
			break;
		default:
			break;
		}
	}

	del_panel(poller_panel);
	delwin(poller_win);
}

static void *
data_thread_routine(void *arg)
{
	int rc;

	while (1) {
		pthread_mutex_lock(&g_thread_lock);
		if (g_quit_app) {
			pthread_mutex_unlock(&g_thread_lock);
			break;
		}
		pthread_mutex_unlock(&g_thread_lock);

		/* Get data from RPC for each object type.
		 * Start with cores since their number should not change. */
		rc = get_cores_data();
		if (rc) {
			print_bottom_message("ERROR occurred while getting cores data");
		}
		rc = get_thread_data();
		if (rc) {
			print_bottom_message("ERROR occurred while getting threads data");
		}

		rc = get_pollers_data();
		if (rc) {
			print_bottom_message("ERROR occurred while getting pollers data");
		}

		usleep(g_sleep_time * SPDK_SEC_TO_USEC);
	}

	return NULL;
}

static void
help_window_display(void)
{
	PANEL *help_panel;
	WINDOW *help_win;
	bool stop_loop = false;
	int c;
	uint64_t row = 1, col = 2, desc_second_row_col = 26, header_footer_col = 0;

	help_win = newwin(HELP_WIN_HEIGHT, HELP_WIN_WIDTH,
			  get_position_for_window(HELP_WIN_HEIGHT, g_max_row),
			  get_position_for_window(HELP_WIN_WIDTH, g_max_col));
	help_panel = new_panel(help_win);
	top_panel(help_panel);
	update_panels();
	doupdate();

	box(help_win, 0, 0);

	/* Header */
	print_in_middle(help_win, row, header_footer_col, HELP_WIN_WIDTH, "HELP", COLOR_PAIR(3));
	mvwhline(help_win, 2, 1, ACS_HLINE, HELP_WIN_WIDTH - 2);
	mvwaddch(help_win, 2, HELP_WIN_WIDTH, ACS_RTEE);
	row = 3;

	/* Content */
	print_left(help_win, row, col, HELP_WIN_WIDTH, "MENU options", COLOR_PAIR(5));
	print_left(help_win, ++row, ++col, HELP_WIN_WIDTH, "[q] Quit		- quit this application",
		   COLOR_PAIR(10));
	print_left(help_win, ++row, col,  HELP_WIN_WIDTH,
		   "[Tab] Next tab	- switch to next tab", COLOR_PAIR(10));
	print_left(help_win, ++row, col,  HELP_WIN_WIDTH,
		   "[1-3] Select tab	- switch to THREADS, POLLERS or CORES tab", COLOR_PAIR(10));
	print_left(help_win, ++row, col,  HELP_WIN_WIDTH,
		   "[PgUp] Previous page	- scroll up to previous page", COLOR_PAIR(10));
	print_left(help_win, ++row, col,  HELP_WIN_WIDTH,
		   "[PgDown] Next page	- scroll down to next page", COLOR_PAIR(10));
	print_left(help_win, ++row, col,  HELP_WIN_WIDTH,
		   "[Up] Arrow key	- go to previous data row", COLOR_PAIR(10));
	print_left(help_win, ++row, col,  HELP_WIN_WIDTH,
		   "[Down] Arrow key	- go to next data row", COLOR_PAIR(10));
	print_left(help_win, ++row, col,  HELP_WIN_WIDTH,
		   "[Right] Arrow key	- go to second sorting window", COLOR_PAIR(10));
	print_left(help_win, ++row, col,  HELP_WIN_WIDTH,
		   "[Left] Arrow key	- close second sorting window", COLOR_PAIR(10));
	print_left(help_win, ++row, col,  HELP_WIN_WIDTH,
		   "[c] Columns		- choose data columns to display", COLOR_PAIR(10));
	print_left(help_win, ++row, col,  HELP_WIN_WIDTH,
		   "[s] Sorting		- change sorting by column", COLOR_PAIR(10));
	print_left(help_win, ++row, col,  HELP_WIN_WIDTH,
		   "[r] Refresh rate	- set refresh rate <0, 255> in seconds", COLOR_PAIR(10));
	print_left(help_win, ++row, desc_second_row_col,  HELP_WIN_WIDTH, "that value in seconds",
		   COLOR_PAIR(10));
	print_left(help_win, ++row, col,  HELP_WIN_WIDTH,
		   "[Enter] Item details	- show current data row details (Enter to open, Esc to close)",
		   COLOR_PAIR(10));
	print_left(help_win, ++row, col,  HELP_WIN_WIDTH,
		   "[t] Total/Interval	- switch to display data measured from the start of SPDK", COLOR_PAIR(10));
	print_left(help_win, ++row, desc_second_row_col,  HELP_WIN_WIDTH,
		   "application or last refresh", COLOR_PAIR(10));
	print_left(help_win, ++row, col,  HELP_WIN_WIDTH, "[h] Help		- show this help window",
		   COLOR_PAIR(10));

	/* Footer */
	mvwhline(help_win, HELP_WIN_HEIGHT - 3, 1, ACS_HLINE, HELP_WIN_WIDTH - 2);
	mvwaddch(help_win, HELP_WIN_HEIGHT - 3, HELP_WIN_WIDTH, ACS_RTEE);

	print_in_middle(help_win, HELP_WIN_HEIGHT - 2, header_footer_col, HELP_WIN_WIDTH,
			"[Esc] Close this window", COLOR_PAIR(10));

	refresh();
	wrefresh(help_win);

	while (!stop_loop) {
		c = wgetch(help_win);

		switch (c) {
		case 27: /* ESC */
			stop_loop = true;
			break;
		default:
			break;
		}
	}

	del_panel(help_panel);
	delwin(help_win);

}

static void
show_stats(pthread_t *data_thread)
{
	const int CURRENT_PAGE_STR_LEN = 50;
	long int time_last, time_dif;
	struct timespec time_now;
	int c;
	int max_row, max_col;
	uint8_t active_tab = THREADS_TAB;
	uint8_t current_page = 0;
	uint8_t max_pages = 1;
	uint16_t required_size = WINDOW_HEADER + 1;
	uint64_t i;
	char current_page_str[CURRENT_PAGE_STR_LEN];
	bool force_refresh = true;

	clock_gettime(CLOCK_MONOTONIC, &time_now);
	time_last = time_now.tv_sec;

	switch_tab(THREADS_TAB);

	while (1) {
		/* Check if interface has to be resized (terminal size changed) */
		getmaxyx(stdscr, max_row, max_col);

		if (max_row != g_max_row || max_col != g_max_col) {
			if (max_row != g_max_row) {
				current_page = 0;
			}
			g_max_row = spdk_max(max_row, required_size);
			g_max_col = max_col;
			g_data_win_size = g_max_row - required_size + 1;
			g_max_data_rows = g_max_row - WINDOW_HEADER;
			resize_interface(active_tab);
		}

		clock_gettime(CLOCK_MONOTONIC, &time_now);
		time_dif = time_now.tv_sec - time_last;
		if (time_dif < 0) {
			time_dif = g_sleep_time;
		}

		if (time_dif >= g_sleep_time || force_refresh) {
			time_last = time_now.tv_sec;
			pthread_mutex_lock(&g_thread_lock);
			max_pages = refresh_tab(active_tab, current_page);
			pthread_mutex_unlock(&g_thread_lock);

			snprintf(current_page_str, CURRENT_PAGE_STR_LEN - 1, "Page: %d/%d", current_page + 1, max_pages);
			mvprintw(g_max_row - 1, 1, "%s", current_page_str);

			refresh();
		}

		c = getch();
		if (c == 'q') {
			pthread_mutex_lock(&g_thread_lock);
			g_quit_app = true;
			pthread_mutex_unlock(&g_thread_lock);
			break;
		}

		force_refresh = true;

		switch (c) {
		case '1':
		case '2':
		case '3':
			active_tab = c - '1';
			current_page = 0;
			g_selected_row = 0;
			switch_tab(active_tab);
			break;
		case '\t':
			if (active_tab < NUMBER_OF_TABS - 1) {
				active_tab++;
			} else {
				active_tab = THREADS_TAB;
			}
			g_selected_row = 0;
			current_page = 0;
			switch_tab(active_tab);
			break;
		case 's':
			sort_type2(active_tab, COL_THREADS_NONE);
			change_sorting(active_tab, 0, NULL);
			break;
		case 'c':
			filter_columns(active_tab);
			break;
		case 'r':
			change_refresh_rate();
			break;
		case 't':
			g_interval_data = !g_interval_data;
			break;
		case KEY_NPAGE: /* PgDown */
			if (current_page + 1 < max_pages) {
				current_page++;
			}
			wclear(g_tabs[active_tab]);
			g_selected_row = 0;
			draw_tabs(active_tab, g_current_sort_col[active_tab], g_current_sort_col2[active_tab]);
			break;
		case KEY_PPAGE: /* PgUp */
			if (current_page > 0) {
				current_page--;
			}
			wclear(g_tabs[active_tab]);
			g_selected_row = 0;
			draw_tabs(active_tab, g_current_sort_col[active_tab], g_current_sort_col2[active_tab]);
			break;
		case KEY_UP: /* Arrow up */
			if (g_selected_row > 0) {
				g_selected_row--;
			}
			break;
		case KEY_DOWN: /* Arrow down */
			if (g_selected_row < g_max_selected_row) {
				g_selected_row++;
			}
			break;
		case 10: /* Enter */
			if (active_tab == THREADS_TAB) {
				show_thread(current_page);
			} else if (active_tab == CORES_TAB) {
				show_core(current_page);
			} else if (active_tab == POLLERS_TAB) {
				show_poller(current_page);
			}
			break;
		case 'h':
			help_window_display();
			break;
		default:
			force_refresh = false;
			break;
		}
	}

	pthread_join(*data_thread, NULL);

	free_poller_history();

	/* Free memory holding current data states before quitting application */
	for (i = 0; i < g_last_pollers_count; i++) {
		free_rpc_poller(&g_pollers_info[i]);
	}
	for (i = 0; i < g_last_threads_count; i++) {
		free_rpc_threads_stats(&g_threads_info[i]);
	}
	free_rpc_core_info(g_cores_info, g_last_cores_count);
}

static void
draw_interface(void)
{
	int i;
	uint16_t required_size =  WINDOW_HEADER + 1;

	getmaxyx(stdscr, g_max_row, g_max_col);
	g_max_row = spdk_max(g_max_row, required_size);
	g_data_win_size = g_max_row - required_size;
	g_max_data_rows = g_max_row - WINDOW_HEADER;

	g_menu_win = newwin(MENU_WIN_HEIGHT, g_max_col, g_max_row - MENU_WIN_HEIGHT - 1,
			    MENU_WIN_LOCATION_COL);
	assert(g_menu_win != NULL);
	draw_menu_win();

	for (i = 0; i < NUMBER_OF_TABS; i++) {
		g_tab_win[i] = newwin(TAB_WIN_HEIGHT, g_max_col / NUMBER_OF_TABS - TABS_SPACING,
				      TAB_WIN_LOCATION_ROW, g_max_col / NUMBER_OF_TABS * i + 1);
		assert(g_tab_win[i] != NULL);
		draw_tab_win(i);

		g_tabs[i] = newwin(g_max_row - MENU_WIN_HEIGHT - TAB_WIN_HEIGHT - 2, g_max_col, TABS_LOCATION_ROW,
				   TABS_LOCATION_COL);
		draw_tabs(i, g_current_sort_col[i], g_current_sort_col2[i]);
		g_panels[i] = new_panel(g_tabs[i]);
		assert(g_panels[i] != NULL);
	}

	update_panels();
	doupdate();
}

static void
finish(int sig)
{
	/* End ncurses mode */
	endwin();
	spdk_jsonrpc_client_close(g_rpc_client);
	exit(0);
}

static void
setup_ncurses(void)
{
	clear();
	noecho();
	timeout(1);
	curs_set(0);
	keypad(stdscr, TRUE);
	start_color();
	init_pair(1, COLOR_BLACK, COLOR_GREEN);
	init_pair(2, COLOR_BLACK, COLOR_WHITE);
	init_pair(3, COLOR_YELLOW, COLOR_BLACK);
	init_pair(4, COLOR_BLACK, COLOR_YELLOW);
	init_pair(5, COLOR_GREEN, COLOR_BLACK);
	init_pair(6, COLOR_RED, COLOR_BLACK);
	init_pair(7, COLOR_BLUE, COLOR_BLACK);
	init_pair(8, COLOR_RED, COLOR_WHITE);
	init_pair(9, COLOR_BLUE, COLOR_WHITE);
	init_pair(10, COLOR_WHITE, COLOR_BLACK);

	if (has_colors() == FALSE) {
		endwin();
		printf("Your terminal does not support color\n");
		exit(1);
	}

	/* Handle signals to exit gracefully cleaning up ncurses */
	(void) signal(SIGINT, finish);
	(void) signal(SIGPIPE, finish);
	(void) signal(SIGABRT, finish);
}

static void
usage(const char *program_name)
{
	printf("%s [options]", program_name);
	printf("\n");
	printf("options:\n");
	printf(" -r <path>  RPC connect address (default: /var/tmp/spdk.sock)\n");
	printf(" -h         show this usage\n");
}

static int
rpc_decode_tick_rate(struct spdk_json_val *val, uint64_t *tick_rate)
{
	struct t_rate {
		uint64_t tr;
	};

	const struct spdk_json_object_decoder rpc_tick_rate_decoder[] = {
		{"tick_rate", offsetof(struct t_rate, tr), spdk_json_decode_uint64}
	};

	int rc;
	struct t_rate tmp;

	rc = spdk_json_decode_object_relaxed(val, rpc_tick_rate_decoder,
					     SPDK_COUNTOF(rpc_tick_rate_decoder), &tmp);

	*tick_rate = tmp.tr;

	return rc;
}

static int
wait_init(pthread_t *data_thread)
{
	struct spdk_jsonrpc_client_response *json_resp = NULL;
	char *uninit_log = "Waiting for SPDK target application to initialize...",
	      *uninit_error = "Unable to read SPDK application state!";
	int c, max_col, rc = 0;
	uint64_t tick_rate;

	max_col = getmaxx(stdscr);
	print_in_middle(stdscr, FIRST_DATA_ROW, 1, max_col, uninit_log, COLOR_PAIR(5));
	rc = rpc_send_req("framework_wait_init", &json_resp);
	if (rc) {
		while (1) {
			print_in_middle(stdscr, FIRST_DATA_ROW, 1, max_col, uninit_error, COLOR_PAIR(8));
			c = getch();
			if (c == 'q') {
				return -1;
			}
		}
	}

	spdk_jsonrpc_client_free_response(json_resp);

	rc = pthread_mutex_init(&g_thread_lock, NULL);
	if (rc) {
		fprintf(stderr, "mutex lock failed to initialize: %d\n", errno);
		return -1;
	}

	memset(&g_threads_info, 0, sizeof(struct rpc_thread_info) * RPC_MAX_THREADS);
	memset(&g_cores_info, 0, sizeof(struct rpc_core_info) * RPC_MAX_CORES);

	/* Decode tick rate */
	rc = rpc_send_req("framework_get_reactors", &json_resp);
	if (rc) {
		return rc;
	}

	if (rpc_decode_tick_rate(json_resp->result, &tick_rate)) {
		spdk_jsonrpc_client_free_response(json_resp);
		return -EINVAL;
	}

	spdk_jsonrpc_client_free_response(json_resp);

	g_tick_rate = tick_rate;

	/* This is to get first batch of data for display functions.
	 * Since data thread makes RPC calls that take more time than
	 * startup of display functions on main thread, without these
	 * calls both threads would be subject to a race condition. */
	rc = get_thread_data();
	if (rc) {
		return -1;
	}

	rc = get_pollers_data();
	if (rc) {
		return -1;
	}

	rc = get_cores_data();
	if (rc) {
		return -1;
	}

	rc = pthread_create(data_thread, NULL, &data_thread_routine, NULL);
	if (rc) {
		fprintf(stderr, "data thread creation failed: %d\n", errno);
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int op, rc;
	char *socket = SPDK_DEFAULT_RPC_ADDR;
	pthread_t data_thread;

	while ((op = getopt(argc, argv, "r:h")) != -1) {
		switch (op) {
		case 'r':
			socket = optarg;
			break;
		default:
			usage(argv[0]);
			return op == 'h' ? 0 : 1;
		}
	}

	g_rpc_client = spdk_jsonrpc_client_connect(socket, AF_UNIX);
	if (!g_rpc_client) {
		fprintf(stderr, "spdk_jsonrpc_client_connect() failed: %d\n", errno);
		return 1;
	}

	initscr();
	init_str_len();
	setup_ncurses();
	draw_interface();

	rc = wait_init(&data_thread);
	if (!rc) {
		show_stats(&data_thread);
	}

	finish(0);

	return (0);
}
