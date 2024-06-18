/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
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

int
main(int argc, char **argv)
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
