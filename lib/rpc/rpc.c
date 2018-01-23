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

#include <sys/file.h>

#include "spdk/stdinc.h"

#include "spdk/queue.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/string.h"

#define RPC_DEFAULT_PORT		"5260"

#define SPDK_DEFAULT_RPC_LOCK_ADDR	"/var/tmp/spdk_sock.lock"

static struct sockaddr_un g_rpc_listen_addr_unix = {};

static struct spdk_jsonrpc_server *g_jsonrpc_server = NULL;

struct spdk_rpc_method {
	const char *name;
	spdk_rpc_method_handler func;
	SLIST_ENTRY(spdk_rpc_method) slist;
};

static SLIST_HEAD(, spdk_rpc_method) g_rpc_methods = SLIST_HEAD_INITIALIZER(g_rpc_methods);

static void
spdk_jsonrpc_handler(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *method,
		     const struct spdk_json_val *params)
{
	struct spdk_rpc_method *m;

	assert(method != NULL);

	SLIST_FOREACH(m, &g_rpc_methods, slist) {
		if (spdk_json_strequal(method, m->name)) {
			m->func(request, params);
			return;
		}
	}

	spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_METHOD_NOT_FOUND, "Method not found");
}

int
spdk_rpc_listen(const char *listen_addr)
{
	struct addrinfo		hints;
	struct addrinfo		*res;
	int			rc, lock_fd;

	memset(&g_rpc_listen_addr_unix, 0, sizeof(g_rpc_listen_addr_unix));

	if (listen_addr[0] == '/') {
		g_rpc_listen_addr_unix.sun_family = AF_UNIX;
		rc = snprintf(g_rpc_listen_addr_unix.sun_path,
			      sizeof(g_rpc_listen_addr_unix.sun_path),
			      "%s", listen_addr);
		if (rc < 0 || (size_t)rc >= sizeof(g_rpc_listen_addr_unix.sun_path)) {
			SPDK_ERRLOG("RPC Listen address Unix socket path too long\n");
			g_rpc_listen_addr_unix.sun_path[0] = '\0';
			return -1;
		}

		lock_fd = open(SPDK_DEFAULT_RPC_LOCK_ADDR, O_RDONLY | O_CREAT, 0600);
		if (lock_fd == -1) {
			SPDK_ERRLOG("Can not open lock file %s\n", SPDK_DEFAULT_RPC_LOCK_ADDR);
			return -1;
		}

		if (access(g_rpc_listen_addr_unix.sun_path, F_OK) == 0) {
			rc = flock(lock_fd, LOCK_EX | LOCK_NB);
			if (rc != 0) {
				SPDK_ERRLOG("RPC Unix domain socket path in use. Specify another.\n");
				return -1;
			} else {
				SPDK_NOTICELOG("Remove the existing RPC Unix domain socket path\n");
				/* Delete the Unix socket file */
				unlink(g_rpc_listen_addr_unix.sun_path);
			}
		}

		g_jsonrpc_server = spdk_jsonrpc_server_listen(AF_UNIX, 0,
				   (struct sockaddr *)&g_rpc_listen_addr_unix,
				   sizeof(g_rpc_listen_addr_unix),
				   spdk_jsonrpc_handler);
	} else {
		char *tmp;
		char *host, *port;

		tmp = strdup(listen_addr);
		if (!tmp) {
			SPDK_ERRLOG("Out of memory\n");
			return -1;
		}

		if (spdk_parse_ip_addr(tmp, &host, &port) < 0) {
			free(tmp);
			SPDK_ERRLOG("Invalid listen address '%s'\n", listen_addr);
			return -1;
		}

		if (port == NULL) {
			port = RPC_DEFAULT_PORT;
		}

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		if (getaddrinfo(host, port, &hints, &res) != 0) {
			free(tmp);
			SPDK_ERRLOG("Unable to look up RPC listen address '%s'\n", listen_addr);
			return -1;
		}

		g_jsonrpc_server = spdk_jsonrpc_server_listen(res->ai_family, res->ai_protocol,
				   res->ai_addr, res->ai_addrlen,
				   spdk_jsonrpc_handler);

		freeaddrinfo(res);
		free(tmp);
	}

	if (g_jsonrpc_server == NULL) {
		SPDK_ERRLOG("spdk_jsonrpc_server_listen() failed\n");
		return -1;
	}

	return 0;
}

void
spdk_rpc_accept(void)
{
	spdk_jsonrpc_server_poll(g_jsonrpc_server);
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

void
spdk_rpc_close(void)
{
	if (g_jsonrpc_server) {
		if (g_rpc_listen_addr_unix.sun_path[0]) {
			/* Delete the Unix socket file */
			unlink(g_rpc_listen_addr_unix.sun_path);
		}

		spdk_jsonrpc_server_shutdown(g_jsonrpc_server);
		g_jsonrpc_server = NULL;
	}
}


static void
spdk_rpc_get_rpc_methods(struct spdk_jsonrpc_request *request,
			 const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	struct spdk_rpc_method *m;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "get_rpc_methods requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	if (w == NULL) {
		return;
	}

	spdk_json_write_array_begin(w);
	SLIST_FOREACH(m, &g_rpc_methods, slist) {
		spdk_json_write_string(w, m->name);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_rpc_methods", spdk_rpc_get_rpc_methods)
