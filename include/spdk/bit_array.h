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

/** \file
 * Bit array data structure
 */

#ifndef SPDK_BIT_ARRAY_H
#define SPDK_BIT_ARRAY_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Variable-length bit array.
 */
struct spdk_bit_array;

/**
 * Return the number of bits a bit array is currently sized to hold.
 */
uint32_t spdk_bit_array_capacity(const struct spdk_bit_array *ba);

/**
 * Create a bit array.
 */
struct spdk_bit_array *spdk_bit_array_create(uint32_t num_bits);

/**
 * Free a bit array and set the pointer to NULL.
 */
void spdk_bit_array_free(struct spdk_bit_array **bap);

/**
 * Create or resize a bit array.
 *
 * To create a new bit array, pass a pointer to a spdk_bit_array pointer that is NULL for bap.
 *
 * The bit array will be sized to hold at least num_bits.
 *
 * If num_bits is smaller than the previous size of the bit array,
 * any data beyond the new num_bits size will be cleared.
 *
 * If num_bits is larger than the previous size of the bit array,
 * any data beyond the old num_bits size will be cleared.
 */
int spdk_bit_array_resize(struct spdk_bit_array **bap, uint32_t num_bits);

/**
 * Get the value of a bit from the bit array.
 *
 * If bit_index is beyond the end of the current size of the bit array,
 * this function will return false (i.e. bits beyond the end of the array are implicitly 0).
 */
bool spdk_bit_array_get(const struct spdk_bit_array *ba, uint32_t bit_index);

/**
 * Set (to 1) a bit in the bit array.
 *
 * If bit_index is beyond the end of the bit array, this function will return -EINVAL.
 * On success, returns 0.
 */
int spdk_bit_array_set(struct spdk_bit_array *ba, uint32_t bit_index);

/**
 * Clear (to 0) a bit in the bit array.
 *
 * If bit_index is beyond the end of the bit array, no action is taken. Bits beyond the end of the
 * bit array are implicitly 0.
 */
void spdk_bit_array_clear(struct spdk_bit_array *ba, uint32_t bit_index);

/**
 * Find the index of the first set bit in the array.
 *
 * \param ba The bit array to search.
 * \param start_bit_index The bit index from which to start searching (0 to start from the beginning
 * of the array).
 *
 * \return The index of the first set bit. If no bits are set, returns UINT32_MAX.
 */
uint32_t spdk_bit_array_find_first_set(const struct spdk_bit_array *ba, uint32_t start_bit_index);

/**
 * Find the index of the first cleared bit in the array.
 *
 * \param ba The bit array to search.
 * \param start_bit_index The bit index from which to start searching (0 to start from the beginning
 * of the array)..
 *
 * \return The index of the first cleared bit. Bits beyond the current size of the array are
 * implicitly cleared, so if all bits within the current size are set, this function will return
 * the current number of bits + 1.
 */
uint32_t spdk_bit_array_find_first_clear(const struct spdk_bit_array *ba, uint32_t start_bit_index);

#ifdef __cplusplus
}
#endif

#endif
