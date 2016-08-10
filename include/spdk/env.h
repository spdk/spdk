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
 * Encapsulated third-party dependencies
 */

#ifndef SPDK_ENV_H
#define SPDK_ENV_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

struct spdk_pci_device;

/**
 * Allocate a pinned, physically contiguous memory buffer with the
 *   given size and alignment. The buffer will be zeroed.
 */
void *
spdk_zmalloc(size_t size, size_t align, uint64_t *phys_addr);

/**
 * Free a memory buffer previously allocated with spdk_malloc.
 *   This call is never made from the performance path.
 */
void
spdk_free(void *buf);

/**
 * Get a monotonic timestamp counter.
 */
uint64_t spdk_get_ticks(void);

/**
 * Get the tick rate of spdk_get_ticks() per second.
 */
uint64_t spdk_get_ticks_hz(void);

/**
 * Delay the given number of microseconds
 */
void spdk_delay_us(unsigned int us);

#define SPDK_VTOPHYS_ERROR	(0xFFFFFFFFFFFFFFFFULL)

uint64_t spdk_vtophys(void *buf);

enum spdk_pci_device_type {
	SPDK_PCI_DEVICE_NVME,
	SPDK_PCI_DEVICE_IOAT,
};

typedef int (*spdk_pci_enum_cb)(void *enum_ctx, struct spdk_pci_device *pci_dev);

int spdk_pci_enumerate(enum spdk_pci_device_type type,
		       spdk_pci_enum_cb enum_cb,
		       void *enum_ctx);

int spdk_pci_device_map_bar(struct spdk_pci_device *dev, uint32_t bar,
			    void **mapped_addr, uint64_t *phys_addr, uint64_t *size);
int spdk_pci_device_unmap_bar(struct spdk_pci_device *dev, uint32_t bar, void *addr);

uint16_t spdk_pci_device_get_domain(struct spdk_pci_device *dev);
uint8_t spdk_pci_device_get_bus(struct spdk_pci_device *dev);
uint8_t spdk_pci_device_get_dev(struct spdk_pci_device *dev);
uint8_t spdk_pci_device_get_func(struct spdk_pci_device *dev);
uint16_t spdk_pci_device_get_vendor_id(struct spdk_pci_device *dev);
uint16_t spdk_pci_device_get_device_id(struct spdk_pci_device *dev);
uint16_t spdk_pci_device_get_subvendor_id(struct spdk_pci_device *dev);
uint16_t spdk_pci_device_get_subdevice_id(struct spdk_pci_device *dev);
uint32_t spdk_pci_device_get_class(struct spdk_pci_device *dev);
const char *spdk_pci_device_get_device_name(struct spdk_pci_device *dev);
int spdk_pci_device_get_serial_number(struct spdk_pci_device *dev, char *sn, size_t len);
int spdk_pci_device_claim(struct spdk_pci_device *dev);

int spdk_pci_device_cfg_read8(struct spdk_pci_device *dev, uint8_t *value, uint32_t offset);
int spdk_pci_device_cfg_write8(struct spdk_pci_device *dev, uint8_t value, uint32_t offset);
int spdk_pci_device_cfg_read16(struct spdk_pci_device *dev, uint16_t *value, uint32_t offset);
int spdk_pci_device_cfg_write16(struct spdk_pci_device *dev, uint16_t value, uint32_t offset);
int spdk_pci_device_cfg_read32(struct spdk_pci_device *dev, uint32_t *value, uint32_t offset);
int spdk_pci_device_cfg_write32(struct spdk_pci_device *dev, uint32_t value, uint32_t offset);

#ifdef __cplusplus
}
#endif

#endif
