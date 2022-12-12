/*
 * Copyright (c) 2021, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * This software product is a proprietary product of NVIDIA CORPORATION &
 * AFFILIATES (the "Company") and all right, title, and interest in and to the
 * software product, including all associated intellectual property rights, are
 * and shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 */

#ifndef __VRDMA_DPA_COM_H__
#define __VRDMA_DPA_COM_H__
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define fence_all() do { \
	asm volatile("fence iorw, iorw" ::: "memory"); \
} while(0)

#define fence_io()  do { \
	asm volatile("fence io, io"     ::: "memory"); \
} while(0)

#define fence_ow()  do { \
	asm volatile("fence ow, ow"     ::: "memory"); \
} while (0)

#define fence_rw()  do { \
	asm volatile("fence rw, rw"     ::: "memory"); \
} while (0)

#define fence_i() do { \
	asm volatile("fence i, i"       ::: "memory"); \
} while (0)

#define fence_o() do { \
	asm volatile("fence o, o"       ::: "memory"); \
} while (0)

#define fence_r() do { \
	asm volatile("fence r, r"       ::: "memory"); \
} while (0)

#define fence_w() do { \
	asm volatile("fence w, w"       ::: "memory"); \
} while (0)

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define MINIMUM(a, b) ((a)<(b) ? (a) : (b))

#if defined(E_MODE_LE)
#define le16_to_cpu(val) (val)
#define le32_to_cpu(val) (val)
#define le64_to_cpu(val) (val)
#define be16_to_cpu(val) __builtin_bswap16(val)
#define be32_to_cpu(val) __builtin_bswap32(val)
#define be64_to_cpu(val) __builtin_bswap64(val)

#define cpu_to_le16(val) (val)
#define cpu_to_le32(val) (val)
#define cpu_to_le64(val) (val)
#define cpu_to_be16(val) __builtin_bswap16(val)
#define cpu_to_be32(val) __builtin_bswap32(val)
#define cpu_to_be64(val) __builtin_bswap64(val)

#elif defined(E_MODE_BE)
#define le16_to_cpu(val) __builtin_bswap16(val)
#define le32_to_cpu(val) __builtin_bswap32(val)
#define le64_to_cpu(val) __builtin_bswap64(val)
#define be16_to_cpu(val) (val)
#define be32_to_cpu(val) (val)
#define be64_to_cpu(val) (val)

#define cpu_to_le16(val) __builtin_bswap16(val)
#define cpu_to_le32(val) __builtin_bswap32(val)
#define cpu_to_le64(val) __builtin_bswap64(val)
#define cpu_to_be16(val) (val)
#define cpu_to_be32(val) (val)
#define cpu_to_be64(val) (val)

#else
# error incorrect BYTE_ORDER
#endif

#endif

