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
#include "spdk/io_channel.h"
#include "spdk/nvme.h"
#include "spdk/nvmf_spec.h"
#include "spdk/trace.h"
#include "spdk/scsi_spec.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

/* read command dword 12 */
struct __attribute__((packed)) nvme_read_cdw12 {
	uint16_t	nlb;		/* number of logical blocks */
	uint16_t	rsvd	: 10;
	uint8_t		prinfo	: 4;	/* protection information field */
	uint8_t		fua	: 1;	/* force unit access */
	uint8_t		lr	: 1;	/* limited retry */
};

bool
spdk_nvmf_ctrlr_dsm_supported(struct spdk_nvmf_ctrlr *ctrlr)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvmf_ns *ns;

	for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
		if (ns->bdev == NULL) {
			continue;
		}

		if (!spdk_bdev_io_type_supported(ns->bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
			SPDK_DEBUGLOG(SPDK_TRACE_NVMF,
				      "Subsystem %s namespace %u (%s) does not support unmap - not enabling DSM\n",
				      spdk_nvmf_subsystem_get_nqn(subsystem),
				      ns->id, spdk_bdev_get_name(ns->bdev));
			return false;
		}
	}

	SPDK_DEBUGLOG(SPDK_TRACE_NVMF, "All devices in Subsystem %s support unmap - enabling DSM\n",
		      spdk_nvmf_subsystem_get_nqn(subsystem));
	return true;
}

static void
nvmf_bdev_ctrlr_complete_cmd(struct spdk_bdev_io *bdev_io, bool success,
			     void *cb_arg)
{
	struct spdk_nvmf_request 	*req = cb_arg;
	struct spdk_nvme_cpl 		*response = &req->rsp->nvme_cpl;
	int				sc, sct;

	spdk_bdev_io_get_nvme_status(bdev_io, &sc, &sct);
	response->status.sc = sc;
	response->status.sct = sct;

	spdk_nvmf_request_complete(req);
	spdk_bdev_free_io(bdev_io);
}

int
spdk_nvmf_bdev_ctrlr_identify_ns(struct spdk_bdev *bdev, struct spdk_nvme_ns_data *nsdata)
{
	uint64_t num_blocks;

	num_blocks = spdk_bdev_get_num_blocks(bdev);

	nsdata->nsze = num_blocks;
	nsdata->ncap = num_blocks;
	nsdata->nuse = num_blocks;
	nsdata->nlbaf = 0;
	nsdata->flbas.format = 0;
	nsdata->lbaf[0].lbads = spdk_u32log2(spdk_bdev_get_block_size(bdev));
	nsdata->noiob = spdk_bdev_get_optimal_io_boundary(bdev);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_bdev_ctrlr_rw_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		       struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	uint64_t lba_address;
	uint64_t blockcnt;
	uint64_t io_bytes;
	uint64_t offset;
	uint64_t llen;
	uint32_t block_size = spdk_bdev_get_block_size(bdev);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct nvme_read_cdw12 *cdw12 = (struct nvme_read_cdw12 *)&cmd->cdw12;

	blockcnt = spdk_bdev_get_num_blocks(bdev);
	lba_address = cmd->cdw11;
	lba_address = (lba_address << 32) + cmd->cdw10;
	offset = lba_address * block_size;
	llen = cdw12->nlb + 1;

	if (lba_address >= blockcnt || llen > blockcnt || lba_address > (blockcnt - llen)) {
		SPDK_ERRLOG("end of media\n");
		response->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	io_bytes = llen * block_size;
	if (io_bytes > req->length) {
		SPDK_ERRLOG("Read/Write NLB > SGL length\n");
		response->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (cmd->opc == SPDK_NVME_OPC_READ) {
		spdk_trace_record(TRACE_NVMF_LIB_READ_START, 0, 0, (uint64_t)req, 0);
		if (spdk_bdev_read(desc, ch, req->data, offset, req->length, nvmf_bdev_ctrlr_complete_cmd,
				   req)) {
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	} else {
		spdk_trace_record(TRACE_NVMF_LIB_WRITE_START, 0, 0, (uint64_t)req, 0);
		if (spdk_bdev_write(desc, ch, req->data, offset, req->length, nvmf_bdev_ctrlr_complete_cmd,
				    req)) {
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}
	}
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;

}

static int
nvmf_bdev_ctrlr_flush_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			  struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	uint64_t nbytes;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	nbytes = spdk_bdev_get_num_blocks(bdev) * spdk_bdev_get_block_size(bdev);
	if (spdk_bdev_flush(desc, ch, 0, nbytes, nvmf_bdev_ctrlr_complete_cmd, req)) {
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

struct nvmf_virtual_ctrlr_unmap {
	struct spdk_nvmf_request	*req;
	uint32_t			count;
};

static void
nvmf_virtual_ctrlr_dsm_cpl(struct spdk_bdev_io *bdev_io, bool success,
			   void *cb_arg)
{
	struct nvmf_virtual_ctrlr_unmap *unmap_ctx = cb_arg;
	struct spdk_nvmf_request 	*req = unmap_ctx->req;
	struct spdk_nvme_cpl 		*response = &req->rsp->nvme_cpl;
	int				sc, sct;

	unmap_ctx->count--;

	if (response->status.sct == SPDK_NVME_SCT_GENERIC &&
	    response->status.sc == SPDK_NVME_SC_SUCCESS) {
		spdk_bdev_io_get_nvme_status(bdev_io, &sc, &sct);
		response->status.sc = sc;
		response->status.sct = sct;
	}

	if (unmap_ctx->count == 0) {
		spdk_nvmf_request_complete(req);
		spdk_bdev_free_io(bdev_io);
		free(unmap_ctx);
	}
}

static int
nvmf_bdev_ctrlr_dsm_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	uint32_t attribute;
	uint16_t nr, i;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	nr = ((cmd->cdw10 & 0x000000ff) + 1);
	if (nr * sizeof(struct spdk_nvme_dsm_range) > req->length) {
		SPDK_ERRLOG("Dataset Management number of ranges > SGL length\n");
		response->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	attribute = cmd->cdw11 & 0x00000007;
	if (attribute & SPDK_NVME_DSM_ATTR_DEALLOCATE) {
		struct nvmf_virtual_ctrlr_unmap *unmap_ctx;
		struct spdk_nvme_dsm_range *dsm_range;
		uint64_t lba;
		uint32_t lba_count;
		uint32_t block_size = spdk_bdev_get_block_size(bdev);

		unmap_ctx = calloc(1, sizeof(*unmap_ctx));
		if (!unmap_ctx) {
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		unmap_ctx->req = req;

		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_SUCCESS;

		dsm_range = (struct spdk_nvme_dsm_range *)req->data;
		for (i = 0; i < nr; i++) {
			lba = dsm_range[i].starting_lba;
			lba_count = dsm_range[i].length;

			unmap_ctx->count++;

			if (spdk_bdev_unmap(desc, ch, lba * block_size, lba_count * block_size,
					    nvmf_virtual_ctrlr_dsm_cpl, unmap_ctx)) {
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

	response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_bdev_ctrlr_nvme_passthru_io(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				 struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	if (spdk_bdev_nvme_io_passthru(desc, ch, &req->cmd->nvme_cmd, req->data, req->length,
				       nvmf_bdev_ctrlr_complete_cmd, req)) {
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
	struct spdk_nvmf_subsystem *subsystem = req->qpair->ctrlr->subsys;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	/* pre-set response details for this command */
	response->status.sc = SPDK_NVME_SC_SUCCESS;
	nsid = cmd->nsid;

	ns = _spdk_nvmf_subsystem_get_ns(subsystem, nsid);
	if (ns == NULL || ns->bdev == NULL) {
		SPDK_ERRLOG("Unsuccessful query for nsid %u\n", cmd->nsid);
		response->status.sc = SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	bdev = ns->bdev;
	desc = ns->desc;
	ch = ns->ch;
	switch (cmd->opc) {
	case SPDK_NVME_OPC_READ:
	case SPDK_NVME_OPC_WRITE:
		return nvmf_bdev_ctrlr_rw_cmd(bdev, desc, ch, req);
	case SPDK_NVME_OPC_FLUSH:
		return nvmf_bdev_ctrlr_flush_cmd(bdev, desc, ch, req);
	case SPDK_NVME_OPC_DATASET_MANAGEMENT:
		return nvmf_bdev_ctrlr_dsm_cmd(bdev, desc, ch, req);
	default:
		return nvmf_bdev_ctrlr_nvme_passthru_io(bdev, desc, ch, req);
	}
}

static int
spdk_nvmf_ns_bdev_attach(struct spdk_nvmf_ns *ns)
{
	if (ns->bdev == NULL) {
		return 0;
	}

	ns->ch = spdk_bdev_get_io_channel(ns->desc);
	if (ns->ch == NULL) {
		SPDK_ERRLOG("io_channel allocation failed\n");
		return -1;
	}

	return 0;
}

static void
spdk_nvmf_ns_bdev_detach(struct spdk_nvmf_ns *ns)
{
	if (ns->bdev == NULL) {
		return;
	}

	if (ns->ch) {
		spdk_put_io_channel(ns->ch);
		ns->ch = NULL;
	}
	if (ns->desc) {
		spdk_bdev_close(ns->desc);
		ns->desc = NULL;
	}
	ns->bdev = NULL;
}

int
spdk_nvmf_subsystem_bdev_attach(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_ns *ns;

	for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
		if (spdk_nvmf_ns_bdev_attach(ns)) {
			return -1;
		}
	}

	return 0;
}

void
spdk_nvmf_subsystem_bdev_detach(struct spdk_nvmf_subsystem *subsystem)
{
	struct spdk_nvmf_ns *ns;

	for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
		spdk_nvmf_ns_bdev_detach(ns);
	}
	subsystem->max_nsid = 0;
}
