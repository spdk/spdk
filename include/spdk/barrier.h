/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   Copyright (c) 2017, IBM Corporation.
 *   All rights reserved.
 */

/** \file
 * Memory barriers
 */

#ifndef SPDK_BARRIER_H
#define SPDK_BARRIER_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Compiler memory barrier */
#define spdk_compiler_barrier() __asm volatile("" ::: "memory")

/** Read memory barrier */
#define spdk_rmb()	_spdk_rmb()

/** Write memory barrier */
#define spdk_wmb()	_spdk_wmb()

/** Full read/write memory barrier */
#define spdk_mb()	_spdk_mb()

/** SMP read memory barrier. */
#define spdk_smp_rmb()	_spdk_smp_rmb()

/** SMP write memory barrier. */
#define spdk_smp_wmb()	_spdk_smp_wmb()

/** SMP read/write memory barrier. */
#define spdk_smp_mb()	_spdk_smp_mb()

/** Invalidate data cache, input is data pointer */
#define spdk_ivdt_dcache(pdata)	_spdk_ivdt_dcache(pdata)

#ifdef __PPC64__

#define _spdk_rmb()	__asm volatile("sync" ::: "memory")
#define _spdk_wmb()	__asm volatile("sync" ::: "memory")
#define _spdk_mb()	__asm volatile("sync" ::: "memory")
#define _spdk_smp_rmb()	__asm volatile("lwsync" ::: "memory")
#define _spdk_smp_wmb()	__asm volatile("lwsync" ::: "memory")
#define _spdk_smp_mb()	spdk_mb()
#define _spdk_ivdt_dcache(pdata)

#elif defined(__aarch64__)

#define _spdk_rmb()	__asm volatile("dsb ld" ::: "memory")
#define _spdk_wmb()	__asm volatile("dsb st" ::: "memory")
#define _spdk_mb()	__asm volatile("dsb sy" ::: "memory")
#define _spdk_smp_rmb()	__asm volatile("dmb ishld" ::: "memory")
#define _spdk_smp_wmb()	__asm volatile("dmb ishst" ::: "memory")
#define _spdk_smp_mb()	__asm volatile("dmb ish" ::: "memory")
#define _spdk_ivdt_dcache(pdata)	asm volatile("dc civac, %0" : : "r"(pdata) : "memory");

#elif defined(__i386__) || defined(__x86_64__)

#define _spdk_rmb()	__asm volatile("lfence" ::: "memory")
#define _spdk_wmb()	__asm volatile("sfence" ::: "memory")
#define _spdk_mb()	__asm volatile("mfence" ::: "memory")
#define _spdk_smp_rmb()	spdk_compiler_barrier()
#define _spdk_smp_wmb()	spdk_compiler_barrier()
#if defined(__x86_64__)
#define _spdk_smp_mb()	__asm volatile("lock addl $0, -128(%%rsp); " ::: "memory");
#elif defined(__i386__)
#define _spdk_smp_mb()	__asm volatile("lock addl $0, -128(%%esp); " ::: "memory");
#endif
#define _spdk_ivdt_dcache(pdata)

#elif defined(__riscv)

#define _spdk_rmb()	__asm__ __volatile__("fence ir, ir" ::: "memory")
#define _spdk_wmb()	__asm__ __volatile__("fence ow, ow" ::: "memory")
#define _spdk_mb()	__asm__ __volatile__("fence iorw, iorw" ::: "memory")
#define _spdk_smp_rmb()	__asm__ __volatile__("fence r, r" ::: "memory")
#define _spdk_smp_wmb()	__asm__ __volatile__("fence w, w" ::: "memory")
#define _spdk_smp_mb()	__asm__ __volatile__("fence rw, rw" ::: "memory")
#define _spdk_ivdt_dcache(pdata)

#elif defined(__loongarch__)

#define _spdk_rmb()	__asm volatile("dbar 0" ::: "memory")
#define _spdk_wmb()	__asm volatile("dbar 0" ::: "memory")
#define _spdk_mb()	__asm volatile("dbar 0" ::: "memory")
#define _spdk_smp_rmb()	__asm volatile("dbar 0" ::: "memory")
#define _spdk_smp_wmb()	__asm volatile("dbar 0" ::: "memory")
#define _spdk_smp_mb()	__asm volatile("dbar 0" ::: "memory")
#define _spdk_ivdt_dcache(pdata)

#else

#define _spdk_rmb()
#define _spdk_wmb()
#define _spdk_mb()
#define _spdk_smp_rmb()
#define _spdk_smp_wmb()
#define _spdk_smp_mb()
#define _spdk_ivdt_dcache(pdata)
#error Unknown architecture

#endif

#ifdef __cplusplus
}
#endif

#endif
