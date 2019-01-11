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
#include "spdk/uuid.h"

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

	/* TODO: don't support for now */
	nsdata->nsrescap.rescap.persist = false;
	nsdata->nsrescap.rescap.write_exclusive = true;
	nsdata->nsrescap.rescap.exclusive_access = true;
	nsdata->nsrescap.rescap.write_exclusive_reg_only = true;
	nsdata->nsrescap.rescap.exclusive_access_reg_only = true;
	nsdata->nsrescap.rescap.write_exclusive_all_reg = true;
	nsdata->nsrescap.rescap.exclusive_access_all_reg = true;
	nsdata->nsrescap.rescap.ignore_existing_key = true;

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

static struct spdk_nvmf_registrant *
nvmf_ctrlr_get_registrant(struct spdk_nvmf_ctrlr *ctrlr)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvmf_registrant *reg, *tmp;

	TAILQ_FOREACH_SAFE(reg, &subsystem->reg_head, link, tmp) {
		if (reg->ctrlr == ctrlr) {
			return reg;
		}
	}

	return NULL;
}

static struct spdk_nvmf_registrant *
nvmf_ctrlr_get_registrant_by_hostid(struct spdk_nvmf_ctrlr *ctrlr,
				    struct spdk_uuid uuid)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvmf_registrant *reg, *tmp;

	TAILQ_FOREACH_SAFE(reg, &subsystem->reg_head, link, tmp) {
		if (spdk_uuid_compare(&reg->hostid, &uuid) == 0) {
			return reg;
		}
	}

	return NULL;
}

static void
spdk_nvmf_ctrlr_reservation_notice(struct spdk_nvmf_ctrlr *ctrlr,
				   struct spdk_nvmf_ns *ns,
				   enum spdk_nvme_reservation_notification_log_page_type type)
{
	struct spdk_nvmf_reservation_log *log;

	switch (type) {
	case SPDK_NVME_RESERVATION_LOG_PAGE_EMPTY:
		return;
	case SPDK_NVME_REGISTRATION_PREEMPTED:
		if (ns->mask & SPDK_NVME_REGISTRATION_PREEMPTED_MASK) {
			return;
		}
		break;
	case SPDK_NVME_RESERVATION_RELEASED:
		if (ns->mask & SPDK_NVME_RESERVATION_RELEASED_MASK) {
			return;
		}
		break;
	case SPDK_NVME_RESERVATION_PREEMPTED:
		if (ns->mask & SPDK_NVME_RESERVATION_PREEMPTED_MASK) {
			return;
		}
		break;
	default:
		return;
	}

	ctrlr->log_page_count++;

	/* Maximum number of queued log pages is 255 */
	if (ctrlr->num_avail_log_pages == 0xff) {
		log = TAILQ_LAST(&ctrlr->log_head, log_page_head);
		log->log.log_page_count = ctrlr->log_page_count;
		return;
	}

	log = calloc(1, sizeof(*log));
	if (!log) {
		return;
	}
	log->log.log_page_count = ctrlr->log_page_count;
	log->log.type = type;
	log->log.num_avail_log_pages = ctrlr->num_avail_log_pages++;
	log->log.nsid = ns->nsid;
	TAILQ_INSERT_TAIL(&ctrlr->log_head, log, link);

	/* Asynchronous event */
	spdk_nvmf_ctrlr_async_event_reservation_notification(ctrlr);

	return;
}

/* current reservation type is all registrants or not */
static inline bool
nvmf_ns_reservation_all_registrants_type(struct spdk_nvmf_ns *ns)
{
	if (ns->holder) {
		return (ns->rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS ||
			ns->rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_ALL_REGS);
	}

	return false;
}

static uint32_t
nvmf_ctrlr_register_registrant_replace_key(struct spdk_nvmf_ctrlr *ctrlr,
		struct spdk_uuid uuid, uint64_t nrkey)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvmf_registrant *reg, *tmp;
	uint32_t count = 0;

	TAILQ_FOREACH_SAFE(reg, &subsystem->reg_head, link, tmp) {
		if (spdk_uuid_compare(&reg->hostid, &uuid) == 0) {
			reg->rkey = nrkey;
			subsystem->gen++;
			count++;
		}
	}

	return count;
}

static int
nvmf_ctrlr_register_registrant(struct spdk_nvmf_ctrlr *ctrlr, uint64_t nrkey)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvmf_registrant *reg;

	reg = calloc(1, sizeof(*reg));
	if (!reg) {
		return -ENOMEM;
	}

	reg->ctrlr = ctrlr;
	reg->rkey = nrkey;
	/* set hostid for the registrant */
	spdk_uuid_copy(&reg->hostid, &ctrlr->hostid);
	TAILQ_INSERT_TAIL(&subsystem->reg_head, reg, link);
	TAILQ_INIT(&reg->ns_head);
	subsystem->regctl++;
	subsystem->gen++;

	return 0;
}

static void
nvme_ctrlr_reservation_notification(struct spdk_nvmf_ns *ns,
				    struct spdk_nvmf_ctrlr *ctrlr,
				    enum spdk_nvme_reservation_notification_log_page_type type)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvmf_registrant *reg, *reg_tmp;
	TAILQ_FOREACH_SAFE(reg, &subsystem->reg_head, link, reg_tmp) {
		if (reg->ctrlr != ctrlr) {
			spdk_nvmf_ctrlr_reservation_notice(reg->ctrlr, ns, type);
		}
	}
}

static void
nvmf_ctrlr_registrant_release_reservation(struct spdk_nvmf_ctrlr *ctrlr,
		struct spdk_nvmf_registrant *reg, bool release_notification)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvmf_ns *ns, *ns_tmp;
	struct spdk_nvmf_registrant *new_reg;

	if (TAILQ_EMPTY(&reg->ns_head)) {
		return;
	}

	TAILQ_FOREACH_SAFE(ns, &reg->ns_head, link, ns_tmp) {
		TAILQ_REMOVE(&reg->ns_head, ns, link);

		new_reg = TAILQ_FIRST(&subsystem->reg_head);
		if (new_reg && nvmf_ns_reservation_all_registrants_type(ns)) {
			/* the next valid registrant is the holder now */
			ns->holder = new_reg;
			TAILQ_INSERT_TAIL(&new_reg->ns_head, ns, link);
		} else {
			/* reservation release notificaton */
			if (release_notification &&
			    (ns->rtype != SPDK_NVME_RESERVE_WRITE_EXCLUSIVE ||
			     ns->rtype != SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS)) {
				nvme_ctrlr_reservation_notification(ns, ctrlr,
								    SPDK_NVME_RESERVATION_RELEASED);
			}
			ns->rtype = 0;
			ns->crkey = 0;
			ns->holder = NULL;
		}
	}
}

static void
nvmf_ctrlr_unregister_registrant(struct spdk_nvmf_ns *ns,
				 struct spdk_nvmf_ctrlr *ctrlr,
				 struct spdk_nvmf_registrant *reg,
				 bool preempt_notification)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;

	/* registration preempted notification */
	if (preempt_notification) {
		nvme_ctrlr_reservation_notification(ns, ctrlr,
						    SPDK_NVME_REGISTRATION_PREEMPTED);
	}
	TAILQ_REMOVE(&subsystem->reg_head, reg, link);
	nvmf_ctrlr_registrant_release_reservation(ctrlr, reg, true);
	free(reg);
	subsystem->regctl--;
	subsystem->gen++;
	return;
}

static uint32_t
nvmf_ctrlr_unregister_registrants_by_key(struct spdk_nvmf_ns *ns,
		struct spdk_nvmf_ctrlr *ctrlr,
		uint64_t rkey)
{
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvmf_registrant *reg, *tmp;
	uint32_t count = 0;

	TAILQ_FOREACH_SAFE(reg, &subsystem->reg_head, link, tmp) {
		if (reg->rkey == rkey) {
			nvmf_ctrlr_unregister_registrant(ns, ctrlr, reg, true);
			count++;
		}
	}
	return count;
}

static uint32_t
nvmf_ctrlr_unregister_all_other_registrants(struct spdk_nvmf_ns *ns,
		struct spdk_nvmf_registrant *reg)
{
	struct spdk_nvmf_ctrlr *ctrlr = reg->ctrlr;
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvmf_registrant *reg_tmp, *reg_tmp2;
	uint32_t count = 0;

	TAILQ_FOREACH_SAFE(reg_tmp, &subsystem->reg_head, link, reg_tmp2) {
		if (reg_tmp != reg) {
			nvmf_ctrlr_unregister_registrant(ns, ctrlr, reg_tmp, true);
			count++;
		}
	}
	return count;
}

/* current registrant is reservation holder or not */
static inline bool
nvmf_ns_registrant_is_holder(struct spdk_nvmf_ns *ns, struct spdk_nvmf_registrant *reg)
{
	if (reg && ns->holder) {
		return (ns->holder == reg ||
			ns->rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS ||
			ns->rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_ALL_REGS);
	}

	return false;
}

static void
nvmf_ns_acquire_reservation(struct spdk_nvmf_ns *ns, uint64_t key,
			    enum spdk_nvme_reservation_type rtype,
			    struct spdk_nvmf_registrant *holder)
{
	ns->rtype = rtype;
	ns->crkey = key;
	ns->holder = holder;
	TAILQ_INSERT_TAIL(&holder->ns_head, ns, link);
}

static uint32_t
nvmf_ctrlr_unregister_all_registrants(struct spdk_nvmf_subsystem *subsystem,
				      struct spdk_nvmf_ctrlr *ctrlr,
				      struct spdk_nvmf_ns *ns)
{
	struct spdk_nvmf_registrant *reg, *tmp;
	uint32_t count = 0;

	TAILQ_FOREACH_SAFE(reg, &subsystem->reg_head, link, tmp) {
		nvmf_ctrlr_unregister_registrant(ns, ctrlr, reg, true);
		count++;
	}

	return count;
}

static void
nvmf_ns_release_reservation(struct spdk_nvmf_ns *ns)
{
	struct spdk_nvmf_registrant *reg;

	reg = ns->holder;
	TAILQ_REMOVE(&reg->ns_head, ns, link);
	ns->rtype = 0;
	ns->crkey = 0;
	ns->holder = NULL;

	/* reservation released notification */
	nvme_ctrlr_reservation_notification(ns, reg->ctrlr,
					    SPDK_NVME_RESERVATION_RELEASED);
}

static int
nvmf_ns_reservation_register(struct spdk_nvmf_request *req,
			     struct spdk_nvmf_ctrlr *ctrlr,
			     struct spdk_nvmf_ns *ns)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	uint8_t rrega, iekey, cptpl;
	struct spdk_nvme_reservation_register_data key;
	struct spdk_nvmf_registrant *reg, *reg_hostid;
	uint8_t status = SPDK_NVME_SC_SUCCESS;
	int rc;

	rrega = cmd->cdw10 & 0x7u;
	iekey = (cmd->cdw10 >> 3) & 0x1u;
	cptpl = (cmd->cdw10 >> 30) & 0x3u;
	memcpy(&key, req->data, sizeof(key));

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "REGISTER: RREGA %u, IEKEY %u, CPTPL %u, "
		      "NRKEY 0x%"PRIx64", NRKEY 0x%"PRIx64"\n",
		      rrega, iekey, cptpl, key.crkey, key.nrkey);

	pthread_mutex_lock(&subsystem->reservation_lock);

	/* TODO: doesn't support for now */
	if (cptpl == SPDK_NVME_RESERVE_PTPL_PERSIST_POWER_LOSS) {
		SPDK_ERRLOG("Can't change persist through power loss for now\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		goto exit;
	}

	/* current session has registrant or not */
	reg = nvmf_ctrlr_get_registrant(ctrlr);
	/* any registrant with same Host Identifier exist ? */
	reg_hostid = nvmf_ctrlr_get_registrant_by_hostid(ctrlr, ctrlr->hostid);

	switch (rrega) {
	case SPDK_NVME_RESERVE_REGISTER_KEY:
		if (reg_hostid) {
			if (reg_hostid->rkey != key.nrkey) {
				SPDK_ERRLOG("The same host already register a "
					    "key with 0x%"PRIx64"\n",
					    reg_hostid->rkey);
				status = SPDK_NVME_SC_RESERVATION_CONFLICT;
				goto exit;
			}
		}
		if (!reg) {
			/* register new controller */
			if (key.nrkey == 0) {
				SPDK_ERRLOG("Can't register zeroed new key\n");
				status = SPDK_NVME_SC_INVALID_FIELD;
				goto exit;
			}
			rc = nvmf_ctrlr_register_registrant(ctrlr, key.nrkey);
			if (rc < 0) {
				status = SPDK_NVME_SC_INVALID_FIELD;
				goto exit;
			}
		} else {
			/* register with same key is not an error */
			if (reg->rkey != key.nrkey) {
				SPDK_ERRLOG("The same host already register a "
					    "key with 0x%"PRIx64"\n",
					    reg->rkey);
				status = SPDK_NVME_SC_RESERVATION_CONFLICT;
				goto exit;
			}
		}
		break;
	case SPDK_NVME_RESERVE_UNREGISTER_KEY:
		if (!reg || (!iekey && reg->rkey != key.crkey)) {
			SPDK_ERRLOG("No registrant or current key doesn't match "
				    "with existing registrant key\n");
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			goto exit;
		}
		nvmf_ctrlr_unregister_registrant(ns, ctrlr, reg, false);
		break;
	case SPDK_NVME_RESERVE_REPLACE_KEY:
		if (!reg || (!iekey && reg->rkey != key.crkey)) {
			SPDK_ERRLOG("No registrant or current key doesn't match "
				    "with existing registrant key\n");
			status = SPDK_NVME_SC_RESERVATION_CONFLICT;
			goto exit;
		}
		if (key.nrkey == 0) {
			SPDK_ERRLOG("Can't register zeroed new key\n");
			status = SPDK_NVME_SC_INVALID_FIELD;
			goto exit;
		}
		nvmf_ctrlr_register_registrant_replace_key(ctrlr, ctrlr->hostid, key.nrkey);
		break;
	default:
		status = SPDK_NVME_SC_INVALID_FIELD;
		goto exit;
	}

exit:
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	req->rsp->nvme_cpl.status.sc = status;
	pthread_mutex_unlock(&subsystem->reservation_lock);
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ns_reservation_acquire(struct spdk_nvmf_request *req,
			    struct spdk_nvmf_ctrlr *ctrlr,
			    struct spdk_nvmf_ns *ns)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	uint8_t racqa, iekey, rtype;
	struct spdk_nvme_reservation_acquire_data key;
	struct spdk_nvmf_registrant *reg;
	bool all_regs = false;
	uint32_t count = 0;
	uint8_t status = SPDK_NVME_SC_SUCCESS;

	racqa = cmd->cdw10 & 0x7u;
	iekey = (cmd->cdw10 >> 3) & 0x1u;
	rtype = (cmd->cdw10 >> 8) & 0xffu;
	memcpy(&key, req->data, sizeof(key));

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "ACQUIIRE: RACQA %u, IEKEY %u, RTYPE %u, "
		      "NRKEY 0x%"PRIx64", PRKEY 0x%"PRIx64"\n",
		      racqa, iekey, rtype, key.crkey, key.prkey);

	pthread_mutex_lock(&subsystem->reservation_lock);

	if (iekey) {
		SPDK_ERRLOG("Ignore existing key field set to 1\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		goto exit;
	}

	reg = nvmf_ctrlr_get_registrant(ctrlr);
	/* must be registrant and CRKEY must match */
	if (!reg || reg->rkey != key.crkey) {
		SPDK_ERRLOG("No registrant or current key doesn't match "
			    "with existing registrant key\n");
		status = SPDK_NVME_SC_RESERVATION_CONFLICT;
		goto exit;
	}

	all_regs = nvmf_ns_reservation_all_registrants_type(ns);

	switch (racqa) {
	case SPDK_NVME_RESERVE_ACQUIRE:
		/* NS already has a valid holder */
		if (ns->holder) {
			if (ns->rtype != rtype ||
			    !nvmf_ns_registrant_is_holder(ns, reg)) {
				SPDK_ERRLOG("Invalid rtype or current "
					    "registrant is not holder\n");
				status = SPDK_NVME_SC_RESERVATION_CONFLICT;
				goto exit;
			}
		} else {
			/* fisrt time for the reservation */
			nvmf_ns_acquire_reservation(ns, key.crkey, rtype, reg);
		}
		break;
	case SPDK_NVME_RESERVE_PREEMPT:
		/* no reservation holder */
		if (!ns->holder) {
			/* unregister with PRKEY */
			nvmf_ctrlr_unregister_registrants_by_key(ns, ctrlr, key.prkey);
			break;
		}
		/* only 1 reservation holder and reservation key is valid */
		if (!all_regs) {
			/* preempt itself */
			if (nvmf_ns_registrant_is_holder(ns, reg) &&
			    ns->crkey == key.prkey) {
				ns->rtype = rtype;
				break;
			}

			if (ns->crkey == key.prkey) {
				nvmf_ctrlr_unregister_registrant(ns, ctrlr, ns->holder, true);
				nvmf_ns_acquire_reservation(ns, key.crkey, rtype, reg);
			} else if (key.prkey != 0) {
				nvmf_ctrlr_unregister_registrants_by_key(ns, ctrlr, key.prkey);
			} else {
				/* PRKEY is zero */
				SPDK_ERRLOG("Current PRKEY is zero\n");
				status = SPDK_NVME_SC_RESERVATION_CONFLICT;
				goto exit;
			}
		} else {
			/* release all other registrants except for the current one */
			if (key.prkey == 0) {
				nvmf_ctrlr_unregister_all_other_registrants(ns, reg);
				assert(ns->holder == reg);
			} else {
				count = nvmf_ctrlr_unregister_registrants_by_key(ns, ctrlr, key.prkey);
				if (count == 0) {
					SPDK_ERRLOG("PRKEY doesn't match any registrant\n");
					status = SPDK_NVME_SC_RESERVATION_CONFLICT;
					goto exit;
				}
			}
		}
		break;
	default:
		break;
	}

exit:
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	req->rsp->nvme_cpl.status.sc = status;
	pthread_mutex_unlock(&subsystem->reservation_lock);
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ns_reservation_release(struct spdk_nvmf_request *req,
			    struct spdk_nvmf_ctrlr *ctrlr,
			    struct spdk_nvmf_ns *ns)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	uint8_t rrela, iekey, rtype;
	struct spdk_nvmf_registrant *reg;
	uint64_t crkey;
	uint8_t status = SPDK_NVME_SC_SUCCESS;

	rrela = cmd->cdw10 & 0x7u;
	iekey = (cmd->cdw10 >> 3) & 0x1u;
	rtype = (cmd->cdw10 >> 8) & 0xffu;
	memcpy(&crkey, req->data, sizeof(crkey));

	SPDK_DEBUGLOG(SPDK_LOG_NVMF, "RELEASE: RRELA %u, IEKEY %u, RTYPE %u, "
		      "CRKEY 0x%"PRIx64"\n",  rrela, iekey, rtype, crkey);

	pthread_mutex_lock(&subsystem->reservation_lock);

	if (iekey) {
		SPDK_ERRLOG("Ignore existing key field set to 1\n");
		status = SPDK_NVME_SC_INVALID_FIELD;
		goto exit;
	}

	reg = nvmf_ctrlr_get_registrant(ctrlr);
	if (!reg || reg->rkey != crkey) {
		SPDK_ERRLOG("No registrant or current key doesn't match "
			    "with existing registrant key\n");
		status = SPDK_NVME_SC_RESERVATION_CONFLICT;
		goto exit;
	}

	switch (rrela) {
	case SPDK_NVME_RESERVE_RELEASE:
		if (!ns->holder) {
			SPDK_DEBUGLOG(SPDK_LOG_NVMF, "RELEASE: no holder\n");
			goto exit;
		}
		if (ns->rtype != rtype) {
			SPDK_ERRLOG("Type doesn't match\n");
			status = SPDK_NVME_SC_INVALID_FIELD;
			goto exit;
		}
		if (!nvmf_ns_registrant_is_holder(ns, reg)) {
			/* not the reservation holder, this isn't an error */
			goto exit;
		}
		nvmf_ns_release_reservation(ns);
		break;
	case SPDK_NVME_RESERVE_CLEAR:
		nvmf_ctrlr_unregister_all_registrants(subsystem, ctrlr, ns);
		break;
	default:
		status = SPDK_NVME_SC_INVALID_FIELD;
		goto exit;
	}

exit:
	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	req->rsp->nvme_cpl.status.sc = status;
	pthread_mutex_unlock(&subsystem->reservation_lock);
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
nvmf_ns_reservation_report(struct spdk_nvmf_request *req,
			   struct spdk_nvmf_ctrlr *ctrlr,
			   struct spdk_nvmf_ns *ns)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvmf_subsystem *subsystem = ctrlr->subsys;
	struct spdk_nvmf_registrant *reg, *tmp;
	struct spdk_nvme_reservation_status_extended_data *status_data;
	struct spdk_nvme_registered_ctrlr_extended_data *ctrlr_data;
	uint8_t *payload;
	uint32_t len, count = 0;

	/* NVMeoF uses Extended Data Structure */
	if ((cmd->cdw11 & 0x00000001u) == 0) {
		SPDK_ERRLOG("NVMeoF uses extended controller data structure, "
			    "please set EDS bit in cdw11 and try again\n");
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	len = sizeof(*status_data) + sizeof(*ctrlr_data) * subsystem->regctl;
	payload = calloc(1, len);
	if (!payload) {
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	status_data = (struct spdk_nvme_reservation_status_extended_data *)payload;
	status_data->data.gen = subsystem->gen;
	status_data->data.rtype = ns->rtype;
	status_data->data.regctl = subsystem->regctl;
	/* TODO: Don't support Persist Through Power Loss State for now */
	status_data->data.ptpls = 0;

	pthread_mutex_lock(&subsystem->reservation_lock);
	TAILQ_FOREACH_SAFE(reg, &subsystem->reg_head, link, tmp) {
		ctrlr_data = (struct spdk_nvme_registered_ctrlr_extended_data *)
			     (payload + sizeof(*status_data) + sizeof(*ctrlr_data) * count);
		ctrlr_data->cntlid = reg->ctrlr->cntlid;
		ctrlr_data->rcsts.status = (ns->holder == reg) ? true : false;
		ctrlr_data->rkey = reg->rkey;
		spdk_uuid_copy((struct spdk_uuid *)ctrlr_data->hostid, &reg->hostid);
		count++;
	}
	pthread_mutex_unlock(&subsystem->reservation_lock);

	memcpy(req->data, payload, spdk_min(len, (cmd->cdw10 + 1) * 4));
	free(payload);

	req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
	req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

static int
spdk_nvmf_reservation_filter(struct spdk_nvmf_request *req,
			     struct spdk_nvmf_ctrlr *ctrlr,
			     struct spdk_nvmf_ns *ns)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_nvmf_registrant *reg;
	uint8_t racqa;

	/* NS must have valid reservation type */
	if (!ns || !ns->holder || !ns->rtype) {
		return 0;
	}

	reg = nvmf_ctrlr_get_registrant(ctrlr);
	if (nvmf_ns_registrant_is_holder(ns, reg)) {
		return 0;
	}

	switch (cmd->opc) {
	case SPDK_NVME_OPC_READ:
	case SPDK_NVME_OPC_COMPARE:
		if (ns->rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS) {
			goto conflict;
		}
		if ((ns->rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_REG_ONLY ||
		     ns->rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_ALL_REGS) && !reg) {
			goto conflict;
		}
		break;
	case SPDK_NVME_OPC_FLUSH:
	case SPDK_NVME_OPC_WRITE:
	case SPDK_NVME_OPC_WRITE_UNCORRECTABLE:
	case SPDK_NVME_OPC_WRITE_ZEROES:
	case SPDK_NVME_OPC_DATASET_MANAGEMENT:
		if (ns->rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE ||
		    ns->rtype == SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS) {
			goto conflict;
		}
		if (!reg) {
			goto conflict;
		}
		break;
	case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
	case SPDK_NVME_OPC_RESERVATION_RELEASE:
		racqa = cmd->cdw10 & 0x7u;
		if (cmd->opc == SPDK_NVME_OPC_RESERVATION_ACQUIRE &&
		    racqa == SPDK_NVME_RESERVE_ACQUIRE) {
			goto conflict;
		}
		if (!reg) {
			goto conflict;
		}
		break;
	default:
		break;
	}

	return 0;

conflict:
	response->status.sct = SPDK_NVME_SCT_GENERIC;
	response->status.sc = SPDK_NVME_SC_RESERVATION_CONFLICT;
	response->status.dnr = 1;
	return -EINVAL;
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
	int rc;

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

	rc = spdk_nvmf_reservation_filter(req, ctrlr, ns);
	if (rc < 0) {
		SPDK_ERRLOG("Reservation conflict for command %u\n", cmd->opc);
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
	case SPDK_NVME_OPC_RESERVATION_REGISTER:
		return nvmf_ns_reservation_register(req, ctrlr, ns);
	case SPDK_NVME_OPC_RESERVATION_REPORT:
		return nvmf_ns_reservation_report(req, ctrlr, ns);
	case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
		return nvmf_ns_reservation_acquire(req, ctrlr, ns);
	case SPDK_NVME_OPC_RESERVATION_RELEASE:
		return nvmf_ns_reservation_release(req, ctrlr, ns);
	default:
		return nvmf_bdev_ctrlr_nvme_passthru_io(bdev, desc, ch, req);
	}
}
