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

#ifndef SPDK_NVME_H
#define SPDK_NVME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include "spdk/pci.h"
#include "nvme_spec.h"

/** \file
 *
 */

#define SPDK_NVME_DEFAULT_RETRY_COUNT	(4)
extern int32_t		spdk_nvme_retry_count;



/** \brief Opaque handle to a controller. Returned by \ref spdk_nvme_probe()'s attach_cb. */
struct spdk_nvme_ctrlr;

/**
 * Callback for spdk_nvme_probe() enumeration.
 *
 * \return true to attach to this device.
 */
typedef bool (*spdk_nvme_probe_cb)(void *cb_ctx, struct spdk_pci_device *pci_dev);

/**
 * Callback for spdk_nvme_probe() to report a device that has been attached to the userspace NVMe driver.
 */
typedef void (*spdk_nvme_attach_cb)(void *cb_ctx, struct spdk_pci_device *pci_dev,
				    struct spdk_nvme_ctrlr *ctrlr);

/**
 * \brief Enumerate the NVMe devices attached to the system and attach the userspace NVMe driver
 * to them if desired.
 *
 * \param probe_cb will be called once per NVMe device found in the system.
 * \param attach_cb will be called for devices for which probe_cb returned true once that NVMe
 * controller has been attached to the userspace driver.
 *
 * If called more than once, only devices that are not already attached to the SPDK NVMe driver
 * will be reported.
 *
 * To stop using the the controller and release its associated resources,
 * call \ref nvme_detach with the spdk_nvme_ctrlr instance returned by this function.
 */
int spdk_nvme_probe(void *cb_ctx, spdk_nvme_probe_cb probe_cb, spdk_nvme_attach_cb attach_cb);

/**
 * \brief Detaches specified device returned by \ref nvme_probe()'s attach_cb from the NVMe driver.
 *
 * On success, the spdk_nvme_ctrlr handle is no longer valid.
 *
 * This function should be called from a single thread while no other threads
 * are actively using the NVMe device.
 *
 */
int spdk_nvme_detach(struct spdk_nvme_ctrlr *ctrlr);

/**
 * \brief Perform a full hardware reset of the NVMe controller.
 *
 * This function should be called from a single thread while no other threads
 * are actively using the NVMe device.
 *
 * Any pointers returned from spdk_nvme_ctrlr_get_ns() and spdk_nvme_ns_get_data() may be invalidated
 * by calling this function.  The number of namespaces as returned by spdk_nvme_ctrlr_get_num_ns() may
 * also change.
 */
int spdk_nvme_ctrlr_reset(struct spdk_nvme_ctrlr *ctrlr);

/**
 * \brief Get the identify controller data as defined by the NVMe specification.
 *
 * This function is thread safe and can be called at any point while the controller is attached to
 *  the SPDK NVMe driver.
 *
 */
const struct spdk_nvme_ctrlr_data *spdk_nvme_ctrlr_get_data(struct spdk_nvme_ctrlr *ctrlr);

/**
 * \brief Get the number of namespaces for the given NVMe controller.
 *
 * This function is thread safe and can be called at any point while the controller is attached to
 *  the SPDK NVMe driver.
 *
 * This is equivalent to calling spdk_nvme_ctrlr_get_data() to get the
 * spdk_nvme_ctrlr_data and then reading the nn field.
 *
 */
uint32_t spdk_nvme_ctrlr_get_num_ns(struct spdk_nvme_ctrlr *ctrlr);

/**
 * \brief Determine if a particular log page is supported by the given NVMe controller.
 *
 * This function is thread safe and can be called at any point while the controller is attached to
 *  the SPDK NVMe driver.
 *
 * \sa spdk_nvme_ctrlr_cmd_get_log_page()
 */
bool spdk_nvme_ctrlr_is_log_page_supported(struct spdk_nvme_ctrlr *ctrlr, uint8_t log_page);

/**
 * \brief Determine if a particular feature is supported by the given NVMe controller.
 *
 * This function is thread safe and can be called at any point while the controller is attached to
 *  the SPDK NVMe driver.
 *
 * \sa spdk_nvme_ctrlr_cmd_get_feature()
 */
bool spdk_nvme_ctrlr_is_feature_supported(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature_code);

/**
 * Signature for callback function invoked when a command is completed.
 *
 * The spdk_nvme_cpl parameter contains the completion status.
 */
typedef void (*spdk_nvme_cmd_cb)(void *, const struct spdk_nvme_cpl *);

/**
 * Signature for callback function invoked when an asynchronous error
 *  request command is completed.
 *
 * The aer_cb_arg parameter is set to the context specified by
 *  spdk_nvme_register_aer_callback().
 * The spdk_nvme_cpl parameter contains the completion status of the
 *  asynchronous event request that was completed.
 */
typedef void (*spdk_nvme_aer_cb)(void *aer_cb_arg,
				 const struct spdk_nvme_cpl *);

void spdk_nvme_ctrlr_register_aer_callback(struct spdk_nvme_ctrlr *ctrlr,
		spdk_nvme_aer_cb aer_cb_fn,
		void *aer_cb_arg);

/**
 * \brief Send the given NVM I/O command to the NVMe controller.
 *
 * This is a low level interface for submitting I/O commands directly. Prefer
 * the spdk_nvme_ns_cmd_* functions instead. The validity of the command will
 * not be checked!
 *
 * When constructing the nvme_command it is not necessary to fill out the PRP
 * list/SGL or the CID. The driver will handle both of those for you.
 *
 * This function is thread safe and can be called at any point after
 * spdk_nvme_register_io_thread().
 *
 */
int spdk_nvme_ctrlr_cmd_io_raw(struct spdk_nvme_ctrlr *ctrlr,
			       struct spdk_nvme_cmd *cmd,
			       void *buf, uint32_t len,
			       spdk_nvme_cmd_cb cb_fn, void *cb_arg);

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
 * \return Number of completions processed (may be 0) or negative on error.
 *
 * This function is thread safe and can be called at any point while the controller is attached to
 *  the SPDK NVMe driver.
 *
 */
int32_t spdk_nvme_ctrlr_process_io_completions(struct spdk_nvme_ctrlr *ctrlr,
		uint32_t max_completions);

/**
 * \brief Send the given admin command to the NVMe controller.
 *
 * This is a low level interface for submitting admin commands directly. Prefer
 * the spdk_nvme_ctrlr_cmd_* functions instead. The validity of the command will
 * not be checked!
 *
 * When constructing the nvme_command it is not necessary to fill out the PRP
 * list/SGL or the CID. The driver will handle both of those for you.
 *
 * This function is thread safe and can be called at any point while the controller is attached to
 *  the SPDK NVMe driver.
 *
 * Call \ref spdk_nvme_ctrlr_process_admin_completions() to poll for completion
 * of commands submitted through this function.
 */
int spdk_nvme_ctrlr_cmd_admin_raw(struct spdk_nvme_ctrlr *ctrlr,
				  struct spdk_nvme_cmd *cmd,
				  void *buf, uint32_t len,
				  spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * \brief Process any outstanding completions for admin commands.
 *
 * This will process completions for admin commands submitted on any thread.
 *
 * This call is non-blocking, i.e. it only processes completions that are ready
 * at the time of this function call. It does not wait for outstanding commands to
 * finish.
 *
 * \return Number of completions processed (may be 0) or negative on error.
 *
 * This function is thread safe and can be called at any point while the controller is attached to
 *  the SPDK NVMe driver.
 */
int32_t spdk_nvme_ctrlr_process_admin_completions(struct spdk_nvme_ctrlr *ctrlr);


/** \brief Opaque handle to a namespace. Obtained by calling spdk_nvme_ctrlr_get_ns(). */
struct spdk_nvme_ns;

/**
 * \brief Get a handle to a namespace for the given controller.
 *
 * Namespaces are numbered from 1 to the total number of namespaces. There will never
 * be any gaps in the numbering. The number of namespaces is obtained by calling
 * spdk_nvme_ctrlr_get_num_ns().
 *
 * This function is thread safe and can be called at any point while the controller is attached to
 *  the SPDK NVMe driver.
 */
struct spdk_nvme_ns *spdk_nvme_ctrlr_get_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t ns_id);

/**
 * \brief Get a specific log page from the NVMe controller.
 *
 * \param log_page The log page identifier.
 * \param nsid Depending on the log page, this may be 0, a namespace identifier, or SPDK_NVME_GLOBAL_NS_TAG.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param cb_fn Callback function to invoke when the log page has been retrieved.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, ENOMEM if resources could not be allocated for this request
 *
 * This function is thread safe and can be called at any point while the controller is attached to
 *  the SPDK NVMe driver.
 *
 * Call \ref spdk_nvme_ctrlr_process_admin_completions() to poll for completion
 * of commands submitted through this function.
 *
 * \sa spdk_nvme_ctrlr_is_log_page_supported()
 */
int spdk_nvme_ctrlr_cmd_get_log_page(struct spdk_nvme_ctrlr *ctrlr,
				     uint8_t log_page, uint32_t nsid,
				     void *payload, uint32_t payload_size,
				     spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * \brief Set specific feature for the given NVMe controller.
 *
 * \param feature The feature identifier.
 * \param cdw11 as defined by the specification for this command.
 * \param cdw12 as defined by the specification for this command.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param cb_fn Callback function to invoke when the feature has been set.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, ENOMEM if resources could not be allocated for this request
 *
 * This function is thread safe and can be called at any point while the controller is attached to
 *  the SPDK NVMe driver.
 *
 * Call \ref spdk_nvme_ctrlr_process_admin_completions() to poll for completion
 * of commands submitted through this function.
 *
 * \sa spdk_nvme_ctrlr_cmd_get_feature()
 */
int spdk_nvme_ctrlr_cmd_set_feature(struct spdk_nvme_ctrlr *ctrlr,
				    uint8_t feature, uint32_t cdw11, uint32_t cdw12,
				    void *payload, uint32_t payload_size,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * \brief Get specific feature from given NVMe controller.
 *
 * \param feature The feature identifier.
 * \param cdw11 as defined by the specification for this command.
 * \param payload The pointer to the payload buffer.
 * \param payload_size The size of payload buffer.
 * \param cb_fn Callback function to invoke when the feature has been retrieved.
 * \param cb_arg Argument to pass to the callback function.
 *
 * \return 0 if successfully submitted, ENOMEM if resources could not be allocated for this request
 *
 * This function is thread safe and can be called at any point while the controller is attached to
 *  the SPDK NVMe driver.
 *
 * Call \ref spdk_nvme_ctrlr_process_admin_completions() to poll for completion
 * of commands submitted through this function.
 *
 * \sa spdk_nvme_ctrlr_cmd_set_feature()
 */
int spdk_nvme_ctrlr_cmd_get_feature(struct spdk_nvme_ctrlr *ctrlr,
				    uint8_t feature, uint32_t cdw11,
				    void *payload, uint32_t payload_size,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * \brief Get the identify namespace data as defined by the NVMe specification.
 *
 * This function is thread safe and can be called at any point while the controller is attached to
 *  the SPDK NVMe driver.
 */
const struct spdk_nvme_ns_data *spdk_nvme_ns_get_data(struct spdk_nvme_ns *ns);

/**
 * \brief Get the namespace id (index number) from the given namespace handle.
 *
 * This function is thread safe and can be called at any point while the controller is attached to
 *  the SPDK NVMe driver.
 */
uint32_t spdk_nvme_ns_get_id(struct spdk_nvme_ns *ns);

/**
 * \brief Get the maximum transfer size, in bytes, for an I/O sent to the given namespace.
 *
 * This function is thread safe and can be called at any point while the controller is attached to
 *  the SPDK NVMe driver.
 */
uint32_t spdk_nvme_ns_get_max_io_xfer_size(struct spdk_nvme_ns *ns);

/**
 * \brief Get the sector size, in bytes, of the given namespace.
 *
 * This function is thread safe and can be called at any point while the controller is attached to
 *  the SPDK NVMe driver.
 */
uint32_t spdk_nvme_ns_get_sector_size(struct spdk_nvme_ns *ns);

/**
 * \brief Get the number of sectors for the given namespace.
 *
 * This function is thread safe and can be called at any point while the controller is attached to
 *  the SPDK NVMe driver.
 */
uint64_t spdk_nvme_ns_get_num_sectors(struct spdk_nvme_ns *ns);

/**
 * \brief Get the size, in bytes, of the given namespace.
 *
 * This function is thread safe and can be called at any point while the controller is attached to
 *  the SPDK NVMe driver.
 */
uint64_t spdk_nvme_ns_get_size(struct spdk_nvme_ns *ns);

/**
 * \brief Namespace command support flags.
 */
enum spdk_nvme_ns_flags {
	SPDK_NVME_NS_DEALLOCATE_SUPPORTED	= 0x1, /**< The deallocate command is supported */
	SPDK_NVME_NS_FLUSH_SUPPORTED		= 0x2, /**< The flush command is supported */
	SPDK_NVME_NS_RESERVATION_SUPPORTED	= 0x4, /**< The reservation command is supported */
	SPDK_NVME_NS_WRITE_ZEROES_SUPPORTED	= 0x8, /**< The write zeroes command is supported */
};

/**
 * \brief Get the flags for the given namespace.
 *
 * See spdk_nvme_ns_flags for the possible flags returned.
 *
 * This function is thread safe and can be called at any point while the controller is attached to
 *  the SPDK NVMe driver.
 */
uint32_t spdk_nvme_ns_get_flags(struct spdk_nvme_ns *ns);

/**
 * Restart the SGL walk to the specified offset when the command has scattered payloads.
 *
 * The cb_arg parameter is the value passed to readv/writev.
 */
typedef void (*spdk_nvme_req_reset_sgl_cb)(void *cb_arg, uint32_t offset);

/**
 * Fill out *address and *length with the current SGL entry and advance to the next
 * entry for the next time the callback is invoked.
 *
 * The cb_arg parameter is the value passed to readv/writev.
 * The address parameter contains the physical address of this segment.
 * The length parameter contains the length of this physical segment.
 */
typedef int (*spdk_nvme_req_next_sge_cb)(void *cb_arg, uint64_t *address, uint32_t *length);

/**
 * \brief Submits a write I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the write I/O
 * \param payload virtual address pointer to the data payload
 * \param lba starting LBA to write the data
 * \param lba_count length (in sectors) for the write operation
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_NVME_IO_FLAGS_* entries
 * 			in spdk/nvme_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * spdk_nvme_register_io_thread().
 */
int spdk_nvme_ns_cmd_write(struct spdk_nvme_ns *ns, void *payload,
			   uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
			   void *cb_arg, uint32_t io_flags);

/**
 * \brief Submits a write I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the write I/O
 * \param lba starting LBA to write the data
 * \param lba_count length (in sectors) for the write operation
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined in nvme_spec.h, for this I/O
 * \param reset_sgl_fn callback function to reset scattered payload
 * \param next_sge_fn callback function to iterate each scattered
 * payload memory segment
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * spdk_nvme_register_io_thread().
 */
int spdk_nvme_ns_cmd_writev(struct spdk_nvme_ns *ns, uint64_t lba, uint32_t lba_count,
			    spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
			    spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			    spdk_nvme_req_next_sge_cb next_sge_fn);

/**
 * \brief Submits a write zeroes I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the write zeroes I/O
 * \param lba starting LBA for this command
 * \param lba_count length (in sectors) for the write zero operation
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined by the SPDK_NVME_IO_FLAGS_* entries
 * 			in spdk/nvme_spec.h, for this I/O.
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * spdk_nvme_register_io_thread().
 */
int spdk_nvme_ns_cmd_write_zeroes(struct spdk_nvme_ns *ns, uint64_t lba,
				  uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
				  uint32_t io_flags);

/**
 * \brief Submits a read I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the read I/O
 * \param payload virtual address pointer to the data payload
 * \param lba starting LBA to read the data
 * \param lba_count length (in sectors) for the read operation
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined in nvme_spec.h, for this I/O
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * spdk_nvme_register_io_thread().
 */
int spdk_nvme_ns_cmd_read(struct spdk_nvme_ns *ns, void *payload,
			  uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
			  void *cb_arg, uint32_t io_flags);

/**
 * \brief Submits a read I/O to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the read I/O
 * \param lba starting LBA to read the data
 * \param lba_count length (in sectors) for the read operation
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 * \param io_flags set flags, defined in nvme_spec.h, for this I/O
 * \param reset_sgl_fn callback function to reset scattered payload
 * \param next_sge_fn callback function to iterate each scattered
 * payload memory segment
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * spdk_nvme_register_io_thread().
 */
int spdk_nvme_ns_cmd_readv(struct spdk_nvme_ns *ns, uint64_t lba, uint32_t lba_count,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
			   spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			   spdk_nvme_req_next_sge_cb next_sge_fn);

/**
 * \brief Submits a deallocation request to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the deallocation request
 * \param payload virtual address pointer to the list of LBA ranges to
 *                deallocate
 * \param num_ranges number of ranges in the list pointed to by payload; must be
 *                between 1 and \ref SPDK_NVME_DATASET_MANAGEMENT_MAX_RANGES, inclusive.
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * spdk_nvme_register_io_thread().
 */
int spdk_nvme_ns_cmd_deallocate(struct spdk_nvme_ns *ns, void *payload,
				uint16_t num_ranges, spdk_nvme_cmd_cb cb_fn,
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
 * spdk_nvme_register_io_thread().
 */
int spdk_nvme_ns_cmd_flush(struct spdk_nvme_ns *ns, spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * \brief Submits a reservation register to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the reservation register request
 * \param payload virtual address pointer to the reservation register data
 * \param ignore_key '1' the current reservation key check is disabled
 * \param action specifies the registration action
 * \param cptpl change the Persist Through Power Loss state
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * spdk_nvme_register_io_thread().
 */
int spdk_nvme_ns_cmd_reservation_register(struct spdk_nvme_ns *ns,
		struct spdk_nvme_reservation_register_data *payload,
		bool ignore_key,
		enum spdk_nvme_reservation_register_action action,
		enum spdk_nvme_reservation_register_cptpl cptpl,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * \brief Submits a reservation release to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the reservation release request
 * \param payload virtual address pointer to current reservation key
 * \param ignore_key '1' the current reservation key check is disabled
 * \param action specifies the reservation release action
 * \param type reservation type for the namespace
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * spdk_nvme_register_io_thread().
 */
int spdk_nvme_ns_cmd_reservation_release(struct spdk_nvme_ns *ns,
		struct spdk_nvme_reservation_key_data *payload,
		bool ignore_key,
		enum spdk_nvme_reservation_release_action action,
		enum spdk_nvme_reservation_type type,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * \brief Submits a reservation acquire to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the reservation acquire request
 * \param payload virtual address pointer to reservation acquire data
 * \param ignore_key '1' the current reservation key check is disabled
 * \param action specifies the reservation acquire action
 * \param type reservation type for the namespace
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * spdk_nvme_register_io_thread().
 */
int spdk_nvme_ns_cmd_reservation_acquire(struct spdk_nvme_ns *ns,
		struct spdk_nvme_reservation_acquire_data *payload,
		bool ignore_key,
		enum spdk_nvme_reservation_acquire_action action,
		enum spdk_nvme_reservation_type type,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * \brief Submits a reservation report to the specified NVMe namespace.
 *
 * \param ns NVMe namespace to submit the reservation report request
 * \param payload virtual address pointer for reservation status data
 * \param len length bytes for reservation status data structure
 * \param cb_fn callback function to invoke when the I/O is completed
 * \param cb_arg argument to pass to the callback function
 *
 * \return 0 if successfully submitted, ENOMEM if an nvme_request
 *	     structure cannot be allocated for the I/O request
 *
 * This function is thread safe and can be called at any point after
 * spdk_nvme_register_io_thread().
 */
int spdk_nvme_ns_cmd_reservation_report(struct spdk_nvme_ns *ns, void *payload,
					uint32_t len, spdk_nvme_cmd_cb cb_fn, void *cb_arg);

/**
 * \brief Get the size, in bytes, of an nvme_request.
 *
 * This is the size of the request objects that need to be allocated by the
 * nvme_alloc_request macro in nvme_impl.h
 *
 * This function is thread safe and can be called at any time.
 *
 */
size_t spdk_nvme_request_size(void);

int spdk_nvme_register_io_thread(void);
void spdk_nvme_unregister_io_thread(void);

#ifdef __cplusplus
}
#endif

#endif
