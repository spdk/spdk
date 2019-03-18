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

#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_memzone.h>
#include <rte_version.h>

static uint64_t
virt_to_phys(void *vaddr)
{
	uint64_t ret;

#if RTE_VERSION >= RTE_VERSION_NUM(17, 11, 0, 3)
	ret = rte_malloc_virt2iova(vaddr);
	if (ret != RTE_BAD_IOVA) {
		return ret;
	}
#else
	ret = rte_malloc_virt2phy(vaddr);
	if (ret != RTE_BAD_PHYS_ADDR) {
		return ret;
	}
#endif

	return spdk_vtophys(vaddr, NULL);
}

void *
spdk_malloc(size_t size, size_t align, uint64_t *phys_addr, int socket_id, uint32_t flags)
{
	if (flags == 0) {
		return NULL;
	}

	void *buf = rte_malloc_socket(NULL, size, align, socket_id);
	if (buf && phys_addr) {
		*phys_addr = virt_to_phys(buf);
	}
	return buf;
}

void *
spdk_zmalloc(size_t size, size_t align, uint64_t *phys_addr, int socket_id, uint32_t flags)
{
	void *buf = spdk_malloc(size, align, phys_addr, socket_id, flags);
	if (buf) {
		memset(buf, 0, size);
	}
	return buf;
}

void
spdk_free(void *buf)
{
	rte_free(buf);
}

void *
spdk_dma_malloc_socket(size_t size, size_t align, uint64_t *phys_addr, int socket_id)
{
	return spdk_malloc(size, align, phys_addr, socket_id, (SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE));
}

void *
spdk_dma_zmalloc_socket(size_t size, size_t align, uint64_t *phys_addr, int socket_id)
{
	return spdk_zmalloc(size, align, phys_addr, socket_id, (SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE));
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
		*phys_addr = virt_to_phys(new_buf);
	}
	return new_buf;
}

void
spdk_dma_free(void *buf)
{
	spdk_free(buf);
}

void *
spdk_memzone_reserve_aligned(const char *name, size_t len, int socket_id,
			     unsigned flags, unsigned align)
{
	const struct rte_memzone *mz;
	unsigned dpdk_flags = 0;

#if RTE_VERSION >= RTE_VERSION_NUM(18, 05, 0, 0)
	/* Older DPDKs do not offer such flag since their
	 * memzones are iova-contiguous by default.
	 */
	if ((flags & SPDK_MEMZONE_NO_IOVA_CONTIG) == 0) {
		dpdk_flags |= RTE_MEMZONE_IOVA_CONTIG;
	}
#endif

	if (socket_id == SPDK_ENV_SOCKET_ID_ANY) {
		socket_id = SOCKET_ID_ANY;
	}

	mz = rte_memzone_reserve_aligned(name, len, socket_id, dpdk_flags, align);

	if (mz != NULL) {
		memset(mz->addr, 0, len);
		return mz->addr;
	} else {
		return NULL;
	}
}

void *
spdk_memzone_reserve(const char *name, size_t len, int socket_id, unsigned flags)
{
	return spdk_memzone_reserve_aligned(name, len, socket_id, flags,
					    RTE_CACHE_LINE_SIZE);
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

static int
spdk_ring_mempool_alloc(struct rte_mempool *mp)
{
	struct spdk_ring *ring;

	ring = spdk_ring_create(SPDK_RING_TYPE_MP_MC, mp->size, mp->socket_id);
	if (ring == NULL) {
		return -ENOMEM;
	}
	mp->pool_data = ring;

	return 0;
}

static void
spdk_ring_mempool_free(struct rte_mempool *mp)
{
	spdk_ring_free((struct spdk_ring *)mp->pool_data);
}

static int
spdk_ring_mempool_enqueue(struct rte_mempool *mp, void *const *obj_table, unsigned n)
{
	return spdk_ring_enqueue(mp->pool_data, obj_table, n) == 0 ? -ENOBUFS : 0;
}

static int
spdk_ring_mempool_dequeue(struct rte_mempool *mp, void **obj_table, unsigned n)
{
	return spdk_ring_dequeue(mp->pool_data, obj_table, n) == 0 ? -ENOBUFS : 0;
}

static unsigned
spdk_ring_mempool_get_count(const struct rte_mempool *mp)
{
	return spdk_ring_count(mp->pool_data);
}

static const struct rte_mempool_ops spdk_mempool_ops = {
	.name = "spdk_ring_mempool",
	.alloc = spdk_ring_mempool_alloc,
	.free = spdk_ring_mempool_free,
	.enqueue = spdk_ring_mempool_enqueue,
	.dequeue = spdk_ring_mempool_dequeue,
	.get_count = spdk_ring_mempool_get_count,
};

MEMPOOL_REGISTER_OPS(spdk_mempool_ops);

struct spdk_mempool *
spdk_mempool_create_ctor(const char *name, size_t count,
			 size_t ele_size, size_t cache_size, int socket_id,
			 spdk_mempool_obj_cb_t *obj_init, void *obj_init_arg)
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
				0, NULL, NULL, (rte_mempool_obj_cb_t *)obj_init, obj_init_arg,
				socket_id, MEMPOOL_F_NO_PHYS_CONTIG);

	return (struct spdk_mempool *)mp;
}

static void
spdk_mempool_free_chunk(struct rte_mempool_memhdr *memhdr, void *opaque)
{
	spdk_free(memhdr->addr);
}

static int
spdk_mempool_populate_aligned(struct rte_mempool *pool, size_t alignment)
{
	size_t ele_size;
	size_t total_allocation_size;
	size_t header_padding = 0;
	size_t alignment_padding = 0;
	size_t initial_padding;
	size_t i, offset;
	struct rte_mempool_memhdr *memhdr;
	void *mem_region;

	/* We need to pad the end of each element with enough room for the header amd trailer */
	header_padding = pool->header_size;
	ele_size = pool->header_size + pool->elt_size + pool->trailer_size;

	if (alignment != 0) {
		/* And we need to align the base of the structures. */
		if (ele_size % alignment) {
			alignment_padding = alignment - (ele_size % alignment);
		}

		ele_size += alignment_padding;

		/* We also need to make sure we have enough space for the first header */
		if (header_padding > alignment) {
			initial_padding = alignment * (header_padding / alignment);
			initial_padding += alignment;
		} else {
			initial_padding = alignment;
		}
	} else {
		initial_padding = header_padding;
	}

	/* Overflow check */
	if (ele_size > SIZE_MAX / pool->size) {
		return -EINVAL;
	}

	total_allocation_size = ele_size * pool->size + initial_padding;
	mem_region = spdk_zmalloc(total_allocation_size, alignment, NULL, pool->socket_id,
				  SPDK_MALLOC_DMA && SPDK_MALLOC_SHARE);
	if (mem_region == NULL) {
		return -ENOMEM;
	}

	memhdr = spdk_zmalloc(sizeof(*memhdr), 0, NULL, pool->socket_id, SPDK_MALLOC_DMA &&
			      SPDK_MALLOC_SHARE);
	if (memhdr == NULL) {
		spdk_free(mem_region);
		return -ENOMEM;
	}

	memhdr->mp = pool;
	memhdr->addr = mem_region;
	memhdr->iova = RTE_BAD_IOVA;
	memhdr->len = total_allocation_size;
	memhdr->opaque = NULL;
	memhdr->free_cb = spdk_mempool_free_chunk;

	STAILQ_INSERT_TAIL(&pool->mem_list, memhdr, next);
	pool->nb_mem_chunks++;

	/* Initial padding is guaranteed to be some multiple of alignment */
	offset = initial_padding;
	for (i = 0; i < pool->size; i++) {
		mem_region = (char *)mem_region + offset;
		/* Make sure all elements are aligned going in. */
		if (alignment != 0) {
			assert(((uintptr_t)mem_region & (~(uintptr_t)(alignment - 1))) == (uintptr_t)mem_region);
		}
		rte_mempool_ops_enqueue_bulk(pool, &mem_region, 1);
		/* This includes the size of the header for the next element. */
		offset += ele_size;
		assert(offset <= total_allocation_size);
	}
	return 0;
}

struct spdk_mempool *
spdk_mempool_create(const char *name, size_t count,
		    size_t ele_size, size_t cache_size, int socket_id)
{
	return spdk_mempool_create_aligned(name, count, ele_size, 0, cache_size, socket_id);
}

struct spdk_mempool *
spdk_mempool_create_aligned(const char *name, size_t count, size_t ele_size,
			    size_t alignment, size_t cache_size, int socket_id)
{
	struct rte_mempool *mp;
	size_t tmp;
	int ret;

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

	mp = rte_mempool_create_empty(name, count, ele_size, cache_size, 0, socket_id,
				      MEMPOOL_F_NO_PHYS_CONTIG);
	if (mp == NULL) {
		goto error;
	}

	ret = rte_mempool_set_ops_byname(mp, "spdk_ring_mempool", NULL);
	if (ret) {
		goto error;
	}

	ret = rte_mempool_ops_alloc(mp);
	if (ret) {
		goto error;
	}

	ret = spdk_mempool_populate_aligned(mp, alignment);
	if (ret) {
		goto error;
	}

	return (struct spdk_mempool *)mp;

error:
	rte_mempool_ops_free(mp);
	return NULL;
}

char *
spdk_mempool_get_name(struct spdk_mempool *mp)
{
	return ((struct rte_mempool *)mp)->name;
}

void
spdk_mempool_free(struct spdk_mempool *mp)
{
	struct rte_mempool *pool = (struct rte_mempool *)mp;

	rte_mempool_free(pool);
}

void *
spdk_mempool_get(struct spdk_mempool *mp)
{
	void *ele = NULL;
	int rc;

	rc = rte_mempool_get((struct rte_mempool *)mp, &ele);
	if (rc != 0) {
		return NULL;
	}
	return ele;
}

int
spdk_mempool_get_bulk(struct spdk_mempool *mp, void **ele_arr, size_t count)
{
	return rte_mempool_get_bulk((struct rte_mempool *)mp, ele_arr, count);
}

void
spdk_mempool_put(struct spdk_mempool *mp, void *ele)
{
	rte_mempool_put((struct rte_mempool *)mp, ele);
}

void
spdk_mempool_put_bulk(struct spdk_mempool *mp, void **ele_arr, size_t count)
{
	rte_mempool_put_bulk((struct rte_mempool *)mp, ele_arr, count);
}

size_t
spdk_mempool_count(const struct spdk_mempool *pool)
{
	return rte_mempool_avail_count((struct rte_mempool *)pool);
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

void spdk_pause(void)
{
	rte_pause();
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
	case SPDK_RING_TYPE_MP_MC:
		flags = 0;
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
spdk_ring_count(struct spdk_ring *ring)
{
	return rte_ring_count((struct rte_ring *)ring);
}

size_t
spdk_ring_enqueue(struct spdk_ring *ring, void *const *objs, size_t count)
{
	int rc;
#if RTE_VERSION < RTE_VERSION_NUM(17, 5, 0, 0)
	rc = rte_ring_enqueue_bulk((struct rte_ring *)ring, objs, count);
	if (rc == 0) {
		return count;
	}

	return 0;
#else
	rc = rte_ring_enqueue_bulk((struct rte_ring *)ring, objs, count, NULL);
	return rc;
#endif
}

size_t
spdk_ring_dequeue(struct spdk_ring *ring, void **objs, size_t count)
{
#if RTE_VERSION < RTE_VERSION_NUM(17, 5, 0, 0)
	return rte_ring_dequeue_burst((struct rte_ring *)ring, objs, count);
#else
	return rte_ring_dequeue_burst((struct rte_ring *)ring, objs, count, NULL);
#endif
}
