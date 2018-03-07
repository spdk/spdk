/*-
 *
 *   Copyright(c) 2015-2017 Intel Corporation. All rights reserved.
 *   Copyright 2014 6WIND S.A.
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

#ifndef _RTE_MEMPOOL_H_
#define _RTE_MEMPOOL_H_

/**
 * @file
 * RTE Mempool.
 *
 * A memory pool is an allocator of fixed-size object. It is
 * identified by its name, and uses a ring to store free objects. It
 * provides some other optional services, like a per-core object
 * cache, and an alignment helper to ensure that objects are padded
 * to spread them equally on all RAM channels, ranks, and so on.
 *
 * Objects owned by a mempool should never be added in another
 * mempool. When an object is freed using rte_mempool_put() or
 * equivalent, the object data is not modified; the user can save some
 * meta-data in the object data and retrieve them when allocating a
 * new object.
 *
 * Note: the mempool implementation is not preemptible. An lcore must not be
 * interrupted by another task that uses the same mempool (because it uses a
 * ring which is not preemptible). Also, usual mempool functions like
 * rte_mempool_get() or rte_mempool_put() are designed to be called from an EAL
 * thread due to the internal per-lcore cache. Due to the lack of caching,
 * rte_mempool_get() or rte_mempool_put() performance will suffer when called
 * by non-EAL threads. Instead, non-EAL threads should call
 * rte_mempool_generic_get() or rte_mempool_generic_put() with a user cache
 * created with rte_mempool_cache_create().
 */

#include <rte_config.h>
#include <rte_spinlock.h>
#include <rte_debug.h>
#include <rte_ring.h>
#include <rte_memcpy.h>

#ifdef __cplusplus
extern "C" {
#endif

/* In order to mock some DPDK functions, we place headers here with the name name as the DPDK headers
 * so these definitions wil be picked up.  Only what's mocked is included.
 */

STAILQ_HEAD(rte_mempool_objhdr_list, rte_mempool_objhdr);
STAILQ_HEAD(rte_mempool_memhdr_list, rte_mempool_memhdr);
struct rte_mempool {
	char name[RTE_MEMZONE_NAMESIZE];
	RTE_STD_C11
	union {
		void *pool_data;
		uint64_t pool_id;
	};
	void *pool_config;
	const struct rte_memzone *mz;
	unsigned int flags;
	int socket_id;
	uint32_t size;
	uint32_t cache_size;
	uint32_t elt_size;
	uint32_t header_size;
	uint32_t trailer_size;
	unsigned private_data_size;
	int32_t ops_index;
	struct rte_mempool_cache *local_cache;
	uint32_t populated_size;
	struct rte_mempool_objhdr_list elt_list;
	uint32_t nb_mem_chunks;
	struct rte_mempool_memhdr_list mem_list;
#ifdef RTE_LIBRTE_MEMPOOL_DEBUG
	struct rte_mempool_debug_stats stats[RTE_MAX_LCORE];
#endif
}  __rte_cache_aligned;
#define RTE_MEMPOOL_OPS_NAMESIZE 32
typedef int (*rte_mempool_alloc_t)(struct rte_mempool *mp);
typedef void (*rte_mempool_free_t)(struct rte_mempool *mp);
typedef int (*rte_mempool_enqueue_t)(struct rte_mempool *mp,
				     void *const *obj_table, unsigned int n);
typedef int (*rte_mempool_dequeue_t)(struct rte_mempool *mp,
				     void **obj_table, unsigned int n);
typedef unsigned(*rte_mempool_get_count)(const struct rte_mempool *mp);
typedef int (*rte_mempool_get_capabilities_t)(const struct rte_mempool *mp,
		unsigned int *flags);
typedef int (*rte_mempool_ops_register_memory_area_t)
(const struct rte_mempool *mp, char *vaddr, rte_iova_t iova, size_t len);
struct rte_mempool_ops {
	char name[RTE_MEMPOOL_OPS_NAMESIZE];
	rte_mempool_alloc_t alloc;
	rte_mempool_free_t free;
	rte_mempool_enqueue_t enqueue;
	rte_mempool_dequeue_t dequeue;
	rte_mempool_get_count get_count;
	rte_mempool_get_capabilities_t get_capabilities;
	rte_mempool_ops_register_memory_area_t register_memory_area;
} __rte_cache_aligned;
#define RTE_MEMPOOL_MAX_OPS_IDX 16
struct rte_mempool_ops_table {
	rte_spinlock_t sl;
	uint32_t num_ops;
	struct rte_mempool_ops ops[RTE_MEMPOOL_MAX_OPS_IDX];
} __rte_cache_aligned;
extern struct rte_mempool_ops_table rte_mempool_ops_table;
void
rte_mempool_free(struct rte_mempool *mp);
static __rte_always_inline void
rte_mempool_put_bulk(struct rte_mempool *mp, void *const *obj_table,
		     unsigned int n);

#ifdef __cplusplus
}
#endif

#endif /* _RTE_MEMPOOL_H_ */
