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
 * IDXD DMA engine driver public interface
 */

#ifndef SPDK_IDXD_H
#define SPDK_IDXD_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/env.h"

/**
 * Opaque handle for a single IDXD channel.
 */
struct spdk_idxd_io_channel;

/**
 * Opaque handle for a single IDXD device.
 */
struct spdk_idxd_device;

/**
 * Signature for configuring a channel
 *
 * \param chan IDXD channel to be configured.
 * \return 0 on success, negative errno on failure.
 */
int spdk_idxd_configure_chan(struct spdk_idxd_io_channel *chan);

/**
 * Reconfigures this channel based on how many current channels there are.
 *
 * \param chan IDXD channel to be set.
 * \param num_channels total number of channels in use.
 * \return 0 on success, negative errno on failure.
 */
int spdk_idxd_reconfigure_chan(struct spdk_idxd_io_channel *chan, uint32_t num_channels);

/**
 * Signature for callback function invoked when a request is completed.
 *
 * \param arg User-specified opaque value corresponding to cb_arg from the
 * request submission.
 * \param status 0 on success, negative errno on failure.
 */
typedef void (*spdk_idxd_req_cb)(void *arg, int status);

/**
 * Callback for spdk_idxd_probe() enumeration.
 *
 * \param cb_ctx User-specified opaque value corresponding to cb_ctx from spdk_idxd_probe().
 * \param pci_dev PCI device that is being probed.
 *
 * \return true to attach to this device.
 */
typedef bool (*spdk_idxd_probe_cb)(void *cb_ctx, struct spdk_pci_device *pci_dev);

/**
 * Callback for spdk_idxd_probe() to report a device that has been attached to
 * the userspace IDXD driver.
 *
 * \param cb_ctx User-specified opaque value corresponding to cb_ctx from spdk_idxd_probe().
 * \param pci_dev PCI device that was attached to the driver.
 * \param idxd IDXD device that was attached to the driver.
 */
typedef void (*spdk_idxd_attach_cb)(void *cb_ctx, struct spdk_pci_device *pci_dev,
				    struct spdk_idxd_device *idxd);

/**
 * Enumerate the IDXD devices attached to the system and attach the userspace
 * IDXD driver to them if desired.
 *
 * If called more than once, only devices that are not already attached to the
 * SPDK IDXD driver will be reported.
 *
 * To stop using the controller and release its associated resources, call
 * spdk_idxd_detach() with the idxd_channel instance returned by this function.
 *
 * \param cb_ctx Opaque value which will be passed back in cb_ctx parameter of
 * the callbacks.
 * \param probe_cb will be called once per IDXD device found in the system.
 * \param attach_cb will be called for devices for which probe_cb returned true
 * once the IDXD controller has been attached to the userspace driver.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_idxd_probe(void *cb_ctx, spdk_idxd_probe_cb probe_cb, spdk_idxd_attach_cb attach_cb);

/**
 * Detach specified device returned by spdk_idxd_probe() from the IDXD driver.
 *
 * \param idxd IDXD device to detach from the driver.
  */
void spdk_idxd_detach(struct spdk_idxd_device *idxd);

/**
 * Sets the IDXD configuration.
 *
 * \param config_number the configuration number for a valid IDXD config.
  */
void spdk_idxd_set_config(uint32_t config_number);

/**
 * Build and submit a DMA engine memory copy request.
 *
 * This function will build the copy descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param dst Destination virtual address.
 * \param src Source virtual address.
 * \param nbytes Number of bytes to copy.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_idxd_submit_copy(struct spdk_idxd_io_channel *chan,
			  void *dst, const void *src, uint64_t nbytes,
			  spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit a DMA engine memory fill request.
 *
 * This function will build the fill descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param dst Destination virtual address.
 * \param fill_pattern Repeating eight-byte pattern to use for memory fill.
 * \param nbytes Number of bytes to fill.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the cb_arg parameter
 * in the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_idxd_submit_fill(struct spdk_idxd_io_channel *chan,
			  void *dst, uint64_t fill_pattern, uint64_t nbytes,
			  spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Check for completed requests on an IDXD channel.
 *
 * \param chan IDXD channel to check for completions.
 */
void spdk_idxd_process_events(struct spdk_idxd_io_channel *chan);

/**
 * Returns an IDXD channel for a given IDXD device.
 *
 * \param idxd IDXD device to get a channel for.
 *
 * \return pointer to an IDXD channel.
 */
struct spdk_idxd_io_channel *spdk_idxd_get_channel(struct spdk_idxd_device *idxd);

/**
 * Free an IDXD channel.
 *
 * \param chan IDXD channel to free.
 */
void spdk_idxd_put_channel(struct spdk_idxd_io_channel *chan);

#ifdef __cplusplus
}
#endif

#endif
