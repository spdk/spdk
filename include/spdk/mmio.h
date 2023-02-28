/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * Memory-mapped I/O utility functions
 */

#ifndef SPDK_MMIO_H
#define SPDK_MMIO_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/barrier.h"

#ifdef __x86_64__
#define SPDK_MMIO_64BIT	1 /* Can do atomic 64-bit memory read/write (over PCIe) */
#else
#define SPDK_MMIO_64BIT	0
#endif

static inline uint8_t
spdk_mmio_read_1(const volatile uint8_t *addr)
{
	spdk_compiler_barrier();
	return *addr;
}

static inline void
spdk_mmio_write_1(volatile uint8_t *addr, uint8_t val)
{
	spdk_compiler_barrier();
	*addr = val;
}

static inline uint16_t
spdk_mmio_read_2(const volatile uint16_t *addr)
{
	spdk_compiler_barrier();
	return *addr;
}

static inline void
spdk_mmio_write_2(volatile uint16_t *addr, uint16_t val)
{
	spdk_compiler_barrier();
	*addr = val;
}

static inline uint32_t
spdk_mmio_read_4(const volatile uint32_t *addr)
{
	spdk_compiler_barrier();
	return *addr;
}

static inline void
spdk_mmio_write_4(volatile uint32_t *addr, uint32_t val)
{
	spdk_compiler_barrier();
	*addr = val;
}

static inline uint64_t
spdk_mmio_read_8(volatile uint64_t *addr)
{
	uint64_t val;
	volatile uint32_t *addr32 = (volatile uint32_t *)addr;

	spdk_compiler_barrier();

	if (SPDK_MMIO_64BIT) {
		val = *addr;
	} else {
		/*
		 * Read lower 4 bytes before upper 4 bytes.
		 * This particular order is required by I/OAT.
		 * If the other order is required, use a pair of spdk_mmio_read_4() calls.
		 */
		val = addr32[0];
		val |= (uint64_t)addr32[1] << 32;
	}

	return val;
}

static inline void
spdk_mmio_write_8(volatile uint64_t *addr, uint64_t val)
{
	volatile uint32_t *addr32 = (volatile uint32_t *)addr;

	spdk_compiler_barrier();

	if (SPDK_MMIO_64BIT) {
		*addr = val;
	} else {
		addr32[0] = (uint32_t)val;
		addr32[1] = (uint32_t)(val >> 32);
	}
}

#ifdef __cplusplus
}
#endif

#endif
