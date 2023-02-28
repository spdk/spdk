/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/event.h"

static char g_path[256];
static bool g_unaffinitize_thread = false;

static void
bdev_svc_usage(void)
{
}

static int
bdev_svc_parse_arg(int ch, char *arg)
{
	return 0;
}

static void
bdev_svc_start(void *arg1)
{
	int fd;
	int shm_id = (intptr_t)arg1;

	if (g_unaffinitize_thread) {
		spdk_unaffinitize_thread();
	}

	snprintf(g_path, sizeof(g_path), "/var/run/spdk_bdev%d", shm_id);
	fd = open(g_path, O_CREAT | O_EXCL | O_RDWR, S_IFREG);
	if (fd < 0) {
		fprintf(stderr, "could not create sentinel file %s\n", g_path);
		exit(1);
	}
	close(fd);
}

static void
bdev_svc_shutdown(void)
{
	unlink(g_path);
	spdk_app_stop(0);
}

int
main(int argc, char **argv)
{
	int rc;
	struct spdk_app_opts opts = {};
	const char *reactor_mask = "0x1";

	/* default value in opts structure */
	spdk_app_opts_init(&opts, sizeof(opts));

	opts.name = "bdev_svc";
	opts.reactor_mask = reactor_mask;
	opts.shutdown_cb = bdev_svc_shutdown;

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "", NULL,
				      bdev_svc_parse_arg, bdev_svc_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	/* User did not specify a reactor mask.  Test scripts may do this when using
	 *  bdev_svc as a primary process to speed up nvme test programs by running
	 *  them as secondary processes.  In that case, we will unaffinitize the thread
	 *  in the bdev_svc_start routine, which will allow the scheduler to move this
	 *  thread so it doesn't conflict with pinned threads in the secondary processes.
	 */
	if (opts.reactor_mask == reactor_mask) {
		g_unaffinitize_thread = true;
	}

	rc = spdk_app_start(&opts, bdev_svc_start, (void *)(intptr_t)opts.shm_id);

	spdk_app_fini();

	return rc;
}
