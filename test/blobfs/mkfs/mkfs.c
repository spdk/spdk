/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/event.h"
#include "spdk/blobfs.h"
#include "spdk/blobfs_bdev.h"
#include "spdk/log.h"
#include "spdk/string.h"

const char *g_bdev_name;
static uint64_t g_cluster_size;

static void
shutdown_cb(void *cb_arg, int fserrno)
{
	if (fserrno) {
		printf("\nFailed to initialize filesystem on bdev %s...", g_bdev_name);
	}

	printf("done.\n");

	spdk_app_stop(0);
}

static void
spdk_mkfs_run(void *arg1)
{
	printf("Initializing filesystem on bdev %s...", g_bdev_name);
	fflush(stdout);

	spdk_blobfs_bdev_create(g_bdev_name, g_cluster_size, shutdown_cb, NULL);
}

static void
mkfs_usage(void)
{
	printf(" -C <size>                 cluster size\n");
}

static int
mkfs_parse_arg(int ch, char *arg)
{
	bool has_prefix;

	switch (ch) {
	case 'C':
		spdk_parse_capacity(arg, &g_cluster_size, &has_prefix);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc = 0;

	if (argc < 3) {
		SPDK_ERRLOG("usage: %s <conffile> <bdevname>\n", argv[0]);
		exit(1);
	}

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "spdk_mkfs";
	opts.json_config_file = argv[1];
	opts.reactor_mask = "0x3";
	opts.shutdown_cb = NULL;

	spdk_fs_set_cache_size(512);
	g_bdev_name = argv[2];
	if ((rc = spdk_app_parse_args(argc, argv, &opts, "C:", NULL,
				      mkfs_parse_arg, mkfs_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	rc = spdk_app_start(&opts, spdk_mkfs_run, NULL);
	spdk_app_fini();

	return rc;
}
