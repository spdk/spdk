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

#include "spdk_cunit.h"

#include "util/lmemp.c"

#define ADEBUG (0)

#if ADEBUG
static void spdk_lmempc_dump_stormap(struct stormap *sm, int32_t elements)
{
	int i;

	for (i = 0; i < elements; i++) {
		if (sm->base != NULL) {
			printf("  %p vaddr %p length 0x%lx raddr 0x%lx\n", sm, sm->base, sm->length, sm->iova);
		}
		sm++;
	}
	return;
}

static void spdk_lmempc_dump_allocator(struct storbase *sb)
{

	if (sb == NULL) { return; }
	printf("Base %p elements %d\n", sb, sb->map_elements);

	if (sb->stormap != NULL) {
		printf("%d used storage:\n", sb->used_elements);
		spdk_lmempc_dump_stormap(sb->stormap, sb->map_elements);
	}

	if (sb->freemap != NULL) {
		printf("%d free storage:\n", sb->free_elements);
		spdk_lmempc_dump_stormap(sb->freemap, sb->map_elements);
	}

	if (sb->rangemap != NULL) {
		printf("%d hugemem:\n", sb->range_elements);
		spdk_lmempc_dump_stormap(sb->rangemap, sb->range_elements);
	}

	return;
}
#endif

/*
 *      Allocator tester.  Optional parameter is size in bytes.  It will be rounded up to nearest hugemem segment size.
 */

static void
alloc_test(void)
{
	struct storbase *sb;
	void *vaddr, * vaddr1, * vaddr2;
	uint64_t iova, iova1, iova2;
	uint64_t iovas = 0x80000000;
	uint64_t elsize = 0x1000000;

	sb = spdk_lmempc_init_allocator(1000);
	if (sb == NULL) {
		printf("Allocator initialization failed.\n");
		exit(2);
	}

#if 0
	printf("Attempting allocation of %ld 0x%lx bytes of memory pool.\n", length, length);
	ret = spdk_lmempc_allocate_mempool(length);
	if (ret) {
		printf("Hugepage allocation failed.\n");
		spdk_lmempc_exit_allocator();
		exit(3);
	}

	/* Get physical address ranges we can use */
	while (1) {
		vaddr = spdk_lmempc_get_range_info(element, &elsize, &iova);
		if (vaddr == NULL) { break; }
		/* Capture start of first range found */
		if (!iovas) { iovas = iova; }
		printf("Free range vaddr 0x%p real %lx, size %ld 0x%lx\n",
		       vaddr, iova, elsize, elsize);
		element++;
	}
	printf("Using real memory starting at 0x%lx size 0x%lx\n", iovas, elsize);
#endif

	/*
	 * Define phony memory pool.  As the allocator doesn't put any
	 * metadata in the memory pool, it's safe to test the allocation
	 * even without any real memory being used.
	 */

	spdk_lmempc_define_mempool(sb, (void *) 0x100000000, iovas, elsize);

	printf("\nTesting three non-specific location allocation/release...\n");

	vaddr = spdk_lmempc_allocate_storage(sb, 0, 0, 4096, 0, &iova);
	vaddr1 = spdk_lmempc_allocate_storage(sb, 0, 0, 4096, 0, &iova1);
	vaddr2 = spdk_lmempc_allocate_storage(sb, 0, 0, 4096, 12, &iova2);
	if (vaddr == NULL) {
		printf("Allocate storage returned zero address.\n");
		CU_ASSERT(1);
	} else {
		printf("Allocate storage returned vaddr %p raddr 0x%lx\n", vaddr, iova);
		printf("Allocate storage returned vaddr1 %p raddr1 %lx\n", vaddr1, iova1);
		printf("Allocate storage returned vaddr2 %p raddr2 %lx\n", vaddr2, iova2);
#if ADEBUG
		printf("After storage allocation:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
		spdk_lmempc_release_storage(sb, vaddr);
		spdk_lmempc_release_real_storage(sb, iova1);
		spdk_lmempc_release_storage(sb, vaddr2);
#if ADEBUG
		printf("After storage release:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
	}

	printf("\nTesting specific address allocation at the front of a block...\n");
	vaddr = spdk_lmempc_allocate_storage(sb, iovas, iovas, 4096, 0, &iova);
	if (vaddr == NULL) {
		printf("Allocate specific storage returned zero address.\n");
		CU_ASSERT(1);
	} else {
		printf("Allocate specific storage returned vaddr %p raddr 0x%lx\n", vaddr, iova);
#if ADEBUG
		printf("After storage allocation:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
		spdk_lmempc_release_real_storage(sb, iova);
#if ADEBUG
		printf("After storage release:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
	}

	printf("\nTesting specific address allocation in the middle of a block...\n");
	vaddr = spdk_lmempc_allocate_storage(sb, (iovas + 0x4000), (iovas + 0x4000), 4096, 0, &iova);
	if (vaddr == NULL) {
		printf("Allocate specific storage returned zero address.\n");
		CU_ASSERT(1);
	} else {
		printf("Allocate specific storage returned vaddr %p raddr 0x%lx\n", vaddr, iova);
#if ADEBUG
		printf("After storage allocation:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
		spdk_lmempc_release_real_storage(sb, iova);
#if ADEBUG
		printf("After storage release:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
	}

	printf("\nTesting any address allocation at/above a passed real address...\n");
	vaddr = spdk_lmempc_allocate_storage(sb, (iovas + 0x8000), ((uint64_t) -1), 4096, 0, &iova);
	if (vaddr == NULL) {
		printf("Allocate storage at/abote address returned zero address.\n");
		CU_ASSERT(1);
	} else {
		printf("Allocate storage at/above address returned vaddr %p raddr 0x%lx\n", vaddr, iova);
#if ADEBUG
		printf("After storage allocation:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
		spdk_lmempc_release_real_storage(sb, iova);
#if ADEBUG
		printf("After storage release:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
	}

	/* These eliminate unnecessary compiler warning */
	iova = 0;
	iova1 = 0;

	printf("\nTesting small-sized buffer, address allocation anywhere...\n");
	vaddr = spdk_lmempc_allocate_storage(sb, 0, 0, 0x38, 0, &iova);
	if (vaddr == NULL) {
		printf("Allocate small-sized buffer returned zero address.\n");
		CU_ASSERT(1);
	} else {
		printf("Allocate small-sized buffer anywhere returned vaddr %p raddr 0x%lx\n", vaddr, iova);
#if ADEBUG
		printf("After storage allocation:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
	}
	printf("\nTesting 4k aligned buffer right after small buffer, address allocation anywhere...\n");
	vaddr1 = spdk_lmempc_allocate_storage(sb, 0, 0x1000, 0x10000, 12, &iova1);
	if (vaddr1 == NULL) {
		printf("Allocate 4k unaligned buffer right after small-sized buffer returned zero address.\n");
		CU_ASSERT(1);
	} else {
		printf("Allocate 4k unaligned buffer right after small-sized buffer returned vaddr %p raddr 0x%lx\n",
		       vaddr1, iova1);
#if ADEBUG
		printf("After storage allocation:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
	}

	spdk_lmempc_release_real_storage(sb, iova);
	spdk_lmempc_release_real_storage(sb, iova1);
#if ADEBUG
	printf("After storage release:\n");
	spdk_lmempc_dump_allocator(sb);
#endif

	printf("\nTesting asking for same range over and over passing the whole high memory range...\n");
	iova = iova1 = iova2 = 0;
	printf("  lowest 0x%lx highest 0x%lx\n", iovas, (iovas + elsize));
	vaddr = spdk_lmempc_allocate_storage(sb, iovas, (iovas + elsize), 4096, 0, &iova);
	vaddr1 = spdk_lmempc_allocate_storage(sb, iovas, (iovas + elsize), 4096, 0, &iova1);
	vaddr2 = spdk_lmempc_allocate_storage(sb, iovas, (iovas + elsize), 4096, 0, &iova2);
	if (vaddr == NULL) {
		printf("Allocate storage returned zero address.\n");
		CU_ASSERT(1);
	} else {
		printf("Allocate storage returned vaddr %p raddr 0x%lx\n", vaddr, iova);
		printf("Allocate storage returned vaddr1 %p raddr1 %lx\n", vaddr1, iova1);
		printf("Allocate storage returned vaddr2 %p raddr2 %lx\n", vaddr2, iova2);
#if ADEBUG
		printf("After storage allocation:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
		spdk_lmempc_release_storage(sb, vaddr);
		spdk_lmempc_release_real_storage(sb, iova1);
		spdk_lmempc_release_storage(sb, vaddr2);
#if ADEBUG
		printf("After storage release:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
	}

	printf("\nTesting asking for same range over and over passing front of the high memory range...\n");
	iova = iova1 = iova2 = 0;
	printf("  lowest 0x%lx highest 0x%lx\n", iovas, (iovas + 0x100000));
	vaddr = spdk_lmempc_allocate_storage(sb, iovas, (iovas + 0x100000), 4096, 0, &iova);
	vaddr1 = spdk_lmempc_allocate_storage(sb, iovas, (iovas + 0x100000), 4096, 0, &iova1);
	vaddr2 = spdk_lmempc_allocate_storage(sb, iovas, (iovas + 0x100000), 4096, 0, &iova2);
	if (vaddr == NULL) {
		printf("Allocate storage returned zero address.\n");
		CU_ASSERT(1);
	} else {
		printf("Allocate storage returned vaddr %p raddr 0x%lx\n", vaddr, iova);
		printf("Allocate storage returned vaddr1 %p raddr1 %lx\n", vaddr1, iova1);
		printf("Allocate storage returned vaddr2 %p raddr2 %lx\n", vaddr2, iova2);
#if ADEBUG
		printf("After storage allocation:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
		spdk_lmempc_release_storage(sb, vaddr);
		spdk_lmempc_release_real_storage(sb, iova1);
		spdk_lmempc_release_storage(sb, vaddr2);
#if ADEBUG
		printf("After storage release:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
	}

	printf("\nTesting asking for same range over and over passing back of the high memory range...\n");
	iova = iova1 = iova2 = 0;
	printf("  lowest 0x%lx highest 0x%lx\n", (iovas + elsize - 0x100000), (iovas + elsize));
	vaddr = spdk_lmempc_allocate_storage(sb, (iovas + elsize - 0x100000), (iovas + elsize), 4096, 0,
					     &iova);
	vaddr1 = spdk_lmempc_allocate_storage(sb, (iovas + elsize - 0x100000), (iovas + elsize), 4096, 0,
					      &iova1);
	vaddr2 = spdk_lmempc_allocate_storage(sb, (iovas + elsize - 0x100000), (iovas + elsize), 4096, 0,
					      &iova2);
	if (vaddr == NULL) {
		printf("Allocate storage returned zero address.\n");
		CU_ASSERT(1);
	} else {
		printf("Allocate storage returned vaddr %p raddr 0x%lx\n", vaddr, iova);
		printf("Allocate storage returned vaddr1 %p raddr1 %lx\n", vaddr1, iova1);
		printf("Allocate storage returned vaddr2 %p raddr2 %lx\n", vaddr2, iova2);
#if ADEBUG
		printf("After storage allocation:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
		spdk_lmempc_release_storage(sb, vaddr);
		spdk_lmempc_release_real_storage(sb, iova1);
		spdk_lmempc_release_storage(sb, vaddr2);
#if ADEBUG
		printf("After storage release:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
	}

	printf("\nTesting asking for same range over and over passing lower than the front of the high memory range...\n");
	iova = iova1 = iova2 = 0;
	printf("  lowest 0x%lx highest 0x%lx\n", (iovas - 0x1000000), (iovas + 0x100000));
	vaddr = spdk_lmempc_allocate_storage(sb, (iovas - 0x1000000), (iovas + 0x100000), 4096, 0, &iova);
	vaddr1 = spdk_lmempc_allocate_storage(sb, (iovas - 0x1000000), (iovas + 0x100000), 4096, 0, &iova1);
	vaddr2 = spdk_lmempc_allocate_storage(sb, (iovas - 0x1000000), (iovas + 0x100000), 4096, 0, &iova2);
	if (vaddr == NULL) {
		printf("Allocate storage returned zero address.\n");
		CU_ASSERT(1);
	} else {
		printf("Allocate storage returned vaddr %p raddr 0x%lx\n", vaddr, iova);
		printf("Allocate storage returned vaddr1 %p raddr1 %lx\n", vaddr1, iova1);
		printf("Allocate storage returned vaddr2 %p raddr2 %lx\n", vaddr2, iova2);
#if ADEBUG
		printf("After storage allocation:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
		spdk_lmempc_release_storage(sb, vaddr);
		spdk_lmempc_release_real_storage(sb, iova1);
		spdk_lmempc_release_storage(sb, vaddr2);
#if ADEBUG
		printf("After storage release:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
	}

	printf("\nTesting asking for same range over and over passing higher than the whole high memory range...\n");
	iova = iova1 = iova2 = 0;
	printf("  lowest 0x%lx highest 0x%lx\n", iovas, (iovas + elsize + 0x100000));
	vaddr = spdk_lmempc_allocate_storage(sb, iovas, (iovas + elsize + 0x100000), 4096, 0, &iova);
	vaddr1 = spdk_lmempc_allocate_storage(sb, iovas, (iovas + elsize) + 0x100000, 4096, 0, &iova1);
	vaddr2 = spdk_lmempc_allocate_storage(sb, iovas, (iovas + elsize + 0x100000), 4096, 0, &iova2);
	if (vaddr == NULL) {
		printf("Allocate storage returned zero address.\n");
		CU_ASSERT(1);
	} else {
		printf("Allocate storage returned vaddr %p raddr 0x%lx\n", vaddr, iova);
		printf("Allocate storage returned vaddr1 %p raddr1 %lx\n", vaddr1, iova1);
		printf("Allocate storage returned vaddr2 %p raddr2 %lx\n", vaddr2, iova2);
#if ADEBUG
		printf("After storage allocation:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
		spdk_lmempc_release_storage(sb, vaddr);
		spdk_lmempc_release_real_storage(sb, iova1);
		spdk_lmempc_release_storage(sb, vaddr2);
#if ADEBUG
		printf("After storage release:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
	}

	printf("\nTesting asking for same range over and over passing bigger than the whole high memory range...\n");
	iova = iova1 = iova2 = 0;
	printf("  lowest 0x%lx highest 0x%lx\n", (iovas - 0x1000000), (iovas + elsize + 0x100000));
	vaddr = spdk_lmempc_allocate_storage(sb, (iovas - 0x1000000), (iovas + elsize + 0x100000), 4096, 0,
					     &iova);
	vaddr1 = spdk_lmempc_allocate_storage(sb, (iovas - 0x1000000), (iovas + elsize) + 0x100000, 4096, 0,
					      &iova1);
	vaddr2 = spdk_lmempc_allocate_storage(sb, (iovas - 0x1000000), (iovas + elsize + 0x100000), 4096, 0,
					      &iova2);
	if (vaddr == NULL) {
		printf("Allocate storage returned zero address.\n");
		CU_ASSERT(1);
	} else {
		printf("Allocate storage returned vaddr %p raddr 0x%lx\n", vaddr, iova);
		printf("Allocate storage returned vaddr1 %p raddr1 %lx\n", vaddr1, iova1);
		printf("Allocate storage returned vaddr2 %p raddr2 %lx\n", vaddr2, iova2);
#if ADEBUG
		printf("After storage allocation:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
		spdk_lmempc_release_storage(sb, vaddr);
		spdk_lmempc_release_real_storage(sb, iova1);
		spdk_lmempc_release_storage(sb, vaddr2);
#if ADEBUG
		printf("After storage release:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
	}

	printf("\nTesting round up of lower address, round down of upper...\n");
	vaddr = spdk_lmempc_allocate_storage(sb, (iovas + 0x7), (iovas + 0x8000 + 0x14), 4096, 12, &iova);
	if (vaddr == NULL) {
		printf("Allocate round up of lower address, round down of upper returned zero address.\n");
		CU_ASSERT(1);
	} else {
		printf("Allocate round up of lower address, round down of upper returned vaddr %p raddr 0x%lx\n",
		       vaddr, iova);
#if ADEBUG
		printf("After storage allocation:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
		spdk_lmempc_release_real_storage(sb, iova);
#if ADEBUG
		printf("After storage release:\n");
		spdk_lmempc_dump_allocator(sb);
#endif
	}

	printf("\nTesting illegal requests...\n");
	vaddr = spdk_lmempc_allocate_storage(sb, (iovas + 0x8000), ((uint64_t) -1), 0, 0, &iova);
	if (vaddr == NULL) {
		printf("Allocate zero length storage fails as expected.\n");
	} else {
		printf("Allocate zero length storage returned value but shouldn't have.\n");
		CU_ASSERT(1);
	}

	vaddr = spdk_lmempc_allocate_storage(sb, (iovas + 0x8000), ((uint64_t) -1), 0, 99, &iova);
	if (vaddr == NULL) {
		printf("Allocate with bad alignment fails as expected.\n");
	} else {
		printf("Allocate with bad alignment returned value but shouldn't have.\n");
		CU_ASSERT(1);
	}
	vaddr = spdk_lmempc_allocate_storage(sb, (iovas - 0x8000), (iovas - 0x4000), 4096, 0, &iova);
	if (vaddr == NULL) {
		printf("Allocate outside free space fails as expected.\n");
	} else {
		printf("Allocate outside free space returned value but shouldn't have.\n");
		CU_ASSERT(1);
	}

	vaddr = spdk_lmempc_allocate_storage(sb, 0, 0, 4096, 0, &iova);
	vaddr2 = spdk_lmempc_allocate_storage(sb, iova, iova, 4096, 0, &iova2);
	if (vaddr2 == NULL) {
		printf("Attempt to allocate already allocated storage fails as expected.\n");
	} else {
		printf("Allocate already-allocated storage returned value but shouldn't have.\n");
		CU_ASSERT(1);
	}

	spdk_lmempc_release_storage(sb, vaddr);

	printf("\nTesting release of storage not allocated.\n");
	spdk_lmempc_release_storage(sb, NULL);
	spdk_lmempc_release_real_storage(sb, 0);
	spdk_lmempc_release_storage(sb, (void *) 0xdead);
	spdk_lmempc_release_real_storage(sb, 0xdead);

#if ADEBUG
	printf("After all tests\n");
	spdk_lmempc_dump_allocator(sb);
#endif

#if 0
	spdk_lmempa_free_mempool(sb);
#endif

	spdk_lmempc_exit_allocator(sb);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("spdk_lmemp", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "alloc_test", alloc_test) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);

	CU_basic_run_tests();

	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
