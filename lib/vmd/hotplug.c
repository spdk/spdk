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

void hp_free_bus_number(struct vmd_hot_plug *hp, uint8_t bus_number)
{
	if (hp && (bus_number - hp->next_bus_number) < RESERVED_HOTPLUG_BUSES) {
		hp->bus_numbers[bus_number - hp->next_bus_number] = bus_number;
	}
}

bool bus_is_hotplug_enabled(vmd_pci_bus *bus)
{
	return ((bus && bus->self && bus->self->pcie_cap)
		&& (bus->self->pcie_cap->slot_cap.bit_field.hotplug_capable)
		&& (bus->self->pcie_cap->slot_control.bit_field.hotplug_interrupt_enable));
}

bool bus_is_hot_insert(vmd_pci_bus *bus)
{
	express_slot_status_register slot_status;
	if (bus && bus->self && bus->self->pcie_cap && bus->self->hp) {
		slot_status.as_uint16_t = bus->self->pcie_cap->slot_status.as_uint16_t;
		return (slot_status.bit_field.presence_detect_changed
			&& slot_status.bit_field.presence_detect_state);
	}

	return false;
}

bool bus_is_hot_removal(vmd_pci_bus *bus)
{
	express_slot_status_register slot_status;
	if (bus && bus->self && bus->self->pcie_cap && bus->self->hp) {
		slot_status.as_uint16_t = bus->self->pcie_cap->slot_status.as_uint16_t;
		return (slot_status.bit_field.presence_detect_changed
			&& slot_status.bit_field.presence_detect_state);
	}

	return false;
}

void hp_clear_slot_status(vmd_pci_device *dev)
{
	if (dev && dev->hp && dev->pcie_cap) {
		uint16_t slot_status = dev->pcie_cap->slot_status.as_uint16_t;
		dev->pcie_cap->slot_status.as_uint16_t = slot_status;
		slot_status = dev->pcie_cap->slot_status.as_uint16_t;
		printf("%s: PCI_DEV(%p) : hp_slot status cleared\n", __func__, dev);
	}
}

/*
 *****************************************************************************
 * @brief - hp_getHotPlugPortChanged : returns a pointer to the hot plug port
 *          corresponding to index. If no hot plug port has changed state for
 *          thios index, NULL is returned.
 * @param index - number 0 - n-1 to return the bus object for the port that changed.
 * @return - vmd_pci_bus* of the port that changed, corresponding to index.
 *****************************************************************************
 */
vmd_pci_bus *hp_get_hotplug_port_changed(void *v, unsigned int index)
{
	unsigned int changed_port_number = 0;
	vmd_adapter *vmd = v;
	vmd_pci_bus *ret_bus = NULL, *next_bus;
	if (vmd == NULL) {
		return NULL;
	}

	next_bus = vmd->bus_list;
	while (next_bus != NULL) {
		if (next_bus->self && next_bus->self->hp) {
			if (bus_is_hot_removal(next_bus) || bus_is_hot_insert(next_bus)) {
				if (changed_port_number == index) {
					ret_bus = next_bus;
					break;
				}
				changed_port_number++;
			}
		}
		next_bus = next_bus->next;
	}

	printf("%s returned HP_BUS = %p\n", __func__, ret_bus);
	return ret_bus;
}

/*
 *  combine both funcs below and put on vmd_hot_plug object with pcibus as input,
 * or overload with bus or device as input.
 */
vmd_pci_bus *is_dev_in_hotplug_path(vmd_pci_device *dev)
{
	vmd_pci_bus *owner = dev->bus;
	if (owner) {
		if (owner->self && owner->self->hp) {
			return owner;
		}
	}
	return NULL;
}

vmd_pci_bus *is_bus_in_hotplug_path(vmd_pci_bus *bus)
{
	vmd_pci_bus *pbus = bus->parent;
	while (pbus != NULL) {
		if (pbus->self && pbus->self->hp) {
			return pbus;
		}
		pbus = pbus->parent;
	}
	return NULL;
}


void hp_remove_device(vmd_pci_bus *hpbus, vmd_pci_device *dev)
{
	if (!hpbus || !dev || !hpbus->vmd) {
		return;
	}
	printf("%s: removing dev(%p) on bus(%p)\n", __func__, dev, hpbus);

	hp_free_base_addrs(hpbus->self->hp, dev);
}


/*
 ************************************************************************************
 * @brief alignBaseAddrs - aligns the address base to the input alignment mask.
 * @param dev - pciDevice for address alignment
 * @param alignment - mask for alignment of port base address.
 ************************************************************************************
 */
void hp_align_base_addrs(struct vmd_hot_plug *hp, uint32_t alignment)
{
	/*
	 *  Device is not in hot plug path, align the base address remaining from membar 1.
	 */
	if (hp->physical_addr & (alignment - 1)) {
		uint32_t pad = alignment - (hp->physical_addr & (alignment - 1));
		hp->physical_addr += pad;
		hp->addr_size -= (uint32_t) pad;
	}
}

/*
 ************************************************************************************
 * @brief getNextBusNumber - Returns the next bus number available for the hot plug port.
 * @param dev - pciDevice for bus number assignment
 * @return - next useful bus bumber.
 ************************************************************************************
 */
uint8_t hp_get_next_bus_number(struct vmd_hot_plug *hp)
{
	int bus_number = 0xff, index = 0xff;
	for (int i = 0; i < RESERVED_HOTPLUG_BUSES; ++i) {
		if (hp->bus_numbers[i] && (bus_number > hp->bus_numbers[i])) {
			bus_number = hp->bus_numbers[i];
			index = i;
		}
	}

	if (index < RESERVED_HOTPLUG_BUSES) {
		hp->bus_numbers[index] = 0;
	}
	return bus_number;
}

/*
 ************************************************************************************
 * @brief - allocates a base address from the range of hot plug assigned addresses
 * @param dev - PciDevice to allocate address for
 * @param size - address size in bytes to allocate
 *
 * return - valid address or 0x0 if no valid address could be allocated
 *************************************************************************************
 */
uint64_t hp_allocate_base_addr(struct vmd_hot_plug *hp, uint32_t size)
{
	/* Round up base address to start on input size boundary */
	int n;
	if (hp->physical_addr & (size - 1)) {
		uint32_t pad = size - (hp->physical_addr & (size - 1));
		hp->physical_addr += pad;
		hp->addr_size -= (uint32_t) pad;
	}

	for (n = 0; n < (int) hp->count; n++) {
		if (!hp->mem[n].in_use && hp->mem[n].size >= size) {
			hp->mem[n].in_use = 1;
			printf("%s: mem[%d] allocated(size) %lx (%x)\n", __func__, n, hp->mem[n].addr, size);
			return hp->mem[n].addr;
		}
	}

	/*
	 * allocate a new address range and save it in the list of allocated addrs.
	 */
	if (hp->addr_size >= size) {
		uint64_t base_address = hp->physical_addr;
		hp->mem[hp->count].addr = hp->physical_addr;
		hp->mem[hp->count].size = size;
		hp->mem[hp->count].in_use = 1;
		hp->physical_addr += size;
		hp->addr_size -= size;
		hp->count++;
		printf("%s: allocated(size) %lx (%x)\n", __func__, base_address, size);
		return base_address;
	}

	printf("%s: returned 0x0 for hp->physicalAddr(%p) hp->addr_size(%x), size(%x)\n", __func__,
	       (void *)hp->physical_addr, hp->addr_size, size);
	return 0x0;
}

void hp_free_base_addrs(struct vmd_hot_plug *hp, vmd_pci_device *device)
{
	int i, j;
	for (i = 0; i < 6; i++) {
		if (device->bar[i].start == 0x0) {
			continue;
		}
		for (j = 0; j < ADDR_ELEM_COUNT; j++) {
			if (hp->mem[j].addr == device->bar[i].start) {
				hp->mem[j].in_use = 0;
				device->bar[i].start = 0x0;
				printf("%s: ************  base address(%llx) freed for device: %p\n",
				       __func__, (long long int)hp->mem[j].addr, device);
				break;
			}
		}
	}
}

/*
 ***********************************************************************************
 * @brief - cleanup device list by removing the devices just hot removed from the slot.
 *          if a bus in the port's bus list is in the device path, then remove this bus.
 *************************************************************************************
 */
void hp_remove_hotplug_devices(vmd_pci_bus *bus)
{
	vmd_pci_bus *bus_list = bus->vmd->bus_list;
	vmd_pci_bus *freebus;
	while (bus_list != NULL) {
		freebus = is_dev_in_hotplug_path(bus_list->self);
		if (freebus == bus) {
			freebus = bus_list;
			bus_list = bus_list->next;
			hp_free_bus_number(bus->self->hp, freebus->bus_number);
		} else {
			bus_list = bus_list->next;
		}
	}
}

void hp_enable_hotplug(struct vmd_hot_plug *hp)
{
	if (!hp) {
		return;
	}

	if (hp->bus->self && hp->bus->self->pcie_cap) {
		express_slot_control_register slot_control;
		slot_control.as_uint16_t = hp->bus->self->pcie_cap->slot_control.as_uint16_t | (1 << 5);
		slot_control.bit_field.hotplug_interrupt_enable = 1;
		hp->bus->self->pcie_cap->slot_control.as_uint16_t = slot_control.as_uint16_t;

		slot_control.as_uint16_t = hp->bus->self->pcie_cap->slot_control.as_uint16_t;

		volatile uint32_t *miscctrlsts0;
		slot_control.bit_field.attention_button_enable = 1;
		slot_control.bit_field.datalink_state_change_enable = 1;
		hp->bus->self->pcie_cap->slot_control.as_uint16_t = slot_control.as_uint16_t;

		miscctrlsts0 = (uint32_t *)((uint8_t *) hp->bus->self->header
					    + MISCCTRLSTS_0_OFFSET);
		*miscctrlsts0 &= ~ENABLE_ACPI_MODE_FOR_HOTPLUG;
		uint16_t data = *miscctrlsts0;
		slot_control.as_uint16_t = data;
	}
}

/*
 ************************************************************************************
 * @brief constructor for vmd_hot_plug Object
 ************************************************************************************
 */
struct vmd_hot_plug *new_hotplug(vmd_pci_bus *bus, uint8_t reserved_buses)
{
	struct vmd_hot_plug *hp;
	uint8_t i;
	if (!bus) {
		return NULL;
	}

	hp = (struct vmd_hot_plug *) calloc(sizeof(struct vmd_hot_plug), sizeof(uint8_t));
	if (!hp) {
		return NULL;
	}

	hp->addr_size = ONE_MB;
	hp->bus = bus;
	hp->reserved_bus_count = reserved_buses;
	if (hp->bus) {
		bus->self->pcie_cap->slot_control.bit_field.attention_indicator_control = 1;
		bus->self->pcie_cap->slot_control.bit_field.power_indicator_control = 1;
		/* hp->physical_addr = vmd_allocate_base_addr(hp->bus->vmd, NULL, hp->addr_size); */
		hp->max_hotplug_bus_number = hp->bus->self->header->one.subordinate;
		hp->next_bus_number = hp->bus->bus_number + 1;

		for (i = hp->next_bus_number; i <= hp->max_hotplug_bus_number; i++)
			if ((i - hp->next_bus_number) < RESERVED_HOTPLUG_BUSES) {
				hp->bus_numbers[i - hp->next_bus_number] = i;
			}

	}

	return hp;
}
