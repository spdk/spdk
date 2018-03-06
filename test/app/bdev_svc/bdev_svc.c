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

#include "spdk/env.h"
#include "spdk/event.h"

static char g_path[256];

static void
bdev_svc_usage(void)
{
}

static void
bdev_svc_parse_arg(int ch, char *arg)
{
}

static void
bdev_svc_start(void *arg1, void *arg2)
{
	int fd;
	int shm_id = (intptr_t)arg1;

	spdk_unaffinitize_thread();

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

	/* default value in opts structure */
	spdk_app_opts_init(&opts);

	opts.name = "bdev_svc";
	opts.shutdown_cb = bdev_svc_shutdown;
	opts.max_delay_us = 1000 * 1000;

	if ((rc = spdk_app_parse_args(argc, argv, &opts, "",
				      bdev_svc_parse_arg, bdev_svc_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		exit(rc);
	}

	rc = spdk_app_start(&opts, bdev_svc_start, (void *)(intptr_t)opts.shm_id, NULL);

	spdk_app_fini();

	return rc;
}
