/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
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

/* Allocate 2 << 15 (32K) descriptors per channel by default. */
#define IOAT_DEFAULT_ORDER			15

#ifdef __x86_64__
#define IOAT_64BIT_IO	1 /* Can do atomic 64-bit memory read/write (over PCIe) */
#else
#define IOAT_64BIT_IO	0
#endif

struct ioat_descriptor {
	ioat_callback_t		callback_fn;
	void			*callback_arg;
	union {
		struct ioat_dma_hw_descriptor		*dma;
		struct ioat_fill_hw_descriptor		*fill;
		struct ioat_xor_hw_descriptor		*xor;
		struct ioat_xor_ext_hw_descriptor	*xor_ext;
		struct ioat_pq_hw_descriptor		*pq;
		struct ioat_pq_ext_hw_descriptor	*pq_ext;
		struct ioat_raw_hw_descriptor		*raw;
	} u;
	uint64_t		hw_desc_bus_addr;
};

/* One of these per allocated PCI device. */
struct ioat_channel {
	SLIST_ENTRY(ioat_channel) next;

	/* Opaque handle to upper layer */
	void                *device;
	uint64_t            max_xfer_size;
	volatile struct ioat_registers *regs;

	volatile uint64_t   *comp_update;

	uint32_t            head;
	uint32_t            tail;

	uint32_t            ring_size_order;
	uint64_t            last_seen;

	struct ioat_descriptor **ring;
};

static inline uint32_t
is_ioat_active(uint64_t status)
{
	return (status & IOAT_CHANSTS_STATUS) == IOAT_CHANSTS_ACTIVE;
}

static inline uint32_t
is_ioat_idle(uint64_t status)
{
	return (status & IOAT_CHANSTS_STATUS) == IOAT_CHANSTS_IDLE;
}

static inline uint32_t
is_ioat_halted(uint64_t status)
{
	return (status & IOAT_CHANSTS_STATUS) == IOAT_CHANSTS_HALTED;
}

static inline uint32_t
is_ioat_suspended(uint64_t status)
{
	return (status & IOAT_CHANSTS_STATUS) == IOAT_CHANSTS_SUSPENDED;
}

#endif /* __IOAT_INTERNAL_H__ */
