/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_internal/cunit.h"

#include "init/rpc.c"
#include "common/lib/test_env.c"

DEFINE_STUB(spdk_rpc_listen, int, (const char *listen_addr), 0);
DEFINE_STUB(spdk_rpc_server_listen, struct spdk_rpc_server *, (const char *listen_addr),
	    (struct spdk_rpc_server *)0xdeadbeef);
DEFINE_STUB(spdk_rpc_verify_methods, bool, (void), true);
DEFINE_STUB_V(spdk_rpc_accept, (void));
DEFINE_STUB_V(spdk_rpc_close, (void));
DEFINE_STUB_V(spdk_rpc_server_accept, (struct spdk_rpc_server *server));
DEFINE_STUB_V(spdk_rpc_server_close, (struct spdk_rpc_server *server));
DEFINE_STUB_V(spdk_rpc_set_state, (uint32_t state));

enum spdk_log_level g_test_log_level = SPDK_LOG_DISABLED;
FILE *g_test_log_file = NULL;
uint8_t g_test_log_level_set_count = 0;
uint8_t g_test_log_file_set_count = 0;

const char *g_test_addr1 = "/var/tmp/test_addr1.sock";
const char *g_test_addr2 = "/var/tmp/test_addr2.sock";

void
spdk_jsonrpc_set_log_level(enum spdk_log_level level)
{
	g_test_log_level = level;
	g_test_log_level_set_count++;
}

void
spdk_jsonrpc_set_log_file(FILE *file)
{
	g_test_log_file = file;
	g_test_log_file_set_count++;
}

static void
reset_global_counters(void)
{
	g_test_log_level_set_count = 0;
	g_test_log_file_set_count = 0;
}

static bool
server_exists(const char *addr)
{
	struct init_rpc_server *server;

	STAILQ_FOREACH(server, &g_init_rpc_servers, link) {
		if (strcmp(addr, server->listen_addr) == 0) {
			return true;
		}
	}

	return false;
}

static bool
server_paused(const char *addr)
{
	struct init_rpc_server *server;

	STAILQ_FOREACH(server, &g_init_rpc_servers, link) {
		if (strcmp(addr, server->listen_addr) == 0 && !server->active) {
			return true;
		}
	}

	return false;
}

static void
initialize_servers(void)
{
	int rc;

	CU_ASSERT(STAILQ_EMPTY(&g_init_rpc_servers));

	rc = spdk_rpc_initialize(g_test_addr1, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(server_exists(g_test_addr1));
	CU_ASSERT(server_paused(g_test_addr1) == false);

	rc = spdk_rpc_initialize(g_test_addr2, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(server_exists(g_test_addr2));
	CU_ASSERT(server_paused(g_test_addr2) == false);
}

static void
test_run_multiple_servers_stop_all(void)
{
	initialize_servers();
	CU_ASSERT(!STAILQ_EMPTY(&g_init_rpc_servers));

	spdk_rpc_finish();
	CU_ASSERT(STAILQ_EMPTY(&g_init_rpc_servers));
}

static void
test_run_multiple_servers_stop_singles(void)
{
	initialize_servers();
	CU_ASSERT(!STAILQ_EMPTY(&g_init_rpc_servers));

	spdk_rpc_server_finish(g_test_addr1);
	CU_ASSERT(!server_exists(g_test_addr1));
	CU_ASSERT(!STAILQ_EMPTY(&g_init_rpc_servers));

	spdk_rpc_server_finish(g_test_addr2);
	CU_ASSERT(!server_exists(g_test_addr2));
	CU_ASSERT(STAILQ_EMPTY(&g_init_rpc_servers));
}

static void
test_rpc_set_spdk_log_opts(void)
{
	struct spdk_rpc_opts server1_opts = {};
	struct spdk_rpc_opts server2_opts = {};
	FILE *test_log_file1 = (void *)0xDEADDEAD;
	FILE *test_log_file2 = (void *)0xBEEFBEEF;

	reset_global_counters();

	server1_opts.log_file = test_log_file1;
	server1_opts.log_level = SPDK_LOG_DEBUG;
	server1_opts.size = sizeof(server1_opts);
	server2_opts.log_file = test_log_file2;
	server2_opts.log_level = SPDK_LOG_ERROR;
	server2_opts.size = sizeof(server2_opts);

	spdk_rpc_initialize(g_test_addr1, &server1_opts);
	CU_ASSERT(g_test_log_file == server1_opts.log_file);
	CU_ASSERT(g_test_log_level == server1_opts.log_level);
	CU_ASSERT(g_test_log_file_set_count == 1);
	CU_ASSERT(g_test_log_level_set_count == 1);

	spdk_rpc_initialize(g_test_addr2, &server2_opts);
	CU_ASSERT(g_test_log_file == server2_opts.log_file);
	CU_ASSERT(g_test_log_level == server2_opts.log_level);
	CU_ASSERT(g_test_log_file_set_count == 2);
	CU_ASSERT(g_test_log_level_set_count == 2);

	spdk_rpc_finish();
}

static void
test_rpc_set_spdk_log_default_opts(void)
{
	FILE *test_log_file_default = NULL;
	enum spdk_log_level test_log_level_default = SPDK_LOG_DISABLED;

	reset_global_counters();

	spdk_rpc_initialize(g_test_addr1, NULL);
	CU_ASSERT(g_test_log_file == test_log_file_default);
	CU_ASSERT(g_test_log_level == test_log_level_default);
	CU_ASSERT(g_test_log_file_set_count == 1);
	CU_ASSERT(g_test_log_level_set_count == 1);

	spdk_rpc_initialize(g_test_addr2, NULL);
	CU_ASSERT(g_test_log_file == test_log_file_default);
	CU_ASSERT(g_test_log_level == test_log_level_default);
	CU_ASSERT(g_test_log_file_set_count == 1);
	CU_ASSERT(g_test_log_level_set_count == 1);

	spdk_rpc_finish();
}

static void
test_pause_resume_servers(void)
{
	initialize_servers();

	spdk_rpc_server_pause(g_test_addr1);
	CU_ASSERT(server_exists(g_test_addr1));
	CU_ASSERT(server_paused(g_test_addr1));

	spdk_rpc_server_pause(g_test_addr2);
	CU_ASSERT(server_exists(g_test_addr2));
	CU_ASSERT(server_paused(g_test_addr2));

	spdk_rpc_server_resume(g_test_addr2);
	CU_ASSERT(!server_paused(g_test_addr2));

	spdk_rpc_server_resume(g_test_addr1);
	CU_ASSERT(!server_paused(g_test_addr1));

	spdk_rpc_finish();
}

static void
test_remove_paused_servers(void)
{
	initialize_servers();

	spdk_rpc_server_pause(g_test_addr1);
	spdk_rpc_server_pause(g_test_addr2);

	spdk_rpc_server_finish(g_test_addr2);
	CU_ASSERT(!server_exists(g_test_addr2));

	CU_ASSERT(server_exists(g_test_addr1));
	CU_ASSERT(server_paused(g_test_addr1));

	spdk_rpc_server_finish(g_test_addr1);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;
	struct spdk_thread *thread;

	CU_initialize_registry();
	suite = CU_add_suite("rpc_suite", NULL, NULL);

	spdk_thread_lib_init(NULL, 0);
	thread = spdk_thread_create(NULL, NULL);
	spdk_set_thread(thread);

	CU_ADD_TEST(suite, test_run_multiple_servers_stop_all);
	CU_ADD_TEST(suite, test_run_multiple_servers_stop_singles);
	CU_ADD_TEST(suite, test_rpc_set_spdk_log_opts);
	CU_ADD_TEST(suite, test_rpc_set_spdk_log_default_opts);
	CU_ADD_TEST(suite, test_pause_resume_servers);
	CU_ADD_TEST(suite, test_remove_paused_servers);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	return num_failures;
}
