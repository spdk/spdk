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
	struct spdk_jsonrpc_client_request *request, *temp;

	STAILQ_FOREACH_SAFE(request, &client->request, stailq, temp) {
		if (request->send_len > 0) {
			SPDK_NOTICELOG("\nSend RPC request send_len(%d) request_id 0x%x\n",
				request->send_len, request->request_id);
			rc = send(client->sockfd, request->send_buf + request->send_offset,
			  	request->send_len, 0);
			if (rc < 0) {
				/* For EINTR we pretend that nothing was send. */
				if (errno == EINTR) {
					rc = 0;
				} else {
					rc = -errno;
					SPDK_ERRLOG("poll() failed (%d): %s\n", errno, spdk_strerror(errno));
				}

				return rc;
			}

			request->send_offset += rc;
			request->send_len -= rc;
		}
		/*if (request->send_len == 0) {
			STAILQ_REMOVE(&client->request, request,
				spdk_jsonrpc_client_request, stailq);
			spdk_jsonrpc_client_free_request(request);
		}*/
	}

	return 0;
}

void
spdk_jsonrpc_client_resend_request(struct spdk_jsonrpc_client *client)
{
	struct spdk_jsonrpc_client_request *request, *temp;

	STAILQ_FOREACH_SAFE(request, &client->request, stailq, temp) {
		request->send_len = request->send_total_len;
		request->send_offset = 0;
	}
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
_spdk_jsonrpc_client_resp_ready_count(struct spdk_jsonrpc_client *client)
{
	return client->resp != NULL && client->resp->ready ? 1 : 0;
}

static int
_spdk_jsonrpc_client_recv(struct spdk_jsonrpc_client *client)
{
	ssize_t rc;

	if (client->recv_buf == NULL) {
		client->recv_buf = malloc(SPDK_JSONRPC_SEND_BUF_SIZE_INIT);
		if (!client->recv_buf) {
			rc = errno;
			SPDK_ERRLOG("malloc() failed (%d): %s\n", (int)rc, spdk_strerror(rc));
			return -rc;
		}
		client->recv_buf_size = SPDK_JSONRPC_SEND_BUF_SIZE_INIT;
		client->recv_offset = 0;
	} else if (client->recv_offset == client->recv_buf_size - 1) {
		rc = recv_buf_expand(client);
		if (rc) {
			return rc;
		}
	}

	rc = recv(client->sockfd, client->recv_buf + client->recv_offset,
		  client->recv_buf_size - client->recv_offset - 1, 0);
	if (rc < 0) {
		/* For EINTR we pretend that nothing was reveived. */
		if (errno == EINTR) {
			return 0;
		} else {
			rc = -errno;
			SPDK_ERRLOG("recv() failed (%d): %s\n", errno, spdk_strerror(errno));
			return rc;
		}
	} else if (rc == 0) {
		return -EIO;
	}

	client->recv_offset += rc;
	client->recv_buf[client->recv_offset] = '\0';

	/* Check to see if we have received a full JSON value. */
	return spdk_jsonrpc_parse_response(client);
}

static int
_spdk_jsonrpc_client_poll(struct spdk_jsonrpc_client *client, int timeout)
{
	int rc;
	struct pollfd pfd = { .fd = client->sockfd, .events = POLLIN | POLLOUT };

	rc = poll(&pfd, 1, timeout);
	if (rc == -1) {
		if (errno == EINTR) {
			/* For EINTR we pretend that nothing was received nor send. */
			rc = 0;
		} else {
			rc = -errno;
			SPDK_ERRLOG("poll() failed (%d): %s\n", errno, spdk_strerror(errno));
		}
	} else if (rc > 0) {
		rc = 0;

		if (pfd.revents & POLLOUT) {
			rc = _spdk_jsonrpc_client_send_request(client);
		}

		if (rc == 0 && (pfd.revents & POLLIN)) {
			rc = _spdk_jsonrpc_client_recv(client);
			/* Incomplete message in buffer isn't an error. */
			if (rc == -EAGAIN) {
				rc = 0;
			}
		}
	}

	return rc ? rc : _spdk_jsonrpc_client_resp_ready_count(client);
}

static int
_spdk_jsonrpc_client_poll_connecting(struct spdk_jsonrpc_client *client, int timeout)
{
	socklen_t rc_len;
	int rc;

	struct pollfd pfd = {
		.fd = client->sockfd,
		.events = POLLOUT
	};

	rc = poll(&pfd, 1, timeout);
	if (rc == 0) {
		return -ENOTCONN;
	} else if (rc == -1) {
		if (errno != EINTR) {
			SPDK_ERRLOG("poll() failed (%d): %s\n", errno, spdk_strerror(errno));
			goto err;
		}

		/* We are still not connected. Caller will have to call us again. */
		return -ENOTCONN;
	} else if (pfd.revents & ~POLLOUT) {
		/* We only poll for POLLOUT */
		goto err;
	} else if ((pfd.revents & POLLOUT) == 0) {
		/* Is this even possible to get here? */
		return -ENOTCONN;
	}

	rc_len = sizeof(int);
	/* connection might fail so need to check SO_ERROR. */
	if (getsockopt(client->sockfd, SOL_SOCKET, SO_ERROR, &rc, &rc_len) == -1) {
		goto err;
	}

	if (rc == 0) {
		client->connected = true;
		return 0;
	}

err:
	return -EIO;
}

static int
_spdk_jsonrpc_client_connect(struct spdk_jsonrpc_client *client, int domain, int protocol,
			     struct sockaddr *server_addr, socklen_t addrlen)
{
	int rc, flags;

	client->sockfd = socket(domain, SOCK_STREAM, protocol);
	if (client->sockfd < 0) {
		rc = errno;
		SPDK_ERRLOG("socket() failed\n");
		return -rc;
	}

	flags = fcntl(client->sockfd, F_GETFL);
	if (flags < 0 || fcntl(client->sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
		rc = errno;
		SPDK_ERRLOG("fcntl(): can't set nonblocking mode for socket (%d): %s\n",
			    errno, spdk_strerror(errno));
		goto err;
	}

	rc = connect(client->sockfd, server_addr, addrlen);
	if (rc != 0) {
		rc = errno;
		if (rc != EINPROGRESS) {
			SPDK_ERRLOG("could not connect to JSON-RPC server: %s\n", spdk_strerror(errno));
			goto err;
		}
	} else {
		client->connected = true;
	}

	return -rc;
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
	STAILQ_INIT(&client->request);

err:
	if (rc != 0 && rc != -EINPROGRESS) {
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
	struct spdk_jsonrpc_client_request *req, *temp;

	if (client->sockfd >= 0) {
		close(client->sockfd);
	}

	free(client->recv_buf);
	if (client->resp) {
		spdk_jsonrpc_client_free_response(&client->resp->jsonrpc);
	}
	STAILQ_FOREACH_SAFE(req, &client->request, stailq, temp) {
		STAILQ_REMOVE(&client->request, req, spdk_jsonrpc_client_request, stailq);
		spdk_jsonrpc_client_free_request(req);
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

void
spdk_jsonrpc_client_remove_request_from_list(struct spdk_jsonrpc_client *client,
			uint32_t request_id)
{
	struct spdk_jsonrpc_client_request *req, *temp;

	STAILQ_FOREACH_SAFE(req, &client->request, stailq, temp) {
		if (req->request_id == request_id) {
			STAILQ_REMOVE(&client->request, req, spdk_jsonrpc_client_request, stailq);
			spdk_jsonrpc_client_free_request(req);
			break;
		}
	}
}

bool
spdk_jsonrpc_client_request_list_empty(struct spdk_jsonrpc_client *client)
{
	return STAILQ_EMPTY(&client->request);
}

int
spdk_jsonrpc_client_poll(struct spdk_jsonrpc_client *client, int timeout)
{
	if (client->connected) {
		return _spdk_jsonrpc_client_poll(client, timeout);
	} else {
		return _spdk_jsonrpc_client_poll_connecting(client, timeout);
	}
}

int spdk_jsonrpc_client_send_request(struct spdk_jsonrpc_client *client,
				     struct spdk_jsonrpc_client_request *req)
{
	STAILQ_INSERT_TAIL(&client->request, req, stailq);
	return 0;
}

void spdk_jsonrpc_set_request_id(struct spdk_jsonrpc_client_request *req,
			uint32_t request_id)
{
	req->request_id = request_id;
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
