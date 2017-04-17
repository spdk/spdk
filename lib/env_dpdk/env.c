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
#include <rte_lcore.h>
#include <rte_launch.h>
#include <rte_memcpy.h>
#include <rte_ring.h>
#include <rte_virtio_net.h>
#include <rte_mbuf.h>
#include <rte_atomic.h>

void *
spdk_malloc_socket(const char *type, size_t size, unsigned align, int socket_arg);


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
spdk_zmalloc_phy(size_t size, size_t align, uint64_t *phys_addr)
{
	void *buf = spdk_malloc(size, align, phys_addr);
	if (buf) {
		memset(buf, 0, size);
	}
	return buf;
}

void *
spdk_zmalloc(const char *type, size_t size, unsigned align)
{
	return rte_zmalloc(type, size, align);
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

int
spdk_mempool_get(struct spdk_mempool *mp, void **obj_p)
{
	return rte_mempool_get((struct rte_mempool *)mp, obj_p);
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

unsigned
spdk_lcore_id(void)
{
	return rte_lcore_id();
}

unsigned
spdk_mempool_avail_count(struct spdk_mempool *pool)
{
#if RTE_VERSION < RTE_VERSION_NUM(16, 7, 0, 1)
	return rte_mempool_count((const struct rte_mempool *)pool);
#else
	return rte_mempool_avail_count((const struct rte_mempool *)pool);
#endif
}

char *
spdk_mempool_name(struct spdk_mempool *pool)
{
	return ((struct rte_mempool *)pool)->name;
}

unsigned
spdk_get_master_lcore(void)
{
	return rte_get_master_lcore();
}

/**
 * Get the next enabled lcore ID.
 *
 * @param i
 *   The current lcore (reference).
 * @param skip_master
 *   If true, do not return the ID of the master lcore.
 * @param wrap
 *   If true, go back to 0 when SPDK_MAX_LCORE is reached; otherwise,
 *   return SPDK_MAX_LCORE.
 * @return
 *   The next lcore_id or SPDK_MAX_LCORE if not found.
 */
unsigned
spdk_get_next_lcore(unsigned i, int skip_master, int wrap)
{
	return rte_get_next_lcore(i, skip_master, wrap);
}

/*
 * Send a message to a slave lcore identified by slave_id to call a
 * function f with argument arg. Once the execution is done, the
 * remote lcore switch in FINISHED state.
 */
int
spdk_eal_remote_launch(int (*f)(void *), void *arg, unsigned slave_id)
{
	return rte_eal_remote_launch(f, arg, slave_id);
}

/*
 * Wait until a lcore finished its job.
 */
int
spdk_eal_wait_lcore(unsigned slave_id)
{
	return rte_eal_wait_lcore(slave_id);
}

void *
spdk_memcpy(void *dest, const void *src, size_t n)
{
	return rte_memcpy(dest, src, n);
}
int
spdk_ring_mp_enqueue(struct spdk_ring *r, void *obj)
{
	return rte_ring_mp_enqueue((struct rte_ring *)r, obj);
}

unsigned
spdk_ring_sc_dequeue_burst(struct spdk_ring *r, void **obj_table, unsigned n)
{
	return rte_ring_sc_dequeue_burst((struct rte_ring *)r, obj_table, n);
}
unsigned
spdk_lcore_to_socket_id(unsigned lcore_id)
{
	return rte_lcore_to_socket_id(lcore_id);
}
struct spdk_ring *
spdk_ring_create(const char *name, unsigned count,
		 int socket_id, unsigned flags)
{
	return (struct spdk_ring *)rte_ring_create(name, count, socket_id, flags);
}

enum spdk_lcore_state_t
spdk_eal_get_lcore_state(unsigned lcore_id) {
	return (enum spdk_lcore_state_t)rte_eal_get_lcore_state(lcore_id);
}

int
spdk_lcore_is_enabled(unsigned lcore_id)
{
	return rte_lcore_is_enabled(lcore_id);
}
void
spdk_eal_mp_wait_lcore(void)
{
	rte_eal_mp_wait_lcore();
}

void
spdk_ring_free(struct spdk_ring *r)
{
	rte_ring_free((struct rte_ring *)r);
}

spdk_phys_addr_t
spdk_mempool_virt2phy(__spdk_unused const struct spdk_mempool *mp, const void *elt)
{
	return rte_mempool_virt2phy((const struct rte_mempool *)mp, elt);
}

unsigned
spdk_socket_id(void)
{
	return rte_socket_id();
}

struct spdk_mempool *
spdk_mempool_create_full(const char *name, unsigned n, unsigned elt_size,
			 unsigned cache_size, unsigned private_data_size,
			 spdk_mempool_ctor_t *mp_init, void *mp_init_arg,
			 spdk_mempool_obj_cb_t *obj_init, void *obj_init_arg,
			 int socket_id, unsigned flags)
{
	return (struct spdk_mempool *)rte_mempool_create(name, n, elt_size, cache_size, private_data_size,
			(rte_mempool_ctor_t *)mp_init, mp_init_arg,
			(rte_mempool_obj_cb_t *)obj_init, obj_init_arg, socket_id,
			flags);
}

void
spdk_exit(int exit_code, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	rte_exit(exit_code, format, ap);
	va_end(ap);
}
unsigned
spdk_lcore_count(void)
{
	return rte_lcore_count();
}

int
spdk_vhost_driver_session_start(void)
{
	return rte_vhost_driver_session_start();
}

int
spdk_vhost_driver_unregister(const char *path)
{
	return rte_vhost_driver_unregister(path);
}

int
spdk_vhost_enable_guest_notification(int vid, uint16_t queue_id, int enable)
{
	return rte_vhost_enable_guest_notification(vid, queue_id, enable);
}

int
spdk_vhost_driver_callback_register(struct virtio_net_device_ops const *const ops)
{
	return rte_vhost_driver_callback_register(ops);
}

void *spdk_malloc_type(const char *type, size_t size, size_t align)
{
	return rte_malloc(type, size, align);
}

void *
spdk_malloc_socket(const char *type, size_t size, unsigned align, int socket_arg)
{
	return rte_malloc_socket(type, size, align, socket_arg);
}

void __spdk_panic(const char *function, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	__rte_panic(function, format, ap);
	va_end(ap);
}

void
spdk_set_log_level(int a)
{
	rte_set_log_level(a);
}
