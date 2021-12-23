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


#ifndef __LIBOCF_ENV_H__
#define __LIBOCF_ENV_H__

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef __USE_GNU
#define __USE_GNU
#endif

#include <linux/limits.h>
#include <linux/stddef.h>

#include "spdk/stdinc.h"
#include "spdk/likely.h"
#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/log.h"

#include "ocf_env_list.h"
#include "ocf/ocf_err.h"

#include "mpool.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef uint64_t sector_t;

#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))

/* linux sector 512-bytes */
#define ENV_SECTOR_SHIFT	9
#define ENV_SECTOR_SIZE (1<<ENV_SECTOR_SHIFT)
#define BYTES_TO_SECTOR(x)	((x) >> ENV_SECTOR_SHIFT)

/* *** MEMORY MANAGEMENT *** */

#define ENV_MEM_NORMAL	0
#define ENV_MEM_NOIO	0
#define ENV_MEM_ATOMIC	0

#define likely spdk_likely
#define unlikely spdk_unlikely

#define min(x, y) MIN(x, y)
#ifndef MIN
#define MIN(x, y) spdk_min(x, y)
#endif

#define ARRAY_SIZE(x) SPDK_COUNTOF(x)

/* LOGGING */
#define ENV_PRIu64 PRIu64

#define ENV_WARN(cond, fmt, args...) ({ \
		if (spdk_unlikely((uintptr_t)(cond))) \
			SPDK_NOTICELOG("WARNING" fmt, ##args); \
	})

#define ENV_WARN_ON(cond) ({ \
	if (spdk_unlikely((uintptr_t)(cond))) \
		SPDK_NOTICELOG("WARNING\n"); \
	})

#define ENV_BUG() ({ \
		SPDK_ERRLOG("BUG\n"); \
		assert(0); \
		abort(); \
	})

#define ENV_BUG_ON(cond) ({ \
		if (spdk_unlikely((uintptr_t)(cond))) { \
			SPDK_ERRLOG("BUG\n"); \
			assert(0); \
			abort(); \
		} \
	})

#define ENV_BUILD_BUG_ON(cond)		_Static_assert(!(cond), "static "\
					"assertion failure")

#define container_of(ptr, type, member) SPDK_CONTAINEROF(ptr, type, member)

static inline void *env_malloc(size_t size, int flags)
{
	return spdk_malloc(size, 0, NULL, SPDK_ENV_LCORE_ID_ANY,
			   SPDK_MALLOC_DMA);
}

static inline void *env_zalloc(size_t size, int flags)
{
	return spdk_zmalloc(size, 0, NULL, SPDK_ENV_LCORE_ID_ANY,
			    SPDK_MALLOC_DMA);
}

static inline void env_free(const void *ptr)
{
	return spdk_free((void *)ptr);
}

static inline void *env_vmalloc(size_t size)
{
	return spdk_malloc(size, 0, NULL, SPDK_ENV_LCORE_ID_ANY,
			   SPDK_MALLOC_DMA);
}

static inline void *env_vzalloc(size_t size)
{
	/* TODO: raw_ram init can request huge amount of memory to store
	 * hashtable in it. need to ensure that allocation succeeds */
	return spdk_zmalloc(size, 0, NULL, SPDK_ENV_LCORE_ID_ANY,
			    SPDK_MALLOC_DMA);
}

static inline void *env_vzalloc_flags(size_t size, int flags)
{
	return env_vzalloc(size);
}

static inline void *env_secure_alloc(size_t size)
{
	return spdk_zmalloc(size, 0, NULL, SPDK_ENV_LCORE_ID_ANY,
			    SPDK_MALLOC_DMA);
}

static inline void env_secure_free(const void *ptr, size_t size)
{
	return spdk_free((void *)ptr);
}

static inline void env_vfree(const void *ptr)
{
	return spdk_free((void *)ptr);
}

static inline uint64_t env_get_free_memory(void)
{
	return -1;
}

/* *** ALLOCATOR *** */

#define OCF_ALLOCATOR_NAME_MAX 24

typedef struct {
	struct spdk_mempool *mempool;
	size_t element_size;
	size_t element_count;
	bool zero;
} env_allocator;

env_allocator *env_allocator_create_extended(uint32_t size, const char *name, int limit, bool zero);

env_allocator *env_allocator_create(uint32_t size, const char *name, bool zero);

void env_allocator_destroy(env_allocator *allocator);

void *env_allocator_new(env_allocator *allocator);

void env_allocator_del(env_allocator *allocator, void *item);

uint32_t env_allocator_item_count(env_allocator *allocator);

/* *** MUTEX *** */

typedef struct {
	pthread_mutex_t m;
} env_mutex;

static inline int env_mutex_init(env_mutex *mutex)
{
	return !!pthread_mutex_init(&mutex->m, NULL);
}

static inline void env_mutex_lock(env_mutex *mutex)
{
	ENV_BUG_ON(pthread_mutex_lock(&mutex->m));
}

static inline int env_mutex_lock_interruptible(env_mutex *mutex)
{
	env_mutex_lock(mutex);
	return 0;
}

static inline int env_mutex_trylock(env_mutex *mutex)
{
	return pthread_mutex_trylock(&mutex->m) ? -OCF_ERR_NO_LOCK : 0;
}

static inline void env_mutex_unlock(env_mutex *mutex)
{
	ENV_BUG_ON(pthread_mutex_unlock(&mutex->m));
}

static inline int env_mutex_is_locked(env_mutex *mutex)
{
	if (env_mutex_trylock(mutex) == 0) {
		env_mutex_unlock(mutex);
		return 0;
	}

	return 1;
}

static inline int env_mutex_destroy(env_mutex *mutex)
{
	if (pthread_mutex_destroy(&mutex->m)) {
		return 1;
	}

	return 0;
}

/* *** RECURSIVE MUTEX *** */

typedef env_mutex env_rmutex;

static inline int env_rmutex_init(env_rmutex *rmutex)
{
	pthread_mutexattr_t attr;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&rmutex->m, &attr);

	return 0;
}

static inline void env_rmutex_lock(env_rmutex *rmutex)
{
	env_mutex_lock(rmutex);
}

static inline int env_rmutex_lock_interruptible(env_rmutex *rmutex)
{
	return env_mutex_lock_interruptible(rmutex);
}

static inline int env_rmutex_trylock(env_rmutex *rmutex)
{
	return env_mutex_trylock(rmutex);
}

static inline void env_rmutex_unlock(env_rmutex *rmutex)
{
	env_mutex_unlock(rmutex);
}

static inline int env_rmutex_is_locked(env_rmutex *rmutex)
{
	return env_mutex_is_locked(rmutex);
}

static inline int env_rmutex_destroy(env_rmutex *rmutex)
{
	return env_mutex_destroy(rmutex);
}

/* *** RW SEMAPHORE *** */
typedef struct {
	pthread_rwlock_t lock;
} env_rwsem;

static inline int env_rwsem_init(env_rwsem *s)
{
	return !!pthread_rwlock_init(&s->lock, NULL);
}

static inline void env_rwsem_up_read(env_rwsem *s)
{
	ENV_BUG_ON(pthread_rwlock_unlock(&s->lock));
}

static inline void env_rwsem_down_read(env_rwsem *s)
{
	ENV_BUG_ON(pthread_rwlock_rdlock(&s->lock));
}

static inline int env_rwsem_down_read_trylock(env_rwsem *s)
{
	return pthread_rwlock_tryrdlock(&s->lock) ? -OCF_ERR_NO_LOCK : 0;
}

static inline void env_rwsem_up_write(env_rwsem *s)
{
	ENV_BUG_ON(pthread_rwlock_unlock(&s->lock));
}

static inline void env_rwsem_down_write(env_rwsem *s)
{
	ENV_BUG_ON(pthread_rwlock_wrlock(&s->lock));
}

static inline int env_rwsem_down_write_trylock(env_rwsem *s)
{
	return pthread_rwlock_trywrlock(&s->lock) ? -OCF_ERR_NO_LOCK : 0;
}

static inline int env_rwsem_is_locked(env_rwsem *s)
{
	if (env_rwsem_down_read_trylock(s) == 0) {
		env_rwsem_up_read(s);
		return 0;
	}

	return 1;
}

static inline int env_rwsem_down_read_interruptible(env_rwsem *s)
{
	return pthread_rwlock_rdlock(&s->lock);
}
static inline int env_rwsem_down_write_interruptible(env_rwsem *s)
{
	return pthread_rwlock_wrlock(&s->lock);
}

static inline int env_rwsem_destroy(env_rwsem *s)
{
	return pthread_rwlock_destroy(&s->lock);
}

/* *** ATOMIC VARIABLES *** */

typedef int env_atomic;

typedef long env_atomic64;

#ifndef atomic_read
#define atomic_read(ptr)       (*(__typeof__(*ptr) *volatile) (ptr))
#endif

#ifndef atomic_set
#define atomic_set(ptr, i)     ((*(__typeof__(*ptr) *volatile) (ptr)) = (i))
#endif

#define atomic_inc(ptr)        ((void) __sync_fetch_and_add(ptr, 1))
#define atomic_dec(ptr)        ((void) __sync_fetch_and_add(ptr, -1))
#define atomic_add(ptr, n)     ((void) __sync_fetch_and_add(ptr, n))
#define atomic_sub(ptr, n)     ((void) __sync_fetch_and_sub(ptr, n))

#define atomic_cmpxchg         __sync_val_compare_and_swap

static inline int env_atomic_read(const env_atomic *a)
{
	return atomic_read(a);
}

static inline void env_atomic_set(env_atomic *a, int i)
{
	atomic_set(a, i);
}

static inline void env_atomic_add(int i, env_atomic *a)
{
	atomic_add(a, i);
}

static inline void env_atomic_sub(int i, env_atomic *a)
{
	atomic_sub(a, i);
}

static inline bool env_atomic_sub_and_test(int i, env_atomic *a)
{
	return __sync_sub_and_fetch(a, i) == 0;
}

static inline void env_atomic_inc(env_atomic *a)
{
	atomic_inc(a);
}

static inline void env_atomic_dec(env_atomic *a)
{
	atomic_dec(a);
}

static inline bool env_atomic_dec_and_test(env_atomic *a)
{
	return __sync_sub_and_fetch(a, 1) == 0;
}

static inline bool env_atomic_inc_and_test(env_atomic *a)
{
	return __sync_add_and_fetch(a, 1) == 0;
}

static inline int env_atomic_add_return(int i, env_atomic *a)
{
	return __sync_add_and_fetch(a, i);
}

static inline int env_atomic_sub_return(int i, env_atomic *a)
{
	return __sync_sub_and_fetch(a, i);
}

static inline int env_atomic_inc_return(env_atomic *a)
{
	return env_atomic_add_return(1, a);
}

static inline int env_atomic_dec_return(env_atomic *a)
{
	return env_atomic_sub_return(1, a);
}

static inline int env_atomic_cmpxchg(env_atomic *a, int old, int new_value)
{
	return atomic_cmpxchg(a, old, new_value);
}

static inline int env_atomic_add_unless(env_atomic *a, int i, int u)
{
	int c, old;
	c = env_atomic_read(a);
	for (;;) {
		if (spdk_unlikely(c == (u))) {
			break;
		}
		old = env_atomic_cmpxchg((a), c, c + (i));
		if (spdk_likely(old == c)) {
			break;
		}
		c = old;
	}
	return c != (u);
}

static inline long env_atomic64_read(const env_atomic64 *a)
{
	return atomic_read(a);
}

static inline void env_atomic64_set(env_atomic64 *a, long i)
{
	atomic_set(a, i);
}

static inline void env_atomic64_add(long i, env_atomic64 *a)
{
	atomic_add(a, i);
}

static inline void env_atomic64_sub(long i, env_atomic64 *a)
{
	atomic_sub(a, i);
}

static inline void env_atomic64_inc(env_atomic64 *a)
{
	atomic_inc(a);
}

static inline void env_atomic64_dec(env_atomic64 *a)
{
	atomic_dec(a);
}

static inline int env_atomic64_add_return(int i, env_atomic *a)
{
	return __sync_add_and_fetch(a, i);
}

static inline int env_atomic64_sub_return(int i, env_atomic *a)
{
	return __sync_sub_and_fetch(a, i);
}

static inline int env_atomic64_inc_return(env_atomic *a)
{
	return env_atomic64_add_return(1, a);
}

static inline int env_atomic64_dec_return(env_atomic *a)
{
	return env_atomic_sub_return(1, a);
}

static inline long env_atomic64_cmpxchg(env_atomic64 *a, long old, long new)
{
	return atomic_cmpxchg(a, old, new);
}

/* *** COMPLETION *** */
typedef struct completion {
	sem_t sem;
} env_completion;

static inline void env_completion_init(env_completion *completion)
{
	sem_init(&completion->sem, 0, 0);
}

static inline void env_completion_wait(env_completion *completion)
{
	sem_wait(&completion->sem);
}

static inline void env_completion_complete(env_completion *completion)
{
	sem_post(&completion->sem);
}

static inline void env_completion_destroy(env_completion *completion)
{
	sem_destroy(&completion->sem);
}

/* *** SPIN LOCKS *** */

typedef struct {
	pthread_spinlock_t lock;
} env_spinlock;

static inline int env_spinlock_init(env_spinlock *l)
{
	return pthread_spin_init(&l->lock, 0);
}

static inline int env_spinlock_trylock(env_spinlock *l)
{
	return pthread_spin_trylock(&l->lock) ? -OCF_ERR_NO_LOCK : 0;
}

static inline void env_spinlock_lock(env_spinlock *l)
{
	ENV_BUG_ON(pthread_spin_lock(&l->lock));
}

static inline void env_spinlock_unlock(env_spinlock *l)
{
	ENV_BUG_ON(pthread_spin_unlock(&l->lock));
}

#define env_spinlock_lock_irqsave(l, flags) \
		(void)flags; \
		env_spinlock_lock(l)

#define env_spinlock_unlock_irqrestore(l, flags) \
		(void)flags; \
		env_spinlock_unlock(l)

static inline void env_spinlock_destroy(env_spinlock *l)
{
	ENV_BUG_ON(pthread_spin_destroy(&l->lock));
}

/* *** RW LOCKS *** */

typedef struct {
	pthread_rwlock_t lock;
} env_rwlock;

static inline void env_rwlock_init(env_rwlock *l)
{
	ENV_BUG_ON(pthread_rwlock_init(&l->lock, NULL));
}

static inline void env_rwlock_read_lock(env_rwlock *l)
{
	ENV_BUG_ON(pthread_rwlock_rdlock(&l->lock));
}

static inline void env_rwlock_read_unlock(env_rwlock *l)
{
	ENV_BUG_ON(pthread_rwlock_unlock(&l->lock));
}

static inline void env_rwlock_write_lock(env_rwlock *l)
{
	ENV_BUG_ON(pthread_rwlock_wrlock(&l->lock));
}

static inline void env_rwlock_write_unlock(env_rwlock *l)
{
	ENV_BUG_ON(pthread_rwlock_unlock(&l->lock));
}

static inline void env_rwlock_destroy(env_rwlock *l)
{
	ENV_BUG_ON(pthread_rwlock_destroy(&l->lock));
}

static inline void env_bit_set(int nr, volatile void *addr)
{
	char *byte = (char *)addr + (nr >> 3);
	char mask = 1 << (nr & 7);

	__sync_or_and_fetch(byte, mask);
}

static inline void env_bit_clear(int nr, volatile void *addr)
{
	char *byte = (char *)addr + (nr >> 3);
	char mask = 1 << (nr & 7);

	__sync_and_and_fetch(byte, ~mask);
}

static inline bool env_bit_test(int nr, const volatile unsigned long *addr)
{
	const char *byte = (char *)addr + (nr >> 3);
	char mask = 1 << (nr & 7);

	return !!(*byte & mask);
}

/* *** WAITQUEUE *** */

typedef struct {
	sem_t sem;
} env_waitqueue;

static inline void env_waitqueue_init(env_waitqueue *w)
{
	sem_init(&w->sem, 0, 0);
}

static inline void env_waitqueue_wake_up(env_waitqueue *w)
{
	sem_post(&w->sem);
}

#define env_waitqueue_wait(w, condition)	\
({						\
	int __ret = 0;				\
	if (!(condition))			\
		sem_wait(&w.sem);		\
	__ret = __ret;				\
})

/* *** SCHEDULING *** */

/* CAS does not need this while in user-space */
static inline void env_schedule(void)
{
}

#define env_cond_resched	env_schedule

static inline int env_in_interrupt(void)
{
	return 0;
}

static inline uint64_t env_get_tick_count(void)
{
	return spdk_get_ticks();
}

static inline uint64_t env_ticks_to_secs(uint64_t j)
{
	return j / spdk_get_ticks_hz();
}

static inline uint64_t env_ticks_to_msecs(uint64_t j)
{
	return env_ticks_to_secs(j) * 1000;
}

static inline uint64_t env_ticks_to_nsecs(uint64_t j)
{
	return env_ticks_to_secs(j) * 1000 * 1000;
}

static inline uint64_t env_ticks_to_usecs(uint64_t j)
{
	return env_ticks_to_secs(j) * 1000 * 1000 * 1000;
}

static inline uint64_t env_secs_to_ticks(uint64_t j)
{
	return j * spdk_get_ticks_hz();
}

/* *** STRING OPERATIONS *** */

/* 512 KB is sufficient amount of memory for OCF operations */
#define ENV_MAX_MEM (512 * 1024)

static inline int env_memset(void *dest, size_t len, uint8_t value)
{
	if (dest == NULL || len == 0) {
		return 1;
	}

	memset(dest, value, len);
	return 0;
}

static inline int env_memcpy(void *dest, size_t dmax, const void *src, size_t len)
{
	if (dest == NULL || src == NULL) {
		return 1;
	}
	if (dmax == 0 || dmax > ENV_MAX_MEM) {
		return 1;
	}
	if (len == 0 || len > dmax) {
		return 1;
	}

	memcpy(dest, src, len);
	return 0;
}

static inline int env_memcmp(const void *aptr, size_t dmax, const void *bptr, size_t len,
			     int *diff)
{
	if (diff == NULL || aptr == NULL || bptr == NULL) {
		return 1;
	}
	if (dmax == 0 || dmax > ENV_MAX_MEM) {
		return 1;
	}
	if (len == 0 || len > dmax) {
		return 1;
	}

	*diff = memcmp(aptr, bptr, len);
	return 0;
}

/* 4096 is sufficient max length for any OCF operation on string */
#define ENV_MAX_STR (4 * 1024)

static inline size_t env_strnlen(const char *src, size_t dmax)
{
	return strnlen(src, dmax);
}

static inline int env_strncpy(char *dest, size_t dmax, const char *src, size_t len)
{
	if (dest == NULL  || src == NULL) {
		return 1;
	}
	if (dmax == 0 || dmax > ENV_MAX_STR) {
		return 1;
	}
	if (len == 0) {
		return 1;
	}
	/* Just copy as many characters as we can instead of return failure */
	len = min(len, dmax);

	strncpy(dest, src, len);
	return 0;
}

#define env_strncmp(s1, slen1, s2, slen2) strncmp(s1, s2, min(slen1, slen2))

static inline char *env_strdup(const char *src, int flags)
{
	int len;
	char *ret;

	if (src == NULL) {
		return NULL;
	}

	len = env_strnlen(src, ENV_MAX_STR) + 1;
	ret = env_malloc(len, flags);

	if (env_strncpy(ret, ENV_MAX_STR, src, len)) {
		return NULL;
	} else {
		return ret;
	}
}

/* *** SORTING *** */

static inline void env_sort(void *base, size_t num, size_t size,
			    int (*cmp_fn)(const void *, const void *),
			    void (*swap_fn)(void *, void *, int size))
{
	qsort(base, num, size, cmp_fn);
}

static inline void env_msleep(uint64_t n)
{
	usleep(n * 1000);
}

static inline void env_touch_softlockup_wd(void)
{
}

/* *** CRC *** */

uint32_t env_crc32(uint32_t crc, uint8_t const *data, size_t len);

/* EXECUTION CONTEXTS */
unsigned env_get_execution_context(void);
void env_put_execution_context(unsigned ctx);
unsigned env_get_execution_context_count(void);

#endif /* __OCF_ENV_H__ */
