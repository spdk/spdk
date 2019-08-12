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

#ifndef SPDK_TRIE_H_
#define SPDK_TRIE_H_

#include "spdk/stdinc.h"
#include "spdk/queue.h"

typedef void *trie_val_t;

#define SPDK_DEFAULT_ROOT ""
#define SPDK_TRIE_SEPARATOR '/'
#define SPDK_TRIE_SEPARATOR_STR "/"
#define SPDK_TRIE_SEPARATOR_LEN strlen(SPDK_TRIE_SEPARATOR_STR)
#define SPDK_TRIE_MAX_KEY_LEN 256


struct trie_node {
	/* char * full_key; */
	char key[SPDK_TRIE_MAX_KEY_LEN];
	trie_val_t value;

	int child_num;
	struct trie_node *parent;
	TAILQ_HEAD(, trie_node)	childs;
	TAILQ_ENTRY(trie_node)	tailq;
};

struct trie_node *spdk_trie_node_create(const char *key, trie_val_t val);

void spdk_trie_node_full_key(struct trie_node *node, char *name);

struct trie_node *spdk_trie_search(struct trie_node *root, const char *key);

typedef void (*spdk_trie_init_with_child_node)(struct trie_node *node);

typedef void (*spdk_trie_free_value)(void *value);

typedef void (*spdk_trie_init_without_child_node)(struct trie_node *node, trie_val_t val);

/* todo change the callback to void * */
void spdk_trie_update(struct trie_node *root, const char *key, trie_val_t val,
		      spdk_trie_init_with_child_node init_wc_node_cb, spdk_trie_init_without_child_node init_woc_node_cb);

/* todo */
void spdk_trie_add_with_parent(struct trie_node *parent, char *key, trie_val_t val,
			       spdk_trie_init_with_child_node init_wc_node_cb, spdk_trie_init_without_child_node init_woc_node_cb);

int spdk_trie_remove(struct trie_node *root, const char *key, spdk_trie_free_value free_cb);

void spdk_trie_free(struct trie_node *root, spdk_trie_free_value free_cb);

struct trie_node *spdk_trie_search(struct trie_node *root, const char *key);

struct trie_node *spdk_trie_search_parent_path(struct trie_node *root, const char *key);

#endif /* SPDK_TRIE_H_ */
