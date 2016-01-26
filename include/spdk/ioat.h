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
 * This file defines the public interface to the I/OAT DMA engine driver.
 */

#ifndef __IOAT_H__
#define __IOAT_H__

#include <inttypes.h>
#include <stdbool.h>

/**
 * Signature for callback function invoked when a request is completed.
 */
typedef void (*ioat_callback_t)(void *arg);

/**
 * Returns true if vendor_id and device_id match a known IOAT PCI device ID.
 */
bool ioat_pci_device_match_id(uint16_t vendor_id, uint16_t device_id);

/**
 * Attach an I/OAT PCI device to the I/OAT userspace driver.
 *
 * To stop using the the device and release its associated resources,
 * call \ref ioat_detach with the ioat_channel instance returned by this function.
 */
struct ioat_channel *ioat_attach(void *device);

/**
 * Detaches specified device returned by \ref ioat_attach() from the I/OAT driver.
 */
int ioat_detach(struct ioat_channel *ioat);

/**
 * Request a DMA engine channel for the calling thread.
 *
 * Must be called before submitting any requests from a thread.
 *
 * The \ref ioat_unregister_thread() function can be called to release the channel.
 */
int ioat_register_thread(void);

/**
 * Unregister the current thread's I/OAT channel.
 *
 * This function can be called after \ref ioat_register_thread() to release the thread's
 * DMA engine channel for use by other threads.
 */
void ioat_unregister_thread(void);

/**
 * Submit a DMA engine memory copy request.
 *
 * Before submitting any requests on a thread, the thread must be registered
 * using the \ref ioat_register_thread() function.
 */
int64_t ioat_submit_copy(void *cb_arg, ioat_callback_t cb_fn,
			 void *dst, const void *src, uint64_t nbytes);

/**
 * Check for completed requests on the current thread.
 *
 * Before submitting any requests on a thread, the thread must be registered
 * using the \ref ioat_register_thread() function.
 *
 * \returns 0 on success or negative if something went wrong.
 */
int ioat_process_events(void);

#endif
