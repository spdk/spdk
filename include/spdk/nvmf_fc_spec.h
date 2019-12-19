/*
 *   BSD LICENSE
 *
 *   Copyright (c) 2018-2019 Broadcom.  All Rights Reserved.
 *   The term "Broadcom" refers to Broadcom Inc. and/or its subsidiaries.
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

#ifndef __NVMF_FC_SPEC_H__
#define __NVMF_FC_SPEC_H__

#include "spdk/env.h"
#include "spdk/nvme.h"

/*
 * FC-NVMe Spec. Definitions
 */

#define FCNVME_R_CTL_CMD_REQ                   0x06
#define FCNVME_R_CTL_DATA_OUT                  0x01
#define FCNVME_R_CTL_CONFIRM                   0x03
#define FCNVME_R_CTL_STATUS                    0x07
#define FCNVME_R_CTL_ERSP_STATUS               0x08
#define FCNVME_R_CTL_LS_REQUEST                0x32
#define FCNVME_R_CTL_LS_RESPONSE               0x33
#define FCNVME_R_CTL_BA_ABTS                   0x81

#define FCNVME_F_CTL_END_SEQ                   0x080000
#define FCNVME_F_CTL_SEQ_INIT                  0x010000

/* END_SEQ | LAST_SEQ | Exchange Responder | SEQ init */
#define FCNVME_F_CTL_RSP                       0x990000

#define FCNVME_TYPE_BLS                        0x0
#define FCNVME_TYPE_FC_EXCHANGE                0x08
#define FCNVME_TYPE_NVMF_DATA                  0x28

#define FCNVME_CMND_IU_FC_ID                   0x28
#define FCNVME_CMND_IU_SCSI_ID                 0xFD

#define FCNVME_CMND_IU_NODATA                  0x00
#define FCNVME_CMND_IU_READ                    0x10
#define FCNVME_CMND_IU_WRITE                   0x01

/* BLS reject error codes */
#define FCNVME_BLS_REJECT_UNABLE_TO_PERFORM    0x09
#define FCNVME_BLS_REJECT_EXP_NOINFO           0x00
#define FCNVME_BLS_REJECT_EXP_INVALID_OXID     0x03

/*
 * FC NVMe Link Services (LS) constants
 */
#define FCNVME_MAX_LS_REQ_SIZE                  1536
#define FCNVME_MAX_LS_RSP_SIZE                  64

#define FCNVME_LS_CA_CMD_MIN_LEN                592
#define FCNVME_LS_CA_DESC_LIST_MIN_LEN          584
#define FCNVME_LS_CA_DESC_MIN_LEN               576

/* this value needs to be in sync with low level driver buffer size */
#define FCNVME_MAX_LS_BUFFER_SIZE               2048

#define FCNVME_GOOD_RSP_LEN                     12
#define FCNVME_ASSOC_HOSTID_LEN                 16


typedef uint64_t FCNVME_BE64;
typedef uint32_t FCNVME_BE32;
typedef uint16_t FCNVME_BE16;

/*
 * FC-NVME LS Commands
 */
enum {
	FCNVME_LS_RSVD                = 0,
	FCNVME_LS_RJT                 = 1,
	FCNVME_LS_ACC                 = 2,
	FCNVME_LS_CREATE_ASSOCIATION  = 3,
	FCNVME_LS_CREATE_CONNECTION	  = 4,
	FCNVME_LS_DISCONNECT          = 5,
};

/*
 * FC-NVME Link Service Descriptors
 */
enum {
	FCNVME_LSDESC_RSVD             = 0x0,
	FCNVME_LSDESC_RQST             = 0x1,
	FCNVME_LSDESC_RJT              = 0x2,
	FCNVME_LSDESC_CREATE_ASSOC_CMD = 0x3,
	FCNVME_LSDESC_CREATE_CONN_CMD  = 0x4,
	FCNVME_LSDESC_DISCONN_CMD      = 0x5,
	FCNVME_LSDESC_CONN_ID          = 0x6,
	FCNVME_LSDESC_ASSOC_ID         = 0x7,
};

/*
 * LS Reject reason_codes
 */
enum fcnvme_ls_rjt_reason {
	FCNVME_RJT_RC_NONE         = 0,     /* no reason - not to be sent */
	FCNVME_RJT_RC_INVAL        = 0x01,  /* invalid NVMe_LS command code */
	FCNVME_RJT_RC_LOGIC        = 0x03,  /* logical error */
	FCNVME_RJT_RC_UNAB         = 0x09,  /* unable to perform request */
	FCNVME_RJT_RC_UNSUP        = 0x0b,  /* command not supported */
	FCNVME_RJT_RC_INPROG       = 0x0e,  /* command already in progress */
	FCNVME_RJT_RC_INV_ASSOC    = 0x40,  /* invalid Association ID */
	FCNVME_RJT_RC_INV_CONN     = 0x41,  /* invalid Connection ID */
	FCNVME_RJT_RC_INV_PARAM    = 0x42,  /* invalid parameters */
	FCNVME_RJT_RC_INSUFF_RES   = 0x43,  /* insufficient resources */
	FCNVME_RJT_RC_INV_HOST     = 0x44,  /* invalid or rejected host */
	FCNVME_RJT_RC_VENDOR       = 0xff,  /* vendor specific error */
};

/*
 * LS Reject reason_explanation codes
 */
enum fcnvme_ls_rjt_explan {
	FCNVME_RJT_EXP_NONE	       = 0x00,  /* No additional explanation */
	FCNVME_RJT_EXP_OXID_RXID   = 0x17,  /* invalid OX_ID-RX_ID combo */
	FCNVME_RJT_EXP_UNAB_DATA   = 0x2a,  /* unable to supply data */
	FCNVME_RJT_EXP_INV_LEN     = 0x2d,  /* invalid payload length */
	FCNVME_RJT_EXP_INV_ESRP    = 0x40,  /* invalid ESRP ratio */
	FCNVME_RJT_EXP_INV_CTL_ID  = 0x41,  /* invalid controller ID */
	FCNVME_RJT_EXP_INV_Q_ID    = 0x42,  /* invalid queue ID */
	FCNVME_RJT_EXP_SQ_SIZE     = 0x43,  /* invalid submission queue size */
	FCNVME_RJT_EXP_INV_HOST_ID = 0x44,  /* invalid or rejected host ID */
	FCNVME_RJT_EXP_INV_HOSTNQN = 0x45,  /* invalid or rejected host NQN */
	FCNVME_RJT_EXP_INV_SUBNQN  = 0x46,  /* invalid or rejected subsys nqn */
};

/*
 * NVMe over FC CMD IU
 */
struct spdk_nvmf_fc_cmnd_iu {
	uint32_t scsi_id: 8,
		 fc_id: 8,
		 cmnd_iu_len: 16;
	uint32_t rsvd0: 24,
		 flags: 8;
	uint64_t conn_id;
	uint32_t cmnd_seq_num;
	uint32_t data_len;
	struct spdk_nvme_cmd cmd;
	uint32_t rsvd1[2];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_cmnd_iu) == 96, "size_mismatch");

/*
 * NVMe over Extended Response IU
 */
struct spdk_nvmf_fc_ersp_iu {
	uint32_t status_code: 8,
		 rsvd0: 8,
		 ersp_len: 16;
	uint32_t response_seq_no;
	uint32_t transferred_data_len;
	uint32_t rsvd1;
	struct spdk_nvme_cpl rsp;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ersp_iu) == 32, "size_mismatch");

/*
 * Transfer ready IU
 */
struct spdk_nvmf_fc_xfer_rdy_iu {
	uint32_t relative_offset;
	uint32_t burst_len;
	uint32_t rsvd;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_xfer_rdy_iu) == 12, "size_mismatch");

/*
 * FC NVME Frame Header
 */
struct spdk_nvmf_fc_frame_hdr {
	FCNVME_BE32 r_ctl: 8,
		    d_id: 24;
	FCNVME_BE32 cs_ctl: 8,
		    s_id: 24;
	FCNVME_BE32 type: 8,
		    f_ctl: 24;
	FCNVME_BE32 seq_id: 8,
		    df_ctl: 8,
		    seq_cnt: 16;
	FCNVME_BE32 ox_id: 16,
		    rx_id: 16;
	FCNVME_BE32 parameter;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_frame_hdr) == 24, "size_mismatch");

/*
 * Request payload word 0
 */
struct spdk_nvmf_fc_ls_rqst_w0 {
	uint8_t	ls_cmd;			/* FCNVME_LS_xxx */
	uint8_t zeros[3];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ls_rqst_w0) == 4, "size_mismatch");

/*
 * LS request information descriptor
 */
struct spdk_nvmf_fc_lsdesc_rqst {
	FCNVME_BE32 desc_tag;		/* FCNVME_LSDESC_xxx */
	FCNVME_BE32 desc_len;
	struct spdk_nvmf_fc_ls_rqst_w0 w0;
	FCNVME_BE32 rsvd12;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_lsdesc_rqst) == 16, "size_mismatch");

/*
 * LS accept header
 */
struct spdk_nvmf_fc_ls_acc_hdr {
	struct spdk_nvmf_fc_ls_rqst_w0 w0;
	FCNVME_BE32 desc_list_len;
	struct spdk_nvmf_fc_lsdesc_rqst rqst;
	/* Followed by cmd-specific ACC descriptors, see next definitions */
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ls_acc_hdr) == 24, "size_mismatch");

/*
 * LS descriptor connection id
 */
struct spdk_nvmf_fc_lsdesc_conn_id {
	FCNVME_BE32 desc_tag;
	FCNVME_BE32 desc_len;
	FCNVME_BE64 connection_id;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_lsdesc_conn_id) == 16, "size_mismatch");

/*
 * LS decriptor association id
 */
struct spdk_nvmf_fc_lsdesc_assoc_id {
	FCNVME_BE32 desc_tag;
	FCNVME_BE32 desc_len;
	FCNVME_BE64 association_id;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_lsdesc_assoc_id) == 16, "size_mismatch");

/*
 * LS Create Association descriptor
 */
struct spdk_nvmf_fc_lsdesc_cr_assoc_cmd {
	FCNVME_BE32  desc_tag;
	FCNVME_BE32  desc_len;
	FCNVME_BE16  ersp_ratio;
	FCNVME_BE16  rsvd10;
	FCNVME_BE32  rsvd12[9];
	FCNVME_BE16  cntlid;
	FCNVME_BE16  sqsize;
	FCNVME_BE32  rsvd52;
	uint8_t hostid[FCNVME_ASSOC_HOSTID_LEN];
	uint8_t hostnqn[SPDK_NVME_NQN_FIELD_SIZE];
	uint8_t subnqn[SPDK_NVME_NQN_FIELD_SIZE];
	uint8_t rsvd584[432];
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_lsdesc_cr_assoc_cmd) == 1016, "size_mismatch");

/*
 * LS Create Association reqeust payload
 */
struct spdk_nvmf_fc_ls_cr_assoc_rqst {
	struct spdk_nvmf_fc_ls_rqst_w0 w0;
	FCNVME_BE32 desc_list_len;
	struct spdk_nvmf_fc_lsdesc_cr_assoc_cmd assoc_cmd;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ls_cr_assoc_rqst) == 1024, "size_mismatch");

/*
 * LS Create Association accept payload
 */
struct spdk_nvmf_fc_ls_cr_assoc_acc {
	struct spdk_nvmf_fc_ls_acc_hdr hdr;
	struct spdk_nvmf_fc_lsdesc_assoc_id assoc_id;
	struct spdk_nvmf_fc_lsdesc_conn_id conn_id;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ls_cr_assoc_acc) == 56, "size_mismatch");

/*
 * LS Create IO Connection descriptor
 */
struct spdk_nvmf_fc_lsdesc_cr_conn_cmd {
	FCNVME_BE32 desc_tag;
	FCNVME_BE32 desc_len;
	FCNVME_BE16 ersp_ratio;
	FCNVME_BE16 rsvd10;
	FCNVME_BE32 rsvd12[9];
	FCNVME_BE16 qid;
	FCNVME_BE16 sqsize;
	FCNVME_BE32 rsvd52;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ls_cr_assoc_acc) == 56, "size_mismatch");

/*
 * LS Create IO Connection payload
 */
struct spdk_nvmf_fc_ls_cr_conn_rqst {
	struct spdk_nvmf_fc_ls_rqst_w0 w0;
	FCNVME_BE32 desc_list_len;
	struct spdk_nvmf_fc_lsdesc_assoc_id assoc_id;
	struct spdk_nvmf_fc_lsdesc_cr_conn_cmd connect_cmd;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ls_cr_conn_rqst) == 80, "size_mismatch");

/*
 * LS Create IO Connection accept payload
 */
struct spdk_nvmf_fc_ls_cr_conn_acc {
	struct spdk_nvmf_fc_ls_acc_hdr hdr;
	struct spdk_nvmf_fc_lsdesc_conn_id conn_id;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ls_cr_conn_acc) == 40, "size_mismatch");

/*
 * LS Disconnect descriptor
 */
struct spdk_nvmf_fc_lsdesc_disconn_cmd {
	FCNVME_BE32 desc_tag;
	FCNVME_BE32 desc_len;
	FCNVME_BE32 rsvd8;
	FCNVME_BE32 rsvd12;
	FCNVME_BE32 rsvd16;
	FCNVME_BE32 rsvd20;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_lsdesc_disconn_cmd) == 24, "size_mismatch");

/*
 * LS Disconnect payload
 */
struct spdk_nvmf_fc_ls_disconnect_rqst {
	struct spdk_nvmf_fc_ls_rqst_w0 w0;
	FCNVME_BE32 desc_list_len;
	struct spdk_nvmf_fc_lsdesc_assoc_id assoc_id;
	struct spdk_nvmf_fc_lsdesc_disconn_cmd disconn_cmd;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ls_disconnect_rqst) == 48, "size_mismatch");

/*
 * LS Disconnect accept payload
 */
struct spdk_nvmf_fc_ls_disconnect_acc {
	struct spdk_nvmf_fc_ls_acc_hdr hdr;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ls_disconnect_acc) == 24, "size_mismatch");

/*
 * LS Reject descriptor
 */
struct spdk_nvmf_fc_lsdesc_rjt {
	FCNVME_BE32 desc_tag;
	FCNVME_BE32 desc_len;
	uint8_t rsvd8;

	uint8_t reason_code;
	uint8_t reason_explanation;

	uint8_t vendor;
	FCNVME_BE32 rsvd12;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_lsdesc_rjt) == 16, "size_mismatch");

/*
 * LS Reject payload
 */
struct spdk_nvmf_fc_ls_rjt {
	struct spdk_nvmf_fc_ls_rqst_w0 w0;
	FCNVME_BE32 desc_list_len;
	struct spdk_nvmf_fc_lsdesc_rqst rqst;
	struct spdk_nvmf_fc_lsdesc_rjt rjt;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_fc_ls_rjt) == 40, "size_mismatch");

/*
 * FC World Wide Name
 */
struct spdk_nvmf_fc_wwn {
	union {
		uint64_t wwn; /* World Wide Names consist of eight bytes */
		uint8_t octets[sizeof(uint64_t)];
	} u;
};

#endif
