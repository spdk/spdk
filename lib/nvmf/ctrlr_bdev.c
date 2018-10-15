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

#include "spdk/stdinc.h"

#include "nvmf_internal.h"

#include "spdk/bdev.h"
#include "spdk/endian.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/nvme.h"
#include "spdk/nvmf_spec.h"
#include "spdk/trace.h"
#include "spdk/scsi_spec.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

static bool
spdk_nvmf_subsystem_bdev_io_type_supported(struct spdk_nvmf_subsystem *subsystem,
		enum spdk_bdev_io_type io_type)
{
	struct spdk_nvmf_ns *ns;

	for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
		if (ns->bdev == NULL) {
			continue;
		}

		if (!spdk_bdev_io_type_supported(ns->bdev, io_type)) {
			SPDK_DEBUGLOG(SPDK_LOG_NVMF,
				      "Subsystem %s namespace %u (%s) does not support io_type %d\n",
				      spdk_nvmf_subsystem_get_nqn(subsystem),
				      ns->opts.nsid, spdk_bdev_get_name(ns->bdev), (int)io_type);
			return false;
		}
	}

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "All devices in Subsystem %s support io_type %d\n",
		      spdk_nvmf_subsystem_get_nqn(subsystem), (int)io_type);
	return true;
}

bool
spdk_nvmf_ctrlr_dsm_supported(struct spdk_nvmf_ctrlr *ctrlr)
{
	return spdk_nvmf_subsystem_bdev_io_type_supported(ctrlr->subsys, SPDK_BDEV_IO_TYPE_UNMAP);
}

bool
spdk_nvmf_ctrlr_write_zeroes_supported(struct spdk_nvmf_ctrlr *ctrlr)
{
	return spdk_nvmf_subsystem_bdev_io_type_supported(ctrlr->subsys, SPDK_BDEV_IO_TYPE_WRITE_ZEROES);
}

static void
nvmf_bdev_ctrlr_complete_cmd(struct spdk_bdev_io *bdev_io, bool success,
			     void *cb_arg)
{
	struct spdk_nvmf_request	*req = cb_arg;
	struct spdk_nvme_cpl		*response = &req->rsp->nvme_cpl;
	int				sc, sct;

	spdk_bdev_io_get_nvme_status(bdev_io, &sct, &sc);
	response->status.sc = sc;
	response->status.sct = sct;

	spdk_nvmf_request_complete(req);
	spdk_bdev_free_io(bdev_io);
}

void
spdk_nvmf_bdev_ctrlr_identify_ns(struct spdk_nvmf_ns *ns, struct spdk_nvme_ns_data *nsdata)
{
	struct spdk_bdev *bdev = ns->bdev;
	uint64_t num_blocks;

	num_blocks = spdk_bdev_get_num_blocks(bdev);

	nsdata->nsze = num_blocks;
	nsdata->ncap = num_blocks;
	nsdata->nuse = num_blocks;
	nsdata->nlbaf = 0;
	nsdata->flbas.format = 0;
	nsdata->lbaf[0].lbads = spdk_u32log2(spdk_bdev_get_block_size(bdev));
	nsdata->noiob = spdk_bdev_get_optimal_io_boundary(bdev);
	nsdata->nmic.can_share = 1;

	SPDK_STATIC_ASSERT(sizeof(nsdata->nguid) == sizeof(ns->opts.nguid), "size mismatch");
	memcpy(nsdata->nguid, ns->opts.nguid, sizeof(nsdata->nguid));

	SPDK_STATIC_ASSERT(sizeof(nsdata->eui64) == sizeof(ns->opts.eui64), "size mismatch");
	memcpy(&nsdata->eui64, ns->opts.eui64, sizeof(nsdata->eui64));
}

static void
nvmf_bdev_ctrlr_get_rw_params(const struct spdk_nvme_cmd *cmd, uint64_t *start_lba,
			      uint64_t *num_blocks)
{
	/* SLBA: CDW10 and CDW11 */
	*start_lba = from_le64(&cmd->cdw10);

	/* NLB: CDW12 bits 15:00, 0's based */
	*num_blocks = (from_le32(&cmd->cdw12) & 0xFFFFu) + 1;
}

static bool
nvmf_bdev_ctrlr_lba_in_range(uint64_t bdev_num_blocks, uint64_t io_start_lba,
			     uint64_t io_num_blocks)
{
	if (io_start_lba + io_num_blocks > bdev_num_blocks ||
	    io_start_lba + io_num_blocks < io_start_lba) {
		return false;
	}

	return true;
}

static void
spdk_nvmf_ctrlr_process_io_cmd_resubmit(void *arg)
{
	struct spdk_nvmf_request *req = arg;

	spdk_nvmf_ctrlr_process_io_cmd(req);
}

static void
nvmf_bdev_ctrl_queue_io(struct spdk_nvmf_request *req, struct spdk_bdev *bdev,
			struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn, void *cb_arg)
{
	int rc;

	req->bdev_io_wait.bdev = bdev;
	req->bdev_io_wait.cb_fn = cb_fn;
	req->bdev_io_wait.cb_arg = cb_arg;

	rc = spdk_bdev_queue_io_wait(bdev, ch, &req->bdev_io_wait);
	if (rc != 0) {
		assert(false);
	}
}

static int
nvmf_bdev_ctrlr_read_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			 struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	uint32_t block_size = spdk_bdev_get_block_size(bdev);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmd, &start_lba, &num_blocks);

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(num_blocks * block_size > req->length)) {
		SPDK_ERRLOG("Read NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    num_blocks, block_size, req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_readv_blocks(desc, ch, req->iov, req->iovcnt, start_lba, num_blocks,
				    nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, spdk_nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static int
nvmf_bdev_ctrlr_write_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			  struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	uint32_t block_size = spdk_bdev_get_block_size(bdev);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmd, &start_lba, &num_blocks);

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(num_blocks * block_size > req->length)) {
		SPDK_ERRLOG("Write NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    num_blocks, block_size, req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_writev_blocks(desc, ch, req->iov, req->iovcnt, start_lba, num_blocks,
				     nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, spdk_nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static int
nvmf_bdev_ctrlr_write_zeroes_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				 struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmd, &start_lba, &num_blocks);

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_write_zeroes_blocks(desc, ch, start_lba, num_blocks,
					   nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, spdk_nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static int
nvmf_bdev_ctrlr_flush_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			  struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	int rc;

	/* As for NVMeoF controller, SPDK always set volatile write
	 * cache bit to 1, return success for those block devices
	 * which can't support FLUSH command.
	 */
	if (!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_FLUSH)) {
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_SUCCESS;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_flush_blocks(desc, ch, 0, spdk_bdev_get_num_blocks(bdev),
				    nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, spdk_nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

struct nvmf_virtual_ctrlr_unmap {
	struct spdk_nvmf_request	*req;
	uint32_t			count;
	struct spdk_bdev_desc		*desc;
	struct spdk_bdev		*bdev;
	struct spdk_io_channel		*ch;
};

static void
nvmf_virtual_ctrlr_dsm_cpl(struct spdk_bdev_io *bdev_io, bool success,
			   void *cb_arg)
{
	struct nvmf_virtual_ctrlr_unmap *unmap_ctx = cb_arg;
	struct spdk_nvmf_request	*req = unmap_ctx->req;
	struct spdk_nvme_cpl		*response = &req->rsp->nvme_cpl;
	int				sc, sct;

	unmap_ctx->count--;

	if (response->status.sct == SPDK_NVME_SCT_GENERIC &&
	    response->status.sc == SPDK_NVME_SC_SUCCESS) {
		spdk_bdev_io_get_nvme_status(bdev_io, &sct, &sc);
		response->status.sc = sc;
		response->status.sct = sct;
	}

	if (unmap_ctx->count == 0) {
		spdk_nvmf_request_complete(req);
		free(unmap_ctx);
	}
	spdk_bdev_free_io(bdev_io);
}

static int
nvmf_bdev_ctrlr_dsm_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
			struct nvmf_virtual_ctrlr_unmap *unmap_ctx);
static void
nvmf_bdev_ctrlr_dsm_cmd_resubmit(void *arg)
{
	struct nvmf_virtual_ctrlr_unmap *unmap_ctx = arg;
	struct spdk_nvmf_request *req = unmap_ctx->req;
	struct spdk_bdev_desc *desc = unmap_ctx->desc;
	struct spdk_bdev *bdev = unmap_ctx->bdev;
	struct spdk_io_channel *ch = unmap_ctx->ch;

	nvmf_bdev_ctrlr_dsm_cmd(bdev, desc, ch, req, unmap_ctx);
}

static int
nvmf_bdev_ctrlr_dsm_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
			struct nvmf_virtual_ctrlr_unmap *unmap_ctx)
{
	uint32_t attribute;
	uint16_t nr, i;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	int rc;

	nr = ((cmd->cdw10 & 0x000000ff) + 1);
	if (nr * sizeof(struct spdk_nvme_dsm_range) > req->length) {
		SPDK_ERRLOG("Dataset Management number of ranges > SGL length\n");
		response->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	attribute = cmd->cdw11 & 0x00000007;
	if (attribute & SPDK_NVME_DSM_ATTR_DEALLOCATE) {
		struct spdk_nvme_dsm_range *dsm_range;
		uint64_t lba;
		uint32_t lba_count;

		if (unmap_ctx == NULL) {
			unmap_ctx = calloc(1, sizeof(*unmap_ctx));
			if (!unmap_ctx) {
				response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
			}

			unmap_ctx->req = req;
			unmap_ctx->desc = desc;
			unmap_ctx->ch = ch;
		}

		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_SUCCESS;

		dsm_range = (struct spdk_nvme_dsm_range *)req->data;
		for (i = unmap_ctx->count; i < nr; i++) {
			lba = dsm_range[i].starting_lba;
			lba_count = dsm_range[i].length;

			unmap_ctx->count++;

			rc = spdk_bdev_unmap_blocks(desc, ch, lba, lba_count,
						    nvmf_virtual_ctrlr_dsm_cpl, unmap_ctx);
			if (rc) {
				if (rc == -ENOMEM) {
					nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_bdev_ctrlr_dsm_cmd_resubmit, unmap_ctx);
					/* Unmap was not yet submitted to bdev */
					unmap_ctx->count--;
					return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
				}
				response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
				unmap_ctx->count--;
				/* We can't return here - we may have to wait for any other
				 * unmaps already sent to complete */
				break;
			}
		}

		if (unmap_ctx->count == 0) {
			free(unmap_ctx);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	}

	response->status.sct = SPDK_NVME_SCT_GENERIC;
	response->status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_bdev_ctrlr_nvme_passthru_io(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				 struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	int rc;

	rc = spdk_bdev_nvme_io_passthru(desc, ch, &req->cmd->nvme_cmd, req->data, req->length,
					nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, spdk_nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
spdk_nvmf_ctrlr_process_io_cmd(struct spdk_nvmf_request *req)
{
	uint32_t nsid;
	struct spdk_nvmf_ns *ns;
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
	struct spdk_nvmf_poll_group *group = req->qpair->group;
	struct spdk_nvmf_ctrlr *ctrlr = req->qpair->ctrlr;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	/* pre-set response details for this command */
	response->status.sc = SPDK_NVME_SC_SUCCESS;
	nsid = cmd->nsid;

	if (spdk_unlikely(ctrlr == NULL)) {
		SPDK_ERRLOG("I/O command sent before CONNECT\n");
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(ctrlr->vcprop.cc.bits.en != 1)) {
		SPDK_ERRLOG("I/O command sent to disabled controller\n");
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ns = _spdk_nvmf_subsystem_get_ns(ctrlr->subsys, nsid);
	if (ns == NULL || ns->bdev == NULL) {
		SPDK_ERRLOG("Unsuccessful query for nsid %u\n", cmd->nsid);
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		response->status.dnr = 1;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	bdev = ns->bdev;
	desc = ns->desc;
	ch = group->sgroups[ctrlr->subsys->id].channels[nsid - 1];
	switch (cmd->opc) {
	case SPDK_NVME_OPC_READ:
		return nvmf_bdev_ctrlr_read_cmd(bdev, desc, ch, req);
	case SPDK_NVME_OPC_WRITE:
		return nvmf_bdev_ctrlr_write_cmd(bdev, desc, ch, req);
	case SPDK_NVME_OPC_WRITE_ZEROES:
		return nvmf_bdev_ctrlr_write_zeroes_cmd(bdev, desc, ch, req);
	case SPDK_NVME_OPC_FLUSH:
		return nvmf_bdev_ctrlr_flush_cmd(bdev, desc, ch, req);
	case SPDK_NVME_OPC_DATASET_MANAGEMENT:
		return nvmf_bdev_ctrlr_dsm_cmd(bdev, desc, ch, req, NULL);
	default:
		return nvmf_bdev_ctrlr_nvme_passthru_io(bdev, desc, ch, req);
	}
}
