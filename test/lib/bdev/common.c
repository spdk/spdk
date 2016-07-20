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

/* This file is included in the bdev test tools, not compiled separately. */

#include "spdk/event.h"

struct blockdev_entry {
	struct spdk_bdev	*bdev;
	struct blockdev_entry	*next;
};

struct blockdev_entry *g_bdevs = NULL;

int
spdk_bdev_db_add(struct spdk_bdev *bdev)
{
	struct blockdev_entry *bdev_entry = calloc(1, sizeof(struct blockdev_entry));

	if (bdev_entry == NULL) {
		return -ENOMEM;
	}

	bdev_entry->bdev = bdev;

	bdev_entry->next = g_bdevs;
	g_bdevs = bdev_entry;

	return 0;
}

int
spdk_bdev_db_delete(struct spdk_bdev *bdev)
{
	/* Deleting is not important */
	return 0;
}

struct spdk_bdev *
spdk_bdev_db_get_by_name(const char *bdev_name)
{
	struct blockdev_entry *bdev_entry = g_bdevs;

	while (bdev_entry != NULL) {
		if (strcmp(bdev_name, bdev_entry->bdev->name) == 0) {
			return bdev_entry->bdev;
		}
		bdev_entry = bdev_entry->next;
	}

	return NULL;
}

static void
bdevtest_init(const char *config_file, const char *cpumask)
{
	struct spdk_app_opts opts;

	spdk_app_opts_init(&opts);
	opts.name = "bdevtest";
	opts.config_file = config_file;
	opts.reactor_mask = cpumask;
	spdk_app_init(&opts);
}
