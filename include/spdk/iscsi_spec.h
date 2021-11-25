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

/**
 * \file
 * iSCSI specification definitions
 */

#ifndef SPDK_ISCSI_SPEC_H
#define SPDK_ISCSI_SPEC_H

#include "spdk/stdinc.h"

#include "spdk/assert.h"

#define ISCSI_BHS_LEN 48
#define ISCSI_DIGEST_LEN 4
#define ISCSI_ALIGNMENT 4

/** support version - RFC3720(10.12.4) */
#define ISCSI_VERSION 0x00

#define ISCSI_ALIGN(SIZE) \
	(((SIZE) + (ISCSI_ALIGNMENT - 1)) & ~(ISCSI_ALIGNMENT - 1))

/** for authentication key (non encoded 1024bytes) RFC3720(5.1/11.1.4) */
#define ISCSI_TEXT_MAX_VAL_LEN 8192

/**
 * RFC 3720 5.1
 * If not otherwise specified, the maximum length of a simple-value
 * (not its encoded representation) is 255 bytes, not including the delimiter
 * (comma or zero byte).
 */
#define ISCSI_TEXT_MAX_SIMPLE_VAL_LEN 255

#define ISCSI_TEXT_MAX_KEY_LEN 63

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
	uint8_t reason		: 7;
	uint8_t reason_1	: 1;
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
	uint8_t write_bit	: 1;
	uint8_t read_bit	: 1;
	uint8_t final_bit	: 1;
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
#define ISCSI_FLAG_FINAL			0x80

/* login flags */
#define ISCSI_LOGIN_TRANSIT			0x80
#define ISCSI_LOGIN_CONTINUE			0x40
#define ISCSI_LOGIN_CURRENT_STAGE_MASK		0x0c
#define ISCSI_LOGIN_CURRENT_STAGE_0		0x04
#define ISCSI_LOGIN_CURRENT_STAGE_1		0x08
#define ISCSI_LOGIN_CURRENT_STAGE_3		0x0c
#define ISCSI_LOGIN_NEXT_STAGE_MASK		0x03
#define ISCSI_LOGIN_NEXT_STAGE_0		0x01
#define ISCSI_LOGIN_NEXT_STAGE_1		0x02
#define ISCSI_LOGIN_NEXT_STAGE_3		0x03

/* text flags */
#define ISCSI_TEXT_CONTINUE			0x40

/* datain flags */
#define ISCSI_DATAIN_ACKNOWLEDGE		0x40
#define ISCSI_DATAIN_OVERFLOW			0x04
#define ISCSI_DATAIN_UNDERFLOW			0x02
#define ISCSI_DATAIN_STATUS			0x01

/* SCSI resp flags */
#define ISCSI_SCSI_BIDI_OVERFLOW		0x10
#define ISCSI_SCSI_BIDI_UNDERFLOW		0x08
#define ISCSI_SCSI_OVERFLOW			0x04
#define ISCSI_SCSI_UNDERFLOW			0x02

/* SCSI task flags */
#define ISCSI_TASK_FUNCTION_MASK		0x7f

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
#define ISCSI_FLAG_SNACK_TYPE_MASK		0x0F	/* 4 bits */

struct iscsi_ahs {
	/* 0-3 */
	uint8_t ahs_len[2];
	uint8_t ahs_type;
	uint8_t ahs_specific1;
	/* 4-x */
	uint8_t ahs_specific2[];
};

#define ISCSI_BHS_LOGIN_GET_TBIT(X) (!!(X & ISCSI_LOGIN_TRANSIT))
#define ISCSI_BHS_LOGIN_GET_CBIT(X) (!!(X & ISCSI_LOGIN_CONTINUE))
#define ISCSI_BHS_LOGIN_GET_CSG(X) ((X & ISCSI_LOGIN_CURRENT_STAGE_MASK) >> 2)
#define ISCSI_BHS_LOGIN_GET_NSG(X) (X & ISCSI_LOGIN_NEXT_STAGE_MASK)

#define ISCSI_CLASS_SUCCESS			0x00
#define ISCSI_CLASS_REDIRECT			0x01
#define ISCSI_CLASS_INITIATOR_ERROR		0x02
#define ISCSI_CLASS_TARGET_ERROR		0x03

/* Class (Success) detailed info: 0 */
#define ISCSI_LOGIN_ACCEPT			0x00

/* Class (Redirection) detailed info: 1 */
#define ISCSI_LOGIN_TARGET_TEMPORARILY_MOVED	0x01
#define ISCSI_LOGIN_TARGET_PERMANENTLY_MOVED	0x02

/* Class (Initiator Error) detailed info: 2 */
#define ISCSI_LOGIN_INITIATOR_ERROR		0x00
#define ISCSI_LOGIN_AUTHENT_FAIL		0x01
#define ISCSI_LOGIN_AUTHORIZATION_FAIL		0x02
#define ISCSI_LOGIN_TARGET_NOT_FOUND		0x03
#define ISCSI_LOGIN_TARGET_REMOVED		0x04
#define ISCSI_LOGIN_UNSUPPORTED_VERSION		0x05
#define ISCSI_LOGIN_TOO_MANY_CONNECTIONS	0x06
#define ISCSI_LOGIN_MISSING_PARMS		0x07
#define ISCSI_LOGIN_CONN_ADD_FAIL		0x08
#define ISCSI_LOGIN_NOT_SUPPORTED_SESSION_TYPE	0x09
#define ISCSI_LOGIN_NO_SESSION			0x0a
#define ISCSI_LOGIN_INVALID_LOGIN_REQUEST	0x0b

/* Class (Target Error) detailed info: 3 */
#define ISCSI_LOGIN_STATUS_TARGET_ERROR		0x00
#define ISCSI_LOGIN_STATUS_SERVICE_UNAVAILABLE	0x01
#define ISCSI_LOGIN_STATUS_NO_RESOURCES		0x02

#endif /* SPDK_ISCSI_SPEC_H */
