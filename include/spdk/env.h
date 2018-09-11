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

/**
 * Memory is dma-safe.
 */
#define SPDK_MALLOC_DMA    0x01

/**
 * Memory is sharable across process boundries.
 */
#define SPDK_MALLOC_SHARE  0x02

#define SPDK_MAX_MEMZONE_NAME_LEN 32
#define SPDK_MAX_MEMPOOL_NAME_LEN 29

/**
 * Memzone flags
 */
#define SPDK_MEMZONE_NO_IOVA_CONTIG 0x00100000 /**< no iova contiguity */

struct spdk_pci_device;

/**
 * \brief Environment initialization options
 */
struct spdk_env_opts {
	const char		*name;
	const char		*core_mask;
	int			shm_id;
	int			mem_channel;
	int			master_core;
	int			mem_size;
	bool			no_pci;
	bool			hugepage_single_segments;
	bool			unlink_hugepage;
	size_t			num_pci_addr;
	struct spdk_pci_addr	*pci_blacklist;
	struct spdk_pci_addr	*pci_whitelist;

	/** Opaque context for use of the env implementation. */
	void			*env_context;
};

/**
 * Allocate dma/sharable memory based on a given dma_flg. It is a physically
 * contiguous memory buffer with the given size, alignment and socket id.
 *
 * \param size Size in bytes.
 * \param align Alignment value for the allocated memory. If '0', the allocated
 * buffer is suitably aligned (in the same manner as malloc()). Otherwise, the
 * allocated buffer is aligned to the multiple of align. In this case, it must
 * be a power of two.
 * \param phys_addr A pointer to the variable to hold the physical address of
 * the allocated buffer is passed. If NULL, the physical address is not returned.
 * \param socket_id Socket ID to allocate memory on, or SPDK_ENV_SOCKET_ID_ANY
 * for any socket.
 * \param flags Combination of SPDK_MALLOC flags (\ref SPDK_MALLOC_DMA, \ref SPDK_MALLOC_SHARE).
 * At least one flag must be specified.
 *
 * \return a pointer to the allocated memory buffer.
 */
void *spdk_malloc(size_t size, size_t align, uint64_t *phys_addr, int socket_id, uint32_t flags);

/**
 * Allocate dma/sharable memory based on a given dma_flg. It is a physically
 * contiguous memory buffer with the given size, alignment and socket id.
 * Also, the buffer will be zeroed.
 *
 * \param size Size in bytes.
 * \param align Alignment value for the allocated memory. If '0', the allocated
 * buffer is suitably aligned (in the same manner as malloc()). Otherwise, the
 * allocated buffer is aligned to the multiple of align. In this case, it must
 * be a power of two.
 * \param phys_addr A pointer to the variable to hold the physical address of
 * the allocated buffer is passed. If NULL, the physical address is not returned.
 * \param socket_id Socket ID to allocate memory on, or SPDK_ENV_SOCKET_ID_ANY
 * for any socket.
 * \param flags Combination of SPDK_MALLOC flags (\ref SPDK_MALLOC_DMA, \ref SPDK_MALLOC_SHARE).
 *
 * \return a pointer to the allocated memory buffer.
 */
void *spdk_zmalloc(size_t size, size_t align, uint64_t *phys_addr, int socket_id, uint32_t flags);

/**
 * Free buffer memory that was previously allocated with spdk_malloc() or spdk_zmalloc().
 *
 * \param buf Buffer to free.
 */
void spdk_free(void *buf);

/**
 * Initialize the default value of opts.
 *
 * \param opts Data structure where SPDK will initialize the default options.
 */
void spdk_env_opts_init(struct spdk_env_opts *opts);

/**
 * Initialize the environment library. This must be called prior to using
 * any other functions in this library.
 *
 * \param opts Environment initialization options.
 * \return 0 on success, or negative errno on failure.
 */
int spdk_env_init(const struct spdk_env_opts *opts);

/**
 * Allocate a pinned, physically contiguous memory buffer with the given size
 * and alignment.
 *
 * \param size Size in bytes.
 * \param align Alignment value for the allocated memory. If '0', the allocated
 * buffer is suitably aligned (in the same manner as malloc()). Otherwise, the
 * allocated buffer is aligned to the multiple of align. In this case, it must
 * be a power of two.
 * \param phys_addr A pointer to the variable to hold the physical address of
 * the allocated buffer is passed. If NULL, the physical address is not returned.
 *
 * \return a pointer to the allocated memory buffer.
 */
void *spdk_dma_malloc(size_t size, size_t align, uint64_t *phys_addr);

/**
 * Allocate a pinned, physically contiguous memory buffer with the given size,
 * alignment and socket id.
 *
 * \param size Size in bytes.
 * \param align Alignment value for the allocated memory. If '0', the allocated
 * buffer is suitably aligned (in the same manner as malloc()). Otherwise, the
 * allocated buffer is aligned to the multiple of align. In this case, it must
 * be a power of two.
 * \param phys_addr A pointer to the variable to hold the physical address of
 * the allocated buffer is passed. If NULL, the physical address is not returned.
 * \param socket_id Socket ID to allocate memory on, or SPDK_ENV_SOCKET_ID_ANY
 * for any socket.
 *
 * \return a pointer to the allocated memory buffer.
 */
void *spdk_dma_malloc_socket(size_t size, size_t align, uint64_t *phys_addr, int socket_id);

/**
 * Allocate a pinned, physically contiguous memory buffer with the given size
 * and alignment. The buffer will be zeroed.
 *
 * \param size Size in bytes.
 * \param align Alignment value for the allocated memory. If '0', the allocated
 * buffer is suitably aligned (in the same manner as malloc()). Otherwise, the
 * allocated buffer is aligned to the multiple of align. In this case, it must
 * be a power of two.
 * \param phys_addr A pointer to the variable to hold the physical address of
 * the allocated buffer is passed. If NULL, the physical address is not returned.
 *
 * \return a pointer to the allocated memory buffer.
 */
void *spdk_dma_zmalloc(size_t size, size_t align, uint64_t *phys_addr);

/**
 * Allocate a pinned, physically contiguous memory buffer with the given size,
 * alignment and socket id. The buffer will be zeroed.
 *
 * \param size Size in bytes.
 * \param align Alignment value for the allocated memory. If '0', the allocated
 * buffer is suitably aligned (in the same manner as malloc()). Otherwise, the
 * allocated buffer is aligned to the multiple of align. In this case, it must
 * be a power of two.
 * \param phys_addr A pointer to the variable to hold the physical address of
 * the allocated buffer is passed. If NULL, the physical address is not returned.
 * \param socket_id Socket ID to allocate memory on, or SPDK_ENV_SOCKET_ID_ANY
 * for any socket.
 *
 * \return a pointer to the allocated memory buffer.
 */
void *spdk_dma_zmalloc_socket(size_t size, size_t align, uint64_t *phys_addr, int socket_id);

/**
 * Resize the allocated and pinned memory buffer with the given new size and
 * alignment. Existing contents are preserved.
 *
 * \param buf Buffer to resize.
 * \param size Size in bytes.
 * \param align Alignment value for the allocated memory. If '0', the allocated
 * buffer is suitably aligned (in the same manner as malloc()). Otherwise, the
 * allocated buffer is aligned to the multiple of align. In this case, it must
 * be a power of two.
 * \param phys_addr A pointer to the variable to hold the physical address of
 * the allocated buffer is passed. If NULL, the physical address is not returned.
 *
 * \return a pointer to the resized memory buffer.
 */
void *spdk_dma_realloc(void *buf, size_t size, size_t align, uint64_t *phys_addr);

/**
 * Free a memory buffer previously allocated, for example from spdk_dma_zmalloc().
 * This call is never made from the performance path.
 *
 * \param buf Buffer to free.
 */
void spdk_dma_free(void *buf);

/**
 * Reserve a named, process shared memory zone with the given size, socket_id
 * and flags.
 *
 * \param name Name to set for this memory zone.
 * \param len Length in bytes.
 * \param socket_id Socket ID to allocate memory on, or SPDK_ENV_SOCKET_ID_ANY
 * for any socket.
 * \param flags Flags to set for this memory zone.
 *
 * \return a pointer to the allocated memory address on success, or NULL on failure.
 */
void *spdk_memzone_reserve(const char *name, size_t len, int socket_id, unsigned flags);

/**
 * Reserve a named, process shared memory zone with the given size, socket_id,
 * flags and alignment.
 *
 * \param name Name to set for this memory zone.
 * \param len Length in bytes.
 * \param socket_id Socket ID to allocate memory on, or SPDK_ENV_SOCKET_ID_ANY
 * for any socket.
 * \param flags Flags to set for this memory zone.
 * \param align Alignment for resulting memzone. Must be a power of 2.
 *
 * \return a pointer to the allocated memory address on success, or NULL on failure.
 */
void *spdk_memzone_reserve_aligned(const char *name, size_t len, int socket_id,
				   unsigned flags, unsigned align);

/**
 * Lookup the memory zone identified by the given name.
 *
 * \param name Name of the memory zone.
 *
 * \return a pointer to the reserved memory address on success, or NULL on failure.
 */
void *spdk_memzone_lookup(const char *name);

/**
 * Free the memory zone identified by the given name.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_memzone_free(const char *name);

/**
 * Dump debug information about all memzones.
 *
 * \param f File to write debug information to.
 */
void spdk_memzone_dump(FILE *f);

struct spdk_mempool;

#define SPDK_MEMPOOL_DEFAULT_CACHE_SIZE	SIZE_MAX

/**
 * Create a thread-safe memory pool.
 *
 * \param name Name for the memory pool.
 * \param count Count of elements.
 * \param ele_size Element size in bytes.
 * \param cache_size How many elements may be cached in per-core caches. Use
 * SPDK_MEMPOOL_DEFAULT_CACHE_SIZE for a reasonable default, or 0 for no per-core cache.
 * \param socket_id Socket ID to allocate memory on, or SPDK_ENV_SOCKET_ID_ANY
 * for any socket.
 *
 * \return a pointer to the created memory pool.
 */
struct spdk_mempool *spdk_mempool_create(const char *name, size_t count,
		size_t ele_size, size_t cache_size, int socket_id);

/**
 * An object callback function for memory pool.
 *
 * Used by spdk_mempool_create_ctor().
 */
typedef void (spdk_mempool_obj_cb_t)(struct spdk_mempool *mp,
				     void *opaque, void *obj, unsigned obj_idx);

/**
 * Create a thread-safe memory pool with user provided initialization function
 * and argument.
 *
 * \param name Name for the memory pool.
 * \param count Count of elements.
 * \param ele_size Element size in bytes.
 * \param cache_size How many elements may be cached in per-core caches. Use
 * SPDK_MEMPOOL_DEFAULT_CACHE_SIZE for a reasonable default, or 0 for no per-core cache.
 * \param socket_id Socket ID to allocate memory on, or SPDK_ENV_SOCKET_ID_ANY
 * for any socket.
 * \param obj_init User provided object calllback initialization function.
 * \param obj_init_arg User provided callback initialization function argument.
 *
 * \return a pointer to the created memory pool.
 */
struct spdk_mempool *spdk_mempool_create_ctor(const char *name, size_t count,
		size_t ele_size, size_t cache_size, int socket_id,
		spdk_mempool_obj_cb_t *obj_init, void *obj_init_arg);

/**
 * Get the name of a memory pool.
 *
 * \param mp Memory pool to query.
 *
 * \return the name of the memory pool.
 */
char *spdk_mempool_get_name(struct spdk_mempool *mp);

/**
 * Free a memory pool.
 */
void spdk_mempool_free(struct spdk_mempool *mp);

/**
 * Get an element from a memory pool. If no elements remain, return NULL.
 *
 * \param mp Memory pool to query.
 *
 * \return a pointer to the element.
 */
void *spdk_mempool_get(struct spdk_mempool *mp);

/**
 * Get multiple elements from a memory pool.
 *
 * \param mp Memory pool to get multiple elements from.
 * \param ele_arr Array of the elements to fill.
 * \param count Count of elements to get.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_mempool_get_bulk(struct spdk_mempool *mp, void **ele_arr, size_t count);

/**
 * Put an element back into the memory pool.
 *
 * \param mp Memory pool to put element back into.
 * \param ele Element to put.
 */
void spdk_mempool_put(struct spdk_mempool *mp, void *ele);

/**
 * Put multiple elements back into the memory pool.
 *
 * \param mp Memory pool to put multiple elements back into.
 * \param ele_arr Array of the elements to put.
 * \param count Count of elements to put.
 */
void spdk_mempool_put_bulk(struct spdk_mempool *mp, void **ele_arr, size_t count);

/**
 * Get the number of entries in the memory pool.
 *
 * \param pool Memory pool to query.
 *
 * \return the number of entries in the memory pool.
 */
size_t spdk_mempool_count(const struct spdk_mempool *pool);

/**
 * Get the number of dedicated CPU cores utilized by this env abstraction.
 *
 * \return the number of dedicated CPU cores.
 */
uint32_t spdk_env_get_core_count(void);

/**
 * Get the CPU core index of the current thread.
 *
 * This will only function when called from threads set up by
 * this environment abstraction. For any other threads \c SPDK_ENV_LCORE_ID_ANY
 * will be returned.
 *
 * \return the CPU core index of the current thread.
 */
uint32_t spdk_env_get_current_core(void);

/**
 * Get the index of the first dedicated CPU core for this application.
 *
 * \return the index of the first dedicated CPU core.
 */
uint32_t spdk_env_get_first_core(void);

/**
 * Get the index of the last dedicated CPU core for this application.
 *
 * \return the index of the last dedicated CPU core.
 */
uint32_t spdk_env_get_last_core(void);

/**
 * Get the index of the next dedicated CPU core for this application.
 *
 * If there is no next core, return UINT32_MAX.
 *
 * \param prev_core Index of previous core.
 *
 * \return the index of the next dedicated CPU core.
 */
uint32_t spdk_env_get_next_core(uint32_t prev_core);

#define SPDK_ENV_FOREACH_CORE(i)		\
	for (i = spdk_env_get_first_core();	\
	     i < UINT32_MAX;			\
	     i = spdk_env_get_next_core(i))

/**
 * Get the socket ID for the given core.
 *
 * \param core CPU core to query.
 *
 * \return the socket ID for the given core.
 */
uint32_t spdk_env_get_socket_id(uint32_t core);

typedef int (*thread_start_fn)(void *);

/**
 * Launch a thread pinned to the given core. Only a single pinned thread may be
 * launched per core. Subsequent attempts to launch pinned threads on that core
 * will fail.
 *
 * \param core The core to pin the thread to.
 * \param fn Entry point on the new thread.
 * \param arg Argument apssed to thread_start_fn
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_env_thread_launch_pinned(uint32_t core, thread_start_fn fn, void *arg);

/**
 * Wait for all threads to exit before returning.
 */
void spdk_env_thread_wait_all(void);

/**
 * Check whether the calling process is primary process.
 *
 * \return true if the calling process is primary process, or false otherwise.
 */
bool spdk_process_is_primary(void);

/**
 * Get a monotonic timestamp counter.
 *
 * \return the monotonic timestamp counter.
 */
uint64_t spdk_get_ticks(void);

/**
 * Get the tick rate of spdk_get_ticks() per second.
 *
 * \return the tick rate of spdk_get_ticks() per second.
 */
uint64_t spdk_get_ticks_hz(void);

/**
 * Delay the given number of microseconds.
 *
 * \param us Number of microseconds.
 */
void spdk_delay_us(unsigned int us);

struct spdk_ring;

enum spdk_ring_type {
	SPDK_RING_TYPE_SP_SC,		/* Single-producer, single-consumer */
	SPDK_RING_TYPE_MP_SC,		/* Multi-producer, single-consumer */
	SPDK_RING_TYPE_MP_MC,		/* Multi-producer, multi-consumer */
};

/**
 * Create a ring.
 *
 * \param type Type for the ring. (SPDK_RING_TYPE_SP_SC or SPDK_RING_TYPE_MP_SC).
 * \param count Size of the ring in elements.
 * \param socket_id Socket ID to allocate memory on, or SPDK_ENV_SOCKET_ID_ANY
 * for any socket.
 *
 * \return a pointer to the created ring.
 */
struct spdk_ring *spdk_ring_create(enum spdk_ring_type type, size_t count, int socket_id);

/**
 * Free the ring.
 *
 * \param ring Ring to free.
 */
void spdk_ring_free(struct spdk_ring *ring);

/**
 * Get the number of objects in the ring.
 *
 * \param ring the ring.
 *
 * \return the number of objects in the ring.
 */
size_t spdk_ring_count(struct spdk_ring *ring);

/**
 * Queue the array of objects (with length count) on the ring.
 *
 * \param ring A pointer to the ring.
 * \param objs A pointer to the array to be queued.
 * \param count Length count of the array of objects.
 *
 * \return the number of objects enqueued.
 */
size_t spdk_ring_enqueue(struct spdk_ring *ring, void **objs, size_t count);

/**
 * Dequeue count objects from the ring into the array objs.
 *
 * \param ring A pointer to the ring.
 * \param objs A pointer to the array to be dequeued.
 * \param count Maximum number of elements to be dequeued.
 *
 * \return the number of objects dequeued which is less than 'count'.
 */
size_t spdk_ring_dequeue(struct spdk_ring *ring, void **objs, size_t count);

#define SPDK_VTOPHYS_ERROR	(0xFFFFFFFFFFFFFFFFULL)

/**
 * Get the physical address of a buffer.
 *
 * \param buf A pointer to a buffer.
 *
 * \return the physical address of this buffer on success, or SPDK_VTOPHYS_ERROR
 * on failure.
 */
uint64_t spdk_vtophys(void *buf);

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

/**
 * Enumerate NVMe devices.
 *
 * \param enum_cb Called when the enumerate operation completes.
 * \param enum_ctx Argument passed to the callback function.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_pci_nvme_enumerate(spdk_pci_enum_cb enum_cb, void *enum_ctx);

/**
 * Enumerate I/OAT device.
 *
 * \param enum_cb Called when the enumerate operation completes.
 * \param enum_ctx Argument passed to the callback function.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_pci_ioat_enumerate(spdk_pci_enum_cb enum_cb, void *enum_ctx);

/**
 * Enumerate virtio device.
 *
 * \param enum_cb Called when the enumerate operation completes.
 * \param enum_ctx Argument passed to the callback function.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_pci_virtio_enumerate(spdk_pci_enum_cb enum_cb, void *enum_ctx);

/**
 * Get a mapping of the virtual address to the BAR of the PCI device.
 *
 * \param dev PCI device.
 * \param bar Index to the BAR.
 * \param mapped_addr A pointer to the pointer to hold the virtual address of
 *  the mapping.
 * \param phys_addr A pointer to the variable to hold the physical address of
 * the mapping.
 * \param size A pointer to the variable to hold the mapped size in bytes.
 *
 * \return 0 on success.
 */
int spdk_pci_device_map_bar(struct spdk_pci_device *dev, uint32_t bar,
			    void **mapped_addr, uint64_t *phys_addr, uint64_t *size);

/**
 * Remove the mapping of the virtual address to the BAR of the PCI device.
 *
 * \param dev PCI device.
 * \param bar Index to the BAR.
 * \param addr Virtual address to remove that must be the mapped_addr returned
 * by a previous call to spdk_pci_device_map_bar().
 *
 * \return 0 on success.
 */
int spdk_pci_device_unmap_bar(struct spdk_pci_device *dev, uint32_t bar, void *addr);

/**
 * Get the domain address of a PCI device.
 *
 * \param dev A pointer to the PCI device.
 *
 * \return the domain address of PCI device.
 */
uint32_t spdk_pci_device_get_domain(struct spdk_pci_device *dev);

/**
 * Get the bus address of a PCI device.
 *
 * \param dev A pointer to the PCI device.
 *
 * \return the bus address of PCI device.
 */
uint8_t spdk_pci_device_get_bus(struct spdk_pci_device *dev);

/**
 * Get the device address of a PCI device.
 *
 * \param dev A pointer to the PCI device.
 *
 * \return the device address of PCI device.
 */
uint8_t spdk_pci_device_get_dev(struct spdk_pci_device *dev);

/**
 * Get the function address of a PCI device.
 *
 * \param dev A pointer to the PCI device.
 *
 * \return the function address of PCI device.
 */
uint8_t spdk_pci_device_get_func(struct spdk_pci_device *dev);

/**
 * Get the PCI address of a PCI device.
 *
 * \param dev A pointer to the PCI device.
 *
 * \return the PCI address structure.
 */
struct spdk_pci_addr spdk_pci_device_get_addr(struct spdk_pci_device *dev);

/**
 * Get the vendor ID of a PCI device.
 *
 * \param dev A pointer to the PCI device.
 *
 * \return the vendor ID of PCI device.
 */
uint16_t spdk_pci_device_get_vendor_id(struct spdk_pci_device *dev);

/**
 * Get the device ID of a PCI device.
 *
 * \param dev A pointer to the PCI device.
 *
 * \return the device ID of PCI device.
 */
uint16_t spdk_pci_device_get_device_id(struct spdk_pci_device *dev);

/**
 * Get the subvendor ID of a PCI device.
 *
 * \param dev A pointer to the PCI device.
 *
 * \return the subvendor ID of PCI device.
 */
uint16_t spdk_pci_device_get_subvendor_id(struct spdk_pci_device *dev);

/**
 * Get the subdevice ID of a PCI device.
 *
 * \param dev A pointer to the PCI device.
 *
 * \return the subdevice ID of PCI device.
 */
uint16_t spdk_pci_device_get_subdevice_id(struct spdk_pci_device *dev);

/**
 * Allocate a PCI ID struct for the PCI device.
 *
 * \param dev A pointer to the PCI device.
 *
 * \return a PCI ID struct.
 */
struct spdk_pci_id spdk_pci_device_get_id(struct spdk_pci_device *dev);

/**
 * Get the NUMA socket ID of a PCI device.
 *
 * \param dev PCI device to get the socket ID of.
 *
 * \return the socket ID (>= 0) of PCI device.
 */
int spdk_pci_device_get_socket_id(struct spdk_pci_device *dev);

/**
 * Get the serial number of a PCI device.
 *
 * \param dev A pointer to the PCI device.
 * \param sn String to store the serial number.
 * \param len Length of the 'sn'.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_pci_device_get_serial_number(struct spdk_pci_device *dev, char *sn, size_t len);

/**
 * Claim a PCI device for exclusive SPDK userspace access.
 *
 * Uses F_SETLK on a shared memory file with the PCI address embedded in its name.
 * As long as this file remains open with the lock acquired, other processes will
 * not be able to successfully call this function on the same PCI device.
 *
 * \param pci_addr PCI address of the device to claim
 *
 * \return -1 if the device has already been claimed, an fd otherwise. This fd
 * should be closed when the application no longer needs access to the PCI device
 * (including when it is hot removed).
 */
int spdk_pci_device_claim(const struct spdk_pci_addr *pci_addr);

/**
 * Detach a PCI device.
 *
 * \param device PCI device to detach.
 */
void spdk_pci_device_detach(struct spdk_pci_device *device);

/**
 * Attach a NVMe device.
 *
 * \param enum_cb Called when the attach operation completes.
 * \param enum_ctx Argument passed to the callback function.
 * \param pci_address PCI address of the NVMe device.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_pci_nvme_device_attach(spdk_pci_enum_cb enum_cb, void *enum_ctx,
				struct spdk_pci_addr *pci_address);

/**
 * Attach a I/OAT device.
 *
 * \param enum_cb Called when the attach operation completes.
 * \param enum_ctx Argument passed to the callback function.
 * \param pci_address PCI address of the I/OAT device.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_pci_ioat_device_attach(spdk_pci_enum_cb enum_cb, void *enum_ctx,
				struct spdk_pci_addr *pci_address);

/**
 * Attach a virtio device.
 *
 * \param enum_cb Called when the attach operation completes.
 * \param enum_ctx Argument passed to the callback function.
 * \param pci_address PCI address of the virtio device.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_pci_virtio_device_attach(spdk_pci_enum_cb enum_cb, void *enum_ctx,
				  struct spdk_pci_addr *pci_address);

/**
 * Read PCI configuration space in any specified size.
 *
 * \param dev PCI device.
 * \param value A pointer to the buffer to hold the value.
 * \param len Length in bytes to read.
 * \param offset Offset in bytes.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_pci_device_cfg_read(struct spdk_pci_device *dev, void *value, uint32_t len,
			     uint32_t offset);

/**
 * Write PCI configuration space in any specified size.
 *
 * \param dev PCI device.
 * \param value A pointer to the value to write.
 * \param len Length in bytes to write.
 * \param offset Offset in bytes.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_pci_device_cfg_write(struct spdk_pci_device *dev, void *value, uint32_t len,
			      uint32_t offset);
/**
 * Read 1 byte from PCI configuration space.
 *
 * \param dev PCI device.
 * \param value A pointer to the buffer to hold the value.
 * \param offset Offset in bytes.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_pci_device_cfg_read8(struct spdk_pci_device *dev, uint8_t *value, uint32_t offset);

/**
 * Write 1 byte to PCI configuration space.
 *
 * \param dev PCI device.
 * \param value A value to write.
 * \param offset Offset in bytes.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_pci_device_cfg_write8(struct spdk_pci_device *dev, uint8_t value, uint32_t offset);

/**
 * Read 2 bytes from PCI configuration space.
 *
 * \param dev PCI device.
 * \param value A pointer to the buffer to hold the value.
 * \param offset Offset in bytes.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_pci_device_cfg_read16(struct spdk_pci_device *dev, uint16_t *value, uint32_t offset);

/**
 * Write 2 bytes to PCI configuration space.
 *
 * \param dev PCI device.
 * \param value A value to write.
 * \param offset Offset in bytes.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_pci_device_cfg_write16(struct spdk_pci_device *dev, uint16_t value, uint32_t offset);

/**
 * Read 4 bytes from PCI configuration space.
 *
 * \param dev PCI device.
 * \param value A pointer to the buffer to hold the value.
 * \param offset Offset in bytes.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_pci_device_cfg_read32(struct spdk_pci_device *dev, uint32_t *value, uint32_t offset);

/**
 * Write 4 bytes to PCI configuration space.
 *
 * \param dev PCI device.
 * \param value A value to write.
 * \param offset Offset in bytes.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_pci_device_cfg_write32(struct spdk_pci_device *dev, uint32_t value, uint32_t offset);

/**
 * Compare two PCI addresses.
 *
 * \param a1 PCI address 1.
 * \param a2 PCI address 2.
 *
 * \return 0 if a1 == a2, less than 0 if a1 < a2, greater than 0 if a1 > a2
 */
int spdk_pci_addr_compare(const struct spdk_pci_addr *a1, const struct spdk_pci_addr *a2);

/**
 * Convert a string representation of a PCI address into a struct spdk_pci_addr.
 *
 * \param addr PCI adddress output on success.
 * \param bdf PCI address in domain:bus:device.function format or
 *	domain.bus.device.function format.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_pci_addr_parse(struct spdk_pci_addr *addr, const char *bdf);

/**
 * Convert a struct spdk_pci_addr to a string.
 *
 * \param bdf String into which a string will be output in the format
 *  domain:bus:device.function. The string must be at least 14 characters in size.
 * \param sz Size of bdf in bytes. Must be at least 14.
 * \param addr PCI address.
 *
 * \return 0 on success, or a negated errno on failure.
 */
int spdk_pci_addr_fmt(char *bdf, size_t sz, const struct spdk_pci_addr *addr);

/**
 * Remove any CPU affinity from the current thread.
 */
void spdk_unaffinitize_thread(void);

/**
 * Call a function with CPU affinity unset.
 *
 * This can be used to run a function that creates other threads without inheriting the calling
 * thread's CPU affinity.
 *
 * \param cb Function to call
 * \param arg Parameter to the function cb().
 *
 * \return the return value of cb().
 */
void *spdk_call_unaffinitized(void *cb(void *arg), void *arg);

/**
 * Page-granularity memory address translation table.
 */
struct spdk_mem_map;

enum spdk_mem_map_notify_action {
	SPDK_MEM_MAP_NOTIFY_REGISTER,
	SPDK_MEM_MAP_NOTIFY_UNREGISTER,
};

typedef int (*spdk_mem_map_notify_cb)(void *cb_ctx, struct spdk_mem_map *map,
				      enum spdk_mem_map_notify_action action,
				      void *vaddr, size_t size);

typedef int (*spdk_mem_map_contiguous_translations)(uint64_t addr_1, uint64_t addr_2);

/**
 * A function table to be implemented by each memory map.
 */
struct spdk_mem_map_ops {
	spdk_mem_map_notify_cb notify_cb;
	spdk_mem_map_contiguous_translations are_contiguous;
};

/**
 * Allocate a virtual memory address translation map.
 *
 * \param default_translation Default translation for the map.
 * \param ops Table of callback functions for map operations.
 * \param cb_ctx Argument passed to the callback function.
 *
 * \return a pointer to the allocated virtual memory address translation map.
 */
struct spdk_mem_map *spdk_mem_map_alloc(uint64_t default_translation,
					const struct spdk_mem_map_ops *ops, void *cb_ctx);

/**
 * Free a memory map previously allocated by spdk_mem_map_alloc().
 *
 * \param pmap Memory map to free.
 */
void spdk_mem_map_free(struct spdk_mem_map **pmap);

/**
 * Register an address translation for a range of virtual memory.
 *
 * \param map Memory map.
 * \param vaddr Virtual address of the region to register - must be 2 MB aligned.
 * \param size Size of the region in bytes - must be multiple of 2 MB in the
 *  current implementation.
 * \param translation Translation to store in the map for this address range.
 *
 * \sa spdk_mem_map_clear_translation().
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_mem_map_set_translation(struct spdk_mem_map *map, uint64_t vaddr, uint64_t size,
				 uint64_t translation);

/**
 * Unregister an address translation.
 *
 * \param map Memory map.
 * \param vaddr Virtual address of the region to unregister - must be 2 MB aligned.
 * \param size Size of the region in bytes - must be multiple of 2 MB in the
 *  current implementation.
 *
 * \sa spdk_mem_map_set_translation().
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_mem_map_clear_translation(struct spdk_mem_map *map, uint64_t vaddr, uint64_t size);

/**
 * Look up the translation of a virtual address in a memory map.
 *
 * \param map Memory map.
 * \param vaddr Virtual address.
 * \param size Contains the size of the memory region pointed to by vaddr.
 * Updated with the size of the memory region for which the translation is valid.
 *
 * \return the translation of vaddr stored in the map, or default_translation
 * as specified in spdk_mem_map_alloc() if vaddr is not present in the map.
 */
uint64_t spdk_mem_map_translate(const struct spdk_mem_map *map, uint64_t vaddr, uint64_t *size);

/**
 * Register the specified memory region for address translation.
 *
 * The memory region must map to pinned huge pages (2MB or greater).
 *
 * \param vaddr Virtual address to register.
 * \param len Length in bytes of the vaddr.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_mem_register(void *vaddr, size_t len);

/**
 * Unregister the specified memory region from vtophys address translation.
 *
 * The caller must ensure all in-flight DMA operations to this memory region
 * are completed or cancelled before calling this function.
 *
 * \param vaddr Virtual address to unregister.
 * \param len Length in bytes of the vaddr.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_mem_unregister(void *vaddr, size_t len);

#ifdef __cplusplus
}
#endif

#endif
