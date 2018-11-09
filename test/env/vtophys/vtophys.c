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
	return rc;
}
