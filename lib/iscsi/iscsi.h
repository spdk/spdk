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

#include <stdint.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <stdbool.h>

#include "spdk/bdev.h"

#include "iscsi/param.h"
#include "iscsi/tgt_node.h"

#include "spdk/assert.h"

#define SPDK_ISCSI_BUILD_ETC "/usr/local/etc/spdk"
#define SPDK_ISCSI_DEFAULT_CONFIG SPDK_ISCSI_BUILD_ETC "/iscsi.conf"
#define SPDK_ISCSI_DEFAULT_AUTHFILE SPDK_ISCSI_BUILD_ETC "/auth.conf"
#define SPDK_ISCSI_DEFAULT_NODEBASE "iqn.2013-10.com.intel.spdk"

extern uint64_t g_flush_timeout;

#define DEFAULT_MAXR2T 4
#define MAX_INITIATOR_NAME 256
#define MAX_TARGET_NAME 256

#define MAX_ISCSI_NAME 256

#define MAX_PORTAL 1024
#define MAX_INITIATOR 256
#define MAX_NETMASK 256
#define MAX_PORTAL_GROUP 4096
#define MAX_INITIATOR_GROUP 4096
#define MAX_ISCSI_TARGET_NODE 4096
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
#define DEFAULT_FLUSH_TIMEOUT 8

/*
 * SPDK iSCSI target currently only supports 64KB as the maximum data segment length
 *  it can receive from initiators.  Other values may work, but no guarantees.
 */
#define SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH  65536

/*
 * SPDK iSCSI target will only send a maximum of SPDK_BDEV_LARGE_RBUF_MAX_SIZE data segments, even if the
 * connection can support more.
*/
#define SPDK_ISCSI_MAX_SEND_DATA_SEGMENT_LENGTH SPDK_BDEV_LARGE_RBUF_MAX_SIZE

/*
 * Defines maximum number of data out buffers each connection can have in
 *  use at any given time.
 */
#define MAX_DATA_OUT_PER_CONNECTION 16

/*
 * Defines maximum number of data in buffers each connection can have in
 *  use at any given time.  An "extra data in buffer" means any buffer after
 *  the first for the iSCSI I/O command.  So this limit does not affect I/O
 *  smaller than SPDK_ISCSI_MAX_SEND_DATA_SEGMENT_LENGTH.
 */
#define MAX_EXTRA_DATAIN_PER_CONNECTION 64

#define NUM_PDU_PER_CONNECTION	(2 * (SPDK_ISCSI_MAX_QUEUE_DEPTH + MAX_EXTRA_DATAIN_PER_CONNECTION + 8))

#define SPDK_ISCSI_MAX_BURST_LENGTH	\
		(SPDK_ISCSI_MAX_RECV_DATA_SEGMENT_LENGTH * MAX_DATA_OUT_PER_CONNECTION)

#define SPDK_ISCSI_FIRST_BURST_LENGTH	8192

/** Defines how long we should wait for a TCP close after responding to a
 *   logout request, before terminating the connection ourselves.
 */
#define ISCSI_LOGOUT_TIMEOUT 5 /* in seconds */

#define ISCSI_BHS_LEN 48
#define ISCSI_DIGEST_LEN 4
#define ISCSI_ALIGNMENT 4
/* support version - RFC3720(10.12.4) */
#define ISCSI_VERSION 0x00

#define ISCSI_ALIGN(SIZE) \
	(((SIZE) + (ISCSI_ALIGNMENT - 1)) & ~(ISCSI_ALIGNMENT - 1))

/* for authentication key (non encoded 1024bytes) RFC3720(5.1/11.1.4) */
#define ISCSI_TEXT_MAX_VAL_LEN 8192
/*
 * RFC 3720 5.1
 * If not otherwise specified, the maximum length of a simple-value
 * (not its encoded representation) is 255 bytes, not including the delimiter
 * (comma or zero byte).
 */
#define ISCSI_TEXT_MAX_SIMPLE_VAL_LEN 255

#define ISCSI_TEXT_MAX_KEY_LEN 63

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

enum iscsi_op {
	/* Initiator opcodes */
	ISCSI_OP_NOPOUT         = 0x00,
	ISCSI_OP_SCSI           = 0x01,
	ISCSI_OP_TASK           = 0x02,
	ISCSI_OP_LOGIN          = 0x03,
	ISCSI_OP_TEXT           = 0x04,
	ISCSI_OP_SCSI_DATAOUT   = 0x05,
	ISCSI_OP_LOGOUT         = 0x06,
	ISCSI_OP_SNACK          = 0x10,
	ISCSI_OP_VENDOR_1C      = 0x1c,
	ISCSI_OP_VENDOR_1D      = 0x1d,
	ISCSI_OP_VENDOR_1E      = 0x1e,

	/* Target opcodes */
	ISCSI_OP_NOPIN          = 0x20,
	ISCSI_OP_SCSI_RSP       = 0x21,
	ISCSI_OP_TASK_RSP       = 0x22,
	ISCSI_OP_LOGIN_RSP      = 0x23,
	ISCSI_OP_TEXT_RSP       = 0x24,
	ISCSI_OP_SCSI_DATAIN    = 0x25,
	ISCSI_OP_LOGOUT_RSP     = 0x26,
	ISCSI_OP_R2T            = 0x31,
	ISCSI_OP_ASYNC          = 0x32,
	ISCSI_OP_VENDOR_3C      = 0x3c,
	ISCSI_OP_VENDOR_3D      = 0x3d,
	ISCSI_OP_VENDOR_3E      = 0x3e,
	ISCSI_OP_REJECT         = 0x3f,
};

enum iscsi_task_func {
	ISCSI_TASK_FUNC_ABORT_TASK = 1,
	ISCSI_TASK_FUNC_ABORT_TASK_SET = 2,
	ISCSI_TASK_FUNC_CLEAR_ACA = 3,
	ISCSI_TASK_FUNC_CLEAR_TASK_SET = 4,
	ISCSI_TASK_FUNC_LOGICAL_UNIT_RESET = 5,
	ISCSI_TASK_FUNC_TARGET_WARM_RESET = 6,
	ISCSI_TASK_FUNC_TARGET_COLD_RESET = 7,
	ISCSI_TASK_FUNC_TASK_REASSIGN = 8,
};

enum iscsi_task_func_resp {
	ISCSI_TASK_FUNC_RESP_COMPLETE = 0,
	ISCSI_TASK_FUNC_RESP_TASK_NOT_EXIST = 1,
	ISCSI_TASK_FUNC_RESP_LUN_NOT_EXIST = 2,
	ISCSI_TASK_FUNC_RESP_TASK_STILL_ALLEGIANT = 3,
	ISCSI_TASK_FUNC_RESP_REASSIGNMENT_NOT_SUPPORTED = 4,
	ISCSI_TASK_FUNC_RESP_FUNC_NOT_SUPPORTED = 5,
	ISCSI_TASK_FUNC_RESP_AUTHORIZATION_FAILED = 6,
	ISCSI_TASK_FUNC_REJECTED = 255
};

struct iscsi_bhs {
	uint8_t opcode		: 6;
	uint8_t immediate	: 1;
	uint8_t reserved	: 1;
	uint8_t flags;
	uint8_t rsv[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint64_t lun;
	uint32_t itt;
	uint32_t ttt;
	uint32_t stat_sn;
	uint32_t exp_stat_sn;
	uint32_t max_stat_sn;
	uint8_t res3[12];
};
SPDK_STATIC_ASSERT(sizeof(struct iscsi_bhs) == ISCSI_BHS_LEN, "ISCSI_BHS_LEN mismatch");

struct iscsi_bhs_async {
	uint8_t opcode		: 6;	/* opcode = 0x32 */
	uint8_t reserved	: 2;
	uint8_t flags;
	uint8_t res[2];

	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];

	uint64_t lun;
	uint32_t ffffffff;
	uint32_t res3;
	uint32_t stat_sn;
	uint32_t exp_cmd_sn;
	uint32_t max_cmd_sn;
	uint8_t async_event;
	uint8_t async_vcode;
	uint16_t param1;
	uint16_t param2;
	uint16_t param3;
	uint8_t res4[4];
};

struct iscsi_bhs_login_req {
	uint8_t opcode		: 6;	/* opcode = 0x03 */
	uint8_t immediate	: 1;
	uint8_t reserved	: 1;
	uint8_t flags;
	uint8_t version_max;
	uint8_t version_min;
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint8_t isid[6];
	uint16_t tsih;
	uint32_t itt;
	uint16_t cid;
	uint16_t res2;
	uint32_t cmd_sn;
	uint32_t exp_stat_sn;
	uint8_t res3[16];
};

struct iscsi_bhs_login_rsp {
	uint8_t opcode		: 6;	/* opcode = 0x23 */
	uint8_t reserved	: 2;
	uint8_t flags;
	uint8_t version_max;
	uint8_t version_act;
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint8_t isid[6];
	uint16_t tsih;
	uint32_t itt;
	uint32_t res2;
	uint32_t stat_sn;
	uint32_t exp_cmd_sn;
	uint32_t max_cmd_sn;
	uint8_t status_class;
	uint8_t status_detail;
	uint8_t res3[10];
};

struct iscsi_bhs_logout_req {
	uint8_t opcode		: 6;	/* opcode = 0x06 */
	uint8_t immediate	: 1;
	uint8_t reserved	: 1;
	uint8_t reason;
	uint8_t res[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint8_t res2[8];
	uint32_t itt;
	uint16_t cid;
	uint16_t res3;
	uint32_t cmd_sn;
	uint32_t exp_stat_sn;
	uint8_t res4[16];
};

struct iscsi_bhs_logout_resp {
	uint8_t opcode		: 6;	/* opcode = 0x26 */
	uint8_t reserved	: 2;
	uint8_t flags;
	uint8_t response;
	uint8_t res;
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint8_t res2[8];
	uint32_t itt;
	uint32_t res3;
	uint32_t stat_sn;
	uint32_t exp_cmd_sn;
	uint32_t max_cmd_sn;
	uint32_t res4;
	uint16_t time_2_wait;
	uint16_t time_2_retain;
	uint32_t res5;
};

struct iscsi_bhs_nop_in {
	uint8_t opcode		: 6;	/* opcode = 0x20 */
	uint8_t reserved	: 2;
	uint8_t flags;
	uint8_t res[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint64_t lun;
	uint32_t itt;
	uint32_t ttt;
	uint32_t stat_sn;
	uint32_t exp_cmd_sn;
	uint32_t max_cmd_sn;
	uint8_t res3[12];
};

struct iscsi_bhs_nop_out {
	uint8_t opcode		: 6;	/* opcode = 0x00 */
	uint8_t immediate	: 1;
	uint8_t reserved	: 1;
	uint8_t flags;
	uint8_t res[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint64_t lun;
	uint32_t itt;
	uint32_t ttt;
	uint32_t cmd_sn;
	uint32_t exp_stat_sn;
	uint8_t res4[16];
};

struct iscsi_bhs_r2t {
	uint8_t opcode		: 6;	/* opcode = 0x31 */
	uint8_t reserved	: 2;
	uint8_t flags;
	uint8_t rsv[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint64_t lun;
	uint32_t itt;
	uint32_t ttt;
	uint32_t stat_sn;
	uint32_t exp_cmd_sn;
	uint32_t max_cmd_sn;
	uint32_t r2t_sn;
	uint32_t buffer_offset;
	uint32_t desired_xfer_len;
};

struct iscsi_bhs_reject {
	uint8_t opcode		: 6;	/* opcode = 0x3f */
	uint8_t reserved	: 2;
	uint8_t flags;
	uint8_t reason;
	uint8_t res;
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint8_t res2[8];
	uint32_t ffffffff;
	uint32_t res3;
	uint32_t stat_sn;
	uint32_t exp_cmd_sn;
	uint32_t max_cmd_sn;
	uint32_t data_sn;
	uint8_t res4[8];
};

struct iscsi_bhs_scsi_req {
	uint8_t opcode		: 6;	/* opcode = 0x01 */
	uint8_t immediate	: 1;
	uint8_t reserved	: 1;
	uint8_t attribute	: 3;
	uint8_t reserved2	: 2;
	uint8_t write		: 1;
	uint8_t read		: 1;
	uint8_t final		: 1;
	uint8_t res[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint64_t lun;
	uint32_t itt;
	uint32_t expected_data_xfer_len;
	uint32_t cmd_sn;
	uint32_t exp_stat_sn;
	uint8_t cdb[16];
};

struct iscsi_bhs_scsi_resp {
	uint8_t opcode		: 6;	/* opcode = 0x21 */
	uint8_t reserved	: 2;
	uint8_t flags;
	uint8_t response;
	uint8_t status;
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint8_t res4[8];
	uint32_t itt;
	uint32_t snacktag;
	uint32_t stat_sn;
	uint32_t exp_cmd_sn;
	uint32_t max_cmd_sn;
	uint32_t exp_data_sn;
	uint32_t bi_read_res_cnt;
	uint32_t res_cnt;
};

struct iscsi_bhs_data_in {
	uint8_t opcode		: 6;	/* opcode = 0x05 */
	uint8_t reserved	: 2;
	uint8_t flags;
	uint8_t res;
	uint8_t status;
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint64_t lun;
	uint32_t itt;
	uint32_t ttt;
	uint32_t stat_sn;
	uint32_t exp_cmd_sn;
	uint32_t max_cmd_sn;
	uint32_t data_sn;
	uint32_t buffer_offset;
	uint32_t res_cnt;
};

struct iscsi_bhs_data_out {
	uint8_t opcode		: 6;	/* opcode = 0x25 */
	uint8_t reserved	: 2;
	uint8_t flags;
	uint8_t res[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint64_t lun;
	uint32_t itt;
	uint32_t ttt;
	uint32_t res3;
	uint32_t exp_stat_sn;
	uint32_t res4;
	uint32_t data_sn;
	uint32_t buffer_offset;
	uint32_t res5;
};

struct iscsi_bhs_snack_req {
	uint8_t opcode		: 6;	/* opcode = 0x10 */
	uint8_t reserved	: 2;
	uint8_t flags;
	uint8_t res[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint64_t lun;
	uint32_t itt;
	uint32_t ttt;
	uint32_t res5;
	uint32_t exp_stat_sn;
	uint8_t res6[8];
	uint32_t beg_run;
	uint32_t run_len;
};

struct iscsi_bhs_task_req {
	uint8_t opcode		: 6;	/* opcode = 0x02 */
	uint8_t immediate	: 1;
	uint8_t reserved	: 1;
	uint8_t flags;
	uint8_t res[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint64_t lun;
	uint32_t itt;
	uint32_t ref_task_tag;
	uint32_t cmd_sn;
	uint32_t exp_stat_sn;
	uint32_t ref_cmd_sn;
	uint32_t exp_data_sn;
	uint8_t res5[8];
};

struct iscsi_bhs_task_resp {
	uint8_t opcode		: 6;	/* opcode = 0x22 */
	uint8_t reserved	: 2;
	uint8_t flags;
	uint8_t response;
	uint8_t res;
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint8_t res2[8];
	uint32_t itt;
	uint32_t res3;
	uint32_t stat_sn;
	uint32_t exp_cmd_sn;
	uint32_t max_cmd_sn;
	uint8_t res4[12];
};

struct iscsi_bhs_text_req {
	uint8_t opcode		: 6;	/* opcode = 0x04 */
	uint8_t immediate	: 1;
	uint8_t reserved	: 1;
	uint8_t flags;
	uint8_t res[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint64_t lun;
	uint32_t itt;
	uint32_t ttt;
	uint32_t cmd_sn;
	uint32_t exp_stat_sn;
	uint8_t res3[16];
};

struct iscsi_bhs_text_resp {
	uint8_t opcode		: 6;	/* opcode = 0x24 */
	uint8_t reserved	: 2;
	uint8_t flags;
	uint8_t res[2];
	uint8_t total_ahs_len;
	uint8_t data_segment_len[3];
	uint64_t lun;
	uint32_t itt;
	uint32_t ttt;
	uint32_t stat_sn;
	uint32_t exp_cmd_sn;
	uint32_t max_cmd_sn;
	uint8_t res4[12];
};

/* generic flags */
#define ISCSI_FLAG_FINAL		0x80

/* login flags */
#define ISCSI_LOGIN_TRANSIT		0x80
#define ISCSI_LOGIN_CONTINUE		0x40
#define ISCSI_LOGIN_CURRENT_STAGE_MASK	0x0c
#define ISCSI_LOGIN_CURRENT_STAGE_0	0x04
#define ISCSI_LOGIN_CURRENT_STAGE_1	0x08
#define ISCSI_LOGIN_CURRENT_STAGE_3	0x0c
#define ISCSI_LOGIN_NEXT_STAGE_MASK	0x03
#define ISCSI_LOGIN_NEXT_STAGE_0	0x01
#define ISCSI_LOGIN_NEXT_STAGE_1	0x02
#define ISCSI_LOGIN_NEXT_STAGE_3	0x03

/* text flags */
#define ISCSI_TEXT_CONTINUE		0x40

/* logout flags */
#define ISCSI_LOGOUT_REASON_MASK	0x7f

/* datain flags */
#define ISCSI_DATAIN_ACKNOLWEDGE	0x40
#define ISCSI_DATAIN_OVERFLOW		0x04
#define ISCSI_DATAIN_UNDERFLOW		0x02
#define ISCSI_DATAIN_STATUS		0x01

/* SCSI resp flags */
#define ISCSI_SCSI_BIDI_OVERFLOW	0x10
#define ISCSI_SCSI_BIDI_UNDERFLOW	0x08
#define ISCSI_SCSI_OVERFLOW		0x04
#define ISCSI_SCSI_UNDERFLOW		0x02

/* SCSI task flags */
#define ISCSI_TASK_FUNCTION_MASK	0x7f

/* Reason for Reject */
#define ISCSI_REASON_RESERVED			0x1
#define ISCSI_REASON_DATA_DIGEST_ERROR		0x2
#define ISCSI_REASON_DATA_SNACK_REJECT		0x3
#define ISCSI_REASON_PROTOCOL_ERROR		0x4
#define ISCSI_REASON_CMD_NOT_SUPPORTED		0x5
#define ISCSI_REASON_IMM_CMD_REJECT		0x6
#define ISCSI_REASON_TASK_IN_PROGRESS		0x7
#define ISCSI_REASON_INVALID_SNACK		0x8
#define ISCSI_REASON_INVALID_PDU_FIELD		0x9
#define ISCSI_REASON_LONG_OPERATION_REJECT	0xa
#define ISCSI_REASON_NEGOTIATION_RESET		0xb
#define ISCSI_REASON_WAIT_FOR_RESET		0xc

#define ISCSI_FLAG_SNACK_TYPE_DATA		0
#define ISCSI_FLAG_SNACK_TYPE_R2T		0
#define ISCSI_FLAG_SNACK_TYPE_STATUS		1
#define ISCSI_FLAG_SNACK_TYPE_DATA_ACK		2
#define ISCSI_FLAG_SNACK_TYPE_RDATA		3
#define ISCSI_FLAG_SNACK_TYPE_MASK	0x0F	/* 4 bits */


/* For spdk_iscsi_login_in related function use, we need to avoid the conflict
 * with other errors
 * */
#define SPDK_ISCSI_LOGIN_ERROR_RESPONSE -1000
#define SPDK_ISCSI_LOGIN_ERROR_PARAMETER -1001
#define SPDK_ISCSI_PARAMETER_EXCHANGE_NOT_ONCE -1002

struct iscsi_ahs {
	/* 0-3 */
	uint8_t ahs_len[2];
	uint8_t ahs_type;
	uint8_t ahs_specific1;
	/* 4-x */
	uint8_t ahs_specific2[];
};

#define ISCSI_AHS_LEN 60

struct spdk_mobj {
	struct rte_mempool *mp;
	void *buf;
	size_t len;
	uint64_t reserved; /* do not use */
};

struct spdk_iscsi_pdu {
	struct iscsi_bhs bhs;
	struct iscsi_ahs *ahs;
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
	int data_ref;
	struct spdk_iscsi_task *task; /* data tied to a task buffer */
	uint32_t cmd_sn;
	uint32_t writev_offset;
	TAILQ_ENTRY(spdk_iscsi_pdu)	tailq;


	/*
	 * 60 bytes of AHS should suffice for now.
	 * This should always be at the end of PDU data structure.
	 * we need to not zero this out when doing memory clear.
	 */
	uint8_t ahs_data[ISCSI_AHS_LEN];
};

enum iscsi_connection_state {
	ISCSI_CONN_STATE_INVALID = 0,
	ISCSI_CONN_STATE_RUNNING = 1,
	ISCSI_CONN_STATE_LOGGED_OUT = 2,
	ISCSI_CONN_STATE_EXITING = 3,
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

	struct spdk_scsi_port initiator_port;
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
	char *authfile;
	char *nodebase;
	pthread_mutex_t mutex;
	TAILQ_HEAD(, spdk_iscsi_portal_grp)	pg_head;
	TAILQ_HEAD(, spdk_iscsi_init_grp)	ig_head;
	int ntargets;
	struct spdk_iscsi_tgt_node *target[MAX_ISCSI_TARGET_NODE];

	int timeout;
	int nopininterval;
	int no_discovery_auth;
	int req_discovery_auth;
	int req_discovery_auth_mutual;
	int discovery_auth_group;

	uint32_t MaxSessions;
	uint32_t MaxConnectionsPerSession;
	uint32_t MaxConnections;
	uint32_t MaxOutstandingR2T;
	uint32_t DefaultTime2Wait;
	uint32_t DefaultTime2Retain;
	uint32_t FirstBurstLength;
	uint32_t MaxBurstLength;
	uint32_t MaxRecvDataSegmentLength;
	uint32_t InitialR2T;
	uint32_t ImmediateData;
	uint32_t DataPDUInOrder;
	uint32_t DataSequenceInOrder;
	uint32_t ErrorRecoveryLevel;
	uint32_t AllowDuplicateIsid;

	struct rte_mempool *pdu_pool;
	struct rte_mempool *pdu_immediate_data_pool;
	struct rte_mempool *pdu_data_out_pool;
	struct rte_mempool *session_pool;
	struct rte_mempool *task_pool;

	struct spdk_iscsi_sess	**session;
};

#define ISCSI_BHS_LOGIN_GET_TBIT(X) (!!(X & ISCSI_LOGIN_TRANSIT))
#define ISCSI_BHS_LOGIN_GET_CBIT(X) (!!(X & ISCSI_LOGIN_CONTINUE))
#define ISCSI_BHS_LOGIN_GET_CSG(X) ((X & ISCSI_LOGIN_CURRENT_STAGE_MASK) >> 2)
#define ISCSI_BHS_LOGIN_GET_NSG(X) (X & ISCSI_LOGIN_NEXT_STAGE_MASK)

#define ISCSI_SECURITY_NEGOTIATION_PHASE	0
#define ISCSI_OPERATIONAL_NEGOTIATION_PHASE	1
#define ISCSI_NSG_RESERVED_CODE			2
#define ISCSI_FULL_FEATURE_PHASE		3

#define ISCSI_CLASS_SUCCESS	0x00
#define ISCSI_CLASS_REDIRECT	0x01
#define ISCSI_CLASS_INITIATOR_ERROR	0x02
#define ISCSI_CLASS_TARGET_ERROR	0x03

/* Class (Success) detailed info: 0 */
#define ISCSI_LOGIN_ACCEPT		0x00

/* Class (Redirection) detailed info: 1 */
#define ISCSI_LOGIN_TARGET_TEMPORARILY_MOVED 0x01
#define ISCSI_LOGIN_TARGET_PERMANENTLY_MOVED	0x02

/* Class(Initiator Error) detailed info: 2 */
#define ISCSI_LOGIN_INITIATOR_ERROR		0x00
#define ISCSI_LOGIN_AUTHENT_FAIL		0x01
#define ISCSI_LOGIN_AUTHORIZATION_FAIL	0x02
#define ISCSI_LOGIN_TARGET_NOT_FOUND	0x03
#define ISCSI_LOGIN_TARGET_REMOVED		0x04
#define ISCSI_LOGIN_UNSUPPORTED_VERSION		0x05
#define ISCSI_LOGIN_TOO_MANY_CONNECTIONS		0x06
#define ISCSI_LOGIN_MISSING_PARMS	0x07
#define ISCSI_LOGIN_CONN_ADD_FAIL	0x08
#define ISCSI_LOGIN_NOT_SUPPORTED_SESSION_TYPE	0x09
#define ISCSI_LOGIN_NO_SESSION		0x0a
#define ISCSI_LOGIN_INVALID_LOGIN_REQUEST	0x0b

/* Class(Target Error) detailed info: 3 */
#define ISCSI_LOGIN_STATUS_TARGET_ERROR		0x00
#define ISCSI_LOGIN_STATUS_SERVICE_UNAVAILABLE	0x01
#define ISCSI_LOGIN_STATUS_NO_RESOURCES		0x02

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
void spdk_del_connection_queued_task(void *tailq, struct spdk_scsi_lun *lun);
void spdk_del_transfer_task(struct spdk_iscsi_conn *conn, uint32_t CmdSN);
bool  spdk_iscsi_is_deferred_free_pdu(struct spdk_iscsi_pdu *pdu);

void spdk_iscsi_shutdown(void);
int spdk_iscsi_negotiate_params(struct spdk_iscsi_conn *conn,
				struct iscsi_param *params, uint8_t *data,
				int alloc_len, int data_len);
int spdk_iscsi_copy_param2var(struct spdk_iscsi_conn *conn);

void process_task_completion(spdk_event_t event);
void process_task_mgmt_completion(spdk_event_t event);

/* Memory management */
void spdk_put_pdu(struct spdk_iscsi_pdu *pdu);
struct spdk_iscsi_pdu *spdk_get_pdu(void);
int spdk_iscsi_conn_handle_queued_datain(struct spdk_iscsi_conn *conn);

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
	return g_spdk_iscsi.FirstBurstLength +
	       ISCSI_DIGEST_LEN + /* data digest */
	       ISCSI_DIGEST_LEN + /* header digest */
	       8 +		   /* bidirectional AHS */
	       52;		   /* extended CDB AHS (for a 64-byte CDB) */
}

static inline int
spdk_get_data_out_buffer_size(void)
{
	return g_spdk_iscsi.MaxRecvDataSegmentLength;
}

#endif /* SPDK_ISCSI_H */
