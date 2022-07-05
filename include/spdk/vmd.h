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

/** \file
 * VMD driver public interface
 */

#ifndef SPDK_VMD_H
#define SPDK_VMD_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "spdk/config.h"
#include "spdk/env.h"

/* Maximum VMD devices - up to 6 per cpu */
#define MAX_VMD_TARGET  24

/**
 * Enumerate VMD devices and hook them into the spdk pci subsystem
 *
 * \return 0 on success, -1 on failure
 */
int spdk_vmd_init(void);

/**
 * Release any resources allocated by the VMD library via spdk_vmd_init().
 */
void spdk_vmd_fini(void);

/**
 * Returns a list of nvme devices found on the given vmd pci BDF.
 *
 * \param vmd_addr pci BDF of the vmd device to return end device list
 * \param nvme_list buffer of exactly MAX_VMD_TARGET to return spdk_pci_device array.
 *
 * \return Returns count of nvme device attached to input VMD.
 */
int spdk_vmd_pci_device_list(struct spdk_pci_addr vmd_addr, struct spdk_pci_device *nvme_list);

/** State of the LEDs */
enum spdk_vmd_led_state {
	SPDK_VMD_LED_STATE_OFF,
	SPDK_VMD_LED_STATE_IDENTIFY,
	SPDK_VMD_LED_STATE_FAULT,
	SPDK_VMD_LED_STATE_REBUILD,
	SPDK_VMD_LED_STATE_UNKNOWN,
};

/**
 * Sets the state of the LED on specified PCI device.  The device needs to be behind VMD.
 *
 * \param pci_device PCI device
 * \param state LED state to set
 *
 * \return 0 on success, negative errno otherwise
 */
int spdk_vmd_set_led_state(struct spdk_pci_device *pci_device, enum spdk_vmd_led_state state);

/**
 * Retrieves the state of the LED on specified PCI device.  The device needs to be behind VMD.
 *
 * \param pci_device PCI device
 * \param state current LED state
 *
 * \return 0 on success, negative errno otherwise
 */
int spdk_vmd_get_led_state(struct spdk_pci_device *pci_device, enum spdk_vmd_led_state *state);

/**
 * Checks for hotplug/hotremove events of the devices behind the VMD.  Needs to be called
 * periodically to detect them.
 *
 * \return number of hotplug events detected or negative errno in case of errors
 */
int spdk_vmd_hotplug_monitor(void);

/**
 * Removes a given device from the PCI subsystem simulating a hot-remove.  If the device is being
 * actively used by another module, the actual detach might be deferred.
 *
 * \param addr Address of a PCI device to remove.
 *
 * \return 0 if the device was successfully removed, negative errno otherwise.
 */
int spdk_vmd_remove_device(const struct spdk_pci_addr *addr);

/**
 * Forces a rescan of the devices behind the VMD.  If a device was previously removed through
 * spdk_vmd_remove_device() this will cause it to be reattached.
 *
 * \return number of new devices found during scanning or negative errno on failure.
 */
int spdk_vmd_rescan(void);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_VMD_H */
