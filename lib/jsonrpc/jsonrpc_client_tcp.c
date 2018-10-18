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
#include "spdk/util.h"

#define RPC_DEFAULT_PORT	"5260"

static int
_spdk_jsonrpc_client_send_request(struct spdk_jsonrpc_client *client)
{
	ssize_t rc;
	struct spdk_jsonrpc_client_request *request = client->request;

	if (!request) {
		return 0;
	}

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

	client->request = NULL;

	spdk_jsonrpc_client_free_request(request);
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

static int
_spdk_jsonrpc_client_poll(struct spdk_jsonrpc_client *client)
{
	ssize_t rc = 0;
	size_t recv_avail;

	_spdk_jsonrpc_client_send_request(client);

	if (client->recv_buf == NULL) {
		/* memory malloc for recv-buf */
		client->recv_buf = malloc(SPDK_JSONRPC_SEND_BUF_SIZE_INIT);
		if (!client->recv_buf) {
			rc = errno;
			SPDK_ERRLOG("malloc() failed (%d): %s\n", (int)rc, spdk_strerror(rc));
			return -rc;
		}
		client->recv_buf_size = SPDK_JSONRPC_SEND_BUF_SIZE_INIT;
		client->recv_offset = 0;
	}

	recv_avail = client->recv_buf_size - client->recv_offset;
	while (recv_avail > 0) {
		rc = recv(client->sockfd, client->recv_buf + client->recv_offset, recv_avail - 1, 0);
		if (rc < 0) {
			if (errno == EINTR) {
				continue;
			} else {
				return errno;
			}
		} else if (rc == 0) {
			return -EIO;
		}

		client->recv_offset += rc;
		recv_avail -= rc;

		client->recv_buf[client->recv_offset] = '\0';

		/* Check to see if we have received a full JSON value. */
		rc = spdk_jsonrpc_parse_response(client);
		if (rc == 0) {
			/* Successfully parsed response */
			return 0;
		} else if (rc && rc != -EAGAIN) {
			SPDK_ERRLOG("jsonrpc parse request failed\n");
			return rc;
		}

		/* Expand receive buffer if larger one is needed */
		if (recv_avail == 1) {
			rc = recv_buf_expand(client);
			if (rc != 0) {
				return rc;
			}
			recv_avail = client->recv_buf_size - client->recv_offset;
		}
	}

	return 0;
}

static int
_spdk_jsonrpc_client_connect(struct spdk_jsonrpc_client *client, int domain, int protocol,
			     struct sockaddr *server_addr, socklen_t addrlen)
{
	int rc;

	client->sockfd = socket(domain, SOCK_STREAM, protocol);
	if (client->sockfd < 0) {
		rc = errno;
		SPDK_ERRLOG("socket() failed\n");
		return -rc;
	}

	rc = connect(client->sockfd, server_addr, addrlen);
	if (rc != 0) {
		SPDK_ERRLOG("could not connect to JSON-RPC server: %s\n", spdk_strerror(errno));
		goto err;
	}

	return 0;
err:
	close(client->sockfd);
	client->sockfd = -1;
	return -rc;
}

struct spdk_jsonrpc_client *
spdk_jsonrpc_client_connect(const char *addr, int addr_family)
{
	struct spdk_jsonrpc_client *client = calloc(1, sizeof(struct spdk_jsonrpc_client));
	/* Unix Domain Socket */
	struct sockaddr_un addr_un = {};
	char *add_in = NULL;
	int rc;

	if (client == NULL) {
		SPDK_ERRLOG("%s\n", spdk_strerror(errno));
		return NULL;
	}

	if (addr_family == AF_UNIX) {
		addr_un.sun_family = AF_UNIX;
		rc = snprintf(addr_un.sun_path, sizeof(addr_un.sun_path), "%s", addr);
		if (rc < 0 || (size_t)rc >= sizeof(addr_un.sun_path)) {
			rc = -EINVAL;
			SPDK_ERRLOG("RPC Listen address Unix socket path too long\n");
			goto err;
		}

		rc = _spdk_jsonrpc_client_connect(client, AF_UNIX, 0, (struct sockaddr *)&addr_un, sizeof(addr_un));
	} else {
		/* TCP/IP socket */
		struct addrinfo		hints;
		struct addrinfo		*res;
		char *host, *port;

		add_in = strdup(addr);
		if (!add_in) {
			rc = -errno;
			SPDK_ERRLOG("%s\n", spdk_strerror(errno));
			goto err;
		}

		rc = spdk_parse_ip_addr(add_in, &host, &port);
		if (rc) {
			SPDK_ERRLOG("Invalid listen address '%s'\n", addr);
			goto err;
		}

		if (port == NULL) {
			port = RPC_DEFAULT_PORT;
		}

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		rc = getaddrinfo(host, port, &hints, &res);
		if (rc != 0) {
			SPDK_ERRLOG("Unable to look up RPC connnect address '%s' (%d): %s\n", addr, rc, gai_strerror(rc));
			rc = -EINVAL;
			goto err;
		}

		rc = _spdk_jsonrpc_client_connect(client, res->ai_family, res->ai_protocol, res->ai_addr,
						  res->ai_addrlen);
		freeaddrinfo(res);
	}

err:
	if (rc != 0) {
		free(client);
		client = NULL;
		errno = -rc;
	}

	free(add_in);
	return client;
}

void
spdk_jsonrpc_client_close(struct spdk_jsonrpc_client *client)
{
	if (client->sockfd >= 0) {
		close(client->sockfd);
	}

	free(client->recv_buf);
	if (client->resp) {
		spdk_jsonrpc_client_free_response(&client->resp->jsonrpc);
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
spdk_jsonrpc_client_recv_response(struct spdk_jsonrpc_client *client)
{
	return _spdk_jsonrpc_client_poll(client);
}

int spdk_jsonrpc_client_send_request(struct spdk_jsonrpc_client *client,
				     struct spdk_jsonrpc_client_request *req)
{
	if (client->request != NULL) {
		return -ENOSPC;
	}

	client->request = req;
	return 0;
}

struct spdk_jsonrpc_client_response *
spdk_jsonrpc_client_get_response(struct spdk_jsonrpc_client *client)
{
	struct spdk_jsonrpc_client_response_internal *r;

	r = client->resp;
	if (r == NULL || r->ready == false) {
		return NULL;
	}

	client->resp = NULL;
	return &r->jsonrpc;
}

void
spdk_jsonrpc_client_free_response(struct spdk_jsonrpc_client_response *resp)
{
	struct spdk_jsonrpc_client_response_internal *r;

	if (!resp) {
		return;
	}

	r = SPDK_CONTAINEROF(resp, struct spdk_jsonrpc_client_response_internal, jsonrpc);
	free(r->buf);
	free(r);
}
