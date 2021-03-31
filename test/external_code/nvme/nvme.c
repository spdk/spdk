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

#include "spdk/mmio.h"
#include "spdk/nvme_spec.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/stdinc.h"
#include "spdk/util.h"
#include "nvme.h"

typedef void (*nvme_cmd_cb)(void *ctx, const struct spdk_nvme_cpl *cpl);

struct nvme_request {
	/* Command identifier and position within qpair's requests array */
	uint16_t			cid;
	/* NVMe command */
	struct spdk_nvme_cmd		cmd;
	/* Completion callback */
	nvme_cmd_cb			cb_fn;
	/* Completion callback's argument */
	void				*cb_arg;
	TAILQ_ENTRY(nvme_request)	tailq;
};

struct nvme_qpair {
	/* Submission queue */
	struct spdk_nvme_cmd		*cmd;
	/* Completion queue */
	struct spdk_nvme_cpl		*cpl;
	/* Physical address of the submission queue */
	uint64_t			sq_paddr;
	/* Physical address of the completion queue */
	uint64_t			cq_paddr;
	/* Submission queue tail doorbell */
	volatile uint32_t		*sq_tdbl;
	/* Completion queue head doorbell */
	volatile uint32_t		*cq_hdbl;
	/* Submission/completion queues pointers */
	uint16_t			sq_head;
	uint16_t			sq_tail;
	uint16_t			cq_head;
	/* Current phase tag value */
	uint8_t				phase;
	/* NVMe requests queue */
	TAILQ_HEAD(, nvme_request)	free_requests;
	struct nvme_request		*requests;
	/* Size of both queues */
	uint32_t			num_entries;
};

enum nvme_ctrlr_state {
	/* Controller has not been initialized yet */
	NVME_CTRLR_STATE_INIT,
	/* Waiting for CSTS.RDY to transition from 0 to 1 so that CC.EN may be set to 0 */
	NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1,
	/* Waiting for CSTS.RDY to transition from 1 to 0 so that CC.EN may be set to 1 */
	NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0,
	/* Enable the controller by writing CC.EN to 1 */
	NVME_CTRLR_STATE_ENABLE,
	/* Waiting for CSTS.RDY to transition from 0 to 1 after enabling the controller */
	NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1,
	/* Identify Controller command will be sent to then controller */
	NVME_CTRLR_STATE_IDENTIFY,
	/* Waiting for Identify Controller command to be completed */
	NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY,
	/* Controller initialization has completed and the controller is ready */
	NVME_CTRLR_STATE_READY,
	/*  Controller initialization error */
	NVME_CTRLR_STATE_ERROR,
};

struct nvme_ctrlr {
	/* Underlying PCI device */
	struct spdk_pci_device			*pci_device;
	/* Pointer to the MMIO register space */
	volatile struct spdk_nvme_registers	*regs;
	/* Stride in uint32_t units between doorbells */
	uint32_t				doorbell_stride_u32;
	/* Controller's memory page size */
	uint32_t				page_size;
	/* Admin queue pair */
	struct nvme_qpair			*admin_qpair;
	/* Controller's identify data */
	struct spdk_nvme_ctrlr_data		*cdata;
	/* State of the controller */
	enum nvme_ctrlr_state			state;
	TAILQ_ENTRY(nvme_ctrlr)			tailq;
};

static struct spdk_pci_id nvme_pci_driver_id[] = {
	{
		.class_id = SPDK_PCI_CLASS_NVME,
		.vendor_id = SPDK_PCI_ANY_ID,
		.device_id = SPDK_PCI_ANY_ID,
		.subvendor_id = SPDK_PCI_ANY_ID,
		.subdevice_id = SPDK_PCI_ANY_ID,
	},
	{ .vendor_id = 0, /* sentinel */ },
};

SPDK_PCI_DRIVER_REGISTER(nvme_external, nvme_pci_driver_id, SPDK_PCI_DRIVER_NEED_MAPPING);
static TAILQ_HEAD(, nvme_ctrlr) g_nvme_ctrlrs = TAILQ_HEAD_INITIALIZER(g_nvme_ctrlrs);

static struct nvme_ctrlr *
find_ctrlr_by_addr(struct spdk_pci_addr *addr)
{
	struct spdk_pci_addr ctrlr_addr;
	struct nvme_ctrlr *ctrlr;

	TAILQ_FOREACH(ctrlr, &g_nvme_ctrlrs, tailq) {
		ctrlr_addr = spdk_pci_device_get_addr(ctrlr->pci_device);
		if (spdk_pci_addr_compare(addr, &ctrlr_addr) == 0) {
			return ctrlr;
		}
	}

	return NULL;
}

static volatile void *
get_pcie_reg_addr(struct nvme_ctrlr *ctrlr, uint32_t offset)
{
	return (volatile void *)((uintptr_t)ctrlr->regs + offset);
}

static void
get_pcie_reg_4(struct nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value)
{
	assert(offset <= sizeof(struct spdk_nvme_registers) - 4);
	*value = spdk_mmio_read_4(get_pcie_reg_addr(ctrlr, offset));
}

static void
get_pcie_reg_8(struct nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value)
{
	assert(offset <= sizeof(struct spdk_nvme_registers) - 8);
	*value = spdk_mmio_read_8(get_pcie_reg_addr(ctrlr, offset));
}

static void
set_pcie_reg_4(struct nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	assert(offset <= sizeof(struct spdk_nvme_registers) - 4);
	spdk_mmio_write_4(get_pcie_reg_addr(ctrlr, offset), value);
}

static void
set_pcie_reg_8(struct nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	assert(offset <= sizeof(struct spdk_nvme_registers) - 8);
	spdk_mmio_write_8(get_pcie_reg_addr(ctrlr, offset), value);
}

static void
nvme_ctrlr_get_cap(struct nvme_ctrlr *ctrlr, union spdk_nvme_cap_register *cap)
{
	get_pcie_reg_8(ctrlr, offsetof(struct spdk_nvme_registers, cap), &cap->raw);
}

static void
nvme_ctrlr_get_cc(struct nvme_ctrlr *ctrlr, union spdk_nvme_cc_register *cc)
{
	get_pcie_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, cc), &cc->raw);
}

static void
nvme_ctrlr_get_csts(struct nvme_ctrlr *ctrlr, union spdk_nvme_csts_register *csts)
{
	get_pcie_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, csts), &csts->raw);
}

static void
nvme_ctrlr_set_cc(struct nvme_ctrlr *ctrlr, const union spdk_nvme_cc_register *cc)
{
	set_pcie_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, cc.raw), cc->raw);
}

static void
nvme_ctrlr_set_asq(struct nvme_ctrlr *ctrlr, uint64_t value)
{
	set_pcie_reg_8(ctrlr, offsetof(struct spdk_nvme_registers, asq), value);
}

static void
nvme_ctrlr_set_acq(struct nvme_ctrlr *ctrlr, uint64_t value)
{
	set_pcie_reg_8(ctrlr, offsetof(struct spdk_nvme_registers, acq), value);
}

static void
nvme_ctrlr_set_aqa(struct nvme_ctrlr *ctrlr, const union spdk_nvme_aqa_register *aqa)
{
	set_pcie_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, aqa.raw), aqa->raw);
}

static void
free_qpair(struct nvme_qpair *qpair)
{
	spdk_free(qpair->cmd);
	spdk_free(qpair->cpl);
	free(qpair->requests);
	free(qpair);
}

static struct nvme_qpair *
init_qpair(struct nvme_ctrlr *ctrlr, uint16_t id, uint16_t num_entries)
{
	struct nvme_qpair *qpair;
	size_t page_align = sysconf(_SC_PAGESIZE);
	size_t queue_align, queue_len;
	volatile uint32_t *doorbell_base;
	uint16_t i;

	qpair = calloc(1, sizeof(*qpair));
	if (!qpair) {
		SPDK_ERRLOG("Failed to allocate queue pair\n");
		return NULL;
	}

	qpair->phase = 1;
	qpair->num_entries = num_entries;
	queue_len = num_entries * sizeof(struct spdk_nvme_cmd);
	queue_align = spdk_max(spdk_align32pow2(queue_len), page_align);
	qpair->cmd = spdk_zmalloc(queue_len, queue_align, NULL,
				  SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!qpair->cmd) {
		SPDK_ERRLOG("Failed to allocate submission queue buffer\n");
		free_qpair(qpair);
		return NULL;
	}

	queue_len = num_entries * sizeof(struct spdk_nvme_cpl);
	queue_align = spdk_max(spdk_align32pow2(queue_len), page_align);
	qpair->cpl = spdk_zmalloc(queue_len, queue_align, NULL,
				  SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!qpair->cpl) {
		SPDK_ERRLOG("Failed to allocate completion queue buffer\n");
		free_qpair(qpair);
		return NULL;
	}

	qpair->requests = calloc(num_entries - 1, sizeof(*qpair->requests));
	if (!qpair->requests) {
		SPDK_ERRLOG("Failed to allocate NVMe request descriptors\n");
		free_qpair(qpair);
		return NULL;
	}

	TAILQ_INIT(&qpair->free_requests);
	for (i = 0; i < num_entries - 1; ++i) {
		qpair->requests[i].cid = i;
		TAILQ_INSERT_TAIL(&qpair->free_requests, &qpair->requests[i], tailq);
	}

	qpair->sq_paddr = spdk_vtophys(qpair->cmd, NULL);
	qpair->cq_paddr = spdk_vtophys(qpair->cpl, NULL);
	if (qpair->sq_paddr == SPDK_VTOPHYS_ERROR || qpair->cq_paddr == SPDK_VTOPHYS_ERROR) {
		SPDK_ERRLOG("Failed to translate the sq/cq virtual address\n");
		free_qpair(qpair);
		return NULL;
	}

	doorbell_base = (volatile uint32_t *)&ctrlr->regs->doorbell[0];
	qpair->sq_tdbl = doorbell_base + (2 * id + 0) * ctrlr->doorbell_stride_u32;
	qpair->cq_hdbl = doorbell_base + (2 * id + 1) * ctrlr->doorbell_stride_u32;

	return qpair;
}

static int
pcie_enum_cb(void *ctx, struct spdk_pci_device *pci_dev)
{
	struct nvme_ctrlr *ctrlr;
	TAILQ_HEAD(, nvme_ctrlr) *ctrlrs = ctx;
	union spdk_nvme_cap_register cap;
	char addr[32] = {};
	uint64_t phys_addr, size;
	uint16_t cmd_reg;
	void *reg_addr;

	spdk_pci_addr_fmt(addr, sizeof(addr), &pci_dev->addr);

	ctrlr = calloc(1, sizeof(*ctrlr));
	if (!ctrlr) {
		SPDK_ERRLOG("Failed to allocate NVMe controller: %s\n", addr);
		return -1;
	}

	if (spdk_pci_device_claim(pci_dev)) {
		SPDK_ERRLOG("Failed to claim PCI device: %s\n", addr);
		free(ctrlr);
		return -1;
	}

	if (spdk_pci_device_map_bar(pci_dev, 0, &reg_addr, &phys_addr, &size)) {
		SPDK_ERRLOG("Failed to allocate BAR0 for NVMe controller: %s\n", addr);
		spdk_pci_device_unclaim(pci_dev);
		free(ctrlr);
		return -1;
	}

	ctrlr->pci_device = pci_dev;
	ctrlr->regs = (volatile struct spdk_nvme_registers *)reg_addr;

	/* Enable PCI busmaster and disable INTx */
	spdk_pci_device_cfg_read16(pci_dev, &cmd_reg, 4);
	cmd_reg |= 0x404;
	spdk_pci_device_cfg_write16(pci_dev, cmd_reg, 4);

	nvme_ctrlr_get_cap(ctrlr, &cap);
	ctrlr->page_size = 1 << (12 + cap.bits.mpsmin);
	ctrlr->doorbell_stride_u32 = 1 << cap.bits.dstrd;

	ctrlr->cdata = spdk_zmalloc(sizeof(*ctrlr->cdata), ctrlr->page_size, NULL,
				    SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctrlr->cdata) {
		SPDK_ERRLOG("Failed to allocate identify data for NVMe controller: %s\n", addr);
		spdk_pci_device_unclaim(pci_dev);
		free(ctrlr);
		return -1;
	}

	/* Initialize admin queue pair with minimum number of entries (2) */
	ctrlr->admin_qpair = init_qpair(ctrlr, 0, SPDK_NVME_ADMIN_QUEUE_MIN_ENTRIES);
	if (!ctrlr->admin_qpair) {
		SPDK_ERRLOG("Failed to initialize admin queue pair for controller: %s\n", addr);
		spdk_pci_device_unclaim(pci_dev);
		spdk_free(ctrlr->cdata);
		free(ctrlr);
		return -1;
	}

	TAILQ_INSERT_TAIL(ctrlrs, ctrlr, tailq);

	return 0;
}

static struct nvme_request *
allocate_request(struct nvme_qpair *qpair)
{
	struct nvme_request *request;

	if ((qpair->sq_tail + 1) % qpair->num_entries == qpair->sq_head) {
		return NULL;
	}

	request = TAILQ_FIRST(&qpair->free_requests);
	assert(request != NULL);
	TAILQ_REMOVE(&qpair->free_requests, request, tailq);
	memset(&request->cmd, 0, sizeof(request->cmd));

	return request;
}

static void
submit_request(struct nvme_qpair *qpair, struct nvme_request *request)
{
	qpair->cmd[qpair->sq_tail] = request->cmd;

	if (spdk_unlikely(++qpair->sq_tail == qpair->num_entries)) {
		qpair->sq_tail = 0;
	}

	spdk_wmb();
	spdk_mmio_write_4(qpair->sq_tdbl, qpair->sq_tail);
}

static void
identify_ctrlr_done(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct nvme_ctrlr *ctrlr = ctx;

	if (spdk_nvme_cpl_is_error(cpl)) {
		SPDK_ERRLOG("Identify Controller command failed\n");
		ctrlr->state = NVME_CTRLR_STATE_ERROR;
		return;
	}

	ctrlr->state = NVME_CTRLR_STATE_READY;
}

static int
identify_ctrlr(struct nvme_ctrlr *ctrlr)
{
	struct nvme_request *request;
	struct spdk_nvme_cmd *cmd;
	uint64_t prp1;

	/* We're only filling a single PRP entry, so the address needs to be page aligned */
	assert(((uintptr_t)ctrlr->cdata & (ctrlr->page_size - 1)) == 0);
	prp1 = spdk_vtophys(ctrlr->cdata, NULL);
	if (prp1 == SPDK_VTOPHYS_ERROR) {
		return -EFAULT;
	}

	request = allocate_request(ctrlr->admin_qpair);
	if (!request) {
		return -EAGAIN;
	}

	request->cb_fn = identify_ctrlr_done;
	request->cb_arg = ctrlr;

	cmd = &request->cmd;
	cmd->cid = request->cid;
	cmd->opc = SPDK_NVME_OPC_IDENTIFY;
	cmd->dptr.prp.prp1 = prp1;
	cmd->cdw10_bits.identify.cns = SPDK_NVME_IDENTIFY_CTRLR;
	cmd->cdw10_bits.identify.cntid = 0;
	cmd->cdw11_bits.identify.csi = 0;
	cmd->nsid = 0;

	submit_request(ctrlr->admin_qpair, request);

	return 0;
}

static int32_t
process_completions(struct nvme_qpair *qpair)
{
	struct spdk_nvme_cpl *cpl;
	struct nvme_request *request;
	int32_t max_completions, num_completions = 0;

	max_completions = qpair->num_entries - 1;
	while (1) {
		cpl = &qpair->cpl[qpair->cq_head];

		if (cpl->status.p != qpair->phase) {
			break;
		}

		if (spdk_unlikely(++qpair->cq_head == qpair->num_entries)) {
			qpair->cq_head = 0;
			qpair->phase = !qpair->phase;
		}

		qpair->sq_head = cpl->sqhd;
		request = &qpair->requests[cpl->cid];
		request->cb_fn(request->cb_arg, cpl);
		TAILQ_INSERT_TAIL(&qpair->free_requests, request, tailq);

		if (++num_completions == max_completions) {
			break;
		}
	}

	if (num_completions > 0) {
		spdk_mmio_write_4(qpair->cq_hdbl, qpair->cq_head);
	}

	return num_completions;
}

static int
process_ctrlr_init(struct nvme_ctrlr *ctrlr)
{
	union spdk_nvme_cc_register cc;
	union spdk_nvme_csts_register csts;
	union spdk_nvme_aqa_register aqa;
	int rc = 0;

	if (ctrlr->state == NVME_CTRLR_STATE_READY) {
		return 0;
	}

	nvme_ctrlr_get_cc(ctrlr, &cc);
	nvme_ctrlr_get_csts(ctrlr, &csts);

	switch (ctrlr->state) {
	case NVME_CTRLR_STATE_INIT:
		if (cc.bits.en) {
			if (csts.bits.rdy == 0) {
				ctrlr->state = NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1;
				break;
			}

			cc.bits.en = 0;
			nvme_ctrlr_set_cc(ctrlr, &cc);
		}
		ctrlr->state = NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0;
		break;
	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_1:
		if (csts.bits.rdy) {
			cc.bits.en = 0;
			nvme_ctrlr_set_cc(ctrlr, &cc);
			ctrlr->state = NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0;
		}
		break;
	case NVME_CTRLR_STATE_DISABLE_WAIT_FOR_READY_0:
		if (csts.bits.rdy == 0) {
			ctrlr->state = NVME_CTRLR_STATE_ENABLE;
		}
		break;
	case NVME_CTRLR_STATE_ENABLE:
		nvme_ctrlr_set_asq(ctrlr, ctrlr->admin_qpair->sq_paddr);
		nvme_ctrlr_set_acq(ctrlr, ctrlr->admin_qpair->cq_paddr);

		aqa.raw = 0;
		aqa.bits.asqs = ctrlr->admin_qpair->num_entries - 1;
		aqa.bits.acqs = ctrlr->admin_qpair->num_entries - 1;
		nvme_ctrlr_set_aqa(ctrlr, &aqa);

		cc.bits.en = 1;
		nvme_ctrlr_set_cc(ctrlr, &cc);
		ctrlr->state = NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1;
		break;
	case NVME_CTRLR_STATE_ENABLE_WAIT_FOR_READY_1:
		if (csts.bits.rdy) {
			ctrlr->state = NVME_CTRLR_STATE_IDENTIFY;
		}
		break;
	case NVME_CTRLR_STATE_IDENTIFY:
		ctrlr->state = NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY;
		rc = identify_ctrlr(ctrlr);
		break;
	case NVME_CTRLR_STATE_WAIT_FOR_IDENTIFY:
		process_completions(ctrlr->admin_qpair);
		break;
	case NVME_CTRLR_STATE_ERROR:
		rc = -1;
		break;
	default:
		assert(0 && "should never get here");
		return -1;
	}

	return rc;
}

static void
free_ctrlr(struct nvme_ctrlr *ctrlr)
{
	spdk_pci_device_unmap_bar(ctrlr->pci_device, 0, (void *)ctrlr->regs);
	spdk_pci_device_unclaim(ctrlr->pci_device);
	spdk_pci_device_detach(ctrlr->pci_device);
	free_qpair(ctrlr->admin_qpair);
	spdk_free(ctrlr->cdata);
	free(ctrlr);
}

static int
probe_internal(struct spdk_pci_addr *addr, nvme_attach_cb attach_cb, void *cb_ctx)
{
	struct nvme_ctrlr *ctrlr, *tmp;
	TAILQ_HEAD(, nvme_ctrlr) ctrlrs = TAILQ_HEAD_INITIALIZER(ctrlrs);
	int rc;

	if (addr == NULL) {
		rc = spdk_pci_enumerate(spdk_pci_get_driver("nvme_external"),
					pcie_enum_cb, &ctrlrs);
	} else {
		rc = spdk_pci_device_attach(spdk_pci_get_driver("nvme_external"),
					    pcie_enum_cb, &ctrlrs, addr);
	}

	if (rc != 0) {
		SPDK_ERRLOG("Failed to enumerate PCI devices\n");
		while (!TAILQ_EMPTY(&ctrlrs)) {
			ctrlr = TAILQ_FIRST(&ctrlrs);
			TAILQ_REMOVE(&ctrlrs, ctrlr, tailq);
			free_ctrlr(ctrlr);
		}

		return rc;
	}

	while (!TAILQ_EMPTY(&ctrlrs)) {
		TAILQ_FOREACH_SAFE(ctrlr, &ctrlrs, tailq, tmp) {
			rc = process_ctrlr_init(ctrlr);
			if (rc != 0) {
				SPDK_ERRLOG("NVMe controller initialization failed\n");
				TAILQ_REMOVE(&ctrlrs, ctrlr, tailq);
				free_ctrlr(ctrlr);
				continue;
			}

			if (ctrlr->state == NVME_CTRLR_STATE_READY) {
				TAILQ_REMOVE(&ctrlrs, ctrlr, tailq);
				TAILQ_INSERT_TAIL(&g_nvme_ctrlrs, ctrlr, tailq);

				if (attach_cb != NULL) {
					attach_cb(cb_ctx, &ctrlr->pci_device->addr, ctrlr);
				}
			}
		}
	}

	return 0;
}

int
nvme_probe(nvme_attach_cb attach_cb, void *cb_ctx)
{
	return probe_internal(NULL, attach_cb, cb_ctx);
}

struct nvme_ctrlr *
nvme_connect(struct spdk_pci_addr *addr)
{
	int rc;

	rc = probe_internal(addr, NULL, NULL);
	if (rc != 0) {
		return NULL;
	}

	return find_ctrlr_by_addr(addr);
}

void
nvme_detach(struct nvme_ctrlr *ctrlr)
{
	TAILQ_REMOVE(&g_nvme_ctrlrs, ctrlr, tailq);
	free_ctrlr(ctrlr);
}

const struct spdk_nvme_ctrlr_data *
nvme_ctrlr_get_data(struct nvme_ctrlr *ctrlr)
{
	return ctrlr->cdata;
}

SPDK_LOG_REGISTER_COMPONENT(nvme_external)
