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
 * Thread
 */

#ifndef SPDK_THREAD_H_
#define SPDK_THREAD_H_

#include "spdk/stdinc.h"

#include "spdk/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_thread;
struct spdk_io_channel_iter;
struct spdk_poller;

/**
 * Callback function for a thread.
 *
 * \param ctx Context passed as arg to spdk_thread_pass_msg().
 */
typedef void (*spdk_thread_fn)(void *ctx);

/**
 * Function to be called to pass a message to a thread.
 *
 * \param fn Callback function for a thread.
 * \param ctx Context passed to fn.
 * \param thread_ctx Context for the thread.
 */
typedef void (*spdk_thread_pass_msg)(spdk_thread_fn fn, void *ctx,
				     void *thread_ctx);

/**
 * Callback function for a poller.
 *
 * \param ctx Context passed as arg to spdk_poller_register().
 * \return 0 to indicate that polling took place but no events were found;
 * positive to indicate that polling took place and some events were processed;
 * negative if the poller does not provide spin-wait information.
 */
typedef int (*spdk_poller_fn)(void *ctx);

/**
 * Function to be called to start a poller for the thread.
 *
 * \param thread_ctx Context for the thread.
 * \param fn Callback function for a poller.
 * \param arg Argument passed to callback.
 * \param period Polling period in microseconds.
 *
 * \return a pointer to the poller on success, or NULL on failure.
 */
typedef struct spdk_poller *(*spdk_start_poller)(void *thread_ctx,
		spdk_poller_fn fn,
		void *arg,
		uint64_t period_microseconds);

/**
 * Function to be called to stop a poller.
 *
 * \param poller Poller to stop.
 * \param thread_ctx Context for the thread.
 */
typedef void (*spdk_stop_poller)(struct spdk_poller *poller, void *thread_ctx);

/**
 * I/O channel creation callback.
 *
 * \param io_device I/O device associated with this channel.
 * \param ctx_buf Context for the I/O device.
 */
typedef int (*spdk_io_channel_create_cb)(void *io_device, void *ctx_buf);

/**
 * I/O channel destruction callback.
 *
 * \param io_device I/O device associated with this channel.
 * \param ctx_buf Context for the I/O device.
 */
typedef void (*spdk_io_channel_destroy_cb)(void *io_device, void *ctx_buf);

/**
 * I/O device unregister callback.
 *
 * \param io_device Unregistered I/O device.
 */
typedef void (*spdk_io_device_unregister_cb)(void *io_device);

/**
 * Called on the appropriate thread for each channel associated with io_device.
 *
 * \param i I/O channel iterator.
 */
typedef void (*spdk_channel_msg)(struct spdk_io_channel_iter *i);

/**
 * spdk_for_each_channel() callback.
 *
 * \param i I/O channel iterator.
 * \param status 0 if it completed successfully, or negative errno if it failed.
 */
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
	uint32_t			destroy_ref;
	TAILQ_ENTRY(spdk_io_channel)	tailq;
	spdk_io_channel_destroy_cb	destroy_cb;

	/*
	 * Modules will allocate extra memory off the end of this structure
	 *  to store references to hardware-specific references (i.e. NVMe queue
	 *  pairs, or references to child device spdk_io_channels (i.e.
	 *  virtual bdevs).
	 */
};

/**
 * Initialize the threading library. Must be called once prior to allocating any threads.
 *
 * \return 0 on success. Negated errno on failure.
 */
int spdk_thread_lib_init(void);

/**
 * Release all resources associated with this library.
 */
void spdk_thread_lib_fini(void);

/**
 * Initializes the calling thread for I/O channel allocation.
 *
 * \param msg_fn A function that may be called from any thread and is passed a function
 * pointer (spdk_thread_fn) that must be called on the same thread that spdk_allocate_thread
 * was called from.
 * DEPRECATED. Only used in tests. Pass NULL for this parameter.
 * \param start_poller_fn Function to be called to start a poller for the thread.
 * DEPRECATED. Only used in tests. Pass NULL for this parameter.
 * \param stop_poller_fn Function to be called to stop a poller for the thread.
 * DEPRECATED. Only used in tests. Pass NULL for this parameter.
 * \param thread_ctx Context that will be passed to msg_fn, start_poller_fn, and stop_poller_fn.
 * DEPRECATED. Only used in tests. Pass NULL for this parameter.
 * \param name Human-readable name for the thread; can be retrieved with spdk_thread_get_name().
 * The string is copied, so the pointed-to data only needs to be valid during the
 * spdk_allocate_thread() call. May be NULL to specify no name.
 *
 * \return a pointer to the allocated thread on success or NULL on failure..
 */
struct spdk_thread *spdk_allocate_thread(spdk_thread_pass_msg msg_fn,
		spdk_start_poller start_poller_fn,
		spdk_stop_poller stop_poller_fn,
		void *thread_ctx,
		const char *name);

/**
 * Release any resources related to the calling thread for I/O channel allocation.
 *
 * All I/O channel references related to the calling thread must be released using
 * spdk_put_io_channel() prior to calling this function.
 */
void spdk_free_thread(void);

/**
 * Perform one iteration worth of processing on the thread. This includes
 * both expired and continuous pollers as well as messages.
 *
 * \param thread The thread to process
 * \param max_msgs The maximum number of messages that will be processed.
 *                 Use 0 to process all pending messages.
 *
 * \return 1 if work was done. 0 if no work was done. -1 if unknown.
 */
int spdk_thread_poll(struct spdk_thread *thread, uint32_t max_msgs);

/**
 * Return the number of ticks until the next timed poller
 * would expire. Timed pollers are pollers for which
 * period_microseconds is greater than 0.
 *
 * \param thread The thread to check poller expiration times on
 * \param now The current time, in ticks.
 *
 * \return Number of ticks. If no timed pollers, return 0.
 */
uint64_t spdk_thread_next_poller_expiration(struct spdk_thread *thread, uint64_t now);

/**
 * Get count of allocated threads.
 */
uint32_t spdk_thread_get_count(void);

/**
 * Get a handle to the current thread.
 *
 * This handle may be passed to other threads and used as the target of
 * spdk_thread_send_msg().
 *
 * \sa spdk_io_channel_get_thread()
 *
 * \return a pointer to the current thread on success or NULL on failure.
 */
struct spdk_thread *spdk_get_thread(void);

/**
 * Get a thread's name.
 *
 * \param thread Thread to query.
 *
 * \return the name of the thread.
 */
const char *spdk_thread_get_name(const struct spdk_thread *thread);

/**
 * Send a message to the given thread.
 *
 * The message may be sent asynchronously - i.e. spdk_thread_send_msg may return
 * prior to `fn` being called.
 *
 * \param thread The target thread.
 * \param fn This function will be called on the given thread.
 * \param ctx This context will be passed to fn when called.
 */
void spdk_thread_send_msg(const struct spdk_thread *thread, spdk_thread_fn fn, void *ctx);

/**
 * Send a message to each thread, serially.
 *
 * The message is sent asynchronously - i.e. spdk_for_each_thread will return
 * prior to `fn` being called on each thread.
 *
 * \param fn This is the function that will be called on each thread.
 * \param ctx This context will be passed to fn when called.
 * \param cpl This will be called on the originating thread after `fn` has been
 * called on each thread.
 */
void spdk_for_each_thread(spdk_thread_fn fn, void *ctx, spdk_thread_fn cpl);

/**
 * Register a poller on the current thread.
 *
 * The poller can be unregistered by calling spdk_poller_unregister().
 *
 * \param fn This function will be called every `period_microseconds`.
 * \param arg Argument passed to fn.
 * \param period_microseconds How often to call `fn`. If 0, call `fn` as often
 *  as possible.
 *
 * \return a pointer to the poller registered on the current thread on success
 * or NULL on failure.
 */
struct spdk_poller *spdk_poller_register(spdk_poller_fn fn,
		void *arg,
		uint64_t period_microseconds);

/**
 * Unregister a poller on the current thread.
 *
 * \param ppoller The poller to unregister.
 */
void spdk_poller_unregister(struct spdk_poller **ppoller);

/**
 * Register the opaque io_device context as an I/O device.
 *
 * After an I/O device is registered, it can return I/O channels using the
 * spdk_get_io_channel() function.
 *
 * \param io_device The pointer to io_device context.
 * \param create_cb Callback function invoked to allocate any resources required
 * for a new I/O channel.
 * \param destroy_cb Callback function invoked to release the resources for an
 * I/O channel.
 * \param ctx_size The size of the context buffer allocated to store references
 * to allocated I/O channel resources.
 * \param name A string name for the device used only for debugging. Optional -
 * may be NULL.
 */
void spdk_io_device_register(void *io_device, spdk_io_channel_create_cb create_cb,
			     spdk_io_channel_destroy_cb destroy_cb, uint32_t ctx_size,
			     const char *name);

/**
 * Unregister the opaque io_device context as an I/O device.
 *
 * The actual unregistration might be deferred until all active I/O channels are
 * destroyed.
 *
 * \param io_device The pointer to io_device context.
 * \param unregister_cb An optional callback function invoked to release any
 * references to this I/O device.
 */
void spdk_io_device_unregister(void *io_device, spdk_io_device_unregister_cb unregister_cb);

/**
 * Get an I/O channel for the specified io_device to be used by the calling thread.
 *
 * The io_device context pointer specified must have previously been registered
 * using spdk_io_device_register(). If an existing I/O channel does not exist
 * yet for the given io_device on the calling thread, it will allocate an I/O
 * channel and invoke the create_cb function pointer specified in spdk_io_device_register().
 * If an I/O channel already exists for the given io_device on the calling thread,
 * its reference is returned rather than creating a new I/O channel.
 *
 * \param io_device The pointer to io_device context.
 *
 * \return a pointer to the I/O channel for this device on success or NULL on failure.
 */
struct spdk_io_channel *spdk_get_io_channel(void *io_device);

/**
 * Release a reference to an I/O channel. This happens asynchronously.
 *
 * Actual release will happen on the same thread that called spdk_get_io_channel()
 * for the specified I/O channel. If this releases the last reference to the
 * I/O channel, The destroy_cb function specified in spdk_io_device_register()
 * will be invoked to release any associated resources.
 *
 * \param ch I/O channel to release a reference.
 */
void spdk_put_io_channel(struct spdk_io_channel *ch);

/**
 * Get the context buffer associated with an I/O channel.
 *
 * \param ch I/O channel.
 *
 * \return a pointer to the context buffer.
 */
static inline void *
spdk_io_channel_get_ctx(struct spdk_io_channel *ch)
{
	return (uint8_t *)ch + sizeof(*ch);
}

/**
 * Get I/O channel from the context buffer. This is the inverse of
 * spdk_io_channel_get_ctx().
 *
 * \param ctx The pointer to the context buffer.
 *
 * \return a pointer to the I/O channel associated with the context buffer.
 */
struct spdk_io_channel *spdk_io_channel_from_ctx(void *ctx);

/**
 * Get the thread associated with an I/O channel.
 *
 * \param ch I/O channel.
 *
 * \return a pointer to the thread associated with the I/O channel
 */
struct spdk_thread *spdk_io_channel_get_thread(struct spdk_io_channel *ch);

/**
 * Call 'fn' on each channel associated with io_device.
 *
 * This happens asynchronously, so fn may be called after spdk_for_each_channel
 * returns. 'fn' will be called for each channel serially, such that two calls
 * to 'fn' will not overlap in time. After 'fn' has been called, call
 * spdk_for_each_channel_continue() to continue iterating.
 *
 * \param io_device 'fn' will be called on each channel associated with this io_device.
 * \param fn Called on the appropriate thread for each channel associated with io_device.
 * \param ctx Context buffer registered to spdk_io_channel_iter that can be obatined
 * form the function spdk_io_channel_iter_get_ctx().
 * \param cpl Called on the thread that spdk_for_each_channel was initially called
 * from when 'fn' has been called on each channel.
 */
void spdk_for_each_channel(void *io_device, spdk_channel_msg fn, void *ctx,
			   spdk_channel_for_each_cpl cpl);

/**
 * Get io_device from the I/O channel iterator.
 *
 * \param i I/O channel iterator.
 *
 * \return a pointer to the io_device.
 */
void *spdk_io_channel_iter_get_io_device(struct spdk_io_channel_iter *i);

/**
 * Get I/O channel from the I/O channel iterator.
 *
 * \param i I/O channel iterator.
 *
 * \return a pointer to the I/O channel.
 */
struct spdk_io_channel *spdk_io_channel_iter_get_channel(struct spdk_io_channel_iter *i);

/**
 * Get context buffer from the I/O channel iterator.
 *
 * \param i I/O channel iterator.
 *
 * \return a pointer to the context buffer.
 */
void *spdk_io_channel_iter_get_ctx(struct spdk_io_channel_iter *i);

/**
 * Helper function to iterate all channels for spdk_for_each_channel().
 *
 * \param i I/O channel iterator.
 * \param status Status for the I/O channel iterator.
 */
void spdk_for_each_channel_continue(struct spdk_io_channel_iter *i, int status);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_THREAD_H_ */
