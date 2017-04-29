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

/*
 *   Copyright (c) 1992-2017 NetApp, Inc.
 *   All rights reserved.
 */


#include "spdk/env.h"
#include "spdk/assert.h"
#include <string.h>
#include <unistd.h>

#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_memzone.h>
#include <rte_version.h>
#include <rte_lcore.h>

/*
 * Insure the definitions in spdk/env.h are equivelant to the DPDK definitions.
 */

SPDK_STATIC_ASSERT(SPDK_MAX_LCORE == RTE_MAX_LCORE, "SPDK_MAX_LCORE != RTE_MAX_LCORE");
SPDK_STATIC_ASSERT(SPDK_MEMPOOL_CACHE_MAX_SIZE == RTE_MEMPOOL_CACHE_MAX_SIZE,
		   "SPDK_MEMPOOL_CACHE_MAX_SIZE != RTE_MEMPOOL_CACHE_MAX_SIZE");

void *
spdk_dma_malloc(size_t size, size_t align, uint64_t *phys_addr)
{
	void *buf = rte_malloc(NULL, size, align);
	if (buf && phys_addr) {
		*phys_addr = rte_malloc_virt2phy(buf);
	}
	return buf;
}

void *
spdk_dma_zmalloc(size_t size, size_t align, uint64_t *phys_addr)
{
	void *buf = spdk_dma_malloc(size, align, phys_addr);
	if (buf) {
		memset(buf, 0, size);
	}
	return buf;
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

unsigned
spdk_mempool_avail_count(const struct spdk_mempool *pool)
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

/**
 * Return the ID of the execution unit we are running on.
 */
unsigned
spdk_lcore_id(void)
{
	return rte_lcore_id();
}

/**
 * Get the id of the master lcore
 */
unsigned
spdk_get_master_lcore(void)
{
	return rte_get_master_lcore();
}

/**
 * Get the ID of the physical socket of the specified lcore
 */
unsigned
spdk_lcore_to_socket_id(unsigned lcore_id)
{
	return rte_lcore_to_socket_id(lcore_id);
}

/**
 * Test if an lcore is enabled.
 */
int
spdk_lcore_is_enabled(unsigned lcore_id)
{
	return rte_lcore_is_enabled(lcore_id);
}

/**
 * Return the number of execution units (lcores) on the system.
 */
unsigned
spdk_lcore_count(void)
{
	return rte_lcore_count();
}

/**
 * Get the next enabled lcore ID.
 */
unsigned
spdk_get_next_lcore(unsigned i, int skip_master, int wrap)
{
	return rte_get_next_lcore(i, skip_master, wrap);
}


/**
 * Wait for all  lcores to finish processing their jobs.
 */
void
spdk_mp_wait_lcore(void)
{
	rte_eal_mp_wait_lcore();
}

/**
 * Return the state of the lcore identified by slave_id.
 */
int
spdk_get_lcore_state(unsigned lcore_id)
{
	return (int) rte_eal_get_lcore_state(lcore_id);
}


/**
 * Wait until a lcore finished its job.
 */
int
spdk_wait_lcore(unsigned slave_id)
{
	return rte_eal_wait_lcore(slave_id);
}

/**
 * Send a message to a slave lcore identified by slave_id to call a
 * function f with argument arg. Once the execution is done, the
 * remote lcore switch in FINISHED state.
 */
int
spdk_remote_launch(int (*f)(void *), void *arg, unsigned slave_id)
{
	return rte_eal_remote_launch(f, arg, slave_id);
}

/**
 * Memcpy
 */
void
spdk_memcpy(void *dst, const void *src, int len)
{
	rte_memcpy(dst, src, len);
}

extern struct rte_tailq_elem rte_ring_tailq;

/**
 * Enqueue several objects on the ring (multi-producers safe).
 *
 */
int
spdk_ring_mp_enqueue_bulk(struct spdk_ring *sr, void *const *obj_table,
			  unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_mp_enqueue_bulk(r, obj_table, n);
}

/**
 * Enqueue several objects on a spdk ring (NOT multi-producers safe).
 *
 */
int
spdk_ring_sp_enqueue_bulk(struct spdk_ring *sr, void *const *obj_table,
			  unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_sp_enqueue_bulk(r, obj_table, n);
}

/**
 * Enqueue several objects on a spdk ring.
 *
 * This function calls the multi-producer or the single-producer
 * version depending on the default behavior that was specified at
 * ring creation time (see flags).
 *
 */
int
spdk_ring_enqueue_bulk(struct spdk_ring *sr, void *const *obj_table,
		       unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_enqueue_bulk(r, obj_table, n);
}

/**
 * Enqueue one object on a spdk ring (multi-producers safe).
 *
 * This function uses a "compare and set" instruction to move the
 * producer index atomically.
 *
 */
int
spdk_ring_mp_enqueue(struct spdk_ring *sr, void *obj)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_mp_enqueue(r, obj);
}

/**
 * Enqueue one object on a spdk ring (NOT multi-producers safe).
 *
 */
int
spdk_ring_sp_enqueue(struct spdk_ring *sr, void *obj)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_sp_enqueue(r, obj);
}

/**
 * Enqueue one object on a spdk ring.
 *
 * This function calls the multi-producer or the single-producer
 * version, depending on the default behaviour that was specified at
 * ring creation time (see flags).
 *
 */
int
spdk_ring_enqueue(struct spdk_ring *sr, void *obj)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_enqueue(r, obj);
}

/**
 * Dequeue several objects from a spdk	ring (multi-consumers safe).
 *
 */
int
spdk_ring_mc_dequeue_bulk(struct spdk_ring *sr, void **obj_table, unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_mc_dequeue_bulk(r, obj_table, n);
}

/**
 * Dequeue several objects from a spdk ring (NOT multi-consumers safe).
 *
 */
int
spdk_ring_sc_dequeue_bulk(struct spdk_ring *sr, void **obj_table, unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_sc_dequeue_bulk(r, obj_table, n);
}

/**
 * Dequeue several objects from a spdk ring.
 *
 * This function calls the multi-consumers or the single-consumer
 * version, depending on the default behaviour that was specified at
 * ring creation time (see flags).
 *
 */
int
spdk_ring_dequeue_bulk(struct spdk_ring *sr, void **obj_table, unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_dequeue_bulk(r, obj_table, n);
}

/**
 * Dequeue one object from a spdk ring (multi-consumers safe).
 *
 * This function uses a "compare and set" instruction to move the
 * consumer index atomically.
 *
 */
int
spdk_ring_mc_dequeue(struct spdk_ring *sr, void **obj_p)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_mc_dequeue(r, obj_p);
}

/**
 * Dequeue one object from a spdk ring (NOT multi-consumers safe).
 *
 */
int
spdk_ring_sc_dequeue(struct spdk_ring *sr, void **obj_p)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_sc_dequeue(r, obj_p);
}

/**
 * Dequeue one object from a spdk ring.
 *
 * This function calls the multi-consumers or the single-consumer
 * version depending on the default behaviour that was specified at
 * ring creation time (see flags).
 *
 */
int
spdk_ring_dequeue(struct spdk_ring *sr, void **obj_p)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_dequeue(r, obj_p);
}

/**
 * Test if a spdk ring is full.
 *
 */
int
spdk_ring_full(const struct spdk_ring *sr)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_full(r);
}

/**
 * Test if a spdk ring is empty.
 *
 */
int
spdk_ring_empty(const struct spdk_ring *sr)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_empty(r);
}

/**
 * Return the number of entries in a spdk ring.
 *
 */
unsigned
spdk_ring_count(const struct spdk_ring *sr)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_count(r);
}

/**
 * Return the number of free entries in a spdk ring.
 *
 */
unsigned
spdk_ring_free_count(const struct spdk_ring *sr)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_free_count(r);
}

/**
 * Enqueue several objects on the ring (multi-producers safe).
 *
 * This function uses a "compare and set" instruction to move the
 * producer index atomically.
 *
 */
unsigned
spdk_ring_mp_enqueue_burst(struct spdk_ring *sr, void *const *obj_table,
			   unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_mp_enqueue_burst(r, obj_table, n);
}

/**
 * Enqueue several objects on a spdk ring (NOT multi-producers safe).
 *
 */
unsigned
spdk_ring_sp_enqueue_burst(struct spdk_ring *sr, void *const *obj_table,
			   unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_sp_enqueue_burst(r, obj_table, n);
}

/**
 * Enqueue several objects on a spdk ring.
 *
 * This function calls the multi-producer or the single-producer
 * version depending on the default behavior that was specified at
 * ring creation time (see flags).
 *
 */
unsigned
spdk_ring_enqueue_burst(struct spdk_ring *sr, void *const *obj_table,
			unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_enqueue_burst(r, obj_table, n);
}

/**
 * Dequeue several objects from a spdk ring (multi-consumers safe). When the request
 * objects are more than the available objects, only dequeue the actual number
 * of objects
 *
 * This function uses a "compare and set" instruction to move the
 * consumer index atomically.
 *
 */
unsigned
spdk_ring_mc_dequeue_burst(struct spdk_ring *sr, void **obj_table, unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_mc_dequeue_burst(r, obj_table, n);
}

/**
 * Dequeue several objects from a spdk ring (NOT multi-consumers safe).When the
 * request objects are more than the available objects, only dequeue the
 * actual number of objects
 */
unsigned
spdk_ring_sc_dequeue_burst(struct spdk_ring *sr, void **obj_table, unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
#if RTE_VERSION < RTE_VERSION_NUM(17,5,0,0)
	return rte_ring_sc_dequeue_burst(r, obj_table, n);
#else
	return rte_ring_sc_dequeue_burst(r, obj_table, n, NULL);
#endif
}

/**
 * Dequeue multiple objects from a spdk ring up to a maximum number.
 *
 * This function calls the multi-consumers or the single-consumer
 * version, depending on the default behaviour that was specified at
 * ring creation time (see flags).
 */
unsigned
spdk_ring_dequeue_burst(struct spdk_ring *sr, void **obj_table, unsigned n)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_dequeue_burst(r, obj_table, n);
}


/**
 * Return the size of memory occupied by a ring
 */
ssize_t
spdk_ring_get_memsize(unsigned count)
{
	return rte_ring_get_memsize(count);
}

/**
 * Create a ring
 */
struct spdk_ring *
spdk_ring_create(const char *name, unsigned count, int socket_id,
		 unsigned flags)
{
	return (struct spdk_ring *) rte_ring_create(name, count, socket_id, flags);
}

/**
 * Free the ring
 */
void
spdk_ring_free(struct spdk_ring *sr)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	rte_ring_free(r);
}

/**
 * Change the high water mark. If *count* is 0, water marking is
 * disabled
 */
int
spdk_ring_set_water_mark(struct spdk_ring *sr, unsigned count)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	return rte_ring_set_water_mark(r, count);
}

/**
 *  Dump the status of the ring on the console
 */
void
spdk_ring_dump(FILE *f, const struct spdk_ring *sr)
{
	struct rte_ring *r = (struct rte_ring *)sr;
	rte_ring_dump(f, r);
}

/**
 * Dump the status of all rings on the console
 */
void
spdk_ring_list_dump(FILE *f)
{
	rte_ring_list_dump(f);
}

/**
 * Search a ring from its name
 */
struct spdk_ring *
spdk_ring_lookup(const char *name)
{
	return (struct spdk_ring *)rte_ring_lookup(name);
}
