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
 * Derived from FreeBSD's bufring.h
 *
 * Copyright (c) 2007-2009 Kip Macy kmacy@freebsd.org
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. The name of Kip Macy nor the names of other
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
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

#include <env_system.h>

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
 * \brief Initialize the default value of opts
 */
void spdk_env_opts_init(struct spdk_env_opts *opts);

/**
 * \brief Initialize the environment library. This must be called prior to using
 * any other functions in this library.
 */
void spdk_env_init(const struct spdk_env_opts *opts);

/**
 * \brief Allocates size bytes and returns a pointer to the allocated memory.
 * The memory is not cleared.  If size is 0, then malloc() returns NULL
 */
void *spdk_malloc(size_t size);

/**
 * \brief Changes the size of the memory block pointed to by ptr to size bytes.
 * The contents will be unchanged to the minimum of the old and new sizes
 */
void *spdk_realloc(void *buf, size_t size);

/**
 * \brief Allocates memory for an array of nmemb elements of size bytes each and
 * returns a pointer to the allocated memory.  The memory is set to zero.
 */
void *spdk_calloc(size_t nmemb, size_t size);

/**
 * \brief Frees the memory space pointed to by ptr, which must have been returned
 * by a previous call to spdk_malloc(), spdk_calloc() or spdk_realloc()
 */
void spdk_free(void *ptr);

/**
 * \brief Returns a pointer to a new string which is a duplicate of the string s.
 * Memory for the new string is obtained with spdk_malloc, and can be freed
 * with spdk_free
 */
void *spdk_strdup(const char *s);

/**
 * Allocate a pinned, physically contiguous memory buffer with the
 *   given size and alignment.
 */
void *spdk_dma_malloc(size_t size, size_t align, uint64_t *phys_addr);

/**
 * Allocate a pinned, physically contiguous memory buffer with the
 *   given size and alignment. The buffer will be zeroed.
 */
void *spdk_dma_zmalloc(size_t size, size_t align, uint64_t *phys_addr);

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

/**
 * \brief Return the number of dedicated CPU cores utilized by
 * 	  this env abstraction
 */
uint32_t spdk_env_get_core_count(void);

/**
 * \brief Return the CPU core index of the current thread. This
 *	  will only function when called from threads set up by
 *	  this environment abstraction.
 */
uint32_t spdk_env_get_current_core(void);

/**
 * \brief Return the index of the first dedicated CPU core for
 *	  this application.
 */
uint32_t spdk_env_get_first_core(void);

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
int spdk_ring_mp_enqueue_bulk(struct spdk_ring *sr, void *const *obj_table, unsigned n);

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
int spdk_ring_sp_enqueue_bulk(struct spdk_ring *sr, void *const *obj_table, unsigned n);

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
int spdk_ring_enqueue_bulk(struct spdk_ring *sr, void *const *obj_table, unsigned n);

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
int spdk_ring_mp_enqueue(struct spdk_ring *sr, void *obj);

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
int spdk_ring_sp_enqueue(struct spdk_ring *sr, void *obj);

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
int spdk_ring_enqueue(struct spdk_ring *sr, void *obj);

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
int spdk_ring_mc_dequeue_bulk(struct spdk_ring *sr, void **obj_table, unsigned n);

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
int spdk_ring_sc_dequeue_bulk(struct spdk_ring *sr, void **obj_table, unsigned n);

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
int spdk_ring_dequeue_bulk(struct spdk_ring *sr, void **obj_table, unsigned n);

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
int spdk_ring_mc_dequeue(struct spdk_ring *sr, void **obj_p);

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
int spdk_ring_sc_dequeue(struct spdk_ring *sr, void **obj_p);

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
int spdk_ring_dequeue(struct spdk_ring *sr, void **obj_p);

/**
 * Test if a spdk ring is full.
 *
 * \param sr
 *   A pointer to the ring structure.
 * \return
 *   - 1: The ring is full.
 *   - 0: The ring is not full.
 */
int spdk_ring_full(const struct spdk_ring *sr);

/**
 * Test if a spdk ring is empty.
 *
 * \param sr
 *   A pointer to the ring structure.
 * \return
 *   - 1: The ring is empty.
 *   - 0: The ring is not empty.
 */
int spdk_ring_empty(const struct spdk_ring *sr);

/**
 * Return the number of entries in a spdk ring.
 *
 * \param sr
 *   A pointer to the ring structure.
 * \return
 *   The number of entries in the ring.
 */
unsigned spdk_ring_count(const struct spdk_ring *sr);

/**
 * Return the number of free entries in a spdk ring.
 *
 * \param sr
 *   A pointer to the ring structure.
 * \return
 *   The number of free entries in the ring.
 */
unsigned spdk_ring_free_count(const struct spdk_ring *sr);

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
unsigned spdk_ring_mp_enqueue_burst(struct spdk_ring *sr, void *const *obj_table, unsigned n);

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
unsigned spdk_ring_sp_enqueue_burst(struct spdk_ring *sr, void *const *obj_table, unsigned n);

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
unsigned spdk_ring_enqueue_burst(struct spdk_ring *sr, void *const *obj_table, unsigned n);

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
unsigned spdk_ring_mc_dequeue_burst(struct spdk_ring *sr, void **obj_table, unsigned n);

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
unsigned spdk_ring_sc_dequeue_burst(struct spdk_ring *sr, void **obj_table, unsigned n);

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
unsigned spdk_ring_dequeue_burst(struct spdk_ring *sr, void **obj_table, unsigned n);

/**
 * return the size of memory occupied by a ring
 */
ssize_t spdk_ring_get_memsize(unsigned count);

/**
 * Create a ring
 */
struct spdk_ring *spdk_ring_create(const char *name, unsigned count, int socket_id, unsigned flags);

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
unsigned spdk_lcore_id(void);

/**
 * Get the id of the master lcore
 *
 * \return
 *   the id of the master lcore
 */
unsigned spdk_get_master_lcore(void);

/**
 * Get the ID of the physical socket of the specified lcore
 *
 * \param lcore_id
 *   the targeted lcore, which MUST be between 0 and RTE_MAX_LCORE-1.
 * \return
 *   the ID of lcoreid's physical socket
 */
unsigned spdk_lcore_to_socket_id(unsigned lcore_id);


/**
 * Test if an lcore is enabled.
 *
 * \param lcore_id
 *   The identifier of the lcore, which MUST be between 0 and
 *   RTE_MAX_LCORE-1.
 * \return
 *   True if the given lcore is enabled; false otherwise.
 */
int spdk_lcore_is_enabled(unsigned lcore_id);

/**
 * Return the number of execution units (lcores) on the system.
 *
 * \return
 *   the number of execution units (lcores) on the system.
 */
unsigned spdk_lcore_count(void);

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
unsigned spdk_get_next_lcore(unsigned i, int skip_master, int wrap);

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
int spdk_wait_lcore(unsigned slave_id);

/**
 * Wait until on all the lcores.
 */
void spdk_mp_wait_lcore(void);

/**
 * State of an lcore.
 */
enum spdk_lcore_state_t {
	SPDK_LCORE_STATE_WAIT,       /**< waiting a new command */
	SPDK_LCORE_STATE_RUNNING,    /**< executing command */
	SPDK_LCORE_STATE_FINISHED,   /**< command executed */
};

/**
 * Get the current state of the lcore.
 */
enum spdk_lcore_state_t spdk_get_lcore_state(unsigned lcore_id);

/**
 * Send a message to a slave lcore identified by slave_id to call a
 * function f with argument arg. Once the execution is done, the
 * remote lcore switch in FINISHED state.
 */
int spdk_remote_launch(int (*f)(void *), void *arg, unsigned slave_id);

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
 * Signal abnormal termination of process
 */
void spdk_abort(void) __attribute__((__noreturn__));

/**
 * Termination the process
 */
void spdk_exit(int status) __attribute__((__noreturn__));

enum spdk_mutex_flags {
	SPDK_MUTEX_RECURSIVE = 1 << 0,
	SPDK_MUTEX_ROBUST = 1 << 1,
	SPDK_MUTEX_SHARED = 1 << 2
};
int spdk_mutex_init(spdk_mutex_t *__mutex, int __flags);

/* Destroy a mutex.  */
int spdk_mutex_destroy(spdk_mutex_t *__mutex);

/* Try locking a mutex.  */
int spdk_mutex_trylock(spdk_mutex_t *__mutex);

/* Lock a mutex.  */
int spdk_mutex_lock(spdk_mutex_t *__mutex);

/* Unlock a mutex.  */
int spdk_mutex_unlock(spdk_mutex_t *__mutex);

/* Mark state protected by robust mutex as consistent  */
int spdk_mutex_consistent(spdk_mutex_t *__mutex);

/**
 * returns the ID of the calling thread
 */
spdk_thread_t spdk_thread_self(void);

/**
 * Set the thread name
 */
void spdk_thread_set_name(spdk_thread_t tid, const char *name);

/*
 * Create a thread specific key
 */
int spdk_thread_key_create(spdk_thread_key_t *key, void (*destructor)(void *));

/*
 * Return the value currently bound to the specified key
 */
void *spdk_thread_getspecific(spdk_thread_key_t key);

/*
 * Associate a thread-specific value with a key obtained via a previous call to spdk_thread_key_create()
 */
int spdk_thread_setspecific(spdk_thread_key_t key, const void *value);

/*
 * Examine and change mask of blocked signals
 */
int spdk_thread_sigmask(int how, const sigset_t *set, sigset_t *oldset);

/**
 * sleeps some number of microseconds.  The default is 1
 */
int spdk_usleep(int usec);

/**
 * returns the process ID of the calling process.
 */
spdk_pid_t spdk_getpid(void);


#ifdef __cplusplus
}
#endif

#endif
