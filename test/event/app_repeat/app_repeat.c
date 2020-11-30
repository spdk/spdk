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

static void _app_repeat_shutdown_cb(void)
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
