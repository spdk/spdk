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

#include "blobfs/tree.c"

void
cache_buffer_free(struct cache_buffer *cache_buffer)
{
	free(cache_buffer);
}

static void
blobfs_tree_op_test(void)
{
	struct cache_tree *tree;
	struct cache_buffer *buffer[5];
	struct cache_buffer *tmp_buffer;
	int i;

	for (i = 0; i < 5; i ++) {
		buffer[i] = calloc(1, sizeof(struct cache_buffer));
		SPDK_CU_ASSERT_FATAL(buffer[i]);
	}

	tree = calloc(1, sizeof(*tree));
	SPDK_CU_ASSERT_FATAL(tree != NULL);

	/* insert buffer[0] */
	buffer[0]->offset = 0;
	tree = tree_insert_buffer(tree, buffer[0]);
	SPDK_CU_ASSERT_FATAL(tree != NULL);
	CU_ASSERT(tree->level == 0);
	tmp_buffer = tree_find_buffer(tree, buffer[0]->offset);
	CU_ASSERT(tmp_buffer == buffer[0]);

	/* insert buffer[1] */
	buffer[1]->offset = CACHE_BUFFER_SIZE;
	/* set the bytes_filled equal = bytes_filled with same non zero value, e.g., 32 */
	buffer[1]->bytes_filled = buffer[1]->bytes_flushed = 32;
	tree = tree_insert_buffer(tree, buffer[1]);
	SPDK_CU_ASSERT_FATAL(tree != NULL);
	CU_ASSERT(tree->level == 0);
	tmp_buffer = tree_find_filled_buffer(tree, buffer[1]->offset);
	CU_ASSERT(tmp_buffer == buffer[1]);

	/* insert buffer[2] */
	buffer[2]->offset = (CACHE_TREE_WIDTH - 1) * CACHE_BUFFER_SIZE;
	tree = tree_insert_buffer(tree, buffer[2]);
	SPDK_CU_ASSERT_FATAL(tree != NULL);
	CU_ASSERT(tree->level == 0);
	tmp_buffer = tree_find_buffer(tree, buffer[2]->offset);
	CU_ASSERT(tmp_buffer == buffer[2]);
	tmp_buffer = tree_find_filled_buffer(tree, buffer[2]->offset);
	CU_ASSERT(tmp_buffer == NULL);

	/* insert buffer[3], set an offset which can not be fit level 0 */
	buffer[3]->offset = CACHE_TREE_LEVEL_SIZE(1);
	tree = tree_insert_buffer(tree, buffer[3]);
	SPDK_CU_ASSERT_FATAL(tree != NULL);
	CU_ASSERT(tree->level == 1);
	tmp_buffer = tree_find_buffer(tree, buffer[3]->offset);
	CU_ASSERT(tmp_buffer == buffer[3]);

	/*  insert buffer[4], set an offset which can not be fit level 1 */
	buffer[4]->offset = CACHE_TREE_LEVEL_SIZE(2);
	tree = tree_insert_buffer(tree, buffer[4]);
	SPDK_CU_ASSERT_FATAL(tree != NULL);
	CU_ASSERT(tree->level == 2);
	tmp_buffer = tree_find_buffer(tree, buffer[4]->offset);
	CU_ASSERT(tmp_buffer == buffer[4]);

	/* delete buffer[0] */
	tree_remove_buffer(tree, buffer[0]);
	/* check whether buffer[0] is still existed or not */
	tmp_buffer = tree_find_buffer(tree, 0);
	CU_ASSERT(tmp_buffer == NULL);

	/* delete buffer[3] */
	tree_remove_buffer(tree, buffer[3]);
	/* check whether buffer[3] is still existed or not */
	tmp_buffer = tree_find_buffer(tree, CACHE_TREE_LEVEL_SIZE(1));
	CU_ASSERT(tmp_buffer == NULL);

	/* free all buffers in the tree */
	tree_free_buffers(tree);

	/* check whether buffer[1] is still existed or not */
	tmp_buffer = tree_find_buffer(tree, CACHE_BUFFER_SIZE);
	CU_ASSERT(tmp_buffer == NULL);
	/* check whether buffer[2] is still existed or not */
	tmp_buffer = tree_find_buffer(tree, (CACHE_TREE_WIDTH - 1) * CACHE_BUFFER_SIZE);
	CU_ASSERT(tmp_buffer == NULL);
	/* check whether buffer[4] is still existed or not */
	tmp_buffer = tree_find_buffer(tree, CACHE_TREE_LEVEL_SIZE(2));
	CU_ASSERT(tmp_buffer == NULL);

	/* According to tree_free_buffers, root will not be freed */
	free(tree);
}

int main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("tree", NULL, NULL);
	CU_ADD_TEST(suite, blobfs_tree_op_test);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
