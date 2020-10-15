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

#ifndef SPDK_ISCSI_H
#define SPDK_ISCSI_H

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/iscsi_spec.h"
#include "spdk/thread.h"
#include "spdk/sock.h"

#include "spdk/scsi.h"
#include "iscsi/param.h"

#include "spdk/assert.h"
#include "spdk/dif.h"
#include "spdk/util.h"

#define SPDK_ISCSI_DEFAULT_NODEBASE "iqn.2016-06.io.spdk"

#define DEFAULT_MAXR2T 4
#define MAX_INITIATOR_PORT_NAME 256
#define MAX_INITIATOR_NAME 223
#define MAX_TARGET_NAME 223

#define MAX_PORTAL 1024
#define MAX_INITIATOR 256
#define MAX_NETMASK 256
#define MAX_ISCSI_CONNECTIONS 1024
#define MAX_PORTAL_ADDR 256
#define MAX_PORTAL_PORT 32

#define DEFAULT_PORT 3260
#define DEFAULT_MAX_SESSIONS 128
#define DEFAULT_MAX_CONNECTIONS_PER_SESSION 2
#define DEFAULT_MAXOUTSTANDINGR2T 1
#define DEFAULT_DEFAULTTIME2WAIT 2
#define DEFAULT_DEFAULTTIME2RETAIN 20
#define DEFAULT_INITIALR2T true
#define DEFAULT_IMMEDIATEDATA true
#define DEFAULT_DATAPDUINORDER true
#define DEFAULT_DATASEQUENCEINORDER true
#define DEFAULT_ERRORRECOVERYLEVEL 0
#define DEFAULT_TIMEOUT 60
#define MAX_NOPININTERVAL 60
#define DEFAULT_NOPININTERVAL 30

/*
 * SPDK iSCSI target currently only supports 64KB as the maximum data segment length
 *  it can receive from initiators.  Other values may work, but no guarantees.
 */
#define SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH  65536

/*
 * Defines maximum number of data out buffers each connection can have in
 *  use at any given time.
 */
#define MAX_DATA_OUT_PER_CONNECTION 16

/*
 * Defines default maximum number of data in buffers each connection can have in
 *  use at any given time. So this limit does not affect I/O smaller than
 *  SPDK_BDEV_SMALL_BUF_MAX_SIZE.
 */
#define DEFAULT_MAX_LARGE_DATAIN_PER_CONNECTION 64

#define SPDK_ISCSI_MAX_BURST_LENGTH	\
		(SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH * MAX_DATA_OUT_PER_CONNECTION)

/*
 * Defines default maximum amount in bytes of unsolicited data the iSCSI
 *  initiator may send to the SPDK iSCSI target during the execution of
 *  a single SCSI command. And it is smaller than the MaxBurstLength.
 */
#define SPDK_ISCSI_FIRST_BURST_LENGTH	8192

/*
 * Defines minimum amount in bytes of unsolicited data the iSCSI initiator
 *  may send to the SPDK iSCSI target during the execution of a single
 *  SCSI command.
 */
#define SPDK_ISCSI_MIN_FIRST_BURST_LENGTH	512

#define SPDK_ISCSI_MAX_FIRST_BURST_LENGTH	16777215

/*
 * Defines default maximum queue depth per connection and this can be
 * changed by configuration file.
 */
#define DEFAULT_MAX_QUEUE_DEPTH	64

/** Defines how long we should wait for a logout request when the target
 *   requests logout to the initiator asynchronously.
 */
#define ISCSI_LOGOUT_REQUEST_TIMEOUT 30 /* in seconds */

/** Defines how long we should wait for a TCP close after responding to a
 *   logout request, before terminating the connection ourselves.
 */
#define ISCSI_LOGOUT_TIMEOUT 5 /* in seconds */

/** Defines how long we should wait until login process completes. */
#define ISCSI_LOGIN_TIMEOUT 30 /* in seconds */

/* For spdk_iscsi_login_in related function use, we need to avoid the conflict
 * with other errors
 * */
#define SPDK_ISCSI_LOGIN_ERROR_RESPONSE -1000
#define SPDK_ISCSI_LOGIN_ERROR_PARAMETER -1001
#define SPDK_ISCSI_PARAMETER_EXCHANGE_NOT_ONCE -1002

#define ISCSI_AHS_LEN 60

struct spdk_mobj {
	struct spdk_mempool *mp;
	void *buf;
};

/*
 * Maximum number of SGL elements, i.e.,
 * BHS, AHS, Header Digest, Data Segment and Data Digest.
 */
#define SPDK_ISCSI_MAX_SGL_DESCRIPTORS	(5)

typedef void (*iscsi_conn_xfer_complete_cb)(void *cb_arg);

struct spdk_iscsi_pdu {
	struct iscsi_bhs bhs;
	struct spdk_mobj *mobj;
	bool is_rejected;
	uint8_t *data_buf;
	uint8_t *data;
	uint8_t header_digest[ISCSI_DIGEST_LEN];
	uint8_t data_digest[ISCSI_DIGEST_LEN];
	size_t data_segment_len;
	int bhs_valid_bytes;
	int ahs_valid_bytes;
	uint32_t data_valid_bytes;
	int hdigest_valid_bytes;
	int ddigest_valid_bytes;
	int ref;
	bool data_from_mempool;  /* indicate whether the data buffer is allocated from mempool */
	struct spdk_iscsi_task *task; /* data tied to a task buffer */
	uint32_t cmd_sn;
	uint32_t writev_offset;
	uint32_t data_buf_len;
	bool dif_insert_or_strip;
	struct spdk_dif_ctx dif_ctx;
	struct spdk_iscsi_conn *conn;

	iscsi_conn_xfer_complete_cb		cb_fn;
	void					*cb_arg;

	/* The sock request ends with a 0 length iovec. Place the actual iovec immediately
	 * after it. There is a static assert below to check if the compiler inserted
	 * any unwanted padding */
	int32_t						mapped_length;
	struct spdk_sock_request			sock_req;
	struct iovec					iov[SPDK_ISCSI_MAX_SGL_DESCRIPTORS];
	TAILQ_ENTRY(spdk_iscsi_pdu)	tailq;


	/*
	 * 60 bytes of AHS should suffice for now.
	 * This should always be at the end of PDU data structure.
	 * we need to not zero this out when doing memory clear.
	 */
	uint8_t ahs[ISCSI_AHS_LEN];

	struct {
		uint16_t length; /* iSCSI SenseLength (big-endian) */
		uint8_t data[32];
	} sense;
};
SPDK_STATIC_ASSERT(offsetof(struct spdk_iscsi_pdu,
			    sock_req) + sizeof(struct spdk_sock_request) == offsetof(struct spdk_iscsi_pdu, iov),
		   "Compiler inserted padding between iov and sock_req");

enum iscsi_connection_state {
	ISCSI_CONN_STATE_INVALID = 0,
	ISCSI_CONN_STATE_RUNNING = 1,
	ISCSI_CONN_STATE_EXITING = 2,
	ISCSI_CONN_STATE_EXITED = 3,
};

enum iscsi_chap_phase {
	ISCSI_CHAP_PHASE_NONE = 0,
	ISCSI_CHAP_PHASE_WAIT_A = 1,
	ISCSI_CHAP_PHASE_WAIT_NR = 2,
	ISCSI_CHAP_PHASE_END = 3,
};

enum session_type {
	SESSION_TYPE_INVALID = 0,
	SESSION_TYPE_NORMAL = 1,
	SESSION_TYPE_DISCOVERY = 2,
};

#define ISCSI_CHAP_CHALLENGE_LEN	1024
#define ISCSI_CHAP_MAX_USER_LEN		255
#define ISCSI_CHAP_MAX_SECRET_LEN	255

struct iscsi_chap_auth {
	enum iscsi_chap_phase chap_phase;

	char user[ISCSI_CHAP_MAX_USER_LEN + 1];
	char secret[ISCSI_CHAP_MAX_SECRET_LEN + 1];
	char muser[ISCSI_CHAP_MAX_USER_LEN + 1];
	char msecret[ISCSI_CHAP_MAX_SECRET_LEN + 1];

	uint8_t chap_id[1];
	uint8_t chap_mid[1];
	int chap_challenge_len;
	uint8_t chap_challenge[ISCSI_CHAP_CHALLENGE_LEN];
	int chap_mchallenge_len;
	uint8_t chap_mchallenge[ISCSI_CHAP_CHALLENGE_LEN];
};

struct spdk_iscsi_auth_secret {
	char user[ISCSI_CHAP_MAX_USER_LEN + 1];
	char secret[ISCSI_CHAP_MAX_SECRET_LEN + 1];
	char muser[ISCSI_CHAP_MAX_USER_LEN + 1];
	char msecret[ISCSI_CHAP_MAX_SECRET_LEN + 1];
	TAILQ_ENTRY(spdk_iscsi_auth_secret) tailq;
};

struct spdk_iscsi_auth_group {
	int32_t tag;
	TAILQ_HEAD(, spdk_iscsi_auth_secret) secret_head;
	TAILQ_ENTRY(spdk_iscsi_auth_group) tailq;
};

struct spdk_iscsi_sess {
	uint32_t connections;
	struct spdk_iscsi_conn **conns;

	struct spdk_scsi_port *initiator_port;
	int tag;

	uint64_t isid;
	uint16_t tsih;
	struct spdk_iscsi_tgt_node *target;
	int queue_depth;

	struct iscsi_param *params;

	enum session_type session_type;
	uint32_t MaxConnections;
	uint32_t MaxOutstandingR2T;
	uint32_t DefaultTime2Wait;
	uint32_t DefaultTime2Retain;
	uint32_t FirstBurstLength;
	uint32_t MaxBurstLength;
	bool InitialR2T;
	bool ImmediateData;
	bool DataPDUInOrder;
	bool DataSequenceInOrder;
	uint32_t ErrorRecoveryLevel;

	uint32_t ExpCmdSN;
	uint32_t MaxCmdSN;

	uint32_t current_text_itt;
};

struct spdk_iscsi_poll_group {
	struct spdk_poller				*poller;
	struct spdk_poller				*nop_poller;
	STAILQ_HEAD(connections, spdk_iscsi_conn)	connections;
	struct spdk_sock_group				*sock_group;
	TAILQ_ENTRY(spdk_iscsi_poll_group)		link;
};

struct spdk_iscsi_opts {
	char *authfile;
	char *nodebase;
	int32_t timeout;
	int32_t nopininterval;
	bool disable_chap;
	bool require_chap;
	bool mutual_chap;
	int32_t chap_group;
	uint32_t MaxSessions;
	uint32_t MaxConnectionsPerSession;
	uint32_t MaxConnections;
	uint32_t MaxQueueDepth;
	uint32_t DefaultTime2Wait;
	uint32_t DefaultTime2Retain;
	uint32_t FirstBurstLength;
	bool ImmediateData;
	uint32_t ErrorRecoveryLevel;
	bool AllowDuplicateIsid;
	uint32_t MaxLargeDataInPerConnection;
	uint32_t MaxR2TPerConnection;
};

struct spdk_iscsi_globals {
	char *authfile;
	char *nodebase;
	pthread_mutex_t mutex;
	uint32_t refcnt;
	TAILQ_HEAD(, spdk_iscsi_portal)		portal_head;
	TAILQ_HEAD(, spdk_iscsi_portal_grp)	pg_head;
	TAILQ_HEAD(, spdk_iscsi_init_grp)	ig_head;
	TAILQ_HEAD(, spdk_iscsi_tgt_node)	target_head;
	TAILQ_HEAD(, spdk_iscsi_auth_group)	auth_group_head;
	TAILQ_HEAD(, spdk_iscsi_poll_group)	poll_group_head;

	int32_t timeout;
	int32_t nopininterval;
	bool disable_chap;
	bool require_chap;
	bool mutual_chap;
	int32_t chap_group;

	uint32_t MaxSessions;
	uint32_t MaxConnectionsPerSession;
	uint32_t MaxConnections;
	uint32_t MaxQueueDepth;
	uint32_t DefaultTime2Wait;
	uint32_t DefaultTime2Retain;
	uint32_t FirstBurstLength;
	bool ImmediateData;
	uint32_t ErrorRecoveryLevel;
	bool AllowDuplicateIsid;
	uint32_t MaxLargeDataInPerConnection;
	uint32_t MaxR2TPerConnection;

	struct spdk_mempool *pdu_pool;
	struct spdk_mempool *pdu_immediate_data_pool;
	struct spdk_mempool *pdu_data_out_pool;
	struct spdk_mempool *session_pool;
	struct spdk_mempool *task_pool;

	struct spdk_iscsi_sess	**session;
};

#define ISCSI_SECURITY_NEGOTIATION_PHASE	0
#define ISCSI_OPERATIONAL_NEGOTIATION_PHASE	1
#define ISCSI_NSG_RESERVED_CODE			2
#define ISCSI_FULL_FEATURE_PHASE		3

/* logout reason */
#define ISCSI_LOGOUT_REASON_CLOSE_SESSION		0
#define ISCSI_LOGOUT_REASON_CLOSE_CONNECTION		1
#define ISCSI_LOGOUT_REASON_REMOVE_CONN_FOR_RECOVERY	2

enum spdk_error_codes {
	SPDK_ISCSI_CONNECTION_FATAL	= -1,
	SPDK_PDU_FATAL		= -2,
};

#define DGET24(B)											\
	(((  (uint32_t) *((uint8_t *)(B)+0)) << 16)				\
	 | (((uint32_t) *((uint8_t *)(B)+1)) << 8)				\
	 | (((uint32_t) *((uint8_t *)(B)+2)) << 0))

#define DSET24(B,D)													\
	(((*((uint8_t *)(B)+0)) = (uint8_t)((uint32_t)(D) >> 16)),		\
	 ((*((uint8_t *)(B)+1)) = (uint8_t)((uint32_t)(D) >> 8)),		\
	 ((*((uint8_t *)(B)+2)) = (uint8_t)((uint32_t)(D) >> 0)))

#define xstrdup(s) (s ? strdup(s) : (char *)NULL)

extern struct spdk_iscsi_globals g_iscsi;
extern struct spdk_iscsi_opts *g_spdk_iscsi_opts;

struct spdk_iscsi_task;
struct spdk_json_write_ctx;

typedef void (*spdk_iscsi_init_cb)(void *cb_arg, int rc);

void spdk_iscsi_init(spdk_iscsi_init_cb cb_fn, void *cb_arg);
typedef void (*spdk_iscsi_fini_cb)(void *arg);
void spdk_iscsi_fini(spdk_iscsi_fini_cb cb_fn, void *cb_arg);
void shutdown_iscsi_conns_done(void);
void spdk_iscsi_config_json(struct spdk_json_write_ctx *w);

struct spdk_iscsi_opts *iscsi_opts_alloc(void);
void iscsi_opts_free(struct spdk_iscsi_opts *opts);
struct spdk_iscsi_opts *iscsi_opts_copy(struct spdk_iscsi_opts *src);
void iscsi_opts_info_json(struct spdk_json_write_ctx *w);
int iscsi_set_discovery_auth(bool disable_chap, bool require_chap,
			     bool mutual_chap, int32_t chap_group);
int iscsi_chap_get_authinfo(struct iscsi_chap_auth *auth, const char *authuser,
			    int ag_tag);
int iscsi_add_auth_group(int32_t tag, struct spdk_iscsi_auth_group **_group);
struct spdk_iscsi_auth_group *iscsi_find_auth_group_by_tag(int32_t tag);
void iscsi_delete_auth_group(struct spdk_iscsi_auth_group *group);
int iscsi_auth_group_add_secret(struct spdk_iscsi_auth_group *group,
				const char *user, const char *secret,
				const char *muser, const char *msecret);
int iscsi_auth_group_delete_secret(struct spdk_iscsi_auth_group *group,
				   const char *user);
void iscsi_auth_groups_info_json(struct spdk_json_write_ctx *w);

void iscsi_task_response(struct spdk_iscsi_conn *conn,
			 struct spdk_iscsi_task *task);
int iscsi_build_iovs(struct spdk_iscsi_conn *conn, struct iovec *iovs, int iovcnt,
		     struct spdk_iscsi_pdu *pdu, uint32_t *mapped_length);
int iscsi_handle_incoming_pdus(struct spdk_iscsi_conn *conn);
void iscsi_task_mgmt_response(struct spdk_iscsi_conn *conn,
			      struct spdk_iscsi_task *task);

void iscsi_free_sess(struct spdk_iscsi_sess *sess);
void iscsi_clear_all_transfer_task(struct spdk_iscsi_conn *conn,
				   struct spdk_scsi_lun *lun,
				   struct spdk_iscsi_pdu *pdu);
bool iscsi_del_transfer_task(struct spdk_iscsi_conn *conn, uint32_t CmdSN);

uint32_t iscsi_pdu_calc_header_digest(struct spdk_iscsi_pdu *pdu);
uint32_t iscsi_pdu_calc_data_digest(struct spdk_iscsi_pdu *pdu);

/* Memory management */
void iscsi_put_pdu(struct spdk_iscsi_pdu *pdu);
struct spdk_iscsi_pdu *iscsi_get_pdu(struct spdk_iscsi_conn *conn);
void iscsi_op_abort_task_set(struct spdk_iscsi_task *task,
			     uint8_t function);
void iscsi_queue_task(struct spdk_iscsi_conn *conn, struct spdk_iscsi_task *task);

static inline uint32_t
iscsi_get_max_immediate_data_size(void)
{
	/*
	 * Specify enough extra space in addition to FirstBurstLength to
	 *  account for a header digest, data digest and additional header
	 *  segments (AHS).  These are not normally used but they do not
	 *  take up much space and we need to make sure the worst-case scenario
	 *  can be satisified by the size returned here.
	 */
	return g_iscsi.FirstBurstLength +
	       ISCSI_DIGEST_LEN + /* data digest */
	       ISCSI_DIGEST_LEN + /* header digest */
	       8 +		   /* bidirectional AHS */
	       52;		   /* extended CDB AHS (for a 64-byte CDB) */
}

#endif /* SPDK_ISCSI_H */
