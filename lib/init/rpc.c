/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/init.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/rpc.h"

#define RPC_SELECT_INTERVAL	4000 /* 4ms */

static struct spdk_poller *g_rpc_poller = NULL;

struct init_rpc_server {
	struct spdk_rpc_server *server;
	char listen_addr[sizeof(((struct sockaddr_un *)0)->sun_path)];
	bool active;
	STAILQ_ENTRY(init_rpc_server) link;
};

static STAILQ_HEAD(, init_rpc_server) g_init_rpc_servers = STAILQ_HEAD_INITIALIZER(
			g_init_rpc_servers);

static int
rpc_subsystem_poll_servers(void *arg)
{
	struct init_rpc_server *init_server;

	STAILQ_FOREACH(init_server, &g_init_rpc_servers, link) {
		if (init_server->active) {
			spdk_rpc_server_accept(init_server->server);
		}
	}

	return SPDK_POLLER_BUSY;
}

static void
rpc_opts_copy(struct spdk_rpc_opts *opts, const struct spdk_rpc_opts *opts_src,
	      size_t size)
{
	assert(opts);
	assert(opts_src);

	opts->size = size;

#define SET_FIELD(field) \
	if (offsetof(struct spdk_rpc_opts, field) + sizeof(opts->field) <= size) { \
		opts->field = opts_src->field; \
	} \

	SET_FIELD(log_file);
	SET_FIELD(log_level);

	/* Do not remove this statement, you should always update this statement when you adding a new field,
	 * and do not forget to add the SET_FIELD statement for your added field. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_rpc_opts) == 24, "Incorrect size");

#undef SET_FIELD
}

static void
rpc_opts_get_default(struct spdk_rpc_opts *opts, size_t size)
{
	assert(opts);

	opts->size = size;

#define SET_FIELD(field, value) \
	if (offsetof(struct spdk_rpc_opts, field) + sizeof(opts->field) <= size) { \
		opts->field = value; \
	} \

	SET_FIELD(log_file, NULL);
	SET_FIELD(log_level, SPDK_LOG_DISABLED);

#undef SET_FIELD
}

static int
rpc_verify_opts_and_methods(const struct spdk_rpc_opts *opts)
{
	if (!spdk_rpc_verify_methods()) {
		return -EINVAL;
	}

	if (opts != NULL && opts->size == 0) {
		SPDK_ERRLOG("size in the options structure should not be zero\n");
		return -EINVAL;
	}

	return 0;
}

static void
rpc_set_spdk_log_opts(const struct spdk_rpc_opts *_opts)
{
	struct spdk_rpc_opts opts;

	rpc_opts_get_default(&opts, sizeof(opts));
	if (_opts != NULL) {
		rpc_opts_copy(&opts, _opts, _opts->size);
	} else if (!STAILQ_EMPTY(&g_init_rpc_servers)) {
		return;
	}

	spdk_jsonrpc_set_log_file(opts.log_file);
	spdk_jsonrpc_set_log_level(opts.log_level);
}

static struct init_rpc_server *
get_server_by_addr(const char *listen_addr)
{
	struct init_rpc_server *init_server;

	STAILQ_FOREACH(init_server, &g_init_rpc_servers, link) {
		if (strcmp(listen_addr, init_server->listen_addr) == 0) {
			return init_server;
		}
	}

	return NULL;
}

int
spdk_rpc_initialize(const char *listen_addr, const struct spdk_rpc_opts *opts)
{
	struct init_rpc_server *init_server;
	int rc;

	if (listen_addr == NULL) {
		/* Not treated as an error */
		return 0;
	}

	rc = rpc_verify_opts_and_methods(opts);
	if (rc) {
		return rc;
	}

	if (get_server_by_addr(listen_addr) != NULL) {
		SPDK_ERRLOG("Socket listen_addr already in use\n");
		return -EADDRINUSE;
	}

	init_server = calloc(1, sizeof(struct init_rpc_server));
	if (init_server == NULL) {
		SPDK_ERRLOG("Unable to allocate init RPC server\n");
		return -ENOMEM;
	}

	rc = snprintf(init_server->listen_addr, sizeof(init_server->listen_addr), "%s",
		      listen_addr);
	if (rc < 0) {
		SPDK_ERRLOG("Unable to copy listen address %s\n", listen_addr);
		free(init_server);
		return -EINVAL;
	}

	/* Listen on the requested address */
	init_server->server = spdk_rpc_server_listen(listen_addr);
	if (init_server->server == NULL) {
		SPDK_ERRLOG("Unable to start RPC service at %s\n", listen_addr);
		free(init_server);
		/* TODO: Eventually, treat this as an error. But it historically has not
		 * been and many tests rely on this gracefully failing. */
		return 0;
	}

	rpc_set_spdk_log_opts(opts);
	init_server->active = true;

	STAILQ_INSERT_TAIL(&g_init_rpc_servers, init_server, link);
	if (g_rpc_poller == NULL) {
		/* Register a poller to periodically check for RPCs */
		g_rpc_poller = SPDK_POLLER_REGISTER(rpc_subsystem_poll_servers, NULL, RPC_SELECT_INTERVAL);
	}

	return 0;
}

void
spdk_rpc_server_finish(const char *listen_addr)
{
	struct init_rpc_server *init_server;

	init_server = get_server_by_addr(listen_addr);
	if (!init_server) {
		SPDK_ERRLOG("No server listening on provided address: %s\n", listen_addr);
		return;
	}

	spdk_rpc_server_close(init_server->server);
	STAILQ_REMOVE(&g_init_rpc_servers, init_server, init_rpc_server, link);
	free(init_server);

	if (STAILQ_EMPTY(&g_init_rpc_servers)) {
		spdk_poller_unregister(&g_rpc_poller);
	}
}

void
spdk_rpc_finish(void)
{
	struct init_rpc_server *init_server, *tmp;

	STAILQ_FOREACH_SAFE(init_server, &g_init_rpc_servers, link, tmp) {
		spdk_rpc_server_finish(init_server->listen_addr);
	}
}

static void
set_server_active_flag(const char *listen_addr, bool is_active)
{
	struct init_rpc_server *init_server;

	init_server = get_server_by_addr(listen_addr);
	if (!init_server) {
		SPDK_ERRLOG("No server listening on provided address: %s\n", listen_addr);
		return;
	}

	init_server->active = is_active;
}

void
spdk_rpc_server_pause(const char *listen_addr)
{
	set_server_active_flag(listen_addr, false);
}

void
spdk_rpc_server_resume(const char *listen_addr)
{
	set_server_active_flag(listen_addr, true);
}
