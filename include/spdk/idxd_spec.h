/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
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
#define PORTAL_SIZE			0x1000
#define WQ_TOTAL_PORTAL_SIZE		(PORTAL_SIZE * 4)
#define PORTAL_STRIDE			0x40
#define PORTAL_MASK			(PORTAL_SIZE - 1)
#define WQCFG_SHIFT			5

#define IDXD_TABLE_OFFSET_MULT		0x100

#define IDXD_CLEAR_CRC_FLAGS		0xFFFFu

#define IDXD_FLAG_FENCE			(1 << 0)
#define IDXD_FLAG_COMPLETION_ADDR_VALID	(1 << 2)
#define IDXD_FLAG_REQUEST_COMPLETION	(1 << 3)
#define IDXD_FLAG_CACHE_CONTROL		(1 << 8)
#define IDXD_FLAG_DEST_READBACK		(1 << 14)
#define IDXD_FLAG_DEST_STEERING_TAG	(1 << 15)
#define IDXD_FLAG_CRC_READ_CRC_SEED	(1 << 16)

#define IAA_FLAG_RD_SRC2_AECS		(1 << 16)
#define IAA_COMP_FLUSH_OUTPUT		(1 << 1)
#define IAA_COMP_APPEND_EOB		(1 << 2)
#define IAA_COMP_FLAGS			(IAA_COMP_FLUSH_OUTPUT | IAA_COMP_APPEND_EOB)
#define IAA_DECOMP_ENABLE		(1 << 0)
#define IAA_DECOMP_FLUSH_OUTPUT		(1 << 1)
#define IAA_DECOMP_CHECK_FOR_EOB	(1 << 2)
#define IAA_DECOMP_STOP_ON_EOB		(1 << 3)
#define IAA_DECOMP_FLAGS		(IAA_DECOMP_ENABLE | \
					IAA_DECOMP_FLUSH_OUTPUT | \
					IAA_DECOMP_CHECK_FOR_EOB | \
					IAA_DECOMP_STOP_ON_EOB)

/*
 * IDXD is a family of devices, DSA and IAA.
 */
enum dsa_completion_status {
	DSA_COMP_NONE			= 0,
	DSA_COMP_SUCCESS		= 1,
	DSA_COMP_SUCCESS_PRED		= 2,
	DSA_COMP_PAGE_FAULT_NOBOF	= 3,
	DSA_COMP_PAGE_FAULT_IR		= 4,
	DSA_COMP_BATCH_FAIL		= 5,
	DSA_COMP_BATCH_PAGE_FAULT	= 6,
	DSA_COMP_DR_OFFSET_NOINC	= 7,
	DSA_COMP_DR_OFFSET_ERANGE	= 8,
	DSA_COMP_DIF_ERR		= 9,
	DSA_COMP_BAD_OPCODE		= 16,
	DSA_COMP_INVALID_FLAGS		= 17,
	DSA_COMP_NOZERO_RESERVE		= 18,
	DSA_COMP_XFER_ERANGE		= 19,
	DSA_COMP_DESC_CNT_ERANGE	= 20,
	DSA_COMP_DR_ERANGE		= 21,
	DSA_COMP_OVERLAP_BUFFERS	= 22,
	DSA_COMP_DCAST_ERR		= 23,
	DSA_COMP_DESCLIST_ALIGN		= 24,
	DSA_COMP_INT_HANDLE_INVAL	= 25,
	DSA_COMP_CRA_XLAT		= 26,
	DSA_COMP_CRA_ALIGN		= 27,
	DSA_COMP_ADDR_ALIGN		= 28,
	DSA_COMP_PRIV_BAD		= 29,
	DSA_COMP_TRAFFIC_CLASS_CONF	= 30,
	DSA_COMP_PFAULT_RDBA		= 31,
	DSA_COMP_HW_ERR1		= 32,
	DSA_COMP_HW_ERR_DRB		= 33,
	DSA_COMP_TRANSLATION_FAIL	= 34,
};

enum iaa_completion_status {
	IAA_COMP_NONE			= 0,
	IAA_COMP_SUCCESS		= 1,
	IAA_COMP_PAGE_FAULT_IR		= 4,
	IAA_COMP_OUTBUF_OVERFLOW	= 5,
	IAA_COMP_BAD_OPCODE		= 16,
	IAA_COMP_INVALID_FLAGS		= 17,
	IAA_COMP_NOZERO_RESERVE		= 18,
	IAA_COMP_INVALID_SIZE		= 19,
	IAA_COMP_OVERLAP_BUFFERS	= 22,
	IAA_COMP_INT_HANDLE_INVAL	= 25,
	IAA_COMP_CRA_XLAT		= 32,
	IAA_COMP_CRA_ALIGN		= 33,
	IAA_COMP_ADDR_ALIGN		= 34,
	IAA_COMP_PRIV_BAD		= 35,
	IAA_COMP_TRAFFIC_CLASS_CONF	= 36,
	IAA_COMP_PFAULT_RDBA		= 37,
	IAA_COMP_HW_ERR1		= 38,
	IAA_COMP_TRANSLATION_FAIL	= 39,
	IAA_COMP_PRS_TIMEOUT		= 40,
	IAA_COMP_WATCHDOG		= 41,
	IAA_COMP_INVALID_COMP_FLAG	= 48,
	IAA_COMP_INVALID_FILTER_FLAG	= 49,
	IAA_COMP_INVALID_NUM_ELEMS	= 50,
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
		uint64_t	src1_addr;
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
		uint32_t	src1_size;
		uint32_t	xfer_size;
		uint32_t	desc_count;
	};
	uint16_t	int_handle;
	union {
		uint16_t	rsvd1;
		uint16_t	compr_flags;
		uint16_t	decompr_flags;
	};
	union {
		struct {
			uint64_t	src2_addr;
			uint32_t	max_dst_size;
			uint32_t	src2_size;
			uint32_t	filter_flags;
			uint32_t	num_inputs;
		} iaa;
		uint8_t		expected_res;
		struct {
			uint64_t	addr;
			uint32_t	max_size;
		} delta;
		uint32_t	delta_rec_size;
		uint64_t	dest2;
		struct {
			uint32_t	seed;
			uint32_t	rsvd;
			uint64_t	addr;
		} crc32c;
		struct {
			uint8_t		src_flags;
			uint8_t		rsvd1;
			uint8_t		flags;
			uint8_t		rsvd2[5];
			uint32_t	ref_tag_seed;
			uint16_t	app_tag_mask;
			uint16_t	app_tag_seed;
		} dif_chk;
		struct {
			uint8_t		rsvd1;
			uint8_t		dest_flag;
			uint8_t		flags;
			uint8_t		rsvd2[13];
			uint32_t	ref_tag_seed;
			uint16_t	app_tag_mask;
			uint16_t	app_tag_seed;
		} dif_ins;
		struct {
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
} __attribute((aligned(64)));
SPDK_STATIC_ASSERT(sizeof(struct idxd_hw_desc) == 64, "size mismatch");

struct dsa_hw_comp_record {
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
		struct {
			uint64_t	rsvd;
			uint32_t	ref_tag;
			uint16_t	app_tag_mask;
			uint16_t	app_tag;
		} dif_ins_comp;
		struct {
			uint32_t	src_ref_tag;
			uint16_t	src_app_tag_mask;
			uint16_t	src_app_tag;
			uint32_t	dest_ref_tag;
			uint16_t	dest_app_tag_mask;
			uint16_t	dest_app_tag;
		} dif_upd_comp;
		uint8_t		op_specific[16];
	};
};
SPDK_STATIC_ASSERT(sizeof(struct dsa_hw_comp_record) == 32, "size mismatch");

struct iaa_hw_comp_record {
	volatile uint8_t	status;
	uint8_t			error_code;
	uint16_t		rsvd;
	uint32_t		bytes_completed;
	uint64_t		fault_addr;
	uint32_t		invalid_flags;
	uint32_t		rsvd2;
	uint32_t		output_size;
	uint8_t			output_bits;
	uint8_t			rsvd3;
	uint16_t		rsvd4;
	uint64_t		rsvd5[4];
};
SPDK_STATIC_ASSERT(sizeof(struct iaa_hw_comp_record) == 64, "size mismatch");

struct iaa_aecs {
	uint32_t crc;
	uint32_t xor_checksum;
	uint32_t rsvd[5];
	uint32_t num_output_accum_bits;
	uint8_t output_accum[256];
	uint32_t ll_sym[286];
	uint32_t rsvd1;
	uint32_t rsvd3;
	uint32_t d_sym[30];
	uint32_t pad[2];
};
SPDK_STATIC_ASSERT(sizeof(struct iaa_aecs) == 1568, "size mismatch");

union idxd_gencap_register {
	struct {
		uint64_t block_on_fault: 1;
		uint64_t overlap_copy: 1;
		uint64_t cache_control_mem: 1;
		uint64_t cache_control_cache: 1;
		uint64_t command_cap: 1;
		uint64_t rsvd: 3;
		uint64_t dest_readback: 1;
		uint64_t drain_readback: 1;
		uint64_t rsvd2: 6;
		uint64_t max_xfer_shift: 5;
		uint64_t max_batch_shift: 4;
		uint64_t max_ims_mult: 6;
		uint64_t config_support: 1;
		uint64_t rsvd3: 32;
	};
	uint64_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_gencap_register) == 8, "size mismatch");

union idxd_wqcap_register {
	struct {
		uint64_t total_wq_size: 16;
		uint64_t num_wqs: 8;
		uint64_t wqcfg_size: 4;
		uint64_t rsvd: 20;
		uint64_t shared_mode: 1;
		uint64_t dedicated_mode: 1;
		uint64_t ats_support: 1;
		uint64_t priority: 1;
		uint64_t occupancy: 1;
		uint64_t occupancy_int: 1;
		uint64_t rsvd1: 10;
	};
	uint64_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_wqcap_register) == 8, "size mismatch");

union idxd_groupcap_register {
	struct {
		uint64_t num_groups: 8;
		uint64_t read_bufs: 8;
		uint64_t read_bufs_ctrl: 1;
		uint64_t read_bus_limit: 1;
		uint64_t rsvd: 46;
	};
	uint64_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_groupcap_register) == 8, "size mismatch");

union idxd_enginecap_register {
	struct {
		uint64_t num_engines: 8;
		uint64_t rsvd: 56;
	};
	uint64_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_enginecap_register) == 8, "size mismatch");

struct idxd_opcap_register {
	uint64_t raw[4];
};
SPDK_STATIC_ASSERT(sizeof(struct idxd_opcap_register) == 32, "size mismatch");

union idxd_offsets_register {
	struct {
		uint64_t grpcfg: 16;
		uint64_t wqcfg: 16;
		uint64_t msix_perm: 16;
		uint64_t ims: 16;
		uint64_t perfmon: 16;
		uint64_t rsvd: 48;
	};
	uint64_t raw[2];
};
SPDK_STATIC_ASSERT(sizeof(union idxd_offsets_register) == 16, "size mismatch");

union idxd_gencfg_register {
	struct {
		uint8_t		global_read_buf_limit;
		uint8_t		reserved0 : 4;
		uint8_t		user_mode_int_enabled : 1;
		uint8_t		reserved1 : 3;
		uint16_t	reserved2;
	};
	uint32_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_gencfg_register) == 4, "size mismatch");

union idxd_genctrl_register {
	struct {
		uint32_t	sw_err_int_enable : 1;
		uint32_t	halt_state_int_enable : 1;
		uint32_t	reserved : 30;
	};
	uint32_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_genctrl_register) == 4, "size mismatch");

union idxd_gensts_register {
	struct {
		uint32_t state: 2;
		uint32_t reset_type: 2;
		uint32_t rsvd: 28;
	};
	uint32_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_gensts_register) == 4, "size mismatch");

union idxd_intcause_register {
	struct {
		uint32_t	software_err : 1;
		uint32_t	command_completion : 1;
		uint32_t	wq_occupancy_below_limit : 1;
		uint32_t	perfmon_counter_overflow : 1;
		uint32_t	halt_state : 1;
		uint32_t	reserved : 26;
		uint32_t	int_handles_revoked : 1;
	};
	uint32_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_intcause_register) == 4, "size mismatch");

union idxd_cmd_register {
	struct {
		uint32_t	operand : 20;
		uint32_t	command_code : 5;
		uint32_t	reserved : 6;
		uint32_t	request_completion_interrupt : 1;
	};
	uint32_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_cmd_register) == 4, "size mismatch");

union idxd_cmdsts_register {
	struct {
		uint32_t err : 8;
		uint32_t result : 16;
		uint32_t rsvd: 7;
		uint32_t active: 1;
	};
	uint32_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_cmdsts_register) == 4, "size mismatch");

union idxd_cmdcap_register {
	struct {
		uint32_t	reserved0 : 1;
		uint32_t	enable_device : 1;
		uint32_t	disable_device : 1;
		uint32_t	drain_all : 1;
		uint32_t	abort_all : 1;
		uint32_t	reset_device : 1;
		uint32_t	enable_wq : 1;
		uint32_t	disable_wq : 1;
		uint32_t	drain_wq : 1;
		uint32_t	abort_wq : 1;
		uint32_t	reset_wq : 1;
		uint32_t	drain_pasid : 1;
		uint32_t	abort_pasid : 1;
		uint32_t	request_int_handle : 1;
		uint32_t	release_int_handle : 1;
		uint32_t	reserved1 : 17;
	};
	uint32_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_cmdcap_register) == 4, "size mismatch");

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
	};
	uint64_t raw[4];
};
SPDK_STATIC_ASSERT(sizeof(union idxd_swerr_register) == 32, "size mismatch");

struct idxd_registers {
	uint32_t			version;
	uint32_t			reserved0;
	uint64_t			reserved1;
	union idxd_gencap_register	gencap;
	uint64_t			reserved2;
	union idxd_wqcap_register	wqcap;
	uint64_t			reserved3;
	union idxd_groupcap_register	groupcap;
	union idxd_enginecap_register	enginecap;
	struct idxd_opcap_register	opcap;
	union idxd_offsets_register	offsets;
	uint64_t			reserved4[2];
	union idxd_gencfg_register	gencfg;
	uint32_t			reserved5;
	union idxd_genctrl_register	genctrl;
	uint32_t			reserved6;
	union idxd_gensts_register	gensts;
	uint32_t			reserved7;
	union idxd_intcause_register	intcause;
	uint32_t			reserved8;
	union idxd_cmd_register		cmd;
	uint32_t			reserved9;
	union idxd_cmdsts_register	cmdsts;
	uint32_t			reserved10;
	union idxd_cmdcap_register	cmdcap;
	uint32_t			reserved11;
	uint64_t			reserved12;
	union idxd_swerr_register	sw_err;
};
SPDK_STATIC_ASSERT(sizeof(struct idxd_registers) == 0xE0, "size mismatch");

union idxd_group_flags {
	struct {
		uint32_t tc_a : 3;
		uint32_t tc_b : 3;
		uint32_t reserved0 : 1;
		uint32_t global_read_buffer_limit: 1;
		uint32_t read_buffers_reserved : 8;
		uint32_t reserved1: 4;
		uint32_t read_buffers_allowed : 8;
		uint32_t reserved2 : 4;
	};
	uint32_t raw;
};
SPDK_STATIC_ASSERT(sizeof(union idxd_group_flags) == 4, "size mismatch");

struct idxd_grpcfg {
	uint64_t wqs[4];
	uint64_t engines;
	union idxd_group_flags flags;

	/* This is not part of the definition, but in practice the stride in the table
	 * is 64 bytes. */
	uint32_t reserved0;
	uint64_t reserved1[2];
};
SPDK_STATIC_ASSERT(sizeof(struct idxd_grpcfg) == 64, "size mismatch");

struct idxd_grptbl {
	struct idxd_grpcfg group[1];
};

union idxd_wqcfg {
	struct {
		uint16_t wq_size;
		uint16_t rsvd;
		uint16_t wq_thresh;
		uint16_t rsvd1;
		uint32_t mode: 1;
		uint32_t bof: 1;
		uint32_t wq_ats_disable: 1;
		uint32_t rsvd2: 1;
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
	};
	uint32_t raw[8];
};
SPDK_STATIC_ASSERT(sizeof(union idxd_wqcfg) == 32, "size mismatch");

#ifdef __cplusplus
}
#endif

#endif /* SPDK_IDXD_SPEC_H */
