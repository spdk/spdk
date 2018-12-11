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

#include "spdk/env.h"
#include "spdk_cunit.h"

#include <rte_config.h>
#include <rte_version.h>
#include <rte_malloc.h>
#include <rte_eal_memconfig.h>

#if RTE_VERSION < RTE_VERSION_NUM(18, 05, 0, 0)

static void
test(void)
{
	printf("DPDK version %s does not support memory callbacks\n", rte_version());
}

#else

struct mem_allocation {
	uintptr_t			vaddr;
	size_t				len;
	TAILQ_ENTRY(mem_allocation)	link;
};

static TAILQ_HEAD(, mem_allocation) g_mem_allocations = TAILQ_HEAD_INITIALIZER(g_mem_allocations);

static void
memory_hotplug_cb(enum rte_mem_event event_type, const void *addr, size_t len, void *arg)
{
	struct mem_allocation *allocation;

	if (event_type == RTE_MEM_EVENT_ALLOC) {
		allocation = calloc(1, sizeof(*allocation));
		SPDK_CU_ASSERT_FATAL(allocation != NULL);

		printf("register %p %ju\n", addr, len);
		allocation->vaddr = (uintptr_t)addr;
		allocation->len = len;
		TAILQ_INSERT_TAIL(&g_mem_allocations, allocation, link);
	} else if (event_type == RTE_MEM_EVENT_FREE) {
		printf("unregister %p %ju\n", addr, len);
		TAILQ_FOREACH(allocation, &g_mem_allocations, link) {
			if (allocation->vaddr == (uintptr_t)addr && allocation->len == len) {
				break;
			}
		}
		SPDK_CU_ASSERT_FATAL(allocation != NULL);
		TAILQ_REMOVE(&g_mem_allocations, allocation, link);
	}
}

static int
memory_iter_cb(const struct rte_memseg_list *msl,
	       const struct rte_memseg *ms, size_t len, void *arg)
{
	struct mem_allocation *allocation;

	allocation = calloc(1, sizeof(*allocation));
	SPDK_CU_ASSERT_FATAL(allocation != NULL);

	printf("register %p %ju\n", ms->addr, len);
	allocation->vaddr = (uintptr_t)ms->addr;
	allocation->len = len;
	TAILQ_INSERT_TAIL(&g_mem_allocations, allocation, link);

	return 0;
}

static void
verify_buffer(void *_buf, size_t len)
{
	uintptr_t buf = (uintptr_t)_buf;
	struct mem_allocation *allocation;

	SPDK_CU_ASSERT_FATAL(_buf != NULL);
	printf("buf %p len %ju ", _buf, len);
	TAILQ_FOREACH(allocation, &g_mem_allocations, link) {
		if (buf >= allocation->vaddr &&
		    buf + len <= allocation->vaddr + allocation->len) {
			break;
		}
	}
	printf("%s\n", allocation == NULL ? "FAILED" : "PASSED");
	CU_ASSERT(allocation != NULL);
}

static void
test(void)
{
	void *buf1, *buf2, *buf3, *buf4;
	size_t len1, len2, len3, len4;

	printf("\n");

	rte_mem_event_callback_register("test", memory_hotplug_cb, NULL);
	rte_memseg_contig_walk(memory_iter_cb, NULL);

	len1 = 3 * 1024 * 1024;
	printf("malloc %ju\n", len1);
	buf1 = rte_malloc(NULL, len1, 0);
	verify_buffer(buf1, len1);

	len2 = 64;
	printf("malloc %ju\n", len2);
	buf2 = rte_malloc(NULL, len2, 0);
	verify_buffer(buf2, len2);

	len3 = 4 * 1024 * 1024;
	printf("malloc %ju\n", len3);
	buf3 = rte_malloc(NULL, len3, 0);
	verify_buffer(buf3, len3);

	printf("free %p %ju\n", buf1, len1);
	rte_free(buf1);
	printf("free %p %ju\n", buf2, len2);
	rte_free(buf2);
	printf("free %p %ju\n", buf3, len3);
	rte_free(buf3);

	len4 = 8 * 1024 * 1024;
	printf("malloc %ju\n", len4);
	buf4 = rte_malloc(NULL, len4, 0);
	verify_buffer(buf4, len4);

	printf("free %p %ju\n", buf4, len4);
	rte_free(buf4);
}

#endif

int
main(int argc, char **argv)
{
	struct spdk_env_opts	opts;
	CU_pSuite		suite = NULL;
	unsigned int		num_failures;

	spdk_env_opts_init(&opts);
	opts.name = "mem_callbacks";
	opts.core_mask = "0x1";
	if (spdk_env_init(&opts) < 0) {
		printf("Err: Unable to initialize SPDK env\n");
		return 1;
	}

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("memory", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "test", test) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
