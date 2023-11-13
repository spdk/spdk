/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/fsdev.h"
#include "spdk/env.h"
#include "spdk/thread.h"

#include "spdk_internal/init.h"
#include "spdk/env.h"

static void
fsdev_initialize_complete(void *cb_arg, int rc)
{
	spdk_subsystem_init_next(rc);
}

static void
fsdev_subsystem_initialize(void)
{
	spdk_fsdev_initialize(fsdev_initialize_complete, NULL);
}

static void
fsdev_subsystem_finish_done(void *cb_arg)
{
	spdk_subsystem_fini_next();
}

static void
fsdev_subsystem_finish(void)
{
	spdk_fsdev_finish(fsdev_subsystem_finish_done, NULL);
}

static void
fsdev_subsystem_config_json(struct spdk_json_write_ctx *w)
{
	spdk_fsdev_subsystem_config_json(w);
}

static struct spdk_subsystem g_spdk_subsystem_fsdev = {
	.name = "fsdev",
	.init = fsdev_subsystem_initialize,
	.fini = fsdev_subsystem_finish,
	.write_config_json = fsdev_subsystem_config_json,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_fsdev);
