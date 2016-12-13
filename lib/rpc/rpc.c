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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "spdk/queue.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/conf.h"
#include "spdk/log.h"

#define RPC_SELECT_INTERVAL	4000 /* 4ms */
#define RPC_DEFAULT_LISTEN_ADDR	"127.0.0.1"

static struct sockaddr_un g_rpc_listen_addr_unix = {};

static struct spdk_poller *g_rpc_poller = NULL;

static struct spdk_jsonrpc_server *g_jsonrpc_server = NULL;

struct spdk_rpc_method {
	const char *name;
	spdk_rpc_method_handler func;
	SLIST_ENTRY(spdk_rpc_method) slist;
};

static SLIST_HEAD(, spdk_rpc_method) g_rpc_methods = SLIST_HEAD_INITIALIZER(g_rpc_methods);

static void
spdk_rpc_server_do_work(void *arg)
{
	spdk_jsonrpc_server_poll(g_jsonrpc_server);
}

static int
enable_rpc(void)
{
	struct spdk_conf_section	*sp;
	char				*val;

	sp = spdk_conf_find_section(NULL, "Rpc");
	if (sp == NULL) {
		return 0;
	}

	val = spdk_conf_section_get_val(sp, "Enable");
	if (val == NULL) {
		return 0;
	}

	if (!strcmp(val, "Yes")) {
		return 1;
	}

	return 0;
}

static const char *
rpc_get_listen_addr(void)
{
	struct spdk_conf_section *sp;
	const char *val;

	sp = spdk_conf_find_section(NULL, "Rpc");
	if (sp == NULL) {
		return 0;
	}

	val = spdk_conf_section_get_val(sp, "Listen");
	if (val == NULL) {
		val = RPC_DEFAULT_LISTEN_ADDR;
	}

	return val;
}

void
spdk_rpc_register_method(const char *method, spdk_rpc_method_handler func)
{
	struct spdk_rpc_method *m;

	m = calloc(1, sizeof(struct spdk_rpc_method));
	assert(m != NULL);

	m->name = strdup(method);
	assert(m->name != NULL);

	m->func = func;

	/* TODO: use a hash table or sorted list */
	SLIST_INSERT_HEAD(&g_rpc_methods, m, slist);
}

static void
spdk_jsonrpc_handler(
	struct spdk_jsonrpc_server_conn *conn,
	const struct spdk_json_val *method,
	const struct spdk_json_val *params,
	const struct spdk_json_val *id)
{
	struct spdk_rpc_method *m;

	assert(method != NULL);

	SLIST_FOREACH(m, &g_rpc_methods, slist) {
		if (spdk_json_strequal(method, m->name)) {
			m->func(conn, params, id);
			return;
		}
	}

	spdk_jsonrpc_send_error_response(conn, id, SPDK_JSONRPC_ERROR_METHOD_NOT_FOUND, "Method not found");
}

static void
spdk_rpc_setup(void *arg)
{
	uint16_t		port;
	char			port_str[16];
	struct addrinfo		hints;
	struct addrinfo		*res;
	const char		*listen_addr;

	memset(&g_rpc_listen_addr_unix, 0, sizeof(g_rpc_listen_addr_unix));

	/* Unregister the one-shot setup poller */
	spdk_poller_unregister(&g_rpc_poller, NULL);

	if (!enable_rpc()) {
		return;
	}

	listen_addr = rpc_get_listen_addr();
	if (!listen_addr) {
		return;
	}

	if (listen_addr[0] == '/') {
		int rc;

		g_rpc_listen_addr_unix.sun_family = AF_UNIX;
		rc = snprintf(g_rpc_listen_addr_unix.sun_path,
			      sizeof(g_rpc_listen_addr_unix.sun_path),
			      "%s.%d", listen_addr, spdk_app_get_instance_id());
		if (rc < 0 || (size_t)rc >= sizeof(g_rpc_listen_addr_unix.sun_path)) {
			SPDK_ERRLOG("RPC Listen address Unix socket path too long\n");
			g_rpc_listen_addr_unix.sun_path[0] = '\0';
			return;
		}

		unlink(g_rpc_listen_addr_unix.sun_path);

		g_jsonrpc_server = spdk_jsonrpc_server_listen(AF_UNIX, 0,
				   (struct sockaddr *)&g_rpc_listen_addr_unix,
				   sizeof(g_rpc_listen_addr_unix),
				   spdk_jsonrpc_handler);
	} else {
		port = SPDK_JSONRPC_PORT_BASE + spdk_app_get_instance_id();
		snprintf(port_str, sizeof(port_str), "%" PRIu16, port);

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		if (getaddrinfo(listen_addr, port_str, &hints, &res) != 0) {
			SPDK_ERRLOG("Unable to look up RPC listen address '%s'\n", listen_addr);
			return;
		}

		g_jsonrpc_server = spdk_jsonrpc_server_listen(res->ai_family, res->ai_protocol,
				   res->ai_addr, res->ai_addrlen,
				   spdk_jsonrpc_handler);

		freeaddrinfo(res);
	}

	if (g_jsonrpc_server == NULL) {
		SPDK_ERRLOG("spdk_jsonrpc_server_listen() failed\n");
		return;
	}

	/* Register the periodic rpc_server_do_work */
	spdk_poller_register(&g_rpc_poller, spdk_rpc_server_do_work, NULL, spdk_app_get_current_core(),
			     NULL, RPC_SELECT_INTERVAL);
}

static int
spdk_rpc_initialize(void)
{
	/*
	 * Defer setup of the RPC service until the reactor has started.  This
	 *  allows us to detect the RPC listen socket as a suitable proxy for determining
	 *  when the SPDK application has finished initialization and ready for logins
	 *  or RPC commands.
	 */
	spdk_poller_register(&g_rpc_poller, spdk_rpc_setup, NULL, spdk_app_get_current_core(),
			     NULL, 0);
	return 0;
}

static void
spdk_rpc_finish_cleanup(struct spdk_event *event)
{
	if (g_jsonrpc_server) {
		spdk_jsonrpc_server_shutdown(g_jsonrpc_server);
	}
}

static int
spdk_rpc_finish(void)
{
	struct spdk_event *complete;

	if (g_rpc_listen_addr_unix.sun_path[0]) {
		/* Delete the Unix socket file */
		unlink(g_rpc_listen_addr_unix.sun_path);
	}

	complete = spdk_event_allocate(spdk_app_get_current_core(), spdk_rpc_finish_cleanup,
				       NULL, NULL, NULL);
	spdk_poller_unregister(&g_rpc_poller, complete);
	return 0;
}

static void
spdk_rpc_config_text(FILE *fp)
{
	fprintf(fp,
		"\n"
		"[Rpc]\n"
		"  # Defines whether to enable configuration via RPC.\n"
		"  # Default is disabled.  Note that the RPC interface is not\n"
		"  # authenticated, so users should be careful about enabling\n"
		"  # RPC in non-trusted environments.\n"
		"  Enable %s\n"
		"  # Listen address for the RPC service.\n"
		"  # May be an IP address or an absolute path to a Unix socket.\n"
		"  Listen %s\n",
		enable_rpc() ? "Yes" : "No", rpc_get_listen_addr());
}

SPDK_SUBSYSTEM_REGISTER(spdk_rpc, spdk_rpc_initialize, spdk_rpc_finish, spdk_rpc_config_text)
