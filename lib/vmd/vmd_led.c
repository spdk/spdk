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
 VMD LED     Attn       Power        LED Amber
 State       Indictor   Indicator
             Control    Control
 ------------------------------------------------
 Off        11b         11b         Off
 Ident      11b         01b         Blink 4Hz
 Fault      01b         11b         On
 Rebuild    01b         01b         Blink 1Hz
 */

#define VMD_LED_ATTN_INDICATOR_OFF         3
#define VMD_LED_ATTN_INDICATOR_IDENT       3
#define VMD_LED_ATTN_INDICATOR_FAULT       1
#define VMD_LED_ATTN_INDICATOR_REBUILD     1

#define VMD_LED_POWER_INDICATOR_OFF        3
#define VMD_LED_POWER_INDICATOR_IDENT      1
#define VMD_LED_POWER_INDICATOR_FAULT      3
#define VMD_LED_POWER_INDICATOR_REBUILD    1


static void set_attn_indicator_control(struct vmd_pci_device *dev, uint16_t value)
{
	if (dev && dev->pcie_cap) {
		printf("%s: write %d to attn indicator control\n", __func__, value);
		union express_slot_control_register slotCtrl;
		slotCtrl.as_uint16_t = dev->pcie_cap->slot_control.as_uint16_t;
		slotCtrl.bit_field.attention_indicator_control = value;
		dev->pcie_cap->slot_control.as_uint16_t = slotCtrl.as_uint16_t;
		slotCtrl.as_uint16_t = dev->pcie_cap->slot_control.as_uint16_t;
	} else {
		printf("dev or dev->pcie_cap is null\n");
	}
}

static void set_power_indicator_control(struct vmd_pci_device *dev, uint16_t value)
{
	if (dev && dev->pcie_cap) {
		printf("%s: write %d to power indicator control\n", __func__, value);
		union express_slot_control_register slotCtrl;
		slotCtrl.as_uint16_t = dev->pcie_cap->slot_control.as_uint16_t;
		slotCtrl.bit_field.power_indicator_control = value;
		dev->pcie_cap->slot_control.as_uint16_t = slotCtrl.as_uint16_t;
		slotCtrl.as_uint16_t = dev->pcie_cap->slot_control.as_uint16_t;
	} else {
		printf("dev or dev->pcie_cap is null\n");
	}
}

static VMD_LED_STATE led_get_state(struct vmd_pci_device *dev)
{
	VMD_LED_STATE state = VMD_LED_STATE_UNSUPPORTED;
	if (!dev) {
		return state;
	}

	union express_slot_control_register slotCtrl = dev->pcie_cap->slot_control;
	uint16_t attnIndicator = slotCtrl.bit_field.attention_indicator_control;
	uint16_t powerIndicator = slotCtrl.bit_field.power_indicator_control;
	printf("attnIndicator = 0x%x\n", attnIndicator);
	printf("powerIndicator = 0x%x\n", powerIndicator);

	if (attnIndicator == VMD_LED_ATTN_INDICATOR_OFF &&
	    powerIndicator == VMD_LED_POWER_INDICATOR_OFF) {
		return VMD_LED_STATE_OFF;
	} else if (attnIndicator == VMD_LED_ATTN_INDICATOR_IDENT &&
		   powerIndicator == VMD_LED_POWER_INDICATOR_IDENT) {
		return VMD_LED_STATE_IDENT;
	} else if (attnIndicator == VMD_LED_ATTN_INDICATOR_FAULT &&
		   powerIndicator == VMD_LED_POWER_INDICATOR_FAULT) {
		return VMD_LED_STATE_FAULT;
	} else if (attnIndicator == VMD_LED_ATTN_INDICATOR_REBUILD &&
		   powerIndicator == VMD_LED_POWER_INDICATOR_REBUILD) {
		return VMD_LED_STATE_REBUILD;
	} else {
		printf("LED state is not supported\n");
		return VMD_LED_STATE_UNSUPPORTED;
	}
	return state;
}


static bool led_set_state(struct vmd_pci_device *bus_dev, VMD_LED_STATE state)
{
	if (!bus_dev) {
		return false;
	}

	switch (state) {
	case VMD_LED_STATE_OFF:
		set_attn_indicator_control(bus_dev, VMD_LED_ATTN_INDICATOR_OFF);
		set_power_indicator_control(bus_dev, VMD_LED_POWER_INDICATOR_OFF);
		break;
	case VMD_LED_STATE_IDENT:
		set_attn_indicator_control(bus_dev, VMD_LED_ATTN_INDICATOR_IDENT);
		set_power_indicator_control(bus_dev, VMD_LED_POWER_INDICATOR_IDENT);
		break;
	case VMD_LED_STATE_FAULT:
		set_attn_indicator_control(bus_dev, VMD_LED_ATTN_INDICATOR_FAULT);
		set_power_indicator_control(bus_dev, VMD_LED_POWER_INDICATOR_FAULT);
		break;
	case VMD_LED_STATE_REBUILD:
		set_attn_indicator_control(bus_dev, VMD_LED_ATTN_INDICATOR_REBUILD);
		set_power_indicator_control(bus_dev, VMD_LED_POWER_INDICATOR_REBUILD);
		break;
	default:
		return false;
	}
	return true;
}


/***********************************************************************************
 * This function takes an domain-BDF input for the SSD that needs the slot LED state updated.
 * The identifying device under VMD is located in the global list of VMD controllers.
 * If the BDF identifies an endpoint, then the LED is attached to the endpoint's parent.
 * If the BDF identifies a type 1 header, then this device has the corresponding LED. This may
 * arise when a user wants to identify a given empty slot under VMD.
 * Inputs:
 *      addr - spdk pci address format of the device to locate the LED
 *      state - state to set the LED to
 *********************************************************************************
 */
bool vmd_led_set_state(struct spdk_pci_addr *addr, VMD_LED_STATE state)
{
	struct vmd_pci_device *dev = spdk_vmd_find_dev(addr);
	if (!dev) {
		return false;
	}

	if (dev->header_type == PCI_HEADER_TYPE_NORMAL) {
		if (dev->parent) {
			dev = dev->parent->self;
		} else {
			return false;
		}
	}

	return led_set_state(dev, state);
}

VMD_LED_STATE vmd_led_get_state(struct spdk_pci_addr *addr)
{
	struct vmd_pci_device *dev = spdk_vmd_find_dev(addr);
	if (!dev) {
		return false;
	}

	if (dev->header_type == PCI_HEADER_TYPE_NORMAL) {
		if (dev->parent) {
			dev = dev->parent->self;
		} else {
			return false;
		}
	}

	return led_get_state(dev);
}
