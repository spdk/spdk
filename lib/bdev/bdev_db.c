/*-
 *   BSD LICENSE
 *
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
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

#include "spdk/bdev_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "spdk/bdev.h"
#include "spdk/log.h"

struct spdk_db_entry {
	struct spdk_bdev *bdev;
	int claimed;
	struct spdk_db_entry *next;
};

static struct spdk_db_entry *bdev_list_head = NULL;

int spdk_bdev_db_add(struct spdk_bdev *bdev)
{
	struct spdk_db_entry *new_entry = calloc(1, sizeof(struct spdk_db_entry));

	if (!new_entry) {
		SPDK_ERRLOG("Failed to allocate DB entry\n");
		return -ENOMEM;
	}

	new_entry->bdev = bdev;
	new_entry->next = bdev_list_head;
	bdev_list_head = new_entry;

	return 0;
}

int spdk_bdev_db_delete(struct spdk_bdev *bdev)
{
	struct spdk_db_entry *prev = NULL;
	struct spdk_db_entry *node = bdev_list_head;

	while (node != NULL) {
		if (node->bdev == bdev) {
			if (prev != NULL) {
				prev->next = node->next;
			} else {
				bdev_list_head = node->next;
			}
			free(node);
			break;
		}
		prev = node;
		node = node->next;
	}

	return 0;
}

struct spdk_bdev *spdk_bdev_db_get_by_name(const char *bdev_name)
{
	struct spdk_db_entry *current = bdev_list_head;

	while (current != NULL) {
		struct spdk_bdev *bdev = current->bdev;

		if (strncmp(bdev_name, bdev->name, sizeof(bdev->name)) == 0) {
			current->claimed++;
			return bdev;
		}

		current = current->next;
	}

	return NULL;
}
