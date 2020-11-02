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
#include "spdk/cpuset.h"
#include "spdk/scsi.h"

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
#define TRACE_ISCSI_READ_FROM_SOCKET_DONE	SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x0)
#define TRACE_ISCSI_FLUSH_WRITEBUF_START	SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x1)
#define TRACE_ISCSI_FLUSH_WRITEBUF_DONE		SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x2)
#define TRACE_ISCSI_READ_PDU			SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x3)
#define TRACE_ISCSI_TASK_DONE			SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x4)
#define TRACE_ISCSI_TASK_QUEUE			SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x5)
#define TRACE_ISCSI_TASK_EXECUTED		SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x6)
#define TRACE_ISCSI_PDU_COMPLETED		SPDK_TPOINT_ID(TRACE_GROUP_ISCSI, 0x7)

enum iscsi_pdu_recv_state {
	/* Ready to wait for PDU */
	ISCSI_PDU_RECV_STATE_AWAIT_PDU_READY,

	/* Active connection waiting for any PDU header */
	ISCSI_PDU_RECV_STATE_AWAIT_PDU_HDR,

	/* Active connection waiting for payload */
	ISCSI_PDU_RECV_STATE_AWAIT_PDU_PAYLOAD,

	/* Active connection does not wait for payload */
	ISCSI_PDU_RECV_STATE_ERROR,
};

struct spdk_poller;
struct spdk_iscsi_conn;

struct spdk_iscsi_lun {
	struct spdk_iscsi_conn		*conn;
	struct spdk_scsi_lun		*lun;
	struct spdk_scsi_lun_desc	*desc;
	struct spdk_poller		*remove_poller;
};

struct spdk_iscsi_conn {
	int				id;
	int				is_valid;
	/*
	 * All fields below this point are reinitialized each time the
	 *  connection object is allocated.  Make sure to update the
	 *  SPDK_ISCSI_CONNECTION_MEMSET() macro if changing which fields
	 *  are initialized when allocated.
	 */
	struct spdk_iscsi_portal	*portal;
	int				pg_tag;
	char				portal_host[MAX_PORTAL_ADDR + 1];
	char				portal_port[MAX_PORTAL_ADDR + 1];
	struct spdk_iscsi_poll_group	*pg;
	struct spdk_sock		*sock;
	struct spdk_iscsi_sess		*sess;

	enum iscsi_connection_state	state;
	int				login_phase;
	bool				is_logged_out;
	struct spdk_iscsi_pdu		*login_rsp_pdu;

	uint64_t	last_flush;
	uint64_t	last_fill;
	uint64_t	last_nopin;

	/* Timer used to destroy connection after requesting logout if
	 *  initiator does not send logout request.
	 */
	struct spdk_poller *logout_request_timer;

	/* Timer used to destroy connection after logout if initiator does
	 *  not close the connection.
	 */
	struct spdk_poller *logout_timer;

	/* Timer used to wait for connection to close
	 */
	struct spdk_poller *shutdown_timer;

	/* Timer used to destroy connection after creating this connection
	 *  if login process does not complete.
	 */
	struct spdk_poller *login_timer;

	struct spdk_iscsi_pdu *pdu_in_progress;
	enum iscsi_pdu_recv_state pdu_recv_state;

	TAILQ_HEAD(, spdk_iscsi_pdu) write_pdu_list;
	TAILQ_HEAD(, spdk_iscsi_pdu) snack_pdu_list;

	uint32_t pending_r2t;

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

	/* To handle the case that SendTargets response is split into
	 * multiple PDUs due to very small MaxRecvDataSegmentLength.
	 */
	uint32_t			send_tgt_completed_size;
	struct iscsi_param		*params_text;

	/* for fast access */
	int header_digest;
	int data_digest;
	int full_feature;

	struct iscsi_param *params;
	bool sess_param_state_negotiated[MAX_SESSION_PARAMS];
	bool conn_param_state_negotiated[MAX_CONNECTION_PARAMS];
	struct iscsi_chap_auth auth;
	bool authenticated;
	bool disable_chap;
	bool require_chap;
	bool mutual_chap;
	int32_t chap_group;
	uint32_t pending_task_cnt;
	uint32_t data_out_cnt;
	uint32_t data_in_cnt;

	uint64_t timeout;
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

	STAILQ_ENTRY(spdk_iscsi_conn) pg_link;
	bool			is_stopped;  /* Set true when connection is stopped for migration */
	TAILQ_HEAD(queued_r2t_tasks, spdk_iscsi_task)	queued_r2t_tasks;
	TAILQ_HEAD(active_r2t_tasks, spdk_iscsi_task)	active_r2t_tasks;
	TAILQ_HEAD(queued_datain_tasks, spdk_iscsi_task)	queued_datain_tasks;

	struct spdk_iscsi_lun	*luns[SPDK_SCSI_DEV_MAX_LUN];

	TAILQ_ENTRY(spdk_iscsi_conn)	conn_link;
};

void iscsi_task_cpl(struct spdk_scsi_task *scsi_task);
void iscsi_task_mgmt_cpl(struct spdk_scsi_task *scsi_task);

int initialize_iscsi_conns(void);
void shutdown_iscsi_conns(void);
void iscsi_conns_request_logout(struct spdk_iscsi_tgt_node *target, int pg_tag);
int iscsi_get_active_conns(struct spdk_iscsi_tgt_node *target);

int iscsi_conn_construct(struct spdk_iscsi_portal *portal, struct spdk_sock *sock);
void iscsi_conn_destruct(struct spdk_iscsi_conn *conn);
void iscsi_conn_handle_nop(struct spdk_iscsi_conn *conn);
void iscsi_conn_schedule(struct spdk_iscsi_conn *conn);
void iscsi_conn_logout(struct spdk_iscsi_conn *conn);
int iscsi_drop_conns(struct spdk_iscsi_conn *conn,
		     const char *conn_match, int drop_all);
int iscsi_conn_handle_queued_datain_tasks(struct spdk_iscsi_conn *conn);
int iscsi_conn_abort_queued_datain_task(struct spdk_iscsi_conn *conn,
					uint32_t ref_task_tag);
int iscsi_conn_abort_queued_datain_tasks(struct spdk_iscsi_conn *conn,
		struct spdk_scsi_lun *lun,
		struct spdk_iscsi_pdu *pdu);

int iscsi_conn_read_data(struct spdk_iscsi_conn *conn, int len, void *buf);
int iscsi_conn_readv_data(struct spdk_iscsi_conn *conn,
			  struct iovec *iov, int iovcnt);
void iscsi_conn_write_pdu(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu,
			  iscsi_conn_xfer_complete_cb cb_fn,
			  void *cb_arg);

void iscsi_conn_free_pdu(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu);

void iscsi_conn_info_json(struct spdk_json_write_ctx *w, struct spdk_iscsi_conn *conn);
void iscsi_conn_pdu_generic_complete(void *cb_arg);
#endif /* SPDK_ISCSI_CONN_H */
