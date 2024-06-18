/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/event.h"

#include "spdk/vhost.h"

static const char *g_pid_path = NULL;

static void
vhost_usage(void)
{
	printf(" -f <path>                 save pid to file under given path\n");
	printf(" -S <path>                 directory where to create vhost sockets (default: pwd)\n");
}

static void
save_pid(const char *pid_path)
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
vhost_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 'f':
		g_pid_path = arg;
		break;
	case 'S':
		spdk_vhost_set_socket_path(arg);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void
vhost_started(void *arg1)
{
}

int
main(int argc, char *argv[])
{
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "vhost";

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "f:S:", NULL,
				      vhost_parse_arg, vhost_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	if (g_pid_path) {
		save_pid(g_pid_path);
	}

	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, vhost_started, NULL);

	spdk_app_fini();

	return rc;
}
