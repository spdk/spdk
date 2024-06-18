/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

#ifndef __IOAT_INTERNAL_H__
#define __IOAT_INTERNAL_H__

#include "spdk/stdinc.h"

#include "spdk/ioat.h"
#include "spdk/ioat_spec.h"
#include "spdk/queue.h"
#include "spdk/mmio.h"

/* Allocate 1 << 15 (32K) descriptors per channel by default. */
#define IOAT_DEFAULT_ORDER			15

struct ioat_descriptor {
	uint64_t		phys_addr;
	spdk_ioat_req_cb	callback_fn;
	void			*callback_arg;
};

/* One of these per allocated PCI device. */
struct spdk_ioat_chan {
	/* Opaque handle to upper layer */
	struct spdk_pci_device		*device;
	uint64_t            max_xfer_size;
	volatile struct spdk_ioat_registers *regs;

	volatile uint64_t   *comp_update;

	uint32_t            head;
	uint32_t            tail;

	uint32_t            ring_size_order;
	uint64_t            last_seen;

	struct ioat_descriptor		*ring;
	union spdk_ioat_hw_desc		*hw_ring;
	uint32_t			dma_capabilities;

	/* tailq entry for attached_chans */
	TAILQ_ENTRY(spdk_ioat_chan)	tailq;
};

static inline uint32_t
is_ioat_active(uint64_t status)
{
	return (status & SPDK_IOAT_CHANSTS_STATUS) == SPDK_IOAT_CHANSTS_ACTIVE;
}

static inline uint32_t
is_ioat_idle(uint64_t status)
{
	return (status & SPDK_IOAT_CHANSTS_STATUS) == SPDK_IOAT_CHANSTS_IDLE;
}

static inline uint32_t
is_ioat_halted(uint64_t status)
{
	return (status & SPDK_IOAT_CHANSTS_STATUS) == SPDK_IOAT_CHANSTS_HALTED;
}

static inline uint32_t
is_ioat_suspended(uint64_t status)
{
	return (status & SPDK_IOAT_CHANSTS_STATUS) == SPDK_IOAT_CHANSTS_SUSPENDED;
}

#endif /* __IOAT_INTERNAL_H__ */
