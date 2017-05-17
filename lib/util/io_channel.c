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

struct spdk_thread {
	pthread_t			thread_id;

	thread_pass_msg_t		fn;
	void				*thread_ctx;

	TAILQ_ENTRY(spdk_thread)	tailq;
};

struct spdk_io_device {
	void				*io_device;
	io_channel_create_cb_t		create_cb;
	io_channel_destroy_cb_t		destroy_cb;
	uint32_t			ctx_size;
	TAILQ_ENTRY(spdk_io_device)	tailq;
	TAILQ_HEAD(, spdk_io_channel)	io_channels;
};

struct spdk_io_channel {
	struct spdk_thread		*thread;
	struct spdk_io_device		*dev;
	uint32_t			ref;
	TAILQ_ENTRY(spdk_io_channel)	tailq;

	/*
	 * Modules will allocate extra memory off the end of this structure
	 *  to store references to hardware-specific references (i.e. NVMe queue
	 *  pairs, or references to child device spdk_io_channels (i.e.
	 *  virtual bdevs).
	 */
};

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static TAILQ_HEAD(, spdk_thread) g_threads = TAILQ_HEAD_INITIALIZER(g_threads);
static TAILQ_HEAD(, spdk_io_device) g_io_devices = TAILQ_HEAD_INITIALIZER(g_io_devices);

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

static struct spdk_io_device *
_get_dev(void *io_device)
{
	struct spdk_io_device *dev;

	TAILQ_FOREACH(dev, &g_io_devices, tailq) {
		if (dev->io_device == io_device) {
			return dev;
		}
	}

	return NULL;
}

void
spdk_allocate_thread(thread_pass_msg_t fn, void *thread_ctx)
{
	struct spdk_thread *thread;

	pthread_mutex_lock(&g_mutex);

	thread = _get_thread();
	if (thread) {
		SPDK_ERRLOG("Double allocated SPDK thread\n");
		pthread_mutex_unlock(&g_mutex);
		return;
	}

	thread = calloc(1, sizeof(*thread));
	if (!thread) {
		SPDK_ERRLOG("Unable to allocate memory for thread\n");
		pthread_mutex_unlock(&g_mutex);
		return;
	}

	thread->thread_id = pthread_self();
	thread->fn = fn;
	thread->thread_ctx = thread_ctx;
	TAILQ_INSERT_TAIL(&g_threads, thread, tailq);

	pthread_mutex_unlock(&g_mutex);
}

void
spdk_free_thread(void)
{
	struct spdk_thread *thread;

	pthread_mutex_lock(&g_mutex);

	thread = _get_thread();
	if (!thread) {
		SPDK_ERRLOG("No thread allocated\n");
		return;
	}

	TAILQ_REMOVE(&g_threads, thread, tailq);
	free(thread);

	pthread_mutex_unlock(&g_mutex);
}

const struct spdk_thread *
spdk_get_thread(void)
{
	struct spdk_thread *thread;

	pthread_mutex_lock(&g_mutex);

	thread = _get_thread();
	if (!thread) {
		SPDK_ERRLOG("No thread allocated\n");
		return NULL;
	}

	pthread_mutex_unlock(&g_mutex);

	return thread;
}

void
spdk_thread_send_msg(const struct spdk_thread *thread, thread_fn_t fn, void *ctx)
{
	thread->fn(fn, ctx, thread->thread_ctx);
}

void
spdk_io_device_register(void *io_device, io_channel_create_cb_t create_cb,
			io_channel_destroy_cb_t destroy_cb, uint32_t ctx_size)
{
	struct spdk_io_device *dev;

	pthread_mutex_lock(&g_mutex);

	dev = _get_dev(io_device);
	if (dev) {
		SPDK_ERRLOG("io_device %p already registered\n", io_device);
		pthread_mutex_unlock(&g_mutex);
		return;
	}

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		SPDK_ERRLOG("could not allocate io_device\n");
		pthread_mutex_unlock(&g_mutex);
		return;
	}

	dev->io_device = io_device;
	dev->create_cb = create_cb;
	dev->destroy_cb = destroy_cb;
	dev->ctx_size = ctx_size;
	TAILQ_INIT(&dev->io_channels);

	TAILQ_INSERT_TAIL(&g_io_devices, dev, tailq);

	pthread_mutex_unlock(&g_mutex);
}

void
spdk_io_device_unregister(void *io_device)
{
	struct spdk_io_device *dev;

	pthread_mutex_lock(&g_mutex);

	dev = _get_dev(io_device);
	if (!dev) {
		SPDK_ERRLOG("io_device %p not found\n", io_device);
		pthread_mutex_unlock(&g_mutex);
		return;
	}

	if (!TAILQ_EMPTY(&dev->io_channels)) {
		SPDK_ERRLOG("Attempted to unregister io_device %p but it still has channels\n", io_device);
		pthread_mutex_unlock(&g_mutex);
		return;
	}

	TAILQ_REMOVE(&g_io_devices, dev, tailq);
	free(dev);

	pthread_mutex_unlock(&g_mutex);
}

struct spdk_io_channel *
spdk_get_io_channel(void *io_device)
{
	struct spdk_io_channel *ch;
	struct spdk_io_device *dev;
	struct spdk_thread *thread;
	int rc;

	pthread_mutex_lock(&g_mutex);

	thread = _get_thread();
	if (!thread) {
		SPDK_ERRLOG("Attempted to get an I/O channel on an unallocated thread.\n");
		SPDK_ERRLOG("Call spdk_allocate_thread on this thread first.\n");
		pthread_mutex_unlock(&g_mutex);
		return NULL;
	}

	dev = _get_dev(io_device);
	if (!dev) {
		SPDK_ERRLOG("Attempted to get an I/O channel for an unregistered device.\n");
		SPDK_ERRLOG("Call spdk_io_device_register first.\n");
		pthread_mutex_unlock(&g_mutex);
		return NULL;
	}

	TAILQ_FOREACH(ch, &dev->io_channels, tailq) {
		if (ch->thread == thread) {
			ch->ref++;
			/*
			 * An I/O channel already exists for this device on this
			 *  thread, so return it.
			 */
			pthread_mutex_unlock(&g_mutex);
			return ch;
		}
	}

	ch = calloc(1, sizeof(*ch) + dev->ctx_size);
	if (ch == NULL) {
		SPDK_ERRLOG("could not calloc spdk_io_channel\n");
		return NULL;
	}

	ch->dev = dev;
	ch->thread = thread;
	ch->ref = 1;

	TAILQ_INSERT_TAIL(&dev->io_channels, ch, tailq);

	pthread_mutex_unlock(&g_mutex);

	rc = dev->create_cb(io_device, (uint8_t *)ch + sizeof(*ch));
	if (rc == -1) {
		pthread_mutex_lock(&g_mutex);
		TAILQ_REMOVE(&dev->io_channels, ch, tailq);
		free(ch);
		pthread_mutex_unlock(&g_mutex);
		return NULL;
	}

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
		pthread_mutex_lock(&g_mutex);
		TAILQ_REMOVE(&ch->dev->io_channels, ch, tailq);
		pthread_mutex_unlock(&g_mutex);
		ch->dev->destroy_cb(ch->dev->io_device, (uint8_t *)ch + sizeof(*ch));
		free(ch);
	}
}

void *
spdk_io_channel_get_ctx(struct spdk_io_channel *ch)
{
	return (uint8_t *)ch + sizeof(*ch);
}

void
spdk_io_device_send_all(void *io_device, thread_fn_t fn, void *ctx)
{
	struct spdk_io_channel *ch, *ch_tmp;
	struct spdk_io_device *dev;

	pthread_mutex_lock(&g_mutex);

	dev = _get_dev(io_device);
	if (!dev) {
		SPDK_ERRLOG("Invalid io_device %p\n", io_device);
		pthread_mutex_unlock(&g_mutex);
		return;
	}

	TAILQ_FOREACH_SAFE(ch, &dev->io_channels, tailq, ch_tmp) {
		pthread_mutex_unlock(&g_mutex);
		spdk_thread_send_msg(ch->thread, fn, ctx);
		pthread_mutex_lock(&g_mutex);
	}

	pthread_mutex_unlock(&g_mutex);
}
