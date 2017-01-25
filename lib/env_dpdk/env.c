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

#include <string.h>
#include <unistd.h>

#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_memzone.h>
#include <rte_version.h>

void *
spdk_malloc(size_t size, size_t align, uint64_t *phys_addr)
{
	void *buf = rte_malloc(NULL, size, align);
	if (buf && phys_addr) {
		*phys_addr = rte_malloc_virt2phy(buf);
	}
	return buf;
}

void *
spdk_zmalloc(size_t size, size_t align, uint64_t *phys_addr)
{
	void *buf = spdk_malloc(size, align, phys_addr);
	if (buf) {
		memset(buf, 0, size);
	}
	return buf;
}

void *
spdk_realloc(void *buf, size_t size, size_t align, uint64_t *phys_addr)
{
	void *new_buf = rte_realloc(buf, size, align);
	if (new_buf && phys_addr) {
		*phys_addr = rte_malloc_virt2phy(new_buf);
	}
	return new_buf;
}

void
spdk_free(void *buf)
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

void *
spdk_call_unaffinitized(void *cb(void *arg), void *arg)
{
	rte_cpuset_t orig_cpuset, new_cpuset;
	void *ret;
	long num_cores, i;

	if (cb == NULL) {
		return NULL;
	}

	rte_thread_get_affinity(&orig_cpuset);

	CPU_ZERO(&new_cpuset);

	num_cores = sysconf(_SC_NPROCESSORS_CONF);

	/* Create a mask containing all CPUs */
	for (i = 0; i < num_cores; i++) {
		CPU_SET(i, &new_cpuset);
	}

	rte_thread_set_affinity(&new_cpuset);

	ret = cb(arg);

	rte_thread_set_affinity(&orig_cpuset);

	return ret;
}
