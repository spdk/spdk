/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Advanced Micro Devices, Inc.
 *   All rights reserved.
 */

#ifndef __AE4DMA_INTERNAL_H__
#define __AE4DMA_INTERNAL_H__

#include "spdk/stdinc.h"
#include "spdk/ae4dma.h"
#include "ae4dma_spec.h"
#include "spdk/queue.h"
#include "spdk/mmio.h"
/**
 * upper_32_bits - return bits 32-63 of a number
 * @n: the number we're accessing
 */
#define upper_32_bits(n) ((uint32_t)(((n) >> 16) >> 16))

/**
 * lower_32_bits - return bits 0-31 of a number
 * @n: the number we're accessing
 */
#define lower_32_bits(n) ((uint32_t)((n) & 0xffffffff))

#define AE4DMA_DESCRIPTORS_PER_CMDQ 32
#define AE4DMA_QUEUE_DESC_SIZE  sizeof(struct spdk_ae4dma_desc)
#define AE4DMA_QUEUE_SIZE(n)  (AE4DMA_DESCRIPTORS_PER_CMDQ * (n))

struct ae4dma_descriptor {
	spdk_ae4dma_req_cb	callback_fn;
	void			*callback_arg;
};

struct ae4dma_cmd_queue {
	volatile struct spdk_ae4dma_hwq_regs *regs;

	/* Queue base address */
	struct spdk_ae4dma_desc *qbase_addr;

	struct ae4dma_descriptor *ring;

	uint64_t tail;
	unsigned int queue_size;
	uint64_t qring_buffer_pa;
	uint64_t qdma_tail;

	/* Queue Statistics */
	uint32_t write_index;
	uint32_t ring_buff_count;
};

struct spdk_ae4dma_chan {
	/* Opaque handle to upper layer */
	struct    spdk_pci_device *device;
	uint64_t  max_xfer_size;

	/* I/O area used for device communication */
	void *io_regs;

	struct ae4dma_cmd_queue cmd_q[AE4DMA_MAX_HW_QUEUES];
	unsigned int cmd_q_count;
	uint32_t	dma_capabilities;

	/* tailq entry for attached_chans */
	TAILQ_ENTRY(spdk_ae4dma_chan)	tailq;
};

/* This function verifies if the command queue is full */
static inline bool
ae4dma_desc_cmdq_full(uint8_t count)
{
	if (count >= (AE4DMA_DESCRIPTORS_PER_CMDQ - 4)) {
		return true;
	} else {
		return false;
	}
}

/* This function verifies the number of queues that can be configured for a ae4dma device */
static inline bool
ae4dma_config_queues_per_device(uint8_t num_hw_queues)
{
	if (num_hw_queues <= AE4DMA_MAX_HW_QUEUES) {
		return false;
	} else {
		return true;
	}
}

#endif /* __AE4DMA_INTERNAL_H__ */
