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
#pragma once

#include "spdk/stdinc.h"
#include "spdk/vmd.h"
#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk_internal/log.h"

#define SUPPPORT_ALL_SSDS

#define MAX_VMD_SUPPORTED 48  /* max number of vmd controllers in a system - */
/* up to 6 per cpu */
#define MAX_VMD_TARGET  24
#define VMD_DOMAIN_START 0x201D

#define PCI_INVALID_VENDORID 0xFFFF
#define ONE_MB (1<<20)
#define PCI_OFFSET_OF(object, member)  ((uint32_t)&((object*)0)->member)
#define TWOS_COMPLEMENT(value) (~(value) + 1)

/*
 *  BAR assignment constants
 */
#define  PCI_DWORD_SHIFT            32
#define  PCI_BASE_ADDR_MASK         0xFFFFFFF0
#define  PCI_BAR_MEMORY_MASK        0x0000000F
#define  PCI_BAR_MEMORY_MEM_IND     0x1
#define  PCI_BAR_MEMORY_TYPE        0x6
#define  PCI_BAR_MEMORY_PREFETCH    0x8
#define  PCI_BAR_MEMORY_TYPE_32     0x0
#define  PCI_BAR_MEMORY_TYPE_64     0x4
#define  PCI_BAR_MB_MASK            0xFFFFF
#define  PCI_PCI_BRIDGE_ADDR_DEF    0xFFF0
#define  PCI_BRIDGE_MEMORY_MASK     0xFFF0
#define  PCI_BRIDGE_PREFETCH_64     0x0001
#define  PCI_BRIDGE_MEMORY_SHIFT    16
#define  PCI_CONFIG_ACCESS_DELAY    500

#define PCI_MAX_CFG_SIZE            0x1000

#define PCI_HEADER_TYPE             0x0e
#define PCI_HEADER_TYPE_NORMAL   0
#define PCI_HEADER_TYPE_BRIDGE   1
#define PCI_MULTI_FUNCTION 0x80

#define PCI_COMMAND_MEMORY 0x2
#define PCI_COMMAND_MASTER 0x4

#define PCIE_TYPE_FLAGS 0xf0
#define PCIE_TYPE_SHIFT 4
#define PCIE_TYPE_ROOT_PORT 0x4
#define PCIE_TYPE_DOWNSTREAM 0x6

#define PCI_CLASS_STORAGE_EXPRESS   0x010802
#define ADDR_ELEM_COUNT 32
#define PCI_MAX_BUS_NUMBER 0x7F
#define RESERVED_HOTPLUG_BUSES 0
#define isHotPlugCapable(slotCap)  ((slotCap) & (1<<6))
#define CONFIG_OFFSET_ADDR(bus, device, function, reg) (((bus)<<20) | (device)<<15 | (function<<12) | (reg))
#define BRIDGE_BASEREG(reg)  (0xFFF0 & ((reg)>>16))

#define MISCCTRLSTS_0_OFFSET  0x188
#define ENABLE_ACPI_MODE_FOR_HOTPLUG  (1 << 3)

/* Bit encodings for Command Register */
#define IO_SPACE_ENABLE               0x0001
#define MEMORY_SPACE_ENABLE           0x0002
#define BUS_MASTER_ENABLE             0x0004

/* Bit encodings for Status Register */
#define PCI_CAPABILITIES_LIST        0x0010
#define PCI_RECEIVED_TARGET_ABORT    0x1000
#define PCI_RECEIVED_MASTER_ABORT    0x2000
#define PCI_SIGNALED_SYSTEM_ERROR    0x4000
#define PCI_DETECTED_PARITY_ERROR    0x8000

/* Capability IDs */
#define CAPABILITY_ID_POWER_MANAGEMENT  0x01
#define CAPABILITY_ID_MSI   0x05
#define CAPABILITY_ID_PCI_EXPRESS   0x10
#define CAPABILITY_ID_MSIX  0x11

#define  PCI_MSIX_ENABLE (1 << 15)          /* bit 15 of MSIX Message Control */
#define  PCI_MSIX_FUNCTION_MASK (1 << 14)   /* bit 14 of MSIX Message Control */

/* extended capability */
#define EXTENDED_CAPABILITY_OFFSET 0x100
#define DEVICE_SERIAL_NUMBER_CAP_ID  0x3

#define BAR_SIZE (1 << 20)

typedef struct _enhanced_capability_hdr {
	uint16_t capability_id;
	uint16_t version: 4;
	uint16_t next: 12;
} pci_enhanced_capability_header;

typedef struct _SerialNumberCapability {
	pci_enhanced_capability_header hdr;
	uint32_t sn_low;
	uint32_t sn_hi;
} serial_number_capability;


typedef struct _PciHeaderCommon {
	uint16_t  vendor_id;
	uint16_t  device_id;
	uint16_t  command;
	uint16_t  status;
	uint32_t  rev_class;
	uint8_t   cache_line_size;
	uint8_t   master_lat_timer;
	uint8_t   header_type;
	uint8_t   BIST;
	uint8_t   rsvd12[36];
	uint8_t   cap_pointer;
	uint8_t   rsvd53[7];
	uint8_t   int_line;
	uint8_t   int_pin;
	uint8_t   rsvd62[2];
} pci_header_common;

typedef struct _PciTypeZeroHeader {
	uint16_t  vendor_id;
	uint16_t  device_id;
	uint16_t  command;
	uint16_t  status;
	uint32_t  rev_class;
	uint8_t   cache_line_size;
	uint8_t   master_lat_timer;
	uint8_t   header_type;
	uint8_t   BIST;
	uint32_t  BAR[6];
	uint32_t  carbus_cis_pointer;
	uint16_t  ssvid;
	uint16_t  ssid;
	uint32_t  exp_rom_base_addr;
	uint8_t   cap_pointer;
	uint8_t   rsvd53[7];
	uint8_t   intLine;
	uint8_t   int_pin;
	uint8_t   min_gnt;
	uint8_t   max_lat;
} pci_header_zero;

typedef struct _PciTypeOneHeader {
	uint16_t  vendor_id;
	uint16_t  device_id;
	uint16_t  command;
	uint16_t  status;
	uint32_t  rev_class;
	uint8_t   cache_line_size;
	uint8_t   master_lat_timer;
	uint8_t   header_type;
	uint8_t   BIST;
	uint32_t  BAR[2];
	uint8_t   primary;
	uint8_t   secondary;
	uint8_t   subordinate;
	uint8_t   secondary_lat_timer;
	uint8_t   io_base;
	uint8_t   io_limit;
	uint16_t  secondary_status;
	uint16_t  mem_base;
	uint16_t  mem_limit;
	uint16_t  prefetch_base;
	uint16_t  prefetch_limit;
	uint32_t  prefetch_base_upper;
	uint32_t  prefetch_limit_upper;
	uint16_t  io_base_upper;
	uint16_t  io_limit_upper;
	uint8_t   cap_pointer;
	uint8_t   rsvd53[3];
	uint32_t  exp_romBase_addr;
	uint8_t   int_line;
	uint8_t   int_pin;
	uint16_t  bridge_control;
} pci_header_one;


typedef struct _PciCapHdr {
	uint8_t   capability_id;
	uint8_t   next;
} pci_capabilities_header;

/*
 * MSI capability structure for msi interrupt vectors
 */
#define MAX_MSIX_TABLE_SIZE 0x800
#define MSIX_ENTRY_VECTOR_CTRL_MASKBIT 1
#define PORT_INT_VECTOR  0;
#define CLEAR_MSIX_DESTINATION_ID 0xfff00fff
struct PCI_MSI_CAPABILITY {
	pci_capabilities_header header;
	union _MsiControl {
		uint16_t as_uint16_t;
		struct _PCI_MSI_MESSAGE_CONTROL {
			uint16_t msi_enable : 1;
			uint16_t multiple_message_capable : 3;
			uint16_t multiple_message_enable : 3;
			uint16_t capable_of_64bits : 1;
			uint16_t per_vector_mask_capable : 1;
			uint16_t reserved : 7;
		} bit;
	} message_control;
	union {
		struct _PCI_MSI_MESSAGE_ADDRESS {
			uint32_t reserved : 2;
			uint32_t address : 30;
		} reg;
		uint32_t  raw;
	} message_address_lower;
	union {
		struct _Option32_bit {
			uint16_t message_data;
		} option32_bit;
		struct _Option64_bit {
			uint32_t  message_address_upper;
			uint16_t  message_data;
			uint16_t  reserved;
			uint32_t  mask_bits;
			uint32_t  pending_bits;
		} option64_bit;
	};
};
typedef struct PCI_MSI_CAPABILITY pci_msi_cap;

typedef struct PcixTablePointer {
	union {
		struct {
			uint32_t BaseIndexRegister : 3;
			uint32_t Reserved : 29;
		} TableBIR;
		uint32_t  TableOffset;
	};
} pcix_table_pointer;


typedef struct _PciMsixCapability {
	struct _PciCapHdr header;
	union _MsixControl {
		uint16_t as_uint16_t;
		struct msg_ctrl {
			uint16_t table_size : 11;
			uint16_t reserved : 3;
			uint16_t function_mask : 1;
			uint16_t msix_enable : 1;
		} bit;
	} message_control;

	pcix_table_pointer message_table;
	pcix_table_pointer   pba_table;
} pci_msix_capability;


typedef struct _pci_misx_table_entry {
	volatile uint32_t  message_addr_lo;
	volatile uint32_t  message_addr_hi;
	volatile uint32_t  message_data;
	volatile uint32_t  vector_control;
} pci_msix_table_entry;

/*
 * Pci express capability
 */
enum PciExpressCapabilities {
	LegacyEndpoint       = 0x1,				/* 0001b Legacy PCI Express Endpoint            */
	ExpressEndpoint      = 0x0,				/* 0000b PCI Express Endpoint                   */
	RootComplexRootPort  = 0x4,				/* 0100b Root Port of PCI Express Root Complex* */
	SwitchUpstreamPort   = 0x5,				/* 0101b Upstream Port of PCI Express Switch*   */
	SwitchDownStreamPort = 0x6,				/* 0110b Downstream Port of PCI Express Switch* */
	ExpressToPciBridge   = 0x7,				/* 0111b PCI Express to PCI/PCI-X Bridge*       */
	PciToExpressBridge   = 0x8,				/* 1000b PCI/PCI-X to PCI Express Bridge*       */
	RCIntegratedEndpoint = 0x9,				/* 1001b Root Complex Integrated Endpoint       */
	RootComplexEventCollector = 0xa,                    /* 1010b Root Complex Event Collector           */
	InvalidCapability = 0xff
};

typedef union _EXPRESS_CAPABILITIES_REGISTER {
	struct {
		uint16_t capability_version : 4;
		uint16_t device_type : 4;
		uint16_t slot_implemented : 1;
		uint16_t interrupt_message_number : 5;
		uint16_t rsv : 2;
	} bit_field;
	uint16_t as_uint16_t;
} express_capability_register;

typedef union _EXPRESS_DEVICE_CAPABILITIES_REGISTER {
	struct {
		uint32_t max_payload_size_supported : 3;
		uint32_t phantom_functions_supported : 2;
		uint32_t extended_tag_supported : 1;
		uint32_t L0s_acceptable_latency : 3;
		uint32_t L1_acceptable_latency : 3;
		uint32_t undefined : 3;
		uint32_t role_based_error_reporting : 1;
		uint32_t rsvd1 : 2;
		uint32_t captured_slot_power_limit : 8;
		uint32_t captured_slot_power_limit_scale : 2;
		uint32_t rsvd2 : 4;
	} bit_field;
	uint32_t as_uint32_t;
} express_device_capability_register;

/*
 * The low 3 bits of the PCI Express device control register dictate whether
 * a device that implements AER routes error messages to the root complex.
 * This mask is used when programming the AER bits in the device control
 * register.
 */
#define EXPRESS_AER_DEVICE_CONTROL_MASK 0x07;
typedef union _EXPRESS_DEVICE_CONTROL_REGISTER {
	struct {
		uint16_t correctable_error_enable : 1;
		uint16_t non_fatal_error_enable : 1;
		uint16_t fatal_error_enable : 1;
		uint16_t unsupported_request_error_enable : 1;
		uint16_t enable_relaxed_order : 1;
		uint16_t max_payload_size : 3;
		uint16_t extended_tag_enable : 1;
		uint16_t phantom_functions_enable : 1;
		uint16_t aux_power_enable : 1;
		uint16_t no_snoop_enable : 1;
		uint16_t max_read_request_size : 3;
		uint16_t bridge_config_retry_enable : 1;
	} bit_field;
	uint16_t as_uint16_t;;
} express_device_control_register;

/*
 * The low 4 bits of the PCI Express device status register hold AER device
 * status. This mask is used when programming the AER bits in the device status
 * register.
 */
#define EXPRESS_AER_DEVICE_STATUS_MASK 0x0F;
typedef union _EXPRESS_DEVICE_STATUS_REGISTER {
	struct {
		uint16_t CorrectableErrorDetected : 1;
		uint16_t NonFatalErrorDetected : 1;
		uint16_t FatalErrorDetected : 1;
		uint16_t UnsupportedRequestDetected : 1;
		uint16_t AuxPowerDetected : 1;
		uint16_t TransactionsPending : 1;
		uint16_t Rsvd : 10;
	} bit_field;
	uint16_t Asuint16_t;
} express_device_status_register;

typedef union _EXPRESS_LINK_CAPABILITIES_REGISTER {
	struct {
		uint32_t MaximumLinkSpeed : 4;
		uint32_t MaximumLinkWidth : 6;
		uint32_t ActiveStatePMSupport : 2;
		uint32_t L0sExitLatency : 3;
		uint32_t L1ExitLatency : 3;
		uint32_t ClockPowerManagement : 1;
		uint32_t SurpriseDownErrorReportingCapable : 1;
		uint32_t DataLinkLayerActiveReportingCapable : 1;
		uint32_t LinkBandwidthNotificationCapability : 1;
		uint32_t AspmOptionalityCompliance : 1;
		uint32_t Rsvd : 1;
		uint32_t PortNumber : 8;
	} bit_field;
	uint32_t Asuint32_t;
} express_link_capability_register;

typedef union _EXPRESS_LINK_CONTROL_REGISTER {
	struct {
		uint16_t ActiveStatePMControl : 2;
		uint16_t Rsvd1 : 1;
		uint16_t ReadCompletionBoundary : 1;
		uint16_t LinkDisable : 1;
		uint16_t RetrainLink : 1;
		uint16_t CommonClockConfig : 1;
		uint16_t ExtendedSynch : 1;
		uint16_t EnableClockPowerManagement : 1;
		uint16_t Rsvd2 : 7;
	} bit_field;
	uint16_t Asuint16_t;
} express_link_control_register;

typedef union _EXPRESS_LINK_STATUS_REGISTER {
	struct {
		uint16_t LinkSpeed : 4;
		uint16_t LinkWidth : 6;
		uint16_t Undefined : 1;
		uint16_t LinkTraining : 1;
		uint16_t SlotClockConfig : 1;
		uint16_t DataLinkLayerActive : 1;
		uint16_t Rsvd : 2;
	} bitField;
	uint16_t Asuint16_t;
} express_link_status_register;

typedef union _EXPRESS_SLOT_CAPABILITIES_REGISTER {
	struct {
		uint32_t attention_button_present : 1;
		uint32_t power_controller_present : 1;
		uint32_t MRL_sensor_present : 1;
		uint32_t attention_indicator_present : 1;
		uint32_t power_indicator_present : 1;
		uint32_t hotplug_surprise : 1;
		uint32_t hotplug_capable : 1;
		uint32_t slot_power_limit : 8;
		uint32_t slotPower_limit_scale : 2;
		uint32_t electromechanical_lock_present : 1;
		uint32_t no_command_completed_support : 1;
		uint32_t physical_slot_number : 13;
	} bit_field;
	uint32_t as_uint32_t;
} express_slot_capabiliies_register;

typedef union _EXPRESS_SLOT_CONTROL_REGISTER {
	struct {
		uint16_t attention_button_enable : 1;
		uint16_t power_fault_detect_enable : 1;
		uint16_t MRLsensor_enable : 1;
		uint16_t presence_detect_enable : 1;
		uint16_t command_completed_enable : 1;
		uint16_t hotplug_interrupt_enable : 1;
		uint16_t attention_indicator_control : 2;
		uint16_t power_indicator_control : 2;
		uint16_t power_controller_control : 1;
		uint16_t electromechanical_lockcontrol : 1;
		uint16_t datalink_state_change_enable : 1;
		uint16_t Rsvd : 3;
	} bit_field;
	uint16_t as_uint16_t;
} express_slot_control_register;

typedef union _EXPRESS_SLOT_STATUS_REGISTER {
	struct {
		uint16_t attention_button_pressed : 1;
		uint16_t power_fault_detected : 1;
		uint16_t MRL_sensor_changed : 1;
		uint16_t presence_detect_changed : 1;
		uint16_t command_completed : 1;
		uint16_t MRL_sensor_state : 1;
		uint16_t presence_detect_state : 1;
		uint16_t electromechanical_lock_engaged : 1;
		uint16_t datalink_state_changed : 1;
		uint16_t rsvd : 7;
	} bit_field;
	uint16_t as_uint16_t;
} express_slot_status_register;

typedef union _EXPRESS_ROOT_CONTROL_REGISTER {
	struct {
		uint16_t CorrectableSerrEnable : 1;
		uint16_t NonFatalSerrEnable : 1;
		uint16_t FatalSerrEnable : 1;
		uint16_t PMEInterruptEnable : 1;
		uint16_t CRSSoftwareVisibilityEnable : 1;
		uint16_t Rsvd : 11;
	} bit_field;
	uint16_t as_uint16_t;
} express_root_control_register;


typedef struct _PciExpressCap {
	uint8_t   capid;
	uint8_t   next_cap;
	express_capability_register  express_cap_register;
	uint32_t  device_cap;
	uint16_t  device_control;
	uint16_t  device_status;
	uint32_t  link_cap;
	uint16_t  link_control;
	uint16_t  link_status;
	express_slot_capabiliies_register slot_cap;
	express_slot_control_register slot_control;
	express_slot_status_register  slot_status;
	uint32_t root_status;
	uint32_t deviceCap2;
	uint16_t  deviceControl2;
	uint16_t  deviceStatus2;
	uint32_t	linkCap2;
	uint16_t  linkControl2;
	uint16_t  linkStatus2;
	uint32_t	slotCap2;
	uint16_t  slotControl2;
	uint16_t  slotStatus2;
} pci_express_cap;

typedef struct _PciMsixCap {
	uint8_t   cap_idd;
	uint8_t   next_cap;
	uint16_t	msg_control_reg;
	uint32_t	msix_table_offset;
	uint32_t	pba_offset;
} pci_msix_cap;

typedef struct _PciHeader {
	union {
		pci_header_common common;
		pci_header_zero zero;
		pci_header_one one;
	};
} pci_header;

struct pci_bars {
	uint64_t vaddr;
	uint64_t start;
	uint32_t size;
};


/*
 * **********************************************************************************
 * PciBus object
 */
/* forward declarations */
struct vmd_hot_plug;
struct vmd_adapter;
struct vmd_pci_device;

typedef struct vmd_pci_bus {
	struct vmd_adapter *vmd;
	struct vmd_pci_bus *parent;		/* parent bus that this bus is attached to(primary bus. */
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

	struct vmd_pci_device *dev_list;     /* list of pci end device attached to this bus */
	struct vmd_pci_bus *next;            /* link for all buses found during scan */
} vmd_pci_bus;

typedef struct vmd_pci_device {
	struct spdk_pci_device pci;
	struct pci_bars bar[6];

	struct vmd_pci_device *parent_bridge, *next;
	vmd_pci_bus *bus, *parent;
	vmd_pci_bus *bus_object;  /* bus tracks pci bus associated with this dev if type 1 dev. */
	vmd_pci_bus *subordinate;
	volatile pci_header *header;
	volatile pci_express_cap *pcie_cap;
	volatile pci_msix_capability *msix_cap;
	volatile pci_msi_cap *msi_cap;
	volatile serial_number_capability *sn_cap;
	volatile pci_msix_table_entry *msix_table;

	uint32_t  class;
	uint16_t  vid;
	uint16_t  did;
	uint16_t  pcie_flags, msix_table_size;
	uint32_t  devfn;

	uint32_t  header_type    : 1;
	uint32_t  multifunction  : 1;
	uint32_t  hotplug_bridge : 1;
	uint32_t  is_added       : 1;
	uint32_t  is_hooked      : 1;
	uint32_t  rsv1           : 12;
	uint32_t  target         : 16;

	struct vmd_hot_plug *hp;
} vmd_pci_device;


/*
 * memory element for base address assignment and reuse
 */
struct pci_mem_mgr {
	uint32_t size : 30;        /* size of memory element */
	uint32_t in_use : 1;
	uint32_t rsv : 1;
	uint64_t addr;
};

typedef struct vmd_hot_plug {
	uint32_t count  : 12;
	uint32_t reserved_bus_count : 4;
	uint32_t max_hotplug_bus_number : 8;
	uint32_t next_bus_number : 8;
	uint32_t addr_size;
	uint64_t physical_addr;
	express_slot_status_register slot_status;
	struct pci_mem_mgr mem[ADDR_ELEM_COUNT];
	uint8_t bus_numbers[RESERVED_HOTPLUG_BUSES];
	vmd_pci_bus *bus;
} vmd_hot_plug;


/*
 * *****************************************************************************
 * The VMD adapter
 */
typedef struct vmd_adapter {
	struct spdk_pci_device pci;
	uint32_t domain;
	/* physical and virtual VMD bars */
	uint64_t cfgbar, cfgbar_size;
	uint64_t membar, membar_size;
	uint64_t msixbar, msixbar_size;
	volatile uint8_t *cfg_vaddr;
	volatile uint8_t  *mem_vaddr;
	volatile uint8_t  *msix_vaddr;
	volatile struct _pci_misx_table_entry *msix_table;
	uint32_t bar_sizes[6];

	uint64_t physical_addr;
	uint32_t current_addr_size;

	uint32_t next_bus_number : 10;
	uint32_t max_pci_bus : 10;
	uint32_t is_hotplug_scan : 1;
	uint32_t is_ready : 1;
	uint32_t processing_hp : 1;
	uint32_t max_payload_size: 3;
	uint32_t rsv : 6;

	/* end devices attached to vmd adapters */
	struct vmd_pci_device *target[MAX_VMD_TARGET];
	uint32_t  dev_count  : 16;
	uint32_t  nvme_count : 8;
	uint32_t  vmd_index  : 8;

	struct vmd_pci_bus vmd_bus, *bus_list;

	struct event_fifo *hp_queue;

} vmd_adapter;

/*
 * Container for all VMD adapter probed in the system.
 */
typedef struct vmd_container {
	uint32_t is_initialized: 8;
	uint32_t count : 16;
	uint32_t rsv : 8;
	struct spdk_pci_addr *vmd_target_addr; /* can target specific vmd or  */
	/* all vmd when null           */
	vmd_adapter vmd[MAX_VMD_SUPPORTED];
} vmd_container;

/*
 * Hot Plug interface
 */
vmd_pci_bus *hp_get_hotplug_port_changed(void *v, unsigned int index);
void hp_free_base_addrs(struct vmd_hot_plug *hp, vmd_pci_device *device);
void hp_free_bus_number(struct vmd_hot_plug *hp, uint8_t bus_number);
bool bus_is_hotplug_enabled(vmd_pci_bus *bus);
bool bus_is_hot_insert(vmd_pci_bus *bus);
bool bus_is_hot_removal(vmd_pci_bus *bus);
void hp_clear_slot_status(vmd_pci_device *dev);
vmd_pci_bus *is_dev_in_hotplug_path(vmd_pci_device *dev);
vmd_pci_bus *is_bus_in_hotplug_path(vmd_pci_bus *bus);
uint8_t hp_get_next_bus_number(struct vmd_hot_plug *hp);
void hp_DrainQueue(void *context);
void hp_align_base_addrs(struct vmd_hot_plug *hp, uint32_t alignment);
uint64_t hp_allocate_base_addr(struct vmd_hot_plug *hp, uint32_t size);
void hp_remove_hotplug_devices(vmd_pci_bus *bus);
void hp_enable_hotplug(struct vmd_hot_plug *hp);
struct vmd_hot_plug *new_hotplug(vmd_pci_bus *newBus, uint8_t reservedBuses);
void hp_remove_device(vmd_pci_bus *hpbus, vmd_pci_device *dev);

/*
 * Pci interface
 */
void    enable_msi(vmd_pci_device *dev);
void    disable_msi(vmd_pci_device *dev);
bool    is_end_device(vmd_pci_device *dev);
bool    is_supported_device(vmd_pci_device *dev);
bool    is_valid_pci_device(vmd_pci_device *dev);

uint8_t scan_single_bus(vmd_pci_bus *bus, vmd_pci_device *parentBridge);
void    update_base_limit_register(vmd_pci_device *dev, uint16_t base, uint16_t limit);
bool    pci_bus_remove_device(vmd_pci_bus *bus, vmd_pci_device *device);
void    init_port_interrupt(vmd_pci_device *dev, volatile pci_msix_table_entry *entry);
uint8_t vmd_scan_pcibus(vmd_pci_bus *bus);
void    vmd_spdk_dev_init(vmd_pci_device *dev);
