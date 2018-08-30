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
#include "jsonrpc_internal.h"

#define RPC_DEFAULT_PORT	"5260"

static struct spdk_jsonrpc_client *
_spdk_jsonrpc_client_connect(int domain, int protocol,
			     struct sockaddr *server_addr, socklen_t addrlen)
{
	struct spdk_jsonrpc_client *client;
	int rc;

	client = calloc(1, sizeof(struct spdk_jsonrpc_client));
	if (client == NULL) {
		return NULL;
	}

	client->sockfd = socket(domain, SOCK_STREAM, protocol);
	if (client->sockfd < 0) {
		SPDK_ERRLOG("socket() failed\n");
		free(client);
		return NULL;
	}

	rc = connect(client->sockfd, server_addr, addrlen);
	if (rc != 0) {
		SPDK_ERRLOG("could not connet JSON-RPC server: %s\n", spdk_strerror(errno));
		close(client->sockfd);
		free(client);
		return NULL;
	}

	/* memory malloc for recv-buf */
	client->recv_buf = malloc(SPDK_JSONRPC_SEND_BUF_SIZE_INIT);
	if (!client->recv_buf) {
		SPDK_ERRLOG("memory malloc for recv-buf failed\n");
		close(client->sockfd);
		free(client);
		return NULL;
	}
	client->recv_buf_size = SPDK_JSONRPC_SEND_BUF_SIZE_INIT;

	return client;
}

struct spdk_jsonrpc_client *
spdk_jsonrpc_client_connect(const char *rpc_sock_addr, int addr_family)
{
	struct spdk_jsonrpc_client *client;

	if (addr_family == AF_UNIX) {
		/* Unix Domain Socket */
		struct sockaddr_un rpc_sock_addr_unix = {};
		int rc;

		rpc_sock_addr_unix.sun_family = AF_UNIX;
		rc = snprintf(rpc_sock_addr_unix.sun_path,
			      sizeof(rpc_sock_addr_unix.sun_path),
			      "%s", rpc_sock_addr);
		if (rc < 0 || (size_t)rc >= sizeof(rpc_sock_addr_unix.sun_path)) {
			SPDK_ERRLOG("RPC Listen address Unix socket path too long\n");
			return NULL;
		}

		client = _spdk_jsonrpc_client_connect(AF_UNIX, 0,
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
			SPDK_ERRLOG("Out of memory\n");
			return NULL;
		}

		if (spdk_parse_ip_addr(tmp, &host, &port) < 0) {
			free(tmp);
			SPDK_ERRLOG("Invalid listen address '%s'\n", rpc_sock_addr);
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
			SPDK_ERRLOG("Unable to look up RPC connnect address '%s'\n", rpc_sock_addr);
			return NULL;
		}

		client = _spdk_jsonrpc_client_connect(res->ai_family, res->ai_protocol,
						      res->ai_addr, res->ai_addrlen);

		freeaddrinfo(res);
		free(tmp);
	}

	return client;
}

void
spdk_jsonrpc_client_close(struct spdk_jsonrpc_client *client)
{
	if (client->sockfd >= 0) {
		close(client->sockfd);
		free(client->recv_buf);
		client->sockfd = -1;
	}

	free(client);
}

struct spdk_jsonrpc_client_request *
spdk_jsonrpc_client_create_request(void)
{
	struct spdk_jsonrpc_client_request *request;

	request = calloc(1, sizeof(*request));
	if (request == NULL) {
		return NULL;
	}

	/* memory malloc for send-buf */
	request->send_buf = malloc(SPDK_JSONRPC_SEND_BUF_SIZE_INIT);
	if (!request->send_buf) {
		SPDK_ERRLOG("memory malloc for send-buf failed\n");
		free(request);
		return NULL;
	}
	request->send_buf_size = SPDK_JSONRPC_SEND_BUF_SIZE_INIT;

	return request;
}

void
spdk_jsonrpc_client_free_request(struct spdk_jsonrpc_client_request *req)
{
	free(req->send_buf);
	free(req);
}

int
spdk_jsonrpc_client_send_request(struct spdk_jsonrpc_client *client,
				 struct spdk_jsonrpc_client_request *request)
{
	ssize_t rc;

	/* Reset offset in request */
	request->send_offset = 0;

	while (request->send_len > 0) {
		rc = send(client->sockfd, request->send_buf + request->send_offset,
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

	return 0;
}

static int
recv_buf_expand(struct spdk_jsonrpc_client *client)
{
	uint8_t *new_buf;

	if (client->recv_buf_size * 2 > SPDK_JSONRPC_SEND_BUF_SIZE_MAX) {
		return -ENOSPC;
	}

	new_buf = realloc(client->recv_buf, client->recv_buf_size * 2);
	if (new_buf == NULL) {
		SPDK_ERRLOG("Resizing recv_buf failed (current size %zu, new size %zu)\n",
			    client->recv_buf_size, client->recv_buf_size * 2);
		return -ENOMEM;
	}

	client->recv_buf = new_buf;
	client->recv_buf_size *= 2;

	return 0;
}

int
spdk_jsonrpc_client_recv_response(struct spdk_jsonrpc_client *client,
				  spdk_jsonrpc_client_response_parser parser_fn,
				  void *parser_ctx)
{
	ssize_t rc = 0;
	size_t recv_avail;
	size_t recv_offset = 0;

	client->parser_fn = parser_fn;
	client->parser_ctx = parser_ctx;

	recv_avail = client->recv_buf_size;

	while (recv_avail > 0) {
		rc = recv(client->sockfd, client->recv_buf + recv_offset, recv_avail, 0);
		if (rc < 0) {
			if (errno == EINTR) {
				continue;
			} else {
				return errno;
			}
		} else if (rc == 0) {
			return -EIO;
		}

		recv_offset += rc;
		recv_avail -= rc;

		/* Check to see if we have received a full JSON value. */
		rc = spdk_jsonrpc_parse_response(client, client->recv_buf, recv_offset);
		if (rc == 0) {
			/* Successfully parsed response */
			return 0;
		} else if (rc != SPDK_JSON_PARSE_INCOMPLETE) {
			SPDK_ERRLOG("jsonrpc parse request failed\n");
			return -EINVAL;
		}

		/* Expand receive buffer if larger one is needed */
		if (recv_avail == 0) {
			rc = recv_buf_expand(client);
			if (rc != 0) {
				return rc;
			}
			recv_avail = client->recv_buf_size - recv_offset;
		}
	}

	return 0;
}
