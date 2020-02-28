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

#include <ncurses.h>
#include <panel.h>


#define RPC_MAX_THREADS 1024
#define RPC_MAX_POLLERS 1024

#define MAX_STRING_LEN 12289 /* 3x 4k monitors + 1 */
#define TAB_WIN_HEIGHT 3
#define TAB_WIN_LOCATION_ROW 1
#define TABS_SPACING 2
#define TABS_LOCATION_ROW 4
#define TABS_LOCATION_COL 0
#define TABS_DATA_START_ROW 3
#define TABS_DATA_START_COL 3
#define MENU_WIN_HEIGHT 3
#define MENU_WIN_SPACING 4
#define MENU_WIN_LOCATION_COL 0

enum tabs {
	THREADS_TAB,
	POLLERS_TAB,
	CORES_TAB,
	NUMBER_OF_TABS,
};

const char *g_tab_title[NUMBER_OF_TABS] = {"[1] THREADS", "[2] POLLERS", "[3] CORES"};
struct spdk_jsonrpc_client *g_rpc_client;
WINDOW *g_menu_win, *g_tab_win[NUMBER_OF_TABS], *g_tabs[NUMBER_OF_TABS];
PANEL *g_tab_panels[NUMBER_OF_TABS];
uint16_t g_max_row, g_max_col;
uint16_t g_data_win_size;
uint32_t g_last_threads_count, g_last_pollers_count, g_last_cores_count;

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
		return -1;
	}

	json_resp = spdk_jsonrpc_client_get_response(g_rpc_client);
	if (json_resp == NULL) {
		return -1;
	}

	/* Check for error response */
	if (json_resp->error != NULL) {
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
print_max_len(WINDOW *win, uint16_t row, uint16_t col, const char *string)
{
	int len, max_col;
	int max_row __attribute__((unused));

	len = strlen(string);
	getmaxyx(win, max_row, max_col);

	assert(row < max_row);

	/* Check if provided string position limit + "..." exceeds screen width */
	if (col + 3 > max_col) {
		col = max_col - 4;
	}

	if (col + len > max_col - 1) {
		char tmp_str[MAX_STRING_LEN];

		snprintf(tmp_str, max_col - col - 3, "%s", string);
		snprintf(&tmp_str[max_col - col - 4], 4, "...");
		mvwprintw(win, row, col, tmp_str);
	} else {
		mvwprintw(win, row, col, string);
	}
	refresh();
	wrefresh(win);
}

static void
draw_menu_win(void)
{
	wbkgd(g_menu_win, COLOR_PAIR(2));
	box(g_menu_win, 0, 0);
	print_max_len(g_menu_win, 1, 1,
		      "   [q] Quit   |   [1-3] TAB selection   |   [PgUp] Previous page   |   [PgDown] Next page   |   [f] Filters   |   [s] Sorting");
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
	print_max_len(g_tab_win[tab], 1, col, g_tab_title[tab]);
}

static void
draw_tabs(enum tabs tab)
{
	static const char *desc[NUMBER_OF_TABS] = {"   Thread name   |   Active pollers   |   Timed pollers   |   Paused pollers   ",
						   "   Poller name   |   Type   |   On thread   ",
						   "   Core mask   "
						  };

	print_max_len(g_tabs[tab], 1, 1, desc[tab]);
	print_max_len(g_tabs[tab], 2, 1, ""); /* Move to next line */
	whline(g_tabs[tab], ACS_HLINE, MAX_STRING_LEN);
	box(g_tabs[tab], 0, 0);
	wrefresh(g_tabs[tab]);
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
		draw_tabs(i);
	}

	draw_tabs(tab);

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
	top_panel(g_tab_panels[tab]);
	update_panels();
	doupdate();
}

static void
refresh_threads_tab(void)
{
	uint64_t i;
	uint16_t j;

	/* Clear screen if number of threads changed */
	if (g_last_threads_count != g_threads_stats.threads.threads_count) {
		for (i = TABS_DATA_START_ROW; i < g_data_win_size; i++) {
			for (j = 1; j < g_max_col - 1; j++) {
				mvwprintw(g_tabs[THREADS_TAB], i, j, " ");
			}
		}

		g_last_threads_count = g_threads_stats.threads.threads_count;
	}

	for (i = 0; i < g_threads_stats.threads.threads_count; i++) {
		print_max_len(g_tabs[THREADS_TAB], TABS_DATA_START_ROW + i, TABS_DATA_START_COL,
			      g_threads_stats.threads.thread_info[i].name);
	}
}

static void
print_pollers(struct rpc_pollers *pollers, uint64_t pollers_count, uint16_t *current_line)
{
	uint64_t i;

	for (i = 0; i < pollers_count; i++) {
		print_max_len(g_tabs[POLLERS_TAB], (*current_line)++, TABS_DATA_START_COL,
			      pollers->pollers[i].name);
	}
}

static void
refresh_pollers_tab(void)
{
	struct rpc_poller_thread_info *thread;
	uint64_t i, pollers_count = 0;
	uint16_t j, current_line = TABS_DATA_START_ROW;

	for (i = 0; i < g_pollers_stats.pollers_threads.threads_count; i++) {
		thread = &g_pollers_stats.pollers_threads.threads[i];
		pollers_count += thread->active_pollers.pollers_count + thread->timed_pollers.pollers_count +
				 thread->paused_pollers.pollers_count;
	}

	/* Clear screen if number of pollers changed */
	if (g_last_pollers_count != pollers_count) {
		for (i = TABS_DATA_START_ROW; i < g_data_win_size; i++) {
			for (j = 1; j < g_max_col - 1; j++) {
				mvwprintw(g_tabs[POLLERS_TAB], i, j, " ");
			}
		}

		g_last_pollers_count = pollers_count;
	}

	for (i = 0; i < g_pollers_stats.pollers_threads.threads_count; i++) {
		thread = &g_pollers_stats.pollers_threads.threads[i];

		print_pollers(&thread->active_pollers, thread->active_pollers.pollers_count, &current_line);
		print_pollers(&thread->timed_pollers, thread->timed_pollers.pollers_count, &current_line);
		print_pollers(&thread->paused_pollers, thread->paused_pollers.pollers_count, &current_line);
	}
}

static void
refresh_cores_tab(void)
{

}

static void
refresh_tab(enum tabs tab)
{
	void (*refresh_function[NUMBER_OF_TABS])(void) = {refresh_threads_tab, refresh_pollers_tab, refresh_cores_tab};
	int color_pair[NUMBER_OF_TABS] = {COLOR_PAIR(2), COLOR_PAIR(2), COLOR_PAIR(2)};
	int i;

	color_pair[tab] = COLOR_PAIR(1);

	for (i = 0; i < NUMBER_OF_TABS; i++) {
		wbkgd(g_tab_win[i], color_pair[i]);
	}

	(*refresh_function[tab])();
	refresh();

	for (i = 0; i < NUMBER_OF_TABS; i++) {
		wrefresh(g_tab_win[i]);
	}
}

static void
show_stats(void)
{
	const char *refresh_error = "ERROR occurred while getting data";
	int c, rc;
	int max_row, max_col;
	uint8_t active_tab = THREADS_TAB;

	switch_tab(THREADS_TAB);

	while (1) {
		/* Check if interface has to be resized (terminal size changed) */
		getmaxyx(stdscr, max_row, max_col);

		if (max_row != g_max_row || max_col != g_max_col) {
			g_max_row = max_row;
			g_max_col = max_col;
			g_data_win_size = g_max_row - MENU_WIN_HEIGHT - TAB_WIN_HEIGHT - TABS_DATA_START_ROW;
			resize_interface(active_tab);
		}

		c = getch();
		if (c == 'q') {
			break;
		}

		switch (c) {
		case '1':
		case '2':
		case '3':
			active_tab = c - '1';
			switch_tab(active_tab);
			break;
		default:
			break;
		}

		rc = get_data();
		if (rc) {
			mvprintw(g_max_row - 1, g_max_col - strlen(refresh_error) - 2, refresh_error);
		}

		refresh_tab(active_tab);

		free_data();

		refresh();
	}
}

static void
draw_interface(void)
{
	int i;

	getmaxyx(stdscr, g_max_row, g_max_col);
	g_data_win_size = g_max_row - MENU_WIN_HEIGHT - TAB_WIN_HEIGHT - TABS_DATA_START_ROW;

	g_menu_win = newwin(MENU_WIN_HEIGHT, g_max_col, g_max_row - MENU_WIN_HEIGHT - 1,
			    MENU_WIN_LOCATION_COL);
	draw_menu_win();

	for (i = 0; i < NUMBER_OF_TABS; i++) {
		g_tab_win[i] = newwin(TAB_WIN_HEIGHT, g_max_col / NUMBER_OF_TABS - TABS_SPACING,
				      TAB_WIN_LOCATION_ROW, g_max_col / NUMBER_OF_TABS * i + 1);
		draw_tab_win(i);

		g_tabs[i] = newwin(g_max_row - MENU_WIN_HEIGHT - TAB_WIN_HEIGHT - 2, g_max_col, TABS_LOCATION_ROW,
				   TABS_LOCATION_COL);
		draw_tabs(i);
		g_tab_panels[i] = new_panel(g_tabs[i]);
	}

	update_panels();
	doupdate();
}

static void
setup_ncurses(void)
{
	clear();
	noecho();
	halfdelay(1);
	curs_set(0);
	start_color();
	init_pair(1, COLOR_BLACK, COLOR_GREEN);
	init_pair(2, COLOR_BLACK, COLOR_WHITE);


	if (has_colors() == FALSE) {
		endwin();
		printf("Your terminal does not support color\n");
		exit(1);
	}
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
	int op;
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

	initscr();
	setup_ncurses();
	draw_interface();
	show_stats();

	/* End curses mode */
	endwin();

	spdk_jsonrpc_client_close(g_rpc_client);

	return (0);
}
