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

#include "spdk/queue.h"
#include "spdk/thread.h"
#include "spdk/assert.h"
#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/event.h"
#include "spdk_internal/log.h"
#include "spdk/bdev_target.h"

static volatile bool g_spdk_ready = false;
static volatile bool g_spdk_start_failure = false;
static pthread_t mSpdkTid;

static void
_bdev_target_shutdown(void)
{
	spdk_app_stop(0);
}

static void
_bdev_target_run(void *arg1, void *arg2)
{
	//TODO: remove it for release!!!
//	spdk_log_set_trace_flag("bdev_target");

	g_spdk_ready = true;
}

static void *
initialize_spdk(void *arg)
{
	int rc;
	struct spdk_app_opts _opts = { 0 };
	struct spdk_app_opts *opts = &_opts;

	spdk_unaffinitize_thread();
	spdk_app_opts_init(opts);
	opts->name = "spdk-bdev-target";
	opts->config_file = (char *)arg;
	opts->shutdown_cb = _bdev_target_shutdown;
	opts->max_delay_us = 1000 * 1000;
	opts->print_level = SPDK_LOG_DEBUG;
	rc = spdk_app_start(opts, _bdev_target_run, NULL, NULL);

	if (rc) {
		g_spdk_start_failure = true;
	} else {
		spdk_app_fini();
	}
	pthread_exit(NULL);

}

int
spdk_env_setup(char *config_file)
{
	if (g_spdk_ready || g_spdk_start_failure) {
		return -EEXIST;
	}

	pthread_create(&mSpdkTid, NULL, &initialize_spdk, config_file);
	while (!g_spdk_ready && !g_spdk_start_failure)
		;
	if (g_spdk_start_failure) {
		fprintf(stderr, "spdk_app_start() unable to start spdk_bdev_target_run()\n");
		return -EIO;
	}

	return 0;
}

void
spdk_env_unset(void)
{
	spdk_app_start_shutdown();
	pthread_join(mSpdkTid, NULL);
}

