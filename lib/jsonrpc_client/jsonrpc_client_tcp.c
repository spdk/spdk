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
#include "jsonrpc_client_internal.h"

#define RPC_DEFAULT_PORT	"5260"

static struct spdk_jsonrpc_client_conn *
_spdk_jsonrpc_client_connect(int domain, int protocol,
			     struct sockaddr *server_addr, socklen_t addrlen)
{
	struct spdk_jsonrpc_client_conn *conn;
	int rc;

	conn = calloc(1, sizeof(struct spdk_jsonrpc_client_conn));
	if (conn == NULL) {
		return NULL;
	}

	conn->sockfd = socket(domain, SOCK_STREAM, protocol);
	if (conn->sockfd < 0) {
		CLIENT_ERRLOG("socket() failed\n");
		free(conn);
		return NULL;
	}

	rc = connect(conn->sockfd, server_addr, addrlen);;
	if (rc != 0) {
		CLIENT_ERRLOG("could not connet JSON-RPC server: %s\n", spdk_strerror(errno));
		close(conn->sockfd);
		free(conn);
		return NULL;
	}

	return conn;
}

struct spdk_jsonrpc_client_conn *
spdk_jsonrpc_client_connect(const char *connect_addr)
{
	struct spdk_jsonrpc_client_conn *conn;

	if (connect_addr[0] == '/') {
		/* Unix Domain Socket */
		struct sockaddr_un rpc_client_addr_unix = {};
		int rc;

		rpc_client_addr_unix.sun_family = AF_UNIX;
		rc = snprintf(rpc_client_addr_unix.sun_path,
			      sizeof(rpc_client_addr_unix.sun_path),
			      "%s", connect_addr);
		if (rc < 0 || (size_t)rc >= sizeof(rpc_client_addr_unix.sun_path)) {
			CLIENT_ERRLOG("RPC Listen address Unix socket path too long\n");
			return NULL;
		}

		conn = _spdk_jsonrpc_client_connect(AF_UNIX, 0,
						    (struct sockaddr *)&rpc_client_addr_unix,
						    sizeof(rpc_client_addr_unix));
	} else {
		/* TCP/IP socket */
		struct addrinfo		hints;
		struct addrinfo		*res;
		char *tmp;
		char *host, *port;

		tmp = strdup(connect_addr);
		if (!tmp) {
			CLIENT_ERRLOG("Out of memory\n");
			return NULL;
		}

		if (spdk_parse_ip_addr(tmp, &host, &port) < 0) {
			free(tmp);
			CLIENT_ERRLOG("Invalid listen address '%s'\n", connect_addr);
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
			CLIENT_ERRLOG("Unable to look up RPC connnect address '%s'\n", connect_addr);
			return NULL;
		}

		conn = _spdk_jsonrpc_client_connect(res->ai_family, res->ai_protocol,
						    res->ai_addr, res->ai_addrlen);

		freeaddrinfo(res);
		free(tmp);
	}

	if (conn) {
		conn->request.send_buf = malloc(SPDK_JSONRPC_CLIENT_BUF_SIZE_INIT);
		conn->response.recv_buf = malloc(SPDK_JSONRPC_CLIENT_BUF_SIZE_INIT);
		if (!conn->request.send_buf || !conn->response.recv_buf) {
			spdk_jsonrpc_client_close(conn);

			return NULL;
		}
		conn->request.send_buf_size = SPDK_JSONRPC_CLIENT_BUF_SIZE_INIT;
		conn->response.recv_buf_size = SPDK_JSONRPC_CLIENT_BUF_SIZE_INIT;
	}

	return conn;
}

void
spdk_jsonrpc_client_close(struct spdk_jsonrpc_client_conn *conn)
{
	if (conn->sockfd >= 0) {
		close(conn->sockfd);
		free(conn->request.send_buf);
		free(conn->response.recv_buf);
		conn->sockfd = -1;
	}
}

int
spdk_jsonrpc_client_send_request(struct spdk_jsonrpc_client_conn *conn)
{
	struct spdk_jsonrpc_client_request *request = &conn->request;
	ssize_t rc;

	while (request->send_len > 0) {
		rc = send(conn->sockfd, request->send_buf + request->send_offset,
			  request->send_len, 0);
		if (rc <= 0) {
			if (rc < 0 && errno == EINTR) {
				rc = 0;
			} else {
				return rc;
			}
		}

		request->send_offset += rc;
		request->send_len -= rc;
	}

	/* Reset offset in request */
	request->send_offset = 0;

	return 0;
}

static int
response_buf_broaden(struct spdk_jsonrpc_client_response *response)
{
	uint8_t *new_buf;

	if (response->recv_buf_size * 2 > SPDK_JSONRPC_CLIENT_BUF_SIZE_MAX) {
		return -ENOMEM;
	}

	new_buf = realloc(response->recv_buf, response->recv_buf_size * 2);
	if (new_buf == NULL) {
		CLIENT_ERRLOG("Resizing recv_buf failed (current size %zu, new size %zu)\n",
			      response->recv_buf_size, response->recv_buf_size * 2);
		return -ENOMEM;
	}

	response->recv_buf = new_buf;
	response->recv_buf_size *= 2;

	return 0;
}

int
spdk_jsonrpc_client_recv_response(struct spdk_jsonrpc_client_conn *conn)
{
	ssize_t rc = 0;
	struct spdk_jsonrpc_client_response *response = &conn->response;
	size_t recv_avail = response->recv_buf_size - response->recv_offset;

	while (recv_avail > 0) {
		rc = recv(conn->sockfd, response->recv_buf + response->recv_offset, recv_avail, 0);
		if (rc < 0) {
			if (errno == EINTR) {
				rc = 0;
				continue;
			} else {
				return errno;
			}
		} else if (rc == 0) {
			return -EIO;
		}

		response->recv_offset += rc;
		recv_avail -= rc;

		/* Check to see if we have received a full JSON value. */
		rc = spdk_jsonrpc_client_parse_response(conn, response->recv_buf, response->recv_offset);
		if (rc > 0) {
			/* Reset offset in response */
			response->recv_offset = 0;
			/* Successfully parsed response */
			return 0;
		} else if (rc < 0) {
			CLIENT_ERRLOG("jsonrpc parse request failed\n");
			return -EINVAL;
		}

		/* Broaden receive buffer if larger one is needed */
		if (recv_avail == 0) {
			rc = response_buf_broaden(response);
			if (rc != 0) {
				return rc;
			}
			recv_avail = response->recv_buf_size - response->recv_offset;
		}
	}

	return 0;
}
