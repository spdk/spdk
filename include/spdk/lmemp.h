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
#include "spdk/string.h"

#ifndef _spdk_lmemp_H
#define _spdk_lmemp_H

#define LENGTH_256MB (256UL * 1024 * 1024)
#define LENGTH_1GB (1024UL * 1024 * 1024)
#define PROTECTION (PROT_READ | PROT_WRITE)

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000 /* arch specific */
#endif

#ifndef MAP_POPULATE
#define MAP_POPULATE 0x8000 /* Not in BSD */
#endif

#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif

#ifndef MAP_HUGE_MASK
#define MAP_HUGE_MASK 0x3f
#endif

#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB    (21 << MAP_HUGE_SHIFT)
#define MAP_HUGE_1GB    (30 << MAP_HUGE_SHIFT)
#endif

/* Only ia64 requires this */
#ifdef __ia64__
#define ADDR (void *)(0x8000000000000000UL)
#define FLAGS (MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_FIXED)
#else
#define ADDR (void *)(0x0UL)
#define FLAGS (MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE)
#endif

#ifndef FALSE
#define FALSE (0)
#define TRUE (1)
#endif

#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KYEL  "\x1B[33m"
#define KBLU  "\x1B[34m"
#define KMAG  "\x1B[35m"
#define KCYN  "\x1B[36m"
#define KWHT  "\x1B[37m"

#define PFN_MASK_SIZE (8)

#define min(x,y) ((x) < (y) ? (x) : (y))
#define max(x,y) ((x) > (y) ? (x) : (y))

/*
 *   Internal structures for storage allocator
 */

struct stormap {
	void *base;		/* Starting virtual address - zero means empty entry */
	uint64_t length;	/* Length in bytes */
	uint64_t iova;		/* "Real" address */
};

struct storbase {		   /* Base structure for memory allocator */
	int32_t map_elements;      /* Number of elements in each storage map array */
	pthread_mutex_t lock;	   /* MP lock */

	struct stormap *stormap;   /* Pointer to allocated storage array */
	int32_t used_elements;     /* Number of allocated segments */

	struct stormap *freemap;   /* Pointer to base of available storage map array */
	int32_t free_elements;     /* Number of available storage areas */

	struct stormap *rangemap;  /* Pointer to base of physically contiguous hugemem area */
	int32_t range_elements;    /* Number of physically contiguous hugemem areas */
};

#if PDEBUG | LDEBUG
static void
dispstor(uint8_t *buffer, uint64_t iova, int size)
{
	int i;

	for (i = 0; i < size; i += 32) {
		printf(KYEL "%p %lx: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x"
		       " %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n" KWHT,
		       &buffer[i], (iova + i),
		       buffer[i + 0], buffer[i + 1], buffer[i + 2], buffer[i + 3],
		       buffer[i + 4], buffer[i + 5], buffer[i + 6], buffer[i + 7],
		       buffer[i + 8], buffer[i + 9], buffer[i + 10], buffer[i + 11],
		       buffer[i + 12], buffer[i + 13], buffer[i + 14], buffer[i + 15],
		       buffer[i + 16], buffer[i + 17], buffer[i + 18], buffer[i + 19],
		       buffer[i + 20], buffer[i + 21], buffer[i + 22], buffer[i + 23],
		       buffer[i + 24], buffer[i + 25], buffer[i + 26], buffer[i + 27],
		       buffer[i + 28], buffer[i + 29], buffer[i + 30], buffer[i + 31]);
	}
	return;
}
#endif

/*
 *   Storage allocator functions
 */

/* Given virtual address, return "real" address */
unsigned long spdk_lmempa_get_phys_addr(void *virtaddr);

/* Allocate huge memory pool passing byte size */
int spdk_lmempa_allocate_mempool(uint64_t size);

/* Release memory pool(s) */
void spdk_lmempa_free_mempool(void);

/* Exit huge memory allocator - releases all memory and tables */
void spdk_lmempa_exit_allocator(void);

/* Initialize huge memory allocator - pass largest expected number of allocated ranges */
int spdk_lmempa_init_allocator(int elements);

/* Release allocated storage given virtual address */
void spdk_lmempa_release_storage(void *address);

/* Release allocated storage given real address */
void spdk_lmempa_release_real_storage(uint64_t iova);

/* Release all allocated storage */
void spdk_lmempa_release_all_storage(void);

/* Allocate length bytes of storage given real address range it should start at.
 * If lowest and highest are the same, only one real address can match the request.
 * If lowest equals zero, then ANY available free storage of the given length is allocated.
 * Alignment defines the buffer alignment (power of 2)
 * Offset defines the I/O byte offset into the buffer.
 * If lowest == 1, returns the lowest free storage that fits.
 * If "lowest" == -1 no buffer is allocated (for "bit bucket" ranges). Only used with SGL functions.
 * If "highest" == -1 then ANY starting address above lowest matches.
 * If iova is non-NULL, the physical address is returned in it.
 */
void *spdk_lmempa_allocate_storage(uint64_t lowest, uint64_t highest, uint64_t size, int8_t align,
				   uint64_t *iova);
void *spdk_lmempa_allocate_zeroed_storage(uint64_t lowest, uint64_t highest, uint64_t size,
		int8_t align,
		uint64_t *iova);

/* Return virtual and real addresses, and byte length of one available free storage range. */
/* Increment element until "zero" virtual address returned. */
void *spdk_lmempa_get_range_info(int element, uint64_t *length, uint64_t *iova);

/*
 *   Allocator routines using a specific memory pool
 */

/* Create memory pool and free list from pool passed by caller */
void spdk_lmempc_define_mempool(struct storbase *sb, void *vaddr, uint64_t iova, uint64_t size);

/* Given base address and length, create free list */
void spdk_lmempc_create_free_list(struct storbase *sb, void *addr, uint64_t length);

/* Exit huge memory allocator - releases all memory and tables */
void spdk_lmempc_exit_allocator(struct storbase *sb);

/* Initialize huge memory allocator - pass largest expected number of allocated ranges */
struct storbase *spdk_lmempc_init_allocator(int elements);

/* Release allocated storage given virtual address */
void spdk_lmempc_release_storage(struct storbase *sb, void *address);

/* Release allocated storage given real address */
void spdk_lmempc_release_real_storage(struct storbase *sb, uint64_t iova);

/* Release all allocated storage */
void spdk_lmempc_release_all_storage(struct storbase *sb);

/* Allocate length bytes of storage given real address range it should start at.
 * If lowest and highest are the same, only one real address can match the request.
 * If lowest equals zero, then ANY available free storage of the given length is allocated.
 * Alignment defines the buffer alignment (power of 2)
 * Offset defines the I/O byte offset into the buffer.
 * If lowest == 1, returns the lowest free storage that fits.
 * If "lowest" == -1 no buffer is allocated (for "bit bucket" ranges). Only used with SGL functions.
 * If "highest" == -1 then ANY starting address above lowest matches.
 * If iova is non-NULL, the physical address is returned in it.
 */
void *spdk_lmempc_allocate_storage(struct storbase *sb, uint64_t lowest, uint64_t highest,
				   uint64_t size,
				   int8_t align, uint64_t *iova);
void *spdk_lmempc_allocate_zeroed_storage(struct storbase *sb, uint64_t lowest, uint64_t highest,
		uint64_t size, int8_t align, uint64_t *iova);

/* Return virtual and real addresses, and byte length of one available free storage range. */
/* Increment element until "zero" virtual address returned. */
void *spdk_lmempc_get_range_info(struct storbase *sb, int element, uint64_t *length,
				 uint64_t *iova);

#endif
