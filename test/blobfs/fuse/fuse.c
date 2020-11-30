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

#define FUSE_USE_VERSION 30
#include "fuse3/fuse.h"
#include "fuse3/fuse_lowlevel.h"

#include "spdk/blobfs.h"
#include "spdk/bdev.h"
#include "spdk/event.h"
#include "spdk/thread.h"
#include "spdk/blob_bdev.h"
#include "spdk/blobfs_bdev.h"
#include "spdk/log.h"
#include "spdk/string.h"

char *g_bdev_name;
char *g_mountpoint;

int g_fuse_argc = 0;
char **g_fuse_argv = NULL;

static void
fuse_run_cb(void *cb_arg, int fserrno)
{
	if (fserrno) {
		printf("Failed to mount filesystem on bdev %s to path %s: %s\n",
		       g_bdev_name, g_mountpoint, spdk_strerror(fserrno));

		spdk_app_stop(0);
		return;
	}

	printf("done.\n");
}

static void
spdk_fuse_run(void *arg1)
{
	printf("Mounting filesystem on bdev %s to path %s...\n",
	       g_bdev_name, g_mountpoint);
	fflush(stdout);

	spdk_blobfs_bdev_mount(g_bdev_name, g_mountpoint, fuse_run_cb, NULL);
}

static void
spdk_fuse_shutdown(void)
{
	spdk_app_stop(0);
}

int main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc = 0;

	if (argc < 4) {
		fprintf(stderr, "usage: %s <conffile> <bdev name> <mountpoint>\n", argv[0]);
		exit(1);
	}

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "spdk_fuse";
	opts.json_config_file = argv[1];
	opts.reactor_mask = "0x3";
	opts.shutdown_cb = spdk_fuse_shutdown;

	g_bdev_name = argv[2];
	g_mountpoint = argv[3];

	/* TODO: mount blobfs with extra FUSE options. */
	g_fuse_argc = argc - 2;
	g_fuse_argv = &argv[2];

	spdk_fs_set_cache_size(512);

	rc = spdk_app_start(&opts, spdk_fuse_run, NULL);
	spdk_app_fini();

	return rc;
}
