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

/**
 * \file
 *
 */

static inline bool nvme_qpair_is_admin_queue(struct nvme_qpair *qpair)
{
	return qpair->id == 0;
}

static inline bool nvme_qpair_is_io_queue(struct nvme_qpair *qpair)
{
	return qpair->id != 0;
}

struct nvme_string {
	uint16_t	value;
	const char 	*str;
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
	{ SPDK_NVME_OPC_NS_ATTACHMENT, "NAMESPACE ATTACHMENT" },
	{ SPDK_NVME_OPC_FORMAT_NVM, "FORMAT NVM" },
	{ SPDK_NVME_OPC_SECURITY_SEND, "SECURITY SEND" },
	{ SPDK_NVME_OPC_SECURITY_RECEIVE, "SECURITY RECEIVE" },
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
nvme_admin_qpair_print_command(struct nvme_qpair *qpair,
			       struct spdk_nvme_cmd *cmd)
{

	nvme_printf(qpair->ctrlr, "%s (%02x) sqid:%d cid:%d nsid:%x "
		    "cdw10:%08x cdw11:%08x\n",
		    nvme_get_string(admin_opcode, cmd->opc), cmd->opc, qpair->id, cmd->cid,
		    cmd->nsid, cmd->cdw10, cmd->cdw11);
}

static void
nvme_io_qpair_print_command(struct nvme_qpair *qpair,
			    struct spdk_nvme_cmd *cmd)
{

	switch ((int)cmd->opc) {
	case SPDK_NVME_OPC_WRITE:
	case SPDK_NVME_OPC_READ:
	case SPDK_NVME_OPC_WRITE_UNCORRECTABLE:
	case SPDK_NVME_OPC_COMPARE:
		nvme_printf(qpair->ctrlr, "%s sqid:%d cid:%d nsid:%d "
			    "lba:%llu len:%d\n",
			    nvme_get_string(io_opcode, cmd->opc), qpair->id, cmd->cid,
			    cmd->nsid,
			    ((unsigned long long)cmd->cdw11 << 32) + cmd->cdw10,
			    (cmd->cdw12 & 0xFFFF) + 1);
		break;
	case SPDK_NVME_OPC_FLUSH:
	case SPDK_NVME_OPC_DATASET_MANAGEMENT:
		nvme_printf(qpair->ctrlr, "%s sqid:%d cid:%d nsid:%d\n",
			    nvme_get_string(io_opcode, cmd->opc), qpair->id, cmd->cid,
			    cmd->nsid);
		break;
	default:
		nvme_printf(qpair->ctrlr, "%s (%02x) sqid:%d cid:%d nsid:%d\n",
			    nvme_get_string(io_opcode, cmd->opc), cmd->opc, qpair->id,
			    cmd->cid, cmd->nsid);
		break;
	}
}

static void
nvme_qpair_print_command(struct nvme_qpair *qpair, struct spdk_nvme_cmd *cmd)
{
	nvme_assert(qpair != NULL, ("qpair can not be NULL"));
	nvme_assert(cmd != NULL, ("cmd can not be NULL"));

	if (nvme_qpair_is_admin_queue(qpair)) {
		nvme_admin_qpair_print_command(qpair, cmd);
	} else {
		nvme_io_qpair_print_command(qpair, cmd);
	}
}

static const struct nvme_string generic_status[] = {
	{ SPDK_NVME_SC_SUCCESS, "SUCCESS" },
	{ SPDK_NVME_SC_INVALID_OPCODE, "INVALID OPCODE" },
	{ SPDK_NVME_SC_INVALID_FIELD, "INVALID_FIELD" },
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
	{ SPDK_NVME_SC_LBA_OUT_OF_RANGE, "LBA OUT OF RANGE" },
	{ SPDK_NVME_SC_CAPACITY_EXCEEDED, "CAPACITY EXCEEDED" },
	{ SPDK_NVME_SC_NAMESPACE_NOT_READY, "NAMESPACE NOT READY" },
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
	{ SPDK_NVME_SC_FIRMWARE_REQUIRES_RESET, "FIRMWARE REQUIRES RESET" },
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

static void
nvme_qpair_print_completion(struct nvme_qpair *qpair,
			    struct spdk_nvme_cpl *cpl)
{
	nvme_printf(qpair->ctrlr, "%s (%02x/%02x) sqid:%d cid:%d cdw0:%x sqhd:%04x p:%x m:%x dnr:%x\n",
		    get_status_string(cpl->status.sct, cpl->status.sc),
		    cpl->status.sct, cpl->status.sc, cpl->sqid, cpl->cid, cpl->cdw0,
		    cpl->sqhd, cpl->status.p, cpl->status.m, cpl->status.dnr);
}

static bool
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
		case SPDK_NVME_SC_ABORTED_BY_REQUEST:
		case SPDK_NVME_SC_NAMESPACE_NOT_READY:
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
nvme_qpair_construct_tracker(struct nvme_tracker *tr, uint16_t cid, uint64_t phys_addr)
{
	tr->prp_bus_addr = phys_addr + offsetof(struct nvme_tracker, prp);
	tr->cid = cid;
}

static void
nvme_qpair_submit_tracker(struct nvme_qpair *qpair, struct nvme_tracker *tr)
{
	struct nvme_request	*req;

	req = tr->req;
	qpair->act_tr[tr->cid] = tr;

	/* Copy the command from the tracker to the submission queue. */
	nvme_copy_command(&qpair->cmd[qpair->sq_tail], &req->cmd);

	if (++qpair->sq_tail == qpair->num_entries) {
		qpair->sq_tail = 0;
	}

	spdk_wmb();
	spdk_mmio_write_4(qpair->sq_tdbl, qpair->sq_tail);
}

static void
nvme_qpair_complete_tracker(struct nvme_qpair *qpair, struct nvme_tracker *tr,
			    struct spdk_nvme_cpl *cpl, bool print_on_error)
{
	struct nvme_request	*req;
	bool			retry, error;

	req = tr->req;

	nvme_assert(req != NULL, ("tr has NULL req\n"));

	error = spdk_nvme_cpl_is_error(cpl);
	retry = error && nvme_completion_is_retry(cpl) &&
		req->retries < spdk_nvme_retry_count;

	if (error && print_on_error) {
		nvme_qpair_print_command(qpair, &req->cmd);
		nvme_qpair_print_completion(qpair, cpl);
	}

	qpair->act_tr[cpl->cid] = NULL;

	nvme_assert(cpl->cid == req->cmd.cid, ("cpl cid does not match cmd cid\n"));

	if (retry) {
		req->retries++;
		nvme_qpair_submit_tracker(qpair, tr);
	} else {
		if (req->cb_fn) {
			req->cb_fn(req->cb_arg, cpl);
		}

		nvme_free_request(req);
		tr->req = NULL;

		LIST_REMOVE(tr, list);
		LIST_INSERT_HEAD(&qpair->free_tr, tr, list);

		/*
		 * If the controller is in the middle of resetting, don't
		 *  try to submit queued requests here - let the reset logic
		 *  handle that instead.
		 */
		if (!STAILQ_EMPTY(&qpair->queued_req) &&
		    !qpair->ctrlr->is_resetting) {
			req = STAILQ_FIRST(&qpair->queued_req);
			STAILQ_REMOVE_HEAD(&qpair->queued_req, stailq);
			nvme_qpair_submit_request(qpair, req);
		}
	}
}

static void
nvme_qpair_manual_complete_tracker(struct nvme_qpair *qpair,
				   struct nvme_tracker *tr, uint32_t sct, uint32_t sc, uint32_t dnr,
				   bool print_on_error)
{
	struct spdk_nvme_cpl	cpl;

	memset(&cpl, 0, sizeof(cpl));
	cpl.sqid = qpair->id;
	cpl.cid = tr->cid;
	cpl.status.sct = sct;
	cpl.status.sc = sc;
	cpl.status.dnr = dnr;
	nvme_qpair_complete_tracker(qpair, tr, &cpl, print_on_error);
}

static void
nvme_qpair_manual_complete_request(struct nvme_qpair *qpair,
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

static inline bool
nvme_qpair_check_enabled(struct nvme_qpair *qpair)
{
	if (!qpair->is_enabled &&
	    !qpair->ctrlr->is_resetting) {
		nvme_qpair_enable(qpair);
	}
	return qpair->is_enabled;
}

/**
 * \page nvme_async_completion NVMe Asynchronous Completion
 *
 * The userspace NVMe driver follows an asynchronous polled model for
 * I/O completion.
 *
 * \section async_io I/O commands
 *
 * The application may submit I/O from one or more threads
 * and must call nvme_ctrlr_process_io_completions()
 * from each thread that submitted I/O.
 *
 * When the application calls nvme_ctrlr_process_io_completions(),
 * if the NVMe driver detects completed I/Os that were submitted on that thread,
 * it will invoke the registered callback function
 * for each I/O within the context of nvme_ctrlr_process_io_completions().
 *
 * \section async_admin Admin commands
 *
 * The application may submit admin commands from one or more threads
 * and must call nvme_ctrlr_process_admin_completions()
 * from at least one thread to receive admin command completions.
 * The thread that processes admin completions need not be the same thread that submitted the
 * admin commands.
 *
 * When the application calls nvme_ctrlr_process_admin_completions(),
 * if the NVMe driver detects completed admin commands submitted from any thread,
 * it will invote the registered callback function
 * for each command within the context of nvme_ctrlr_process_admin_completions().
 *
 * It is the application's responsibility to manage the order of submitted admin commands.
 * If certain admin commands must be submitted while no other commands are outstanding,
 * it is the application's responsibility to enforce this rule
 * using its own synchronization method.
 */

/**
 * \brief Checks for and processes completions on the specified qpair.
 *
 * For each completed command, the request's callback function will
 *  be called if specified as non-NULL when the request was submitted.
 *
 * \sa nvme_cb_fn_t
 */
int32_t
nvme_qpair_process_completions(struct nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_tracker	*tr;
	struct spdk_nvme_cpl	*cpl;
	uint32_t num_completions = 0;

	if (!nvme_qpair_check_enabled(qpair)) {
		/*
		 * qpair is not enabled, likely because a controller reset is
		 *  is in progress.  Ignore the interrupt - any I/O that was
		 *  associated with this interrupt will get retried when the
		 *  reset is complete.
		 */
		return 0;
	}

	if (max_completions == 0) {
		/*
		 * max_completions == 0 means unlimited; set it to the max uint32_t value
		 *  to avoid a special case in the loop.  The maximum possible queue size is
		 *  only 64K, so num_completions will never reach this value.
		 */
		max_completions = UINT32_MAX;
	}

	while (1) {
		cpl = &qpair->cpl[qpair->cq_head];

		if (cpl->status.p != qpair->phase)
			break;

		tr = qpair->act_tr[cpl->cid];

		if (tr != NULL) {
			nvme_qpair_complete_tracker(qpair, tr, cpl, true);
		} else {
			nvme_printf(qpair->ctrlr,
				    "cpl does not map to outstanding cmd\n");
			nvme_qpair_print_completion(qpair, cpl);
			nvme_assert(0, ("received completion for unknown cmd\n"));
		}

		if (++qpair->cq_head == qpair->num_entries) {
			qpair->cq_head = 0;
			qpair->phase = !qpair->phase;
		}

		if (++num_completions == max_completions) {
			break;
		}
	}

	if (num_completions > 0) {
		spdk_mmio_write_4(qpair->cq_hdbl, qpair->cq_head);
	}

	return num_completions;
}

int
nvme_qpair_construct(struct nvme_qpair *qpair, uint16_t id,
		     uint16_t num_entries, uint16_t num_trackers,
		     struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_tracker	*tr;
	uint16_t		i;
	volatile uint32_t	*doorbell_base;
	uint64_t		phys_addr = 0;

	nvme_assert(num_entries != 0, ("invalid num_entries\n"));
	nvme_assert(num_trackers != 0, ("invalid num_trackers\n"));

	qpair->id = id;
	qpair->num_entries = num_entries;

	qpair->ctrlr = ctrlr;

	/* cmd and cpl rings must be aligned on 4KB boundaries. */
	qpair->cmd = nvme_malloc("qpair_cmd",
				 qpair->num_entries * sizeof(struct spdk_nvme_cmd),
				 0x1000,
				 &qpair->cmd_bus_addr);
	if (qpair->cmd == NULL) {
		nvme_printf(ctrlr, "alloc qpair_cmd failed\n");
		goto fail;
	}
	qpair->cpl = nvme_malloc("qpair_cpl",
				 qpair->num_entries * sizeof(struct spdk_nvme_cpl),
				 0x1000,
				 &qpair->cpl_bus_addr);
	if (qpair->cpl == NULL) {
		nvme_printf(ctrlr, "alloc qpair_cpl failed\n");
		goto fail;
	}

	doorbell_base = &ctrlr->regs->doorbell[0].sq_tdbl;
	qpair->sq_tdbl = doorbell_base + (2 * id + 0) * ctrlr->doorbell_stride_u32;
	qpair->cq_hdbl = doorbell_base + (2 * id + 1) * ctrlr->doorbell_stride_u32;

	LIST_INIT(&qpair->free_tr);
	LIST_INIT(&qpair->outstanding_tr);
	STAILQ_INIT(&qpair->queued_req);

	for (i = 0; i < num_trackers; i++) {
		/*
		 * Round alignment up to next power of 2.  This ensures the PRP
		 *  list embedded in the nvme_tracker object will not span a
		 *  4KB boundary.
		 */
		tr = nvme_malloc("nvme_tr", sizeof(*tr), nvme_align32pow2(sizeof(*tr)), &phys_addr);
		if (tr == NULL) {
			nvme_printf(ctrlr, "nvme_tr failed\n");
			goto fail;
		}
		nvme_qpair_construct_tracker(tr, i, phys_addr);
		LIST_INSERT_HEAD(&qpair->free_tr, tr, list);
	}

	qpair->act_tr = calloc(num_trackers, sizeof(struct nvme_tracker *));
	if (qpair->act_tr == NULL) {
		nvme_printf(ctrlr, "alloc nvme_act_tr failed\n");
		goto fail;
	}
	nvme_qpair_reset(qpair);
	return 0;
fail:
	nvme_qpair_destroy(qpair);
	return -1;
}

static void
nvme_admin_qpair_abort_aers(struct nvme_qpair *qpair)
{
	struct nvme_tracker	*tr;

	tr = LIST_FIRST(&qpair->outstanding_tr);
	while (tr != NULL) {
		if (tr->req->cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
			nvme_qpair_manual_complete_tracker(qpair, tr,
							   SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_ABORTED_SQ_DELETION, 0,
							   false);
			tr = LIST_FIRST(&qpair->outstanding_tr);
		} else {
			tr = LIST_NEXT(tr, list);
		}
	}
}

static void
_nvme_admin_qpair_destroy(struct nvme_qpair *qpair)
{
	nvme_admin_qpair_abort_aers(qpair);
}


void
nvme_qpair_destroy(struct nvme_qpair *qpair)
{
	struct nvme_tracker	*tr;

	if (nvme_qpair_is_admin_queue(qpair)) {
		_nvme_admin_qpair_destroy(qpair);
	}
	if (qpair->cmd)
		nvme_free(qpair->cmd);
	if (qpair->cpl)
		nvme_free(qpair->cpl);
	if (qpair->act_tr)
		free(qpair->act_tr);

	while (!LIST_EMPTY(&qpair->free_tr)) {
		tr = LIST_FIRST(&qpair->free_tr);
		LIST_REMOVE(tr, list);
		nvme_free(tr);
	}
}

/**
 * \page nvme_io_submission NVMe I/O Submission
 *
 * I/O is submitted to an NVMe namespace using nvme_ns_cmd_xxx functions
 * defined in nvme_ns_cmd.c.  The NVMe driver submits the I/O request
 * as an NVMe submission queue entry on the nvme_qpair associated with
 * the logical core that submits the I/O.
 *
 * \sa nvme_ns_cmd_read, nvme_ns_cmd_write, nvme_ns_cmd_deallocate,
 *     nvme_ns_cmd_flush, nvme_get_ioq_idx
 */

static void
_nvme_fail_request_bad_vtophys(struct nvme_qpair *qpair, struct nvme_tracker *tr)
{
	/*
	 * Bad vtophys translation, so abort this request and return
	 *  immediately.
	 */
	nvme_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC,
					   SPDK_NVME_SC_INVALID_FIELD,
					   1 /* do not retry */, true);
}

static void
_nvme_fail_request_ctrlr_failed(struct nvme_qpair *qpair, struct nvme_request *req)
{
	nvme_qpair_manual_complete_request(qpair, req, SPDK_NVME_SCT_GENERIC,
					   SPDK_NVME_SC_ABORTED_BY_REQUEST, true);
}

/**
 * Build PRP list describing physically contiguous payload buffer.
 */
static int
_nvme_qpair_build_contig_request(struct nvme_qpair *qpair, struct nvme_request *req,
				 struct nvme_tracker *tr)
{
	uint64_t phys_addr;
	void *seg_addr;
	uint32_t nseg, cur_nseg, modulo, unaligned;
	void *payload = req->payload.u.contig + req->payload_offset;

	phys_addr = nvme_vtophys(payload);
	if (phys_addr == NVME_VTOPHYS_ERROR) {
		_nvme_fail_request_bad_vtophys(qpair, tr);
		return -1;
	}
	nseg = req->payload_size >> nvme_u32log2(PAGE_SIZE);
	modulo = req->payload_size & (PAGE_SIZE - 1);
	unaligned = phys_addr & (PAGE_SIZE - 1);
	if (modulo || unaligned) {
		nseg += 1 + ((modulo + unaligned - 1) >> nvme_u32log2(PAGE_SIZE));
	}

	tr->req->cmd.psdt = SPDK_NVME_PSDT_PRP;
	tr->req->cmd.dptr.prp.prp1 = phys_addr;
	if (nseg == 2) {
		seg_addr = payload + PAGE_SIZE - unaligned;
		tr->req->cmd.dptr.prp.prp2 = nvme_vtophys(seg_addr);
	} else if (nseg > 2) {
		cur_nseg = 1;
		tr->req->cmd.dptr.prp.prp2 = (uint64_t)tr->prp_bus_addr;
		while (cur_nseg < nseg) {
			seg_addr = payload + cur_nseg * PAGE_SIZE - unaligned;
			phys_addr = nvme_vtophys(seg_addr);
			if (phys_addr == NVME_VTOPHYS_ERROR) {
				_nvme_fail_request_bad_vtophys(qpair, tr);
				return -1;
			}
			tr->prp[cur_nseg - 1] = phys_addr;
			cur_nseg++;
		}
	}

	return 0;
}

static int
_nvme_qpair_build_sgl_request(struct nvme_qpair *qpair, struct nvme_request *req,
			      struct nvme_tracker *tr)
{
	int rc;
	uint64_t phys_addr;
	uint32_t data_transfered, remaining_transfer_len, length;
	uint32_t nseg, cur_nseg, total_nseg, last_nseg, modulo, unaligned;
	uint32_t sge_count = 0;
	uint64_t prp2 = 0;
	struct nvme_request *parent;

	/*
	 * Build scattered payloads.
	 */

	parent = req->parent ? req->parent : req;
	nvme_assert(req->payload.type == NVME_PAYLOAD_TYPE_SGL, ("sgl payload type required\n"));
	nvme_assert(req->payload.u.sgl.reset_sgl_fn != NULL, ("sgl reset callback required\n"));
	req->payload.u.sgl.reset_sgl_fn(parent->cb_arg, req->payload_offset);

	remaining_transfer_len = req->payload_size;
	total_nseg = 0;
	last_nseg = 0;

	while (remaining_transfer_len > 0) {
		nvme_assert(req->payload.u.sgl.next_sge_fn != NULL, ("sgl callback required\n"));
		rc = req->payload.u.sgl.next_sge_fn(parent->cb_arg, &phys_addr, &length);
		if (rc) {
			_nvme_fail_request_bad_vtophys(qpair, tr);
			return -1;
		}

		data_transfered = nvme_min(remaining_transfer_len, length);

		nseg = data_transfered >> nvme_u32log2(PAGE_SIZE);
		modulo = data_transfered & (PAGE_SIZE - 1);
		unaligned = phys_addr & (PAGE_SIZE - 1);
		if (modulo || unaligned) {
			nseg += 1 + ((modulo + unaligned - 1) >> nvme_u32log2(PAGE_SIZE));
		}

		if (total_nseg == 0) {
			req->cmd.psdt = SPDK_NVME_PSDT_PRP;
			req->cmd.dptr.prp.prp1 = phys_addr;
		}

		total_nseg += nseg;
		sge_count++;
		remaining_transfer_len -= data_transfered;

		if (total_nseg == 2) {
			if (sge_count == 1)
				tr->req->cmd.dptr.prp.prp2 = phys_addr + PAGE_SIZE - unaligned;
			else if (sge_count == 2)
				tr->req->cmd.dptr.prp.prp2 = phys_addr;
			/* save prp2 value */
			prp2 = tr->req->cmd.dptr.prp.prp2;
		} else if (total_nseg > 2) {
			if (sge_count == 1)
				cur_nseg = 1;
			else
				cur_nseg = 0;

			tr->req->cmd.dptr.prp.prp2 = (uint64_t)tr->prp_bus_addr;
			while (cur_nseg < nseg) {
				if (prp2) {
					tr->prp[0] = prp2;
					tr->prp[last_nseg + 1] = phys_addr + cur_nseg * PAGE_SIZE - unaligned;
				} else
					tr->prp[last_nseg] = phys_addr + cur_nseg * PAGE_SIZE - unaligned;

				last_nseg++;
				cur_nseg++;

				/* physical address and length check */
				if (remaining_transfer_len || (!remaining_transfer_len && (cur_nseg < nseg))) {
					if ((length & (PAGE_SIZE - 1)) || unaligned) {
						_nvme_fail_request_bad_vtophys(qpair, tr);
						return -1;
					}
				}
			}
		}
	}

	return 0;
}

void
nvme_qpair_submit_request(struct nvme_qpair *qpair, struct nvme_request *req)
{
	int			rc;
	struct nvme_tracker	*tr;
	struct nvme_request	*child_req;

	nvme_qpair_check_enabled(qpair);

	if (req->num_children) {
		/*
		 * This is a split (parent) request. Submit all of the children but not the parent
		 * request itself, since the parent is the original unsplit request.
		 */
		TAILQ_FOREACH(child_req, &req->children, child_tailq) {
			nvme_qpair_submit_request(qpair, child_req);
		}
		return;
	}

	tr = LIST_FIRST(&qpair->free_tr);

	if (tr == NULL || !qpair->is_enabled) {
		/*
		 * No tracker is available, or the qpair is disabled due to
		 *  an in-progress controller-level reset or controller
		 *  failure.
		 */

		if (qpair->ctrlr->is_failed) {
			_nvme_fail_request_ctrlr_failed(qpair, req);
		} else {
			/*
			 * Put the request on the qpair's request queue to be
			 *  processed when a tracker frees up via a command
			 *  completion or when the controller reset is
			 *  completed.
			 */
			STAILQ_INSERT_TAIL(&qpair->queued_req, req, stailq);
		}
		return;
	}

	LIST_REMOVE(tr, list); /* remove tr from free_tr */
	LIST_INSERT_HEAD(&qpair->outstanding_tr, tr, list);
	tr->req = req;
	req->cmd.cid = tr->cid;

	if (req->payload_size == 0) {
		/* Null payload - leave PRP fields zeroed */
	} else if (req->payload.type == NVME_PAYLOAD_TYPE_CONTIG) {
		rc = _nvme_qpair_build_contig_request(qpair, req, tr);
		if (rc < 0) {
			return;
		}
	} else if (req->payload.type == NVME_PAYLOAD_TYPE_SGL) {
		rc = _nvme_qpair_build_sgl_request(qpair, req, tr);
		if (rc < 0) {
			return;
		}
	} else {
		nvme_assert(0, ("invalid NVMe payload type %d\n", req->payload.type));
		_nvme_fail_request_bad_vtophys(qpair, tr);
		return;
	}

	nvme_qpair_submit_tracker(qpair, tr);
}

void
nvme_qpair_reset(struct nvme_qpair *qpair)
{
	qpair->sq_tail = qpair->cq_head = 0;

	/*
	 * First time through the completion queue, HW will set phase
	 *  bit on completions to 1.  So set this to 1 here, indicating
	 *  we're looking for a 1 to know which entries have completed.
	 *  we'll toggle the bit each time when the completion queue
	 *  rolls over.
	 */
	qpair->phase = 1;

	memset(qpair->cmd, 0,
	       qpair->num_entries * sizeof(struct spdk_nvme_cmd));
	memset(qpair->cpl, 0,
	       qpair->num_entries * sizeof(struct spdk_nvme_cpl));
}

static void
_nvme_admin_qpair_enable(struct nvme_qpair *qpair)
{
	struct nvme_tracker		*tr;
	struct nvme_tracker		*tr_temp;

	/*
	 * Manually abort each outstanding admin command.  Do not retry
	 *  admin commands found here, since they will be left over from
	 *  a controller reset and its likely the context in which the
	 *  command was issued no longer applies.
	 */
	LIST_FOREACH_SAFE(tr, &qpair->outstanding_tr, list, tr_temp) {
		nvme_printf(qpair->ctrlr,
			    "aborting outstanding admin command\n");
		nvme_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC,
						   SPDK_NVME_SC_ABORTED_BY_REQUEST, 1 /* do not retry */, true);
	}

	qpair->is_enabled = true;
}

static void
_nvme_io_qpair_enable(struct nvme_qpair *qpair)
{
	STAILQ_HEAD(, nvme_request)	temp;
	struct nvme_tracker		*tr;
	struct nvme_tracker		*tr_temp;
	struct nvme_request		*req;

	qpair->is_enabled = true;
	/*
	 * Manually abort each outstanding I/O.  This normally results in a
	 *  retry, unless the retry count on the associated request has
	 *  reached its limit.
	 */
	LIST_FOREACH_SAFE(tr, &qpair->outstanding_tr, list, tr_temp) {
		nvme_printf(qpair->ctrlr, "aborting outstanding i/o\n");
		nvme_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC,
						   SPDK_NVME_SC_ABORTED_BY_REQUEST, 0, true);
	}


	STAILQ_INIT(&temp);
	STAILQ_SWAP(&qpair->queued_req, &temp, nvme_request);

	while (!STAILQ_EMPTY(&temp)) {
		req = STAILQ_FIRST(&temp);
		STAILQ_REMOVE_HEAD(&temp, stailq);

		nvme_printf(qpair->ctrlr, "resubmitting queued i/o\n");
		nvme_qpair_print_command(qpair, &req->cmd);
		nvme_qpair_submit_request(qpair, req);
	}
}

void
nvme_qpair_enable(struct nvme_qpair *qpair)
{
	if (nvme_qpair_is_io_queue(qpair)) {
		_nvme_io_qpair_enable(qpair);
	} else {
		_nvme_admin_qpair_enable(qpair);
	}
}

static void
_nvme_admin_qpair_disable(struct nvme_qpair *qpair)
{
	qpair->is_enabled = false;
	nvme_admin_qpair_abort_aers(qpair);
}

static void
_nvme_io_qpair_disable(struct nvme_qpair *qpair)
{
	qpair->is_enabled = false;
}

void
nvme_qpair_disable(struct nvme_qpair *qpair)
{
	if (nvme_qpair_is_io_queue(qpair)) {
		_nvme_io_qpair_disable(qpair);
	} else {
		_nvme_admin_qpair_disable(qpair);
	}
}

void
nvme_qpair_fail(struct nvme_qpair *qpair)
{
	struct nvme_tracker		*tr;
	struct nvme_request		*req;

	while (!STAILQ_EMPTY(&qpair->queued_req)) {
		req = STAILQ_FIRST(&qpair->queued_req);
		STAILQ_REMOVE_HEAD(&qpair->queued_req, stailq);
		nvme_printf(qpair->ctrlr, "failing queued i/o\n");
		nvme_qpair_manual_complete_request(qpair, req, SPDK_NVME_SCT_GENERIC,
						   SPDK_NVME_SC_ABORTED_BY_REQUEST, true);
	}

	/* Manually abort each outstanding I/O. */
	while (!LIST_EMPTY(&qpair->outstanding_tr)) {
		tr = LIST_FIRST(&qpair->outstanding_tr);
		/*
		 * Do not remove the tracker.  The abort_tracker path will
		 *  do that for us.
		 */
		nvme_printf(qpair->ctrlr, "failing outstanding i/o\n");
		nvme_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC,
						   SPDK_NVME_SC_ABORTED_BY_REQUEST, 1 /* do not retry */, true);
	}
}

