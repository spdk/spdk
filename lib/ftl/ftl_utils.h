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

#ifndef FTL_UTILS_H
#define FTL_UTILS_H

#include <spdk/thread.h>
#include <stdatomic.h>

typedef void (*ftl_thread_fn)(void *ctx);
typedef int (*ftl_poller_fn)(void *ctx);

struct ftl_poller;

struct ftl_thread {
	/* Spdk thread */
	struct spdk_thread			*thread;

	/* Thread's name */
	const char				*name;

	/* Thread's id */
	pthread_t				tid;

	/* Communication pipe */
	struct spdk_ring			*ring;

	/* Running flag */
	atomic_int				running;

	/* Initialize flag */
	atomic_int				init;

	/* Thread's loop */
	ftl_thread_fn				fn;

	/* Loop's context */
	void					*ctx;

	/* Poller list */
	LIST_HEAD(, ftl_poller)			pollers;
};

struct ftl_thread *ftl_thread_init(const char *name, size_t qsize, ftl_thread_fn fn,
				   void *ctx, int start);
void	ftl_thread_send_msg(struct ftl_thread *thread, spdk_thread_fn fn, void *ctx);
void	ftl_thread_process(struct ftl_thread *thread);
int	ftl_thread_initialized(struct ftl_thread *thread);
void	ftl_thread_set_initialized(struct ftl_thread *thread);
void	ftl_thread_free(struct ftl_thread *thread);
int	ftl_thread_start(struct ftl_thread *thread);
int	ftl_thread_running(const struct ftl_thread *thread);
void	ftl_thread_stop(struct ftl_thread *thread);
void	ftl_thread_join(struct ftl_thread *thread);

static inline void
ftl_set_bit(unsigned int bit, void *bitmap)
{
	*((char *)bitmap + (bit / CHAR_BIT)) |= 1 << (bit % CHAR_BIT);
}

static inline int
ftl_get_bit(unsigned int bit, const void *bitmap)
{
	return (*((const char *)bitmap + (bit / CHAR_BIT)) >> (bit % CHAR_BIT)) & 1;
}

static inline void
ftl_clr_bit(unsigned int bit, void *bitmap)
{
	*((char *)bitmap + (bit / CHAR_BIT)) &= ~(1 << (bit % CHAR_BIT));
}

#define enabled(x) \
	(x##_ENABLED)

#define ftl_div_up(a, b) \
	(((a) / (b)) + !!((a) % (b)))

#define ftl_range_intersect(s1, e1, s2, e2) \
	((s1) <= (e2) && (s2) <= (e1))

#endif /* FTL_UTILS_H */
