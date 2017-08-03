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

#include "spdk_cunit.h"
#include "tree.h"

#include "tree.c"

void
spdk_cache_buffer_free(struct cache_buffer *cache_buffer)
{
}

static void
insert_buffer_test(void)
{
	struct cache_tree tree = {};
	struct cache_tree *tree_p = &tree;
	struct cache_buffer buffer[4];

	buffer[0].offset = 0;
	tree_p = spdk_tree_insert_buffer(tree_p, &buffer[0]);
	CU_ASSERT(tree_p != NULL);
	CU_ASSERT(tree_p->level == 0);

	buffer[1].offset = CACHE_BUFFER_SIZE;
	tree_p = spdk_tree_insert_buffer(tree_p, &buffer[1]);
	CU_ASSERT(tree_p != NULL);
	CU_ASSERT(tree_p->level == 0);

	/* set an offset which can not be fit level 0 */
	buffer[2].offset = CACHE_TREE_LEVEL_SIZE(1);
	tree_p = spdk_tree_insert_buffer(tree_p, &buffer[2]);
	CU_ASSERT(tree_p != NULL);
	CU_ASSERT(tree_p->level == 1);

	/* set an offset which can not be fit level 1 */
	buffer[3].offset = CACHE_TREE_LEVEL_SIZE(2);
	tree_p = spdk_tree_insert_buffer(tree_p, &buffer[3]);
	CU_ASSERT(tree_p != NULL);
	CU_ASSERT(tree_p->level == 2);

	spdk_tree_free_buffers(tree_p);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("tree", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "insert_buffer_test", insert_buffer_test) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
