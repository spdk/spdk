/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_NVMF_CMD_H_
#define SPDK_NVMF_CMD_H_

#include "spdk/stdinc.h"
#include "spdk/nvmf.h"
#include "spdk/bdev.h"

enum spdk_nvmf_request_exec_status {
	SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE,
	SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS,
};

/**
 * Fills the identify controller attributes for the specified controller
 *
 * \param ctrlr The NVMe-oF controller
 * \param cdata The filled in identify controller attributes
 * \return \ref spdk_nvmf_request_exec_status
 */
int spdk_nvmf_ctrlr_identify_ctrlr(struct spdk_nvmf_ctrlr *ctrlr,
				   struct spdk_nvme_ctrlr_data *cdata);

/**
 * Fills the I/O Command Set specific Identify Namespace data structure (CNS
 * 05h)
 *
 * \param ctrlr The NVMe-oF controller
 * \param cmd The NVMe command
 * \param rsp The NVMe command completion
 * \param nsdata The filled in I/O command set specific identify namespace
 * attributes
 * \param nsdata_size The size of nsdata
 * \return \ref spdk_nvmf_request_exec_status
 */
int spdk_nvmf_ns_identify_iocs_specific(struct spdk_nvmf_ctrlr *ctrlr,
					struct spdk_nvme_cmd *cmd,
					struct spdk_nvme_cpl *rsp,
					void *nsdata,
					size_t nsdata_size);

/**
 * Fills the I/O Command Set specific Identify Controller data structure (CNS
 * 06h)
 *
 * \param ctrlr The NVMe-oF controller
 * \param cmd The NVMe command
 * \param rsp The NVMe command completion
 * \param cdata The filled in I/O command set specific identify controller
 * attributes
 * \param cdata_size The size of cdata
 * \return \ref spdk_nvmf_request_exec_status
 */
int spdk_nvmf_ctrlr_identify_iocs_specific(struct spdk_nvmf_ctrlr *ctrlr,
		struct spdk_nvme_cmd *cmd,
		struct spdk_nvme_cpl *rsp,
		void *cdata,
		size_t cdata_size);

/**
 * Fills the identify namespace attributes for the specified controller
 *
 * \param ctrlr The NVMe-oF controller
 * \param cmd The NVMe command
 * \param rsp The NVMe command completion
 * \param nsdata The filled in identify namespace attributes
 * \return \ref spdk_nvmf_request_exec_status
 */
int spdk_nvmf_ctrlr_identify_ns(struct spdk_nvmf_ctrlr *ctrlr,
				struct spdk_nvme_cmd *cmd,
				struct spdk_nvme_cpl *rsp,
				struct spdk_nvme_ns_data *nsdata);

/**
 * Callback function definition for a custom admin command handler.
 *
 * A function of this type is passed to \ref spdk_nvmf_set_custom_admin_cmd_hdlr.
 * It is called for every admin command that is processed by the NVMe-oF subsystem.
 * If the function handled the admin command then it must return a value from
 * \ref spdk_nvmf_request_exec_status. If the function did not handle the
 * admin command then it should return -1. In this case the SPDK default admin
 * command processing is applied to the request.
 *
 * \param req The NVMe-oF request of the admin command that is currently
 *            processed
 * \return \ref spdk_nvmf_request_exec_status if the command has been handled
 *         by the handler or -1 if the command wasn't handled
 */
typedef int (*spdk_nvmf_custom_cmd_hdlr)(struct spdk_nvmf_request *req);

/**
 * Installs a custom admin command handler.
 *
 * \param opc NVMe admin command OPC for which the handler should be installed.
 * \param hdlr The handler function. See \ref spdk_nvmf_custom_cmd_hdlr.
 */
void spdk_nvmf_set_custom_admin_cmd_hdlr(uint8_t opc, spdk_nvmf_custom_cmd_hdlr hdlr);

/**
 * Forward an NVMe admin command to a namespace
 *
 * This function forwards all NVMe admin commands of value opc to the specified
 * namespace id.
 * If forward_nsid is 0, the command is sent to the namespace that was specified in the
 * original command.
 *
 * \param opc - NVMe admin command OPC
 * \param forward_nsid - nsid or 0
 */
void spdk_nvmf_set_passthru_admin_cmd(uint8_t opc, uint32_t forward_nsid);

/**
 * Callback function that is called right before the admin command reply
 * is sent back to the initiator.
 *
 * \param req The NVMe-oF request
 */
typedef void (*spdk_nvmf_nvme_passthru_cmd_cb)(struct spdk_nvmf_request *req);

/**
 * Submits the NVMe-oF request to a bdev.
 *
 * This function can be used in a custom admin handler to send the command contained
 * in the req to a bdev. Once the bdev completes the command, the specified cb_fn
 * is called (which can be NULL if not needed).
 *
 * \param bdev The \ref spdk_bdev
 * \param desc The \ref spdk_bdev_desc
 * \param ch The \ref spdk_io_channel
 * \param req The \ref spdk_nvmf_request passed to the bdev for processing
 * \param cb_fn A callback function (or NULL) that is called before the request
 * is completed.
 *
 * \return A \ref spdk_nvmf_request_exec_status
 */
int spdk_nvmf_bdev_ctrlr_nvme_passthru_admin(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch, struct spdk_nvmf_request *req, spdk_nvmf_nvme_passthru_cmd_cb cb_fn);

/**
 * Attempts to abort a request in the specified bdev
 *
 * \param bdev Bdev that is processing req_to_abort
 * \param desc Bdev desc
 * \param ch Channel on which req_to_abort was originally submitted
 * \param req Abort cmd req
 * \param req_to_abort The request that should be aborted
 */
int spdk_nvmf_bdev_ctrlr_abort_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				   struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
				   struct spdk_nvmf_request *req_to_abort);

/**
 * Provide access to the underlying bdev that is associated with a namespace.
 *
 * This function can be used to communicate with the bdev. For example,
 * a \ref spdk_nvmf_custom_admin_cmd_hdlr can use \ref spdk_nvmf_bdev_nvme_passthru_admin
 * to pass on a \ref spdk_nvmf_request to a NVMe bdev.
 *
 * \param nsid The namespace id of a namespace that is valid for the
 * underlying subsystem
 * \param req The NVMe-oF request that is being processed
 * \param bdev Returns the \ref spdk_bdev corresponding to the namespace id
 * \param desc Returns the \ref spdk_bdev_desc corresponding to the namespace id
 * \param ch Returns the \ref spdk_io_channel corresponding to the namespace id
 *
 * \return 0 upon success
 * \return -EINVAL if the namespace id can't be found
 */
int spdk_nvmf_request_get_bdev(uint32_t nsid,
			       struct spdk_nvmf_request *req,
			       struct spdk_bdev **bdev,
			       struct spdk_bdev_desc **desc,
			       struct spdk_io_channel **ch);

/**
 * Get the NVMe-oF controller associated with this request.
 *
 * \param req The NVMe-oF request
 *
 * \return The NVMe-oF controller
 */
struct spdk_nvmf_ctrlr *spdk_nvmf_request_get_ctrlr(struct spdk_nvmf_request *req);

/**
 * Get the NVMe-oF subsystem associated with this request.
 *
 * \param req The NVMe-oF request
 *
 * \return The NVMe-oF subsystem
 */
struct spdk_nvmf_subsystem *spdk_nvmf_request_get_subsystem(struct spdk_nvmf_request *req);

/**
 * Get the data and length associated with this request.
 *
 *
 * \param req The NVMe-oF request
 * \param data The data buffer associated with this request
 * \param length The length of the data buffer
 */
void spdk_nvmf_request_get_data(struct spdk_nvmf_request *req, void **data, uint32_t *length);

/**
 * Copy the data from the given @buf into the request iovec.
 *
 * \param req The NVMe-oF request
 * \param buf The data buffer
 * \param buflen The length of the data buffer
 *
 * \return the number of bytes copied
 */
size_t spdk_nvmf_request_copy_from_buf(struct spdk_nvmf_request *req,
				       void *buf, size_t buflen);

/**
 * Copy the data from the request iovec into the given @buf.
 *
 * \param req The NVMe-oF request
 * \param buf The data buffer
 * \param buflen The length of the data buffer
 *
 * \return the number of bytes copied
 */
size_t spdk_nvmf_request_copy_to_buf(struct spdk_nvmf_request *req,
				     void *buf, size_t buflen);

/**
 * Get the NVMe-oF command associated with this request.
 *
 * \param req The NVMe-oF request
 *
 * \return The NVMe command
 */
struct spdk_nvme_cmd *spdk_nvmf_request_get_cmd(struct spdk_nvmf_request *req);

/**
 * Get the NVMe-oF completion associated with this request.
 *
 * \param req The NVMe-oF request
 *
 * \return The NVMe completion
 */
struct spdk_nvme_cpl *spdk_nvmf_request_get_response(struct spdk_nvmf_request *req);

/**
 * Get the request to abort that is associated with this request.
 * The req to abort is only set if the request processing a SPDK_NVME_OPC_ABORT cmd
 *
 * \param req The NVMe-oF abort request
 *
 * \return req_to_abort The NVMe-oF request that is in process of being aborted
 */
struct spdk_nvmf_request *spdk_nvmf_request_get_req_to_abort(struct spdk_nvmf_request *req);

#endif /* SPDK_NVMF_CMD_H_ */
