/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/log.h"
#include "spdk/util.h"

#include "ftl_bitmap.h"
#include "ftl_internal.h"

typedef unsigned long bitmap_word;

const size_t ftl_bitmap_buffer_alignment = sizeof(bitmap_word);

#define FTL_BITMAP_WORD_SHIFT	spdk_u32log2(sizeof(bitmap_word) * 8)
#define FTL_BITMAP_WORD_MASK	(~(~0UL << FTL_BITMAP_WORD_SHIFT))

uint64_t
ftl_bitmap_bits_to_size(uint64_t bits)
{
	uint64_t size;

	if (bits < ftl_bitmap_buffer_alignment) {
		bits = ftl_bitmap_buffer_alignment;
	}

	size = spdk_divide_round_up(bits, 8);
	size = spdk_divide_round_up(size, ftl_bitmap_buffer_alignment) * ftl_bitmap_buffer_alignment;

	return size;
}

uint64_t
ftl_bitmap_bits_to_blocks(uint64_t bits)
{
	uint64_t size = ftl_bitmap_bits_to_size(bits);

	return spdk_divide_round_up(size, FTL_BLOCK_SIZE);
}

struct ftl_bitmap {
	bitmap_word *buf;
	size_t size;
};

struct ftl_bitmap *ftl_bitmap_create(void *buf, size_t size)
{
	struct ftl_bitmap *bitmap;

	if ((uintptr_t)buf % ftl_bitmap_buffer_alignment) {
		SPDK_ERRLOG("Buffer for bitmap must be aligned to %lu bytes\n",
			    ftl_bitmap_buffer_alignment);
		return NULL;
	}

	if (size % ftl_bitmap_buffer_alignment) {
		SPDK_ERRLOG("Size of buffer for bitmap must be divisible by %lu bytes\n",
			    ftl_bitmap_buffer_alignment);
		return NULL;
	}

	bitmap = calloc(1, sizeof(*bitmap));
	if (!bitmap) {
		return NULL;
	}

	bitmap->buf = buf;
	bitmap->size = size / sizeof(bitmap_word);

	return bitmap;
}

void
ftl_bitmap_destroy(struct ftl_bitmap *bitmap)
{
	free(bitmap);
}

static inline void
locate_bit(const struct ftl_bitmap *bitmap, uint64_t bit,
	   bitmap_word **word_out, uint8_t *word_bit_idx_out)
{
	size_t word_idx = bit >> FTL_BITMAP_WORD_SHIFT;

	assert(word_idx < bitmap->size);

	*word_bit_idx_out = bit & FTL_BITMAP_WORD_MASK;
	*word_out = &bitmap->buf[word_idx];
}

bool
ftl_bitmap_get(const struct ftl_bitmap *bitmap, uint64_t bit)
{
	bitmap_word *word;
	uint8_t word_bit_idx;

	locate_bit(bitmap, bit, &word, &word_bit_idx);

	return *word & (1UL << word_bit_idx);
}

void
ftl_bitmap_set(struct ftl_bitmap *bitmap, uint64_t bit)
{
	bitmap_word *word;
	uint8_t word_bit_idx;

	locate_bit(bitmap, bit, &word, &word_bit_idx);

	*word |= (1UL << word_bit_idx);
}

void
ftl_bitmap_clear(struct ftl_bitmap *bitmap, uint64_t bit)
{
	bitmap_word *word;
	uint8_t word_bit_idx;

	locate_bit(bitmap, bit, &word, &word_bit_idx);

	*word &= ~(1UL << word_bit_idx);
}

static uint64_t
ftl_bitmap_find_first(struct ftl_bitmap *bitmap, uint64_t start_bit,
		      uint64_t end_bit, bool value)
{
	bitmap_word skip = (value ? 0 : ~0UL);
	bitmap_word word;
	size_t i, end;
	uint64_t ret;

	assert(start_bit <= end_bit);

	i = start_bit >> FTL_BITMAP_WORD_SHIFT;
	assert(i < bitmap->size);

	word = (bitmap->buf[i] ^ skip) & (~0UL << (start_bit & FTL_BITMAP_WORD_MASK));
	if (word != 0) {
		goto found;
	}

	end = spdk_min((end_bit >> FTL_BITMAP_WORD_SHIFT) + 1, bitmap->size);
	for (i = i + 1; i < end; i++) {
		word = bitmap->buf[i] ^ skip;
		if (word != 0) {
			goto found;
		}
	}

	return UINT64_MAX;
found:
	ret = (i << FTL_BITMAP_WORD_SHIFT) + __builtin_ctzl(word);
	if (ret > end_bit) {
		return UINT64_MAX;
	}
	return ret;
}

uint64_t
ftl_bitmap_find_first_set(struct ftl_bitmap *bitmap, uint64_t start_bit, uint64_t end_bit)
{
	return ftl_bitmap_find_first(bitmap, start_bit, end_bit, true);
}

uint64_t
ftl_bitmap_find_first_clear(struct ftl_bitmap *bitmap, uint64_t start_bit,
			    uint64_t end_bit)
{
	return ftl_bitmap_find_first(bitmap, start_bit, end_bit, false);
}

uint64_t
ftl_bitmap_count_set(struct ftl_bitmap *bitmap)
{
	size_t i;
	bitmap_word *word = bitmap->buf;
	uint64_t count = 0;

	for (i = 0; i < bitmap->size; i++, word++) {
		count += __builtin_popcountl(*word);
	}

	return count;
}
