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

#include "spdk/io_channel.h"
#include "spdk/log.h"

#ifdef __linux__
#include <sys/prctl.h>
#endif

#ifdef __FreeBSD__
#include <pthread_np.h>
#endif

/*
 * SPDK has tried to limit dependence on thread-local storage for use
 * cases where SPDK libraries are used in non-POSIX userspace environments.
 * So use __thread here with a fallback implementation if SPDK_FORBID_TLS
 * is defined for a non-POSIX userspace environment.
 */
#ifndef SPDK_FORBID_TLS
static __thread struct spdk_thread *g_thread = NULL;
#endif

static pthread_mutex_t g_devlist_mutex = PTHREAD_MUTEX_INITIALIZER;

struct io_device {
	void			*io_device;
	spdk_io_channel_create_cb create_cb;
	spdk_io_channel_destroy_cb destroy_cb;
	spdk_io_device_unregister_cb unregister_cb;
	uint32_t		ctx_size;
	uint32_t		for_each_count;
	TAILQ_ENTRY(io_device)	tailq;

	bool			unregistered;
};

static TAILQ_HEAD(, io_device) g_io_devices = TAILQ_HEAD_INITIALIZER(g_io_devices);

struct spdk_thread {
	pthread_t thread_id;
	spdk_thread_pass_msg msg_fn;
	spdk_start_poller start_poller_fn;
	spdk_stop_poller stop_poller_fn;
	void *thread_ctx;
	TAILQ_HEAD(, spdk_io_channel) io_channels;
	TAILQ_ENTRY(spdk_thread) tailq;
	char *name;
};

static TAILQ_HEAD(, spdk_thread) g_threads = TAILQ_HEAD_INITIALIZER(g_threads);

static struct spdk_thread *
_get_thread(void)
{
#ifndef SPDK_FORBID_TLS
	return g_thread;
#else
	pthread_t thread_id;
	struct spdk_thread *thread;

	thread_id = pthread_self();

	thread = NULL;
	TAILQ_FOREACH(thread, &g_threads, tailq) {
		if (thread->thread_id == thread_id) {
			return thread;
		}
	}

	return NULL;
#endif
}

static void
_set_thread_name(const char *thread_name)
{
#if defined(__linux__)
	prctl(PR_SET_NAME, thread_name, 0, 0, 0);
#elif defined(__FreeBSD__)
	pthread_set_name_np(pthread_self(), thread_name);
#else
#error missing platform support for thread name
#endif
}

struct spdk_thread *
spdk_allocate_thread(spdk_thread_pass_msg msg_fn,
		     spdk_start_poller start_poller_fn,
		     spdk_stop_poller stop_poller_fn,
		     void *thread_ctx, const char *name)
{
	struct spdk_thread *thread;

	pthread_mutex_lock(&g_devlist_mutex);

	thread = _get_thread();
	if (thread) {
		SPDK_ERRLOG("Double allocated SPDK thread\n");
		pthread_mutex_unlock(&g_devlist_mutex);
		return NULL;
	}

	thread = calloc(1, sizeof(*thread));
	if (!thread) {
		SPDK_ERRLOG("Unable to allocate memory for thread\n");
		pthread_mutex_unlock(&g_devlist_mutex);
		return NULL;
	}

	thread->thread_id = pthread_self();
	thread->msg_fn = msg_fn;
	thread->start_poller_fn = start_poller_fn;
	thread->stop_poller_fn = stop_poller_fn;
	thread->thread_ctx = thread_ctx;
	TAILQ_INIT(&thread->io_channels);
	TAILQ_INSERT_TAIL(&g_threads, thread, tailq);
	if (name) {
		_set_thread_name(name);
		thread->name = strdup(name);
	}

	pthread_mutex_unlock(&g_devlist_mutex);

#ifndef SPDK_FORBID_TLS
	g_thread = thread;
#endif

	return thread;
}

void
spdk_free_thread(void)
{
	struct spdk_thread *thread;

#ifndef SPDK_FORBID_TLS
	g_thread = NULL;
#endif

	pthread_mutex_lock(&g_devlist_mutex);

	thread = _get_thread();
	if (!thread) {
		SPDK_ERRLOG("No thread allocated\n");
		pthread_mutex_unlock(&g_devlist_mutex);
		return;
	}

	TAILQ_REMOVE(&g_threads, thread, tailq);
	free(thread->name);
	free(thread);

	pthread_mutex_unlock(&g_devlist_mutex);
}

struct spdk_thread *
spdk_get_thread(void)
{
	struct spdk_thread *thread;

	pthread_mutex_lock(&g_devlist_mutex);

	thread = _get_thread();
	if (!thread) {
		SPDK_ERRLOG("No thread allocated\n");
	}

	pthread_mutex_unlock(&g_devlist_mutex);

	return thread;
}

const char *
spdk_thread_get_name(const struct spdk_thread *thread)
{
	return thread->name;
}

void
spdk_thread_send_msg(const struct spdk_thread *thread, spdk_thread_fn fn, void *ctx)
{
	thread->msg_fn(fn, ctx, thread->thread_ctx);
}


struct spdk_poller *
spdk_poller_register(spdk_poller_fn fn,
		     void *arg,
		     uint64_t period_microseconds)
{
	struct spdk_thread *thread;
	struct spdk_poller *poller;

	thread = spdk_get_thread();
	if (!thread) {
		abort();
	}

	if (!thread->start_poller_fn || !thread->stop_poller_fn) {
		SPDK_ERRLOG("No related functions to start requested poller\n");
		abort();
	}

	poller = thread->start_poller_fn(thread->thread_ctx, fn, arg, period_microseconds);
	if (!poller) {
		SPDK_ERRLOG("Unable to start requested poller\n");
		abort();
	}

	return poller;
}

void
spdk_poller_unregister(struct spdk_poller **ppoller)
{
	struct spdk_thread *thread;
	struct spdk_poller *poller;

	poller = *ppoller;
	if (poller == NULL) {
		return;
	}

	*ppoller = NULL;

	thread = spdk_get_thread();

	if (thread) {
		thread->stop_poller_fn(poller, thread->thread_ctx);
	}
}

struct call_thread {
	struct spdk_thread *cur_thread;
	spdk_thread_fn fn;
	void *ctx;

	struct spdk_thread *orig_thread;
	spdk_thread_fn cpl;
};

static void
spdk_on_thread(void *ctx)
{
	struct call_thread *ct = ctx;

	ct->fn(ct->ctx);

	pthread_mutex_lock(&g_devlist_mutex);
	ct->cur_thread = TAILQ_NEXT(ct->cur_thread, tailq);
	pthread_mutex_unlock(&g_devlist_mutex);

	if (!ct->cur_thread) {
		spdk_thread_send_msg(ct->orig_thread, ct->cpl, ct->ctx);
		free(ctx);
	} else {
		spdk_thread_send_msg(ct->cur_thread, spdk_on_thread, ctx);
	}
}

void
spdk_for_each_thread(spdk_thread_fn fn, void *ctx, spdk_thread_fn cpl)
{
	struct call_thread *ct;

	ct = calloc(1, sizeof(*ct));
	if (!ct) {
		SPDK_ERRLOG("Unable to perform thread iteration\n");
		cpl(ctx);
		return;
	}

	ct->fn = fn;
	ct->ctx = ctx;
	ct->cpl = cpl;

	pthread_mutex_lock(&g_devlist_mutex);
	ct->orig_thread = _get_thread();
	ct->cur_thread = TAILQ_FIRST(&g_threads);
	pthread_mutex_unlock(&g_devlist_mutex);

	spdk_thread_send_msg(ct->cur_thread, spdk_on_thread, ct);
}

void
spdk_io_device_register(void *io_device, spdk_io_channel_create_cb create_cb,
			spdk_io_channel_destroy_cb destroy_cb, uint32_t ctx_size)
{
	struct io_device *dev, *tmp;

	dev = calloc(1, sizeof(struct io_device));
	if (dev == NULL) {
		SPDK_ERRLOG("could not allocate io_device\n");
		return;
	}

	dev->io_device = io_device;
	dev->create_cb = create_cb;
	dev->destroy_cb = destroy_cb;
	dev->unregister_cb = NULL;
	dev->ctx_size = ctx_size;
	dev->for_each_count = 0;
	dev->unregistered = false;

	pthread_mutex_lock(&g_devlist_mutex);
	TAILQ_FOREACH(tmp, &g_io_devices, tailq) {
		if (tmp->io_device == io_device) {
			SPDK_ERRLOG("io_device %p already registered\n", io_device);
			free(dev);
			pthread_mutex_unlock(&g_devlist_mutex);
			return;
		}
	}
	TAILQ_INSERT_TAIL(&g_io_devices, dev, tailq);
	pthread_mutex_unlock(&g_devlist_mutex);
}

static void
_spdk_io_device_attempt_free(struct io_device *dev)
{
	struct spdk_thread *thread;
	struct spdk_io_channel *ch;

	pthread_mutex_lock(&g_devlist_mutex);
	TAILQ_FOREACH(thread, &g_threads, tailq) {
		TAILQ_FOREACH(ch, &thread->io_channels, tailq) {
			if (ch->dev == dev) {
				/* A channel that references this I/O
				 * device still exists. Defer deletion
				 * until it is removed.
				 */
				pthread_mutex_unlock(&g_devlist_mutex);
				return;
			}
		}
	}
	pthread_mutex_unlock(&g_devlist_mutex);

	if (dev->unregister_cb) {
		dev->unregister_cb(dev->io_device);
	}

	free(dev);
}

void
spdk_io_device_unregister(void *io_device, spdk_io_device_unregister_cb unregister_cb)
{
	struct io_device *dev;

	pthread_mutex_lock(&g_devlist_mutex);
	TAILQ_FOREACH(dev, &g_io_devices, tailq) {
		if (dev->io_device == io_device) {
			break;
		}
	}

	if (!dev) {
		SPDK_ERRLOG("io_device %p not found\n", io_device);
		pthread_mutex_unlock(&g_devlist_mutex);
		return;
	}

	if (dev->for_each_count > 0) {
		SPDK_ERRLOG("io_device %p has %u for_each calls outstanding\n", io_device, dev->for_each_count);
		pthread_mutex_unlock(&g_devlist_mutex);
		return;
	}

	dev->unregister_cb = unregister_cb;
	dev->unregistered = true;
	TAILQ_REMOVE(&g_io_devices, dev, tailq);
	pthread_mutex_unlock(&g_devlist_mutex);
	_spdk_io_device_attempt_free(dev);
}

struct spdk_io_channel *
spdk_get_io_channel(void *io_device)
{
	struct spdk_io_channel *ch;
	struct spdk_thread *thread;
	struct io_device *dev;
	int rc;

	pthread_mutex_lock(&g_devlist_mutex);
	TAILQ_FOREACH(dev, &g_io_devices, tailq) {
		if (dev->io_device == io_device) {
			break;
		}
	}
	if (dev == NULL) {
		SPDK_ERRLOG("could not find io_device %p\n", io_device);
		pthread_mutex_unlock(&g_devlist_mutex);
		return NULL;
	}

	thread = _get_thread();
	if (!thread) {
		SPDK_ERRLOG("No thread allocated\n");
		pthread_mutex_unlock(&g_devlist_mutex);
		return NULL;
	}

	TAILQ_FOREACH(ch, &thread->io_channels, tailq) {
		if (ch->dev == dev) {
			ch->ref++;
			/*
			 * An I/O channel already exists for this device on this
			 *  thread, so return it.
			 */
			pthread_mutex_unlock(&g_devlist_mutex);
			return ch;
		}
	}

	ch = calloc(1, sizeof(*ch) + dev->ctx_size);
	if (ch == NULL) {
		SPDK_ERRLOG("could not calloc spdk_io_channel\n");
		pthread_mutex_unlock(&g_devlist_mutex);
		return NULL;
	}

	ch->dev = dev;
	ch->destroy_cb = dev->destroy_cb;
	ch->thread = thread;
	ch->ref = 1;
	TAILQ_INSERT_TAIL(&thread->io_channels, ch, tailq);

	pthread_mutex_unlock(&g_devlist_mutex);

	rc = dev->create_cb(io_device, (uint8_t *)ch + sizeof(*ch));
	if (rc == -1) {
		pthread_mutex_lock(&g_devlist_mutex);
		TAILQ_REMOVE(&ch->thread->io_channels, ch, tailq);
		free(ch);
		pthread_mutex_unlock(&g_devlist_mutex);
		return NULL;
	}

	return ch;
}

static void
_spdk_put_io_channel(void *arg)
{
	struct spdk_io_channel *ch = arg;

	if (ch->ref == 0) {
		SPDK_ERRLOG("ref already zero\n");
		return;
	}

	ch->ref--;

	if (ch->ref > 0) {
		return;
	}

	ch->destroy_cb(ch->dev->io_device, spdk_io_channel_get_ctx(ch));

	pthread_mutex_lock(&g_devlist_mutex);
	TAILQ_REMOVE(&ch->thread->io_channels, ch, tailq);
	pthread_mutex_unlock(&g_devlist_mutex);

	if (ch->dev->unregistered) {
		_spdk_io_device_attempt_free(ch->dev);
	}
	free(ch);
}

void
spdk_put_io_channel(struct spdk_io_channel *ch)
{
	spdk_thread_send_msg(ch->thread, _spdk_put_io_channel, ch);
}

struct spdk_io_channel *
spdk_io_channel_from_ctx(void *ctx)
{
	return (struct spdk_io_channel *)((uint8_t *)ctx - sizeof(struct spdk_io_channel));
}

struct spdk_thread *
spdk_io_channel_get_thread(struct spdk_io_channel *ch)
{
	return ch->thread;
}

struct spdk_io_channel_iter {
	void *io_device;
	struct io_device *dev;
	spdk_channel_msg fn;
	int status;
	void *ctx;
	struct spdk_io_channel *ch;

	struct spdk_thread *cur_thread;

	struct spdk_thread *orig_thread;
	spdk_channel_for_each_cpl cpl;
};

void *
spdk_io_channel_iter_get_io_device(struct spdk_io_channel_iter *i)
{
	return i->io_device;
}

struct spdk_io_channel *
spdk_io_channel_iter_get_channel(struct spdk_io_channel_iter *i)
{
	return i->ch;
}

void *
spdk_io_channel_iter_get_ctx(struct spdk_io_channel_iter *i)
{
	return i->ctx;
}

static void
_call_completion(void *ctx)
{
	struct spdk_io_channel_iter *i = ctx;

	if (i->cpl != NULL) {
		i->cpl(i, i->status);
	}
	free(i);
}

static void
_call_channel(void *ctx)
{
	struct spdk_io_channel_iter *i = ctx;
	struct spdk_io_channel *ch;

	/*
	 * It is possible that the channel was deleted before this
	 *  message had a chance to execute.  If so, skip calling
	 *  the fn() on this thread.
	 */
	pthread_mutex_lock(&g_devlist_mutex);
	TAILQ_FOREACH(ch, &i->cur_thread->io_channels, tailq) {
		if (ch->dev->io_device == i->io_device) {
			break;
		}
	}
	pthread_mutex_unlock(&g_devlist_mutex);

	if (ch) {
		i->fn(i);
	} else {
		spdk_for_each_channel_continue(i, 0);
	}
}

void
spdk_for_each_channel(void *io_device, spdk_channel_msg fn, void *ctx,
		      spdk_channel_for_each_cpl cpl)
{
	struct spdk_thread *thread;
	struct spdk_io_channel *ch;
	struct spdk_io_channel_iter *i;

	i = calloc(1, sizeof(*i));
	if (!i) {
		SPDK_ERRLOG("Unable to allocate iterator\n");
		return;
	}

	i->io_device = io_device;
	i->fn = fn;
	i->ctx = ctx;
	i->cpl = cpl;

	pthread_mutex_lock(&g_devlist_mutex);
	i->orig_thread = _get_thread();

	TAILQ_FOREACH(thread, &g_threads, tailq) {
		TAILQ_FOREACH(ch, &thread->io_channels, tailq) {
			if (ch->dev->io_device == io_device) {
				ch->dev->for_each_count++;
				i->dev = ch->dev;
				i->cur_thread = thread;
				i->ch = ch;
				pthread_mutex_unlock(&g_devlist_mutex);
				spdk_thread_send_msg(thread, _call_channel, i);
				return;
			}
		}
	}

	pthread_mutex_unlock(&g_devlist_mutex);

	cpl(i, 0);

	free(i);
}

void
spdk_for_each_channel_continue(struct spdk_io_channel_iter *i, int status)
{
	struct spdk_thread *thread;
	struct spdk_io_channel *ch;

	assert(i->cur_thread == spdk_get_thread());

	i->status = status;

	pthread_mutex_lock(&g_devlist_mutex);
	if (status) {
		goto end;
	}
	thread = TAILQ_NEXT(i->cur_thread, tailq);
	while (thread) {
		TAILQ_FOREACH(ch, &thread->io_channels, tailq) {
			if (ch->dev->io_device == i->io_device) {
				i->cur_thread = thread;
				i->ch = ch;
				pthread_mutex_unlock(&g_devlist_mutex);
				spdk_thread_send_msg(thread, _call_channel, i);
				return;
			}
		}
		thread = TAILQ_NEXT(thread, tailq);
	}

end:
	i->dev->for_each_count--;
	i->ch = NULL;
	pthread_mutex_unlock(&g_devlist_mutex);

	spdk_thread_send_msg(i->orig_thread, _call_completion, i);
}
