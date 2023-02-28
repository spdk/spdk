/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * IDXD driver public interface
 */

#ifndef SPDK_IDXD_H
#define SPDK_IDXD_H

#include "spdk/stdinc.h"
#include "spdk/idxd_spec.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/env.h"

/* The following flags control the behavior of I/O operations to IDXD. These flags
 * are often mapped to DSA specification values to ensure they have a unique value,
 * but do not necessarily correspond 1:1 with the hardware-defined flags.
 */

/* This flag indicates that IDXD should bypass the CPU cache for write operations,
 * landing the output directly into main memory. This is considered a hint, rather
 * than a guarantee.
 *
 * Note: While the value here is defined to be IDXD_FLAG_CACHE_CONTROL, this is only
 * to ensure that the flag has a unique value. The meaning here is the reverse of
 * IDXD_FLAG_CACHE_CONTROL - i.e. not specifying a flag writes data into CPU cache
 * because writing to cache is a more sensible default behavior.
 */
#define SPDK_IDXD_FLAG_NONTEMPORAL IDXD_FLAG_CACHE_CONTROL

/* The following flag is optional and specifies that the destination is persistent memory. The
 * low level library will not set this flag.
 */
#define SPDK_IDXD_FLAG_PERSISTENT IDXD_FLAG_DEST_STEERING_TAG

/**
 * Opaque handle for a single IDXD channel.
 */
struct spdk_idxd_io_channel;

/**
 * Opaque handle for a single IDXD device.
 */
struct spdk_idxd_device;

/**
 * Get the socket that this device is on
 *
 * \param idxd device to query
 * \return socket number.
 */
uint32_t spdk_idxd_get_socket(struct spdk_idxd_device *idxd);

/**
 * Signature for callback function invoked when a request is completed.
 *
 * \param arg User-specified opaque value corresponding to cb_arg from the
 * request submission.
 * \param status 0 on success, negative errno on failure.
 */
typedef void (*spdk_idxd_req_cb)(void *arg, int status);

/**
 * Callback for spdk_idxd_probe() to report a device that has been attached to
 * the userspace IDXD driver.
 *
 * \param cb_ctx User-specified opaque value corresponding to cb_ctx from spdk_idxd_probe().
 * \param idxd IDXD device that was attached to the driver.
 */
typedef void (*spdk_idxd_attach_cb)(void *cb_ctx, struct spdk_idxd_device *idxd);

/**
 * Callback for spdk_idxd_probe() to report a device that has been found.
 *
 * \param cb_ctx User-specified opaque value corresponding to cb_ctx from spdk_idxd_probe().
 * \param dev PCI device that is in question.
 * \return true if the caller wants the device, false if not..
 */
typedef bool (*spdk_idxd_probe_cb)(void *cb_ctx, struct spdk_pci_device *dev);

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
 * \param probe_cb callback to determine if the device being probe should be attached.
 * \param attach_cb will be called for devices for which probe_cb returned true
 * once the IDXD controller has been attached to the userspace driver.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_idxd_probe(void *cb_ctx, spdk_idxd_attach_cb attach_cb,
		    spdk_idxd_probe_cb probe_cb);

/**
 * Detach specified device returned by spdk_idxd_probe() from the IDXD driver.
 *
 * \param idxd IDXD device to detach from the driver.
  */
void spdk_idxd_detach(struct spdk_idxd_device *idxd);

/**
 * Sets the IDXD configuration.
 *
 * \param kernel_mode true if using kernel driver.
  */
void spdk_idxd_set_config(bool kernel_mode);

/**
 * Build and submit an idxd memory copy request.
 *
 * This function will build the copy descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param diov Destination iovec
 * \param diovcnt Number of elements in diov
 * \param siov Source iovec
 * \param siovcnt Number of elements in siov
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_idxd_submit_copy(struct spdk_idxd_io_channel *chan,
			  struct iovec *diov, uint32_t diovcnt,
			  struct iovec *siov, uint32_t siovcnt,
			  int flags, spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit an idxd dualcast request.
 *
 * This function will build the dual cast descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param dst1 First destination virtual address (must be 4K aligned).
 * \param dst2 Second destination virtual address (must be 4K aligned).
 * \param src Source virtual address.
 * \param nbytes Number of bytes to copy.
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_idxd_submit_dualcast(struct spdk_idxd_io_channel *chan,
			      void *dst1, void *dst2, const void *src, uint64_t nbytes, int flags,
			      spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit a memory compare request.
 *
 * This function will build the compare descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param siov1 First source iovec
 * \param siov1cnt Number of elements in siov1
 * \param siov2 Second source iovec
 * \param siov2cnt Number of elements in siov2
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_idxd_submit_compare(struct spdk_idxd_io_channel *chan,
			     struct iovec *siov1, size_t siov1cnt,
			     struct iovec *siov2, size_t siov2cnt,
			     int flags, spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit a idxd memory fill request.
 *
 * This function will build the fill descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param diov Destination iovec
 * \param diovcnt Number of elements in diov
 * \param fill_pattern Repeating eight-byte pattern to use for memory fill.
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the cb_arg parameter
 * in the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_idxd_submit_fill(struct spdk_idxd_io_channel *chan,
			  struct iovec *diov, size_t diovcnt,
			  uint64_t fill_pattern, int flags, spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit a memory CRC32-C request.
 *
 * This function will build the CRC-32C descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param siov Source iovec
 * \param siovcnt Number of elements in siov
 * \param seed Four byte CRC-32C seed value.
 * \param crc_dst Resulting calculation.
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the cb_arg parameter
 * in the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_idxd_submit_crc32c(struct spdk_idxd_io_channel *chan,
			    struct iovec *siov, size_t siovcnt,
			    uint32_t seed, uint32_t *crc_dst, int flags,
			    spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit a copy combined with CRC32-C request.
 *
 * This function will build the descriptor for copy plus CRC32-C and then immediately
 * submit by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param diov Destination iovec
 * \param diovcnt Number of elements in diov
 * \param siov Source iovec
 * \param siovcnt Number of elements in siov
 * \param seed Four byte CRC-32C seed value.
 * \param crc_dst Resulting calculation.
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the cb_arg parameter
 * in the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_idxd_submit_copy_crc32c(struct spdk_idxd_io_channel *chan,
				 struct iovec *diov, size_t diovcnt,
				 struct iovec *siov, size_t siovcnt,
				 uint32_t seed, uint32_t *crc_dst, int flags,
				 spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit an IAA memory compress request.
 *
 * This function will build the compress descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param dst Destination to write the compressed data to.
 * \param nbytes Length in bytes. The dst buffer should be large enough to hold the compressed data.
 * \param siov Source iovec
 * \param siovcnt Number of elements in siov
 * \param output_size The size of the compressed data
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_idxd_submit_compress(struct spdk_idxd_io_channel *chan,
			      void *dst, uint64_t nbytes,
			      struct iovec *siov, uint32_t siovcnt, uint32_t *output_size,
			      int flags, spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit an IAA memory decompress request.
 *
 * This function will build the decompress descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param diov Destination iovec. diov with diovcnt must be large enough to hold decompressed data.
 * \param diovcnt Number of elements in diov for decompress buffer.
 * \param siov Source iovec
 * \param siovcnt Number of elements in siov
 * \param flags Flags, optional flags that can vary per operation.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_idxd_submit_decompress(struct spdk_idxd_io_channel *chan,
				struct iovec *diov, uint32_t diovcnt,
				struct iovec *siov, uint32_t siovcnt,
				int flags, spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Build and submit an IDXD raw request.
 *
 * This function will process the supplied descriptor and then immediately submit
 * by writing to the proper device portal.
 *
 * \param chan IDXD channel to submit request.
 * \param desc proprely formatted IDXD descriptor.  Memory addresses should be physical.
 * The completion address will be filled in by the lower level library.
 * \param cb_fn Callback function which will be called when the request is complete.
 * \param cb_arg Opaque value which will be passed back as the arg parameter in
 * the completion callback.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_idxd_submit_raw_desc(struct spdk_idxd_io_channel *chan,
			      struct idxd_hw_desc *desc,
			      spdk_idxd_req_cb cb_fn, void *cb_arg);

/**
 * Check for completed requests on an IDXD channel.
 *
 * \param chan IDXD channel to check for completions.
 * \return number of operations completed.
 */
int spdk_idxd_process_events(struct spdk_idxd_io_channel *chan);

/**
 * Returns an IDXD channel for a given IDXD device.
 *
 * \param idxd IDXD device to get a channel for.
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
