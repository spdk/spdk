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

#include "vmdpci.h"

#include "spdk/stdinc.h"

/*
 * Container for all VMD adapter probed in the system.
 */
struct vmd_container {
	uint32_t count;
	/* can target specific vmd or all vmd when null */
	struct spdk_pci_addr *vmd_target_addr;
	vmd_adapter vmd[MAX_VMD_SUPPORTED];
} vmd_container;

static struct vmd_container g_vmd_container;

static int
vmd_map_bars(vmd_adapter *vmd, struct spdk_pci_device *dev)
{
	if (!(vmd && dev)) {
		return -1;
	}

	int rc = spdk_pci_device_map_bar(dev, 0, (void **)&vmd->cfg_vaddr,
					 &vmd->cfgbar, &vmd->cfgbar_size);
	if (rc == 0) {
		rc = spdk_pci_device_map_bar(dev, 2, (void **)&vmd->mem_vaddr,
					     &vmd->membar, &vmd->membar_size);
	}

	if (rc == 0) {
		rc = spdk_pci_device_map_bar(dev, 4, (void **)&vmd->msix_vaddr,
					     &vmd->msixbar, &vmd->msixbar_size);
	}

	if (rc == 0) {
		vmd->physical_addr = vmd->membar;
		vmd->current_addr_size = vmd->membar_size;
	}
	return rc;
}

static int
vmd_enumerate_devices(vmd_adapter *vmd)
{
	if (vmd == NULL) {
		return -1;
	}

	vmd->vmd_bus.vmd = vmd;
	vmd->vmd_bus.secondary_bus = vmd->vmd_bus.subordinate_bus = 0;
	vmd->vmd_bus.primary_bus = vmd->vmd_bus.bus_number = 0;
	vmd->vmd_bus.domain = vmd->pci.addr.domain;

	return vmd_scan_pcibus(&vmd->vmd_bus);
}

static int
vmd_enum_cb(void *ctx, struct spdk_pci_device *pci_dev)
{
	uint32_t cmd_reg = 0;
	char bdf[32] = {0};
	struct vmd_container *vmd_c = ctx;
	size_t i;

	if (!(pci_dev && ctx)) {
		return -1;
	}

	/*
	 * If vmd target addr is NULL, then all spdk returned devices are consumed
	 */
	if (vmd_c->vmd_target_addr &&
	    spdk_pci_addr_compare(&pci_dev->addr, vmd_c->vmd_target_addr)) {
		return -1;
	}

	spdk_pci_device_cfg_read32(pci_dev, &cmd_reg, 4);
	cmd_reg |= 0x6;                      /* PCI bus master/memory enable. */
	spdk_pci_device_cfg_write32(pci_dev, cmd_reg, 4);

	spdk_pci_addr_fmt(bdf, sizeof(bdf), &pci_dev->addr);
	SPDK_DEBUGLOG(SPDK_LOG_VMD, "Found a VMD[ %d ] at %s\n", vmd_c->count, bdf);

	/* map vmd bars */
	i = vmd_c->count;
	vmd_c->vmd[i].pci = *pci_dev;
	vmd_c->vmd[i].vmd_index = i;
	vmd_c->vmd[i].domain =
		(pci_dev->addr.bus << 16) | (pci_dev->addr.dev << 8) | pci_dev->addr.func;
	vmd_c->vmd[i].max_pci_bus = PCI_MAX_BUS_NUMBER;
	if (vmd_map_bars(&vmd_c->vmd[i], pci_dev) == -1) {
		return -1;
	}

	SPDK_DEBUGLOG(SPDK_LOG_VMD, "vmd config bar(%p) vaddr(%p) size(%x)\n",
		      (void *)vmd_c->vmd[i].cfgbar, (void *)vmd_c->vmd[i].cfg_vaddr,
		      (uint32_t)vmd_c->vmd[i].cfgbar_size);
	SPDK_DEBUGLOG(SPDK_LOG_VMD, "vmd mem bar(%p) vaddr(%p) size(%x)\n",
		      (void *)vmd_c->vmd[i].membar, (void *)vmd_c->vmd[i].mem_vaddr,
		      (uint32_t)vmd_c->vmd[i].membar_size);
	SPDK_DEBUGLOG(SPDK_LOG_VMD, "vmd msix bar(%p) vaddr(%p) size(%x)\n\n",
		      (void *)vmd_c->vmd[i].msixbar, (void *)vmd_c->vmd[i].msix_vaddr,
		      (uint32_t)vmd_c->vmd[i].msixbar_size);

	vmd_c->count = i + 1;

	vmd_enumerate_devices(&vmd_c->vmd[i]);

	return 0;
}

void
vmd_dev_init(vmd_pci_device *dev)
{
	uint8_t bdf[32];

	/* TODO: Initialize device */
	if (vmd_is_supported_device(dev)) {
		spdk_pci_addr_fmt(bdf, sizeof(bdf), &dev->pci.addr);
		SPDK_DEBUGLOG(SPDK_LOG_VMD, "Initalizing NVMe device at %s\n", bdf);
	}
}

int
spdk_vmd_pci_device_list(struct spdk_pci_addr vmd_addr, struct spdk_pci_device *nvme_list)
{
	int cnt = 0;

	if (!nvme_list) {
		return -1;
	}

	for (int i = 0; i < MAX_VMD_TARGET; ++i) {
		if (spdk_pci_addr_compare(&vmd_addr, &g_vmd_container.vmd[i].pci.addr) == 0) {
			vmd_pci_bus *bus = g_vmd_container.vmd[i].bus_list;
			while (bus != NULL) {
				vmd_pci_device *dev = bus->dev_list;
				while (dev != NULL) {
					nvme_list[cnt++] = dev->pci;
					if (!dev->is_hooked) {
						vmd_dev_init(dev);
						dev->is_hooked = 1;
					}
					dev = dev->next;
				}
				bus = bus->next;
			}
		}
	}

	return cnt;
}

int
spdk_vmd_probe(struct spdk_pci_addr *vmd_bdf)
{
	g_vmd_container.vmd_target_addr = vmd_bdf;
	spdk_pci_enumerate(spdk_pci_vmd_get_driver(), vmd_enum_cb, &g_vmd_container);
	g_vmd_container.vmd_target_addr = NULL;

	return g_vmd_container.count;
}

SPDK_LOG_REGISTER_COMPONENT("vmd", SPDK_LOG_VMD)
