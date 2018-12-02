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

#include "spdk/vmd.h"
#include "spdk/env.h"
#include "spdk/util.h"

#include "spdk_internal/log.h"

/* The VMD device itself */
struct vmd_root {
	/** The PCI object */
	struct spdk_pci_device *pci;

	/** Mapped PCI BAR0 */
	void *bar0_vaddr;
	uint64_t bar0_paddr;
	uint64_t bar0_size;
};

/* A PCI device behind a VMD */
struct vmd_device {
	/** VMD itself */
	struct vmd_root *vmd;
	/** Hooked PCI structure */
	struct spdk_pci_device pci;
};

static int
spdk_vmd_dev_map_bar(struct spdk_pci_device *_dev, uint32_t bar,
		     void **mapped_addr, uint64_t *phys_addr, uint64_t *size)
{
	struct vmd_device *dev = SPDK_CONTAINEROF(_dev, struct vmd_device, pci);

	/* FIXME: return selected area from the parent vmd device */
	// *mapped_addr = (void*)((uintptr_t)dev->vmd->bar0_vaddr + 0x1234);
	// *phys_addr = dev->vmd->bar0_paddr + 0x1234;
	// *size = 0x4321;
	(void)dev;
	return -1;
}

static int
spdk_vmd_dev_unmap_bar(struct spdk_pci_device *_dev, uint32_t bar, void *addr)
{
	struct vmd_device *dev = SPDK_CONTAINEROF(_dev, struct vmd_device, pci);

	/* FIXME */
	(void)dev;
	return -1;
}

static int
spdk_vmd_dev_cfg_read(struct spdk_pci_device *_dev, void *value, uint32_t len,
		      uint32_t offset)
{
	struct vmd_device *dev = SPDK_CONTAINEROF(_dev, struct vmd_device, pci);

	// FIXME: memcpy(value, dev->vmd->bar0_vaddr + 0x5678, len);
	(void)dev;
	return -1;
}

static int
spdk_vmd_dev_cfg_write(struct spdk_pci_device *_dev, void *value, uint32_t len,
		       uint32_t offset)
{
	struct vmd_device *dev = SPDK_CONTAINEROF(_dev, struct vmd_device, pci);

	// FIXME: memcpy(dev->vmd->bar0_vaddr + 0x5678, value, len);
	(void)dev;
	return -1;
}

static void
spdk_vmd_dev_detach(struct spdk_pci_device *_dev)
{
	struct vmd_device *dev = SPDK_CONTAINEROF(_dev, struct vmd_device, pci);

	/* FIXME */
	(void)dev;
}

static int
vmd_enum_cb(void *ctx, struct spdk_pci_device *pci_dev)
{
	int rc, *enum_cnt = ctx;
	struct vmd_device *tmp;
	uint32_t cmd_reg;
	struct vmd_root *vmd_root;
	char bdf[32];

	spdk_pci_addr_fmt(bdf, sizeof(bdf), &pci_dev->addr);
	SPDK_DEBUGLOG(SPDK_LOG_VMD, "Found a VMD at %s\n", bdf);
	/* attach just one VMD for now */
	if ((*enum_cnt)++ > 0) {
		return 1;
	}

	SPDK_DEBUGLOG(SPDK_LOG_VMD, "Attaching a VMD at %s\n", bdf);
	vmd_root = calloc(1, sizeof(*vmd_root));
	assert(vmd_root != NULL);

	vmd_root->pci = pci_dev;

	/* Enable PCI busmaster. */
	spdk_pci_device_cfg_read32(pci_dev, &cmd_reg, 4);
	cmd_reg |= 0x4;
	spdk_pci_device_cfg_write32(pci_dev, cmd_reg, 4);

	/* Map a VMD PCI BAR */
	rc = spdk_pci_device_map_bar(pci_dev, 0,
				     &vmd_root->bar0_vaddr,
				     &vmd_root->bar0_paddr,
				     &vmd_root->bar0_size);
	if (rc != 0) {
		return -1;
	}

	/* alloc a pci object for a device behind a VMD */
	tmp = calloc(1, sizeof(*tmp));
	assert(tmp != NULL);

	tmp->vmd = vmd_root;

	/* assign bdf and pci id */
	tmp->pci.addr.domain = 0x10000;
	tmp->pci.addr.bus = 0x4;
	tmp->pci.addr.dev = 0x2;
	tmp->pci.addr.func = 0x1;
	tmp->pci.id.vendor_id = 0x10;
	tmp->pci.id.device_id = 0x20;

	/* set vmd-specific callbacks */
	tmp->pci.map_bar = spdk_vmd_dev_map_bar;
	tmp->pci.unmap_bar = spdk_vmd_dev_unmap_bar;
	tmp->pci.cfg_read = spdk_vmd_dev_cfg_read;
	tmp->pci.cfg_write = spdk_vmd_dev_cfg_write;
	tmp->pci.detach = spdk_vmd_dev_detach;

	spdk_pci_addr_fmt(bdf, sizeof(bdf), &tmp->pci.addr);
	SPDK_DEBUGLOG(SPDK_LOG_VMD, "Hooked an NVMe device at %s\n", bdf);
	spdk_pci_hook_device(spdk_pci_nvme_get_driver(), &tmp->pci);
	return 0;
}

int
spdk_vmd_probe(void)
{
	int enum_cnt = 0;

	SPDK_DEBUGLOG(SPDK_LOG_VMD, "Enumerating the PCI bus\n");
	return spdk_pci_enumerate(spdk_pci_vmd_get_driver(), vmd_enum_cb, &enum_cnt);
}

SPDK_LOG_REGISTER_COMPONENT("vmd", SPDK_LOG_VMD)
