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

#include "spdk/string.h"
#include "spdk/jsonrpc.h"
#include "spdk/rpc_client.h"

#define RPC_DEFAULT_PORT	"5260"

struct spdk_rpc_client_conn *
spdk_rpc_client_connect(const char *rpc_sock_addr, int addr_family)
{
	struct spdk_jsonrpc_client_conn *conn;

	if (addr_family == AF_UNIX) {
		/* Unix Domain Socket */
		struct sockaddr_un rpc_sock_addr_unix = {};
		int rc;

		rpc_sock_addr_unix.sun_family = AF_UNIX;
		rc = snprintf(rpc_sock_addr_unix.sun_path,
			      sizeof(rpc_sock_addr_unix.sun_path),
			      "%s", rpc_sock_addr);
		if (rc < 0 || (size_t)rc >= sizeof(rpc_sock_addr_unix.sun_path)) {
			CLIENT_ERRLOG("RPC Listen address Unix socket path too long\n");
			return NULL;
		}

		conn = spdk_jsonrpc_client_connect(AF_UNIX, 0,
						   (struct sockaddr *)&rpc_sock_addr_unix,
						   sizeof(rpc_sock_addr_unix));
	} else {
		/* TCP/IP socket */
		struct addrinfo		hints;
		struct addrinfo		*res;
		char *tmp;
		char *host, *port;

		tmp = strdup(rpc_sock_addr);
		if (!tmp) {
			CLIENT_ERRLOG("Out of memory\n");
			return NULL;
		}

		if (spdk_parse_ip_addr(tmp, &host, &port) < 0) {
			free(tmp);
			CLIENT_ERRLOG("Invalid listen address '%s'\n", rpc_sock_addr);
			return NULL;
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
			CLIENT_ERRLOG("Unable to look up RPC connnect address '%s'\n", rpc_sock_addr);
			return NULL;
		}

		conn = spdk_jsonrpc_client_connect(res->ai_family, res->ai_protocol,
						   res->ai_addr, res->ai_addrlen);

		freeaddrinfo(res);
		free(tmp);
	}

	return (struct spdk_rpc_client_conn *)conn;
}
void
spdk_rpc_client_close(struct spdk_rpc_client_conn *conn)
{
	spdk_jsonrpc_client_close((struct spdk_jsonrpc_client_conn *)conn);
}
