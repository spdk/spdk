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

#include "spdk/ftp.h"
#include "spdk/sock.h"
#include "ftp_internal.h"
#include "spdk_internal/log.h"
#include "spdk/assert.h"
#include "spdk/util.h"


#define FTP_TCP_MAX_ACCEPT_SOCK_ONE_TIME 16

struct spdk_ftp_tcp_poll_group {
	struct spdk_ftp_server_poll_group group;
	struct spdk_sock_group *sock_group;

	TAILQ_HEAD(, spdk_ftp_tcp_conn)  conns;

};
struct spdk_ftp_tcp_port {
	struct spdk_sock			*listen_sock;
	uint32_t				ref;
	TAILQ_ENTRY(spdk_ftp_tcp_port)		link;
};

struct spdk_ftp_tcp_server {
	struct spdk_ftp_server		server;

	pthread_mutex_t				lock;

	TAILQ_HEAD(, spdk_ftp_tcp_port)	ports;
};
struct spdk_ftp_tcp_conn {
	struct spdk_ftp_conn conn;
	struct spdk_ftp_tcp_poll_group *group;
	struct spdk_ftp_tcp_port *port;
	struct spdk_sock			*sock;
	/* IP address */
	char					initiator_addr[SPDK_FTP_TRADDR_MAX_LEN];
	char					target_addr[SPDK_FTP_TRADDR_MAX_LEN];

	/* IP port */
	uint16_t				initiator_port;
	uint16_t				target_port;

	TAILQ_ENTRY(spdk_ftp_tcp_conn)	link;
};
#define SPDK_FTP_TCP_DEFAULT_IO_UNIT_SIZE 131072
#define SPDK_FTP_TCP_DEFAULT_MAX_IO_SIZE 131072
#define FTPD_CONNECT_TIMEOUT_S 60
#define FTPD_IDLE_SESSION_TIMEOUT_S 300
static void
spdk_ftp_tcp_opts_init(struct spdk_ftp_server_opts *opts)
{
	printf("tcp init done\n");
	opts->io_unit_size = SPDK_FTP_TCP_DEFAULT_IO_UNIT_SIZE;
	opts->max_io_size = SPDK_FTP_TCP_DEFAULT_MAX_IO_SIZE;
	opts->spdk_ftpd_anonymous_enable = 1;
	opts->spdk_ftpd_pasv_enable = 1;
	opts->spdk_ftpd_connect_timeout = FTPD_CONNECT_TIMEOUT_S;
	opts->spdk_ftpd_idle_session_timeout = FTPD_IDLE_SESSION_TIMEOUT_S;

}
static struct spdk_ftp_server *
spdk_ftp_tcp_create(struct spdk_ftp_server_opts *opts)
{
	struct spdk_ftp_tcp_server *tserver;

	tserver = calloc(1, sizeof(*tserver));
	if (!tserver) {
		return NULL;
	}

	TAILQ_INIT(&tserver->ports);

	tserver->server.ops = &spdk_ftp_server_tcp;

	SPDK_NOTICELOG("*** FTP TCP Server Init ***\n");

	pthread_mutex_init(&tserver->lock, NULL);
	return &tserver->server;

}
static struct spdk_ftp_server_poll_group *
spdk_ftp_tcp_poll_group_create(struct spdk_ftp_server *server)
{

	struct spdk_ftp_tcp_poll_group *tgroup;
	tgroup = calloc(1, sizeof(*tgroup));

	if (!tgroup) {
		return NULL;
	}

	tgroup->sock_group = spdk_sock_group_create(&tgroup->group);
	if (!tgroup->sock_group) {
		goto cleanup;
	}

	// RUI TODO:
	// need to init qpairs and pending_data_buf_queue
	TAILQ_INIT(&tgroup->conns);
	return &tgroup->group;
cleanup:
	free(tgroup);
	return NULL;


}

static int
spdk_ftp_tcp_destroy(struct spdk_ftp_server *server)
{
	struct spdk_ftp_tcp_server	*tserver;

	assert(server != NULL);
	tserver = SPDK_CONTAINEROF(server, struct spdk_ftp_tcp_server, server);

	pthread_mutex_destroy(&tserver->lock);
	free(tserver);
	return 0;
}
static int
spdk_ftp_tcp_poll_group_poll(struct spdk_ftp_server_poll_group *group)
{

	/* struct spdk_ftp_tcp_poll_group *tgroup;
	int rc; */
	return 0;

}

static int
spdk_ftp_tcp_listen(struct spdk_ftp_server *server)
{
	struct spdk_ftp_tcp_server *tserver;
	struct spdk_ftp_tcp_port *port;

	tserver = SPDK_CONTAINEROF(server, struct spdk_ftp_tcp_server, server);

	pthread_mutex_lock(&tserver->lock);

	// RUI TODO: find existing port

	port = calloc(1, sizeof(*port));

	if (!port) {
		SPDK_ERRLOG("Port allocation failed\n");
		free(port);
		pthread_mutex_unlock(&tserver->lock);
		return -ENOMEM;
	}

	port->ref = 1;

	port->listen_sock = spdk_sock_listen(server->opts.ipaddr, server->opts.spdk_ftpd_listen_port);

	if (port->listen_sock == NULL) {
		SPDK_ERRLOG("spdk_sock_listen failed:\n");
		free(port);
		pthread_mutex_unlock(&tserver->lock);
		return -errno;
	}

	SPDK_NOTICELOG("*** FTP-TCP Target Listening on %s port %d ***\n",
		       server->opts.ipaddr, server->opts.spdk_ftpd_listen_port);
	TAILQ_INSERT_TAIL(&tserver->ports, port, link);
	pthread_mutex_unlock(&tserver->lock);

	return 0;
}
static void
spdk_ftp_tcp_conn_destroy(struct spdk_ftp_tcp_conn *tconn)
{
	// int err=0;

	SPDK_DEBUGLOG(SPDK_LOG_FTP_TCP, "enter\n");
	spdk_sock_close(&tconn->sock);
	free(tconn);
	SPDK_DEBUGLOG(SPDK_LOG_FTP_TCP, "Leave\n");
}

static void
_spdk_ftp_tcp_handle_connect(struct spdk_ftp_server *server,
			     struct spdk_ftp_tcp_port *port,
			     struct spdk_sock *sock, new_conn_fn cb_fn)
{
	struct spdk_ftp_tcp_conn *tconn;
	int rc;

	SPDK_DEBUGLOG(SPDK_LOG_FTP_TCP, "New connection accepted on port %d\n",
		      server->opts.spdk_ftpd_listen_port);

	// 判断sock_priority
	tconn = calloc(1, sizeof(struct spdk_ftp_tcp_conn));
	if (tconn == NULL) {
		SPDK_ERRLOG("Could not allocate new connection.\n");
		spdk_sock_close(&sock);
		return;
	}
	tconn->sock = sock;
	// RUI TODO: init other para

	rc = spdk_sock_getaddr(tconn->sock, tconn->target_addr,
			       sizeof(tconn->target_addr), &tconn->target_port,
			       tconn->initiator_addr, sizeof(tconn->initiator_addr),
			       &tconn->initiator_port);
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_getaddr() failed of tconn=%p\n", tconn);
		spdk_ftp_tcp_conn_destroy(tconn);
		return;
	}

	cb_fn(&tconn->conn);

}

static void
spdk_ftp_tcp_port_accept(struct spdk_ftp_server *server, struct spdk_ftp_tcp_port *port,
			 new_conn_fn cb_fn)
{
	struct spdk_sock *sock;
	int i;

	for (i = 0; i < FTP_TCP_MAX_ACCEPT_SOCK_ONE_TIME; i++) {
		sock = spdk_sock_accept(port->listen_sock);
		if (sock) {
			printf("sock accepted\n");
			_spdk_ftp_tcp_handle_connect(server, port, sock, cb_fn);
		}
	}
}


static void
spdk_ftp_tcp_accept(struct spdk_ftp_server *server, new_conn_fn cb_fn)
{
	struct spdk_ftp_tcp_server *tserver;
	struct spdk_ftp_tcp_port *port;

	tserver = SPDK_CONTAINEROF(server, struct spdk_ftp_tcp_server, server);

	TAILQ_FOREACH(port, &tserver->ports, link) {
		spdk_ftp_tcp_port_accept(server, port, cb_fn);
	}
}
static int
spdk_ftp_tcp_sock_process(struct spdk_ftp_tcp_conn *tconn)
{
	return 0;
}
static void
spdk_ftp_tcp_sock_cb(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_ftp_tcp_conn *tconn = arg;
	int rc;

	assert(tconn != NULL);
	rc = spdk_ftp_tcp_sock_process(tconn);

	/* check the following two factors:
	 * rc: The socket is closed
	 * State of tqpair: The tqpair is in EXITING state due to internal error
	 */
	if (rc < 0) {
		spdk_ftp_conn_disconnect(&tconn->conn, NULL, NULL);
	}

}
static int
spdk_ftp_tcp_poll_group_add(struct spdk_ftp_server_poll_group *group,
			    struct spdk_ftp_conn *conn)
{
	struct spdk_ftp_tcp_poll_group *tgroup;
	struct spdk_ftp_tcp_conn *tconn;
	int rc;

	tgroup = SPDK_CONTAINEROF(group, struct spdk_ftp_tcp_poll_group, group);
	tconn = SPDK_CONTAINEROF(conn, struct spdk_ftp_tcp_conn, conn);

	rc = spdk_sock_group_add_sock(tgroup->sock_group, tconn->sock, spdk_ftp_tcp_sock_cb, tconn);
	if (rc != 0) {
		SPDK_ERRLOG("Could not add sock to sock_group\n");
		spdk_ftp_tcp_conn_destroy(tconn);
		return -1;
	}

	// RUI TODO : init other things

	tconn->group = tgroup;
	TAILQ_INSERT_TAIL(&tgroup->conns, tconn, link);

	return 0;



}
const struct spdk_ftp_server_ops spdk_ftp_server_tcp = {
	.type = SPDK_FTP_TCP,
	.opts_init = spdk_ftp_tcp_opts_init,
	.create = spdk_ftp_tcp_create,
	.destroy = spdk_ftp_tcp_destroy,

	.listen = spdk_ftp_tcp_listen,
	.accept = spdk_ftp_tcp_accept,



	.poll_group_create = spdk_ftp_tcp_poll_group_create,
	.poll_group_add = spdk_ftp_tcp_poll_group_add,
	.poll_group_poll = spdk_ftp_tcp_poll_group_poll,
};

SPDK_LOG_REGISTER_COMPONENT("ftp_tcp", SPDK_LOG_FTP_TCP)
