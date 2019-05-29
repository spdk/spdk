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

static unsigned char *device_type[] = {
	"PCI Express Endpoint",
	"Legacy PCI Express Endpoint",
	"Reserved 1",
	"Reserved 2",
	"Root Port of PCI Express Root Complex",
	"Upstream Port of PCI Express Switch",
	"Downstream Port of PCI Express Switch",
	"PCI Express to PCI/PCI-X Bridge",
	"PCI/PCI-X to PCI Express Bridge",
	"Root Complex Integrated Endpoint",
	"Root Complex Event Collector",
	"Reserved Capability"
};

static int g_end_device_count;

static bool
vmd_is_valid_cfg_addr(vmd_pci_bus *bus, uint64_t addr)
{
	if (bus == NULL || !addr || bus->vmd == NULL) {
		return false;
	}

	return addr >= (uint64_t)bus->vmd->cfg_vaddr &&
	       addr < bus->vmd->cfgbar_size + (uint64_t)bus->vmd->cfg_vaddr;
}

static void
vmd_align_base_addrs(vmd_adapter *vmd, vmd_pci_device *dev, uint32_t alignment)
{
	/*
	 *  Device is not in hot plug path, align the base address remaining from membar 1.
	 */
	if (vmd) {
		if (vmd->physical_addr & (alignment - 1)) {
			uint32_t pad = alignment - (vmd->physical_addr & (alignment - 1));
			vmd->physical_addr += pad;
			vmd->current_addr_size -= pad;
		}
	}
}

/*
 *  Allocates an address from vmd membar for the input memory size
 *  vmdAdapter - vmd adapter object
 *  dev - vmd_pci_device to allocate a base address for.
 *  size - size of the memory window requested.
 *  Size must be an integral multiple of 2. Addresses are returned on the size boundary.
 *  Returns physical address within the VMD membar window, or 0x0 if cannot allocate window.
 *  Consider increasing the size of vmd membar if 0x0 is returned.
 */
static uint64_t
vmd_allocate_base_addr(vmd_adapter *vmd, vmd_pci_device *dev, uint32_t size)
{
	uint64_t base_address = 0;

	if (size && ((size & (~size + 1)) != size)) {
		return base_address;
	}

	/*
	 *  If device is downstream of a hot plug port, allocate address from the
	 *  range dedicated for the hot plug slot. Search the list of addresses allocated to determine
	 *  if a free range exists that satisfy the input request.  If a free range cannot be found,
	 *  get a buffer from the  unused chunk. First fit algorithm, is used.
	 */
	if (dev) {
		vmd_pci_bus *hp_bus = vmd_is_dev_in_hotplug_path(dev);
		if (hp_bus && hp_bus->self) {
			return vmd_hp_allocate_base_addr(hp_bus->self->hp, size);
		}
	}

	/* Ensure physical membar allocated is size aligned */
	if (vmd->physical_addr & (size - 1)) {
		uint32_t pad = size - (vmd->physical_addr & (size - 1));
		vmd->physical_addr += pad;
		vmd->current_addr_size -= pad;
	}

	/* Allocate from membar if enough memory is left */
	if (vmd->current_addr_size >= size) {
		base_address = vmd->physical_addr;
		vmd->physical_addr += size;
		vmd->current_addr_size -= size;
	}

	printf("%s: allocated(size) %lx (%x)\n", __func__, base_address, size);

	return base_address;
}

static bool
vmd_is_end_device(vmd_pci_device *dev)
{
	return (dev && dev->header) &&
	       ((dev->header->common.header_type & ~PCI_MULTI_FUNCTION) == PCI_HEADER_TYPE_NORMAL);
}

static void
vmd_update_base_limit_register(vmd_pci_device *dev, uint16_t base, uint16_t limit)
{
	if (base && limit && dev != NULL) {
		vmd_pci_bus *bus = dev->parent;
		while (bus && bus->self != NULL) {
			vmd_pci_device *bridge = bus->self;

			/* This is only for 32-bit memory space, need to revisit to support 64-bit */
			if (bridge->header->one.mem_base > base) {
				bridge->header->one.mem_base = base;
				base = bridge->header->one.mem_base;
			}

			if (bridge->header->one.mem_limit < limit) {
				bridge->header->one.mem_limit = limit;
				limit = bridge->header->one.mem_limit;
			}
			bus = bus->parent;
		}
	}
}

static bool
vmd_assign_base_addrs(vmd_pci_device *dev)
{
	uint16_t mem_base = 0, mem_limit = 0;
	unsigned char mem_attr = 0;
	int last = dev->header_type ? 2 : 6;
	vmd_adapter *vmd = NULL;
	bool ret_val = false;
	uint32_t bar_value;

	if (dev && dev->bus) {
		vmd = dev->bus->vmd;
	}

	if (!vmd) {
		return 0;
	}

	vmd_align_base_addrs(vmd, vmd->is_hotplug_scan ? dev : NULL, ONE_MB);

	for (int i = 0; i < last; i++) {
		bar_value = dev->header->zero.BAR[i];
		dev->header->zero.BAR[i] = ~(0U);
		dev->bar[i].size = dev->header->zero.BAR[i];
		dev->header->zero.BAR[i] = bar_value;

		if (dev->bar[i].size == ~(0U) || dev->bar[i].size == 0  ||
		    dev->header->zero.BAR[i] & 1) {
			dev->bar[i].size = 0;
			continue;
		}
		mem_attr = dev->bar[i].size & PCI_BASE_ADDR_MASK;
		dev->bar[i].size = TWOS_COMPLEMENT(dev->bar[i].size & PCI_BASE_ADDR_MASK);
		dev->bar[i].start = vmd_allocate_base_addr(vmd, dev, dev->bar[i].size);
		dev->header->zero.BAR[i] = (uint32_t)dev->bar[i].start;

		if (!dev->bar[i].start) {
			if (mem_attr == (PCI_BAR_MEMORY_PREFETCH | PCI_BAR_MEMORY_TYPE_64)) { i++; }
			continue;
		}

		dev->bar[i].vaddr = ((uint64_t)vmd->mem_vaddr + (dev->bar[i].start - vmd->membar));
		mem_limit = BRIDGE_BASEREG(dev->header->zero.BAR[i]) +
			    BRIDGE_BASEREG(dev->bar[i].size - 1);
		if (!mem_base) {
			mem_base = BRIDGE_BASEREG(dev->header->zero.BAR[i]);
		}

		ret_val = true;

		if (mem_attr == (PCI_BAR_MEMORY_PREFETCH | PCI_BAR_MEMORY_TYPE_64)) {
			i++;
			if (i < last) {
				dev->header->zero.BAR[i] = (uint32_t)(dev->bar[i].start >> PCI_DWORD_SHIFT);
			}
		}
	}

	/* Enable device MEM and bus mastering */
	dev->header->zero.command |= (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
	uint16_t cmd = dev->header->zero.command;
	cmd++;

	if (dev->msix_cap && ret_val) {
		uint32_t table_offset = ((volatile pci_msix_cap *)dev->msix_cap)->msix_table_offset;
		if (dev->bar[table_offset & 0x3].vaddr) {
			dev->msix_table = (volatile pci_msix_table_entry *)
					  (dev->bar[table_offset & 0x3].vaddr + (table_offset & 0xfff8));
		}
	}

	if (ret_val && vmd_is_end_device(dev)) {
		vmd_update_base_limit_register(dev, mem_base, mem_limit);
	}

	return ret_val;
}

static void
vmd_get_device_capabilities(vmd_pci_device *dev)

{
	volatile uint8_t *config_space;
	uint8_t capabilities_offset;
	pci_capabilities_header *capabilities_hdr;

	if (!dev) {
		return;
	}

	config_space = (volatile uint8_t *)dev->header;
	if ((dev->header->common.status  & PCI_CAPABILITIES_LIST) == 0) {
		return;
	}

	capabilities_offset = dev->header->zero.cap_pointer;
	if (dev->header->common.header_type & PCI_HEADER_TYPE_BRIDGE) {
		capabilities_offset = dev->header->one.cap_pointer;
	}

	while (capabilities_offset > 0) {
		capabilities_hdr = (pci_capabilities_header *)&config_space[capabilities_offset];
		switch (capabilities_hdr->capability_id) {
		case CAPABILITY_ID_PCI_EXPRESS:
			dev->pcie_cap = (volatile pci_express_cap *)(capabilities_hdr);
			break;

		case CAPABILITY_ID_MSI:
			dev->msi_cap = (volatile pci_msi_cap *)capabilities_hdr;
			break;

		case CAPABILITY_ID_MSIX:
			dev->msix_cap = (volatile pci_msix_capability *)capabilities_hdr;
			dev->msix_table_size = dev->msix_cap->message_control.bit.table_size + 1;
			break;

		default:
			break;
		}
		capabilities_offset = capabilities_hdr->next;
	}
}

static volatile pci_enhanced_capability_header *
vmd_get_enhanced_capabilities(vmd_pci_device *dev, uint16_t capability_id)
{
	uint8_t *data;
	uint16_t cap_offset = EXTENDED_CAPABILITY_OFFSET;
	volatile pci_enhanced_capability_header *cap_hdr = NULL;

	data = (uint8_t *)dev->header;
	while (cap_offset >= EXTENDED_CAPABILITY_OFFSET) {
		cap_hdr = (volatile pci_enhanced_capability_header *) &data[cap_offset];
		if (cap_hdr->capability_id == capability_id) {
			return cap_hdr;
		}
		cap_offset = cap_hdr->next;
		if (cap_offset == 0 || cap_offset < EXTENDED_CAPABILITY_OFFSET) {
			break;
		}
	}

	return NULL;
}

static void
vmd_read_config_space(vmd_pci_device *dev)
{
	/*
	 * Writes to the pci config space is posted weite. To ensure transaction reaches its destination
	 * before another write is posed, an immediate read of the written value should be performed.
	 */
	dev->header->common.command |= (BUS_MASTER_ENABLE | MEMORY_SPACE_ENABLE);
	{ uint16_t cmd = dev->header->common.command; (void)cmd; }

	vmd_get_device_capabilities(dev);
	dev->sn_cap = (serial_number_capability *)vmd_get_enhanced_capabilities(dev,
			DEVICE_SERIAL_NUMBER_CAP_ID);
}

static vmd_pci_device *
vmd_alloc_dev(vmd_pci_bus *bus, uint32_t devfn)
{
	vmd_pci_device *dev = NULL;
	pci_header volatile *header;
	uint8_t header_type;
	uint32_t rev_class;

	if (bus == NULL || bus->vmd == NULL) {
		return dev;
	}

	header = (pci_header * volatile)(bus->vmd->cfg_vaddr +
					 CONFIG_OFFSET_ADDR(bus->bus_number, devfn, 0, 0));
	if (!vmd_is_valid_cfg_addr(bus, (uint64_t)header)) {
		return NULL;
	}

	if (header->common.vendor_id == PCI_INVALID_VENDORID || header->common.vendor_id == 0x0) {
		return NULL;
	}

	printf("    *** PCI DEVICE FOUND : %04x:%04x ***\n",
	       header->common.vendor_id, header->common.device_id);
	if ((dev = calloc(1, sizeof(vmd_pci_device))) == NULL) {
		return NULL;
	}

	dev->header = header;
	dev->vid = dev->header->common.vendor_id;
	dev->did = dev->header->common.device_id;
	dev->bus = bus;
	dev->parent = bus;
	dev->devfn = devfn;
	header_type = dev->header->common.header_type;
	rev_class = dev->header->common.rev_class;
	dev->class = rev_class >> 8;
	dev->header_type = header_type & 0x7;

	if (header_type == PCI_HEADER_TYPE_BRIDGE) {
		dev->header->one.mem_base = 0xfff0;
		dev->header->one.mem_limit = 0x0;
		dev->header->one.prefetch_base_upper = 0x0;
		dev->header->one.prefetch_limit_upper = 0x0;
		dev->header->one.io_base_upper = 0x0;
		dev->header->one.io_limit_upper = 0x0;
		dev->header->one.primary = 0;
		dev->header->one.secondary = 0;
		dev->header->one.subordinate = 0;
	}

	vmd_read_config_space(dev);

	return dev;
}

static void
vmd_add_bus_to_list(vmd_adapter *vmd, vmd_pci_bus *bus)
{
	vmd_pci_bus *blist;

	if (!bus || !vmd) {
		return;
	}

	blist = vmd->bus_list;
	bus->next = NULL;
	if (blist == NULL) {
		vmd->bus_list = bus;
		return;
	}

	while (blist->next != NULL) {
		blist = blist->next;
	}

	blist->next = bus;
}

static void
vmd_pcibus_remove_device(vmd_pci_bus *bus, vmd_pci_device *device)
{
	vmd_pci_device *list = bus->dev_list;

	if (list == device) {
		bus->dev_list = NULL;
	}

	while (list->next != NULL) {
		if (list->next == device) {
			list->next = list->next->next;
		}
		list = list->next;
	}
}


static bool
vmd_bus_add_device(vmd_pci_bus *bus, vmd_pci_device *device)
{
	if (!bus || !device) {
		return 0;
	}

	vmd_pci_device *next_dev = bus->dev_list;
	device->next = NULL;
	if (next_dev == NULL) {
		bus->dev_list = device;
		return 1;
	}

	while (next_dev->next != NULL) {
		next_dev = next_dev->next;
	}

	next_dev->next = device;

	return 1;
}

static vmd_pci_bus *
vmd_create_new_bus(vmd_pci_bus *parent, vmd_pci_device *bridge, uint8_t bus_number)
{
	vmd_pci_bus *new_bus;
	if (!parent) {
		return NULL;
	}

	new_bus = (vmd_pci_bus *)calloc(1, sizeof(vmd_pci_bus));
	if (!new_bus) {
		return NULL;
	}
	new_bus->parent = parent;
	new_bus->domain = parent->domain;
	new_bus->bus_number = bus_number;
	new_bus->secondary_bus = new_bus->subordinate_bus = bus_number;
	new_bus->self = bridge;
	new_bus->vmd = parent->vmd;
	bridge->subordinate = new_bus;

	bridge->pci.addr.bus = new_bus->bus_number;
	bridge->pci.addr.dev = bridge->devfn;
	bridge->pci.addr.func = 0;
	bridge->pci.addr.domain = parent->vmd->pci.addr.domain;

	return new_bus;
}

/*
 * Assigns a bus number from the list of available
 * bus numbers. If the device is downstream of a hot plug port,
 * assign the bus number from thiose assigned to the HP port. Otherwise,
 * assign the next bus number from the vmd bus number list.
 */
static uint8_t
vmd_get_next_bus_number(vmd_pci_device *dev, vmd_adapter *vmd)
{
	uint8_t bus = 0xff;

	if (dev) {
		vmd_pci_bus  *hp_bus = vmd_is_dev_in_hotplug_path(dev);
		if (hp_bus && hp_bus->self && hp_bus->self->hp) {
			return vmd_hp_get_next_bus_number(hp_bus->self->hp);
		}
	}

	/* Device is not under a hot plug path. Return next global bus number */
	if ((vmd->next_bus_number + 1) < vmd->max_pci_bus) {
		bus = vmd->next_bus_number;
		vmd->next_bus_number++;
	}
	return bus;
}

static uint8_t
vmd_get_hotplug_bus_numbers(vmd_pci_device *dev)
{
	uint8_t bus_number = 0xff;

	if (dev && dev->bus && dev->bus->vmd &&
	    ((dev->bus->vmd->next_bus_number + RESERVED_HOTPLUG_BUSES) < dev->bus->vmd->max_pci_bus)) {
		bus_number = RESERVED_HOTPLUG_BUSES;
		dev->bus->vmd->next_bus_number += RESERVED_HOTPLUG_BUSES;
	}

	return bus_number;
}

static void
vmd_enable_msix(vmd_pci_device *dev)
{
	volatile uint16_t control;

	if (!(dev && dev->msix_cap)) {
		return;
	}

	control = dev->msix_cap->message_control.as_uint16_t | (1 << 14);
	dev->msix_cap->message_control.as_uint16_t = control;
	control = dev->msix_cap->message_control.as_uint16_t;
	dev->msix_cap->message_control.as_uint16_t = (control | (1 << 15));
	control = dev->msix_cap->message_control.as_uint16_t;
	control = control & ~(1 << 14);
	dev->msix_cap->message_control.as_uint16_t = control;
	control = dev->msix_cap->message_control.as_uint16_t;
}

static void
vmd_disable_msix(vmd_pci_device *dev)
{
	volatile uint16_t control;

	if (!(dev && dev->msix_cap)) {
		return;
	}

	control = dev->msix_cap->message_control.as_uint16_t | (1 << 14);
	dev->msix_cap->message_control.as_uint16_t = control;
	control = dev->msix_cap->message_control.as_uint16_t & ~(1 << 15);
	dev->msix_cap->message_control.as_uint16_t = control;
	control = dev->msix_cap->message_control.as_uint16_t;
}

/*
 * Set up MSI-X table entries for the port. Vmd MSIX vector 0 is used for
 * port interrupt, so vector 0 is mapped to all MSIX entries for the port.
 */
static void
vmd_setup_msix(vmd_pci_device *dev, volatile struct _pci_misx_table_entry *vmdEntry)
{
	int entry;

	if (!dev || !vmdEntry || !dev->msix_cap) {
		return;
	}

	vmd_disable_msix(dev);
	if (dev->msix_table == NULL || dev->msix_table_size > MAX_MSIX_TABLE_SIZE) {
		return;
	}

	for (entry = 0; entry < dev->msix_table_size; ++entry) {
		dev->msix_table[entry].vector_control = 1;
	}
	vmd_enable_msix(dev);
}

static void
vmd_bus_update_bridge_info(vmd_pci_device *bridge)
{
	/* Update the subordinate bus of all bridges above this bridge */
	volatile vmd_pci_device *dev = bridge;
	uint8_t subordinate_bus;

	if (!dev) {
		return;
	}
	subordinate_bus = bridge->header->one.subordinate;
	while (dev->parent_bridge != NULL) {
		dev = dev->parent_bridge;
		if (dev->header->one.subordinate < subordinate_bus) {
			dev->header->one.subordinate = subordinate_bus;
			subordinate_bus = dev->header->one.subordinate;
		}
	}
}

/*
 * Scans a single bus for all devices attached and return a count of
 * how many devices found. In the VMD topology, it is assume there are no multi-
 * function devices. Hence a bus(bridge) will not have multi function with both type
 * 0 and 1 header.
 *
 * The other option  for implementing this function is the bus is an int and
 * create a new device PciBridge. PciBridge would inherit from PciDevice with extra fields,
 * sub/pri/sec bus. The input becomes PciPort, bus number and parent_bridge.
 *
 * The bus number is scanned and if a device is found, based on the header_type, create
 * either PciBridge(1) or PciDevice(0).
 *
 * If a PciBridge, assign bus numbers and rescan new bus. The currenty PciBridge being
 * scanned becomes the passed in parent_bridge with the new bus number.
 *
 * The linked list becomes list of pciBridges with PciDevices attached.
 *
 * Return count of how many devices found(type1 + type 0 header devices)
 */
static uint8_t
vmd_scan_single_bus(vmd_pci_bus *bus, vmd_pci_device *parent_bridge)
{
	/* assuming only single function devices are on the bus */
	vmd_pci_device *new_dev;
	vmd_pci_bus *new_bus;
	int  device_number, dev_cnt = 0;
	express_slot_capabiliies_register slot_cap;
	uint8_t new_bus_num;

	if (bus == NULL) {
		return 0;
	}

	for (device_number = 0; device_number < 32; device_number++) {
		new_dev = vmd_alloc_dev(bus, device_number);
		if (new_dev == NULL) {
			continue;
		}

		dev_cnt++;
		if (new_dev->header->common.header_type & PCI_HEADER_TYPE_BRIDGE) {
			slot_cap.as_uint32_t = 0;
			if (new_dev->pcie_cap != NULL) {
				slot_cap.as_uint32_t = new_dev->pcie_cap->slot_cap.as_uint32_t;
			}

			new_bus_num = vmd_get_next_bus_number(bus->vmd->is_hotplug_scan ? new_dev : NULL, bus->vmd);
			if (new_bus_num == 0xff) {
				free(new_dev);
				return dev_cnt;
			}
			new_bus = vmd_create_new_bus(bus, new_dev, new_bus_num);
			if (!new_bus) {
				free(new_dev);
				return dev_cnt;
			}
			new_bus->primary_bus = bus->secondary_bus;
			new_bus->self = new_dev;
			new_dev->bus_object = new_bus;

			if (slot_cap.bit_field.hotplug_capable) {
				new_bus->hotplug_buses = vmd_get_hotplug_bus_numbers(new_dev);
				new_bus->subordinate_bus += new_bus->hotplug_buses;
			}
			new_dev->parent_bridge = parent_bridge;
			new_dev->header->one.primary = new_bus->primary_bus;
			new_dev->header->one.secondary = new_bus->secondary_bus;
			new_dev->header->one.subordinate = new_bus->subordinate_bus;

			vmd_bus_update_bridge_info(new_dev);
			vmd_add_bus_to_list(bus->vmd, new_bus);

			/* Attach hot plug instance if HP is supported */
			if (slot_cap.bit_field.hotplug_capable) {
				new_dev->hp = vmd_new_hotplug(new_bus, new_bus->hotplug_buses);
			}

			vmd_dev_init(new_dev);

			dev_cnt += vmd_scan_single_bus(new_bus, new_dev);
			if (new_dev->pcie_cap != NULL) {
				if (new_dev->pcie_cap->express_cap_register.bit_field.device_type == SwitchUpstreamPort) {
					return dev_cnt;
				}
			}
		} else {
			/* Attach the device to the current bus and assign base addresses */
			vmd_bus_add_device(bus, new_dev);
			g_end_device_count++;
			if (vmd_assign_base_addrs(new_dev)) {
				vmd_setup_msix(new_dev, &bus->vmd->msix_table[0]);
				vmd_dev_init(new_dev);
				if (vmd_is_supported_device(new_dev)) {
					vmd_adapter *vmd = bus->vmd;
					vmd->target[vmd->nvme_count] = new_dev;
					vmd->nvme_count++;
				}
			} else {
				printf("%s: Removing failed device: %p\n", __func__, new_dev);
				vmd_pcibus_remove_device(bus, new_dev);
				if (dev_cnt) {
					dev_cnt--;
				}
			}
		}
	}

	return dev_cnt;
}

static void
vmd_print_pci_info(vmd_pci_device *dev)
{
	if (dev == NULL) {
		return;
	}
	if (dev->header == NULL) {
		return;
	}

	if (dev->pcie_cap != NULL) {
		printf("PCI DEVICE: [%04X:%04X] type(%x) : %s\n",
		       dev->header->common.vendor_id, dev->header->common.device_id,
		       dev->pcie_cap->express_cap_register.bit_field.device_type,
		       device_type[dev->pcie_cap->express_cap_register.bit_field.device_type]);
	} else {
		printf("PCI DEVICE: [%04X:%04X]\n", dev->header->common.vendor_id, dev->header->common.device_id);
	}

	printf("        DOMAIN:BDF: %04x:%02x:%02x:%x\n", dev->pci.addr.domain,
	       dev->pci.addr.bus, dev->pci.addr.dev, dev->pci.addr.func);

	if (!(dev->header_type & PCI_HEADER_TYPE_BRIDGE) && dev->bus) {
		printf("        base addr: %x : %p\n", dev->header->zero.BAR[0], (void *)dev->bar[0].vaddr);
	}

	if ((dev->header_type & PCI_HEADER_TYPE_BRIDGE)) {
		printf("        Primary = %d, Secondary = %d, Subordinate = %d\n",
		       dev->header->one.primary, dev->header->one.secondary, dev->header->one.subordinate);
		if (dev->pcie_cap && dev->pcie_cap->express_cap_register.bit_field.slot_implemented) {
			printf("        Slot implemented on this device.\n");
			if (dev->pcie_cap->slot_cap.bit_field.hotplug_capable) {
				printf("Device has HOT-PLUG capable slot.\n");
			}
		}
	}

	if (dev->sn_cap != NULL) {
		uint8_t *snLow = (uint8_t *)&dev->sn_cap->sn_low;
		uint8_t *snHi = (uint8_t *)&dev->sn_cap->sn_hi;
		printf("        SN: %02x-%02x-%02x-%02x-%02x-%02x-%02x-%02x\n",
		       snHi[3], snHi[2], snHi[1], snHi[0], snLow[3], snLow[2], snLow[1], snLow[0]);
	}

	printf("\n");
}

static void
vmd_pci_print(vmd_pci_bus *bus_list)
{
	vmd_pci_bus *bus = bus_list;

	printf("\n ...PCIE devices attached to VMD %04x:%02x:%02x:%x...\n",
	       bus_list->vmd->pci.addr.domain, bus_list->vmd->pci.addr.bus,
	       bus_list->vmd->pci.addr.dev, bus_list->vmd->pci.addr.func);
	printf("----------------------------------------------\n");

	while (bus != NULL) {
		vmd_print_pci_info(bus->self);
		vmd_pci_device *dev = bus->dev_list;
		while (dev != NULL) {
			vmd_print_pci_info(dev);
			dev = dev->next;
		}
		bus = bus->next;
	}
}

uint8_t
vmd_scan_pcibus(vmd_pci_bus *bus)
{
	vmd_pci_bus *new_bus = bus;
	int dev_cnt;

	if (bus == NULL || bus->vmd == NULL) {
		return 0;
	}

	g_end_device_count = 0;
	vmd_add_bus_to_list(bus->vmd, new_bus);
	bus->vmd->next_bus_number = bus->bus_number + 1;
	dev_cnt = vmd_scan_single_bus(new_bus, NULL);
	printf(" **** VMD scan found %d devices\n", dev_cnt);
	printf("      VMD scan found %d END DEVICES\n", g_end_device_count);
	vmd_pci_print(bus->vmd->bus_list);
	return (uint8_t)dev_cnt;
}

bool
vmd_is_supported_device(vmd_pci_device *dev)
{
	bool isSupported = false;

	if (dev && dev->header
	    && (dev->class == PCI_CLASS_STORAGE_EXPRESS)
#ifndef SUPPORT_ALL_SSDS
	    && (dev->header->common.vendor_id == 0x8086)
#endif
	   ) {
		isSupported = true;
	}
	return isSupported;
}

SPDK_LOG_REGISTER_COMPONENT("vmd_pci", SPDK_LOG_VMD_PCI)
