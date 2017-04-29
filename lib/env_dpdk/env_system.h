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

#ifndef SPDK_ENV_SYSTEM_H
#define SPDK_ENV_SYSTEM_H

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stddef.h>  /* for offsetof */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/queue.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h> /* for struct iovec */
#include <unistd.h>

#ifdef __FreeBSD__
#include <pthread_np.h>
#endif

#if defined(__i386__) || defined(__x86_64__)
#include <x86intrin.h>
#endif

#include <rte_config.h>
#include <rte_lcore.h>
#include <rte_memcpy.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_version.h>

typedef pthread_mutex_t spdk_mutex_t;
typedef pthread_t spdk_thread_t;
typedef pthread_key_t spdk_thread_key_t;
typedef pid_t spdk_pid_t;

#define SPDK_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER

static inline
void __attribute__((noreturn)) spdk_abort(void)
{
	abort();
}

static inline
void __attribute__((noreturn)) spdk_exit(int status)
{
	exit(status);
}

static inline
void *spdk_malloc(size_t size)
{
	return malloc(size);
}

static inline
void *spdk_realloc(void *buf, size_t size)
{
	return realloc(buf, size);
}

static inline
void *spdk_calloc(size_t nmemb, size_t size)
{
	return calloc(nmemb, size);
}

static inline
void spdk_free(void *ptr)
{
	free(ptr);
}

static inline
void *spdk_strdup(const char *s)
{
	return strdup(s);
}

static inline
FILE *spdk_fopen(const char *filename, const char *modes)
{
	return fopen(filename, modes);
}

static inline
char *spdk_fgets(char *s, int n, FILE *stream)
{
	return fgets(s, n, stream);
}

static inline
int spdk_fflush(FILE *stream)
{
	return fflush(stream);
}

static inline
int spdk_feof(FILE *stream)
{
	return feof(stream);
}

static inline
int spdk_fclose(FILE *stream)
{
	return fclose(stream);
}


/* Destroy a mutex.  */
static inline
int spdk_mutex_destroy(spdk_mutex_t *__mutex)
{
	return pthread_mutex_destroy(__mutex);
}

/* Try locking a mutex.  */
static inline
int spdk_mutex_trylock(spdk_mutex_t *__mutex)
{
	return pthread_mutex_trylock(__mutex);
}

/* Lock a mutex.  */
static inline
int spdk_mutex_lock(spdk_mutex_t *__mutex)
{
	return pthread_mutex_lock(__mutex);
}

/* Unlock a mutex.  */
static inline
int spdk_mutex_unlock(spdk_mutex_t *__mutex)
{
	return pthread_mutex_unlock(__mutex);
}

static inline
int spdk_mutex_consistent(spdk_mutex_t *__mutex)
{
#ifndef __FreeBSD__
	return pthread_mutex_consistent(__mutex);
#else
	return 0;
#endif
}

static inline
spdk_thread_t spdk_thread_self(void)
{
	return pthread_self();
}

static inline
void spdk_thread_set_name(spdk_thread_t tid, const char *thread_name)
{
#if defined(__linux__)
	prctl(PR_SET_NAME, thread_name, 0, 0, 0);
#elif defined(__FreeBSD__)
	pthread_set_name_np(tid, thread_name);
#else
#error missing platform support for thread name
#endif
}

static inline
int spdk_thread_key_create(spdk_thread_key_t *key, void (*destructor)(void *))
{
	return pthread_key_create(key, destructor);
}

static inline
void *spdk_thread_getspecific(spdk_thread_key_t key)
{
	return pthread_getspecific(key);
}

static inline
int spdk_thread_setspecific(spdk_thread_key_t key, const void *value)
{
	return pthread_setspecific(key, value);
}

static inline
int spdk_thread_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
	return pthread_sigmask(how, set, oldset);
}

static inline
int spdk_usleep(int usec)
{
	return usleep(usec);
}

static inline
spdk_pid_t spdk_getpid(void)
{
	return getpid();
}


#endif /* SPDK_ENV_SYSTEM_H */
