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

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/sock.h"
#include "spdk/string.h"
#include "spdk/conf.h"
#include "spdk/likely.h"

#include "spdk_internal/event.h"
#include "spdk_internal/log.h"

#include "memcached/memcached.h"
#include "memcached/conn.h"

#include "memcached/portal_grp.h"
#include "memcached/init_grp.h"
#include "memcached/tgt_node.h"
#include "memcached/memcached_cmd.h"

struct spdk_memcached_opts *g_spdk_memcached_opts = NULL;

static spdk_memcached_init_cb g_init_cb_fn = NULL;
static void *g_init_cb_arg = NULL;

static spdk_memcached_fini_cb g_fini_cb_fn = NULL;
static void *g_fini_cb_arg;


struct spdk_thread *memcached_thd[64] = {};


struct spdk_memcached_globals g_spdk_memcached = {
	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.portal_head = TAILQ_HEAD_INITIALIZER(g_spdk_memcached.portal_head),
	.pg_head = TAILQ_HEAD_INITIALIZER(g_spdk_memcached.pg_head),
	.ig_head = TAILQ_HEAD_INITIALIZER(g_spdk_memcached.ig_head),
	.target_head = TAILQ_HEAD_INITIALIZER(g_spdk_memcached.target_head),
};

static void spdk_memcached_init_complete(int rc);

#if 1 /*  Pools */
static void
spdk_mobj_ctor(struct spdk_mempool *mp, __attribute__((unused)) void *arg,
	       void *_m, __attribute__((unused)) unsigned i)
{
	struct spdk_mobj *m = _m;

	m->mp = mp;
	m->buf = (uint8_t *)m + sizeof(struct spdk_mobj);
	m->buf = (void *)((unsigned long)((uint8_t *)m->buf + MEMCACHED_DATA_BUFFER_ALIGNMENT) &
			  ~MEMCACHED_DATA_BUFFER_MASK);
}

#define NUM_CMD_PER_CONNECTION(memcached)	(8 * (memcached->MaxQueueDepth))
#define CMD_POOL_SIZE(memcached)		(memcached->MaxConnections * NUM_CMD_PER_CONNECTION(memcached))
#define DISKDATA_POOL_SIZE(memcached)	(memcached->MaxConnections * MAX_DISKDATA_PER_CONNECTION)

static int
memcached_initialize_cmd_pool(void)
{
	struct spdk_memcached_globals *memcached = &g_spdk_memcached;
	int dout_mobj_size = SPDK_MEMCACHED_MAX_DISKDATA_LENGTH +
			     sizeof(struct spdk_mobj) + MEMCACHED_DATA_BUFFER_ALIGNMENT;

	/* create CMD pool */
	memcached->cmd_pool = spdk_mempool_create("Memcd_cmd_pool",
			      CMD_POOL_SIZE(memcached),
			      sizeof(struct spdk_memcached_cmd),
			      256, SPDK_ENV_SOCKET_ID_ANY);
	if (!memcached->cmd_pool) {
		SPDK_ERRLOG("create cmd pool failed\n");
		return -1;
	}

	memcached->diskdata_pool = spdk_mempool_create_ctor("Memcd_diskdata_pool",
				   DISKDATA_POOL_SIZE(memcached),
				   dout_mobj_size, 256,
				   spdk_env_get_socket_id(spdk_env_get_current_core()),
				   spdk_mobj_ctor, NULL);
	if (!memcached->diskdata_pool) {
		SPDK_ERRLOG("create cmd diskdata pool failed -- pool size(%d), mobj size(%d)\n",
			    DISKDATA_POOL_SIZE(memcached), dout_mobj_size);
		return -1;
	}

	return 0;
}


static int
memcached_initialize_all_pools(void)
{
	if (memcached_initialize_cmd_pool() != 0) {
		return -1;
	}

	return 0;
}

static void
memcached_check_pool(struct spdk_mempool *pool, size_t count)
{
	if (pool && spdk_mempool_count(pool) != count) {
		SPDK_ERRLOG("spdk_mempool_count(%s) == %zu, should be %zu\n",
			    spdk_mempool_get_name(pool), spdk_mempool_count(pool), count);
	}
}


static void
memcached_check_pools(void)
{
	struct spdk_memcached_globals *memcached = &g_spdk_memcached;

	memcached_check_pool(memcached->cmd_pool, CMD_POOL_SIZE(memcached));
}

static void
memcached_free_pools(void)
{
	struct spdk_memcached_globals *memcached = &g_spdk_memcached;

	spdk_mempool_free(memcached->cmd_pool);
}
#endif


#if 1 /* memcached poll group functions */
static int
memcached_poll_group_poll(void *ctx)
{
	struct spdk_memcached_poll_group *group = ctx;
	struct spdk_memcached_conn *conn, *tmp;
	int rc;

	if (spdk_unlikely(STAILQ_EMPTY(&group->connections))) {
		return 0;
	}

	rc = spdk_sock_group_poll(group->sock_group);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to poll sock_group=%p\n", group->sock_group);
	}

	STAILQ_FOREACH_SAFE(conn, &group->connections, link, tmp) {
		if (conn->state == MEMCACHED_CONN_STATE_EXITING) {
			spdk_memcached_conn_destruct(conn);
		}
	}

	return -1;
}

static int
memcached_poll_group_handle_nop(void *ctx)
{
	struct spdk_memcached_poll_group *group = ctx;
	struct spdk_memcached_conn *conn, *tmp;

	STAILQ_FOREACH_SAFE(conn, &group->connections, link, tmp) {
		spdk_memcached_conn_handle_nop(conn);
	}

	return -1;
}

static void
memcached_create_poll_group(void *ctx)
{
	struct spdk_memcached_poll_group *pg;

	assert(g_spdk_memcached.poll_group != NULL);
	pg = &g_spdk_memcached.poll_group[spdk_env_get_current_core()];
	pg->core = spdk_env_get_current_core();

	STAILQ_INIT(&pg->connections);
	pg->sock_group = spdk_sock_group_create();
	assert(pg->sock_group != NULL);

	pg->poller = spdk_poller_register(memcached_poll_group_poll, pg, 0);

	/* set the period to 1 sec */
	pg->nop_poller = spdk_poller_register(memcached_poll_group_handle_nop, pg, 1000000);
}

static void
memcached_unregister_poll_group(void *ctx)
{
	struct spdk_memcached_poll_group *pg;

	assert(g_spdk_memcached.poll_group != NULL);
	pg = &g_spdk_memcached.poll_group[spdk_env_get_current_core()];
	assert(pg->poller != NULL);
	assert(pg->sock_group != NULL);

	spdk_sock_group_close(&pg->sock_group);
	spdk_poller_unregister(&pg->poller);
	spdk_poller_unregister(&pg->nop_poller);
}

static void
memcached_initialize_poll_group(spdk_msg_fn cpl)
{
	size_t num_poll_groups = spdk_env_get_last_core() + 1;

	g_spdk_memcached.poll_group = calloc(num_poll_groups, sizeof(struct spdk_memcached_poll_group));
	if (!g_spdk_memcached.poll_group) {
		SPDK_ERRLOG("Failed to allocated memcached poll group\n");
		spdk_memcached_init_complete(-1);
		return;
	}

	/* Send a message to each thread and create a poll group */
	spdk_for_each_thread(memcached_create_poll_group, NULL, cpl);
}
#endif

static void
memcached_parse_configuration(void *ctx)
{
	int rc;

	rc = spdk_memcached_parse_portal_grps();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_memcached_parse_portal_grps() failed\n");
		goto end;
	}

	/* init_grps are not necessary yet. */
	rc = spdk_memcached_parse_init_grps();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_memcached_parse_init_grps() failed\n");
		goto end;
	}

	rc = spdk_memcached_parse_tgt_nodes();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_memcached_parse_tgt_nodes() failed\n");
	}

end:
	spdk_memcached_init_complete(rc);
}

#if 1 /* spdk_memcached_init start */
/* memcached_initialize_global_params start */
static void
memcached_opts_init(struct spdk_memcached_opts *opts)
{
	opts->MaxConnections = MAX_MEMCACHED_CONNECTIONS;
	opts->MaxQueueDepth = DEFAULT_MAX_QUEUE_DEPTH;
	opts->timeout = DEFAULT_TIMEOUT;
	opts->min_connections_per_core = DEFAULT_CONNECTIONS_PER_LCORE;
}

static struct spdk_memcached_opts *
memcached_opts_alloc(void)
{
	struct spdk_memcached_opts *opts;

	opts = calloc(1, sizeof(*opts));
	if (!opts) {
		SPDK_ERRLOG("calloc() failed for memcached options\n");
		return NULL;
	}

	memcached_opts_init(opts);

	return opts;
}

static void
memcached_opts_free(struct spdk_memcached_opts *opts)
{
	free(opts);
}

static int
memcached_read_config_file_params(struct spdk_conf_section *sp,
				  struct spdk_memcached_opts *opts)
{
	return 0;
}

static int
memcached_parse_options(struct spdk_memcached_opts **popts)
{
	struct spdk_memcached_opts *opts;
	struct spdk_conf_section *sp;
	int rc;

	opts = memcached_opts_alloc();
	if (!opts) {
		SPDK_ERRLOG("spdk_memcached_opts_alloc_failed() failed\n");
		return -ENOMEM;
	}

	/* Process parameters */
	SPDK_DEBUGLOG(SPDK_LOG_MEMCACHED, "spdk_memcached_read_config_file_parmas\n");
	sp = spdk_conf_find_section(NULL, "memcached");
	if (sp != NULL) {
		rc = memcached_read_config_file_params(sp, opts);
		if (rc != 0) {
			free(opts);
			SPDK_ERRLOG("spdk_memcached_read_config_file_params() failed\n");
			return rc;
		}
	}

	*popts = opts;

	return 0;
}

static int
memcached_set_global_params(struct spdk_memcached_opts *opts)
{

	/*
	 * For now, just support same number of total connections, rather
	 *  than MaxSessions * MaxConnectionsPerSession.  After we add better
	 *  handling for low resource conditions from our various buffer
	 *  pools, we can bump this up to support more connections.
	 */
	g_spdk_memcached.MaxConnections = opts->MaxConnections;
	g_spdk_memcached.MaxQueueDepth = opts->MaxQueueDepth;
	g_spdk_memcached.timeout = opts->timeout;
	spdk_memcached_conn_set_min_per_core(opts->min_connections_per_core);

	return 0;
}

static int
memcached_initialize_global_params(void)
{
	int rc;

	if (!g_spdk_memcached_opts) {
		rc = memcached_parse_options(&g_spdk_memcached_opts);
		if (rc != 0) {
			SPDK_ERRLOG("memcached_parse_options() failed\n");
			return rc;
		}
	}

	rc = memcached_set_global_params(g_spdk_memcached_opts);
	if (rc != 0) {
		SPDK_ERRLOG("memcached_set_global_params() failed\n");
	}

	memcached_opts_free(g_spdk_memcached_opts);
	g_spdk_memcached_opts = NULL;

	return rc;
}
#endif /* memcached_initialize_global_params end */

static int
memcached_parse_globals(void)
{
	int rc;

	rc = memcached_initialize_global_params();
	if (rc != 0) {
		SPDK_ERRLOG("spdk_memcached_initialize_memcached_global_params() failed\n");
		assert(false);
		return -1;
	}

	rc = memcached_initialize_all_pools();
	if (rc != 0) {
		SPDK_ERRLOG("spdk_initialize_all_pools() failed\n");
		return -1;
	}

	rc = spdk_memcached_initialze_conns();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_memcached_initialze_conns() failed\n");
		return rc;
	}

	memcached_initialize_poll_group(memcached_parse_configuration);
	return 0;
}

static void
spdk_memcached_init_complete(int rc)
{
	spdk_memcached_init_cb cb_fn = g_init_cb_fn;
	void *cb_arg = g_init_cb_arg;

	SPDK_NOTICELOG("SPDK memcached service is initialized\n");

	g_init_cb_fn = NULL;
	g_init_cb_arg = NULL;

	cb_fn(cb_arg, rc);
}

void
spdk_memcached_init(spdk_memcached_init_cb cb_fn, void *cb_arg)
{
	int rc;

	assert(cb_fn != NULL);
	g_init_cb_fn = cb_fn;
	g_init_cb_arg = cb_arg;

	rc = memcached_parse_globals();
	if (rc < 0) {
		SPDK_ERRLOG("spdk_memcached_parse_globals() failed\n");
		spdk_memcached_init_complete(-1);
	}

	/*
	 * spdk_memcached_parse_configuration() will be called as the callback to
	 * spdk_initialize_memcached_poll_group() and will complete memcached
	 * subsystem initialization.
	 */
}

#if 1 /* spdk_memcached_fini start */
static void
spdk_memcached_fini_done(void *arg)
{
	memcached_check_pools();
	memcached_free_pools();

	spdk_memcached_shutdown_tgt_nodes();
	spdk_memcached_init_grps_destroy();
	spdk_memcached_portal_grps_destroy();
	free(g_spdk_memcached.poll_group);

	pthread_mutex_destroy(&g_spdk_memcached.mutex);
	g_fini_cb_fn(g_fini_cb_arg);
}

void
spdk_shutdown_memcached_conns_done(void)
{
	if (g_spdk_memcached.poll_group) {
		spdk_for_each_thread(memcached_unregister_poll_group, NULL, spdk_memcached_fini_done);
	} else {
		spdk_memcached_fini_done(NULL);
	}
}

void
spdk_memcached_fini(spdk_memcached_fini_cb cb_fn, void *cb_arg)
{
	g_fini_cb_fn = cb_fn;
	g_fini_cb_arg = cb_arg;

	spdk_memcached_portal_grp_close_all();
	spdk_shutdown_memcached_conns();

	/*
	 * spdk_shutdown_memcached_conns_done() will be called as the callback to
	 * spdk_shutdown_memcached_conns() and will complete memcached
	 * subsystem shutdown.
	 */
}
#endif

void
spdk_memcached_write_config_json(struct spdk_json_write_ctx *w)
{
	return;
}


SPDK_LOG_REGISTER_COMPONENT("memcached", SPDK_LOG_MEMCACHED)
