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

#ifndef SPDK_TREE_H_
#define SPDK_TREE_H_

struct cache_buffer {
	uint8_t			*buf;
	uint64_t		offset;
	uint32_t		buf_size;
	uint32_t		bytes_filled;
	uint32_t		bytes_flushed;
	bool			in_progress;
};

#define CACHE_BUFFER_SHIFT (18)
#define CACHE_BUFFER_SIZE (1U << CACHE_BUFFER_SHIFT)
#define NEXT_CACHE_BUFFER_OFFSET(offset)	\
	(((offset + CACHE_BUFFER_SIZE) >> CACHE_BUFFER_SHIFT) << CACHE_BUFFER_SHIFT)

#define CACHE_TREE_SHIFT 6
#define CACHE_TREE_WIDTH (1U << CACHE_TREE_SHIFT)
#define CACHE_TREE_LEVEL_SHIFT(level)	(CACHE_BUFFER_SHIFT + (level) * CACHE_TREE_SHIFT)
#define CACHE_TREE_LEVEL_SIZE(level)	(1ULL << CACHE_TREE_LEVEL_SHIFT(level))
#define CACHE_TREE_LEVEL_MASK(level)	(CACHE_TREE_LEVEL_SIZE(level) - 1)
#define CACHE_TREE_INDEX(level, offset)	((offset >> CACHE_TREE_LEVEL_SHIFT(level)) & (CACHE_TREE_WIDTH - 1))

struct cache_tree {
	uint8_t			level;
	uint64_t		present_mask;
	union {
		struct cache_buffer	*buffer[CACHE_TREE_WIDTH];
		struct cache_tree	*tree[CACHE_TREE_WIDTH];
	} u;
};

void cache_buffer_free(struct cache_buffer *cache_buffer);

struct cache_tree *tree_insert_buffer(struct cache_tree *root, struct cache_buffer *buffer);
void tree_free_buffers(struct cache_tree *tree);
struct cache_buffer *tree_find_buffer(struct cache_tree *tree, uint64_t offset);
struct cache_buffer *tree_find_filled_buffer(struct cache_tree *tree, uint64_t offset);
void tree_remove_buffer(struct cache_tree *tree, struct cache_buffer *buffer);

#endif /* SPDK_TREE_H_ */
