/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_BITMAP_H_
#define FTL_BITMAP_H_

#include "spdk/stdinc.h"

struct ftl_bitmap;

/**
 * @brief The required alignment for buffer used for bitmap
 */
extern const size_t ftl_bitmap_buffer_alignment;

/**
 * @brief Converts number of bits to bitmap size need to create it
 *
 * @param bits Number of bits
 *
 * @return Size needed to create bitmap which will hold space for specified number of bits
 */
uint64_t ftl_bitmap_bits_to_size(uint64_t bits);

/**
 * @brief Converts number of bits to blocks
 *
 * @param bits Number of bits
 *
 * @return Number of blocks needed to create bitmap which will hold space for specified number of bits
 */
uint64_t ftl_bitmap_bits_to_blocks(uint64_t bits);

/**
 * @brief Creates a bitmap object using a preallocated buffer
 *
 * @param buf The buffer
 * @param size Size of the buffer
 *
 * @return On success - pointer to the allocated bitmap object, otherwise NULL
 */
struct ftl_bitmap *ftl_bitmap_create(void *buf, size_t size);

/**
 * @brief Destroys the bitmap object
 *
 * @param bitmap The bitmap
 */
void ftl_bitmap_destroy(struct ftl_bitmap *bitmap);

/**
 * @brief Gets the value of the specified bit
 *
 * @param bitmap The bitmap
 * @param bit Index of the bit
 *
 * @return True if bit is set, otherwise false
 */
bool ftl_bitmap_get(const struct ftl_bitmap *bitmap, uint64_t bit);

/**
 * @brief Sets the specified bit
 *
 * @param bitmap The bitmap
 * @param bit Index of the bit
 */
void ftl_bitmap_set(struct ftl_bitmap *bitmap, uint64_t bit);

/**
 * @brief Clears the specified bit
 *
 * @param bitmap The bitmap
 * @param bit Index of the bit
 */
void ftl_bitmap_clear(struct ftl_bitmap *bitmap, uint64_t bit);

/**
 * @brief Finds the first set bit
 *
 * @param bitmap The bitmap
 * @param start_bit Index of the bit from which to begin searching
 * @param end_bit Index of the bit up to which to search
 *
 * @return Index of the first set bit or UINT64_MAX if none found
 */
uint64_t ftl_bitmap_find_first_set(struct ftl_bitmap *bitmap, uint64_t start_bit, uint64_t end_bit);

/**
 * @brief Finds the first clear bit
 *
 * @param bitmap The bitmap
 * @param start_bit Index of the bit from which to begin searching
 * @param end_bit Index of the bit up to which to search
 *
 * @return Index of the first clear bit or UINT64_MAX if none found
 */
uint64_t ftl_bitmap_find_first_clear(struct ftl_bitmap *bitmap, uint64_t start_bit,
				     uint64_t end_bit);

/**
 * @brief Iterates over and counts set bits
 *
 * @param bitmap The bitmap
 *
 * @return Count of sets bits
 */
uint64_t ftl_bitmap_count_set(struct ftl_bitmap *bitmap);

#endif /* FTL_BITMAP_H_ */
