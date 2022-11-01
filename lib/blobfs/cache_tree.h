/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
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
