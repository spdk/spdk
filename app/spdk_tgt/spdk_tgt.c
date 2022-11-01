/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/config.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/vhost.h"

#ifdef SPDK_CONFIG_VHOST
#define SPDK_VHOST_OPTS "S:"
#else
#define SPDK_VHOST_OPTS
#endif

static const char *g_pid_path = NULL;
static const char g_spdk_tgt_get_opts_string[] = "f:" SPDK_VHOST_OPTS;

static void
spdk_tgt_usage(void)
{
	printf(" -f <file>                 pidfile save pid to file under given path\n");
#ifdef SPDK_CONFIG_VHOST
	printf(" -S <path>                 directory where to create vhost sockets (default: pwd)\n");
#endif
}

static void
spdk_tgt_save_pid(const char *pid_path)
{
	FILE *pid_file;

	pid_file = fopen(pid_path, "w");
	if (pid_file == NULL) {
		fprintf(stderr, "Couldn't create pid file '%s': %s\n", pid_path, strerror(errno));
		exit(EXIT_FAILURE);
	}

	fprintf(pid_file, "%d\n", getpid());
	fclose(pid_file);
}


static int
spdk_tgt_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 'f':
		g_pid_path = arg;
		break;
#ifdef SPDK_CONFIG_VHOST
	case 'S':
		spdk_vhost_set_socket_path(arg);
		break;
#endif
	default:
		return -EINVAL;
	}
	return 0;
}

static void
spdk_tgt_started(void *arg1)
{
	if (g_pid_path) {
		spdk_tgt_save_pid(g_pid_path);
	}

	if (getenv("MEMZONE_DUMP") != NULL) {
		spdk_memzone_dump(stdout);
		fflush(stdout);
	}
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "spdk_tgt";
	if ((rc = spdk_app_parse_args(argc, argv, &opts, g_spdk_tgt_get_opts_string,
				      NULL, spdk_tgt_parse_arg, spdk_tgt_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		return rc;
	}

	rc = spdk_app_start(&opts, spdk_tgt_started, NULL);
	spdk_app_fini();

	return rc;
}
