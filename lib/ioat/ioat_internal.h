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

#ifndef __IOAT_INTERNAL_H__
#define __IOAT_INTERNAL_H__

#include "spdk/ioat.h"
#include "spdk/ioat_spec.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "spdk/queue.h"
#include "spdk/mmio.h"

/* Allocate 2 << 15 (32K) descriptors per channel by default. */
#define IOAT_DEFAULT_ORDER			15

struct ioat_descriptor {
	spdk_ioat_req_cb	callback_fn;
	void			*callback_arg;
};

/* One of these per allocated PCI device. */
struct spdk_ioat_chan {
	SLIST_ENTRY(spdk_ioat_chan) next;

	/* Opaque handle to upper layer */
	void                *device;
	uint64_t            max_xfer_size;
	volatile struct spdk_ioat_registers *regs;

	volatile uint64_t   *comp_update;

	uint32_t            head;
	uint32_t            tail;

	uint32_t            ring_size_order;
	uint64_t            last_seen;

	struct ioat_descriptor		*ring;
	union spdk_ioat_hw_desc		*hw_ring;
	uint64_t			hw_ring_phys_addr;
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
