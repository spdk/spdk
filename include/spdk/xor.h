/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

/**
 * \file
 * XOR utility functions
 */

#ifndef SPDK_XOR_H
#define SPDK_XOR_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Generate XOR from multiple source buffers.
 *
 * \param dest Destination buffer.
 * \param sources Array of source buffers.
 * \param n Number of source buffers in the array.
 * \param len Length of each buffer in bytes.
 * \return 0 on success, negative error code otherwise.
 */
int spdk_xor_gen(void *dest, void **sources, uint32_t n, uint32_t len);

/**
 * Get the optimal buffer alignment for XOR functions.
 *
 * \return The alignment in bytes.
 */
size_t spdk_xor_get_optimal_alignment(void);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_XOR_H */
