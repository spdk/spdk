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

#include <spdk/stdinc.h>
#include <spdk/env.h>
#include <spdk/log.h>
#include <spdk/ocssd.h>
#include "ocssd_utils.h"

struct ocssd_msg {
	spdk_thread_fn			fn;

	void				*ctx;
};

struct ocssd_poller {
	/* Poller function */
	ocssd_poller_fn			fn;

	/* Poller's argument */
	void				*arg;

	/* Poller's frequency */
	uint64_t			period_ms;

	/* List link */
	LIST_ENTRY(ocssd_poller)	list_entry;
};

static struct spdk_poller *
ocssd_thread_start_poller(void *thread_ctx, spdk_poller_fn fn, void *arg, uint64_t period_ms)
{
	struct ocssd_thread *thread = thread_ctx;
	struct ocssd_poller *poller;

	poller = calloc(1, sizeof(*poller));
	if (!poller) {
		return NULL;
	}

	poller->fn = fn;
	poller->arg = arg;
	poller->period_ms = period_ms;

	LIST_INSERT_HEAD(&thread->pollers, poller, list_entry);

	return (struct spdk_poller *)poller;
}

static void
ocssd_thread_stop_poller(struct spdk_poller *spdk_poller, void *thread_ctx)
{
	struct ocssd_poller *poller = (struct ocssd_poller *)spdk_poller;

	LIST_REMOVE(poller, list_entry);
	free(poller);
}

static void
ocssd_thread_pass_msg(spdk_thread_fn fn, void *ctx, void *thread_ctx)
{
	struct ocssd_thread *thread = thread_ctx;
	struct ocssd_msg *msg;
	size_t count;

	msg = calloc(1, sizeof(*msg));
	msg->fn = fn;
	msg->ctx = ctx;

	count = spdk_ring_enqueue(thread->ring, (void **)&msg, 1);
	if (count != 1) {
		SPDK_ERRLOG("Unable to send message to thread: [%s]\n",
			    thread->name);
		free(msg);
		assert(0);
	}
}

static void *
ocssd_trampoline(void *ctx)
{
	struct ocssd_thread *thread = ctx;

	thread->thread = spdk_allocate_thread(ocssd_thread_pass_msg,
					      ocssd_thread_start_poller,
					      ocssd_thread_stop_poller,
					      thread, thread->name);
	if (!thread->thread) {
		return NULL;
	}

	thread->fn(thread->ctx);
	return NULL;
}

struct ocssd_thread *
ocssd_thread_init(const char *name, size_t qsize, ocssd_thread_fn fn,
		  void *ctx, int start)
{
	struct ocssd_thread *thread;

	thread = calloc(1, sizeof(*thread));
	if (!thread) {
		return NULL;
	}

	thread->ctx = ctx;
	thread->name = name;
	thread->running = 1;
	thread->fn = fn;
	thread->ring = spdk_ring_create(SPDK_RING_TYPE_MP_SC, qsize,
					SPDK_ENV_SOCKET_ID_ANY);
	if (!thread->ring) {
		goto error;
	}

	if (start && ocssd_thread_start(thread)) {
		goto error;
	}

	return thread;
error:
	ocssd_thread_free(thread);
	return NULL;
}

static void
ocssd_thread_process_msg(struct ocssd_thread *thread)
{
	struct ocssd_msg *msg;

	if (spdk_ring_dequeue(thread->ring, (void **)&msg, 1)) {
		msg->fn(msg->ctx);
		free(msg);
	}
}

int
ocssd_thread_start(struct ocssd_thread *thread)
{
	return pthread_create(&thread->tid, NULL, ocssd_trampoline, thread);
}

void
ocssd_thread_process(struct ocssd_thread *thread)
{
	struct ocssd_poller *poll, *tpoll;

	ocssd_thread_process_msg(thread);

	LIST_FOREACH_SAFE(poll, &thread->pollers, list_entry, tpoll) {
		poll->fn(poll->arg);
	}
}

void
ocssd_thread_send_msg(struct ocssd_thread *thread, spdk_thread_fn fn, void *ctx)
{
	spdk_thread_send_msg(thread->thread, fn, ctx);
}

int
ocssd_thread_initialized(struct ocssd_thread *thread)
{
	return atomic_load(&thread->init);
}

void
ocssd_thread_set_initialized(struct ocssd_thread *thread)
{
	atomic_store(&thread->init, 1);
}

void
ocssd_thread_free(struct ocssd_thread *thread)
{
	spdk_ring_free(thread->ring);
	free(thread);
}

int
ocssd_thread_running(const struct ocssd_thread *thread)
{
	return atomic_load(&((struct ocssd_thread *)thread)->running);
}

void
ocssd_thread_join(struct ocssd_thread *thread)
{
	pthread_join(thread->tid, NULL);
}

void
ocssd_thread_stop(struct ocssd_thread *thread)
{
	atomic_store(&thread->running, 0);
}
