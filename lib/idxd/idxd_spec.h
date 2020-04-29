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

/**
 * \file
 * IDXD specification definitions
 */

#ifndef SPDK_IDXD_SPEC_H
#define SPDK_IDXD_SPEC_H

#include "spdk/stdinc.h"
#include "spdk/assert.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IDXD_MMIO_BAR			0
#define IDXD_WQ_BAR			2
#define PORTAL_SIZE			(4096 * 4)

#define CFG_ENGINE_OFFSET		0x20
#define CFG_FLAG_OFFSET			0x28

#define IDXD_CMD_SHIFT			20

#define IDXD_VERSION_OFFSET		0x00
#define IDXD_GENCAP_OFFSET		0x10
#define IDXD_WQCAP_OFFSET		0x20
#define IDXD_GRPCAP_OFFSET		0x30
#define IDXD_OPCAP_OFFSET		0x40
#define IDXD_ENGCAP_OFFSET		0x38
#define IDXD_OPCAP_OFFSET		0x40
#define IDXD_TABLE_OFFSET		0x60
#define IDXD_GENCFG_OFFSET		0x80
#define IDXD_GENCTRL_OFFSET		0x88
#define IDXD_GENSTATUS_OFFSET		0x90
#define IDXD_INTCAUSE_OFFSET		0x98
#define IDXD_CMD_OFFSET			0xa0
#define IDXD_CMDSTS_OFFSET		0xa8
#define IDXD_SWERR_OFFSET		0xc0
#define IDXD_TABLE_OFFSET_MULT		0x100

#define IDXD_OPCAP_WORDS		0x4

#define IDXD_CLEAR_CRC_FLAGS		0xFFFFu

#define IDXD_FLAG_FENCE                 (1 << 0)
#define IDXD_FLAG_COMPLETION_ADDR_VALID (1 << 2)
#define IDXD_FLAG_REQUEST_COMPLETION    (1 << 3)
#define IDXD_FLAG_CACHE_CONTROL         (1 << 8)

/*
 * IDXD is a family of devices, DSA is the only currently
 * supported one.
 */
enum dsa_completion_status {
	IDXD_COMP_NONE			= 0,
	IDXD_COMP_SUCCESS		= 1,
	IDXD_COMP_SUCCESS_PRED		= 2,
	IDXD_COMP_PAGE_FAULT_NOBOF	= 3,
	IDXD_COMP_PAGE_FAULT_IR		= 4,
	IDXD_COMP_BATCH_FAIL		= 5,
	IDXD_COMP_BATCH_PAGE_FAULT	= 6,
	IDXD_COMP_DR_OFFSET_NOINC	= 7,
	IDXD_COMP_DR_OFFSET_ERANGE	= 8,
	IDXD_COMP_DIF_ERR		= 9,
	IDXD_COMP_BAD_OPCODE		= 16,
	IDXD_COMP_INVALID_FLAGS		= 17,
	IDXD_COMP_NOZERO_RESERVE	= 18,
	IDXD_COMP_XFER_ERANGE		= 19,
	IDXD_COMP_DESC_CNT_ERANGE	= 20,
	IDXD_COMP_DR_ERANGE		= 21,
	IDXD_COMP_OVERLAP_BUFFERS	= 22,
	IDXD_COMP_DCAST_ERR		= 23,
	IDXD_COMP_DESCLIST_ALIGN	= 24,
	IDXD_COMP_INT_HANDLE_INVAL	= 25,
	IDXD_COMP_CRA_XLAT		= 26,
	IDXD_COMP_CRA_ALIGN		= 27,
	IDXD_COMP_ADDR_ALIGN		= 28,
	IDXD_COMP_PRIV_BAD		= 29,
	IDXD_COMP_TRAFFIC_CLASS_CONF	= 30,
	IDXD_COMP_PFAULT_RDBA		= 31,
	IDXD_COMP_HW_ERR1		= 32,
	IDXD_COMP_HW_ERR_DRB		= 33,
	IDXD_COMP_TRANSLATION_FAIL	= 34,
};

enum idxd_wq_state {
	WQ_DISABLED	= 0,
	WQ_ENABLED	= 1,
};

enum idxd_wq_flag {
	WQ_FLAG_DEDICATED	= 0,
	WQ_FLAG_BOF		= 1,
};

enum idxd_wq_type {
	WQT_NONE	= 0,
	WQT_KERNEL	= 1,
	WQT_USER	= 2,
	WQT_MDEV	= 3,
};

enum idxd_dev_state {
	IDXD_DEVICE_STATE_DISABLED	= 0,
	IDXD_DEVICE_STATE_ENABLED	= 1,
	IDXD_DEVICE_STATE_DRAIN		= 2,
	IDXD_DEVICE_STATE_HALT		= 3,
};

enum idxd_device_reset_type {
	IDXD_DEVICE_RESET_SOFTWARE	= 0,
	IDXD_DEVICE_RESET_FLR		= 1,
	IDXD_DEVICE_RESET_WARM		= 2,
	IDXD_DEVICE_RESET_COLD		= 3,
};

enum idxd_cmds {
	IDXD_ENABLE_DEV		= 1,
	IDXD_DISABLE_DEV	= 2,
	IDXD_DRAIN_ALL		= 3,
	IDXD_ABORT_ALL		= 4,
	IDXD_RESET_DEVICE	= 5,
	IDXD_ENABLE_WQ		= 6,
	IDXD_DISABLE_WQ		= 7,
	IDXD_DRAIN_WQ		= 8,
	IDXD_ABORT_WQ		= 9,
	IDXD_RESET_WQ		= 10,
};

enum idxd_cmdsts_err {
	IDXD_CMDSTS_SUCCESS		= 0,
	IDXD_CMDSTS_INVAL_CMD		= 1,
	IDXD_CMDSTS_INVAL_WQIDX		= 2,
	IDXD_CMDSTS_HW_ERR		= 3,
	IDXD_CMDSTS_ERR_DEV_ENABLED	= 16,
	IDXD_CMDSTS_ERR_CONFIG		= 17,
	IDXD_CMDSTS_ERR_BUSMASTER_EN	= 18,
	IDXD_CMDSTS_ERR_PASID_INVAL	= 19,
	IDXD_CMDSTS_ERR_WQ_SIZE_ERANGE	= 20,
	IDXD_CMDSTS_ERR_GRP_CONFIG	= 21,
	IDXD_CMDSTS_ERR_GRP_CONFIG2	= 22,
	IDXD_CMDSTS_ERR_GRP_CONFIG3	= 23,
	IDXD_CMDSTS_ERR_GRP_CONFIG4	= 24,
	IDXD_CMDSTS_ERR_DEV_NOTEN	= 32,
	IDXD_CMDSTS_ERR_WQ_ENABLED	= 33,
	IDXD_CMDSTS_ERR_WQ_SIZE		= 34,
	IDXD_CMDSTS_ERR_WQ_PRIOR	= 35,
	IDXD_CMDSTS_ERR_WQ_MODE		= 36,
	IDXD_CMDSTS_ERR_BOF_EN		= 37,
	IDXD_CMDSTS_ERR_PASID_EN	= 38,
	IDXD_CMDSTS_ERR_MAX_BATCH_SIZE	= 39,
	IDXD_CMDSTS_ERR_MAX_XFER_SIZE	= 40,
	IDXD_CMDSTS_ERR_DIS_DEV_EN	= 49,
	IDXD_CMDSTS_ERR_DEV_NOT_EN	= 50,
	IDXD_CMDSTS_ERR_INVAL_INT_IDX	= 65,
	IDXD_CMDSTS_ERR_NO_HANDLE	= 66,
};

enum idxd_wq_hw_state {
	IDXD_WQ_DEV_DISABLED	= 0,
	IDXD_WQ_DEV_ENABLED	= 1,
	IDXD_WQ_DEV_BUSY	= 2,
};

struct idxd_hw_desc {
	uint32_t	pasid: 20;
	uint32_t	rsvd: 11;
	uint32_t	priv: 1;
	uint32_t	flags: 24;
	uint32_t	opcode: 8;
	uint64_t	completion_addr;
	union {
		uint64_t	src_addr;
		uint64_t	readback_addr;
		uint64_t	pattern;
		uint64_t	desc_list_addr;
	};
	union {
		uint64_t	dst_addr;
		uint64_t	readback_addr2;
		uint64_t	src2_addr;
		uint64_t	comp_pattern;
	};
	union {
		uint32_t	xfer_size;
		uint32_t	desc_count;
	};
	uint16_t	int_handle;
	uint16_t	rsvd1;
	union {
		uint8_t		expected_res;
		struct delta {
			uint64_t	addr;
			uint32_t	max_size;
		} delta;
		uint32_t	delta_rec_size;
		uint64_t	dest2;
		struct crc32c {
			uint32_t	seed;
			uint32_t	rsvd;
			uint64_t	addr;
		} crc32c;
		struct dif_chk {
			uint8_t		src_flags;
			uint8_t		rsvd1;
			uint8_t		flags;
			uint8_t		rsvd2[5];
			uint32_t	ref_tag_seed;
			uint16_t	app_tag_mask;
			uint16_t	app_tag_seed;
		} dif_chk;
		struct dif_ins {
			uint8_t		rsvd1;
			uint8_t		dest_flag;
			uint8_t		flags;
			uint8_t		rsvd2[13];
			uint32_t	ref_tag_seed;
			uint16_t	app_tag_mask;
			uint16_t	app_tag_seed;
		} dif_ins;
		struct dif_upd {
			uint8_t		src_flags;
			uint8_t		dest_flags;
			uint8_t		flags;
			uint8_t		rsvd[5];
			uint32_t	src_ref_tag_seed;
			uint16_t	src_app_tag_mask;
			uint16_t	src_app_tag_seed;
			uint32_t	dest_ref_tag_seed;
			uint16_t	dest_app_tag_mask;
			uint16_t	dest_app_tag_seed;
		} dif_upd;
		uint8_t		op_specific[24];
	};
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct idxd_hw_desc) == 64, "size mismatch");

struct idxd_hw_comp_record {
	volatile uint8_t	status;
	union {
		uint8_t		result;
		uint8_t		dif_status;
	};
	uint16_t		rsvd;
	uint32_t		bytes_completed;
	uint64_t		fault_addr;
	union {
		uint32_t	delta_rec_size;
		uint32_t	crc32c_val;
		struct {
			uint32_t	dif_chk_ref_tag;
			uint16_t	dif_chk_app_tag_mask;
			uint16_t	dif_chk_app_tag;
		};
		struct dif_ins_comp {
			uint64_t	rsvd;
			uint32_t	ref_tag;
			uint16_t	app_tag_mask;
			uint16_t	app_tag;
		} dif_ins_comp;
		struct dif_upd_comp {
			uint32_t	src_ref_tag;
			uint16_t	src_app_tag_mask;
			uint16_t	src_app_tag;
			uint32_t	dest_ref_tag;
			uint16_t	dest_app_tag_mask;
			uint16_t	dest_app_tag;
		} dif_upd_comp;
		uint8_t		op_specific[16];
	};
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct idxd_hw_comp_record) == 32, "size mismatch");

union idxd_gencap_register {
	struct {
		uint64_t block_on_fault: 1;
		uint64_t overlap_copy: 1;
		uint64_t cache_control_mem: 1;
		uint64_t cache_control_cache: 1;
		uint64_t rsvd: 3;
		uint64_t int_handle_req: 1;
		uint64_t dest_readback: 1;
		uint64_t drain_readback: 1;
		uint64_t rsvd2: 6;
		uint64_t max_xfer_shift: 5;
		uint64_t max_batch_shift: 4;
		uint64_t max_ims_mult: 6;
		uint64_t config_en: 1;
		uint64_t max_descs_per_engine: 8;
		uint64_t rsvd3: 24;
	} __attribute__((packed));
	uint64_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_gencap_register) == 8, "size mismatch");

union idxd_wqcap_register {
	struct {
		uint64_t total_wq_size: 16;
		uint64_t num_wqs: 8;
		uint64_t rsvd: 24;
		uint64_t shared_mode: 1;
		uint64_t dedicated_mode: 1;
		uint64_t rsvd2: 1;
		uint64_t priority: 1;
		uint64_t occupancy: 1;
		uint64_t occupancy_int: 1;
		uint64_t rsvd3: 10;
	} __attribute__((packed));
	uint64_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_wqcap_register) == 8, "size mismatch");

union idxd_groupcap_register {
	struct {
		uint64_t num_groups: 8;
		uint64_t total_tokens: 8;
		uint64_t token_en: 1;
		uint64_t token_limit: 1;
		uint64_t rsvd: 46;
	} __attribute__((packed));
	uint64_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_groupcap_register) == 8, "size mismatch");

union idxd_enginecap_register {
	struct {
		uint64_t num_engines: 8;
		uint64_t rsvd: 56;
	} __attribute__((packed));
	uint64_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_enginecap_register) == 8, "size mismatch");

struct idxd_opcap_register {
	uint64_t raw[4];
};
SPDK_STATIC_ASSERT(sizeof(struct idxd_opcap_register) == 32, "size mismatch");

struct idxd_registers {
	uint32_t version;
	union idxd_gencap_register gencap;
	union idxd_wqcap_register wqcap;
	union idxd_groupcap_register groupcap;
	union idxd_enginecap_register enginecap;
	struct idxd_opcap_register opcap;
};
SPDK_STATIC_ASSERT(sizeof(struct idxd_registers) == 72, "size mismatch");

union idxd_offsets_register {
	struct {
		uint64_t grpcfg: 16;
		uint64_t wqcfg: 16;
		uint64_t msix_perm: 16;
		uint64_t ims: 16;
		uint64_t perfmon: 16;
		uint64_t rsvd: 48;
	} __attribute__((packed));
	uint64_t raw[2];
};
SPDK_STATIC_ASSERT(sizeof(union idxd_offsets_register) == 16, "size mismatch");

union idxd_genstatus_register {
	struct {
		uint32_t state: 2;
		uint32_t reset_type: 2;
		uint32_t rsvd: 28;
	} __attribute__((packed));
	uint32_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_genstatus_register) == 4, "size mismatch");

union idxd_cmdsts_reg {
	struct {
		uint8_t err;
		uint16_t result;
		uint8_t rsvd: 7;
		uint8_t active: 1;
	} __attribute__((packed));
	uint32_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_cmdsts_reg) == 4, "size mismatch");

union idxd_swerr_register {
	struct {
		uint64_t valid: 1;
		uint64_t overflow: 1;
		uint64_t desc_valid: 1;
		uint64_t wq_idx_valid: 1;
		uint64_t batch: 1;
		uint64_t fault_rw: 1;
		uint64_t priv: 1;
		uint64_t rsvd: 1;
		uint64_t error: 8;
		uint64_t wq_idx: 8;
		uint64_t rsvd2: 8;
		uint64_t operation: 8;
		uint64_t pasid: 20;
		uint64_t rsvd3: 4;
		uint64_t batch_idx: 16;
		uint64_t rsvd4: 16;
		uint64_t invalid_flags: 32;
		uint64_t fault_addr;
		uint64_t rsvd5;
	} __attribute__((packed));
	uint64_t raw[4];
};
SPDK_STATIC_ASSERT(sizeof(union idxd_swerr_register) == 32, "size mismatch");

union idxd_group_flags {
	struct {
		uint32_t tc_a: 3;
		uint32_t tc_b: 3;
		uint32_t rsvd: 1;
		uint32_t use_token_limit: 1;
		uint32_t tokens_reserved: 8;
		uint32_t rsvd2: 4;
		uint32_t tokens_allowed: 8;
		uint32_t rsvd3: 4;
	} __attribute__((packed));
	uint32_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_group_flags) == 4, "size mismatch");

struct idxd_grpcfg {
	uint64_t wqs[4];
	uint64_t engines;
	union idxd_group_flags flags;
};
SPDK_STATIC_ASSERT(sizeof(struct idxd_grpcfg) == 48, "size mismatch");

union idxd_wqcfg {
	struct {
		uint16_t wq_size;
		uint16_t rsvd;
		uint16_t wq_thresh;
		uint16_t rsvd1;
		uint32_t mode: 1;
		uint32_t bof: 1;
		uint32_t rsvd2: 2;
		uint32_t priority: 4;
		uint32_t pasid: 20;
		uint32_t pasid_en: 1;
		uint32_t priv: 1;
		uint32_t rsvd3: 2;
		uint32_t max_xfer_shift: 5;
		uint32_t max_batch_shift: 4;
		uint32_t rsvd4: 23;
		uint16_t occupancy_inth;
		uint16_t occupancy_table_sel: 1;
		uint16_t rsvd5: 15;
		uint16_t occupancy_limit;
		uint16_t occupancy_int_en: 1;
		uint16_t rsvd6: 15;
		uint16_t occupancy;
		uint16_t occupancy_int: 1;
		uint16_t rsvd7: 12;
		uint16_t mode_support: 1;
		uint16_t wq_state: 2;
		uint32_t rsvd8;
	} __attribute__((packed));
	uint32_t raw[8];
};
SPDK_STATIC_ASSERT(sizeof(union idxd_wqcfg) == 32, "size mismatch");

#ifdef __cplusplus
}
#endif

#endif /* SPDK_IDXD_SPEC_H */
