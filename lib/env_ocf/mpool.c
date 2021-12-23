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

#include "spdk/env.h"
#include "ocf_env.h"

#include "mpool.h"

struct env_mpool {
	env_allocator *allocator[env_mpool_max];
	/* Handles to memory pools */

	uint32_t hdr_size;
	/* Data header size (constant allocation part) */

	uint32_t elem_size;
	/* Per element size increment (variable allocation part) */

	uint32_t mpool_max;
	/* Max mpool allocation order */

	bool fallback;
	/* Fallback to vmalloc */
};

struct env_mpool *env_mpool_create(uint32_t hdr_size, uint32_t elem_size,
				   int flags, int mpool_max, bool fallback,
				   const uint32_t limits[env_mpool_max],
				   const char *name_prefix, bool zero)
{
	int i;
	char name[OCF_ALLOCATOR_NAME_MAX] = {};
	int ret;
	int size;

	struct env_mpool *mpool = env_zalloc(sizeof(struct env_mpool), ENV_MEM_NOIO);
	if (!mpool) {
		return NULL;
	}

	mpool->hdr_size = hdr_size;
	mpool->elem_size = elem_size;
	mpool->mpool_max = mpool_max;
	mpool->fallback = fallback;

	for (i = 0; i < min(env_mpool_max, mpool_max + 1); i++) {
		ret = snprintf(name, sizeof(name), "%s_%u", name_prefix, (1 << i));
		if (ret < 0 || ret >= (int)sizeof(name)) {
			goto err;
		}

		size = hdr_size + (elem_size * (1 << i));

		mpool->allocator[i] = env_allocator_create_extended(size, name,
				      limits ? limits[i] : -1, zero);

		if (!mpool->allocator[i]) {
			goto err;
		}
	}

	return mpool;

err:
	env_mpool_destroy(mpool);
	return NULL;
}

void env_mpool_destroy(struct env_mpool *mpool)
{
	if (mpool) {
		int i;

		for (i = 0; i < env_mpool_max; i++) {
			if (mpool->allocator[i]) {
				env_allocator_destroy(mpool->allocator[i]);
			}
		}

		env_free(mpool);
	}
}

static env_allocator *env_mpool_get_allocator(struct env_mpool *mpool,
		uint32_t count)
{
	unsigned int idx;

	if (unlikely(count == 0)) {
		return mpool->allocator[env_mpool_1];
	}

	idx = 31 - __builtin_clz(count);

	if (__builtin_ffs(count) <= idx) {
		idx++;
	}

	if (idx >= env_mpool_max || idx > mpool->mpool_max) {
		return NULL;
	}

	return mpool->allocator[idx];
}

void *env_mpool_new(struct env_mpool *mpool, uint32_t count)
{
	void *items = NULL;
	env_allocator *allocator;
	size_t size = mpool->hdr_size + (mpool->elem_size * count);

	allocator = env_mpool_get_allocator(mpool, count);

	if (allocator) {
		items = env_allocator_new(allocator);
	} else if (mpool->fallback) {
		items = env_vmalloc(size);
	}

	return items;
}

bool env_mpool_del(struct env_mpool *mpool,
		   void *items, uint32_t count)
{
	env_allocator *allocator;

	allocator = env_mpool_get_allocator(mpool, count);

	if (allocator) {
		env_allocator_del(allocator, items);
	} else if (mpool->fallback) {
		env_vfree(items);
	} else {
		return false;
	}

	return true;
}
