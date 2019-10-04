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

#include "spdk/stdinc.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "vmd.h"

struct vmd_led_indicator_config {
	uint8_t attention_indicator	: 2;
	uint8_t power_indicator		: 2;
	uint8_t reserved		: 4;
};

/*
 * VMD LED     Attn       Power       LED Amber
 * State       Indicator  Indicator
 *             Control    Control
 * ------------------------------------------------
 * Off         11b        11b         Off
 * Ident       11b        01b         Blink 4Hz
 * Fault       01b        11b         On
 * Rebuild     01b        01b         Blink 1Hz
 */
static const struct vmd_led_indicator_config g_led_config[] = {
	[SPDK_VMD_LED_STATE_OFF]	= { .attention_indicator = 3, .power_indicator = 3 },
	[SPDK_VMD_LED_STATE_IDENTIFY]	= { .attention_indicator = 3, .power_indicator = 1 },
	[SPDK_VMD_LED_STATE_FAULT]	= { .attention_indicator = 1, .power_indicator = 3 },
	[SPDK_VMD_LED_STATE_REBUILD]	= { .attention_indicator = 1, .power_indicator = 1 },
};

static void
vmd_led_set_indicator_control(struct vmd_pci_device *vmd_device, enum spdk_vmd_led_state state)
{
	const struct vmd_led_indicator_config *config;
	union express_slot_control_register slot_control;

	assert(state >= SPDK_VMD_LED_STATE_OFF && state <= SPDK_VMD_LED_STATE_REBUILD);
	config = &g_led_config[state];

	slot_control = vmd_device->pcie_cap->slot_control;
	slot_control.bit_field.attention_indicator_control = config->attention_indicator;
	slot_control.bit_field.power_indicator_control = config->power_indicator;

	/*
	 * Due to the fact that writes to the PCI config space are posted writes, we need to issue
	 * a read to the register we've just written to ensure it reached its destination.
	 * TODO: wrap all register writes with a function taking care of that.
	 */
	vmd_device->pcie_cap->slot_control = slot_control;
	vmd_device->cached_slot_control = vmd_device->pcie_cap->slot_control;
}

static unsigned int
vmd_led_get_state(struct vmd_pci_device *vmd_device)
{
	const struct vmd_led_indicator_config *config;
	union express_slot_control_register slot_control;
	unsigned int state;

	slot_control = vmd_device->cached_slot_control;
	for (state = SPDK_VMD_LED_STATE_OFF; state <= SPDK_VMD_LED_STATE_REBUILD; ++state) {
		config = &g_led_config[state];

		if (slot_control.bit_field.attention_indicator_control == config->attention_indicator &&
		    slot_control.bit_field.power_indicator_control == config->power_indicator) {
			return state;
		}
	}

	return SPDK_VMD_LED_STATE_UNKNOWN;
}

/*
 * The identifying device under VMD is located in the global list of VMD controllers.  If the BDF
 * identifies an endpoint, then the LED is attached to the endpoint's parent.  If the BDF identifies
 * a type 1 header, then this device has the corresponding LED. This may arise when a user wants to
 * identify a given empty slot under VMD.
 */
static struct vmd_pci_device *
vmd_get_led_device(const struct spdk_pci_device *pci_device)
{
	struct vmd_pci_device *vmd_device;

	assert(strcmp(spdk_pci_device_get_type(pci_device), "vmd") == 0);

	vmd_device = vmd_find_device(&pci_device->addr);
	if (spdk_unlikely(vmd_device == NULL)) {
		return NULL;
	}

	if (vmd_device->header_type == PCI_HEADER_TYPE_NORMAL) {
		if (spdk_unlikely(vmd_device->parent == NULL)) {
			return NULL;
		}

		return vmd_device->parent->self;
	}

	return vmd_device;
}

int
spdk_vmd_set_led_state(struct spdk_pci_device *pci_device, enum spdk_vmd_led_state state)
{
	struct vmd_pci_device *vmd_device;

	if (state < SPDK_VMD_LED_STATE_OFF || state > SPDK_VMD_LED_STATE_REBUILD) {
		SPDK_ERRLOG("Invalid LED state\n");
		return -EINVAL;
	}

	vmd_device = vmd_get_led_device(pci_device);
	if (spdk_unlikely(vmd_device == NULL)) {
		SPDK_ERRLOG("The PCI device is not behind the VMD\n");
		return -ENODEV;
	}

	vmd_led_set_indicator_control(vmd_device, state);
	return 0;
}

int
spdk_vmd_get_led_state(struct spdk_pci_device *pci_device, enum spdk_vmd_led_state *state)
{
	struct vmd_pci_device *vmd_device;

	vmd_device = vmd_get_led_device(pci_device);
	if (spdk_unlikely(vmd_device == NULL)) {
		SPDK_ERRLOG("The PCI device is not behind the VMD\n");
		return -ENODEV;
	}

	*state = (enum spdk_vmd_led_state)vmd_led_get_state(vmd_device);
	return 0;
}
