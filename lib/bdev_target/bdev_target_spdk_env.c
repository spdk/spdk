/*
 * INTEL CONFIDENTIAL
 *
 * Copyright 2018 Intel Corporation
 *
 * This software and the related documents are Intel copyrighted materials, and
 * your use of them is governed by the express license under which they were
 * provided to you (License). Unless the License provides otherwise, you may not
 * use, modify, copy, publish, distribute, disclose or transmit this software or
 * the related documents without Intel's prior written permission.
 * This software and the related documents are provided as is, with no express or
 * implied warranties, other than those that are expressly stated in the License.
 */


#include "spdk/queue.h"
#include "spdk/thread.h"
#include "spdk/assert.h"
#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/event.h"
#include "spdk_internal/log.h"
#include "spdk/bdev_target.h"

enum spdk_env_state {
	SPDK_ENV_CLOSED,
	SPDK_ENV_INIT,
	SPDK_ENV_RUN,
};

static volatile int g_spdk_ready = SPDK_ENV_CLOSED;
static volatile bool g_spdk_start_failure = false;
static pthread_t mSpdkTid;
static bool g_debug = false;

static void
_bdev_target_shutdown(void)
{
	spdk_app_stop(0);
}

static void
_bdev_target_run(void *arg1, void *arg2)
{
	if (g_debug) {
		spdk_log_set_trace_flag("bdev_target");
	}

	g_spdk_ready = SPDK_ENV_RUN;
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
spdk_env_setup(char *config_file, bool debug)
{
	if (g_spdk_ready == SPDK_ENV_RUN) {
		return 0;
	} else if (g_spdk_ready == SPDK_ENV_INIT || g_spdk_start_failure) {
		return -EEXIST;
	}

	g_spdk_ready = SPDK_ENV_INIT;
	g_debug = debug;

	pthread_create(&mSpdkTid, NULL, &initialize_spdk, config_file);
	while (g_spdk_ready != SPDK_ENV_RUN && !g_spdk_start_failure)
		;
	if (g_spdk_start_failure) {
		g_spdk_ready = SPDK_ENV_CLOSED;
		fprintf(stderr, "spdk_app_start() unable to start spdk_bdev_target_run()\n");
		return -EIO;
	}

	return 0;
}

void
spdk_env_unset(void)
{
	if (g_spdk_ready == SPDK_ENV_CLOSED) {
		return;
	}

	spdk_app_start_shutdown();
	pthread_join(mSpdkTid, NULL);
	g_spdk_ready = SPDK_ENV_CLOSED;
}

