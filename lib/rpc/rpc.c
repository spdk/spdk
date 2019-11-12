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

#include <sys/file.h>

#include "spdk/stdinc.h"

#include "spdk/queue.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/version.h"

#define RPC_DEFAULT_PORT	"5260"

static struct sockaddr_un g_rpc_listen_addr_unix = {};
static char g_rpc_lock_path[sizeof(g_rpc_listen_addr_unix.sun_path) + sizeof(".lock")];
static int g_rpc_lock_fd = -1;

static struct spdk_jsonrpc_server *g_jsonrpc_server = NULL;
static uint32_t g_rpc_state;
static bool g_rpcs_correct = true;

struct spdk_rpc_method {
	const char *name;
	spdk_rpc_method_handler func;
	SLIST_ENTRY(spdk_rpc_method) slist;
	uint32_t state_mask;
	bool is_deprecated;
	struct spdk_rpc_method *is_alias_of;
	bool deprecation_warning_printed;
};

static SLIST_HEAD(, spdk_rpc_method) g_rpc_methods = SLIST_HEAD_INITIALIZER(g_rpc_methods);

void
spdk_rpc_set_state(uint32_t state)
{
	g_rpc_state = state;
}

uint32_t
spdk_rpc_get_state(void)
{
	return g_rpc_state;
}

static struct spdk_rpc_method *
_get_rpc_method(const struct spdk_json_val *method)
{
	struct spdk_rpc_method *m;

	SLIST_FOREACH(m, &g_rpc_methods, slist) {
		if (spdk_json_strequal(method, m->name)) {
			return m;
		}
	}

	return NULL;
}

static struct spdk_rpc_method *
_get_rpc_method_raw(const char *method)
{
	struct spdk_json_val method_val;

	method_val.type = SPDK_JSON_VAL_STRING;
	method_val.len = strlen(method);
	method_val.start = (char *)method;

	return _get_rpc_method(&method_val);
}

static void
spdk_jsonrpc_handler(struct spdk_jsonrpc_request *request,
		     const struct spdk_json_val *method,
		     const struct spdk_json_val *params)
{
	struct spdk_rpc_method *m;

	assert(method != NULL);

	m = _get_rpc_method(method);
	if (m == NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_METHOD_NOT_FOUND, "Method not found");
		return;
	}

	if (m->is_alias_of != NULL) {
		if (m->is_deprecated && !m->deprecation_warning_printed) {
			SPDK_WARNLOG("RPC method %s is deprecated.  Use %s instead.\n", m->name, m->is_alias_of->name);
			m->deprecation_warning_printed = true;
		}
		m = m->is_alias_of;
	}

	if ((m->state_mask & g_rpc_state) == g_rpc_state) {
		m->func(request, params);
	} else {
		spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_STATE,
						     "Method is allowed in any state in the mask (%"PRIx32"),"
						     " but current state is (%"PRIx32")",
						     m->state_mask, g_rpc_state);
	}
}

int
spdk_rpc_listen(const char *listen_addr)
{
	struct addrinfo		hints;
	struct addrinfo		*res;

	memset(&g_rpc_listen_addr_unix, 0, sizeof(g_rpc_listen_addr_unix));

	if (listen_addr[0] == '/') {
		int rc;

		g_rpc_listen_addr_unix.sun_family = AF_UNIX;
		rc = snprintf(g_rpc_listen_addr_unix.sun_path,
			      sizeof(g_rpc_listen_addr_unix.sun_path),
			      "%s", listen_addr);
		if (rc < 0 || (size_t)rc >= sizeof(g_rpc_listen_addr_unix.sun_path)) {
			SPDK_ERRLOG("RPC Listen address Unix socket path too long\n");
			g_rpc_listen_addr_unix.sun_path[0] = '\0';
			return -1;
		}

		snprintf(g_rpc_lock_path, sizeof(g_rpc_lock_path), "%s.lock",
			 g_rpc_listen_addr_unix.sun_path);

		g_rpc_lock_fd = open(g_rpc_lock_path, O_RDONLY | O_CREAT, 0600);
		if (g_rpc_lock_fd == -1) {
			SPDK_ERRLOG("Cannot open lock file %s: %s\n",
				    g_rpc_lock_path, spdk_strerror(errno));
			return -1;
		}

		rc = flock(g_rpc_lock_fd, LOCK_EX | LOCK_NB);
		if (rc != 0) {
			SPDK_ERRLOG("RPC Unix domain socket path %s in use. Specify another.\n",
				    g_rpc_listen_addr_unix.sun_path);
			return -1;
		}

		/*
		 * Since we acquired the lock, it is safe to delete the Unix socket file
		 * if it still exists from a previous process.
		 */
		unlink(g_rpc_listen_addr_unix.sun_path);

		g_jsonrpc_server = spdk_jsonrpc_server_listen(AF_UNIX, 0,
				   (struct sockaddr *)&g_rpc_listen_addr_unix,
				   sizeof(g_rpc_listen_addr_unix),
				   spdk_jsonrpc_handler);
		if (g_jsonrpc_server == NULL) {
			close(g_rpc_lock_fd);
			g_rpc_lock_fd = -1;
			unlink(g_rpc_lock_path);
			g_rpc_lock_path[0] = '\0';
		}
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
spdk_rpc_register_method(const char *method, spdk_rpc_method_handler func, uint32_t state_mask)
{
	struct spdk_rpc_method *m;

	m = _get_rpc_method_raw(method);
	if (m != NULL) {
		SPDK_ERRLOG("duplicate RPC %s registered...\n", method);
		g_rpcs_correct = false;
		return;
	}

	m = calloc(1, sizeof(struct spdk_rpc_method));
	assert(m != NULL);

	m->name = strdup(method);
	assert(m->name != NULL);

	m->func = func;
	m->state_mask = state_mask;

	/* TODO: use a hash table or sorted list */
	SLIST_INSERT_HEAD(&g_rpc_methods, m, slist);
}

void
spdk_rpc_register_alias_deprecated(const char *method, const char *alias)
{
	struct spdk_rpc_method *m, *base;

	base = _get_rpc_method_raw(method);
	if (base == NULL) {
		SPDK_ERRLOG("cannot create alias %s - method %s does not exist\n",
			    alias, method);
		g_rpcs_correct = false;
		return;
	}

	if (base->is_alias_of != NULL) {
		SPDK_ERRLOG("cannot create alias %s of alias %s\n", alias, method);
		g_rpcs_correct = false;
		return;
	}

	m = calloc(1, sizeof(struct spdk_rpc_method));
	assert(m != NULL);

	m->name = strdup(alias);
	assert(m->name != NULL);

	m->is_alias_of = base;
	m->is_deprecated = true;
	m->state_mask = base->state_mask;

	/* TODO: use a hash table or sorted list */
	SLIST_INSERT_HEAD(&g_rpc_methods, m, slist);
}

bool
spdk_rpc_verify_methods(void)
{
	return g_rpcs_correct;
}

int
spdk_rpc_is_method_allowed(const char *method, uint32_t state_mask)
{
	struct spdk_rpc_method *m;

	SLIST_FOREACH(m, &g_rpc_methods, slist) {
		if (strcmp(m->name, method) != 0) {
			continue;
		}

		if ((m->state_mask & state_mask) == state_mask) {
			return 0;
		} else {
			return -EPERM;
		}
	}

	return -ENOENT;
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

		if (g_rpc_lock_fd != -1) {
			close(g_rpc_lock_fd);
			g_rpc_lock_fd = -1;
		}

		if (g_rpc_lock_path[0]) {
			unlink(g_rpc_lock_path);
			g_rpc_lock_path[0] = '\0';
		}
	}
}

struct rpc_get_methods {
	bool current;
	bool include_aliases;
};

static const struct spdk_json_object_decoder rpc_get_methods_decoders[] = {
	{"current", offsetof(struct rpc_get_methods, current), spdk_json_decode_bool, true},
	{"include_aliases", offsetof(struct rpc_get_methods, include_aliases), spdk_json_decode_bool, true},
};

static void
spdk_rpc_get_methods(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_get_methods req = {};
	struct spdk_json_write_ctx *w;
	struct spdk_rpc_method *m;

	if (params != NULL) {
		if (spdk_json_decode_object(params, rpc_get_methods_decoders,
					    SPDK_COUNTOF(rpc_get_methods_decoders), &req)) {
			SPDK_ERRLOG("spdk_json_decode_object failed\n");
			spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
							 "Invalid parameters");
			return;
		}
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_array_begin(w);
	SLIST_FOREACH(m, &g_rpc_methods, slist) {
		if (m->is_alias_of != NULL && !req.include_aliases) {
			continue;
		}
		if (req.current && ((m->state_mask & g_rpc_state) != g_rpc_state)) {
			continue;
		}
		spdk_json_write_string(w, m->name);
	}
	spdk_json_write_array_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("rpc_get_methods", spdk_rpc_get_methods, SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(rpc_get_methods, get_rpc_methods)

static void
spdk_rpc_spdk_get_version(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;

	if (params != NULL) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_get_version method requires no parameters");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string_fmt(w, "version", "%s", SPDK_VERSION_STRING);
	spdk_json_write_named_object_begin(w, "fields");
	spdk_json_write_named_uint32(w, "major", SPDK_VERSION_MAJOR);
	spdk_json_write_named_uint32(w, "minor", SPDK_VERSION_MINOR);
	spdk_json_write_named_uint32(w, "patch", SPDK_VERSION_PATCH);
	spdk_json_write_named_string_fmt(w, "suffix", "%s", SPDK_VERSION_SUFFIX);
#ifdef SPDK_GIT_COMMIT
	spdk_json_write_named_string_fmt(w, "commit", "%s", SPDK_GIT_COMMIT_STRING);
#endif
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("spdk_get_version", spdk_rpc_spdk_get_version,
		  SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)
SPDK_RPC_REGISTER_ALIAS_DEPRECATED(spdk_get_version, get_spdk_version)
