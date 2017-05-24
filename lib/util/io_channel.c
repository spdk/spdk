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

static pthread_mutex_t g_devlist_mutex = PTHREAD_MUTEX_INITIALIZER;

struct io_device {
	void			*io_device_ctx;
	spdk_io_channel_create_cb create_cb;
	spdk_io_channel_destroy_cb destroy_cb;
	uint32_t		ctx_size;
	TAILQ_ENTRY(io_device)	tailq;
};

static TAILQ_HEAD(, io_device) g_io_devices = TAILQ_HEAD_INITIALIZER(g_io_devices);

struct spdk_io_channel {
	struct spdk_thread		*thread;
	void				*io_device;
	uint32_t			ref;
	TAILQ_ENTRY(spdk_io_channel)	tailq;
	spdk_io_channel_destroy_cb	destroy_cb;

	/*
	 * Modules will allocate extra memory off the end of this structure
	 *  to store references to hardware-specific references (i.e. NVMe queue
	 *  pairs, or references to child device spdk_io_channels (i.e.
	 *  virtual bdevs).
	 */
};

struct spdk_thread {
	pthread_t thread_id;
	spdk_thread_pass_msg fn;
	void *thread_ctx;
	TAILQ_HEAD(, spdk_io_channel) io_channels;
	TAILQ_ENTRY(spdk_thread) tailq;
};

static TAILQ_HEAD(, spdk_thread) g_threads = TAILQ_HEAD_INITIALIZER(g_threads);

static struct spdk_thread *
_get_thread(void)
{
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
}

struct spdk_thread *
spdk_allocate_thread(spdk_thread_pass_msg fn, void *thread_ctx)
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
	thread->fn = fn;
	thread->thread_ctx = thread_ctx;
	TAILQ_INIT(&thread->io_channels);
	TAILQ_INSERT_TAIL(&g_threads, thread, tailq);

	pthread_mutex_unlock(&g_devlist_mutex);

	return thread;
}

void
spdk_free_thread(void)
{
	struct spdk_thread *thread;

	pthread_mutex_lock(&g_devlist_mutex);

	thread = _get_thread();
	if (!thread) {
		SPDK_ERRLOG("No thread allocated\n");
		pthread_mutex_unlock(&g_devlist_mutex);
		return;
	}

	TAILQ_REMOVE(&g_threads, thread, tailq);
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

void
spdk_thread_send_msg(const struct spdk_thread *thread, spdk_thread_fn fn, void *ctx)
{
	thread->fn(fn, ctx, thread->thread_ctx);
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

	dev->io_device_ctx = io_device;
	dev->create_cb = create_cb;
	dev->destroy_cb = destroy_cb;
	dev->ctx_size = ctx_size;

	pthread_mutex_lock(&g_devlist_mutex);
	TAILQ_FOREACH(tmp, &g_io_devices, tailq) {
		if (tmp->io_device_ctx == io_device) {
			SPDK_ERRLOG("io_device %p already registered\n", io_device);
			free(dev);
			pthread_mutex_unlock(&g_devlist_mutex);
			return;
		}
	}
	TAILQ_INSERT_TAIL(&g_io_devices, dev, tailq);
	pthread_mutex_unlock(&g_devlist_mutex);
}

void
spdk_io_device_unregister(void *io_device)
{
	struct io_device *dev;

	pthread_mutex_lock(&g_devlist_mutex);
	TAILQ_FOREACH(dev, &g_io_devices, tailq) {
		if (dev->io_device_ctx == io_device) {
			TAILQ_REMOVE(&g_io_devices, dev, tailq);
			free(dev);
			pthread_mutex_unlock(&g_devlist_mutex);
			return;
		}
	}
	SPDK_ERRLOG("io_device %p not found\n", io_device);
	pthread_mutex_unlock(&g_devlist_mutex);
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
		if (dev->io_device_ctx == io_device) {
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

	pthread_mutex_unlock(&g_devlist_mutex);

	TAILQ_FOREACH(ch, &thread->io_channels, tailq) {
		if (ch->io_device == io_device) {
			ch->ref++;
			/*
			 * An I/O channel already exists for this device on this
			 *  thread, so return it.
			 */
			return ch;
		}
	}

	ch = calloc(1, sizeof(*ch) + dev->ctx_size);
	if (ch == NULL) {
		SPDK_ERRLOG("could not calloc spdk_io_channel\n");
		return NULL;
	}
	rc = dev->create_cb(io_device, (uint8_t *)ch + sizeof(*ch));
	if (rc == -1) {
		free(ch);
		return NULL;
	}

	ch->io_device = io_device;
	ch->destroy_cb = dev->destroy_cb;
	ch->thread = thread;
	ch->ref = 1;
	TAILQ_INSERT_TAIL(&thread->io_channels, ch, tailq);
	return ch;
}

void
spdk_put_io_channel(struct spdk_io_channel *ch)
{
	if (ch->ref == 0) {
		SPDK_ERRLOG("ref already zero\n");
		return;
	}

	ch->ref--;

	if (ch->ref == 0) {
		TAILQ_REMOVE(&ch->thread->io_channels, ch, tailq);
		ch->destroy_cb(ch->io_device, (uint8_t *)ch + sizeof(*ch));
		free(ch);
	}
}

void *
spdk_io_channel_get_ctx(struct spdk_io_channel *ch)
{
	return (uint8_t *)ch + sizeof(*ch);
}
