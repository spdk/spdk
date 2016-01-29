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

#ifndef __IOAT_SPEC_H__
#define __IOAT_SPEC_H__

#include <inttypes.h>

#include "spdk/assert.h"

#define IOAT_INTRCTRL_MASTER_INT_EN	0x01

#define IOAT_VER_3_0                0x30
#define IOAT_VER_3_3                0x33

/* DMA Channel Registers */
#define IOAT_CHANCTRL_CHANNEL_PRIORITY_MASK	0xF000
#define IOAT_CHANCTRL_COMPL_DCA_EN		0x0200
#define IOAT_CHANCTRL_CHANNEL_IN_USE		0x0100
#define IOAT_CHANCTRL_DESCRIPTOR_ADDR_SNOOP_CONTROL	0x0020
#define IOAT_CHANCTRL_ERR_INT_EN		0x0010
#define IOAT_CHANCTRL_ANY_ERR_ABORT_EN		0x0008
#define IOAT_CHANCTRL_ERR_COMPLETION_EN		0x0004
#define IOAT_CHANCTRL_INT_REARM			0x0001

/* DMA Channel Capabilities */
#define	IOAT_DMACAP_PB			(1 << 0)
#define	IOAT_DMACAP_DCA			(1 << 4)
#define	IOAT_DMACAP_BFILL		(1 << 6)
#define	IOAT_DMACAP_XOR			(1 << 8)
#define	IOAT_DMACAP_PQ			(1 << 9)
#define	IOAT_DMACAP_DMA_DIF		(1 << 10)

struct ioat_registers {
	uint8_t		chancnt;
	uint8_t		xfercap;
	uint8_t		genctrl;
	uint8_t		intrctrl;
	uint32_t	attnstatus;
	uint8_t		cbver;		/* 0x08 */
	uint8_t		reserved4[0x3]; /* 0x09 */
	uint16_t	intrdelay;	/* 0x0C */
	uint16_t	cs_status;	/* 0x0E */
	uint32_t	dmacapability;	/* 0x10 */
	uint8_t		reserved5[0x6C]; /* 0x14 */
	uint16_t	chanctrl;	/* 0x80 */
	uint8_t		reserved6[0x2];	/* 0x82 */
	uint8_t		chancmd;	/* 0x84 */
	uint8_t		reserved3[1];	/* 0x85 */
	uint16_t	dmacount;	/* 0x86 */
	uint64_t	chansts;	/* 0x88 */
	uint64_t	chainaddr;	/* 0x90 */
	uint64_t	chancmp;	/* 0x98 */
	uint8_t		reserved2[0x8];	/* 0xA0 */
	uint32_t	chanerr;	/* 0xA8 */
	uint32_t	chanerrmask;	/* 0xAC */
} __attribute__((packed));

#define IOAT_CHANCMD_RESET		0x20
#define IOAT_CHANCMD_SUSPEND		0x04

#define IOAT_CHANSTS_STATUS		0x7ULL
#define IOAT_CHANSTS_ACTIVE		0x0
#define IOAT_CHANSTS_IDLE		0x1
#define IOAT_CHANSTS_SUSPENDED		0x2
#define IOAT_CHANSTS_HALTED		0x3
#define IOAT_CHANSTS_ARMED		0x4

#define IOAT_CHANSTS_UNAFFILIATED_ERROR	0x8ULL
#define IOAT_CHANSTS_SOFT_ERROR		0x10ULL

#define IOAT_CHANSTS_COMPLETED_DESCRIPTOR_MASK	(~0x3FULL)

#define IOAT_CHANCMP_ALIGN		8	/* CHANCMP address must be 64-bit aligned */

struct ioat_generic_hw_descriptor {
	uint32_t size;
	union {
		uint32_t control_raw;
		struct {
			uint32_t int_enable: 1;
			uint32_t src_snoop_disable: 1;
			uint32_t dest_snoop_disable: 1;
			uint32_t completion_update: 1;
			uint32_t fence: 1;
			uint32_t reserved2: 1;
			uint32_t src_page_break: 1;
			uint32_t dest_page_break: 1;
			uint32_t bundle: 1;
			uint32_t dest_dca: 1;
			uint32_t hint: 1;
			uint32_t reserved: 13;
			uint32_t op: 8;
		} control;
	} u;
	uint64_t src_addr;
	uint64_t dest_addr;
	uint64_t next;
	uint64_t op_specific[4];
};

struct ioat_dma_hw_descriptor {
	uint32_t size;
	union {
		uint32_t control_raw;
		struct {
			uint32_t int_enable: 1;
			uint32_t src_snoop_disable: 1;
			uint32_t dest_snoop_disable: 1;
			uint32_t completion_update: 1;
			uint32_t fence: 1;
			uint32_t null: 1;
			uint32_t src_page_break: 1;
			uint32_t dest_page_break: 1;
			uint32_t bundle: 1;
			uint32_t dest_dca: 1;
			uint32_t hint: 1;
			uint32_t reserved: 13;
#define IOAT_OP_COPY 0x00
			uint32_t op: 8;
		} control;
	} u;
	uint64_t src_addr;
	uint64_t dest_addr;
	uint64_t next;
	uint64_t reserved;
	uint64_t reserved2;
	uint64_t user1;
	uint64_t user2;
};

struct ioat_fill_hw_descriptor {
	uint32_t size;
	union {
		uint32_t control_raw;
		struct {
			uint32_t int_enable: 1;
			uint32_t reserved: 1;
			uint32_t dest_snoop_disable: 1;
			uint32_t completion_update: 1;
			uint32_t fence: 1;
			uint32_t reserved2: 2;
			uint32_t dest_page_break: 1;
			uint32_t bundle: 1;
			uint32_t reserved3: 15;
#define IOAT_OP_FILL 0x01
			uint32_t op: 8;
		} control;
	} u;
	uint64_t src_data;
	uint64_t dest_addr;
	uint64_t next;
	uint64_t reserved;
	uint64_t next_dest_addr;
	uint64_t user1;
	uint64_t user2;
};

struct ioat_xor_hw_descriptor {
	uint32_t size;
	union {
		uint32_t control_raw;
		struct {
			uint32_t int_enable: 1;
			uint32_t src_snoop_disable: 1;
			uint32_t dest_snoop_disable: 1;
			uint32_t completion_update: 1;
			uint32_t fence: 1;
			uint32_t src_count: 3;
			uint32_t bundle: 1;
			uint32_t dest_dca: 1;
			uint32_t hint: 1;
			uint32_t reserved: 13;
#define IOAT_OP_XOR 0x87
#define IOAT_OP_XOR_VAL 0x88
			uint32_t op: 8;
		} control;
	} u;
	uint64_t src_addr;
	uint64_t dest_addr;
	uint64_t next;
	uint64_t src_addr2;
	uint64_t src_addr3;
	uint64_t src_addr4;
	uint64_t src_addr5;
};

struct ioat_xor_ext_hw_descriptor {
	uint64_t src_addr6;
	uint64_t src_addr7;
	uint64_t src_addr8;
	uint64_t next;
	uint64_t reserved[4];
};

struct ioat_pq_hw_descriptor {
	uint32_t size;
	union {
		uint32_t control_raw;
		struct {
			uint32_t int_enable: 1;
			uint32_t src_snoop_disable: 1;
			uint32_t dest_snoop_disable: 1;
			uint32_t completion_update: 1;
			uint32_t fence: 1;
			uint32_t src_count: 3;
			uint32_t bundle: 1;
			uint32_t dest_dca: 1;
			uint32_t hint: 1;
			uint32_t p_disable: 1;
			uint32_t q_disable: 1;
			uint32_t reserved: 11;
#define IOAT_OP_PQ 0x89
#define IOAT_OP_PQ_VAL 0x8a
			uint32_t op: 8;
		} control;
	} u;
	uint64_t src_addr;
	uint64_t p_addr;
	uint64_t next;
	uint64_t src_addr2;
	uint64_t src_addr3;
	uint8_t  coef[8];
	uint64_t q_addr;
};

struct ioat_pq_ext_hw_descriptor {
	uint64_t src_addr4;
	uint64_t src_addr5;
	uint64_t src_addr6;
	uint64_t next;
	uint64_t src_addr7;
	uint64_t src_addr8;
	uint64_t reserved[2];
};

struct ioat_pq_update_hw_descriptor {
	uint32_t size;
	union {
		uint32_t control_raw;
		struct {
			uint32_t int_enable: 1;
			uint32_t src_snoop_disable: 1;
			uint32_t dest_snoop_disable: 1;
			uint32_t completion_update: 1;
			uint32_t fence: 1;
			uint32_t src_cnt: 3;
			uint32_t bundle: 1;
			uint32_t dest_dca: 1;
			uint32_t hint: 1;
			uint32_t p_disable: 1;
			uint32_t q_disable: 1;
			uint32_t reserved: 3;
			uint32_t coef: 8;
#define IOAT_OP_PQ_UP 0x8b
			uint32_t op: 8;
		} control;
	} u;
	uint64_t src_addr;
	uint64_t p_addr;
	uint64_t next;
	uint64_t src_addr2;
	uint64_t p_src;
	uint64_t q_src;
	uint64_t q_addr;
};

struct ioat_raw_hw_descriptor {
	uint64_t field[8];
};

union ioat_hw_descriptor {
	struct ioat_raw_hw_descriptor raw;
	struct ioat_generic_hw_descriptor generic;
	struct ioat_dma_hw_descriptor dma;
	struct ioat_fill_hw_descriptor fill;
	struct ioat_xor_hw_descriptor xor;
	struct ioat_xor_ext_hw_descriptor xor_ext;
	struct ioat_pq_hw_descriptor pq;
	struct ioat_pq_ext_hw_descriptor pq_ext;
	struct ioat_pq_update_hw_descriptor pq_update;
};
SPDK_STATIC_ASSERT(sizeof(union ioat_hw_descriptor) == 64, "incorrect ioat_hw_descriptor layout");

#endif /* __IOAT_SPEC_H__ */
