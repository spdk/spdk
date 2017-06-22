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

#include "spdk/env.h"

#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_memzone.h>
#include <rte_version.h>

void *
spdk_dma_malloc_socket(size_t size, size_t align, uint64_t *phys_addr, int socket_id)
{
	void *buf = rte_malloc_socket(NULL, size, align, socket_id);
	if (buf && phys_addr) {
		*phys_addr = rte_malloc_virt2phy(buf);
	}
	return buf;
}

void *
spdk_dma_zmalloc_socket(size_t size, size_t align, uint64_t *phys_addr, int socket_id)
{
	void *buf = spdk_dma_malloc_socket(size, align, phys_addr, socket_id);
	if (buf) {
		memset(buf, 0, size);
	}
	return buf;
}

void *
spdk_dma_malloc(size_t size, size_t align, uint64_t *phys_addr)
{
	return spdk_dma_malloc_socket(size, align, phys_addr, SPDK_ENV_SOCKET_ID_ANY);
}

void *
spdk_dma_zmalloc(size_t size, size_t align, uint64_t *phys_addr)
{
	return spdk_dma_zmalloc_socket(size, align, phys_addr, SPDK_ENV_SOCKET_ID_ANY);
}

void *
spdk_dma_realloc(void *buf, size_t size, size_t align, uint64_t *phys_addr)
{
	void *new_buf = rte_realloc(buf, size, align);
	if (new_buf && phys_addr) {
		*phys_addr = rte_malloc_virt2phy(new_buf);
	}
	return new_buf;
}

void
spdk_dma_free(void *buf)
{
	rte_free(buf);
}

void *
spdk_memzone_reserve(const char *name, size_t len, int socket_id, unsigned flags)
{
	const struct rte_memzone *mz;

	if (socket_id == SPDK_ENV_SOCKET_ID_ANY) {
		socket_id = SOCKET_ID_ANY;
	}

	mz = rte_memzone_reserve(name, len, socket_id, flags);

	if (mz != NULL) {
		memset(mz->addr, 0, len);
		return mz->addr;
	} else {
		return NULL;
	}
}

void *
spdk_memzone_lookup(const char *name)
{
	const struct rte_memzone *mz = rte_memzone_lookup(name);

	if (mz != NULL) {
		return mz->addr;
	} else {
		return NULL;
	}
}

int
spdk_memzone_free(const char *name)
{
	const struct rte_memzone *mz = rte_memzone_lookup(name);

	if (mz != NULL) {
		return rte_memzone_free(mz);
	}

	return -1;
}

void
spdk_memzone_dump(FILE *f)
{
	rte_memzone_dump(f);
}

struct spdk_mempool *
spdk_mempool_create(const char *name, size_t count,
		    size_t ele_size, size_t cache_size, int socket_id)
{
	struct rte_mempool *mp;
	size_t tmp;

	if (socket_id == SPDK_ENV_SOCKET_ID_ANY) {
		socket_id = SOCKET_ID_ANY;
	}

	/* No more than half of all elements can be in cache */
	tmp = (count / 2) / rte_lcore_count();
	if (cache_size > tmp) {
		cache_size = tmp;
	}

	if (cache_size > RTE_MEMPOOL_CACHE_MAX_SIZE) {
		cache_size = RTE_MEMPOOL_CACHE_MAX_SIZE;
	}

	mp = rte_mempool_create(name, count, ele_size, cache_size,
				0, NULL, NULL, NULL, NULL,
				socket_id, 0);

	return (struct spdk_mempool *)mp;
}

void
spdk_mempool_free(struct spdk_mempool *mp)
{
#if RTE_VERSION >= RTE_VERSION_NUM(16, 7, 0, 1)
	rte_mempool_free((struct rte_mempool *)mp);
#endif
}

void *
spdk_mempool_get(struct spdk_mempool *mp)
{
	void *ele = NULL;

	rte_mempool_get((struct rte_mempool *)mp, &ele);

	return ele;
}

void
spdk_mempool_put(struct spdk_mempool *mp, void *ele)
{
	rte_mempool_put((struct rte_mempool *)mp, ele);
}

void
spdk_mempool_put_bulk(struct spdk_mempool *mp, void *const *ele_arr, size_t count)
{
	rte_mempool_put_bulk((struct rte_mempool *)mp, ele_arr, count);
}

size_t
spdk_mempool_count(const struct spdk_mempool *pool)
{
#if RTE_VERSION < RTE_VERSION_NUM(16, 7, 0, 1)
	return rte_mempool_count((struct rte_mempool *)pool);
#else
	return rte_mempool_avail_count((struct rte_mempool *)pool);
#endif
}

bool
spdk_process_is_primary(void)
{
	return (rte_eal_process_type() == RTE_PROC_PRIMARY);
}

uint64_t spdk_get_ticks(void)
{
	return rte_get_timer_cycles();
}

uint64_t spdk_get_ticks_hz(void)
{
	return rte_get_timer_hz();
}

void spdk_delay_us(unsigned int us)
{
	rte_delay_us(us);
}

void
spdk_unaffinitize_thread(void)
{
	rte_cpuset_t new_cpuset;
	long num_cores, i;

	CPU_ZERO(&new_cpuset);

	num_cores = sysconf(_SC_NPROCESSORS_CONF);

	/* Create a mask containing all CPUs */
	for (i = 0; i < num_cores; i++) {
		CPU_SET(i, &new_cpuset);
	}

	rte_thread_set_affinity(&new_cpuset);
}

void *
spdk_call_unaffinitized(void *cb(void *arg), void *arg)
{
	rte_cpuset_t orig_cpuset;
	void *ret;

	if (cb == NULL) {
		return NULL;
	}

	rte_thread_get_affinity(&orig_cpuset);

	spdk_unaffinitize_thread();

	ret = cb(arg);

	rte_thread_set_affinity(&orig_cpuset);

	return ret;
}

struct spdk_ring *
spdk_ring_create(enum spdk_ring_type type, size_t count, int socket_id)
{
	char ring_name[64];
	static uint32_t ring_num = 0;
	unsigned flags = 0;

	switch (type) {
	case SPDK_RING_TYPE_SP_SC:
		flags = RING_F_SP_ENQ | RING_F_SC_DEQ;
		break;
	case SPDK_RING_TYPE_MP_SC:
		flags = RING_F_SC_DEQ;
		break;
	default:
		return NULL;
	}

	snprintf(ring_name, sizeof(ring_name), "ring_%u_%d",
		 __sync_fetch_and_add(&ring_num, 1), getpid());

	return (struct spdk_ring *)rte_ring_create(ring_name, count, socket_id, flags);
}

void
spdk_ring_free(struct spdk_ring *ring)
{
	rte_ring_free((struct rte_ring *)ring);
}

size_t
spdk_ring_enqueue(struct spdk_ring *ring, void **objs, size_t count)
{
	int rc;
#if RTE_VERSION < RTE_VERSION_NUM(17, 5, 0, 0)
	rc = rte_ring_mp_enqueue_bulk((struct rte_ring *)ring, objs, count);
	if (rc == 0) {
		return count;
	}

	return 0;
#else
	rc = rte_ring_mp_enqueue_bulk((struct rte_ring *)ring, objs, count, NULL);
	return rc;
#endif
}

size_t
spdk_ring_dequeue(struct spdk_ring *ring, void **objs, size_t count)
{
#if RTE_VERSION < RTE_VERSION_NUM(17, 5, 0, 0)
	return rte_ring_sc_dequeue_burst((struct rte_ring *)ring, objs, count);
#else
	return rte_ring_sc_dequeue_burst((struct rte_ring *)ring, objs, count, NULL);
#endif
}
