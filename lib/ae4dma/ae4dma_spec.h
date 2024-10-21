/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Advanced Micro Devices, Inc.
 *   All rights reserved.
 */

/**
 * AE4DMA specification definitions
 */

#ifndef SPDK_AE4DMA_SPEC_H
#define SPDK_AE4DMA_SPEC_H

#include "spdk/stdinc.h"
#include "spdk/assert.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * An AE4DMA engine has 16 DMA queues. Each queue supports 32 descriptors
 */

#define AE4DMA_MAX_HW_QUEUES		16
#define AE4DMA_QUEUE_START_INDEX	0
#define AE4DMA_CMD_QUEUE_ENABLE	0x1

/** Common to all queues */
#define AE4DMA_COMMON_CONFIG_OFFSET 0x00
#define AE4DMA_PCIE_BAR 0

/* Descriptor status */
enum spdk_ae4dma_dma_status {
	AE4DMA_DMA_DESC_SUBMITTED = 0,
	AE4DMA_DMA_DESC_VALIDATED = 1,
	AE4DMA_DMA_DESC_PROCESSED = 2,
	AE4DMA_DMA_DESC_COMPLETED = 3,
	AE4DMA_DMA_DESC_ERROR = 4,
};

/* HW Queue status */
enum spdk_ae4dma_hwqueue_status {
	AE4DMA_HWQUEUE_EMPTY = 0,
	AE4DMA_HWQUEUE_FULL = 1,
	AE4DMA_HWQUEUE_NOT_EMPTY = 4
};

/*
 * descriptor for AE4DMA commands
 * 8 32-bit words:
 * word 0: source memory type; destination memory type ; control bits
 * word 1: desc_id; error code; status
 * word 2: length
 * word 3: reserved
 * word 4: upper 32 bits of source pointer
 * word 5: low 32 bits of source pointer
 * word 6: upper 32 bits of destination pointer
 * word 7: low 32 bits of destination pointer
 */

/* AE4DMA Descriptor - DWORD0 - Controls bits: Reserved for future use */
#define AE4DMA_DWORD0_STOP_ON_COMPLETION	BIT(0)
#define AE4DMA_DWORD0_INTERRUPT_ON_COMPLETION	BIT(1)
#define AE4DMA_DWORD0_START_OF_MESSAGE		BIT(3)
#define AE4DMA_DWORD0_END_OF_MESSAGE		BIT(4)
#define AE4DMA_DWORD0_DESTINATION_MEMORY_TYPE	GENMASK(5, 4)
#define AE4DMA_DWORD0_SOURCE_MEMEORY_TYPE	GENMASK(7, 6)

#define AE4DMA_DWORD0_DESTINATION_MEMORY_TYPE_MEMORY	0x0
#define AE4DMA_DWORD0_DESTINATION_MEMORY_TYPE_IOMEMORY	(1<<4)
#define AE4DMA_DWORD0_SOURCE_MEMEORY_TYPE_MEMORY	0x0
#define AE4DMA_DWORD0_SOURCE_MEMEORY_TYPE_IOMEMORY	(1<<6)

struct spdk_ae4dma_desc_dword0 {
	uint8_t	byte0;
	uint8_t	byte1;
	uint16_t timestamp;
};

struct spdk_ae4dma_desc_dword1 {
	uint8_t	status;
	uint8_t	err_code;
	uint16_t desc_id;
};

struct spdk_ae4dma_desc {
	struct spdk_ae4dma_desc_dword0 dw0;
	struct spdk_ae4dma_desc_dword1 dw1;
	uint32_t length;
	uint32_t reserved;
	uint32_t src_lo;
	uint32_t src_hi;
	uint32_t dst_lo;
	uint32_t dst_hi;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_ae4dma_desc) == 32, "incorrect ae4dma_hw_desc layout");

/*
 * Registers for each queue :4 bytes length
 * Effective address : offset + reg
 */

struct spdk_ae4dma_hwq_regs {
	union {
		uint32_t control_raw;
		struct {
			uint32_t queue_enable: 1;
			uint32_t reserved_internal: 31;
		} control;
	} control_reg;

	union {
		uint32_t status_raw;
		struct {
			uint32_t reserved0: 1;
			uint32_t queue_status: 2; /* 0–empty, 1–full, 2–stopped, 3–error , 4–Not Empty */
			uint32_t reserved1: 21;
			uint32_t interrupt_type: 4;
			uint32_t reserved2: 4;
		} status;
	} status_reg;

	uint32_t max_idx;
	uint32_t read_idx;
	uint32_t write_idx;

	union {
		uint32_t intr_status_raw;
		struct {
			uint32_t intr_status: 1;
			uint32_t reserved: 31;
		} intr_status;
	} intr_status_reg;

	uint32_t qbase_lo;
	uint32_t qbase_hi;

} __attribute__((packed)) __attribute__((aligned));


#ifdef __cplusplus
}
#endif

#endif /* SPDK_AE4DMA_SPEC_H */
