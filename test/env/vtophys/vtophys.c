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

#define DEFAULT_TRANSLATION	0xDEADBEEF0BADF00DULL

static struct spdk_mem_map *g_mem_map;
static void *g_last_registered_vaddr;

static int
vtophys_negative_test(void)
{
	void *p = NULL;
	int i;
	unsigned int size = 1;
	int rc = 0;

	for (i = 0; i < 31; i++) {
		p = malloc(size);
		if (p == NULL) {
			continue;
		}

		if (spdk_vtophys(p) != SPDK_VTOPHYS_ERROR) {
			rc = -1;
			printf("Err: VA=%p is mapped to a huge_page,\n", p);
			free(p);
			break;
		}

		free(p);
		size = size << 1;
	}

	/* Test addresses that are not in the valid x86-64 usermode range */

	if (spdk_vtophys((void *)0x0000800000000000ULL) != SPDK_VTOPHYS_ERROR) {
		rc = -1;
		printf("Err: kernel-mode address incorrectly allowed\n");
	}

	if (!rc) {
		printf("vtophys_negative_test passed\n");
	} else {
		printf("vtophys_negative_test failed\n");
	}

	return rc;
}

static int
vtophys_positive_test(void)
{
	void *p = NULL;
	int i;
	unsigned int size = 1;
	int rc = 0;

	for (i = 0; i < 31; i++) {
		p = spdk_dma_zmalloc(size, 512, NULL);
		if (p == NULL) {
			continue;
		}

		if (spdk_vtophys(p) == SPDK_VTOPHYS_ERROR) {
			rc = -1;
			printf("Err: VA=%p is not mapped to a huge_page,\n", p);
			spdk_dma_free(p);
			break;
		}

		spdk_dma_free(p);
		size = size << 1;
	}

	if (!rc) {
		printf("vtophys_positive_test passed\n");
	} else {
		printf("vtophys_positive_test failed\n");
	}

	return rc;
}

static int
test_map_notify(void *cb_ctx, struct spdk_mem_map *map,
		enum spdk_mem_map_notify_action action,
		void *vaddr, size_t size)
{
	const char *action_str = "unknown";

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		action_str = "register";
		g_last_registered_vaddr = vaddr;
		spdk_mem_map_set_translation(map, (uint64_t)vaddr, size, (uint64_t)vaddr);
		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
		action_str = "unregister";
		break;
	}

	printf("%s: %s %p-%p (%zu bytes)\n", __func__, action_str, vaddr, vaddr + size - 1, size);
	return 0;
}

static int
test_map_notify_fail(void *cb_ctx, struct spdk_mem_map *map,
		     enum spdk_mem_map_notify_action action,
		     void *vaddr, size_t size)
{

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		if (vaddr == g_last_registered_vaddr) {
			/* Test the error handling */
			spdk_mem_map_clear_translation(g_mem_map, (uint64_t)vaddr, size);
			return -1;
		}
		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
		/* Clear the same region in the other mem_map to be able to
		 * verify that there was no memory left still registered after
		 * the mem_map creation failure.
		 */
		spdk_mem_map_clear_translation(g_mem_map, (uint64_t)vaddr, size);
		break;
	}

	return 0;
}

static int
test_map_notify_verify(void *cb_ctx, struct spdk_mem_map *map,
		       enum spdk_mem_map_notify_action action,
		       void *vaddr, size_t size)
{
	uint64_t reg, reg_size;

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER:
		reg = spdk_mem_map_translate(g_mem_map, (uint64_t)vaddr, &reg_size);
		if (reg != DEFAULT_TRANSLATION) {
			return -1;
		}
		break;
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
		break;
	}

	return 0;
}

static int
mem_map_test(void)
{
	struct spdk_mem_map *map;
	struct spdk_mem_map_ops test_map_ops = {
		.notify_cb = test_map_notify,
		.are_contiguous = NULL
	};

	g_mem_map = spdk_mem_map_alloc(DEFAULT_TRANSLATION, &test_map_ops, NULL);
	if (g_mem_map == NULL) {
		return 1;
	}

	test_map_ops.notify_cb = test_map_notify_fail;
	map = spdk_mem_map_alloc(DEFAULT_TRANSLATION, &test_map_ops, NULL);
	if (map != NULL) {
		printf("Err: successfully created incomplete mem_map\n");
		spdk_mem_map_free(&map);
		spdk_mem_map_free(&g_mem_map);
		return 1;
	}

	/* Register another map to walk through all memory regions
	 * again and verify that all of them were unregistered by
	 * the failed mem_map alloc above.
	 */
	test_map_ops.notify_cb = test_map_notify_verify;
	map = spdk_mem_map_alloc(DEFAULT_TRANSLATION, &test_map_ops, NULL);
	if (map == NULL) {
		printf("Err: failed mem_map creation leaked memory registrations\n");
		spdk_mem_map_free(&g_mem_map);
		return 1;
	}

	spdk_mem_map_free(&map);
	spdk_mem_map_free(&g_mem_map);
	return 0;
}

int
main(int argc, char **argv)
{
	int			rc;
	struct spdk_env_opts	opts;

	spdk_env_opts_init(&opts);
	opts.name = "vtophys";
	opts.core_mask = "0x1";
	opts.mem_size = 256;
	if (spdk_env_init(&opts) < 0) {
		printf("Err: Unable to initialize SPDK env\n");
		return 1;
	}

	rc = vtophys_negative_test();
	if (rc < 0) {
		return rc;
	}

	rc = vtophys_positive_test();
	if (rc < 0) {
		return rc;
	}

	rc = mem_map_test();
	return rc;
}
