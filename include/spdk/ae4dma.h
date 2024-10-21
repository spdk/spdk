/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2025 Advanced Micro Devices, Inc.
 *   All rights reserved.
 */

/** file
 * AE4DMA engine driver public interface
 */

#ifndef SPDK_AE4DMA_H
#define SPDK_AE4DMA_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/env.h"

/**
 * Opaque handle for a single AE4DMA channel returned by \ref spdk_ae4dma_probe().
 */
struct spdk_ae4dma_chan;

/**
 * Signature for callback function invoked when a request is completed.
 *
 * \param arg User-specified opaque value corresponding to cb_arg from the
 * request submission.
 */
typedef void (*spdk_ae4dma_req_cb)(void *arg, int status);

/**
 * Callback for spdk_ae4dma_probe() enumeration.
 *
 * \param cb_ctx User-specified opaque value corresponding to cb_ctx from spdk_ae4dma_probe().
 * \param pci_dev PCI device that is being probed.
 *
 * \return true to attach to this device.
 */
typedef bool (*spdk_ae4dma_probe_cb)(void *cb_ctx, struct spdk_pci_device *pci_dev);

/**
 * Callback for spdk_ae4dma_probe() to report a device that has been attached to
 * the userspace AE4DMA driver.
 *
 * \param cb_ctx User-specified opaque value corresponding to cb_ctx from spdk_ae4dma_probe().
 * \param pci_dev PCI device that was attached to the driver.
 * \param ae4dma AE4DMA channel that was attached to the driver.
 */
typedef void (*spdk_ae4dma_attach_cb)(void *cb_ctx, struct spdk_pci_device *pci_dev,
				      struct spdk_ae4dma_chan *ae4dma);

/**
 * Enumerate the AE4DMA devices attached to the system and attach the userspace
 * AE4DMA driver to them if desired.
 *
 * If called more than once, only devices that are not already attached to the
 * SPDK AE4DMA driver will be reported.
 *
 * To stop using the controller and release its associated resources, call
 * spdk_ae4dma_detach() with the ae4dma_channel instance returned by this function.
 *
 * \param cb_ctx Opaque value which will be passed back in cb_ctx parameter of
 * the callbacks.
 * \param probe_cb will be called once per AE4DMA device found in the system.
 * \param attach_cb will be called for devices for which probe_cb returned true
 * once the AE4DMA controller has been attached to the userspace driver.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_ae4dma_probe(void *cb_ctx, spdk_ae4dma_probe_cb probe_cb, spdk_ae4dma_attach_cb attach_cb);

/**
 * Detach specified device returned by spdk_ae4dma_probe() from the AE4DMA driver.
 *
 * \param ae4dma AE4DMA  channel to detach from the driver.
  */
void spdk_ae4dma_detach(struct spdk_ae4dma_chan *ae4dma);

/**
 * Get the maximum number of descriptors supported by the library.
 *
 * \param chan AE4DMA channel
 *
 * \return maximum number of descriptors.
 */
uint32_t spdk_ae4dma_get_max_descriptors(struct spdk_ae4dma_chan *chan);

/**
 * Build a DMA engine memory copy request.
 *
 * This function will build the descriptor in the channel's ring.  The
 * caller must also explicitly call spdk_ae4dma_flush to submit the
 * descriptor, possibly after building additional descriptors.
 *
 * \param chan AE4DMA channel to build request.
 * \param hwq_id HW queue of the channel to be used for buiding descriptors.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param dst Destination virtual address.
 * \param src Source virtual address.
 * \param nbytes Number of bytes to copy.
 *
 * \return 0 on success, negative errno on failure.
 * the spdk_ae4dma_build_copy with iov feature
 */
int
spdk_ae4dma_build_copy(struct spdk_ae4dma_chan *ae4dma, int hwq_id, void *cb_arg,
		       spdk_ae4dma_req_cb cb_fn,
		       struct iovec *diov, uint32_t diovcnt,
		       struct iovec *siov, uint32_t siovcnt);


/**
 * Flush previously built descriptors.
 *
 * Descriptors are flushed by incrementing the write_index register of the
 * command queue.
 *
 * This function increments the write_index register of the paticular queue
 * and flush the descriptor to hardware for further processing.
 *
 * \param chan AE4DMA channel details.
 * hwq_id HW queue of the particular channel to be flushed from.
 */
void spdk_ae4dma_flush(struct spdk_ae4dma_chan *chan, int hwq_id);

/**
 * Check for completed requests on an AE4DMA channel.
 *
 * This function checks the read_index register to check for the completed
 * requests.
 *
 * \param chan AE4DMA channel to check for completions.
 * \param hwq_id HW queue on which the read_index needs to verified.
 *
 * \return number of events handled on success, negative errno on failure.
 */
int spdk_ae4dma_process_events(struct spdk_ae4dma_chan *chan, int hwq_id);

#ifdef __cplusplus
}
#endif

#endif
