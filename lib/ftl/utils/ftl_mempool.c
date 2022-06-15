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
#include "spdk/queue.h"
#include "spdk/util.h"
#include "spdk/env.h"
#include "spdk/likely.h"

#include "ftl_mempool.h"

struct ftl_mempool_element {
	SLIST_ENTRY(ftl_mempool_element) entry;
};

struct ftl_mempool {
	SLIST_HEAD(, ftl_mempool_element) list;
	size_t element_size;
	void *buffer;
	size_t buffer_size;
	size_t count;
	size_t alignment;
	int socket_id;
};


static size_t element_size_aligned(size_t size, size_t alignment)
{
	if (!alignment) {
		return size;
	}

	if (size % alignment) {
		return (size / alignment + 1) * alignment;
	}

	return size;
}

static inline bool is_element_valid(struct ftl_mempool *mpool, void *element)
{
	if (element < mpool->buffer) {
		return false;
	}

	if (element + mpool->element_size > mpool->buffer + mpool->buffer_size) {
		return false;
	}

	if (!mpool->alignment) {
		return true;
	}

	if ((size_t)element % mpool->alignment) {
		return false;
	}

	return true;
}

struct ftl_mempool *ftl_mempool_create(size_t count, size_t size,
				       size_t alignment, int socket_id)
{
	struct ftl_mempool *mp;
	void *buffer;
	size_t i;

	assert(count > 0);
	assert(size > 0);

	if (!spdk_u64_is_pow2(alignment)) {
		return NULL;
	}

	mp = calloc(1, sizeof(*mp));
	if (!mp) {
		return NULL;
	}

	size = spdk_max(size, sizeof(struct ftl_mempool_element));

	mp->count = count;
	mp->element_size = element_size_aligned(size, alignment);
	mp->alignment = alignment;
	mp->socket_id = socket_id;
	SLIST_INIT(&mp->list);

	mp->buffer_size = mp->element_size * mp->count;
	mp->buffer = spdk_dma_malloc_socket(mp->buffer_size, mp->alignment,
					    NULL, socket_id);
	if (!mp->buffer) {
		free(mp);
		return NULL;
	}

	buffer = mp->buffer;
	for (i = 0; i < count; i++, buffer += mp->element_size) {
		struct ftl_mempool_element *el = buffer;
		assert(is_element_valid(mp, el));
		SLIST_INSERT_HEAD(&mp->list, el, entry);
	}

	return mp;
}

void ftl_mempool_destroy(struct ftl_mempool *mpool)
{
	if (!mpool) {
		return;
	}

	spdk_dma_free(mpool->buffer);
	free(mpool);
}

void *ftl_mempool_get(struct ftl_mempool *mpool)
{
	if (spdk_unlikely(SLIST_EMPTY(&mpool->list))) {
		return NULL;
	}

	struct ftl_mempool_element *el = SLIST_FIRST(&mpool->list);
	SLIST_REMOVE_HEAD(&mpool->list, entry);

	return el;
}

void ftl_mempool_put(struct ftl_mempool *mpool, void *element)
{
	struct ftl_mempool_element *el = element;

	assert(is_element_valid(mpool, element));
	SLIST_INSERT_HEAD(&mpool->list, el, entry);
}
