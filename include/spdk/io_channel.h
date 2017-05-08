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

#define SPDK_IO_PRIORITY_DEFAULT	100

struct spdk_io_channel;

typedef int (*io_channel_create_cb_t)(void *io_device, uint32_t priority, void *ctx_buf,
				      void *unique_ctx);
typedef void (*io_channel_destroy_cb_t)(void *io_device, void *ctx_buf);

/**
 * \brief Initializes the calling thread for I/O channel allocation.
 */
void spdk_allocate_thread(void);

/**
 * \brief Releases any resources related to the calling thread for I/O channel allocation.
 *
 * All I/O channel references related to the calling thread must be released using
 *  spdk_put_io_channel() prior to calling this function.
 */
void spdk_free_thread(void);

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
void spdk_io_device_register(void *io_device, io_channel_create_cb_t create_cb,
			     io_channel_destroy_cb_t destroy_cb, uint32_t ctx_size);

/**
 * \brief Unregister the opaque io_device context as an I/O device.
 *
 * Callers must ensure they release references to any I/O channel related to this
 *  device before calling this function.
 */
void spdk_io_device_unregister(void *io_device);

/**
 * \brief Gets an I/O channel for the specified io_device to be used by the calling thread.
 *
 * The io_device context pointer specified must have previously been registered using
 *  spdk_io_device_register().  If an existing I/O channel does not exist yet for the given
 *  io_device on the calling thread, it will allocate an I/O channel and invoke the create_cb
 *  function pointer specified in spdk_io_device_register().  If an I/O channel already
 *  exists for the given io_device on the calling thread, its reference is returned rather
 *  than creating a new I/O channel.
 *
 * The priority parameter allows callers to create different I/O channels to the same
 *  I/O device with varying priorities.  Currently this value must be set to
 *  SPDK_IO_PRIORITY_DEFAULT.
 *
 * The unique parameter allows callers to specify that an existing channel should not
 *  be used to satisfy this request, even if the io_device and priority fields match.
 *
 * The unique_ctx parameter allows callers to pass channel-specific context to the create_cb
 *  handler for unique channels.  This value must be NULL for shared channels.
 */
struct spdk_io_channel *spdk_get_io_channel(void *io_device, uint32_t priority, bool unique,
		void *unique_ctx);

/**
 * \brief Releases a reference to an I/O channel.
 *
 * Must be called from the same thread that called spdk_get_io_channel() for the specified
 *  I/O channel.  If this releases the last reference to the I/O channel, The destroy_cb
 *  function specified in spdk_io_device_register() will be invoked to release any
 *  associated resources.
 */
void spdk_put_io_channel(struct spdk_io_channel *ch);

/**
 * \brief Returns the context buffer associated with an I/O channel.
 */
void *spdk_io_channel_get_ctx(struct spdk_io_channel *ch);

#endif /* SPDK_IO_CHANNEL_H_ */
