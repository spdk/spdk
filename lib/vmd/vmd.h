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

#ifndef VMD_H
#define VMD_H

#include "spdk/stdinc.h"
#include "spdk/vmd.h"
#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/log.h"
#include "vmd_spec.h"

struct vmd_hot_plug;
struct vmd_adapter;
struct vmd_pci_device;

struct pci_bars {
	uint64_t vaddr;
	uint64_t start;
	uint32_t size;
};

struct vmd_pci_bus {
	struct vmd_adapter *vmd;
	struct vmd_pci_bus *parent;	/* parent bus that this bus is attached to(primary bus. */
	struct vmd_pci_device *self;		/* Pci device that describes this bus(bar, bus numbers, etc */

	uint32_t  domain           : 8;
	uint32_t  hotplug_buses    : 10;
	uint32_t  is_added         : 1;
	uint32_t  hp_event_queued  : 1;
	uint32_t  rsv              : 12;

	uint32_t  bus_number      : 8;
	uint32_t  primary_bus     : 8;
	uint32_t  secondary_bus   : 8;
	uint32_t  subordinate_bus : 8;

	TAILQ_HEAD(, vmd_pci_device) dev_list;	/* list of pci end device attached to this bus */
	TAILQ_ENTRY(vmd_pci_bus) tailq;		/* link for all buses found during scan */
};

/*
 * memory element for base address assignment and reuse
 */
struct pci_mem_mgr {
	uint32_t			size : 30;        /* size of memory element */
	uint32_t			in_use : 1;
	uint32_t			rsv : 1;
	uint64_t			addr;
	TAILQ_ENTRY(pci_mem_mgr)	tailq;
};

struct vmd_hot_plug {
	uint32_t count  : 12;
	uint32_t reserved_bus_count : 4;
	uint32_t max_hotplug_bus_number : 8;
	uint32_t next_bus_number : 8;
	struct pci_bars bar;
	union express_slot_status_register slot_status;
	struct pci_mem_mgr mem[ADDR_ELEM_COUNT];
	uint8_t bus_numbers[RESERVED_HOTPLUG_BUSES];
	struct vmd_pci_bus *bus;
	TAILQ_HEAD(, pci_mem_mgr) free_mem_queue;
	TAILQ_HEAD(, pci_mem_mgr) alloc_mem_queue;
	TAILQ_HEAD(, pci_mem_mgr) unused_mem_queue;
};

struct vmd_pci_device {
	struct spdk_pci_device pci;
	struct pci_bars bar[6];

	struct vmd_pci_device *parent_bridge;
	struct vmd_pci_bus *bus, *parent;
	struct vmd_pci_bus *bus_object;  /* bus tracks pci bus associated with this dev if type 1 dev. */
	struct vmd_pci_bus *subordinate;
	volatile struct pci_header *header;
	volatile struct pci_express_cap *pcie_cap;
	volatile struct pci_msix_capability *msix_cap;
	volatile struct pci_msi_cap *msi_cap;
	volatile struct serial_number_capability *sn_cap;
	volatile struct pci_msix_table_entry *msix_table;

	TAILQ_ENTRY(vmd_pci_device) tailq;

	uint32_t  class;
	uint16_t  vid;
	uint16_t  did;
	uint16_t  pcie_flags, msix_table_size;
	uint32_t  devfn;
	bool      hotplug_capable;

	uint32_t  header_type    : 1;
	uint32_t  multifunction  : 1;
	uint32_t  hotplug_bridge : 1;
	uint32_t  is_added       : 1;
	uint32_t  is_hooked      : 1;
	uint32_t  rsv1           : 12;
	uint32_t  target         : 16;

	struct vmd_hot_plug hp;
	/* Cached version of the slot_control register */
	union express_slot_control_register cached_slot_control;
};

/*
 * The VMD adapter
 */
struct vmd_adapter {
	struct spdk_pci_device *pci;
	uint32_t domain;
	/* physical and virtual VMD bars */
	uint64_t cfgbar, cfgbar_size;
	uint64_t membar, membar_size;
	uint64_t msixbar, msixbar_size;
	volatile uint8_t *cfg_vaddr;
	volatile uint8_t *mem_vaddr;
	volatile uint8_t *msix_vaddr;
	volatile struct pci_msix_table_entry *msix_table;
	uint32_t bar_sizes[6];

	uint64_t physical_addr;
	uint32_t current_addr_size;

	uint32_t next_bus_number : 10;
	uint32_t max_pci_bus : 10;
	uint32_t is_hotplug_scan : 1;
	uint32_t is_ready : 1;
	uint32_t processing_hp : 1;
	uint32_t max_payload_size: 3;
	uint32_t root_port_updated : 1;
	uint32_t scan_completed : 1;
	uint32_t rsv : 4;

	/* end devices attached to vmd adapters */
	struct vmd_pci_device *target[MAX_VMD_TARGET];
	uint32_t  dev_count  : 16;
	uint32_t  nvme_count : 8;
	uint32_t  vmd_index  : 8;

	struct vmd_pci_bus vmd_bus;

	TAILQ_HEAD(, vmd_pci_bus) bus_list;

	struct event_fifo *hp_queue;
};

/* TODO: Temporary stubs for Hot Plug interface */
static inline struct vmd_pci_bus *
vmd_is_dev_in_hotplug_path(struct vmd_pci_device *dev)
{
	return NULL;
}

static inline void
vmd_hp_enable_hotplug(struct vmd_hot_plug *hp)
{

}

static inline uint8_t
vmd_hp_get_next_bus_number(struct vmd_hot_plug *hp)
{
	assert(false);
	return 0;
}

struct vmd_pci_device *vmd_find_device(const struct spdk_pci_addr *addr);

#endif /* VMD_H */
