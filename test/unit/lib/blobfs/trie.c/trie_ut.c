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
#include "blobfs/trie.c"

enum spdk_dir_node_type {
	SPDK_NODE_INVALID = 0,
	SPDK_NODE_DIR = 1,
	SPDK_NODE_FILE = 2
};


struct spdk_dir_node {
	enum spdk_dir_node_type type;

	uint64_t			mode;
	uint32_t			ref;
};

void trie_free_value(struct trie_node *node);
void trie_update_with_child_node_cb(struct trie_node *node);



void trie_free_value(struct trie_node *node)
{

}


void trie_update_with_child_node_cb(struct trie_node *node)
{
	struct spdk_dir_node *value;

	value = spdk_trie_node_get_val(node);

	if (value->type != SPDK_NODE_INVALID) {
		return;
	}

	value->type = SPDK_NODE_DIR;
}

static void
blobfs_trie_path_test(void)
{
	struct spdk_dir_node *root_dir, *temp_file;
	struct trie_node *root, *temp_node;
	char path[100];
	memset(path, 0, 100);

	root = spdk_trie_node_create("", sizeof(struct spdk_dir_node));
	root_dir = spdk_trie_node_get_val(root);
	root_dir->type = SPDK_NODE_DIR;

	temp_node = spdk_trie_update(root, "/a/d", sizeof(struct spdk_dir_node),
				     trie_update_with_child_node_cb);
	temp_file = spdk_trie_node_get_val(temp_node);
	temp_file->type = SPDK_NODE_FILE;

	spdk_trie_node_full_key(temp_node, path);
	CU_ASSERT(strcmp(path, "/a/d") == 0);

	spdk_trie_free(root, NULL);
}



static void
blobfs_trie_remove_test(void)
{
	struct spdk_dir_node *root_dir, *temp_file;
	struct trie_node *root, *temp_node;
	int r;

	root = spdk_trie_node_create("", sizeof(struct spdk_dir_node));
	root_dir = spdk_trie_node_get_val(root);

	root_dir->type = SPDK_NODE_DIR;
	temp_node = spdk_trie_update(root, "/a", sizeof(struct spdk_dir_node),
				     trie_update_with_child_node_cb);
	temp_file = spdk_trie_node_get_val(temp_node);
	temp_file->type = SPDK_NODE_DIR;

	temp_node = spdk_trie_update(root, "/a/d", sizeof(struct spdk_dir_node),
				     trie_update_with_child_node_cb);
	temp_file = spdk_trie_node_get_val(temp_node);
	temp_file->type = SPDK_NODE_FILE;

	r = spdk_trie_remove(root, "/a/d", trie_free_value);
	CU_ASSERT(r == 0);

	temp_node = spdk_trie_search(root, "/a/d");
	CU_ASSERT(temp_node == NULL);

	r = spdk_trie_remove(root, "/a", NULL);
	CU_ASSERT(r == 0);

	r = spdk_trie_remove(root, "/a/d/", NULL);
	CU_ASSERT(r == 0);
	CU_ASSERT(root->child_num == 0);

	spdk_trie_free(root, NULL);
}



static void
blobfs_trie_update_test(void)
{
	struct spdk_dir_node *root_dir, *temp_file;
	struct trie_node *root;
	struct trie_node *temp_node;

	root = spdk_trie_node_create("", sizeof(struct spdk_dir_node));
	root_dir = spdk_trie_node_get_val(root);
	root_dir->type = SPDK_NODE_DIR;

	temp_node = spdk_trie_update(root, "/a", sizeof(struct spdk_dir_node),
				     trie_update_with_child_node_cb);
	temp_file = spdk_trie_node_get_val(temp_node);
	temp_file->type = SPDK_NODE_DIR;

	temp_node = spdk_trie_update(root, "/a/d", sizeof(struct spdk_dir_node),
				     trie_update_with_child_node_cb);
	temp_file = spdk_trie_node_get_val(temp_node);
	temp_file->type = SPDK_NODE_FILE;

	temp_node = spdk_trie_search(root, "/a");
	temp_file = spdk_trie_node_get_val(temp_node);
	CU_ASSERT(temp_node != NULL);
	CU_ASSERT(temp_file->type == SPDK_NODE_DIR);
	CU_ASSERT(strcmp(temp_node->key, "a") == 0);
	CU_ASSERT(temp_node->child_num == 1);

	temp_node = spdk_trie_update(root, "/b/c", sizeof(struct spdk_dir_node),
				     trie_update_with_child_node_cb);
	temp_file = spdk_trie_node_get_val(temp_node);
	temp_file->type = SPDK_NODE_FILE;

	temp_node = spdk_trie_search(root, "/b/c");
	temp_file = spdk_trie_node_get_val(temp_node);
	CU_ASSERT(temp_node != NULL);
	CU_ASSERT(temp_file->type == SPDK_NODE_FILE);
	CU_ASSERT(strcmp(temp_node->key, "c") == 0);

	temp_node = temp_node->parent;
	temp_file = spdk_trie_node_get_val(temp_node);
	CU_ASSERT(temp_node != NULL);
	CU_ASSERT(temp_file->type == SPDK_NODE_DIR);
	CU_ASSERT(strcmp(temp_node->key, "b") == 0);

	spdk_trie_free(root, trie_free_value);
}


static void
blobfs_trie_create_test(void)
{
	struct spdk_dir_node *root_dir, *temp_file;
	struct trie_node *root;
	struct trie_node *temp_node;

	root = spdk_trie_node_create("", sizeof(struct spdk_dir_node));
	root_dir = spdk_trie_node_get_val(root);
	root_dir->type = SPDK_NODE_DIR;
	CU_ASSERT(root != NULL);

	temp_node = spdk_trie_node_create("a", sizeof(struct spdk_dir_node));
	temp_file = spdk_trie_node_get_val(temp_node);
	temp_file->type = SPDK_NODE_FILE;
	CU_ASSERT(temp_node != NULL);

	spdk_trie_free(root, trie_free_value);
	spdk_trie_free(temp_node, trie_free_value);
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

	if (
		CU_add_test(suite, "blobfs_trie_create_test", blobfs_trie_create_test) == NULL  ||
		CU_add_test(suite, "blobfs_trie_update_test", blobfs_trie_update_test) == NULL  ||
		CU_add_test(suite, "blobfs_trie_remove_test", blobfs_trie_remove_test) == NULL  ||
		CU_add_test(suite, "blobfs_trie_path_test", blobfs_trie_path_test) == NULL
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
