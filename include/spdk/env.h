/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   Copyright (c) NetApp, Inc.
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

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_ENV_SOCKET_ID_ANY	(-1)
#define SPDK_ENV_LCORE_ID_ANY	(UINT32_MAX)

struct spdk_pci_device;

/**
 * \brief Environment initialization options
 */
struct spdk_env_opts {
	const char 		*name;
	const char 		*core_mask;
	int 			shm_id;
	int	 		mem_channel;
	int	 		master_core;
	int			mem_size;
	bool			no_pci;

	/** Opaque context for use of the env implementation. */
	void			*env_context;
};

/**
 * \brief Initialize the default value of opts
 */
void spdk_env_opts_init(struct spdk_env_opts *opts);

/**
 * \brief Initialize the environment library. This must be called prior to using
 * any other functions in this library.
 * \return 0 on success, or a negated errno value on failure.
 */
int spdk_env_init(const struct spdk_env_opts *opts);

/**
 * Allocate a pinned, physically contiguous memory buffer with the
 *   given size and alignment.
 */
void *spdk_dma_malloc(size_t size, size_t align, uint64_t *phys_addr);

/**
 * Allocate a pinned, physically contiguous memory buffer with the
 *   given size, alignment and socket id.
 */
void *spdk_dma_malloc_socket(size_t size, size_t align, uint64_t *phys_addr, int socket_id);

/**
 * Allocate a pinned, physically contiguous memory buffer with the
 *   given size and alignment. The buffer will be zeroed.
 */
void *spdk_dma_zmalloc(size_t size, size_t align, uint64_t *phys_addr);

/**
 * Allocate a pinned, physically contiguous memory buffer with the
 *   given size, alignment and socket id. The buffer will be zeroed.
 */
void *spdk_dma_zmalloc_socket(size_t size, size_t align, uint64_t *phys_addr, int socket_id);

/**
 * Resize the allocated and pinned memory buffer with the given
 *   new size and alignment. Existing contents are preserved.
 */
void *spdk_dma_realloc(void *buf, size_t size, size_t align, uint64_t *phys_addr);

/**
 * Free a memory buffer previously allocated with spdk_dma_zmalloc.
 *   This call is never made from the performance path.
 */
void spdk_dma_free(void *buf);

/**
 * Reserve a named, process shared memory zone with the given size,
 *   socket_id and flags.
 * Return a pointer to the allocated memory address. If the allocation
 *   cannot be done, return NULL.
 * Note: to pick any socket id, just set socket_id to SPDK_ENV_SOCKET_ID_ANY.
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

#define SPDK_MEMPOOL_DEFAULT_CACHE_SIZE	SIZE_MAX

/**
 * Create a thread-safe memory pool.
 *
 * \param cache_size How many elements may be cached in per-core caches. Use
 *        SPDK_MEMPOOL_DEFAULT_CACHE_SIZE for a reasonable default, or 0 for no
 *	  per-core cache.
 * \param socket_id Socket ID to allocate memory on, or SPDK_ENV_SOCKET_ID_ANY for any socket.
 */
struct spdk_mempool *spdk_mempool_create(const char *name, size_t count,
		size_t ele_size, size_t cache_size, int socket_id);

/**
 * An object callback function for mempool.
 *
 * Used by spdk_mempool_create_ctor
 */
typedef void (spdk_mempool_obj_cb_t)(struct spdk_mempool *mp,
				     void *opaque, void *obj, unsigned obj_idx);

/**
 * Create a thread-safe memory pool with user provided initialization function and argument.
 *
 * \param cache_size How many elements may be cached in per-core caches. Use
 *        SPDK_MEMPOOL_DEFAULT_CACHE_SIZE for a reasonable default, or 0 for no
 *	  per-core cache.
 * \param socket_id Socket ID to allocate memory on, or SPDK_ENV_SOCKET_ID_ANY for any socket.
 * \param obj_init User provided object calllback initialization function.
 * \param obj_init_arg User provided callback initialization function argument.
 */
struct spdk_mempool *spdk_mempool_create_ctor(const char *name, size_t count,
		size_t ele_size, size_t cache_size, int socket_id,
		spdk_mempool_obj_cb_t *obj_init, void *obj_init_arg);

/**
 * Get the name of a mempool
 */
char *spdk_mempool_get_name(struct spdk_mempool *mp);

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
 * Return the number of entries in the mempool.
 */
size_t spdk_mempool_count(const struct spdk_mempool *pool);

/**
 * \brief Return the number of dedicated CPU cores utilized by
 * 	  this env abstraction
 */
uint32_t spdk_env_get_core_count(void);

/**
 * \brief Return the CPU core index of the current thread.
 *
 * This will only function when called from threads set up by
 * this environment abstraction. For any other threads
 * \c SPDK_ENV_LCORE_ID_ANY will be returned.
 */
uint32_t spdk_env_get_current_core(void);

/**
 * \brief Return the index of the first dedicated CPU core for
 *	  this application.
 */
uint32_t spdk_env_get_first_core(void);

/**
 * \brief Return the index of the last dedicated CPU core for
 *	  this application.
 */
uint32_t spdk_env_get_last_core(void);

/**
 * \brief Return the index of the next dedicated CPU core for
 *	  this application.
 *        If there is no next core, return UINT32_MAX.
 */
uint32_t spdk_env_get_next_core(uint32_t prev_core);

#define SPDK_ENV_FOREACH_CORE(i)		\
	for (i = spdk_env_get_first_core();	\
	     i < UINT32_MAX;			\
	     i = spdk_env_get_next_core(i))

/**
 * \brief Return the socket ID for the given core.
 */
uint32_t spdk_env_get_socket_id(uint32_t core);

typedef int (*thread_start_fn)(void *);

/**
 * \brief Launch a thread pinned to the given core. Only a single pinned thread
 * may be launched per core. Subsequent attempts to launch pinned threads on
 * that core will fail.
 *
 * \param core The core to pin the thread to.
 * \param fn Entry point on the new thread.
 * \param arg Argument apssed to thread_start_fn
 */
int spdk_env_thread_launch_pinned(uint32_t core, thread_start_fn fn, void *arg);

/**
 * \brief Wait for all threads to exit before returning.
 */
void spdk_env_thread_wait_all(void);

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

struct spdk_ring;

enum spdk_ring_type {
	SPDK_RING_TYPE_SP_SC,		/* Single-producer, single-consumer */
	SPDK_RING_TYPE_MP_SC,		/* Multi-producer, single-consumer */
};

/**
 * Create a ring
 */
struct spdk_ring *spdk_ring_create(enum spdk_ring_type type, size_t count, int socket_id);

/**
 * Free the ring
 */
void spdk_ring_free(struct spdk_ring *ring);

/**
 * Get the number of objects in the ring.
 *
 * \param ring the ring
 * \return number of objects in the ring
 */
size_t spdk_ring_count(struct spdk_ring *ring);

/**
 * Queue the array of objects (with length count) on the ring.
 *
 * Return the number of objects enqueued.
 */
size_t spdk_ring_enqueue(struct spdk_ring *ring, void **objs, size_t count);

/**
 * Dequeue count objects from the ring into the array objs.
 *
 * Return the number of objects dequeued.
 */
size_t spdk_ring_dequeue(struct spdk_ring *ring, void **objs, size_t count);

#define SPDK_VTOPHYS_ERROR	(0xFFFFFFFFFFFFFFFFULL)

uint64_t spdk_vtophys(void *buf);

/**
 * \brief Struct for physical memory regions we wish to register (e.g NVMe CMBs/PMRs)
 *
 */
struct spdk_phys_region {
	uint64_t paddr;
	uint64_t size;
	uint64_t vaddr;
};

struct spdk_pci_addr {
	uint32_t			domain;
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
int spdk_pci_virtio_enumerate(spdk_pci_enum_cb enum_cb, void *enum_ctx);

struct spdk_pci_device *spdk_pci_get_device(struct spdk_pci_addr *pci_addr);

int spdk_pci_device_map_bar(struct spdk_pci_device *dev, uint32_t bar,
			    void **mapped_addr, uint64_t *phys_addr, uint64_t *size);
int spdk_pci_device_unmap_bar(struct spdk_pci_device *dev, uint32_t bar, void *addr);

uint32_t spdk_pci_device_get_domain(struct spdk_pci_device *dev);
uint8_t spdk_pci_device_get_bus(struct spdk_pci_device *dev);
uint8_t spdk_pci_device_get_dev(struct spdk_pci_device *dev);
uint8_t spdk_pci_device_get_func(struct spdk_pci_device *dev);

struct spdk_pci_addr spdk_pci_device_get_addr(struct spdk_pci_device *dev);

uint16_t spdk_pci_device_get_vendor_id(struct spdk_pci_device *dev);
uint16_t spdk_pci_device_get_device_id(struct spdk_pci_device *dev);
uint16_t spdk_pci_device_get_subvendor_id(struct spdk_pci_device *dev);
uint16_t spdk_pci_device_get_subdevice_id(struct spdk_pci_device *dev);

struct spdk_pci_id spdk_pci_device_get_id(struct spdk_pci_device *dev);

/**
 * Get the NUMA socket ID of a PCI device.
 *
 * \param dev PCI device to get the socket ID of.
 *
 * \return Socket ID (>= 0), or negative if unknown.
 */
int spdk_pci_device_get_socket_id(struct spdk_pci_device *dev);

int spdk_pci_device_get_serial_number(struct spdk_pci_device *dev, char *sn, size_t len);

/**
 * Claim a PCI device for exclusive SPDK userspace access.
 *
 * Uses F_SETLK on a shared memory file with the PCI address embedded in its name.
 *  As long as this file remains open with the lock acquired, other processes will
 *  not be able to successfully call this function on the same PCI device.
 *
 * \param pci_addr PCI address of the device to claim
 *
 * \return -1 if the device has already been claimed, an fd otherwise.  This fd
 *	   should be closed when the application no longer needs access to the
 *	   PCI device (including when it is hot removed).
 */
int spdk_pci_device_claim(const struct spdk_pci_addr *pci_addr);
void spdk_pci_device_detach(struct spdk_pci_device *device);

int spdk_pci_nvme_device_attach(spdk_pci_enum_cb enum_cb, void *enum_ctx,
				struct spdk_pci_addr *pci_address);
int spdk_pci_ioat_device_attach(spdk_pci_enum_cb enum_cb, void *enum_ctx,
				struct spdk_pci_addr *pci_address);
int spdk_pci_virtio_device_attach(spdk_pci_enum_cb enum_cb, void *enum_ctx,
				  struct spdk_pci_addr *pci_address);

int spdk_pci_device_cfg_read(struct spdk_pci_device *dev, void *value, uint32_t len,
			     uint32_t offset);
int spdk_pci_device_cfg_write(struct spdk_pci_device *dev, void *value, uint32_t len,
			      uint32_t offset);
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
 * \param bdf PCI address in domain:bus:device.function format or
 *	domain.bus.device.function format
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
 * Removes any CPU affinitization from the current thread.
 */
void spdk_unaffinitize_thread(void);

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

/**
 * Page-granularity memory address translation table
 */
struct spdk_mem_map;

enum spdk_mem_map_notify_action {
	SPDK_MEM_MAP_NOTIFY_REGISTER,
	SPDK_MEM_MAP_NOTIFY_UNREGISTER,
};

typedef int (*spdk_mem_map_notify_cb)(void *cb_ctx, struct spdk_mem_map *map,
				      enum spdk_mem_map_notify_action action,
				      void *vaddr, size_t size);

/**
 * Allocate a virtual memory address translation map
 */
struct spdk_mem_map *spdk_mem_map_alloc(uint64_t default_translation,
					spdk_mem_map_notify_cb notify_cb, void *cb_ctx);

/**
 * Free a memory map previously allocated by spdk_mem_map_alloc()
 */
void spdk_mem_map_free(struct spdk_mem_map **pmap);

/**
 * Register an address translation for a range of virtual memory.
 *
 * \param map Memory map
 * \param vaddr Virtual address of the region to register - must be 2 MB aligned.
 * \param size Size of the region - must be 2 MB in the current implementation.
 * \param translation Translation to store in the map for this address range.
 *
 * \sa spdk_mem_map_clear_translation()
 */
int spdk_mem_map_set_translation(struct spdk_mem_map *map, uint64_t vaddr, uint64_t size,
				 uint64_t translation);

/**
 * Unregister an address translation.
 *
 * \sa spdk_mem_map_set_translation()
 */
int spdk_mem_map_clear_translation(struct spdk_mem_map *map, uint64_t vaddr, uint64_t size);

/**
 * Look up the translation of a virtual address in a memory map.
 */
uint64_t spdk_mem_map_translate(const struct spdk_mem_map *map, uint64_t vaddr);

/**
 * Register the specified memory region for address translation.
 * The memory region must map to pinned huge pages (2MB or greater).
 */
int spdk_mem_register(void *vaddr, size_t len, uint64_t paddr);

/**
 * Unregister the specified memory region from vtophys address translation.
 * The caller must ensure all in-flight DMA operations to this memory region
 *  are completed or cancelled before calling this function.
 */
int spdk_mem_unregister(void *vaddr, size_t len);

#ifdef __cplusplus
}
#endif

#endif
