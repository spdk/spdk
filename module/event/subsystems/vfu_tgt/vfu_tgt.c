/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/vfu_target.h"

#include "spdk_internal/init.h"

static void
vfu_subsystem_init_done(int rc)
{
	spdk_subsystem_init_next(rc);
}

static void
vfu_target_subsystem_init(void)
{
	spdk_vfu_init(vfu_subsystem_init_done);
}

static void
vfu_target_subsystem_fini_done(void)
{
	spdk_subsystem_fini_next();
}

static void
vfu_target_subsystem_fini(void)
{
	spdk_vfu_fini(vfu_target_subsystem_fini_done);
}

static struct spdk_subsystem g_spdk_subsystem_vfu_target = {
	.name = "vfio_user_target",
	.init = vfu_target_subsystem_init,
	.fini = vfu_target_subsystem_fini,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_vfu_target);
