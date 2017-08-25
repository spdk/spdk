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
#include "spdk/string.h"

struct spdk_jsonrpc_server *
spdk_jsonrpc_server_listen(int domain, int protocol,
			   struct sockaddr *listen_addr, socklen_t addrlen,
			   spdk_jsonrpc_handle_request_fn handle_request)
{
	struct spdk_jsonrpc_server *server;
	int rc, val;
	char buf[64];

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
		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_ERRLOG("could not bind JSON-RPC server: %s\n", buf);
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
spdk_jsonrpc_server_conn_close(struct spdk_jsonrpc_server_conn *conn)
{
	conn->closed = true;

	if (conn->sockfd >= 0) {
		close(conn->sockfd);
		conn->sockfd = -1;
	}
}

static void
spdk_jsonrpc_server_conn_remove(struct spdk_jsonrpc_server_conn *conn)
{
	struct spdk_jsonrpc_server *server = conn->server;
	int conn_idx = conn - server->conns;

	spdk_jsonrpc_server_conn_close(conn);

	spdk_ring_free(conn->send_queue);

	/* Swap conn with the last entry in conns */
	server->conns[conn_idx] = server->conns[server->num_conns - 1];
	server->num_conns--;
}

static int
spdk_jsonrpc_server_accept(struct spdk_jsonrpc_server *server)
{
	struct spdk_jsonrpc_server_conn *conn;
	int rc, conn_idx, nonblock;

	rc = accept(server->sockfd, NULL, NULL);
	if (rc >= 0) {
		assert(server->num_conns < SPDK_JSONRPC_MAX_CONNS);
		conn_idx = server->num_conns;
		conn = &server->conns[conn_idx];
		conn->server = server;
		conn->sockfd = rc;
		conn->closed = false;
		conn->recv_len = 0;
		conn->outstanding_requests = 0;
		conn->send_request = NULL;
		conn->send_queue = spdk_ring_create(SPDK_RING_TYPE_SP_SC, 128, SPDK_ENV_SOCKET_ID_ANY);
		if (conn->send_queue == NULL) {
			SPDK_ERRLOG("send_queue allocation failed\n");
			close(conn->sockfd);
			return -1;
		}

		nonblock = 1;
		rc = ioctl(conn->sockfd, FIONBIO, &nonblock);
		if (rc != 0) {
			SPDK_ERRLOG("ioctl(FIONBIO) failed\n");
			close(conn->sockfd);
			return -1;
		}

		server->num_conns++;

		return 0;
	}

	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
		return 0;
	}

	return -1;
}

void
spdk_jsonrpc_server_handle_request(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *method, const struct spdk_json_val *params)
{
	request->conn->server->handle_request(request, method, params);
}

void
spdk_jsonrpc_server_handle_error(struct spdk_jsonrpc_request *request, int error)
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

	spdk_jsonrpc_send_error_response(request, error, msg);
}

static int
spdk_jsonrpc_server_conn_recv(struct spdk_jsonrpc_server_conn *conn)
{
	ssize_t rc;
	size_t recv_avail = SPDK_JSONRPC_RECV_BUF_SIZE - conn->recv_len;
	char buf[64];

	rc = recv(conn->sockfd, conn->recv_buf + conn->recv_len, recv_avail, 0);
	if (rc == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
			return 0;
		}
		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_DEBUGLOG(SPDK_TRACE_RPC, "recv() failed: %s\n", buf);
		return -1;
	}

	if (rc == 0) {
		SPDK_DEBUGLOG(SPDK_TRACE_RPC, "remote closed connection\n");
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

void
spdk_jsonrpc_server_send_response(struct spdk_jsonrpc_server_conn *conn,
				  struct spdk_jsonrpc_request *request)
{
	/* Queue the response to be sent */
	spdk_ring_enqueue(conn->send_queue, (void **)&request, 1);
}

static int
spdk_jsonrpc_server_conn_send(struct spdk_jsonrpc_server_conn *conn)
{
	struct spdk_jsonrpc_request *request;
	ssize_t rc;
	char buf[64];

more:
	if (conn->outstanding_requests == 0) {
		return 0;
	}

	if (conn->send_request == NULL) {
		if (spdk_ring_dequeue(conn->send_queue, (void **)&conn->send_request, 1) != 1) {
			return 0;
		}
	}

	request = conn->send_request;
	if (request == NULL) {
		/* Nothing to send right now */
		return 0;
	}

	rc = send(conn->sockfd, request->send_buf + request->send_offset,
		  request->send_len, 0);
	if (rc < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
			return 0;
		}

		spdk_strerror_r(errno, buf, sizeof(buf));
		SPDK_DEBUGLOG(SPDK_TRACE_RPC, "send() failed: %s\n", buf);
		return -1;
	}

	request->send_offset += rc;
	request->send_len -= rc;

	if (request->send_len == 0) {
		/*
		 * Full response has been sent.
		 * Free it and set send_request to NULL to move on to the next queued response.
		 */
		conn->send_request = NULL;
		spdk_jsonrpc_free_request(request);
		goto more;
	}

	return 0;
}

int
spdk_jsonrpc_server_poll(struct spdk_jsonrpc_server *server)
{
	int rc, i;
	struct spdk_jsonrpc_server_conn *conn;

	for (i = 0; i < server->num_conns; i++) {
		conn = &server->conns[i];

		if (conn->closed) {
			struct spdk_jsonrpc_request *request;

			/*
			 * The client closed the connection, but there may still be requests
			 * outstanding; we have no way to cancel outstanding requests, so wait until
			 * each outstanding request sends a response (which will be discarded, since
			 * the connection is closed).
			 */

			if (conn->send_request) {
				spdk_jsonrpc_free_request(conn->send_request);
				conn->send_request = NULL;
			}

			while (spdk_ring_dequeue(conn->send_queue, (void **)&request, 1) == 1) {
				spdk_jsonrpc_free_request(request);
			}

			if (conn->outstanding_requests == 0) {
				SPDK_DEBUGLOG(SPDK_TRACE_RPC, "all outstanding requests completed\n");
				spdk_jsonrpc_server_conn_remove(conn);
			}
		}
	}

	/* Check listen socket */
	if (server->num_conns < SPDK_JSONRPC_MAX_CONNS) {
		spdk_jsonrpc_server_accept(server);
	}

	for (i = 0; i < server->num_conns; i++) {
		conn = &server->conns[i];

		if (conn->closed) {
			continue;
		}

		rc = spdk_jsonrpc_server_conn_send(conn);
		if (rc != 0) {
			spdk_jsonrpc_server_conn_close(conn);
			continue;
		}

		rc = spdk_jsonrpc_server_conn_recv(conn);
		if (rc != 0) {
			spdk_jsonrpc_server_conn_close(conn);
			continue;
		}
	}

	return 0;
}
