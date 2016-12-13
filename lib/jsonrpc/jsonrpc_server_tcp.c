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

#include "jsonrpc_internal.h"

struct spdk_jsonrpc_server *
spdk_jsonrpc_server_listen(int domain, int protocol,
			   struct sockaddr *listen_addr, socklen_t addrlen,
			   spdk_jsonrpc_handle_request_fn handle_request)
{
	struct spdk_jsonrpc_server *server;
	int rc, val;

	server = calloc(1, sizeof(struct spdk_jsonrpc_server));
	if (server == NULL) {
		return NULL;
	}

	server->handle_request = handle_request;

	server->sockfd = socket(domain, SOCK_STREAM, protocol);
	if (server->sockfd < 0) {
		SPDK_ERRLOG("socket() failed\n");
		free(server);
		return NULL;
	}

	val = 1;
	setsockopt(server->sockfd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
	if (protocol == IPPROTO_TCP) {
		setsockopt(server->sockfd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
	}

	val = 1;
	rc = ioctl(server->sockfd, FIONBIO, &val);
	if (rc != 0) {
		SPDK_ERRLOG("ioctl(FIONBIO) failed\n");
		close(server->sockfd);
		free(server);
		return NULL;
	}

	rc = bind(server->sockfd, listen_addr, addrlen);
	if (rc != 0) {
		SPDK_ERRLOG("could not bind JSON-RPC server: %s\n", strerror(errno));
		close(server->sockfd);
		free(server);
		return NULL;
	}

	rc = listen(server->sockfd, 512);
	if (rc != 0) {
		SPDK_ERRLOG("listen() failed, errno = %d\n", errno);
		close(server->sockfd);
		free(server);
		return NULL;
	}

	/* Put listen socket in pollfds[0] */
	server->pollfds[0].fd = server->sockfd;
	server->pollfds[0].events = POLLIN;

	return server;
}

void
spdk_jsonrpc_server_shutdown(struct spdk_jsonrpc_server *server)
{
	int i;

	close(server->sockfd);

	for (i = 0; i < server->num_conns; i++) {
		close(server->conns[i].sockfd);
	}

	free(server);
}

static void
spdk_jsonrpc_server_conn_remove(struct spdk_jsonrpc_server_conn *conn)
{
	struct spdk_jsonrpc_server *server = conn->server;
	int conn_idx = conn - server->conns;

	close(conn->sockfd);

	/* Swap conn with the last entry in conns */
	server->conns[conn_idx] = server->conns[server->num_conns - 1];
	server->num_conns--;
}

static int
spdk_jsonrpc_server_accept(struct spdk_jsonrpc_server *server)
{
	struct spdk_jsonrpc_server_conn *conn;
	struct pollfd *pfd;
	int rc, conn_idx, nonblock;

	rc = accept(server->sockfd, NULL, NULL);
	if (rc >= 0) {
		assert(server->num_conns < SPDK_JSONRPC_MAX_CONNS);
		conn_idx = server->num_conns;
		conn = &server->conns[conn_idx];
		conn->server = server;
		conn->sockfd = rc;
		conn->recv_len = 0;
		conn->send_len = 0;
		conn->json_writer = 0;

		nonblock = 1;
		rc = ioctl(conn->sockfd, FIONBIO, &nonblock);
		if (rc != 0) {
			SPDK_ERRLOG("ioctl(FIONBIO) failed\n");
			close(conn->sockfd);
			return -1;
		}

		/* Add connection to pollfds */
		pfd = &server->pollfds[conn_idx + 1];
		pfd->fd = conn->sockfd;
		pfd->events = POLLIN | POLLOUT;

		server->num_conns++;

		return 0;
	}

	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
		return 0;
	}

	return -1;
}

int
spdk_jsonrpc_server_write_cb(void *cb_ctx, const void *data, size_t size)
{
	struct spdk_jsonrpc_server_conn *conn = cb_ctx;

	if (SPDK_JSONRPC_SEND_BUF_SIZE - conn->send_len < size) {
		SPDK_ERRLOG("Not enough space in send buf\n");
		return -1;
	}

	memcpy(conn->send_buf + conn->send_len, data, size);
	conn->send_len += size;

	return 0;
}

void
spdk_jsonrpc_server_handle_request(struct spdk_jsonrpc_server_conn *conn,
				   const struct spdk_json_val *method, const struct spdk_json_val *params,
				   const struct spdk_json_val *id)
{
	conn->server->handle_request(conn, method, params, id);
}

void
spdk_jsonrpc_server_handle_error(struct spdk_jsonrpc_server_conn *conn, int error,
				 const struct spdk_json_val *method, const struct spdk_json_val *params,
				 const struct spdk_json_val *id)
{
	const char *msg;

	switch (error) {
	case SPDK_JSONRPC_ERROR_PARSE_ERROR:
		msg = "Parse error";
		break;

	case SPDK_JSONRPC_ERROR_INVALID_REQUEST:
		msg = "Invalid request";
		break;

	case SPDK_JSONRPC_ERROR_METHOD_NOT_FOUND:
		msg = "Method not found";
		break;

	case SPDK_JSONRPC_ERROR_INVALID_PARAMS:
		msg = "Invalid parameters";
		break;

	case SPDK_JSONRPC_ERROR_INTERNAL_ERROR:
		msg = "Internal error";
		break;

	default:
		msg = "Error";
		break;
	}

	spdk_jsonrpc_send_error_response(conn, id, error, msg);
}

static int
spdk_jsonrpc_server_conn_recv(struct spdk_jsonrpc_server_conn *conn)
{
	ssize_t rc;
	size_t recv_avail = SPDK_JSONRPC_RECV_BUF_SIZE - conn->recv_len;

	rc = recv(conn->sockfd, conn->recv_buf + conn->recv_len, recv_avail, 0);
	if (rc == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
			return 0;
		}

		SPDK_TRACELOG(SPDK_TRACE_RPC, "recv() failed: %s\n", strerror(errno));
		return -1;
	}

	if (rc == 0) {
		SPDK_TRACELOG(SPDK_TRACE_RPC, "remote closed connection\n");
		return -1;
	}

	conn->recv_len += rc;

	rc = spdk_jsonrpc_parse_request(conn, conn->recv_buf, conn->recv_len);
	if (rc < 0) {
		SPDK_ERRLOG("jsonrpc parse request failed\n");
		return -1;
	}

	if (rc > 0) {
		/*
		 * Successfully parsed a request - move any data past the end of the
		 * parsed request down to the beginning.
		 */
		assert((size_t)rc <= conn->recv_len);
		memmove(conn->recv_buf, conn->recv_buf + rc, conn->recv_len - rc);
		conn->recv_len -= rc;
	}

	return 0;
}

static int
spdk_jsonrpc_server_conn_send(struct spdk_jsonrpc_server_conn *conn)
{
	ssize_t rc;

	rc = send(conn->sockfd, conn->send_buf, conn->send_len, 0);
	if (rc < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
			return 0;
		}

		SPDK_TRACELOG(SPDK_TRACE_RPC, "send() failed: %s\n", strerror(errno));
		return -1;
	}

	if (rc == 0) {
		SPDK_TRACELOG(SPDK_TRACE_RPC, "remote closed connection\n");
		return -1;
	}

	conn->send_len -= rc;

	return 0;
}

int
spdk_jsonrpc_server_poll(struct spdk_jsonrpc_server *server)
{
	int rc, i;
	struct pollfd *pfd;
	struct spdk_jsonrpc_server_conn *conn;

	rc = poll(server->pollfds, server->num_conns + 1, 0);

	if (rc < 0) {
		if (errno == EINTR) {
			return 0;
		}

		SPDK_ERRLOG("jsonrpc poll() failed\n");
		return -1;
	}

	if (rc == 0) {
		/* No sockets are ready */
		return 0;
	}

	/* Check listen socket */
	if (server->num_conns < SPDK_JSONRPC_MAX_CONNS) {
		pfd = &server->pollfds[0];
		if (pfd->revents) {
			spdk_jsonrpc_server_accept(server);
		}
		pfd->revents = 0;
	}

	for (i = 0; i < server->num_conns; i++) {
		pfd = &server->pollfds[i + 1];
		conn = &server->conns[i];
		if (conn->send_len) {
			/*
			 * If there is any data to send, keep sending it until the send buffer
			 *  is empty.  Each response should be allowed the full send buffer, so
			 *  don't accept any new requests until the previous response is sent out.
			 */
			if (pfd->revents & POLLOUT) {
				rc = spdk_jsonrpc_server_conn_send(conn);
				if (rc != 0) {
					SPDK_TRACELOG(SPDK_TRACE_RPC, "closing conn due to send failure\n");
					spdk_jsonrpc_server_conn_remove(conn);
				}
			}
		} else {
			/*
			 * No data to send - we can receive a new request.
			 */
			if (pfd->revents & POLLIN) {
				rc = spdk_jsonrpc_server_conn_recv(conn);
				if (rc != 0) {
					SPDK_TRACELOG(SPDK_TRACE_RPC, "closing conn due to recv failure\n");
					spdk_jsonrpc_server_conn_remove(conn);
				}
			}
		}
		pfd->revents = 0;
	}

	return 0;
}
