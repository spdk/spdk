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
#include "spdk/log.h"
#include "spdk/stdinc.h"
#include "nvme.h"

struct nvme_ctrlr {
	/* Underlying PCI device */
	struct spdk_pci_device			*pci_device;
	/* Pointer to the MMIO register space */
	volatile struct spdk_nvme_registers	*regs;
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

void nvme_ctrlr_get_cap(struct nvme_ctrlr *ctrlr, union spdk_nvme_cap_register *cap);

void
nvme_ctrlr_get_cap(struct nvme_ctrlr *ctrlr, union spdk_nvme_cap_register *cap)
{
	get_pcie_reg_8(ctrlr, offsetof(struct spdk_nvme_registers, cap), &cap->raw);
}

void
nvme_ctrlr_get_cc(struct nvme_ctrlr *ctrlr, union spdk_nvme_cc_register *cc);

void
nvme_ctrlr_get_cc(struct nvme_ctrlr *ctrlr, union spdk_nvme_cc_register *cc)
{
	get_pcie_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, cc), &cc->raw);
}

void nvme_ctrlr_get_csts(struct nvme_ctrlr *ctrlr, union spdk_nvme_csts_register *csts);

void
nvme_ctrlr_get_csts(struct nvme_ctrlr *ctrlr, union spdk_nvme_csts_register *csts)
{
	get_pcie_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, csts), &csts->raw);
}

void nvme_ctrlr_set_cc(struct nvme_ctrlr *ctrlr, const union spdk_nvme_cc_register *cc);

void
nvme_ctrlr_set_cc(struct nvme_ctrlr *ctrlr, const union spdk_nvme_cc_register *cc)
{
	set_pcie_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, cc.raw), cc->raw);
}

void nvme_ctrlr_set_asq(struct nvme_ctrlr *ctrlr, uint64_t value);

void
nvme_ctrlr_set_asq(struct nvme_ctrlr *ctrlr, uint64_t value)
{
	set_pcie_reg_8(ctrlr, offsetof(struct spdk_nvme_registers, asq), value);
}

void nvme_ctrlr_set_acq(struct nvme_ctrlr *ctrlr, uint64_t value);

void
nvme_ctrlr_set_acq(struct nvme_ctrlr *ctrlr, uint64_t value)
{
	set_pcie_reg_8(ctrlr, offsetof(struct spdk_nvme_registers, acq), value);
}

void nvme_ctrlr_set_aqa(struct nvme_ctrlr *ctrlr, const union spdk_nvme_aqa_register *aqa);

void
nvme_ctrlr_set_aqa(struct nvme_ctrlr *ctrlr, const union spdk_nvme_aqa_register *aqa)
{
	set_pcie_reg_4(ctrlr, offsetof(struct spdk_nvme_registers, aqa.raw), aqa->raw);
}

static int
pcie_enum_cb(void *ctx, struct spdk_pci_device *pci_dev)
{
	struct nvme_ctrlr *ctrlr;
	TAILQ_HEAD(, nvme_ctrlr) *ctrlrs = ctx;
	char addr[32] = {};
	uint64_t phys_addr, size;
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
	TAILQ_INSERT_TAIL(ctrlrs, ctrlr, tailq);

	return 0;
}

static void
free_ctrlr(struct nvme_ctrlr *ctrlr)
{
	spdk_pci_device_unmap_bar(ctrlr->pci_device, 0, (void *)ctrlr->regs);
	spdk_pci_device_unclaim(ctrlr->pci_device);
	spdk_pci_device_detach(ctrlr->pci_device);
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

	TAILQ_FOREACH_SAFE(ctrlr, &ctrlrs, tailq, tmp) {
		TAILQ_REMOVE(&ctrlrs, ctrlr, tailq);
		TAILQ_INSERT_TAIL(&g_nvme_ctrlrs, ctrlr, tailq);

		if (attach_cb != NULL) {
			attach_cb(cb_ctx, &ctrlr->pci_device->addr, ctrlr);
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

SPDK_LOG_REGISTER_COMPONENT(nvme_external)
