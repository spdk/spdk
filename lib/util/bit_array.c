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

#include "spdk/stdinc.h"

#include "spdk/bit_array.h"
#include "spdk/env.h"

#include "spdk/likely.h"
#include "spdk/util.h"

typedef uint64_t spdk_bit_array_word;
#define SPDK_BIT_ARRAY_WORD_TZCNT(x)	(__builtin_ctzll(x))
#define SPDK_BIT_ARRAY_WORD_POPCNT(x)	(__builtin_popcountll(x))
#define SPDK_BIT_ARRAY_WORD_C(x)	((spdk_bit_array_word)(x))
#define SPDK_BIT_ARRAY_WORD_BYTES	sizeof(spdk_bit_array_word)
#define SPDK_BIT_ARRAY_WORD_BITS	(SPDK_BIT_ARRAY_WORD_BYTES * 8)
#define SPDK_BIT_ARRAY_WORD_INDEX_SHIFT	spdk_u32log2(SPDK_BIT_ARRAY_WORD_BITS)
#define SPDK_BIT_ARRAY_WORD_INDEX_MASK	((1u << SPDK_BIT_ARRAY_WORD_INDEX_SHIFT) - 1)

struct spdk_bit_array {
	uint32_t bit_count;
	spdk_bit_array_word words[];
};

struct spdk_bit_array *
spdk_bit_array_create(uint32_t num_bits)
{
	struct spdk_bit_array *ba = NULL;

	spdk_bit_array_resize(&ba, num_bits);

	return ba;
}

void
spdk_bit_array_free(struct spdk_bit_array **bap)
{
	struct spdk_bit_array *ba;

	if (!bap) {
		return;
	}

	ba = *bap;
	*bap = NULL;
	spdk_dma_free(ba);
}

static inline uint32_t
spdk_bit_array_word_count(uint32_t num_bits)
{
	return (num_bits + SPDK_BIT_ARRAY_WORD_BITS - 1) >> SPDK_BIT_ARRAY_WORD_INDEX_SHIFT;
}

static inline spdk_bit_array_word
spdk_bit_array_word_mask(uint32_t num_bits)
{
	assert(num_bits < SPDK_BIT_ARRAY_WORD_BITS);
	return (SPDK_BIT_ARRAY_WORD_C(1) << num_bits) - 1;
}

int
spdk_bit_array_resize(struct spdk_bit_array **bap, uint32_t num_bits)
{
	struct spdk_bit_array *new_ba;
	uint32_t old_word_count, new_word_count;
	size_t new_size;

	/*
	 * Max number of bits allowed is UINT32_MAX - 1, because we use UINT32_MAX to denote
	 * when a set or cleared bit cannot be found.
	 */
	if (!bap || num_bits == UINT32_MAX) {
		return -EINVAL;
	}

	new_word_count = spdk_bit_array_word_count(num_bits);
	new_size = offsetof(struct spdk_bit_array, words) + new_word_count * SPDK_BIT_ARRAY_WORD_BYTES;

	/*
	 * Always keep one extra word with a 0 and a 1 past the actual required size so that the
	 * find_first functions can just keep going until they match.
	 */
	new_size += SPDK_BIT_ARRAY_WORD_BYTES;

	new_ba = (struct spdk_bit_array *)spdk_dma_realloc(*bap, new_size, 64, NULL);
	if (!new_ba) {
		return -ENOMEM;
	}

	/*
	 * Set up special extra word (see above comment about find_first_clear).
	 *
	 * This is set to 0b10 so that find_first_clear will find a 0 at the very first
	 * bit past the end of the buffer, and find_first_set will find a 1 at the next bit
	 * past that.
	 */
	new_ba->words[new_word_count] = 0x2;

	if (*bap == NULL) {
		old_word_count = 0;
		new_ba->bit_count = 0;
	} else {
		old_word_count = spdk_bit_array_word_count(new_ba->bit_count);
	}

	if (new_word_count > old_word_count) {
		/* Zero out new entries */
		memset(&new_ba->words[old_word_count], 0,
		       (new_word_count - old_word_count) * SPDK_BIT_ARRAY_WORD_BYTES);
	} else if (new_word_count == old_word_count && num_bits < new_ba->bit_count) {
		/* Make sure any existing partial last word is cleared beyond the new num_bits. */
		uint32_t last_word_bits;
		spdk_bit_array_word mask;

		last_word_bits = num_bits & SPDK_BIT_ARRAY_WORD_INDEX_MASK;
		mask = spdk_bit_array_word_mask(last_word_bits);
		new_ba->words[old_word_count - 1] &= mask;
	}

	new_ba->bit_count = num_bits;
	*bap = new_ba;
	return 0;
}

uint32_t
spdk_bit_array_capacity(const struct spdk_bit_array *ba)
{
	return ba->bit_count;
}

static inline int
_spdk_bit_array_get_word(const struct spdk_bit_array *ba, uint32_t bit_index,
			 uint32_t *word_index, uint32_t *word_bit_index)
{
	if (spdk_unlikely(bit_index >= ba->bit_count)) {
		return -EINVAL;
	}

	*word_index = bit_index >> SPDK_BIT_ARRAY_WORD_INDEX_SHIFT;
	*word_bit_index = bit_index & SPDK_BIT_ARRAY_WORD_INDEX_MASK;

	return 0;
}

bool
spdk_bit_array_get(const struct spdk_bit_array *ba, uint32_t bit_index)
{
	uint32_t word_index, word_bit_index;

	if (_spdk_bit_array_get_word(ba, bit_index, &word_index, &word_bit_index)) {
		return false;
	}

	return (ba->words[word_index] >> word_bit_index) & 1U;
}

int
spdk_bit_array_set(struct spdk_bit_array *ba, uint32_t bit_index)
{
	uint32_t word_index, word_bit_index;

	if (_spdk_bit_array_get_word(ba, bit_index, &word_index, &word_bit_index)) {
		return -EINVAL;
	}

	ba->words[word_index] |= (SPDK_BIT_ARRAY_WORD_C(1) << word_bit_index);
	return 0;
}

void
spdk_bit_array_clear(struct spdk_bit_array *ba, uint32_t bit_index)
{
	uint32_t word_index, word_bit_index;

	if (_spdk_bit_array_get_word(ba, bit_index, &word_index, &word_bit_index)) {
		/*
		 * Clearing past the end of the bit array is a no-op, since bit past the end
		 * are implicitly 0.
		 */
		return;
	}

	ba->words[word_index] &= ~(SPDK_BIT_ARRAY_WORD_C(1) << word_bit_index);
}

static inline uint32_t
_spdk_bit_array_find_first(const struct spdk_bit_array *ba, uint32_t start_bit_index,
			   spdk_bit_array_word xor_mask)
{
	uint32_t word_index, first_word_bit_index;
	spdk_bit_array_word word, first_word_mask;
	const spdk_bit_array_word *words, *cur_word;

	if (spdk_unlikely(start_bit_index >= ba->bit_count)) {
		return ba->bit_count;
	}

	word_index = start_bit_index >> SPDK_BIT_ARRAY_WORD_INDEX_SHIFT;
	words = ba->words;
	cur_word = &words[word_index];

	/*
	 * Special case for first word: skip start_bit_index % SPDK_BIT_ARRAY_WORD_BITS bits
	 * within the first word.
	 */
	first_word_bit_index = start_bit_index & SPDK_BIT_ARRAY_WORD_INDEX_MASK;
	first_word_mask = spdk_bit_array_word_mask(first_word_bit_index);

	word = (*cur_word ^ xor_mask) & ~first_word_mask;

	/*
	 * spdk_bit_array_resize() guarantees that an extra word with a 1 and a 0 will always be
	 * at the end of the words[] array, so just keep going until a word matches.
	 */
	while (word == 0) {
		word = *++cur_word ^ xor_mask;
	}

	return ((uintptr_t)cur_word - (uintptr_t)words) * 8 + SPDK_BIT_ARRAY_WORD_TZCNT(word);
}


uint32_t
spdk_bit_array_find_first_set(const struct spdk_bit_array *ba, uint32_t start_bit_index)
{
	uint32_t bit_index;

	bit_index = _spdk_bit_array_find_first(ba, start_bit_index, 0);

	/*
	 * If we ran off the end of the array and found the 1 bit in the extra word,
	 * return UINT32_MAX to indicate no actual 1 bits were found.
	 */
	if (bit_index >= ba->bit_count) {
		bit_index = UINT32_MAX;
	}

	return bit_index;
}

uint32_t
spdk_bit_array_find_first_clear(const struct spdk_bit_array *ba, uint32_t start_bit_index)
{
	uint32_t bit_index;

	bit_index = _spdk_bit_array_find_first(ba, start_bit_index, SPDK_BIT_ARRAY_WORD_C(-1));

	/*
	 * If we ran off the end of the array and found the 0 bit in the extra word,
	 * return UINT32_MAX to indicate no actual 0 bits were found.
	 */
	if (bit_index >= ba->bit_count) {
		bit_index = UINT32_MAX;
	}

	return bit_index;
}

uint32_t
spdk_bit_array_count_set(const struct spdk_bit_array *ba)
{
	const spdk_bit_array_word *cur_word = ba->words;
	uint32_t word_count = spdk_bit_array_word_count(ba->bit_count);
	uint32_t set_count = 0;

	while (word_count--) {
		/*
		 * No special treatment is needed for the last (potentially partial) word, since
		 * spdk_bit_array_resize() makes sure the bits past bit_count are cleared.
		 */
		set_count += SPDK_BIT_ARRAY_WORD_POPCNT(*cur_word++);
	}

	return set_count;
}

uint32_t
spdk_bit_array_count_clear(const struct spdk_bit_array *ba)
{
	return ba->bit_count - spdk_bit_array_count_set(ba);
}
