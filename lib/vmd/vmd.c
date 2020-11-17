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
#include "spdk/likely.h"

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

/*
 * Container for all VMD adapter probed in the system.
 */
struct vmd_container {
	uint32_t count;
	struct vmd_adapter vmd[MAX_VMD_SUPPORTED];
};

static struct vmd_container g_vmd_container;
static uint8_t g_end_device_count;

static bool
vmd_is_valid_cfg_addr(struct vmd_pci_bus *bus, uint64_t addr)
{
	return addr >= (uint64_t)bus->vmd->cfg_vaddr &&
	       addr < bus->vmd->cfgbar_size + (uint64_t)bus->vmd->cfg_vaddr;
}

static void
vmd_align_base_addrs(struct vmd_adapter *vmd, uint32_t alignment)
{
	uint32_t pad;

	/*
	 *  Device is not in hot plug path, align the base address remaining from membar 1.
	 */
	if (vmd->physical_addr & (alignment - 1)) {
		pad = alignment - (vmd->physical_addr & (alignment - 1));
		vmd->physical_addr += pad;
		vmd->current_addr_size -= pad;
	}
}

static bool
vmd_device_is_enumerated(const struct vmd_pci_device *vmd_device)
{
	return vmd_device->header->one.prefetch_base_upper == VMD_UPPER_BASE_SIGNATURE &&
	       vmd_device->header->one.prefetch_limit_upper == VMD_UPPER_LIMIT_SIGNATURE;
}

static bool
vmd_device_is_root_port(const struct vmd_pci_device *vmd_device)
{
	return vmd_device->header->common.vendor_id == 0x8086 &&
	       (vmd_device->header->common.device_id == 0x2030 ||
		vmd_device->header->common.device_id == 0x2031 ||
		vmd_device->header->common.device_id == 0x2032 ||
		vmd_device->header->common.device_id == 0x2033);
}

static void
vmd_hotplug_coalesce_regions(struct vmd_hot_plug *hp)
{
	struct pci_mem_mgr *region, *prev;

	do {
		prev = NULL;
		TAILQ_FOREACH(region, &hp->free_mem_queue, tailq) {
			if (prev != NULL && (prev->addr + prev->size == region->addr)) {
				break;
			}

			prev = region;
		}

		if (region != NULL) {
			prev->size += region->size;
			TAILQ_REMOVE(&hp->free_mem_queue, region, tailq);
			TAILQ_INSERT_TAIL(&hp->unused_mem_queue, region, tailq);
		}
	} while (region != NULL);
}

static void
vmd_hotplug_free_region(struct vmd_hot_plug *hp, struct pci_mem_mgr *region)
{
	struct pci_mem_mgr *current, *prev = NULL;

	assert(region->addr >= hp->bar.start && region->addr < hp->bar.start + hp->bar.size);

	TAILQ_FOREACH(current, &hp->free_mem_queue, tailq) {
		if (current->addr > region->addr) {
			break;
		}

		prev = current;
	}

	if (prev != NULL) {
		assert(prev->addr + prev->size <= region->addr);
		assert(current == NULL || (region->addr + region->size <= current->addr));
		TAILQ_INSERT_AFTER(&hp->free_mem_queue, prev, region, tailq);
	} else {
		TAILQ_INSERT_HEAD(&hp->free_mem_queue, region, tailq);
	}

	vmd_hotplug_coalesce_regions(hp);
}

static void
vmd_hotplug_free_addr(struct vmd_hot_plug *hp, uint64_t addr)
{
	struct pci_mem_mgr *region;

	TAILQ_FOREACH(region, &hp->alloc_mem_queue, tailq) {
		if (region->addr == addr) {
			break;
		}
	}

	assert(region != NULL);
	TAILQ_REMOVE(&hp->alloc_mem_queue, region, tailq);

	vmd_hotplug_free_region(hp, region);
}

static uint64_t
vmd_hotplug_allocate_base_addr(struct vmd_hot_plug *hp, uint32_t size)
{
	struct pci_mem_mgr *region = NULL, *free_region;

	TAILQ_FOREACH(region, &hp->free_mem_queue, tailq) {
		if (region->size >= size) {
			break;
		}
	}

	if (region == NULL) {
		SPDK_DEBUGLOG(vmd, "Unable to find free hotplug memory region of size:"
			      "%"PRIx32"\n", size);
		return 0;
	}

	TAILQ_REMOVE(&hp->free_mem_queue, region, tailq);
	if (size < region->size) {
		free_region = TAILQ_FIRST(&hp->unused_mem_queue);
		if (free_region == NULL) {
			SPDK_DEBUGLOG(vmd, "Unable to find unused descriptor to store the "
				      "free region of size: %"PRIu32"\n", region->size - size);
		} else {
			TAILQ_REMOVE(&hp->unused_mem_queue, free_region, tailq);
			free_region->size = region->size - size;
			free_region->addr = region->addr + size;
			region->size = size;
			vmd_hotplug_free_region(hp, free_region);
		}
	}

	TAILQ_INSERT_TAIL(&hp->alloc_mem_queue, region, tailq);

	return region->addr;
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
vmd_allocate_base_addr(struct vmd_adapter *vmd, struct vmd_pci_device *dev, uint32_t size)
{
	uint64_t base_address = 0, padding = 0;
	struct vmd_pci_bus *hp_bus;

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
		hp_bus = dev->parent;
		if (hp_bus && hp_bus->self && hp_bus->self->hotplug_capable) {
			return vmd_hotplug_allocate_base_addr(&hp_bus->self->hp, size);
		}
	}

	/* Ensure physical membar allocated is size aligned */
	if (vmd->physical_addr & (size - 1)) {
		padding = size - (vmd->physical_addr & (size - 1));
	}

	/* Allocate from membar if enough memory is left */
	if (vmd->current_addr_size >= size + padding) {
		base_address = vmd->physical_addr + padding;
		vmd->physical_addr += size + padding;
		vmd->current_addr_size -= size + padding;
	}

	SPDK_DEBUGLOG(vmd, "allocated(size) %" PRIx64 " (%x)\n", base_address, size);

	return base_address;
}

static bool
vmd_is_end_device(struct vmd_pci_device *dev)
{
	return (dev && dev->header) &&
	       ((dev->header->common.header_type & ~PCI_MULTI_FUNCTION) == PCI_HEADER_TYPE_NORMAL);
}

static void
vmd_update_base_limit_register(struct vmd_pci_device *dev, uint16_t base, uint16_t limit)
{
	struct vmd_pci_bus *bus;
	struct vmd_pci_device *bridge;

	if (base == 0 ||  limit == 0) {
		return;
	}

	if (dev->header->common.header_type == PCI_HEADER_TYPE_BRIDGE) {
		bus = dev->bus_object;
	} else {
		bus = dev->parent;
	}

	bridge = bus->self;
	SPDK_DEBUGLOG(vmd, "base:limit = %x:%x\n", bridge->header->one.mem_base,
		      bridge->header->one.mem_limit);

	if (dev->bus->vmd->scan_completed) {
		return;
	}

	while (bus && bus->self != NULL) {
		bridge = bus->self;

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

static uint64_t
vmd_get_base_addr(struct vmd_pci_device *dev, uint32_t index, uint32_t size)
{
	struct vmd_pci_bus *bus = dev->parent;

	if (dev->header_type == PCI_HEADER_TYPE_BRIDGE) {
		return dev->header->zero.BAR[index] & ~0xf;
	} else {
		if (bus->self->hotplug_capable) {
			return vmd_hotplug_allocate_base_addr(&bus->self->hp, size);
		} else {
			return (uint64_t)bus->self->header->one.mem_base << 16;
		}
	}
}

static bool
vmd_assign_base_addrs(struct vmd_pci_device *dev)
{
	uint16_t mem_base = 0, mem_limit = 0;
	unsigned char mem_attr = 0;
	int last;
	struct vmd_adapter *vmd = NULL;
	bool ret_val = false;
	uint32_t bar_value;
	uint32_t table_offset;

	if (dev && dev->bus) {
		vmd = dev->bus->vmd;
	}

	if (!vmd) {
		return 0;
	}

	vmd_align_base_addrs(vmd, ONE_MB);

	last = dev->header_type ? 2 : 6;
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

		if (vmd->scan_completed) {
			dev->bar[i].start = vmd_get_base_addr(dev, i, dev->bar[i].size);
		} else {
			dev->bar[i].start = vmd_allocate_base_addr(vmd, dev, dev->bar[i].size);
		}

		dev->header->zero.BAR[i] = (uint32_t)dev->bar[i].start;

		if (!dev->bar[i].start) {
			if (mem_attr == (PCI_BAR_MEMORY_PREFETCH | PCI_BAR_MEMORY_TYPE_64)) {
				i++;
			}
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
		table_offset = ((volatile struct pci_msix_cap *)dev->msix_cap)->msix_table_offset;
		if (dev->bar[table_offset & 0x3].vaddr) {
			dev->msix_table = (volatile struct pci_msix_table_entry *)
					  (dev->bar[table_offset & 0x3].vaddr + (table_offset & 0xfff8));
		}
	}

	if (ret_val && vmd_is_end_device(dev)) {
		vmd_update_base_limit_register(dev, mem_base, mem_limit);
	}

	return ret_val;
}

static void
vmd_get_device_capabilities(struct vmd_pci_device *dev)

{
	volatile uint8_t *config_space;
	uint8_t capabilities_offset;
	struct pci_capabilities_header *capabilities_hdr;

	config_space = (volatile uint8_t *)dev->header;
	if ((dev->header->common.status  & PCI_CAPABILITIES_LIST) == 0) {
		return;
	}

	capabilities_offset = dev->header->zero.cap_pointer;
	if (dev->header->common.header_type & PCI_HEADER_TYPE_BRIDGE) {
		capabilities_offset = dev->header->one.cap_pointer;
	}

	while (capabilities_offset > 0) {
		capabilities_hdr = (struct pci_capabilities_header *)
				   &config_space[capabilities_offset];
		switch (capabilities_hdr->capability_id) {
		case CAPABILITY_ID_PCI_EXPRESS:
			dev->pcie_cap = (volatile struct pci_express_cap *)(capabilities_hdr);
			break;

		case CAPABILITY_ID_MSI:
			dev->msi_cap = (volatile struct pci_msi_cap *)capabilities_hdr;
			break;

		case CAPABILITY_ID_MSIX:
			dev->msix_cap = (volatile struct pci_msix_capability *)capabilities_hdr;
			dev->msix_table_size = dev->msix_cap->message_control.bit.table_size + 1;
			break;

		default:
			break;
		}
		capabilities_offset = capabilities_hdr->next;
	}
}

static volatile struct pci_enhanced_capability_header *
vmd_get_enhanced_capabilities(struct vmd_pci_device *dev, uint16_t capability_id)
{
	uint8_t *data;
	uint16_t cap_offset = EXTENDED_CAPABILITY_OFFSET;
	volatile struct pci_enhanced_capability_header *cap_hdr = NULL;

	data = (uint8_t *)dev->header;
	while (cap_offset >= EXTENDED_CAPABILITY_OFFSET) {
		cap_hdr = (volatile struct pci_enhanced_capability_header *) &data[cap_offset];
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
vmd_read_config_space(struct vmd_pci_device *dev)
{
	/*
	 * Writes to the pci config space is posted weite. To ensure transaction reaches its destination
	 * before another write is posed, an immediate read of the written value should be performed.
	 */
	dev->header->common.command |= (BUS_MASTER_ENABLE | MEMORY_SPACE_ENABLE);
	{ uint16_t cmd = dev->header->common.command; (void)cmd; }

	vmd_get_device_capabilities(dev);
	dev->sn_cap = (struct serial_number_capability *)vmd_get_enhanced_capabilities(dev,
			DEVICE_SERIAL_NUMBER_CAP_ID);
}

static void
vmd_update_scan_info(struct vmd_pci_device *dev)
{
	struct vmd_adapter *vmd_adapter = dev->bus->vmd;

	if (vmd_adapter->root_port_updated) {
		return;
	}

	if (dev->header_type == PCI_HEADER_TYPE_NORMAL) {
		return;
	}

	if (vmd_device_is_root_port(dev)) {
		vmd_adapter->root_port_updated = 1;
		SPDK_DEBUGLOG(vmd, "root_port_updated = %d\n",
			      vmd_adapter->root_port_updated);
		SPDK_DEBUGLOG(vmd, "upper:limit = %x : %x\n",
			      dev->header->one.prefetch_base_upper,
			      dev->header->one.prefetch_limit_upper);
		if (vmd_device_is_enumerated(dev)) {
			vmd_adapter->scan_completed = 1;
			SPDK_DEBUGLOG(vmd, "scan_completed = %d\n",
				      vmd_adapter->scan_completed);
		}
	}
}

static void
vmd_reset_base_limit_registers(struct vmd_pci_device *dev)
{
	uint32_t reg __attribute__((unused));

	assert(dev->header_type != PCI_HEADER_TYPE_NORMAL);
	/*
	 * Writes to the pci config space are posted writes.
	 * To ensure transaction reaches its destination
	 * before another write is posted, an immediate read
	 * of the written value should be performed.
	 */
	dev->header->one.mem_base = 0xfff0;
	reg = dev->header->one.mem_base;
	dev->header->one.mem_limit = 0x0;
	reg = dev->header->one.mem_limit;
	dev->header->one.prefetch_base = 0x0;
	reg = dev->header->one.prefetch_base;
	dev->header->one.prefetch_limit = 0x0;
	reg = dev->header->one.prefetch_limit;
	dev->header->one.prefetch_base_upper = 0x0;
	reg = dev->header->one.prefetch_base_upper;
	dev->header->one.prefetch_limit_upper = 0x0;
	reg = dev->header->one.prefetch_limit_upper;
	dev->header->one.io_base_upper = 0x0;
	reg = dev->header->one.io_base_upper;
	dev->header->one.io_limit_upper = 0x0;
	reg = dev->header->one.io_limit_upper;
	dev->header->one.primary = 0;
	reg = dev->header->one.primary;
	dev->header->one.secondary = 0;
	reg = dev->header->one.secondary;
	dev->header->one.subordinate = 0;
	reg = dev->header->one.subordinate;
}

static void
vmd_init_hotplug(struct vmd_pci_device *dev, struct vmd_pci_bus *bus)
{
	struct vmd_adapter *vmd = bus->vmd;
	struct vmd_hot_plug *hp = &dev->hp;
	size_t mem_id;

	dev->hotplug_capable = true;
	hp->bar.size = 1 << 20;

	if (!vmd->scan_completed) {
		hp->bar.start = vmd_allocate_base_addr(vmd, NULL, hp->bar.size);
		bus->self->header->one.mem_base = BRIDGE_BASEREG(hp->bar.start);
		bus->self->header->one.mem_limit =
			bus->self->header->one.mem_base + BRIDGE_BASEREG(hp->bar.size - 1);
	} else {
		hp->bar.start = (uint64_t)bus->self->header->one.mem_base << 16;
	}

	hp->bar.vaddr = (uint64_t)vmd->mem_vaddr + (hp->bar.start - vmd->membar);

	TAILQ_INIT(&hp->free_mem_queue);
	TAILQ_INIT(&hp->unused_mem_queue);
	TAILQ_INIT(&hp->alloc_mem_queue);

	hp->mem[0].size = hp->bar.size;
	hp->mem[0].addr = hp->bar.start;

	TAILQ_INSERT_TAIL(&hp->free_mem_queue, &hp->mem[0], tailq);

	for (mem_id = 1; mem_id < ADDR_ELEM_COUNT; ++mem_id) {
		TAILQ_INSERT_TAIL(&hp->unused_mem_queue, &hp->mem[mem_id], tailq);
	}

	SPDK_DEBUGLOG(vmd, "%s: mem_base:mem_limit = %x : %x\n", __func__,
		      bus->self->header->one.mem_base, bus->self->header->one.mem_limit);
}

static bool
vmd_bus_device_present(struct vmd_pci_bus *bus, uint32_t devfn)
{
	volatile struct pci_header *header;

	header = (volatile struct pci_header *)(bus->vmd->cfg_vaddr +
						CONFIG_OFFSET_ADDR(bus->bus_number, devfn, 0, 0));
	if (!vmd_is_valid_cfg_addr(bus, (uint64_t)header)) {
		return false;
	}

	if (header->common.vendor_id == PCI_INVALID_VENDORID || header->common.vendor_id == 0x0) {
		return false;
	}

	return true;
}

static struct vmd_pci_device *
vmd_alloc_dev(struct vmd_pci_bus *bus, uint32_t devfn)
{
	struct vmd_pci_device *dev = NULL;
	struct pci_header volatile *header;
	uint8_t header_type;
	uint32_t rev_class;

	/* Make sure we're not creating two devices on the same dev/fn */
	TAILQ_FOREACH(dev, &bus->dev_list, tailq) {
		if (dev->devfn == devfn) {
			return NULL;
		}
	}

	if (!vmd_bus_device_present(bus, devfn)) {
		return NULL;
	}

	header = (struct pci_header * volatile)(bus->vmd->cfg_vaddr +
						CONFIG_OFFSET_ADDR(bus->bus_number, devfn, 0, 0));

	SPDK_DEBUGLOG(vmd, "PCI device found: %04x:%04x ***\n",
		      header->common.vendor_id, header->common.device_id);

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
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
		vmd_update_scan_info(dev);
		if (!dev->bus->vmd->scan_completed) {
			vmd_reset_base_limit_registers(dev);
		}
	}

	vmd_read_config_space(dev);

	return dev;
}

static struct vmd_pci_bus *
vmd_create_new_bus(struct vmd_pci_bus *parent, struct vmd_pci_device *bridge, uint8_t bus_number)
{
	struct vmd_pci_bus *new_bus;

	new_bus = calloc(1, sizeof(*new_bus));
	if (!new_bus) {
		return NULL;
	}

	new_bus->parent = parent;
	new_bus->domain = parent->domain;
	new_bus->bus_number = bus_number;
	new_bus->secondary_bus = new_bus->subordinate_bus = bus_number;
	new_bus->self = bridge;
	new_bus->vmd = parent->vmd;
	TAILQ_INIT(&new_bus->dev_list);

	bridge->subordinate = new_bus;

	bridge->pci.addr.bus = new_bus->bus_number;
	bridge->pci.addr.dev = bridge->devfn;
	bridge->pci.addr.func = 0;
	bridge->pci.addr.domain = parent->vmd->pci->addr.domain;

	return new_bus;
}

/*
 * Assigns a bus number from the list of available
 * bus numbers. If the device is downstream of a hot plug port,
 * assign the bus number from thiose assigned to the HP port. Otherwise,
 * assign the next bus number from the vmd bus number list.
 */
static uint8_t
vmd_get_next_bus_number(struct vmd_pci_device *dev, struct vmd_adapter *vmd)
{
	uint8_t bus = 0xff;
	struct vmd_pci_bus *hp_bus;

	if (dev) {
		hp_bus = vmd_is_dev_in_hotplug_path(dev);
		if (hp_bus && hp_bus->self && hp_bus->self->hotplug_capable) {
			return vmd_hp_get_next_bus_number(&hp_bus->self->hp);
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
vmd_get_hotplug_bus_numbers(struct vmd_pci_device *dev)
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
vmd_enable_msix(struct vmd_pci_device *dev)
{
	volatile uint16_t control;

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
vmd_disable_msix(struct vmd_pci_device *dev)
{
	volatile uint16_t control;

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
vmd_setup_msix(struct vmd_pci_device *dev, volatile struct pci_msix_table_entry *vmdEntry)
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
vmd_bus_update_bridge_info(struct vmd_pci_device *bridge)
{
	/* Update the subordinate bus of all bridges above this bridge */
	volatile struct vmd_pci_device *dev = bridge;
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

static bool
vmd_is_supported_device(struct vmd_pci_device *dev)
{
	return dev->class == PCI_CLASS_STORAGE_EXPRESS;
}

static int
vmd_dev_map_bar(struct spdk_pci_device *pci_dev, uint32_t bar,
		void **mapped_addr, uint64_t *phys_addr, uint64_t *size)
{
	struct vmd_pci_device *dev = SPDK_CONTAINEROF(pci_dev, struct vmd_pci_device, pci);

	*size = dev->bar[bar].size;
	*phys_addr = dev->bar[bar].start;
	*mapped_addr = (void *)dev->bar[bar].vaddr;

	return 0;
}

static int
vmd_dev_unmap_bar(struct spdk_pci_device *_dev, uint32_t bar, void *addr)
{
	return 0;
}

static int
vmd_dev_cfg_read(struct spdk_pci_device *_dev, void *value, uint32_t len,
		 uint32_t offset)
{
	struct vmd_pci_device *dev = SPDK_CONTAINEROF(_dev, struct vmd_pci_device, pci);
	volatile uint8_t *src = (volatile uint8_t *)dev->header;
	uint8_t *dst = value;
	size_t i;

	if (len + offset > PCI_MAX_CFG_SIZE) {
		return -1;
	}

	for (i = 0; i < len; ++i) {
		dst[i] = src[offset + i];
	}

	return 0;
}

static int
vmd_dev_cfg_write(struct spdk_pci_device *_dev,  void *value,
		  uint32_t len, uint32_t offset)
{
	struct vmd_pci_device *dev = SPDK_CONTAINEROF(_dev, struct vmd_pci_device, pci);
	volatile uint8_t *dst = (volatile uint8_t *)dev->header;
	uint8_t *src = value;
	size_t i;

	if ((len + offset) > PCI_MAX_CFG_SIZE) {
		return -1;
	}

	for (i = 0; i < len; ++i) {
		dst[offset + i] = src[i];
	}

	return 0;
}

static void
vmd_dev_detach(struct spdk_pci_device *dev)
{
	struct vmd_pci_device *vmd_device = (struct vmd_pci_device *)dev;
	struct vmd_pci_device *bus_device = vmd_device->bus->self;
	struct vmd_pci_bus *bus = vmd_device->bus;
	size_t i, num_bars = vmd_device->header_type ? 2 : 6;

	spdk_pci_unhook_device(dev);
	TAILQ_REMOVE(&bus->dev_list, vmd_device, tailq);

	/* Release the hotplug region if the device is under hotplug-capable bus */
	if (bus_device && bus_device->hotplug_capable) {
		for (i = 0; i < num_bars; ++i) {
			if (vmd_device->bar[i].start != 0) {
				vmd_hotplug_free_addr(&bus_device->hp, vmd_device->bar[i].start);
			}
		}
	}

	free(dev);
}

static void
vmd_dev_init(struct vmd_pci_device *dev)
{
	uint8_t bdf[32];

	dev->pci.addr.domain = dev->bus->vmd->domain;
	dev->pci.addr.bus = dev->bus->bus_number;
	dev->pci.addr.dev = dev->devfn;
	dev->pci.addr.func = 0;
	dev->pci.id.vendor_id = dev->header->common.vendor_id;
	dev->pci.id.device_id = dev->header->common.device_id;
	dev->pci.type = "vmd";
	dev->pci.map_bar = vmd_dev_map_bar;
	dev->pci.unmap_bar = vmd_dev_unmap_bar;
	dev->pci.cfg_read = vmd_dev_cfg_read;
	dev->pci.cfg_write = vmd_dev_cfg_write;
	dev->hotplug_capable = false;
	if (dev->pcie_cap != NULL) {
		dev->cached_slot_control = dev->pcie_cap->slot_control;
	}

	if (vmd_is_supported_device(dev)) {
		spdk_pci_addr_fmt(bdf, sizeof(bdf), &dev->pci.addr);
		SPDK_DEBUGLOG(vmd, "Initalizing NVMe device at %s\n", bdf);
		dev->pci.parent = dev->bus->vmd->pci;
		spdk_pci_hook_device(spdk_pci_nvme_get_driver(), &dev->pci);
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
vmd_scan_single_bus(struct vmd_pci_bus *bus, struct vmd_pci_device *parent_bridge)
{
	/* assuming only single function devices are on the bus */
	struct vmd_pci_device *new_dev;
	struct vmd_adapter *vmd;
	union express_slot_capabilities_register slot_cap;
	struct vmd_pci_bus *new_bus;
	uint8_t  device_number, dev_cnt = 0;
	uint8_t new_bus_num;

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

			if (slot_cap.bit_field.hotplug_capable && new_dev->pcie_cap != NULL &&
			    new_dev->pcie_cap->express_cap_register.bit_field.slot_implemented) {
				new_bus->hotplug_buses = vmd_get_hotplug_bus_numbers(new_dev);
				new_bus->subordinate_bus += new_bus->hotplug_buses;

				/* Attach hot plug instance if HP is supported */
				/* Hot inserted SSDs can be assigned port bus of sub-ordinate + 1 */
				SPDK_DEBUGLOG(vmd, "hotplug_capable/slot_implemented = "
					      "%x:%x\n", slot_cap.bit_field.hotplug_capable,
					      new_dev->pcie_cap->express_cap_register.bit_field.slot_implemented);
			}

			new_dev->parent_bridge = parent_bridge;
			new_dev->header->one.primary = new_bus->primary_bus;
			new_dev->header->one.secondary = new_bus->secondary_bus;
			new_dev->header->one.subordinate = new_bus->subordinate_bus;

			vmd_bus_update_bridge_info(new_dev);
			TAILQ_INSERT_TAIL(&bus->vmd->bus_list, new_bus, tailq);

			vmd_dev_init(new_dev);

			if (slot_cap.bit_field.hotplug_capable && new_dev->pcie_cap != NULL &&
			    new_dev->pcie_cap->express_cap_register.bit_field.slot_implemented) {
				vmd_init_hotplug(new_dev, new_bus);
			}

			dev_cnt += vmd_scan_single_bus(new_bus, new_dev);
			if (new_dev->pcie_cap != NULL) {
				if (new_dev->pcie_cap->express_cap_register.bit_field.device_type == SwitchUpstreamPort) {
					return dev_cnt;
				}
			}
		} else {
			/* Attach the device to the current bus and assign base addresses */
			TAILQ_INSERT_TAIL(&bus->dev_list, new_dev, tailq);
			g_end_device_count++;
			if (vmd_assign_base_addrs(new_dev)) {
				vmd_setup_msix(new_dev, &bus->vmd->msix_table[0]);
				vmd_dev_init(new_dev);
				if (vmd_is_supported_device(new_dev)) {
					vmd = bus->vmd;
					vmd->target[vmd->nvme_count] = new_dev;
					vmd->nvme_count++;
				}
			} else {
				SPDK_DEBUGLOG(vmd, "Removing failed device:%p\n", new_dev);
				TAILQ_REMOVE(&bus->dev_list, new_dev, tailq);
				free(new_dev);
				if (dev_cnt) {
					dev_cnt--;
				}
			}
		}
	}

	return dev_cnt;
}

static void
vmd_print_pci_info(struct vmd_pci_device *dev)
{
	if (!dev) {
		return;
	}

	if (dev->pcie_cap != NULL) {
		SPDK_INFOLOG(vmd, "PCI DEVICE: [%04X:%04X] type(%x) : %s\n",
			     dev->header->common.vendor_id, dev->header->common.device_id,
			     dev->pcie_cap->express_cap_register.bit_field.device_type,
			     device_type[dev->pcie_cap->express_cap_register.bit_field.device_type]);
	} else {
		SPDK_INFOLOG(vmd, "PCI DEVICE: [%04X:%04X]\n",
			     dev->header->common.vendor_id, dev->header->common.device_id);
	}

	SPDK_INFOLOG(vmd, "\tDOMAIN:BDF: %04x:%02x:%02x:%x\n", dev->pci.addr.domain,
		     dev->pci.addr.bus, dev->pci.addr.dev, dev->pci.addr.func);

	if (!(dev->header_type & PCI_HEADER_TYPE_BRIDGE) && dev->bus) {
		SPDK_INFOLOG(vmd, "\tbase addr: %x : %p\n",
			     dev->header->zero.BAR[0], (void *)dev->bar[0].vaddr);
	}

	if ((dev->header_type & PCI_HEADER_TYPE_BRIDGE)) {
		SPDK_INFOLOG(vmd, "\tPrimary = %d, Secondary = %d, Subordinate = %d\n",
			     dev->header->one.primary, dev->header->one.secondary, dev->header->one.subordinate);
		if (dev->pcie_cap && dev->pcie_cap->express_cap_register.bit_field.slot_implemented) {
			SPDK_INFOLOG(vmd, "\tSlot implemented on this device.\n");
			if (dev->pcie_cap->slot_cap.bit_field.hotplug_capable) {
				SPDK_INFOLOG(vmd, "Device has HOT-PLUG capable slot.\n");
			}
		}
	}

	if (dev->sn_cap != NULL) {
		uint8_t *snLow = (uint8_t *)&dev->sn_cap->sn_low;
		uint8_t *snHi = (uint8_t *)&dev->sn_cap->sn_hi;

		SPDK_INFOLOG(vmd, "\tSN: %02x-%02x-%02x-%02x-%02x-%02x-%02x-%02x\n",
			     snHi[3], snHi[2], snHi[1], snHi[0], snLow[3], snLow[2], snLow[1], snLow[0]);
	}
}

static void
vmd_cache_scan_info(struct vmd_pci_device *dev)
{
	uint32_t reg __attribute__((unused));

	if (dev->header_type == PCI_HEADER_TYPE_NORMAL) {
		return;
	}

	SPDK_DEBUGLOG(vmd, "vendor/device id:%x:%x\n", dev->header->common.vendor_id,
		      dev->header->common.device_id);

	if (vmd_device_is_root_port(dev)) {
		dev->header->one.prefetch_base_upper = VMD_UPPER_BASE_SIGNATURE;
		reg = dev->header->one.prefetch_base_upper;
		dev->header->one.prefetch_limit_upper = VMD_UPPER_LIMIT_SIGNATURE;
		reg = dev->header->one.prefetch_limit_upper;

		SPDK_DEBUGLOG(vmd, "prefetch: %x:%x\n",
			      dev->header->one.prefetch_base_upper,
			      dev->header->one.prefetch_limit_upper);
	}
}

static uint8_t
vmd_scan_pcibus(struct vmd_pci_bus *bus)
{
	struct vmd_pci_bus *bus_entry;
	struct vmd_pci_device *dev;
	uint8_t dev_cnt;

	g_end_device_count = 0;
	TAILQ_INSERT_TAIL(&bus->vmd->bus_list, bus, tailq);
	bus->vmd->next_bus_number = bus->bus_number + 1;
	dev_cnt = vmd_scan_single_bus(bus, NULL);

	SPDK_DEBUGLOG(vmd, "VMD scan found %u devices\n", dev_cnt);
	SPDK_DEBUGLOG(vmd, "VMD scan found %u END DEVICES\n", g_end_device_count);

	SPDK_INFOLOG(vmd, "PCIe devices attached to VMD %04x:%02x:%02x:%x...\n",
		     bus->vmd->pci->addr.domain, bus->vmd->pci->addr.bus,
		     bus->vmd->pci->addr.dev, bus->vmd->pci->addr.func);

	TAILQ_FOREACH(bus_entry, &bus->vmd->bus_list, tailq) {
		if (bus_entry->self != NULL) {
			vmd_print_pci_info(bus_entry->self);
			vmd_cache_scan_info(bus_entry->self);
		}

		TAILQ_FOREACH(dev, &bus_entry->dev_list, tailq) {
			vmd_print_pci_info(dev);
		}
	}

	return dev_cnt;
}

static int
vmd_map_bars(struct vmd_adapter *vmd, struct spdk_pci_device *dev)
{
	int rc;

	rc = spdk_pci_device_map_bar(dev, 0, (void **)&vmd->cfg_vaddr,
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
vmd_enumerate_devices(struct vmd_adapter *vmd)
{
	vmd->vmd_bus.vmd = vmd;
	vmd->vmd_bus.secondary_bus = vmd->vmd_bus.subordinate_bus = 0;
	vmd->vmd_bus.primary_bus = vmd->vmd_bus.bus_number = 0;
	vmd->vmd_bus.domain = vmd->pci->addr.domain;

	return vmd_scan_pcibus(&vmd->vmd_bus);
}

struct vmd_pci_device *
vmd_find_device(const struct spdk_pci_addr *addr)
{
	struct vmd_pci_bus *bus;
	struct vmd_pci_device *dev;
	int i;

	for (i = 0; i < MAX_VMD_TARGET; ++i) {
		TAILQ_FOREACH(bus, &g_vmd_container.vmd[i].bus_list, tailq) {
			if (bus->self) {
				if (spdk_pci_addr_compare(&bus->self->pci.addr, addr) == 0) {
					return bus->self;
				}
			}

			TAILQ_FOREACH(dev, &bus->dev_list, tailq) {
				if (spdk_pci_addr_compare(&dev->pci.addr, addr) == 0) {
					return dev;
				}
			}
		}
	}

	return NULL;
}

static int
vmd_enum_cb(void *ctx, struct spdk_pci_device *pci_dev)
{
	uint32_t cmd_reg = 0;
	char bdf[32] = {0};
	struct vmd_container *vmd_c = ctx;
	size_t i;

	spdk_pci_device_cfg_read32(pci_dev, &cmd_reg, 4);
	cmd_reg |= 0x6;                      /* PCI bus master/memory enable. */
	spdk_pci_device_cfg_write32(pci_dev, cmd_reg, 4);

	spdk_pci_addr_fmt(bdf, sizeof(bdf), &pci_dev->addr);
	SPDK_DEBUGLOG(vmd, "Found a VMD[ %d ] at %s\n", vmd_c->count, bdf);

	/* map vmd bars */
	i = vmd_c->count;
	vmd_c->vmd[i].pci = pci_dev;
	vmd_c->vmd[i].vmd_index = i;
	vmd_c->vmd[i].domain =
		(pci_dev->addr.bus << 16) | (pci_dev->addr.dev << 8) | pci_dev->addr.func;
	vmd_c->vmd[i].max_pci_bus = PCI_MAX_BUS_NUMBER;
	TAILQ_INIT(&vmd_c->vmd[i].bus_list);

	if (vmd_map_bars(&vmd_c->vmd[i], pci_dev) == -1) {
		return -1;
	}

	SPDK_DEBUGLOG(vmd, "vmd config bar(%p) vaddr(%p) size(%x)\n",
		      (void *)vmd_c->vmd[i].cfgbar, (void *)vmd_c->vmd[i].cfg_vaddr,
		      (uint32_t)vmd_c->vmd[i].cfgbar_size);
	SPDK_DEBUGLOG(vmd, "vmd mem bar(%p) vaddr(%p) size(%x)\n",
		      (void *)vmd_c->vmd[i].membar, (void *)vmd_c->vmd[i].mem_vaddr,
		      (uint32_t)vmd_c->vmd[i].membar_size);
	SPDK_DEBUGLOG(vmd, "vmd msix bar(%p) vaddr(%p) size(%x)\n\n",
		      (void *)vmd_c->vmd[i].msixbar, (void *)vmd_c->vmd[i].msix_vaddr,
		      (uint32_t)vmd_c->vmd[i].msixbar_size);

	vmd_c->count = i + 1;

	vmd_enumerate_devices(&vmd_c->vmd[i]);

	return 0;
}

int
spdk_vmd_pci_device_list(struct spdk_pci_addr vmd_addr, struct spdk_pci_device *nvme_list)
{
	int cnt = 0;
	struct vmd_pci_bus *bus;
	struct vmd_pci_device *dev;

	if (!nvme_list) {
		return -1;
	}

	for (int i = 0; i < MAX_VMD_TARGET; ++i) {
		if (spdk_pci_addr_compare(&vmd_addr, &g_vmd_container.vmd[i].pci->addr) == 0) {
			TAILQ_FOREACH(bus, &g_vmd_container.vmd[i].bus_list, tailq) {
				TAILQ_FOREACH(dev, &bus->dev_list, tailq) {
					nvme_list[cnt++] = dev->pci;
					if (!dev->is_hooked) {
						vmd_dev_init(dev);
						dev->is_hooked = 1;
					}
				}
			}
		}
	}

	return cnt;
}

static void
vmd_clear_hotplug_status(struct vmd_pci_bus *bus)
{
	struct vmd_pci_device *device = bus->self;
	uint16_t status __attribute__((unused));

	status = device->pcie_cap->slot_status.as_uint16_t;
	device->pcie_cap->slot_status.as_uint16_t = status;
	status = device->pcie_cap->slot_status.as_uint16_t;

	status = device->pcie_cap->link_status.as_uint16_t;
	device->pcie_cap->link_status.as_uint16_t = status;
	status = device->pcie_cap->link_status.as_uint16_t;
}

static void
vmd_bus_handle_hotplug(struct vmd_pci_bus *bus)
{
	uint8_t num_devices, sleep_count;

	for (sleep_count = 0; sleep_count < 20; ++sleep_count) {
		/* Scan until a new device is found */
		num_devices = vmd_scan_single_bus(bus, bus->self);
		if (num_devices > 0) {
			break;
		}

		spdk_delay_us(200000);
	}

	if (num_devices == 0) {
		SPDK_ERRLOG("Timed out while scanning for hotplugged devices\n");
	}
}

static void
vmd_bus_handle_hotremove(struct vmd_pci_bus *bus)
{
	struct vmd_pci_device *device, *tmpdev;

	TAILQ_FOREACH_SAFE(device, &bus->dev_list, tailq, tmpdev) {
		if (!vmd_bus_device_present(bus, device->devfn)) {
			device->pci.internal.pending_removal = true;

			/* If the device isn't attached, remove it immediately */
			if (!device->pci.internal.attached) {
				vmd_dev_detach(&device->pci);
			}
		}
	}
}

int
spdk_vmd_hotplug_monitor(void)
{
	struct vmd_pci_bus *bus;
	struct vmd_pci_device *device;
	int num_hotplugs = 0;
	uint32_t i;

	for (i = 0; i < g_vmd_container.count; ++i) {
		TAILQ_FOREACH(bus, &g_vmd_container.vmd[i].bus_list, tailq) {
			device = bus->self;
			if (device == NULL || !device->hotplug_capable) {
				continue;
			}

			if (device->pcie_cap->slot_status.bit_field.datalink_state_changed != 1) {
				continue;
			}

			if (device->pcie_cap->link_status.bit_field.datalink_layer_active == 1) {
				SPDK_DEBUGLOG(vmd, "Device hotplug detected on bus "
					      "%"PRIu32"\n", bus->bus_number);
				vmd_bus_handle_hotplug(bus);
			} else {
				SPDK_DEBUGLOG(vmd, "Device hotremove detected on bus "
					      "%"PRIu32"\n", bus->bus_number);
				vmd_bus_handle_hotremove(bus);
			}

			vmd_clear_hotplug_status(bus);
			num_hotplugs++;
		}
	}

	return num_hotplugs;
}

int
spdk_vmd_init(void)
{
	return spdk_pci_enumerate(spdk_pci_vmd_get_driver(), vmd_enum_cb, &g_vmd_container);
}

void
spdk_vmd_fini(void)
{
	uint32_t i;

	for (i = 0; i < g_vmd_container.count; ++i) {
		spdk_pci_device_detach(g_vmd_container.vmd[i].pci);
	}
}

SPDK_LOG_REGISTER_COMPONENT(vmd)
