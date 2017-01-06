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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct spdk_pci_device;

/**
 * Allocate a pinned, physically contiguous memory buffer with the
 *   given size and alignment.
 */
void *spdk_malloc(size_t size, size_t align, uint64_t *phys_addr);

/**
 * Allocate a pinned, physically contiguous memory buffer with the
 *   given size and alignment. The buffer will be zeroed.
 */
void *spdk_zmalloc(size_t size, size_t align, uint64_t *phys_addr);

/**
 * Resize the allocated and pinned memory buffer with the given
 *   new size and alignment. Existing contents are preserved.
 */
void *spdk_realloc(void *buf, size_t size, size_t align, uint64_t *phys_addr);

/**
 * Free a memory buffer previously allocated with spdk_zmalloc.
 *   This call is never made from the performance path.
 */
void spdk_free(void *buf);

/**
 * Reserve a named, process shared memory zone with the given size,
 *   socket_id and flags.
 * Return a pointer to the allocated memory address. If the allocation
 *   cannot be done, return NULL.
 * Note: to pick any socket id, just set socket_id to -1.
 */
void *spdk_memzone_reserve(const char *name, size_t len, int socket_id, unsigned flags);

/**
 * Lookup the memory zone identified by the given name.
 * Return a pointer to the reserved memory address. If the reservation
 *   cannot be found, return NULL.
 */
void *spdk_memzone_lookup(const char *name);

/**
 * Free the memory zone identified by the given name.
 */
int spdk_memzone_free(const char *name);

/**
 * Dump debug information about all memzones.
 */
void spdk_memzone_dump(FILE *f);

struct spdk_mempool;

/**
 * Create a thread-safe memory pool. Cache size is the number of
 * elements in a thread-local cache. Can be 0 for no caching, or -1
 * for unspecified.
 */
struct spdk_mempool *spdk_mempool_create(const char *name, size_t count,
		size_t ele_size, size_t cache_size);

/**
 * Free a memory pool.
 */
void spdk_mempool_free(struct spdk_mempool *mp);

/**
 * Get an element from a memory pool. If no elements remain, return NULL.
 */
void *spdk_mempool_get(struct spdk_mempool *mp);

/**
 * Put an element back into the memory pool.
 */
void spdk_mempool_put(struct spdk_mempool *mp, void *ele);

/**
 * Put multiple elements back into the memory pool.
 */
void spdk_mempool_put_bulk(struct spdk_mempool *mp, void *const *ele_arr, size_t count);


/**
 * Return true if the calling process is primary process
 */
bool spdk_process_is_primary(void);

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

/**
 * Register the specified memory region for vtophys address translation.
 * The memory region must map to pinned huge pages (2MB or greater).
 */
void spdk_vtophys_register(void *vaddr, uint64_t len);

/**
 * Unregister the specified memory region from vtophys address translation.
 * The caller must ensure all in-flight DMA operations to this memory region
 *  are completed or cancelled before calling this function.
 */
void spdk_vtophys_unregister(void *vaddr, uint64_t len);

struct spdk_pci_addr {
	uint16_t			domain;
	uint8_t				bus;
	uint8_t				dev;
	uint8_t				func;
};

struct spdk_pci_id {
	uint16_t	vendor_id;
	uint16_t	device_id;
	uint16_t	subvendor_id;
	uint16_t	subdevice_id;
};

typedef int (*spdk_pci_enum_cb)(void *enum_ctx, struct spdk_pci_device *pci_dev);

int spdk_pci_nvme_enumerate(spdk_pci_enum_cb enum_cb, void *enum_ctx);
int spdk_pci_ioat_enumerate(spdk_pci_enum_cb enum_cb, void *enum_ctx);

struct spdk_pci_device *spdk_pci_get_device(struct spdk_pci_addr *pci_addr);

int spdk_pci_device_map_bar(struct spdk_pci_device *dev, uint32_t bar,
			    void **mapped_addr, uint64_t *phys_addr, uint64_t *size);
int spdk_pci_device_unmap_bar(struct spdk_pci_device *dev, uint32_t bar, void *addr);

uint16_t spdk_pci_device_get_domain(struct spdk_pci_device *dev);
uint8_t spdk_pci_device_get_bus(struct spdk_pci_device *dev);
uint8_t spdk_pci_device_get_dev(struct spdk_pci_device *dev);
uint8_t spdk_pci_device_get_func(struct spdk_pci_device *dev);

struct spdk_pci_addr spdk_pci_device_get_addr(struct spdk_pci_device *dev);

uint16_t spdk_pci_device_get_vendor_id(struct spdk_pci_device *dev);
uint16_t spdk_pci_device_get_device_id(struct spdk_pci_device *dev);
uint16_t spdk_pci_device_get_subvendor_id(struct spdk_pci_device *dev);
uint16_t spdk_pci_device_get_subdevice_id(struct spdk_pci_device *dev);

struct spdk_pci_id spdk_pci_device_get_id(struct spdk_pci_device *dev);

int spdk_pci_device_get_serial_number(struct spdk_pci_device *dev, char *sn, size_t len);
int spdk_pci_device_claim(const struct spdk_pci_addr *pci_addr);
void spdk_pci_device_detach(struct spdk_pci_device *device);

int spdk_pci_nvme_device_attach(spdk_pci_enum_cb enum_cb, void *enum_ctx,
				struct spdk_pci_addr *pci_address);
int spdk_pci_ioat_device_attach(spdk_pci_enum_cb enum_cb, void *enum_ctx,
				struct spdk_pci_addr *pci_address);

int spdk_pci_device_cfg_read8(struct spdk_pci_device *dev, uint8_t *value, uint32_t offset);
int spdk_pci_device_cfg_write8(struct spdk_pci_device *dev, uint8_t value, uint32_t offset);
int spdk_pci_device_cfg_read16(struct spdk_pci_device *dev, uint16_t *value, uint32_t offset);
int spdk_pci_device_cfg_write16(struct spdk_pci_device *dev, uint16_t value, uint32_t offset);
int spdk_pci_device_cfg_read32(struct spdk_pci_device *dev, uint32_t *value, uint32_t offset);
int spdk_pci_device_cfg_write32(struct spdk_pci_device *dev, uint32_t value, uint32_t offset);

/**
 * Compare two PCI addresses.
 *
 * \return 0 if a1 == a2, less than 0 if a1 < a2, greater than 0 if a1 > a2
 */
int spdk_pci_addr_compare(const struct spdk_pci_addr *a1, const struct spdk_pci_addr *a2);

/**
 * Convert a string representation of a PCI address into a struct spdk_pci_addr.
 *
 * \param addr PCI adddress output on success
 * \param bdf PCI address in domain:bus:device.function format
 *
 * \return 0 on success, or a negated errno value on failure.
 */
int spdk_pci_addr_parse(struct spdk_pci_addr *addr, const char *bdf);

/**
 * Convert a struct spdk_pci_addr to a string.
 *
 * \param bdf String into which a string will be output in the format
 *            domain:bus:device.function. The string must be at least
 *            14 characters in size.
 * \param sz Size of bdf. Must be at least 14.
 * \param addr PCI address input
 *
 * \return 0 on success, or a negated errno value on failure.
 */
int spdk_pci_addr_fmt(char *bdf, size_t sz, const struct spdk_pci_addr *addr);

/**
 * Call a function with CPU affinity unset.
 *
 * This can be used to run a function that creates other threads without inheriting the calling
 * thread's CPU affinity.
 *
 * \param cb function to call
 * \param arg parameter to cb function
 *
 * \return the return value of cb()
 */
void *spdk_call_unaffinitized(void *cb(void *arg), void *arg);

#ifdef __cplusplus
}
#endif

#endif
