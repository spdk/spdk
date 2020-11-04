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
#include "spdk/log.h"

/* Number of buffers for mempool
 * Need to be power of two - 1 for better memory utilization
 * It depends on memory usage of OCF which
 * in itself depends on the workload
 * It is a big number because OCF uses allocators
 * for every request it sends and recieves
 */
#define ENV_ALLOCATOR_NBUFS 32767

/* Use unique index for env allocators */
static env_atomic g_env_allocator_index = 0;

void *
env_allocator_new(env_allocator *allocator)
{
	void *mem = spdk_mempool_get(allocator->mempool);

	if (spdk_likely(mem)) {
		memset(mem, 0, allocator->element_size);
	}

	return mem;
}

env_allocator *
env_allocator_create(uint32_t size, const char *name)
{
	env_allocator *allocator;
	char qualified_name[128] = {0};

	snprintf(qualified_name, 128, "ocf_env_%d", env_atomic_inc_return(&g_env_allocator_index));

	allocator = calloc(1, sizeof(*allocator));
	if (!allocator) {
		return NULL;
	}

	allocator->mempool = spdk_mempool_create(qualified_name,
			     ENV_ALLOCATOR_NBUFS, size,
			     SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
			     SPDK_ENV_SOCKET_ID_ANY);

	if (!allocator->mempool) {
		SPDK_ERRLOG("mempool creation failed\n");
		free(allocator);
		return NULL;
	}

	allocator->element_size = size;

	return allocator;
}

void
env_allocator_del(env_allocator *allocator, void *item)
{
	spdk_mempool_put(allocator->mempool, item);
}

void
env_allocator_destroy(env_allocator *allocator)
{
	if (allocator) {
		if (ENV_ALLOCATOR_NBUFS - spdk_mempool_count(allocator->mempool)) {
			SPDK_ERRLOG("Not all objects deallocated\n");
			assert(false);
		}

		spdk_mempool_free(allocator->mempool);
		free(allocator);
	}
}
/* *** CRC *** */

uint32_t
env_crc32(uint32_t crc, uint8_t const *message, size_t len)
{
	return spdk_crc32_ieee_update(message, len, crc);
}

/* EXECUTION CONTEXTS */
pthread_mutex_t *exec_context_mutex;

static void __attribute__((constructor)) init_execution_context(void)
{
	unsigned count = env_get_execution_context_count();
	unsigned i;

	ENV_BUG_ON(count == 0);
	exec_context_mutex = malloc(count * sizeof(exec_context_mutex[0]));
	ENV_BUG_ON(exec_context_mutex == NULL);
	for (i = 0; i < count; i++) {
		ENV_BUG_ON(pthread_mutex_init(&exec_context_mutex[i], NULL));
	}
}

static void __attribute__((destructor)) deinit_execution_context(void)
{
	unsigned count = env_get_execution_context_count();
	unsigned i;

	ENV_BUG_ON(count == 0);
	ENV_BUG_ON(exec_context_mutex == NULL);

	for (i = 0; i < count; i++) {
		ENV_BUG_ON(pthread_mutex_destroy(&exec_context_mutex[i]));
	}
	free(exec_context_mutex);
}

/* get_execuction_context must assure that after the call finishes, the caller
 * will not get preempted from current execution context. For userspace env
 * we simulate this behavior by acquiring per execution context mutex. As a
 * result the caller might actually get preempted, but no other thread will
 * execute in this context by the time the caller puts current execution ctx. */
unsigned env_get_execution_context(void)
{
	unsigned cpu;

	cpu = sched_getcpu();
	cpu = (cpu == -1) ?  0 : cpu;

	ENV_BUG_ON(pthread_mutex_lock(&exec_context_mutex[cpu]));

	return cpu;
}

void env_put_execution_context(unsigned ctx)
{
	pthread_mutex_unlock(&exec_context_mutex[ctx]);
}

unsigned env_get_execution_context_count(void)
{
	int num = sysconf(_SC_NPROCESSORS_ONLN);

	return (num == -1) ? 0 : num;
}
