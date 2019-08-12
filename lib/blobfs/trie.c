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

#include "trie.h"
#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/queue.h"


void _spdk_trie_node_full_key(struct trie_node *node, char *name, int append_len);

char *_trie_strdup(const char *s);

char *
_trie_strdup(const char *s)
{
	char *new;
	int len;

	if (strlen(s) + 1 > SPDK_TRIE_MAX_KEY_LEN) {
		return NULL;
	}

	len = s[0] == SPDK_TRIE_SEPARATOR ? strlen(s) + 1 : strlen(s) + 2;
	new = (char *)calloc(1, len);
	if (new == NULL) {
		return NULL;
	}

	if (s[0] == SPDK_TRIE_SEPARATOR) {
		new = memcpy(new, s, len);
	} else {
		new[0] = SPDK_TRIE_SEPARATOR;
		memcpy(new + 1, s, len - 1);
	}

	return new;
}

struct trie_node *spdk_trie_node_from_val(void *val)
{
	return (struct trie_node *)((uint8_t *)val - sizeof(struct trie_node));
}


struct trie_node *spdk_trie_node_create(const char *key, uint32_t val_size)
{
	struct trie_node *node;

	node = (struct trie_node *)calloc(1, sizeof(*node) + val_size);

	if (node == NULL) {
		printf("Malloc failed\n");
		return NULL;
	}
	snprintf(node->key, sizeof(node->key), "%s", key);
	TAILQ_INIT(&node->childs);
	node->child_num = 0;
	node->parent = NULL;

	return node;
}

void
spdk_trie_free(struct trie_node *root, spdk_trie_free_cb free_cb)
{
	struct trie_node *node, *tmp;

	if (root == NULL) {
		return;
	}

	if (root->parent != NULL) {
		TAILQ_REMOVE(&root->parent->childs, root, tailq);
		root->parent->child_num -= 1;
	}

	if (root->child_num == 0) {
		if (free_cb != NULL) {
			free_cb(root);
		}
		free(root);
		root = NULL;
	} else {
		TAILQ_FOREACH_SAFE(node, &root->childs, tailq, tmp) {
			spdk_trie_free(node, free_cb);
		}
		if (free_cb != NULL) {
			free_cb(root);
		}
		free(root);
		root = NULL;
	}

}

struct trie_node *
spdk_trie_update(struct trie_node *root, const char *key, uint32_t val_size,
		 spdk_trie_init_middle_path init_wc_node_cb)
{
	char *data, *free_data, *token;
	struct trie_node *cur_node, *node, *old_node;
	int no_dir = 0;

	data = _trie_strdup(key);
	free_data = data;
	cur_node = root;
	token = spdk_strsepq(&data, SPDK_TRIE_SEPARATOR_STR);

	while (token != NULL) {
		/* /xxxx/ the first str len is 0 */
		if (strlen(token) == 0) {
			token = spdk_strsepq(&data, SPDK_TRIE_SEPARATOR_STR);
			continue;
		}

		old_node = cur_node;
		TAILQ_FOREACH(node, &cur_node->childs, tailq) {
			if (strncmp(node->key, token, strlen(node->key)) == 0) {
				cur_node = node;
			}
		}

		no_dir = cur_node == old_node ? 1 : 0;

		if (no_dir) {
			node = spdk_trie_node_create(token, val_size);
			node->parent = old_node;
			TAILQ_INSERT_TAIL(&old_node->childs, node, tailq);
			old_node->child_num += 1;
			cur_node = node;
		}

		token = spdk_strsepq(&data, SPDK_TRIE_SEPARATOR_STR);
		/* if (strlen(token) == 0) means path is error ! */
		if (no_dir) {

			if (token == NULL) {
				free(free_data);
				return node;
			}

			if (token != NULL && init_wc_node_cb != NULL) {
				init_wc_node_cb(node);
			}

		} else { /* replace the node */
			if (token == NULL) {
				free(free_data);
				return node;
			}
		}
	}

	free(free_data);
	return NULL;
}


void _spdk_trie_node_full_key(struct trie_node *node, char *name, int append_len)
{
	char temp[append_len];

	memset(&temp, 0, append_len);

	if (append_len != 0) {
		snprintf(temp, append_len, "%s", name);
	}

	memcpy(name, node->key, strlen(node->key));
	memcpy(name + strlen(node->key), SPDK_TRIE_SEPARATOR_STR, SPDK_TRIE_SEPARATOR_LEN);
	memcpy(name + strlen(node->key) + SPDK_TRIE_SEPARATOR_LEN, temp, append_len);
	append_len = strlen(node->key) + SPDK_TRIE_SEPARATOR_LEN + append_len;

	if (node->parent == NULL) {
		return;
	}

	_spdk_trie_node_full_key(node->parent, name, append_len);
}

/*
 * full name always sep + ...... + sep
 */
void
spdk_trie_node_full_key(struct trie_node *node, char *name)
{
	if (node == NULL || name == NULL) {
		return;
	}

	_spdk_trie_node_full_key(node, name, 0);
}

int
spdk_trie_remove(struct trie_node *root, const char *key, spdk_trie_free_cb free_cb)
{
	struct trie_node *node;
	char *data;

	data = _trie_strdup(key);
	node = spdk_trie_search(root, data);

	if (node == NULL) {
		free(data);
		printf("not found!\n");
		return 0;
	}

	if (free_cb != NULL) {
		free_cb(node);
	}

	spdk_trie_free(node, free_cb);
	free(data);
	return 0;
}


struct trie_node *
spdk_trie_search(struct trie_node *root, const char *key)
{
	char *data, *token, *free_data;
	struct trie_node *cur_node, *node, *old_node;

	data = _trie_strdup(key);
	free_data = data; /* spdk_strsepq will change the data pointer */
	cur_node = root;
	while ((token = spdk_strsepq(&data, SPDK_TRIE_SEPARATOR_STR)) != NULL) {
		if (strlen(token) != 0) {
			old_node = cur_node;
			TAILQ_FOREACH(node, &cur_node->childs, tailq) {
				if (strncmp(node->key, token, strlen(node->key)) == 0) {
					cur_node = node;
				}
			}

			if (old_node == cur_node) {
				free(free_data);
				return NULL;
			}
		}
	}

	free(free_data);
	return cur_node;
}

struct trie_node *
spdk_trie_search_parent_path(struct trie_node *root, const char *key)
{
	char *data;
	char parent[SPDK_TRIE_MAX_KEY_LEN];
	int remain;

	data = _trie_strdup(key);
	remain = strlen(strrchr(data, SPDK_TRIE_SEPARATOR));
	if ((strlen(data) - remain) == 0) {
		free(data);
		return root;
	}

	memcpy(parent, data, (strlen(data) - remain) > SPDK_TRIE_MAX_KEY_LEN ? SPDK_TRIE_MAX_KEY_LEN :
	       (strlen(data) - remain));
	free(data);
	return spdk_trie_search(root, parent);
}
