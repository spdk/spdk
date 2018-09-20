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
#include "ocf/ocf_def.h"
#include "ocf_env.h"

#include "spdk/crc32.h"
#include "spdk/env.h"
#include "spdk_internal/log.h"

struct _env_allocator {
	/* Memory pool ID unique name */
	char *name;

	/* Size of specific item of memory pool */
	uint32_t item_size;

	/* Number of currently allocated items in pool */
	env_atomic count;
};

static inline size_t
env_allocator_align(size_t size)
{
	if (size <= 2) {
		return size;
	}
	return (1ULL << 32) >> __builtin_clz(size - 1);
}

struct _env_allocator_item {
	uint32_t flags;
	uint32_t cpu;
	char data[];
};

void *
env_allocator_new(env_allocator *allocator)
{
	struct _env_allocator_item *item = NULL;

	item = spdk_dma_zmalloc(allocator->item_size, 0, NULL);

	if (item) {
		item->cpu = 0;
		env_atomic_inc(&allocator->count);
	} else {
		return NULL;
	}

	return &item->data;
}

env_allocator *
env_allocator_create(uint32_t size, const char *name)
{
	size_t name_size;
	env_allocator *allocator;

	allocator = spdk_dma_zmalloc(sizeof(*allocator), 0, NULL);

	allocator->item_size = size + sizeof(struct _env_allocator_item);
	allocator->name = env_strdup(name, 0);

	return allocator;
}

void
env_allocator_del(env_allocator *allocator, void *obj)
{
	struct _env_allocator_item *item = container_of(obj, struct _env_allocator_item, data);

	env_atomic_dec(&allocator->count);

	spdk_dma_free(item);
}

void
env_allocator_destroy(env_allocator *allocator)
{
	if (allocator) {
		if (env_atomic_read(&allocator->count)) {
			SPDK_ERRLOG("Not all object deallocated\n");
		}

		spdk_dma_free(allocator->name);
		spdk_dma_free(allocator);
	}
}

uint32_t
env_allocator_item_count(env_allocator *allocator)
{
	return env_atomic_read(&allocator->count);
}

/* *** WAITQUEUE *** */

void
env_waitqueue_init(env_waitqueue *w)
{
	w->completed = false;
	w->waiting = false;
	w->co = NULL;
}

void
env_waitqueue_wake_up(env_waitqueue *w)
{
	w->completed = true;
	if (!w->waiting || !w->co) {
		return;
	}
}

/* *** COMPLETION *** */

void
env_completion_init(env_completion *completion)
{
	atomic_set(&completion->atom, 1);
}

void
env_completion_wait(env_completion *completion)
{
	while (atomic_read(&completion->atom));
}

void
env_completion_complete(env_completion *completion)
{
	atomic_set(&completion->atom, 0);
}

/* *** CRC *** */

uint32_t
env_crc32(uint32_t crc, uint8_t const *message, size_t len)
{
	return spdk_crc32_ieee_update(message, len, crc);
}
