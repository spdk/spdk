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

/*
 *  combine both funcs below and put on vmd_hot_plug object with pcibus as input,
 * or overload with bus or device as input.
 */
struct vmd_pci_bus *
vmd_is_dev_in_hotplug_path(struct vmd_pci_device *dev)
{
	struct vmd_pci_bus *owner = dev->bus;

	if (owner) {
		if (owner->self && owner->self->hp) {
			return owner;
		}
	}
	return NULL;
}

/*
 ************************************************************************************
 * @brief getNextBusNumber - Returns the next bus number available for the hot plug port.
 * @param dev - pciDevice for bus number assignment
 * @return - next useful bus bumber.
 ************************************************************************************
 */
uint8_t
vmd_hp_get_next_bus_number(struct vmd_hot_plug *hp)
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
uint64_t
vmd_hp_allocate_base_addr(struct vmd_hot_plug *hp, uint32_t size)
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

void
vmd_hp_enable_hotplug(struct vmd_hot_plug *hp)
{
	if (!hp) {
		return;
	}

	if (hp->bus->self && hp->bus->self->pcie_cap) {
		union express_slot_control_register slot_control;
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
struct vmd_hot_plug *
vmd_new_hotplug(struct vmd_pci_bus *bus, uint8_t reserved_buses)
{
	struct vmd_hot_plug *hp;
	uint8_t i;
	if (!bus) {
		return NULL;
	}

	hp = calloc(sizeof(struct vmd_hot_plug), 1);
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
