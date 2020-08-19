/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
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

/* VFIO transport extensions for spdk_nvme_ctrlr */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/vfio_user_pci.h"
#include "nvme_internal.h"
#include "nvme_pcie_internal.h"

#include <linux/vfio.h>

#define NVME_MAX_XFER_SIZE		(131072)
#define NVME_MAX_SGES			(1)

struct nvme_vfio_ctrlr {
	struct nvme_pcie_ctrlr pctrlr;

	volatile uint32_t *doorbell_base;
	int bar0_fd;
	struct vfio_device *dev;
};

static inline uint64_t
vfio_vtophys(const void *vaddr, uint64_t *size)
{
	return (uint64_t)(uintptr_t)vaddr;
}

static inline struct nvme_vfio_ctrlr *
nvme_vfio_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	return SPDK_CONTAINEROF(pctrlr, struct nvme_vfio_ctrlr, pctrlr);
}

static int
nvme_vfio_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	struct nvme_vfio_ctrlr *vctrlr = nvme_vfio_ctrlr(ctrlr);

	assert(offset <= sizeof(struct spdk_nvme_registers) - 4);
	SPDK_DEBUGLOG(nvme_vfio, "ctrlr %s: offset 0x%x, value 0x%x\n", ctrlr->trid.traddr, offset, value);

	return spdk_vfio_user_pci_bar_access(vctrlr->dev, VFIO_PCI_BAR0_REGION_INDEX,
					     offset, 4, &value, true);
}

static int
nvme_vfio_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	struct nvme_vfio_ctrlr *vctrlr = nvme_vfio_ctrlr(ctrlr);

	assert(offset <= sizeof(struct spdk_nvme_registers) - 8);
	SPDK_DEBUGLOG(nvme_vfio, "ctrlr %s: offset 0x%x, value 0x%"PRIx64"\n", ctrlr->trid.traddr, offset,
		      value);

	return spdk_vfio_user_pci_bar_access(vctrlr->dev, VFIO_PCI_BAR0_REGION_INDEX,
					     offset, 8, &value, true);
}

static int
nvme_vfio_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value)
{
	struct nvme_vfio_ctrlr *vctrlr = nvme_vfio_ctrlr(ctrlr);
	int ret;

	assert(offset <= sizeof(struct spdk_nvme_registers) - 4);

	ret = spdk_vfio_user_pci_bar_access(vctrlr->dev, VFIO_PCI_BAR0_REGION_INDEX,
					    offset, 4, value, false);
	if (ret != 0) {
		SPDK_ERRLOG("ctrlr %p, offset %x\n", ctrlr, offset);
		return ret;
	}

	SPDK_DEBUGLOG(nvme_vfio, "ctrlr %s: offset 0x%x, value 0x%x\n", ctrlr->trid.traddr, offset, *value);

	return 0;
}

static int
nvme_vfio_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value)
{
	struct nvme_vfio_ctrlr *vctrlr = nvme_vfio_ctrlr(ctrlr);
	int ret;

	assert(offset <= sizeof(struct spdk_nvme_registers) - 8);

	ret = spdk_vfio_user_pci_bar_access(vctrlr->dev, VFIO_PCI_BAR0_REGION_INDEX,
					    offset, 8, value, false);
	if (ret != 0) {
		SPDK_ERRLOG("ctrlr %p, offset %x\n", ctrlr, offset);
		return ret;
	}

	SPDK_DEBUGLOG(nvme_vfio, "ctrlr %s: offset 0x%x, value 0x%"PRIx64"\n", ctrlr->trid.traddr, offset,
		      *value);

	return 0;
}

static int
nvme_vfio_ctrlr_set_asq(struct spdk_nvme_ctrlr *ctrlr, uint64_t value)
{
	return nvme_vfio_ctrlr_set_reg_8(ctrlr, offsetof(struct spdk_nvme_registers, asq),
					 value);
}

static int
nvme_vfio_ctrlr_set_acq(struct spdk_nvme_ctrlr *ctrlr, uint64_t value)
{
	return nvme_vfio_ctrlr_set_reg_8(ctrlr, offsetof(struct spdk_nvme_registers, acq),
					 value);
}

static int
nvme_vfio_ctrlr_set_aqa(struct spdk_nvme_ctrlr *ctrlr, const union spdk_nvme_aqa_register *aqa)
{
	return nvme_vfio_ctrlr_set_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, aqa.raw),
					 aqa->raw);
}

/* Instead of using path as the bar0 file descriptor, we can also use
 * SPARSE MMAP to get the doorbell mmaped address.
 */
static int
nvme_vfio_setup_bar0(struct nvme_vfio_ctrlr *vctrlr, const char *path)
{
	volatile uint32_t *doorbell;
	int fd;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		SPDK_ERRLOG("Failed to open file %s\n", path);
		return fd;
	}

	doorbell = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x1000);
	if (doorbell == MAP_FAILED) {
		SPDK_ERRLOG("Failed to mmap file %s\n", path);
		close(fd);
		return -EFAULT;
	}

	vctrlr->bar0_fd = fd;
	vctrlr->doorbell_base = doorbell;

	return 0;
}

static void
nvme_vfio_bar0_destruct(struct nvme_vfio_ctrlr *vctrlr)
{
	if (vctrlr->doorbell_base) {
		munmap((void *)vctrlr->doorbell_base, 0x1000);
	}

	close(vctrlr->bar0_fd);
}

static struct spdk_nvme_ctrlr *
	nvme_vfio_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
			  const struct spdk_nvme_ctrlr_opts *opts,
			  void *devhandle)
{
	struct nvme_vfio_ctrlr *vctrlr;
	struct nvme_pcie_ctrlr *pctrlr;
	uint16_t cmd_reg;
	union spdk_nvme_cap_register cap;
	union spdk_nvme_vs_register vs;
	int ret;
	char ctrlr_path[PATH_MAX];
	char ctrlr_bar0[PATH_MAX];

	snprintf(ctrlr_path, sizeof(ctrlr_path), "%s/cntrl", trid->traddr);
	snprintf(ctrlr_bar0, sizeof(ctrlr_bar0), "%s/bar0", trid->traddr);

	ret = access(ctrlr_path, F_OK);
	if (ret != 0) {
		SPDK_ERRLOG("Access path %s failed\n", ctrlr_path);
		return NULL;
	}

	ret = access(ctrlr_bar0, F_OK);
	if (ret != 0) {
		SPDK_ERRLOG("Access path %s failed\n", ctrlr_bar0);
		return NULL;
	}

	vctrlr = calloc(1, sizeof(*vctrlr));
	if (!vctrlr) {
		return NULL;
	}

	ret = nvme_vfio_setup_bar0(vctrlr, ctrlr_bar0);
	if (ret != 0) {
		free(vctrlr);
		return NULL;
	}

	vctrlr->dev = spdk_vfio_user_setup(ctrlr_path);
	if (!vctrlr->dev) {
		SPDK_ERRLOG("Error to setup vfio device\n");
		nvme_vfio_bar0_destruct(vctrlr);
		free(vctrlr);
		return NULL;
	}

	pctrlr = &vctrlr->pctrlr;
	pctrlr->doorbell_base = vctrlr->doorbell_base;
	pctrlr->ctrlr.is_removed = false;
	pctrlr->ctrlr.opts = *opts;
	pctrlr->ctrlr.trid = *trid;
	pctrlr->ctrlr.opts.use_cmb_sqs = false;

	ret = nvme_ctrlr_construct(&pctrlr->ctrlr);
	if (ret != 0) {
		goto exit;
	}

	/* Enable PCI busmaster and disable INTx */
	ret = spdk_vfio_user_pci_bar_access(vctrlr->dev, VFIO_PCI_CONFIG_REGION_INDEX, 4, 2,
					    &cmd_reg, false);
	if (ret != 0) {
		SPDK_ERRLOG("Read PCI CMD REG failed\n");
		goto exit;
	}
	cmd_reg |= 0x404;
	ret = spdk_vfio_user_pci_bar_access(vctrlr->dev, VFIO_PCI_CONFIG_REGION_INDEX, 4, 2,
					    &cmd_reg, true);
	if (ret != 0) {
		SPDK_ERRLOG("Write PCI CMD REG failed\n");
		goto exit;
	}

	if (nvme_ctrlr_get_cap(&pctrlr->ctrlr, &cap)) {
		SPDK_ERRLOG("get_cap() failed\n");
		goto exit;
	}

	if (nvme_ctrlr_get_vs(&pctrlr->ctrlr, &vs)) {
		SPDK_ERRLOG("get_vs() failed\n");
		goto exit;
	}

	nvme_ctrlr_init_cap(&pctrlr->ctrlr, &cap, &vs);
	/* Doorbell stride is 2 ^ (dstrd + 2),
	 * but we want multiples of 4, so drop the + 2 */
	pctrlr->doorbell_stride_u32 = 1 << cap.bits.dstrd;

	ret = nvme_pcie_ctrlr_construct_admin_qpair(&pctrlr->ctrlr, pctrlr->ctrlr.opts.admin_queue_size);
	if (ret != 0) {
		nvme_ctrlr_destruct(&pctrlr->ctrlr);
		goto exit;
	}

	/* Construct the primary process properties */
	ret = nvme_ctrlr_add_process(&pctrlr->ctrlr, 0);
	if (ret != 0) {
		nvme_ctrlr_destruct(&pctrlr->ctrlr);
		goto exit;
	}

	return &pctrlr->ctrlr;

exit:
	nvme_vfio_bar0_destruct(vctrlr);
	spdk_vfio_user_release(vctrlr->dev);
	free(vctrlr);
	return NULL;
}

static int
nvme_vfio_ctrlr_scan(struct spdk_nvme_probe_ctx *probe_ctx,
		     bool direct_connect)
{
	int ret;

	if (probe_ctx->trid.trtype != SPDK_NVME_TRANSPORT_VFIOUSER) {
		SPDK_ERRLOG("Can only use SPDK_NVME_TRANSPORT_VFIOUSER");
		return -EINVAL;
	}

	ret = access(probe_ctx->trid.traddr, F_OK);
	if (ret != 0) {
		SPDK_ERRLOG("Error to access file %s\n", probe_ctx->trid.traddr);
		return ret;
	}
	SPDK_NOTICELOG("Scan controller : %s\n", probe_ctx->trid.traddr);

	return nvme_ctrlr_probe(&probe_ctx->trid, probe_ctx, NULL);
}

static int
nvme_vfio_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_qpair *vadminq = nvme_pcie_qpair(ctrlr->adminq);
	union spdk_nvme_aqa_register aqa;

	if (nvme_vfio_ctrlr_set_asq(ctrlr, vadminq->cmd_bus_addr)) {
		SPDK_ERRLOG("set_asq() failed\n");
		return -EIO;
	}

	if (nvme_vfio_ctrlr_set_acq(ctrlr, vadminq->cpl_bus_addr)) {
		SPDK_ERRLOG("set_acq() failed\n");
		return -EIO;
	}

	aqa.raw = 0;
	/* acqs and asqs are 0-based. */
	aqa.bits.acqs = nvme_pcie_qpair(ctrlr->adminq)->num_entries - 1;
	aqa.bits.asqs = nvme_pcie_qpair(ctrlr->adminq)->num_entries - 1;

	if (nvme_vfio_ctrlr_set_aqa(ctrlr, &aqa)) {
		SPDK_ERRLOG("set_aqa() failed\n");
		return -EIO;
	}

	return 0;
}

static int
nvme_vfio_qpair_destroy(struct spdk_nvme_qpair *qpair);

static int
nvme_vfio_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_vfio_ctrlr *vctrlr = nvme_vfio_ctrlr(ctrlr);

	if (ctrlr->adminq) {
		nvme_vfio_qpair_destroy(ctrlr->adminq);
	}

	nvme_ctrlr_destruct_finish(ctrlr);

	nvme_ctrlr_free_processes(ctrlr);

	nvme_vfio_bar0_destruct(vctrlr);
	spdk_vfio_user_release(vctrlr->dev);
	free(vctrlr);

	return 0;
}

static  uint32_t
nvme_vfio_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	return NVME_MAX_XFER_SIZE;
}

static uint16_t
nvme_vfio_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
	return NVME_MAX_SGES;
}

static struct spdk_nvme_qpair *
nvme_vfio_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				const struct spdk_nvme_io_qpair_opts *opts)
{
	struct nvme_pcie_qpair *vqpair;
	struct spdk_nvme_qpair *qpair;
	int rc;

	assert(ctrlr != NULL);

	vqpair = spdk_zmalloc(sizeof(*vqpair), 64, NULL,
			      SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
	if (vqpair == NULL) {
		return NULL;
	}

	vqpair->num_entries = opts->io_queue_size;
	vqpair->flags.delay_cmd_submit = opts->delay_cmd_submit;

	qpair = &vqpair->qpair;

	rc = nvme_qpair_init(qpair, qid, ctrlr, opts->qprio, opts->io_queue_requests);
	if (rc != 0) {
		nvme_vfio_qpair_destroy(qpair);
		return NULL;
	}

	rc = nvme_pcie_qpair_construct(qpair, opts);

	if (rc != 0) {
		nvme_vfio_qpair_destroy(qpair);
		return NULL;
	}

	return qpair;
}

static void
nvme_vfio_qpair_abort_trackers(struct spdk_nvme_qpair *qpair, uint32_t dnr);

static int
nvme_vfio_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_completion_poll_status *status;
	int rc;

	assert(ctrlr != NULL);

	if (ctrlr->is_removed) {
		goto free;
	}

	status = calloc(1, sizeof(*status));
	if (!status) {
		SPDK_ERRLOG("Failed to allocate status tracker\n");
		return -ENOMEM;
	}

	/* Delete the I/O submission queue */
	rc = nvme_pcie_ctrlr_cmd_delete_io_sq(ctrlr, qpair, nvme_completion_poll_cb, status);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to send request to delete_io_sq with rc=%d\n", rc);
		free(status);
		return rc;
	}
	if (nvme_wait_for_completion(ctrlr->adminq, status)) {
		if (!status->timed_out) {
			free(status);
		}
		return -1;
	}

	memset(status, 0, sizeof(*status));
	/* Delete the completion queue */
	rc = nvme_pcie_ctrlr_cmd_delete_io_cq(ctrlr, qpair, nvme_completion_poll_cb, status);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to send request to delete_io_cq with rc=%d\n", rc);
		free(status);
		return rc;
	}
	if (nvme_wait_for_completion(ctrlr->adminq, status)) {
		if (!status->timed_out) {
			free(status);
		}
		return -1;
	}
	free(status);

free:
	if (qpair->no_deletion_notification_needed == 0) {
		/* Abort the rest of the I/O */
		nvme_vfio_qpair_abort_trackers(qpair, 1);
	}

	nvme_vfio_qpair_destroy(qpair);
	return 0;
}

static inline void
nvme_vfio_qpair_ring_sq_doorbell(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair	*vqpair = nvme_pcie_qpair(qpair);

	if (qpair->first_fused_submitted) {
		/* This is first cmd of two fused commands - don't ring doorbell */
		qpair->first_fused_submitted = 0;
		return;
	}

	spdk_wmb();
	spdk_mmio_write_4(vqpair->sq_tdbl, vqpair->sq_tail);
}

static inline void
nvme_vfio_qpair_ring_cq_doorbell(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair	*vqpair = nvme_pcie_qpair(qpair);

	spdk_mmio_write_4(vqpair->cq_hdbl, vqpair->cq_head);
}

static void
nvme_vfio_qpair_submit_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr)
{
	struct nvme_request	*req;
	struct nvme_pcie_qpair	*vqpair = nvme_pcie_qpair(qpair);

	req = tr->req;
	assert(req != NULL);

	if (req->cmd.fuse == SPDK_NVME_IO_FLAGS_FUSE_FIRST) {
		/* This is first cmd of two fused commands - don't ring doorbell */
		qpair->first_fused_submitted = 1;
	}

	vqpair->cmd[vqpair->sq_tail] = req->cmd;

	if (spdk_unlikely(++vqpair->sq_tail == vqpair->num_entries)) {
		vqpair->sq_tail = 0;
	}

	if (spdk_unlikely(vqpair->sq_tail == vqpair->sq_head)) {
		SPDK_ERRLOG("sq_tail is passing sq_head!\n");
	}

	nvme_vfio_qpair_ring_sq_doorbell(qpair);
}

static void
nvme_vfio_qpair_complete_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr,
				 struct spdk_nvme_cpl *cpl, bool print_on_error)
{
	struct nvme_pcie_qpair		*vqpair = nvme_pcie_qpair(qpair);
	struct nvme_request		*req;
	bool				retry, error;
	bool				req_from_current_proc = true;

	req = tr->req;

	assert(req != NULL);

	error = spdk_nvme_cpl_is_error(cpl);
	retry = error && nvme_completion_is_retry(cpl) &&
		req->retries < vqpair->retry_count;

	if (error && print_on_error && !qpair->ctrlr->opts.disable_error_logging) {
		spdk_nvme_qpair_print_command(qpair, &req->cmd);
		spdk_nvme_qpair_print_completion(qpair, cpl);
	}

	assert(cpl->cid == req->cmd.cid);

	if (retry) {
		req->retries++;
		nvme_vfio_qpair_submit_tracker(qpair, tr);
	} else {
		/* Only check admin requests from different processes. */
		if (nvme_qpair_is_admin_queue(qpair) && req->pid != getpid()) {
			req_from_current_proc = false;
			nvme_pcie_qpair_insert_pending_admin_request(qpair, req, cpl);
		} else {
			nvme_complete_request(tr->cb_fn, tr->cb_arg, qpair, req, cpl);
		}

		if (req_from_current_proc == true) {
			nvme_qpair_free_request(qpair, req);
		}

		tr->req = NULL;

		TAILQ_REMOVE(&vqpair->outstanding_tr, tr, tq_list);
		TAILQ_INSERT_HEAD(&vqpair->free_tr, tr, tq_list);
	}
}

static void
nvme_vfio_qpair_manual_complete_tracker(struct spdk_nvme_qpair *qpair,
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
	nvme_vfio_qpair_complete_tracker(qpair, tr, &cpl, print_on_error);
}

static void
nvme_vfio_qpair_abort_trackers(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker *tr, *temp, *last;

	last = TAILQ_LAST(&pqpair->outstanding_tr, nvme_outstanding_tr_head);

	/* Abort previously submitted (outstanding) trs */
	TAILQ_FOREACH_SAFE(tr, &pqpair->outstanding_tr, tq_list, temp) {
		if (!qpair->ctrlr->opts.disable_error_logging) {
			SPDK_ERRLOG("aborting outstanding command\n");
		}
		nvme_vfio_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC,
							SPDK_NVME_SC_ABORTED_BY_REQUEST, dnr, true);

		if (tr == last) {
			break;
		}
	}
}

static void
nvme_vfio_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	nvme_vfio_qpair_abort_trackers(qpair, dnr);
}

static void
nvme_vfio_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair	*vqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker	*tr;

	tr = TAILQ_FIRST(&vqpair->outstanding_tr);
	while (tr != NULL) {
		assert(tr->req != NULL);
		if (tr->req->cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
			nvme_vfio_qpair_manual_complete_tracker(qpair, tr,
								SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_ABORTED_SQ_DELETION, 0,
								false);
			tr = TAILQ_FIRST(&vqpair->outstanding_tr);
		} else {
			tr = TAILQ_NEXT(tr, tq_list);
		}
	}
}

static void
nvme_vfio_admin_qpair_destroy(struct spdk_nvme_qpair *qpair)
{
	nvme_vfio_admin_qpair_abort_aers(qpair);
}

static int
nvme_vfio_qpair_destroy(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *vqpair = nvme_pcie_qpair(qpair);

	if (nvme_qpair_is_admin_queue(qpair)) {
		nvme_vfio_admin_qpair_destroy(qpair);
	}

	spdk_free(vqpair->cmd);
	spdk_free(vqpair->cpl);

	if (vqpair->tr) {
		spdk_free(vqpair->tr);
	}

	nvme_qpair_deinit(qpair);

	spdk_free(vqpair);

	return 0;
}

static inline int
nvme_vfio_prp_list_append(struct nvme_tracker *tr, uint32_t *prp_index, void *virt_addr, size_t len,
			  uint32_t page_size)
{
	struct spdk_nvme_cmd *cmd = &tr->req->cmd;
	uintptr_t page_mask = page_size - 1;
	uint64_t phys_addr;
	uint32_t i;

	SPDK_DEBUGLOG(nvme_vfio, "prp_index:%u virt_addr:%p len:%u\n",
		      *prp_index, virt_addr, (uint32_t)len);

	if (spdk_unlikely(((uintptr_t)virt_addr & 3) != 0)) {
		SPDK_ERRLOG("virt_addr %p not dword aligned\n", virt_addr);
		return -EFAULT;
	}

	i = *prp_index;
	while (len) {
		uint32_t seg_len;

		/*
		 * prp_index 0 is stored in prp1, and the rest are stored in the prp[] array,
		 * so prp_index == count is valid.
		 */
		if (spdk_unlikely(i > SPDK_COUNTOF(tr->u.prp))) {
			SPDK_ERRLOG("out of PRP entries\n");
			return -EFAULT;
		}

		phys_addr = vfio_vtophys(virt_addr, NULL);

		if (i == 0) {
			SPDK_DEBUGLOG(nvme_vfio, "prp1 = %p\n", (void *)phys_addr);
			cmd->dptr.prp.prp1 = phys_addr;
			seg_len = page_size - ((uintptr_t)virt_addr & page_mask);
		} else {
			if ((phys_addr & page_mask) != 0) {
				SPDK_ERRLOG("PRP %u not page aligned (%p)\n", i, virt_addr);
				return -EFAULT;
			}

			SPDK_DEBUGLOG(nvme_vfio, "prp[%u] = %p\n", i - 1, (void *)phys_addr);
			tr->u.prp[i - 1] = phys_addr;
			seg_len = page_size;
		}

		seg_len = spdk_min(seg_len, len);
		virt_addr += seg_len;
		len -= seg_len;
		i++;
	}

	cmd->psdt = SPDK_NVME_PSDT_PRP;
	if (i <= 1) {
		cmd->dptr.prp.prp2 = 0;
	} else if (i == 2) {
		cmd->dptr.prp.prp2 = tr->u.prp[0];
		SPDK_DEBUGLOG(nvme_vfio, "prp2 = %p\n", (void *)cmd->dptr.prp.prp2);
	} else {
		cmd->dptr.prp.prp2 = tr->prp_sgl_bus_addr;
		SPDK_DEBUGLOG(nvme_vfio, "prp2 = %p (PRP list)\n", (void *)cmd->dptr.prp.prp2);
	}

	*prp_index = i;
	return 0;
}

static int
nvme_vfio_qpair_build_contig_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
				     struct nvme_tracker *tr, bool dword_aligned)
{
	uint32_t prp_index = 0;
	int rc;

	rc = nvme_vfio_prp_list_append(tr, &prp_index, req->payload.contig_or_cb_arg + req->payload_offset,
				       req->payload_size, qpair->ctrlr->page_size);
	if (rc) {
		nvme_vfio_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC,
							SPDK_NVME_SC_INVALID_FIELD,
							1 /* do not retry */, true);
	}

	return rc;
}

static int
nvme_vfio_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	struct nvme_tracker	*tr;
	int			rc = 0;
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	struct nvme_pcie_qpair	*vqpair = nvme_pcie_qpair(qpair);

	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
		nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	}

	tr = TAILQ_FIRST(&vqpair->free_tr);

	if (tr == NULL) {
		/* Inform the upper layer to try again later. */
		rc = -EAGAIN;
		goto exit;
	}

	TAILQ_REMOVE(&vqpair->free_tr, tr, tq_list); /* remove tr from free_tr */
	TAILQ_INSERT_TAIL(&vqpair->outstanding_tr, tr, tq_list);
	tr->req = req;
	tr->cb_fn = req->cb_fn;
	tr->cb_arg = req->cb_arg;
	req->cmd.cid = tr->cid;

	if (req->payload_size != 0) {
		rc = nvme_vfio_qpair_build_contig_request(qpair, req, tr, true);
		if (rc) {
			goto exit;
		}
	}

	nvme_vfio_qpair_submit_tracker(qpair, tr);

exit:
	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	}

	return rc;
}

static int32_t
nvme_vfio_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_pcie_qpair	*vqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker	*tr;
	struct spdk_nvme_cpl	*cpl, *next_cpl;
	uint32_t		 num_completions = 0;
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	uint16_t		 next_cq_head;
	uint8_t			 next_phase;
	bool			 next_is_valid = false;

	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
		nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	}

	if (max_completions == 0 || max_completions > vqpair->max_completions_cap) {
		/*
		 * max_completions == 0 means unlimited, but complete at most
		 * max_completions_cap batch of I/O at a time so that the completion
		 * queue doorbells don't wrap around.
		 */
		max_completions = vqpair->max_completions_cap;
	}

	while (1) {
		cpl = &vqpair->cpl[vqpair->cq_head];

		if (!next_is_valid && cpl->status.p != vqpair->flags.phase) {
			break;
		}

		if (spdk_likely(vqpair->cq_head + 1 != vqpair->num_entries)) {
			next_cq_head = vqpair->cq_head + 1;
			next_phase = vqpair->flags.phase;
		} else {
			next_cq_head = 0;
			next_phase = !vqpair->flags.phase;
		}
		next_cpl = &vqpair->cpl[next_cq_head];
		next_is_valid = (next_cpl->status.p == next_phase);
		if (next_is_valid) {
			__builtin_prefetch(&vqpair->tr[next_cpl->cid]);
		}

		if (spdk_unlikely(++vqpair->cq_head == vqpair->num_entries)) {
			vqpair->cq_head = 0;
			vqpair->flags.phase = !vqpair->flags.phase;
		}

		tr = &vqpair->tr[cpl->cid];
		/* Prefetch the req's STAILQ_ENTRY since we'll need to access it
		 * as part of putting the req back on the qpair's free list.
		 */
		__builtin_prefetch(&tr->req->stailq);
		vqpair->sq_head = cpl->sqhd;

		if (tr->req) {
			nvme_vfio_qpair_complete_tracker(qpair, tr, cpl, true);
		} else {
			SPDK_ERRLOG("cpl does not map to outstanding cmd\n");
			spdk_nvme_qpair_print_completion(qpair, cpl);
			assert(0);
		}

		if (++num_completions == max_completions) {
			break;
		}
	}

	if (num_completions > 0) {
		nvme_vfio_qpair_ring_cq_doorbell(qpair);
	}

	if (vqpair->flags.delay_cmd_submit) {
		if (vqpair->last_sq_tail != vqpair->sq_tail) {
			nvme_vfio_qpair_ring_sq_doorbell(qpair);
			vqpair->last_sq_tail = vqpair->sq_tail;
		}
	}

	/* Before returning, complete any pending admin request. */
	if (spdk_unlikely(nvme_qpair_is_admin_queue(qpair))) {
		nvme_pcie_qpair_complete_pending_admin_request(qpair);

		nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);
	}

	return num_completions;
}

const struct spdk_nvme_transport_ops vfio_ops = {
	.name = "VFIOUSER",
	.type = SPDK_NVME_TRANSPORT_VFIOUSER,
	.ctrlr_construct = nvme_vfio_ctrlr_construct,
	.ctrlr_scan = nvme_vfio_ctrlr_scan,
	.ctrlr_destruct = nvme_vfio_ctrlr_destruct,
	.ctrlr_enable = nvme_vfio_ctrlr_enable,

	.ctrlr_set_reg_4 = nvme_vfio_ctrlr_set_reg_4,
	.ctrlr_set_reg_8 = nvme_vfio_ctrlr_set_reg_8,
	.ctrlr_get_reg_4 = nvme_vfio_ctrlr_get_reg_4,
	.ctrlr_get_reg_8 = nvme_vfio_ctrlr_get_reg_8,

	.ctrlr_get_max_xfer_size = nvme_vfio_ctrlr_get_max_xfer_size,
	.ctrlr_get_max_sges = nvme_vfio_ctrlr_get_max_sges,

	.ctrlr_create_io_qpair = nvme_vfio_ctrlr_create_io_qpair,
	.ctrlr_delete_io_qpair = nvme_vfio_ctrlr_delete_io_qpair,
	.ctrlr_connect_qpair = nvme_pcie_ctrlr_connect_qpair,
	.ctrlr_disconnect_qpair = nvme_pcie_ctrlr_disconnect_qpair,
	.admin_qpair_abort_aers = nvme_vfio_admin_qpair_abort_aers,

	.qpair_reset = nvme_pcie_qpair_reset,
	.qpair_abort_reqs = nvme_vfio_qpair_abort_reqs,
	.qpair_submit_request = nvme_vfio_qpair_submit_request,
	.qpair_process_completions = nvme_vfio_qpair_process_completions,

	.poll_group_create = nvme_pcie_poll_group_create,
	.poll_group_connect_qpair = nvme_pcie_poll_group_connect_qpair,
	.poll_group_disconnect_qpair = nvme_pcie_poll_group_disconnect_qpair,
	.poll_group_add = nvme_pcie_poll_group_add,
	.poll_group_remove = nvme_pcie_poll_group_remove,
	.poll_group_process_completions = nvme_pcie_poll_group_process_completions,
	.poll_group_destroy = nvme_pcie_poll_group_destroy,
};

SPDK_NVME_TRANSPORT_REGISTER(vfio, &vfio_ops);

SPDK_LOG_REGISTER_COMPONENT(nvme_vfio)
