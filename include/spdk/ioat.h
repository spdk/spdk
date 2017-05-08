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
 * I/OAT DMA engine driver public interface
 */

#ifndef SPDK_IOAT_H
#define SPDK_IOAT_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/env.h"

/**
 * Opaque handle for a single I/OAT channel returned by \ref spdk_ioat_probe().
 */
struct spdk_ioat_chan;

/**
 * Signature for callback function invoked when a request is completed.
 *
 * \param arg User-specified opaque value corresponding to cb_arg from the request submission.
 */
typedef void (*spdk_ioat_req_cb)(void *arg);

/**
 * Callback for spdk_ioat_probe() enumeration.
 *
 * \param cb_ctx User-specified opaque value corresponding to cb_ctx from spdk_ioat_probe().
 * \param pci_dev PCI device that is being probed.
 *
 * \return true to attach to this device.
 */
typedef bool (*spdk_ioat_probe_cb)(void *cb_ctx, struct spdk_pci_device *pci_dev);

/**
 * Callback for spdk_ioat_probe() to report a device that has been attached to the userspace I/OAT driver.
 *
 * \param cb_ctx User-specified opaque value corresponding to cb_ctx from spdk_ioat_probe().
 * \param pci_dev PCI device that was attached to the driver.
 * \param ioat I/OAT channel that was attached to the driver.
 */
typedef void (*spdk_ioat_attach_cb)(void *cb_ctx, struct spdk_pci_device *pci_dev,
				    struct spdk_ioat_chan *ioat);

/**
 * \brief Enumerate the I/OAT devices attached to the system and attach the userspace I/OAT driver
 * to them if desired.
 *
 * \param cb_ctx Opaque value which will be passed back in cb_ctx parameter of the callbacks.
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
 *
 * \param ioat I/OAT channel to detach from the driver.
 */
int spdk_ioat_detach(struct spdk_ioat_chan *ioat);

/**
 * Submit a DMA engine memory copy request.
 *
 * \param chan I/OAT channel to submit request.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in the completion callback.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param dst Destination virtual address.
 * \param src Source virtual address.
 * \param nbytes Number of bytes to copy.
 */
int64_t spdk_ioat_submit_copy(struct spdk_ioat_chan *chan,
			      void *cb_arg, spdk_ioat_req_cb cb_fn,
			      void *dst, const void *src, uint64_t nbytes);

/**
 * Submit a DMA engine memory fill request.
 *
 * \param chan I/OAT channel to submit request.
 * \param cb_arg Opaque value which will be passed back as the cb_arg parameter in the completion callback.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param dst Destination virtual address.
 * \param fill_pattern Repeating eight-byte pattern to use for memory fill.
 * \param nbytes Number of bytes to fill.
 */
int64_t spdk_ioat_submit_fill(struct spdk_ioat_chan *chan,
			      void *cb_arg, spdk_ioat_req_cb cb_fn,
			      void *dst, uint64_t fill_pattern, uint64_t nbytes);

/**
 * Check for completed requests on an I/OAT channel.
 *
 * \param chan I/OAT channel to check for completions.
 *
 * \returns 0 on success or negative if something went wrong.
 */
int spdk_ioat_process_events(struct spdk_ioat_chan *chan);

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
 * \param chan I/OAT channel to query.
 *
 * \return A combination of flags from \ref spdk_ioat_dma_capability_flags.
 */
uint32_t spdk_ioat_get_dma_capabilities(struct spdk_ioat_chan *chan);

#ifdef __cplusplus
}
#endif

#endif
