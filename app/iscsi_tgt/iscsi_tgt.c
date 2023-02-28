/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/event.h"
#include "iscsi/iscsi.h"
#include "spdk/log.h"

static int g_daemon_mode = 0;

static void
iscsi_usage(void)
{
	printf(" -b                        run iscsi target background, the default is foreground\n");
}

static void
spdk_startup(void *arg1)
{
	if (getenv("MEMZONE_DUMP") != NULL) {
		spdk_memzone_dump(stdout);
		fflush(stdout);
	}
}

static int
iscsi_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 'b':
		g_daemon_mode = 1;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	int rc;
	struct spdk_app_opts opts = {};

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "iscsi";
	if ((rc = spdk_app_parse_args(argc, argv, &opts, "b", NULL,
				      iscsi_parse_arg, iscsi_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	if (g_daemon_mode) {
		if (daemon(1, 0) < 0) {
			SPDK_ERRLOG("Start iscsi target daemon failed.\n");
			exit(EXIT_FAILURE);
		}
	}

	opts.shutdown_cb = NULL;

	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, spdk_startup, NULL);
	if (rc) {
		SPDK_ERRLOG("Start iscsi target daemon:  spdk_app_start() retn non-zero\n");
	}

	spdk_app_fini();

	return rc;
}
