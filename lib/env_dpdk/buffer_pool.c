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

#include "env_internal.h"

struct spdk_bufferpool {
	struct spdk_mempool *mp;
	size_t alignment;
	size_t element_size;
	void *mem_region;
};

static void
bufferpool_cb(struct spdk_mempool *mp, void *opaque, void *obj, unsigned obj_idx)
{
	struct spdk_bufferpool *buf_pool = opaque;
	struct spdk_bufferpool_ele *entry = obj;

	entry->buffer = (void *)((uintptr_t)buf_pool->mem_region + (uintptr_t)(
					 buf_pool->element_size * obj_idx));
	assert(((uintptr_t)entry->buffer & (~(uintptr_t)(buf_pool->alignment - 1))) ==
	       (uintptr_t)entry->buffer);
	return;
}

struct spdk_bufferpool *
spdk_bufferpool_create(const char *name, size_t count,
		       size_t ele_size, size_t alignment, size_t cache_size, int socket_id)
{
	struct spdk_bufferpool *buf_pool = NULL;
	size_t final_ele_size = ele_size;
	size_t alignment_padding;
	size_t allocation_size;

	if (alignment != 0) {
		/* We need to make sure that each element can be aligned */
		if (final_ele_size % alignment) {
			alignment_padding = alignment - (final_ele_size % alignment);
			final_ele_size += alignment_padding;
		}
	}

	/* Overflow check */
	if (final_ele_size > SIZE_MAX / count) {
		goto error;
	}

	buf_pool = calloc(1, sizeof(struct spdk_bufferpool));
	if (buf_pool == NULL) {
		goto error;
	}

	buf_pool->element_size = final_ele_size;
	buf_pool->alignment = alignment;

	allocation_size = final_ele_size * count;

	buf_pool->mem_region = spdk_zmalloc(allocation_size, alignment, NULL, socket_id,
					    SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE);
	if (buf_pool->mem_region == NULL) {
		goto error;
	}

	buf_pool->mp = (struct spdk_mempool *)spdk_mempool_create_ctor(name, count, sizeof(void *),
			cache_size, socket_id, bufferpool_cb, buf_pool);
	if (buf_pool->mp == NULL) {
		goto error;
	}

	return buf_pool;

error:
	if (buf_pool) {
		if (buf_pool->mp) {
			spdk_mempool_free(buf_pool->mp);
		}

		if (buf_pool->mem_region) {
			spdk_free(buf_pool->mem_region);
		}

		free(buf_pool);
	}
	return NULL;
}

char *
spdk_bufferpool_get_name(struct spdk_bufferpool *bp)
{
	return spdk_mempool_get_name((struct spdk_mempool *)bp->mp);
}

void
spdk_bufferpool_free(struct spdk_bufferpool *bp)
{
	spdk_mempool_free((struct spdk_mempool *)bp->mp);
	spdk_free(bp->mem_region);
	free(bp);
}

void *
spdk_bufferpool_get(struct spdk_bufferpool *bp)
{
	return spdk_mempool_get((struct spdk_mempool *)bp->mp);
}

int
spdk_bufferpool_get_bulk(struct spdk_bufferpool *bp, void **ele_arr, size_t count)
{
	return spdk_mempool_get_bulk((struct spdk_mempool *)bp->mp, ele_arr, count);
}

void
spdk_bufferpool_put(struct spdk_bufferpool *bp, void *ele)
{
	spdk_mempool_put((struct spdk_mempool *)bp->mp, ele);
}

void
spdk_bufferpool_put_bulk(struct spdk_bufferpool *bp, void **ele_arr, size_t count)
{
	spdk_mempool_put_bulk((struct spdk_mempool *)bp->mp, ele_arr, count);
}

size_t
spdk_bufferpool_count(const struct spdk_bufferpool *bp)
{
	return spdk_mempool_count((struct spdk_mempool *)bp->mp);
}
