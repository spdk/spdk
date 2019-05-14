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

#include "spdk/conf.h"
#include "spdk/event.h"

#include "spdk/vhost.h"

#define UNIX 0
#define VVU 1

/* Log levels */
#define VHOST_LOG_NONE 0
#define VHOST_LOG_INFO 1
#define VHOST_LOG_DEBUG 2

static const char *g_pid_path = NULL;

static int chosen_trans = UNIX;
static int valid_basename = 0;
static int log_level = VHOST_LOG_NONE;

static void
vhost_usage(void)
{
	printf(" -f <path>                 save pid to file under given path\n");
	printf(" -S <path>                 directory where UNIX domain sockets will be created (default: pwd) or PCI address of the VVU device\n");
	printf(" -T <option>               choose vhost transport (\"unix\" or \"vvu\") - \"unix\" is the default\n");
	printf(" -l <option>               choose vhost log level (\"none\", \"info\", \"debug\") - \"none\" is the default\n");
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
spdk_vhost_set_vhost_log_level(char *arg)
{
	if (strcmp(arg, "none") == 0) {
		return VHOST_LOG_NONE;
	} else if (strcmp(arg, "info") == 0) {
		return VHOST_LOG_INFO;
	} else if (strcmp(arg, "debug") == 0) {
		return VHOST_LOG_DEBUG;
	} else {
		SPDK_ERRLOG("Invalid vhost log level %s\n", arg);
		return -EINVAL;
	}
}

static int
vhost_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 'f':
		g_pid_path = arg;
		break;
	case 'S':
		valid_basename = spdk_vhost_set_socket_path(arg);
		break;
	case 'T':
		chosen_trans = spdk_vhost_set_transport(arg);
		break;
	case 'l':
		log_level = spdk_vhost_set_vhost_log_level(arg);
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

	spdk_app_opts_init(&opts);
	opts.name = "vhost";

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "f:S:T:l:", NULL,
				      vhost_parse_arg, vhost_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	switch (log_level) {
	case VHOST_LOG_NONE:
		break;
	case VHOST_LOG_INFO:
		opts.env_context = "--log-level=user1:7";
		break;
	case VHOST_LOG_DEBUG:
		opts.env_context = "--log-level=user1:8";
		break;
	default:
		fprintf(stderr, "Exiting...\n");
		exit(EXIT_FAILURE);

	}

	if (chosen_trans < 0) {
		fprintf(stderr, "Exiting...\n");
		exit(EXIT_FAILURE);
	}

	if (chosen_trans == VVU && valid_basename != 1) {
		fprintf(stderr, "Couldn't create VVU transport without a PCI address for the VVU device.\n");
		fprintf(stderr, "Exiting...\n");
		exit(EXIT_FAILURE);
	}

	if (g_pid_path) {
		save_pid(g_pid_path);
	}

	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, vhost_started, NULL);

	spdk_app_fini();

	return rc;
}
