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

#ifndef SPDK_ISCSI_CONN_H
#define SPDK_ISCSI_CONN_H

#include "spdk/stdinc.h"

#include "iscsi/iscsi.h"
#include "spdk/queue.h"
#include "spdk/event.h"

/*
 * MAX_CONNECTION_PARAMS: The numbers of the params in conn_param_table
 * MAX_SESSION_PARAMS: The numbers of the params in sess_param_table
 */
#define MAX_CONNECTION_PARAMS 14
#define MAX_SESSION_PARAMS 19

#define MAX_ADDRBUF 64
#define MAX_INITIATOR_ADDR (MAX_ADDRBUF)
#define MAX_TARGET_ADDR (MAX_ADDRBUF)

#define OWNER_ISCSI_CONN		0x1

#define OBJECT_ISCSI_PDU		0x1

#define TRACE_GROUP_ISCSI		0x1
#define TRACE_READ_FROM_SOCKET_DONE	SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x0)
#define TRACE_FLUSH_WRITEBUF_START	SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x1)
#define TRACE_FLUSH_WRITEBUF_DONE	SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x2)
#define TRACE_READ_PDU			SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x3)
#define TRACE_ISCSI_TASK_DONE		SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x4)
#define TRACE_ISCSI_TASK_QUEUE		SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x5)
#define TRACE_ISCSI_CONN_ACTIVE		SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x6)
#define TRACE_ISCSI_CONN_IDLE		SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x7)

struct spdk_iscsi_conn {
	int				id;
	int				is_valid;
	int				is_idle;
	/*
	 * All fields below this point are reinitialized each time the
	 *  connection object is allocated.  Make sure to update the
	 *  SPDK_ISCSI_CONNECTION_MEMSET() macro if changing which fields
	 *  are initialized when allocated.
	 */
	struct spdk_iscsi_portal		*portal;
	uint32_t			lcore;
	int				sock;
	struct spdk_iscsi_sess	*sess;

	enum iscsi_connection_state	state;
	int				login_phase;

	uint64_t	last_flush;
	uint64_t	last_fill;
	uint64_t	last_nopin;

	/* Timer used to destroy connection after logout if initiator does
	 *  not close the connection.
	 */
	struct spdk_poller *logout_timer;

	/* Timer used to wait for connection to close
	 */
	struct spdk_poller *shutdown_timer;

	struct spdk_iscsi_pdu *pdu_in_progress;

	TAILQ_HEAD(, spdk_iscsi_pdu) write_pdu_list;
	TAILQ_HEAD(, spdk_iscsi_pdu) snack_pdu_list;

	int pending_r2t;
	struct spdk_iscsi_task *outstanding_r2t_tasks[DEFAULT_MAXR2T];

	uint16_t cid;

	/* IP address */
	char initiator_addr[MAX_INITIATOR_ADDR];
	char target_addr[MAX_TARGET_ADDR];

	/* Initiator/Target port binds */
	char				initiator_name[MAX_INITIATOR_NAME];
	struct spdk_scsi_port		*initiator_port;
	char				target_short_name[MAX_TARGET_NAME];
	struct spdk_scsi_port		*target_port;
	struct spdk_iscsi_tgt_node	*target;
	struct spdk_scsi_dev		*dev;

	/* for fast access */
	int header_digest;
	int data_digest;
	int full_feature;

	struct iscsi_param *params;
	bool sess_param_state_negotiated[MAX_SESSION_PARAMS];
	bool conn_param_state_negotiated[MAX_CONNECTION_PARAMS];
	struct iscsi_chap_auth auth;
	int authenticated;
	int req_auth;
	int req_mutual;
	uint64_t last_activity_tsc;
	uint32_t pending_task_cnt;
	uint32_t data_out_cnt;
	uint32_t data_in_cnt;
	bool pending_activate_event;

	int timeout;
	uint64_t nopininterval;
	bool nop_outstanding;

	/*
	 * This is the maximum data segment length that iscsi target can send
	 *  to the initiator on this connection.  Not to be confused with the
	 *  maximum data segment length that initiators can send to iscsi target, which
	 *  is statically defined as SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH.
	 */
	int MaxRecvDataSegmentLength;

	uint32_t StatSN;
	uint32_t exp_statsn;
	uint32_t ttt; /* target transfer tag */
	char *partial_text_parameter;

	STAILQ_ENTRY(spdk_iscsi_conn) link;
	struct spdk_poller	*poller;
	TAILQ_HEAD(queued_r2t_tasks, spdk_iscsi_task)	queued_r2t_tasks;
	TAILQ_HEAD(active_r2t_tasks, spdk_iscsi_task)	active_r2t_tasks;
	TAILQ_HEAD(queued_datain_tasks, spdk_iscsi_task)	queued_datain_tasks;
};

extern struct spdk_iscsi_conn *g_conns_array;

int spdk_initialize_iscsi_conns(void);
void spdk_shutdown_iscsi_conns(void);

int spdk_iscsi_conn_construct(struct spdk_iscsi_portal *portal, int sock);
void spdk_iscsi_conn_destruct(struct spdk_iscsi_conn *conn);
void spdk_iscsi_conn_logout(struct spdk_iscsi_conn *conn);
int spdk_iscsi_drop_conns(struct spdk_iscsi_conn *conn,
			  const char *conn_match, int drop_all);
void spdk_iscsi_conn_set_min_per_core(int count);
void spdk_iscsi_set_min_conn_idle_interval(int interval_in_us);

int spdk_iscsi_conn_read_data(struct spdk_iscsi_conn *conn, int len,
			      void *buf);

void spdk_iscsi_conn_free_pdu(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu);

#endif /* SPDK_ISCSI_CONN_H */
