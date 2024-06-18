/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_MEMORY_H
#define SPDK_MEMORY_H

#include "spdk/stdinc.h"

#ifndef __linux__
#define VFIO_ENABLED 0
#else
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0)
#define VFIO_ENABLED 1
#else
#define VFIO_ENABLED 0
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SHIFT_2MB		21 /* (1 << 21) == 2MB */
#define VALUE_2MB		(1ULL << SHIFT_2MB)
#define MASK_2MB		(VALUE_2MB - 1)

#define SHIFT_4KB		12 /* (1 << 12) == 4KB */
#define VALUE_4KB		(1ULL << SHIFT_4KB)
#define MASK_4KB		(VALUE_4KB - 1)

#define _2MB_OFFSET(ptr)	(((uintptr_t)(ptr)) & MASK_2MB)
#define _2MB_PAGE(ptr)		FLOOR_2MB((uintptr_t)(ptr))
#define FLOOR_2MB(x)		(((uintptr_t)(x)) & ~MASK_2MB)
#define CEIL_2MB(x)		FLOOR_2MB(((uintptr_t)(x)) + VALUE_2MB - 1)

#ifdef __cplusplus
}
#endif

#endif /* SPDK_MEMORY_H */
