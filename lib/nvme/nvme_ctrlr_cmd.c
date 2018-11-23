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
			   struct spdk_nvme_qpair *qpair,
			   struct spdk_nvme_cmd *cmd,
			   void *buf, uint32_t len,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;

	req = nvme_allocate_request_contig(qpair, buf, len, cb_fn, cb_arg);

	if (req == NULL) {
		return -ENOMEM;
	}

	memcpy(&req->cmd, cmd, sizeof(req->cmd));

	return nvme_qpair_submit_request(qpair, req);
}

int
spdk_nvme_ctrlr_cmd_io_raw_with_md(struct spdk_nvme_ctrlr *ctrlr,
				   struct spdk_nvme_qpair *qpair,
				   struct spdk_nvme_cmd *cmd,
				   void *buf, uint32_t len, void *md_buf,
				   spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct nvme_payload payload;

	payload = NVME_PAYLOAD_CONTIG(buf, md_buf);

	req = nvme_allocate_request(qpair, &payload, len, cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	memcpy(&req->cmd, cmd, sizeof(req->cmd));

	return nvme_qpair_submit_request(qpair, req);
}

int
spdk_nvme_ctrlr_cmd_admin_raw(struct spdk_nvme_ctrlr *ctrlr,
			      struct spdk_nvme_cmd *cmd,
			      void *buf, uint32_t len,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request	*req;
	int			rc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_contig(ctrlr->adminq, buf, len, cb_fn, cb_arg);
	if (req == NULL) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -ENOMEM;
	}

	memcpy(&req->cmd, cmd, sizeof(req->cmd));

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	return rc;
}

int
nvme_ctrlr_cmd_identify(struct spdk_nvme_ctrlr *ctrlr, uint8_t cns, uint16_t cntid, uint32_t nsid,
			void *payload, size_t payload_size,
			spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_user_copy(ctrlr->adminq,
					      payload, payload_size,
					      cb_fn, cb_arg, false);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_IDENTIFY;
	cmd->cdw10 = cns | ((uint32_t)cntid << 16);
	cmd->nsid = nsid;

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

int
nvme_ctrlr_cmd_attach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			 struct spdk_nvme_ctrlr_list *payload, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request			*req;
	struct spdk_nvme_cmd			*cmd;
	int					rc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_user_copy(ctrlr->adminq,
					      payload, sizeof(struct spdk_nvme_ctrlr_list),
					      cb_fn, cb_arg, true);
	if (req == NULL) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_NS_ATTACHMENT;
	cmd->nsid = nsid;
	cmd->cdw10 = SPDK_NVME_NS_CTRLR_ATTACH;

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	return rc;
}

int
nvme_ctrlr_cmd_detach_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid,
			 struct spdk_nvme_ctrlr_list *payload, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request			*req;
	struct spdk_nvme_cmd			*cmd;
	int					rc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_user_copy(ctrlr->adminq,
					      payload, sizeof(struct spdk_nvme_ctrlr_list),
					      cb_fn, cb_arg, true);
	if (req == NULL) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_NS_ATTACHMENT;
	cmd->nsid = nsid;
	cmd->cdw10 = SPDK_NVME_NS_CTRLR_DETACH;

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	return rc;
}

int
nvme_ctrlr_cmd_create_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns_data *payload,
			 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request			*req;
	struct spdk_nvme_cmd			*cmd;
	int					rc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_user_copy(ctrlr->adminq,
					      payload, sizeof(struct spdk_nvme_ns_data),
					      cb_fn, cb_arg, true);
	if (req == NULL) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_NS_MANAGEMENT;
	cmd->cdw10 = SPDK_NVME_NS_MANAGEMENT_CREATE;

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	return rc;
}

int
nvme_ctrlr_cmd_delete_ns(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, spdk_nvme_cmd_cb cb_fn,
			 void *cb_arg)
{
	struct nvme_request			*req;
	struct spdk_nvme_cmd			*cmd;
	int					rc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
	if (req == NULL) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_NS_MANAGEMENT;
	cmd->cdw10 = SPDK_NVME_NS_MANAGEMENT_DELETE;
	cmd->nsid = nsid;

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	return rc;
}

int
nvme_ctrlr_cmd_doorbell_buffer_config(struct spdk_nvme_ctrlr *ctrlr, uint64_t prp1, uint64_t prp2,
				      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request			*req;
	struct spdk_nvme_cmd			*cmd;
	int					rc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
	if (req == NULL) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_DOORBELL_BUFFER_CONFIG;
	cmd->dptr.prp.prp1 = prp1;
	cmd->dptr.prp.prp2 = prp2;

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	return rc;
}

int
nvme_ctrlr_cmd_format(struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid, struct spdk_nvme_format *format,
		      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;
	int rc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
	if (req == NULL) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_FORMAT_NVM;
	cmd->nsid = nsid;
	memcpy(&cmd->cdw10, format, sizeof(uint32_t));

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return rc;
}

int
spdk_nvme_ctrlr_cmd_set_feature(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature,
				uint32_t cdw11, uint32_t cdw12, void *payload, uint32_t payload_size,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;
	int rc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_user_copy(ctrlr->adminq, payload, payload_size, cb_fn, cb_arg,
					      true);
	if (req == NULL) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_SET_FEATURES;
	cmd->cdw10 = feature;
	cmd->cdw11 = cdw11;
	cmd->cdw12 = cdw12;

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return rc;
}

int
spdk_nvme_ctrlr_cmd_get_feature(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature,
				uint32_t cdw11, void *payload, uint32_t payload_size,
				spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;
	int rc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_user_copy(ctrlr->adminq, payload, payload_size, cb_fn, cb_arg,
					      false);
	if (req == NULL) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd->cdw10 = feature;
	cmd->cdw11 = cdw11;

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return rc;
}

int
spdk_nvme_ctrlr_cmd_get_feature_ns(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature,
				   uint32_t cdw11, void *payload,
				   uint32_t payload_size, spdk_nvme_cmd_cb cb_fn,
				   void *cb_arg, uint32_t ns_id)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;
	int rc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_user_copy(ctrlr->adminq, payload, payload_size, cb_fn, cb_arg,
					      false);
	if (req == NULL) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd->cdw10 = feature;
	cmd->cdw11 = cdw11;
	cmd->nsid = ns_id;

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return rc;
}

int spdk_nvme_ctrlr_cmd_set_feature_ns(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature,
				       uint32_t cdw11, uint32_t cdw12, void *payload,
				       uint32_t payload_size, spdk_nvme_cmd_cb cb_fn,
				       void *cb_arg, uint32_t ns_id)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;
	int rc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_user_copy(ctrlr->adminq, payload, payload_size, cb_fn, cb_arg,
					      true);
	if (req == NULL) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_SET_FEATURES;
	cmd->cdw10 = feature;
	cmd->cdw11 = cdw11;
	cmd->cdw12 = cdw12;
	cmd->nsid = ns_id;

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return rc;
}

int
nvme_ctrlr_cmd_set_num_queues(struct spdk_nvme_ctrlr *ctrlr,
			      uint32_t num_queues, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	uint32_t cdw11;

	cdw11 = ((num_queues - 1) << 16) | (num_queues - 1);
	return spdk_nvme_ctrlr_cmd_set_feature(ctrlr, SPDK_NVME_FEAT_NUMBER_OF_QUEUES, cdw11, 0,
					       NULL, 0, cb_fn, cb_arg);
}

int
nvme_ctrlr_cmd_get_num_queues(struct spdk_nvme_ctrlr *ctrlr,
			      spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	return spdk_nvme_ctrlr_cmd_get_feature(ctrlr, SPDK_NVME_FEAT_NUMBER_OF_QUEUES, 0, NULL, 0,
					       cb_fn, cb_arg);
}

int
nvme_ctrlr_cmd_set_async_event_config(struct spdk_nvme_ctrlr *ctrlr,
				      union spdk_nvme_feat_async_event_configuration config, spdk_nvme_cmd_cb cb_fn,
				      void *cb_arg)
{
	uint32_t cdw11;

	cdw11 = config.raw;
	return spdk_nvme_ctrlr_cmd_set_feature(ctrlr, SPDK_NVME_FEAT_ASYNC_EVENT_CONFIGURATION, cdw11, 0,
					       NULL, 0,
					       cb_fn, cb_arg);
}

int
nvme_ctrlr_cmd_set_host_id(struct spdk_nvme_ctrlr *ctrlr, void *host_id, uint32_t host_id_size,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	uint32_t cdw11;

	if (host_id_size == 16) {
		/* 128-bit extended host identifier */
		cdw11 = 1;
	} else if (host_id_size == 8) {
		/* 64-bit host identifier */
		cdw11 = 0;
	} else {
		SPDK_ERRLOG("Invalid host ID size %u\n", host_id_size);
		return -EINVAL;
	}

	return spdk_nvme_ctrlr_cmd_set_feature(ctrlr, SPDK_NVME_FEAT_HOST_IDENTIFIER, cdw11, 0,
					       host_id, host_id_size, cb_fn, cb_arg);
}


/* follow spec to find out async event type which can be cleared or retained by cdw10.bit15 */
static bool is_support_rae(uint8_t log_page)
{
	uint8_t aet;

	switch (log_page) {
	/* ********************************************************
	 * from spec:
	 * 1.Error event
	 * 2.SMART / Health Status event
	 * 3.I/O Command Set events
	 *   NVM Command Set Events
	 * 4.Vendor Specific event
	 * above cases can use Get Log Page command with the Retain Asynchronous
	 * Event field cleared to ‘0’.
	 * ********************************************************** */
	case SPDK_NVME_LOG_ERROR:
		aet = SPDK_NVME_ASYNC_EVENT_TYPE_ERROR;
		break;
	case SPDK_NVME_LOG_HEALTH_INFORMATION:
		aet = SPDK_NVME_ASYNC_EVENT_TYPE_SMART;
		break;
	case SPDK_NVME_LOG_FIRMWARE_SLOT:
	case SPDK_NVME_LOG_CHANGED_NS_LIST:
		aet = SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE;
		break;
	case SPDK_NVME_LOG_COMMAND_EFFECTS_LOG:
		aet = SPDK_NVME_ASYNC_EVENT_TYPE_IO;
		break;
	default:
		aet = SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE;
		break;
	}

	switch (aet) {
	case SPDK_NVME_ASYNC_EVENT_TYPE_ERROR:
	case SPDK_NVME_ASYNC_EVENT_TYPE_SMART:
	case SPDK_NVME_ASYNC_EVENT_TYPE_IO:
	case SPDK_NVME_ASYNC_EVENT_TYPE_VENDOR:
		return true;
	case SPDK_NVME_ASYNC_EVENT_TYPE_NOTICE:
		break;
	default:
		break;
	}

	return false;
}

uint16_t spdk_nvme_cmd_get_log_page_retain_async_event(uint16_t log_page, bool retain)
{
	/* return logpage with setting the the RAE bit enabled to retain async event */
	return is_support_rae(log_page) & retain ? (1 << 15 | log_page) : (log_page & ~(1 << 15)) ;
}

int
spdk_nvme_ctrlr_cmd_get_log_page(struct spdk_nvme_ctrlr *ctrlr, uint16_t log_page,
				 uint32_t nsid, void *payload, uint32_t payload_size,
				 uint64_t offset, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;
	uint32_t numd, numdl, numdu;
	uint32_t lpol, lpou;
	int rc;

	if (payload_size == 0) {
		return -EINVAL;
	}

	if (offset & 3) {
		return -EINVAL;
	}

	numd = payload_size / sizeof(uint32_t) - 1u;
	numdl = numd & 0xFFFFu;
	numdu = (numd >> 16) & 0xFFFFu;

	lpol = (uint32_t)offset;
	lpou = (uint32_t)(offset >> 32);

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);

	if (offset && !ctrlr->cdata.lpa.edlp) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -EINVAL;
	}

	req = nvme_allocate_request_user_copy(ctrlr->adminq,
					      payload, payload_size, cb_fn, cb_arg, false);
	if (req == NULL) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	cmd->nsid = nsid;
	cmd->cdw10 = numdl << 16;
	cmd->cdw10 |= log_page;
	cmd->cdw11 = numdu;
	cmd->cdw12 = lpol;
	cmd->cdw13 = lpou;


	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return rc;
}

static void
spdk_nvme_ctrlr_cmd_abort_cpl(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_request	*req, *next, *tmp;
	struct spdk_nvme_ctrlr	*ctrlr;
	int			rc;

	req = ctx;
	ctrlr = (struct spdk_nvme_ctrlr *)req->user_buffer;

	ctrlr->outstanding_aborts--;
	STAILQ_FOREACH_SAFE(next, &ctrlr->queued_aborts, stailq, tmp) {
		STAILQ_REMOVE_HEAD(&ctrlr->queued_aborts, stailq);
		ctrlr->outstanding_aborts++;
		rc = nvme_ctrlr_submit_admin_request(ctrlr, next);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to submit queued abort.\n");
			memset(&next->cpl, 0, sizeof(next->cpl));
			next->cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			next->cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			next->cpl.status.dnr = 1;
			nvme_complete_request(next, &req->cpl);
			nvme_free_request(next);
		} else {
			/* If the first abort succeeds, stop iterating. */
			break;
		}
	}

	req->user_cb_fn(req->user_cb_arg, cpl);
}

int
spdk_nvme_ctrlr_cmd_abort(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
			  uint16_t cid, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	int rc;
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;
	uint16_t sqid;

	if (qpair) {
		sqid = qpair->id;
	} else {
		sqid = ctrlr->adminq->id; /* 0 */
	}

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_null(ctrlr->adminq, spdk_nvme_ctrlr_cmd_abort_cpl, NULL);
	if (req == NULL) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -ENOMEM;
	}
	req->cb_arg = req;
	req->user_cb_fn = cb_fn;
	req->user_cb_arg = cb_arg;
	req->user_buffer = ctrlr; /* This is a hack to get to the ctrlr in the
				   * completion handler. */

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_ABORT;
	cmd->cdw10 = (cid << 16) | sqid;

	if (ctrlr->outstanding_aborts >= ctrlr->cdata.acl) {
		STAILQ_INSERT_TAIL(&ctrlr->queued_aborts, req, stailq);
		rc = 0;
	} else {
		ctrlr->outstanding_aborts++;
		rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
	}

	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	return rc;
}

int
nvme_ctrlr_cmd_fw_commit(struct spdk_nvme_ctrlr *ctrlr,
			 const struct spdk_nvme_fw_commit *fw_commit,
			 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;
	int rc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_null(ctrlr->adminq, cb_fn, cb_arg);
	if (req == NULL) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_FIRMWARE_COMMIT;
	memcpy(&cmd->cdw10, fw_commit, sizeof(uint32_t));

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return rc;

}

int
nvme_ctrlr_cmd_fw_image_download(struct spdk_nvme_ctrlr *ctrlr,
				 uint32_t size, uint32_t offset, void *payload,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;
	int rc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_user_copy(ctrlr->adminq, payload, size, cb_fn, cb_arg, true);
	if (req == NULL) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD;
	cmd->cdw10 = (size >> 2) - 1;
	cmd->cdw11 = offset >> 2;

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return rc;
}

int
spdk_nvme_ctrlr_cmd_security_receive(struct spdk_nvme_ctrlr *ctrlr, uint8_t secp,
				     uint16_t spsp, uint8_t nssf, void *payload,
				     uint32_t payload_size, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;
	int rc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_user_copy(ctrlr->adminq, payload, payload_size,
					      cb_fn, cb_arg, false);
	if (req == NULL) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_SECURITY_RECEIVE;
	cmd->cdw10 = ((uint32_t)secp << 24) | ((uint32_t)spsp << 8) | ((uint32_t)nssf);
	cmd->cdw11 = payload_size;

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return rc;
}

int
spdk_nvme_ctrlr_cmd_security_send(struct spdk_nvme_ctrlr *ctrlr, uint8_t secp,
				  uint16_t spsp, uint8_t nssf, void *payload,
				  uint32_t payload_size, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;
	int rc;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	req = nvme_allocate_request_user_copy(ctrlr->adminq, payload, payload_size,
					      cb_fn, cb_arg, true);
	if (req == NULL) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_SECURITY_SEND;
	cmd->cdw10 = ((uint32_t)secp << 24) | ((uint32_t)spsp << 8) | ((uint32_t)nssf);
	cmd->cdw11 = payload_size;

	rc = nvme_ctrlr_submit_admin_request(ctrlr, req);
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	return rc;
}
