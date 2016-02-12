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

#ifndef SPDK_IOAT_H
#define SPDK_IOAT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <inttypes.h>
#include <stdbool.h>
#include "spdk/pci.h"

#include "spdk/pci.h"

/**
 * Opaque handle for a single I/OAT channel returned by \ref spdk_ioat_probe().
 */
struct spdk_ioat_chan;

/**
 * Signature for callback function invoked when a request is completed.
 */
typedef void (*spdk_ioat_req_cb)(void *arg);

/**
 * Callback for spdk_ioat_probe() enumeration.
 *
 * \return true to attach to this device.
 */
typedef bool (*spdk_ioat_probe_cb)(void *cb_ctx, struct spdk_pci_device *pci_dev);

/**
 * Callback for spdk_ioat_probe() to report a device that has been attached to the userspace I/OAT driver.
 */
typedef void (*spdk_ioat_attach_cb)(void *cb_ctx, struct spdk_pci_device *pci_dev,
				    struct spdk_ioat_chan *ioat);

/**
 * \brief Enumerate the I/OAT devices attached to the system and attach the userspace I/OAT driver
 * to them if desired.
 *
 * \param probe_cb will be called once per I/OAT device found in the system.
 * \param attach_cb will be called for devices for which probe_cb returned true once the I/OAT
 * controller has been attached to the userspace driver.
 *
 * If called more than once, only devices that are not already attached to the SPDK I/OAT driver
 * will be reported.
 *
 * To stop using the the controller and release its associated resources,
 * call \ref spdk_ioat_detach with the ioat_channel instance returned by this function.
 */
int spdk_ioat_probe(void *cb_ctx, spdk_ioat_probe_cb probe_cb, spdk_ioat_attach_cb attach_cb);

/**
 * Detaches specified device returned by \ref spdk_ioat_probe() from the I/OAT driver.
 */
int spdk_ioat_detach(struct spdk_ioat_chan *ioat);

/**
 * Request a DMA engine channel for the calling thread.
 *
 * Must be called before submitting any requests from a thread.
 *
 * The \ref spdk_ioat_unregister_thread() function can be called to release the channel.
 */
int spdk_ioat_register_thread(void);

/**
 * Unregister the current thread's I/OAT channel.
 *
 * This function can be called after \ref spdk_ioat_register_thread() to release the thread's
 * DMA engine channel for use by other threads.
 */
void spdk_ioat_unregister_thread(void);

/**
 * Submit a DMA engine memory copy request.
 *
 * Before submitting any requests on a thread, the thread must be registered
 * using the \ref spdk_ioat_register_thread() function.
 */
int64_t spdk_ioat_submit_copy(void *cb_arg, spdk_ioat_req_cb cb_fn,
			      void *dst, const void *src, uint64_t nbytes);

/**
 * Submit a DMA engine memory fill request.
 *
 * Before submitting any requests on a thread, the thread must be registered
 * using the \ref spdk_ioat_register_thread() function.
 */
int64_t spdk_ioat_submit_fill(void *cb_arg, spdk_ioat_req_cb cb_fn,
			      void *dst, uint64_t fill_pattern, uint64_t nbytes);

/**
 * Check for completed requests on the current thread.
 *
 * Before submitting any requests on a thread, the thread must be registered
 * using the \ref spdk_ioat_register_thread() function.
 *
 * \returns 0 on success or negative if something went wrong.
 */
int spdk_ioat_process_events(void);

/**
 * DMA engine capability flags
 */
enum spdk_ioat_dma_capability_flags {
	SPDK_IOAT_ENGINE_COPY_SUPPORTED	= 0x1, /**< The memory copy is supported */
	SPDK_IOAT_ENGINE_FILL_SUPPORTED	= 0x2, /**< The memory fill is supported */
};

/**
 * Get the DMA engine capabilities.
 *
 * Before submitting any requests on a thread, the thread must be registered
 * using the \ref spdk_ioat_register_thread() function.
 */
uint32_t spdk_ioat_get_dma_capabilities(void);

#ifdef __cplusplus
}
#endif

#endif
