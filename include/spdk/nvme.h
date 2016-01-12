/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
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

#ifndef SPDK_NVME_H
#define SPDK_NVME_H

#include <stddef.h>
#include "nvme_spec.h"

/** \file
 *
 */

#define NVME_DEFAULT_RETRY_COUNT	(4)
extern int32_t		nvme_retry_count;

#ifdef __cplusplus
extern "C" {
#endif

/** \brief Opaque handle to a controller. Obtained by calling nvme_attach(). */
struct nvme_controller;

/**
 * \brief Attaches specified device to the NVMe driver.
 *
 * On success, the nvme_controller handle is valid for other nvme_ctrlr_* functions.
 * On failure, the return value will be NULL.
 *
 * This function should be called from a single thread while no other threads or drivers
 * are actively using the NVMe device.
 *
 * To stop using the the controller and release its associated resources,
 * call \ref nvme_detach with the nvme_controller instance returned by this function.
 */
struct nvme_controller *nvme_attach(void *devhandle);

/**
 * \brief Detaches specified device returned by \ref nvme_attach() from the NVMe driver.
 *
 * On success, the nvme_controller handle is no longer valid.
 *
 * This function should be called from a single thread while no other threads
 * are actively using the NVMe device.
 *
 */
int nvme_detach(struct nvme_controller *ctrlr);

/**
 * \brief Perform a full hardware reset of the NVMe controller.
 *
 * This function should be called from a single thread while no other threads
 * are actively using the NVMe device.
 *
 * Any pointers returned from nvme_ctrlr_get_ns() and nvme_ns_get_data() may be invalidated
 * by calling this function.  The number of namespaces as returned by nvme_ctrlr_get_num_ns() may
 * also change.
 */
int nvme_ctrlr_reset(struct nvme_controller *ctrlr);

/**
 * \brief Get the identify controller data as defined by the NVMe specification.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 */
const struct nvme_controller_data *nvme_ctrlr_get_data(struct nvme_controller *ctrlr);

/**
 * \brief Get the number of namespaces for the given NVMe controller.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 * This is equivalent to calling nvme_ctrlr_get_data() to get the
 * nvme_controller_data and then reading the nn field.
 *
 */
uint32_t nvme_ctrlr_get_num_ns(struct nvme_controller *ctrlr);

/**
 * Signature for callback function invoked when a command is completed.
 *
 * The nvme_completion parameter contains the completion status.
 */
typedef void (*nvme_cb_fn_t)(void *, const struct nvme_completion *);

/**
 * Signature for callback function invoked when an asynchronous error
 *  request command is completed.
 *
 * The aer_cb_arg parameter is set to the context specified by
 *  nvme_register_aer_callback().
 * The nvme_completion parameter contains the completion status of the
 *  asynchronous event request that was completed.
 */
typedef void (*nvme_aer_cb_fn_t)(void *aer_cb_arg,
				 const struct nvme_completion *);

void nvme_ctrlr_register_aer_callback(struct nvme_controller *ctrlr,
				      nvme_aer_cb_fn_t aer_cb_fn,
				      void *aer_cb_arg);

/**
 * \brief Send the given NVM I/O command to the NVMe controller.
 *
 * This is a low level interface for submitting I/O commands directly. Prefer
 * the nvme_ns_cmd_* functions instead. The validity of the command will
 * not be checked!
 *
 * When constructing the nvme_command it is not necessary to fill out the PRP
 * list/SGL or the CID. The driver will handle both of those for you.
 *
 * This function is thread safe and can be called at any point after
 * nvme_register_io_thread().
 *
 */
int nvme_ctrlr_cmd_io_raw(struct nvme_controller *ctrlr,
			  struct nvme_command *cmd,
			  void *buf, uint32_t len,
			  nvme_cb_fn_t cb_fn, void *cb_arg);

/**
 * \brief Process any outstanding completions for I/O submitted on the current thread.
 *
 * This will only process completions for I/O that were submitted on the same thread
 * that this function is called from. This call is also non-blocking, i.e. it only
 * processes completions that are ready at the time of this function call. It does not
 * wait for outstanding commands to finish.
 *
 * \param max_completions Limit the number of completions to be processed in one call, or 0
 * for unlimited.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 */
void nvme_ctrlr_process_io_completions(struct nvme_controller *ctrlr, uint32_t max_completions);

/**
 * \brief Send the given admin command to the NVMe controller.
 *
 * This is a low level interface for submitting admin commands directly. Prefer
 * the nvme_ctrlr_cmd_* functions instead. The validity of the command will
 * not be checked!
 *
 * When constructing the nvme_command it is not necessary to fill out the PRP
 * list/SGL or the CID. The driver will handle both of those for you.
 *
 * This function is thread safe and can be called at any point after
 * \ref nvme_attach().
 *
 * Call \ref nvme_ctrlr_process_admin_completions() to poll for completion
 * of commands submitted through this function.
 */
int nvme_ctrlr_cmd_admin_raw(struct nvme_controller *ctrlr,
			     struct nvme_command *cmd,
			     void *buf, uint32_t len,
			     nvme_cb_fn_t cb_fn, void *cb_arg);

/**
 * \brief Process any outstanding completions for admin commands.
 *
 * This will process completions for admin commands submitted on any thread.
 *
 * This call is non-blocking, i.e. it only processes completions that are ready
 * at the time of this function call. It does not wait for outstanding commands to
 * finish.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 */
void nvme_ctrlr_process_admin_completions(struct nvme_controller *ctrlr);


/** \brief Opaque handle to a namespace. Obtained by calling nvme_ctrlr_get_ns(). */
struct nvme_namespace;

/**
 * \brief Get a handle to a namespace for the given controller.
 *
 * Namespaces are numbered from 1 to the total number of namespaces. There will never
 * be any gaps in the numbering. The number of namespaces is obtained by calling
 * nvme_ctrlr_get_num_ns().
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 */
struct nvme_namespace *nvme_ctrlr_get_ns(struct nvme_controller *ctrlr, uint32_t ns_id);

/**
 * \brief Get the identify namespace data as defined by the NVMe specification.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 */
const struct nvme_namespace_data *nvme_ns_get_data(struct nvme_namespace *ns);

/**
 * \brief Get the namespace id (index number) from the given namespace handle.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 */
uint32_t nvme_ns_get_id(struct nvme_namespace *ns);

/**
 * \brief Get the maximum transfer size, in bytes, for an I/O sent to the given namespace.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 */
uint32_t nvme_ns_get_max_io_xfer_size(struct nvme_namespace *ns);

/**
 * \brief Get the sector size, in bytes, of the given namespace.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 */
uint32_t nvme_ns_get_sector_size(struct nvme_namespace *ns);

/**
 * \brief Get the number of sectors for the given namespace.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 */
uint64_t nvme_ns_get_num_sectors(struct nvme_namespace *ns);

/**
 * \brief Get the size, in bytes, of the given namespace.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 */
uint64_t nvme_ns_get_size(struct nvme_namespace *ns);

enum nvme_namespace_flags {
	NVME_NS_DEALLOCATE_SUPPORTED	= 0x1,
	NVME_NS_FLUSH_SUPPORTED		= 0x2,
};

/**
 * \brief Get the flags for the given namespace.
 *
 * See nvme_namespace_flags for the possible flags returned.
 *
 * This function is thread safe and can be called at any point after nvme_attach().
 *
 */
uint32_t nvme_ns_get_flags(struct nvme_namespace *ns);

/**
 * \brief Submits a write I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the write I/O
 * \param payload virtual address pointer to the data payload
 * \param lba starting LBA to write the data
 * \param lba_count length (in sectors) for the write operation
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * nvme_register_io_thread().
 */
int nvme_ns_cmd_write(struct nvme_namespace *ns, void *payload,
		      uint64_t lba, uint32_t lba_count, nvme_cb_fn_t cb_fn,
		      void *cb_arg);

/**
 * \brief Submits a zero write I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the zero write I/O
 * \param lba starting LBA to write zero
 * \param lba_count length (in sectors) for the write zero operation
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * nvme_register_io_thread().
 */
int nvme_ns_cmd_zero(struct nvme_namespace *ns, uint64_t lba,
		      uint32_t lba_count, nvme_cb_fn_t cb_fn, void *cb_arg);

/**
 * \brief Submits a read I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the read I/O
 * \param payload virtual address pointer to the data payload
 * \param lba starting LBA to read the data
 * \param lba_count length (in sectors) for the read operation
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * nvme_register_io_thread().
 */
int nvme_ns_cmd_read(struct nvme_namespace *ns, void *payload,
		     uint64_t lba, uint32_t lba_count, nvme_cb_fn_t cb_fn,
		     void *cb_arg);

/**
 * \brief Submits a deallocation request to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the deallocation request
 * \param payload virtual address pointer to the list of LBA ranges to
 *                deallocate
 * \param num_ranges number of ranges in the list pointed to by payload
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * nvme_register_io_thread().
 */
int nvme_ns_cmd_deallocate(struct nvme_namespace *ns, void *payload,
			   uint8_t num_ranges, nvme_cb_fn_t cb_fn,
			   void *cb_arg);

/**
 * \brief Submits a flush request to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the flush request
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * nvme_register_io_thread().
 */
int nvme_ns_cmd_flush(struct nvme_namespace *ns, nvme_cb_fn_t cb_fn,
		      void *cb_arg);

/**
 * \brief Get the size, in bytes, of an nvme_request.
 *
 * This is the size of the request objects that need to be allocated by the
 * nvme_alloc_request macro in nvme_impl.h
 *
 * This function is thread safe and can be called at any time.
 *
 */
size_t nvme_request_size(void);

int nvme_register_io_thread(void);
void nvme_unregister_io_thread(void);

#ifdef __cplusplus
}
#endif

#endif
