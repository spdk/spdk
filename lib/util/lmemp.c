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

/*
 *     Crummy storage allocator
 *     TODO:
 *		NUMA?
 *
 */

#define SDEBUG (0)	/* Debug messages for storage alloc */
#define HDEBUG (0)      /* Debug messages for hugemem alloc */

#include "spdk/lmemp.h"
#include "spdk/crc16.h"

struct storbase *g_spdk_storbase = NULL;

int g_spdk_page_size = 0;

/*
 *   Debugging aids
 */

/*
 *   Huge memory allocator functions
 */

#if PDEBUG || SDEBUG
static void
spdk_lmempa_dump_stormap(struct stormap *sm, int32_t elements)
{
	int i;

	for (i = 0; i < elements; i++) {
		if (sm->base != NULL) {
			printf("  %p vaddr %p length 0x%lx raddr %lx\n", sm, sm->base, sm->length, sm->iova);
		}
		sm++;
	}
	return;
}

static void
spdk_lmempa_dump_allocator(struct storbase *sb)
{

	if (sb == NULL) { return; }
	printf("Base %p elements %d\n", sb, sb->map_elements);

	if (sb->stormap != NULL) {
		printf("%d used storage:\n", sb->used_elements);
		spdk_lmempa_dump_stormap(sb->stormap, sb->map_elements);
	}

	if (sb->freemap != NULL) {
		printf("%d free storage:\n", sb->free_elements);
		spdk_lmempa_dump_stormap(sb->freemap, sb->map_elements);
	}

	if (sb->rangemap != NULL) {
		printf("%d hugemem:\n", sb->range_elements);
		spdk_lmempa_dump_stormap(sb->rangemap, sb->range_elements);
	}

	return;
}
#endif

unsigned long
spdk_lmempa_get_phys_addr(void *virtaddr)
{
	int fd, retval;
	uint64_t page, physaddr;
	unsigned long virt_pfn;
	off_t offset;

	/* Open the pagemap file for the current process */
	fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0) {
		printf("Cannot open /proc/self/pagemap: %s\n", strerror(errno));
		return 0;
	}

	virt_pfn = (unsigned long)virtaddr / g_spdk_page_size;
	offset = sizeof(uint64_t) * virt_pfn;

#if HDEBUG
	printf("Page table element 0x%lx seek to 0x%lx\n", virt_pfn, offset);
#endif

	/* Seek to the page that the buffer is on */
	if (lseek(fd, offset, SEEK_SET) == (off_t) -1) {
		printf("Seek failed in /proc/self/pagemap: %s\n", strerror(errno));
		close(fd);
		return 0;
	}

	/* The page frame number is in bits 0-54 so read the first 7 bytes and clear the 55th bit */
	retval = read(fd, &page, PFN_MASK_SIZE);

	close(fd);

	if (retval < 0) {
		printf("Cannot read /proc/self/pagemap: %s\n", strerror(errno));
		return 0;
	} else if (retval != PFN_MASK_SIZE) {
		printf("Read %d bytes from /proc/self/pagemap but expected %d\n", retval, PFN_MASK_SIZE);
		return 0;
	}

#if HDEBUG
	printf("Page value returned %lx\n", page);
#endif

	/*
	 * the pfn (page frame number) are bits 0-54 (see
	 * pagemap.txt in linux Documentation)
	 */
	if ((page & 0x7fffffffffffffULL) == 0) { return (0); }

	physaddr = ((page & 0x7fffffffffffffULL) * g_spdk_page_size) + ((unsigned long)virtaddr %
			g_spdk_page_size);

	return (physaddr);
}

/* Lock page in physical memory and prevent from swapping. */
static int
mem_lock_page(const void *virt)
{
	unsigned long virtual = (unsigned long)virt;
	unsigned long aligned = (virtual & ~(g_spdk_page_size - 1));

	return (mlock((void *)aligned, g_spdk_page_size));
}

/* Given base address and length, create free list */
void
spdk_lmempc_create_free_list(struct storbase *sb, void *addr, uint64_t length)
{
	struct stormap *sa, * sr;
	char *caddr;
	uint64_t physaddr, lastpaddr = 0;
	int i;
	uint64_t l;
	uint64_t slength = 0;
	char firstone = 1;

	if (sb == NULL) { return; }

	/* Create a range block for the whole range */
	for (i = 0, sr = sb->rangemap; i < sb->map_elements; i++, sr++) {
		if (sr->base == NULL) {
			sr->base = addr;
			sr->length = length;
			sr->iova =  spdk_lmempa_get_phys_addr(addr);
			sb->range_elements++;
			break;
		}
	}

#if SDEBUG
	printf("%d memory segment(s):\n", sb->range_elements);
	spdk_lmempa_dump_stormap(sb->rangemap, sb->map_elements);
#endif

	/* Create initial virtually contiguous available block */
	for (i = 0, sa = sb->freemap; i < sb->map_elements; i++, sa++) {
		if (sa->base == NULL) {
			sa->base = addr;
			sa->length = 0;
			sa->iova =  spdk_lmempa_get_phys_addr(addr);
			sb->free_elements++;
			break;
		}
	}

	for (caddr = (char *) addr, l = 0; l < length; l += g_spdk_page_size) {
		physaddr = spdk_lmempa_get_phys_addr(&caddr[l]);
		if (!physaddr) {
			printf("Virtual address %p returned zero physical address. Aborting app.\n", &caddr[i]);
			break;
		}
		/* if (!sa->iova) sa->iova = physaddr; */
		if (physaddr != (lastpaddr + g_spdk_page_size)) {
#if SDEBUG
			printf("Virtual address %p physical address 0x%lx\n", &caddr[i], physaddr);
#endif

			/* Close out one area and start another if virtual address not contigous */
			if (!firstone) {
				sa->length = length;
				for (i = 0, sa = sb->freemap; i < sb->map_elements; i++, sa++) {
					if (sa->base == NULL) {
						sa->base = &caddr[l];
						sa->length = 0;
						sa->iova = physaddr;
						sb->free_elements++;
						break;
					}
				}
				firstone = 0;
			} else { slength += g_spdk_page_size; }
		} else { slength += g_spdk_page_size; }
		lastpaddr = physaddr;
	}
	sa->length = slength;

#if SDEBUG
	printf("%d available contiguous segment(s):\n", sb->free_elements);
	spdk_lmempa_dump_stormap(sb->freemap, sb->map_elements);
#endif
	return;
}

/* Use memory pool passed by caller */
void
spdk_lmempc_define_mempool(struct storbase *sb, void *vaddr, uint64_t iova, uint64_t size)
{
	struct stormap *sm;
	int i;

	if (sb == NULL) { return; }
	/* Put passed range on the pool list */
	for (i = 0, sm = sb->rangemap; i < sb->range_elements; i++, sm++) {
		if (sm->base == NULL) {
			sm->base = vaddr;
			sm->iova = iova;
			sm->length = size;
			sb->range_elements++;
			break;
		}
	}
	/* Put passed range on the free list */
	for (i = 0, sm = sb->freemap; i < sb->map_elements; i++, sm++) {
		if (sm->base == NULL) {
			sm->base = vaddr;
			sm->iova = iova;
			sm->length = size;
			sb->free_elements++;
			break;
		}
	}
	return;
}

/* Allocate hugemem in units of bytes) */
int
spdk_lmempa_allocate_mempool(uint64_t size)
{
	struct storbase *sb = g_spdk_storbase;
	uint64_t length = ((size + 2097151) / 2097152) * 2097152;
	uint64_t offset;
	char *caddr;
	void *addr;
	int flags = FLAGS;
	int rc;
	unsigned int lockedany = 0;

	if (sb == NULL) {
		printf("allocate_mempool: Allocator not initialized.\n");
		return (-1);
	}

#if SDEBUG
	printf("Page size is %d 0x%x bytes\n", g_spdk_page_size, g_spdk_page_size);
	printf("Mapping %ld 0x%lx bytes\n", length, length);
#endif

	if (!length) {
		printf("Huge memory request size is zero.\n");
		return (-1);
	}

#if SDEBUG
	printf("Allocating huge memory...\n");
#endif

	addr = mmap(ADDR, length, PROTECTION, flags, -1, 0);
	if (addr == MAP_FAILED) {
		perror("mmap");
		return (-1);
	}

#if SDEBUG
	printf("Locking huge memory...\n");
#endif
	for (caddr = (char *) addr, offset = 0; offset < length; offset += g_spdk_page_size) {
		rc = mem_lock_page(&caddr[offset]);
		if (rc < 0) {
			printf("%s locking page %p\n", strerror(errno), &caddr[offset]);
			break;
		} else { lockedany++; }
	}

	if (!lockedany) {
		printf("Unable to lock any huge memory.  Releasing and aborting.\n");
		spdk_lmempa_free_mempool();
		return (-1);
	}
	if (lockedany != (length / g_spdk_page_size)) {
		printf("Allocated 0x%x pages but only locked 0x%lx pages.\n",
		       lockedany, (length / g_spdk_page_size));
	}

	spdk_lmempc_create_free_list(sb, addr, length);

	return (0);
}

void
spdk_lmempa_free_mempool(void)
{
	struct storbase *sb = g_spdk_storbase;
	struct stormap *sm;
	int i;
	uint64_t rlength, ulength;
	uint8_t *ubase;

	if (g_spdk_storbase == NULL) {
		printf("free mempool: Allocator not initialized.\n");
		return;
	}

	for (i = 0, sm = g_spdk_storbase->rangemap; i < g_spdk_storbase->range_elements; i++) {
		/* munmap length must be integral of hugepage size */
		rlength = ((sm->length + (LENGTH_1GB - 1)) / LENGTH_1GB) * LENGTH_1GB;
		ubase = (uint8_t *) sm->base;
		while (rlength) {
			ulength = min(rlength, LENGTH_1GB);
#if SDEBUG
			printf("Unmapping %p, length 0x%lx rlength 0x%lx\n", ubase, ulength, rlength);
#endif
			if (munmap(ubase, ulength)) {
				perror("munmap failed");
				/* break; */
			}
			ubase += LENGTH_1GB;
			rlength -= LENGTH_1GB;
		}
	}

	/* Clear the decks */
	memset((char *) sb->stormap, 0, (sizeof(struct stormap) * sb->map_elements));
	memset((char *) sb->freemap, 0, (sizeof(struct stormap) * sb->map_elements));
	memset((char *) sb->rangemap, 0, (sizeof(struct stormap) * sb->map_elements));
	sb->used_elements = 0;
	sb->free_elements = 0;
	sb->range_elements = 0;

	return;
}

void
spdk_lmempc_exit_allocator(struct storbase *sb)
{

#if SDEBUG
	printf("Exiting allocator...\n");
#endif

	if (sb == NULL) { return; }

	if (sb->stormap != NULL) { free(sb->stormap); }

	if (sb->freemap != NULL) { free(sb->freemap); }

	if (sb->rangemap != NULL) { free(sb->rangemap); }

	free((char *) sb);
	return;
}

struct storbase *
spdk_lmempc_init_allocator(int elements)
{
	struct storbase *sb;

	g_spdk_page_size = getpagesize();

	sb = (struct storbase *) malloc(sizeof(struct storbase));
	if (sb == NULL) {
		printf("Insufficient storage to allocate base block.\n");
		return (NULL);
	}
	memset((char *) sb, 0, sizeof(struct storbase));
	sb->map_elements = elements;

#if SDEBUG
	printf("Init allocator with %d elements called with %d\n",
	       sb->map_elements, elements);
#endif

	pthread_mutex_init(&sb->lock, NULL);

	sb->stormap = (struct stormap *) malloc(sizeof(struct stormap) * elements);
	if (sb->stormap == NULL) {
		printf("Insufficient storage to allocate used storage map.\n");
		spdk_lmempc_exit_allocator(sb);
		return (NULL);
	}
	memset((char *) sb->stormap, 0, (sizeof(struct stormap) * elements));

	sb->freemap = (struct stormap *) malloc(sizeof(struct stormap) * elements);
	if (sb->freemap == NULL) {
		printf("Insufficient storage to allocate free storage map.\n");
		spdk_lmempc_exit_allocator(sb);
		return (NULL);
	}
	memset((char *) sb->freemap, 0, (sizeof(struct stormap) * elements));

	sb->rangemap = (struct stormap *) malloc(sizeof(struct stormap) * elements);
	if (sb->rangemap == NULL) {
		printf("Insufficient storage to allocate hugemem range map.\n");
		spdk_lmempc_exit_allocator(sb);
		return (NULL);
	}
	memset((char *) sb->rangemap, 0, (sizeof(struct stormap) * elements));

#if SDEBUG
	spdk_lmempa_dump_allocator(sb);
#endif

	return (sb);
}

static void
garbage_collect(struct storbase *sb)
{
	struct stormap *sf, * sf2;
	int i, j;
	char found_one;

	if (sb == NULL) { return; }

#if SDEBUG
	printf("Garbage collecting free list\n");
#endif
	do {
		/* No point doing anything if the free list is already merged */
		if (sb->free_elements <= 1) { return; }
		found_one = 0;
		for (i = 0,  sf = sb->freemap; i < sb->map_elements; i++, sf++) {
			if (sf->base != NULL) {
				for (j = 0,  sf2 = sb->freemap; j < sb->map_elements; j++, sf2++) {
					if (sf2->base == NULL) { continue; }
					if (sf2 == sf) { continue; }
					if ((sf2->base + sf2->length) == sf->base) {
#if SDEBUG
						printf("Merging...\n");
						spdk_lmempa_dump_stormap(sf, 1);
						spdk_lmempa_dump_stormap(sf2, 1);
#endif
						sf2->length += sf->length;
						memset((char *) sf, 0, sizeof(struct stormap));
						sb->free_elements--;
						if (sb->free_elements <= 1) { return; }
						found_one = 1;
					}
				}
			}
		}
	} while (found_one == 1);
	return;
}

static void
compress_free_list(struct storbase *sb)
{
	struct stormap *sf, * sf2;
	int i, j, k;

	if (sb == NULL) { return; }

#if SDEBUG
	printf("Compressing free list\n");
#endif

	for (i = 0, j = sb->free_elements, sf = sb->freemap; i < sb->map_elements; i++, sf++) {
		if (sf->base == NULL) {	/* Destination empty.  Find something to put in it */
			for (k = (i + 1), sf2 = (sf + 1); k < sb->map_elements; k++, sf2++) {
				if (sf2->base != NULL) {
#if SDEBUG
					printf("Moving to/from...\n");
					spdk_lmempa_dump_stormap(sf, 1);
					spdk_lmempa_dump_stormap(sf2, 1);
#endif
					memcpy((char *)sf, (char *)sf2, sizeof(struct stormap));
					memset((char *)sf2, 0, sizeof(struct stormap));
					break;
				}
			}
		}
		j--;
		if (!j) { break; }
	}
	return;
}

static void
release_extent(struct storbase *sb, struct stormap *sa)
{
	struct stormap *sf;
	int i, j;

	for (i = 0, j = sb->free_elements, sf = sb->freemap; i < sb->map_elements; i++, sf++) {
		if (sf->base != NULL) {
			/* Attempt to attach this to the END of an existing free extent */
			if ((sf->iova + sf->length) == sa->iova) {
#if SDEBUG
				printf("Allocated block:\n");
				spdk_lmempa_dump_stormap(sa, 1);
#endif
#if SDEBUG
				printf("Merged to end:\n");
				spdk_lmempa_dump_stormap(sf, 1);
#endif
				sf->length += sa->length;
				memset((char *)sa, 0, sizeof(struct stormap));
				sb->used_elements--;
#if SDEBUG
				printf("Used elements %d merged block:\n", sb->used_elements);
				spdk_lmempa_dump_stormap(sf, 1);
#endif
				return;
			}
			/* Attempt to attach this to the FRONT of an existing free extent */
			if ((sa->iova + sa->length) == sf->iova) {
#if SDEBUG
				printf("Allocated block:\n");
				spdk_lmempa_dump_stormap(sa, 1);
#endif
#if SDEBUG
				printf("Merged to front:\n");
				spdk_lmempa_dump_stormap(sf, 1);
#endif
				sf->length += sa->length;
				sf->base = sa->base;
				sf->iova = sa->iova;

				memset((char *)sa, 0, sizeof(struct stormap));
				sb->used_elements--;
#if SDEBUG
				printf("Used elements %d merged block:\n", sb->used_elements);
				spdk_lmempa_dump_stormap(sf, 1);
#endif
				return;
			}
			j--;
			if (!j) { break; }
		}
	}

	/* Create new free element */
	for (i = 0, sf = sb->freemap; i < sb->map_elements; i++, sf++) {
		if (sf->base == NULL) {
#if SDEBUG
			printf("Allocated block:\n");
			spdk_lmempa_dump_stormap(sa, 1);
#endif
			memcpy((char *) sf, (char *) sa, sizeof(struct stormap));
			sb->free_elements++;
			memset((char *)sa, 0, sizeof(struct stormap));
			sb->used_elements--;
#if SDEBUG
			printf("Free elements %d new free block:\n", sb->free_elements);
			spdk_lmempa_dump_stormap(sf, 1);
#endif
			return;
		}
	}
	printf("Unable to either merge or allocate free structure.\n");
	return;
}

void
spdk_lmempc_release_storage(struct storbase *sb, void *address)
{
	struct stormap *sa;
	int i, j;

#if SDEBUG
	printf("Releasing %p\n", address);
#endif

	if (address == NULL) {
		printf("Attempting to release storage with NULL address.\n");
		return;
	}

	if (sb == NULL) {
		printf("hpc release storage Allocator not initialized.\n");
		return;
	}

	if (sb->stormap == NULL) {
		printf("Allocator storage map not initialized.\n");
		return;
	}

	if (!sb->used_elements) {
		printf("Attempting to release unallocated storage at %p\n", address);
		return;
	}

	pthread_mutex_lock(&sb->lock);

	for (i = 0,  sa = sb->stormap, j = sb->used_elements; i < sb->map_elements; i++, sa++) {
		if (sa->base != NULL) {
			if (sa->base == address) {
				/* Combine released area into prior one or create new free element */
				release_extent(sb, sa);
				/* Clean up free list if nothing allocated */
				if (!sb->used_elements) {
					compress_free_list(sb);
					garbage_collect(sb);
				}
				pthread_mutex_unlock(&sb->lock);
				return;
			}
			j--;
			if (!j) {
				printf("Attempting to release unallocated storage at %p\n", address);
				pthread_mutex_unlock(&sb->lock);
				return;
			}
		}
	}
	pthread_mutex_unlock(&sb->lock);

	return;
}

void
spdk_lmempc_release_real_storage(struct storbase *sb, uint64_t iova)
{
	struct stormap *sa;
	int i, j;

#if SDEBUG
	printf("Releasing real address %lx\n", iova);
#endif

	if (!iova) {
		printf("Attempting to release real storage with NULL address.\n");
		return;
	}

	if (sb == NULL) {
		printf("hpc release real storage Allocator not initialized.\n");
		return;
	}

	if (sb->stormap == NULL) {
		printf("Allocator storage map not initialized.\n");
		return;
	}

	if (!sb->used_elements) {
		printf("Attempting to release unallocated real storage at %lx\n", iova);
		return;
	}

	pthread_mutex_lock(&sb->lock);

	for (i = 0,  sa = sb->stormap, j = sb->used_elements; i < sb->map_elements; i++, sa++) {
		if (sa->iova == iova) {
			/* Combine released area into prior one or create new free element */
			release_extent(sb, sa);
			/* Clean up free list if nothing allocated */
			if (!sb->used_elements) {
				compress_free_list(sb);
				garbage_collect(sb);
			}
			pthread_mutex_unlock(&sb->lock);
			return;
		}
		if (sa->base != NULL) { j--; }
		if (!j) {
			printf("Attempting to release unallocated real storage at %lx\n", iova);
			pthread_mutex_unlock(&sb->lock);
			return;
		}
	}
	pthread_mutex_unlock(&sb->lock);

	return;
}

void
spdk_lmempc_release_all_storage(struct storbase *sb)
{
	struct stormap *sa;
	int i;

#if SDEBUG
	printf("Releasing all storage\n");
#endif

	if (sb == NULL) {
		printf("hpc release all storage Allocator not initialized.\n");
		return;
	}

	if (sb->stormap == NULL) {
		printf("Allocator storage map not initialized.\n");
		return;
	}

	if (!sb->used_elements) {
		return;
	}

	for (i = 0,  sa = sb->stormap; i < sb->map_elements; i++, sa++) {
		if (sa->base != NULL) {
#if SDEBUG
			printf("Releasing real address %lx\n", sa->iova);
#endif
			spdk_lmempc_release_real_storage(sb, sa->iova);
		}
	}

	return;
}

/* Called holding mutex lock */
static void *
create_extent(struct storbase *sb, struct stormap *sf, uint64_t iova, uint64_t size)
{
	struct stormap *sa, * sn;
	int i, j;
	uint64_t olength = sf->length;

#if SDEBUG
	printf("Existing block:\n");
	spdk_lmempa_dump_stormap(sf, 1);
#endif

	for (i = 0, sa = sb->stormap; i < sb->map_elements; i++, sa++) {
		if (sa->base == NULL) {
			sa->iova = iova;
			sa->length = size;
			sa->base = (iova - sf->iova) + sf->base;
			sb->used_elements++;
#if SDEBUG
			printf("New block:\n");
			spdk_lmempa_dump_stormap(sa, 1);
#endif
			/* Deal with updating (and possibly adding to an) existing free entry */
			if ((sf->base == sa->base) && (sf->length != size)) {
				/* Taken from a portion of the front of a free block */
				sf->base += size;
				sf->iova += size;
				sf->length -= size;
			} else if ((sa->length + (sa->base - sf->base)) < olength) {
				/* Deal with unused free storage past the allocated block */
				sf->length = (sa->base - sf->base);
				for (j = 0, sn = sb->freemap; j < sb->map_elements; j++, sn++) {
					if (sn->base == NULL) {
						sn->base = sa->base + size;
						sn->iova = sa->iova + size;
						sn->length = (olength - (sa->length + sf->length));
						sb->free_elements++;
#if SDEBUG
						printf("Split block:\n");
						spdk_lmempa_dump_stormap(sn, 1);
#endif
						break;
					}
				}
			} else { sf->length -= size; }

#if SDEBUG
			printf("Trimmed block:\n");
			spdk_lmempa_dump_stormap(sf, 1);
#endif
#if SDEBUG
			printf("Used elements %d allocated block:\n", sb->used_elements);
			spdk_lmempa_dump_stormap(sa, 1);
#endif
			if (!sf->length) {
				memset((char *) sf, 0, sizeof(struct stormap));
				sb->free_elements++;
#if SDEBUG
				printf("Used entire free block, free block elements %d\n", sb->free_elements);
#endif
			}
			pthread_mutex_unlock(&sb->lock);
			return (sa->base);
		}
	}
	printf("Unable to locate unused allocated storage block.\n");
	pthread_mutex_unlock(&sb->lock);
	return (NULL);
}

void *
spdk_lmempc_allocate_storage(struct storbase *sb, uint64_t lowest, uint64_t highest, uint64_t size,
			     int8_t align, uint64_t *iova)
{
	struct stormap *sf;
	uint64_t mhigh, iove, llowest, lhighest;
	uint64_t mask;
	int i, j;

#if SDEBUG
	printf("allocate lowest 0x%lx highest 0x%lx size 0x%lx align 0x%x\n",
	       lowest, highest, size, align);
#endif

	if (sb == NULL) {
		printf("hpc allocate storage Allocator not initialized.\n");
		return (NULL);
	}

	if (sb->stormap == NULL) {
		printf("Allocator storage map not initialized.\n");
		return (NULL);
	}

	if (sb->used_elements == sb->map_elements) {
		printf("Request number of allocation elements %d exceeded.\n", sb->map_elements);
		return (NULL);
	}

	if (highest < lowest) {
		printf("Cannot pass allocation range where highest < lowest\n");
		return (NULL);
	}

	if (!size) {
		printf("Cannot request zero length allocation.\n");
		return (NULL);
	}

	if (align > 63) {
		printf("Alignment power of two must be 0 - 63.\n");
		return (NULL);
	}

	mask = (1 << align) - 1;

#if SDEBUG
	if (lowest == 1) {
		printf("Requesting lowest available address of size 0x%lx bytes.\n", size);
	} else if (!lowest) {
		printf("Requesting 0x%lx bytes anywhere aligned to 0x%lx.\n", size, highest);
	} else {
		printf("Requesting 0x%lx bytes between 0x%lx and 0x%lx.\n", size, lowest, highest);
	}
#endif

	pthread_mutex_lock(&sb->lock);
	if (lowest == 1) {
		/* Allocate lowest free storage that fits */
		uint64_t lowest = (uint64_t) -1;

		/* Look at each free range to see if this might suit */
		for (i = 0,  j = sb->free_elements, sf = sb->freemap; i < sb->map_elements; i++, sf++) {
			if (sf->base != NULL) {
				if (sf->length >= size) {
					if (sf->iova < lowest) { lowest = sf->iova; }
				}
			}
			j--;
			if (!j) { break; }
		}
		/* Nothing seemed to match */
		if (lowest == (uint64_t) -1) { return (NULL); }

		/* Return buffer at lowest address.  Offset and alignment ignored. */
		if (iova != NULL) { *iova = sf->iova; }
		return (create_extent(sb, sf, sf->iova, size));
	}

	/* We don't look for "already accessed" as if it is, it shouldn't be in the free list too. */

	/* Look at each free range to see if this might suit */
	for (i = 0,  j = sb->free_elements, sf = sb->freemap; i < sb->map_elements; i++, sf++) {
		if (sf->base != NULL) {
			/* Any possibilty of fitting at all?  If not, go on to the next segment */
			if (size <= sf->length) {
				iove = sf->iova + sf->length;
				/* Get highest usable starting storage address that will fit in this range
				 * Absent other restrictions, we attempt to allocate from the top
				 * of a segment as splitting it in two is the least code. */
				mhigh = iove - size;

				/* Zero "lowest" means anywhere is fine - so use the end of this segment */
				if (!lowest) {
					/* TODO: Best fit? */
					/* printf("H %lx mh %lx sz %x iov %lx ln %x align %x\n", */
					/* highest, mhigh, size, sf->iova, sf->length, align); */
					if (align) {
						/* Requesting specific alignment - round down, add to length */
						size += mhigh - (mhigh & ~mask);
						mhigh &= ~mask;
						/* printf("H %lx mh %lx sz %x iov %lx ln %x align %x\n", */
						/* highest, mhigh, size, sf->iova, sf->length, align); */
						if (size > sf->length) { continue; }
						if (mhigh < sf->iova) { continue; }
					}
#if SDEBUG
					printf("Choosing any address 0x%lx\n", mhigh);
#endif
					if (iova != NULL) { *iova = mhigh; }
					return (create_extent(sb, sf, mhigh, size));
				}

				/* If "highest" == -1 then ANY starting address above lowest matches */
				/* So choose the end of this segment */
				if (highest == (uint64_t) -1) {
					/* TODO: Best fit? */
					/* If lowest address is within this block */
					if (lowest >= sf->iova) {
						if (align) {
							/* Requesting specific alignment - round down, add to length */
							size += mhigh - (mhigh & ~mask);
							mhigh &= ~mask;
							/* printf("H %lx mh %lx sz %x iov %lx ln %x align %x\n", */
							/* highest, mhigh, size, sf->iova, sf->length, align); */
							if (size > sf->length) { continue; }
							if (mhigh < sf->iova) { continue; }
						}
#if SDEBUG
						printf("Choosing anything above 0x%lx 0x%lx\n", lowest, mhigh);
#endif
						/* Grab off the end, trim the start */
						if (iova != NULL) { *iova = mhigh; }
						return (create_extent(sb, sf, mhigh, size));
					} else { continue; }
				}

				/* Requesting EXACT starting address? */
				if (lowest == highest) {
					if ((lowest >= sf->iova) && (lowest <= mhigh)) {
						/* Exact address in this free range and there's */
						/* enough space for the size requested */
#if SDEBUG
						printf("Choosing exact 0x%lx\n", lowest);
#endif
						if (iova != NULL) { *iova = lowest; }
						return (create_extent(sb, sf, lowest, size));
					} else { continue; }
				}

				/* At this point, we have a range of acceptable starting addresses */
				/* Don't bother checking if requested range can't possibly be in this segment */
				if (highest < sf->iova) { continue; }
				if (lowest >= iove) { continue; }

				/* Lowest address >= segment start address and highest < segment end address */
				llowest = (lowest >= sf->iova) ? lowest : sf->iova;
				lhighest = (highest <= iove) ? highest : iove;

				/* Do alignment fixup first - round lowest up, highest down */
				if (align) {
					if (llowest & mask) { llowest = (llowest & ~mask) + (mask + 1); }
					if (lhighest & mask) { lhighest = lhighest & ~mask; }
				}
				/* printf("%d: L %lx H %lx S %lx E %lx MH %lx LL %lx LH %lx\n", */
				/* i, lowest, highest, sf->iova, iove, mhigh, llowest, lhighest); */

				if ((llowest >= sf->iova) && ((sf->iova + size) <= lhighest)) {
					/* Bound chosen storage by upper starting limit if necessary */
					mhigh = lhighest - size;
					/* printf("  mh %lx\n", mhigh); */
#if SDEBUG
					printf("Choosing 0x%lx\n", mhigh);
#endif
					/* Grab off the end, trim the start (if any) */
					if (iova != NULL) { *iova = mhigh; }
					return (create_extent(sb, sf, mhigh, size));
				}
			}
			j--;
			if (!j) { break; }
		}
	}

#if SDEBUG
	printf("Couldn't find any memory:\n");
	spdk_lmempa_dump_allocator(sb);
#endif
	pthread_mutex_unlock(&sb->lock);

	return (NULL);
}

void *
spdk_lmempc_allocate_zeroed_storage(struct storbase *sb, uint64_t lowest, uint64_t highest,
				    uint64_t size, int8_t align, uint64_t *iova)
{
	void *vaddr;

	vaddr = spdk_lmempc_allocate_storage(sb, lowest, highest, size, align, iova);
	if (vaddr != NULL) {
		memset((char *) vaddr, 0, size);
	}
	return (vaddr);
}

void *
spdk_lmempc_get_range_info(struct storbase *sb, int element, uint64_t *length, uint64_t *iova)
{
	struct stormap *sf;
	int i, j;

	if (sb == NULL) {
		printf("hpc get range info Allocator not initialized.\n");
		return (NULL);
	}

	if (sb->stormap == NULL) {
		printf("Allocator storage map not initialized.\n");
		return (NULL);
	}

	/* Look at each huge memory range to see if this element is the one to be returned */
	for (i = 0, j = 0, sf = sb->rangemap; i < sb->map_elements; i++, sf++) {
		if (sf->base != NULL) {
			if (j == element) {
				*iova = sf->iova;
				*length = sf->length;
				return (sf->base);
			} else { j++; }
		}
	}
	return (NULL);
}

/* Exit huge memory allocator - releases all memory and tables */
void
spdk_lmempa_exit_allocator(void)
{
	/* Can't hurt */
	spdk_lmempa_free_mempool();

	spdk_lmempc_exit_allocator(g_spdk_storbase);
	g_spdk_storbase = NULL;
	return;
}

/* Initialize huge memory allocator - pass largest expected number of allocated ranges */
int
spdk_lmempa_init_allocator(int elements)
{
	struct storbase *sb;

	if (g_spdk_storbase != NULL) {
		printf("Allocator base already allocated.\n");
		return (-1);
	}

	sb = spdk_lmempc_init_allocator(elements);
	if (sb != NULL) {
		g_spdk_storbase = sb;
		return (0);
	}
	return (-1);
}

/* Release allocated storage given virtual address */
void
spdk_lmempa_release_storage(void *address)
{
	return (spdk_lmempc_release_storage(g_spdk_storbase, address));
}

/* Release allocated storage given real address */
void
spdk_lmempa_release_real_storage(uint64_t iova)
{
	return (spdk_lmempc_release_real_storage(g_spdk_storbase, iova));
}

/* Release all allocated storage */
void
spdk_lmempa_release_all_storage(void)
{
	return (spdk_lmempc_release_all_storage(g_spdk_storbase));
}

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
void *
spdk_lmempa_allocate_storage(uint64_t lowest, uint64_t highest, uint64_t size, int8_t align,
			     uint64_t *iova)
{
	return (spdk_lmempc_allocate_storage(g_spdk_storbase, lowest, highest, size, align, iova));
}

void *
spdk_lmempa_allocate_zeroed_storage(uint64_t lowest, uint64_t highest, uint64_t size, int8_t align,
				    uint64_t *iova)
{
	return (spdk_lmempc_allocate_zeroed_storage(g_spdk_storbase, lowest, highest, size, align, iova));
}

/* Return virtual and real addresses, and byte length of one available free storage range. */
/* Increment element until "zero" virtual address returned. */
void *
spdk_lmempa_get_range_info(int element, uint64_t *length, uint64_t *iova)
{
	return (spdk_lmempc_get_range_info(g_spdk_storbase, element, length, iova));
}
