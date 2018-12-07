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

/* Number of buffers for mempool
 * Need to be power of two
 * Need to be sufficient for amount of memory that OCF uses
 */
#define ENV_ALLOCATOR_NBUFS 32768

/* Use unique index for env allocators */
static int g_env_allocator_index = 0;

struct _env_allocator {
	/* Memory pool ID unique name */
	char *name;

	/* Size of specific item of memory pool */
	uint32_t item_size;

	/* Number of currently allocated items in pool */
	env_atomic count;

	/* Memory pool for efficient allocation */
	struct spdk_mempool *pool;
};

void *
env_allocator_new(env_allocator *allocator)
{
	void *item = spdk_mempool_get(allocator->pool);

	if (item) {
		env_atomic_inc(&allocator->count);
	}

	return item;
}

env_allocator *
env_allocator_create(uint32_t size, const char *name)
{
	env_allocator *allocator;
	size_t name_size = strlen("ocf_env_99999") + 1;

	allocator = malloc(sizeof(*allocator));
	if (allocator == NULL) {
		return NULL;
	}

	allocator->name = calloc(1, name_size);
	if (allocator->name == NULL) {
		free(allocator);
		return NULL;
	}
	snprintf(allocator->name, name_size, "ocf_env_%d", g_env_allocator_index++);

	allocator->item_size = size;
	allocator->count = 0;

	allocator->pool = spdk_mempool_create(allocator->name,
					      ENV_ALLOCATOR_NBUFS, allocator->item_size,
					      SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
					      SPDK_ENV_SOCKET_ID_ANY);
	if (allocator->pool == NULL) {
		free(allocator->name);
		free(allocator);
		return NULL;
	}

	return allocator;
}

void
env_allocator_del(env_allocator *allocator, void *item)
{
	env_atomic_dec(&allocator->count);
	spdk_mempool_put(allocator->pool, item);
}

void
env_allocator_destroy(env_allocator *allocator)
{
	if (allocator) {
		if (env_atomic_read(&allocator->count)) {
			SPDK_ERRLOG("Not all objects deallocated\n");
			assert(false);
		}

		spdk_mempool_free(allocator->pool);
		free(allocator->name);
		free(allocator);
	}
}

uint32_t
env_allocator_item_count(env_allocator *allocator)
{
	return env_atomic_read(&allocator->count);
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
	while (atomic_read(&completion->atom)) {
		spdk_pause();
	}
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
