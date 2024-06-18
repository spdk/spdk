/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation. All rights reserved.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/util.h"
#include "spdk/assert.h"

/* The following will automatically generate several version of
 * this function, targeted at different architectures. This
 * is only supported by GCC 6 or newer. */
#if defined(__GNUC__) && __GNUC__ >= 6 && !defined(__clang__) \
	&& (defined(__i386__) || defined(__x86_64__)) \
	&& defined(__ELF__)
__attribute__((target_clones("bmi", "arch=core2", "arch=atom", "default")))
#endif
uint32_t
spdk_u32log2(uint32_t x)
{
	if (x == 0) {
		/* log(0) is undefined */
		return 0;
	}
	SPDK_STATIC_ASSERT(sizeof(x) == sizeof(unsigned int), "Incorrect size");
	return 31u - __builtin_clz(x);
}

/* The following will automatically generate several version of
 * this function, targeted at different architectures. This
 * is only supported by GCC 6 or newer. */
#if defined(__GNUC__) && __GNUC__ >= 6 && !defined(__clang__) \
	&& (defined(__i386__) || defined(__x86_64__)) \
	&& defined(__ELF__)
__attribute__((target_clones("bmi", "arch=core2", "arch=atom", "default")))
#endif
uint64_t
spdk_u64log2(uint64_t x)
{
	if (x == 0) {
		/* log(0) is undefined */
		return 0;
	}
	SPDK_STATIC_ASSERT(sizeof(x) == sizeof(unsigned long long), "Incorrect size");
	return 63u - __builtin_clzll(x);
}
