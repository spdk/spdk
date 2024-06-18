/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/queue.h"
#include "spdk/util.h"
#include "spdk/env.h"
#include "spdk/likely.h"

#include "ftl_mempool.h"
#include "ftl_bitmap.h"

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
	struct ftl_bitmap *inuse_bmp;
	void *inuse_buf;
};

static inline bool is_element_valid(struct ftl_mempool *mpool,
				    void *element)  __attribute__((unused));

static inline bool ftl_mempool_is_initialized(struct ftl_mempool *mpool) __attribute__((unused));

static size_t
element_size_aligned(size_t size, size_t alignment)
{
	if (!alignment) {
		return size;
	}

	if (size % alignment) {
		return (size / alignment + 1) * alignment;
	}

	return size;
}

static inline bool
is_element_valid(struct ftl_mempool *mpool, void *element)
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

	if ((element - mpool->buffer) % mpool->element_size != 0) {
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

void
ftl_mempool_destroy(struct ftl_mempool *mpool)
{
	if (!mpool) {
		return;
	}

	spdk_dma_free(mpool->buffer);
	free(mpool);
}

static inline bool
ftl_mempool_is_initialized(struct ftl_mempool *mpool)
{
	return mpool->inuse_buf == NULL;
}

void *
ftl_mempool_get(struct ftl_mempool *mpool)
{
	struct ftl_mempool_element *el;

	assert(ftl_mempool_is_initialized(mpool));
	if (spdk_unlikely(SLIST_EMPTY(&mpool->list))) {
		return NULL;
	}

	el = SLIST_FIRST(&mpool->list);
	SLIST_REMOVE_HEAD(&mpool->list, entry);

	return el;
}

void
ftl_mempool_put(struct ftl_mempool *mpool, void *element)
{
	struct ftl_mempool_element *el = element;

	assert(ftl_mempool_is_initialized(mpool));
	assert(is_element_valid(mpool, element));
	SLIST_INSERT_HEAD(&mpool->list, el, entry);
}

struct ftl_mempool *
ftl_mempool_create_ext(void *buffer, size_t count, size_t size, size_t alignment)
{
	struct ftl_mempool *mp;
	size_t inuse_buf_sz;

	assert(buffer);
	assert(count > 0);
	assert(size > 0);

	mp = calloc(1, sizeof(*mp));
	if (!mp) {
		goto error;
	}

	size = spdk_max(size, sizeof(struct ftl_mempool_element));

	mp->count = count;
	mp->element_size = element_size_aligned(size, alignment);
	mp->alignment = alignment;
	SLIST_INIT(&mp->list);

	/* Calculate underlying inuse_bmp's buf size */
	inuse_buf_sz = spdk_divide_round_up(mp->count, 8);
	/* The bitmap size must be a multiple of word size (8b) - round up */
	if (inuse_buf_sz & 7UL) {
		inuse_buf_sz &= ~7UL;
		inuse_buf_sz += 8;
	}

	mp->inuse_buf = calloc(1, inuse_buf_sz);
	if (!mp->inuse_buf) {
		goto error;
	}

	mp->inuse_bmp = ftl_bitmap_create(mp->inuse_buf, inuse_buf_sz);
	if (!mp->inuse_bmp) {
		goto error;
	}

	/* Map the buffer */
	mp->buffer_size = mp->element_size * mp->count;
	mp->buffer = buffer;

	return mp;

error:
	ftl_mempool_destroy_ext(mp);
	return NULL;
}

void
ftl_mempool_destroy_ext(struct ftl_mempool *mpool)
{
	if (!mpool) {
		return;
	}

	if (mpool->inuse_bmp) {
		ftl_bitmap_destroy(mpool->inuse_bmp);
	}
	free(mpool->inuse_buf);
	free(mpool);
}

void
ftl_mempool_initialize_ext(struct ftl_mempool *mpool)
{
	struct ftl_mempool_element *el;
	void *buffer = mpool->buffer;
	size_t i;

	assert(!ftl_mempool_is_initialized(mpool));

	for (i = 0; i < mpool->count; i++, buffer += mpool->element_size) {
		if (ftl_bitmap_get(mpool->inuse_bmp, i)) {
			continue;
		}
		el = buffer;
		assert(is_element_valid(mpool, el));
		SLIST_INSERT_HEAD(&mpool->list, el, entry);
	}

	ftl_bitmap_destroy(mpool->inuse_bmp);
	mpool->inuse_bmp = NULL;

	free(mpool->inuse_buf);
	mpool->inuse_buf = NULL;
}

ftl_df_obj_id
ftl_mempool_get_df_obj_id(struct ftl_mempool *mpool, void *df_obj_ptr)
{
	return ftl_df_get_obj_id(mpool->buffer, df_obj_ptr);
}

size_t
ftl_mempool_get_df_obj_index(struct ftl_mempool *mpool, void *df_obj_ptr)
{
	return ftl_mempool_get_df_obj_id(mpool, df_obj_ptr) / mpool->element_size;
}

void *
ftl_mempool_get_df_ptr(struct ftl_mempool *mpool, ftl_df_obj_id df_obj_id)
{
	return ftl_df_get_obj_ptr(mpool->buffer, df_obj_id);
}

void *
ftl_mempool_claim_df(struct ftl_mempool *mpool, ftl_df_obj_id df_obj_id)
{
	struct ftl_mempool_element *el = ftl_df_get_obj_ptr(mpool->buffer, df_obj_id);

	assert(!ftl_mempool_is_initialized(mpool));
	assert(df_obj_id % mpool->element_size == 0);
	assert(df_obj_id / mpool->element_size < mpool->count);

	ftl_bitmap_set(mpool->inuse_bmp, df_obj_id / mpool->element_size);
	return el;
}

void
ftl_mempool_release_df(struct ftl_mempool *mpool, ftl_df_obj_id df_obj_id)
{
	assert(!ftl_mempool_is_initialized(mpool));
	assert(df_obj_id % mpool->element_size == 0);
	assert(df_obj_id / mpool->element_size < mpool->count);

	ftl_bitmap_clear(mpool->inuse_bmp, df_obj_id / mpool->element_size);
}
