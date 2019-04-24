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

vmd_container g_vmd_container = {0, 0};

static int vmd_enumerate_devices(vmd_adapter *vmd);

/* *********************************************************************************** */
/* spdk interface functions */
static int
spdk_vmd_dev_map_bar(struct spdk_pci_device *pci_dev, uint32_t bar,
		     void **mapped_addr, uint64_t *phys_addr, uint64_t *size)
{
	if (!(pci_dev && mapped_addr && phys_addr && size)) {
		return -1;
	}
	vmd_pci_device *dev = SPDK_CONTAINEROF(pci_dev, vmd_pci_device, pci);
	*size = dev->bar[bar].size;
	*phys_addr = dev->bar[bar].start;
	*mapped_addr = (void *)dev->bar[bar].vaddr;

	return 0;
}

/*
 * Maps vmd owned device bar
 */
static int
spdk_vmd_dev_unmap_bar(struct spdk_pci_device *_dev, uint32_t bar, void *addr)
{
	return 0;
}

/*
 * Reads vmd owned device config space. Translates to a MMIO read: Use dword access
 */
static int
spdk_vmd_dev_cfg_read(struct spdk_pci_device *_dev, void *value, uint32_t len,
		      uint32_t offset)
{
	if (!(_dev && value)) {
		return -1;
	}

	vmd_pci_device *dev = SPDK_CONTAINEROF(_dev, vmd_pci_device, pci);
	volatile uint8_t *src = (volatile uint8_t *)dev->header;
	uint8_t *dst = (uint8_t *)value;
	if (len + offset > PCI_MAX_CFG_SIZE) {
		return -1;
	}
	for (uint32_t i = 0; i < len; ++i) {
		dst[i] = src[offset + i];
	}

	return 0;
}
/*
 * Writes VMD owned device config space: MMIO write therough VMD cfg bar
 * translates to MMIO access: use dword access
 */
static int
spdk_vmd_dev_cfg_write(struct spdk_pci_device *_dev,  void *value,
		       uint32_t len, uint32_t offset)
{
	if (!(_dev && value)) {
		return -1;
	}

	vmd_pci_device *dev = SPDK_CONTAINEROF(_dev, vmd_pci_device, pci);
	volatile uint8_t *dst = (volatile uint8_t *)dev->header;
	uint8_t *src = (uint8_t *)value;
	if ((len + offset) > PCI_MAX_CFG_SIZE) {
		return -1;
	}
	for (uint32_t i = 0; i < len; ++i) {
		dst[offset + i] = src[i];
	}

	return 0;
}

static void
spdk_vmd_dev_detach(struct spdk_pci_device *_dev)
{
	return;
}


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


/*
 ****************************************************************
 * @brief vmd_enumerate_devices - Enumerates the vmd adapter's pcie domain
 * @input vmd - pointer to the vmd adapter to be enumerated.
 *
 * @return - The number of pci devices(type 1, type 0) found during enumeration.
 */
int
vmd_enumerate_devices(vmd_adapter *vmd)
{
	if (vmd == NULL) {
		return -1;
	}

	printf("%s\n", __func__);
	vmd->vmd_bus.vmd = vmd;
	vmd->vmd_bus.secondary_bus = vmd->vmd_bus.subordinate_bus = 0;
	vmd->vmd_bus.primary_bus = vmd->vmd_bus.bus_number = 0;
	vmd->vmd_bus.domain = vmd->pci.addr.domain;

	return (vmd_scan_pcibus(&vmd->vmd_bus));
}


static bool
is_equal_addr(struct spdk_pci_addr *addr, struct spdk_pci_addr *cmp)
{
	if (!(addr && cmp)) {
		return false;
	}
	return (addr->domain == cmp->domain &&
		addr->bus == cmp->bus &&
		addr->dev == cmp->dev && addr->func == cmp->func) ?
	       true : false;
}


/*
 * *****************************************************************************
 * @brief - vmd_enum_cb: Callback for from spdk enumeration for each VMD
 * device under spdk control.
 * Args:
 *      ctx - callback context provided by caller to spdk_pci_enumerate
 *      dev - vmd device found.
 * ****************************************************************************
 */
static int
vmd_enum_cb(void *ctx, struct spdk_pci_device *pci_dev)
{
	uint32_t cmd_reg = 0;
	char bdf[32] = {0};
	vmd_container *vmd_c = (vmd_container *) ctx;

	if (!(pci_dev && ctx)) {
		return -1;
	}

	/*
	 * if vmd target addr is NULL, then all spdk returned devices are consumed
	 */
	if (vmd_c->vmd_target_addr &&
	    !is_equal_addr(&pci_dev->addr, vmd_c->vmd_target_addr)) {
		return -1;
	}
	spdk_pci_device_cfg_read32(pci_dev, &cmd_reg, 4);
	cmd_reg |= 0x6;                      /* PCI bus master/memory enable. */
	spdk_pci_device_cfg_write32(pci_dev, cmd_reg, 4);

	spdk_pci_addr_fmt(bdf, sizeof(bdf), &pci_dev->addr);
	printf("%s: Found a VMD[ %d ] at %s\n", __func__,  vmd_c->count, bdf);

	/* map vmd bars */
	int i = vmd_c->count;
	vmd_c->vmd[i].pci = *pci_dev;
	vmd_c->vmd[i].vmd_index = i;
	vmd_c->vmd[i].domain =
		(pci_dev->addr.bus << 16) | (pci_dev->addr.dev << 8) | pci_dev->addr.func;
	vmd_c->vmd[i].max_pci_bus = PCI_MAX_BUS_NUMBER;
	if (vmd_map_bars(&vmd_c->vmd[i], pci_dev) == -1) {
		return -1;
	}

	printf("\n%s: vmd config bar(%p) vaddr(%p) size(%x)\n", __func__,
	       (void *)vmd_c->vmd[i].cfgbar, (void *)vmd_c->vmd[i].cfg_vaddr, (uint32_t)vmd_c->vmd[i].cfgbar_size);
	printf("%s: vmd mem bar(%p) vaddr(%p) size(%x)\n", __func__,
	       (void *)vmd_c->vmd[i].membar, (void *)vmd_c->vmd[i].mem_vaddr, (uint32_t)vmd_c->vmd[i].membar_size);
	printf("%s: vmd msix bar(%p) vaddr(%p) size(%x)\n\n", __func__,
	       (void *)vmd_c->vmd[i].msixbar, (void *)vmd_c->vmd[i].msix_vaddr,
	       (uint32_t)vmd_c->vmd[i].msixbar_size);

	vmd_c->count = i + 1;

	i = vmd_enumerate_devices(&vmd_c->vmd[i]);

	return 0;
}

/*
 *************************************************************
 * Initializes and hooks an NVMe device returned in the list obtained from calling
 * get_vmd_pci_device_list(). This device can now be passed to NVMe driver using
 * the device domain-BDF. Domain is now a non-zero value.
 */
void vmd_spdk_dev_init(vmd_pci_device *dev)
{
	if (!dev || !dev->bus || !dev->bus->vmd || !dev->header) {
		return;
	}

	dev->pci.addr.domain = dev->bus->vmd->domain;
	dev->pci.addr.bus = dev->bus->bus_number;
	dev->pci.addr.dev = dev->devfn;
	dev->pci.addr.func = 0;
	dev->pci.id.vendor_id = dev->header->common.vendor_id;
	dev->pci.id.device_id = dev->header->common.device_id;
	dev->pci.map_bar = spdk_vmd_dev_map_bar;
	dev->pci.unmap_bar = spdk_vmd_dev_unmap_bar;
	dev->pci.cfg_read = spdk_vmd_dev_cfg_read;
	dev->pci.cfg_write = spdk_vmd_dev_cfg_write;
	dev->pci.detach = spdk_vmd_dev_detach;

	if (is_supported_device(dev)) {
		uint8_t bdf[32];
		spdk_pci_addr_fmt(bdf, sizeof(bdf), &dev->pci.addr);
		printf("Hooked an NVMe device at %s\n", bdf);
		spdk_pci_hook_device(spdk_pci_nvme_get_driver(), &dev->pci);
	}
}


/*
 ******************************************************************
 * @brief:  get_vmd_pci_device_list : Returns a list of nvme devices
 *              found on the given vmd pci BDF.
 * @input: -vmd_addr: pci BDF of the vmd device to return end device list
 *          -nvme_list: buffer of up to MAX_VMD_TARGET to return spdk_pci_device array.
 * @return: Returns count of nvme device attached to input VMD.
 */
int get_vmd_pci_device_list(
	IN_ARG struct spdk_pci_addr vmd_addr,
	OUT_ARG struct spdk_pci_device *nvme_list)
{
	int cnt = 0;

	if (!nvme_list) {
		return -1;
	}

	for (int i = 0; i < MAX_VMD_TARGET; ++i) {
		if (is_equal_addr(&vmd_addr, &g_vmd_container.vmd[i].pci.addr)) {
			vmd_pci_bus *bus = g_vmd_container.vmd[i].bus_list;
			while (bus != NULL) {
				vmd_pci_device *dev = bus->dev_list;
				while (dev != NULL) {
					nvme_list[cnt++] = dev->pci;
					if (!dev->is_hooked) {
						vmd_spdk_dev_init(dev);
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

/*
 *******************************************************************************
 * @brief - Creates a list of vmd pci devices that can be scanned to determine attached nvme devices
 * @return: - number of VMD devices available in the system.
 * ** Revisit:: Do we want a single application to have a list of all available vmd devices,
 * or do we want to have different apps or instances of apps bind to a separate VMD?
 */
int spdk_vmd_probe(struct spdk_pci_addr *vmd_bdf)
{
	g_vmd_container.vmd_target_addr = vmd_bdf;
	(void)spdk_pci_enumerate(spdk_pci_vmd_get_driver(), vmd_enum_cb, &g_vmd_container);
	g_vmd_container.vmd_target_addr = NULL;

	return g_vmd_container.count;
}

SPDK_LOG_REGISTER_COMPONENT("vmd", SPDK_LOG_VMD)
