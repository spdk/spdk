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

#include "vmd.h"

#include "spdk/stdinc.h"

static inline bool vmd_is_hot_insert(struct vmd_pci_bus *bus)
{
	union express_link_status_register link_status = {};
	union express_slot_status_register slot_status = {};

	if (!bus) {
		return false;
	}

	link_status.as_uint16_t = bus->self->pcie_cap->link_status.as_uint16_t;
	slot_status.as_uint16_t = bus->self->pcie_cap->slot_status.as_uint16_t;
	if ((slot_status.bit_field.datalink_state_changed == 1) &&
	    (link_status.bit_field.data_link_layer_active == 1)) {
		SPDK_DEBUGLOG(SPDK_LOG_VMD, "VMD: Device insert detected.\n");
		return true;
	}

	return false;
}


static inline bool vmd_is_hot_remove(struct vmd_pci_bus *bus)
{
	union express_link_status_register link_status = {};
	union express_slot_status_register slot_status = {};

	if (!bus) {
		return false;
	}

	link_status.as_uint16_t = bus->self->pcie_cap->link_status.as_uint16_t;
	slot_status.as_uint16_t = bus->self->pcie_cap->slot_status.as_uint16_t;
	if ((slot_status.bit_field.datalink_state_changed == 1) &&
	    (link_status.bit_field.data_link_layer_active == 0)) {
		SPDK_DEBUGLOG(SPDK_LOG_VMD, "VMD: Device remove detected.\n");
		return true;
	}

	return false;
}

inline static void
vmd_hp_clear_slot_status(struct vmd_pci_bus *bus)
{
	uint16_t slot_status, link_status;
	if (bus && bus->self) {
		slot_status = bus->self->pcie_cap->slot_status.as_uint16_t;
		bus->self->pcie_cap->slot_status.as_uint16_t = slot_status;
		slot_status = bus->self->pcie_cap->slot_status.as_uint16_t;

		link_status = bus->self->pcie_cap->link_status.as_uint16_t;
		bus->self->pcie_cap->link_status.as_uint16_t = link_status;
		link_status = bus->self->pcie_cap->link_status.as_uint16_t;
		SPDK_DEBUGLOG(SPDK_LOG_VMD, "%s: slot/link status = 0x%x:0x%x\n", __func__, slot_status,
			      link_status);
	}
}


#define HP_SCAN_DELAY (200000) /* 200ms usleep */
inline static struct spdk_pci_addr
vmd_process_hot_insert(struct vmd_pci_bus *bus)
{
	struct vmd_pci_device *dev;
	struct spdk_pci_addr addr = {-1, -1, -1, -1};
	int sleep_count = 20;
	uint8_t dev_count;
	struct pci_header volatile *header;

	/*
	 * Poll until the inserted device config space is accessible
	 */
	header = (struct pci_header * volatile)(bus->vmd->cfg_vaddr +
						CONFIG_OFFSET_ADDR(bus->bus_number, 0, 0, 0));
	while ((header->common.vendor_id == PCI_INVALID_VENDORID || header->common.vendor_id == 0x0) &&
	       sleep_count-- > 0) {
		usleep(HP_SCAN_DELAY);
	}

	/*
	 * scan for the device on this bus and attach a device object to bus if found
	 */
	dev_count = vmd_scan_single_bus(bus, bus->self);
	if (bus->dev_list && dev_count) {
		dev = bus->dev_list;
		addr.domain = dev->pci.addr.domain;
		addr.bus = dev->pci.addr.bus;
		addr.dev = dev->pci.addr.dev;
		addr.func = dev->pci.addr.func;

		/*
		 * Debug code to test inserted SSD has MMIO access
		 */
		printf("Device [%04x:%04x] inserted at pci address[%x:%x:%x.%x\n",
		       dev->did, dev->vid, dev->pci.addr.domain, dev->pci.addr.bus,
		       dev->pci.addr.dev, dev->pci.addr.func);

		for (dev_count = 0; dev_count < 2; dev_count++) {
			printf("\t Device MMIO *BAR0 offset[%d] = %08x\n",
			       dev_count << 2, ((uint32_t *)dev->bar[0].vaddr)[dev_count]);
		}
	}

	vmd_hp_clear_slot_status(bus);

	return addr;
}

inline static struct spdk_pci_addr
vmd_process_hot_remove(struct vmd_pci_bus *bus)
{
	struct vmd_pci_device *dev_removed;
	struct spdk_pci_addr addr = {0, 0, 0, 0};
	/*
	 * Physical device removed from bus. Cleanup device object on bus and free
	 * resources back to system
	 */
	if (!(bus && bus->dev_list)) {
		return addr;
	}

	dev_removed = bus->dev_list;
	vmd_pcibus_remove_device(bus, dev_removed);

	printf("PCI device[%04x:%04x] at D-BDF[%x:%02x:%02x.%02x] hot removed\n",
	       dev_removed->vid, dev_removed->did, dev_removed->pci.addr.domain,
	       dev_removed->pci.addr.bus, dev_removed->pci.addr.dev, dev_removed->pci.addr.func);

	/*
	 * return the d-bdf of the removed device?
	 */
	addr.domain = dev_removed->pci.addr.domain;
	addr.bus = dev_removed->pci.addr.bus;
	addr.dev = dev_removed->pci.addr.dev;
	addr.func = dev_removed->pci.addr.func;

	vmd_hp_clear_slot_status(bus);

	free(dev_removed);
	return addr;
}


bool
spdk_vmd_hotplug_handler(void *vmd_dev, struct spdk_pci_addr *out_addr, bool *is_inserted)
{
	struct vmd_pci_bus *bus;
	struct vmd_pci_device *dev;
	struct vmd_adapter *vmd = (struct vmd_adapter *)vmd_dev;

	if (!vmd || !vmd->bus_list) {
		return false;
	}

	do  {
		bus = vmd->bus_list;
		while (bus != NULL) {
			dev = bus->self;

			/*
			 * if port is hot pluggable, check if it has hot plug event
			 */
			if (dev && dev->hp) {
				if (vmd_is_hot_insert(bus)) {
					vmd_process_hot_insert(bus);
					*is_inserted = true;
					return true;
				} else if (vmd_is_hot_remove(bus)) {
					*out_addr = vmd_process_hot_remove(bus);
					*is_inserted = false;
					return true;
				}
			}

			bus = bus->next;
		}
	} while (0);

	return false;
}
