/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/event.h"
#include "spdk/string.h"
#include "spdk/thread.h"

struct spdk_app_opts g_opts = {};
static const char g_app_repeat_get_opts_string[] = "t:";
static int g_repeat_times = 2;

static void
app_repeat_usage(void)
{
	printf(" -t <num>                  number of times to repeat calling spdk_app_start/stop\n");
}

static int
app_repeat_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 't':
		g_repeat_times = spdk_strtol(arg, 0);
		if (g_repeat_times < 2) {
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void
app_repeat_started(void *arg1)
{
	int index = *(int *)arg1;

	printf("spdk_app_start is called in Round %d.\n", index);
}

static void
_app_repeat_shutdown_cb(void)
{
	printf("Shutdown signal received, stop current app iteration\n");
	spdk_app_stop(0);
}

int
main(int argc, char **argv)
{
	int rc;
	int i;

	spdk_app_opts_init(&g_opts, sizeof(g_opts));
	g_opts.name = "app_repeat";
	g_opts.shutdown_cb = _app_repeat_shutdown_cb;
	if ((rc = spdk_app_parse_args(argc, argv, &g_opts, g_app_repeat_get_opts_string,
				      NULL, app_repeat_parse_arg, app_repeat_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		return rc;
	}

	for (i = 0; i < g_repeat_times; i++) {
		rc = spdk_app_start(&g_opts, app_repeat_started, &i);
		spdk_app_fini();

		if (rc) {
			fprintf(stderr, "Failed to call spdk_app_start in Round %d.\n", i);
			break;
		}
	}

	return rc;
}
