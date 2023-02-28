/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/event.h"

static void
nvmf_usage(void)
{
}

static int
nvmf_parse_arg(int ch, char *arg)
{
	return 0;
}

static void
nvmf_tgt_started(void *arg1)
{
	if (getenv("MEMZONE_DUMP") != NULL) {
		spdk_memzone_dump(stdout);
		fflush(stdout);
	}
}

int
main(int argc, char **argv)
{
	int rc;
	struct spdk_app_opts opts = {};

	/* default value in opts */
	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "nvmf";
	if ((rc = spdk_app_parse_args(argc, argv, &opts, "", NULL,
				      nvmf_parse_arg, nvmf_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, nvmf_tgt_started, NULL);
	spdk_app_fini();
	return rc;
}
