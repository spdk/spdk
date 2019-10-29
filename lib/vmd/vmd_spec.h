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


#ifndef VMD_SPEC_H
#define VMD_SPEC_H

#define MAX_VMD_SUPPORTED 48  /* max number of vmd controllers in a system - */
#define VMD_DOMAIN_START 0x201D

#define PCI_INVALID_VENDORID 0xFFFF
#define ONE_MB (1<<20)
#define PCI_OFFSET_OF(object, member)  ((uint32_t)&((object*)0)->member)
#define TWOS_COMPLEMENT(value) (~(value) + 1)

#define VMD_UPPER_BASE_SIGNATURE  0xFFFFFFEF
#define VMD_UPPER_LIMIT_SIGNATURE 0xFFFFFFED

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
#define RESERVED_HOTPLUG_BUSES 1
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

struct pci_enhanced_capability_header {
	uint16_t capability_id;
	uint16_t version: 4;
	uint16_t next: 12;
};

struct serial_number_capability {
	struct pci_enhanced_capability_header hdr;
	uint32_t sn_low;
	uint32_t sn_hi;
};

struct pci_header_common {
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
};

struct pci_header_zero {
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
};

struct pci_header_one {
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
};

struct pci_capabilities_header {
	uint8_t   capability_id;
	uint8_t   next;
};

/*
 * MSI capability structure for msi interrupt vectors
 */
#define MAX_MSIX_TABLE_SIZE 0x800
#define MSIX_ENTRY_VECTOR_CTRL_MASKBIT 1
#define PORT_INT_VECTOR  0;
#define CLEAR_MSIX_DESTINATION_ID 0xfff00fff
struct pci_msi_cap {
	struct pci_capabilities_header header;
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

struct pcix_table_pointer {
	union {
		struct {
			uint32_t BaseIndexRegister : 3;
			uint32_t Reserved : 29;
		} TableBIR;
		uint32_t  TableOffset;
	};
};

struct pci_msix_capability {
	struct pci_capabilities_header header;
	union _MsixControl {
		uint16_t as_uint16_t;
		struct msg_ctrl {
			uint16_t table_size : 11;
			uint16_t reserved : 3;
			uint16_t function_mask : 1;
			uint16_t msix_enable : 1;
		} bit;
	} message_control;

	struct pcix_table_pointer message_table;
	struct pcix_table_pointer   pba_table;
};

struct pci_msix_table_entry {
	volatile uint32_t  message_addr_lo;
	volatile uint32_t  message_addr_hi;
	volatile uint32_t  message_data;
	volatile uint32_t  vector_control;
};

/*
 * Pci express capability
 */
enum PciExpressCapabilities {
	/* 0001b Legacy PCI Express Endpoint            */
	LegacyEndpoint       = 0x1,
	/* 0000b PCI Express Endpoint                   */
	ExpressEndpoint      = 0x0,
	/* 0100b Root Port of PCI Express Root Complex* */
	RootComplexRootPort  = 0x4,
	/* 0101b Upstream Port of PCI Express Switch*   */
	SwitchUpstreamPort   = 0x5,
	/* 0110b Downstream Port of PCI Express Switch* */
	SwitchDownStreamPort = 0x6,
	/* 0111b PCI Express to PCI/PCI-X Bridge*       */
	ExpressToPciBridge   = 0x7,
	/* 1000b PCI/PCI-X to PCI Express Bridge*       */
	PciToExpressBridge   = 0x8,
	/* 1001b Root Complex Integrated Endpoint       */
	RCIntegratedEndpoint = 0x9,
	/* 1010b Root Complex Event Collector           */
	RootComplexEventCollector = 0xa,
	InvalidCapability = 0xff
};

union express_capability_register {
	struct {
		uint16_t capability_version : 4;
		uint16_t device_type : 4;
		uint16_t slot_implemented : 1;
		uint16_t interrupt_message_number : 5;
		uint16_t rsv : 2;
	} bit_field;
	uint16_t as_uint16_t;
};

union express_slot_capabilities_register {
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
};

union express_slot_control_register {
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
};

union express_slot_status_register {
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
};

union express_root_control_register {
	struct {
		uint16_t CorrectableSerrEnable : 1;
		uint16_t NonFatalSerrEnable : 1;
		uint16_t FatalSerrEnable : 1;
		uint16_t PMEInterruptEnable : 1;
		uint16_t CRSSoftwareVisibilityEnable : 1;
		uint16_t Rsvd : 11;
	} bit_field;
	uint16_t as_uint16_t;
};

union express_link_capability_register {
	struct {
		uint32_t maximum_link_speed : 4;
		uint32_t maximum_link_width : 6;
		uint32_t active_state_pms_support : 2;
		uint32_t l0_exit_latency : 3;
		uint32_t l1_exit_latency : 3;
		uint32_t clock_power_management : 1;
		uint32_t surprise_down_error_reporting_capable : 1;
		uint32_t datalink_layer_active_reporting_capable : 1;
		uint32_t link_bandwidth_notification_capability : 1;
		uint32_t aspm_optionality_compliance : 1;
		uint32_t rsvd : 1;
		uint32_t port_number : 8;
	} bit_field;
	uint32_t as_uint32_t;
};

union express_link_control_register {
	struct {
		uint16_t active_state_pm_control : 2;
		uint16_t rsvd1 : 1;
		uint16_t read_completion_boundary : 1;
		uint16_t link_disable : 1;
		uint16_t retrain_link : 1;
		uint16_t common_clock_config : 1;
		uint16_t extended_synch : 1;
		uint16_t enable_clock_power_management : 1;
		uint16_t rsvd2 : 7;
	} bit_field;
	uint16_t as_uint16_t;
};

union express_link_status_register {
	struct {
		uint16_t link_speed : 4;
		uint16_t link_width : 6;
		uint16_t undefined : 1;
		uint16_t link_training : 1;
		uint16_t slot_clock_config : 1;
		uint16_t datalink_layer_active : 1;
		uint16_t asvd : 2;
	} bit_field;
	uint16_t as_uint16_t;
};

struct pci_express_cap {
	uint8_t capid;
	uint8_t next_cap;
	union express_capability_register express_cap_register;
	uint32_t device_cap;
	uint16_t device_control;
	uint16_t device_status;
	union express_link_capability_register link_cap;
	union express_link_control_register link_control;
	union express_link_status_register link_status;
	union express_slot_capabilities_register slot_cap;
	union express_slot_control_register slot_control;
	union express_slot_status_register slot_status;
	uint32_t root_status;
	uint32_t deviceCap2;
	uint16_t deviceControl2;
	uint16_t deviceStatus2;
	uint32_t linkCap2;
	uint16_t linkControl2;
	uint16_t linkStatus2;
	uint32_t slotCap2;
	uint16_t slotControl2;
	uint16_t slotStatus2;
};

struct pci_msix_cap {
	uint8_t   cap_idd;
	uint8_t   next_cap;
	uint16_t  msg_control_reg;
	uint32_t  msix_table_offset;
	uint32_t  pba_offset;
};

struct pci_header {
	union {
		struct pci_header_common common;
		struct pci_header_zero zero;
		struct pci_header_one one;
	};
};

#endif /* VMD_SPEC_H */
