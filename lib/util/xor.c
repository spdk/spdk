/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/xor.h"
#include "spdk/config.h"
#include "spdk/assert.h"
#include "spdk/util.h"

/* maximum number of source buffers */
#define SPDK_XOR_MAX_SRC	256

static inline bool
is_aligned(void *ptr, size_t alignment)
{
	uintptr_t p = (uintptr_t)ptr;

	return p == SPDK_ALIGN_FLOOR(p, alignment);
}

static bool
buffers_aligned(void *dest, void **sources, uint32_t n, size_t alignment)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		if (!is_aligned(sources[i], alignment)) {
			return false;
		}
	}

	return is_aligned(dest, alignment);
}

static void
xor_gen_unaligned(void *dest, void **sources, uint32_t n, uint32_t len)
{
	uint32_t i, j;

	for (i = 0; i < len; i++) {
		uint8_t b = 0;

		for (j = 0; j < n; j++) {
			b ^= ((uint8_t *)sources[j])[i];
		}
		((uint8_t *)dest)[i] = b;
	}
}

static void
xor_gen_basic(void *dest, void **sources, uint32_t n, uint32_t len)
{
	uint32_t shift;
	uint32_t len_div, len_rem;
	uint32_t i, j;

	if (!buffers_aligned(dest, sources, n, sizeof(uint64_t))) {
		xor_gen_unaligned(dest, sources, n, len);
		return;
	}

	shift = spdk_u32log2(sizeof(uint64_t));
	len_div = len >> shift;
	len_rem = len_div << shift;

	for (i = 0; i < len_div; i++) {
		uint64_t w = 0;

		for (j = 0; j < n; j++) {
			w ^= ((uint64_t *)sources[j])[i];
		}
		((uint64_t *)dest)[i] = w;
	}

	if (len_rem < len) {
		void *sources2[SPDK_XOR_MAX_SRC];

		for (j = 0; j < n; j++) {
			sources2[j] = sources[j] + len_rem;
		}

		xor_gen_unaligned(dest + len_rem, sources2, n, len - len_rem);
	}
}

#ifdef SPDK_CONFIG_ISAL
#include "isa-l/include/raid.h"

#define SPDK_XOR_BUF_ALIGN 32

static int
do_xor_gen(void *dest, void **sources, uint32_t n, uint32_t len)
{
	if (buffers_aligned(dest, sources, n, SPDK_XOR_BUF_ALIGN)) {
		void *buffers[SPDK_XOR_MAX_SRC + 1];

		if (n >= INT_MAX) {
			return -EINVAL;
		}

		memcpy(buffers, sources, n * sizeof(buffers[0]));
		buffers[n] = dest;

		if (xor_gen(n + 1, len, buffers)) {
			return -EINVAL;
		}
	} else {
		xor_gen_basic(dest, sources, n, len);
	}

	return 0;
}

#else

#define SPDK_XOR_BUF_ALIGN sizeof(uint64_t)

static inline int
do_xor_gen(void *dest, void **sources, uint32_t n, uint32_t len)
{
	xor_gen_basic(dest, sources, n, len);
	return 0;
}

#endif

int
spdk_xor_gen(void *dest, void **sources, uint32_t n, uint32_t len)
{
	if (n < 2 || n > SPDK_XOR_MAX_SRC) {
		return -EINVAL;
	}

	return do_xor_gen(dest, sources, n, len);
}

size_t
spdk_xor_get_optimal_alignment(void)
{
	return SPDK_XOR_BUF_ALIGN;
}

SPDK_STATIC_ASSERT(SPDK_XOR_BUF_ALIGN > 0 && !(SPDK_XOR_BUF_ALIGN & (SPDK_XOR_BUF_ALIGN - 1)),
		   "Must be power of 2");
