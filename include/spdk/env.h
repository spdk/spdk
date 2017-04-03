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

/*
 *   Copyright (c) 1992-2017 NetApp, Inc.
 *   All rights reserved.
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

#define SPDK_ENV_SOCKET_ID_ANY	(-1)

struct spdk_pci_device;

/**
 * \brief Environment initialization options
 */
struct spdk_env_opts {
	const char 		*name;
	const char 		*core_mask;
	int 			shm_id;
	int	 		dpdk_mem_channel;
	int	 		dpdk_master_core;
	int			dpdk_mem_size;
};

/**
 * State of an lcore.
 */
enum spdk_lcore_state_t {
	WAIT_0,       /**< waiting a new command */
	RUNNING_1,    /**< executing command */
	FINISHED_2,   /**< command executed */
};


/**
 * \brief Initialize the default value of opts
*/
void spdk_env_opts_init(struct spdk_env_opts *opts);

/**
 * \brief Initialize the environment library. This must be called prior to using
 * any other functions in this library.
*/
void spdk_env_init(const struct spdk_env_opts *opts);

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

#define SPDK_MEMPOOL_CACHE_MAX_SIZE 512

/**
 * Create a thread-safe memory pool. Cache size is the number of
 * elements in a thread-local cache. Can be 0 for no caching, or -1
 * for unspecified.
 *
 * \param socket_id Socket ID to allocate memory on, or SPDK_ENV_SOCKET_ID_ANY for any socket.
 */
struct spdk_mempool *spdk_mempool_create(const char *name, size_t count,
		size_t ele_size, size_t cache_size, int socket_id);

/**
 * Free a memory pool.
 */
void spdk_mempool_free(struct spdk_mempool *mp);

/**
 * Get an element from a memory pool. If no elements remain, return NULL.
 */
void *spdk_mempool_get(struct spdk_mempool *mp);

/**
 * Get an element from a memory pool and return the element handle.
 * If no elements remain, return -ENOENT.
 */
int spdk_mempool_get2(struct spdk_mempool *mp, void **elm);


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
unsigned spdk_mempool_avail_count(const struct spdk_mempool *pool);

char *spdk_mempool_name(const struct spdk_mempool *pool);

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

/**
 * Get the NUMA socket ID of a PCI device.
 *
 * \param dev PCI device to get the socket ID of.
 *
 * \return Socket ID (>= 0), or negative if unknown.
 */
int spdk_pci_device_get_socket_id(struct spdk_pci_device *dev);

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

struct spdk_ring;

#define RING_F_SP_ENQ 0x0001 /**< The default enqueue is "single-producer". */
#define RING_F_SC_DEQ 0x0002 /**< The default dequeue is "single-consumer". */

/**
 * Enqueue several objects on the ring (multi-producers safe).
 *
 * This function uses a "compare and set" instruction to move the
 * producer index atomically.
 *
 * \param sr
 *   A pointer to the spdk_ring structure.
 * \param obj_table
 *   A pointer to a table of void * pointers (objects).
 * \param n
 *   The number of objects to add in the ring from the obj_table.
 * \return
 *   - 0: Success; objects enqueue.
 *   - -EDQUOT: Quota exceeded. The objects have been enqueued, but the
 *     high water mark is exceeded.
 *   - -ENOBUFS: Not enough room in the ring to enqueue, no object is enqueued.
 */
int
spdk_ring_mp_enqueue_bulk(struct spdk_ring *sr, void *const *obj_table,
			  unsigned n);
/**
 * Enqueue several objects on a spdk ring (NOT multi-producers safe).
 *
 * \param sr
 *   A pointer to the ring structure.
 * \param obj_table
 *   A pointer to a table of void * pointers (objects).
 * \param n
 *   The number of objects to add in the ring from the obj_table.
 * \return
 *   - 0: Success; objects enqueued.
 *   - -EDQUOT: Quota exceeded. The objects have been enqueued, but the
 *     high water mark is exceeded.
 *   - -ENOBUFS: Not enough room in the ring to enqueue; no object is enqueued.
 */
int
spdk_ring_sp_enqueue_bulk(struct spdk_ring *sr, void *const *obj_table,
			  unsigned n);

/**
 * Enqueue several objects on a spdk ring.
 *
 * This function calls the multi-producer or the single-producer
 * version depending on the default behavior that was specified at
 * ring creation time (see flags).
 *
 * \param sr
 *   A pointer to the spdk ring structure.
 * \param obj_table
 *   A pointer to a table of void * pointers (objects).
 * \param n
 *   The number of objects to add in the ring from the obj_table.
 * \return
 *   - 0: Success; objects enqueued.
 *   - -EDQUOT: Quota exceeded. The objects have been enqueued, but the
 *     high water mark is exceeded.
 *   - -ENOBUFS: Not enough room in the ring to enqueue; no object is enqueued.
 */
int
spdk_ring_enqueue_bulk(struct spdk_ring *sr, void *const *obj_table,
		       unsigned n);
/**
 * Enqueue one object on a spdk ring (multi-producers safe).
 *
 * This function uses a "compare and set" instruction to move the
 * producer index atomically.
 *
 * \param sr
 *   A pointer to the spdk ring structure.
 * \param obj
 *   A pointer to the object to be added.
 * \return
 *   - 0: Success; objects enqueued.
 *   - -EDQUOT: Quota exceeded. The objects have been enqueued, but the
 *     high water mark is exceeded.
 *   - -ENOBUFS: Not enough room in the ring to enqueue; no object is enqueued.
 */
int
spdk_ring_mp_enqueue(struct spdk_ring *sr, void *obj);

/**
 * Enqueue one object on a spdk ring (NOT multi-producers safe).
 *
 * \param sr
 *   A pointer to the spdk ring structure.
 * \param obj
 *   A pointer to the object to be added.
 * \return
 *   - 0: Success; objects enqueued.
 *   - -EDQUOT: Quota exceeded. The objects have been enqueued, but the
 *     high water mark is exceeded.
 *   - -ENOBUFS: Not enough room in the ring to enqueue; no object is enqueued.
 */
int
spdk_ring_sp_enqueue(struct spdk_ring *sr, void *obj);

/**
 * Enqueue one object on a spdk ring.
 *
 * This function calls the multi-producer or the single-producer
 * version, depending on the default behaviour that was specified at
 * ring creation time (see flags).
 *
 * \param sr
 *   A pointer to the ring structure.
 * \param obj
 *   A pointer to the object to be added.
 * \return
 *   - 0: Success; objects enqueued.
 *   - -EDQUOT: Quota exceeded. The objects have been enqueued, but the
 *     high water mark is exceeded.
 *   - -ENOBUFS: Not enough room in the ring to enqueue; no object is enqueued.
 */
int
spdk_ring_enqueue(struct spdk_ring *sr, void *obj);

/**
 * Dequeue several objects from a spdk  ring (multi-consumers safe).
 *
 * This function uses a "compare and set" instruction to move the
 * consumer index atomically.
 *
 * \param sr
 *   A pointer to the spdk ring structure.
 * \param obj_table
 *   A pointer to a table of void * pointers (objects) that will be filled.
 * \param n
 *   The number of objects to dequeue from the ring to the obj_table.
 * \return
 *   - 0: Success; objects dequeued.
 *   - -ENOENT: Not enough entries in the ring to dequeue; no object is
 *     dequeued.
 */
int
spdk_ring_mc_dequeue_bulk(struct spdk_ring *sr, void **obj_table, unsigned n);

/**
 * Dequeue several objects from a spdk ring (NOT multi-consumers safe).
 *
 * \param sr
 *   A pointer to the ring structure.
 * \param obj_table
 *   A pointer to a table of void * pointers (objects) that will be filled.
 * \param n
 *   The number of objects to dequeue from the ring to the obj_table,
 *   must be strictly positive.
 * \return
 *   - 0: Success; objects dequeued.
 *   - -ENOENT: Not enough entries in the ring to dequeue; no object is
 *     dequeued.
 */
int
spdk_ring_sc_dequeue_bulk(struct spdk_ring *sr, void **obj_table, unsigned n);

/**
 * Dequeue several objects from a spdk ring.
 *
 * This function calls the multi-consumers or the single-consumer
 * version, depending on the default behaviour that was specified at
 * ring creation time (see flags).
 *
 * \param sr
 *   A pointer to the ring spdk structure.
 * \param obj_table
 *   A pointer to a table of void * pointers (objects) that will be filled.
 * \param n
 *   The number of objects to dequeue from the ring to the obj_table.
 * \return
 *   - 0: Success; objects dequeued.
 *   - -ENOENT: Not enough entries in the ring to dequeue, no object is
 *     dequeued.
 */
int
spdk_ring_dequeue_bulk(struct spdk_ring *sr, void **obj_table, unsigned n);

/**
 * Dequeue one object from a spdk ring (multi-consumers safe).
 *
 * This function uses a "compare and set" instruction to move the
 * consumer index atomically.
 *
 * \param sr
 *   A pointer to the ring structure.
 * \param obj_p
 *   A pointer to a void * pointer (object) that will be filled.
 * \return
 *   - 0: Success; objects dequeued.
 *   - -ENOENT: Not enough entries in the ring to dequeue; no object is
 *     dequeued.
 */
int
spdk_ring_mc_dequeue(struct spdk_ring *sr, void **obj_p);
/**
 * Dequeue one object from a spdk ring (NOT multi-consumers safe).
 *
 * \param sr
 *   A pointer to the ring structure.
 * \param obj_p
 *   A pointer to a void * pointer (object) that will be filled.
 * \return
 *   - 0: Success; objects dequeued.
 *   - -ENOENT: Not enough entries in the ring to dequeue, no object is
 *     dequeued.
 */
int
spdk_ring_sc_dequeue(struct spdk_ring *sr, void **obj_p);

/**
 * Dequeue one object from a spdk ring.
 *
 * This function calls the multi-consumers or the single-consumer
 * version depending on the default behaviour that was specified at
 * ring creation time (see flags).
 *
 * \param sr
 *   A pointer to the ring structure.
 * \param obj_p
 *   A pointer to a void * pointer (object) that will be filled.
 * \return
 *   - 0: Success, objects dequeued.
 *   - -ENOENT: Not enough entries in the ring to dequeue, no object is
 *     dequeued.
 */
int
spdk_ring_dequeue(struct spdk_ring *sr, void **obj_p);

/**
 * Test if a spdk ring is full.
 *
 * \param sr
 *   A pointer to the ring structure.
 * \return
 *   - 1: The ring is full.
 *   - 0: The ring is not full.
 */
int
spdk_ring_full(const struct spdk_ring *sr);

/**
 * Test if a spdk ring is empty.
 *
 * \param sr
 *   A pointer to the ring structure.
 * \return
 *   - 1: The ring is empty.
 *   - 0: The ring is not empty.
 */
int
spdk_ring_empty(const struct spdk_ring *sr);

/**
 * Return the number of entries in a spdk ring.
 *
 * \param sr
 *   A pointer to the ring structure.
 * \return
 *   The number of entries in the ring.
 */
unsigned
spdk_ring_count(const struct spdk_ring *sr);

/**
 * Return the number of free entries in a spdk ring.
 *
 * \param sr
 *   A pointer to the ring structure.
 * \return
 *   The number of free entries in the ring.
 */
unsigned
spdk_ring_free_count(const struct spdk_ring *sr);

/**
 * Enqueue several objects on the ring (multi-producers safe).
 *
 * This function uses a "compare and set" instruction to move the
 * producer index atomically.
 *
 * \param sr
 *   A pointer to the ring structure.
 * \param obj_table
 *   A pointer to a table of void * pointers (objects).
 * \param n
 *   The number of objects to add in the ring from the obj_table.
 * \return
 *   - n: Actual number of objects enqueued.
 */
unsigned
spdk_ring_mp_enqueue_burst(struct spdk_ring *sr, void *const *obj_table,
			   unsigned n);

/**
 * Enqueue several objects on a spdk ring (NOT multi-producers safe).
 *
 * \param sr
 *   A pointer to the spdk ring structure.
 * \param obj_table
 *   A pointer to a table of void * pointers (objects).
 * \param n
 *   The number of objects to add in the ring from the obj_table.
 * \return
 *   - n: Actual number of objects enqueued.
 */
unsigned
spdk_ring_sp_enqueue_burst(struct spdk_ring *sr, void *const *obj_table,
			   unsigned n);
/**
 * Enqueue several objects on a spdk ring.
 *
 * This function calls the multi-producer or the single-producer
 * version depending on the default behavior that was specified at
 * ring creation time (see flags).
 *
 * \param sr
 *   A pointer to the spdk ring structure.
 * \param obj_table
 *   A pointer to a table of void * pointers (objects).
 * \param n
 *   The number of objects to add in the ring from the obj_table.
 * \return
 *   - n: Actual number of objects enqueued.
 */
unsigned
spdk_ring_enqueue_burst(struct spdk_ring *sr, void *const *obj_table,
			unsigned n);
/**
 * Dequeue several objects from a spdk ring (multi-consumers safe). When the request
 * objects are more than the available objects, only dequeue the actual number
 * of objects
 *
 * This function uses a "compare and set" instruction to move the
 * consumer index atomically.
 *
 * \param sr
 *   A pointer to the ring structure.
 * \param obj_table
 *   A pointer to a table of void * pointers (objects) that will be filled.
 * \param n
 *   The number of objects to dequeue from the ring to the obj_table.
 * \return
 *   - n: Actual number of objects dequeued, 0 if ring is empty
 */
unsigned
spdk_ring_mc_dequeue_burst(struct spdk_ring *sr, void **obj_table, unsigned n);

/**
 * Dequeue several objects from a spdk ring (NOT multi-consumers safe).When the
 * request objects are more than the available objects, only dequeue the
 * actual number of objects
 *
 * \param sr
 *   A pointer to the ring structure.
 * \param obj_table
 *   A pointer to a table of void * pointers (objects) that will be filled.
 * \param n
 *   The number of objects to dequeue from the ring to the obj_table.
 * \return
 *   - n: Actual number of objects dequeued, 0 if ring is empty
 */
unsigned
spdk_ring_sc_dequeue_burst(struct spdk_ring *sr, void **obj_table, unsigned n);

/**
 * Dequeue multiple objects from a spdk ring up to a maximum number.
 *
 * This function calls the multi-consumers or the single-consumer
 * version, depending on the default behaviour that was specified at
 * ring creation time (see flags).
 *
 * \param sr
 *   A pointer to the spdk ring structure.
 * \param obj_table
 *   A pointer to a table of void * pointers (objects) that will be filled.
 * \param n
 *   The number of objects to dequeue from the ring to the obj_table.
 * \return
 *   - Number of objects dequeued
 */
unsigned
spdk_ring_dequeue_burst(struct spdk_ring *sr, void **obj_table, unsigned n);

/**
 * return the size of memory occupied by a ring
 */
ssize_t spdk_ring_get_memsize(unsigned count);

/**
 * Create a ring
 */
struct spdk_ring *
spdk_ring_create(const char *name, unsigned count, int socket_id, unsigned flags);

/**
 * free the ring
 */
void spdk_ring_free(struct spdk_ring *sr);

/**
 * Change the high water mark. If *count* is 0, water marking is
 * disabled
 */
int spdk_ring_set_water_mark(struct spdk_ring *sr, unsigned count);

/**
 * dump the status of the ring on the console
 */
void spdk_ring_dump(FILE *f, const struct spdk_ring *sr);

/**
 * dump the status of all rings on the console
 */
void spdk_ring_list_dump(FILE *f);

/**
 * search a ring from its name
 */
struct spdk_ring *spdk_ring_lookup(const char *name);

#define SPDK_MAX_LCORE 128

/**
 * Return the ID of the execution unit we are running on.
 * \return
 *  Logical core ID (in EAL thread) or LCORE_ID_ANY (in non-EAL thread)
 */
unsigned
spdk_lcore_id(void);

/**
 * Get the id of the master lcore
 *
 * \return
 *   the id of the master lcore
 */
unsigned
spdk_get_master_lcore(void);

/**
 * Get the ID of the physical socket of the specified lcore
 *
 * \param lcore_id
 *   the targeted lcore, which MUST be between 0 and RTE_MAX_LCORE-1.
 * \return
 *   the ID of lcoreid's physical socket
 */
unsigned
spdk_lcore_to_socket_id(unsigned lcore_id);


/**
 * Test if an lcore is enabled.
 *
 * \param lcore_id
 *   The identifier of the lcore, which MUST be between 0 and
 *   RTE_MAX_LCORE-1.
 * \return
 *   True if the given lcore is enabled; false otherwise.
 */
int
spdk_lcore_is_enabled(unsigned lcore_id);

/**
 * Return the number of execution units (lcores) on the system.
 *
 * \return
 *   the number of execution units (lcores) on the system.
 */
unsigned
spdk_lcore_count(void);

/**
 * Get the next enabled lcore ID.
 *
 * \param i
 *   The current lcore (reference).
 * \param skip_master
 *   If true, do not return the ID of the master lcore.
 * \param wrap
 *   If true, go back to 0 when SPDK_MAX_LCORE is reached; otherwise,
 *   return SPDK_MAX_LCORE.
 * \return
 *   The next lcore_id or SPDK_MAX_LCORE if not found.
 */
unsigned
spdk_get_next_lcore(unsigned i, int skip_master, int wrap);

/**
 * Macro to browse all running lcores.
 */
#define SPDK_LCORE_FOREACH(i)						\
	for (i = spdk_get_next_lcore(-1, 0, 0);				\
	     i<SPDK_MAX_LCORE;						\
	     i = spdk_get_next_lcore(i, 0, 0))

/**
 * Macro to browse all running lcores except the master lcore.
 */
#define SPDK_LCORE_FOREACH_SLAVE(i)					\
	for (i = spdk_get_next_lcore(-1, 1, 0);				\
	     i<SPDK_MAX_LCORE;						\
	     i = spdk_get_next_lcore(i, 1, 0))

/**
 * Wait until a lcore finished its job.
 */
int
spdk_wait_lcore(unsigned slave_id);

/**
 * Wait until on all the lcores.
 */
void
spdk_mp_wait_lcore(void);

/**
 * Get the current state of the lcore.
 */
int
spdk_get_lcore_state(unsigned lcore_id);

/**
 * Send a message to a slave lcore identified by slave_id to call a
 * function f with argument arg. Once the execution is done, the
 * remote lcore switch in FINISHED state.
 */
int
spdk_remote_launch(int (*f)(void *), void *arg, unsigned slave_id);

/**
 * Memcpy
 */
void
spdk_memcpy(void *dst, const void *src, int len);

/**
 * Page-granularity memory address translation table
 */
struct spdk_mem_map;

enum spdk_mem_map_notify_action {
	SPDK_MEM_MAP_NOTIFY_REGISTER,
	SPDK_MEM_MAP_NOTIFY_UNREGISTER,
};

typedef void (*spdk_mem_map_notify_cb)(void *cb_ctx, struct spdk_mem_map *map,
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
void spdk_mem_map_set_translation(struct spdk_mem_map *map, uint64_t vaddr, uint64_t size,
				  uint64_t translation);

/**
 * Unregister an address translation.
 *
 * \sa spdk_mem_map_set_translation()
 */
void spdk_mem_map_clear_translation(struct spdk_mem_map *map, uint64_t vaddr, uint64_t size);

/**
 * Look up the translation of a virtual address in a memory map.
 */
uint64_t spdk_mem_map_translate(const struct spdk_mem_map *map, uint64_t vaddr);

/**
 * Register the specified memory region for address translation.
 * The memory region must map to pinned huge pages (2MB or greater).
 */
void spdk_mem_register(void *vaddr, size_t len);

/**
 * Unregister the specified memory region from vtophys address translation.
 * The caller must ensure all in-flight DMA operations to this memory region
 *  are completed or cancelled before calling this function.
 */
void spdk_mem_unregister(void *vaddr, size_t len);

/**
 * Return the physical address of an element in the mempool.
 *
 * @param mp
 *   A pointer to the mempool structure.
 * @param elt
 *   A pointer (virtual address) to the element of the pool.
 * @return
 *   The physical address of the elt element.
 *   If the mempool was created with MEMPOOL_F_NO_PHYS_CONTIG, the
 *   returned value is RTE_BAD_PHYS_ADDR.
 */
uint64_t spdk_mempool_virt2phy(struct spdk_mempool *mp, void *elt);

/**
 * Return the ID of the physical socket of the logical core we are running on.
 * @return
 *   the ID of current lcoreid's physical socket
 */
int spdk_socket_id(void);

/**
 * Panic the system
 */
#define spdk_panic(...) spdk_panic_(__func__, __VA_ARGS__, "")
#define spdk_panic_(func, format, ...) spdk_panic_fmt(func, format "%.0s", __VA_ARGS__)
void spdk_panic_fmt(const char *func, const char *format, ...);

/**
 * Create a new mempool named *name* in memory.
 *
 * This function uses ``memzone_reserve`` to allocate memory. The
 * pool contains n elements of elt_size. Its size is set to n.
 *
 * @param name
 *   The name of the mempool.
 * @param n
 *   The number of elements in the mempool. The optimum size (in terms of
 *   memory usage) for a mempool is when n is a power of two minus one:
 *   n = (2^q - 1).
 * @param elt_size
 *   The size of each element.
 * @param cache_size
 *   If cache_size is non-zero, the spdk_mempool library will try to
 *   limit the accesses to the common lockless pool, by maintaining a
 *   per-lcore object cache. This argument must be lower or equal to
 *   SPDK_MEMPOOL_CACHE_MAX_SIZE and n / 1.5. It is advised to choose
 *   cache_size to have "n modulo cache_size == 0": if this is
 *   not the case, some elements will always stay in the pool and will
 *   never be used. The access to the per-lcore table is of course
 *   faster than the multi-producer/consumer pool. The cache can be
 *   disabled if the cache_size argument is set to 0; it can be useful to
 *   avoid losing objects in cache.
 * @param obj_init
 *   A function pointer that is called for each object at
 *   initialization of the pool. The user can set some meta data in
 *   objects if needed. This parameter can be NULL if not needed.
 *   See: spdk_mempool_obj_init_t
 * @param obj_init_arg
 *   An opaque pointer to data that can be used as an argument for
 *   each call to the object constructor function.
 * @param socket_id
 *   The *socket_id* argument is the socket identifier in the case of
 *   NUMA. The value can be *SOCKET_ID_ANY* if there is no NUMA
 *   constraint for the reserved zone.
 * @return
 *   The pointer to the new allocated mempool, on success. NULL on error
 */

#define SPDK_MEMPOOL_CACHE_MAX_SIZE 512

typedef void (spdk_mempool_obj_init_t)(struct spdk_mempool *mp, void *arg, void *obj, unsigned i);

struct spdk_mempool *spdk_mempool_create_init(const char *name, size_t count, size_t ele_size,
		size_t cache_size, spdk_mempool_obj_init_t *obj_init, void *obj_init_arg, int socket_id);

#ifdef __cplusplus
}
#endif

#endif
