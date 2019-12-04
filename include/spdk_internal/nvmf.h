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

#ifndef SPDK_NVMF_INTERNAL_H_
#define SPDK_NVMF_INTERNAL_H_

#include "spdk/stdinc.h"
#include "spdk/nvmf.h"

typedef enum _spdk_nvmf_request_exec_status {
	SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE,
	SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS,
} spdk_nvmf_request_exec_status;

/**
 * \brief Get identify data from controller
 *
 * \param ctrlr The controller for which the data should be retrieved
 *
 * \param cdata The structure to be filled with the identify data
 *
 * \return \ref spdk_nvmf_request_exec_status
 */
int spdk_nvmf_ctrlr_identify_ctrlr(struct spdk_nvmf_ctrlr *ctrlr,
				   struct spdk_nvme_ctrlr_data *cdata);

/**
 * \brief Get identify data from the namespace
 *
 * \param ctrlr The controller handling the namespace
 * \param cmd The NVMe command
 * \param rsp The NVMe completion queue (response)
 * \param nsdata The structure to be filled with the identify namespace data
 *
 * \param cdata The structure to be filled with the identify data
 *
 * \return \ref spdk_nvmf_request_exec_status
 */
int spdk_nvmf_ctrlr_identify_ns(struct spdk_nvmf_ctrlr *ctrlr,
				struct spdk_nvme_cmd *cmd,
				struct spdk_nvme_cpl *rsp,
				struct spdk_nvme_ns_data *nsdata);

/**
 * \brief Callback function definition for a custom command handler.
 *
 * A function of this type is passed to \ref spdk_nvmf_set_custom_admin_cmd_hdlr.
 * It is called for every admin command that is processed by the NVMe-oF subsystem.
 * If this function handled the admin command then it must return a value from
 * \ref spdk_nvmf_request_exec_status. If the function did not handle the
 * admin command it should return -1. In this case the SPDK default admin
 * command processing is applied to the request.
 *
 * \param req The NVMF request of the admin command that is currently
 *            processed
 * \return \ref spdk_nvmf_request_exec_status if the command has been handled
 *         by the handler or -1 if the command wasn't handled
 */
typedef int (*spdk_nvmf_custom_cmd_hdlr)(struct spdk_nvmf_request *req);

/**
 * \brief Installs a custom admin command handler.
 *
 * \param opc The admin command OPC for which the handler should be set.
 *
 * \param hdlr The handler function. See \ref spdk_nvmf_custom_admin_cmd_hdlr.
 */
void spdk_nvmf_set_custom_admin_cmd_hdlr(uint8_t opc, spdk_nvmf_custom_cmd_hdlr hdlr);

/**
 * \brief Passthru the admin command to a specific namespace.
 *
 * This function can be used to passthru admin command "opc" to a specific bdev.
 * If forward_nsid is set to 0, then the admin command is passed thru to the namespace
 * specified in the cmd.nsid value.
 * If forward_nsid is set to a value greater than 0 and smaller than 0xffffffffu,
 * then the admin command is passed thru to the namespace identified by forward_nsid
 *
 * \param opc The admin command OPC that should be passed thru.
 * \param forward_nsid 0 or a valid namespace id
 */
void spdk_nvmf_set_passthru_admin_cmd(uint8_t opc, uint32_t forward_nsid);

/**
 * \brief Callback function that is called right before the command reply
 * is sent back to the inititator.
 *
 * This function allows additional modifications of the command reply.
 *
 * \param The NVMF request
 */
typedef void (*spdk_nvmf_nvme_passthru_cmd_cb)(struct spdk_nvmf_request *req);

/**
 * \brief Submits the NVMF request to a bdev.
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
int spdk_nvmf_bdev_nvme_passthru_admin(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				       struct spdk_io_channel *ch, struct spdk_nvmf_request *req, spdk_nvmf_nvme_passthru_cmd_cb cb_fn);


/**
 * \brief Provide access to the underlying bdev that is associated with a namespace.
 *
 * This function can be used to communicate with the bdev. For example,
 * a \ref spdk_nvmf_custom_admin_cmd_hdlr can use \ref spdk_nvmf_bdev_nvme_passthru_admin
 * to pass on a \ref spdk_nvmf_request to a NVMe bdev.
 *
 * \param nsid The namespace id of a namespace that is valid for the
 * underlying subsystem
 * \param req The NVMF request that is being processed
 * \param bdev Returns the \ref spdk_bdev corresponding to the namespace id
 * \param desc Returns the \ref spdk_bdev_desc corresponding to the namespace id
 * \param ch Returns the \ref spdk_io_channel corresponding to the namespace id
 *
 * \return 0 upon success
 * \return -EINVAL if the namespace id can't be found
 */
int
spdk_nvmf_request_get_bdev(uint32_t nsid,
			   struct spdk_nvmf_request *req,
			   struct spdk_bdev **bdev,
			   struct spdk_bdev_desc **desc,
			   struct spdk_io_channel **ch);

/**
 * \brief Get the NVMe-oF controller associated with this request.
 *
 * \param req The NVMe-oF request
 *
 * \return The NVMe-oF controller
 */
struct spdk_nvmf_ctrlr *
spdk_nvmf_request_get_ctrlr(struct spdk_nvmf_request *req);

/**
 * \brief Get the NVMe-oF subsystem associated with this request.
 *
 * \param req The NVMe-oF request
 *
 * \return The NVMe-oF subsystem
 */
struct spdk_nvmf_subsystem *
spdk_nvmf_request_get_subsystem(struct spdk_nvmf_request *req);

/**
 * \brief Get the data and length associated with this request.
 *
 * \param req The NVMe-oF request
 * \param data The data buffer associated with this request
 * \param length The length of the data buffer
 */
void
spdk_nvmf_request_get_data(struct spdk_nvmf_request *req, void **data, uint32_t *length);

/**
 * \brief Get the NVMe-oF command associated with this request.
 *
 * \param req The NVMe-oF request
 *
 * \return The NVMe command
 */
struct spdk_nvme_cmd *
spdk_nvmf_request_get_cmd(struct spdk_nvmf_request *req);

/**
 * \brief Get the NVMe-oF completion associated with this request.
 *
 * \param req The NVMe-oF request
 *
 * \return The NVMe completion
 */
struct spdk_nvme_cpl *
spdk_nvmf_request_get_response(struct spdk_nvmf_request *req);

/**
 * \brief Example handler for the identify admin command.
 *
 * This example handler returns an identify response that
 * contains attributes from the underlying NVMe device.
 *
 * \param req The NVMe-oF request
 *
 * \return \ref spdk_nvmf_request_exec_status if handled
 *         -1 if not handled
 */
int
spdk_nvmf_custom_identify_hdlr(struct spdk_nvmf_request *req);

#endif /* SPDK_NVMF_INTERNAL_H_ */
