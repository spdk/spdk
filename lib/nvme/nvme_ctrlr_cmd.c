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

#include "nvme_internal.h"

int
spdk_nvme_ctrlr_cmd_io_raw(struct spdk_nvme_ctrlr *ctrlr,
			   struct spdk_nvme_cmd *cmd,
			   void *buf, uint32_t len,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;

	req = nvme_allocate_request_contig(buf, len, cb_fn, cb_arg);

	if (req == NULL) {
		return ENOMEM;
	}

	memcpy(&req->cmd, cmd, sizeof(req->cmd));

	nvme_ctrlr_submit_io_request(ctrlr, req);
	return 0;
}

int
spdk_nvme_ctrlr_cmd_admin_raw(struct spdk_nvme_ctrlr *ctrlr,
			      struct spdk_nvme_cmd *cmd,
			      void *buf, uint32_t len,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;

	nvme_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_contig(buf, len, cb_fn, cb_arg);
	if (req == NULL) {
		nvme_mutex_unlock(&ctrlr->ctrlr_lock);
		return ENOMEM;
	}

	memcpy(&req->cmd, cmd, sizeof(req->cmd));

	nvme_ctrlr_submit_admin_request(ctrlr, req);

	nvme_mutex_unlock(&ctrlr->ctrlr_lock);
	return 0;
}

void
nvme_ctrlr_cmd_identify_controller(struct spdk_nvme_ctrlr *ctrlr, void *payload,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_contig(payload,
					   sizeof(struct spdk_nvme_ctrlr_data),
					   cb_fn, cb_arg);

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_IDENTIFY;

	/*
	 * TODO: create an identify command data structure, which
	 *  includes this CNS bit in cdw10.
	 */
	cmd->cdw10 = 1;

	nvme_ctrlr_submit_admin_request(ctrlr, req);
}

void
nvme_ctrlr_cmd_identify_namespace(struct spdk_nvme_ctrlr *ctrlr, uint16_t nsid,
				  void *payload, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_contig(payload,
					   sizeof(struct spdk_nvme_ns_data),
					   cb_fn, cb_arg);

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_IDENTIFY;

	/*
	 * TODO: create an identify command data structure
	 */
	cmd->nsid = nsid;

	nvme_ctrlr_submit_admin_request(ctrlr, req);
}

void
nvme_ctrlr_cmd_create_io_cq(struct spdk_nvme_ctrlr *ctrlr,
			    struct nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn,
			    void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(cb_fn, cb_arg);

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_CREATE_IO_CQ;

	/*
	 * TODO: create a create io completion queue command data
	 *  structure.
	 */
	cmd->cdw10 = ((io_que->num_entries - 1) << 16) | io_que->id;
	/*
	 * 0x2 = interrupts enabled
	 * 0x1 = physically contiguous
	 */
	cmd->cdw11 = (io_que->id << 16) | 0x1;
	cmd->dptr.prp.prp1 = io_que->cpl_bus_addr;

	nvme_ctrlr_submit_admin_request(ctrlr, req);
}

void
nvme_ctrlr_cmd_create_io_sq(struct spdk_nvme_ctrlr *ctrlr,
			    struct nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(cb_fn, cb_arg);

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_CREATE_IO_SQ;

	/*
	 * TODO: create a create io submission queue command data
	 *  structure.
	 */
	cmd->cdw10 = ((io_que->num_entries - 1) << 16) | io_que->id;
	/* 0x1 = physically contiguous */
	cmd->cdw11 = (io_que->id << 16) | 0x1;
	cmd->dptr.prp.prp1 = io_que->cmd_bus_addr;

	nvme_ctrlr_submit_admin_request(ctrlr, req);
}

int
spdk_nvme_ctrlr_cmd_set_feature(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature,
				uint32_t cdw11, uint32_t cdw12, void *payload, uint32_t payload_size,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	nvme_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_null(cb_fn, cb_arg);
	if (req == NULL) {
		nvme_mutex_unlock(&ctrlr->ctrlr_lock);
		return ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_SET_FEATURES;
	cmd->cdw10 = feature;
	cmd->cdw11 = cdw11;
	cmd->cdw12 = cdw12;

	nvme_ctrlr_submit_admin_request(ctrlr, req);
	nvme_mutex_unlock(&ctrlr->ctrlr_lock);

	return 0;
}

int
spdk_nvme_ctrlr_cmd_get_feature(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature,
				uint32_t cdw11, void *payload, uint32_t payload_size,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	nvme_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_null(cb_fn, cb_arg);
	if (req == NULL) {
		nvme_mutex_unlock(&ctrlr->ctrlr_lock);
		return ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd->cdw10 = feature;
	cmd->cdw11 = cdw11;

	nvme_ctrlr_submit_admin_request(ctrlr, req);
	nvme_mutex_unlock(&ctrlr->ctrlr_lock);

	return 0;
}

void
nvme_ctrlr_cmd_set_num_queues(struct spdk_nvme_ctrlr *ctrlr,
			      uint32_t num_queues, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	uint32_t cdw11;

	cdw11 = ((num_queues - 1) << 16) | (num_queues - 1);
	spdk_nvme_ctrlr_cmd_set_feature(ctrlr, SPDK_NVME_FEAT_NUMBER_OF_QUEUES, cdw11, 0,
					NULL, 0, cb_fn, cb_arg);
}

void
nvme_ctrlr_cmd_set_async_event_config(struct spdk_nvme_ctrlr *ctrlr,
				      union spdk_nvme_critical_warning_state state, spdk_nvme_cmd_cb cb_fn,
				      void *cb_arg)
{
	uint32_t cdw11;

	cdw11 = state.raw;
	spdk_nvme_ctrlr_cmd_set_feature(ctrlr, SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION, cdw11, 0, NULL, 0,
					cb_fn, cb_arg);
}

int
spdk_nvme_ctrlr_cmd_get_log_page(struct spdk_nvme_ctrlr *ctrlr, uint8_t log_page,
				 uint32_t nsid, void *payload, uint32_t payload_size, spdk_nvme_cmd_cb cb_fn,
				 void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	nvme_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_contig(payload, payload_size, cb_fn, cb_arg);
	if (req == NULL) {
		nvme_mutex_unlock(&ctrlr->ctrlr_lock);
		return ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	cmd->nsid = nsid;
	cmd->cdw10 = ((payload_size / sizeof(uint32_t)) - 1) << 16;
	cmd->cdw10 |= log_page;

	nvme_ctrlr_submit_admin_request(ctrlr, req);
	nvme_mutex_unlock(&ctrlr->ctrlr_lock);

	return 0;
}

void
nvme_ctrlr_cmd_abort(struct spdk_nvme_ctrlr *ctrlr, uint16_t cid,
		     uint16_t sqid, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(cb_fn, cb_arg);

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_ABORT;
	cmd->cdw10 = (cid << 16) | sqid;

	nvme_ctrlr_submit_admin_request(ctrlr, req);
}
