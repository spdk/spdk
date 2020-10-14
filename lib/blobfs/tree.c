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

#include "spdk/blobfs.h"
#include "tree.h"

#include "spdk/queue.h"
#include "spdk/assert.h"
#include "spdk/env.h"
#include "spdk/log.h"

struct cache_buffer *
tree_find_buffer(struct cache_tree *tree, uint64_t offset)
{
	uint64_t index;

	while (tree != NULL) {
		index = offset / CACHE_TREE_LEVEL_SIZE(tree->level);
		if (index >= CACHE_TREE_WIDTH) {
			return NULL;
		}
		if (tree->level == 0) {
			return tree->u.buffer[index];
		} else {
			offset &= CACHE_TREE_LEVEL_MASK(tree->level);
			tree = tree->u.tree[index];
		}
	}

	return NULL;
}

struct cache_buffer *
tree_find_filled_buffer(struct cache_tree *tree, uint64_t offset)
{
	struct cache_buffer *buf;

	buf = tree_find_buffer(tree, offset);
	if (buf != NULL && buf->bytes_filled > 0) {
		return buf;
	} else {
		return NULL;
	}
}

struct cache_tree *
tree_insert_buffer(struct cache_tree *root, struct cache_buffer *buffer)
{
	struct cache_tree *tree;
	uint64_t index, offset;

	offset = buffer->offset;
	while (offset >= CACHE_TREE_LEVEL_SIZE(root->level + 1)) {
		if (root->present_mask != 0) {
			tree = calloc(1, sizeof(*tree));
			assert(tree != NULL);
			tree->level = root->level + 1;
			tree->u.tree[0] = root;
			root = tree;
			root->present_mask = 0x1ULL;
		} else {
			root->level++;
		}
	}

	tree = root;
	while (tree->level > 0) {
		index = offset / CACHE_TREE_LEVEL_SIZE(tree->level);
		assert(index < CACHE_TREE_WIDTH);
		offset &= CACHE_TREE_LEVEL_MASK(tree->level);
		if (tree->u.tree[index] == NULL) {
			tree->u.tree[index] = calloc(1, sizeof(*tree));
			assert(tree->u.tree[index] != NULL);
			tree->u.tree[index]->level = tree->level - 1;
			tree->present_mask |= (1ULL << index);
		}
		tree = tree->u.tree[index];
	}

	index = offset / CACHE_BUFFER_SIZE;
	assert(index < CACHE_TREE_WIDTH);
	assert(tree->u.buffer[index] == NULL);
	tree->u.buffer[index] = buffer;
	tree->present_mask |= (1ULL << index);
	return root;
}

void
tree_remove_buffer(struct cache_tree *tree, struct cache_buffer *buffer)
{
	struct cache_tree *child;
	uint64_t index;

	index = CACHE_TREE_INDEX(tree->level, buffer->offset);

	if (tree->level == 0) {
		assert(tree->u.buffer[index] != NULL);
		assert(buffer == tree->u.buffer[index]);
		tree->present_mask &= ~(1ULL << index);
		tree->u.buffer[index] = NULL;
		cache_buffer_free(buffer);
		return;
	}

	child = tree->u.tree[index];
	assert(child != NULL);
	tree_remove_buffer(child, buffer);
	if (child->present_mask == 0) {
		tree->present_mask &= ~(1ULL << index);
		tree->u.tree[index] = NULL;
		free(child);
	}
}

void
tree_free_buffers(struct cache_tree *tree)
{
	struct cache_buffer *buffer;
	struct cache_tree *child;
	uint32_t i;

	if (tree->present_mask == 0) {
		return;
	}

	if (tree->level == 0) {
		for (i = 0; i < CACHE_TREE_WIDTH; i++) {
			buffer = tree->u.buffer[i];
			if (buffer != NULL && buffer->in_progress == false &&
			    buffer->bytes_filled == buffer->bytes_flushed) {
				cache_buffer_free(buffer);
				tree->u.buffer[i] = NULL;
				tree->present_mask &= ~(1ULL << i);
			}
		}
	} else {
		for (i = 0; i < CACHE_TREE_WIDTH; i++) {
			child = tree->u.tree[i];
			if (child != NULL) {
				tree_free_buffers(child);
				if (child->present_mask == 0) {
					free(child);
					tree->u.tree[i] = NULL;
					tree->present_mask &= ~(1ULL << i);
				}
			}
		}
	}
}
