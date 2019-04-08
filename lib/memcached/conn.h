/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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

#ifndef SPDK_MEMCACHED_CONN_H_
#define SPDK_MEMCACHED_CONN_H_

#include "spdk/stdinc.h"

#include "spdk/queue.h"
#include "spdk/cpuset.h"

#include "memcached/memcached.h"
#include "memcached/recv_buf.h"

#define MAX_MEMCACHED_CONNECTIONS 1024

#define MAX_ADDRBUF 64
#define MAX_INITIATOR_ADDR (MAX_ADDRBUF)
#define MAX_TARGET_ADDR (MAX_ADDRBUF)

struct spdk_poller;

struct spdk_memcached_conn {
	int				id;
	int				is_valid;

	/*
	 * All fields below this point are reinitialized each time the
	 *  connection object is allocated.  Make sure to update the
	 *  SPDK_memcached_CONNECTION_MEMSET() macro if changing which fields
	 *  are initialized when allocated.
	 */
	struct spdk_memcached_portal	*portal;
	int				pg_tag;
	char				*portal_host;
	char				*portal_port;
	struct spdk_cpuset		*portal_cpumask;
	uint32_t			lcore;
	struct spdk_sock		*sock;

	/* IP address */
	char initiator_addr[MAX_INITIATOR_ADDR];
	char target_addr[MAX_TARGET_ADDR];

	enum memcached_connection_state	state;
	struct spdk_thread *thd; /* in which thd the conn is */
	struct spdk_memcached_tgt_node	*target;

	struct spdk_memcached_cmd *cmd_in_recv;
	struct spdk_memcached_conn_recv_buf recv_buf;
	TAILQ_HEAD(, spdk_memcached_cmd) write_cmd_list;

	/* Timer used to destroy connection after logout if initiator does
	 *  not close the connection.
	 */
	struct spdk_poller *logout_timer;

	/* Timer used to wait for connection to close
	 */
	struct spdk_poller *shutdown_timer;

	struct memcached_param *params;

	int timeout;

	STAILQ_ENTRY(spdk_memcached_conn) link;
	struct spdk_poller	*flush_poller;
	bool			is_stopped;  /* Set true when connection is stopped for migration */

	// can be removed
	/* Initiator/Target port binds */
	char				initiator_name[MAX_INITIATOR_NAME];
	char				target_short_name[MAX_TARGET_NAME];
};

extern struct spdk_memcached_conn *g_conns_array;

int spdk_memcached_initialze_conns(void); // called by memcached subsystem
void spdk_shutdown_memcached_conns(void); // called by memcached subsystem

int spdk_memcached_conn_construct(struct spdk_memcached_portal *portal,
				  struct spdk_sock *sock); // called by acceptor if client requests conn; doing spdk_memcached_conn_handle_incoming_pdus and flush_pdu
void spdk_memcached_conn_destruct(struct spdk_memcached_conn
				  *conn); // called by memcached subsystem if find one conn is in exiting
void spdk_memcached_conn_handle_nop(struct spdk_memcached_conn
				    *conn); // called by iscsi for nop, maybe not useful for memcached
//void spdk_memcached_conn_logout(struct spdk_memcached_conn *conn);
//int spdk_memcached_drop_conns(struct spdk_memcached_conn *conn,
//			  const char *conn_match, int drop_all);
//void spdk_memcached_conn_migration(struct spdk_memcached_conn *conn);
void spdk_memcached_conn_set_min_per_core(int count);
int spdk_memcached_conn_get_min_per_core(void);

int spdk_memcached_conn_flush_cmds(void *conn);
int spdk_memcached_conn_read_data(struct spdk_memcached_conn *conn, int len, void *buf);
int spdk_memcached_conn_readv_data(struct spdk_memcached_conn *conn,
				   struct iovec *iov, int iovcnt);
void spdk_memcached_conn_write_cmd(struct spdk_memcached_conn *conn,
				   struct spdk_memcached_cmd *cmd);

void spdk_memcached_conn_free_cmd(struct spdk_memcached_conn *conn, struct spdk_memcached_cmd *cmd);

#endif /* SPDK_CONN_H_ */
