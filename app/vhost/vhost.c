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

#include "spdk/log.h"
#include "spdk/conf.h"
#include "spdk/event.h"

#include "spdk/vhost.h"


#define SPDK_VHOST_DEFAULT_CONFIG "/usr/local/etc/spdk/vhost.conf"
#define SPDK_VHOST_DEFAULT_ENABLE_COREDUMP true
#define SPDK_VHOST_DEFAULT_MEM_SIZE 1024

static const char *g_socket_path = NULL;
static const char *g_pid_path = NULL;

static void
vhost_app_opts_init(struct spdk_app_opts *opts)
{
	spdk_app_opts_init(opts);
	opts->name = "vhost";
	opts->config_file = SPDK_VHOST_DEFAULT_CONFIG;
	opts->mem_size = SPDK_VHOST_DEFAULT_MEM_SIZE;
}

static void
vhost_usage(void)
{
	printf(" -f pidfile save pid to file under given path\n");
	printf(" -S dir     directory where to create vhost sockets (default: pwd)\n");
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

static void
vhost_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 'f':
		g_pid_path = arg;
		break;
	case 'S':
		g_socket_path = arg;
		break;
	}
}

int
main(int argc, char *argv[])
{
	struct spdk_app_opts opts = {};
	int rc;

	vhost_app_opts_init(&opts);

	SPDK_MAIN_APP_PARSE_ARGS(argc, argv, &opts, "f:S:",
				 vhost_parse_arg, vhost_usage);

	if (g_pid_path) {
		save_pid(g_pid_path);
	}

	/* Blocks until the application is exiting */
	rc = spdk_app_start(&opts, spdk_vhost_startup, (void *)g_socket_path, NULL);
	if (rc < 0) {
		SPDK_ERRLOG("spdk_app_start() unable to start spdk_vhost_startup()\n");
	}

	spdk_app_fini();

	return rc;
}
