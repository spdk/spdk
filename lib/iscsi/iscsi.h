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

#include "iscsi/param.h"
#include "iscsi/tgt_node.h"

#include "spdk/assert.h"

#define SPDK_ISCSI_BUILD_ETC "/usr/local/etc/spdk"
#define SPDK_ISCSI_DEFAULT_CONFIG SPDK_ISCSI_BUILD_ETC "/iscsi.conf"
#define SPDK_ISCSI_DEFAULT_AUTHFILE SPDK_ISCSI_BUILD_ETC "/auth.conf"
#define SPDK_ISCSI_DEFAULT_NODEBASE "iqn.2016-06.io.spdk"

#define DEFAULT_MAXR2T 4
#define MAX_INITIATOR_NAME 256
#define MAX_TARGET_NAME 256

#define MAX_PORTAL 1024
#define MAX_INITIATOR 256
#define MAX_NETMASK 256
#define MAX_SESSIONS 1024
#define MAX_ISCSI_CONNECTIONS MAX_SESSIONS
#define MAX_FIRSTBURSTLENGTH	16777215

#define DEFAULT_PORT 3260
#define DEFAULT_MAX_SESSIONS 128
#define DEFAULT_MAX_CONNECTIONS_PER_SESSION 2
#define DEFAULT_MAXOUTSTANDINGR2T 1
#define DEFAULT_DEFAULTTIME2WAIT 2
#define DEFAULT_DEFAULTTIME2RETAIN 20
#define DEFAULT_FIRSTBURSTLENGTH 8192
#define DEFAULT_INITIALR2T 1
#define DEFAULT_IMMEDIATEDATA 1
#define DEFAULT_DATAPDUINORDER 1
#define DEFAULT_DATASEQUENCEINORDER 1
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
 * SPDK iSCSI target will only send a maximum of SPDK_BDEV_LARGE_BUF_MAX_SIZE data segments, even if the
 * connection can support more.
 */
#define SPDK_ISCSI_MAX_SEND_DATA_SEGMENT_LENGTH SPDK_BDEV_LARGE_BUF_MAX_SIZE

/*
 * Defines maximum number of data out buffers each connection can have in
 *  use at any given time.
 */
#define MAX_DATA_OUT_PER_CONNECTION 16

/*
 * Defines maximum number of data in buffers each connection can have in
 *  use at any given time. So this limit does not affect I/O smaller than
 *  SPDK_BDEV_SMALL_BUF_MAX_SIZE.
 */
#define MAX_LARGE_DATAIN_PER_CONNECTION 64

/*
 * Defines default maximum queue depth per connection and this can be
 * changed by configuration file.
 */
#define DEFAULT_MAX_QUEUE_DEPTH	64

#define SPDK_ISCSI_MAX_BURST_LENGTH	\
		(SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH * MAX_DATA_OUT_PER_CONNECTION)

#define SPDK_ISCSI_FIRST_BURST_LENGTH	8192

/** Defines how long we should wait for a TCP close after responding to a
 *   logout request, before terminating the connection ourselves.
 */
#define ISCSI_LOGOUT_TIMEOUT 5 /* in seconds */

/* according to RFC1982 */
#define SN32_CMPMAX (((uint32_t)1U) << (32 - 1))
#define SN32_LT(S1,S2) \
	(((uint32_t)(S1) != (uint32_t)(S2))				\
	    && (((uint32_t)(S1) < (uint32_t)(S2)			\
		    && ((uint32_t)(S2) - (uint32_t)(S1) < SN32_CMPMAX))	\
		|| ((uint32_t)(S1) > (uint32_t)(S2)			\
		    && ((uint32_t)(S1) - (uint32_t)(S2) > SN32_CMPMAX))))
#define SN32_GT(S1,S2) \
	(((uint32_t)(S1) != (uint32_t)(S2))				\
	    && (((uint32_t)(S1) < (uint32_t)(S2)			\
		    && ((uint32_t)(S2) - (uint32_t)(S1) > SN32_CMPMAX))	\
		|| ((uint32_t)(S1) > (uint32_t)(S2)			\
		    && ((uint32_t)(S1) - (uint32_t)(S2) < SN32_CMPMAX))))

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
	size_t len;
	uint64_t reserved; /* do not use */
};

struct spdk_iscsi_pdu {
	struct iscsi_bhs bhs;
	struct spdk_mobj *mobj;
	uint8_t *data_buf;
	uint8_t *data;
	uint8_t header_digest[ISCSI_DIGEST_LEN];
	uint8_t data_digest[ISCSI_DIGEST_LEN];
	size_t data_segment_len;
	int bhs_valid_bytes;
	int ahs_valid_bytes;
	int data_valid_bytes;
	int hdigest_valid_bytes;
	int ddigest_valid_bytes;
	int ref;
	bool data_from_mempool;  /* indicate whether the data buffer is allocated from mempool */
	struct spdk_iscsi_task *task; /* data tied to a task buffer */
	uint32_t cmd_sn;
	uint32_t writev_offset;
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

enum iscsi_connection_state {
	ISCSI_CONN_STATE_INVALID = 0,
	ISCSI_CONN_STATE_RUNNING = 1,
	ISCSI_CONN_STATE_LOGGED_OUT = 2,
	ISCSI_CONN_STATE_EXITING = 3,
	ISCSI_CONN_STATE_EXITED = 4,
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

#define ISCSI_CHAP_CHALLENGE_LEN 1024
struct iscsi_chap_auth {
	enum iscsi_chap_phase chap_phase;

	char *user;
	char *secret;
	char *muser;
	char *msecret;

	uint8_t chap_id[1];
	uint8_t chap_mid[1];
	int chap_challenge_len;
	uint8_t chap_challenge[ISCSI_CHAP_CHALLENGE_LEN];
	int chap_mchallenge_len;
	uint8_t chap_mchallenge[ISCSI_CHAP_CHALLENGE_LEN];
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
	uint32_t InitialR2T;
	uint32_t ImmediateData;
	uint32_t DataPDUInOrder;
	uint32_t DataSequenceInOrder;
	uint32_t ErrorRecoveryLevel;

	uint32_t ExpCmdSN;
	uint32_t MaxCmdSN;

	uint32_t current_text_itt;
};

struct spdk_iscsi_globals {
	char *chapfile;
	char *nodebase;
	pthread_mutex_t mutex;
	TAILQ_HEAD(, spdk_iscsi_portal)		portal_head;
	TAILQ_HEAD(, spdk_iscsi_portal_grp)	pg_head;
	TAILQ_HEAD(, spdk_iscsi_init_grp)	ig_head;
	int ntargets;
	TAILQ_HEAD(, spdk_iscsi_tgt_node)	target_head;

	int timeout;
	int nopininterval;
	bool disable_chap_for_discovery;
	bool require_chap_for_discovery;
	bool mutual_chap_for_discovery;
	int chap_group_for_discovery;

	uint32_t MaxSessions;
	uint32_t MaxConnectionsPerSession;
	uint32_t MaxConnections;
	uint32_t MaxQueueDepth;
	uint32_t DefaultTime2Wait;
	uint32_t DefaultTime2Retain;
	uint32_t ImmediateData;
	uint32_t ErrorRecoveryLevel;
	uint32_t AllowDuplicateIsid;

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

enum spdk_error_codes {
	SPDK_SUCCESS		= 0,
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

extern struct spdk_iscsi_globals g_spdk_iscsi;

struct spdk_iscsi_task;

int spdk_iscsi_init(void);
typedef void (*spdk_iscsi_fini_cb)(void *arg);
void spdk_iscsi_fini(spdk_iscsi_fini_cb cb_fn, void *cb_arg);
void spdk_iscsi_fini_done(void);
void spdk_iscsi_config_text(FILE *fp);

int spdk_iscsi_send_nopin(struct spdk_iscsi_conn *conn);
void spdk_iscsi_task_response(struct spdk_iscsi_conn *conn,
			      struct spdk_iscsi_task *task);
int spdk_iscsi_execute(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu *pdu);
int spdk_iscsi_build_iovecs(struct spdk_iscsi_conn *conn,
			    struct iovec *iovec, struct spdk_iscsi_pdu *pdu);
int
spdk_iscsi_read_pdu(struct spdk_iscsi_conn *conn, struct spdk_iscsi_pdu **_pdu);
void spdk_iscsi_task_mgmt_response(struct spdk_iscsi_conn *conn,
				   struct spdk_iscsi_task *task);

int spdk_iscsi_conn_params_init(struct iscsi_param **params);
int spdk_iscsi_sess_params_init(struct iscsi_param **params);

void spdk_free_sess(struct spdk_iscsi_sess *sess);
void spdk_clear_all_transfer_task(struct spdk_iscsi_conn *conn,
				  struct spdk_scsi_lun *lun);
void spdk_del_transfer_task(struct spdk_iscsi_conn *conn, uint32_t CmdSN);
bool spdk_iscsi_is_deferred_free_pdu(struct spdk_iscsi_pdu *pdu);

int spdk_iscsi_negotiate_params(struct spdk_iscsi_conn *conn,
				struct iscsi_param **params_p, uint8_t *data,
				int alloc_len, int data_len);
int spdk_iscsi_copy_param2var(struct spdk_iscsi_conn *conn);

void spdk_iscsi_task_cpl(struct spdk_scsi_task *scsi_task);
void spdk_iscsi_task_mgmt_cpl(struct spdk_scsi_task *scsi_task);

/* Memory management */
void spdk_put_pdu(struct spdk_iscsi_pdu *pdu);
struct spdk_iscsi_pdu *spdk_get_pdu(void);
int spdk_iscsi_conn_handle_queued_datain_tasks(struct spdk_iscsi_conn *conn);

static inline int
spdk_get_immediate_data_buffer_size(void)
{
	/*
	 * Specify enough extra space in addition to FirstBurstLength to
	 *  account for a header digest, data digest and additional header
	 *  segments (AHS).  These are not normally used but they do not
	 *  take up much space and we need to make sure the worst-case scenario
	 *  can be satisified by the size returned here.
	 */
	return SPDK_ISCSI_FIRST_BURST_LENGTH +
	       ISCSI_DIGEST_LEN + /* data digest */
	       ISCSI_DIGEST_LEN + /* header digest */
	       8 +		   /* bidirectional AHS */
	       52;		   /* extended CDB AHS (for a 64-byte CDB) */
}

static inline int
spdk_get_data_out_buffer_size(void)
{
	return SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH;
}

#endif /* SPDK_ISCSI_H */
