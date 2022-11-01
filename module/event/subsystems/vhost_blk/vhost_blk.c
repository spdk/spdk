/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/vhost.h"

#include "spdk_internal/init.h"

static void
vhost_blk_subsystem_init_done(int rc)
{
	spdk_subsystem_init_next(rc);
}

static void
vhost_blk_subsystem_init(void)
{
	spdk_vhost_blk_init(vhost_blk_subsystem_init_done);
}

static void
vhost_blk_subsystem_fini_done(void)
{
	spdk_subsystem_fini_next();
}

static void
vhost_blk_subsystem_fini(void)
{
	spdk_vhost_blk_fini(vhost_blk_subsystem_fini_done);
}

static struct spdk_subsystem g_spdk_subsystem_vhost_blk = {
	.name = "vhost_blk",
	.init = vhost_blk_subsystem_init,
	.fini = vhost_blk_subsystem_fini,
	.write_config_json = spdk_vhost_blk_config_json,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_vhost_blk);
SPDK_SUBSYSTEM_DEPEND(vhost_blk, bdev)
