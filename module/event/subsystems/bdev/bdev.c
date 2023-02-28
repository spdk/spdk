/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/thread.h"

#include "spdk_internal/init.h"
#include "spdk/env.h"

static void
bdev_initialize_complete(void *cb_arg, int rc)
{
	spdk_subsystem_init_next(rc);
}

static void
bdev_subsystem_initialize(void)
{
	spdk_bdev_initialize(bdev_initialize_complete, NULL);
}

static void
bdev_subsystem_finish_done(void *cb_arg)
{
	spdk_subsystem_fini_next();
}

static void
bdev_subsystem_finish(void)
{
	spdk_bdev_finish(bdev_subsystem_finish_done, NULL);
}

static void
bdev_subsystem_config_json(struct spdk_json_write_ctx *w)
{
	spdk_bdev_subsystem_config_json(w);
}

static struct spdk_subsystem g_spdk_subsystem_bdev = {
	.name = "bdev",
	.init = bdev_subsystem_initialize,
	.fini = bdev_subsystem_finish,
	.write_config_json = bdev_subsystem_config_json,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_bdev);
SPDK_SUBSYSTEM_DEPEND(bdev, accel)
SPDK_SUBSYSTEM_DEPEND(bdev, vmd)
SPDK_SUBSYSTEM_DEPEND(bdev, sock)
SPDK_SUBSYSTEM_DEPEND(bdev, iobuf)
