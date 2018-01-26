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

/** \file
 * IO channel
 */

#ifndef SPDK_IO_CHANNEL_H_
#define SPDK_IO_CHANNEL_H_

#include "spdk/stdinc.h"

#include "spdk/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_thread;
struct spdk_io_channel_iter;
struct spdk_poller;

typedef void (*spdk_thread_fn)(void *ctx);
typedef void (*spdk_thread_pass_msg)(spdk_thread_fn fn, void *ctx,
				     void *thread_ctx);

typedef void (*spdk_poller_fn)(void *ctx);
typedef struct spdk_poller *(*spdk_start_poller)(void *thread_ctx,
		spdk_poller_fn fn,
		void *arg,
		uint64_t period_microseconds);
typedef void (*spdk_stop_poller)(struct spdk_poller *poller, void *thread_ctx);

typedef int (*spdk_io_channel_create_cb)(void *io_device, void *ctx_buf);
typedef int (*spdk_io_channel_destroy_cb)(void *io_device, void *ctx_buf);

typedef void (*spdk_io_device_unregister_cb)(void *io_device);

typedef void (*spdk_channel_msg)(struct spdk_io_channel_iter *i);
typedef void (*spdk_channel_for_each_cpl)(struct spdk_io_channel_iter *i, int status);

/**
 * \brief Represents a per-thread channel for accessing an I/O device.
 *
 * An I/O device may be a physical entity (i.e. NVMe controller) or a software
 *  entity (i.e. a blobstore).
 *
 * This structure is not part of the API - all accesses should be done through
 *  spdk_io_channel function calls.
 */
struct spdk_io_channel {
	struct spdk_thread		*thread;
	struct io_device		*dev;
	uint32_t			ref;
	TAILQ_ENTRY(spdk_io_channel)	tailq;
	spdk_io_channel_destroy_cb	destroy_cb;
	bool				destroying;

	/*
	 * Modules will allocate extra memory off the end of this structure
	 *  to store references to hardware-specific references (i.e. NVMe queue
	 *  pairs, or references to child device spdk_io_channels (i.e.
	 *  virtual bdevs).
	 */
};

/**
 * \brief Initializes the calling thread for I/O channel allocation.
 *
 * @param fn A function that may be called from any thread and is
 *          passed a function pointer (spdk_thread_fn) that must be
 *          called on the same thread that spdk_allocate_thread
 *          was called from.
 * @param thread_ctx Context that will be passed to fn.
 * @param name Human-readable name for the thread; can be retrieved with spdk_thread_get_name().
 *             The string is copied, so the pointed-to data only needs to be valid during the
 *             spdk_allocate_thread() call.  May be NULL to specify no name.
 */
struct spdk_thread *spdk_allocate_thread(spdk_thread_pass_msg msg_fn,
		spdk_start_poller start_poller_fn,
		spdk_stop_poller stop_poller_fn,
		void *thread_ctx,
		const char *name);

/**
 * \brief Releases any resources related to the calling thread for I/O channel allocation.
 *
 * All I/O channel references related to the calling thread must be released using
 *  spdk_put_io_channel() prior to calling this function.
 */
void spdk_free_thread(void);

/**
 * \brief Get a handle to the current thread. This handle may be passed
 * to other threads and used as the target of spdk_thread_send_msg().
 *
 * \sa spdk_io_channel_get_thread()
 */
struct spdk_thread *spdk_get_thread(void);

/**
 * \brief Get a thread's name.
 */
const char *spdk_thread_get_name(const struct spdk_thread *thread);

/**
 * \brief Send a message to the given thread. The message
 * may be sent asynchronously - i.e. spdk_thread_send_msg
 * may return prior to `fn` being called.
 *
 * @param thread The target thread.
 * @param fn This function will be called on the given thread.
 * @param ctx This context will be passed to fn when called.
 */
void spdk_thread_send_msg(const struct spdk_thread *thread, spdk_thread_fn fn, void *ctx);

/**
 * \brief Send a message to each thread, serially. The message
 * is sent asynchronously - i.e. spdk_for_each_thread
 * will return prior to `fn` being called on each thread.
 *
 * @param fn This is the function that will be called on each thread.
 * @param ctx This context will be passed to fn when called.
 * @param cpl This will be called on the originating thread after `fn` has been
 *            called on each thread.
 */
void spdk_for_each_thread(spdk_thread_fn fn, void *ctx, spdk_thread_fn cpl);

/**
 * \brief Register a poller on the current thread. The poller can be
 * unregistered by calling spdk_poller_unregister().
 *
 * @param fn This function will be called every `period_microseconds`
 * @param arg Passed to fn
 * @param period_microseconds How often to call `fn`. If 0, call `fn` as often as possible.
 */
struct spdk_poller *spdk_poller_register(spdk_thread_fn fn,
		void *arg,
		uint64_t period_microseconds);

/**
 * \brief Unregister a poller on the current thread.
 *
 * @param ppoller The poller to unregister.
 */
void spdk_poller_unregister(struct spdk_poller **ppoller);

/**
 * \brief Register the opaque io_device context as an I/O device.
 *
 * After an I/O device is registered, it can return I/O channels using the
 *  spdk_get_io_channel() function.  create_cb is the callback function invoked
 *  to allocate any resources required for a new I/O channel.  destroy_cb is the
 *  callback function invoked to release the resources for an I/O channel.  ctx_size
 *  is the size of the context buffer allocated to store references to allocated I/O
 *  channel resources.
 */
void spdk_io_device_register(void *io_device, spdk_io_channel_create_cb create_cb,
			     spdk_io_channel_destroy_cb destroy_cb, uint32_t ctx_size);

/**
 * \brief Unregister the opaque io_device context as an I/O device.
 *
 * The actual unregistration might be deferred until all active I/O channels are destroyed.
 *  unregister_cb is an optional callback function invoked to release any references to
 *  this I/O device.
 */
void spdk_io_device_unregister(void *io_device, spdk_io_device_unregister_cb unregister_cb);

/**
 * \brief Gets an I/O channel for the specified io_device to be used by the calling thread.
 *
 * The io_device context pointer specified must have previously been registered using
 *  spdk_io_device_register().  If an existing I/O channel does not exist yet for the given
 *  io_device on the calling thread, it will allocate an I/O channel and invoke the create_cb
 *  function pointer specified in spdk_io_device_register().  If an I/O channel already
 *  exists for the given io_device on the calling thread, its reference is returned rather
 *  than creating a new I/O channel.
 */
struct spdk_io_channel *spdk_get_io_channel(void *io_device);

/**
 * \brief Releases a reference to an I/O channel. This happens asynchronously.
 *
 * Actual release will happen on the same thread that called spdk_get_io_channel() for the
 *  specified I/O channel.  If this releases the last reference to the I/O channel, The
 *  destroy_cb function specified in spdk_io_device_register() will be invoked to release
 *  any associated resources.
 */
void spdk_put_io_channel(struct spdk_io_channel *ch);

/**
 * \brief Returns the context buffer associated with an I/O channel.
 */
static inline void *
spdk_io_channel_get_ctx(struct spdk_io_channel *ch)
{
	return (uint8_t *)ch + sizeof(*ch);
}

/**
 *  \brief Returns an I/O channel from a context buffer. This is
 * the inverse of spdk_io_channel_get_ctx().
 */
struct spdk_io_channel *spdk_io_channel_from_ctx(void *ctx);

/**
 * \brief Returns the spdk_thread associated with an I/O channel.
 */
struct spdk_thread *spdk_io_channel_get_thread(struct spdk_io_channel *ch);

/**
 * \brief Call 'fn' on each channel associated with io_device. This happens
 * asynchronously, so fn may be called after spdk_for_each_channel returns.
 * 'fn' will be called on the correct thread for each channel. 'fn' will be
 * called for each channel serially, such that two calls to 'fn' will not
 * overlap in time. After 'fn' has been called, call
 * spdk_for_each_channel_continue() to continue iterating.
 *
 * Once 'fn' has been called on each channel, 'cpl' will be called
 * on the thread that spdk_for_each_channel was initially called from.
 */
void spdk_for_each_channel(void *io_device, spdk_channel_msg fn, void *ctx,
			   spdk_channel_for_each_cpl cpl);

void *spdk_io_channel_iter_get_io_device(struct spdk_io_channel_iter *i);

struct spdk_io_channel *spdk_io_channel_iter_get_channel(struct spdk_io_channel_iter *i);

void *spdk_io_channel_iter_get_ctx(struct spdk_io_channel_iter *i);

void spdk_for_each_channel_continue(struct spdk_io_channel_iter *i, int status);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_IO_CHANNEL_H_ */
