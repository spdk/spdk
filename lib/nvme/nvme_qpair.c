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

static void nvme_qpair_fail(struct spdk_nvme_qpair *qpair);

struct nvme_string {
	uint16_t	value;
	const char	*str;
};

static const struct nvme_string admin_opcode[] = {
	{ SPDK_NVME_OPC_DELETE_IO_SQ, "DELETE IO SQ" },
	{ SPDK_NVME_OPC_CREATE_IO_SQ, "CREATE IO SQ" },
	{ SPDK_NVME_OPC_GET_LOG_PAGE, "GET LOG PAGE" },
	{ SPDK_NVME_OPC_DELETE_IO_CQ, "DELETE IO CQ" },
	{ SPDK_NVME_OPC_CREATE_IO_CQ, "CREATE IO CQ" },
	{ SPDK_NVME_OPC_IDENTIFY, "IDENTIFY" },
	{ SPDK_NVME_OPC_ABORT, "ABORT" },
	{ SPDK_NVME_OPC_SET_FEATURES, "SET FEATURES" },
	{ SPDK_NVME_OPC_GET_FEATURES, "GET FEATURES" },
	{ SPDK_NVME_OPC_ASYNC_EVENT_REQUEST, "ASYNC EVENT REQUEST" },
	{ SPDK_NVME_OPC_NS_MANAGEMENT, "NAMESPACE MANAGEMENT" },
	{ SPDK_NVME_OPC_FIRMWARE_COMMIT, "FIRMWARE COMMIT" },
	{ SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD, "FIRMWARE IMAGE DOWNLOAD" },
	{ SPDK_NVME_OPC_DEVICE_SELF_TEST, "DEVICE SELF-TEST" },
	{ SPDK_NVME_OPC_NS_ATTACHMENT, "NAMESPACE ATTACHMENT" },
	{ SPDK_NVME_OPC_KEEP_ALIVE, "KEEP ALIVE" },
	{ SPDK_NVME_OPC_DIRECTIVE_SEND, "DIRECTIVE SEND" },
	{ SPDK_NVME_OPC_DIRECTIVE_RECEIVE, "DIRECTIVE RECEIVE" },
	{ SPDK_NVME_OPC_VIRTUALIZATION_MANAGEMENT, "VIRTUALIZATION MANAGEMENT" },
	{ SPDK_NVME_OPC_NVME_MI_SEND, "NVME-MI SEND" },
	{ SPDK_NVME_OPC_NVME_MI_RECEIVE, "NVME-MI RECEIVE" },
	{ SPDK_NVME_OPC_DOORBELL_BUFFER_CONFIG, "DOORBELL BUFFER CONFIG" },
	{ SPDK_NVME_OPC_FORMAT_NVM, "FORMAT NVM" },
	{ SPDK_NVME_OPC_SECURITY_SEND, "SECURITY SEND" },
	{ SPDK_NVME_OPC_SECURITY_RECEIVE, "SECURITY RECEIVE" },
	{ SPDK_NVME_OPC_SANITIZE, "SANITIZE" },
	{ 0xFFFF, "ADMIN COMMAND" }
};

static const struct nvme_string io_opcode[] = {
	{ SPDK_NVME_OPC_FLUSH, "FLUSH" },
	{ SPDK_NVME_OPC_WRITE, "WRITE" },
	{ SPDK_NVME_OPC_READ, "READ" },
	{ SPDK_NVME_OPC_WRITE_UNCORRECTABLE, "WRITE UNCORRECTABLE" },
	{ SPDK_NVME_OPC_COMPARE, "COMPARE" },
	{ SPDK_NVME_OPC_WRITE_ZEROES, "WRITE ZEROES" },
	{ SPDK_NVME_OPC_DATASET_MANAGEMENT, "DATASET MANAGEMENT" },
	{ SPDK_NVME_OPC_RESERVATION_REGISTER, "RESERVATION REGISTER" },
	{ SPDK_NVME_OPC_RESERVATION_REPORT, "RESERVATION REPORT" },
	{ SPDK_NVME_OPC_RESERVATION_ACQUIRE, "RESERVATION ACQUIRE" },
	{ SPDK_NVME_OPC_RESERVATION_RELEASE, "RESERVATION RELEASE" },
	{ 0xFFFF, "IO COMMAND" }
};

static const char *
nvme_get_string(const struct nvme_string *strings, uint16_t value)
{
	const struct nvme_string *entry;

	entry = strings;

	while (entry->value != 0xFFFF) {
		if (entry->value == value) {
			return entry->str;
		}
		entry++;
	}
	return entry->str;
}

static void
nvme_admin_qpair_print_command(struct spdk_nvme_qpair *qpair,
			       struct spdk_nvme_cmd *cmd)
{

	SPDK_NOTICELOG("%s (%02x) sqid:%d cid:%d nsid:%x "
		       "cdw10:%08x cdw11:%08x\n",
		       nvme_get_string(admin_opcode, cmd->opc), cmd->opc, qpair->id, cmd->cid,
		       cmd->nsid, cmd->cdw10, cmd->cdw11);
}

static void
nvme_io_qpair_print_command(struct spdk_nvme_qpair *qpair,
			    struct spdk_nvme_cmd *cmd)
{
	assert(qpair != NULL);
	assert(cmd != NULL);
	switch ((int)cmd->opc) {
	case SPDK_NVME_OPC_WRITE:
	case SPDK_NVME_OPC_READ:
	case SPDK_NVME_OPC_WRITE_UNCORRECTABLE:
	case SPDK_NVME_OPC_COMPARE:
		SPDK_NOTICELOG("%s sqid:%d cid:%d nsid:%d "
			       "lba:%llu len:%d\n",
			       nvme_get_string(io_opcode, cmd->opc), qpair->id, cmd->cid,
			       cmd->nsid,
			       ((unsigned long long)cmd->cdw11 << 32) + cmd->cdw10,
			       (cmd->cdw12 & 0xFFFF) + 1);
		break;
	case SPDK_NVME_OPC_FLUSH:
	case SPDK_NVME_OPC_DATASET_MANAGEMENT:
		SPDK_NOTICELOG("%s sqid:%d cid:%d nsid:%d\n",
			       nvme_get_string(io_opcode, cmd->opc), qpair->id, cmd->cid,
			       cmd->nsid);
		break;
	default:
		SPDK_NOTICELOG("%s (%02x) sqid:%d cid:%d nsid:%d\n",
			       nvme_get_string(io_opcode, cmd->opc), cmd->opc, qpair->id,
			       cmd->cid, cmd->nsid);
		break;
	}
}

void
nvme_qpair_print_command(struct spdk_nvme_qpair *qpair, struct spdk_nvme_cmd *cmd)
{
	assert(qpair != NULL);
	assert(cmd != NULL);

	if (nvme_qpair_is_admin_queue(qpair)) {
		nvme_admin_qpair_print_command(qpair, cmd);
	} else {
		nvme_io_qpair_print_command(qpair, cmd);
	}
}

static const struct nvme_string generic_status[] = {
	{ SPDK_NVME_SC_SUCCESS, "SUCCESS" },
	{ SPDK_NVME_SC_INVALID_OPCODE, "INVALID OPCODE" },
	{ SPDK_NVME_SC_INVALID_FIELD, "INVALID FIELD" },
	{ SPDK_NVME_SC_COMMAND_ID_CONFLICT, "COMMAND ID CONFLICT" },
	{ SPDK_NVME_SC_DATA_TRANSFER_ERROR, "DATA TRANSFER ERROR" },
	{ SPDK_NVME_SC_ABORTED_POWER_LOSS, "ABORTED - POWER LOSS" },
	{ SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, "INTERNAL DEVICE ERROR" },
	{ SPDK_NVME_SC_ABORTED_BY_REQUEST, "ABORTED - BY REQUEST" },
	{ SPDK_NVME_SC_ABORTED_SQ_DELETION, "ABORTED - SQ DELETION" },
	{ SPDK_NVME_SC_ABORTED_FAILED_FUSED, "ABORTED - FAILED FUSED" },
	{ SPDK_NVME_SC_ABORTED_MISSING_FUSED, "ABORTED - MISSING FUSED" },
	{ SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT, "INVALID NAMESPACE OR FORMAT" },
	{ SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR, "COMMAND SEQUENCE ERROR" },
	{ SPDK_NVME_SC_INVALID_SGL_SEG_DESCRIPTOR, "INVALID SGL SEGMENT DESCRIPTOR" },
	{ SPDK_NVME_SC_INVALID_NUM_SGL_DESCIRPTORS, "INVALID NUMBER OF SGL DESCRIPTORS" },
	{ SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID, "DATA SGL LENGTH INVALID" },
	{ SPDK_NVME_SC_METADATA_SGL_LENGTH_INVALID, "METADATA SGL LENGTH INVALID" },
	{ SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID, "SGL DESCRIPTOR TYPE INVALID" },
	{ SPDK_NVME_SC_INVALID_CONTROLLER_MEM_BUF, "INVALID CONTROLLER MEMORY BUFFER" },
	{ SPDK_NVME_SC_INVALID_PRP_OFFSET, "INVALID PRP OFFSET" },
	{ SPDK_NVME_SC_ATOMIC_WRITE_UNIT_EXCEEDED, "ATOMIC WRITE UNIT EXCEEDED" },
	{ SPDK_NVME_SC_OPERATION_DENIED, "OPERATION DENIED" },
	{ SPDK_NVME_SC_INVALID_SGL_OFFSET, "INVALID SGL OFFSET" },
	{ SPDK_NVME_SC_HOSTID_INCONSISTENT_FORMAT, "HOSTID INCONSISTENT FORMAT" },
	{ SPDK_NVME_SC_KEEP_ALIVE_EXPIRED, "KEEP ALIVE EXPIRED" },
	{ SPDK_NVME_SC_KEEP_ALIVE_INVALID, "KEEP ALIVE INVALID" },
	{ SPDK_NVME_SC_ABORTED_PREEMPT, "ABORTED - PREEMPT AND ABORT" },
	{ SPDK_NVME_SC_SANITIZE_FAILED, "SANITIZE FAILED" },
	{ SPDK_NVME_SC_SANITIZE_IN_PROGRESS, "SANITIZE IN PROGRESS" },
	{ SPDK_NVME_SC_SGL_DATA_BLOCK_GRANULARITY_INVALID, "DATA BLOCK GRANULARITY INVALID" },
	{ SPDK_NVME_SC_COMMAND_INVALID_IN_CMB, "COMMAND NOT SUPPORTED FOR QUEUE IN CMB" },
	{ SPDK_NVME_SC_LBA_OUT_OF_RANGE, "LBA OUT OF RANGE" },
	{ SPDK_NVME_SC_CAPACITY_EXCEEDED, "CAPACITY EXCEEDED" },
	{ SPDK_NVME_SC_NAMESPACE_NOT_READY, "NAMESPACE NOT READY" },
	{ SPDK_NVME_SC_RESERVATION_CONFLICT, "RESERVATION CONFLICT" },
	{ SPDK_NVME_SC_FORMAT_IN_PROGRESS, "FORMAT IN PROGRESS" },
	{ 0xFFFF, "GENERIC" }
};

static const struct nvme_string command_specific_status[] = {
	{ SPDK_NVME_SC_COMPLETION_QUEUE_INVALID, "INVALID COMPLETION QUEUE" },
	{ SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER, "INVALID QUEUE IDENTIFIER" },
	{ SPDK_NVME_SC_MAXIMUM_QUEUE_SIZE_EXCEEDED, "MAX QUEUE SIZE EXCEEDED" },
	{ SPDK_NVME_SC_ABORT_COMMAND_LIMIT_EXCEEDED, "ABORT CMD LIMIT EXCEEDED" },
	{ SPDK_NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED, "ASYNC LIMIT EXCEEDED" },
	{ SPDK_NVME_SC_INVALID_FIRMWARE_SLOT, "INVALID FIRMWARE SLOT" },
	{ SPDK_NVME_SC_INVALID_FIRMWARE_IMAGE, "INVALID FIRMWARE IMAGE" },
	{ SPDK_NVME_SC_INVALID_INTERRUPT_VECTOR, "INVALID INTERRUPT VECTOR" },
	{ SPDK_NVME_SC_INVALID_LOG_PAGE, "INVALID LOG PAGE" },
	{ SPDK_NVME_SC_INVALID_FORMAT, "INVALID FORMAT" },
	{ SPDK_NVME_SC_FIRMWARE_REQ_CONVENTIONAL_RESET, "FIRMWARE REQUIRES CONVENTIONAL RESET" },
	{ SPDK_NVME_SC_INVALID_QUEUE_DELETION, "INVALID QUEUE DELETION" },
	{ SPDK_NVME_SC_FEATURE_ID_NOT_SAVEABLE, "FEATURE ID NOT SAVEABLE" },
	{ SPDK_NVME_SC_FEATURE_NOT_CHANGEABLE, "FEATURE NOT CHANGEABLE" },
	{ SPDK_NVME_SC_FEATURE_NOT_NAMESPACE_SPECIFIC, "FEATURE NOT NAMESPACE SPECIFIC" },
	{ SPDK_NVME_SC_FIRMWARE_REQ_NVM_RESET, "FIRMWARE REQUIRES NVM RESET" },
	{ SPDK_NVME_SC_FIRMWARE_REQ_RESET, "FIRMWARE REQUIRES RESET" },
	{ SPDK_NVME_SC_FIRMWARE_REQ_MAX_TIME_VIOLATION, "FIRMWARE REQUIRES MAX TIME VIOLATION" },
	{ SPDK_NVME_SC_FIRMWARE_ACTIVATION_PROHIBITED, "FIRMWARE ACTIVATION PROHIBITED" },
	{ SPDK_NVME_SC_OVERLAPPING_RANGE, "OVERLAPPING RANGE" },
	{ SPDK_NVME_SC_NAMESPACE_INSUFFICIENT_CAPACITY, "NAMESPACE INSUFFICIENT CAPACITY" },
	{ SPDK_NVME_SC_NAMESPACE_ID_UNAVAILABLE, "NAMESPACE ID UNAVAILABLE" },
	{ SPDK_NVME_SC_NAMESPACE_ALREADY_ATTACHED, "NAMESPACE ALREADY ATTACHED" },
	{ SPDK_NVME_SC_NAMESPACE_IS_PRIVATE, "NAMESPACE IS PRIVATE" },
	{ SPDK_NVME_SC_NAMESPACE_NOT_ATTACHED, "NAMESPACE NOT ATTACHED" },
	{ SPDK_NVME_SC_THINPROVISIONING_NOT_SUPPORTED, "THINPROVISIONING NOT SUPPORTED" },
	{ SPDK_NVME_SC_CONTROLLER_LIST_INVALID, "CONTROLLER LIST INVALID" },
	{ SPDK_NVME_SC_DEVICE_SELF_TEST_IN_PROGRESS, "DEVICE SELF-TEST IN PROGRESS" },
	{ SPDK_NVME_SC_BOOT_PARTITION_WRITE_PROHIBITED, "BOOT PARTITION WRITE PROHIBITED" },
	{ SPDK_NVME_SC_INVALID_CTRLR_ID, "INVALID CONTROLLER ID" },
	{ SPDK_NVME_SC_INVALID_SECONDARY_CTRLR_STATE, "INVALID SECONDARY CONTROLLER STATE" },
	{ SPDK_NVME_SC_INVALID_NUM_CTRLR_RESOURCES, "INVALID NUMBER OF CONTROLLER RESOURCES" },
	{ SPDK_NVME_SC_INVALID_RESOURCE_ID, "INVALID RESOURCE IDENTIFIER" },
	{ SPDK_NVME_SC_CONFLICTING_ATTRIBUTES, "CONFLICTING ATTRIBUTES" },
	{ SPDK_NVME_SC_INVALID_PROTECTION_INFO, "INVALID PROTECTION INFO" },
	{ SPDK_NVME_SC_ATTEMPTED_WRITE_TO_RO_PAGE, "WRITE TO RO PAGE" },
	{ 0xFFFF, "COMMAND SPECIFIC" }
};

static const struct nvme_string media_error_status[] = {
	{ SPDK_NVME_SC_WRITE_FAULTS, "WRITE FAULTS" },
	{ SPDK_NVME_SC_UNRECOVERED_READ_ERROR, "UNRECOVERED READ ERROR" },
	{ SPDK_NVME_SC_GUARD_CHECK_ERROR, "GUARD CHECK ERROR" },
	{ SPDK_NVME_SC_APPLICATION_TAG_CHECK_ERROR, "APPLICATION TAG CHECK ERROR" },
	{ SPDK_NVME_SC_REFERENCE_TAG_CHECK_ERROR, "REFERENCE TAG CHECK ERROR" },
	{ SPDK_NVME_SC_COMPARE_FAILURE, "COMPARE FAILURE" },
	{ SPDK_NVME_SC_ACCESS_DENIED, "ACCESS DENIED" },
	{ SPDK_NVME_SC_DEALLOCATED_OR_UNWRITTEN_BLOCK, "DEALLOCATED OR UNWRITTEN BLOCK" },
	{ 0xFFFF, "MEDIA ERROR" }
};

static const char *
get_status_string(uint16_t sct, uint16_t sc)
{
	const struct nvme_string *entry;

	switch (sct) {
	case SPDK_NVME_SCT_GENERIC:
		entry = generic_status;
		break;
	case SPDK_NVME_SCT_COMMAND_SPECIFIC:
		entry = command_specific_status;
		break;
	case SPDK_NVME_SCT_MEDIA_ERROR:
		entry = media_error_status;
		break;
	case SPDK_NVME_SCT_VENDOR_SPECIFIC:
		return "VENDOR SPECIFIC";
	default:
		return "RESERVED";
	}

	return nvme_get_string(entry, sc);
}

void
nvme_qpair_print_completion(struct spdk_nvme_qpair *qpair,
			    struct spdk_nvme_cpl *cpl)
{
	SPDK_NOTICELOG("%s (%02x/%02x) sqid:%d cid:%d cdw0:%x sqhd:%04x p:%x m:%x dnr:%x\n",
		       get_status_string(cpl->status.sct, cpl->status.sc),
		       cpl->status.sct, cpl->status.sc, cpl->sqid, cpl->cid, cpl->cdw0,
		       cpl->sqhd, cpl->status.p, cpl->status.m, cpl->status.dnr);
}

bool
nvme_completion_is_retry(const struct spdk_nvme_cpl *cpl)
{
	/*
	 * TODO: spec is not clear how commands that are aborted due
	 *  to TLER will be marked.  So for now, it seems
	 *  NAMESPACE_NOT_READY is the only case where we should
	 *  look at the DNR bit.
	 */
	switch ((int)cpl->status.sct) {
	case SPDK_NVME_SCT_GENERIC:
		switch ((int)cpl->status.sc) {
		case SPDK_NVME_SC_NAMESPACE_NOT_READY:
		case SPDK_NVME_SC_FORMAT_IN_PROGRESS:
			if (cpl->status.dnr) {
				return false;
			} else {
				return true;
			}
		case SPDK_NVME_SC_INVALID_OPCODE:
		case SPDK_NVME_SC_INVALID_FIELD:
		case SPDK_NVME_SC_COMMAND_ID_CONFLICT:
		case SPDK_NVME_SC_DATA_TRANSFER_ERROR:
		case SPDK_NVME_SC_ABORTED_POWER_LOSS:
		case SPDK_NVME_SC_INTERNAL_DEVICE_ERROR:
		case SPDK_NVME_SC_ABORTED_BY_REQUEST:
		case SPDK_NVME_SC_ABORTED_SQ_DELETION:
		case SPDK_NVME_SC_ABORTED_FAILED_FUSED:
		case SPDK_NVME_SC_ABORTED_MISSING_FUSED:
		case SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT:
		case SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR:
		case SPDK_NVME_SC_LBA_OUT_OF_RANGE:
		case SPDK_NVME_SC_CAPACITY_EXCEEDED:
		default:
			return false;
		}
	case SPDK_NVME_SCT_COMMAND_SPECIFIC:
	case SPDK_NVME_SCT_MEDIA_ERROR:
	case SPDK_NVME_SCT_VENDOR_SPECIFIC:
	default:
		return false;
	}
}

static void
nvme_qpair_manual_complete_request(struct spdk_nvme_qpair *qpair,
				   struct nvme_request *req, uint32_t sct, uint32_t sc,
				   bool print_on_error)
{
	struct spdk_nvme_cpl	cpl;
	bool			error;

	memset(&cpl, 0, sizeof(cpl));
	cpl.sqid = qpair->id;
	cpl.status.sct = sct;
	cpl.status.sc = sc;

	error = spdk_nvme_cpl_is_error(&cpl);

	if (error && print_on_error) {
		nvme_qpair_print_command(qpair, &req->cmd);
		nvme_qpair_print_completion(qpair, &cpl);
	}

	if (req->cb_fn) {
		req->cb_fn(req->cb_arg, &cpl);
	}

	nvme_free_request(req);
}

int32_t
spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	int32_t ret;

	if (qpair->ctrlr->is_failed) {
		nvme_qpair_fail(qpair);
		return 0;
	}

	qpair->in_completion_context = 1;
	ret = nvme_transport_qpair_process_completions(qpair, max_completions);
	qpair->in_completion_context = 0;
	if (qpair->delete_after_completion_context) {
		/*
		 * A request to delete this qpair was made in the context of this completion
		 *  routine - so it is safe to delete it now.
		 */
		spdk_nvme_ctrlr_free_io_qpair(qpair);
	}
	return ret;
}

int
nvme_qpair_init(struct spdk_nvme_qpair *qpair, uint16_t id,
		struct spdk_nvme_ctrlr *ctrlr,
		enum spdk_nvme_qprio qprio,
		uint32_t num_requests)
{
	size_t req_size_padded;
	uint32_t i;

	qpair->id = id;
	qpair->qprio = qprio;

	qpair->in_completion_context = 0;
	qpair->delete_after_completion_context = 0;
	qpair->no_deletion_notification_needed = 0;

	qpair->ctrlr = ctrlr;
	qpair->trtype = ctrlr->trid.trtype;

	STAILQ_INIT(&qpair->free_req);
	STAILQ_INIT(&qpair->queued_req);

	req_size_padded = (sizeof(struct nvme_request) + 63) & ~(size_t)63;

	qpair->req_buf = spdk_dma_zmalloc(req_size_padded * num_requests, 64, NULL);
	if (qpair->req_buf == NULL) {
		return -ENOMEM;
	}

	for (i = 0; i < num_requests; i++) {
		struct nvme_request *req = qpair->req_buf + i * req_size_padded;

		STAILQ_INSERT_HEAD(&qpair->free_req, req, stailq);
	}

	return 0;
}

int
nvme_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	int			rc = 0;
	struct nvme_request	*child_req, *tmp;
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	bool			child_req_failed = false;

	if (ctrlr->is_failed) {
		nvme_free_request(req);
		return -ENXIO;
	}

	if (req->num_children) {
		/*
		 * This is a split (parent) request. Submit all of the children but not the parent
		 * request itself, since the parent is the original unsplit request.
		 */
		TAILQ_FOREACH_SAFE(child_req, &req->children, child_tailq, tmp) {
			if (!child_req_failed) {
				rc = nvme_qpair_submit_request(qpair, child_req);
				if (rc != 0) {
					child_req_failed = true;
				}
			} else { /* free remaining child_reqs since one child_req fails */
				nvme_request_remove_child(req, child_req);
				nvme_free_request(child_req);
			}
		}

		return rc;
	}

	return nvme_transport_qpair_submit_request(qpair, req);
}

static void
_nvme_io_qpair_enable(struct spdk_nvme_qpair *qpair)
{
	struct nvme_request		*req;

	/* Manually abort each queued I/O. */
	while (!STAILQ_EMPTY(&qpair->queued_req)) {
		req = STAILQ_FIRST(&qpair->queued_req);
		STAILQ_REMOVE_HEAD(&qpair->queued_req, stailq);
		SPDK_ERRLOG("aborting queued i/o\n");
		nvme_qpair_manual_complete_request(qpair, req, SPDK_NVME_SCT_GENERIC,
						   SPDK_NVME_SC_ABORTED_BY_REQUEST, true);
	}
}

void
nvme_qpair_enable(struct spdk_nvme_qpair *qpair)
{
	if (nvme_qpair_is_io_queue(qpair)) {
		_nvme_io_qpair_enable(qpair);
	}

	nvme_transport_qpair_enable(qpair);
}

void
nvme_qpair_disable(struct spdk_nvme_qpair *qpair)
{
	nvme_transport_qpair_disable(qpair);
}

static void
nvme_qpair_fail(struct spdk_nvme_qpair *qpair)
{
	struct nvme_request		*req;

	while (!STAILQ_EMPTY(&qpair->queued_req)) {
		req = STAILQ_FIRST(&qpair->queued_req);
		STAILQ_REMOVE_HEAD(&qpair->queued_req, stailq);
		SPDK_ERRLOG("failing queued i/o\n");
		nvme_qpair_manual_complete_request(qpair, req, SPDK_NVME_SCT_GENERIC,
						   SPDK_NVME_SC_ABORTED_BY_REQUEST, true);
	}

	nvme_transport_qpair_fail(qpair);
}
