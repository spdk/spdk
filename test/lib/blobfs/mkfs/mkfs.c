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

#include "spdk/blobfs.h"
#include "spdk/bdev.h"
#include "spdk/event.h"
#include "spdk/blob_bdev.h"
#include "spdk/log.h"

struct spdk_bs_dev *g_bs_dev;
const char *g_bdev_name;

static void
stop_cb(void *ctx, int fserrno)
{
	spdk_app_stop(0);
}

static void
shutdown_cb(void *arg1, void *arg2)
{
	struct spdk_filesystem *fs = arg1;

	printf("done.\n");
	spdk_fs_unload(fs, stop_cb, NULL);
}

static void
init_cb(void *ctx, struct spdk_filesystem *fs, int fserrno)
{
	struct spdk_event *event;

	event = spdk_event_allocate(0, shutdown_cb, fs, NULL);
	spdk_event_call(event);
}

static void
spdk_mkfs_run(void *arg1, void *arg2)
{
	struct spdk_bdev *bdev;

	bdev = spdk_bdev_get_by_name(g_bdev_name);

	if (bdev == NULL) {
		SPDK_ERRLOG("bdev %s not found\n", g_bdev_name);
		spdk_app_stop(-1);
		return;
	}

	printf("Initializing filesystem on bdev %s...", g_bdev_name);
	fflush(stdout);
	g_bs_dev = spdk_bdev_create_bs_dev(bdev);
	spdk_fs_init(g_bs_dev, NULL, init_cb, NULL);
}

int main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};

	if (argc < 3) {
		SPDK_ERRLOG("usage: %s <conffile> <bdevname>\n", argv[0]);
		exit(1);
	}

	spdk_app_opts_init(&opts);
	opts.name = "spdk_mkfs";
	opts.config_file = argv[1];
	opts.reactor_mask = "0x3";
	opts.mem_size = 1024;
	opts.shutdown_cb = NULL;

	spdk_fs_set_cache_size(512);

	g_bdev_name = argv[2];
	spdk_app_start(&opts, spdk_mkfs_run, NULL, NULL);
	spdk_app_fini();

	return 0;
}
